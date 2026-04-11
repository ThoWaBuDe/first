#pragma once
// ─── VariableRefsTool ─────────────────────────────────────────────────────────
// Analysis: Wo wird eine Variable definiert/verwendet?

#include "../ClangToolBase.h"

class VariableRefsTool : public ClangToolBase
{
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "variable_refs"; }

    QString description() const override
    {
        return "Find where a variable is defined and used.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"variable_name", prop("string", "Variable name")}};
    }

    QJsonArray required() const override { return {"file", "variable_name"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString file = args.value("file").toString();
        QString varName = args.value("variable_name").toString();
        if (file.isEmpty() || varName.isEmpty()) return ToolResult::err("Error: file and variable_name required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto vars = m_clang->getAllVariables();
        QString result = QString("References to '%1':\n").arg(varName);
        for (const auto &v : vars) {
            if (v.name == varName || v.displayName == varName) {
                result += QString("  %1:%2 (%3)\n").arg(v.file).arg(v.line).arg(v.kind);
            }
        }
        return ToolResult::ok(result.trimmed());
    }
};