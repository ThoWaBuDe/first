#include "Agent/Agent.h"
#include "Agent/AgentChat.h"
#include "Agent/AgentPlan.h"
#include "Agent/AgentExecute.h"
#include "Agent/AgentAssemble.h"
#include "Agent/AgentUtils.h"
#include "Agent/AgentImport.h"   // NEU
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QFileInfo>
#include <QDir>

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

Agent::Agent(const QString &modelPath, QObject *parent)
    : QObject(parent)
    , m_modelPath(modelPath)
    , m_chatModel()
    , m_mcp(this)
    , m_commands(this)
{
    m_chat     = new AgentChat(*this);
    m_plan     = new AgentPlan(*this);
    m_execute  = new AgentExecute(*this);
    m_assemble = new AgentAssemble(*this);
    m_import   = new AgentImport(*this);   // NEU

    m_worker = new LlamaWorker();
    m_worker->moveToThread(&m_workerThread);

    qRegisterMetaType<LlamaWorker::SamplerProfile>();
    qRegisterMetaType<ChatTemplate::Preset>();
    qRegisterMetaType<ToolCallFormat::Preset>();
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
    connect(m_worker, &LlamaWorker::toolCallFormatDetected,
            this,     &Agent::onToolFormatDetected);

    connect(&m_mcp, &McpManager::serverDied, this, [this](const QString &name) {
        emit appendTools(
            QString("MCP-Server gestorben: %1").arg(name), "error");
    });
    connect(&m_mcp, &McpManager::serverRestarting,
            this, [this](const QString &name, int attempt, int delaySec) {
        emit appendTools(
            QString("<b>MCP Neustart:</b> <i>%1</i> — Versuch %2/3 in %3s...")
            .arg(name.toHtmlEscaped()).arg(attempt).arg(delaySec), "system");
    });
    connect(&m_mcp, &McpManager::serverRestored,
            this, [this](const QString &name) {
        emit appendTools(
            QString("<b>MCP wiederhergestellt:</b> <i>%1</i>")
            .arg(name.toHtmlEscaped()), "system");
    });
    connect(&m_mcp, &McpManager::serverGaveUp,
            this, [this](const QString &name) {
        emit appendTools(
            QString("<b>MCP aufgegeben:</b> <i>%1</i> — Bitte LlamaQt neu starten.")
            .arg(name.toHtmlEscaped()), "error");
    });
}

Agent::~Agent()
{
    if (m_worker) m_worker->stopGeneration();
    m_workerThread.quit();
    m_workerThread.wait(3000);
    delete m_chat;
    delete m_plan;
    delete m_execute;
    delete m_assemble;
    delete m_import;   // NEU
}

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

QString Agent::buildFullSystemPrompt() const
{
    QString userPart = AppConfig::instance().userSystemPrompt().trimmed();
    QString mcpPart  = m_mcp.buildToolsSystemPrompt(m_activeToolFormat);
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
            "Custom Chat-Template: Parsing noch nicht implementiert — Fallback ChatML.",
            "system");
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
                QString("Chat-Template (Auto): <b>%1</b>")
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

void Agent::onToolFormatDetected(ToolCallFormat::Preset detectedFormat,
                                  const QString &source)
{
    AppConfig &cfg = AppConfig::instance();
    ToolCallFormat::Preset userChoice = cfg.toolCallFormatPreset();

    ToolCallFormat::Preset effective;
    if (userChoice == ToolCallFormat::Preset::Auto) {
        effective = detectedFormat;
    } else {
        effective = userChoice;
    }

    m_activeToolFormat = effective;
    cfg.setEffectiveToolCallFormat(effective);

    m_chatModel.setSystemPrompt(buildFullSystemPrompt());

    QString effectiveName = ToolCallFormat::presetName(effective);

    if (userChoice == ToolCallFormat::Preset::Auto) {
        emit appendTools(
            QString("Tool-Format (Auto via %1): <b>%2</b>")
            .arg(source.toHtmlEscaped(), effectiveName.toHtmlEscaped()),
            "system");
    } else {
        emit appendTools(
            QString("Tool-Format: erkannt=<i>%1</i>, eingestellt=<b>%2</b>")
            .arg(ToolCallFormat::presetName(detectedFormat).toHtmlEscaped(),
                 effectiveName.toHtmlEscaped()),
            "system");
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
                    QString("<b>Plan-Modus:</b> %1").arg(text.toHtmlEscaped()), "user");
                m_plan->startPlan(auftrag);
                return;
            }
            if (result.prompt == "__EXECUTE__") {
                if (m_taskTree.isEmpty()) {
                    emit appendTools("Kein Plan geladen.", "error");
                    emit inputEnabled(true);
                    return;
                }
                if (m_taskTree.nextPending() == nullptr) {
                    emit appendTools("Alle Nodes bereits erledigt.", "system");
                    emit inputEnabled(true);
                    return;
                }
                emit appendChat("<b>[System]</b> Execute-Modus gestartet.", "system");
                m_execute->startExecute();
                return;
            }
            if (result.prompt == "__SAVEDB__") {
                bool ok1 = m_taskTree.save();
                bool ok2 = m_executeMemory.save();
                if (ok1 && ok2) {
                    emit appendTools(
                        QString("<b>DB gespeichert:</b> %1 Nodes, %2 Thoughts")
                        .arg(m_taskTree.nodeCount())
                        .arg(m_executeMemory.count()), "system");
                } else {
                    emit appendTools("<b>Fehler beim Speichern der DB.</b>", "error");
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
                        QString("<b>DB geladen:</b> %1 Nodes, %2 Thoughts")
                        .arg(m_taskTree.nodeCount())
                        .arg(m_executeMemory.count()), "system");
                } else {
                    emit appendTools("<b>Fehler beim Laden der DB.</b>", "error");
                }
                emit inputEnabled(true);
                return;
            }
            if (result.prompt == "__CODEASSEMBLE__") {
                if (m_taskTree.isEmpty()) {
                    emit appendTools("Kein Plan geladen.", "error");
                    emit inputEnabled(true);
                    return;
                }
                m_assemble->assembleProject();
                emit inputEnabled(true);
                return;
            }
            // NEU: /import → AgentImport (nicht mehr AgentChat)
            if (result.prompt.startsWith("__IMPORT__:")) {
                QString importPath = result.prompt.mid(11);
                emit appendChat(
                    QString("<b>Import:</b> %1").arg(importPath.toHtmlEscaped()),
                    "user");
                m_mode = AgentMode::Plan;
                emit modeChanged(m_mode);
                m_import->startImport(importPath);
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
    m_generating        = false;
    m_updatingThoughts  = false;
    m_continuationCount = 0;
    m_pendingToolCalls.clear();
    m_pendingToolIdx = 0;

    if (m_worker) m_worker->stopGeneration();
    m_toolFailCount.clear();

    if (m_mode == AgentMode::Execute || m_mode == AgentMode::Plan) {
        if (m_currentNode && m_currentNode->status == TaskStatus::Running) {
            m_taskTree.setStatus(m_currentNode, TaskStatus::Pending);
            m_taskTree.save();
            emit taskTreeUpdated();
        }
        m_currentNode = nullptr;
    }

    m_mode = AgentMode::Chat;
    emit modeChanged(m_mode);
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());

    emit inputEnabled(true);
    emit statusChanged("Gestoppt");
}

void Agent::onClearChat()
{
    ++m_sessionId;
    m_generating        = false;
    m_updatingThoughts  = false;
    m_pendingToolCalls.clear();
    m_pendingToolIdx = 0;

    if (m_worker) m_worker->stopGeneration();

    if (m_currentNode && m_currentNode->status == TaskStatus::Running) {
        m_taskTree.setStatus(m_currentNode, TaskStatus::Pending);
        m_taskTree.save();
    }
    m_currentNode = nullptr;

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
    m_continuationCount = 0;

    emit appendChat("Chat gelöscht.", "system");
    m_chat->emitStats();
    emit inputEnabled(true);
    emit statusChanged("Bereit");
    emit taskTreeUpdated();
}

void Agent::onFileSavedByUser(const QString &filePath)
{
    QString name = QFileInfo(filePath).fileName();
    emit appendChat(
        QString("[System: User hat <b>%1</b> manuell gespeichert.]")
        .arg(name.toHtmlEscaped()), "system");
    m_chatModel.addToolResult("editor_notify",
        QString("[User hat '%1' manuell bearbeitet und gespeichert. "
                "Bitte read_file aufrufen bevor du str_replace verwendest.]").arg(name));
}

void Agent::onPlanApproved()
{
    if (m_mode != AgentMode::Plan) return;
    m_taskTree.save();
    emit appendTools(
        QString("<b>Plan gespeichert:</b> %1 Knoten").arg(m_taskTree.nodeCount()),
        "system");
    m_execute->startExecute();
}

void Agent::onPlanRejected()
{
    if (m_mode != AgentMode::Plan) return;
    m_mode = AgentMode::Chat;
    emit modeChanged(m_mode);
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());
    emit appendChat("<b>[System]</b> Plan abgelehnt.", "system");
    emit inputEnabled(true);
    emit statusChanged("Bereit");
}

void Agent::onModelLoaded()
{
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());
    emit inputEnabled(true);
    emit statusChanged("Modell geladen – bereit");
    emit appendChat(
        "Modell geladen: " + QFileInfo(m_modelPath).fileName(), "system");

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
void Agent::onGenerationDone(const QString &fullResponse)
{
    m_generating = false;
    m_chat->emitStats();

    uint32_t mySession = m_sessionId;

    auto hasToolCall = [&]() {
        return ToolCallFormat::containsToolCall(fullResponse, m_activeToolFormat);
    };
    auto isComplete = [&]() {
        return ToolCallFormat::isCompleteToolCall(fullResponse, m_activeToolFormat);
    };

    // ── Thoughts-Update ───────────────────────────────────────────────────
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

    // ── Import-Modus (NEU) ────────────────────────────────────────────────
    // Import läuft im Plan-Modus, aber m_import->isImportMode() unterscheidet
    // ihn vom normalen /plan-Befehl.
    if (m_import->isImportMode()) {
        if (hasToolCall() && isComplete()) {
            m_chatModel.addAssistantMessage(fullResponse);
            m_import->handleImportToolCall(fullResponse, mySession);
            return;
        }
        if (hasToolCall() && !isComplete()) {
            ++m_continuationCount;
            if (m_continuationCount > MAX_CONTINUATIONS) {
                m_continuationCount = 0;
                emit appendTools(
                    "Import: Tool-Call unvollständig — nächste Datei.", "error");
                if (m_currentNode) {
                    m_taskTree.setStatus(m_currentNode, TaskStatus::Failed);
                    m_taskTree.save();
                    emit taskTreeUpdated();
                    m_currentNode = nullptr;
                }
                m_import->advanceImport();
                return;
            }
            m_chatModel.addAssistantMessage(fullResponse);
            m_generating      = true;
            m_generatedTokens = 0;
            startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
        // Kein Tool-Call → Ergebnis verarbeiten (plan_done o.ä.)
        m_continuationCount = 0;
        m_import->handleImportResult(fullResponse, mySession);
        return;
    }

    // ── Plan-Modus ────────────────────────────────────────────────────────
    if (m_mode == AgentMode::Plan) {
        if (fullResponse.contains("<plan>") && fullResponse.contains("</plan>")) {
            m_plan->handlePlanJson(fullResponse, mySession);
            return;
        }
        if (hasToolCall() && isComplete()) {
            m_chatModel.addAssistantMessage(fullResponse);
            m_plan->handlePlanToolCall(fullResponse, mySession);
            return;
        }
        if (hasToolCall() && !isComplete()) {
            ++m_continuationCount;
            if (m_continuationCount > MAX_CONTINUATIONS) {
                m_continuationCount = 0;
                emit appendTools("Plan: Tool-Call unvollständig — abgebrochen.", "error");
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
        if (hasToolCall() && !isComplete()) {
            ++m_continuationCount;
            if (m_continuationCount > MAX_CONTINUATIONS) {
                m_continuationCount = 0;
                emit appendTools("Execute: Tool-Call unvollständig — Node als Failed.", "error");
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
            QString("[Zusammenfassung:\n%1]").arg(fullResponse));
        m_chatModel.addAssistantMessage(
            "Verstanden. Ich habe die bisherige Konversation im Überblick.");
        emit appendTools(
            QString("<b>Zusammenfassung:</b><br><pre style='font-size:10px'>%1</pre>")
            .arg(fullResponse.left(500).toHtmlEscaped()), "system");
        emit inputEnabled(true);
        emit statusChanged("Zusammenfassung fertig");
        return;
    }

    if (hasToolCall() && !isComplete()) {
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

    if (hasToolCall() && isComplete()) {
        m_chatModel.addAssistantMessage(fullResponse);
        m_chat->handleToolCall(fullResponse, mySession);
        return;
    }

    m_chatModel.addAssistantMessage(fullResponse);
    m_logger.logAssistant(fullResponse);
    emit inputEnabled(true);
    emit statusChanged("Bereit");
}

void Agent::startGeneration(LlamaWorker::SamplerProfile profile)
{
    QMetaObject::invokeMethod(m_worker, "generate",
                              Qt::QueuedConnection,
                              Q_ARG(QVector<ChatMessage>, m_chatModel.messages()),
                              Q_ARG(LlamaWorker::SamplerProfile, profile));
}
