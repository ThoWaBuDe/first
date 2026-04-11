#pragma once
// ─── McpServer ────────────────────────────────────────────────────────────────
// Gemeinsamer JSON-RPC 2.0 Loop und Tool-Registry für alle LlamaQt MCP-Server.
//
// Pattern: Template Method + Command + Registry
//
//   Template Method: run() definiert den Ablauf (lesen → dispatchen → schreiben).
//                    Der konkrete Server füllt nur die Tools.
//
//   Command:         Jedes Tool ist ein Tool-Struct mit name/description/
//                    inputSchema/execute. execute() ist ein std::function —
//                    damit kann man Lambdas, freie Funktionen oder Member-
//                    Funktionen anbinden.
//
//   Registry:        registerTool() trägt Tools in eine QHash ein.
//                    Der Dispatch in run() ist dann O(1).
//
// Verwendung:
//
//   McpServer server("llamaqt-filesystem", "2.3");
//
//   server.registerTool({
//       "read_file",
//       "Liest eine Datei...",
//       QJsonObject{ {"path", ...} },          // inputSchema
//       {"path"},                               // required fields
//       [&](const QJsonObject &args) -> ToolResult {
//           // ... Implementierung
//           return ToolResult::ok("Inhalt...");
//       }
//   });
//
//   server.run();   // blockiert bis stdin geschlossen wird

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QVector>
#include <QTextStream>
#include <functional>
#include <iostream>

// ─── ToolResult ───────────────────────────────────────────────────────────────
// Rückgabetyp für Tool-Implementierungen.
// Trennt "was ist das Ergebnis" von "wie wird es serialisiert".
struct ToolResult {
    QString text;
    bool    isError = false;

    // Factory-Methoden — lesbarer als Konstruktor mit bool-Parameter
    static ToolResult ok(const QString &text)    { return {text, false}; }
    static ToolResult err(const QString &text)   { return {text, true};  }
};

// ─── Tool ─────────────────────────────────────────────────────────────────────
// Command-Pattern: ein Tool = ein Wert-Objekt.
// execute bekommt die JSON-Argumente und gibt ein ToolResult zurück.
struct Tool {
    QString     name;
    QString     description;
    QJsonObject properties;   // JSON Schema properties
    QJsonArray  required;     // required field names

    std::function<ToolResult(const QJsonObject &args)> execute;

    // JSON Schema für tools/list Antwort
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

    // Tool registrieren. Reihenfolge bestimmt die Reihenfolge in tools/list.
    void registerTool(Tool tool)
    {
        m_toolOrder.append(tool.name);
        m_tools.insert(tool.name, std::move(tool));
    }

    // Hauptschleife: liest JSON-RPC von stdin, schreibt Antworten auf stdout.
    // Blockiert bis stdin EOF.
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
                for (const QString &name : m_toolOrder)
                    toolList.append(m_tools[name].toSchema());

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

                if (!m_tools.contains(toolName)) {
                    sendResult(id, ToolResult::err(
                        QString("Error: unknown tool '%1'").arg(toolName)));
                    continue;
                }

                // Tool ausführen — execute() ist das Lambda/die Funktion
                // die beim registerTool() übergeben wurde.
                ToolResult result = m_tools[toolName].execute(args);
                sendResult(id, result);
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
    QString              m_name;
    QString              m_version;
    QHash<QString, Tool> m_tools;
    QVector<QString>     m_toolOrder;  // Reihenfolge für tools/list

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
