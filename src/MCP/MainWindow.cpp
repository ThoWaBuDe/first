#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "Config/AppConfig.h"
#include <QScrollBar>
#include <QTextCursor>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QMetaObject>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_agent(new Agent(AppConfig::instance().modelPath(), this))
{
    setupUi();
    setupConnections();
    m_agent->start();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setupUi()
{
    ui->setupUi(this);

    addDockWidget(Qt::RightDockWidgetArea, ui->toolDock);
    ui->toolDock->setMinimumWidth(350);

    m_editorDock = new EditorDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_editorDock);
    m_editorDock->hide();

    m_incusDock = new IncusDock(this);
    addDockWidget(Qt::RightDockWidgetArea, m_incusDock);
    m_incusDock->hide();

    QMenu *viewMenu = menuBar()->addMenu("&Ansicht");
    viewMenu->addAction(m_editorDock->toggleViewAction());
    viewMenu->addAction(m_incusDock->toggleViewAction());

    ui->chatView->document()->setDefaultStyleSheet(R"(
        body       { font-family: 'Noto Sans', sans-serif; font-size: 13px; }
        .user      { color: #1a73e8; margin: 6px 0; }
        .assistant { color: #202124; margin: 6px 0; }
        .error     { color: #c5221f; }
        .system    { color: #888; font-style: italic; font-size: 11px; }
        b          { font-weight: 600; }
    )");
    ui->chatView->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    ui->toolView->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    ui->toolView->document()->setDefaultStyleSheet(R"(
        body   { font-family: monospace; font-size: 11px; }
        .tool  { color: #188038; background: #f1f8f4;
                 padding: 4px; border-radius: 3px; margin: 4px 0; }
        .think { color: #7b5ea7; background: #f5f0fa;
                 padding: 4px; border-radius: 3px; margin: 4px 0;
                 font-style: italic; }
        .stats { color: #555; font-size: 10px; }
        .error { color: #c5221f; }
        .system{ color: #888; font-size: 10px; font-style: italic; }
        b      { font-weight: 600; }
        pre    { white-space: pre-wrap; word-wrap: break-word;
                 margin: 2px 0; }
    )");

    ui->inputEdit->setFixedHeight(66);
    ui->inputEdit->setPlaceholderText(
        "Nachricht eingeben... (Enter = Senden, Shift+Enter = Zeilenumbruch)");
    ui->inputEdit->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    ui->inputEdit->setAcceptRichText(false);
    ui->inputEdit->installEventFilter(this);

    ui->statusLabel->setStyleSheet("color: #888; font-size: 11px;");
    ui->statsLabel->setStyleSheet(
        "color: #555; font-size: 11px; font-family: monospace;");

    QMenu *fileMenu = menuBar()->addMenu("&Datei");

    QAction *settingsAction = fileMenu->addAction("⚙ Einstellungen...");
    settingsAction->setShortcut(QKeySequence("Ctrl+,"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettingsClicked);

    fileMenu->addSeparator();

    QAction *quitAction = fileMenu->addAction("Beenden");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QMainWindow::close);

    m_searchBar = new SearchBar(ui->centralWidget);

    auto *centralLayout = qobject_cast<QVBoxLayout*>(
        ui->centralWidget->layout());
    if (centralLayout) {
        int insertIdx = centralLayout->count() - 1;
        centralLayout->insertWidget(insertIdx, m_searchBar);
    }

    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated,
            this, &MainWindow::onSearchRequested);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->inputEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);
        bool shiftHeld = key->modifiers() & Qt::ShiftModifier;
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (!shiftHeld) {
                onSendClicked();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::setupConnections()
{
    // ─── UI → Agent ───────────────────────────────────────────────────────
    connect(ui->sendButton,  &QPushButton::clicked,
            this,            &MainWindow::onSendClicked);
    connect(ui->stopButton,  &QPushButton::clicked,
            m_agent,         &Agent::onStop);
    connect(ui->clearButton, &QPushButton::clicked,
            m_agent,         &Agent::onClearChat);
    connect(ui->clearToolsButton, &QPushButton::clicked,
            this,            &MainWindow::onClearToolsClicked);

    // ─── Agent → UI ───────────────────────────────────────────────────────
    connect(m_agent, &Agent::appendChat,
            this,    &MainWindow::onAppendChat);
    connect(m_agent, &Agent::appendChatToken,
            this,    &MainWindow::onAppendChatToken);
    connect(m_agent, &Agent::appendTools,
            this,    &MainWindow::onAppendTools);
    connect(m_agent,         &Agent::statusChanged,
            ui->statusLabel, &QLabel::setText);
    connect(m_agent, &Agent::inputEnabled,
            this,    &MainWindow::onInputEnabled);
    connect(m_agent, &Agent::statsUpdated,
            this,    &MainWindow::onStatsUpdated);

    // ─── EditorDock ───────────────────────────────────────────────────────
    connect(m_editorDock, &EditorDock::fileSavedByUser,
            m_agent,      &Agent::onFileSavedByUser);

    connect(m_editorDock, &EditorDock::searchRequested,
             this, &MainWindow::onSearchRequested);

    // ─── IncusDock ────────────────────────────────────────────────────────
    // Wenn der User einen laufenden Container auswählt, startet Agent
    // die MCP-Server neu — diesmal via "incus exec <container> -- <binary>".
    connect(m_incusDock, &IncusDock::activeContainerChanged,
            m_agent,     &Agent::onIncusContainerChanged);
}

bool MainWindow::isScrolledToBottom(QScrollBar *sb) const
{
    return (sb->maximum() - sb->value()) <= AUTOSCROLL_THRESHOLD;
}

void MainWindow::scrollToBottomIfNeeded(QScrollBar *sb)
{
    if (isScrolledToBottom(sb))
        sb->setValue(sb->maximum());
}

void MainWindow::onSendClicked()
{
    QString text = ui->inputEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;
    ui->inputEdit->clear();
    m_agent->onUserMessage(text);
}

void MainWindow::onClearToolsClicked()
{
    ui->toolView->clear();
}

void MainWindow::onSearchRequested()
{
    QPlainTextEdit *editor = m_editorDock->currentEditor();
    if (m_editorDock->isVisible() && editor) {
        m_searchBar->attachTo(editor);
    } else if (m_editorDock->isVisible()) {
        m_searchBar->attachTo(ui->toolView);
    } else {
        m_searchBar->attachTo(ui->chatView);
    }
    m_searchBar->openBar();
}

void MainWindow::onAppendChat(const QString &html, const QString &cssClass)
{
    QScrollBar *sb = ui->chatView->verticalScrollBar();
    bool wasAtBottom = isScrolledToBottom(sb);
    ui->chatView->append(
        QString(R"(<p class="%1">%2</p>)").arg(cssClass, html));
    if (wasAtBottom)
        sb->setValue(sb->maximum());
}

void MainWindow::onAppendChatToken(const QString &text)
{
    QScrollBar *sb = ui->chatView->verticalScrollBar();
    bool wasAtBottom = isScrolledToBottom(sb);
    QTextCursor cursor = ui->chatView->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    if (wasAtBottom)
        sb->setValue(sb->maximum());
}

void MainWindow::onAppendTools(const QString &html, const QString &cssClass)
{
    QScrollBar *sb = ui->toolView->verticalScrollBar();
    bool wasAtBottom = isScrolledToBottom(sb);
    ui->toolView->append(
        QString(R"(<p class="%1">%2</p>)").arg(cssClass, html));
    if (wasAtBottom)
        sb->setValue(sb->maximum());
}

void MainWindow::onInputEnabled(bool enabled)
{
    ui->inputEdit->setEnabled(enabled);
    ui->sendButton->setEnabled(enabled);
    ui->stopButton->setEnabled(!enabled);

    if (enabled)
        ui->inputEdit->setFocus();
}

void MainWindow::onStatsUpdated(int promptTokens, int generatedTokens,
                                 int totalTokens,  int ctxSize)
{
    int used = promptTokens + generatedTokens;
    int pct  = (ctxSize > 0) ? (used * 100 / ctxSize) : 0;

    QString color = "#188038";
    if (pct > 60) color = "#e37400";
    if (pct > 85) color = "#c5221f";

    ui->statsLabel->setText(
        QString("Prompt: %1 | Gen: %2 | Gesamt: %3 / %4 (%5%) | Total: %6")
        .arg(promptTokens).arg(generatedTokens)
        .arg(used).arg(ctxSize).arg(pct)
        .arg(totalTokens));
    ui->statsLabel->setStyleSheet(
        QString("color: %1; font-size: 11px; font-family: monospace;")
        .arg(color));
}

void MainWindow::onSettingsClicked()
{
    ConfigDialog dlg(this);

    connect(&dlg, &ConfigDialog::samplersChanged, this, [this]() {
        QMetaObject::invokeMethod(m_agent->worker(), "rebuildSamplers",
                                  Qt::QueuedConnection);
        emit m_agent->appendTools(
            "Sampler neu gebaut mit aktuellen Einstellungen.", "system");
    });

    dlg.exec();
}
