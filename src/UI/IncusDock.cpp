#include "UI/IncusDock.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QFile>
#include <QDateTime>
#include <QSettings>

// ─── Konstruktor ─────────────────────────────────────────────────────────────

IncusDock::IncusDock(QWidget *parent)
    : QDockWidget("Incus Container", parent)
    , m_incus(new IncusManager(this))
{
    setObjectName("IncusDock");
    setupUi();

    // Gespeicherte Konfiguration laden
    QSettings s;
    s.beginGroup("Incus");
    const bool useSocket = s.value("use_socket", true).toBool();
    m_radioSocket->setChecked(useSocket);
    m_radioHttps->setChecked(!useSocket);
    m_socketPath->setText(s.value("socket_path",
                           "/var/lib/incus/unix.socket").toString());
    m_httpsHost->setText(s.value("https_host").toString());
    m_httpsPort->setText(s.value("https_port", "8443").toString());
    m_certPath->setText(s.value("cert_path").toString());
    m_enableIncus->setChecked(s.value("enabled", false).toBool());
    m_remoteUrl->setText(s.value("remote_url").toString());
    m_enableFilesystem->setChecked(s.value("feat_filesystem", true).toBool());
    m_enableSysinfo->setChecked(s.value("feat_sysinfo", true).toBool());
    m_enableCompile->setChecked(s.value("feat_compile", true).toBool());
    s.endGroup();

    onTransportChanged();  // UI-Sichtbarkeit initialisieren

    if (m_enableIncus->isChecked())
        applyIncusManagerConfig();
}

// ─── UI Aufbau ────────────────────────────────────────────────────────────────

void IncusDock::setupUi()
{
    QTabWidget *tabs = new QTabWidget;
    tabs->addTab(createSetupTab(),     "Setup");
    tabs->addTab(createContainerTab(), "Container");
    tabs->addTab(createDeployTab(),    "Deploy");
    tabs->addTab(createRemoteTab(),    "Remote");

    QWidget *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->addWidget(tabs);
    setWidget(w);
}

// ─── Setup-Tab ────────────────────────────────────────────────────────────────

QWidget *IncusDock::createSetupTab()
{
    QWidget *w    = new QWidget;
    auto    *vlay = new QVBoxLayout(w);

    // Aktivieren
    m_enableIncus = new QCheckBox("Incus verwenden");
    connect(m_enableIncus, &QCheckBox::toggled, this, [this](bool on) {
        QSettings s; s.beginGroup("Incus");
        s.setValue("enabled", on); s.endGroup();
        if (on) applyIncusManagerConfig();
    });
    vlay->addWidget(m_enableIncus);

    // Status
    m_incusStatus = new QLabel("Status: nicht geprüft");
    m_incusStatus->setStyleSheet("color: gray;");
    vlay->addWidget(m_incusStatus);

    // Pakete installieren
    auto *installGroup = new QGroupBox("Host-Vorbereitung");
    auto *ilay = new QVBoxLayout(installGroup);
    m_installBtn = new QPushButton("incus + incus-extra + virt-viewer installieren");
    connect(m_installBtn, &QPushButton::clicked, this, &IncusDock::onInstallIncus);
    ilay->addWidget(m_installBtn);
    ilay->addWidget(new QLabel("(öffnet Terminal, sudo nötig)"));
    vlay->addWidget(installGroup);

    // Transport
    auto *transGroup = new QGroupBox("Transport");
    auto *tlay = new QVBoxLayout(transGroup);

    m_radioSocket = new QRadioButton("Unix Socket (lokal, empfohlen)");
    m_radioHttps  = new QRadioButton("HTTPS (remote)");
    connect(m_radioSocket, &QRadioButton::toggled,
            this, &IncusDock::onTransportChanged);

    tlay->addWidget(m_radioSocket);

    // Socket-Zeile
    auto *sockRow = new QHBoxLayout;
    sockRow->addWidget(new QLabel("Socket:"));
    m_socketPath = new QLineEdit;
    sockRow->addWidget(m_socketPath);
    auto *browseSock = new QPushButton("...");
    browseSock->setMaximumWidth(30);
    connect(browseSock, &QPushButton::clicked, this, [this]() {
        const QString f = QFileDialog::getOpenFileName(
            this, "Socket-Datei wählen", "/var/lib/incus");
        if (!f.isEmpty()) m_socketPath->setText(f);
    });
    sockRow->addWidget(browseSock);
    tlay->addLayout(sockRow);

    tlay->addWidget(m_radioHttps);

    // HTTPS-Gruppe (wird ausgeblendet bei Socket)
    m_httpsGroup = new QWidget;
    auto *hlay = new QGridLayout(m_httpsGroup);
    hlay->setContentsMargins(16, 0, 0, 0);

    m_httpsHost = new QLineEdit; m_httpsHost->setPlaceholderText("192.168.1.x");
    m_httpsPort = new QLineEdit; m_httpsPort->setPlaceholderText("8443");
    m_httpsPort->setMaximumWidth(80);
    m_certPath  = new QLineEdit; m_certPath->setReadOnly(true);
    m_genCertBtn = new QPushButton("Zertifikat generieren");
    connect(m_genCertBtn, &QPushButton::clicked,
            this, &IncusDock::onGenerateCert);

    m_trustCmdLabel = new QLabel;
    m_trustCmdLabel->setWordWrap(true);
    m_trustCmdLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_trustCmdLabel->setStyleSheet("font-family: monospace; color: #555;");

    hlay->addWidget(new QLabel("Host:"),       0, 0);
    hlay->addWidget(m_httpsHost,               0, 1);
    hlay->addWidget(new QLabel("Port:"),       0, 2);
    hlay->addWidget(m_httpsPort,               0, 3);
    hlay->addWidget(new QLabel("Zertifikat:"), 1, 0);
    hlay->addWidget(m_certPath,                1, 1, 1, 2);
    hlay->addWidget(m_genCertBtn,              1, 3);
    hlay->addWidget(m_trustCmdLabel,           2, 0, 1, 4);
    tlay->addWidget(m_httpsGroup);

    vlay->addWidget(transGroup);

    // Übernehmen
    auto *applyBtn = new QPushButton("Übernehmen");
    connect(applyBtn, &QPushButton::clicked, this, &IncusDock::onApplySetup);
    vlay->addWidget(applyBtn);

    vlay->addStretch();
    return w;
}

// ─── Container-Tab ────────────────────────────────────────────────────────────

QWidget *IncusDock::createContainerTab()
{
    QWidget *w    = new QWidget;
    auto    *vlay = new QVBoxLayout(w);

    // Container-Liste
    m_containerList = new QListWidget;
    connect(m_containerList, &QListWidget::currentRowChanged,
            this, &IncusDock::onContainerSelected);
    vlay->addWidget(m_containerList);

    m_containerStatus = new QLabel("Kein Container ausgewählt");
    m_containerStatus->setStyleSheet("color: gray; font-size: 11px;");
    vlay->addWidget(m_containerStatus);

    // Aktions-Buttons
    auto *btnRow = new QHBoxLayout;
    m_refreshBtn = new QPushButton("Aktualisieren");
    m_startBtn   = new QPushButton("Starten");
    m_stopBtn    = new QPushButton("Stoppen");
    m_deleteBtn  = new QPushButton("Löschen");
    m_startBtn->setEnabled(false);
    m_stopBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);
    connect(m_refreshBtn, &QPushButton::clicked,
            this, &IncusDock::onRefreshContainers);
    connect(m_startBtn,  &QPushButton::clicked,
            this, &IncusDock::onStartContainer);
    connect(m_stopBtn,   &QPushButton::clicked,
            this, &IncusDock::onStopContainer);
    connect(m_deleteBtn, &QPushButton::clicked,
            this, &IncusDock::onDeleteContainer);
    btnRow->addWidget(m_refreshBtn);
    btnRow->addWidget(m_startBtn);
    btnRow->addWidget(m_stopBtn);
    btnRow->addWidget(m_deleteBtn);
    vlay->addLayout(btnRow);

    // Neuen Container anlegen
    auto *createGroup = new QGroupBox("Container anlegen");
    auto *clay = new QGridLayout(createGroup);

    m_newContainerName = new QLineEdit;
    m_newContainerName->setPlaceholderText("z.B. llamaqt-dev");
    m_imageCombo = new QComboBox;
    // Standard-Images — werden nach Verbindung aus Incus geladen
    m_imageCombo->addItems({"debian-trixie", "ubuntu/24.04",
                             "ubuntu/22.04", "debian/bookworm"});

    m_createBtn = new QPushButton("Anlegen");
    connect(m_createBtn, &QPushButton::clicked,
            this, &IncusDock::onCreateContainer);

    clay->addWidget(new QLabel("Name:"),  0, 0);
    clay->addWidget(m_newContainerName,   0, 1);
    clay->addWidget(new QLabel("Image:"), 1, 0);
    clay->addWidget(m_imageCombo,         1, 1);
    clay->addWidget(m_createBtn,          2, 0, 1, 2);
    vlay->addWidget(createGroup);

    return w;
}

// ─── Deploy-Tab ───────────────────────────────────────────────────────────────

QWidget *IncusDock::createDeployTab()
{
    QWidget *w    = new QWidget;
    auto    *vlay = new QVBoxLayout(w);

    m_deployTarget = new QLabel("Kein aktiver Container");
    m_deployTarget->setStyleSheet("font-weight: bold;");
    vlay->addWidget(m_deployTarget);

    m_deployBtn = new QPushButton("MCP-Server deployen");
    m_deployBtn->setEnabled(false);
    connect(m_deployBtn, &QPushButton::clicked, this, &IncusDock::onDeploy);
    vlay->addWidget(m_deployBtn);

    m_deployProgress = new QProgressBar;
    m_deployProgress->setRange(0, 0);  // indeterminate
    m_deployProgress->setVisible(false);
    vlay->addWidget(m_deployProgress);

    vlay->addWidget(new QLabel("Build-Log:"));
    m_deployLog = new QTextEdit;
    m_deployLog->setReadOnly(true);
    m_deployLog->setFont(QFont("Monospace", 9));
    vlay->addWidget(m_deployLog);

    return w;
}

// ─── Remote-Tab ───────────────────────────────────────────────────────────────

QWidget *IncusDock::createRemoteTab()
{
    QWidget *w    = new QWidget;
    auto    *vlay = new QVBoxLayout(w);

    vlay->addWidget(new QLabel("MCP Server URL (SSE):"));
    m_remoteUrl = new QLineEdit;
    m_remoteUrl->setPlaceholderText("http://10.200.0.2:9000");
    vlay->addWidget(m_remoteUrl);

    auto *featGroup = new QGroupBox("Aktive Features");
    auto *flay = new QVBoxLayout(featGroup);
    m_enableFilesystem = new QCheckBox("Filesystem-Tools");
    m_enableSysinfo    = new QCheckBox("Sysinfo-Tools");
    m_enableCompile    = new QCheckBox("Compile-Tools");
    flay->addWidget(m_enableFilesystem);
    flay->addWidget(m_enableSysinfo);
    flay->addWidget(m_enableCompile);
    vlay->addWidget(featGroup);

    m_saveRemoteBtn = new QPushButton("Speichern");
    connect(m_saveRemoteBtn, &QPushButton::clicked,
            this, &IncusDock::onSaveRemoteConfig);
    vlay->addWidget(m_saveRemoteBtn);

    vlay->addStretch();
    return w;
}

// ─── Slot: Setup ──────────────────────────────────────────────────────────────

void IncusDock::onInstallIncus()
{
    // Script in /tmp schreiben und in xterm ausführen
    const QString script = QApplication::applicationDirPath()
                           + "/../scripts/incus-setup.sh";

    if (!QFile::exists(script)) {
        QMessageBox::warning(this, "Script nicht gefunden",
            "incus-setup.sh nicht gefunden.\n"
            "Bitte manuell ausführen:\n"
            "  sudo apt install incus incus-extra virt-viewer");
        return;
    }

    QProcess::startDetached("x-terminal-emulator",
                            {"-e", "bash", script});
}

void IncusDock::onGenerateCert()
{
    QString trustCmd, error;
    if (!m_incus->generateClientCert(trustCmd, error)) {
        QMessageBox::critical(this, "Fehler", error);
        return;
    }
    QSettings s; s.beginGroup("Incus");
    m_certPath->setText(s.value("cert_path").toString());
    s.endGroup();

    m_trustCmdLabel->setText(
        "Auf dem Remote-Host ausführen:\n" + trustCmd);
    QMessageBox::information(this, "Zertifikat erstellt",
        "Zertifikat wurde erstellt.\n\n"
        "Führe auf dem Remote-Host aus:\n\n" + trustCmd);
}

void IncusDock::onTransportChanged()
{
    const bool socket = m_radioSocket->isChecked();
    m_socketPath->setEnabled(socket);
    if (m_httpsGroup) m_httpsGroup->setVisible(!socket);
}

void IncusDock::onApplySetup()
{
    QSettings s; s.beginGroup("Incus");
    s.setValue("use_socket", m_radioSocket->isChecked());
    s.setValue("socket_path", m_socketPath->text());
    s.setValue("https_host", m_httpsHost->text());
    s.setValue("https_port", m_httpsPort->text());
    s.setValue("cert_path",  m_certPath->text());
    s.endGroup();

    applyIncusManagerConfig();
    onRefreshContainers();
}

void IncusDock::applyIncusManagerConfig()
{
    if (m_radioSocket->isChecked()) {
        m_incus->setUnixSocket(m_socketPath->text());
    } else {
        m_incus->setHttps(m_httpsHost->text(),
                          m_httpsPort->text().toInt(),
                          m_certPath->text(), {});
    }

    if (m_incus->isConfigured()) {
        m_incusStatus->setText("Status: Socket vorhanden ✓");
        m_incusStatus->setStyleSheet("color: green;");
    } else {
        m_incusStatus->setText("Status: nicht erreichbar");
        m_incusStatus->setStyleSheet("color: red;");
    }
}

// ─── Slot: Container ──────────────────────────────────────────────────────────

void IncusDock::onRefreshContainers()
{
    m_containerList->clear();
    m_incus->listContainers([this](QVector<IncusContainer> list, QString error) {
        if (!error.isEmpty()) {
            m_containerStatus->setText("Fehler: " + error);
            return;
        }
        m_containers = list;
        for (const auto &c : list) {
            const QString icon = c.isRunning() ? "▶ " : "■ ";
            const QString ip   = c.ipv4.isEmpty() ? "" : "  [" + c.ipv4 + "]";
            m_containerList->addItem(icon + c.name + "  " + c.status + ip);
        }
    });
}

void IncusDock::onContainerSelected(int row)
{
    qDebug() << "onContainerSelected row=" << row
             << "containers.size=" << m_containers.size();


    if (row < 0 || row >= m_containers.size()) {
        m_startBtn->setEnabled(false);
        m_stopBtn->setEnabled(false);
        m_deleteBtn->setEnabled(false);
        return;
    }
    const IncusContainer &c = m_containers[row];
    m_startBtn->setEnabled(c.isStopped());
    m_stopBtn->setEnabled(c.isRunning());
    m_deleteBtn->setEnabled(true);

    m_containerStatus->setText(
        QString("Name: %1  Status: %2  IP: %3  Image: %4")
        .arg(c.name, c.status,
             c.ipv4.isEmpty() ? "-" : c.ipv4,
             c.image.isEmpty() ? "-" : c.image));

    // Als aktiven Container setzen
    m_activeContainer = c.name;
    m_activeIp        = c.ipv4;
    m_deployTarget->setText(
        QString("Container: %1   IP: %2").arg(c.name)
        .arg(c.ipv4.isEmpty() ? "(gestoppt)" : c.ipv4));
    m_deployBtn->setEnabled(c.isRunning());

    if (c.isRunning()) {
        // NEU: Container in AppConfig speichern
        AppConfig::instance().setIncusContainer(c.name);
        AppConfig::instance().setIncusEnabled(true);
        AppConfig::instance().setIncusBinDir("/usr/lib/llamaqt-mcp");
        qDebug() << "emitting activeContainerChanged:" << c.name << c.ipv4;
        emit activeContainerChanged(c.name, c.ipv4);
    }
}

void IncusDock::onCreateContainer()
{
    const QString name  = m_newContainerName->text().trimmed();
    const QString image = m_imageCombo->currentText();

    if (name.isEmpty()) {
        QMessageBox::warning(this, "Name fehlt", "Bitte Container-Name eingeben.");
        return;
    }

    m_createBtn->setEnabled(false);
    m_incus->createContainer(name, image, [this, name](bool ok, QString err) {
        m_createBtn->setEnabled(true);
        if (!ok) {
            QMessageBox::critical(this, "Fehler beim Anlegen", err);
            return;
        }
        m_newContainerName->clear();
        onRefreshContainers();
    });
}

void IncusDock::onStartContainer()
{
    const QString name = selectedContainerName();
    if (name.isEmpty()) return;
    m_incus->startContainer(name, [this](bool ok, QString err) {
        if (!ok) QMessageBox::critical(this, "Fehler", err);
        onRefreshContainers();
    });
}

void IncusDock::onStopContainer()
{
    const QString name = selectedContainerName();
    if (name.isEmpty()) return;
    m_incus->stopContainer(name, [this](bool ok, QString err) {
        if (!ok) QMessageBox::critical(this, "Fehler", err);
        onRefreshContainers();
    });
}

void IncusDock::onDeleteContainer()
{
    const QString name = selectedContainerName();
    if (name.isEmpty()) return;
    if (QMessageBox::question(this, "Löschen",
        QString("Container '%1' wirklich löschen?").arg(name))
        != QMessageBox::Yes) return;

    m_incus->deleteContainer(name, [this](bool ok, QString err) {
        if (!ok) QMessageBox::critical(this, "Fehler", err);
        onRefreshContainers();
    });
}

// ─── Slot: Deploy ─────────────────────────────────────────────────────────────
// Deploy-Ablauf:
//   1. Source-Archiv erstellen (mcp-servers/ aus App-Verzeichnis)
//   2. Archiv per incus file push in Container kopieren
//   3. build-in-container.sh per incus exec ausführen
//   4. Ausgabe live ins Log schreiben

void IncusDock::onDeploy()
{
    if (m_activeContainer.isEmpty()) return;

    m_deployBtn->setEnabled(false);
    m_deployProgress->setVisible(true);
    m_deployLog->clear();

    const QString appDir    = QApplication::applicationDirPath() + "/..";
    const QString archiveTmp = "/tmp/mcp-servers.tar.gz";

    log("=== Deploy Start: " + QDateTime::currentDateTime().toString() + " ===");
    log("Container: " + m_activeContainer);

    // Schritt 1: Archiv erstellen
    log("[1/3] Source-Archiv erstellen...");
    QProcess *tar = new QProcess(this);
    connect(tar, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, tar, archiveTmp, appDir](int exitCode, QProcess::ExitStatus) {
        log(QString::fromLocal8Bit(tar->readAllStandardError()));
        tar->deleteLater();

        if (exitCode != 0) {
            log("FEHLER: tar fehlgeschlagen");
            m_deployBtn->setEnabled(true);
            m_deployProgress->setVisible(false);
            return;
        }

        // Schritt 2: push
        log("[2/3] Archiv in Container kopieren...");
        m_incus->pushFile(m_activeContainer, archiveTmp, "/tmp/mcp-servers.tar.gz",
                          [this, appDir](bool ok, QString err) {
            if (!ok) {
                log("FEHLER beim Kopieren: " + err);
                m_deployBtn->setEnabled(true);
                m_deployProgress->setVisible(false);
                return;
            }

            // Schritt 3: build-in-container.sh kopieren und ausführen
            log("[3/3] Build im Container starten...");
            m_incus->pushFile(m_activeContainer,
                              appDir + "/scripts/build-in-container.sh",
                              "/tmp/build-in-container.sh",
                              [this](bool ok2, QString err2) {
                if (!ok2) {
                    log("FEHLER beim Script-Kopieren: " + err2);
                    m_deployBtn->setEnabled(true);
                    m_deployProgress->setVisible(false);
                    return;
                }

                m_incus->exec(m_activeContainer,
                              {"bash", "/tmp/build-in-container.sh"},
                              [this](int exitCode, QString out, QString err) {
                    log(out);
                    if (!err.isEmpty()) log("STDERR: " + err);
                    if (exitCode == 0) {
                        log("=== Deploy erfolgreich ===");
                        // Remote-URL automatisch setzen
                        if (!m_activeIp.isEmpty()) {
                            m_remoteUrl->setText(
                                "http://" + m_activeIp + ":9000");
                        }
                    } else {
                        log("=== Deploy fehlgeschlagen (exit " +
                            QString::number(exitCode) + ") ===");
                    }
                    m_deployBtn->setEnabled(true);
                    m_deployProgress->setVisible(false);
                });
            });
        });
    });

    tar->start("tar", {"czf", archiveTmp, "-C", appDir, "mcp-servers/"});
}

// ─── Slot: Remote Config ──────────────────────────────────────────────────────

void IncusDock::onSaveRemoteConfig()
{
    QSettings s; s.beginGroup("Incus");
    s.setValue("remote_url",      m_remoteUrl->text());
    s.setValue("feat_filesystem", m_enableFilesystem->isChecked());
    s.setValue("feat_sysinfo",    m_enableSysinfo->isChecked());
    s.setValue("feat_compile",    m_enableCompile->isChecked());
    s.endGroup();
}

// ─── Hilfsfunktionen ─────────────────────────────────────────────────────────

void IncusDock::log(const QString &msg)
{
    m_deployLog->append(msg);
}

QString IncusDock::selectedContainerName() const
{
    const int row = m_containerList->currentRow();
    if (row < 0 || row >= m_containers.size()) return {};
    return m_containers[row].name;
}
