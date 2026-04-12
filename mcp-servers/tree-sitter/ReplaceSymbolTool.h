#pragma once
// ─── ReplaceSymbolTool ────────────────────────────────────────────────────────
// Ersetzt den Quelltext einer benannten Funktion oder Klasse chirurgisch.
//
// "Chirurgisch" bedeutet:
//   - Nur der Rumpf dieser Funktion wird ersetzt
//   - Der Rest der Datei bleibt byte-genau erhalten
//   - Auto-Commit vor und nach der Änderung (git als Undo-System)
//
// Workflow für das Modell:
//   1. get_symbol("Agent::onToolCall")    → Quelltext lesen
//   2. Änderungen vornehmen
//   3. replace_symbol("Agent::onToolCall", neuer_code, "Agent.cpp")
//
// Warum braucht es 'file' als Pflichtfeld?
//   Im Gegensatz zu get_symbol (der bei Mehrdeutigkeit eine Liste zurückgibt)
//   muss replace_symbol GENAU wissen wo es schreiben soll.
//   Ein versehentliches Überschreiben der falschen Datei wäre ein Bug.
//
// Analogie (AVR): wie ein In-System-Programmer der genau einen Flash-Block
//   überschreibt — er muss die genaue Adresse kennen, keine Schätzung.
//
// Schreibzugriff: nur in writable Roots (Sandbox).
// LlamaQT-Quellcode (Sources) ist read-only — dort kein replace_symbol.

#include "TreeSitterToolBase.h"
#include "../filesystem/GitHelper.h"

#include <QFile>
#include <QTextStream>
#include <QFileInfo>

class ReplaceSymbolTool : public TreeSitterToolBase
{
public:
    // Konstruktor mit GitHelper — analog zu FilesystemToolBase.
    ReplaceSymbolTool(PathPolicy *policy, GitHelper *git)
        : TreeSitterToolBase(policy), m_git(git)
    {}

    QString name() const override { return "replace_symbol"; }

    QString description() const override
    {
        return
            "Replace the source code of a named function or class in-place. "
            "Only the specified symbol is replaced; the rest of the file is unchanged. "
            "The 'file' parameter is required to avoid ambiguity — use get_symbol first "
            "to find the exact file. "
            "Only allowed in the sandbox (writable root). "
            "The previous state is committed to git automatically before replacing. "
            "Tip: use get_symbol to read the current code, edit it, then call replace_symbol.";
    }

    QJsonObject properties() const override
    {
        return {
            {"symbol",   prop("string",
                "Symbol to replace. Examples: 'Agent::onToolCall', 'doGenerate', 'LlamaWorker'.")},
            {"new_code", prop("string",
                "Complete new source code for the symbol (full function or class definition).")},
            {"file",     prop("string",
                "File containing the symbol (relative to sandbox or absolute). "
                "Required — use get_symbol to find the exact file first.")}
        };
    }

    QJsonArray required() const override { return req({"symbol", "new_code", "file"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString symbol  = args["symbol"].toString().trimmed();
        QString newCode = args["new_code"].toString();
        QString file    = args["file"].toString().trimmed();

        if (symbol.isEmpty() || newCode.isEmpty() || file.isEmpty())
            return ToolResult::err("Error: 'symbol', 'new_code', and 'file' are required.");

        // Pfad auflösen — nur writable Roots erlaubt
        auto [absPath, pathErr] = resolveWrite(file);
        if (!pathErr.isEmpty()) return ToolResult::err(pathErr);

        // Datei lesen
        QFile f(absPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return ToolResult::err(QString("Error: Cannot read file: %1").arg(file));
        QString content = QTextStream(&f).readAll();
        f.close();

        // Symbol aufteilen
        QString className, funcName;
        if (symbol.contains("::")) {
            className = symbol.section("::", 0, 0).trimmed();
            funcName  = symbol.section("::", 1, 1).trimmed();
        } else {
            funcName = symbol;
        }

        // tree-sitter parsen um Byte-Offsets des Symbols zu finden
        QString parseErr;
        ParseResult pr = parseFile(absPath, parseErr);
        if (!pr.valid())
            return ToolResult::err("Error parsing file: " + parseErr);

        TSNode root = ts_tree_root_node(pr.tree);

        // Symbol-Knoten finden und Byte-Bereich merken
        uint32_t startByte = 0, endByte = 0;
        bool found = false;

        traverseTree(root, [&](TSNode node) {
            if (found) return;
            QString type = ts_node_type(node);

            if (type == "function_definition") {
                TSNode decl = childByField(node, "declarator");
                QString fn = extractIdentifier(decl, pr.src);
                QString qn = extractQualifiedName(decl, pr.src);

                bool match = false;
                if (!className.isEmpty())
                    match = (qn == className + "::" + funcName) || (fn == funcName);
                else
                    match = (fn == funcName || qn.endsWith("::" + funcName));

                if (match) {
                    startByte = ts_node_start_byte(node);
                    endByte   = ts_node_end_byte(node);
                    found     = true;
                }

            } else if ((type == "class_specifier" || type == "struct_specifier")
                       && className.isEmpty()) {
                TSNode nm = childByField(node, "name");
                if (nodeValid(nm) && nodeText(nm, pr.src) == funcName) {
                    startByte = ts_node_start_byte(node);
                    endByte   = ts_node_end_byte(node);
                    found     = true;
                }
            }
        });

        if (!found)
            return ToolResult::err(
                QString("Error: Symbol '%1' not found in %2. "
                        "Use get_symbol to verify the exact name and file.")
                .arg(symbol, file));

        // Git-Commit vor der Änderung
        m_git->autoCommit(absPath,
            QString("before replace_symbol %1 in %2").arg(symbol, file));

        // Inhalt zusammensetzen:
        // [Anfang der Datei] + [neuer Code] + [Rest der Datei]
        //
        // Analogie (AVR): wie ein Flash-Schreiber der nur einen Sektor
        // überschreibt und den Rest des Flash unberührt lässt.
        // startByte/endByte sind die "Sektor-Grenzen".
        QByteArray before = pr.src.left(static_cast<int>(startByte));
        QByteArray after  = pr.src.mid(static_cast<int>(endByte));

        // Zeilenumbruch zwischen altem Ende und neuem Anfang erhalten
        if (!after.isEmpty() && after[0] != '\n' && !newCode.endsWith('\n'))
            newCode += '\n';

        QByteArray result = before + newCode.toUtf8() + after;

        // Datei zurückschreiben
        if (!f.open(QIODevice::WriteOnly))
            return ToolResult::err(
                QString("Error: Cannot write to file: %1").arg(file));
        f.write(result);
        f.close();

        // Git-Commit nach der Änderung
        m_git->autoCommit(absPath,
            QString("replace_symbol %1 in %2").arg(symbol, file));

        int startLine = static_cast<int>(
            pr.src.left(static_cast<int>(startByte)).count('\n')) + 1;
        int endLine   = static_cast<int>(
            pr.src.left(static_cast<int>(endByte)).count('\n')) + 1;

        return ToolResult::ok(
            QString("OK: Replaced '%1' in %2 (was lines %3–%4).")
            .arg(symbol, file).arg(startLine).arg(endLine));
    }

private:
    GitHelper *m_git;

    // Pfad für Schreibzugriff auflösen
    QPair<QString,QString> resolveWrite(const QString &path) const
    {
        auto rp = m_policy->resolveWrite(path);
        if (!rp.valid) return {"", rp.error};
        return {rp.absPath, ""};
    }

    static QString extractIdentifier(TSNode node, const QByteArray &src)
    {
        QString found;
        traverseTree(node, [&](TSNode n) {
            if (found.isEmpty() && QString(ts_node_type(n)) == "identifier")
                found = nodeText(n, src);
        });
        return found;
    }

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
};
