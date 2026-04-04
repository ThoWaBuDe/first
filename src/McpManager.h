#pragma once
#include <QObject>
#include <QVector>
#include <QHash>
#include <functional>
#include "McpClient.h"

// ─── McpManager ───────────────────────────────────────────────────────────────
// Verwaltet mehrere MCP-Server und präsentiert ihre Tools als einheitliche
// Schnittstelle nach oben (zu MainWindow) und unten (zu den McpClients).
//
// Verantwortlichkeiten:
//   1. Server starten und initialisieren
//   2. Tool-Namen → richtiger Server mappen (Routing)
//   3. Aggregierten System-Prompt aus allen Tool-Beschreibungen bauen
//   4. callTool() an den zuständigen Server weiterleiten
//
// Pattern: Facade — versteckt die Komplexität mehrerer McpClients hinter
//          einer einfachen Schnittstelle (containsTool / callTool).
class McpManager : public QObject {
    Q_OBJECT

public:
    using ToolCallback = McpClient::ToolCallback;

    explicit McpManager(QObject *parent = nullptr);

    // Server hinzufügen und starten.
    // onAllReady wird aufgerufen wenn ALLE Server initialisiert sind.
    void addServer(const QString &binary,
                   const QStringList &args = {});

    void startAll(std::function<void(bool ok, QStringList errors)> onAllReady);

    // Prüft ob ein Tool-Name bekannt ist (von einem der Server advertised)
    bool containsTool(const QString &name) const;

    // Tool aufrufen — automatisches Routing zum richtigen Server
    void callTool(const QString &name,
                  const QJsonObject &arguments,
                  ToolCallback callback);

    // System-Prompt mit allen Tool-Beschreibungen aller Server
    QString buildToolsSystemPrompt() const;

    // Tool-Call Format Beschreibung (für den System-Prompt Kopf)
    static QString toolCallHeader();

    // Debug: gibt Server + Tool-Namen strukturiert zurueck
    struct ServerToolInfo {
        QString         serverName;
        QStringList     toolNames;
    };
    QVector<ServerToolInfo> debugToolInfo() const;

signals:
    void serverDied(const QString &serverName);

private:
    struct ServerEntry {
        QString    binary;
        QStringList args;
        McpClient *client = nullptr;
        bool       ready  = false;
        QString    error;
    };

    QVector<ServerEntry>    m_servers;

    // Tool-Name → Index in m_servers (für schnelles Routing)
    QHash<QString, int>     m_toolIndex;

    // Hilfsfunktion: JSON Schema → lesbarer System-Prompt Text
    static QString schemaToPrompt(const QString &toolName,
                                  const QString &description,
                                  const QJsonObject &inputSchema);
};
