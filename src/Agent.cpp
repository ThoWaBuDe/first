#include "Agent.h"
#include "AgentChat.h"
#include "AgentPlan.h"
#include "AgentExecute.h"
#include "AgentAssemble.h"
#include "AgentUtils.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QFileInfo>
#include <QDir>

// ── Whitelists ────────────────────────────────────────────────────────────────
const QStringList Agent::PLAN_ALLOWED_TOOLS = {
    "read_file", "list_dir", "get_symbol", "get_project_index",
    "rebuild_index", "get_time", "sys_info", "disk_free", "get_pwd",
};

const QStringList Agent::EXECUTE_ALLOWED_TOOLS = {
    "read_file", "list_dir", "get_symbol", "get_project_index",
    "get_function_body", "get_class_members", "get_includes",
    "check_syntax", "read_multiple_files", "grep_code", "search_code",
    "web_search", "get_time", "sys_info", "get_pwd", "disk_free",
};

// ─── Konstruktor ──────────────────────────────────────────────────────────────
Agent::Agent(const QString &modelPath, QObject *parent)
    : QObject(parent)
    , m_modelPath(modelPath)
    , m_chatModel()
    , m_mcp(this)
    , m_commands(this)
{
    // Teilklassen erzeugen (Komposition)
    // Alle bekommen *this als Referenz — lifetime ist durch Agent garantiert.
    m_chat     = new AgentChat(*this);
    m_plan     = new AgentPlan(*this);
    m_execute  = new AgentExecute(*this);
    m_assemble = new AgentAssemble(*this);

    m_worker = new LlamaWorker();
    m_worker->moveToThread(&m_workerThread);

    qRegisterMetaType<LlamaWorker::SamplerProfile>();
    qRegisterMetaType<ChatTemplate::Preset>();
    qRegisterMetaType<QVector<ChatMessage>>("QVector<ChatMessage>");
    qRegisterMetaType<AgentMode>();

    connect(m_worker, &LlamaWorker::tokenGenerated,
            this,     &Agent::onTokenReceived);
    connect(m_worker, &LlamaWorker::generationDone,
            this,     &Agent::onGenerationDone);
    connect(m_worker, &LlamaWorker::modelLoaded,
            this,     &Agent::onModelLoaded);
    connect(m_worker, &LlamaWorker::errorOccurred,
            this,     &Agent::onError);
    connect(m_worker, &LlamaWorker::statsUpdate,
            this,     &Agent::onStatsUpdate);
    connect(&m_workerThread, &QThread::finished,
            m_worker, &QObject::deleteLater);
    connect(m_worker, &LlamaWorker::chatTemplateDetected,
            this,     &Agent::onChatTemplateDetected);
    connect(&m_mcp, &McpManager::serverDied, this, [this](const QString &name) {
        emit appendTools(
            QString("MCP-Server gestorben: %1").arg(name), "error");
    });
}

Agent::~Agent()
{
    if (m_worker) m_worker->stopGeneration();
    m_workerThread.quit();
    m_workerThread.wait(3000);

    // Teilklassen löschen (keine Qt-Parent-Ownership nötig, da kein QObject)
    delete m_chat;
    delete m_plan;
    delete m_execute;
    delete m_assemble;
}

// ─── start ────────────────────────────────────────────────────────────────────
void Agent::start()
{
    m_workerThread.start();

    AppConfig &cfg = AppConfig::instance();
    m_logger.setEnabled(cfg.chatLoggingEnabled());
    if (!cfg.chatLogDir().isEmpty())
        m_logger.setLogDir(cfg.chatLogDir());

    connect(&cfg, &AppConfig::chatLoggingChanged,
            &m_logger, &ChatLogger::setEnabled);

    QString dbPath = cfg.taskDbPath();
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    m_taskTree.setDbPath(dbPath);

    QString binDir = QCoreApplication::applicationDirPath();
    m_mcp.addServer(binDir + "/mcp-servers/filesystem/llamaqt-filesystem");
    m_mcp.addServer(binDir + "/mcp-servers/sysinfo/llamaqt-sysinfo");
    m_mcp.addServer(binDir + "/mcp-servers/compile/llamaqt-compile");
    m_mcp.addServer(binDir + "/mcp-servers/websearch/llamaqt-websearch");
    m_mcp.addServer(binDir + "/mcp-servers/tree-sitter/llamaqt-treesitter");
    m_mcp.addServer(binDir + "/mcp-servers/clang/llamaqt-clang");

    m_mcp.startAll([this](bool ok, QStringList errors) {
        if (!ok)
            for (const QString &e : errors)
                emit appendTools("MCP Fehler: " + e, "error");
        QMetaObject::invokeMethod(m_worker, "initialize",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, m_modelPath));
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// SYSTEM-PROMPT + CHAT-TEMPLATE
// ═════════════════════════════════════════════════════════════════════════════

QString Agent::buildFullSystemPrompt() const
{
    QString userPart = AppConfig::instance().userSystemPrompt().trimmed();
    QString mcpPart  = m_mcp.buildToolsSystemPrompt();
    if (userPart.isEmpty()) return mcpPart;
    return userPart + "\n\n" + mcpPart;
}

void Agent::applyChatTemplate()
{
    ChatTemplate::Preset preset = AppConfig::instance().chatTemplatePreset();
    ChatTemplate tmpl;
    if (preset == ChatTemplate::Preset::Auto) {
        tmpl = ChatTemplate::forPreset(m_detectedPreset);
    } else if (preset == ChatTemplate::Preset::Custom) {
        emit appendTools(
            "Custom Chat-Template: Parsing noch nicht implementiert — "
            "Fallback ChatML.", "system");
        tmpl = ChatTemplate::chatML();
    } else {
        tmpl = ChatTemplate::forPreset(preset);
    }
    m_chatModel.setChatTemplate(tmpl);
}

void Agent::onChatTemplateDetected(const QString &jinjaTemplate,
                                    ChatTemplate::Preset detectedPreset)
{
    AppConfig::instance().setDetectedJinjaTemplate(jinjaTemplate);
    m_detectedPreset = detectedPreset;
    applyChatTemplate();

    QString presetName  = ChatTemplate::presetName(detectedPreset);
    ChatTemplate::Preset userChoice = AppConfig::instance().chatTemplatePreset();

    if (jinjaTemplate.isEmpty()) {
        emit appendTools(
            QString("Chat-Template: kein Template im GGUF → <b>%1</b> (Fallback)")
            .arg(presetName.toHtmlEscaped()), "system");
    } else {
        if (userChoice == ChatTemplate::Preset::Auto) {
            emit appendTools(
                QString("Chat-Template (Auto): <b>%1</b> — "
                        "llama_chat_apply_template aktiv")
                .arg(presetName.toHtmlEscaped()), "system");
        } else {
            QString chosenName = ChatTemplate::presetName(userChoice);
            emit appendTools(
                QString("Chat-Template: GGUF=<i>%1</i>, Einstellung=<b>%2</b>")
                .arg(presetName.toHtmlEscaped(), chosenName.toHtmlEscaped()),
                "system");
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SLOTS: Eingaben von der UI
// ═════════════════════════════════════════════════════════════════════════════

void Agent::onUserMessage(const QString &text)
{
    if (text.trimmed().isEmpty() || m_generating) return;

    if (CommandProcessor::isCommand(text)) {
        auto result = m_commands.process(text, m_generating);
        if (result.handled) {
            if (!result.notice.isEmpty())
                emit appendTools(result.notice.toHtmlEscaped(),
                                  result.noticeCssClass);

            if (result.prompt == "__SUMMARIZE__") {
                emit inputEnabled(false);
                emit statusChanged("Zusammenfassen...");
                m_chat->summarizeContext();
                return;
            }
            if (result.prompt.startsWith("__PLAN__:")) {
                QString auftrag = result.prompt.mid(9);
                emit appendChat(
                    QString("<b>Plan-Modus:</b> %1")
                    .arg(text.toHtmlEscaped()), "user");
                m_plan->startPlan(auftrag);
                return;
            }
            if (result.prompt == "__EXECUTE__") {
                if (m_taskTree.isEmpty()) {
                    emit appendTools(
                        "Kein Plan geladen. Bitte zuerst /plan oder /loadDB.",
                        "error");
                    emit inputEnabled(true);
                    return;
                }
                if (m_taskTree.nextPending() == nullptr) {
                    emit appendTools(
                        "Alle Nodes bereits erledigt. "
                        "Neuen Plan erstellen mit /plan oder /loadDB.", "system");
                    emit inputEnabled(true);
                    return;
                }
                emit appendChat(
                    "<b>[System]</b> Execute-Modus gestartet.", "system");
                m_execute->startExecute();
                return;
            }
            if (result.prompt == "__SAVEDB__") {
                bool ok1 = m_taskTree.save();
                bool ok2 = m_executeMemory.save();
                if (ok1 && ok2) {
                    emit appendTools(
                        QString("<b>DB gespeichert:</b> %1 Nodes, %2 Thoughts "
                                "in <code>%3</code>")
                        .arg(m_taskTree.nodeCount())
                        .arg(m_executeMemory.count())
                        .arg(AppConfig::instance().taskDbPath().toHtmlEscaped()),
                        "system");
                } else {
                    emit appendTools(
                        "<b>Fehler beim Speichern der DB.</b>", "error");
                }
                emit inputEnabled(true);
                return;
            }
            if (result.prompt == "__LOADDB__") {
                bool ok1 = m_taskTree.load();
                bool ok2 = m_executeMemory.load();
                if (ok1) {
                    emit taskTreeUpdated();
                    emit appendTools(
                        QString("<b>DB geladen:</b> %1 Nodes, %2 Thoughts "
                                "aus <code>%3</code>")
                        .arg(m_taskTree.nodeCount())
                        .arg(m_executeMemory.count())
                        .arg(AppConfig::instance().taskDbPath().toHtmlEscaped()),
                        "system");
                } else {
                    emit appendTools(
                        "<b>Fehler beim Laden der DB.</b>", "error");
                }
                emit inputEnabled(true);
                return;
            }
            if (result.prompt == "__CODEASSEMBLE__") {
                if (m_taskTree.isEmpty()) {
                    emit appendTools(
                        "Kein Plan geladen. Bitte zuerst /loadDB oder /plan.",
                        "error");
                    emit inputEnabled(true);
                    return;
                }
                m_assemble->assembleProject();
                emit inputEnabled(true);
                return;
            }
            if (result.prompt.isEmpty()) return;

            emit appendChat(
                QString("<b>Du:</b> %1").arg(text.toHtmlEscaped()), "user");
            emit appendChat("<b>Assistent:</b> ", "assistant");

            m_chatModel.addUserMessage(result.prompt);
            m_currentResponse.clear();
            m_thinkBuffer.clear();
            m_inThinkBlock      = false;
            m_generatedTokens   = 0;
            m_continuationCount = 0;
            m_generating        = true;

            emit inputEnabled(false);
            emit statusChanged("Generiere...");
            startGeneration(LlamaWorker::SamplerProfile::Chat);
            return;
        }
    }

    m_chat->checkContextUsage();

    m_logger.logUser(text);
    m_chatModel.addUserMessage(text);
    emit appendChat(QString("<b>Du:</b> %1").arg(text.toHtmlEscaped()), "user");
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
    ++m_sessionId;
    m_generating = false;
    if (m_worker) m_worker->stopGeneration();
    m_toolFailCount.clear();

    if (m_mode == AgentMode::Plan) {
        m_mode = AgentMode::Chat;
        emit modeChanged(m_mode);
        m_chatModel.setSystemPrompt(buildFullSystemPrompt());
    }
    emit inputEnabled(true);
    emit statusChanged("Gestoppt");
}

void Agent::onClearChat()
{
    ++m_sessionId;
    m_generating = false;
    if (m_worker) m_worker->stopGeneration();

    m_mode = AgentMode::Chat;
    emit modeChanged(m_mode);

    m_chatModel.clear();
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock      = false;
    m_generatedTokens   = 0;
    m_totalTokens       = 0;
    m_promptTokens      = 0;
    m_toolFailCount.clear();
    m_planRetryCount    = 0;

    emit appendChat("Chat gelöscht.", "system");
    m_chat->emitStats();
    emit inputEnabled(true);
    emit statusChanged("Bereit");
}

void Agent::onFileSavedByUser(const QString &filePath)
{
    QString name = QFileInfo(filePath).fileName();
    QString notice = QString(
        "[System: User hat <b>%1</b> manuell gespeichert. "
        "Bitte Datei vor weiteren Änderungen neu einlesen.]")
        .arg(name.toHtmlEscaped());
    emit appendChat(notice, "system");
    m_chatModel.addToolResult("editor_notify",
        QString("[User hat '%1' manuell bearbeitet und gespeichert. "
                "Bitte read_file aufrufen bevor du str_replace oder "
                "write_file verwendest.]").arg(name));
    m_logger.logSystem(
        QString("User hat %1 manuell gespeichert.").arg(filePath));
}

void Agent::onPlanApproved()
{
    if (m_mode != AgentMode::Plan) return;
    m_taskTree.save();
    emit appendTools(
        QString("<b>Plan gespeichert:</b> %1 Knoten in <code>%2</code>")
        .arg(m_taskTree.nodeCount())
        .arg(AppConfig::instance().taskDbPath().toHtmlEscaped()),
        "system");
    m_execute->startExecute();
}

void Agent::onPlanRejected()
{
    if (m_mode != AgentMode::Plan) return;
    m_mode = AgentMode::Chat;
    emit modeChanged(m_mode);
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());
    emit appendChat(
        "<b>[System]</b> Plan abgelehnt. Zurück zum Chat-Modus.", "system");
    emit appendTools("Plan abgelehnt vom User.", "system");
    emit inputEnabled(true);
    emit statusChanged("Bereit");
}

// ═════════════════════════════════════════════════════════════════════════════
// PRIVATE SLOTS: Worker-Callbacks
// ═════════════════════════════════════════════════════════════════════════════

void Agent::onModelLoaded()
{
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());
    emit inputEnabled(true);
    emit statusChanged("Modell geladen – bereit");
    emit appendChat(
        "Modell geladen: " + QFileInfo(m_modelPath).fileName(), "system");
    emit appendTools(
        "<b>Sampler-Profile:</b><br>"
        "&nbsp;Chat: Top-K 40 | Temp 0.7 | Top-P 0.95<br>"
        "&nbsp;Tool: Top-K 20 | Temp 0.1 | Top-P 0.50",
        "system");

    QString toolDebug = "<b>MCP Tools:</b><br>";
    int toolCount = 0;
    for (const auto &info : m_mcp.debugToolInfo()) {
        toolDebug += QString("&nbsp;<i>%1</i>: ")
                     .arg(info.serverName.toHtmlEscaped());
        toolDebug += info.toolNames.join(", ").toHtmlEscaped() + "<br>";
        toolCount += info.toolNames.size();
    }
    toolDebug += toolCount == 0
        ? "<b>WARNUNG: Keine Tools!</b>"
        : QString("<b>%1 Tools geladen</b>").arg(toolCount);
    emit appendTools(toolDebug, "system");
}

void Agent::onStatsUpdate(int promptTokens, int ctxSize)
{
    m_promptTokens = promptTokens;
    m_ctxSize      = ctxSize;
    m_chat->emitStats();
}

void Agent::onError(const QString &error)
{
    m_generating = false;
    if (m_mode == AgentMode::Plan) {
        m_mode = AgentMode::Chat;
        emit modeChanged(m_mode);
        m_chatModel.setSystemPrompt(buildFullSystemPrompt());
    }
    emit inputEnabled(true);
    emit appendTools(
        QString("Fehler: %1").arg(error.toHtmlEscaped()), "error");
    emit statusChanged("Fehler");
}

void Agent::onTokenReceived(const QString &token)
{
    ++m_generatedTokens;
    ++m_totalTokens;
    m_currentResponse += token;
    m_chat->filterToken(token);
    if (m_generatedTokens % 10 == 0) m_chat->emitStats();
}

// ─── onGenerationDone ─────────────────────────────────────────────────────────
// FIX C: Doppelter Thoughts-Block entfernt.
// Nur noch ein einziger m_updatingThoughts-Zweig ganz oben.
// Danach saubere Trennung Plan / Execute / Chat.
void Agent::onGenerationDone(const QString &fullResponse)
{
    m_generating = false;
    m_chat->emitStats();

    uint32_t mySession = m_sessionId;

    // ── Thoughts-Update (nach jedem Execute-Node) ─────────────────────────
    // FIX C: Dieser Block existiert jetzt NUR EINMAL.
    // parseFromLlmOutput() ist das robuste Parsing (v2).
    if (m_updatingThoughts) {
        m_updatingThoughts = false;
        m_executeMemory.parseFromLlmOutput(fullResponse);
        m_executeMemory.save();
        emit appendTools(
            QString("<b>Thoughts aktualisiert:</b> %1 Einträge.")
            .arg(m_executeMemory.count()), "system");
        m_execute->advanceExecute();
        return;
    }

    // ── Plan-Modus ────────────────────────────────────────────────────────
    if (m_mode == AgentMode::Plan) {
        if (fullResponse.contains("<plan>") &&
            fullResponse.contains("</plan>")) {
            m_plan->handlePlanJson(fullResponse, mySession);
            return;
        }
        if (fullResponse.contains("<tool_call>") &&
            fullResponse.contains("</tool_call>")) {
            m_chatModel.addAssistantMessage(fullResponse);
            m_plan->handlePlanToolCall(fullResponse, mySession);
            return;
        }
        // Unvollständiger Tool-Call → Continuation
        if (fullResponse.contains("<tool_call>") &&
            !fullResponse.contains("</tool_call>")) {
            ++m_continuationCount;
            if (m_continuationCount > MAX_CONTINUATIONS) {
                m_continuationCount = 0;
                emit appendTools(
                    "Plan: Tool-Call unvollständig nach Fortsetzungen — "
                    "abgebrochen.", "error");
                m_mode = AgentMode::Chat;
                emit modeChanged(m_mode);
                m_chatModel.setSystemPrompt(buildFullSystemPrompt());
                emit inputEnabled(true);
                emit statusChanged("Bereit");
                return;
            }
            m_chatModel.addAssistantMessage(fullResponse);
            m_generating      = true;
            m_generatedTokens = 0;
            startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
        // Prosa → weitermachen (Modell denkt noch nach)
        m_chatModel.addAssistantMessage(fullResponse);
        m_generating      = true;
        m_generatedTokens = 0;
        m_currentResponse.clear();
        m_thinkBuffer.clear();
        m_inThinkBlock = false;
        emit appendChat("<b>Assistent:</b> ", "assistant");
        startGeneration(LlamaWorker::SamplerProfile::Chat);
        return;
    }

    // ── Execute-Modus ─────────────────────────────────────────────────────
    if (m_mode == AgentMode::Execute) {
        if (m_execute->isExecuteToolCall(fullResponse)) {
            m_chatModel.addAssistantMessage(fullResponse);
            m_execute->handleExecuteToolCall(fullResponse, mySession);
            return;
        }
        if (fullResponse.contains("<tool_call>") &&
            !fullResponse.contains("</tool_call>")) {
            ++m_continuationCount;
            if (m_continuationCount > MAX_CONTINUATIONS) {
                m_continuationCount = 0;
                emit appendTools(
                    "Execute: Tool-Call unvollständig — Node als Failed.",
                    "error");
                if (m_currentNode) {
                    m_taskTree.setStatus(m_currentNode, TaskStatus::Failed);
                    m_taskTree.save();
                    emit taskTreeUpdated();
                }
                m_execute->advanceExecute();
                return;
            }
            m_chatModel.addAssistantMessage(fullResponse);
            m_generating = true;
            m_generatedTokens = 0;
            startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
        m_continuationCount = 0;
        m_execute->handleExecuteCode(fullResponse, mySession);
        return;
    }

    // ── Chat-Modus ────────────────────────────────────────────────────────
    if (m_summarizing) {
        m_summarizing = false;
        m_chatModel.clear();
        m_chatModel.setSystemPrompt(buildFullSystemPrompt());
        m_chatModel.addUserMessage(
            QString("[Zusammenfassung der bisherigen Konversation:\n%1]")
            .arg(fullResponse));
        m_chatModel.addAssistantMessage(
            "Verstanden. Ich habe die bisherige Konversation im Überblick.");
        emit appendTools(
            QString("<b>Zusammenfassung erstellt:</b><br>"
                    "<pre style='font-size:10px'>%1</pre>")
            .arg(fullResponse.left(500).toHtmlEscaped() +
                 (fullResponse.length() > 500 ? "..." : "")),
            "system");
        emit inputEnabled(true);
        emit statusChanged("Zusammenfassung fertig — Kontext geleert");
        return;
    }

    if (fullResponse.contains("<tool_call>") &&
        !fullResponse.contains("</tool_call>")) {
        ++m_continuationCount;
        if (m_continuationCount > MAX_CONTINUATIONS) {
            m_continuationCount = 0;
            emit appendTools(
                QString("Tool-Call nach %1 Fortsetzungen unvollständig — "
                        "abgebrochen.").arg(MAX_CONTINUATIONS), "error");
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

    if (fullResponse.contains("<tool_call>") &&
        fullResponse.contains("</tool_call>")) {
        m_chatModel.addAssistantMessage(fullResponse);
        m_chat->handleToolCall(fullResponse, mySession);
        return;
    }

    m_chatModel.addAssistantMessage(fullResponse);
    m_logger.logAssistant(fullResponse);
    emit inputEnabled(true);
    emit statusChanged("Bereit");
}

// ─── startGeneration ──────────────────────────────────────────────────────────
void Agent::startGeneration(LlamaWorker::SamplerProfile profile)
{
    QMetaObject::invokeMethod(m_worker, "generate",
                              Qt::QueuedConnection,
                              Q_ARG(QVector<ChatMessage>, m_chatModel.messages()),
                              Q_ARG(LlamaWorker::SamplerProfile, profile));
}
