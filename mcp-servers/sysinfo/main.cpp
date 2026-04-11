// ─── LlamaQt MCP SysInfo Server v3.0 ─────────────────────────────────────────
// Umgebaut auf McpServer + PathPolicy + ToolBase (Klassen-basiert).
//
// Änderungen gegenüber v2.0:
//   - JSON-RPC Loop über McpServer.h (gemeinsame Infrastruktur)
//   - Tool-Handler als ToolBase-Klassen (eine .h pro Tool)
//   - PathPolicy integriert (vorbereitet für spätere Dateizugriffe)
//   - 6 neue Tools: process_list, networking, system_status, disk_io, cuda_info, battery

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <memory>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"

#include "GetTimeTool.h"
#include "GetPwdTool.h"
#include "DiskFreeTool.h"
#include "SysInfoTool.h"
#include "GpuInfoTool.h"
#include "SetPowerLimitTool.h"
#include "ProcessListTool.h"
#include "NetworkingTool.h"
#include "SystemStatusTool.h"
#include "DiskIoTool.h"
#include "CudaInfoTool.h"
#include "BatteryTool.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // PathPolicy aufbauen (vorbereitet für spätere Erweiterungen)
    // sysinfo braucht keine Dateizugriffe, aber die Infrastruktur ist bereit
    QString sandboxPath = QDir::homePath() + "/llamatools";
    PathPolicy policy;
    policy.addRoot(sandboxPath, true);

    QTextStream err(stderr);
    err << "[llamaqt-sysinfo v3.0]\n";
    err << "  Sandbox: " << sandboxPath << "\n";
    err.flush();

    // McpServer aufbauen + Tools registrieren
    McpServer server("llamaqt-sysinfo", "3.0");

    server.registerTool(std::make_unique<GetTimeTool>(&policy));
    server.registerTool(std::make_unique<GetPwdTool>(&policy));
    server.registerTool(std::make_unique<DiskFreeTool>(&policy));
    server.registerTool(std::make_unique<SysInfoTool>(&policy));
    server.registerTool(std::make_unique<GpuInfoTool>(&policy));
    server.registerTool(std::make_unique<SetPowerLimitTool>(&policy));
    server.registerTool(std::make_unique<ProcessListTool>(&policy));
    server.registerTool(std::make_unique<NetworkingTool>(&policy));
    server.registerTool(std::make_unique<SystemStatusTool>(&policy));
    server.registerTool(std::make_unique<DiskIoTool>(&policy));
    server.registerTool(std::make_unique<CudaInfoTool>(&policy));
    server.registerTool(std::make_unique<BatteryTool>(&policy));

    server.run();
    return 0;
}