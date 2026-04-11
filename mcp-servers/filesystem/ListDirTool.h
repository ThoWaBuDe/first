#pragma once
// ─── ListDirTool ──────────────────────────────────────────────────────────────
// Listet den Inhalt eines Verzeichnisses auf.
// Tags: [D] Directory, [F] File, [L] Symlink.
// Lesezugriff auf alle bekannten Roots (auch read-only).

#include "FilesystemToolBase.h"
#include <QDir>
#include <QFileInfo>

class ListDirTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "list_dir"; }

    QString description() const override
    {
        return
            "List the contents of a directory. Returns one entry per line with a "
            "type tag: [D] directory, [F] file, [L] symlink. "
            "Works in all known roots (sandbox and read-only sources). "
            "Defaults to the sandbox root if no path is given.";
    }

    QJsonObject properties() const override
    {
        return {
            {"path", prop("string", "Relative or absolute path to directory. Defaults to sandbox root.")}
        };
    }

    QJsonArray required() const override { return req({}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path = args.value("path").toString(".");

        auto [abs, err] = resolveRead(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        QDir dir(abs);
        if (!dir.exists()) return ToolResult::err("Error: Directory does not exist.");

        QStringList entries = dir.entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot,
            QDir::Name | QDir::DirsFirst);

        if (entries.isEmpty()) return ToolResult::ok("(empty directory)");

        QStringList result;
        for (const QString &name : entries) {
            QFileInfo fi(abs + "/" + name);
            QString tag = fi.isSymLink() ? "[L]" : (fi.isDir() ? "[D]" : "[F]");
            result << QString("%1 %2").arg(tag, name);
        }
        return ToolResult::ok(result.join('\n'));
    }
};
