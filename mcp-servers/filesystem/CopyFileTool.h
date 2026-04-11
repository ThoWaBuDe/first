#pragma once
// ─── CopyFileTool ─────────────────────────────────────────────────────────────
// Kopiert eine Datei innerhalb der Sandbox.
// Ziel darf nicht existieren (kein stilles Überschreiben).
// Quelle kann auch aus read-only Roots gelesen werden.

#include "FilesystemToolBase.h"
#include <QFile>
#include <QFileInfo>

class CopyFileTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "copy_file"; }

    QString description() const override
    {
        return
            "Copy a file to a new location within the sandbox. "
            "The source can be in any known root (including read-only). "
            "The destination must be inside the sandbox (writable root) "
            "and must not already exist. "
            "The copy is committed to git automatically.";
    }

    QJsonObject properties() const override
    {
        return {
            {"source",      prop("string", "Relative or absolute path of the source file.")},
            {"destination", prop("string", "Relative path for the copy (must not already exist).")}
        };
    }

    QJsonArray required() const override { return req({"source", "destination"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString src = args.value("source").toString();
        QString dst = args.value("destination").toString();
        if (src.isEmpty() || dst.isEmpty())
            return ToolResult::err("Error: source and destination required.");

        // Quelle: Lesen aus allen bekannten Roots erlaubt
        auto [srcAbs, srcErr] = resolveRead(src);
        if (!srcErr.isEmpty()) return ToolResult::err(srcErr);

        // Ziel: nur in writable Root (Sandbox)
        auto [dstAbs, dstErr] = resolveWrite(dst);
        if (!dstErr.isEmpty()) return ToolResult::err(dstErr);

        if (!QFileInfo(srcAbs).exists())
            return ToolResult::err(QString("Error: Source not found: %1").arg(src));

        if (QFile::copy(srcAbs, dstAbs)) {
            m_git->autoCommit(dstAbs, QString("copy %1 to %2").arg(src, dst));
            return ToolResult::ok(QString("OK: Copied %1 → %2").arg(src, dst));
        }
        return ToolResult::err("Error: Copy failed (destination may already exist).");
    }
};
