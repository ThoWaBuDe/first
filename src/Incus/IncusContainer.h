#pragma once
#include <QString>

// ─── IncusContainer ───────────────────────────────────────────────────────────
// Reines Datenmodell — kein QObject, keine Logik.
// Beschreibt einen Incus-Container so wie die REST API ihn liefert.
//
// Status-Strings kommen direkt von Incus:
//   "Running", "Stopped", "Frozen", "Error"
//
// Analogie (AVR): struct-in-EEPROM — nur Daten, keine ISR.

struct IncusContainer {
    QString name;       // z.B. "llamaqt-dev"
    QString status;     // "Running" | "Stopped" | "Frozen" | "Error"
    QString image;      // z.B. "debian/trixie"
    QString ipv4;       // erste IPv4-Adresse oder leer
    QString arch;       // "x86_64"

    bool isRunning() const { return status == "Running"; }
    bool isStopped() const { return status == "Stopped"; }
};
