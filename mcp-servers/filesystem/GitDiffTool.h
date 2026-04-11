#pragma once
// ─── GitDiffTool ──────────────────────────────────────────────────────────────
// Zeigt unified diff aller uncommitted Änderungen (working tree gegen HEAD).

#include "FilesystemToolBase.h"

class GitDiffTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "git_diff"; }

    QString description() const override
    {
        return
            "Show the unified diff of all uncommitted changes relative to HEAD "
            "in the sandbox repository.";
    }

    QJsonObject properties() const override { return {}; }
    QJsonArray  required()   const override { return req({}); }

    ToolResult execute(const QJsonObject &) override
    {
        QString sandbox  = m_policy->primarySandbox();
        QString repoPath = m_git->repoFor(sandbox);
        auto [out, code] = m_git->run(repoPath, {"diff", "HEAD"});
        return code == 0
            ? ToolResult::ok(out.isEmpty() ? "No differences." : out)
            : ToolResult::err(out);
    }
};
