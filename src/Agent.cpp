#include "Agent.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>
#include <QFileInfo>
#include <QDir>

const QStringList Agent::PLAN_ALLOWED_TOOLS = {
    "read_file",
    "list_dir",
    "get_symbol",
    "get_project_index",
    "rebuild_index",
    "get_time",
    "sys_info",
    "disk_free",
    "get_pwd",
};

const QStringList Agent::EXECUTE_ALLOWED_TOOLS = {
    // Lese-Tools
    "read_file",
    "list_dir",
    "get_symbol",
    "get_project_index",
    "get_function_body",
    "get_class_members",
    "get_includes",
    "check_syntax",
    "read_multiple_files",
    "grep_code",
    "search_code",
    // Web
    "web_search",
    // System (read-only)
    "get_time",
    "sys_info",
    "get_pwd",
    "disk_free",
};

// ─── Konstruktor ──────────────────────────────────────────────────────────────
Agent::Agent(const QString &modelPath, QObject *parent)
    : QObject(parent)
    , m_modelPath(modelPath)
    , m_chatModel()
    , m_mcp(this)
    , m_commands(this)
{
    m_worker = new LlamaWorker();
    m_worker->moveToThread(&m_workerThread);

    qRegisterMetaType<LlamaWorker::SamplerProfile>();
    qRegisterMetaType<ChatTemplate::Preset>();
    qRegisterMetaType<QVector<ChatMessage>>("QVector<ChatMessage>");
    qRegisterMetaType<AgentMode>();

    connect(m_worker, &LlamaWorker::tokenGenerated,  this, &Agent::onTokenReceived);
    connect(m_worker, &LlamaWorker::generationDone,  this, &Agent::onGenerationDone);
    connect(m_worker, &LlamaWorker::modelLoaded,     this, &Agent::onModelLoaded);
    connect(m_worker, &LlamaWorker::errorOccurred,   this, &Agent::onError);
    connect(m_worker, &LlamaWorker::statsUpdate,     this, &Agent::onStatsUpdate);
    connect(&m_workerThread, &QThread::finished,     m_worker, &QObject::deleteLater);
    connect(m_worker, &LlamaWorker::chatTemplateDetected,
            this,     &Agent::onChatTemplateDetected);

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
            "Custom Chat-Template: Parsing noch nicht implementiert — Fallback ChatML.", "system");
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

    QString presetName = ChatTemplate::presetName(detectedPreset);
    ChatTemplate::Preset userChoice = AppConfig::instance().chatTemplatePreset();

    if (jinjaTemplate.isEmpty()) {
        emit appendTools(
            QString("Chat-Template: kein Template im GGUF → <b>%1</b> (Fallback)")
            .arg(presetName.toHtmlEscaped()), "system");
    } else {
        if (userChoice == ChatTemplate::Preset::Auto) {
            emit appendTools(
                QString("Chat-Template (Auto): <b>%1</b> — llama_chat_apply_template aktiv")
                .arg(presetName.toHtmlEscaped()), "system");
        } else {
            QString chosenName = ChatTemplate::presetName(userChoice);
            emit appendTools(
                QString("Chat-Template: GGUF=<i>%1</i>, Einstellung=<b>%2</b>")
                .arg(presetName.toHtmlEscaped(), chosenName.toHtmlEscaped()), "system");
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
                emit appendTools(result.notice.toHtmlEscaped(), result.noticeCssClass);

            if (result.prompt == "__SUMMARIZE__") {
                emit inputEnabled(false);
                emit statusChanged("Zusammenfassen...");
                summarizeContext();
                return;
            }

            if (result.prompt.startsWith("__PLAN__:")) {
                QString auftrag = result.prompt.mid(9);
                emit appendChat(
                    QString("<b>Plan-Modus:</b> %1").arg(text.toHtmlEscaped()), "user");
                startPlan(auftrag);
                return;
            }
            // ─── Execute-Modus ────────────────────────────────────────────────────
            // /execute → Agent::startExecute()
            if (result.prompt == "__EXECUTE__") {
                if (m_taskTree.isEmpty()) {
                    emit appendTools(
                        "Kein Plan geladen. Bitte zuerst /plan oder /loadDB ausführen.",
                        "error");
                    emit inputEnabled(true);
                    return;
                }
                if (m_taskTree.nextPending() == nullptr) {
                    emit appendTools(
                        "Alle Nodes bereits erledigt. "
                        "Neuen Plan erstellen mit /plan oder /loadDB.",
                        "system");
                    emit inputEnabled(true);
                    return;
                }
                emit appendChat("<b>[System]</b> Execute-Modus gestartet.", "system");
                startExecute();
                return;
            }

            // ─── SaveDB ───────────────────────────────────────────────────────────
            // /saveDB → TaskTree + ExecuteMemory speichern
            if (result.prompt == "__SAVEDB__") {
                bool ok1 = m_taskTree.save();
                bool ok2 = m_executeMemory.save();
                if (ok1 && ok2) {
                    emit appendTools(
                        QString("<b>DB gespeichert:</b> %1 Nodes, %2 Thoughts in <code>%3</code>")
                            .arg(m_taskTree.nodeCount())
                            .arg(m_executeMemory.count())
                            .arg(AppConfig::instance().taskDbPath().toHtmlEscaped()),
                        "system");
                } else {
                    emit appendTools("<b>Fehler beim Speichern der DB.</b>", "error");
                }
                emit inputEnabled(true);
                return;
            }

            // ─── LoadDB ───────────────────────────────────────────────────────────
            // /loadDB → TaskTree + ExecuteMemory laden, PlannerDock aktualisieren
            if (result.prompt == "__LOADDB__") {
                bool ok1 = m_taskTree.load();
                bool ok2 = m_executeMemory.load();
                if (ok1) {
                    emit taskTreeUpdated();
                    emit appendTools(
                        QString("<b>DB geladen:</b> %1 Nodes, %2 Thoughts aus <code>%3</code>")
                            .arg(m_taskTree.nodeCount())
                            .arg(m_executeMemory.count())
                            .arg(AppConfig::instance().taskDbPath().toHtmlEscaped()),
                        "system");
                } else {
                    emit appendTools("<b>Fehler beim Laden der DB.</b>", "error");
                }
                emit inputEnabled(true);
                return;
            }
            if (result.prompt.isEmpty()) return;

            if (result.prompt == "__CODEASSEMBLE__") {
            if (m_taskTree.isEmpty()) {
            emit appendTools(
                               "Kein Plan geladen. Bitte zuerst /loadDB oder /plan ausführen.",
                            "error");
                        emit inputEnabled(true);
                        return;
                    }
                    assembleProject();
                    emit inputEnabled(true);
                    return;
            }
            emit appendChat(QString("<b>Du:</b> %1").arg(text.toHtmlEscaped()), "user");
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

    checkContextUsage();

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
    emitStats();
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
    m_logger.logSystem(QString("User hat %1 manuell gespeichert.").arg(filePath));
}

// ═════════════════════════════════════════════════════════════════════════════
// onPlanApproved() — GEÄNDERT: wechselt jetzt zu Execute statt Chat
// Ersetzt die bisherige Implementierung komplett.
// ═════════════════════════════════════════════════════════════════════════════

void Agent::onPlanApproved()
{
    if (m_mode != AgentMode::Plan) return;

    m_taskTree.save();
    emit appendTools(
        QString("<b>Plan gespeichert:</b> %1 Knoten in <code>%2</code>")
            .arg(m_taskTree.nodeCount())
            .arg(AppConfig::instance().taskDbPath().toHtmlEscaped()),
        "system");

    // Execute-Modus starten
    startExecute();
}

void Agent::onPlanRejected()
{
    if (m_mode != AgentMode::Plan) return;

    m_mode = AgentMode::Chat;
    emit modeChanged(m_mode);
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());

    emit appendChat("<b>[System]</b> Plan abgelehnt. Zurück zum Chat-Modus.", "system");
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
    emit appendChat("Modell geladen: " + QFileInfo(m_modelPath).fileName(), "system");
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

    if (m_mode == AgentMode::Plan) {
        m_mode = AgentMode::Chat;
        emit modeChanged(m_mode);
        m_chatModel.setSystemPrompt(buildFullSystemPrompt());
    }

    emit inputEnabled(true);
    emit appendTools(QString("Fehler: %1").arg(error.toHtmlEscaped()), "error");
    emit statusChanged("Fehler");
}

void Agent::onTokenReceived(const QString &token)
{
    ++m_generatedTokens;
    ++m_totalTokens;
    m_currentResponse += token;
    filterToken(token);
    if (m_generatedTokens % 10 == 0) emitStats();
}

void Agent::onGenerationDone(const QString &fullResponse)
{
    m_generating = false;
    emitStats();

    uint32_t mySession = m_sessionId;

    if (m_updatingThoughts) {
        m_updatingThoughts = false;

        // Robustes Parsing: <think>-Blöcke, Nummerierung, Artefakte entfernen
        m_executeMemory.parseFromLlmOutput(fullResponse);
        m_executeMemory.save();

        emit appendTools(
            QString("<b>Thoughts aktualisiert:</b> %1 Einträge.")
                .arg(m_executeMemory.count()), "system");

        advanceExecute();
        return;
    }

    // ─── Plan-Modus ───────────────────────────────────────────────────────
    if (m_mode == AgentMode::Plan) {
        if (fullResponse.contains("<plan>") && fullResponse.contains("</plan>")) {
            handlePlanJson(fullResponse, mySession);
            return;
        }
        if (fullResponse.contains("<tool_call>") && fullResponse.contains("</tool_call>")) {
            m_chatModel.addAssistantMessage(fullResponse);
            handlePlanToolCall(fullResponse, mySession);
            return;
        }
        if (fullResponse.contains("<tool_call>") && !fullResponse.contains("</tool_call>")) {
            ++m_continuationCount;
            if (m_continuationCount > MAX_CONTINUATIONS) {
                m_continuationCount = 0;
                emit appendTools(
                    "Plan: Tool-Call unvollständig nach Fortsetzungen — abgebrochen.", "error");
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
        emit appendChat(fullResponse.toHtmlEscaped(), "assistant");
        m_generating      = true;
        m_generatedTokens = 0;
        m_currentResponse.clear();
        m_thinkBuffer.clear();
        m_inThinkBlock = false;
        emit appendChat("<b>Assistent:</b> ", "assistant");
        startGeneration(LlamaWorker::SamplerProfile::Chat);
        return;
    }

    // ─── Thoughts-Update (nach jedem Execute-Node) ────────────────────────
    // m_updatingThoughts ist true wenn updateThoughts() einen LLM-Aufruf
    // gestartet hat. Der Output ist die neue Thoughts-Liste.
    if (m_updatingThoughts) {
        m_updatingThoughts = false;

        // Thoughts-Liste aus Response parsen
        // Format: eine Erkenntnis pro Zeile, kein Markdown, kein Prefix
        QStringList newThoughts;
        for (const QString &line : fullResponse.split('\n', Qt::SkipEmptyParts)) {
            QString entry = line.trimmed();
            // Nummerierung entfernen falls das Modell sie trotzdem hinzufügt
            // "1. QTimer muss..." → "QTimer muss..."
            if (entry.length() > 2 && entry[0].isDigit() && entry[1] == '.') {
                entry = entry.mid(2).trimmed();
            }
            if (!entry.isEmpty() && entry.length() > 3)
                newThoughts << entry;
        }

        m_executeMemory.setEntries(newThoughts);
        m_executeMemory.save();

        emit appendTools(
            QString("<b>Thoughts aktualisiert:</b> %1 Einträge.")
                .arg(newThoughts.size()), "system");

        // Nächsten Node starten
        advanceExecute();
        return;
    }

    // ─── Execute-Modus ────────────────────────────────────────────────────
    if (m_mode == AgentMode::Execute) {
        if (isExecuteToolCall(fullResponse)) {
            m_chatModel.addAssistantMessage(fullResponse);
            handleExecuteToolCall(fullResponse, mySession);
            return;
        }
        if (fullResponse.contains("<tool_call>") && !fullResponse.contains("</tool_call>")) {
            ++m_continuationCount;
            if (m_continuationCount > MAX_CONTINUATIONS) {
                m_continuationCount = 0;
                emit appendTools("Execute: Tool-Call unvollständig — Node als Failed.", "error");
                if (m_currentNode) {
                    m_taskTree.setStatus(m_currentNode, TaskStatus::Failed);
                    m_taskTree.save();
                    emit taskTreeUpdated();
                }
                advanceExecute();
                return;
            }
            m_chatModel.addAssistantMessage(fullResponse);
            m_generating = true; m_generatedTokens = 0;
            startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
        m_continuationCount = 0;
        handleExecuteCode(fullResponse, mySession);
        return;
    }

    // ─── Chat-Modus ───────────────────────────────────────────────────────
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

    if (fullResponse.contains("<tool_call>") && fullResponse.contains("</tool_call>")) {
        m_chatModel.addAssistantMessage(fullResponse);
        handleToolCall(fullResponse, mySession);
        return;
    }

    m_chatModel.addAssistantMessage(fullResponse);
    m_logger.logAssistant(fullResponse);
    emit inputEnabled(true);
    emit statusChanged("Bereit");
}

// ═════════════════════════════════════════════════════════════════════════════
// PLAN-MODUS: PRIVATE METHODEN
// ═════════════════════════════════════════════════════════════════════════════

// ─── startPlan ───────────────────────────────────────────────────────────────
void Agent::startPlan(const QString &auftrag)
{
    m_mode = AgentMode::Plan;
    emit modeChanged(m_mode);

    // RAM-Tree leeren — jeder /plan-Aufruf startet frisch.
    // DB bleibt erhalten (wird bei Bestätigung überschrieben).
    // Analogie AVR: Buffer-Reset vor neuem DMA-Transfer.
    m_taskTree.clear();

    m_planRetryCount    = 0;
    m_continuationCount = 0;

    m_chatModel.clear();
    m_chatModel.setSystemPrompt(buildPlannerSystemPrompt(auftrag));

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock    = false;
    m_generatedTokens = 0;
    m_generating      = true;

    m_chatModel.addUserMessage(
        QString("Bitte analysiere das Projekt und erstelle einen Plan für: %1").arg(auftrag));

    emit appendChat("<b>Assistent (Plan-Analyse):</b> ", "assistant");
    emit appendTools(
        QString("<b>Plan-Modus gestartet:</b> %1<br>"
                "<small>Erlaubte Tools: read_file, list_dir, get_symbol, ...</small>")
        .arg(auftrag.toHtmlEscaped()), "system");

    emit inputEnabled(false);
    emit statusChanged("Plan-Analyse läuft...");
    startGeneration(LlamaWorker::SamplerProfile::Chat);
}

// ─── buildPlannerSystemPrompt ────────────────────────────────────────────────
QString Agent::buildPlannerSystemPrompt(const QString &auftrag) const
{
    Q_UNUSED(auftrag)
    return QString(
        "Du bist ein Planungs-Agent fuer C++/Qt6 Projekte.\n"
        "\n"
        "DEINE AUFGABE:\n"
        "1. Analysiere das Projekt mit den verfuegbaren Lese-Tools\n"
        "2. Erstelle danach GENAU EINEN <plan>...</plan> Block\n"
        "\n"
        "ERLAUBTE TOOLS (nur Lesen, kein Schreiben!):\n"
        "\n"
        "read_file -- Datei lesen\n"
        "  <tool_call>{\"name\": \"read_file\", \"arguments\": {\"path\": \"datei.cpp\"}}</tool_call>\n"
        "  Mit Range: {\"path\": \"datei.cpp\", \"start_line\": 1, \"end_line\": 50}\n"
        "\n"
        "list_dir -- Verzeichnis auflisten\n"
        "  <tool_call>{\"name\": \"list_dir\", \"arguments\": {\"path\": \".\"}}</tool_call>\n"
        "\n"
        "get_symbol -- Symbol in Datei suchen\n"
        "  <tool_call>{\"name\": \"get_symbol\", \"arguments\": {\"path\": \"datei.cpp\", \"symbol\": \"MyClass\"}}</tool_call>\n"
        "\n"
        "get_project_index -- Projektuebersicht\n"
        "  <tool_call>{\"name\": \"get_project_index\", \"arguments\": {}}</tool_call>\n"
        "\n"
        "get_time, sys_info, disk_free, get_pwd -- Systeminfos\n"
        "\n"
        "VERBOTEN: write_file, str_replace, append_file, cmake_build, check_run\n"
        "\n"
        "HIERARCHIE-REGELN:\n"
        "level 0 (H0) -- Gesamtziel (genau 1x, die Wurzel)\n"
        "level 1 (H1) -- Dateigruppe oder Modul\n"
        "level 2 (H2) -- Einzelne Datei/Klasse\n"
        "level 3 (H3) -- Implementierungsschritt innerhalb einer Datei\n"
        "level 4+ (H4+) -- Feinere Details wenn noetig\n"
        "\n"
        "H3-REGEL:\n"
        "  Wenn ein H2-Knoten Implementierungsschritte enthaelt\n"
        "  (Methoden, Algorithmen, Logik) -- gib diese als children[] mit level=3 aus.\n"
        "  Reine Interface-Dateien (.h ohne Implementierung) duerfen Blatt auf H2 bleiben.\n"
        "\n"
        "scope-BEDEUTUNG:\n"
        "  external -- oeffentliches Interface (public, .h Deklarationen)\n"
        "  internal -- Implementierungsdetail (private, .cpp Definitionen)\n"
        "\n"
        "dependsOn-VERWENDUNG:\n"
        "  Liste von Titeln anderer Knoten auf die dieser Knoten angewiesen ist.\n"
        "  Beispiel: 'GameState.cpp' haengt von 'GameState.h' ab.\n"
        "\n"
        "PLAN-FORMAT:\n"
        "<plan>\n"
        "{\n"
        "  \"goal\": \"Kurztitel\",\n"
        "  \"children\": [\n"
        "    {\n"
        "      \"title\": \"H1-Gruppe\",\n"
        "      \"level\": 1,\n"
        "      \"scope\": \"external\",\n"
        "      \"description\": \"Was hier zu tun ist.\",\n"
        "      \"dependsOn\": [],\n"
        "      \"children\": [\n"
        "        {\n"
        "          \"title\": \"GameState.cpp\",\n"
        "          \"level\": 2,\n"
        "          \"scope\": \"internal\",\n"
        "          \"description\": \"Implementierung aller Methoden\",\n"
        "          \"dependsOn\": [\"GameState.h\"],\n"
        "          \"children\": [\n"
        "            {\n"
        "              \"title\": \"makeMove() implementieren\",\n"
        "              \"level\": 3,\n"
        "              \"scope\": \"internal\",\n"
        "              \"description\": \"Prueft Gueltigkeit, setzt Feld, wechselt Spieler\",\n"
        "              \"dependsOn\": [\"GameState.h\"],\n"
        "              \"children\": []\n"
        "            }\n"
        "          ]\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n"
        "</plan>\n"
        "\n"
        "REGELN:\n"
        "- Jeder Knoten hat: title, level, scope, description, dependsOn, children\n"
        "- dependsOn: [] wenn keine Abhaengigkeiten\n"
        "- children: [] wenn Blatt\n"
        "- Keine Prosa nach </plan>\n"
        "\n"
        "Antworte auf Deutsch."
    );
}

// ─── handlePlanToolCall ───────────────────────────────────────────────────────
void Agent::handlePlanToolCall(const QString &fullResponse, uint32_t sessionId)
{
    int start     = fullResponse.indexOf("<tool_call>") + 11;
    int end       = fullResponse.indexOf("</tool_call>", start);
    QString block = fullResponse.mid(start, end - start).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);

    if (doc.isNull()) {
        QString repaired = repairJson(block);
        if (!repaired.isEmpty())
            doc = QJsonDocument::fromJson(repaired.toUtf8());
        else {
            m_chatModel.addToolResult("json_error",
                QString("[SYSTEM: Ungültiges JSON im Tool-Call. Fehler: %1. "
                        "Bitte korrektes JSON verwenden.]").arg(pe.errorString()));
            m_generating      = true;
            m_generatedTokens = 0;
            m_currentResponse.clear();
            m_thinkBuffer.clear();
            m_inThinkBlock = false;
            emit appendChat("<b>Assistent (Plan-Analyse):</b> ", "assistant");
            startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
    }

    QString     toolName = doc.object().value("name").toString();
    QJsonObject toolArgs = doc.object().value("arguments").toObject();

    emit appendTools(
        QString("<b>Plan-Tool: %1</b><br><pre>%2</pre>")
        .arg(toolName.toHtmlEscaped(),
             QString::fromUtf8(QJsonDocument(toolArgs)
                               .toJson(QJsonDocument::Indented)).toHtmlEscaped()),
        "tool");

    if (!PLAN_ALLOWED_TOOLS.contains(toolName)) {
        QString errMsg = QString(
            "[SYSTEM: Tool '%1' ist im Plan-Modus VERBOTEN. "
            "Nur Lese-Tools erlaubt. Erstelle den <plan>...</plan> Block.]")
            .arg(toolName);
        emit appendTools(
            QString("Plan-Whitelist: <b>%1</b> verboten.").arg(toolName.toHtmlEscaped()),
            "error");
        m_chatModel.addToolResult(toolName, errMsg);
        m_generating      = true;
        m_generatedTokens = 0;
        m_currentResponse.clear();
        m_thinkBuffer.clear();
        m_inThinkBlock = false;
        emit appendChat("<b>Assistent (Plan-Analyse):</b> ", "assistant");
        startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    if (!m_mcp.containsTool(toolName)) {
        m_chatModel.addToolResult(toolName,
            QString("Fehler: Tool '%1' nicht verfügbar.").arg(toolName));
        m_generating      = true;
        m_generatedTokens = 0;
        startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    emit statusChanged(QString("Plan-Tool: %1...").arg(toolName));
    QString tKey = toolCallKey(toolName, toolArgs);

    m_mcp.callTool(toolName, toolArgs,
        [this, toolName, tKey, sessionId](QString result, QString error) {
            if (sessionId != m_sessionId) return;

            QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);
            bool    isErr      = !error.isEmpty();

            if (!isErr && toolResult.length() > AppConfig::instance().maxToolResultChars()) {
                int maxChars = AppConfig::instance().maxToolResultChars();
                int cut = toolResult.lastIndexOf('\n', maxChars);
                if (cut < maxChars / 2) cut = maxChars;
                toolResult = toolResult.left(cut)
                    + QString("\n\n[... gekürzt: %1 von %2 Zeichen.]")
                      .arg(cut).arg(result.length());
            }

            if (isErr) {
                int &failCount = m_toolFailCount[tKey];
                ++failCount;
                if (failCount >= DEADLOCK_ABORT) {
                    emit appendTools(
                        QString("<b>Plan: DEADLOCK ABBRUCH</b> '%1' (%2x)")
                        .arg(toolName.toHtmlEscaped()).arg(failCount), "error");
                    m_toolFailCount.remove(tKey);
                    m_mode = AgentMode::Chat;
                    emit modeChanged(m_mode);
                    m_chatModel.setSystemPrompt(buildFullSystemPrompt());
                    emit inputEnabled(true);
                    emit statusChanged("Plan fehlgeschlagen");
                    return;
                }
                toolResult += "\n\n" + deadlockEscalationPrompt(toolName, failCount);
            } else {
                m_toolFailCount.remove(tKey);
            }

            emit appendTools(
                QString("<b>Plan-Ergebnis [%1]:</b><br><pre>%2</pre>")
                .arg(toolName.toHtmlEscaped(),
                     toolResult.left(800).toHtmlEscaped() +
                     (toolResult.length() > 800 ? "\n..." : "")),
                isErr ? "error" : "tool");

            m_chatModel.addToolResult(toolName, toolResult);
            m_generating      = true;
            m_generatedTokens = 0;
            m_currentResponse.clear();
            m_thinkBuffer.clear();
            m_inThinkBlock = false;
            emit appendChat("<b>Assistent (Plan-Analyse):</b> ", "assistant");
            emit statusChanged("Plan-Analyse läuft...");
            startGeneration(LlamaWorker::SamplerProfile::Chat);
        });
}

// ─── handlePlanJson ───────────────────────────────────────────────────────────
void Agent::handlePlanJson(const QString &fullResponse, uint32_t sessionId)
{
    int planStart = fullResponse.indexOf("<plan>") + 6;
    int planEnd   = fullResponse.indexOf("</plan>", planStart);
    QString planJson = fullResponse.mid(planStart, planEnd - planStart).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(planJson.toUtf8(), &pe);

    if (doc.isNull()) {
        emit appendTools(
            QString("<b>Plan JSON-Fehler:</b> %1<br><pre>%2</pre>")
            .arg(pe.errorString().toHtmlEscaped(),
                 planJson.left(300).toHtmlEscaped()), "error");

        QString repaired = repairJson(planJson);
        if (!repaired.isEmpty()) {
            doc = QJsonDocument::fromJson(repaired.toUtf8());
            emit appendTools("<b>Plan JSON repariert.</b>", "system");
        } else {
            ++m_planRetryCount;
            if (m_planRetryCount > MAX_PLAN_RETRIES) {
                emit appendTools(
                    "<b>Plan fehlgeschlagen:</b> JSON nach Repair und Retry ungültig.", "error");
                m_mode = AgentMode::Chat;
                emit modeChanged(m_mode);
                m_chatModel.setSystemPrompt(buildFullSystemPrompt());
                emit inputEnabled(true);
                emit statusChanged("Plan fehlgeschlagen");
                return;
            }

            QString errFeedback = QString(
                "[SYSTEM: Dein <plan> Block enthielt ungültiges JSON. Fehler: %1. "
                "Bitte sende den vollständigen <plan>...</plan> Block erneut "
                "mit korrektem JSON.]").arg(pe.errorString());
            m_chatModel.addAssistantMessage(fullResponse);
            m_chatModel.addToolResult("plan_json_error", errFeedback);
            m_generating      = true;
            m_generatedTokens = 0;
            m_currentResponse.clear();
            m_thinkBuffer.clear();
            m_inThinkBlock = false;
            emit appendChat("<b>Assistent (Plan-Korrektur):</b> ", "assistant");
            emit statusChanged("Plan-JSON wird korrigiert...");
            startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
    }

    // ── JSON valide → TaskTree aufbauen ───────────────────────────────────
    QJsonObject root = doc.object();
    QString goalTitle = root.value("goal").toString("Unbenanntes Ziel");

    TaskNode *goalNode = m_taskTree.createNode(
        goalTitle, "", static_cast<int>(TaskLevel::Goal),
        TaskScope::External, 0, nullptr);
    goalNode->status = TaskStatus::Pending;

    // ── Erster Pass: Knoten aufbauen ──────────────────────────────────────
    // titleToId   — Titel → Node-ID (für zweiten Pass)
    // pendingDeps — Node-ID → Titel-Liste (unaufgelöste dependsOn)
    //
    // Analogie AVR: wie ein Assembler-erster-Pass der Symboltabelle aufbaut,
    // bevor im zweiten Pass forward references aufgelöst werden.
    QHash<QString, qint64>     titleToId;
    QHash<qint64, QStringList> pendingDeps;

    // Wurzel-Knoten ebenfalls in titleToId eintragen
    titleToId.insert(goalTitle, goalNode->id);

    QJsonArray children = root.value("children").toArray();
    int nodeCount = 1;
    for (int i = 0; i < children.size(); ++i)
        nodeCount += parsePlanNode(children[i].toObject(), goalNode, 1,
                                   titleToId, pendingDeps);

    // ── Zweiter Pass: dependsOn-Titel → Node-IDs auflösen ────────────────
    // Jetzt sind alle Knoten erstellt und titleToId vollständig befüllt.
    // Analogie AVR: Linker-zweiter-Pass — alle Symbole sind bekannt,
    // forward references können aufgelöst werden.
    int resolvedDeps   = 0;
    int unresolvedDeps = 0;
    for (auto it = pendingDeps.constBegin(); it != pendingDeps.constEnd(); ++it) {
        TaskNode *node = m_taskTree.findById(it.key());
        if (!node) continue;

        for (const QString &depTitle : it.value()) {
            qint64 depId = titleToId.value(depTitle, -1);
            if (depId >= 0) {
                m_taskTree.addDependency(node, m_taskTree.findById(depId));
                ++resolvedDeps;
            } else {
                ++unresolvedDeps;
                emit appendTools(
                    QString("<b>dependsOn nicht aufgelöst:</b> '%1' → '%2'")
                    .arg(node->title.toHtmlEscaped(), depTitle.toHtmlEscaped()),
                    "system");
            }
        }
    }

    if (resolvedDeps > 0 || unresolvedDeps > 0) {
        emit appendTools(
            QString("<b>Abhängigkeiten:</b> %1 aufgelöst, %2 nicht gefunden.")
            .arg(resolvedDeps).arg(unresolvedDeps), "system");
    }

    emit appendTools(
        QString("<b>Plan erstellt:</b> %1 Knoten, Ziel: <i>%2</i>")
        .arg(nodeCount).arg(goalTitle.toHtmlEscaped()), "system");

    emit taskTreeUpdated();
    emit planReady();
    emit inputEnabled(true);
    emit statusChanged("Plan bereit — bitte bestätigen oder ablehnen");
}

// ─── parsePlanNode ────────────────────────────────────────────────────────────
// Rekursiver Aufbau des TaskTree aus einem JSON-Objekt.
// Pattern: Composite (GoF).
//
// Neu: titleToId und pendingDeps für dependsOn-Auflösung im zweiten Pass.
// Der Knoten registriert seinen Titel in titleToId und seine dependsOn-Titel
// in pendingDeps — handlePlanJson() löst sie nach vollständigem Parsen auf.
int Agent::parsePlanNode(const QJsonObject &obj, TaskNode *parent, int depth,
                          QHash<QString, qint64> &titleToId,
                          QHash<qint64, QStringList> &pendingDeps)
{
    QString title = obj.value("title").toString(
        QString("Unbenannte Aufgabe (H%1)").arg(depth));

    int level = obj.contains("level")
                ? obj.value("level").toInt(depth)
                : depth;

    TaskScope scope = TaskScope::Internal;
    if (obj.value("scope").toString() == "external")
        scope = TaskScope::External;

    QString description = obj.value("description").toString();
    int order = obj.value("order").toInt(0);

    TaskNode *node = m_taskTree.createNode(
        title, description, level, scope, order, parent);

    // Titel → ID registrieren (erster Treffer gewinnt bei Kollision)
    if (!titleToId.contains(title))
        titleToId.insert(title, node->id);

    // dependsOn-Titel für zweiten Pass merken
    QJsonArray deps = obj.value("dependsOn").toArray();
    if (!deps.isEmpty()) {
        QStringList depTitles;
        for (const QJsonValue &v : deps)
            depTitles << v.toString();
        if (!depTitles.isEmpty())
            pendingDeps.insert(node->id, depTitles);
    }

    // Kinder rekursiv
    QJsonArray children = obj.value("children").toArray();
    int count = 1;
    for (int i = 0; i < children.size(); ++i)
        count += parsePlanNode(children[i].toObject(), node, depth + 1,
                               titleToId, pendingDeps);
    return count;
}

// ═════════════════════════════════════════════════════════════════════════════
// PRIVATE METHODEN (unverändert)
// ═════════════════════════════════════════════════════════════════════════════

void Agent::startGeneration(LlamaWorker::SamplerProfile profile)
{
    QMetaObject::invokeMethod(m_worker, "generate",
                              Qt::QueuedConnection,
                              Q_ARG(QVector<ChatMessage>,        m_chatModel.messages()),
                              Q_ARG(LlamaWorker::SamplerProfile, profile));
}

QString Agent::repairJson(const QString &broken) const
{
    QString s = broken.trimmed();
    if (!QJsonDocument::fromJson(s.toUtf8()).isNull()) return s;

    int openObj = s.count('{') - s.count('}');
    int openArr = s.count('[') - s.count(']');
    QString fixed = s;
    for (int i = 0; i < openArr; ++i) fixed += ']';
    for (int i = 0; i < openObj; ++i) fixed += '}';
    if (!QJsonDocument::fromJson(fixed.toUtf8()).isNull()) return fixed;

    QString noTrailing = fixed;
    for (const QString &close : {QString("}"), QString("]")}) {
        int pos = noTrailing.lastIndexOf(close);
        while (pos > 0) {
            int comma = noTrailing.lastIndexOf(',', pos - 1);
            if (comma < 0) break;
            bool onlyWs = true;
            for (int i = comma + 1; i < pos; ++i) {
                if (!noTrailing[i].isSpace()) { onlyWs = false; break; }
            }
            if (onlyWs) { noTrailing.remove(comma, 1); pos = comma; }
            else break;
        }
    }
    if (!QJsonDocument::fromJson(noTrailing.toUtf8()).isNull()) return noTrailing;

    if (!s.contains('"') && s.contains('\'')) {
        QString withDouble = noTrailing;
        withDouble.replace('\'', '"');
        if (!QJsonDocument::fromJson(withDouble.toUtf8()).isNull()) return withDouble;
    }
    return {};
}

QString Agent::toolCallKey(const QString &toolName, const QJsonObject &args) const
{
    QStringList parts;
    parts << toolName;
    QStringList keys = args.keys();
    keys.sort();
    for (const QString &k : keys) {
        QString val = QString::fromUtf8(
            QJsonDocument(QJsonObject{{k, args[k]}}).toJson(QJsonDocument::Compact));
        parts << val;
    }
    return parts.join('|');
}

QString Agent::deadlockEscalationPrompt(const QString &toolName, int count) const
{
    if (count >= DEADLOCK_ABORT)
        return QString("[SYSTEM: Tool '%1' ist %2 Mal hintereinander fehlgeschlagen. "
                       "ABBRUCH. Erkläre dem Nutzer was schiefgelaufen ist.]")
               .arg(toolName).arg(count);
    if (count >= DEADLOCK_REDIRECT)
        return QString("[SYSTEM: Tool '%1' schlägt wiederholt fehl (%2 Mal). "
                       "Suche einen ANDEREN Weg zum Ziel.]").arg(toolName).arg(count);
    return QString("[SYSTEM: Tool '%1' hat %2 Mal hintereinander den gleichen Fehler. "
                   "Überprüfe deine Argumente sorgfältig.]").arg(toolName).arg(count);
}

void Agent::handleToolCall(const QString &fullResponse, uint32_t sessionId)
{
    int start     = fullResponse.indexOf("<tool_call>") + 11;
    int end       = fullResponse.indexOf("</tool_call>", start);
    QString block = fullResponse.mid(start, end - start).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);

    if (doc.isNull()) {
        emit appendTools(
            QString("<b>JSON-Fehler:</b> %1<br><pre>%2</pre>")
            .arg(pe.errorString().toHtmlEscaped(), block.toHtmlEscaped()), "error");

        QString repaired = repairJson(block);
        if (!repaired.isEmpty()) {
            doc = QJsonDocument::fromJson(repaired.toUtf8());
            emit appendTools(
                QString("<b>JSON repariert:</b><br><pre>%1</pre>")
                .arg(repaired.toHtmlEscaped()), "tool");
        } else {
            QString errFeedback = QString(
                "[SYSTEM: Dein Tool-Call enthielt ungültiges JSON. Fehler: %1. "
                "Bitte sende den Tool-Call erneut mit korrektem JSON.]")
                .arg(pe.errorString());
            m_chatModel.addToolResult("json_error", errFeedback);
            m_generating      = true;
            m_generatedTokens = 0;
            m_currentResponse.clear();
            m_thinkBuffer.clear();
            m_inThinkBlock = false;
            emit appendChat("<b>Assistent:</b> ", "assistant");
            emit statusChanged("JSON-Fehler — nochmal...");
            startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
    }

    QString     toolName = doc.object().value("name").toString();
    QJsonObject toolArgs = doc.object().value("arguments").toObject();

    m_logger.logToolCall(toolName,
        QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Indented)));

    emit appendTools(
        QString("<b>Tool-Call: %1</b><br><pre>%2</pre>")
        .arg(toolName.toHtmlEscaped(),
             QString::fromUtf8(QJsonDocument(toolArgs)
                               .toJson(QJsonDocument::Indented)).toHtmlEscaped()),
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
    QString tKey = toolCallKey(toolName, toolArgs);

    m_mcp.callTool(toolName, toolArgs,
        [this, toolName, tKey, sessionId](QString result, QString error) {
            if (sessionId != m_sessionId) return;

            QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);
            bool    isErr      = !error.isEmpty();

            if (!isErr && toolResult.length() > AppConfig::instance().maxToolResultChars()) {
                int maxChars = AppConfig::instance().maxToolResultChars();
                int cut = toolResult.lastIndexOf('\n', maxChars);
                if (cut < maxChars / 2) cut = maxChars;
                toolResult = toolResult.left(cut)
                    + QString("\n\n[... gekürzt: %1 von %2 Zeichen angezeigt.]")
                      .arg(cut).arg(result.length());
            }

            if (isErr) {
                int &failCount = m_toolFailCount[tKey];
                ++failCount;
                if (failCount >= DEADLOCK_ABORT) {
                    emit appendTools(
                        QString("<b>DEADLOCK ABBRUCH</b>: '%1' hat %2 Mal versagt.")
                        .arg(toolName.toHtmlEscaped()).arg(failCount), "error");
                    m_chatModel.addToolResult(toolName, deadlockEscalationPrompt(toolName, failCount));
                    m_toolFailCount.remove(tKey);
                    m_generating      = true;
                    m_generatedTokens = 0;
                    m_currentResponse.clear();
                    m_thinkBuffer.clear();
                    m_inThinkBlock = false;
                    emit appendChat("<b>Assistent:</b> ", "assistant");
                    emit statusChanged("Abbruch...");
                    startGeneration(LlamaWorker::SamplerProfile::Chat);
                    return;
                }
                if (failCount == DEADLOCK_WARN || failCount == DEADLOCK_REDIRECT) {
                    emit appendTools(
                        QString("<b>Deadlock-Warnung (Stufe %1):</b> '%2' fehlgeschlagen.")
                        .arg(failCount).arg(toolName.toHtmlEscaped()), "error");
                    toolResult += "\n\n" + deadlockEscalationPrompt(toolName, failCount);
                }
            } else {
                m_toolFailCount.remove(tKey);
            }

            emit appendTools(
                QString("<b>Ergebnis [%1]:</b><br><pre>%2</pre>")
                .arg(toolName.toHtmlEscaped(), toolResult.toHtmlEscaped()),
                isErr ? "error" : "tool");

            m_logger.logToolResult(toolName, toolResult, isErr);

            static const QStringList fileWriteTools = {"str_replace", "write_file", "append_file"};
            if (!isErr && fileWriteTools.contains(toolName)) {
                QJsonObject diffArgs;
                diffArgs["stat_only"] = false;
                m_mcp.callTool("git_diff", diffArgs,
                    [this, sessionId](QString diffResult, QString diffError) {
                        if (sessionId != m_sessionId) return;
                        if (!diffError.isEmpty() || diffResult.isEmpty()) return;
                        QString html = "<b>Git Diff:</b><br><pre style='font-size:10px'>";
                        for (const QString &line : diffResult.split('\n')) {
                            QString esc = line.toHtmlEscaped();
                            if (line.startsWith('+') && !line.startsWith("+++"))
                                html += QString("<span style='color:#188038;background:#e6f4ea'>%1</span>\n").arg(esc);
                            else if (line.startsWith('-') && !line.startsWith("---"))
                                html += QString("<span style='color:#c5221f;background:#fce8e6'>%1</span>\n").arg(esc);
                            else if (line.startsWith("@@"))
                                html += QString("<span style='color:#1a73e8'>%1</span>\n").arg(esc);
                            else
                                html += QString("<span style='color:#666'>%1</span>\n").arg(esc);
                        }
                        html += "</pre>";
                        emit appendTools(html, "tool");
                    });
            }

            m_chatModel.addToolResult(toolName, toolResult);
            m_generating      = true;
            m_generatedTokens = 0;
            m_currentResponse.clear();
            m_thinkBuffer.clear();
            m_inThinkBlock = false;
            emit appendChat("<b>Assistent:</b> ", "assistant");
            emit statusChanged("Tool-Ergebnis verarbeiten...");
            startGeneration(LlamaWorker::SamplerProfile::Tool);
        });
}

void Agent::filterToken(const QString &token)
{
    static constexpr int MAX_TAG_LEN = 12;
    if (!m_inThinkBlock) {
        m_thinkBuffer += token;
        if (m_thinkBuffer.endsWith("<think>")) {
            QString before = m_thinkBuffer; before.chop(7);
            if (!before.isEmpty()) emit appendChatToken(before);
            m_inThinkBlock = true; m_thinkBuffer.clear();
        } else if (m_thinkBuffer.length() > MAX_TAG_LEN ||
                   !QString("<think>").startsWith(m_thinkBuffer.right(MAX_TAG_LEN))) {
            emit appendChatToken(m_thinkBuffer); m_thinkBuffer.clear();
        }
    } else {
        m_thinkBuffer += token;
        if (m_thinkBuffer.endsWith("</think>")) {
            QString thinkText = m_thinkBuffer; thinkText.chop(8);
            emit appendTools(
                QString("<b>Thinking:</b><br><span style='white-space:pre-wrap'>%1</span>")
                .arg(thinkText.trimmed().toHtmlEscaped()), "think");
            m_logger.logThinking(thinkText.trimmed());
            m_inThinkBlock = false; m_thinkBuffer.clear();
        }
    }
}

void Agent::emitStats()
{
    emit statsUpdated(m_promptTokens, m_generatedTokens, m_totalTokens, m_ctxSize);
}

void Agent::checkContextUsage()
{
    if (m_summarizing || m_ctxSize == 0) return;
    int used = m_promptTokens + m_generatedTokens;
    int pct  = (used * 100) / m_ctxSize;
    if (pct >= AppConfig::instance().summarizeThreshold()) {
        emit appendTools(
            QString("<b>Kontext bei %1% — automatisches Zusammenfassen...</b>").arg(pct), "system");
        summarizeContext();
    }
}

void Agent::summarizeContext()
{
    if (m_summarizing) return;
    m_summarizing = true;

    QString history;
    for (const auto &msg : m_chatModel.messages()) {
        switch (msg.role) {
            case ChatMessage::Role::System:    break;
            case ChatMessage::Role::User:      history += "User: "      + msg.content + "\n\n"; break;
            case ChatMessage::Role::Assistant: history += "Assistant: " + msg.content + "\n\n"; break;
            case ChatMessage::Role::Tool:      history += "Tool: "      + msg.content + "\n\n"; break;
        }
    }

    QString summarizePrompt = QString(
        "Fasse die folgende Konversation in maximal 300 Wörtern zusammen. "
        "Behalte alle wichtigen Fakten, getroffenen Entscheidungen, "
        "Dateipfade, Fehlermeldungen und offene Aufgaben. "
        "Schreibe nur die Zusammenfassung, keine Einleitung.\n\n"
        "--- Konversation ---\n%1\n--- Ende ---"
    ).arg(history.left(12000));

    m_chatModel.addUserMessage(summarizePrompt);
    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock    = false;
    m_generatedTokens = 0;
    m_generating      = true;
    startGeneration(LlamaWorker::SamplerProfile::Chat);
}

QString Agent::computeDiffHtml(const QString &before, const QString &after,
                                const QString &filename) const
{
    QStringList oldLines = before.split('\n');
    QStringList newLines = after.split('\n');
    int m = oldLines.size(), n = newLines.size();

    if (m > 300 || n > 300)
        return QString("<b>Diff %1</b> (zu groß für LCS, %2→%3 Zeilen)")
               .arg(filename.toHtmlEscaped()).arg(m).arg(n);

    std::vector<int> dp((m+1)*(n+1), 0);
    auto at = [&](int i, int j) -> int& { return dp[i*(n+1)+j]; };
    for (int i = 1; i <= m; ++i)
        for (int j = 1; j <= n; ++j)
            at(i,j) = (oldLines[i-1]==newLines[j-1]) ? at(i-1,j-1)+1
                                                      : std::max(at(i-1,j),at(i,j-1));

    struct DiffLine { enum Type{Equal,Added,Removed}type; QString text; };
    QVector<DiffLine> diffLines;
    int i=m, j=n;
    while (i>0||j>0) {
        if (i>0&&j>0&&oldLines[i-1]==newLines[j-1])
            { diffLines.prepend({DiffLine::Equal,oldLines[i-1]}); --i;--j; }
        else if (j>0&&(i==0||at(i,j-1)>=at(i-1,j)))
            { diffLines.prepend({DiffLine::Added,newLines[j-1]}); --j; }
        else
            { diffLines.prepend({DiffLine::Removed,oldLines[i-1]}); --i; }
    }

    static constexpr int CONTEXT=2;
    QVector<bool> changed(diffLines.size(),false), show(diffLines.size(),false);
    for (int k=0;k<diffLines.size();++k) if (diffLines[k].type!=DiffLine::Equal) changed[k]=true;
    for (int k=0;k<diffLines.size();++k) {
        if (!changed[k]) continue;
        for (int c=std::max(0,k-CONTEXT);c<=std::min((int)diffLines.size()-1,k+CONTEXT);++c)
            show[c]=true;
    }

    QString html = QString("<b>Diff: %1</b><br><pre style='font-size:10px'>")
                   .arg(filename.toHtmlEscaped());
    bool inGap=false;
    for (int k=0;k<diffLines.size();++k) {
        if (!show[k]) {
            if (!inGap){html+="<span style='color:#aaa'>...</span>\n";inGap=true;}
            continue;
        }
        inGap=false;
        QString esc=diffLines[k].text.toHtmlEscaped();
        switch (diffLines[k].type) {
            case DiffLine::Added:
                html+=QString("<span style='color:#188038;background:#e6f4ea'>+ %1</span>\n").arg(esc); break;
            case DiffLine::Removed:
                html+=QString("<span style='color:#c5221f;background:#fce8e6'>- %1</span>\n").arg(esc); break;
            case DiffLine::Equal:
                html+=QString("<span style='color:#666'>  %1</span>\n").arg(esc); break;
        }
    }
    html += "</pre>";
    return html;
}



// ═════════════════════════════════════════════════════════════════════════════
// EXECUTE-MODUS: NEUE METHODEN
// ═════════════════════════════════════════════════════════════════════════════

// ─── startExecute ────────────────────────────────────────────────────────────
// Wechselt in Execute-Modus, lädt Thoughts aus DB, startet ersten Node.
//
// Ablauf:
//   1. Modus setzen
//   2. Thoughts laden (aus SQLite, same DB wie TaskTree)
//   3. frisches ChatModel für Execute-Kontext
//   4. advanceExecute() — ersten Pending-Node holen und starten
void Agent::startExecute()
{
    m_mode = AgentMode::Execute;
    emit modeChanged(m_mode);

    // Thoughts aus DB laden
    const AppConfig &cfg = AppConfig::instance();
    m_executeMemory.setDbPath(cfg.taskDbPath());
    m_executeMemory.setMaxEntries(cfg.executeMemoryMaxEntries());
    m_executeMemory.load();

    emit appendTools(
        QString("<b>Execute-Modus gestartet.</b> Thoughts: %1 Einträge.")
            .arg(m_executeMemory.count()), "system");

    if (!advanceExecute()) {
        // Kein Pending-Node — alles schon Done?
        emit appendChat("<b>[System]</b> Alle Nodes bereits erledigt.", "system");
        m_mode = AgentMode::Chat;
        emit modeChanged(m_mode);
        m_chatModel.setSystemPrompt(buildFullSystemPrompt());
        emit inputEnabled(true);
        emit statusChanged("Bereit");
        emit executeFinished();
    }
}

// ─── advanceExecute ──────────────────────────────────────────────────────────
// NEU: buildPrompt wird jetzt im Node gespeichert (für Debugging).
bool Agent::advanceExecute()
{
    TaskNode *node = m_taskTree.nextPending();
    if (!node) {
        emit appendTools("<b>Execute: alle Nodes abgearbeitet!</b>", "system");
        emit executeFinished();
        m_executeMemory.save();
        m_mode = AgentMode::Chat;
        emit modeChanged(m_mode);
        m_chatModel.setSystemPrompt(buildFullSystemPrompt());
        emit inputEnabled(true);
        emit statusChanged("Execute abgeschlossen");
        return false;
    }

    m_currentNode = node;
    m_taskTree.setStatus(node, TaskStatus::Running);
    emit taskTreeUpdated();
    emit executeNodeStarted(node->id);

    // Prompt aufbauen
    QString executePrompt = buildExecutePrompt(node);

    // buildPrompt im Node speichern (Debugging: was hat das Modell gesehen?)
    m_taskTree.setBuildPrompt(node, executePrompt);

    // Frisches ChatModel für jeden Node
    m_chatModel.clear();
    m_chatModel.setSystemPrompt(buildExecuteSystemPrompt());
    m_chatModel.addUserMessage(executePrompt);

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock      = false;
    m_generatedTokens   = 0;
    m_generating        = true;
    m_continuationCount = 0;

    emit appendChat(
        QString("<b>Execute Node:</b> %1 [%2]")
            .arg(node->title.toHtmlEscaped(),
                 TaskNode::levelName(node->level).toHtmlEscaped()),
        "system");
    emit appendTools("<b>Execute: alle Nodes abgearbeitet!</b>", "system");
    emit executeFinished();
    m_executeMemory.save();

    // Automatisch assemblieren wenn Sandbox-Projekt konfiguriert
    if (!AppConfig::instance().executeSandboxProject().isEmpty())
         assembleProject();

    m_mode = AgentMode::Chat;
    emit modeChanged(m_mode);
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());
    emit inputEnabled(true);
    emit statusChanged("Execute abgeschlossen");
    return false;
    startGeneration(LlamaWorker::SamplerProfile::Chat);
    return true;
}

// ─── buildExecuteSystemPrompt ────────────────────────────────────────────────
// Dateityp-bewusster System-Prompt.
//
// Problem v1: "Gib NUR C++ Code aus" verwirrt das Modell bei CMakeLists.txt
// Lösung v2: Dateityp aus Node-Titel ableiten, Prompt entsprechend anpassen
//
// Warum im System-Prompt und nicht im User-Prompt?
//   Der System-Prompt definiert die Rolle und Ausgabe-Konvention.
//   Der User-Prompt enthält die eigentliche Aufgabe.
//   Beides zu mischen würde die Aufgabe unübersichtlicher machen.
QString Agent::buildExecuteSystemPrompt() const
{
    // Dateityp aus aktuellem Node-Titel ableiten
    QString lang = "Code";
    QString fileTypeHint = "";
    if (m_currentNode) {
        lang = m_currentNode->languageName();
        fileTypeHint = QString(
                           "Du implementierst eine %1.\n"
                           "Ausgabe: NUR %2-Code.\n\n"
                           ).arg(m_currentNode->fileTypeDescription(), lang);
    }

    return QString(
               "Du bist ein Implementierungs-Agent fuer C++/Qt6 Projekte.\n"
               "\n"
               "%1"
               "AUSGABE-FORMAT (wichtig!):\n"
               "Umschliesse deinen Code mit diesen Tags:\n"
               "\n"
               "<code>\n"
               "...dein %2-Code hier...\n"
               "</code>\n"
               "\n"
               "Kein Text vor <code>, kein Text nach </code>.\n"
               "Kein Markdown (keine ```-Fences).\n"
               "Nur der reine Code zwischen den Tags.\n"
               "\n"
               "LESE-TOOLS (bei Bedarf):\n"
               "read_file, get_function_body, get_class_members, get_includes,\n"
               "list_dir, get_symbol, search_code, grep_code, read_multiple_files,\n"
               "web_search, get_time, sys_info, get_pwd\n"
               "\n"
               "VERBOTEN: write_file, str_replace, append_file, cmake_build\n"
               "\n"
               "WORKFLOW:\n"
               "1. Lies zuerst was du brauchst (optional)\n"
               "2. Implementiere dann die Aufgabe\n"
               "3. Gib den Code zwischen <code>...</code> aus\n"
               "\n"
               "Antworte auf Deutsch wenn du Text ausgibst (z.B. bei Fehlern).\n"
               "Fehler-Text AUSSERHALB der <code>-Tags schreiben.\n"
               ).arg(fileTypeHint, lang);
}

// ─── buildExecutePrompt ──────────────────────────────────────────────────────
// Lichtkegel für den aktuellen Node aufbauen.
//
// Struktur:
//   [Thoughts]         — Kurzzeitgedächtnis (was bisher gelernt wurde)
//   [Vertikaler Pfad]  — H0 → H1 → H2 → H3 (aktuell)
//   [Abhängigkeiten]   — dependsOn-Nodes (Interfaces die dieser Node braucht)
//   [Aufgabe]          — was genau zu implementieren ist
//
// Das ist der "Taschenlampen-Lichtkegel" — minimaler aber vollständiger Kontext.
// Mehr würde das 9B-Modell überlasten, weniger wäre zu wenig.
QString Agent::buildExecutePrompt(const TaskNode *node) const
{
    QString prompt;

    // ── Thoughts ──────────────────────────────────────────────────────────
    if (!m_executeMemory.isEmpty()) {
        prompt += "## Bisherige Erkenntnisse (Thoughts)\n\n";
        prompt += m_executeMemory.toPromptString();
        prompt += "\n\n";
    }

    // ── Vertikaler Kontext (Pfad H0→aktuell) ─────────────────────────────
    // verticalContext() aus TaskNode liefert den Pfad + description des aktuellen Nodes.
    prompt += "## Aufgaben-Kontext\n\n";
    prompt += node->verticalContext();
    prompt += "\n";

    // ── Horizontaler Kontext (dependsOn) ─────────────────────────────────
    // buildContext() liefert bereits beides (vertikal + horizontal).
    // Wir nutzen nur den horizontalen Teil direkt.
    if (!node->dependsOn.isEmpty()) {
        prompt += "\n## Abhängigkeiten (Interfaces die du nutzen darfst)\n\n";
        for (qint64 depId : node->dependsOn) {
            const TaskNode *dep = m_taskTree.findById(depId);
            if (!dep) continue;
            prompt += QString("### %1 [%2]\n").arg(dep->title, TaskNode::levelName(dep->level));
            if (!dep->description.isEmpty())
                prompt += dep->description + "\n";
            // Falls der abhängige Node bereits Code hat (result) → mit ausgeben.
            // Das ist der Charme: das Modell sieht das Interface direkt.
            if (!dep->result.isEmpty()) {
                prompt += "\n```cpp\n" + dep->result + "\n```\n";
            }
            prompt += "\n";
        }
    }

    // ── Explizite Aufgabe ─────────────────────────────────────────────────
    prompt += "## Deine Aufgabe\n\n";
    prompt += QString("Implementiere: **%1**\n\n").arg(node->title);
    if (!node->description.isEmpty())
        prompt += node->description + "\n\n";
    prompt += "Gib NUR den C++ Code aus. Kein Text davor oder danach.";

    return prompt;
}

// ─── isExecuteToolCall ───────────────────────────────────────────────────────
// Prüft ob der Output ein Tool-Call ist oder roher Code.
// Tool-Call: beginnt mit <tool_call> (nach Whitespace/Thinking-Block).
// Roher Code: alles andere.
//
// Warum nicht einfach contains("<tool_call>")?
//   Weil roher Code Kommentare wie "// <tool_call>" enthalten könnte.
//   Wir prüfen ob der Response einen vollständigen Tool-Call enthält.
bool Agent::isExecuteToolCall(const QString &response) const
{
    return response.contains("<tool_call>") && response.contains("</tool_call>");
}

// ═════════════════════════════════════════════════════════════════════════════
// SNIPPET 3: Agent.cpp — assembleProject() Implementierung
// ═════════════════════════════════════════════════════════════════════════════

// ─── assembleProject ─────────────────────────────────────────────────────────
// Assembliert alle H2-Nodes zu Dateien auf Disk.
//
// Zielverzeichnis: ~/llamatools/[executeSandboxProject]/
// Wenn executeSandboxProject leer → Fehler + Hinweis an User.
//
// Wird aufgerufen:
//   1. Automatisch am Ende von advanceExecute() wenn alle Nodes Done
//   2. Manuell via /codeAssemble
void Agent::assembleProject()
{
    const AppConfig &cfg = AppConfig::instance();
    QString project = cfg.executeSandboxProject().trimmed();

    if (project.isEmpty()) {
        emit appendTools(
            "<b>Assembly fehlgeschlagen:</b> Kein Sandbox-Projekt konfiguriert.<br>"
            "Bitte im Einstellungen-Dialog unter <b>⚙ Execute → Projekt</b> "
            "ein Unterverzeichnis angeben (z.B. 'MeinProjekt').",
            "error");
        return;
    }

    // Zielverzeichnis aufbauen
    QString targetDir = QDir::homePath() + "/llamatools/" + project;

    emit appendTools(
        QString("<b>Assembly gestartet:</b> Ziel: <code>%1</code>")
            .arg(targetDir.toHtmlEscaped()),
        "system");

    bool onlyDone = cfg.assembleOnlyDone();

    // Assembly ausführen
    CodeAssembler::AssemblyResult result =
        m_assembler.assemble(targetDir, m_taskTree, onlyDone);

    // Ergebnis anzeigen
    if (result.filesWritten > 0) {
        QString html = QString("<b>Assembly abgeschlossen:</b> "
                               "%1 Datei(en) geschrieben, "
                               "%2 übersprungen<br>")
                           .arg(result.filesWritten)
                           .arg(result.filesSkipped);

        // Liste der geschriebenen Dateien (max 10 anzeigen)
        html += "<small>";
        int shown = 0;
        for (const QString &path : result.writtenPaths) {
            if (shown >= 10) {
                html += QString("... und %1 weitere<br>")
                .arg(result.writtenPaths.size() - shown);
                break;
            }
            // Nur Dateiname anzeigen, nicht den ganzen Pfad
            html += QString("&nbsp;✓ <code>%1</code><br>")
                        .arg(QFileInfo(path).fileName().toHtmlEscaped());
            ++shown;
        }
        html += "</small>";

        emit appendTools(html, "system");
        emit appendChat(
            QString("<b>[Assembly]</b> %1 Datei(en) in <code>%2</code> geschrieben.")
                .arg(result.filesWritten)
                .arg(targetDir.toHtmlEscaped()),
            "system");
    } else {
        emit appendTools(
            QString("<b>Assembly:</b> Keine Dateien geschrieben "
                    "(%1 übersprungen).<br>"
                    "<small>Haben alle Nodes einen result-Inhalt?</small>")
                .arg(result.filesSkipped),
            "system");
    }

    // Fehler anzeigen
    for (const QString &err : result.errors) {
        emit appendTools(
            QString("<b>Assembly-Fehler:</b> %1").arg(err.toHtmlEscaped()),
            "error");
    }

    emit statusChanged(result.success()
                           ? "Assembly abgeschlossen"
                           : "Assembly mit Fehlern");
}

// ─── handleExecuteCode ───────────────────────────────────────────────────────
// Verarbeitet den vollständigen LLM-Output im Execute-Modus.
//
// NEU gegenüber v1:
//   - <code>...</code> Tags extrahieren → nur das landet in node->result
//   - Alles außerhalb der Tags → node->sideOutput (Thinking, Erklärungen)
//   - buildPrompt wird im Node gespeichert (für Debugging)
//   - Markdown-Fences als Fallback wenn Tags fehlen
//
// Warum Tags statt rohem Code?
//   Roher Code: Modell schreibt Erklärungen rein → schwer zu trennen
//   Tags: eindeutige Grenze, Modell muss explizit "umschalten"
//   Falls Tags fehlen (Modell folgt nicht): Fallback auf ganzen Response
void Agent::handleExecuteCode(const QString &fullResponse, uint32_t sessionId)
{
    if (!m_currentNode) return;

    QString code;
    QString sideOut;

    // ── Code aus <code>...</code> extrahieren ─────────────────────────────
    int codeStart = fullResponse.indexOf("<code>");
    int codeEnd   = fullResponse.indexOf("</code>");

    if (codeStart >= 0 && codeEnd > codeStart) {
        // Alles vor <code> → sideOutput (Thinking, Intro-Text)
        sideOut = fullResponse.left(codeStart).trimmed();

        // Code zwischen den Tags
        code = fullResponse.mid(codeStart + 6, codeEnd - codeStart - 6).trimmed();

        // Alles nach </code> → auch sideOutput (Erklärungen, Hinweise)
        QString after = fullResponse.mid(codeEnd + 7).trimmed();
        if (!after.isEmpty()) {
            if (!sideOut.isEmpty()) sideOut += "\n\n";
            sideOut += after;
        }
    } else {
        // Fallback: keine Tags → ganzen Response als Code behandeln
        // Markdown-Fences entfernen falls vorhanden
        code = fullResponse.trimmed();
        if (code.startsWith("```")) {
            int firstNewline = code.indexOf('\n');
            if (firstNewline > 0)
                code = code.mid(firstNewline + 1);
            if (code.endsWith("```"))
                code.chop(3);
            code = code.trimmed();
        }
        sideOut = "[Hinweis: Modell hat keine <code>-Tags verwendet. "
                  "Gesamter Output als Code behandelt.]";

        emit appendTools(
            "<b>Execute:</b> Keine &lt;code&gt;-Tags gefunden — "
            "Fallback auf gesamten Output.", "system");
    }

    // ── Thinking aus sideOutput entfernen (sauber für Anzeige) ───────────
    // <think>-Blöcke landen im sideOutput aber wir wollen sie trotzdem
    // sauber von anderem Text trennen.
    static const QRegularExpression thinkRe(
        "<think>.*?</think>",
        QRegularExpression::DotMatchesEverythingOption);
    sideOut.remove(thinkRe);
    sideOut = sideOut.trimmed();

    // ── In Node speichern ─────────────────────────────────────────────────
    m_taskTree.setResult(m_currentNode, code);
    m_taskTree.setSideOutput(m_currentNode, sideOut);
    // buildPrompt wurde bereits in advanceExecute() gesetzt
    m_taskTree.setStatus(m_currentNode, TaskStatus::Done);
    m_taskTree.save();

    emit appendTools(
        QString("<b>Node fertig:</b> %1<br>"
                "<small>%2 Zeichen Code | %3 Zeichen sideOutput</small>")
            .arg(m_currentNode->title.toHtmlEscaped())
            .arg(code.length())
            .arg(sideOut.length()),
        "system");

    emit executeNodeDone(m_currentNode->id);
    emit taskTreeUpdated();

    // Thoughts nach jedem Node updaten
    updateThoughts(m_currentNode, sessionId);
}

// ─── handleExecuteToolCall ───────────────────────────────────────────────────
// Lese-Tool-Call im Execute-Modus verarbeiten.
// Whitelist: EXECUTE_ALLOWED_TOOLS (Lesen + websearch, kein Schreiben).
//
// Fast identisch mit handlePlanToolCall() — gleiche Struktur,
// andere Whitelist und anderer Kontext-String.
void Agent::handleExecuteToolCall(const QString &fullResponse, uint32_t sessionId)
{
    int start     = fullResponse.indexOf("<tool_call>") + 11;
    int end       = fullResponse.indexOf("</tool_call>", start);
    QString block = fullResponse.mid(start, end - start).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);

    if (doc.isNull()) {
        QString repaired = repairJson(block);
        if (!repaired.isEmpty())
            doc = QJsonDocument::fromJson(repaired.toUtf8());
        else {
            m_chatModel.addToolResult("json_error",
                                      QString("[SYSTEM: Ungültiges JSON. Fehler: %1]").arg(pe.errorString()));
            m_generating      = true;
            m_generatedTokens = 0;
            m_currentResponse.clear();
            m_thinkBuffer.clear();
            m_inThinkBlock = false;
            emit appendChat("<b>Assistent (Execute):</b> ", "assistant");
            startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
    }

    QString     toolName = doc.object().value("name").toString();
    QJsonObject toolArgs = doc.object().value("arguments").toObject();

    emit appendTools(
        QString("<b>Execute-Tool: %1</b><br><pre>%2</pre>")
            .arg(toolName.toHtmlEscaped(),
                 QString::fromUtf8(QJsonDocument(toolArgs)
                                       .toJson(QJsonDocument::Indented)).toHtmlEscaped()),
        "tool");

    // Whitelist prüfen
    if (!EXECUTE_ALLOWED_TOOLS.contains(toolName)) {
        QString errMsg = QString(
                             "[SYSTEM: Tool '%1' ist im Execute-Modus VERBOTEN. "
                             "Nur Lese-Tools und websearch erlaubt. "
                             "Implementiere den Code direkt ohne zu schreiben.]")
                             .arg(toolName);
        emit appendTools(
            QString("Execute-Whitelist: <b>%1</b> verboten.").arg(toolName.toHtmlEscaped()),
            "error");
        m_chatModel.addToolResult(toolName, errMsg);
        m_generating      = true;
        m_generatedTokens = 0;
        m_currentResponse.clear();
        m_thinkBuffer.clear();
        m_inThinkBlock = false;
        emit appendChat("<b>Assistent (Execute):</b> ", "assistant");
        startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    if (!m_mcp.containsTool(toolName)) {
        m_chatModel.addToolResult(toolName,
                                  QString("Fehler: Tool '%1' nicht verfügbar.").arg(toolName));
        m_generating      = true;
        m_generatedTokens = 0;
        startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    emit statusChanged(QString("Execute-Tool: %1...").arg(toolName));
    QString tKey = toolCallKey(toolName, toolArgs);

    m_mcp.callTool(toolName, toolArgs,
                   [this, toolName, tKey, sessionId](QString result, QString error) {
                       if (sessionId != m_sessionId) return;

                       QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);
                       bool    isErr      = !error.isEmpty();

                       if (!isErr && toolResult.length() > AppConfig::instance().maxToolResultChars()) {
                           int maxChars = AppConfig::instance().maxToolResultChars();
                           int cut = toolResult.lastIndexOf('\n', maxChars);
                           if (cut < maxChars / 2) cut = maxChars;
                           toolResult = toolResult.left(cut)
                                        + QString("\n\n[... gekürzt: %1 von %2 Zeichen.]")
                                              .arg(cut).arg(result.length());
                       }

                       if (isErr) {
                           int &failCount = m_toolFailCount[tKey];
                           ++failCount;
                           if (failCount >= DEADLOCK_ABORT) {
                               emit appendTools(
                                   QString("<b>Execute: DEADLOCK ABBRUCH</b> '%1'")
                                       .arg(toolName.toHtmlEscaped()), "error");
                               m_toolFailCount.remove(tKey);
                               // Node als Failed markieren + nächsten versuchen
                               if (m_currentNode) {
                                   m_taskTree.setStatus(m_currentNode, TaskStatus::Failed);
                                   m_taskTree.save();
                                   emit taskTreeUpdated();
                               }
                               advanceExecute();
                               return;
                           }
                           toolResult += "\n\n" + deadlockEscalationPrompt(toolName, failCount);
                       } else {
                           m_toolFailCount.remove(tKey);
                       }

                       emit appendTools(
                           QString("<b>Execute-Ergebnis [%1]:</b><br><pre>%2</pre>")
                               .arg(toolName.toHtmlEscaped(),
                                    toolResult.left(800).toHtmlEscaped() +
                                        (toolResult.length() > 800 ? "\n..." : "")),
                           isErr ? "error" : "tool");

                       m_chatModel.addToolResult(toolName, toolResult);
                       m_generating      = true;
                       m_generatedTokens = 0;
                       m_currentResponse.clear();
                       m_thinkBuffer.clear();
                       m_inThinkBlock = false;
                       emit appendChat("<b>Assistent (Execute):</b> ", "assistant");
                       emit statusChanged("Execute läuft...");
                       startGeneration(LlamaWorker::SamplerProfile::Chat);
                   });
}

// ─── updateThoughts ──────────────────────────────────────────────────────────
// NEU: parseFromLlmOutput() statt setEntries() — robustes Parsing.
void Agent::updateThoughts(const TaskNode *node, uint32_t sessionId)
{
    m_updatingThoughts = true;

    QString summarizePrompt = QString(
                                  "Du hast gerade folgenden Node implementiert:\n"
                                  "Titel: %1\n"
                                  "Beschreibung: %2\n"
                                  "\n"
                                  "Bisherige Thoughts:\n"
                                  "%3\n"
                                  "\n"
                                  "Aufgabe: Aktualisiere die Thoughts-Liste.\n"
                                  "- Behalte wichtige Erkenntnisse aus der bisherigen Liste\n"
                                  "- Fuege neue Erkenntnisse aus dieser Implementierung hinzu\n"
                                  "- Entferne ueberholte oder unwichtige Eintraege\n"
                                  "- Maximal %4 Eintraege\n"
                                  "- Format: eine Erkenntnis pro Zeile\n"
                                  "- KEINE Nummerierung (keine '1.', '2.' etc.)\n"
                                  "- KEIN Markdown (keine **, keine ```)\n"
                                  "- NUR die Liste ausgeben, kein anderer Text\n"
                                  "- KEIN <think>-Block, direkt die Liste"
                                  ).arg(node->title,
                                       node->description,
                                       m_executeMemory.toPromptString(),
                                       QString::number(AppConfig::instance().executeMemoryMaxEntries()));

    ChatModel summarizeModel;
    summarizeModel.setChatTemplate(m_chatModel.chatTemplate());
    summarizeModel.setSystemPrompt(
        "Du bist ein Assistent der Kurzzeitgedaechtnis-Listen komprimiert. "
        "Antworte NUR mit der Liste. Eine Erkenntnis pro Zeile. "
        "Keine Nummerierung. Kein Markdown. Kein anderer Text.");
    summarizeModel.addUserMessage(summarizePrompt);

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock    = false;
    m_generatedTokens = 0;
    m_generating      = true;

    emit statusChanged("Thoughts werden aktualisiert...");
    m_chatModel = std::move(summarizeModel);
    startGeneration(LlamaWorker::SamplerProfile::Chat);
}


