#pragma once
// ─── GitCheckoutTool ──────────────────────────────────────────────────────────
// Führt git checkout auf einen Ref (Commit, Tag, Branch) aus.
// Remote-Operationen (push/pull/clone/fetch) sind in GitHelper::run() gesperrt.

#include "FilesystemToolBase.h"

class GitCheckoutTool : public FilesystemToolBase
{
public:
    using FilesystemToolBase::FilesystemToolBase;

    QString name() const override { return "git_checkout"; }

    QString description() const override
    {
        return
            "Check out a git ref (commit hash, tag, or branch) in the sandbox repository. "
            "Use 'HEAD~1' to go back one commit, 'HEAD~2' for two, etc. "
            "Network operations (push/pull/clone/fetch) are not allowed.";
    }

    QJsonObject properties() const override
    {
        return {
            {"ref", prop("string", "Git ref to check out (e.g. 'HEAD~1', a commit hash, or branch name).")}
        };
    }

    QJsonArray required() const override { return req({}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString ref      = args.value("ref").toString("HEAD~1");
        QString sandbox  = m_policy->primarySandbox();
        QString repoPath = m_git->repoFor(sandbox);
        auto [out, code] = m_git->run(repoPath, {"checkout", ref});
        return code == 0
            ? ToolResult::ok(QString("OK: checked out %1").arg(ref))
            : ToolResult::err(out);
    }
};
