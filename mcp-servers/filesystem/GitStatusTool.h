#pragma once
// ─── GitStatusTool ────────────────────────────────────────────────────────────
// Zeigt git status (Branch + geänderte/ungetrackte Dateien) der Sandbox.

#include "FilesystemToolBase.h"

class GitStatusTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "git_status"; }

    QString description() const override
    {
        return
            "Show the current git status (branch and modified/untracked files) "
            "for the sandbox repository.";
    }

    QJsonObject properties() const override { return {}; }
    QJsonArray  required()   const override { return req({}); }

    ToolResult execute(const QJsonObject &) override
    {
        QString sandbox  = m_policy->primarySandbox();
        QString repoPath = m_git->repoFor(sandbox);
        auto [out, code] = m_git->run(repoPath, {"status", "--short", "--branch"});
        return code == 0
            ? ToolResult::ok(out.isEmpty() ? "Working tree clean." : out)
            : ToolResult::err(out);
    }
};
