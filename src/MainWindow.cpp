#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "AppConfig.h"
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

// ─── setupUi ─────────────────────────────────────────────────────────────────
void MainWindow::setupUi()
{
    ui->setupUi(this);

    // ─── Tool-Dock (rechts) ───────────────────────────────────────────────
    addDockWidget(Qt::RightDockWidgetArea, ui->toolDock);
    ui->toolDock->setMinimumWidth(350);

    // ─── Editor-Dock (unten) ──────────────────────────────────────────────
    m_editorDock = new EditorDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_editorDock);
    m_editorDock->hide();

    // ─── Planner-Dock (links) ─────────────────────────────────────────────
    // Zeigt den TaskTree + Approval-Buttons im Plan-Modus.
    // Initial ausgeblendet — wird durch Agent::modeChanged(Plan) eingeblendet.
    //
    // Warum links?
    //   - Rechts ist toolDock (Tool-Calls, Thinking)
    //   - Unten ist editorDock (Code-Editor)
    //   - Links ist noch frei → natürliche Position für Navigation/Plan
    //
    // Warum nicht tabbedDock mit toolDock?
    //   Plan und Tool-Calls sollen gleichzeitig sichtbar sein:
    //   User sieht links den Plan-Tree, rechts die laufenden Tool-Calls.
    m_plannerDock = new PlannerDock(m_agent, this);
    addDockWidget(Qt::LeftDockWidgetArea, m_plannerDock);
    m_plannerDock->hide();   // initial ausgeblendet

    // ─── Ansicht-Menü: alle Docks togglebar ───────────────────────────────
    QMenu *viewMenu = menuBar()->addMenu("&Ansicht");
    viewMenu->addAction(m_editorDock->toggleViewAction());
    viewMenu->addAction(m_plannerDock->toggleViewAction());
    // toggleViewAction() liefert eine QAction die den Dock ein-/ausblendet.
    // Qt erstellt sie automatisch für jeden QDockWidget.

    // ─── chatView ──────────────────────────────────────────────────────────
    ui->chatView->document()->setDefaultStyleSheet(R"(
        body       { font-family: 'Noto Sans', sans-serif; font-size: 13px; }
        .user      { color: #1a73e8; margin: 6px 0; }
        .assistant { color: #202124; margin: 6px 0; }
        .error     { color: #c5221f; }
        .system    { color: #888; font-style: italic; font-size: 11px; }
        b          { font-weight: 600; }
    )");
    ui->chatView->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    // ─── toolView ─────────────────────────────────────────────────────────
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

    // ─── inputEdit ────────────────────────────────────────────────────────
    ui->inputEdit->setFixedHeight(66);
    ui->inputEdit->setPlaceholderText(
        "Nachricht eingeben... (Enter = Senden, Shift+Enter = Zeilenumbruch)");
    ui->inputEdit->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    ui->inputEdit->setAcceptRichText(false);
    ui->inputEdit->installEventFilter(this);

    ui->statusLabel->setStyleSheet("color: #888; font-size: 11px;");
    ui->statsLabel->setStyleSheet(
        "color: #555; font-size: 11px; font-family: monospace;");

    // ─── Menü-Bar ─────────────────────────────────────────────────────────
    QMenu *fileMenu = menuBar()->addMenu("&Datei");

    QAction *settingsAction = fileMenu->addAction("⚙ Einstellungen...");
    settingsAction->setShortcut(QKeySequence("Ctrl+,"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettingsClicked);

    fileMenu->addSeparator();

    QAction *quitAction = fileMenu->addAction("Beenden");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QMainWindow::close);

    // NodeGraphView — initial versteckt, togglebar über Ansicht-Menü
    m_graphView = new NodeGraphView(this);
    addDockWidget(Qt::RightDockWidgetArea, m_graphView);
    //m_graphView->hide();

    connect(m_graphView, &QDockWidget::visibilityChanged,
            this, [this](bool visible) {
                if (visible)
                    m_graphView->refresh(m_agent->taskTree());
            });


    // Im Ansicht-Menü eintragen
    viewMenu->addAction(m_graphView->toggleViewAction());

    // Graph aktualisieren wenn TaskTree sich ändert
    connect(m_agent, &Agent::taskTreeUpdated,
            this,    &MainWindow::onRefreshGraph);

    // Node selektiert → PlannerDock synchronisieren (optional)
    connect(m_graphView, &NodeGraphView::nodeSelected,
            this, [this](qint64 nodeId) {
                // TODO: PlannerDock auf diesen Node scrollen
                Q_UNUSED(nodeId)
            });

    // ─── SearchBar ────────────────────────────────────────────────────────────
    // Eine SearchBar Instanz für alle TextEdits.
    // attachTo() wechselt den aktiven TextEdit.
    m_searchBar = new SearchBar(ui->centralWidget);

    // In das zentralWidget-Layout einbauen — ÜBER dem inputEdit.
    // Layout ist QVBoxLayout (mainLayout in setupUi):
    //   chatView → statusLabel → searchBar (NEU) → inputLayout
    // Suche den Einfügepunkt: vor dem inputLayout.
    auto *centralLayout = qobject_cast<QVBoxLayout*>(
        ui->centralWidget->layout());
    if (centralLayout) {
        // Index des inputLayouts finden (letztes Item)
        int insertIdx = centralLayout->count() - 1;
        centralLayout->insertWidget(insertIdx, m_searchBar);
    }

    // Ctrl+F — kontextsensitiv je nach aktivem Widget
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated,
            this, &MainWindow::onSearchRequested);
}

// ─── eventFilter ─────────────────────────────────────────────────────────────
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

// ─── setupConnections ────────────────────────────────────────────────────────
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

    // ─── Modus-Änderung ────────────────────────────────────────────────────
    connect(m_agent, &Agent::modeChanged,
            this,    &MainWindow::onModeChanged);

    // ─── EditorDock ───────────────────────────────────────────────────────
    connect(m_editorDock, &EditorDock::fileSavedByUser,
            m_agent,      &Agent::onFileSavedByUser);

    connect(m_editorDock, &EditorDock::searchRequested,
             this, &MainWindow::onSearchRequested);
}

// ═════════════════════════════════════════════════════════════════════════════
// HILFSFUNKTIONEN: Smart-Autoscroll
// ═════════════════════════════════════════════════════════════════════════════

bool MainWindow::isScrolledToBottom(QScrollBar *sb) const
{
    return (sb->maximum() - sb->value()) <= AUTOSCROLL_THRESHOLD;
}

void MainWindow::scrollToBottomIfNeeded(QScrollBar *sb)
{
    if (isScrolledToBottom(sb))
        sb->setValue(sb->maximum());
}

// ═════════════════════════════════════════════════════════════════════════════
// SLOTS: UI-Events
// ═════════════════════════════════════════════════════════════════════════════

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

// ─── onModeChanged ───────────────────────────────────────────────────────────
// Reagiert auf Modus-Wechsel des Agent.
// Plan-Modus → PlannerDock einblenden
// Chat-Modus → PlannerDock ausblenden (optional: User kann es offen lassen)
//
// Warum nur einblenden, nicht erzwungen ausblenden?
//   User könnte den Plan-Tree auch nach der Approval noch sehen wollen
//   (z.B. um den bestätigten Plan nachzulesen).
//   Wir blenden nur im Plan-Modus automatisch ein.
void MainWindow::onModeChanged(AgentMode mode)
{
    if (mode == AgentMode::Plan) {
        m_plannerDock->show();
        m_plannerDock->raise();
    }
    // Bei Chat/Execute: Dock bleibt wie es ist (User entscheidet)
}

void MainWindow::onRefreshGraph()
{
    if (m_graphView && m_graphView->isVisible())
        m_graphView->refresh(m_agent->taskTree());
}

void MainWindow::onSearchRequested()
{
    // Kontextsensitiv: wählt den aktiven TextEdit.
    // Priorität: EditorDock > toolView > chatView
    //
    // Warum diese Reihenfolge?
    //   EditorDock ist der primäre Code-Editor — dort sucht man am häufigsten.
    //   toolView zeigt Tool-Ergebnisse — auch sinnvoll zu durchsuchen.
    //   chatView ist der Chat — Fallback.

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

// ═════════════════════════════════════════════════════════════════════════════
// SLOTS: Agent → UI Darstellung
// ═════════════════════════════════════════════════════════════════════════════

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

    // Stop-Button: aktiv wenn generiert wird ODER im Execute/Plan-Modus
    // (zwischen zwei Nodes ist m_generating kurz false, aber der Modus bleibt)
    bool agentBusy = (m_agent->mode() == AgentMode::Execute ||
                      m_agent->mode() == AgentMode::Plan);
    ui->stopButton->setEnabled(!enabled || agentBusy);

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
