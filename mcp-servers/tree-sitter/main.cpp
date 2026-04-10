// ─── LlamaQt MCP TreeSitter Server ───────────────────────────────────────────
// Structural C/C++/CMake code analysis via tree-sitter.
//
// Allowed file extensions: .c .cpp .h .hpp CMakeLists.txt
// No sandbox restriction — read-only, any path on the filesystem.
//
// Tools:
//   list_symbols      — functions, classes, structs, enums with line numbers
//   get_function_body — extract one function by name
//   get_class_members — members + methods of a class/struct
//   get_includes      — #include list of a file
//   get_class_hierarchy — base classes (class Foo : public Bar)
//   get_call_graph    — functions called by a given function (one level)
//   check_syntax      — report syntax errors with line + column

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <iostream>
#include <string>
#include <vector>

// tree-sitter C API
#include <tree_sitter/api.h>

// ── Grammar entry points ──────────────────────────────────────────────────────
// Jede Grammar-Library exportiert eine C-Funktion die die Sprache zurückgibt.
// Deklariert als "extern C" damit der C++ Linker die C-Symbole findet.
extern "C" {
    const TSLanguage *tree_sitter_c(void);
    const TSLanguage *tree_sitter_cpp(void);
    const TSLanguage *tree_sitter_cmake(void);
}

// ─── JSON-RPC helpers ─────────────────────────────────────────────────────────
static void sendResponse(const QJsonObject &msg)
{
    QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    std::cout << line.toStdString();
    std::cout.flush();
}

static void sendResult(int id, const QString &text, bool isError = false)
{
    sendResponse({
        {"jsonrpc","2.0"}, {"id",id},
        {"result", QJsonObject{
            {"content", QJsonArray{QJsonObject{{"type","text"},{"text",text}}}},
            {"isError", isError}
        }}
    });
}

static void sendError(int id, int code, const QString &message)
{
    sendResponse({{"jsonrpc","2.0"},{"id",id},
                  {"error",QJsonObject{{"code",code},{"message",message}}}});
}

// ─── Konfiguration ────────────────────────────────────────────────────────────
// Zwei Root-Verzeichnisse:
//   LLAMAQT_SANDBOX  — Projektdateien des Modells  (Standard: ~/llamatools)
//   LLAMAQT_SOURCES  — LlamaQt Quellcode selbst    (Standard: ~/ai/LlamaQT)
//
// Relative Pfade werden gegen BEIDE Roots geprüft — die erste die existiert
// gewinnt. Absolute Pfade werden direkt verwendet.
static QString g_sandboxRoot;
static QString g_sourcesRoot;

static void initRoots()
{
    g_sandboxRoot = qEnvironmentVariable("LLAMAQT_SANDBOX");
    if (g_sandboxRoot.isEmpty())
        g_sandboxRoot = QDir::homePath() + "/llamatools";
    while (g_sandboxRoot.endsWith('/')) g_sandboxRoot.chop(1);

    g_sourcesRoot = qEnvironmentVariable("LLAMAQT_SOURCES");
    if (g_sourcesRoot.isEmpty())
        g_sourcesRoot = QDir::homePath() + "/ai/LlamaQT";
    while (g_sourcesRoot.endsWith('/')) g_sourcesRoot.chop(1);
}

// Löst einen Pfad auf:
//   absolut  → direkt verwenden (QFile::exists prüft ob er gültig ist)
//   relativ  → zuerst in Sandbox suchen, dann in Sources, dann Sandbox als Fallback
//
// Dadurch funktionieren beide Konventionen:
//   "LlamaQT/src/Agent.h"              → Sandbox-Root
//   "src/Agent.h"                       → Sources-Root (kein LlamaQT/ Prefix)
//   "/home/thomas/ai/LlamaQT/src/Agent.h" → absolut, direkt
static QString resolvePath(const QString &path)
{
    if (path.startsWith('/'))
        return path;   // absolut → unverändert

    // Relativ: beide Roots ausprobieren, erste existierende gewinnt
    QString inSandbox = g_sandboxRoot + '/' + path;
    if (QFile::exists(inSandbox))
        return inSandbox;

    QString inSources = g_sourcesRoot + '/' + path;
    if (QFile::exists(inSources))
        return inSources;

    // Nichts gefunden — Sandbox als Fallback (Fehlermeldung kommt von readFile)
    return inSandbox;
}

// ─── File utilities ───────────────────────────────────────────────────────────

// Prüft ob der Dateipfad eine erlaubte Extension hat.
// Wir arbeiten read-only, aber wir wollen keine Binärdateien parsen.
static bool isAllowedFile(const QString &path)
{
    QFileInfo fi(path);
    QString name = fi.fileName();
    QString suf  = fi.suffix().toLower();

    if (name == "CMakeLists.txt") return true;
    return (suf == "c" || suf == "cpp" || suf == "h" || suf == "hpp");
}

// ─── Qt-Macro Neutralisierung (Bug 1) ────────────────────────────────────────
// Qt-Macros wie Q_OBJECT, signals:, slots: sind kein Standard-C++ —
// tree-sitter's C++ Grammar kennt sie nicht und erzeugt ERROR-Nodes mitten
// in der Klassendefinition. Dadurch findet der Parser die Klasse nicht mehr
// als class_specifier.
//
// Strategie: Wir ersetzen die Macros VOR dem Parsen durch gleich lange
// Leerzeichen-Blöcke. Gleiche Länge = alle Byte-Offsets (Zeilennummern!)
// bleiben exakt korrekt. Der Baum ist danach syntaktisch sauber.
//
// Wichtig: wir ersetzen nur vollständige Tokens (word boundary via Zustandsmaschine),
// nicht Teilstrings. "Q_OBJECT_FOO" soll nicht angefasst werden.
static QByteArray neutralizeQtMacros(const QByteArray &src)
{
    // Liste der Macros die ersetzt werden.
    // Sortiert nach Länge (längste zuerst) verhindert Teilersetzungen.
    static const QList<QByteArray> macros = {
        "Q_OBJECT",
        "Q_GADGET",
        "Q_INTERFACES",
        "Q_PROPERTY",
        "Q_INVOKABLE",
        "Q_REVISION",
        "Q_SIGNALS",
        "Q_SLOTS",
        "Q_ENUMS",
        "Q_FLAGS",
        "signals",
        "slots",
        "emit",
    };

    QByteArray result = src;

    for (const QByteArray &macro : macros) {
        int pos = 0;
        while ((pos = result.indexOf(macro, pos)) != -1) {
            // Prüfe ob es ein vollständiges Token ist (kein Bezeichner davor/danach).
            // Zeichen vor dem Match: muss kein Bezeichner-Zeichen sein
            bool prevOk = (pos == 0) ||
                          (!std::isalnum(static_cast<unsigned char>(result[pos-1]))
                           && result[pos-1] != '_');
            // Zeichen nach dem Match
            int after = pos + macro.size();
            bool nextOk = (after >= result.size()) ||
                          (!std::isalnum(static_cast<unsigned char>(result[after]))
                           && result[after] != '_');

            if (prevOk && nextOk) {
                // Ersetze durch gleich viele Leerzeichen — Offsets bleiben korrekt
                result.replace(pos, macro.size(), QByteArray(macro.size(), ' '));
            }
            pos += macro.size();
        }
    }
    return result;
}

// Liest die komplette Datei als QByteArray (UTF-8).
// tree-sitter arbeitet auf rohen Bytes — das ist bewusst so,
// weil es sprachunabhängig mit beliebigen Encodings umgehen kann.
static bool readFile(const QString &path, QByteArray &out, QString &err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err = "Cannot open file: " + path;
        return false;
    }
    out = f.readAll();
    return true;
}

// Wählt die passende tree-sitter Grammar anhand der Dateiendung.
// Rückgabe: Zeiger auf TSLanguage oder nullptr wenn unbekannt.
static const TSLanguage *languageForFile(const QString &path)
{
    QFileInfo fi(path);
    QString name = fi.fileName();
    QString suf  = fi.suffix().toLower();

    if (name == "CMakeLists.txt") return tree_sitter_cmake();
    if (suf == "c" || suf == "h") return tree_sitter_c();
    // .cpp .hpp → C++ Grammar
    // Die C++ Grammar kann auch reinen C-Code parsen,
    // aber für .h/.c nehmen wir die spezifischere C-Grammar.
    return tree_sitter_cpp();
}

// ─── ParseResult ─────────────────────────────────────────────────────────────
// RAII-Wrapper um TSTree + TSParser.
// Destruktor gibt den Speicher frei — tree-sitter ist eine C-Library
// und kennt keine C++ Destruktoren, also machen wir das manuell.
struct ParseResult {
    TSParser *parser = nullptr;
    TSTree   *tree   = nullptr;
    QByteArray src;   // source muss am Leben bleiben solange tree genutzt wird

    ~ParseResult() {
        if (tree)   ts_tree_delete(tree);
        if (parser) ts_parser_delete(parser);
    }

    // Copy verboten — zwei Objekte würden denselben Zeiger freigeben (double free).
    ParseResult(const ParseResult &) = delete;
    ParseResult &operator=(const ParseResult &) = delete;

    // Move erlaubt — Besitz der Zeiger wird übertragen, Quelle auf nullptr gesetzt.
    // Das ist nötig damit "return result;" aus parseFile() funktioniert.
    // Pattern: Move-Konstruktor = Zeiger klauen + Quelle entwaffnen.
    ParseResult(ParseResult &&other) noexcept
        : parser(other.parser), tree(other.tree), src(std::move(other.src))
    {
        other.parser = nullptr;  // Destruktor der Quelle gibt jetzt nichts frei
        other.tree   = nullptr;
    }

    ParseResult() = default;
};

// Parst eine Datei und gibt ParseResult zurück.
// Bei Fehler ist tree == nullptr und errOut enthält die Fehlermeldung.
// Bug 2: Pfad wird zuerst aufgelöst (relativ → absolut via Sandbox-Root).
// Bug 1: Qt-Macros werden vor dem Parsen neutralisiert.
static ParseResult parseFile(const QString &rawPath, QString &errOut)
{
    ParseResult result;

    // Bug 2: Pfad auflösen
    QString path = resolvePath(rawPath);

    if (!isAllowedFile(path)) {
        errOut = "File type not allowed. Supported: .c .cpp .h .hpp CMakeLists.txt";
        return result;
    }

    if (!readFile(path, result.src, errOut))
        return result;

    // Bug 1: Qt-Macros neutralisieren bevor tree-sitter den Source sieht.
    // Nur für .h/.hpp/.cpp Dateien nötig, nicht für CMake.
    QFileInfo fi(path);
    QString suf = fi.suffix().toLower();
    if (suf == "h" || suf == "hpp" || suf == "cpp" || suf == "c")
        result.src = neutralizeQtMacros(result.src);

    const TSLanguage *lang = languageForFile(path);
    if (!lang) {
        errOut = "No grammar available for this file type.";
        return result;
    }

    // TSParser anlegen und Sprache setzen
    result.parser = ts_parser_new();
    ts_parser_set_language(result.parser, lang);

    // Parsen: src.data() = Zeiger auf Bytes, src.size() = Länge
    // tree-sitter parst IMMER — auch bei Syntaxfehlern.
    // Fehler sind als ERROR-Nodes im Baum sichtbar, kein Absturz.
    result.tree = ts_parser_parse_string(
        result.parser,
        nullptr,               // kein alter Baum (kein inkrementelles Parsen hier)
        result.src.constData(),
        static_cast<uint32_t>(result.src.size())
    );

    return result;
}

// ─── Node helpers ─────────────────────────────────────────────────────────────

// Gibt den Text zurück den ein Node im Quellcode überspannt.
// ts_node_start_byte / ts_node_end_byte sind Byte-Offsets in src.
static QString nodeText(TSNode node, const QByteArray &src)
{
    uint32_t start = ts_node_start_byte(node);
    uint32_t end   = ts_node_end_byte(node);
    return QString::fromUtf8(src.mid(static_cast<int>(start),
                                     static_cast<int>(end - start)));
}

// Zeile (1-basiert) des Node-Starts.
// ts_node_start_point gibt {row, column} zurück — row ist 0-basiert.
static int nodeLine(TSNode node)
{
    return static_cast<int>(ts_node_start_point(node).row) + 1;
}

// Sucht ein direktes Kind mit einem bestimmten "field name".
// Field names sind in der Grammar definiert, z.B. "name" für den Bezeichner
// einer Funktion. ts_node_child_by_field_name ist die sicherste Methode.
static TSNode childByField(TSNode node, const char *field)
{
    return ts_node_child_by_field_name(node, field, static_cast<uint32_t>(strlen(field)));
}

// Prüft ob ein Node gültig ist (nicht null/missing).
static bool valid(TSNode node) { return !ts_node_is_null(node); }

// Rekursive Traversal aller Nodes: ruft callback(node) für jeden Node auf.
// tree-sitter hat keinen eingebauten Iterator — wir nutzen TSTreeCursor
// der effizienter ist als rekursive ts_node_child() Aufrufe.
static void traverseTree(TSNode root, std::function<void(TSNode)> callback)
{
    TSTreeCursor cursor = ts_tree_cursor_new(root);

    // Depth-first traversal mit dem Cursor.
    // Ablauf: erst Kind, dann Geschwister, dann hoch und Geschwister.
    bool reachedRoot = false;
    while (!reachedRoot) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        callback(node);

        // Versuche ein Kind zu betreten
        if (ts_tree_cursor_goto_first_child(&cursor)) continue;

        // Kein Kind — versuche Geschwister
        if (ts_tree_cursor_goto_next_sibling(&cursor)) continue;

        // Kein Geschwister — gehe hoch bis wir ein Geschwister finden
        while (true) {
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                reachedRoot = true;
                break;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor)) break;
        }
    }

    ts_tree_cursor_delete(&cursor);
}

// ═══════════════════════════════════════════════════════════════════════════════
// TOOL IMPLEMENTATIONS
// ═══════════════════════════════════════════════════════════════════════════════

// ─── list_symbols ─────────────────────────────────────────────────────────────
// Sammelt alle top-level Definitionen einer Datei.
// Wir suchen nach Node-Typen die Definitionen darstellen.
// Die Typ-Namen kommen aus der tree-sitter Grammar — ts_node_type() gibt
// den String zurück wie er in der Grammar definiert ist.
static QString toolListSymbols(const QString &path)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return "Error: " + err;

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;

    // Wir traversieren nur eine Ebene tief (direkte Kinder des Root-Nodes),
    // weil wir top-level Symbole wollen, keine verschachtelten Hilfsfunktionen.
    // Ausnahme: Klassen/Structs haben ihre Members tiefer — die kommen in
    // get_class_members.
    uint32_t childCount = ts_node_child_count(root);
    for (uint32_t i = 0; i < childCount; i++) {
        TSNode child = ts_node_child(root, i);
        QString type = ts_node_type(child);

        // C/C++ Funktionsdefinition
        if (type == "function_definition") {
            TSNode declarator = childByField(child, "declarator");
            // Der declarator kann verschachtelt sein: pointer_declarator →
            // function_declarator → identifier. Wir suchen den innersten Namen.
            // Einfache Heuristik: gehe durch Kinder bis wir "identifier" finden.
            QString name;
            traverseTree(declarator, [&](TSNode n) {
                if (name.isEmpty() && QString(ts_node_type(n)) == "identifier")
                    name = nodeText(n, pr.src);
            });
            if (!name.isEmpty())
                output += QString("function  %1  (line %2)\n").arg(name).arg(nodeLine(child));
        }
        // C++ Klassendeklaration
        else if (type == "class_specifier") {
            TSNode nameNode = childByField(child, "name");
            if (valid(nameNode))
                output += QString("class     %1  (line %2)\n")
                          .arg(nodeText(nameNode, pr.src)).arg(nodeLine(child));
        }
        // C/C++ Struct
        else if (type == "struct_specifier") {
            TSNode nameNode = childByField(child, "name");
            QString name = valid(nameNode) ? nodeText(nameNode, pr.src) : "<anonymous>";
            output += QString("struct    %1  (line %2)\n").arg(name).arg(nodeLine(child));
        }
        // Enum
        else if (type == "enum_specifier") {
            TSNode nameNode = childByField(child, "name");
            QString name = valid(nameNode) ? nodeText(nameNode, pr.src) : "<anonymous>";
            output += QString("enum      %1  (line %2)\n").arg(name).arg(nodeLine(child));
        }
        // typedef — häufig in C für struct typedefs
        else if (type == "type_definition") {
            // type_definition hat ein "declarator" field mit dem neuen Namen
            TSNode decl = childByField(child, "declarator");
            if (valid(decl))
                output += QString("typedef   %1  (line %2)\n")
                          .arg(nodeText(decl, pr.src)).arg(nodeLine(child));
        }
        // CMake: Funktionsaufrufe auf oberster Ebene (target_link_libraries etc.)
        else if (type == "normal_command" || type == "function_def" || type == "macro_def") {
            // Bei CMake ist der "Name" das erste Kind (command identifier)
            TSNode nameNode = ts_node_child(child, 0);
            if (valid(nameNode))
                output += QString("cmake     %1  (line %2)\n")
                          .arg(nodeText(nameNode, pr.src)).arg(nodeLine(child));
        }
    }

    return output.isEmpty() ? "No symbols found." : output.trimmed();
}

// ─── get_function_body ────────────────────────────────────────────────────────
// Extrahiert den kompletten Quelltext einer Funktion anhand ihres Namens.
// Gibt Startzeile + Endzeile zusätzlich aus — der Agent kann damit
// gezielt read_file mit Zeilenbereich aufrufen wenn er nur einen Teil braucht.
static QString toolGetFunctionBody(const QString &path, const QString &funcName)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return "Error: " + err;

    TSNode root = ts_tree_root_node(pr.tree);
    QString result;

    traverseTree(root, [&](TSNode node) {
        if (result.isEmpty() && QString(ts_node_type(node)) == "function_definition") {
            // Suche den Funktionsnamen im declarator
            TSNode decl = childByField(node, "declarator");
            QString foundName;
            traverseTree(decl, [&](TSNode n) {
                if (foundName.isEmpty() && QString(ts_node_type(n)) == "identifier")
                    foundName = nodeText(n, pr.src);
            });

            if (foundName == funcName) {
                int startLine = nodeLine(node);
                int endLine   = static_cast<int>(ts_node_end_point(node).row) + 1;
                result = QString("// %1() — lines %2–%3\n").arg(funcName).arg(startLine).arg(endLine);
                result += nodeText(node, pr.src);
            }
        }
    });

    return result.isEmpty()
        ? QString("Function '%1' not found in %2").arg(funcName).arg(path)
        : result;
}

// ─── get_class_members ────────────────────────────────────────────────────────
// Gibt alle Members (Felder + Methoden) einer Klasse oder eines Structs aus.
// Jede Member-Deklaration mit Zeile.
static QString toolGetClassMembers(const QString &path, const QString &className)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return "Error: " + err;

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;

    // Suche class_specifier oder struct_specifier mit passendem Namen
    traverseTree(root, [&](TSNode node) {
        if (!output.isEmpty()) return;
        QString type = ts_node_type(node);
        if (type != "class_specifier" && type != "struct_specifier") return;

        TSNode nameNode = childByField(node, "name");
        if (!valid(nameNode) || nodeText(nameNode, pr.src) != className) return;

        // Gefunden — jetzt den body traversieren
        TSNode body = childByField(node, "body");
        if (!valid(body)) return;

        output += QString("Members of %1 (line %2):\n").arg(className).arg(nodeLine(node));

        uint32_t n = ts_node_child_count(body);
        for (uint32_t i = 0; i < n; i++) {
            TSNode member = ts_node_child(body, i);
            QString mtype = ts_node_type(member);

            // Felddekl. (int x; oder Type m_foo;)
            if (mtype == "field_declaration") {
                TSNode decl = childByField(member, "declarator");
                QString name;
                if (valid(decl)) {
                    traverseTree(decl, [&](TSNode nd) {
                        if (name.isEmpty() && QString(ts_node_type(nd)) == "field_identifier")
                            name = nodeText(nd, pr.src);
                    });
                }
                if (!name.isEmpty())
                    output += QString("  field     %1  (line %2)\n").arg(name).arg(nodeLine(member));
            }
            // Methoden-Deklaration (nur Signatur, kein Body — im Header)
            else if (mtype == "declaration") {
                TSNode decl = childByField(member, "declarator");
                QString name;
                traverseTree(decl, [&](TSNode nd) {
                    if (name.isEmpty() && QString(ts_node_type(nd)) == "identifier")
                        name = nodeText(nd, pr.src);
                });
                if (!name.isEmpty())
                    output += QString("  method    %1  (line %2)\n").arg(name).arg(nodeLine(member));
            }
            // Methoden-Definition mit Body (inline in Klasse)
            else if (mtype == "function_definition") {
                TSNode decl = childByField(member, "declarator");
                QString name;
                traverseTree(decl, [&](TSNode nd) {
                    if (name.isEmpty() && QString(ts_node_type(nd)) == "identifier")
                        name = nodeText(nd, pr.src);
                });
                if (!name.isEmpty())
                    output += QString("  method    %1  (line %2) [inline]\n").arg(name).arg(nodeLine(member));
            }
            // Access specifier (public: private: protected:)
            else if (mtype == "access_specifier") {
                output += QString("  [%1]\n").arg(nodeText(member, pr.src).trimmed());
            }
        }
    });

    return output.isEmpty()
        ? QString("Class or struct '%1' not found in %2").arg(className).arg(path)
        : output.trimmed();
}

// ─── get_includes ─────────────────────────────────────────────────────────────
// Alle #include Direktiven einer Datei mit Zeilennummer.
static QString toolGetIncludes(const QString &path)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return "Error: " + err;

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;

    traverseTree(root, [&](TSNode node) {
        // preproc_include ist der Node-Typ für #include in der C/C++ Grammar
        if (QString(ts_node_type(node)) == "preproc_include") {
            // Das Argument ist entweder system_lib_string (<foo.h>)
            // oder string_literal ("foo.h")
            QString include = nodeText(node, pr.src).trimmed();
            output += QString("line %1: %2\n").arg(nodeLine(node)).arg(include);
        }
    });

    return output.isEmpty() ? "No includes found." : output.trimmed();
}

// ─── get_class_hierarchy ──────────────────────────────────────────────────────
// Findet alle Klassen und ihre Basisklassen (class Foo : public Bar, Baz).
// Gibt auch an in welcher Datei und Zeile die Deklaration steht.
static QString toolGetClassHierarchy(const QString &path)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return "Error: " + err;

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;

    traverseTree(root, [&](TSNode node) {
        if (QString(ts_node_type(node)) != "class_specifier") return;

        TSNode nameNode = childByField(node, "name");
        QString className = valid(nameNode) ? nodeText(nameNode, pr.src) : "<anonymous>";

        // base_class_clause ist das optionale ": public Bar, ..." Teil
        // Es ist ein direktes Kind von class_specifier
        QStringList bases;
        uint32_t n = ts_node_child_count(node);
        for (uint32_t i = 0; i < n; i++) {
            TSNode child = ts_node_child(node, i);
            if (QString(ts_node_type(child)) == "base_class_clause") {
                // Alle type_identifier innerhalb des clause = Basisklassen-Namen
                traverseTree(child, [&](TSNode bc) {
                    if (QString(ts_node_type(bc)) == "type_identifier")
                        bases << nodeText(bc, pr.src);
                });
            }
        }

        if (bases.isEmpty())
            output += QString("class %1  (line %2)  — no base classes\n")
                      .arg(className).arg(nodeLine(node));
        else
            output += QString("class %1  (line %2)  : %3\n")
                      .arg(className).arg(nodeLine(node)).arg(bases.join(", "));
    });

    return output.isEmpty() ? "No classes found." : output.trimmed();
}

// ─── get_call_graph ───────────────────────────────────────────────────────────
// Welche Funktionen ruft eine gegebene Funktion auf?
// Wir suchen innerhalb des Funktions-Body nach call_expression Nodes.
// call_expression hat ein "function" field mit dem Namen.
// ACHTUNG: Dies ist syntaktisch, nicht semantisch — foo.bar() und bar()
// werden beide als "bar" gelistet. Methodenaufrufe via Objekt sind erkennbar.
static QString toolGetCallGraph(const QString &path, const QString &funcName)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return "Error: " + err;

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

        // Alle call_expression Nodes im Body dieser Funktion
        TSNode body = childByField(node, "body");
        if (!valid(body)) { output += "  (no body)\n"; return; }

        QStringList calls;
        traverseTree(body, [&](TSNode cn) {
            if (QString(ts_node_type(cn)) != "call_expression") return;

            TSNode func = childByField(cn, "function");
            if (!valid(func)) return;

            QString callType = ts_node_type(func);
            QString callName;

            if (callType == "identifier") {
                // Einfacher Funktionsaufruf: foo()
                callName = nodeText(func, pr.src);
            } else if (callType == "field_expression") {
                // Methoden-/Member-Aufruf: obj.method() oder obj->method()
                TSNode field = childByField(func, "field");
                if (valid(field))
                    callName = nodeText(func, pr.src); // ganzer Ausdruck: "obj.method"
            } else if (callType == "qualified_identifier") {
                // Namespace-Aufruf: Foo::bar()
                callName = nodeText(func, pr.src);
            }

            if (!callName.isEmpty() && !calls.contains(callName))
                calls << QString("  %1  (line %2)").arg(callName).arg(nodeLine(cn));
        });

        if (calls.isEmpty())
            output += "  (no function calls found)\n";
        else
            output += calls.join("\n") + "\n";
    });

    return found ? output.trimmed()
                 : QString("Function '%1' not found in %2").arg(funcName).arg(path);
}

// ─── check_syntax ─────────────────────────────────────────────────────────────
// tree-sitter parst immer — Fehler werden als ERROR oder MISSING Nodes
// in den Baum eingebettet. Wir traversieren und sammeln alle ERROR-Nodes.
// Das ist kein vollständiger Compiler-Check, aber findet grobe Syntaxfehler
// wie fehlende Semikolons, nicht-geschlossene Klammern etc.
static QString toolCheckSyntax(const QString &path)
{
    QString err;
    ParseResult pr = parseFile(path, err);
    if (!pr.tree) return "Error: " + err;

    TSNode root = ts_tree_root_node(pr.tree);
    QString output;
    int errorCount = 0;

    traverseTree(root, [&](TSNode node) {
        // ts_node_has_error() → true wenn dieser Node ODER ein Kind einen Fehler hat
        // ts_node_is_error()  → true wenn dieser Node selbst ein ERROR-Node ist
        // ts_node_is_missing() → true für eingefügte "Phantom"-Tokens (MISSING)
        if (ts_node_is_error(node)) {
            TSPoint sp = ts_node_start_point(node);
            TSPoint ep = ts_node_end_point(node);
            QString snippet = nodeText(node, pr.src).left(60).replace('\n', ' ');
            output += QString("ERROR   line %1 col %2 — \"%3\"\n")
                      .arg(sp.row + 1).arg(sp.column + 1).arg(snippet);
            errorCount++;
        } else if (ts_node_is_missing(node)) {
            TSPoint sp = ts_node_start_point(node);
            // MISSING = tree-sitter hat ein Token "erfunden" um weiterparsern zu können
            // z.B. fehlendes Semikolon → MISSING ";"
            output += QString("MISSING line %1 col %2 — expected '%3'\n")
                      .arg(sp.row + 1).arg(sp.column + 1)
                      .arg(ts_node_type(node));
            errorCount++;
        }
    });

    if (errorCount == 0)
        return QString("No syntax errors found in %1").arg(path);

    return QString("%1 issue(s) in %2:\n").arg(errorCount).arg(path) + output.trimmed();
}

// ═══════════════════════════════════════════════════════════════════════════════
// TOOL LIST & MAIN
// ═══════════════════════════════════════════════════════════════════════════════

static QJsonArray makeToolList()
{
    auto prop = [](const QString &type, const QString &desc) {
        return QJsonObject{{"type", type}, {"description", desc}};
    };
    auto tool = [](const QString &name, const QString &desc,
                   const QJsonObject &props, const QJsonArray &req = {}) {
        return QJsonObject{{"name", name}, {"description", desc},
            {"inputSchema", QJsonObject{{"type","object"},{"properties",props},{"required",req}}}};
    };

    return QJsonArray{
        tool("list_symbols",
            "List all top-level symbols (functions, classes, structs, enums, typedefs) "
            "in a C/C++/CMake file with line numbers. "
            "Supported extensions: .c .cpp .h .hpp CMakeLists.txt. "
            "Path can be absolute (/home/thomas/ai/LlamaQT/src/Agent.h) "
            "or relative to sandbox root (LlamaQT/src/Agent.h).",
            {{"path", prop("string", "Absolute or sandbox-relative path to the source file")}},
            {"path"}),

        tool("get_function_body",
            "Extract the complete source of a named function from a C/C++ file. "
            "Returns the function with start and end line numbers. "
            "Path can be absolute or relative to sandbox root.",
            {{"path",      prop("string", "Absolute or sandbox-relative path to the source file")},
             {"function",  prop("string", "Exact function name")}},
            {"path","function"}),

        tool("get_class_members",
            "List all fields and methods of a class or struct with line numbers and "
            "access specifiers (public/private/protected). "
            "Works on C++ class and struct definitions. Qt classes (Q_OBJECT) are supported. "
            "Path can be absolute or relative to sandbox root.",
            {{"path",  prop("string", "Absolute or sandbox-relative path to the source file")},
             {"class", prop("string", "Exact class or struct name")}},
            {"path","class"}),

        tool("get_includes",
            "List all #include directives in a C/C++ file with line numbers. "
            "Path can be absolute or relative to sandbox root.",
            {{"path", prop("string", "Absolute or sandbox-relative path to the source file")}},
            {"path"}),

        tool("get_class_hierarchy",
            "Show all classes in a file and their base classes "
            "(inheritance: class Foo : public Bar). "
            "Qt classes (Q_OBJECT, signals, slots) are supported. "
            "Path can be absolute or relative to sandbox root.",
            {{"path", prop("string", "Absolute or sandbox-relative path to the source file")}},
            {"path"}),

        tool("get_call_graph",
            "List all function calls made by a given function (one level deep). "
            "Includes method calls (obj.method), qualified calls (Ns::func) and "
            "plain calls (func). Returns call site line numbers. "
            "Path can be absolute or relative to sandbox root.",
            {{"path",     prop("string", "Absolute or sandbox-relative path to the source file")},
             {"function", prop("string", "Exact function name to analyze")}},
            {"path","function"}),

        tool("check_syntax",
            "Check a source file for syntax errors using tree-sitter. "
            "Reports ERROR nodes (unexpected tokens) and MISSING nodes "
            "(tokens that tree-sitter had to invent to continue parsing) "
            "with line and column numbers. "
            "Note: this is a syntax check, not a full compiler check. "
            "Path can be absolute or relative to sandbox root.",
            {{"path", prop("string", "Absolute or sandbox-relative path to the source file")}},
            {"path"})
    };
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Roots einmalig beim Start initialisieren.
    // Ab jetzt löst resolvePath() relative Pfade korrekt auf.
    initRoots();

    QTextStream errStream(stderr);
    errStream << "[llamaqt-treesitter] Sandbox root:  " << g_sandboxRoot << "\n";
    errStream << "[llamaqt-treesitter] Sources root:  " << g_sourcesRoot << "\n";
    errStream.flush();

    QJsonArray tools = makeToolList();
    QTextStream in(stdin);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            errStream << "[llamaqt-treesitter] JSON parse error: "
                      << parseErr.errorString() << "\n";
            errStream.flush();
            continue;
        }

        QJsonObject msg = doc.object();
        QString method  = msg.value("method").toString();
        bool hasId      = msg.contains("id");
        int  id         = msg.value("id").toInt(-1);

        if (method == "initialize") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                {"protocolVersion","2024-11-05"},
                {"capabilities",   QJsonObject{}},
                {"serverInfo",     QJsonObject{{"name","llamaqt-treesitter"},{"version","1.0"}}}
            }}});
            continue;
        }
        if (method == "notifications/initialized") continue;

        if (method == "tools/list") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},
                          {"result",QJsonObject{{"tools",tools}}}});
            continue;
        }

        if (method == "tools/call") {
            QJsonObject params   = msg.value("params").toObject();
            QString toolName     = params.value("name").toString();
            QJsonObject args     = params.value("arguments").toObject();

            QString path         = args.value("path").toString();

            // Sicherheitscheck: nur erlaubte Dateiendungen
            if (!isAllowedFile(path) && toolName != "check_syntax") {
                // check_syntax soll auch bei unbekannten Dateien einen Fehler geben
            }

            QString result;

            if (toolName == "list_symbols") {
                result = toolListSymbols(path);
            } else if (toolName == "get_function_body") {
                result = toolGetFunctionBody(path, args.value("function").toString());
            } else if (toolName == "get_class_members") {
                result = toolGetClassMembers(path, args.value("class").toString());
            } else if (toolName == "get_includes") {
                result = toolGetIncludes(path);
            } else if (toolName == "get_class_hierarchy") {
                result = toolGetClassHierarchy(path);
            } else if (toolName == "get_call_graph") {
                result = toolGetCallGraph(path, args.value("function").toString());
            } else if (toolName == "check_syntax") {
                result = toolCheckSyntax(path);
            } else {
                sendResult(id, QString("Unknown tool: '%1'").arg(toolName), true);
                continue;
            }

            bool isErr = result.startsWith("Error:");
            sendResult(id, result, isErr);
            continue;
        }

        if (hasId) sendError(id, -32601, "Method not found: " + method);
    }
    return 0;
}
