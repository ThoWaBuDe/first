#pragma once
// ─── GetClassHierarchyTool ────────────────────────────────────────────────────
// Zeigt alle Klassen einer Datei mit ihren Basisklassen.
// Erkennt: class Foo : public Bar, protected Baz { ... }

#include "TreeSitterToolBase.h"

class GetClassHierarchyTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "get_class_hierarchy"; }

    QString description() const override
    {
        return
            "Show all classes in a file and their base classes. "
            "Qt classes (Q_OBJECT, signals, slots) are supported. "
            "Path can be relative to a known root or absolute.";
    }

    QJsonObject properties() const override
    {
        return {{"path", prop("string",
            "Relative or absolute path to the source file.")}};
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

        traverseTree(root, [&](TSNode node) {
            if (QString(ts_node_type(node)) != "class_specifier") return;

            TSNode nameNode = childByField(node, "name");
            QString className = nodeValid(nameNode)
                                ? nodeText(nameNode, pr.src)
                                : "<anonymous>";

            // Basisklassen aus base_class_clause sammeln.
            // Jeder type_identifier darin ist eine Basisklasse.
            QStringList bases;
            uint32_t n = ts_node_child_count(node);
            for (uint32_t i = 0; i < n; i++) {
                TSNode child = ts_node_child(node, i);
                if (QString(ts_node_type(child)) == "base_class_clause") {
                    traverseTree(child, [&](TSNode bc) {
                        if (QString(ts_node_type(bc)) == "type_identifier")
                            bases << nodeText(bc, pr.src);
                    });
                }
            }

            output += bases.isEmpty()
                ? QString("class %1  (line %2)  — no base classes\n")
                  .arg(className).arg(nodeLine(node))
                : QString("class %1  (line %2)  : %3\n")
                  .arg(className).arg(nodeLine(node)).arg(bases.join(", "));
        });

        return ToolResult::ok(output.isEmpty()
            ? "No classes found."
            : output.trimmed());
    }
};
