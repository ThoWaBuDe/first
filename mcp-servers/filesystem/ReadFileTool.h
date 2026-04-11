#pragma once
// ─── ReadFileTool ─────────────────────────────────────────────────────────────
// Liest eine Datei aus der Sandbox oder einer bekannten read-only Root.
//
// Neu gegenüber v2.3:
//   - Absolute Pfade sind erlaubt wenn sie in einer bekannten Root liegen
//     (PathPolicy::resolveRead() prüft das).
//   - Optionaler Zeilenbereich: start_line / end_line.
//   - Truncation bei MAX_READ_CHARS mit Hinweis.

#include "FilesystemToolBase.h"
#include <QFile>
#include <QTextStream>

class ReadFileTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "read_file"; }

    QString description() const override
    {
        return
            "Read a text file and return its content. "
            "Paths can be relative to the sandbox root (e.g. 'myproject/main.cpp') "
            "or absolute if they are inside a known root. "
            "Content is truncated at 8192 characters; use search_code or grep_code "
            "to locate specific sections in large files. "
            "Optional: specify start_line and end_line to read a slice of the file.";
    }

    QJsonObject properties() const override
    {
        return {
            {"path",       prop("string",  "Relative or absolute path to the file.")},
            {"start_line", prop("integer", "First line to return (1-based, optional).")},
            {"end_line",   prop("integer", "Last line to return (inclusive, optional).")}
        };
    }

    QJsonArray required() const override { return req({"path"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path = args.value("path").toString();
        if (path.isEmpty()) return ToolResult::err("Error: 'path' is required.");

        auto [abs, err] = resolveRead(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        QFile file(abs);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return ToolResult::err(QString("Error: Cannot open file: %1").arg(path));

        // Optionaler Zeilenbereich
        int startLine = args.value("start_line").toInt(0);
        int endLine   = args.value("end_line").toInt(0);

        QTextStream in(&file);

        if (startLine > 0 || endLine > 0) {
            // Zeilenbasierter Modus
            QStringList result;
            int lineNum = 0;
            while (!in.atEnd()) {
                ++lineNum;
                QString line = in.readLine();
                if (startLine > 0 && lineNum < startLine) continue;
                if (endLine   > 0 && lineNum > endLine)   break;
                result << line;
            }
            return ToolResult::ok(result.join('\n'));
        }

        // Vollständiger Modus mit Truncation
        QString content = in.readAll();
        if (content.length() > MAX_READ_CHARS) {
            int cut = content.lastIndexOf('\n', MAX_READ_CHARS);
            if (cut < 0) cut = MAX_READ_CHARS;
            content = content.left(cut) + "\n[... truncated — use start_line/end_line for more]";
        }
        return ToolResult::ok(content);
    }
};
