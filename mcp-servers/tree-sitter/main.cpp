// ─── LlamaQt MCP TreeSitter Server v3.0 ──────────────────────────────────────
// Neu in v3.0:
//   - GitHelper integriert (für ReplaceSymbolTool — schreibt in Sandbox)
//   - GetSymbolTool    — Funktionsrumpf/Klasse by name
//   - ReplaceSymbolTool — chirurgisches Editieren by name
//   - GetProjectIndexTool — Markdown-Index on-demand
//   - RebuildIndexTool — Cache explizit neu bauen
//   - McpConfig statt Umgebungsvariablen (liest INI direkt)
//
// Architektur:
//   - Zwei Roots: Sandbox (read+write für replace_symbol) + Sources (read-only)
//   - GitHelper lebt in main() auf dem Stack — same lifetime wie server
//   - McpConfig liest ~/.config/LlamaQt/LlamaQt.conf einmalig beim Start

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <memory>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"
#include "../common/McpConfig.h"
#include "../filesystem/GitHelper.h"

// Bestehende Tools
#include "ListSymbolsTool.h"
#include "GetFunctionBodyTool.h"
#include "GetClassMembersTool.h"
#include "GetIncludesTool.h"
#include "GetClassHierarchyTool.h"
#include "GetCallGraphTool.h"
#include "CheckSyntaxTool.h"

// Neue Tools v3.0
#include "GetSymbolTool.h"
#include "ReplaceSymbolTool.h"
#include "IndexTools.h"         // GetProjectIndexTool + RebuildIndexTool

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ── Konfiguration aus INI lesen ───────────────────────────────────────────
    // McpConfig liest ~/.config/LlamaQt/LlamaQt.conf direkt.
    // Umgebungsvariablen überschreiben INI-Werte (Fallback-Kompatibilität).
    McpConfig cfg;

    QString sandboxPath = cfg.sandboxRoot();
    QString sourcesPath = cfg.sourceRoot();
    QString trashPath   = cfg.trashPath();

    // ── PathPolicy aufbauen ───────────────────────────────────────────────────
    // Sandbox: writable=true  → replace_symbol darf hier schreiben
    // Sources: writable=false → nur Lesen (LlamaQT-Quellcode)
    PathPolicy policy;
    policy.addRoot(sandboxPath, true);
    policy.addRoot(sourcesPath, false);

    // ── GitHelper aufbauen ────────────────────────────────────────────────────
    // Neu in v3.0: ReplaceSymbolTool braucht git für auto-commit.
    // GitHelper lebt auf dem Stack in main() — überlebt alle Tools garantiert.
    GitHelper git(policy);

    // ── Logging ───────────────────────────────────────────────────────────────
    QTextStream err(stderr);
    err << "[llamaqt-treesitter v3.0]\n";
    err << "  Sandbox (rw): " << sandboxPath << "\n";
    err << "  Sources (ro): " << sourcesPath << "\n";
    err << "  Trash:        " << trashPath   << "\n";
    err << "  Index cache:  " << cfg.cachePath() << "\n";
    err.flush();

    // ── McpServer aufbauen ────────────────────────────────────────────────────
    McpServer server("llamaqt-treesitter", "3.0");

    // ── Bestehende Tools (v2.1) ───────────────────────────────────────────────
    server.registerTool(std::make_unique<ListSymbolsTool>(&policy));
    server.registerTool(std::make_unique<GetFunctionBodyTool>(&policy));
    server.registerTool(std::make_unique<GetClassMembersTool>(&policy));
    server.registerTool(std::make_unique<GetIncludesTool>(&policy));
    server.registerTool(std::make_unique<GetClassHierarchyTool>(&policy));
    server.registerTool(std::make_unique<GetCallGraphTool>(&policy));
    server.registerTool(std::make_unique<CheckSyntaxTool>(&policy));

    // ── Neue Tools (v3.0) ─────────────────────────────────────────────────────
    // GetSymbolTool: liest aus beiden Roots (kein GitHelper nötig)
    server.registerTool(std::make_unique<GetSymbolTool>(&policy));

    // ReplaceSymbolTool: schreibt in Sandbox → braucht GitHelper
    server.registerTool(std::make_unique<ReplaceSymbolTool>(&policy, &git));

    // Index-Tools: lesen INI selbst via McpConfig
    server.registerTool(std::make_unique<GetProjectIndexTool>(&policy));
    server.registerTool(std::make_unique<RebuildIndexTool>(&policy));

    server.run();
    return 0;
}
