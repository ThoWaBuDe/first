#pragma once
// ─── UnusedIncludesTool ────────────────────────────────────────────────────────

#include "../ClangToolBase.h"

class UnusedIncludesTool : public ClangToolBase
{
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "unused_includes"; }

    QString description() const override
    {
        return "Find unused #include directives in a file.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File to analyze")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString file = args.value("file").toString();
        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto includes = m_clang->getAllIncludes();
        if (includes.isEmpty()) {
            return ToolResult::ok("No #include directives found.");
        }

        QString result = "Found #includes:\n";
        for (const auto &inc : includes) {
            result += "  " + inc.displayName + "\n";
        }
        result += "\nNote: Use clang -E -H to verify which are actually used.";

        return ToolResult::ok(result.trimmed());
    }
};