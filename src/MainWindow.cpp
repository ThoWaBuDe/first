#include "MainWindow.h"
#include "LlamaWorker.h"
#include "ui_MainWindow.h"

#include <QScrollBar>
#include <QTextCursor>
#include <QMetaObject>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_chatModel()
{
    ui->setupUi(this);

    ui->chatView->document()->setDefaultStyleSheet(R"(
        body      { font-family: 'Noto Sans', sans-serif; font-size: 13px; }
        .user     { color: #1a73e8; margin: 6px 0; }
        .assistant{ color: #202124; margin: 6px 0; }
        .tool     { color: #188038; font-family: monospace; font-size: 12px;
                    background: #f1f8f4; padding: 4px; border-radius: 3px; }
        .error    { color: #c5221f; }
        .system   { color: #888; font-style: italic; font-size: 11px; }
        b         { font-weight: 600; }
    )");

    ui->statusLabel->setStyleSheet("color: #888; font-size: 11px;");

    connect(ui->inputLine,   &QLineEdit::returnPressed,  this, &MainWindow::onSendClicked);
    connect(ui->sendButton,  &QPushButton::clicked,      this, &MainWindow::onSendClicked);
    connect(ui->stopButton,  &QPushButton::clicked,      this, &MainWindow::onStopClicked);
    connect(ui->clearButton, &QPushButton::clicked,      this, &MainWindow::onClearClicked);

    setupWorker();
}

MainWindow::~MainWindow()
{
    if (m_worker) m_worker->stopGeneration();
    m_workerThread.quit();
    m_workerThread.wait(3000);
    delete ui;
}

void MainWindow::setupWorker()
{
    m_worker = new LlamaWorker();
    m_worker->moveToThread(&m_workerThread);

    QString binDir = QCoreApplication::applicationDirPath();
    m_mcp.addServer(binDir + "/mcp-servers/filesystem/llamaqt-filesystem");
    m_mcp.addServer(binDir + "/mcp-servers/sysinfo/llamaqt-sysinfo");
    m_mcp.addServer(binDir + "/mcp-servers/compile/llamaqt-compile");

    connect(&m_mcp, &McpManager::serverDied, this, [this](const QString &name){
        appendToChat(QString("MCP-Server gestorben: %1").arg(name), "error");
    });

    connect(m_worker, &LlamaWorker::tokenGenerated,  this, &MainWindow::onTokenReceived);
    connect(m_worker, &LlamaWorker::generationDone,  this, &MainWindow::onGenerationDone);
    connect(m_worker, &LlamaWorker::modelLoaded,     this, &MainWindow::onModelLoaded);
    connect(m_worker, &LlamaWorker::errorOccurred,   this, &MainWindow::onError);
    connect(&m_workerThread, &QThread::finished,     m_worker, &QObject::deleteLater);

    m_workerThread.start();

    m_mcp.startAll([this](bool ok, QStringList errors) {
        if (!ok)
            for (const QString &e : errors)
                appendToChat("MCP Fehler: " + e, "error");
        m_chatModel.setSystemPrompt(m_mcp.buildToolsSystemPrompt());
        QMetaObject::invokeMethod(m_worker, "initialize",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, MODEL_PATH));
    });
}

void MainWindow::onSendClicked()
{
    QString text = ui->inputLine->text().trimmed();
    if (text.isEmpty() || m_generating) return;
    ui->inputLine->clear();
    sendMessage(text);
}

void MainWindow::sendMessage(const QString &userText)
{
    m_chatModel.addUserMessage(userText);
    appendToChat(QString("<b>Du:</b> %1").arg(userText.toHtmlEscaped()), "user");
    appendToChat("<b>Assistent:</b> ", "assistant");
    m_currentResponse.clear();
    m_generating = true;
    setInputEnabled(false);
    ui->statusLabel->setText("Generiere...");
    QMetaObject::invokeMethod(m_worker, "generate",
                              Qt::QueuedConnection,
                              Q_ARG(QString, m_chatModel.buildPrompt()));
}

void MainWindow::onTokenReceived(const QString &token)
{
    m_currentResponse += token;
    QTextCursor cursor = ui->chatView->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(token);
    QScrollBar *sb = ui->chatView->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void MainWindow::onGenerationDone(const QString &fullResponse)
{
    m_generating = false;

    if (fullResponse.contains("<tool_call>") && !fullResponse.contains("</tool_call>")) {
        ++m_continuationCount;
        if (m_continuationCount > MAX_CONTINUATIONS) {
            m_continuationCount = 0;
            appendToChat(QString("Tool-Call nach %1 Fortsetzungen unvollstaendig.")
                         .arg(MAX_CONTINUATIONS), "error");
            m_chatModel.addAssistantMessage(fullResponse);
            setInputEnabled(true);
            return;
        }
        ui->statusLabel->setText(QString("Fortsetzung %1/%2...")
                                 .arg(m_continuationCount).arg(MAX_CONTINUATIONS));
        m_chatModel.addAssistantMessage(fullResponse);
        m_generating = true;
        QMetaObject::invokeMethod(m_worker, "generate", Qt::QueuedConnection,
                                  Q_ARG(QString, m_chatModel.buildPrompt()));
        return;
    }

    m_continuationCount = 0;

    if (fullResponse.contains("<tool_call>") && fullResponse.contains("</tool_call>")) {
        m_chatModel.addAssistantMessage(fullResponse);
        int start     = fullResponse.indexOf("<tool_call>") + 11;
        int end       = fullResponse.indexOf("</tool_call>", start);
        QString block = fullResponse.mid(start, end - start).trimmed();

        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);
        if (pe.error != QJsonParseError::NoError) {
            appendToChat(QString("Tool-Call JSON Fehler: %1").arg(pe.errorString()), "error");
            setInputEnabled(true);
            return;
        }

        QString     toolName = doc.object().value("name").toString();
        QJsonObject toolArgs = doc.object().value("arguments").toObject();

        if (!m_mcp.containsTool(toolName)) {
            m_chatModel.addToolResult(toolName,
                QString("Fehler: Unbekanntes Tool '%1'.").arg(toolName));
            m_generating = true;
            QMetaObject::invokeMethod(m_worker, "generate", Qt::QueuedConnection,
                                      Q_ARG(QString, m_chatModel.buildPrompt()));
            return;
        }

        ui->statusLabel->setText(QString("Tool: %1...").arg(toolName));
        m_mcp.callTool(toolName, toolArgs,
            [this, toolName](QString result, QString error) {
                QString tr = error.isEmpty() ? result : ("Fehler: " + error);
                appendToChat(QString("<b>Tool [%1]:</b><br><pre>%2</pre>")
                             .arg(toolName.toHtmlEscaped(), tr.toHtmlEscaped()), "tool");
                m_chatModel.addToolResult(toolName, tr);
                m_generating = true;
                ui->statusLabel->setText("Tool-Ergebnis verarbeiten...");
                appendToChat("<b>Assistent:</b> ", "assistant");
                m_currentResponse.clear();
                QMetaObject::invokeMethod(m_worker, "generate", Qt::QueuedConnection,
                                          Q_ARG(QString, m_chatModel.buildPrompt()));
            });
        return;
    }

    m_chatModel.addAssistantMessage(fullResponse);
    setInputEnabled(true);
    ui->statusLabel->setText(QString("Bereit - %1 Tokens").arg(fullResponse.split(' ').size()));
    ui->inputLine->setFocus();
}

void MainWindow::onStopClicked()
{
    if (m_worker) m_worker->stopGeneration();
    ui->statusLabel->setText("Gestoppt");
}

void MainWindow::onClearClicked()
{
    m_chatModel.clear();
    ui->chatView->clear();
    m_currentResponse.clear();
    appendToChat("Chat geloescht.", "system");
}

void MainWindow::onModelLoaded()
{
    setInputEnabled(true);
    ui->statusLabel->setText("Modell geladen - bereit");
    appendToChat("Modell geladen: Qwen3.5-9B-Q6_K", "system");

    QString toolDebug = "<b>MCP Tools advertised:</b><br>";
    int toolCount = 0;
    for (const auto &info : m_mcp.debugToolInfo()) {
        toolDebug += QString("&nbsp;&nbsp;<i>Server: %1</i><br>")
                     .arg(info.serverName.toHtmlEscaped());
        for (const QString &tool : info.toolNames) {
            toolDebug += QString("&nbsp;&nbsp;&nbsp;&nbsp;[%1]<br>").arg(tool.toHtmlEscaped());
            ++toolCount;
        }
    }
    toolDebug += toolCount == 0
        ? "&nbsp;&nbsp;<b>WARNUNG: Keine Tools!</b>"
        : QString("<b>Gesamt: %1 Tools</b>").arg(toolCount);
    appendToChat(toolDebug, "system");

    int promptLen = m_chatModel.messages().isEmpty()
                    ? 0 : m_chatModel.messages()[0].content.length();
    appendToChat(QString("System-Prompt: %1 Zeichen").arg(promptLen), "system");
    ui->inputLine->setFocus();
}

void MainWindow::onError(const QString &error)
{
    m_generating = false;
    setInputEnabled(true);
    appendToChat(QString("Fehler: %1").arg(error.toHtmlEscaped()), "error");
    ui->statusLabel->setText("Fehler");
}

void MainWindow::appendToChat(const QString &text, const QString &cssClass)
{
    ui->chatView->append(QString(R"(<p class="%1">%2</p>)").arg(cssClass, text));
}

void MainWindow::setInputEnabled(bool enabled)
{
    ui->inputLine->setEnabled(enabled);
    ui->sendButton->setEnabled(enabled);
    ui->stopButton->setEnabled(!enabled);
}
