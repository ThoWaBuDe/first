#pragma once
// ─── CheckSyntaxTool ──────────────────────────────────────────────────────────
// Prüft eine Quelldatei auf Syntaxfehler via tree-sitter.
//
// tree-sitter ist fehlerresistent — es parst immer einen vollständigen Baum,
// markiert aber fehlerhafte Stellen als ERROR- oder MISSING-Knoten:
//   ERROR   = unerwartetes Token (der Parser wusste nicht was er damit soll)
//   MISSING = erwartetes Token fehlt (der Parser hat es "erfunden")
//
// Wichtig: das ist kein vollständiger Compiler-Check.
// tree-sitter prüft nur Syntax, nicht Typen, Deklarationen etc.

#include "TreeSitterToolBase.h"

class CheckSyntaxTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "check_syntax"; }

    QString description() const override
    {
        return
            "Check a source file for syntax errors using tree-sitter. "
            "Reports ERROR nodes (unexpected tokens) and MISSING nodes "
            "(tokens the parser had to invent) with line and column numbers. "
            "Note: syntax check only — not a full compiler check (no type checking). "
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
        int errorCount = 0;

        traverseTree(root, [&](TSNode node) {
            if (ts_node_is_error(node)) {
                TSPoint sp = ts_node_start_point(node);
                // Snippet: erste 60 Zeichen, Zeilenumbrüche entfernen
                QString snippet = nodeText(node, pr.src)
                                  .left(60).replace('\n', ' ');
                output += QString("ERROR   line %1 col %2 — \"%3\"\n")
                          .arg(sp.row + 1).arg(sp.column + 1).arg(snippet);
                errorCount++;
            } else if (ts_node_is_missing(node)) {
                TSPoint sp = ts_node_start_point(node);
                output += QString("MISSING line %1 col %2 — expected '%3'\n")
                          .arg(sp.row + 1).arg(sp.column + 1)
                          .arg(ts_node_type(node));
                errorCount++;
            }
        });

        return ToolResult::ok(errorCount == 0
            ? QString("No syntax errors found in %1")
              .arg(QFileInfo(abs).fileName())
            : QString("%1 issue(s):\n").arg(errorCount) + output.trimmed());
    }
};
