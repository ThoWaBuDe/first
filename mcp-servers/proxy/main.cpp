// ─── llamaqt-mcp-proxy ────────────────────────────────────────────────────────
// HTTP+SSE Multiplexer für alle LlamaQt MCP-Server.
// Läuft im Container als systemd-Service.
//
// Architektur:
//   Ein HTTP-Port (Standard: 9000)
//   GET  /sse       → SSE-Stream öffnen (Client hält offen)
//   POST /message   → JSON-RPC Request (tools/list, tools/call, initialize)
//
// Tool-Registry:
//   Beim Start werden alle konfigurierten MCP-Server via stdio gestartet.
//   Jedes Tool kennt seinen Server-Index.
//   tools/call → Proxy leitet an den richtigen stdio-Server weiter.
//
// Warum ein Proxy statt mehrere SSE-Server?
//   1. Nur ein Port nötig → einfaches Firewall-Setup
//   2. Client macht einen Handshake, nicht N
//   3. Tool-Namespace bleibt flach (wie bisher)
//
// Analogie (AVR):
//   Der Proxy ist der I2C-Bus: viele Slaves (Server), ein Master (Client),
//   ein gemeinsamer Bus (Port 9000). Adressierung = Tool-Name.
//
// Pattern:
//   Command-Dispatcher: incoming JSON-RPC → switch(method) → handler
//   Observer (SSE): alle offenen SSE-Verbindungen bekommen Events
//
// Kompiliert als Qt-Konsolenanwendung (kein GUI benötigt).

#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QProcess>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>
#include <QTextStream>
#include <QCommandLineParser>
#include <QHash>
#include <QVector>
#include <functional>
#include <memory>

// ─── Stdio MCP Client (embedded, kein Header nötig) ──────────────────────────
// Vereinfachter McpClient direkt im Proxy — kein Qt-Widget-Overhead.

struct StdioServer {
    QString     name;
    QString     binary;
    QStringList args;
    QProcess   *process   = nullptr;
    QByteArray  readBuf;
    QJsonArray  tools;
    bool        ready     = false;

    // Pending callbacks: id → callback
    QHash<int, std::function<void(QJsonObject)>> pending;
    int nextId = 100;  // ab 100 damit nicht mit Proxy-IDs kollidiert

    void send(const QJsonObject &msg) {
        if (!process || process->state() != QProcess::Running) return;
        const QByteArray line =
            QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
        process->write(line);
    }
};

// ─── HTTP/SSE Verbindung ──────────────────────────────────────────────────────
struct SseConnection {
    QTcpSocket *socket = nullptr;
    bool        headerSent = false;
    QString     clientId;  // für Logging

    void sendEvent(const QString &eventType, const QJsonObject &data) {
        if (!socket || !socket->isOpen()) return;
        const QByteArray payload =
            QJsonDocument(data).toJson(QJsonDocument::Compact);
        const QString sseMsg = QString("event: %1\ndata: %2\n\n")
                               .arg(eventType)
                               .arg(QString::fromUtf8(payload));
        socket->write(sseMsg.toUtf8());
        socket->flush();
    }

    void sendEndpointEvent() {
        if (!socket || !socket->isOpen()) return;
        // Teile dem Client mit, wohin er JSON-RPC POSTs schicken soll
        const QString sseMsg = "event: endpoint\ndata: /message\n\n";
        socket->write(sseMsg.toUtf8());
        socket->flush();
    }
};

// ─── Proxy Hauptklasse ────────────────────────────────────────────────────────
class McpProxy : public QObject {
    Q_OBJECT

public:
    McpProxy(int port, QObject *parent = nullptr)
        : QObject(parent), m_port(port)
    {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection,
                this, &McpProxy::onNewConnection);
    }

    bool start() {
        if (!m_server->listen(QHostAddress::Any, m_port)) {
            qCritical() << "Port" << m_port << "konnte nicht geöffnet werden:"
                        << m_server->errorString();
            return false;
        }
        qInfo() << "llamaqt-mcp-proxy lauscht auf Port" << m_port;
        return true;
    }

    // Stdio-Server registrieren und starten
    void addServer(const QString &name, const QString &binary,
                   const QStringList &args = {}) {
        auto *srv = new StdioServer();
        srv->name   = name;
        srv->binary = binary;
        srv->args   = args;

        m_servers.append(srv);
        const int idx = m_servers.size() - 1;

        startStdioServer(idx);
    }

    // Alle Tools als flaches Array (für tools/list Antwort)
    QJsonArray allTools() const {
        QJsonArray all;
        for (const StdioServer *srv : m_servers)
            for (const auto &t : srv->tools)
                all.append(t);
        return all;
    }

private slots:
    void onNewConnection() {
        while (m_server->hasPendingConnections()) {
            QTcpSocket *socket = m_server->nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead,
                    this, [this, socket]() { onSocketData(socket); });
            connect(socket, &QTcpSocket::disconnected,
                    this, [this, socket]() { onSocketDisconnected(socket); });
        }
    }

    void onSocketData(QTcpSocket *socket) {
        // HTTP Request puffern
        if (!m_socketBuffers.contains(socket))
            m_socketBuffers[socket] = {};
        m_socketBuffers[socket].append(socket->readAll());

        const QByteArray &buf = m_socketBuffers[socket];
        const int headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;  // Header noch nicht komplett

        // Request-Zeile parsen
        const QString firstLine = QString::fromUtf8(
            buf.left(buf.indexOf('\n'))).trimmed();
        const QStringList parts = firstLine.split(' ');
        if (parts.size() < 2) return;

        const QString method = parts[0];
        const QString path   = parts[1].split('?')[0];

        if (method == "GET" && path == "/sse") {
            handleSseRequest(socket, buf);
        } else if (method == "POST" && path == "/message") {
            const QByteArray body = buf.mid(headerEnd + 4);
            // Content-Length prüfen
            QString clHeader;
            for (const QByteArray &line : buf.left(headerEnd).split('\n')) {
                if (line.toLower().startsWith("content-length:"))
                    clHeader = QString::fromUtf8(line.mid(15)).trimmed();
            }
            const int cl = clHeader.toInt();
            if (body.size() < cl) return;  // Body noch nicht vollständig

            handlePostRequest(socket, body.left(cl));
            m_socketBuffers.remove(socket);
        } else {
            // 404
            socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            socket->close();
            m_socketBuffers.remove(socket);
        }
    }

    void onSocketDisconnected(QTcpSocket *socket) {
        m_socketBuffers.remove(socket);
        m_sseClients.remove(socket);
        socket->deleteLater();
    }

private:
    // ── SSE-Verbindung einrichten ─────────────────────────────────────────
    void handleSseRequest(QTcpSocket *socket, const QByteArray &/*headers*/) {
        // HTTP-Antwort: SSE-Header
        const QByteArray response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        socket->write(response);

        SseConnection conn;
        conn.socket      = socket;
        conn.headerSent  = true;
        conn.clientId    = socket->peerAddress().toString();
        m_sseClients[socket] = conn;

        // Endpoint-Event senden: Client weiß jetzt wohin er POSTen soll
        m_sseClients[socket].sendEndpointEvent();
        m_socketBuffers.remove(socket);

        qInfo() << "SSE-Client verbunden:" << conn.clientId;
    }

    // ── JSON-RPC POST verarbeiten ─────────────────────────────────────────
    void handlePostRequest(QTcpSocket *socket, const QByteArray &body) {
        // Immer 202 Accepted antworten — Antwort kommt per SSE
        socket->write("HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n");

        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) return;

        const QJsonObject msg = doc.object();
        const QString method  = msg["method"].toString();
        const int     id      = msg["id"].toInt(-1);

        // ── Dispatcher ────────────────────────────────────────────────────
        if (method == "initialize") {
            // Sofort antworten (Proxy hat keine eigene Capability)
            sendSseResponse(socket, id, QJsonObject{
                {"protocolVersion", "2024-11-05"},
                {"capabilities",    QJsonObject{}},
                {"serverInfo",      QJsonObject{
                    {"name",    "llamaqt-mcp-proxy"},
                    {"version", "1.0"}
                }}
            });

        } else if (method == "tools/list") {
            sendSseResponse(socket, id, QJsonObject{
                {"tools", allTools()}
            });

        } else if (method == "tools/call") {
            const QJsonObject params = msg["params"].toObject();
            const QString     tool   = params["name"].toString();
            const QJsonObject args   = params["arguments"].toObject();
            routeToolCall(socket, id, tool, args);

        } else {
            // Unbekannte Methode
            sendSseError(socket, id, -32601, "Method not found");
        }
    }

    // ── Tool-Call zum richtigen Server weiterleiten ───────────────────────
    // Das ist der "I2C-Bus-Arbitrierer" — findet den richtigen Slave.
    void routeToolCall(QTcpSocket *sseSocket, int clientId,
                       const QString &toolName, const QJsonObject &args) {
        // Welcher Server kennt dieses Tool?
        StdioServer *target = nullptr;
        for (StdioServer *srv : m_servers) {
            for (const auto &t : srv->tools) {
                if (t.toObject()["name"].toString() == toolName) {
                    target = srv;
                    break;
                }
            }
            if (target) break;
        }

        if (!target) {
            sendSseError(sseSocket, clientId, -32602,
                         "Tool not found: " + toolName);
            return;
        }

        // Server-internen Request-ID vergeben
        const int serverId = target->nextId++;
        target->pending[serverId] = [this, sseSocket, clientId]
                                    (const QJsonObject &result) {
            if (result.contains("error")) {
                sendSseError(sseSocket, clientId,
                             result["error"].toObject()["code"].toInt(),
                             result["error"].toObject()["message"].toString());
            } else {
                sendSseResponse(sseSocket, clientId, result["result"].toObject());
            }
        };

        target->send({
            {"jsonrpc", "2.0"},
            {"id",      serverId},
            {"method",  "tools/call"},
            {"params",  QJsonObject{
                {"name",      toolName},
                {"arguments", args}
            }}
        });
    }

    // ── SSE-Antwort an Client schicken ────────────────────────────────────
    void sendSseResponse(QTcpSocket *socket, int id, const QJsonObject &result) {
        if (!m_sseClients.contains(socket)) return;
        m_sseClients[socket].sendEvent("message", {
            {"jsonrpc", "2.0"},
            {"id",      id},
            {"result",  result}
        });
    }

    void sendSseError(QTcpSocket *socket, int id, int code, const QString &msg) {
        if (!m_sseClients.contains(socket)) return;
        m_sseClients[socket].sendEvent("message", {
            {"jsonrpc", "2.0"},
            {"id",      id},
            {"error",   QJsonObject{
                {"code",    code},
                {"message", msg}
            }}
        });
    }

    // ── Stdio-Server starten und initialisieren ───────────────────────────
    void startStdioServer(int idx) {
        StdioServer *srv = m_servers[idx];
        srv->process = new QProcess(this);

        // readyRead: JSON-Zeilen vom Server lesen
        connect(srv->process, &QProcess::readyReadStandardOutput,
                this, [this, srv]() {
            srv->readBuf.append(srv->process->readAllStandardOutput());
            while (true) {
                const int nl = srv->readBuf.indexOf('\n');
                if (nl < 0) break;
                const QByteArray line = srv->readBuf.left(nl);
                srv->readBuf.remove(0, nl + 1);

                QJsonParseError pe;
                const QJsonObject msg =
                    QJsonDocument::fromJson(line, &pe).object();
                if (pe.error != QJsonParseError::NoError) continue;

                handleStdioResponse(srv, msg);
            }
        });

        srv->process->start(srv->binary, srv->args);
        if (!srv->process->waitForStarted(3000)) {
            qWarning() << "Server konnte nicht gestartet werden:" << srv->binary;
            return;
        }

        // Initialisierung: initialize → listTools
        srv->pending[1] = [this, srv](const QJsonObject &) {
            // initialize fertig → listTools
            srv->pending[2] = [srv](const QJsonObject &resp) {
                srv->tools = resp["result"].toObject()["tools"].toArray();
                srv->ready = true;
                qInfo() << "Server" << srv->name << "bereit,"
                        << srv->tools.size() << "Tools registriert";
            };
            srv->send({
                {"jsonrpc", "2.0"}, {"id", 2},
                {"method", "tools/list"}, {"params", QJsonObject{}}
            });
        };
        srv->send({
            {"jsonrpc", "2.0"}, {"id", 1},
            {"method", "initialize"},
            {"params", QJsonObject{
                {"protocolVersion", "2024-11-05"},
                {"capabilities",    QJsonObject{}},
                {"clientInfo",      QJsonObject{
                    {"name", "llamaqt-mcp-proxy"}, {"version", "1.0"}
                }}
            }}
        });
    }

    void handleStdioResponse(StdioServer *srv, const QJsonObject &msg) {
        const int id = msg["id"].toInt(-1);
        if (id >= 0 && srv->pending.contains(id)) {
            auto cb = srv->pending.take(id);
            cb(msg);
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    int                         m_port;
    QTcpServer                 *m_server = nullptr;
    QVector<StdioServer*>       m_servers;
    QHash<QTcpSocket*, SseConnection>  m_sseClients;
    QHash<QTcpSocket*, QByteArray>     m_socketBuffers;
};

#include "main.moc"

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("llamaqt-mcp-proxy");
    app.setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOpt({"p", "port"}, "HTTP-Port (Standard: 9000)", "port", "9000");
    QCommandLineOption sandboxOpt({"s", "sandbox"}, "Sandbox-Pfad", "path", "/home/llamaqt/sandbox");
    parser.addOption(portOpt);
    parser.addOption(sandboxOpt);
    parser.process(app);

    const int     port    = parser.value(portOpt).toInt();
    const QString sandbox = parser.value(sandboxOpt);

    McpProxy proxy(port);

    // MCP-Server registrieren — Pfade nach dpkg install
    // Die Binaries landen unter /usr/lib/llamaqt-mcp/
    const QString binDir = "/usr/lib/llamaqt-mcp";

    proxy.addServer("filesystem", binDir + "/llamaqt-filesystem",
                    {"--sandbox", sandbox});
    proxy.addServer("sysinfo",    binDir + "/llamaqt-sysinfo");
    proxy.addServer("compile",    binDir + "/llamaqt-compile",
                    {"--sandbox", sandbox});

    if (!proxy.start()) return 1;

    return app.exec();
}
