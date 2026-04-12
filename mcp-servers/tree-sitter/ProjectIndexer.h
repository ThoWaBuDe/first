#pragma once
// ─── ProjectIndexer ───────────────────────────────────────────────────────────
// Generiert einen Markdown-Index des Sandbox-Projekts für das LLM.
//
// Was der Index enthält:
//   - Verzeichnisstruktur (kompakt, ohne .git)
//   - Pro Datei: alle Klassen mit Basisklassen
//   - Pro Klasse: Felder und Methoden mit Zeilennummern
//   - Top-Level Funktionen
//
// Cache-Strategie (analog zu make):
//   - Index liegt als .md Datei im Cache
//   - Bei jedem Aufruf von get() wird geprüft: ist der Cache älter als
//     die jüngste Datei in der Sandbox?
//   - Wenn ja: neu generieren
//   - Das ist dasselbe Prinzip wie make's Timestamp-Vergleich:
//     "ist das Ziel älter als eine der Quellen?"
//
// Warum nur Sandbox, nicht Sources?
//   - LlamaQT-Quellcode (Sources) ändert sich während einer Session nicht
//   - Das Modell arbeitet in der Sandbox — dort finden Änderungen statt
//   - Sources werden einmalig beim ersten Build indiziert
//
// Pattern: Strategy (parseFile ist austauschbar) + Template Method

#include "TreeSitterToolBase.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QString>
#include <QStringList>

class ProjectIndexer
{
public:
    // Konstruktor bekommt beide Roots und den Cache-Pfad.
    // Dependency Injection — kein Singleton, kein globaler State.
    ProjectIndexer(const QString &sandboxRoot,
                   const QString &sourceRoot,
                   const QString &cachePath)
        : m_sandboxRoot(sandboxRoot)
        , m_sourceRoot(sourceRoot)
        , m_cachePath(cachePath)
    {}

    // ── get() ─────────────────────────────────────────────────────────────
    // Hauptmethode: gibt den Index zurück.
    // Liest Cache wenn aktuell, generiert neu wenn veraltet.
    // Analog zu make: prüft Timestamps bevor es Arbeit macht.
    QString get(bool forceRebuild = false)
    {
        if (!forceRebuild && isCacheValid()) {
            return readCache();
        }
        QString index = generate();
        writeCache(index);
        return index;
    }

    // Für das rebuild_index Tool: Cache explizit löschen + neu generieren.
    QString rebuild()
    {
        QFile::remove(m_cachePath);
        return get(true);
    }

private:
    QString m_sandboxRoot;
    QString m_sourceRoot;
    QString m_cachePath;

    // ── Cache-Validierung ─────────────────────────────────────────────────
    // Prüft: existiert der Cache, und ist er neuer als alle Sandbox-Dateien?
    // Analogie (AVR/make): wie ein Dependency-Check —
    //   "ist output.hex neuer als main.c und config.h?"
    bool isCacheValid() const
    {
        QFileInfo cacheInfo(m_cachePath);
        if (!cacheInfo.exists()) return false;

        QDateTime cacheTime = cacheInfo.lastModified();
        QDateTime newestSource = newestFileTime(m_sandboxRoot);

        // Cache ist gültig wenn er neuer ist als alle Quelldateien
        return cacheTime > newestSource;
    }

    // Findet den neuesten Timestamp einer Datei in einem Verzeichnis (rekursiv).
    // Analogie: make prüft alle .o-Abhängigkeiten — wir prüfen alle .cpp/.h.
    static QDateTime newestFileTime(const QString &root)
    {
        QDateTime newest;
        QDirIterator it(root,
                        QStringList() << "*.cpp" << "*.h" << "*.hpp"
                                      << "*.c"   << "CMakeLists.txt",
                        QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            QDateTime t = it.fileInfo().lastModified();
            if (!newest.isValid() || t > newest)
                newest = t;
        }
        return newest;
    }

    QString readCache() const
    {
        QFile f(m_cachePath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QTextStream(&f).readAll();
    }

    void writeCache(const QString &content) const
    {
        // Cache-Verzeichnis anlegen falls nicht vorhanden
        QDir().mkpath(QFileInfo(m_cachePath).absolutePath());
        QFile f(m_cachePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream(&f) << content;
    }

    // ── generate() ────────────────────────────────────────────────────────
    // Generiert den vollständigen Markdown-Index.
    // Struktur:
    //   # Project Index
    //   ## Verzeichnisstruktur
    //   ## Sandbox: <datei>
    //      ### class Foo : Bar
    //          - field: m_x
    //          - method: doSomething() line 42
    //   ## Sources: <datei>  (nur Klassen/Funktionen, kein Code)
    QString generate() const
    {
        QString out;
        QTextStream ts(&out);

        ts << "# LlamaQt — Project Index\n";
        ts << "> Automatisch generiert. Zeigt Sandbox-Inhalt + LlamaQt-Quellcode.\n\n";

        // ── Verzeichnisstruktur ────────────────────────────────────────────
        ts << "## Verzeichnisstruktur (Sandbox)\n\n";
        ts << "```\n";
        ts << buildDirTree(m_sandboxRoot, "", 0, 3);
        ts << "```\n\n";

        ts << "## Verzeichnisstruktur (LlamaQt Sources)\n\n";
        ts << "```\n";
        ts << buildDirTree(m_sourceRoot, "", 0, 2);
        ts << "```\n\n";

        // ── Sandbox-Dateien ────────────────────────────────────────────────
        ts << "## Sandbox — Symbole\n\n";
        indexDirectory(m_sandboxRoot, "Sandbox", ts);

        // ── Source-Dateien ─────────────────────────────────────────────────
        ts << "## LlamaQt Sources — Symbole\n\n";
        indexDirectory(m_sourceRoot, "Sources", ts);

        return out;
    }

    // ── Verzeichnisbaum als ASCII ──────────────────────────────────────────
    // Rekursiv, maxDepth Ebenen. Analog zu TreeTool.h aber als QString.
    static QString buildDirTree(const QString &path, const QString &prefix,
                                int depth, int maxDepth)
    {
        if (depth > maxDepth) return prefix + "...\n";

        QDir dir(path);
        if (!dir.exists()) return {};

        QStringList entries = dir.entryList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
            QDir::Name | QDir::DirsFirst);
        entries.removeAll(".git");
        entries.removeAll("build");   // Build-Artefakte überspringen

        QString result;
        for (int i = 0; i < entries.size(); ++i) {
            bool isLast = (i == entries.size() - 1);
            QFileInfo fi(path + "/" + entries[i]);
            QString branch = isLast ? "└── " : "├── ";
            QString indent = isLast ? "    " : "│   ";

            if (fi.isSymLink()) {
                result += prefix + branch + entries[i] + " [symlink]\n";
            } else if (fi.isDir()) {
                result += prefix + branch + entries[i] + "/\n";
                result += buildDirTree(fi.absoluteFilePath(),
                                       prefix + indent, depth + 1, maxDepth);
            } else {
                result += prefix + branch + entries[i] + "\n";
            }
        }
        return result;
    }

    // ── Verzeichnis indizieren ─────────────────────────────────────────────
    // Iteriert alle C++/CMake-Dateien und extrahiert Symbole via tree-sitter.
    void indexDirectory(const QString &root, const QString &label,
                        QTextStream &ts) const
    {
        QDirIterator it(root,
                        QStringList() << "*.h" << "*.hpp" << "*.cpp"
                                      << "*.c" << "CMakeLists.txt",
                        QDir::Files,
                        QDirIterator::Subdirectories);

        // Dateien sammeln und sortieren — reproduzierbarer Output
        QStringList files;
        while (it.hasNext()) {
            QString p = it.next();
            if (QFileInfo(p).isSymLink()) continue;
            if (p.contains("/build/"))    continue;
            files << p;
        }
        files.sort();

        for (const QString &filePath : files) {
            QString rel = filePath.mid(root.length() + 1);
            QString symbols = extractSymbols(filePath);
            if (symbols.isEmpty()) continue;

            ts << "### " << rel << "\n\n";
            ts << symbols;
            ts << "\n";
        }
    }

    // ── Symbole aus einer Datei extrahieren ────────────────────────────────
    // Nutzt tree-sitter (via TreeSitterToolBase-Infrastruktur).
    // Gibt Markdown zurück:
    //   **class Foo** : Bar (line 12)
    //   - field: m_x (line 14)
    //   - method: doSomething() (line 20)
    //   function: freeFunc() (line 55)
    QString extractSymbols(const QString &filePath) const
    {
        QString parseErr;
        ParseResult pr = TreeSitterToolBase::parseFile(filePath, parseErr);
        if (!pr.valid()) return {};

        TSNode root = ts_tree_root_node(pr.tree);
        QString out;

        // Top-Level-Knoten durchgehen (direkte Kinder des Root)
        uint32_t n = ts_node_child_count(root);
        for (uint32_t i = 0; i < n; i++) {
            TSNode child = ts_node_child(root, i);
            QString type = ts_node_type(child);

            if (type == "class_specifier" || type == "struct_specifier") {
                out += extractClass(child, pr.src, type == "struct_specifier");

            } else if (type == "function_definition") {
                QString name = extractFuncName(child, pr.src);
                if (!name.isEmpty())
                    out += QString("- function: **%1**  (line %2)\n")
                           .arg(name)
                           .arg(TreeSitterToolBase::nodeLine(child));

            } else if (type == "declaration") {
                // Freie Variablen / typedef auf Top-Level
                TSNode decl = TreeSitterToolBase::childByField(child, "declarator");
                if (TreeSitterToolBase::nodeValid(decl)) {
                    QString name = TreeSitterToolBase::nodeText(decl, pr.src).trimmed();
                    if (!name.isEmpty() && name.length() < 60)
                        out += QString("- decl: %1  (line %2)\n")
                               .arg(name)
                               .arg(TreeSitterToolBase::nodeLine(child));
                }
            }
        }
        return out;
    }

    // ── Klasse/Struct extrahieren ──────────────────────────────────────────
    // Liest Name, Basisklassen, Felder und Methoden.
    static QString extractClass(TSNode node, const QByteArray &src, bool isStruct)
    {
        QString tag = isStruct ? "struct" : "class";

        // Klassenname
        TSNode nameNode = TreeSitterToolBase::childByField(node, "name");
        QString className = TreeSitterToolBase::nodeValid(nameNode)
                            ? TreeSitterToolBase::nodeText(nameNode, src)
                            : "<anonymous>";

        // Basisklassen sammeln
        QStringList bases;
        uint32_t nc = ts_node_child_count(node);
        for (uint32_t i = 0; i < nc; i++) {
            TSNode ch = ts_node_child(node, i);
            if (QString(ts_node_type(ch)) == "base_class_clause") {
                TreeSitterToolBase::traverseTree(ch, [&](TSNode bn) {
                    if (QString(ts_node_type(bn)) == "type_identifier")
                        bases << TreeSitterToolBase::nodeText(bn, src);
                });
            }
        }

        QString header = bases.isEmpty()
            ? QString("**%1 %2**  (line %3)\n")
              .arg(tag, className)
              .arg(TreeSitterToolBase::nodeLine(node))
            : QString("**%1 %2** : %3  (line %4)\n")
              .arg(tag, className, bases.join(", "))
              .arg(TreeSitterToolBase::nodeLine(node));

        // Body durchgehen: Felder und Methoden
        TSNode body = TreeSitterToolBase::childByField(node, "body");
        if (!TreeSitterToolBase::nodeValid(body)) return header;

        QString members;
        uint32_t nb = ts_node_child_count(body);
        for (uint32_t i = 0; i < nb; i++) {
            TSNode m  = ts_node_child(body, i);
            QString mt = ts_node_type(m);

            if (mt == "field_declaration") {
                // Feldname aus dem declarator
                TSNode decl = TreeSitterToolBase::childByField(m, "declarator");
                QString nm;
                if (TreeSitterToolBase::nodeValid(decl)) {
                    TreeSitterToolBase::traverseTree(decl, [&](TSNode nd) {
                        if (nm.isEmpty() &&
                            QString(ts_node_type(nd)) == "field_identifier")
                            nm = TreeSitterToolBase::nodeText(nd, src);
                    });
                }
                if (!nm.isEmpty())
                    members += QString("  - field: %1  (line %2)\n")
                               .arg(nm)
                               .arg(TreeSitterToolBase::nodeLine(m));

            } else if (mt == "function_definition" || mt == "declaration") {
                // Methode
                TSNode decl = TreeSitterToolBase::childByField(m, "declarator");
                QString nm;
                TreeSitterToolBase::traverseTree(decl, [&](TSNode nd) {
                    if (nm.isEmpty() &&
                        QString(ts_node_type(nd)) == "identifier")
                        nm = TreeSitterToolBase::nodeText(nd, src);
                });
                if (!nm.isEmpty()) {
                    QString tag2 = (mt == "function_definition") ? " [inline]" : "";
                    members += QString("  - method: %1%2  (line %3)\n")
                               .arg(nm, tag2)
                               .arg(TreeSitterToolBase::nodeLine(m));
                }

            } else if (mt == "access_specifier") {
                members += QString("  [%1]\n")
                           .arg(TreeSitterToolBase::nodeText(m, src).trimmed());
            }
        }

        return header + members;
    }

    // ── Funktionsname aus function_definition extrahieren ──────────────────
    static QString extractFuncName(TSNode node, const QByteArray &src)
    {
        TSNode decl = TreeSitterToolBase::childByField(node, "declarator");
        QString name;
        TreeSitterToolBase::traverseTree(decl, [&](TSNode n) {
            if (name.isEmpty() &&
                QString(ts_node_type(n)) == "identifier")
                name = TreeSitterToolBase::nodeText(n, src);
        });
        return name;
    }
};
