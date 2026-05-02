#!/bin/bash
# ─── build-in-container.sh ───────────────────────────────────────────────────
# Baut die MCP-Server im Container und erstellt ein .deb Paket.
# Wird von LlamaQt via `incus exec` aufgerufen.
#
# Ablauf:
#   1. Build-Abhängigkeiten installieren (einmalig, danach gecacht)
#   2. Source-Archiv entpacken
#   3. cmake + make (parallel)
#   4. cpack → .deb
#   5. .deb installieren
#
# Analogie (AVR): Makefile-Build — Timestamps entscheiden was neu gebaut wird.
# Hier: apt-cache + cmake-Cache übernehmen diese Rolle.

set -e

ARCHIVE="/tmp/mcp-servers.tar.gz"
BUILD_DIR="/tmp/llamaqt-mcp-build"
INSTALL_DIR="/usr/lib/llamaqt-mcp"

echo "[build] Starte MCP-Server Build im Container"

# ── Schritt 1: Abhängigkeiten ─────────────────────────────────────────────────
echo "[1/5] Build-Abhängigkeiten prüfen..."
if ! dpkg -l | grep -q cmake; then
    apt-get update -qq
    apt-get install -y -qq \
        cmake build-essential \
        qt6-base-dev libqt6core6 \
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

# ── Schritt 3: cmake konfigurieren ───────────────────────────────────────────
echo "[3/5] cmake konfigurieren..."
cd "$BUILD_DIR/mcp-servers/proxy"
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=""

# ── Schritt 4: Bauen ─────────────────────────────────────────────────────────
echo "[4/5] Bauen ($(nproc) CPUs)..."
# Erst die einzelnen MCP-Server bauen
for SERVER in filesystem sysinfo compile; do
    echo "  Baue $SERVER..."
    cmake "$BUILD_DIR/mcp-servers/$SERVER" \
          -B "/tmp/build-$SERVER" \
          -DCMAKE_BUILD_TYPE=Release
    make -C "/tmp/build-$SERVER" -j"$(nproc)"
done

# Dann den Proxy
make -j"$(nproc)"

# ── Schritt 5: .deb erstellen und installieren ───────────────────────────────
echo "[5/5] .deb erstellen..."
cpack -G DEB

DEB_FILE=$(ls ./*.deb 2>/dev/null | head -1)
if [ -z "$DEB_FILE" ]; then
    echo "ERROR: Kein .deb gefunden!" >&2
    exit 1
fi

echo "[5/5] installiere $DEB_FILE..."
dpkg -i "$DEB_FILE"

echo ""
echo "[build] Fertig! llamaqt-mcp-proxy läuft als systemd-Service."
echo "  Port: 9000"
echo "  Status: $(systemctl is-active llamaqt-mcp 2>/dev/null || echo 'unbekannt')"
