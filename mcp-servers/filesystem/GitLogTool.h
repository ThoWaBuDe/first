#pragma once
// ─── GitLogTool ───────────────────────────────────────────────────────────────
// Zeigt die letzten N Commits (kurzer Hash + Nachricht).

#include "FilesystemToolBase.h"

class GitLogTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "git_log"; }

    QString description() const override
    {
        return
            "Show the last N git commits (one per line: short hash + message). "
            "Defaults to 10 commits.";
    }

    QJsonObject properties() const override
    {
        return {
            {"n", prop("integer", "Number of commits to show (1-50, default 10).")}
        };
    }

    QJsonArray required() const override { return req({}); }

    ToolResult execute(const QJsonObject &args) override
    {
        int n = qBound(1, args.value("n").toInt(10), 50);
        QString sandbox  = m_policy->primarySandbox();
        QString repoPath = m_git->repoFor(sandbox);
        auto [out, code] = m_git->run(repoPath,
            {"log", "--oneline", QString("-%1").arg(n)});
        return code == 0
            ? ToolResult::ok(out.isEmpty() ? "No commits yet." : out)
            : ToolResult::err(out);
    }
};
