#include "MainWindow.h"
#include "ui_MainWindow.h"
#include <QScrollBar>
#include <QTextCursor>

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

    ui->chatView->document()->setDefaultStyleSheet(R"(
        body       { font-family: 'Noto Sans', sans-serif; font-size: 13px; }
        .user      { color: #1a73e8; margin: 6px 0; }
        .assistant { color: #202124; margin: 6px 0; }
        .error     { color: #c5221f; }
        .system    { color: #888; font-style: italic; font-size: 11px; }
        b          { font-weight: 600; }
    )");

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
    )");

    ui->statusLabel->setStyleSheet("color: #888; font-size: 11px;");
    ui->statsLabel->setStyleSheet(
        "color: #555; font-size: 11px; font-family: monospace;");
}

// ─── setupConnections ────────────────────────────────────────────────────────
// Alle Signal/Slot-Verbindungen an einem Ort.
// Architektur auf einen Blick lesbar.
void MainWindow::setupConnections()
{
    // ─── UI → Agent ───────────────────────────────────────────────────────
    connect(ui->sendButton,  &QPushButton::clicked,
            this,            &MainWindow::onSendClicked);
    connect(ui->inputLine,   &QLineEdit::returnPressed,
            this,            &MainWindow::onSendClicked);
    connect(ui->stopButton,  &QPushButton::clicked,
            m_agent,         &Agent::onStop);
    connect(ui->clearButton, &QPushButton::clicked,
            m_agent,         &Agent::onClearChat);
    connect(ui->clearToolsButton, &QPushButton::clicked,
            this,            &MainWindow::onClearToolsClicked);

    // ─── Agent → UI ───────────────────────────────────────────────────────

    // Neuer Block in chatView (öffnet <p>)
    connect(m_agent, &Agent::appendChat,
            this,    &MainWindow::onAppendChat);

    // Token in laufenden Block einfügen (kein neues <p>)
    connect(m_agent, &Agent::appendChatToken,
            this,    &MainWindow::onAppendChatToken);

    connect(m_agent, &Agent::appendTools,
            this,    &MainWindow::onAppendTools);

    // statusChanged direkt ans Label — kein eigener Slot nötig
    connect(m_agent,         &Agent::statusChanged,
            ui->statusLabel, &QLabel::setText);

    connect(m_agent, &Agent::inputEnabled,
            this,    &MainWindow::onInputEnabled);

    connect(m_agent, &Agent::statsUpdated,
            this,    &MainWindow::onStatsUpdated);
}

// ═════════════════════════════════════════════════════════════════════════════
// SLOTS: UI-Events
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::onSendClicked()
{
    QString text = ui->inputLine->text().trimmed();
    if (text.isEmpty()) return;
    ui->inputLine->clear();
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
// Öffnet einen neuen <p>-Block in chatView.
// QTextEdit::append() erzeugt intern einen neuen QTextBlock (Absatz).
// Nach diesem Aufruf zeigt der Cursor ans Ende — onAppendChatToken
// schreibt genau dort hinein.
void MainWindow::onAppendChat(const QString &html, const QString &cssClass)
{
    ui->chatView->append(
        QString(R"(<p class="%1">%2</p>)").arg(cssClass, html));

    QScrollBar *sb = ui->chatView->verticalScrollBar();
    sb->setValue(sb->maximum());
}

// ─── onAppendChatToken ───────────────────────────────────────────────────────
// Hängt einen Token an den LAUFENDEN Absatz an — ohne neuen <p>-Block.
//
// Warum QTextCursor statt append()?
//   append() ruft intern QTextEdit::insertHtml() mit einem neuen <p>-Block.
//   Das erzeugt für jeden Token einen eigenen Absatz → Tokens in Einzelzeilen.
//
//   QTextCursor::movePosition(End) positioniert den Cursor ans Dokumentende,
//   also ans Ende des letzten <p>-Blocks der von onAppendChat() geöffnet wurde.
//   insertText() schreibt dort hinein — im laufenden Absatz, kein Zeilenumbruch.
//
// Warum insertText() statt insertHtml()?
//   Tokens sind reiner Text (Wörter, Satzzeichen, Leerzeichen).
//   insertHtml() würde HTML-Entities escapen müssen und ist langsamer.
//   insertText() ist die direkte, schnelle Variante für Plain-Text.
//
// Analogie AVR: wie UART_putc() — schreibt ein Zeichen ohne Zeilenumbruch,
// im Gegensatz zu UART_puts() mit \n am Ende.
void MainWindow::onAppendChatToken(const QString &text)
{
    QTextCursor cursor = ui->chatView->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);

    QScrollBar *sb = ui->chatView->verticalScrollBar();
    sb->setValue(sb->maximum());
}

// ─── onAppendTools ───────────────────────────────────────────────────────────
void MainWindow::onAppendTools(const QString &html, const QString &cssClass)
{
    ui->toolView->append(
        QString(R"(<p class="%1">%2</p>)").arg(cssClass, html));

    QScrollBar *sb = ui->toolView->verticalScrollBar();
    sb->setValue(sb->maximum());
}

// ─── onInputEnabled ──────────────────────────────────────────────────────────
void MainWindow::onInputEnabled(bool enabled)
{
    ui->inputLine->setEnabled(enabled);
    ui->sendButton->setEnabled(enabled);
    ui->stopButton->setEnabled(!enabled);

    if (enabled)
        ui->inputLine->setFocus();
}

// ─── onStatsUpdated ──────────────────────────────────────────────────────────
// Reine Darstellungslogik — deshalb in der View, nicht im Agent.
// Farb-Schwellwerte: grün < 60% < orange < 85% < rot.
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
