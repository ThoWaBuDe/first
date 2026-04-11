#pragma once
// ─── AppendFileTool ───────────────────────────────────────────────────────────
// Hängt Text ans Ende einer Datei an (oder erstellt sie).
// Nur writable Roots. Auto-Commit nach dem Anhängen.

#include "FilesystemToolBase.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

class AppendFileTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "append_file"; }

    QString description() const override
    {
        return
            "Append text to the end of an existing file, or create it if it does not exist. "
            "Limited to 16384 characters per call. "
            "Only allowed inside the sandbox (writable root). "
            "The appended content is committed to git automatically.";
    }

    QJsonObject properties() const override
    {
        return {
            {"path",    prop("string", "Relative path to the file.")},
            {"content", prop("string", "Text to append (max 16384 chars).")}
        };
    }

    QJsonArray required() const override { return req({"path", "content"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path    = args.value("path").toString();
        QString content = args.value("content").toString();
        if (path.isEmpty()) return ToolResult::err("Error: 'path' is required.");
        if (content.length() > MAX_WRITE_CHARS)
            return ToolResult::err("Error: content too large.");

        auto [abs, err] = resolveWrite(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        QDir().mkpath(QFileInfo(abs).absolutePath());

        QFile file(abs);
        if (!file.open(QIODevice::Append | QIODevice::Text))
            return ToolResult::err(QString("Error: Cannot append to: %1").arg(path));

        QTextStream(&file) << content;
        m_git->autoCommit(abs, QString("append_file %1").arg(path));
        return ToolResult::ok(QString("OK: %1 bytes appended.").arg(content.length()));
    }
};
