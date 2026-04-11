#pragma once
// ─── SearchCodeTool ───────────────────────────────────────────────────────────
// Sucht nach einem Regex-Pattern und zeigt den ersten Treffer pro Datei
// mit N Kontext-Zeilen darüber und darunter.
// Besser als grep_code wenn man den Zusammenhang verstehen möchte.

#include "FilesystemToolBase.h"
#include <QDirIterator>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QFileInfo>

class SearchCodeTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "search_code"; }

    QString description() const override
    {
        return
            "Search for a regex pattern and return the first match per file "
            "with surrounding context lines. "
            "Unlike grep_code, this shows N lines above and below each match, "
            "making it easier to understand the surrounding code. "
            "Works in all known roots.";
    }

    QJsonObject properties() const override
    {
        return {
            {"pattern", prop("string",  "ECMAScript/PCRE regex to search for.")},
            {"path",    prop("string",  "Relative or absolute directory to search. Defaults to sandbox root.")},
            {"context", prop("integer", "Number of context lines above and below each match (0-10, default 3).")}
        };
    }

    QJsonArray required() const override { return req({"pattern"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString pattern = args.value("pattern").toString();
        QString path    = args.value("path").toString(".");
        int context     = qBound(0, args.value("context").toInt(3), 10);
        if (pattern.isEmpty()) return ToolResult::err("Error: 'pattern' required.");

        QRegularExpression re(pattern);
        if (!re.isValid()) return ToolResult::err("Invalid regex.");

        auto [abs, err] = resolveRead(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        QString rootPath = m_policy->primarySandbox();
        for (const auto &r : m_policy->roots())
            if (abs.startsWith(r.path) && r.path.length() > rootPath.length())
                rootPath = r.path;

        QDirIterator it(abs, QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);

        QStringList results;
        while (it.hasNext()) {
            QString filePath = it.next();
            if (QFileInfo(filePath).isSymLink()) continue;

            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

            QString rel   = filePath.mid(rootPath.length() + 1);
            QStringList lines = QTextStream(&file).readAll().split('\n');

            for (int i = 0; i < lines.size(); ++i) {
                if (re.match(lines[i]).hasMatch()) {
                    results << QString("%1:%2:").arg(rel).arg(i + 1);
                    int from = qMax(0, i - context);
                    int to   = qMin(lines.size() - 1, i + context);
                    for (int j = from; j <= to; ++j) {
                        QString pfx = (j == i) ? ">>" : "  ";
                        results << QString("  %1 %2 | %3")
                                   .arg(pfx)
                                   .arg(j + 1, 4)
                                   .arg(lines[j].left(120));
                    }
                    results << "";
                    break;  // nur erster Treffer pro Datei
                }
            }
        }
        return ToolResult::ok(results.isEmpty() ? "No matches." : results.join('\n'));
    }
};
