#pragma once
// ─── ListSymbolsTool ──────────────────────────────────────────────────────────
// Listet alle Top-Level-Symbole einer C/C++/CMake-Datei mit Zeilennummern.
// Top-Level = direkte Kinder des Root-Knotens (keine geschachtelten Methoden).

#include "TreeSitterToolBase.h"

class ListSymbolsTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "list_symbols"; }

    QString description() const override
    {
        return
            "List all top-level symbols (functions, classes, structs, enums, typedefs) "
            "in a C/C++/CMake file with line numbers. "
            "Path can be relative to a known root or absolute.";
    }

    QJsonObject properties() const override
    {
        return {{"path", prop("string",
            "Relative or absolute path (.c .cpp .h .hpp CMakeLists.txt).")}};
    }

    QJsonArray required() const override { return req({"path"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        auto [abs, err] = resolveRead(args["path"].toString());
        if (!err.isEmpty()) return ToolResult::err(err);

        QString parseErr;
        ParseResult pr = parseFile(abs, parseErr);
        if (!pr.valid()) return ToolResult::err("Error: " + parseErr);

        TSNode root = ts_tree_root_node(pr.tree);
        QString output;
        uint32_t n = ts_node_child_count(root);

        for (uint32_t i = 0; i < n; i++) {
            TSNode child = ts_node_child(root, i);
            QString type = ts_node_type(child);

            if (type == "function_definition") {
                TSNode decl = childByField(child, "declarator");
                QString name;
                traverseTree(decl, [&](TSNode nd) {
                    if (name.isEmpty() &&
                        QString(ts_node_type(nd)) == "identifier")
                        name = nodeText(nd, pr.src);
                });
                if (!name.isEmpty())
                    output += QString("function  %1  (line %2)\n")
                              .arg(name).arg(nodeLine(child));

            } else if (type == "class_specifier") {
                TSNode nm = childByField(child, "name");
                if (nodeValid(nm))
                    output += QString("class     %1  (line %2)\n")
                              .arg(nodeText(nm, pr.src)).arg(nodeLine(child));

            } else if (type == "struct_specifier") {
                TSNode nm = childByField(child, "name");
                output += QString("struct    %1  (line %2)\n")
                          .arg(nodeValid(nm) ? nodeText(nm, pr.src)
                                             : "<anonymous>")
                          .arg(nodeLine(child));

            } else if (type == "enum_specifier") {
                TSNode nm = childByField(child, "name");
                output += QString("enum      %1  (line %2)\n")
                          .arg(nodeValid(nm) ? nodeText(nm, pr.src)
                                             : "<anonymous>")
                          .arg(nodeLine(child));

            } else if (type == "type_definition") {
                TSNode decl = childByField(child, "declarator");
                if (nodeValid(decl))
                    output += QString("typedef   %1  (line %2)\n")
                              .arg(nodeText(decl, pr.src)).arg(nodeLine(child));

            } else if (type == "normal_command" ||
                       type == "function_def"   ||
                       type == "macro_def") {
                TSNode nm = ts_node_child(child, 0);
                if (nodeValid(nm))
                    output += QString("cmake     %1  (line %2)\n")
                              .arg(nodeText(nm, pr.src)).arg(nodeLine(child));
            }
        }

        return output.isEmpty()
            ? ToolResult::ok("No symbols found.")
            : ToolResult::ok(output.trimmed());
    }
};
