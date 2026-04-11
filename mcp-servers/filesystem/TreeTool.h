#pragma once
// ─── TreeTool ─────────────────────────────────────────────────────────────────
// Zeigt Verzeichnisstruktur als ASCII-Baum.
// .git wird ausgeblendet. Symlinks werden angezeigt, aber nicht verfolgt.

#include "FilesystemToolBase.h"
#include <QDir>
#include <QFileInfo>

class TreeTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "tree"; }

    QString description() const override
    {
        return
            "Show the directory structure as an ASCII tree. "
            "Symlinks are shown but not followed. The .git directory is hidden. "
            "Works in all known roots (sandbox and read-only sources).";
    }

    QJsonObject properties() const override
    {
        return {
            {"path",      prop("string",  "Relative or absolute path. Defaults to sandbox root.")},
            {"max_depth", prop("integer", "Maximum depth to traverse (1-10, default 6).")}
        };
    }

    QJsonArray required() const override { return req({}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path     = args.value("path").toString(".");
        int     maxDepth = qBound(1, args.value("max_depth").toInt(MAX_TREE_DEPTH), 10);

        auto [abs, err] = resolveRead(path);
        if (!err.isEmpty()) return ToolResult::err(err);
        if (!QDir(abs).exists()) return ToolResult::err("Error: Directory does not exist.");

        QStringList lines;
        QString label = (path == "." || path.isEmpty())
                        ? "."
                        : QFileInfo(abs).fileName();
        lines << label + "/";
        buildTree(abs, "", lines, 1, maxDepth);
        return ToolResult::ok(lines.join('\n'));
    }

private:
    static void buildTree(const QString &dirPath, const QString &prefix,
                          QStringList &lines, int depth, int maxDepth)
    {
        if (depth > maxDepth) { lines << prefix + "..."; return; }

        QDir dir(dirPath);
        QStringList entries = dir.entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
            QDir::Name | QDir::DirsFirst);
        entries.removeAll(".git");

        for (int i = 0; i < entries.size(); ++i) {
            bool    isLast = (i == entries.size() - 1);
            QString name   = entries[i];
            QString full   = dirPath + "/" + name;
            QFileInfo fi(full);

            QString branch = isLast ? "└── " : "├── ";
            QString indent = isLast ? "    " : "│   ";

            if (fi.isSymLink()) {
                lines << prefix + branch + name + " [symlink]";
            } else if (fi.isDir()) {
                lines << prefix + branch + name + "/";
                buildTree(full, prefix + indent, lines, depth + 1, maxDepth);
            } else {
                lines << prefix + branch + name;
            }
        }
    }
};
