#pragma once
// ─── TreeSitterToolBase ───────────────────────────────────────────────────────
// Zwischenschicht zwischen ToolBase und den konkreten tree-sitter Tools.
//
// Enthält:
//   1. Parse-Infrastruktur: ParseResult, parseFile(), Hilfsfunktionen
//   2. Basisklasse TreeSitterToolBase mit Konstruktor-Injektion (PathPolicy*)
//
// Analog zu FilesystemToolBase.h im filesystem-Server — dort leben die
// Limits und resolveRead()/resolveWrite() in der Basisklasse.
// Hier leben ParseResult und parseFile() in der Basisklasse.
//
// Warum PathPolicy* statt PathPolicy&?
//   Tool-Objekte leben in unique_ptr und werden in std::unordered_map
//   gespeichert. unique_ptr ist move-only — Referenz-Member verbieten
//   den impliziten Move-Constructor. Zeiger haben dieses Problem nicht.
//   Die PathPolicy selbst lebt in main() auf dem Stack und überlebt
//   garantiert alle Tool-Objekte.
//
// tree-sitter C API wird hier zentral eingebunden — alle Tool-Header
// brauchen nur TreeSitterToolBase.h zu inkludieren.

#include "../common/ToolBase.h"
#include "../common/PathPolicy.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <tree_sitter/api.h>
#include <functional>
#include <cstring>
#include <cctype>

// Externe C-Funktionen aus den Grammar-Bibliotheken.
// 'extern "C"' verhindert C++ Name-Mangling — die Symbole heißen in der
// .so/.a Datei genau so wie in C, ohne Typ-Suffix.
extern "C" {
    const TSLanguage *tree_sitter_cpp(void);
    const TSLanguage *tree_sitter_cmake(void);
}

// ─── ParseResult ─────────────────────────────────────────────────────────────
// RAII-Wrapper um TSParser + TSTree + Quelldatei-Inhalt.
//
// RAII = Resource Acquisition Is Initialization:
//   Analogie (AVR): wie ein Treiber der im Konstruktor den Peripheral
//   initialisiert und im Destruktor wieder freigibt — automatisch, ohne
//   manuelles Aufräumen. Kein ts_tree_delete() vergessen möglich.
//
// Nicht kopierbar (delete), aber verschiebbar (move) —
// genau wie unique_ptr: ein Besitzer zur Zeit.
struct ParseResult {
    TSParser  *parser = nullptr;
    TSTree    *tree   = nullptr;
    QByteArray src;

    ~ParseResult() {
        if (tree)   ts_tree_delete(tree);
        if (parser) ts_parser_delete(parser);
    }

    // Kopieren verboten — zwei Objekte dürfen denselben TSTree nicht besitzen.
    ParseResult(const ParseResult &) = delete;
    ParseResult &operator=(const ParseResult &) = delete;

    // Verschieben erlaubt — Ownership geht über, Original wird genullt.
    ParseResult(ParseResult &&o) noexcept
        : parser(o.parser), tree(o.tree), src(std::move(o.src))
    { o.parser = nullptr; o.tree = nullptr; }

    ParseResult() = default;

    // Hilfsmethode: ist das Ergebnis gültig?
    bool valid() const { return tree != nullptr; }
};

// ─── TreeSitterToolBase ───────────────────────────────────────────────────────
class TreeSitterToolBase : public ToolBase
{
public:
    explicit TreeSitterToolBase(PathPolicy *policy)
        : m_policy(policy)
    {}

protected:
    PathPolicy *m_policy;

    // ── Schema-Hilfsmethoden ─────────────────────────────────────────────────
    // Analog zu FilesystemToolBase::prop() und req().

    static QJsonObject prop(const QString &type, const QString &desc)
    {
        return QJsonObject{{"type", type}, {"description", desc}};
    }

    static QJsonArray req(std::initializer_list<const char*> fields)
    {
        QJsonArray arr;
        for (const char *f : fields) arr.append(f);
        return arr;
    }

    // ── Pfad-Auflösung ───────────────────────────────────────────────────────

    // Löst einen Pfad für Lesezugriff auf (alle bekannten Roots).
    // Gibt {absPath, ""} bei Erfolg, {"", Fehlermeldung} bei Fehler.
    QPair<QString,QString> resolveRead(const QString &path) const
    {
        auto rp = m_policy->resolveRead(path);
        if (!rp.valid) return {"", rp.error};
        return {rp.absPath, ""};
    }

    // ── Datei-Typ-Prüfung ────────────────────────────────────────────────────

    static bool isAllowedFile(const QString &path)
    {
        QFileInfo fi(path);
        QString name = fi.fileName();
        QString suf  = fi.suffix().toLower();
        if (name == "CMakeLists.txt") return true;
        return (suf == "c" || suf == "cpp" || suf == "h" || suf == "hpp");
    }

    // ── Qt-Macro-Neutralisierung ─────────────────────────────────────────────
    // Qt-Macros (Q_OBJECT, signals:, slots: etc.) verwirren den C++-Parser
    // von tree-sitter. Wir ersetzen sie durch Leerzeichen gleicher Länge —
    // so bleiben Byte-Offsets (= Zeilennummern) korrekt.
    //
    // Analogie (AVR): wie ein UART-Filter der bestimmte Steuerzeichen durch
    // Leerzeichen ersetzt ohne die Frame-Länge zu ändern.
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

    // ── Sprache bestimmen ────────────────────────────────────────────────────

    static const TSLanguage *languageForFile(const QString &path)
    {
        if (QFileInfo(path).fileName() == "CMakeLists.txt")
            return tree_sitter_cmake();
        return tree_sitter_cpp();
    }

    // ── Parsen ───────────────────────────────────────────────────────────────
    // Öffnet die Datei, neutralisiert Qt-Macros (bei C/C++), parst mit
    // tree-sitter. Gibt ein gültiges ParseResult oder errOut gesetzt zurück.
    //
    // Rückgabe per Wert ist effizient wegen Move-Semantik (RVO/NRVO):
    // der Compiler optimiert die Kopie weg — wie DMA statt memcpy.
    static ParseResult parseFile(const QString &absPath, QString &errOut)
    {
        ParseResult result;

        if (!isAllowedFile(absPath)) {
            errOut = "File type not allowed. Supported: .c .cpp .h .hpp CMakeLists.txt";
            return result;
        }

        QFile f(absPath);
        if (!f.open(QIODevice::ReadOnly)) {
            errOut = "Cannot open file: " + absPath;
            return result;
        }
        result.src = f.readAll();

        QString suf = QFileInfo(absPath).suffix().toLower();
        if (suf == "h" || suf == "hpp" || suf == "cpp" || suf == "c")
            result.src = neutralizeQtMacros(result.src);

        result.parser = ts_parser_new();
        ts_parser_set_language(result.parser, languageForFile(absPath));
        result.tree = ts_parser_parse_string(
            result.parser, nullptr,
            result.src.constData(),
            static_cast<uint32_t>(result.src.size())
        );
        return result;
    }

    // ── Node-Hilfsfunktionen ─────────────────────────────────────────────────
    // Klein und inline — der Compiler kann sie wegoptimieren.

    static QString nodeText(TSNode node, const QByteArray &src)
    {
        uint32_t s = ts_node_start_byte(node);
        uint32_t e = ts_node_end_byte(node);
        return QString::fromUtf8(src.mid(static_cast<int>(s),
                                         static_cast<int>(e - s)));
    }

    static int nodeLine(TSNode node)
    {
        return static_cast<int>(ts_node_start_point(node).row) + 1;
    }

    static TSNode childByField(TSNode node, const char *field)
    {
        return ts_node_child_by_field_name(
            node, field, static_cast<uint32_t>(std::strlen(field)));
    }

    static bool nodeValid(TSNode node) { return !ts_node_is_null(node); }

    // ── Baum-Traversierung ───────────────────────────────────────────────────
    // Iterativer Preorder-Durchlauf mit TSTreeCursor.
    //
    // Warum iterativ statt rekursiv?
    //   Große C++-Dateien können sehr tiefe AST-Bäume erzeugen.
    //   Rekursion würde den Stack überlasten (kein Tail-Call in C++).
    //   TSTreeCursor ist ein expliziter Stack — sicher für beliebige Tiefe.
    //   Analogie (AVR): iterativer DFS statt rekursivem, weil der AVR-Stack
    //   nur 256 Byte hat.
    //
    // cb wird für jeden Knoten aufgerufen — std::function erlaubt Lambdas
    // die lokale Variablen per Referenz capturieren ([&]).
    static void traverseTree(TSNode root,
                              const std::function<void(TSNode)> &cb)
    {
        TSTreeCursor cursor = ts_tree_cursor_new(root);
        bool reachedRoot = false;
        while (!reachedRoot) {
            cb(ts_tree_cursor_current_node(&cursor));
            if (ts_tree_cursor_goto_first_child(&cursor))  continue;
            if (ts_tree_cursor_goto_next_sibling(&cursor)) continue;
            while (true) {
                if (!ts_tree_cursor_goto_parent(&cursor)) {
                    reachedRoot = true; break;
                }
                if (ts_tree_cursor_goto_next_sibling(&cursor)) break;
            }
        }
        ts_tree_cursor_delete(&cursor);
    }
};
