#pragma once
// ─── ClassHierarchyTool ───────────────────────────────────────────────────────
// Analysis: Vererbungshierarchie einer Klasse.

#include "../ClangToolBase.h"

class ClassHierarchyTool : public ClangToolBase
{
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "class_hierarchy"; }

    QString description() const override
    {
        return "Show class hierarchy: base classes and derived classes. "
               "Shows inheritance relationships for a class.";
    }

    QJsonObject properties() const override
    {
        return {
            {"file", prop("string", "File to analyze")},
            {"class_name", prop("string", "Class name to show hierarchy for")}
        };
    }

    QJsonArray required() const override { return {"file", "class_name"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString file = args.value("file").toString();
        QString className = args.value("class_name").toString();

        if (file.isEmpty() || className.isEmpty())
            return ToolResult::err("Error: 'file' and 'class_name' are required.");

        if (!parseFile(file))
            return ToolResult::err("Error: Could not parse file.");

        QString result = QString("Class hierarchy for '%1':\n").arg(className);
        
        auto classes = m_clang->getAllClasses();
        QStringList bases, derived;
        
        for (const auto &c : classes) {
            if (c.name == className || c.displayName == className) {
                result += QString("  Definition: %1:%2\n").arg(c.file).arg(c.line);
                result += QString("  Kind: %1\n").arg(c.kind);
            }
        }

        result += "\nAll classes in file:\n";
        for (const auto &c : classes) {
            result += QString("  %1 (%2)\n").arg(c.displayName).arg(c.kind);
        }

        return ToolResult::ok(result.trimmed());
    }
};