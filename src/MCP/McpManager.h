#pragma once
#include <QObject>
#include <QVector>
#include <QHash>
#include <QTimer>
#include <functional>
#include "MCP/McpClient.h"
#include "Chat/ToolCallFormat.h"

// ─── McpManager ───────────────────────────────────────────────────────────────
// Facade über mehrere McpClients mit Auto-Restart (Backoff).
//
// Incus-Erweiterung:
//   addServer() hat optionalen incusContainer-Parameter.
//   Wenn gesetzt: QProcess startet "incus exec <container> -- <binary>"
//   statt das Binary direkt. McpClient merkt nichts davon.
//
//   setIncusContainer() setzt den Container für alle registrierten Server
//   auf einmal — wird von Agent::onIncusContainerChanged() aufgerufen.

class McpManager : public QObject {
    Q_OBJECT

public:
    using ToolCallback = McpClient::ToolCallback;

    explicit McpManager(QObject *parent = nullptr);
    ~McpManager() override;

    // incusContainer leer    → lokaler Prozess (bisheriges Verhalten)
    // incusContainer gesetzt → incus exec <container> -- <binary> <args>
    void addServer(const QString &binary,
                   const QStringList &args = {},
                   const QString &incusContainer = {});

    void startAll(std::function<void(bool ok, QStringList errors)> onAllReady);

    // Setzt den incusContainer für alle registrierten Server auf einmal.
    // Beim nächsten startAll()/startServer() wird dieser Container verwendet.
    // Leerer String = zurück zu lokalen Prozessen.
    void setIncusContainer(const QString &container);

    // Setzt Container + Binary-Prefix nur für einen Bereich der Server-Liste.
    // firstIdx/lastIdx inklusiv (0-basiert).
    // newBinary wird nur gesetzt wenn nicht leer — so bleibt der lokale
    // Pfad für tree-sitter/clang unverändert.
    // Wird von Agent::onIncusContainerChanged() aufgerufen.
    void setIncusContainerForRange(int firstIdx, int lastIdx,
                                   const QString &container,
                                   const QString &binDir);

    bool containsTool(const QString &name) const;

    void callTool(const QString &name,
                  const QJsonObject &arguments,
                  ToolCallback callback);

    QString buildToolsSystemPrompt(
        ToolCallFormat::Preset fmt = ToolCallFormat::Preset::QwenXmlTags) const;

    static QString toolCallHeader(
        ToolCallFormat::Preset fmt = ToolCallFormat::Preset::QwenXmlTags);

    struct ServerToolInfo {
        QString     serverName;
        QStringList toolNames;
    };
    QVector<ServerToolInfo> debugToolInfo() const;

signals:
    void serverDied(const QString &serverName);
    void serverRestarting(const QString &serverName, int attemptNr, int delaySeconds);
    void serverRestored(const QString &serverName);
    void serverGaveUp(const QString &serverName);

private slots:
    void onRestartTimer(int serverIdx);

private:
    static constexpr int MAX_RESTART_ATTEMPTS = 3;
    static const int BACKOFF_DELAYS_MS[MAX_RESTART_ATTEMPTS];

    struct ServerEntry {
        QString     binary;
        QStringList args;
        QString     incusContainer;  // leer = lokal, gesetzt = via incus exec
        McpClient  *client       = nullptr;
        bool        ready        = false;
        QString     error;
        int         restartCount = 0;
        QTimer     *restartTimer = nullptr;
    };

    QVector<ServerEntry>    m_servers;
    QHash<QString, int>     m_toolIndex;

    void startServer(int idx,
                     std::function<void(bool ok, QString error)> onReady);
    void rebuildToolIndex(int idx);
    void scheduleRestart(int serverIdx);

    static QString schemaToPrompt(const QString &toolName,
                                  const QString &description,
                                  const QJsonObject &inputSchema);
};
