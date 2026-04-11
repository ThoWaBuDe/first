#pragma once
// ─── MoveFileTool ─────────────────────────────────────────────────────────────
// Verschiebt/benennt eine Datei oder ein Verzeichnis innerhalb der Sandbox.
// Zieldatei wird in den Papierkorb verschoben falls sie existiert.
//
// Neu: Trash-Pfad via LLAMAQT_TRASH Umgebungsvariable konfigurierbar.
// Default: ~/.llamatools_trash

#include "FilesystemToolBase.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>

class MoveFileTool : public FilesystemToolBase
{
public:
    explicit MoveFileTool(PathPolicy *policy, GitHelper *git, const QString &trashRoot)
        : FilesystemToolBase(policy, git), m_trashRoot(trashRoot)
    {}

    QString name() const override { return "move_file"; }

    QString description() const override
    {
        return
            "Move or rename a file or directory within the sandbox. "
            "If the destination already exists, it is moved to the trash "
            "before the move. The previous state is committed to git. "
            "Only allowed inside the sandbox (writable root).";
    }

    QJsonObject properties() const override
    {
        return {
            {"source",      prop("string", "Relative path of the file to move.")},
            {"destination", prop("string", "Relative destination path.")}
        };
    }

    QJsonArray required() const override { return req({"source", "destination"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString src = args.value("source").toString();
        QString dst = args.value("destination").toString();
        if (src.isEmpty() || dst.isEmpty())
            return ToolResult::err("Error: source and destination required.");

        auto [srcAbs, srcErr] = resolveWrite(src);
        if (!srcErr.isEmpty()) return ToolResult::err(srcErr);
        auto [dstAbs, dstErr] = resolveWrite(dst);
        if (!dstErr.isEmpty()) return ToolResult::err(dstErr);

        if (!QFileInfo(srcAbs).exists())
            return ToolResult::err("Error: Source not found.");

        m_git->autoCommit(srcAbs, QString("before move %1").arg(src));

        // Zieldatei in Papierkorb, falls vorhanden
        if (QFileInfo(dstAbs).exists()) {
            QString trashPath = trashPathFor(dst);
            QDir().mkpath(QFileInfo(trashPath).absolutePath());
            if (!QFile::rename(dstAbs, trashPath))
                return ToolResult::err(
                    QString("Error: Could not move existing destination to trash: %1").arg(dst));
        }

        if (QFile::rename(srcAbs, dstAbs)) {
            m_git->autoCommit(dstAbs, QString("move %1 to %2").arg(src, dst));
            return ToolResult::ok(QString("OK: Moved %1 → %2").arg(src, dst));
        }
        return ToolResult::err("Error: Move failed.");
    }

private:
    QString m_trashRoot;

    QString trashPathFor(const QString &originalRel) const
    {
        QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        QString safeName  = QFileInfo(originalRel).fileName().replace("/", "_");
        return m_trashRoot + "/" + timestamp + "_" + safeName;
    }
};
