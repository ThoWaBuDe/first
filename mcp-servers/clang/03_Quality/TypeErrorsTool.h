#pragma once
#include "../ClangToolBase.h"

class TypeErrorsTool : public ClangToolBase {
public: using ClangToolBase::ClangToolBase;
    QString name() const override { return "type_errors"; }
    QString description() const override { return "Show compiler diagnostics and type errors."; }
    QJsonObject properties() const override { return {{"file", prop("string", "File")}}; }
    QJsonArray required() const override { return {"file"}; }
    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");
        QString result = "Diagnostics:\n";
        for (const auto &d : m_clang->diagnostics()) result += "  " + d + "\n";
        return ToolResult::ok(result.trimmed());
    }
};