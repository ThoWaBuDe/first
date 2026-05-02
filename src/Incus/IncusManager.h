#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QLocalSocket>
#include <QSslConfiguration>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include "Incus/IncusContainer.h"

// ─── IncusManager ─────────────────────────────────────────────────────────────
// REST API Client für den Incus-Daemon.
//
// Transport — Dualstack, konfigurierbar:
//   Unix Socket  /var/lib/incus/unix.socket  (lokal, Standard)
//   HTTPS        https://host:8443           (remote, TLS-Client-Zertifikat)
//
// Warum Dualstack?
//   Unix Socket = kein Port, kein TLS-Overhead, automatisch durch
//   Gruppenmitgliedschaft (incus-admin) gesichert. Für lokale Nutzung ideal.
//   HTTPS = für Remote-Zugriff auf anderen Rechnern nötig.
//
// Incus REST API Grundstruktur:
//   GET  /1.0/instances          → Liste aller Container
//   POST /1.0/instances          → Container anlegen
//   PUT  /1.0/instances/{name}/state → starten/stoppen
//   DEL  /1.0/instances/{name}   → Container löschen
//   GET  /1.0/images             → verfügbare Images
//
// Alle Operationen sind async — Ergebnis kommt per Callback (Lambda).
// Pattern: Command-Callback — kein blocking, kein Polling.
//
// Analogie (AVR): Wie ein UART-DMA-Transfer — du schickst den Befehl ab
// und bekommst per Interrupt (hier: Qt-Signal/Callback) Bescheid wenn fertig.

class IncusManager : public QObject {
    Q_OBJECT

public:
    // ── Transport-Auswahl ─────────────────────────────────────────────────
    enum class Transport {
        UnixSocket,  // lokal: /var/lib/incus/unix.socket
        Https        // remote: https://host:8443
    };

    explicit IncusManager(QObject *parent = nullptr);
    ~IncusManager() override;

    // ── Konfiguration ─────────────────────────────────────────────────────

    // Lokal: Unix Socket
    void setUnixSocket(const QString &path = "/var/lib/incus/unix.socket");

    // Remote: HTTPS mit Client-Zertifikat
    // certPath/keyPath = PEM-Dateien (von generateClientCert() erstellt)
    void setHttps(const QString &host, int port,
                  const QString &certPath, const QString &keyPath);

    Transport transport() const { return m_transport; }
    bool isConfigured() const;

    // ── TLS-Zertifikat für Remote ──────────────────────────────────────────
    // Erstellt ~/.config/llamaqt/client.crt + client.key
    // Gibt den Befehl zurück den der User auf dem Remote-Host ausführen muss:
    //   incus config trust add-certificate ~/.config/llamaqt/client.crt
    bool generateClientCert(QString &outTrustCommand, QString &outError);

    // ── Container-Operationen ──────────────────────────────────────────────

    using Callback      = std::function<void(bool ok, QString error)>;
    using ListCallback  = std::function<void(QVector<IncusContainer> list,
                                              QString error)>;
    using ImagesCallback = std::function<void(QStringList images, QString error)>;

    // Alle Container auflisten
    void listContainers(ListCallback cb);

    // Container anlegen (image z.B. "debian/trixie", "ubuntu/24.04")
    void createContainer(const QString &name, const QString &image, Callback cb);

    // Container starten / stoppen / löschen
    void startContainer (const QString &name, Callback cb);
    void stopContainer  (const QString &name, Callback cb);
    void deleteContainer(const QString &name, Callback cb);

    // Verfügbare lokale Images (aus `incus image list`)
    void listImages(ImagesCallback cb);

    // Datei in Container kopieren (für Deploy)
    // localPath = Datei auf dem Host
    // remotePath = Pfad im Container, z.B. "/tmp/mcp-servers.tar.gz"
    void pushFile(const QString &containerName,
                  const QString &localPath,
                  const QString &remotePath,
                  Callback cb);

    // Befehl im Container ausführen
    // stdout + stderr kommen als combinedOutput zurück
    void exec(const QString &containerName,
              const QStringList &command,
              std::function<void(int exitCode, QString output, QString error)> cb);

signals:
    void connectionError(const QString &msg);

private:
    // ── Interner HTTP-Adapter ──────────────────────────────────────────────
    // Abstrahiert Unix-Socket vs. HTTPS hinter derselben Interface.
    // Intern: QNetworkAccessManager für HTTPS,
    //         eigener Socket-Adapter für Unix.

    void sendRequest(const QString       &method,
                     const QString       &path,
                     const QJsonObject   &body,
                     std::function<void(int httpStatus,
                                        QJsonObject response,
                                        QString     error)> cb);

    // Wartet auf asynchrone Incus-Operation (type=async → /operations/UUID/wait)
    void waitForOperation(const QString &opUrl, Callback cb);

    // Unix Socket: HTTP über QLocalSocket
    void sendUnixRequest(const QString     &method,
                         const QString     &path,
                         const QJsonObject &body,
                         std::function<void(int, QJsonObject, QString)> cb);

    // HTTPS: HTTP über QNetworkAccessManager
    void sendHttpsRequest(const QString     &method,
                          const QString     &path,
                          const QJsonObject &body,
                          std::function<void(int, QJsonObject, QString)> cb);

    IncusContainer parseContainer(const QJsonObject &obj) const;

    Transport               m_transport  = Transport::UnixSocket;
    QString                 m_socketPath = "/var/lib/incus/unix.socket";
    QString                 m_httpsHost;
    int                     m_httpsPort  = 8443;
    QString                 m_certPath;
    QString                 m_keyPath;

    QNetworkAccessManager  *m_nam = nullptr;  // lazy init für HTTPS
};
