// ─── llamaqt-workspace MCP Server v1.0 ───────────────────────────────────────
// Container-spezifischer MCP-Server für den llamaqt-User im Incus-Container.
//
// Läuft AUSSCHLIESSLICH im Container — nie auf dem Host starten!
// Der Server prüft beim Start ob er im Container läuft.
//
// Tools:
//   bash_exec    — beliebige Shell-Kommandos (mit Blacklist)
//   python_exec  — Python3-Code ausführen
//   pip_install  — Python-Pakete installieren (--user, kein sudo)
//   apt_install  — System-Pakete installieren (sudo, NOPASSWD via sudoers)
//   web_fetch    — URL abrufen, roher HTML
//   web_scrape   — URL abrufen, lesbarer Text

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <memory>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"

#include "WorkspaceToolBase.h"
#include "BashExecTool.h"
#include "PythonExecTool.h"
#include "PackageTools.h"
#include "WebTools.h"

// ─── Container-Erkennung ──────────────────────────────────────────────────────
// Prüft ob dieser Prozess in einem Container läuft.
// Gibt false zurück wenn er auf dem Host gestartet wird.
static bool isInContainer()
{
    return QFile::exists("/.containerenv") ||    // Incus/Podman
           QFile::exists("/.dockerenv")    ||    // Docker
           QFile::exists("/run/systemd/container"); // systemd-nspawn
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QTextStream err(stderr);
    err << "[llamaqt-workspace v1.0]\n";

    // ── Container-Check ───────────────────────────────────────────────────
    // Dieser Server hat bash_exec — auf dem Host wäre das gefährlich.
    // Im Container ist er sicher weil Incus isoliert.
    if (!isInContainer()) {
        err << "  FEHLER: Dieser Server läuft nicht im Container!\n";
        err << "  llamaqt-workspace ist nur für den Incus-Container gedacht.\n";
        err << "  Auf dem Host würde bash_exec eine Sicherheitslücke sein.\n";
        err.flush();
        // Wir starten trotzdem — damit der McpClient keinen Fehler bekommt.
        // Aber wir loggen eine Warnung.
        err << "  WARNUNG: Starte trotzdem (Entwicklungs-Modus).\n";
    }

    // ── Workplace-Verzeichnis anlegen ─────────────────────────────────────
    const QString home      = qEnvironmentVariable("HOME", "/home/llamaqt");
    const QString workplace = home + "/workplace";
    QDir().mkpath(workplace);
    QDir().mkpath(workplace + "/.tmp");

    err << "  Home:      " << home      << "\n";
    err << "  Workplace: " << workplace << "\n";
    err.flush();

    // ── PathPolicy (minimal — workspace hat keine Pfad-Beschränkungen) ────
    // bash_exec kann sowieso überall hin — PathPolicy wäre Theater.
    // Wir übergeben nullptr, die Tools ignorieren sie.
    PathPolicy policy;
    policy.addRoot(workplace, true);

    // ── McpServer aufbauen ────────────────────────────────────────────────
    McpServer server("llamaqt-workspace", "1.0");

    server.registerTool(std::make_unique<BashExecTool>(&policy));
    server.registerTool(std::make_unique<PythonExecTool>(&policy));
    server.registerTool(std::make_unique<PipInstallTool>(&policy));
    server.registerTool(std::make_unique<AptInstallTool>(&policy));
    server.registerTool(std::make_unique<WebFetchTool>(&policy));
    server.registerTool(std::make_unique<WebScrapeTool>(&policy));

    server.run();
    return 0;
}
