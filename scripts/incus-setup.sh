#!/bin/bash
# ─── incus-setup.sh ───────────────────────────────────────────────────────────
# Host-Vorbereitung für LlamaQt Incus-Integration.
# Wird von LlamaQt generiert und dem User zum Ausführen angeboten.
#
# Was dieses Script macht:
#   1. Incus + Extras installieren
#   2. User zur incus-admin Gruppe hinzufügen
#   3. Incus initialisieren (Standard-Netzwerk, Storage)
#   4. LlamaQt-Netzwerk und -Profil anlegen
#   5. Basis-Image für Debian Trixie laden

set -e

echo "=== LlamaQt Incus Setup ==="
echo ""

# ── Schritt 1: Pakete installieren ───────────────────────────────────────────
echo "[1/5] Pakete installieren..."
sudo apt install -y incus incus-extra virt-viewer

# ── Schritt 2: User-Gruppen ───────────────────────────────────────────────────
echo "[2/5] User '$USER' zur incus-admin Gruppe hinzufügen..."
sudo adduser "$USER" incus-admin
echo "  HINWEIS: Logout/Login nötig damit Gruppenrechte wirksam werden!"

# ── Schritt 3: Incus initialisieren ───────────────────────────────────────────
echo "[3/5] Incus initialisieren..."
# --auto: Standard-Einstellungen (dir-basierter Storage, Standard-Netzwerk)
sudo incus admin init --auto

# ── Schritt 4: LlamaQt Netzwerk + Profil ─────────────────────────────────────
echo "[4/5] LlamaQt Netzwerk und Profil erstellen..."

# Netzwerk nur anlegen wenn nicht vorhanden
if ! incus network show llamaqt-net &>/dev/null; then
    incus network create llamaqt-net ipv4.address=10.200.0.1/24 ipv4.nat=true
    echo "  Netzwerk llamaqt-net angelegt (10.200.0.0/24)"
else
    echo "  Netzwerk llamaqt-net existiert bereits"
fi

# Profil anlegen
if ! incus profile show llamaqt &>/dev/null; then
    incus profile create llamaqt
    incus profile device add llamaqt eth0 nic \
        nictype=bridged parent=llamaqt-net
    echo "  Profil llamaqt angelegt"
else
    echo "  Profil llamaqt existiert bereits"
fi

# ── Schritt 5: Basis-Image laden ──────────────────────────────────────────────
echo "[5/5] Debian Trixie Image laden..."
echo "  (Das kann einige Minuten dauern...)"

if ! incus image list | grep -q debian-trixie; then
    incus image copy images:debian/trixie local: --alias debian-trixie
    echo "  Image debian-trixie geladen"
else
    echo "  Image debian-trixie ist bereits vorhanden"
fi

echo ""
echo "=== Setup abgeschlossen ==="
echo ""
echo "WICHTIG: Bitte jetzt ausloggen und neu einloggen"
echo "damit die Gruppenrechte (incus-admin) wirksam werden."
echo ""
echo "Danach LlamaQt neu starten."
