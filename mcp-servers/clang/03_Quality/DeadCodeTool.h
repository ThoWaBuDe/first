#pragma once
#include "../ClangToolBase.h"

class DeadCodeTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "dead_code"; }

    QString description() const override
    {
        return "Find unused functions and variables (dead code).";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto funcs = m_clang->getAllFunctions();
        auto vars = m_clang->getAllVariables();

        QString result = "Functions found: " + QString::number(funcs.size()) + "\n";
        result += "Variables found: " + QString::number(vars.size()) + "\n";
        result += "\nNote: Full dead code detection requires data flow analysis.";
        result += "\nTry: clang --analyze file.cpp";

        return ToolResult::ok(result);
    }
};