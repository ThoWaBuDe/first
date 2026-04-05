#include "McpManager.h"
#include <QJsonArray>
#include <QJsonDocument>

McpManager::McpManager(QObject *parent)
    : QObject(parent)
{}

// ─── addServer ───────────────────────────────────────────────────────────────
void McpManager::addServer(const QString &binary, const QStringList &args)
{
    ServerEntry entry;
    entry.binary = binary;
    entry.args   = args;
    m_servers.append(entry);
}

// ─── startAll ────────────────────────────────────────────────────────────────
// Startet alle Server parallel. onAllReady wird aufgerufen wenn der letzte
// fertig ist (oder fehlgeschlagen ist).
// Pattern: Scatter-Gather — alle gleichzeitig starten, auf alle warten.
void McpManager::startAll(std::function<void(bool, QStringList)> onAllReady)
{
    if (m_servers.isEmpty()) {
        onAllReady(true, {});
        return;
    }

    auto remaining = std::make_shared<int>(m_servers.size());
    auto errors    = std::make_shared<QStringList>();

    for (int i = 0; i < m_servers.size(); ++i) {
        ServerEntry &entry = m_servers[i];

        entry.client = new McpClient(this);

        connect(entry.client, &McpClient::serverDied,
                this,         &McpManager::serverDied);

        entry.client->start(entry.binary, entry.args,
            [this, i, remaining, errors, onAllReady](bool ok, QString err) {

                m_servers[i].ready = ok;
                m_servers[i].error = err;

                if (ok) {
                    for (const QJsonValue &toolVal : m_servers[i].client->availableTools()) {
                        QString name = toolVal.toObject().value("name").toString();
                        m_toolIndex[name] = i;
                    }
                } else {
                    errors->append(QString("%1: %2").arg(m_servers[i].binary, err));
                }

                --(*remaining);
                if (*remaining == 0) {
                    bool allOk = errors->isEmpty();
                    onAllReady(allOk, *errors);
                }
            });
    }
}

// ─── containsTool ────────────────────────────────────────────────────────────
bool McpManager::containsTool(const QString &name) const
{
    return m_toolIndex.contains(name);
}

// ─── callTool ────────────────────────────────────────────────────────────────
void McpManager::callTool(const QString &name,
                           const QJsonObject &arguments,
                           ToolCallback callback)
{
    auto it = m_toolIndex.find(name);
    if (it == m_toolIndex.end()) {
        callback({}, QString("Unbekanntes Tool: '%1'").arg(name));
        return;
    }

    int serverIdx = it.value();
    McpClient *client = m_servers[serverIdx].client;

    if (!client || !client->isRunning()) {
        callback({}, QString("Server fuer Tool '%1' laeuft nicht.").arg(name));
        return;
    }

    client->callTool(name, arguments, callback);
}

// ─── buildToolsSystemPrompt ──────────────────────────────────────────────────
// Baut den System-Prompt aus den advertisierten Tools aller Server.
QString McpManager::buildToolsSystemPrompt() const
{
    QString prompt = toolCallHeader();
    prompt += "\n";

    int toolNum = 1;
    for (const ServerEntry &entry : m_servers) {
        if (!entry.ready || !entry.client) continue;

        for (const QJsonValue &toolVal : entry.client->availableTools()) {
            QJsonObject tool = toolVal.toObject();
            QString name     = tool.value("name").toString();
            QString desc     = tool.value("description").toString();
            QJsonObject schema = tool.value("inputSchema").toObject();

            prompt += QString("%1. %2\n").arg(toolNum++).arg(name);
            prompt += schemaToPrompt(name, desc, schema);
            prompt += "\n";
        }
    }

    prompt += "\nAntworte auf Deutsch.";
    return prompt;
}

// ─── toolCallHeader ──────────────────────────────────────────────────────────
// Offizielles Qwen3 Hermes-Format fuer Tool-Calls.
//
// Qwen3 wurde auf genau diesen Wortlaut trainiert — je naeher der System-Prompt
// am Trainingsformat liegt, desto zuverlaessiger haelt sich das Modell daran.
//
// Quelle: https://qwen.readthedocs.io/en/latest/framework/function_call.html
// Format:
//   <tool_call>
//   {"name": <function-name>, "arguments": <args-json-object>}
//   </tool_call>
QString McpManager::toolCallHeader()
{
    return
        "You may call one or more functions to assist with the user query.\n\n"
        "For each function call, return a json object with function name and "
        "arguments within <tool_call></tool_call> XML tags:\n"
        "<tool_call>\n"
        "{\"name\": <function-name>, \"arguments\": <args-json-object>}\n"
        "</tool_call>\n\n"
        "Wait for the result before making the next tool call.\n"
        "Available tools:\n";
}

// ─── schemaToPrompt ──────────────────────────────────────────────────────────
// Konvertiert ein JSON Schema in lesbaren Text fuer den System-Prompt.
QString McpManager::schemaToPrompt(const QString &toolName,
                                    const QString &description,
                                    const QJsonObject &inputSchema)
{
    Q_UNUSED(toolName)
    QString result;

    if (!description.isEmpty())
        result += QString("   Beschreibung: %1\n").arg(description);

    QJsonObject props   = inputSchema.value("properties").toObject();
    QJsonArray  required = inputSchema.value("required").toArray();

    if (!props.isEmpty()) {
        result += "   Argumente:\n";
        for (auto it = props.begin(); it != props.end(); ++it) {
            QString propName = it.key();
            QJsonObject prop = it.value().toObject();
            QString type     = prop.value("type").toString("string");
            QString propDesc = prop.value("description").toString();

            bool isRequired = false;
            for (const QJsonValue &r : required)
                if (r.toString() == propName) { isRequired = true; break; }

            result += QString("     - %1 (%2%3)%4\n")
                      .arg(propName)
                      .arg(type)
                      .arg(isRequired ? ", required" : ", optional")
                      .arg(propDesc.isEmpty() ? "" : ": " + propDesc);
        }
    }

    return result;
}

// ─── debugToolInfo ───────────────────────────────────────────────────────────
QVector<McpManager::ServerToolInfo> McpManager::debugToolInfo() const
{
    QVector<ServerToolInfo> result;
    for (const ServerEntry &entry : m_servers) {
        if (!entry.client) continue;
        ServerToolInfo info;
        info.serverName = entry.client->serverName();
        for (const QJsonValue &tv : entry.client->availableTools())
            info.toolNames << tv.toObject().value("name").toString();
        result.append(info);
    }
    return result;
}
