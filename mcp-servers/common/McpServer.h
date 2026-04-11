#pragma once
// ─── McpServer ────────────────────────────────────────────────────────────────
// Gemeinsamer JSON-RPC 2.0 Loop und Tool-Registry für alle LlamaQt MCP-Server.
//
// Include-Hierarchie (azyklisch):
//   ToolResult.h  ←  ToolBase.h  ←  McpServer.h
//
// McpServer.h inkludiert ToolBase.h direkt — das ist sicher weil ToolBase.h
// nicht mehr McpServer.h inkludiert (ToolResult ist in ToolResult.h).
//
// Warum std::unordered_map statt QHash für unique_ptr?
//   QHash ist copy-on-write (CoW): intern kann Qt die Map kopieren wenn
//   man per [] liest. Das erfordert dass der Value-Typ kopierbar ist.
//   unique_ptr ist absichtlich NICHT kopierbar (deleted copy constructor).
//   std::unordered_map hat kein CoW — move-only Types sind erlaubt.
//   emplace() mit std::move() funktioniert korrekt.
//
// Key-Typ: std::string statt QString — std::unordered_map braucht std::hash,
// der für std::string eingebaut ist, nicht für QString.
// Konvertierung: QString::toStdString() / QString::fromStdString().
//
// McpServer.h ist header-only — kein McpServer.cpp mehr nötig.
// run() ist inline implementiert (langer Code, aber nur einmal kompiliert
// da McpServer.h via #pragma once nur einmal pro TU eingebunden wird).

#include "ToolResult.h"
#include "ToolBase.h"

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QVector>
#include <QTextStream>
#include <QHash>

#include <functional>
#include <memory>
#include <unordered_map>
#include <iostream>

// ─── Tool-Struct ──────────────────────────────────────────────────────────────
// Bleibt für Lambda-basierte Server (z.B. tree-sitter) erhalten.
// QHash<QString, Tool> ist ok — Tool enthält kein unique_ptr, ist kopierbar.
struct Tool {
    QString     name;
    QString     description;
    QJsonObject properties;
    QJsonArray  required;

    std::function<ToolResult(const QJsonObject &args)> execute;

    QJsonObject toSchema() const
    {
        return QJsonObject{
            {"name",        name},
            {"description", description},
            {"inputSchema", QJsonObject{
                {"type",       "object"},
                {"properties", properties},
                {"required",   required}
            }}
        };
    }
};

// ─── McpServer ────────────────────────────────────────────────────────────────
class McpServer
{
public:
    McpServer(const QString &name, const QString &version)
        : m_name(name), m_version(version)
    {}

    // ToolBase-Klasse registrieren (bevorzugt).
    // Server übernimmt Ownership via unique_ptr.
    void registerTool(std::unique_ptr<ToolBase> tool)
    {
        QString toolName = tool->name();
        m_toolOrder.append(toolName);
        // emplace() mit rvalue — kein Kopieren, nur verschieben.
        // Analogie (AVR): wie DMA-Transfer, kein Byte-für-Byte-Kopieren.
        m_tools.emplace(toolName.toStdString(), std::move(tool));
    }

    // Tool-Struct registrieren (für Lambda-basierte Server).
    void registerTool(Tool tool)
    {
        m_toolOrder.append(tool.name);
        m_legacyTools.insert(tool.name, std::move(tool));
    }

    // Hauptschleife: liest JSON-RPC Zeilen von stdin, schreibt auf stdout.
    // Blockiert bis stdin EOF (= MCP-Client trennt Verbindung).
    void run()
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

            if (method == "tools/list") {
                QJsonArray toolList;
                for (const QString &name : m_toolOrder) {
                    auto it = m_tools.find(name.toStdString());
                    if (it != m_tools.end())
                        toolList.append(it->second->toSchema());
                    else if (m_legacyTools.contains(name))
                        toolList.append(m_legacyTools[name].toSchema());
                }
                sendResponse({
                    {"jsonrpc", "2.0"}, {"id", id},
                    {"result", QJsonObject{{"tools", toolList}}}
                });
                continue;
            }

            if (method == "tools/call") {
                QJsonObject params = msg.value("params").toObject();
                QString toolName   = params.value("name").toString();
                QJsonObject args   = params.value("arguments").toObject();

                auto it = m_tools.find(toolName.toStdString());
                if (it != m_tools.end()) {
                    sendResult(id, it->second->execute(args));
                } else if (m_legacyTools.contains(toolName)) {
                    sendResult(id, m_legacyTools[toolName].execute(args));
                } else {
                    sendResult(id, ToolResult::err(
                        QString("Error: unknown tool '%1'").arg(toolName)));
                }
                continue;
            }

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

private:
    QString  m_name;
    QString  m_version;

    // unordered_map: move-only Values (unique_ptr) sind erlaubt.
    // std::string als Key: std::hash<std::string> ist eingebaut.
    std::unordered_map<std::string, std::unique_ptr<ToolBase>> m_tools;

    // QHash für kopierbare Tool-Structs (Lambda-basierte Server).
    QHash<QString, Tool> m_legacyTools;

    // Reihenfolge für tools/list.
    QVector<QString> m_toolOrder;

    static void sendResponse(const QJsonObject &msg)
    {
        QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
        std::cout << line.toStdString();
        std::cout.flush();
    }

    static void sendResult(int id, const ToolResult &result)
    {
        sendResponse({
            {"jsonrpc", "2.0"}, {"id", id},
            {"result", QJsonObject{
                {"content", QJsonArray{
                    QJsonObject{{"type", "text"}, {"text", result.text}}
                }},
                {"isError", result.isError}
            }}
        });
    }
};
