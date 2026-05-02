#include "MCP/McpClientSse.h"

#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonParseError>

// ─── Konstruktor / Destruktor ─────────────────────────────────────────────────

McpClientSse::McpClientSse(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{}

McpClientSse::~McpClientSse()
{
    if (m_sseReply) {
        m_sseReply->abort();
        m_sseReply->deleteLater();
    }
}

// ─── Verbindung aufbauen ──────────────────────────────────────────────────────

void McpClientSse::start(const QString &url,
                          std::function<void(bool, QString)> onReady)
{
    m_baseUrl  = url;
    m_onReady  = onReady;
    m_state    = State::WaitingForEndpoint;
    openSseStream();
}

// ─── SSE-Stream öffnen ────────────────────────────────────────────────────────
// GET /sse → bleibt offen, Server schickt Events als Text-Stream.
//
// HTTP SSE Format:
//   event: endpoint\n
//   data: /message\n
//   \n
//   event: message\n
//   data: {"jsonrpc":"2.0",...}\n
//   \n
//
// Der Stream bleibt offen bis die Verbindung abbricht.
// readyRead() kommt bei jedem eingehenden Datenchunk.

void McpClientSse::openSseStream()
{
    QNetworkRequest req{QUrl(m_baseUrl + "/sse")};
    req.setRawHeader("Accept", "text/event-stream");
    req.setRawHeader("Cache-Control", "no-cache");

    m_sseReply = m_nam->get(req);

    connect(m_sseReply, &QNetworkReply::readyRead,
            this, &McpClientSse::onSseData);
    connect(m_sseReply, &QNetworkReply::errorOccurred,
            this, [this](QNetworkReply::NetworkError) { onSseError(); });
    connect(m_sseReply, &QNetworkReply::finished,
            this, &McpClientSse::onSseError);
}

// ─── Eingehende SSE-Daten ─────────────────────────────────────────────────────
// Puffer füllen, nach vollständigen Events suchen.
// Ein SSE-Event endet mit Doppel-Newline (\n\n).
//
// Parser-Zustand: m_sseBuffer ist der "UART-Ringbuffer" —
// wir lesen solange Bytes bis wir ein komplettes Paket haben.

void McpClientSse::onSseData()
{
    m_sseBuffer.append(m_sseReply->readAll());

    // Events durch \n\n getrennt
    while (true) {
        const int sep = m_sseBuffer.indexOf("\n\n");
        if (sep < 0) break;  // noch kein vollständiges Event

        const QByteArray eventData = m_sseBuffer.left(sep);
        m_sseBuffer.remove(0, sep + 2);

        // Event-Felder parsen: "event: ...\ndata: ..."
        QByteArray eventType;
        QByteArray dataLine;

        for (const QByteArray &line : eventData.split('\n')) {
            if (line.startsWith("event:"))
                eventType = line.mid(6).trimmed();
            else if (line.startsWith("data:"))
                dataLine  = line.mid(5).trimmed();
        }

        if (!dataLine.isEmpty())
            processSseEvent(eventType, dataLine);
    }
}

void McpClientSse::onSseError()
{
    if (m_state == State::WaitingForEndpoint ||
        m_state == State::Initializing       ||
        m_state == State::ListingTools) {
        const QString err = m_sseReply
            ? m_sseReply->errorString()
            : "SSE-Verbindung getrennt";
        if (m_onReady) m_onReady(false, err);
        m_onReady = nullptr;
    }

    if (m_sseReply) {
        m_sseReply->deleteLater();
        m_sseReply = nullptr;
    }
    emit disconnected();
}

// ─── SSE-Event verarbeiten ────────────────────────────────────────────────────

void McpClientSse::processSseEvent(const QByteArray &eventType,
                                    const QByteArray &data)
{
    // Schritt 1: endpoint-Event → POST-URL merken, dann initialize schicken
    if (eventType == "endpoint") {
        m_postEndpoint = QString::fromUtf8(data);
        if (!m_postEndpoint.startsWith("http"))
            m_postEndpoint = m_baseUrl + m_postEndpoint;

        m_state = State::Initializing;
        doInitialize();
        return;
    }

    // Schritt 2: message-Event → JSON-RPC Response dispatchen
    if (eventType == "message" || eventType.isEmpty()) {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) return;
        dispatchResponse(doc.object());
    }
}

// ─── JSON-RPC Request senden (POST) ──────────────────────────────────────────

void McpClientSse::postRequest(const QJsonObject &msg)
{
    QNetworkRequest req{QUrl(m_postEndpoint)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    const QByteArray body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    QNetworkReply *reply  = m_nam->post(req, body);

    // POST-Antwort ist bei MCP immer leer (202 Accepted).
    // Die eigentliche Antwort kommt per SSE. Reply nur für Fehler prüfen.
    connect(reply, &QNetworkReply::finished, this, [reply]() {
        reply->deleteLater();
    });
}

// ─── Response dispatchen ─────────────────────────────────────────────────────
// Analog zu McpClient::handleResponse() — id → pending callback auflösen.

void McpClientSse::dispatchResponse(const QJsonObject &msg)
{
    const int id = msg["id"].toInt(-1);

    // Initialisierungs-Antworten
    if (m_state == State::Initializing && id == 1) {
        // initialize-Response erhalten → listTools
        m_state = State::ListingTools;
        doListTools();
        return;
    }

    if (m_state == State::ListingTools && id == 2) {
        // tools/list-Response
        m_tools = msg["result"].toObject()["tools"].toArray();
        m_state = State::Ready;
        if (m_onReady) {
            m_onReady(true, {});
            m_onReady = nullptr;
        }
        return;
    }

    // Tool-Call Antworten
    if (id >= 0 && m_pending.contains(id)) {
        ToolCallback cb = m_pending.take(id);
        if (msg.contains("error")) {
            cb({}, msg["error"].toObject()["message"].toString());
        } else {
            // MCP tools/call result: {"content":[{"type":"text","text":"..."}]}
            const QJsonArray content =
                msg["result"].toObject()["content"].toArray();
            QString text;
            for (const auto &item : content)
                text += item.toObject()["text"].toString();
            cb(text, {});
        }
    }
}

// ─── Initialisierungssequenz ──────────────────────────────────────────────────
// Identisch zu McpClient::doInitialize() / doListTools()

void McpClientSse::doInitialize()
{
    postRequest({
        {"jsonrpc", "2.0"},
        {"id",      1},
        {"method",  "initialize"},
        {"params",  QJsonObject{
            {"protocolVersion", "2024-11-05"},
            {"capabilities",    QJsonObject{}},
            {"clientInfo",      QJsonObject{
                {"name",    "LlamaQt"},
                {"version", "1.0"}
            }}
        }}
    });
}

void McpClientSse::doListTools()
{
    postRequest({
        {"jsonrpc", "2.0"},
        {"id",      2},
        {"method",  "tools/list"},
        {"params",  QJsonObject{}}
    });
}

// ─── Tool aufrufen ────────────────────────────────────────────────────────────

void McpClientSse::callTool(const QString &name,
                              const QJsonObject &arguments,
                              ToolCallback callback)
{
    const int id = m_nextId++;
    m_pending[id] = callback;

    postRequest({
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"method",  "tools/call"},
        {"params",  QJsonObject{
            {"name",      name},
            {"arguments", arguments}
        }}
    });
}
