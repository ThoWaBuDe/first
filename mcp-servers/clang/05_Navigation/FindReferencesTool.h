#pragma once
// ─── FindReferencesTool ───────────────────────────────────────────────────────
// Navigation: Finde alle Referenzen auf ein Symbol.

#include "../ClangToolBase.h"

class FindReferencesTool : public ClangToolBase
{
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "find_references"; }

    QString description() const override
    {
        return "Find all references to a function, class, variable, or type. "
               "Returns list of file:line locations where the symbol is used.";
    }

    QJsonObject properties() const override
    {
        return {
            {"file", prop("string", "File to analyze")},
            {"symbol_name", prop("string", "Symbol name to find references for")},
            {"line", prop("integer", "Line number (if using cursor position)")},
            {"column", prop("integer", "Column number (if using cursor position)")}
        };
    }

    QJsonArray required() const override { return {"file", "symbol_name"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString file = args.value("file").toString();
        QString symbolName = args.value("symbol_name").toString();

        if (file.isEmpty() || symbolName.isEmpty())
            return ToolResult::err("Error: 'file' and 'symbol_name' are required.");

        if (!parseFile(file))
            return ToolResult::err("Error: Could not parse file: " + m_clang->diagnostics().join("\n"));

        QString usr;
        
        auto funcs = m_clang->getAllFunctions();
        for (const auto &f : funcs) {
            if (f.name == symbolName || f.displayName == symbolName) {
                usr = f.usr;
                break;
            }
        }
        
        if (usr.isEmpty()) {
            auto classes = m_clang->getAllClasses();
            for (const auto &c : classes) {
                if (c.name == symbolName || c.displayName == symbolName) {
                    usr = c.usr;
                    break;
                }
            }
        }
        
        if (usr.isEmpty()) {
            auto vars = m_clang->getAllVariables();
            for (const auto &v : vars) {
                if (v.name == symbolName || v.displayName == symbolName) {
                    usr = v.usr;
                    break;
                }
            }
        }

        if (usr.isEmpty())
            return ToolResult::err(QString("Symbol '%1' not found in file.").arg(symbolName));

        auto refs = m_clang->findReferences(symbolName, usr);
        
        if (refs.isEmpty())
            return ToolResult::ok(QString("No references found for '%1'.").arg(symbolName));

        QString result = QString("References to '%1':\n").arg(symbolName);
        for (const auto &ref : refs) {
            result += QString("  %1:%2\n").arg(ref.file).arg(ref.line);
        }
        
        return ToolResult::ok(result.trimmed());
    }
};