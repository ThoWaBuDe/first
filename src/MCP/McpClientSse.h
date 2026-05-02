#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <functional>

// ─── McpClientSse ─────────────────────────────────────────────────────────────
// MCP-Client mit HTTP+SSE Transport (Model Context Protocol Spec, §5.2).
//
// Protokoll-Ablauf:
//   1. GET  /sse           → SSE-Stream öffnen (bleibt offen!)
//      Server schickt sofort: "event: endpoint\ndata: /message\n\n"
//   2. POST /message       → JSON-RPC Requests (initialize, tools/list, tools/call)
//      Server antwortet nicht per HTTP 200 body, sondern per SSE-Event!
//   3. SSE-Events parsen   → "event: message\ndata: {...JSON...}\n\n"
//
// Warum SSE statt normales HTTP?
//   MCP ist bidirektional: Server kann auch unaufgefordert Notifications
//   schicken (z.B. tool-list-changed). SSE erlaubt Server→Client push
//   ohne Polling. Client→Server läuft über POST.
//
// Unterschied zu McpClient (stdio):
//   McpClient:    JSON-Zeilen über stdin/stdout eines Prozesses
//   McpClientSse: JSON-RPC über HTTP POST, Antworten über SSE-Stream
//
// Analogie (AVR):
//   McpClient    = UART synchron (blockierend lesen/schreiben)
//   McpClientSse = UART mit RX-Interrupt (SSE) + TX-Buffer (POST)
//
// Pattern: Active Object — alle Callbacks im Qt-Event-Loop-Thread.
//          Pending-Map (id → callback) wie bei McpClient.

class McpClientSse : public QObject {
    Q_OBJECT

public:
    using ToolCallback  = std::function<void(QString result, QString error)>;
    using ToolsCallback = std::function<void(QJsonArray tools)>;

    explicit McpClientSse(QObject *parent = nullptr);
    ~McpClientSse() override;

    // Verbindung aufbauen.
    // url = Basis-URL des Proxy, z.B. "http://localhost:9000"
    // onReady wird aufgerufen wenn initialize + listTools abgeschlossen.
    void start(const QString &url,
               std::function<void(bool ok, QString error)> onReady);

    void callTool(const QString &name,
                  const QJsonObject &arguments,
                  ToolCallback callback);

    QJsonArray availableTools() const { return m_tools; }
    bool isConnected() const { return m_sseReply != nullptr; }
    QString serverUrl() const { return m_baseUrl; }

signals:
    void disconnected();

private slots:
    void onSseData();
    void onSseError();

private:
    // SSE-Stream öffnen
    void openSseStream();

    // JSON-RPC Request per POST senden
    void postRequest(const QJsonObject &msg);

    // Eingehende SSE-Daten verarbeiten
    void processSseEvent(const QByteArray &eventType,
                         const QByteArray &data);

    // Antwort auf pending Request dispatchen
    void dispatchResponse(const QJsonObject &msg);

    // Initialisierungssequenz (wie McpClient)
    void doInitialize();
    void doListTools();

    QString                 m_baseUrl;
    QString                 m_postEndpoint;  // aus SSE endpoint-Event

    QNetworkAccessManager  *m_nam      = nullptr;
    QNetworkReply          *m_sseReply = nullptr;  // offener SSE-Stream
    QByteArray              m_sseBuffer;            // unverarbeitete Bytes

    // Pending Requests: id → callback
    QHash<int, ToolCallback>           m_pending;
    int                                m_nextId = 1;

    enum class State { Idle, WaitingForEndpoint, Initializing,
                       ListingTools, Ready };
    State m_state = State::Idle;

    std::function<void(bool, QString)> m_onReady;
    QJsonArray                         m_tools;
};
