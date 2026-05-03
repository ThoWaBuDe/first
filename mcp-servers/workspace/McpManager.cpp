#include "McpManager.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>
#include <QFileInfo>

const int McpManager::BACKOFF_DELAYS_MS[MAX_RESTART_ATTEMPTS] = {
    1000, 5000, 30000,
};

McpManager::McpManager(QObject *parent) : QObject(parent) {}

void McpManager::addServer(const QString &binary,
                            const QStringList &args,
                            const QString &incusContainer)
{
    ServerEntry entry;
    entry.binary         = binary;
    entry.args           = args;
    entry.incusContainer = incusContainer;
    m_servers.append(entry);
}

void McpManager::setIncusContainer(const QString &container)
{
    for (ServerEntry &entry : m_servers)
        entry.incusContainer = container;
}

void McpManager::setIncusContainerForRange(int firstIdx, int lastIdx,
                                            const QString &container,
                                            const QString &binDir)
{
    for (int i = firstIdx; i <= lastIdx && i < m_servers.size(); ++i) {
        ServerEntry &entry = m_servers[i];
        entry.incusContainer = container;
        // Binary-Pfad auf Container-Pfad umbiegen wenn binDir gesetzt
        if (!binDir.isEmpty() && !container.isEmpty()) {
            // Dateiname aus aktuellem Binary-Pfad extrahieren
            // z.B. ".../llamaqt-filesystem" → "llamaqt-filesystem"
            QString baseName = QFileInfo(entry.binary).fileName();
            entry.binary = binDir + "/" + baseName;
        }
    }
}

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

void McpManager::startServer(int idx,
                              std::function<void(bool, QString)> onReady)
{
    ServerEntry &entry = m_servers[idx];

    if (entry.client) {
        for (auto it = m_toolIndex.begin(); it != m_toolIndex.end(); ) {
            if (it.value() == idx) it = m_toolIndex.erase(it);
            else ++it;
        }
        entry.client->deleteLater();
        entry.client = nullptr;
        entry.ready  = false;
    }

    entry.client = new McpClient(this);

    connect(entry.client, &McpClient::serverDied,
            this, [this, idx](const QString &name) {
        emit serverDied(name);
        m_servers[idx].ready = false;
        scheduleRestart(idx);
    });

    // ── Transport-Entscheidung ────────────────────────────────────────────
    // incusContainer leer → lokaler Prozess (bisheriges Verhalten)
    // incusContainer gesetzt → incus exec <container> -- <binary> [args...]
    //
    // McpClient selbst startet nur einen QProcess — er weiß nicht ob er
    // mit einem lokalen oder Container-Prozess spricht. Das Protokoll
    // (stdio JSON-RPC 2.0) ist in beiden Fällen identisch.

    QString     effectiveBinary;
    QStringList effectiveArgs;

    if (entry.incusContainer.isEmpty()) {
        effectiveBinary = entry.binary;
        effectiveArgs   = entry.args;
    } else {
        // "incus exec <container> -- runuser -u llamaqt -- <binary> [args...]"
        // runuser akzeptiert Usernamen (im Gegensatz zu incus --user das UIDs braucht)
        effectiveBinary = "incus";
        effectiveArgs   = {"exec", entry.incusContainer,
                           "--env", "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                           "--", "runuser", "-u", "llamaqt", "--",
                           entry.binary};
        effectiveArgs  += entry.args;
    }

    entry.client->start(effectiveBinary, effectiveArgs,
        [this, idx, onReady](bool ok, QString err) {
            m_servers[idx].ready = ok;
            m_servers[idx].error = err;
            if (ok) {
                rebuildToolIndex(idx);
                m_servers[idx].restartCount = 0;
            }
            onReady(ok, err);
        });
}

void McpManager::rebuildToolIndex(int idx)
{
    if (!m_servers[idx].client) return;
    for (const QJsonValue &toolVal : m_servers[idx].client->availableTools()) {
        QString name = toolVal.toObject().value("name").toString();
        m_toolIndex[name] = idx;
    }
}

void McpManager::scheduleRestart(int serverIdx)
{
    ServerEntry &entry = m_servers[serverIdx];

    if (entry.restartCount >= MAX_RESTART_ATTEMPTS) {
        QString name = entry.client
                       ? entry.client->serverName()
                       : QFileInfo(entry.binary).baseName();
        emit serverGaveUp(name);
        return;
    }

    int delayMs = BACKOFF_DELAYS_MS[entry.restartCount];
    int attempt = entry.restartCount + 1;

    QString name = entry.client
                   ? entry.client->serverName()
                   : QFileInfo(entry.binary).baseName();

    emit serverRestarting(name, attempt, delayMs / 1000);
    ++entry.restartCount;

    if (entry.restartTimer) {
        entry.restartTimer->stop();
        entry.restartTimer->deleteLater();
        entry.restartTimer = nullptr;
    }

    entry.restartTimer = new QTimer(this);
    entry.restartTimer->setSingleShot(true);
    entry.restartTimer->setInterval(delayMs);

    connect(entry.restartTimer, &QTimer::timeout,
            this, [this, serverIdx]() { onRestartTimer(serverIdx); });

    entry.restartTimer->start();
}

void McpManager::onRestartTimer(int serverIdx)
{
    ServerEntry &entry = m_servers[serverIdx];
    if (entry.restartTimer) {
        entry.restartTimer->deleteLater();
        entry.restartTimer = nullptr;
    }

    startServer(serverIdx, [this, serverIdx](bool ok, QString) {
        ServerEntry &e = m_servers[serverIdx];
        QString name = e.client
                       ? e.client->serverName()
                       : QFileInfo(e.binary).baseName();
        if (ok)
            emit serverRestored(name);
        else
            scheduleRestart(serverIdx);
    });
}

bool McpManager::containsTool(const QString &name) const
{
    return m_toolIndex.contains(name);
}

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
        callback({}, QString("Server für Tool '%1' läuft nicht.").arg(name));
        return;
    }

    client->callTool(name, arguments, callback);
}

QString McpManager::buildToolsSystemPrompt(ToolCallFormat::Preset fmt) const
{
    QString prompt = toolCallHeader(fmt);
    prompt += "\n";

    int toolNum = 1;
    for (const ServerEntry &entry : m_servers) {
        if (!entry.ready || !entry.client) continue;
        for (const QJsonValue &toolVal : entry.client->availableTools()) {
            QJsonObject tool   = toolVal.toObject();
            QString name       = tool.value("name").toString();
            QString desc       = tool.value("description").toString();
            QJsonObject schema = tool.value("inputSchema").toObject();
            prompt += QString("%1. %2\n").arg(toolNum++).arg(name);
            prompt += schemaToPrompt(name, desc, schema);
            prompt += "\n";
        }
    }
    prompt += "\nAntworte auf Deutsch.";
    return prompt;
}

QString McpManager::toolCallHeader(ToolCallFormat::Preset fmt)
{
    return ToolCallFormat::systemPromptHeader(fmt);
}

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
