#pragma once
// ─── PatchFileTool ────────────────────────────────────────────────────────────
// Wendet einen unified diff Patch auf eine Datei an.
//
// Algorithmus (Kurzfassung — ausführlich in main.cpp v2.3 dokumentiert):
//
//   Ein unified diff besteht aus Hunks. Jeder Hunk beginnt mit:
//     @@ -oldStart,oldLen +newStart,newLen @@
//   Danach Zeilen mit Präfix:
//     ' '  Kontextzeile (in old UND new)
//     '-'  nur in old (wird gelöscht)
//     '+'  nur in new (wird eingefügt)
//
//   Wir lesen die Datei in QStringList. Für jeden Hunk:
//     1. oldBlock und newBlock aufbauen
//     2. Validieren (Größen vs. @@ Header)
//     3. Abgleich mit Dateiinhalt an Position (oldStart + offset)
//     4. Ersetzen: oldBlock → newBlock
//     5. offset += (newLen - oldLen)  — für Folgehunks
//
//   Der offset ist wie ein Zeigerversatz in C: durch frühere Einfügungen/
//   Löschungen sind absolute Zeilennummern in nachfolgenden Hunks verschoben.

#include "FilesystemToolBase.h"
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>

class PatchFileTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "patch_file"; }

    QString description() const override
    {
        return
            "Apply a unified diff patch to an existing file. "
            "The diff must be in standard unified diff format as produced by "
            "'diff -u' or 'git diff'. Include the '--- a/...' and '+++ b/...' "
            "header lines and one or more @@ hunks. "
            "Each hunk must have at least 2-3 context lines (' ' prefix) around "
            "the changes to allow unique matching. "
            "Lines starting with '-' are removed, '+' are inserted, ' ' are context. "
            "Only allowed inside the sandbox (writable root). "
            "Use this tool for multi-location edits or when str_replace would require "
            "many separate calls. Changes are committed to git automatically.";
    }

    QJsonObject properties() const override
    {
        return {
            {"path", prop("string", "Relative path to the file to patch.")},
            {"diff", prop("string",
                "Unified diff content including --- and +++ headers and @@ hunks. "
                "Max 32768 characters.")}
        };
    }

    QJsonArray required() const override { return req({"path", "diff"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path        = args.value("path").toString();
        QString diffContent = args.value("diff").toString();

        if (path.isEmpty() || diffContent.isEmpty())
            return ToolResult::err("Error: 'path' and 'diff' are required.");
        if (diffContent.length() > MAX_WRITE_CHARS * 2)
            return ToolResult::err(
                QString("Error: diff too large (max %1 chars).").arg(MAX_WRITE_CHARS * 2));

        auto [abs, err] = resolveWrite(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        QFile file(abs);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return ToolResult::err(QString("Error: Cannot read %1").arg(path));

        QStringList patchedLines;
        {
            QTextStream in(&file);
            while (!in.atEnd()) patchedLines << in.readLine();
        }
        file.close();

        m_git->autoCommit(abs, QString("before patch_file %1").arg(path));

        // Hunk-Header Regex: @@ -oldStart,oldLen +newStart,newLen @@
        // Komma+Länge optional (z.B. "@@ -1 +1 @@" = 1 Zeile).
        QRegularExpression hunkRe(R"(^@@[ \t]+-(\d+),?(\d*)[ \t]+\+(\d+),?(\d*)[ \t]+@@)");

        QStringList diffLines = diffContent.split('\n');
        int offset  = 0;
        int hunkNum = 0;
        int i       = 0;

        while (i < diffLines.size()) {
            const QString &dline = diffLines[i];

            // Datei-Header "--- a/..." und "+++ b/..." überspringen.
            // Ohne diesen Check würden sie als gelöschte/neue Zeilen landen.
            if (dline.startsWith("--- ") || dline.startsWith("+++ ")) {
                ++i; continue;
            }

            auto match = hunkRe.match(dline);
            if (!match.hasMatch()) { ++i; continue; }

            ++hunkNum;
            int oldStart = match.captured(1).toInt() - 1;  // 0-basiert
            int oldLen   = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
            int newLen   = match.captured(4).isEmpty() ? 1 : match.captured(4).toInt();
            ++i;

            QStringList oldBlock, newBlock;

            while (i < diffLines.size() && !diffLines[i].startsWith("@@")) {
                const QString &dl = diffLines[i++];
                if (dl.startsWith('\\')) continue;  // "\ No newline at end of file"

                if (dl.startsWith('-')) {
                    oldBlock << dl.mid(1);
                } else if (dl.startsWith('+')) {
                    newBlock << dl.mid(1);
                } else {
                    // Kontextzeile: ' ' + Inhalt, oder Leerzeile
                    QString content = dl.startsWith(' ') ? dl.mid(1) : dl;
                    oldBlock << content;
                    newBlock << content;
                }
            }

            if (oldBlock.size() != oldLen)
                return ToolResult::err(
                    QString("Error: Hunk %1 header says oldLen=%2 but parsed %3 old lines.")
                        .arg(hunkNum).arg(oldLen).arg(oldBlock.size()));
            if (newBlock.size() != newLen)
                return ToolResult::err(
                    QString("Error: Hunk %1 header says newLen=%2 but parsed %3 new lines.")
                        .arg(hunkNum).arg(newLen).arg(newBlock.size()));

            int applyAt = oldStart + offset;
            if (applyAt < 0 || applyAt + oldBlock.size() > patchedLines.size())
                return ToolResult::err(
                    QString("Error: Hunk %1 position %2 is out of bounds (file has %3 lines).")
                        .arg(hunkNum).arg(applyAt + 1).arg(patchedLines.size()));

            for (int j = 0; j < oldBlock.size(); ++j) {
                if (patchedLines[applyAt + j] != oldBlock[j])
                    return ToolResult::err(
                        QString("Error: Hunk %1 does not match file at line %2.\n"
                                "  Expected: \"%3\"\n"
                                "  Found:    \"%4\"")
                            .arg(hunkNum).arg(applyAt + j + 1)
                            .arg(oldBlock[j]).arg(patchedLines[applyAt + j]));
            }

            // oldBlock durch newBlock ersetzen
            patchedLines.erase(patchedLines.begin() + applyAt,
                               patchedLines.begin() + applyAt + oldBlock.size());
            for (int j = 0; j < newBlock.size(); ++j)
                patchedLines.insert(applyAt + j, newBlock[j]);

            // Offset für Folgehunks anpassen
            // Analogie: wenn du 3 Bytes aus einem UART-Puffer löschst und
            // 5 einfügst, verschieben sich alle folgenden Indizes um +2.
            offset += (newBlock.size() - oldBlock.size());
        }

        if (hunkNum == 0)
            return ToolResult::err("Error: No valid hunks found in diff.");

        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return ToolResult::err("Error: Cannot write to file.");
        {
            QTextStream out(&file);
            for (const QString &l : patchedLines) out << l << "\n";
        }
        file.close();

        m_git->autoCommit(abs, QString("patch_file %1").arg(path));
        return ToolResult::ok(
            QString("OK: Applied %1 hunk(s) to %2.").arg(hunkNum).arg(path));
    }
};
