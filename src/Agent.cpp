#include "Agent.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

// ─── Konstruktor ──────────────────────────────────────────────────────────────
Agent::Agent(const QString &modelPath, QObject *parent)
    : QObject(parent)
    , m_modelPath(modelPath)
    , m_chatModel()
    , m_mcp(this)
{
    m_worker = new LlamaWorker();
    m_worker->moveToThread(&m_workerThread);

    qRegisterMetaType<LlamaWorker::SamplerProfile>();

    connect(m_worker, &LlamaWorker::tokenGenerated,  this, &Agent::onTokenReceived);
    connect(m_worker, &LlamaWorker::generationDone,  this, &Agent::onGenerationDone);
    connect(m_worker, &LlamaWorker::modelLoaded,     this, &Agent::onModelLoaded);
    connect(m_worker, &LlamaWorker::errorOccurred,   this, &Agent::onError);
    connect(m_worker, &LlamaWorker::statsUpdate,     this, &Agent::onStatsUpdate);
    connect(&m_workerThread, &QThread::finished,     m_worker, &QObject::deleteLater);

    connect(&m_mcp, &McpManager::serverDied, this, [this](const QString &name) {
        emit appendTools(QString("MCP-Server gestorben: %1").arg(name), "error");
    });
}

Agent::~Agent()
{
    if (m_worker) m_worker->stopGeneration();
    m_workerThread.quit();
    m_workerThread.wait(3000);
}

// ─── start ───────────────────────────────────────────────────────────────────
void Agent::start()
{
    m_workerThread.start();

    QString binDir = QCoreApplication::applicationDirPath();
    m_mcp.addServer(binDir + "/mcp-servers/filesystem/llamaqt-filesystem");
    m_mcp.addServer(binDir + "/mcp-servers/sysinfo/llamaqt-sysinfo");
    m_mcp.addServer(binDir + "/mcp-servers/compile/llamaqt-compile");
    m_mcp.addServer(binDir + "/mcp-servers/websearch/llamaqt-websearch");

    m_mcp.startAll([this](bool ok, QStringList errors) {
        if (!ok)
            for (const QString &e : errors)
                emit appendTools("MCP Fehler: " + e, "error");

        m_chatModel.setSystemPrompt(m_mcp.buildToolsSystemPrompt());

        QMetaObject::invokeMethod(m_worker, "initialize",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, m_modelPath));
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// SLOTS: Eingaben von der UI
// ═════════════════════════════════════════════════════════════════════════════

void Agent::onUserMessage(const QString &text)
{
    if (text.trimmed().isEmpty() || m_generating) return;

    m_chatModel.addUserMessage(text);

    // Neuer Block für die User-Zeile
    emit appendChat(QString("<b>Du:</b> %1").arg(text.toHtmlEscaped()), "user");

    // Neuer Block "Assistent:" — in diesen Block schreibt appendChatToken hinein
    emit appendChat("<b>Assistent:</b> ", "assistant");

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock      = false;
    m_generatedTokens   = 0;
    m_continuationCount = 0;
    m_generating        = true;

    emit inputEnabled(false);
    emit statusChanged("Generiere...");

    startGeneration(LlamaWorker::SamplerProfile::Chat);
}

void Agent::onStop()
{
    if (m_worker) m_worker->stopGeneration();
    m_generating = false;
    emit inputEnabled(true);
    emit statusChanged("Gestoppt");
}

void Agent::onClearChat()
{
    m_chatModel.clear();
    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock      = false;
    m_generatedTokens   = 0;
    m_totalTokens       = 0;
    m_promptTokens      = 0;
    emit appendChat("Chat gelöscht.", "system");
    emitStats();
}

// ═════════════════════════════════════════════════════════════════════════════
// PRIVATE SLOTS: Worker-Callbacks
// ═════════════════════════════════════════════════════════════════════════════

void Agent::onModelLoaded()
{
    emit inputEnabled(true);
    emit statusChanged("Modell geladen – bereit");
    emit appendChat("Modell geladen: Qwen3.5-9B-Q6_K", "system");

    emit appendTools(
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
    emit appendTools(toolDebug, "system");

    int promptLen = m_chatModel.messages().isEmpty()
                    ? 0 : m_chatModel.messages()[0].content.length();
    emit appendTools(QString("System-Prompt: %1 Zeichen").arg(promptLen), "stats");
}

void Agent::onStatsUpdate(int promptTokens, int ctxSize)
{
    m_promptTokens = promptTokens;
    m_ctxSize      = ctxSize;
    emitStats();
}

void Agent::onError(const QString &error)
{
    m_generating = false;
    emit inputEnabled(true);
    emit appendTools(QString("Fehler: %1").arg(error.toHtmlEscaped()), "error");
    emit statusChanged("Fehler");
}

// ─── onTokenReceived ─────────────────────────────────────────────────────────
// Jeder Token vom Worker landet hier.
// filterToken() entscheidet: sichtbar (→ appendChatToken) oder Think (→ appendTools).
void Agent::onTokenReceived(const QString &token)
{
    ++m_generatedTokens;
    ++m_totalTokens;
    m_currentResponse += token;

    filterToken(token);

    if (m_generatedTokens % 10 == 0)
        emitStats();
}

// ─── onGenerationDone ────────────────────────────────────────────────────────
// Agenten-Loop: drei Fälle — offener Tool-Call, vollständiger Tool-Call,
// normale Antwort.
void Agent::onGenerationDone(const QString &fullResponse)
{
    m_generating = false;
    emitStats();

    // ─── Fall A: Offener Tool-Call → Continuation ─────────────────────────
    if (fullResponse.contains("<tool_call>") && !fullResponse.contains("</tool_call>")) {
        ++m_continuationCount;
        if (m_continuationCount > MAX_CONTINUATIONS) {
            m_continuationCount = 0;
            emit appendTools(
                QString("Tool-Call nach %1 Fortsetzungen unvollständig — abgebrochen.")
                .arg(MAX_CONTINUATIONS), "error");
            m_chatModel.addAssistantMessage(fullResponse);
            emit inputEnabled(true);
            emit statusChanged("Bereit");
            return;
        }
        emit statusChanged(QString("Fortsetzung %1/%2...")
                           .arg(m_continuationCount).arg(MAX_CONTINUATIONS));
        m_chatModel.addAssistantMessage(fullResponse);
        m_generating      = true;
        m_generatedTokens = 0;
        startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    m_continuationCount = 0;

    // ─── Fall B: Vollständiger Tool-Call ──────────────────────────────────
    if (fullResponse.contains("<tool_call>") && fullResponse.contains("</tool_call>")) {
        m_chatModel.addAssistantMessage(fullResponse);
        handleToolCall(fullResponse);
        return;
    }

    // ─── Fall C: Normale Antwort ──────────────────────────────────────────
    m_chatModel.addAssistantMessage(fullResponse);
    emit inputEnabled(true);
    emit statusChanged("Bereit");
}

// ═════════════════════════════════════════════════════════════════════════════
// PRIVATE METHODEN
// ═════════════════════════════════════════════════════════════════════════════

// ─── startGeneration ─────────────────────────────────────────────────────────
void Agent::startGeneration(LlamaWorker::SamplerProfile profile)
{
    QMetaObject::invokeMethod(m_worker, "generate",
                              Qt::QueuedConnection,
                              Q_ARG(QString,                     m_chatModel.buildPrompt()),
                              Q_ARG(LlamaWorker::SamplerProfile, profile));
}

// ─── handleToolCall ──────────────────────────────────────────────────────────
void Agent::handleToolCall(const QString &fullResponse)
{
    int start     = fullResponse.indexOf("<tool_call>") + 11;
    int end       = fullResponse.indexOf("</tool_call>", start);
    QString block = fullResponse.mid(start, end - start).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        emit appendTools(
            QString("Tool-Call JSON Fehler: %1<br><pre>%2</pre>")
            .arg(pe.errorString().toHtmlEscaped(), block.toHtmlEscaped()),
            "error");
        emit inputEnabled(true);
        emit statusChanged("Bereit");
        return;
    }

    QString     toolName = doc.object().value("name").toString();
    QJsonObject toolArgs = doc.object().value("arguments").toObject();

    emit appendTools(
        QString("<b>Tool-Call: %1</b><br><pre>%2</pre>")
        .arg(toolName.toHtmlEscaped(),
             QString::fromUtf8(QJsonDocument(toolArgs)
                               .toJson(QJsonDocument::Indented))
             .toHtmlEscaped()),
        "tool");

    if (!m_mcp.containsTool(toolName)) {
        QString errMsg = QString("Fehler: Unbekanntes Tool '%1'.").arg(toolName);
        emit appendTools(errMsg, "error");
        m_chatModel.addToolResult(toolName, errMsg);
        m_generating      = true;
        m_generatedTokens = 0;
        startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    emit statusChanged(QString("Tool: %1...").arg(toolName));

    m_mcp.callTool(toolName, toolArgs,
        [this, toolName](QString result, QString error) {
            QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);
            bool    isErr      = !error.isEmpty();

            emit appendTools(
                QString("<b>Ergebnis [%1]:</b><br><pre>%2</pre>")
                .arg(toolName.toHtmlEscaped(), toolResult.toHtmlEscaped()),
                isErr ? "error" : "tool");

            m_chatModel.addToolResult(toolName, toolResult);
            m_generating      = true;
            m_generatedTokens = 0;
            m_currentResponse.clear();
            m_thinkBuffer.clear();
            m_inThinkBlock = false;

            // Neuen Assistent-Block öffnen — appendChatToken schreibt hinein
            emit appendChat("<b>Assistent:</b> ", "assistant");
            emit statusChanged("Tool-Ergebnis verarbeiten...");

            startGeneration(LlamaWorker::SamplerProfile::Tool);
        });
}

// ─── filterToken ─────────────────────────────────────────────────────────────
// Pattern: State Machine, zwei Zustände: NORMAL und THINK.
//
// NORMAL-Zustand (m_inThinkBlock == false):
//   Tokens in m_thinkBuffer akkumulieren.
//   Sobald "<think>" vollständig → in THINK-Zustand.
//   Falls "<think>" nicht möglich → Buffer per appendChatToken ausgeben.
//   appendChatToken schreibt in den LAUFENDEN Absatz (kein neues <p>).
//
// THINK-Zustand (m_inThinkBlock == true):
//   Tokens akkumulieren bis "</think>" vollständig.
//   Dann: gesamter Block per appendTools(..., "think") in toolView.
//   Zurück zu NORMAL.
//
// Warum Buffer?
//   "<think>" kann über mehrere Tokens verteilt ankommen:
//   Token 1: "<"   Token 2: "thi"   Token 3: "nk>"
//   Der Buffer wartet bis das Tag komplett ist — erst dann entscheiden.
//   Analogie AVR: UART-Empfangspuffer wartet auf Steuerzeichen \n.
void Agent::filterToken(const QString &token)
{
    static constexpr int MAX_TAG_LEN = 12;

    if (!m_inThinkBlock) {
        m_thinkBuffer += token;

        if (m_thinkBuffer.endsWith("<think>")) {
            // "<think>" erkannt — Text davor noch ausgeben
            QString before = m_thinkBuffer;
            before.chop(7);  // len("<think>") == 7
            if (!before.isEmpty())
                emit appendChatToken(before);  // in laufenden Absatz, kein \n

            m_inThinkBlock = true;
            m_thinkBuffer.clear();

        } else if (m_thinkBuffer.length() > MAX_TAG_LEN ||
                   !QString("<think>").startsWith(
                       m_thinkBuffer.right(MAX_TAG_LEN))) {
            // Kein "<think>" möglich → sofort ausgeben
            emit appendChatToken(m_thinkBuffer);  // kein <p>, nur Text anhängen
            m_thinkBuffer.clear();
        }

    } else {
        // THINK-Zustand: akkumulieren bis </think>
        m_thinkBuffer += token;

        if (m_thinkBuffer.endsWith("</think>")) {
            QString thinkText = m_thinkBuffer;
            thinkText.chop(8);  // len("</think>") == 8
            thinkText = thinkText.trimmed();

            emit appendTools(
                QString("<b>Thinking:</b><br>"
                        "<span style='white-space:pre-wrap'>%1</span>")
                .arg(thinkText.toHtmlEscaped()),
                "think");

            m_inThinkBlock = false;
            m_thinkBuffer.clear();
        }
    }
}

// ─── emitStats ───────────────────────────────────────────────────────────────
void Agent::emitStats()
{
    emit statsUpdated(m_promptTokens, m_generatedTokens,
                      m_totalTokens,  m_ctxSize);
}
