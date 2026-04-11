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

    PathPolicy() = default;

    explicit PathPolicy(const QString &sandboxPath)
    {
        addRoot(sandboxPath, true);
    }

    void addRoot(const QString &path, bool writable)
    {
        QString clean = path;
        while (clean.endsWith('/') && clean.size() > 1)
            clean.chop(1);
        m_roots.append({clean, writable});
    }

    // ── Pfad-Auflösung ────────────────────────────────────────────────────────

    ResolvedPath resolve(const QString &path, bool forWrite = false) const
    {
        if (path.isEmpty())
            return invalid("Error: empty path.");

        QString abs;

        if (path.startsWith('/')) {
            abs = QFileInfo(path).absoluteFilePath();

            const Root *root = rootFor(abs);
            if (!root)
                return invalid(QString("Error: path outside all known roots: %1").arg(path));

            if (forWrite && !root->writable)
                return invalid(QString("Error: path is read-only (not in sandbox): %1").arg(path));
        } else {
            abs = resolveRelative(path, forWrite);
            if (abs.isEmpty())
                return invalid(QString("Error: no suitable root found for: %1").arg(path));
        }

        QString symlinkErr = checkSymlinks(abs);
        if (!symlinkErr.isEmpty())
            return invalid(symlinkErr);

        const Root *root = rootFor(abs);
        if (!root)
            return invalid(QString("Error: resolved path escaped all roots: %1").arg(abs));

        if (forWrite && !root->writable)
            return invalid(QString("Error: write not allowed outside sandbox: %1").arg(abs));

        return { abs, root->writable, true, {} };
    }

    ResolvedPath resolveRead(const QString &path) const
    {
        return resolve(path, false);
    }

    ResolvedPath resolveWrite(const QString &path) const
    {
        return resolve(path, true);
    }

    // ── Abfragen ──────────────────────────────────────────────────────────────

    QString primarySandbox() const
    {
        for (const Root &r : m_roots)
            if (r.writable) return r.path;
        return {};
    }

    const QVector<Root> &roots() const { return m_roots; }

private:
    QVector<Root> m_roots;

    const Root *rootFor(const QString &absPath) const
    {
        for (const Root &r : m_roots)
            if (absPath.startsWith(r.path + '/') || absPath == r.path)
                return &r;
        return nullptr;
    }

    QString resolveRelative(const QString &rel, bool forWrite) const
    {
        for (const Root &r : m_roots) {
            if (forWrite && !r.writable) continue;
            QString candidate = r.path + '/' + rel;
            if (QFileInfo::exists(candidate))
                return candidate;
        }

        if (!forWrite) {
            for (const Root &r : m_roots) {
                if (r.writable) continue;
                QString candidate = r.path + '/' + rel;
                if (QFileInfo::exists(candidate))
                    return candidate;
            }
        }

        for (const Root &r : m_roots) {
            if (forWrite && !r.writable) continue;
            return r.path + '/' + rel;
        }
        return {};
    }

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
