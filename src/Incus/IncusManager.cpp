#include "Incus/IncusManager.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslKey>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QLocalSocket>
#include <QTcpSocket>
#include <QTimer>

// ─── Konstruktor / Destruktor ─────────────────────────────────────────────────

IncusManager::IncusManager(QObject *parent)
    : QObject(parent)
{}

IncusManager::~IncusManager() = default;

// ─── Konfiguration ────────────────────────────────────────────────────────────

void IncusManager::setUnixSocket(const QString &path)
{
    m_transport  = Transport::UnixSocket;
    m_socketPath = path;
}

void IncusManager::setHttps(const QString &host, int port,
                             const QString &certPath, const QString &keyPath)
{
    m_transport = Transport::Https;
    m_httpsHost = host;
    m_httpsPort = port;
    m_certPath  = certPath;
    m_keyPath   = keyPath;

    // QNetworkAccessManager lazy init
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
}

bool IncusManager::isConfigured() const
{
    if (m_transport == Transport::UnixSocket)
        return QFile::exists(m_socketPath);
    return !m_httpsHost.isEmpty();
}

// ─── TLS-Zertifikat generieren ────────────────────────────────────────────────
// Verwendet openssl auf dem Host um ein Self-Signed Client-Zertifikat zu
// erstellen. Das ist einmalig nötig für Remote-Verbindungen.
//
// Algorithmus:
//   1. Konfig-Verzeichnis sicherstellen
//   2. openssl req -x509 ... aufrufen (blocking, einmalig ok)
//   3. Trust-Befehl für den User zurückgeben

bool IncusManager::generateClientCert(QString &outTrustCommand, QString &outError)
{
    const QString configDir = QStandardPaths::writableLocation(
                                  QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);

    const QString certFile = configDir + "/client.crt";
    const QString keyFile  = configDir + "/client.key";

    if (QFile::exists(certFile) && QFile::exists(keyFile)) {
        outTrustCommand = QString("incus config trust add-certificate %1").arg(certFile);
        return true;
    }

    QProcess openssl;
    openssl.start("openssl", {
        "req", "-x509", "-newkey", "rsa:4096",
        "-keyout", keyFile,
        "-out",    certFile,
        "-days",   "3650",
        "-nodes",
        "-subj",   "/CN=llamaqt-client"
    });
    openssl.waitForFinished(10000);

    if (openssl.exitCode() != 0) {
        outError = QString("openssl fehlgeschlagen: %1")
                   .arg(QString::fromLocal8Bit(openssl.readAllStandardError()));
        return false;
    }

    m_certPath = certFile;
    m_keyPath  = keyFile;
    outTrustCommand = QString("incus config trust add-certificate %1").arg(certFile);
    return true;
}

// ─── Container-Operationen ────────────────────────────────────────────────────

void IncusManager::listContainers(ListCallback cb)
{
    sendRequest("GET", "/1.0/instances?recursion=1", {}, [cb](int status,
                                                               QJsonObject resp,
                                                               QString err) {
        if (!err.isEmpty()) { cb({}, err); return; }
        if (status != 200) {
            cb({}, QString("HTTP %1").arg(status));
            return;
        }

        // Incus liefert: {"metadata": [...], "type": "sync"}
        QVector<IncusContainer> result;
        const QJsonArray arr = resp["metadata"].toArray();
        for (const auto &item : arr) {
            const QJsonObject obj = item.toObject();
            IncusContainer c;
            c.name   = obj["name"].toString();
            c.status = obj["status"].toString();
            c.image  = obj["config"].toObject()["image.description"].toString();
            c.arch   = obj["architecture"].toString();

            // Erste IPv4 aus state.network
            const QJsonObject net = obj["state"].toObject()["network"].toObject();
            for (auto it = net.begin(); it != net.end(); ++it) {
                const QJsonArray addrs = it.value().toObject()["addresses"].toArray();
                for (const auto &a : addrs) {
                    const QJsonObject ao = a.toObject();
                    if (ao["family"].toString() == "inet" &&
                        ao["address"].toString() != "127.0.0.1") {
                        c.ipv4 = ao["address"].toString();
                        break;
                    }
                }
                if (!c.ipv4.isEmpty()) break;
            }
            result.append(c);
        }
        cb(result, {});
    });
}

void IncusManager::createContainer(const QString &name, const QString &image,
                                   Callback cb)
{
    // Image-Alias: "debian/trixie" → source type=image, alias=...
    // Incus unterscheidet lokale Images von remote images:trixie
    const QJsonObject body {
        {"name", name},
        {"source", QJsonObject{
            {"type",  "image"},
            {"alias", image}
        }},
        {"config", QJsonObject{}}
    };

    sendRequest("POST", "/1.0/instances", body, [this, cb](int status,
                                                            QJsonObject resp,
                                                            QString err) {
        if (!err.isEmpty()) { cb(false, err); return; }
        if (status == 202) {
            // Async-Operation — warten bis Container erstellt
            const QString opUrl = resp["operation"].toString();
            waitForOperation(opUrl, cb);
        } else if (status == 200) {
            cb(true, {});
        } else {
            cb(false, QString("HTTP %1: %2").arg(status)
               .arg(resp["error"].toString()));
        }
    });
}

void IncusManager::startContainer(const QString &name, Callback cb)
{
    const QString path = QString("/1.0/instances/%1/state").arg(name);
    const QJsonObject body { {"action", "start"}, {"timeout", 30} };

    sendRequest("PUT", path, body, [this, cb](int status, QJsonObject resp,
                                               QString err) {
        if (!err.isEmpty()) { cb(false, err); return; }
        if (status == 202)
            waitForOperation(resp["operation"].toString(), cb);
        else if (status == 200)
            cb(true, {});
        else
            cb(false, QString("HTTP %1").arg(status));
    });
}

void IncusManager::stopContainer(const QString &name, Callback cb)
{
    const QString path = QString("/1.0/instances/%1/state").arg(name);
    const QJsonObject body { {"action", "stop"}, {"timeout", 30}, {"force", false} };

    sendRequest("PUT", path, body, [this, cb](int status, QJsonObject resp,
                                               QString err) {
        if (!err.isEmpty()) { cb(false, err); return; }
        if (status == 202)
            waitForOperation(resp["operation"].toString(), cb);
        else if (status == 200)
            cb(true, {});
        else
            cb(false, QString("HTTP %1").arg(status));
    });
}

void IncusManager::deleteContainer(const QString &name, Callback cb)
{
    sendRequest("DELETE", QString("/1.0/instances/%1").arg(name), {},
                [this, cb](int status, QJsonObject resp, QString err) {
        if (!err.isEmpty()) { cb(false, err); return; }
        if (status == 202)
            waitForOperation(resp["operation"].toString(), cb);
        else if (status == 200)
            cb(true, {});
        else
            cb(false, QString("HTTP %1").arg(status));
    });
}

void IncusManager::listImages(ImagesCallback cb)
{
    sendRequest("GET", "/1.0/images?recursion=1", {}, [cb](int status,
                                                            QJsonObject resp,
                                                            QString err) {
        if (!err.isEmpty()) { cb({}, err); return; }
        QStringList result;
        for (const auto &item : resp["metadata"].toArray()) {
            const QJsonObject obj = item.toObject();
            // Aliases als lesbare Namen
            for (const auto &alias : obj["aliases"].toArray()) {
                const QString n = alias.toObject()["name"].toString();
                if (!n.isEmpty()) result << n;
            }
        }
        cb(result, {});
    });
}

void IncusManager::pushFile(const QString &containerName,
                             const QString &localPath,
                             const QString &remotePath,
                             Callback cb)
{
    // Incus REST API: POST /1.0/instances/{name}/files?path=...
    // Body = raw file content (kein JSON), Header X-Incus-* für Metadaten.
    // Für die Implementierung nutzen wir `incus file push` CLI als Workaround,
    // da raw-binary über QNetworkRequest komplex ist.
    //
    // Analogie (AVR): SPI DMA-Transfer — die Daten gehen direkt raus,
    // kein overhead durch Protokoll-Schichten.

    exec(containerName, {}, [](int, QString, QString){});  // (Platzhalter)

    QProcess *proc = new QProcess(this);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [proc, cb](int exitCode, QProcess::ExitStatus) {
        const QString err = exitCode == 0 ? QString{}
            : QString::fromLocal8Bit(proc->readAllStandardError());
        cb(exitCode == 0, err);
        proc->deleteLater();
    });
    proc->start("incus", {"file", "push", localPath,
                           containerName + remotePath});
}

void IncusManager::exec(const QString &containerName,
                        const QStringList &command,
                        std::function<void(int, QString, QString)> cb)
{
    // Incus exec via CLI — REST-Exec ist komplex (WebSocket).
    // Für Build-Zwecke reicht CLI vollständig.
    QProcess *proc = new QProcess(this);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [proc, cb](int exitCode, QProcess::ExitStatus) {
        const QString out = QString::fromLocal8Bit(proc->readAllStandardOutput());
        const QString err = QString::fromLocal8Bit(proc->readAllStandardError());
        cb(exitCode, out, err);
        proc->deleteLater();
    });

    QStringList args = {"exec", containerName, "--"};
    args << command;
    proc->start("incus", args);
}

// ─── Async-Operation warten ───────────────────────────────────────────────────
// Incus gibt bei langen Operationen (create, start) HTTP 202 zurück mit
// einer Operations-URL. Wir pollen /operations/{uuid}/wait.
//
// Pattern: Rekursiver Callback — wie ein Watchdog der sich selbst neu startet

void IncusManager::waitForOperation(const QString &opUrl, Callback cb)
{
    const QString waitPath = opUrl + "/wait?timeout=60";
    sendRequest("GET", waitPath, {}, [cb](int status, QJsonObject resp,
                                          QString err) {
        if (!err.isEmpty()) { cb(false, err); return; }
        const QString opStatus = resp["metadata"].toObject()["status"].toString();
        if (opStatus == "Success")
            cb(true, {});
        else
            cb(false, resp["metadata"].toObject()["err"].toString());
    });
}

// ─── Interner HTTP-Dispatcher ─────────────────────────────────────────────────
// Leitet an Unix-Socket oder HTTPS weiter je nach Transport.

void IncusManager::sendRequest(const QString &method,
                                const QString &path,
                                const QJsonObject &body,
                                std::function<void(int, QJsonObject, QString)> cb)
{
    if (m_transport == Transport::UnixSocket)
        sendUnixRequest(method, path, body, cb);
    else
        sendHttpsRequest(method, path, body, cb);
}

// ─── Unix Socket Transport ────────────────────────────────────────────────────
// HTTP/1.1 direkt über Unix Domain Socket.
// Qt hat kein eingebautes "HTTP über Unix Socket" — wir bauen den Request
// manuell als Rohtext und lesen die Antwort selbst.
//
// Aufbau einer HTTP/1.1 Anfrage:
//   METHOD /path HTTP/1.1\r\n
//   Host: localhost\r\n
//   Content-Type: application/json\r\n
//   Content-Length: N\r\n
//   \r\n
//   {body-json}
//
// Analogie (AVR): Wie UART ohne Hardware-FIFO — wir schreiben Byte für Byte
// und lesen bis "\r\n\r\n" (Header-Ende) kommt.

// ─── Hilfsfunktion: Chunked Transfer Encoding dekodieren ─────────────────────
    //
    // HTTP "Transfer-Encoding: chunked" funktioniert so:
    //
    //   <Größe in Hex>\r\n
    //   <Daten dieser Größe>\r\n
    //   <nächste Größe in Hex>\r\n
    //   <Daten>\r\n
    //   0\r\n          ← letzter Chunk: Größe 0 = Ende
    //   \r\n
    //
    // Beispiel aus Incus:
    //   1119\r\n
    //   {"type":"sync",...}\r\n
    //   0\r\n
    //   \r\n
    //
    // Wir lesen Chunk für Chunk und kleben die Daten zusammen.
    // Wenn kein Chunked Encoding vorliegt (normale Antwort), geben wir
    // den Body unverändert zurück.
    //
    // Algorithmus: Zustandsautomat mit zwei Zuständen:
    //   READING_SIZE  → liest "<hex>\r\n"
    //   READING_DATA  → liest <size> Bytes + "\r\n"

    static QByteArray decodeChunked(const QByteArray &body)
{
    QByteArray result;
    int pos = 0;

    while (pos < body.size()) {
        // ── Chunk-Größe lesen ──────────────────────────────────────────────
        // Suche das \r\n das die Größenzeile abschließt
        const int lineEnd = body.indexOf("\r\n", pos);
        if (lineEnd < 0) break;  // kein vollständiger Chunk mehr

        const QByteArray sizeLine = body.mid(pos, lineEnd - pos);
        bool ok = false;
        const int chunkSize = sizeLine.trimmed().toInt(&ok, 16);  // Hex → Int
        if (!ok) break;  // Parse-Fehler

        pos = lineEnd + 2;  // hinter das \r\n springen

        // ── Letzter Chunk? ─────────────────────────────────────────────────
        if (chunkSize == 0) break;  // 0\r\n = Ende

        // ── Chunk-Daten lesen ──────────────────────────────────────────────
        if (pos + chunkSize > body.size()) break;  // unvollständig

        result.append(body.mid(pos, chunkSize));
        pos += chunkSize;

        // Abschließendes \r\n nach den Daten überspringen
        if (pos + 2 <= body.size() && body.mid(pos, 2) == "\r\n")
            pos += 2;
    }

    return result;
}

// ─── Unix Socket Transport ────────────────────────────────────────────────────
// HTTP/1.1 direkt über Unix Domain Socket.
//
// Qt's QLocalSocket kann keine HTTP-Requests — wir bauen den Request
// manuell als Rohtext und parsen die Antwort selbst.
//
// Aufbau einer HTTP/1.1 Anfrage:
//   METHOD /path HTTP/1.1\r\n
//   Host: localhost\r\n
//   Content-Type: application/json\r\n   (nur wenn Body vorhanden)
//   Content-Length: N\r\n
//   \r\n
//   {body-json}
//
// Incus antwortet mit Transfer-Encoding: chunked — deshalb brauchen
// wir decodeChunked() um den Body zu extrahieren.
//
// Pattern: Zustandsautomat über Qt-Signale:
//   connected → senden
//   readyRead → puffern
//   disconnected → parsen + Callback

// ─── Hilfsfunktion: Chunked Transfer Encoding dekodieren ─────────────────────
//
// HTTP "Transfer-Encoding: chunked" funktioniert so:
//
//   <Größe in Hex>\r\n
//   <Daten dieser Größe>\r\n
//   <nächste Größe in Hex>\r\n
//   <Daten>\r\n
//   0\r\n          ← letzter Chunk: Größe 0 = Ende
//   \r\n
//
// Wir lesen Chunk für Chunk und kleben die Daten zusammen.

// ─── Unix Socket Transport ────────────────────────────────────────────────────

void IncusManager::sendUnixRequest(
    const QString &method, const QString &path, const QJsonObject &body,
    std::function<void(int, QJsonObject, QString)> cb)
{
    QLocalSocket *sock = new QLocalSocket(this);

    auto buf    = QSharedPointer<QByteArray>::create();
    auto called = std::make_shared<bool>(false);

    // ── Signal 1: Verbunden → Request senden ──────────────────────────────
    connect(sock, &QLocalSocket::connected, this, [sock, method, path, body]() {
        const QByteArray bodyBytes = body.isEmpty()
        ? QByteArray{}
        : QJsonDocument(body).toJson(QJsonDocument::Compact);

        QString request;
        request += method + " " + path + " HTTP/1.1\r\n";
        request += "Host: localhost\r\n";
        if (!bodyBytes.isEmpty()) {
            request += "Content-Type: application/json\r\n";
            request += QString("Content-Length: %1\r\n").arg(bodyBytes.size());
        } else {
            request += "Content-Length: 0\r\n";
        }
        request += "Connection: close\r\n";
        request += "\r\n";

        qDebug() << "=== SENDING REQUEST ===";
        qDebug() << request;

        sock->write(request.toUtf8());
        if (!bodyBytes.isEmpty())
            sock->write(bodyBytes);
    });

    // ── Signal 2: Daten empfangen → puffern ───────────────────────────────
    connect(sock, &QLocalSocket::readyRead, this, [sock, buf]() {
        const QByteArray chunk = sock->readAll();
        buf->append(chunk);
        qDebug() << "=== DATA RECEIVED ===" << chunk.size()
                 << "bytes, total:" << buf->size();
    });

    // ── Signal 3: Verbindung getrennt → parsen ────────────────────────────
    connect(sock, &QLocalSocket::disconnected, this,
            [sock, buf, cb, called]() {
                if (*called) { sock->deleteLater(); return; }
                *called = true;

                qDebug() << "=== DISCONNECTED, buf size ===" << buf->size();

                int httpStatus = 0;
                QJsonObject respJson;
                QString error;

                const int headerEnd = buf->indexOf("\r\n\r\n");
                qDebug() << "headerEnd:" << headerEnd;

                if (headerEnd < 0) {
                    error = "Keine vollständige HTTP-Antwort";
                } else {
                    const QString statusLine = QString::fromUtf8(
                                                   buf->left(buf->indexOf('\n'))).trimmed();
                    const QStringList parts = statusLine.split(' ');
                    if (parts.size() >= 2)
                        httpStatus = parts[1].toInt();

                    qDebug() << "httpStatus:" << httpStatus;

                    const QByteArray headers = buf->left(headerEnd);
                    const bool isChunked = headers.toLower()
                                               .contains("transfer-encoding: chunked");

                    qDebug() << "isChunked:" << isChunked;

                    QByteArray jsonBody = buf->mid(headerEnd + 4);

                    qDebug() << "raw jsonBody first 100 bytes:" << jsonBody.left(100);

                    if (isChunked) {
                        jsonBody = decodeChunked(jsonBody);
                        qDebug() << "decoded jsonBody first 100 bytes:" << jsonBody.left(100);
                    }

                    QJsonParseError pe;
                    const QJsonDocument doc = QJsonDocument::fromJson(jsonBody, &pe);
                    if (pe.error == QJsonParseError::NoError && doc.isObject()) {
                        respJson = doc.object();
                        qDebug() << "JSON parsed OK, keys:" << respJson.keys();
                    } else if (!jsonBody.trimmed().isEmpty()) {
                        error = QString("JSON-Parse-Fehler: %1 (body: %2)")
                        .arg(pe.errorString())
                            .arg(QString::fromUtf8(jsonBody.left(200)));
                        qDebug() << "JSON parse ERROR:" << error;
                    }
                }

                sock->deleteLater();
                cb(httpStatus, respJson, error);
            });

    // ── Signal 4: Socket-Fehler ────────────────────────────────────────────
    connect(sock, &QLocalSocket::errorOccurred, this,
            [sock, buf, cb, called](QLocalSocket::LocalSocketError errCode) {
                if (errCode == QLocalSocket::PeerClosedError) return;
                // PeerClosedError = Gegenseite hat Connection: close gemacht.
                // Das ist kein Fehler — disconnected-Signal kommt danach und
                // parst die Antwort. Hier nichts tun.
                if (errCode == QLocalSocket::PeerClosedError) return;

                if (*called) { sock->deleteLater(); return; }
                *called = true;

                const QString msg = sock->errorString();
                qDebug() << "=== SOCKET ERROR ===" << errCode << msg;
                sock->deleteLater();
                cb(0, {}, "Socket-Fehler: " + msg);
            });

    sock->connectToServer(m_socketPath);
}


// ─── HTTPS Transport ──────────────────────────────────────────────────────────
// Standard QNetworkAccessManager mit Client-Zertifikat.
// Incus verwendet self-signed Server-Zertifikate → SSL-Verifikation angepasst.

void IncusManager::sendHttpsRequest(
        const QString &method, const QString &path, const QJsonObject &body,
        std::function<void(int, QJsonObject, QString)> cb)
{
    if (!m_nam) {
        cb(0, {}, "HTTPS nicht konfiguriert");
        return;
    }

    const QString url = QString("https://%1:%2%3").arg(m_httpsHost)
                        .arg(m_httpsPort).arg(path);
    QNetworkRequest req{QUrl(url)};

    // Client-Zertifikat laden
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    if (!m_certPath.isEmpty() && !m_keyPath.isEmpty()) {
        QFile certFile(m_certPath), keyFile(m_keyPath);
        certFile.open(QIODevice::ReadOnly);
        keyFile.open(QIODevice::ReadOnly);
        ssl.setLocalCertificate(QSslCertificate(&certFile));
        ssl.setPrivateKey(QSslKey(&keyFile, QSsl::Rsa));
    }
    // Incus hat self-signed Zertifikat → Peer-Verifikation auf LoEx-Modus
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(ssl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    const QByteArray bodyBytes = body.isEmpty()
        ? QByteArray{}
        : QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = nullptr;
    if      (method == "GET")    reply = m_nam->get(req);
    else if (method == "POST")   reply = m_nam->post(req, bodyBytes);
    else if (method == "PUT")    reply = m_nam->put(req, bodyBytes);
    else if (method == "DELETE") reply = m_nam->deleteResource(req);
    else { cb(0, {}, "Unbekannte Methode: " + method); return; }

    connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        QString error;
        QJsonObject respJson;

        if (reply->error() != QNetworkReply::NoError) {
            error = reply->errorString();
        } else {
            QJsonParseError pe;
            const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
            if (pe.error == QJsonParseError::NoError && doc.isObject())
                respJson = doc.object();
        }
        reply->deleteLater();
        cb(status, respJson, error);
    });
}
