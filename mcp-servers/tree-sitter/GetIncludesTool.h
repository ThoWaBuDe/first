#pragma once
// ─── GetIncludesTool ──────────────────────────────────────────────────────────
// Listet alle #include-Direktiven einer Datei mit Zeilennummern.

#include "TreeSitterToolBase.h"

class GetIncludesTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "get_includes"; }

    QString description() const override
    {
        return
            "List all #include directives with line numbers. "
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
            if (QString(ts_node_type(node)) == "preproc_include")
                output += QString("line %1: %2\n")
                          .arg(nodeLine(node))
                          .arg(nodeText(node, pr.src).trimmed());
        });

        return ToolResult::ok(output.isEmpty()
            ? "No includes found."
            : output.trimmed());
    }
};
