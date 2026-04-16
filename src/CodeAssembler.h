#pragma once
// ─── CodeAssembler ────────────────────────────────────────────────────────────
// Assembliert den TaskTree zu echten Dateien auf Disk.
//
// v2 Fixes:
//   buildFileContent() — Duplikat-#includes entfernen
//   shouldInclude()    — Placeholder-result ("...", zu kurz) überspringen
//   assembleNode()     — Unterverzeichnis aus Titel (z.B. "src/foo.cpp")

#include "TaskTree.h"
#include <QString>
#include <QStringList>
#include <QSet>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>

class CodeAssembler
{
public:
    struct AssemblyResult {
        int         filesWritten = 0;
        int         filesSkipped = 0;
        int         filesFailed  = 0;
        QStringList writtenPaths;
        QStringList errors;
        bool success() const { return filesFailed == 0; }
    };

    explicit CodeAssembler() = default;

    AssemblyResult assemble(const QString  &targetDir,
                            const TaskTree &tree,
                            bool            onlyDone = true)
    {
        AssemblyResult result;

        if (targetDir.isEmpty()) {
            result.errors << "Kein Zielverzeichnis angegeben.";
            result.filesFailed = 1;
            return result;
        }

        if (!QDir().mkpath(targetDir)) {
            result.errors << QString("Konnte Verzeichnis nicht anlegen: %1").arg(targetDir);
            result.filesFailed = 1;
            return result;
        }

        tree.traverse([&](const TaskNode *node) {
            if (node->level != static_cast<int>(TaskLevel::Class)) return;
            assembleNode(node, targetDir, onlyDone, result);
        });

        return result;
    }

    void assembleNode(const TaskNode  *node,
                      const QString   &targetDir,
                      bool             onlyDone,
                      AssemblyResult  &result)
    {
        if (!node) return;

        if (onlyDone && node->status != TaskStatus::Done) {
            result.filesSkipped++;
            return;
        }

        QString assembled = buildFileContent(node, onlyDone);
        if (assembled.trimmed().isEmpty()) {
            result.filesSkipped++;
            return;
        }

        // Zieldatei — Unterverzeichnis aus Titel möglich (z.B. "src/foo.cpp")
        QString relPath  = node->title.trimmed();
        QString fullPath = QDir(targetDir).filePath(relPath);
        QFileInfo fi(fullPath);

        if (!fi.absoluteDir().exists()) {
            if (!QDir().mkpath(fi.absolutePath())) {
                result.errors << QString("Konnte Unterverzeichnis nicht anlegen: %1")
                                 .arg(fi.absolutePath());
                result.filesFailed++;
                return;
            }
        }

        QFile file(fullPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            result.errors << QString("Konnte nicht schreiben: %1 — %2")
                             .arg(fullPath, file.errorString());
            result.filesFailed++;
            return;
        }

        QTextStream out(&file);
        out << assembled;
        file.close();

        result.filesWritten++;
        result.writtenPaths << fullPath;
    }

private:
    // ── Datei-Inhalt bauen ────────────────────────────────────────────────────
    QString buildFileContent(const TaskNode *node, bool onlyDone) const
    {
        // H2-Teil sammeln
        QString h2Content;
        if (!node->result.isEmpty() && shouldInclude(node, onlyDone))
            h2Content = node->result;

        // H3-Kinder sammeln
        QString childContent;
        for (const TaskNode *child : node->children) {
            if (child->level != static_cast<int>(TaskLevel::Impl)) continue;
            if (!shouldInclude(child, onlyDone)) continue;
            if (child->result.isEmpty()) continue;

            if (!childContent.isEmpty()) childContent += "\n";
            childContent += child->result;
            if (!childContent.endsWith('\n')) childContent += '\n';
        }

        if (h2Content.isEmpty() && childContent.isEmpty())
            return {};

        // H2 + H3 zusammenfügen
        QString combined;
        if (!h2Content.isEmpty()) {
            combined = h2Content;
            if (!combined.endsWith('\n')) combined += '\n';
            if (!childContent.isEmpty()) combined += '\n';
        }
        combined += childContent;

        // ── Duplikat-#includes entfernen ──────────────────────────────────────
        // Problem: Modell wiederholt manchmal #include-Blöcke in H3-Nodes.
        // Lösung: alle #include-Zeilen einmalig sammeln, Duplikate entfernen.
        //
        // Analogie AVR: wie ein Linker der duplicate symbols auflöst —
        // die erste Definition gewinnt, alle weiteren werden verworfen.
        combined = deduplicateIncludes(combined);

        return combined;
    }

    // ── #include-Duplikate entfernen ──────────────────────────────────────────
    // Durchläuft alle Zeilen, merkt sich gesehene #include-Zeilen,
    // entfernt Wiederholungen.
    QString deduplicateIncludes(const QString &code) const
    {
        QStringList lines  = code.split('\n');
        QSet<QString> seen;
        QStringList   result;

        for (const QString &line : lines) {
            QString trimmed = line.trimmed();

            if (trimmed.startsWith("#include")) {
                if (seen.contains(trimmed)) {
                    // Duplikat — überspringen
                    continue;
                }
                seen.insert(trimmed);
            }

            result << line;
        }

        return result.join('\n');
    }

    // ── Node für Assembly freigeben? ──────────────────────────────────────────
    // v2: Placeholder-result überspringen.
    //
    // Placeholder-Erkennung:
    //   "..."           — expliziter Platzhalter
    //   Zu kurz (<= 5)  — kein sinnvoller Code
    //   Nur Whitespace  — leer
    bool shouldInclude(const TaskNode *node, bool onlyDone) const
    {
        if (onlyDone && node->status != TaskStatus::Done)
            return false;

        // result auf Sinnhaftigkeit prüfen
        QString r = node->result.trimmed();
        if (r.isEmpty()) return false;
        if (r == "...") return false;
        if (r.length() <= 5) return false;

        // Nur aus "..." bestehender Content mit Whitespace
        static const QRegularExpression placeholderRe("^\\.+$");
        if (placeholderRe.match(r).hasMatch()) return false;

        return true;
    }
};
