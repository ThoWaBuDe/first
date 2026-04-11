#pragma once
// ─── GrepCodeTool ─────────────────────────────────────────────────────────────
// Regex-Suche über alle Dateien rekursiv.
// Bis zu MAX_GREP_HITS Treffer, Format: datei:zeile: inhalt
// Liest aus allen bekannten Roots (auch read-only).

#include "FilesystemToolBase.h"
#include <QDirIterator>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QFileInfo>

class GrepCodeTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "grep_code"; }

    QString description() const override
    {
        return
            "Search all files in a directory (or the sandbox) for lines matching "
            "a regular expression. Returns up to 200 matches in 'file:line: content' format. "
            "Works in all known roots. "
            "For results with surrounding context lines, use search_code instead.";
    }

    QJsonObject properties() const override
    {
        return {
            {"pattern", prop("string", "ECMAScript/PCRE regex to search for.")},
            {"path",    prop("string", "Relative or absolute directory to search. Defaults to sandbox root.")}
        };
    }

    QJsonArray required() const override { return req({"pattern"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString pattern = args.value("pattern").toString();
        QString path    = args.value("path").toString(".");
        if (pattern.isEmpty()) return ToolResult::err("Error: 'pattern' is required.");

        QRegularExpression re(pattern);
        if (!re.isValid())
            return ToolResult::err(QString("Invalid regex: %1").arg(re.errorString()));

        auto [abs, err] = resolveRead(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        // Welche Root gehört zu diesem Pfad? Für relative Ausgabe.
        // Wir bestimmen die längste passende Root.
        QString rootPath = m_policy->primarySandbox();
        for (const auto &r : m_policy->roots()) {
            if (abs.startsWith(r.path) && r.path.length() > rootPath.length())
                rootPath = r.path;
        }

        QDirIterator it(abs, QStringList() << "*",
                        QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);

        QStringList results;
        int hits = 0;
        while (it.hasNext() && hits < MAX_GREP_HITS) {
            QString filePath = it.next();
            // Symlinks überspringen — Sicherheit
            if (QFileInfo(filePath).isSymLink()) continue;

            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

            QString rel = filePath.mid(rootPath.length() + 1);
            QTextStream in(&file);
            int lineNum = 0;
            while (!in.atEnd()) {
                QString line = in.readLine();
                ++lineNum;
                if (re.match(line).hasMatch()) {
                    results << QString("%1:%2: %3").arg(rel).arg(lineNum).arg(line.trimmed());
                    ++hits;
                    if (hits >= MAX_GREP_HITS) break;
                }
            }
        }
        return ToolResult::ok(results.isEmpty() ? "No matches." : results.join('\n'));
    }
};
