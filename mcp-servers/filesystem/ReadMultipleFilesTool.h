#pragma once
// ─── ReadMultipleFilesTool ────────────────────────────────────────────────────
// Liest bis zu MAX_MULTIPLE_FILES Dateien in einem Aufruf.
// Gibt jede Datei mit "=== path ===" Header getrennt zurück.

#include "FilesystemToolBase.h"
#include <QFile>
#include <QTextStream>
#include <QJsonArray>

class ReadMultipleFilesTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "read_multiple_files"; }

    QString description() const override
    {
        return
            "Read up to 15 files in one call. Returns each file's content separated "
            "by a '=== path ===' header. Files that cannot be read show an error "
            "instead of their content. Each file is still subject to the 8192-char "
            "truncation limit. Works in all known roots.";
    }

    QJsonObject properties() const override
    {
        return {
            {"paths", prop("array", "Array of relative or absolute file paths (max 15).")}
        };
    }

    QJsonArray required() const override { return req({"paths"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QJsonArray paths = args.value("paths").toArray();
        if (paths.isEmpty())
            return ToolResult::err("Error: 'paths' array required.");
        if (paths.size() > MAX_MULTIPLE_FILES)
            return ToolResult::err(
                QString("Error: Max %1 files allowed.").arg(MAX_MULTIPLE_FILES));

        QStringList output;
        for (const QJsonValue &v : paths) {
            QString p = v.toString().trimmed();
            if (p.isEmpty()) continue;

            output << QString("=== %1 ===").arg(p);

            auto [abs, err] = resolveRead(p);
            if (!err.isEmpty()) {
                output << "ERROR: " + err;
                output << "";
                continue;
            }

            QFile file(abs);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                output << QString("ERROR: Cannot open file: %1").arg(p);
                output << "";
                continue;
            }

            QString content = QTextStream(&file).readAll();
            if (content.length() > MAX_READ_CHARS) {
                int cut = content.lastIndexOf('\n', MAX_READ_CHARS);
                if (cut < 0) cut = MAX_READ_CHARS;
                content = content.left(cut) + "\n[... truncated]";
            }
            output << content;
            output << "";
        }
        return ToolResult::ok(output.join('\n'));
    }
};
