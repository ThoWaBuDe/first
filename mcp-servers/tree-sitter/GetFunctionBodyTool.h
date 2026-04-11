#pragma once
// ─── GetFunctionBodyTool ──────────────────────────────────────────────────────
// Extrahiert den vollständigen Quelltext einer benannten Funktion.
// Gibt Startzeile, Endzeile und den vollständigen Funktionstext zurück.

#include "TreeSitterToolBase.h"

class GetFunctionBodyTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "get_function_body"; }

    QString description() const override
    {
        return
            "Extract the complete source of a named function. "
            "Returns the function body with start and end line numbers. "
            "Path can be relative to a known root or absolute.";
    }

    QJsonObject properties() const override
    {
        return {
            {"path",     prop("string", "Relative or absolute path to the source file.")},
            {"function", prop("string", "Exact function name.")}
        };
    }

    QJsonArray required() const override { return req({"path", "function"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        auto [abs, err] = resolveRead(args["path"].toString());
        if (!err.isEmpty()) return ToolResult::err(err);

        QString funcName = args["function"].toString();
        if (funcName.isEmpty())
            return ToolResult::err("Error: 'function' is required.");

        QString parseErr;
        ParseResult pr = parseFile(abs, parseErr);
        if (!pr.valid()) return ToolResult::err("Error: " + parseErr);

        TSNode root = ts_tree_root_node(pr.tree);
        QString result;

        traverseTree(root, [&](TSNode node) {
            if (!result.isEmpty()) return;
            if (QString(ts_node_type(node)) != "function_definition") return;

            // Funktionsname aus dem declarator-Teilbaum extrahieren.
            // Der declarator enthält den Namen tief verschachtelt —
            // wir traversieren ihn bis wir das erste 'identifier' finden.
            TSNode decl = childByField(node, "declarator");
            QString found;
            traverseTree(decl, [&](TSNode n) {
                if (found.isEmpty() &&
                    QString(ts_node_type(n)) == "identifier")
                    found = nodeText(n, pr.src);
            });

            if (found != funcName) return;

            int sl = nodeLine(node);
            int el = static_cast<int>(ts_node_end_point(node).row) + 1;
            result = QString("// %1() — lines %2–%3\n")
                     .arg(funcName).arg(sl).arg(el);
            result += nodeText(node, pr.src);
        });

        return result.isEmpty()
            ? ToolResult::err(QString("Function '%1' not found.").arg(funcName))
            : ToolResult::ok(result);
    }
};
