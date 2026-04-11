#pragma once
// ─── TypeHierarchyTool ─────────────────────────────────────────────────────────
// Navigation: Type-Hierarchie anzeigen.

#include "../ClangToolBase.h"

class TypeHierarchyTool : public ClangToolBase
{
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "type_hierarchy"; }

    QString description() const override
    {
        return "Show type hierarchy for a class or struct.";
    }

    QJsonObject properties() const override
    {
        return {
            {"file", prop("string", "File to analyze")},
            {"class_name", prop("string", "Class name")}
        };
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString file = args.value("file").toString();
        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto classes = m_clang->getAllClasses();
        QString result = "Type Hierarchy:\n";
        for (const auto &c : classes) {
            result += QString("  %1 (%2) - %3:%4\n")
                .arg(c.displayName).arg(c.kind).arg(c.file).arg(c.line);
        }

        return ToolResult::ok(result.trimmed());
    }
};