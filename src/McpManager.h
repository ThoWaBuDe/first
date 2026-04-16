#pragma once
// ─── McpManager ───────────────────────────────────────────────────────────────
// Verwaltet mehrere MCP-Server.
//
// NEU (Punkt S): Auto-Restart mit Exponential Backoff
//   Wenn ein Server crasht → automatischer Neustart nach Wartezeit.
//   Backoff-Stufen: 1s → 5s → 30s → aufgeben (3 Versuche).
//
//   Warum Backoff statt sofortiger Neustart?
//   Wenn der Server wegen einer kaputten Datei oder fehlender Abhängigkeit
//   crasht, würde ein sofortiger Neustart in einer Crash-Loop enden.
//   Backoff gibt dem System Zeit sich zu stabilisieren — und dem User Zeit
//   das Problem zu bemerken.
//
//   Analogie AVR: Watchdog-Timer mit Progressive Timeout —
//   kurze Resets bei transienten Fehlern, langer Timeout bei hartem Fehler.

#include <QObject>
#include <QVector>
#include <QHash>
#include <QTimer>
#include <functional>
#include "McpClient.h"

class McpManager : public QObject {
    Q_OBJECT

public:
    using ToolCallback = McpClient::ToolCallback;

    explicit McpManager(QObject *parent = nullptr);

    void addServer(const QString &binary, const QStringList &args = {});

    // Alle Server starten. onAllReady wenn letzter fertig (oder fehlgeschlagen).
    void startAll(std::function<void(bool ok, QStringList errors)> onAllReady);

    bool containsTool(const QString &name) const;

    void callTool(const QString &name,
                  const QJsonObject &arguments,
                  ToolCallback callback);

    QString buildToolsSystemPrompt() const;
    static QString toolCallHeader();

    struct ServerToolInfo {
        QString     serverName;
        QStringList toolNames;
    };
    QVector<ServerToolInfo> debugToolInfo() const;

signals:
    void serverDied(const QString &serverName);
    // NEU: Informiert UI über Restart-Versuche
    void serverRestarting(const QString &serverName, int attemptNr, int delaySeconds);
    void serverRestored(const QString &serverName);
    void serverGaveUp(const QString &serverName);

private slots:
    // NEU: Wird von QTimer nach Backoff-Wartezeit aufgerufen
    void onRestartTimer(int serverIdx);

private:
    // ── Backoff-Konfiguration ─────────────────────────────────────────────
    // 3 Versuche: 1s, 5s, 30s — dann aufgeben
    static constexpr int MAX_RESTART_ATTEMPTS = 3;
    static const int BACKOFF_DELAYS_MS[MAX_RESTART_ATTEMPTS]; // {1000, 5000, 30000}

    struct ServerEntry {
        QString     binary;
        QStringList args;
        McpClient  *client       = nullptr;
        bool        ready        = false;
        QString     error;

        // Restart-Zustand
        int         restartCount = 0;   // bisherige Restart-Versuche
        QTimer     *restartTimer = nullptr; // Backoff-Timer (owned by McpManager)
    };

    QVector<ServerEntry>    m_servers;
    QHash<QString, int>     m_toolIndex; // Tool-Name → Server-Index

    // ── Server (neu) starten ──────────────────────────────────────────────
    // Wird beim ersten Start UND bei Restarts aufgerufen.
    // onReady: Callback wenn Start (erfolgreich oder fehlgeschlagen) abgeschlossen.
    void startServer(int idx,
                     std::function<void(bool ok, QString error)> onReady);

    // Tool-Index für einen Server neu aufbauen (nach Restart)
    void rebuildToolIndex(int idx);

    // NEU: Restart mit Backoff einleiten
    void scheduleRestart(int serverIdx);

    static QString schemaToPrompt(const QString &toolName,
                                  const QString &description,
                                  const QJsonObject &inputSchema);
};
