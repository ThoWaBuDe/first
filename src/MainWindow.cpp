#include "MainWindow.h"
#include "LlamaWorker.h"
#include "ui_MainWindow.h"

#include <QScrollBar>
#include <QTextCursor>
#include <QMetaObject>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

// ─── Konstruktor ──────────────────────────────────────────────────────────────
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_chatModel()
{
    ui->setupUi(this);

    addDockWidget(Qt::RightDockWidgetArea, ui->toolDock);
    ui->toolDock->setMinimumWidth(350);

    ui->chatView->document()->setDefaultStyleSheet(R"(
        body      { font-family: 'Noto Sans', sans-serif; font-size: 13px; }
        .user     { color: #1a73e8; margin: 6px 0; }
        .assistant{ color: #202124; margin: 6px 0; }
        .error    { color: #c5221f; }
        .system   { color: #888; font-style: italic; font-size: 11px; }
        b         { font-weight: 600; }
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
    ui->statsLabel->setStyleSheet("color: #555; font-size: 11px; font-family: monospace;");

    connect(ui->inputLine,       &QLineEdit::returnPressed,  this, &MainWindow::onSendClicked);
    connect(ui->sendButton,      &QPushButton::clicked,      this, &MainWindow::onSendClicked);
    connect(ui->stopButton,      &QPushButton::clicked,      this, &MainWindow::onStopClicked);
    connect(ui->clearButton,     &QPushButton::clicked,      this, &MainWindow::onClearClicked);
    connect(ui->clearToolsButton,&QPushButton::clicked,      this, &MainWindow::onClearToolsClicked);

    setupWorker();
}

// ─── Destruktor ───────────────────────────────────────────────────────────────
MainWindow::~MainWindow()
{
    if (m_worker) m_worker->stopGeneration();
    m_workerThread.quit();
    m_workerThread.wait(3000);
    delete ui;
}

// ─── setupWorker ─────────────────────────────────────────────────────────────
void MainWindow::setupWorker()
{
    m_worker = new LlamaWorker();
    m_worker->moveToThread(&m_workerThread);

    // SamplerProfile fuer QueuedConnection registrieren.
    // Ohne das kann Qt den Typ nicht durch die Thread-Grenze transportieren.
    // Muss vor dem ersten invokeMethod mit diesem Typ passieren.
    qRegisterMetaType<LlamaWorker::SamplerProfile>();

    QString binDir = QCoreApplication::applicationDirPath();
    m_mcp.addServer(binDir + "/mcp-servers/filesystem/llamaqt-filesystem");
    m_mcp.addServer(binDir + "/mcp-servers/sysinfo/llamaqt-sysinfo");
    m_mcp.addServer(binDir + "/mcp-servers/compile/llamaqt-compile");
    m_mcp.addServer(binDir + "/mcp-servers/websearch/llamaqt-websearch");

    connect(&m_mcp, &McpManager::serverDied, this, [this](const QString &name){
        appendToTools(QString("MCP-Server gestorben: %1").arg(name), "error");
    });

    connect(m_worker, &LlamaWorker::tokenGenerated,  this, &MainWindow::onTokenReceived);
    connect(m_worker, &LlamaWorker::generationDone,  this, &MainWindow::onGenerationDone);
    connect(m_worker, &LlamaWorker::modelLoaded,     this, &MainWindow::onModelLoaded);
    connect(m_worker, &LlamaWorker::errorOccurred,   this, &MainWindow::onError);
    connect(m_worker, &LlamaWorker::statsUpdate,     this, &MainWindow::onStatsUpdate);
    connect(&m_workerThread, &QThread::finished,     m_worker, &QObject::deleteLater);

    m_workerThread.start();

    m_mcp.startAll([this](bool ok, QStringList errors) {
        if (!ok)
            for (const QString &e : errors)
                appendToTools("MCP Fehler: " + e, "error");
        m_chatModel.setSystemPrompt(m_mcp.buildToolsSystemPrompt());
        QMetaObject::invokeMethod(m_worker, "initialize",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, MODEL_PATH));
    });
}

// ─── onSendClicked ───────────────────────────────────────────────────────────
void MainWindow::onSendClicked()
{
    QString text = ui->inputLine->text().trimmed();
    if (text.isEmpty() || m_generating) return;
    ui->inputLine->clear();
    sendMessage(text);
}

// ─── sendMessage ─────────────────────────────────────────────────────────────
// Startet eine neue Generierung — immer mit Chat-Profil.
// Das Modell antwortet zunaechst frei; erst wenn ein Tool-Call erkannt wird,
// schaltet onGenerationDone() auf Tool-Profil um.
void MainWindow::sendMessage(const QString &userText)
{
    m_chatModel.addUserMessage(userText);
    appendToChat(QString("<b>Du:</b> %1").arg(userText.toHtmlEscaped()), "user");
    appendToChat("<b>Assistent:</b> ", "assistant");

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock    = false;
    m_generatedTokens = 0;
    m_generating      = true;
    setInputEnabled(false);
    ui->statusLabel->setText("Generiere...");

    // Chat-Profil: erste Antwort auf eine User-Nachricht ist immer Chat.
    QMetaObject::invokeMethod(m_worker, "generate",
                              Qt::QueuedConnection,
                              Q_ARG(QString,                    m_chatModel.buildPrompt()),
                              Q_ARG(LlamaWorker::SamplerProfile, LlamaWorker::SamplerProfile::Chat));
}

// ─── onTokenReceived ─────────────────────────────────────────────────────────
// Thinking-Filter als State-Machine (unveraendert).
void MainWindow::onTokenReceived(const QString &token)
{
    ++m_generatedTokens;
    ++m_totalTokens;
    m_currentResponse += token;

    static constexpr int MAX_TAG_LEN = 12;

    if (!m_inThinkBlock) {
        m_thinkBuffer += token;

        if (m_thinkBuffer.endsWith("<think>")) {
            QString before = m_thinkBuffer;
            before.chop(7);
            if (!before.isEmpty()) {
                QTextCursor cursor = ui->chatView->textCursor();
                cursor.movePosition(QTextCursor::End);
                cursor.insertText(before);
                QScrollBar *sb = ui->chatView->verticalScrollBar();
                sb->setValue(sb->maximum());
            }
            m_inThinkBlock = true;
            m_thinkBuffer.clear();

        } else if (m_thinkBuffer.length() > MAX_TAG_LEN ||
                   !QString("<think>").startsWith(m_thinkBuffer.right(MAX_TAG_LEN))) {
            QTextCursor cursor = ui->chatView->textCursor();
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(m_thinkBuffer);
            QScrollBar *sb = ui->chatView->verticalScrollBar();
            sb->setValue(sb->maximum());
            m_thinkBuffer.clear();
        }

    } else {
        m_thinkBuffer += token;

        if (m_thinkBuffer.endsWith("</think>")) {
            QString thinkText = m_thinkBuffer;
            thinkText.chop(8);
            thinkText = thinkText.trimmed();

            appendToTools(
                QString("<b>Thinking:</b><br><span style='white-space:pre-wrap'>%1</span>")
                .arg(thinkText.toHtmlEscaped()), "think");

            m_inThinkBlock = false;
            m_thinkBuffer.clear();
        }
    }

    if (m_generatedTokens % 10 == 0)
        updateStats();
}

// ─── onGenerationDone ────────────────────────────────────────────────────────
// Profil-Logik:
//   Fall A (Continuation):  Tool-Profil — das Modell soll ein abgebrochenes
//                           JSON zu Ende schreiben.
//   Fall B (Tool-Call):     nach Tool-Ergebnis → Tool-Profil, weil das Modell
//                           entweder direkt antwortet oder einen naechsten
//                           Tool-Call schreibt.
//   Fall C (normale Antwort): kein weiterer generate()-Aufruf noetig.
void MainWindow::onGenerationDone(const QString &fullResponse)
{
    m_generating = false;
    updateStats();

    // Fall A: offener Tool-Call → Continuation mit Tool-Profil
    if (fullResponse.contains("<tool_call>") && !fullResponse.contains("</tool_call>")) {
        ++m_continuationCount;
        if (m_continuationCount > MAX_CONTINUATIONS) {
            m_continuationCount = 0;
            appendToTools(QString("Tool-Call nach %1 Fortsetzungen unvollstaendig.")
                          .arg(MAX_CONTINUATIONS), "error");
            m_chatModel.addAssistantMessage(fullResponse);
            setInputEnabled(true);
            return;
        }
        ui->statusLabel->setText(QString("Fortsetzung %1/%2...")
                                 .arg(m_continuationCount).arg(MAX_CONTINUATIONS));
        m_chatModel.addAssistantMessage(fullResponse);
        m_generating      = true;
        m_generatedTokens = 0;

        // Tool-Profil: das Modell soll das angefangene JSON praezise fortfuehren.
        QMetaObject::invokeMethod(m_worker, "generate",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString,                    m_chatModel.buildPrompt()),
                                  Q_ARG(LlamaWorker::SamplerProfile, LlamaWorker::SamplerProfile::Tool));
        return;
    }

    m_continuationCount = 0;

    // Fall B: vollstaendiger Tool-Call
    if (fullResponse.contains("<tool_call>") && fullResponse.contains("</tool_call>")) {
        m_chatModel.addAssistantMessage(fullResponse);

        int start     = fullResponse.indexOf("<tool_call>") + 11;
        int end       = fullResponse.indexOf("</tool_call>", start);
        QString block = fullResponse.mid(start, end - start).trimmed();

        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);
        if (pe.error != QJsonParseError::NoError) {
            appendToTools(QString("Tool-Call JSON Fehler: %1").arg(pe.errorString()), "error");
            setInputEnabled(true);
            return;
        }

        QString     toolName = doc.object().value("name").toString();
        QJsonObject toolArgs = doc.object().value("arguments").toObject();

        appendToTools(
            QString("<b>Tool-Call: %1</b><br><pre>%2</pre>")
            .arg(toolName.toHtmlEscaped(),
                 QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Indented))
                 .toHtmlEscaped()),
            "tool");

        if (!m_mcp.containsTool(toolName)) {
            appendToTools(QString("Fehler: Unbekanntes Tool '%1'.").arg(toolName), "error");
            m_chatModel.addToolResult(toolName,
                QString("Fehler: Unbekanntes Tool '%1'.").arg(toolName));
            m_generating = true;
            // Tool-Profil: Modell soll korrekten Tool-Namen waehlen.
            QMetaObject::invokeMethod(m_worker, "generate",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString,                    m_chatModel.buildPrompt()),
                                      Q_ARG(LlamaWorker::SamplerProfile, LlamaWorker::SamplerProfile::Tool));
            return;
        }

        ui->statusLabel->setText(QString("Tool: %1...").arg(toolName));

        m_mcp.callTool(toolName, toolArgs,
            [this, toolName](QString result, QString error) {
                QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);
                bool    isErr      = !error.isEmpty();

                appendToTools(
                    QString("<b>Ergebnis [%1]:</b><br><pre>%2</pre>")
                    .arg(toolName.toHtmlEscaped(), toolResult.toHtmlEscaped()),
                    isErr ? "error" : "tool");

                m_chatModel.addToolResult(toolName, toolResult);
                m_generating      = true;
                m_generatedTokens = 0;
                ui->statusLabel->setText("Tool-Ergebnis verarbeiten...");
                appendToChat("<b>Assistent:</b> ", "assistant");
                m_currentResponse.clear();
                m_thinkBuffer.clear();
                m_inThinkBlock = false;

                // Tool-Profil: nach Tool-Ergebnis erwartet man entweder
                // einen naechsten Tool-Call oder eine abschliessende Antwort.
                // Das Modell muss praezise auf das Ergebnis eingehen.
                QMetaObject::invokeMethod(m_worker, "generate",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString,                    m_chatModel.buildPrompt()),
                                          Q_ARG(LlamaWorker::SamplerProfile, LlamaWorker::SamplerProfile::Tool));
            });
        return;
    }

    // Fall C: normale Antwort — kein generate() mehr noetig.
    m_chatModel.addAssistantMessage(fullResponse);
    setInputEnabled(true);
    ui->statusLabel->setText("Bereit");
    ui->inputLine->setFocus();
}

// ─── onStatsUpdate ───────────────────────────────────────────────────────────
void MainWindow::onStatsUpdate(int promptTokens, int ctxSize)
{
    m_promptTokens = promptTokens;
    m_ctxSize      = ctxSize;
    updateStats();
}

// ─── updateStats ─────────────────────────────────────────────────────────────
void MainWindow::updateStats()
{
    int used  = m_promptTokens + m_generatedTokens;
    int pct   = (m_ctxSize > 0) ? (used * 100 / m_ctxSize) : 0;

    QString color = "#188038";
    if (pct > 60) color = "#e37400";
    if (pct > 85) color = "#c5221f";

    ui->statsLabel->setText(
        QString("Prompt: %1 | Gen: %2 | Gesamt: %3 / %4 (%5%) | Total: %6")
        .arg(m_promptTokens)
        .arg(m_generatedTokens)
        .arg(used)
        .arg(m_ctxSize)
        .arg(pct)
        .arg(m_totalTokens));

    ui->statsLabel->setStyleSheet(
        QString("color: %1; font-size: 11px; font-family: monospace;").arg(color));
}

// ─── onStopClicked ───────────────────────────────────────────────────────────
void MainWindow::onStopClicked()
{
    if (m_worker) m_worker->stopGeneration();
    m_generating = false;
    setInputEnabled(true);
    ui->statusLabel->setText("Gestoppt");
}

// ─── onClearClicked ──────────────────────────────────────────────────────────
void MainWindow::onClearClicked()
{
    m_chatModel.clear();
    ui->chatView->clear();
    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock    = false;
    m_generatedTokens = 0;
    m_totalTokens     = 0;
    m_promptTokens    = 0;
    updateStats();
    appendToChat("Chat geloescht.", "system");
}

// ─── onClearToolsClicked ─────────────────────────────────────────────────────
void MainWindow::onClearToolsClicked()
{
    ui->toolView->clear();
}

// ─── onModelLoaded ───────────────────────────────────────────────────────────
void MainWindow::onModelLoaded()
{
    setInputEnabled(true);
    ui->statusLabel->setText("Modell geladen - bereit");
    appendToChat("Modell geladen: Qwen3.5-9B-Q6_K", "system");

    // Sampler-Profile in toolView anzeigen
    appendToTools(
        "<b>Sampler-Profile:</b><br>"
        "&nbsp;Chat: Top-K 40 | Temp 0.7 | Top-P 0.95<br>"
        "&nbsp;Tool: Top-K 20 | Temp 0.1 | Top-P 0.50",
        "system");

    QString toolDebug = "<b>MCP Tools:</b><br>";
    int toolCount = 0;
    for (const auto &info : m_mcp.debugToolInfo()) {
        toolDebug += QString("&nbsp;<i>%1</i>: ").arg(info.serverName.toHtmlEscaped());
        toolDebug += info.toolNames.join(", ").toHtmlEscaped() + "<br>";
        toolCount += info.toolNames.size();
    }
    toolDebug += toolCount == 0
        ? "<b>WARNUNG: Keine Tools!</b>"
        : QString("<b>%1 Tools geladen</b>").arg(toolCount);
    appendToTools(toolDebug, "system");

    int promptLen = m_chatModel.messages().isEmpty()
                    ? 0 : m_chatModel.messages()[0].content.length();
    appendToTools(QString("System-Prompt: %1 Zeichen").arg(promptLen), "stats");

    ui->inputLine->setFocus();
}

// ─── onError ─────────────────────────────────────────────────────────────────
void MainWindow::onError(const QString &error)
{
    m_generating = false;
    setInputEnabled(true);
    appendToTools(QString("Fehler: %1").arg(error.toHtmlEscaped()), "error");
    ui->statusLabel->setText("Fehler");
}

// ─── appendToChat ────────────────────────────────────────────────────────────
void MainWindow::appendToChat(const QString &text, const QString &cssClass)
{
    ui->chatView->append(
        QString(R"(<p class="%1">%2</p>)").arg(cssClass, text));
}

// ─── appendToTools ───────────────────────────────────────────────────────────
void MainWindow::appendToTools(const QString &text, const QString &cssClass)
{
    ui->toolView->append(
        QString(R"(<p class="%1">%2</p>)").arg(cssClass, text));
    QScrollBar *sb = ui->toolView->verticalScrollBar();
    sb->setValue(sb->maximum());
}

// ─── setInputEnabled ─────────────────────────────────────────────────────────
void MainWindow::setInputEnabled(bool enabled)
{
    ui->inputLine->setEnabled(enabled);
    ui->sendButton->setEnabled(enabled);
    ui->stopButton->setEnabled(!enabled);
}
