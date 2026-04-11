// ─── LlamaQt MCP Filesystem Server v3.0 ──────────────────────────────────────
// Umgebaut auf McpServer + PathPolicy + ToolBase (Klassen-basiert).
//
// Änderungen gegenüber v2.3:
//   - Tool-Logik vollständig in eigene Klassen ausgelagert (eine .h pro Tool)
//   - ToolBase als gemeinsames Interface (mcp-servers/common/ToolBase.h)
//   - McpServer hält std::unique_ptr<ToolBase> — kein manuelles delete
//   - PathPolicy verwaltet alle Pfad-Sicherheitsprüfungen (Symlink, Roots)
//   - GitHelper kapselt alle git-Operationen als Klasse
//   - Absolute Pfade für Lesezugriff erlaubt (wenn in bekannter Root)
//   - list_symbols entfernt (tree-sitter MCP macht das besser)
//   - Trash-Pfad via LLAMAQT_TRASH Umgebungsvariable konfigurierbar
//   - main() ist reine Verdrahtung — keine Business-Logik mehr hier
//
// Zwei Roots (via Umgebungsvariablen konfigurierbar):
//   LLAMAQT_SANDBOX  — Projektdateien (Standard: ~/llamatools)     read+write
//   LLAMAQT_SOURCES  — LlamaQt Quellcode (Standard: ~/ai/LlamaQT) read-only
//   LLAMAQT_TRASH    — Papierkorb      (Standard: ~/.llamatools_trash)

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <memory>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"
#include "GitHelper.h"

// ── Tool-Klassen ──────────────────────────────────────────────────────────────
// Jede Klasse ist vollständig in ihrer eigenen .h Datei definiert.
// main.cpp kennt alle Tool-Klassen — aber nur für die Registrierung.
// McpServer und GitHelper kennen keine konkreten Tool-Klassen.
#include "ReadFileTool.h"
#include "WriteFileTool.h"
#include "AppendFileTool.h"
#include "StrReplaceTool.h"
#include "PatchFileTool.h"
#include "ListDirTool.h"
#include "MkdirTool.h"
#include "GrepCodeTool.h"
#include "TreeTool.h"
#include "SearchCodeTool.h"
#include "FindFilesTool.h"
#include "ReadMultipleFilesTool.h"
#include "MoveFileTool.h"
#include "CopyFileTool.h"
#include "GitStatusTool.h"
#include "GitDiffTool.h"
#include "GitLogTool.h"
#include "GitCheckoutTool.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ── Konfiguration aus Umgebungsvariablen ──────────────────────────────────
    // qEnvironmentVariable() gibt den Wert der Env-Variable zurück,
    // oder den Default-Wert wenn die Variable nicht gesetzt ist.
    QString sandboxPath = qEnvironmentVariable(
        "LLAMAQT_SANDBOX",
        QDir::homePath() + "/llamatools");

    QString sourcesPath = qEnvironmentVariable(
        "LLAMAQT_SOURCES",
        QDir::homePath() + "/ai/LlamaQT");

    QString trashPath = qEnvironmentVariable(
        "LLAMAQT_TRASH",
        QDir::homePath() + "/.llamatools_trash");

    // Sandbox anlegen falls nicht vorhanden
    QDir().mkpath(sandboxPath);
    QDir().mkpath(trashPath);

    // ── PathPolicy aufbauen ───────────────────────────────────────────────────
    // Zwei Roots: Sandbox (schreibbar) + LlamaQt-Sources (read-only).
    // PathPolicy prüft für jeden Pfad:
    //   - Zugehörigkeit zu einer Root
    //   - Symlink-Freiheit auf jeder Pfad-Komponente
    //   - Schreiberlaubnis (nur writable Roots)
    PathPolicy policy;
    policy.addRoot(sandboxPath, true);    // writable = Sandbox
    policy.addRoot(sourcesPath, false);   // read-only = LlamaQt-Quellcode

    // ── GitHelper aufbauen ────────────────────────────────────────────────────
    // Bekommt Policy per Referenz — braucht primarySandbox() für git-Repos.
    // GitHelper lebt auf dem Stack in main(), genauso wie policy und server.
    // Alle drei haben dieselbe Lebensdauer → keine Dangling-Pointer-Gefahr.
    GitHelper git(policy);

    // ── Logging ───────────────────────────────────────────────────────────────
    QTextStream err(stderr);
    err << "[llamaqt-filesystem v3.0]\n";
    err << "  Sandbox (rw): " << sandboxPath << "\n";
    err << "  Sources (ro): " << sourcesPath << "\n";
    err << "  Trash:        " << trashPath   << "\n";
    err.flush();

    // ── McpServer aufbauen ────────────────────────────────────────────────────
    McpServer server("llamaqt-filesystem", "3.0");

    // ── Tools registrieren ────────────────────────────────────────────────────
    // Reihenfolge bestimmt die Reihenfolge in tools/list (sichtbar für das LLM).
    // make_unique<T>(args) erzeugt ein unique_ptr<T> — server übernimmt Ownership.
    //
    // Dependency Injection Chain:
    //   main() kennt policy und git.
    //   Tool-Konstruktoren bekommen &policy und &git.
    //   McpServer kennt nur ToolBase* — weiß nichts von policy oder git.
    //
    // Analogie (AVR): main() initialisiert UART, SPI etc. und übergibt
    // Zeiger an die jeweiligen Treiber. Die Treiber selbst kennen sich nicht.

    // Datei-Tools
    server.registerTool(std::make_unique<ReadFileTool>(&policy, &git));
    server.registerTool(std::make_unique<WriteFileTool>(&policy, &git));
    server.registerTool(std::make_unique<AppendFileTool>(&policy, &git));
    server.registerTool(std::make_unique<StrReplaceTool>(&policy, &git));
    server.registerTool(std::make_unique<PatchFileTool>(&policy, &git));

    // Verzeichnis-Tools
    server.registerTool(std::make_unique<ListDirTool>(&policy, &git));
    server.registerTool(std::make_unique<MkdirTool>(&policy, &git));
    server.registerTool(std::make_unique<TreeTool>(&policy, &git));

    // Such-Tools
    server.registerTool(std::make_unique<GrepCodeTool>(&policy, &git));
    server.registerTool(std::make_unique<SearchCodeTool>(&policy, &git));
    server.registerTool(std::make_unique<FindFilesTool>(&policy, &git));
    server.registerTool(std::make_unique<ReadMultipleFilesTool>(&policy, &git));

    // Datei-Operationen
    // MoveFileTool bekommt zusätzlich den trashPath — eigener Konstruktor
    server.registerTool(std::make_unique<MoveFileTool>(&policy, &git, trashPath));
    server.registerTool(std::make_unique<CopyFileTool>(&policy, &git));

    // Git-Tools
    server.registerTool(std::make_unique<GitStatusTool>(&policy, &git));
    server.registerTool(std::make_unique<GitDiffTool>(&policy, &git));
    server.registerTool(std::make_unique<GitLogTool>(&policy, &git));
    server.registerTool(std::make_unique<GitCheckoutTool>(&policy, &git));

    // ── Hauptschleife ─────────────────────────────────────────────────────────
    // run() blockiert bis stdin EOF (MCP-Client trennt die Verbindung).
    // Danach werden server, git, policy auf dem Stack automatisch zerstört —
    // unique_ptr-Destruktoren löschen alle Tool-Objekte, kein manuelles delete.
    server.run();
    return 0;
}
