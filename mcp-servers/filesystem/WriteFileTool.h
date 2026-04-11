#pragma once
// ─── WriteFileTool ────────────────────────────────────────────────────────────
// Schreibt (überschreibt) eine Datei in der Sandbox.
// Nur writable Roots erlaubt (PathPolicy::resolveWrite).
// Auto-Commit vor dem Schreiben.

#include "FilesystemToolBase.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

class WriteFileTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "write_file"; }

    QString description() const override
    {
        return
            "Write (overwrite) a file with new content. "
            "Creates parent directories automatically. "
            "Limited to 16384 characters. "
            "The previous state is committed to git before writing. "
            "Only allowed inside the sandbox (writable root). "
            "Use append_file to add to an existing file without overwriting.";
    }

    QJsonObject properties() const override
    {
        return {
            {"path",    prop("string", "Relative path to the file within the sandbox.")},
            {"content", prop("string", "Full content to write (max 16384 chars).")}
        };
    }

    QJsonArray required() const override { return req({"path", "content"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path    = args.value("path").toString();
        QString content = args.value("content").toString();
        if (path.isEmpty()) return ToolResult::err("Error: 'path' is required.");
        if (content.length() > MAX_WRITE_CHARS)
            return ToolResult::err(QString("Error: content too large (max %1 chars).").arg(MAX_WRITE_CHARS));

        auto [abs, err] = resolveWrite(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        m_git->autoCommit(abs, QString("before write_file %1").arg(path));

        QDir().mkpath(QFileInfo(abs).absolutePath());

        QFile file(abs);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return ToolResult::err(QString("Error: Cannot write: %1").arg(path));

        QTextStream(&file) << content;
        m_git->autoCommit(abs, QString("write_file %1").arg(path));
        return ToolResult::ok(QString("OK: %1 bytes written.").arg(content.length()));
    }
};
