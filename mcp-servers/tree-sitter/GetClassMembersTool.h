#pragma once
// ─── GetClassMembersTool ──────────────────────────────────────────────────────
// Listet alle Felder und Methoden einer Klasse oder Struct mit Zeilennummern
// und Access-Specifiers (public/protected/private).
// Qt-Klassen (Q_OBJECT, signals:, slots:) werden unterstützt.

#include "TreeSitterToolBase.h"

class GetClassMembersTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "get_class_members"; }

    QString description() const override
    {
        return
            "List all fields and methods of a class or struct with line numbers "
            "and access specifiers (public/protected/private). "
            "Qt classes (Q_OBJECT, signals, slots) are supported. "
            "Path can be relative to a known root or absolute.";
    }

    QJsonObject properties() const override
    {
        return {
            {"path",  prop("string", "Relative or absolute path to the source file.")},
            {"class", prop("string", "Exact class or struct name.")}
        };
    }

    QJsonArray required() const override { return req({"path", "class"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        auto [abs, err] = resolveRead(args["path"].toString());
        if (!err.isEmpty()) return ToolResult::err(err);

        QString className = args["class"].toString();
        if (className.isEmpty())
            return ToolResult::err("Error: 'class' is required.");

        QString parseErr;
        ParseResult pr = parseFile(abs, parseErr);
        if (!pr.valid()) return ToolResult::err("Error: " + parseErr);

        TSNode root = ts_tree_root_node(pr.tree);
        QString output;

        traverseTree(root, [&](TSNode node) {
            if (!output.isEmpty()) return;
            QString type = ts_node_type(node);
            if (type != "class_specifier" && type != "struct_specifier") return;

            TSNode nameNode = childByField(node, "name");
            if (!nodeValid(nameNode) ||
                nodeText(nameNode, pr.src) != className) return;

            TSNode body = childByField(node, "body");
            if (!nodeValid(body)) return;

            output += QString("Members of %1 (line %2):\n")
                      .arg(className).arg(nodeLine(node));

            uint32_t n = ts_node_child_count(body);
            for (uint32_t i = 0; i < n; i++) {
                TSNode m  = ts_node_child(body, i);
                QString mt = ts_node_type(m);

                if (mt == "field_declaration") {
                    TSNode decl = childByField(m, "declarator");
                    QString nm;
                    if (nodeValid(decl))
                        traverseTree(decl, [&](TSNode nd) {
                            if (nm.isEmpty() &&
                                QString(ts_node_type(nd)) == "field_identifier")
                                nm = nodeText(nd, pr.src);
                        });
                    if (!nm.isEmpty())
                        output += QString("  field     %1  (line %2)\n")
                                  .arg(nm).arg(nodeLine(m));

                } else if (mt == "declaration" || mt == "function_definition") {
                    TSNode decl = childByField(m, "declarator");
                    QString nm;
                    traverseTree(decl, [&](TSNode nd) {
                        if (nm.isEmpty() &&
                            QString(ts_node_type(nd)) == "identifier")
                            nm = nodeText(nd, pr.src);
                    });
                    if (!nm.isEmpty()) {
                        QString tag = (mt == "function_definition")
                                      ? " [inline]" : "";
                        output += QString("  method    %1  (line %2)%3\n")
                                  .arg(nm).arg(nodeLine(m)).arg(tag);
                    }

                } else if (mt == "access_specifier") {
                    output += QString("  [%1]\n")
                              .arg(nodeText(m, pr.src).trimmed());
                }
            }
        });

        return output.isEmpty()
            ? ToolResult::err(QString("Class '%1' not found.").arg(className))
            : ToolResult::ok(output.trimmed());
    }
};
