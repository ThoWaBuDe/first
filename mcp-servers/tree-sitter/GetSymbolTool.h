#pragma once
// ─── GetSymbolTool ────────────────────────────────────────────────────────────
// Liefert den vollständigen Quelltext eines benannten Symbols.
//
// Ein "Symbol" ist hier:
//   - Eine Funktion/Methode:  "Agent::onToolCall"  oder nur "onToolCall"
//   - Eine Klasse/Struct:     "Agent"
//
// Wenn der Name mehrdeutig ist (kommt in mehreren Dateien vor),
// werden alle Treffer als Liste zurückgemeldet und das Modell muss
// mit "file" spezifischer werden.
//
// Analogie (AVR): wie ein Oszilloskop das du auf einen bestimmten Pin
// anklemmst — du bekommst genau dieses Signal, nicht den ganzen Bus.
//
// Pattern: Template Method (resolveRead + parseFile aus TreeSitterToolBase)

#include "TreeSitterToolBase.h"
#include <QDirIterator>
#include <QFileInfo>

class GetSymbolTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "get_symbol"; }

    QString description() const override
    {
        return
            "Get the complete source code of a named symbol (function, method, or class). "
            "Use 'ClassName::methodName' for methods, or just 'functionName' for free functions. "
            "If the symbol exists in multiple files, a list of matches is returned — "
            "then call again with the 'file' parameter to select the specific one. "
            "Returns start line, end line, and the full source text. "
            "Path can be relative to a known root or absolute. "
            "Use replace_symbol to edit the returned code.";
    }

    QJsonObject properties() const override
    {
        return {
            {"symbol", prop("string",
                "Symbol name. Examples: 'Agent::onToolCall', 'doGenerate', 'LlamaWorker'.")},
            {"file",   prop("string",
                "Optional: restrict search to this file (relative or absolute path). "
                "Required if the symbol appears in multiple files.")}
        };
    }

    QJsonArray required() const override { return req({"symbol"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString symbol = args["symbol"].toString().trimmed();
        QString file   = args["file"].toString().trimmed();

        if (symbol.isEmpty())
            return ToolResult::err("Error: 'symbol' is required.");

        // Symbol aufteilen: "Agent::onToolCall" → class="Agent", func="onToolCall"
        QString className, funcName;
        if (symbol.contains("::")) {
            className = symbol.section("::", 0, 0).trimmed();
            funcName  = symbol.section("::", 1, 1).trimmed();
        } else {
            funcName = symbol;   // könnte Klasse oder Funktion sein
        }

        // Suchliste aufbauen
        QStringList searchFiles;
        if (!file.isEmpty()) {
            auto [abs, err] = resolveRead(file);
            if (!err.isEmpty()) return ToolResult::err(err);
            searchFiles << abs;
        } else {
            searchFiles = collectSourceFiles();
        }

        // In allen relevanten Dateien suchen
        struct Match {
            QString file;
            QString text;
            int startLine;
            int endLine;
        };
        QList<Match> matches;

        for (const QString &fp : searchFiles) {
            QString parseErr;
            ParseResult pr = parseFile(fp, parseErr);
            if (!pr.valid()) continue;

            TSNode root = ts_tree_root_node(pr.tree);

            traverseTree(root, [&](TSNode node) {
                QString type = ts_node_type(node);

                if (type == "function_definition") {
                    // Funktionsname extrahieren
                    TSNode decl = childByField(node, "declarator");
                    QString fn = extractIdentifier(decl, pr.src);
                    if (fn.isEmpty()) return;

                    // Klassenkontext prüfen (bei Methoden in .cpp):
                    // "void Agent::onToolCall" → qualifizierter Bezeichner
                    QString fullName = extractQualifiedName(decl, pr.src);

                    bool nameMatch = false;
                    if (!className.isEmpty()) {
                        // Suche nach "ClassName::funcName"
                        nameMatch = (fullName == className + "::" + funcName)
                                 || (fn == funcName);
                    } else {
                        // Suche nur nach Funktionsname oder Klassenname
                        nameMatch = (fn == funcName || fullName.endsWith("::" + funcName));
                    }

                    if (nameMatch) {
                        int sl = nodeLine(node);
                        int el = static_cast<int>(ts_node_end_point(node).row) + 1;
                        matches.append({fp, nodeText(node, pr.src), sl, el});
                    }

                } else if ((type == "class_specifier" || type == "struct_specifier")
                           && !funcName.isEmpty() == false) {
                    // Klassensuche: nur wenn kein "::" im Symbol
                    if (!className.isEmpty()) return;
                    TSNode nm = childByField(node, "name");
                    if (!nodeValid(nm)) return;
                    if (nodeText(nm, pr.src) != funcName) return;

                    int sl = nodeLine(node);
                    int el = static_cast<int>(ts_node_end_point(node).row) + 1;
                    matches.append({fp, nodeText(node, pr.src), sl, el});
                }
            });
        }

        if (matches.isEmpty())
            return ToolResult::err(
                QString("Symbol '%1' not found. "
                        "Try get_project_index to see available symbols, "
                        "or grep_code to locate it manually.").arg(symbol));

        // Mehrdeutig → Liste zurückmelden
        if (matches.size() > 1) {
            QString list = QString("Symbol '%1' found in %2 locations — "
                                   "call again with 'file' parameter:\n\n")
                           .arg(symbol).arg(matches.size());
            for (const auto &m : matches)
                list += QString("- %1  (lines %2–%3)\n")
                        .arg(m.file).arg(m.startLine).arg(m.endLine);
            return ToolResult::ok(list);
        }

        // Eindeutig → Quelltext zurückgeben
        const auto &m = matches.first();
        QString rel = relativePath(m.file);
        return ToolResult::ok(
            QString("// %1 — %2  lines %3–%4\n")
            .arg(symbol, rel).arg(m.startLine).arg(m.endLine)
            + m.text);
    }

private:
    // Alle Quelldateien aus beiden Roots sammeln
    QStringList collectSourceFiles() const
    {
        QStringList result;
        for (const auto &root : m_policy->roots()) {
            QDirIterator it(root.path,
                            QStringList() << "*.h" << "*.hpp"
                                          << "*.cpp" << "*.c",
                            QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QString p = it.next();
                if (!QFileInfo(p).isSymLink() && !p.contains("/build/"))
                    result << p;
            }
        }
        return result;
    }

    // Einfachen Identifier aus einem Knoten extrahieren (erstes 'identifier')
    static QString extractIdentifier(TSNode node, const QByteArray &src)
    {
        QString found;
        traverseTree(node, [&](TSNode n) {
            if (found.isEmpty() && QString(ts_node_type(n)) == "identifier")
                found = nodeText(n, src);
        });
        return found;
    }

    // Qualifizierten Namen extrahieren: "Agent::onToolCall"
    // tree-sitter nennt das "qualified_identifier"
    static QString extractQualifiedName(TSNode node, const QByteArray &src)
    {
        QString found;
        traverseTree(node, [&](TSNode n) {
            if (found.isEmpty() &&
                QString(ts_node_type(n)) == "qualified_identifier")
                found = nodeText(n, src);
        });
        return found.simplified();
    }

    // Absoluten Pfad in relativen umwandeln (für lesbare Ausgabe)
    QString relativePath(const QString &abs) const
    {
        for (const auto &root : m_policy->roots()) {
            if (abs.startsWith(root.path + "/"))
                return abs.mid(root.path.length() + 1);
        }
        return abs;
    }
};
