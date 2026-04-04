#pragma once
#include <QObject>
#include <QProcess>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <functional>

// ─── McpClient ────────────────────────────────────────────────────────────────
// Verbindet sich mit einem MCP-Server via stdio (JSON-RPC 2.0).
//
// Protokoll-Ablauf:
//   1. start()      → QProcess startet Server-Binary
//   2. initialize() → MCP-Handshake (protocolVersion aushandeln)
//   3. listTools()  → Server advertised seine Tools (Name + Schema)
//   4. callTool()   → Tool aufrufen, Ergebnis async per Callback
//
// Transport: newline-delimited JSON über stdin/stdout des Server-Prozesses.
// Jede Nachricht = eine JSON-Zeile. Kein HTTP, kein Port, kein Daemon.
//
// Pattern: Active Object — eigene Event-Loop-Integration via QProcess Signals.
//          Alle Callbacks laufen im GUI-Thread (Qt QueuedConnection).
//
// JSON-RPC 2.0 Grundstruktur:
//   Request:      {"jsonrpc":"2.0", "id":N, "method":"...", "params":{...}}
//   Response:     {"jsonrpc":"2.0", "id":N, "result":{...}}
//   Notification: {"jsonrpc":"2.0", "method":"...", "params":{...}}  // kein id
class McpClient : public QObject {
    Q_OBJECT

public:
    using ToolCallback   = std::function<void(QString result, QString error)>;
    using ToolsCallback  = std::function<void(QJsonArray tools)>;

    explicit McpClient(QObject *parent = nullptr);
    ~McpClient() override;

    // Server-Binary starten und MCP-Handshake durchführen.
    // onReady wird aufgerufen wenn initialize + listTools abgeschlossen.
    void start(const QString &serverBinary,
               const QStringList &args,
               std::function<void(bool ok, QString error)> onReady);

    // Alle Tools dieses Servers (nach erfolgreichem start())
    QJsonArray availableTools() const { return m_tools; }

    // Tool aufrufen — async, Ergebnis kommt per Callback.
    // error ist leer wenn erfolgreich.
    void callTool(const QString &name,
                  const QJsonObject &arguments,
                  ToolCallback callback);

    bool isRunning() const;
    QString serverName() const { return m_serverName; }

signals:
    void serverDied(const QString &serverName);

private slots:
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

private:
    void sendRequest(const QJsonObject &msg);
    void handleResponse(const QJsonObject &msg);
    void handleNotification(const QJsonObject &msg);

    // Initialisierungs-Sequenz (drei Schritte):
    void doInitialize();
    void doListTools();

    QProcess  m_process;
    QByteArray m_readBuffer;  // Puffer für unvollständige Zeilen

    // Pending Requests: id → callback
    // Pattern: Future/Promise ohne std::future — Qt-idiomatisch mit Lambda
    QHash<int, ToolCallback> m_pending;
    int m_nextId = 1;

    // Initialisierungs-State
    enum class State { Idle, Initializing, ListingTools, Ready };
    State  m_state = State::Idle;
    std::function<void(bool, QString)> m_onReady;

    QJsonArray m_tools;
    QString    m_serverName;
};
