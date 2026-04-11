#pragma once
// ─── GoToDeclarationTool ───────────────────────────────────────────────────────
// Navigation: Springe zur Deklaration.

#include "../ClangToolBase.h"

class GoToDeclarationTool : public ClangToolBase
{
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "go_to_declaration"; }

    QString description() const override
    {
        return "Navigate to the declaration of a symbol.";
    }

    QJsonObject properties() const override
    {
        return {
            {"file", prop("string", "File path")},
            {"line", prop("integer", "Line number")},
            {"column", prop("integer", "Column number")},
            {"symbol_name", prop("string", "Symbol name")}
        };
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString file = args.value("file").toString();
        QString symbolName = args.value("symbol_name").toString();

        if (file.isEmpty())
            return ToolResult::err("Error: 'file' is required.");

        if (!parseFile(file))
            return ToolResult::err("Error: Could not parse file.");

        if (!symbolName.isEmpty()) {
            auto funcs = m_clang->getAllFunctions();
            for (const auto &f : funcs) {
                if (f.name == symbolName) {
                    return ToolResult::ok(QString("Declaration: %1:%2").arg(f.file).arg(f.line));
                }
            }
        }

        return ToolResult::ok("Note: Use go_to_definition for full navigation.");
    }
};