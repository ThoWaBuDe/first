#!/bin/bash
# ─── build-in-container.sh ───────────────────────────────────────────────────
# Baut alle MCP-Server im Container und erstellt ein .deb Paket.
# Wird von LlamaQt via `incus exec` aufgerufen.
#
# Ablauf:
#   1. Build-Abhängigkeiten installieren (einmalig, danach gecacht)
#   2. Source-Archiv entpacken
#   3. cmake + make für filesystem, sysinfo, compile, websearch
#   4. cmake + make für den Proxy
#   5. cpack → .deb (enthält alle Binaries)
#   6. .deb installieren

set -e

ARCHIVE="/tmp/mcp-servers.tar.gz"
BUILD_DIR="/tmp/llamaqt-mcp-build"

echo "[build] Starte MCP-Server Build im Container"

# ── Schritt 1: Abhängigkeiten ─────────────────────────────────────────────────
echo "[1/5] Build-Abhängigkeiten prüfen..."
if ! dpkg -l | grep -q cmake; then
    apt-get update -qq
    apt-get install -y -qq \
        cmake build-essential \
        qt6-base-dev \
        pkg-config
    echo "  Abhängigkeiten installiert"
else
    echo "  Abhängigkeiten bereits vorhanden"
fi

# ── Schritt 2: Source entpacken ───────────────────────────────────────────────
echo "[2/5] Source entpacken..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
tar xzf "$ARCHIVE" -C "$BUILD_DIR"

# ── Schritt 3: Einzelne MCP-Server bauen ─────────────────────────────────────
# Jeder Server landet unter /tmp/build-<name>/llamaqt-<name>
# Das muss mit den install()-Regeln in proxy/CMakeLists.txt übereinstimmen.
echo "[3/5] MCP-Server bauen ($(nproc) CPUs)..."
for SERVER in filesystem sysinfo compile websearch; do
    echo "  Baue $SERVER..."
    cmake "$BUILD_DIR/mcp-servers/$SERVER" \
          -B "/tmp/build-$SERVER" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="/tmp/build-$SERVER"
    make -C "/tmp/build-$SERVER" -j"$(nproc)"

    # Prüfen ob Binary entstanden ist
    if [ ! -f "/tmp/build-$SERVER/llamaqt-$SERVER" ]; then
        echo "FEHLER: /tmp/build-$SERVER/llamaqt-$SERVER nicht gefunden!" >&2
        ls /tmp/build-$SERVER/ >&2
        exit 1
    fi
    echo "  ✓ llamaqt-$SERVER"
done

# ── Schritt 4: Proxy bauen ────────────────────────────────────────────────────
echo "[4/5] Proxy bauen..."
cmake "$BUILD_DIR/mcp-servers/proxy" \
      -B "$BUILD_DIR/proxy-build" \
      -DCMAKE_BUILD_TYPE=Release
make -C "$BUILD_DIR/proxy-build" -j"$(nproc)"

# ── Schritt 5: .deb erstellen und installieren ───────────────────────────────
echo "[5/5] .deb erstellen..."
cd "$BUILD_DIR/proxy-build"
cpack -G DEB

DEB_FILE=$(ls ./*.deb 2>/dev/null | head -1)
if [ -z "$DEB_FILE" ]; then
    echo "ERROR: Kein .deb gefunden!" >&2
    exit 1
fi

echo "[5/5] installiere $DEB_FILE..."
dpkg -i "$DEB_FILE"

# Prüfen ob alle Binaries korrekt installiert wurden
echo ""
echo "Installierte Binaries:"
ls -la /usr/lib/llamaqt-mcp/ 2>/dev/null || echo "  WARNUNG: Verzeichnis nicht gefunden!"

echo ""
echo "[build] Fertig! llamaqt-mcp-proxy läuft als systemd-Service."
echo "  Port: 9000"
echo "  Status: $(systemctl is-active llamaqt-mcp 2>/dev/null || echo 'unbekannt')"
