// ─── McpServer.cpp ────────────────────────────────────────────────────────────
// Implementierung des JSON-RPC 2.0 Loops.
//
// Warum eine .cpp Datei?
//   McpServer.h forward-deklariert ToolBase (class ToolBase;).
//   Die Implementierung von registerTool(unique_ptr<ToolBase>) und run()
//   braucht die vollständige Definition von ToolBase — also den #include.
//   Das wäre in einer reinen Header-Datei ein zirkuläres Include-Problem:
//     McpServer.h  →  ToolBase.h  →  McpServer.h  (für ToolResult)
//   Lösung: ToolResult ist in McpServer.h, die ToolBase-Nutzung ist in McpServer.cpp.

#include "McpServer.h"
#include "ToolBase.h"

#include <QTextStream>
#include <QJsonDocument>
#include <QJsonParseError>

void McpServer::registerTool(std::unique_ptr<ToolBase> tool)
{
    // Name vor dem Move merken — nach std::move() ist tool ungültig.
    QString toolName = tool->name();
    m_toolOrder.append(toolName);
    m_tools.insert(toolName, std::move(tool));
}

void McpServer::run()
{
    QTextStream in(stdin);
    QTextStream errStream(stderr);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            errStream << "[" << m_name << "] JSON parse error: "
                      << parseErr.errorString() << "\n";
            errStream.flush();
            continue;
        }

        QJsonObject msg = doc.object();
        QString method  = msg.value("method").toString();
        bool hasId      = msg.contains("id");
        int  id         = msg.value("id").toInt(-1);

        // ── initialize ────────────────────────────────────────────────────────
        if (method == "initialize") {
            sendResponse({
                {"jsonrpc", "2.0"}, {"id", id},
                {"result", QJsonObject{
                    {"protocolVersion", "2024-11-05"},
                    {"capabilities",   QJsonObject{}},
                    {"serverInfo",     QJsonObject{
                        {"name",    m_name},
                        {"version", m_version}
                    }}
                }}
            });
            continue;
        }

        if (method == "notifications/initialized") continue;

        // ── tools/list ────────────────────────────────────────────────────────
        // Gibt alle registrierten Tools aus — ToolBase-Objekte und Legacy-Structs.
        // Reihenfolge: wie in m_toolOrder (= Reihenfolge der registerTool()-Aufrufe).
        if (method == "tools/list") {
            QJsonArray toolList;
            for (const QString &name : m_toolOrder) {
                if (m_tools.contains(name))
                    toolList.append(m_tools[name]->toSchema());
                else if (m_legacyTools.contains(name))
                    toolList.append(m_legacyTools[name].toSchema());
            }
            sendResponse({
                {"jsonrpc", "2.0"}, {"id", id},
                {"result", QJsonObject{{"tools", toolList}}}
            });
            continue;
        }

        // ── tools/call ────────────────────────────────────────────────────────
        // Dispatch: Name → ToolBase::execute() oder Tool::execute().
        if (method == "tools/call") {
            QJsonObject params = msg.value("params").toObject();
            QString toolName   = params.value("name").toString();
            QJsonObject args   = params.value("arguments").toObject();

            if (m_tools.contains(toolName)) {
                // Neuer Weg: virtueller Dispatch über ToolBase*
                ToolResult result = m_tools[toolName]->execute(args);
                sendResult(id, result);
            } else if (m_legacyTools.contains(toolName)) {
                // Alter Weg: Lambda-Aufruf
                ToolResult result = m_legacyTools[toolName].execute(args);
                sendResult(id, result);
            } else {
                sendResult(id, ToolResult::err(
                    QString("Error: unknown tool '%1'").arg(toolName)));
            }
            continue;
        }

        // ── unbekannte Methode ────────────────────────────────────────────────
        if (hasId) {
            sendResponse({
                {"jsonrpc", "2.0"}, {"id", id},
                {"error", QJsonObject{
                    {"code",    -32601},
                    {"message", "Method not found: " + method}
                }}
            });
        }
    }
}
