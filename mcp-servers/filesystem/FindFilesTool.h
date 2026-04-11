#pragma once
// ─── FindFilesTool ────────────────────────────────────────────────────────────
// Sucht Dateien nach Glob-Pattern (z.B. *.cpp, test_*.h).
// Gibt relative Pfade zurück, einen pro Zeile.

#include "FilesystemToolBase.h"
#include <QDirIterator>
#include <QFileInfo>

class FindFilesTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "find_files"; }

    QString description() const override
    {
        return
            "Find files matching a glob pattern (e.g. '*.cpp', 'test_*.h'). "
            "Returns relative paths, one per line. Searches recursively by default. "
            "Works in all known roots.";
    }

    QJsonObject properties() const override
    {
        return {
            {"pattern",   prop("string",  "Glob pattern, e.g. '*.cpp' or 'CMakeLists.txt'.")},
            {"path",      prop("string",  "Relative or absolute directory to search. Defaults to sandbox root.")},
            {"recursive", prop("boolean", "Search subdirectories recursively (default: true).")}
        };
    }

    QJsonArray required() const override { return req({"pattern"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString pattern   = args.value("pattern").toString();
        QString path      = args.value("path").toString(".");
        bool    recursive = args.value("recursive").toBool(true);
        if (pattern.isEmpty()) return ToolResult::err("Error: 'pattern' required.");

        auto [abs, err] = resolveRead(path);
        if (!err.isEmpty()) return ToolResult::err(err);

        QString rootPath = m_policy->primarySandbox();
        for (const auto &r : m_policy->roots())
            if (abs.startsWith(r.path) && r.path.length() > rootPath.length())
                rootPath = r.path;

        auto flags = recursive
                     ? QDirIterator::Subdirectories
                     : QDirIterator::NoIteratorFlags;
        QDirIterator it(abs, QStringList() << pattern,
                        QDir::Files | QDir::NoDotAndDotDot, flags);

        QStringList found;
        while (it.hasNext()) {
            QString f = it.next();
            if (QFileInfo(f).isSymLink()) continue;
            found << f.mid(rootPath.length() + 1);
        }
        return ToolResult::ok(found.isEmpty() ? "No files found." : found.join('\n'));
    }
};
