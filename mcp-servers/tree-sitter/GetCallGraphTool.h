#pragma once
// ─── GetCallGraphTool ─────────────────────────────────────────────────────────
// Listet alle Funktionsaufrufe einer Funktion (eine Ebene tief).
// Erkennt: einfache Aufrufe, Methoden-Aufrufe (obj.func()), qualifizierte
// Aufrufe (Ns::func()).

#include "TreeSitterToolBase.h"

class GetCallGraphTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "get_call_graph"; }

    QString description() const override
    {
        return
            "List all function calls made by a given function (one level deep). "
            "Includes plain calls, method calls and qualified calls (Ns::func). "
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
        QString output;
        bool found = false;

        traverseTree(root, [&](TSNode node) {
            if (found) return;
            if (QString(ts_node_type(node)) != "function_definition") return;

            TSNode decl = childByField(node, "declarator");
            QString name;
            traverseTree(decl, [&](TSNode n) {
                if (name.isEmpty() &&
                    QString(ts_node_type(n)) == "identifier")
                    name = nodeText(n, pr.src);
            });
            if (name != funcName) return;

            found = true;
            output += QString("Call graph for %1() (line %2):\n")
                      .arg(funcName).arg(nodeLine(node));

            TSNode body = childByField(node, "body");
            if (!nodeValid(body)) { output += "  (no body)\n"; return; }

            QStringList calls;
            traverseTree(body, [&](TSNode cn) {
                if (QString(ts_node_type(cn)) != "call_expression") return;
                TSNode func = childByField(cn, "function");
                if (!nodeValid(func)) return;

                QString ct = ts_node_type(func);
                QString callName;
                // identifier          → einfacher Aufruf: foo()
                // field_expression    → Methoden-Aufruf:  obj.foo()
                // qualified_identifier → qualifiziert:    Ns::foo()
                if (ct == "identifier" ||
                    ct == "field_expression" ||
                    ct == "qualified_identifier")
                    callName = nodeText(func, pr.src);

                if (!callName.isEmpty() && !calls.contains(callName))
                    calls << QString("  %1  (line %2)")
                             .arg(callName).arg(nodeLine(cn));
            });

            output += calls.isEmpty()
                ? "  (no function calls found)\n"
                : calls.join("\n") + "\n";
        });

        return found
            ? ToolResult::ok(output.trimmed())
            : ToolResult::err(
                QString("Function '%1' not found.").arg(funcName));
    }
};
