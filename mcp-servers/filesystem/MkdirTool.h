#pragma once
// ─── MkdirTool ────────────────────────────────────────────────────────────────
// Erstellt ein Verzeichnis (inkl. Elternverzeichnisse).
// Nur in writable Roots (Sandbox).

#include "FilesystemToolBase.h"
#include <QDir>

class MkdirTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "mkdir"; }

    QString description() const override
    {
        return
            "Create a directory (including all parent directories). "
            "Does nothing if the directory already exists. "
            "Only allowed inside the sandbox (writable root).";
    }

    QJsonObject properties() const override
    {
        return {
            {"path", prop("string", "Relative path of the directory to create.")}
        };
    }

    QJsonArray required() const override { return req({"path"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path = args.value("path").toString();
        if (path.isEmpty()) return ToolResult::err("Error: 'path' is required.");

        auto [abs, err] = resolveWrite(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        if (QDir(abs).exists())
            return ToolResult::ok(QString("OK: '%1' already exists.").arg(path));

        if (QDir().mkpath(abs))
            return ToolResult::ok(QString("OK: Directory created: %1").arg(path));

        return ToolResult::err(QString("Error: Could not create %1").arg(path));
    }
};
