#include "McpClient.h"
#include <QJsonDocument>
#include <QDebug>

McpClient::McpClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_process, &QProcess::readyReadStandardOutput,
            this, &McpClient::onReadyRead);
    connect(&m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &McpClient::onProcessFinished);
}

McpClient::~McpClient()
{
    // Signal trennen BEVOR terminate() — sonst feuert finished → onProcessFinished
    // → emit serverDied → McpManager::scheduleRestart greift auf bereits
    // halb-zerstörten McpClient zu (Use-after-free / Segfault).
    disconnect(&m_process, nullptr, this, nullptr);

    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(2000))
            m_process.kill();  // Notfall: SIGKILL wenn terminate ignoriert wird
    }
}

// ─── start ───────────────────────────────────────────────────────────────────
// Startet das Server-Binary und führt den MCP-Handshake durch.
void McpClient::start(const QString &serverBinary,
                      const QStringList &args,
                      std::function<void(bool, QString)> onReady)
{
    m_onReady = onReady;
    m_state   = State::Initializing;

    m_process.setProgram(serverBinary);
    m_process.setArguments(args);
    m_process.start();

    if (!m_process.waitForStarted(3000)) {
        onReady(false, QString("Server-Binary nicht startbar: %1").arg(serverBinary));
        return;
    }

    doInitialize();
}

// ─── doInitialize ────────────────────────────────────────────────────────────
// MCP Schritt 1: initialize Request.
// Der Server antwortet mit seinen Capabilities und der protocolVersion.
// Danach schicken wir die "initialized" Notification (kein id = fire-and-forget).
void McpClient::doInitialize()
{
    // Request mit id — Server muss antworten
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 0;  // id=0 reserviert für initialize
    req["method"]  = "initialize";
    req["params"]  = QJsonObject{
        {"protocolVersion", "2024-11-05"},
        {"capabilities",    QJsonObject{}},
        {"clientInfo",      QJsonObject{{"name", "LlamaQt"}, {"version", "1.0"}}}
    };
    sendRequest(req);
}

// ─── doListTools ─────────────────────────────────────────────────────────────
// MCP Schritt 3: tools/list.
// Server antwortet mit Array aller Tools (name + description + inputSchema).
// Das inputSchema ist JSON Schema — wir nutzen es für den System-Prompt.
void McpClient::doListTools()
{
    int id = m_nextId++;
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"]      = id;
    req["method"]  = "tools/list";
    req["params"]  = QJsonObject{};

    // Callback: m_tools wurde bereits in handleResponse gesetzt
    // (dort haben wir Zugriff auf das rohe result-Objekt vor der Text-Extraktion)
    // Hier nur noch Fehlerbehandlung und onReady aufrufen.
    m_pending[id] = [this](QString /*result*/, QString error) {
        if (!error.isEmpty()) {
            m_onReady(false, "tools/list fehlgeschlagen: " + error);
            return;
        }
        m_state = State::Ready;
        m_onReady(true, {});
    };

    sendRequest(req);
}

// ─── callTool ────────────────────────────────────────────────────────────────
// Ruft ein Tool auf dem Server auf. Ergebnis kommt async per Callback.
//
// JSON-RPC tools/call Request:
//   {"jsonrpc":"2.0","id":N,"method":"tools/call",
//    "params":{"name":"read_file","arguments":{"path":"foo.cpp"}}}
void McpClient::callTool(const QString &name,
                         const QJsonObject &arguments,
                         ToolCallback callback)
{
    int id = m_nextId++;

    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"]      = id;
    req["method"]  = "tools/call";
    req["params"]  = QJsonObject{
        {"name",      name},
        {"arguments", arguments}
    };

    m_pending[id] = callback;
    sendRequest(req);
}

// ─── sendRequest ─────────────────────────────────────────────────────────────
// Serialisiert ein JSON-Objekt als eine Zeile und schreibt es in stdin des Servers.
// newline-delimited JSON: jede Nachricht = eine Zeile.
void McpClient::sendRequest(const QJsonObject &msg)
{
    QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    m_process.write(line);
}

// ─── onReadyRead ─────────────────────────────────────────────────────────────
// Liest verfügbare Bytes aus stdout des Servers.
// Wichtig: Daten können in mehreren Chunks ankommen (TCP-ähnliches Splitting).
// Daher: Bytes in m_readBuffer akkumulieren, nach '\n' splitten.
//
// Analogie AVR: UART-Empfang mit Ringpuffer — Zeichen akkumulieren bis '\n'.
void McpClient::onReadyRead()
{
    m_readBuffer += m_process.readAllStandardOutput();

    // Alle vollständigen Zeilen verarbeiten
    int newlinePos;
    while ((newlinePos = m_readBuffer.indexOf('\n')) != -1) {
        QByteArray line = m_readBuffer.left(newlinePos).trimmed();
        m_readBuffer.remove(0, newlinePos + 1);

        if (line.isEmpty()) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError) {
            qWarning() << "McpClient: ungültiges JSON vom Server:" << err.errorString();
            continue;
        }

        QJsonObject obj = doc.object();

        // Notification (kein "id") vs Response (hat "id")
        if (!obj.contains("id")) {
            handleNotification(obj);
        } else {
            handleResponse(obj);
        }
    }
}

// ─── handleResponse ──────────────────────────────────────────────────────────
// Verarbeitet eine JSON-RPC Response vom Server.
// id=0 → initialize Response → "initialized" Notification senden → listTools
// id>0 → Tool-Call Response → Callback aufrufen
void McpClient::handleResponse(const QJsonObject &msg)
{
    int id = msg.value("id").toInt(-1);

    // ─── initialize Response (id=0) ──────────────────────────────────────
    if (id == 0) {
        // Server hat initialize beantwortet → wir schicken initialized Notification
        // Notification hat KEIN id-Feld (fire-and-forget)
        QJsonObject notif;
        notif["jsonrpc"] = "2.0";
        notif["method"]  = "notifications/initialized";
        sendRequest(notif);

        // Server-Name aus der Response extrahieren
        m_serverName = msg.value("result").toObject()
                          .value("serverInfo").toObject()
                          .value("name").toString("unknown");

        // Schritt 3: Tools abfragen
        m_state = State::ListingTools;
        doListTools();
        return;
    }

    // ─── Tool-Call Response ───────────────────────────────────────────────
    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        qWarning() << "McpClient: unbekannte Response id:" << id;
        return;
    }

    ToolCallback callback = it.value();
    m_pending.erase(it);

    // Fehler-Response: {"jsonrpc":"2.0","id":N,"error":{"code":-32000,"message":"..."}}
    if (msg.contains("error")) {
        QString errMsg = msg.value("error").toObject()
                            .value("message").toString("Unbekannter Fehler");
        callback({}, errMsg);
        return;
    }

    // Erfolg: result enthaelt das Tool-Ergebnis
    QJsonObject result = msg.value("result").toObject();

    // Sonderfall: tools/list Response hat "tools"-Array direkt im result-Objekt
    // (kein "content"-Wrapper wie bei tool-calls).
    // Wir erkennen das daran dass "tools" vorhanden und "content" fehlt.
    if (result.contains("tools") && !result.contains("content")) {
        m_tools = result.value("tools").toArray();
        callback({}, {});  // Signal an doListTools-Callback: fertig, kein Fehler
        return;
    }

    // Normale Tool-Call Response:
    // MCP spec: result.content ist Array von {type:"text", text:"..."}
    QJsonArray content = result.value("content").toArray();

    QString text;
    for (const QJsonValue &item : content) {
        QJsonObject part = item.toObject();
        if (part.value("type").toString() == "text") {
            text += part.value("text").toString();
        }
    }

    bool isError = result.value("isError").toBool(false);
    if (isError) {
        callback({}, text);
    } else {
        callback(text, {});
    }
}

// ─── handleNotification ──────────────────────────────────────────────────────
// Notifications vom Server (kein id, keine Antwort erwartet).
// Aktuell nur geloggt — könnte für Progress-Events genutzt werden.
void McpClient::handleNotification(const QJsonObject &msg)
{
    QString method = msg.value("method").toString();
    qDebug() << "McpClient: Notification vom Server:" << method;
}

// ─── onProcessFinished ───────────────────────────────────────────────────────
void McpClient::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(exitCode)
    Q_UNUSED(status)

    // Alle ausstehenden Callbacks mit Fehler abschließen
    for (auto &cb : m_pending) {
        cb({}, "Server-Prozess beendet.");
    }
    m_pending.clear();

    emit serverDied(m_serverName);
}

bool McpClient::isRunning() const
{
    return m_process.state() == QProcess::Running;
}
