// ─── LlamaQt MCP WebSearch Server v3.0 ────────────────────────────────────────
// Umgebaut auf McpServer + PathPolicy + ToolBase (Klassen-basiert).

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <QProcessEnvironment>
#include <memory>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"

#include "WebsearchToolBase.h"
#include "WebSearchTool.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString apiKey = QProcessEnvironment::systemEnvironment()
                     .value("TAVILY_API_KEY");

    QString sandboxPath = QDir::homePath() + "/llamatools";
    PathPolicy policy;
    policy.addRoot(sandboxPath, true);

    QTextStream err(stderr);
    err << "[llamaqt-websearch v3.0]\n";
    if (apiKey.isEmpty()) {
        err << "  WARNING: TAVILY_API_KEY not set.\n"
            << "  Web search will fail. Set it with:\n"
            << "    export TAVILY_API_KEY=\"tvly-...\"\n";
    } else {
        err << "  API Key: set\n";
    }
    err << "  Sandbox: " << sandboxPath << "\n";
    err.flush();

    McpServer server("llamaqt-websearch", "3.0");

    server.registerTool(std::make_unique<WebSearchTool>(&policy, apiKey));

    server.run();
    return 0;
}