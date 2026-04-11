#pragma once
// ─── PathPolicy ───────────────────────────────────────────────────────────────
// Gemeinsame Pfad-Sicherheitslogik für alle LlamaQt MCP-Server.
//
// Konzept: Ein Server kennt eine oder mehrere RootPaths.
// Jede Root hat ein writable-Flag:
//   writable = true  → Lesen + Schreiben erlaubt (Sandbox)
//   writable = false → nur Lesen erlaubt (z.B. LlamaQt-Quellcode)
//
// Pfad-Auflösung:
//   Absoluter Pfad  → direkt verwenden, Root-Zugehörigkeit wird geprüft
//   Relativer Pfad  → erste Root in der der Pfad existiert gewinnt,
//                     Fallback: erste Root (für neue Dateien beim Schreiben)
//
// Sicherheit:
//   - Symlink-Check auf jeder Pfad-Komponente (verhindert Sandbox-Escape)
//   - Schreiben außerhalb aller writable Roots → abgelehnt
//   - Absoluter Pfad außerhalb aller bekannten Roots → abgelehnt

#include <QString>
#include <QStringList>
#include <QVector>
#include <QFileInfo>
#include <QDir>

class PathPolicy
{
public:
    // Eine Root-Definition: Pfad + Schreiberlaubnis
    struct Root {
        QString path;       // absoluter Pfad, kein trailing slash
        bool    writable;   // true = Sandbox (lesen+schreiben), false = read-only
    };

    // Ergebnis einer Pfad-Auflösung
    struct ResolvedPath {
        QString absPath;    // aufgelöster absoluter Pfad
        bool    writable;   // darf hier geschrieben werden?
        bool    valid;      // false → Fehler, siehe error
        QString error;      // Fehlermeldung wenn !valid
    };

    // ── Konstruktion ──────────────────────────────────────────────────────────

    // Leerer Policy — Roots müssen via addRoot() hinzugefügt werden
    PathPolicy() = default;

    // Convenience: eine einzelne Sandbox-Root
    explicit PathPolicy(const QString &sandboxPath)
    {
        addRoot(sandboxPath, true);
    }

    // Roots hinzufügen
    void addRoot(const QString &path, bool writable)
    {
        QString clean = path;
        while (clean.endsWith('/') && clean.size() > 1)
            clean.chop(1);
        m_roots.append({clean, writable});
    }

    // ── Pfad-Auflösung ────────────────────────────────────────────────────────

    // Löst einen Pfad auf und prüft Sicherheit.
    // forWrite = true: Schreibzugriff — nur writable Roots erlaubt.
    ResolvedPath resolve(const QString &path, bool forWrite = false) const
    {
        if (path.isEmpty())
            return invalid("Error: empty path.");

        QString abs;

        if (path.startsWith('/')) {
            // Absoluter Pfad: direkt verwenden, Root-Zugehörigkeit prüfen
            abs = QFileInfo(path).absoluteFilePath();

            const Root *root = rootFor(abs);
            if (!root)
                return invalid(QString("Error: path outside all known roots: %1").arg(path));

            if (forWrite && !root->writable)
                return invalid(QString("Error: path is read-only (not in sandbox): %1").arg(path));
        } else {
            // Relativer Pfad: Roots der Reihe nach ausprobieren
            // Für Lesezugriff: erste Root in der der Pfad existiert
            // Für Schreibzugriff: erste *writable* Root in der der Pfad existiert,
            //                     Fallback: erste writable Root (für neue Dateien)
            abs = resolveRelative(path, forWrite);
            if (abs.isEmpty())
                return invalid(QString("Error: no suitable root found for: %1").arg(path));
        }

        // Symlink-Check auf jeder Pfad-Komponente
        QString symlinkErr = checkSymlinks(abs);
        if (!symlinkErr.isEmpty())
            return invalid(symlinkErr);

        // Nochmal Root-Check nach Symlink-Auflösung (Paranoia)
        const Root *root = rootFor(abs);
        if (!root)
            return invalid(QString("Error: resolved path escaped all roots: %1").arg(abs));

        if (forWrite && !root->writable)
            return invalid(QString("Error: write not allowed outside sandbox: %1").arg(abs));

        return { abs, root->writable, true, {} };
    }

    // Convenience: nur Lesen
    ResolvedPath resolveRead(const QString &path) const
    {
        return resolve(path, false);
    }

    // Convenience: Schreiben (nur in writable Roots)
    ResolvedPath resolveWrite(const QString &path) const
    {
        return resolve(path, true);
    }

    // ── Abfragen ──────────────────────────────────────────────────────────────

    // Erste writable Root (= primäre Sandbox) — für git, trash etc.
    QString primarySandbox() const
    {
        for (const Root &r : m_roots)
            if (r.writable) return r.path;
        return {};
    }

    // Alle Roots zurückgeben (für Logging)
    const QVector<Root> &roots() const { return m_roots; }

private:
    QVector<Root> m_roots;

    // Gibt die Root zurück zu der absPath gehört, oder nullptr
    const Root *rootFor(const QString &absPath) const
    {
        for (const Root &r : m_roots)
            if (absPath.startsWith(r.path + '/') || absPath == r.path)
                return &r;
        return nullptr;
    }

    // Relativen Pfad auflösen: durchsucht Roots in Reihenfolge
    QString resolveRelative(const QString &rel, bool forWrite) const
    {
        // Schreibzugriff: nur writable Roots durchsuchen
        // Lesezugriff: alle Roots durchsuchen

        // Erst: existierende Datei suchen (in der richtigen Root-Kategorie)
        for (const Root &r : m_roots) {
            if (forWrite && !r.writable) continue;
            QString candidate = r.path + '/' + rel;
            if (QFileInfo::exists(candidate))
                return candidate;
        }

        // Für Lesezugriff: auch read-only Roots versuchen wenn nichts gefunden
        if (!forWrite) {
            for (const Root &r : m_roots) {
                if (r.writable) continue; // schon oben probiert
                QString candidate = r.path + '/' + rel;
                if (QFileInfo::exists(candidate))
                    return candidate;
            }
        }

        // Nichts gefunden: Fallback auf erste passende Root
        // (für neue Dateien beim Schreiben, oder Fehlermeldung beim Lesen)
        for (const Root &r : m_roots) {
            if (forWrite && !r.writable) continue;
            return r.path + '/' + rel;
        }
        return {};
    }

    // Symlink-Check: jede Pfad-Komponente prüfen.
    // Gibt Fehlermeldung zurück wenn ein Symlink gefunden wird, sonst "".
    // Verhindert: /sandbox/evil -> /etc  (Sandbox-Escape via Symlink)
    static QString checkSymlinks(const QString &absPath)
    {
        QStringList parts = absPath.split('/', Qt::SkipEmptyParts);
        QString current;
        for (const QString &part : parts) {
            current += '/' + part;
            if (QFileInfo(current).isSymLink())
                return QString("Error: symlink detected in path — not allowed: %1").arg(current);
        }
        return {};
    }

    static ResolvedPath invalid(const QString &error)
    {
        return { {}, false, false, error };
    }
};
