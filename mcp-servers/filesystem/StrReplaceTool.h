#pragma once
// ─── StrReplaceTool ───────────────────────────────────────────────────────────
// Ersetzt einen eindeutigen String in einer Datei.
// old_str muss genau einmal vorkommen — sonst Fehler.
// Auto-Commit vor und nach der Änderung.

#include "FilesystemToolBase.h"
#include <QFile>
#include <QTextStream>

class StrReplaceTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "str_replace"; }

    QString description() const override
    {
        return
            "Replace a unique string in a file with a new string. "
            "old_str must appear exactly once in the file — if it appears multiple times, "
            "add more surrounding lines as context until it is unique. "
            "new_str defaults to empty string (deletion) if omitted. "
            "Only allowed inside the sandbox (writable root). "
            "Prefer this tool for small, targeted edits; use patch_file for larger changes.";
    }

    QJsonObject properties() const override
    {
        return {
            {"path",    prop("string", "Relative path to the file.")},
            {"old_str", prop("string", "Exact string to find (must be unique in the file).")},
            {"new_str", prop("string", "Replacement string. Omit to delete old_str.")}
        };
    }

    QJsonArray required() const override { return req({"path", "old_str"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path   = args.value("path").toString();
        QString oldStr = args.value("old_str").toString();
        QString newStr = args.value("new_str").toString();  // default: leer = löschen
        if (path.isEmpty() || oldStr.isEmpty())
            return ToolResult::err("Error: path and old_str required.");

        auto [abs, err] = resolveWrite(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        QFile file(abs);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return ToolResult::err(QString("Error: Cannot read: %1").arg(path));

        QString content = QTextStream(&file).readAll();
        file.close();

        if (!content.contains(oldStr))
            return ToolResult::err(QString("Error: old_str not found in %1.").arg(path));
        if (content.count(oldStr) > 1)
            return ToolResult::err(
                QString("Error: old_str appears %1 times in %2. Add more context.")
                    .arg(content.count(oldStr)).arg(path));

        m_git->autoCommit(abs, QString("before str_replace %1").arg(path));
        content.replace(oldStr, newStr);

        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return ToolResult::err("Error: Cannot write file.");
        QTextStream(&file) << content;

        m_git->autoCommit(abs, QString("str_replace %1").arg(path));
        return ToolResult::ok("OK: Replacement done.");
    }
};
