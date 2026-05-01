#pragma once
#include <QObject>
#include <QVector>
#include <QHash>
#include <QTimer>
#include <functional>
#include "MCP/McpClient.h"
#include "Chat/ToolCallFormat.h"

// ─── McpManager ───────────────────────────────────────────────────────────────
// NEU: buildToolsSystemPrompt() und toolCallHeader() bekommen
// ToolCallFormat::Preset als Parameter.
// Dadurch gibt McpManager automatisch das richtige Format-Beispiel aus.

class McpManager : public QObject {
    Q_OBJECT

public:
    using ToolCallback = McpClient::ToolCallback;

    explicit McpManager(QObject *parent = nullptr);

    void addServer(const QString &binary, const QStringList &args = {});
    void startAll(std::function<void(bool ok, QStringList errors)> onAllReady);

    bool containsTool(const QString &name) const;

    void callTool(const QString &name,
                  const QJsonObject &arguments,
                  ToolCallback callback);

    // NEU: Format als Parameter — Agent übergibt m_activeToolFormat
    QString buildToolsSystemPrompt(
        ToolCallFormat::Preset fmt = ToolCallFormat::Preset::QwenXmlTags) const;

    // NEU: delegiert an ToolCallFormat::systemPromptHeader()
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
