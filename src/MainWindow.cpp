#include "MainWindow.h"
#include "ui_MainWindow.h"
#include <QScrollBar>
#include <QTextCursor>
#include <QKeyEvent>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_agent(new Agent(MODEL_PATH, this))
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

    addDockWidget(Qt::RightDockWidgetArea, ui->toolDock);
    ui->toolDock->setMinimumWidth(350);

    // ─── chatView ──────────────────────────────────────────────────────────
    ui->chatView->document()->setDefaultStyleSheet(R"(
        body       { font-family: 'Noto Sans', sans-serif; font-size: 13px; }
        .user      { color: #1a73e8; margin: 6px 0; }
        .assistant { color: #202124; margin: 6px 0; }
        .error     { color: #c5221f; }
        .system    { color: #888; font-style: italic; font-size: 11px; }
        b          { font-weight: 600; }
    )");

    // chatView bricht bereits um (WidgetWidth ist Default) — explizit setzen
    ui->chatView->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    // ─── toolView: Word-Wrap ───────────────────────────────────────────────
    // Problem: toolView zeigt lange JSON-Blöcke, Tool-Ergebnisse und
    // Thinking-Text. Diese brechen ohne Word-Wrap nicht um — horizontales
    // Scrollen ist sehr unkomfortabel.
    //
    // Lösung: WrapAtWordBoundaryOrAnywhere
    //   - Bricht an Wortgrenzen um wenn möglich
    //   - Bricht notfalls auch mitten im Wort/Token (wichtig für JSON ohne Spaces)
    //   - Kein harter Umbruch im gespeicherten Text — nur visuell
    //
    // Warum nicht WrapAnywhere allein?
    //   WrapAnywhere bricht überall, auch mitten in gut lesbaren Wörtern.
    //   WrapAtWordBoundaryOrAnywhere ist der Kompromiss.
    //
    // Warum nicht CSS word-wrap:break-word?
    //   Qt's QTextEdit rendert HTML aber ignoriert CSS word-wrap in QTextDocument.
    //   setWordWrapMode() ist die Qt-native Lösung.
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

    // ─── inputEdit: QTextEdit statt QLineEdit ─────────────────────────────
    // QTextEdit erlaubt Shift+Enter für Zeilenumbrüche in der Eingabe.
    // Enter allein sendet (wird in eventFilter / setupConnections abgefangen).
    // Feste Höhe: 3 Zeilen, wächst nicht (kein Layout-Shift).
    ui->inputEdit->setFixedHeight(66);  // ca. 3 Zeilen bei 13px Font
    ui->inputEdit->setPlaceholderText(
        "Nachricht eingeben... (Enter = Senden, Shift+Enter = Zeilenumbruch)");
    ui->inputEdit->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    ui->inputEdit->setAcceptRichText(false);  // nur Plain-Text

    // installEventFilter damit wir Enter/Shift+Enter abfangen
    ui->inputEdit->installEventFilter(this);

    ui->statusLabel->setStyleSheet("color: #888; font-size: 11px;");
    ui->statsLabel->setStyleSheet(
        "color: #555; font-size: 11px; font-family: monospace;");
}

// ─── eventFilter ─────────────────────────────────────────────────────────────
// Abfangen von Enter und Shift+Enter im inputEdit.
//
// Pattern: Interceptor / Decorator für Qt-Events.
//
// Enter ohne Modifier → Senden
// Enter + Shift       → normaler Zeilenumbruch (QTextEdit-Default)
// Alle anderen Keys   → normal weitergeben
//
// Warum eventFilter statt subclassing?
//   Wir nutzen Qt Designer (.ui Datei) — eigene QTextEdit-Subklasse würde
//   "promoted widget" in Designer erfordern. eventFilter ist einfacher.
bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->inputEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);
        bool shiftHeld = key->modifiers() & Qt::ShiftModifier;

        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (!shiftHeld) {
                // Enter ohne Shift → Senden
                onSendClicked();
                return true;  // Event nicht weiterleiten (kein Zeilenumbruch)
            }
            // Shift+Enter → QTextEdit behandelt es normal (Zeilenumbruch)
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
}

// ═════════════════════════════════════════════════════════════════════════════
// HILFSFUNKTIONEN: Smart-Autoscroll
// ═════════════════════════════════════════════════════════════════════════════

// ─── isScrolledToBottom ──────────────────────────────────────────────────────
// Prüft ob der Scrollbar "am Ende" ist — mit AUTOSCROLL_THRESHOLD Pixel Puffer.
//
// Warum ein Schwellwert?
//   Qt kann den Scrollbar-Maximum-Wert asynchron aktualisieren während
//   Text eingefügt wird. Ein harter == Vergleich würde manchmal fälschlich
//   "nicht am Ende" melden. 20px Puffer fängt das ab.
bool MainWindow::isScrolledToBottom(QScrollBar *sb) const
{
    return (sb->maximum() - sb->value()) <= AUTOSCROLL_THRESHOLD;
}

// ─── scrollToBottomIfNeeded ──────────────────────────────────────────────────
// Scrollt ans Ende — aber nur wenn der Nutzer nicht aktiv hochgescrollt hat.
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
    // toPlainText() weil inputEdit kein RichText akzeptiert
    QString text = ui->inputEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;
    ui->inputEdit->clear();
    m_agent->onUserMessage(text);
}

void MainWindow::onClearToolsClicked()
{
    ui->toolView->clear();
}

// ═════════════════════════════════════════════════════════════════════════════
// SLOTS: Agent → UI Darstellung
// ═════════════════════════════════════════════════════════════════════════════

// ─── onAppendChat ────────────────────────────────────────────────────────────
void MainWindow::onAppendChat(const QString &html, const QString &cssClass)
{
    QScrollBar *sb = ui->chatView->verticalScrollBar();
    bool wasAtBottom = isScrolledToBottom(sb);

    ui->chatView->append(
        QString(R"(<p class="%1">%2</p>)").arg(cssClass, html));

    // Smart-Autoscroll: nur wenn vorher am Ende
    if (wasAtBottom)
        sb->setValue(sb->maximum());
}

// ─── onAppendChatToken ───────────────────────────────────────────────────────
// QTextCursor::insertText() — kein neuer Absatz, in laufenden Block
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

// ─── onAppendTools ───────────────────────────────────────────────────────────
void MainWindow::onAppendTools(const QString &html, const QString &cssClass)
{
    QScrollBar *sb = ui->toolView->verticalScrollBar();
    bool wasAtBottom = isScrolledToBottom(sb);

    ui->toolView->append(
        QString(R"(<p class="%1">%2</p>)").arg(cssClass, html));

    if (wasAtBottom)
        sb->setValue(sb->maximum());
}

// ─── onInputEnabled ──────────────────────────────────────────────────────────
void MainWindow::onInputEnabled(bool enabled)
{
    ui->inputEdit->setEnabled(enabled);
    ui->sendButton->setEnabled(enabled);
    ui->stopButton->setEnabled(!enabled);

    if (enabled)
        ui->inputEdit->setFocus();
}

// ─── onStatsUpdated ──────────────────────────────────────────────────────────
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
