// ─── LlamaQt MCP Compile Server v3.0 ─────────────────────────────────────────
// Umgebaut auf McpServer + PathPolicy + ToolBase (Klassen-basiert).

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <memory>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"

#include "CompileToolBase.h"
#include "CmakeBuildTool.h"
#include "PkgStatusTool.h"
#include "CheckRunTool.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString sandboxPath = QDir::homePath() + "/llamatools";
    PathPolicy policy;
    policy.addRoot(sandboxPath, true);

    QTextStream err(stderr);
    err << "[llamaqt-compile v3.0]\n";
    err << "  Sandbox: " << sandboxPath << "\n";
    err.flush();

    McpServer server("llamaqt-compile", "3.0");

    server.registerTool(std::make_unique<CmakeBuildTool>(&policy));
    server.registerTool(std::make_unique<PkgStatusTool>(&policy));
    server.registerTool(std::make_unique<CheckRunTool>(&policy));

    server.run();
    return 0;
}