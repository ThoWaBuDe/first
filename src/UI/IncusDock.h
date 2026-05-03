#pragma once
#include <QDockWidget>
#include <QTabWidget>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QListWidget>
#include <QTextEdit>
#include <QProgressBar>
#include <QGroupBox>
#include <QRadioButton>
#include "Incus/IncusManager.h"
#include "Incus/IncusContainer.h"
#include "Config/AppConfig.h"

// ─── IncusDock ────────────────────────────────────────────────────────────────
// QDockWidget mit 4 Tabs für die gesamte Incus-Integration.
//
// Tabs:
//   [Setup]     — Incus aktivieren, Pakete installieren, Transport wählen
//   [Container] — Container anlegen, starten, stoppen, löschen
//   [Deploy]    — MCP-Server bauen und deployen
//   [Remote]    — Remote MCP Config, Features an/aus
//
// Pattern: Presenter — IncusDock kennt IncusManager,
//          aber IncusManager kennt IncusDock nicht.
//          Kommunikation nur über Callbacks und Signals.

class IncusDock : public QDockWidget {
    Q_OBJECT

public:
    explicit IncusDock(QWidget *parent = nullptr);

signals:
    // Container-Status hat sich geändert → McpManager neu konfigurieren
    void activeContainerChanged(const QString &containerName,
                                const QString &ip);

private slots:
    // Setup-Tab
    void onInstallIncus();
    void onGenerateCert();
    void onTransportChanged();
    void onApplySetup();

    // Container-Tab
    void onRefreshContainers();
    void onCreateContainer();
    void onStartContainer();
    void onStopContainer();
    void onDeleteContainer();
    void onContainerSelected(int row);

    // Deploy-Tab
    void onDeploy();

    // Remote-Tab
    void onSaveRemoteConfig();

private:
    void setupUi();
    QWidget *createSetupTab();
    QWidget *createContainerTab();
    QWidget *createDeployTab();
    QWidget *createRemoteTab();

    void log(const QString &msg);
    void applyIncusManagerConfig();
    QString selectedContainerName() const;

    // ── Manager ───────────────────────────────────────────────────────────
    IncusManager *m_incus;

    // ── Setup-Tab ─────────────────────────────────────────────────────────
    QCheckBox    *m_enableIncus;
    QLabel       *m_incusStatus;
    QPushButton  *m_installBtn;
    QRadioButton *m_radioSocket;
    QRadioButton *m_radioHttps;
    QLineEdit    *m_socketPath;
    QWidget      *m_httpsGroup;
    QLineEdit    *m_httpsHost;
    QLineEdit    *m_httpsPort;
    QLineEdit    *m_certPath;
    QPushButton  *m_genCertBtn;
    QLabel       *m_trustCmdLabel;   // zeigt den incus trust add Befehl

    // ── Container-Tab ─────────────────────────────────────────────────────
    QListWidget  *m_containerList;
    QLabel       *m_containerStatus;
    QPushButton  *m_refreshBtn;
    QPushButton  *m_createBtn;
    QPushButton  *m_startBtn;
    QPushButton  *m_stopBtn;
    QPushButton  *m_deleteBtn;
    QComboBox    *m_imageCombo;
    QLineEdit    *m_newContainerName;

    // ── Deploy-Tab ────────────────────────────────────────────────────────
    QLabel       *m_deployTarget;   // "Container: llamaqt-dev  IP: 10.0.0.2"
    QPushButton  *m_deployBtn;
    QTextEdit    *m_deployLog;
    QProgressBar *m_deployProgress;

    // ── Remote-Tab ────────────────────────────────────────────────────────
    QCheckBox    *m_enableFilesystem;
    QCheckBox    *m_enableSysinfo;
    QCheckBox    *m_enableCompile;
    QLineEdit    *m_remoteUrl;      // z.B. "http://10.200.0.2:9000"
    QPushButton  *m_saveRemoteBtn;

    // Gemeinsames Log (unten im Deploy-Tab)
    QVector<IncusContainer> m_containers;
    QString                  m_activeContainer;
    QString                  m_activeIp;
};
