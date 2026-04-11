// ─── LlamaQt MCP TreeSitter Server v2.1 ──────────────────────────────────────
// Umgebaut auf McpServer + PathPolicy + ToolBase (Klassen-basiert).
//
// Änderungen gegenüber v2.0:
//   - Tool-Logik vollständig in eigene Klassen ausgelagert (eine .h pro Tool)
//   - TreeSitterToolBase als gemeinsame Zwischenschicht
//   - ParseResult + Hilfsfunktionen in TreeSitterToolBase.h
//   - main() ist reine Verdrahtung — keine Tool-Logik mehr hier
//   - Zwei Roots via Umgebungsvariablen (wie filesystem-Server)
//
// Zwei Roots:
//   LLAMAQT_SANDBOX  — Projektdateien (Standard: ~/llamatools)     read-only
//   LLAMAQT_SOURCES  — LlamaQt Quellcode (Standard: ~/ai/LlamaQT) read-only
// (tree-sitter schreibt nie — beide Roots sind read-only aus Sicht der Tools)

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <memory>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"

#include "ListSymbolsTool.h"
#include "GetFunctionBodyTool.h"
#include "GetClassMembersTool.h"
#include "GetIncludesTool.h"
#include "GetClassHierarchyTool.h"
#include "GetCallGraphTool.h"
#include "CheckSyntaxTool.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ── Konfiguration aus Umgebungsvariablen ──────────────────────────────────
    QString sandboxPath = qEnvironmentVariable(
        "LLAMAQT_SANDBOX",
        QDir::homePath() + "/llamatools");

    QString sourcesPath = qEnvironmentVariable(
        "LLAMAQT_SOURCES",
        QDir::homePath() + "/ai/LlamaQT");

    // ── PathPolicy aufbauen ───────────────────────────────────────────────────
    // Beide Roots sind für den tree-sitter-Server read-only:
    //   writable=true  bedeutet in PathPolicy nur "auch Schreiben erlaubt".
    //   Wir setzen writable=true für die Sandbox damit relative Pfade
    //   korrekt aufgelöst werden (resolveRead sucht zuerst in writable Roots).
    //   Die Tool-Klassen rufen nur resolveRead() auf — kein Schreiben möglich.
    PathPolicy policy;
    policy.addRoot(sandboxPath, true);   // Sandbox: Lesen + (theoretisch) Schreiben
    policy.addRoot(sourcesPath, false);  // Sources: nur Lesen

    QTextStream err(stderr);
    err << "[llamaqt-treesitter v2.1]\n";
    err << "  Sandbox: " << sandboxPath << "\n";
    err << "  Sources: " << sourcesPath << "\n";
    err.flush();

    // ── McpServer aufbauen + Tools registrieren ───────────────────────────────
    // Dependency Injection Chain:
    //   main() kennt policy.
    //   Alle Tools bekommen &policy im Konstruktor.
    //   McpServer kennt nur ToolBase* — weiß nichts von PathPolicy.
    McpServer server("llamaqt-treesitter", "2.1");

    server.registerTool(std::make_unique<ListSymbolsTool>(&policy));
    server.registerTool(std::make_unique<GetFunctionBodyTool>(&policy));
    server.registerTool(std::make_unique<GetClassMembersTool>(&policy));
    server.registerTool(std::make_unique<GetIncludesTool>(&policy));
    server.registerTool(std::make_unique<GetClassHierarchyTool>(&policy));
    server.registerTool(std::make_unique<GetCallGraphTool>(&policy));
    server.registerTool(std::make_unique<CheckSyntaxTool>(&policy));

    server.run();
    return 0;
}
