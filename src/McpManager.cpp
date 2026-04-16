#include "McpManager.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>
#include <QFileInfo>

// ─── Backoff-Tabelle ──────────────────────────────────────────────────────────
// 3 Versuche: 1s → 5s → 30s
// Analogie AVR: progressive Watchdog-Timeouts —
// erster Reset schnell (transienter Fehler?), dann immer länger.
const int McpManager::BACKOFF_DELAYS_MS[MAX_RESTART_ATTEMPTS] = {
    1000,   // Versuch 1: 1 Sekunde
    5000,   // Versuch 2: 5 Sekunden
    30000,  // Versuch 3: 30 Sekunden
};

McpManager::McpManager(QObject *parent) : QObject(parent) {}

// ─── addServer ────────────────────────────────────────────────────────────────
void McpManager::addServer(const QString &binary, const QStringList &args)
{
    ServerEntry entry;
    entry.binary = binary;
    entry.args   = args;
    m_servers.append(entry);
}

// ─── startAll ────────────────────────────────────────────────────────────────
// Scatter-Gather: alle Server parallel starten.
// onAllReady wenn letzter fertig.
void McpManager::startAll(std::function<void(bool, QStringList)> onAllReady)
{
    if (m_servers.isEmpty()) { onAllReady(true, {}); return; }

    auto remaining = std::make_shared<int>(m_servers.size());
    auto errors    = std::make_shared<QStringList>();

    for (int i = 0; i < m_servers.size(); ++i) {
        startServer(i, [this, i, remaining, errors, onAllReady](bool ok, QString err) {
            if (!ok)
                errors->append(QString("%1: %2").arg(m_servers[i].binary, err));

            --(*remaining);
            if (*remaining == 0) {
                bool allOk = errors->isEmpty();
                onAllReady(allOk, *errors);
            }
        });
    }
}

// ─── startServer ─────────────────────────────────────────────────────────────
// Startet einen einzelnen Server (Erststart oder Restart nach Crash).
// Verbindet serverDied-Signal mit scheduleRestart().
void McpManager::startServer(int idx,
                              std::function<void(bool, QString)> onReady)
{
    ServerEntry &entry = m_servers[idx];

    // Alten Client aufräumen falls Restart
    if (entry.client) {
        // Tool-Index bereinigen
        for (auto it = m_toolIndex.begin(); it != m_toolIndex.end(); ) {
            if (it.value() == idx) it = m_toolIndex.erase(it);
            else ++it;
        }
        entry.client->deleteLater();
        entry.client = nullptr;
        entry.ready  = false;
    }

    entry.client = new McpClient(this);

    // serverDied → Restart mit Backoff einleiten
    // Lambda captured idx by value — korrekt, da idx stabil ist.
    connect(entry.client, &McpClient::serverDied,
            this, [this, idx](const QString &name) {
        emit serverDied(name);
        m_servers[idx].ready = false;
        scheduleRestart(idx);
    });

    entry.client->start(entry.binary, entry.args,
        [this, idx, onReady](bool ok, QString err) {
            m_servers[idx].ready = ok;
            m_servers[idx].error = err;

            if (ok) {
                rebuildToolIndex(idx);
                // Erfolgreicher Start → Restart-Zähler zurücksetzen
                m_servers[idx].restartCount = 0;
            }

            onReady(ok, err);
        });
}

// ─── rebuildToolIndex ─────────────────────────────────────────────────────────
// Baut den Tool-Index für einen Server neu auf.
// Aufgerufen nach (Neu-)Start eines Servers.
void McpManager::rebuildToolIndex(int idx)
{
    if (!m_servers[idx].client) return;
    for (const QJsonValue &toolVal : m_servers[idx].client->availableTools()) {
        QString name = toolVal.toObject().value("name").toString();
        m_toolIndex[name] = idx;
    }
}

// ─── scheduleRestart ──────────────────────────────────────────────────────────
// Plant einen Neustart mit Backoff-Verzögerung.
//
// Ablauf:
//   1. restartCount prüfen → aufgeben wenn MAX erreicht
//   2. Delay aus BACKOFF_DELAYS_MS[restartCount] holen
//   3. QTimer mit singleShot erstellen → onRestartTimer() aufrufen
//
// QTimer::singleShot mit Lambda: kein permanenter Timer — feuert genau einmal.
// Analogie AVR: One-Shot-Timer statt Free-Running-Timer.
void McpManager::scheduleRestart(int serverIdx)
{
    ServerEntry &entry = m_servers[serverIdx];

    if (entry.restartCount >= MAX_RESTART_ATTEMPTS) {
        QString name = entry.client
                       ? entry.client->serverName()
                       : QFileInfo(entry.binary).baseName();
        emit serverGaveUp(name);
        qWarning() << "McpManager: Server" << name
                   << "nach" << MAX_RESTART_ATTEMPTS
                   << "Versuchen aufgegeben.";
        return;
    }

    int delayMs = BACKOFF_DELAYS_MS[entry.restartCount];
    int attempt = entry.restartCount + 1;

    QString name = entry.client
                   ? entry.client->serverName()
                   : QFileInfo(entry.binary).baseName();

    emit serverRestarting(name, attempt, delayMs / 1000);

    qWarning() << "McpManager: Server" << name
               << "gestorben — Neustart in" << delayMs << "ms"
               << "(Versuch" << attempt << "von" << MAX_RESTART_ATTEMPTS << ")";

    ++entry.restartCount;

    // Alten Timer aufräumen falls vorhanden
    if (entry.restartTimer) {
        entry.restartTimer->stop();
        entry.restartTimer->deleteLater();
        entry.restartTimer = nullptr;
    }

    // Neuen One-Shot-Timer erstellen
    // parent=this damit Qt beim Löschen von McpManager den Timer mitlöscht.
    entry.restartTimer = new QTimer(this);
    entry.restartTimer->setSingleShot(true);
    entry.restartTimer->setInterval(delayMs);

    // Lambda captured serverIdx by value — stabil
    connect(entry.restartTimer, &QTimer::timeout,
            this, [this, serverIdx]() {
        onRestartTimer(serverIdx);
    });

    entry.restartTimer->start();
}

// ─── onRestartTimer ──────────────────────────────────────────────────────────
// Backoff-Zeit abgelaufen → Server tatsächlich neu starten.
void McpManager::onRestartTimer(int serverIdx)
{
    ServerEntry &entry = m_servers[serverIdx];

    // Timer-Referenz bereinigen (ist schon gefeuert, aber Zeiger noch gültig)
    if (entry.restartTimer) {
        entry.restartTimer->deleteLater();
        entry.restartTimer = nullptr;
    }

    qWarning() << "McpManager: Starte Server neu:"
               << QFileInfo(entry.binary).baseName();

    startServer(serverIdx, [this, serverIdx](bool ok, QString err) {
        ServerEntry &e = m_servers[serverIdx];
        QString name = e.client
                       ? e.client->serverName()
                       : QFileInfo(e.binary).baseName();

        if (ok) {
            qWarning() << "McpManager: Server" << name << "erfolgreich neu gestartet.";
            emit serverRestored(name);
        } else {
            qWarning() << "McpManager: Server" << name
                       << "Neustart fehlgeschlagen:" << err;
            // Noch ein Versuch falls restartCount < MAX
            scheduleRestart(serverIdx);
        }
    });
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

    int        serverIdx = it.value();
    McpClient *client    = m_servers[serverIdx].client;

    if (!client || !client->isRunning()) {
        callback({}, QString("Server für Tool '%1' läuft nicht "
                             "(möglicherweise wird neu gestartet).")
                     .arg(name));
        return;
    }

    client->callTool(name, arguments, callback);
}

// ─── buildToolsSystemPrompt ───────────────────────────────────────────────────
QString McpManager::buildToolsSystemPrompt() const
{
    QString prompt = toolCallHeader();
    prompt += "\n";

    int toolNum = 1;
    for (const ServerEntry &entry : m_servers) {
        if (!entry.ready || !entry.client) continue;
        for (const QJsonValue &toolVal : entry.client->availableTools()) {
            QJsonObject tool  = toolVal.toObject();
            QString name      = tool.value("name").toString();
            QString desc      = tool.value("description").toString();
            QJsonObject schema= tool.value("inputSchema").toObject();
            prompt += QString("%1. %2\n").arg(toolNum++).arg(name);
            prompt += schemaToPrompt(name, desc, schema);
            prompt += "\n";
        }
    }
    prompt += "\nAntworte auf Deutsch.";
    return prompt;
}

// ─── toolCallHeader ──────────────────────────────────────────────────────────
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
QString McpManager::schemaToPrompt(const QString &toolName,
                                    const QString &description,
                                    const QJsonObject &inputSchema)
{
    Q_UNUSED(toolName)
    QString result;
    if (!description.isEmpty())
        result += QString("   Beschreibung: %1\n").arg(description);

    QJsonObject props    = inputSchema.value("properties").toObject();
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

// ─── debugToolInfo ────────────────────────────────────────────────────────────
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
