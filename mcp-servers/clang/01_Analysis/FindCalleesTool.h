#pragma once
// ─── FindCalleesTool ─────────────────────────────────────────────────────────
// Analysis: Welche Funktionen ruft diese Funktion auf?

#include "../ClangToolBase.h"

class FindCalleesTool : public ClangToolBase
{
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "find_callees"; }

    QString description() const override
    {
        return "Find all functions called by the specified function. "
               "Shows the call graph (downward).";
    }

    QJsonObject properties() const override
    {
        return {
            {"file", prop("string", "File to analyze")},
            {"function_name", prop("string", "Function name to find callees for")}
        };
    }

    QJsonArray required() const override { return {"file", "function_name"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString file = args.value("file").toString();
        QString funcName = args.value("function_name").toString();

        if (file.isEmpty() || funcName.isEmpty())
            return ToolResult::err("Error: 'file' and 'function_name' are required.");

        if (!parseFile(file))
            return ToolResult::err("Error: Could not parse file.");

        QString usr;
        auto funcs = m_clang->getAllFunctions();
        for (const auto &f : funcs) {
            if (f.name == funcName || f.displayName == funcName) {
                usr = f.usr;
                break;
            }
        }

        if (usr.isEmpty())
            return ToolResult::err(QString("Function '%1' not found.").arg(funcName));

        auto callees = m_clang->getCallees(usr);
        
        if (callees.isEmpty())
            return ToolResult::ok(QString("No callees found for '%1'.").arg(funcName));

        return ToolResult::ok(formatSymbols(callees, QString("Callees of %1").arg(funcName)));
    }
};