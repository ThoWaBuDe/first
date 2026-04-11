// ─── LlamaQt MCP TreeSitter Server v2.0 ──────────────────────────────────────
// Strukturelle C/C++/CMake Code-Analyse via tree-sitter.
//
// Verwendet McpServer und PathPolicy aus mcp-servers/common/.
//
// Zwei Roots (konfigurierbar via Umgebungsvariablen):
//   LLAMAQT_SANDBOX  — Projektdateien (Standard: ~/llamatools)     read+write
//   LLAMAQT_SOURCES  — LlamaQt Quellcode (Standard: ~/ai/LlamaQT) read-only
//
// Tools:
//   list_symbols      — Funktionen, Klassen, Structs, Enums mit Zeilennummern
//   get_function_body — eine Funktion extrahieren
//   get_class_members — Members + Methoden einer Klasse
//   get_includes      — #include Liste
//   get_class_hierarchy — Basisklassen (class Foo : public Bar)
//   get_call_graph    — Funktionsaufrufe einer Funktion (eine Ebene)
//   check_syntax      — Syntaxfehler mit Zeile + Spalte

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <functional>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"

// tree-sitter C API
#include <tree_sitter/api.h>

extern "C" {
    const TSLanguage *tree_sitter_cpp(void);
    const TSLanguage *tree_sitter_cmake(void);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PARSE-INFRASTRUKTUR
// ═══════════════════════════════════════════════════════════════════════════════

// Erlaubte Datei-Extensions für den tree-sitter Server
static bool isAllowedFile(const QString &path)
{
    QFileInfo fi(path);
    QString name = fi.fileName();
    QString suf  = fi.suffix().toLower();
    if (name == "CMakeLists.txt") return true;
    return (suf == "c" || suf == "cpp" || suf == "h" || suf == "hpp");
}

// Qt-Macros neutralisieren: Q_OBJECT, signals:, slots: etc. werden durch
// gleich lange Leerzeichen ersetzt damit tree-sitter nicht stolpert.
// Gleiche Länge = Byte-Offsets (Zeilennummern) bleiben korrekt.
static QByteArray neutralizeQtMacros(const QByteArray &src)
{
    static const QList<QByteArray> macros = {
        "Q_OBJECT", "Q_GADGET", "Q_INTERFACES", "Q_PROPERTY",
        "Q_INVOKABLE", "Q_REVISION", "Q_SIGNALS", "Q_SLOTS",
        "Q_ENUMS", "Q_FLAGS", "signals", "slots", "emit",
    };
    QByteArray result = src;
    for (const QByteArray &macro : macros) {
        int pos = 0;
        while ((pos = result.indexOf(macro, pos)) != -1) {
            bool prevOk = (pos == 0) ||
                          (!std::isalnum(static_cast<unsigned char>(result[pos-1]))
                           && result[pos-1] != '_');
            int after = pos + macro.size();
            bool nextOk = (after >= result.size()) ||
                          (!std::isalnum(static_cast<unsigned char>(result[after]))
                           && result[after] != '_');
            if (prevOk && nextOk)
                result.replace(pos, macro.size(), QByteArray(macro.size(), ' '));
            pos += macro.size();
        }
    }
    return result;
}

static const TSLanguage *languageForFile(const QString &path)
{
    if (QFileInfo(path).fileName() == "CMakeLists.txt")
        return tree_sitter_cmake();
    return tree_sitter_cpp();
}

// RAII-Wrapper um TSTree + TSParser.
struct ParseResult {
    TSParser  *parser = nullptr;
    TSTree    *tree   = nullptr;
    QByteArray src;

    ~ParseResult() {
        if (tree)   ts_tree_delete(tree);
        if (parser) ts_parser_delete(parser);
    }
    ParseResult(const ParseResult &) = delete;
    ParseResult &operator=(const ParseResult &) = delete;
    ParseResult(ParseResult &&o) noexcept
        : parser(o.parser), tree(o.tree), src(std::move(o.src))
    { o.parser = nullptr; o.tree = nullptr; }
    ParseResult() = default;
};

// Parst eine Datei. Pfad muss bereits aufgelöst sein (absolut).
static ParseResult parseFile(const QString &path, QString &errOut)
{
    ParseResult result;
    if (!isAllowedFile(path)) {
        errOut = "File type not allowed. Supported: .c .cpp .h .hpp CMakeLists.txt";
        return result;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        errOut = "Cannot open file: " + path;
        return result;
    }
    result.src = f.readAll();

    // Qt-Macros neutralisieren (nur für C/C++ Dateien, nicht CMake)
    QString suf = QFileInfo(path).suffix().toLower();
    if (suf == "h" || suf == "hpp" || suf == "cpp" || suf == "c")
        result.src = neutralizeQtMacros(result.src);

    result.parser = ts_parser_new();
    ts_parser_set_language(result.parser, languageForFile(path));
    result.tree = ts_parser_parse_string(
        result.parser, nullptr,
        result.src.constData(),
        static_cast<uint32_t>(result.src.size())
    );
    return result;
}

// ─── Node helpers ─────────────────────────────────────────────────────────────
static QString nodeText(TSNode node, const QByteArray &src)
{
    uint32_t s = ts_node_start_byte(node), e = ts_node_end_byte(node);
    return QString::fromUtf8(src.mid(static_cast<int>(s), static_cast<int>(e-s)));
}
static int nodeLine(TSNode node)
{
    return static_cast<int>(ts_node_start_point(node).row) + 1;
}
static TSNode childByField(TSNode node, const char *field)
{
    return ts_node_child_by_field_name(node, field, static_cast<uint32_t>(strlen(field)));
}
static bool valid(TSNode node) { return !ts_node_is_null(node); }

static void traverseTree(TSNode root, std::function<void(TSNode)> cb)
{
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool reachedRoot = false;
    while (!reachedRoot) {
        cb(ts_tree_cursor_current_node(&cursor));
        if (ts_tree_cursor_goto_first_child(&cursor))   continue;
        if (ts_tree_cursor_goto_next_sibling(&cursor))  continue;
        while (true) {
            if (!ts_tree_cursor_goto_parent(&cursor)) { reachedRoot = true; break; }
            if (ts_tree_cursor_goto_next_sibling(&cursor)) break;
        }
    }
    ts_tree_cursor_delete(&cursor);
}

// ═══════════════════════════════════════════════════════════════════════════════
// TOOL IMPLEMENTIERUNGEN
// Jede Funktion bekommt den bereits aufgelösten absoluten Pfad.
// Rückgabe: ToolResult (ok oder err).
// ═══════════════════════════════════════════════════════════════════════════════

static ToolResult toolListSymbols(const QString &path)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return ToolResult::err("Error: " + err);

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
                if (name.isEmpty() && QString(ts_node_type(nd)) == "identifier")
                    name = nodeText(nd, pr.src);
            });
            if (!name.isEmpty())
                output += QString("function  %1  (line %2)\n").arg(name).arg(nodeLine(child));
        } else if (type == "class_specifier") {
            TSNode nm = childByField(child, "name");
            if (valid(nm))
                output += QString("class     %1  (line %2)\n")
                          .arg(nodeText(nm, pr.src)).arg(nodeLine(child));
        } else if (type == "struct_specifier") {
            TSNode nm = childByField(child, "name");
            output += QString("struct    %1  (line %2)\n")
                      .arg(valid(nm) ? nodeText(nm, pr.src) : "<anonymous>")
                      .arg(nodeLine(child));
        } else if (type == "enum_specifier") {
            TSNode nm = childByField(child, "name");
            output += QString("enum      %1  (line %2)\n")
                      .arg(valid(nm) ? nodeText(nm, pr.src) : "<anonymous>")
                      .arg(nodeLine(child));
        } else if (type == "type_definition") {
            TSNode decl = childByField(child, "declarator");
            if (valid(decl))
                output += QString("typedef   %1  (line %2)\n")
                          .arg(nodeText(decl, pr.src)).arg(nodeLine(child));
        } else if (type == "normal_command" || type == "function_def" || type == "macro_def") {
            TSNode nm = ts_node_child(child, 0);
            if (valid(nm))
                output += QString("cmake     %1  (line %2)\n")
                          .arg(nodeText(nm, pr.src)).arg(nodeLine(child));
        }
    }
    return output.isEmpty() ? ToolResult::ok("No symbols found.")
                            : ToolResult::ok(output.trimmed());
}

static ToolResult toolGetFunctionBody(const QString &path, const QString &funcName)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return ToolResult::err("Error: " + err);

    TSNode root = ts_tree_root_node(pr.tree);
    QString result;
    traverseTree(root, [&](TSNode node) {
        if (!result.isEmpty()) return;
        if (QString(ts_node_type(node)) != "function_definition") return;
        TSNode decl = childByField(node, "declarator");
        QString found;
        traverseTree(decl, [&](TSNode n) {
            if (found.isEmpty() && QString(ts_node_type(n)) == "identifier")
                found = nodeText(n, pr.src);
        });
        if (found == funcName) {
            int sl = nodeLine(node);
            int el = static_cast<int>(ts_node_end_point(node).row) + 1;
            result = QString("// %1() — lines %2–%3\n").arg(funcName).arg(sl).arg(el);
            result += nodeText(node, pr.src);
        }
    });
    return result.isEmpty()
        ? ToolResult::err(QString("Function '%1' not found in %2").arg(funcName, path))
        : ToolResult::ok(result);
}

static ToolResult toolGetClassMembers(const QString &path, const QString &className)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return ToolResult::err("Error: " + err);

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;
    traverseTree(root, [&](TSNode node) {
        if (!output.isEmpty()) return;
        QString type = ts_node_type(node);
        if (type != "class_specifier" && type != "struct_specifier") return;
        TSNode nameNode = childByField(node, "name");
        if (!valid(nameNode) || nodeText(nameNode, pr.src) != className) return;

        TSNode body = childByField(node, "body");
        if (!valid(body)) return;
        output += QString("Members of %1 (line %2):\n").arg(className).arg(nodeLine(node));

        uint32_t n = ts_node_child_count(body);
        for (uint32_t i = 0; i < n; i++) {
            TSNode m = ts_node_child(body, i);
            QString mt = ts_node_type(m);
            if (mt == "field_declaration") {
                TSNode decl = childByField(m, "declarator");
                QString nm;
                if (valid(decl))
                    traverseTree(decl, [&](TSNode nd) {
                        if (nm.isEmpty() && QString(ts_node_type(nd)) == "field_identifier")
                            nm = nodeText(nd, pr.src);
                    });
                if (!nm.isEmpty())
                    output += QString("  field     %1  (line %2)\n").arg(nm).arg(nodeLine(m));
            } else if (mt == "declaration" || mt == "function_definition") {
                TSNode decl = childByField(m, "declarator");
                QString nm;
                traverseTree(decl, [&](TSNode nd) {
                    if (nm.isEmpty() && QString(ts_node_type(nd)) == "identifier")
                        nm = nodeText(nd, pr.src);
                });
                if (!nm.isEmpty()) {
                    QString tag = (mt == "function_definition") ? " [inline]" : "";
                    output += QString("  method    %1  (line %2)%3\n").arg(nm).arg(nodeLine(m)).arg(tag);
                }
            } else if (mt == "access_specifier") {
                output += QString("  [%1]\n").arg(nodeText(m, pr.src).trimmed());
            }
        }
    });
    return output.isEmpty()
        ? ToolResult::err(QString("Class '%1' not found in %2").arg(className, path))
        : ToolResult::ok(output.trimmed());
}

static ToolResult toolGetIncludes(const QString &path)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return ToolResult::err("Error: " + err);

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;
    traverseTree(root, [&](TSNode node) {
        if (QString(ts_node_type(node)) == "preproc_include")
            output += QString("line %1: %2\n").arg(nodeLine(node))
                      .arg(nodeText(node, pr.src).trimmed());
    });
    return ToolResult::ok(output.isEmpty() ? "No includes found." : output.trimmed());
}

static ToolResult toolGetClassHierarchy(const QString &path)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return ToolResult::err("Error: " + err);

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;
    traverseTree(root, [&](TSNode node) {
        if (QString(ts_node_type(node)) != "class_specifier") return;
        TSNode nameNode = childByField(node, "name");
        QString className = valid(nameNode) ? nodeText(nameNode, pr.src) : "<anonymous>";

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
            ? QString("class %1  (line %2)  — no base classes\n").arg(className).arg(nodeLine(node))
            : QString("class %1  (line %2)  : %3\n").arg(className).arg(nodeLine(node)).arg(bases.join(", "));
    });
    return ToolResult::ok(output.isEmpty() ? "No classes found." : output.trimmed());
}

static ToolResult toolGetCallGraph(const QString &path, const QString &funcName)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return ToolResult::err("Error: " + err);

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;
    bool found = false;

    traverseTree(root, [&](TSNode node) {
        if (found) return;
        if (QString(ts_node_type(node)) != "function_definition") return;
        TSNode decl = childByField(node, "declarator");
        QString name;
        traverseTree(decl, [&](TSNode n) {
            if (name.isEmpty() && QString(ts_node_type(n)) == "identifier")
                name = nodeText(n, pr.src);
        });
        if (name != funcName) return;

        found = true;
        output += QString("Call graph for %1() (line %2):\n").arg(funcName).arg(nodeLine(node));
        TSNode body = childByField(node, "body");
        if (!valid(body)) { output += "  (no body)\n"; return; }

        QStringList calls;
        traverseTree(body, [&](TSNode cn) {
            if (QString(ts_node_type(cn)) != "call_expression") return;
            TSNode func = childByField(cn, "function");
            if (!valid(func)) return;
            QString ct = ts_node_type(func);
            QString callName;
            if (ct == "identifier")
                callName = nodeText(func, pr.src);
            else if (ct == "field_expression" || ct == "qualified_identifier")
                callName = nodeText(func, pr.src);
            if (!callName.isEmpty() && !calls.contains(callName))
                calls << QString("  %1  (line %2)").arg(callName).arg(nodeLine(cn));
        });
        output += calls.isEmpty() ? "  (no function calls found)\n" : calls.join("\n") + "\n";
    });
    return found
        ? ToolResult::ok(output.trimmed())
        : ToolResult::err(QString("Function '%1' not found in %2").arg(funcName, path));
}

static ToolResult toolCheckSyntax(const QString &path)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return ToolResult::err("Error: " + err);

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;
    int errorCount = 0;
    traverseTree(root, [&](TSNode node) {
        if (ts_node_is_error(node)) {
            TSPoint sp = ts_node_start_point(node);
            QString snippet = nodeText(node, pr.src).left(60).replace('\n', ' ');
            output += QString("ERROR   line %1 col %2 — \"%3\"\n")
                      .arg(sp.row+1).arg(sp.column+1).arg(snippet);
            errorCount++;
        } else if (ts_node_is_missing(node)) {
            TSPoint sp = ts_node_start_point(node);
            output += QString("MISSING line %1 col %2 — expected '%3'\n")
                      .arg(sp.row+1).arg(sp.column+1).arg(ts_node_type(node));
            errorCount++;
        }
    });
    return ToolResult::ok(errorCount == 0
        ? QString("No syntax errors found in %1").arg(path)
        : QString("%1 issue(s) in %2:\n").arg(errorCount).arg(path) + output.trimmed());
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ── PathPolicy aufbauen ───────────────────────────────────────────────────
    // Zwei Roots: Sandbox (schreibbar) + LlamaQt-Sources (read-only).
    // Werte kommen aus Umgebungsvariablen, Fallback auf Defaults.
    PathPolicy policy;
    policy.addRoot(
        qEnvironmentVariable("LLAMAQT_SANDBOX",
            QDir::homePath() + "/llamatools"),
        true   // writable = Sandbox
    );
    policy.addRoot(
        qEnvironmentVariable("LLAMAQT_SOURCES",
            QDir::homePath() + "/ai/LlamaQT"),
        false  // read-only = LlamaQt-Quellcode, nicht anfassen!
    );

    QTextStream err(stderr);
    err << "[llamaqt-treesitter] Sandbox (rw): " << policy.primarySandbox() << "\n";
    for (const auto &r : policy.roots())
        if (!r.writable)
            err << "[llamaqt-treesitter] Sources (ro): " << r.path << "\n";
    err.flush();

    // ── Hilfsfunktion: Pfad auflösen + validieren ─────────────────────────────
    // Kapselt PathPolicy::resolveRead() + isAllowedFile() in einem Lambda.
    // Wird von jedem Tool-Lambda verwendet.
    auto resolveSrc = [&](const QString &raw, QString &absPath) -> bool {
        auto rp = policy.resolveRead(raw);
        if (!rp.valid) { absPath = rp.error; return false; }
        absPath = rp.absPath;
        return true;
    };

    // ── McpServer aufbauen + Tools registrieren ───────────────────────────────
    McpServer server("llamaqt-treesitter", "2.0");

    // Gemeinsame Schema-Bausteine
    auto pathProp = QJsonObject{{"type","string"},
        {"description","Absolute or sandbox-relative path (.c .cpp .h .hpp CMakeLists.txt)"}};
    auto funcProp = QJsonObject{{"type","string"},{"description","Exact function name"}};
    auto clsProp  = QJsonObject{{"type","string"},{"description","Exact class or struct name"}};

    server.registerTool({
        "list_symbols",
        "List all top-level symbols (functions, classes, structs, enums, typedefs) "
        "in a C/C++/CMake file with line numbers. "
        "Path can be absolute or relative to sandbox root.",
        {{"path", pathProp}}, {"path"},
        [&](const QJsonObject &args) -> ToolResult {
            QString abs;
            if (!resolveSrc(args["path"].toString(), abs)) return ToolResult::err(abs);
            return toolListSymbols(abs);
        }
    });

    server.registerTool({
        "get_function_body",
        "Extract the complete source of a named function. "
        "Returns function body with start and end line numbers. "
        "Path can be absolute or relative to sandbox root.",
        {{"path", pathProp}, {"function", funcProp}}, {"path","function"},
        [&](const QJsonObject &args) -> ToolResult {
            QString abs;
            if (!resolveSrc(args["path"].toString(), abs)) return ToolResult::err(abs);
            return toolGetFunctionBody(abs, args["function"].toString());
        }
    });

    server.registerTool({
        "get_class_members",
        "List all fields and methods of a class or struct with line numbers "
        "and access specifiers. Qt classes (Q_OBJECT) are supported. "
        "Path can be absolute or relative to sandbox root.",
        {{"path", pathProp}, {"class", clsProp}}, {"path","class"},
        [&](const QJsonObject &args) -> ToolResult {
            QString abs;
            if (!resolveSrc(args["path"].toString(), abs)) return ToolResult::err(abs);
            return toolGetClassMembers(abs, args["class"].toString());
        }
    });

    server.registerTool({
        "get_includes",
        "List all #include directives with line numbers. "
        "Path can be absolute or relative to sandbox root.",
        {{"path", pathProp}}, {"path"},
        [&](const QJsonObject &args) -> ToolResult {
            QString abs;
            if (!resolveSrc(args["path"].toString(), abs)) return ToolResult::err(abs);
            return toolGetIncludes(abs);
        }
    });

    server.registerTool({
        "get_class_hierarchy",
        "Show all classes in a file and their base classes. "
        "Qt classes (Q_OBJECT, signals, slots) are supported. "
        "Path can be absolute or relative to sandbox root.",
        {{"path", pathProp}}, {"path"},
        [&](const QJsonObject &args) -> ToolResult {
            QString abs;
            if (!resolveSrc(args["path"].toString(), abs)) return ToolResult::err(abs);
            return toolGetClassHierarchy(abs);
        }
    });

    server.registerTool({
        "get_call_graph",
        "List all function calls made by a given function (one level deep). "
        "Includes plain calls, method calls and qualified calls (Ns::func). "
        "Path can be absolute or relative to sandbox root.",
        {{"path", pathProp}, {"function", funcProp}}, {"path","function"},
        [&](const QJsonObject &args) -> ToolResult {
            QString abs;
            if (!resolveSrc(args["path"].toString(), abs)) return ToolResult::err(abs);
            return toolGetCallGraph(abs, args["function"].toString());
        }
    });

    server.registerTool({
        "check_syntax",
        "Check a source file for syntax errors. Reports ERROR nodes "
        "(unexpected tokens) and MISSING nodes (invented tokens) "
        "with line and column numbers. "
        "Note: syntax check only, not a full compiler check. "
        "Path can be absolute or relative to sandbox root.",
        {{"path", pathProp}}, {"path"},
        [&](const QJsonObject &args) -> ToolResult {
            QString abs;
            if (!resolveSrc(args["path"].toString(), abs)) return ToolResult::err(abs);
            return toolCheckSyntax(abs);
        }
    });

    server.run();
    return 0;
}
