#include "Agent/Agent.h"
#include "Agent/AgentChat.h"
#include "Agent/AgentUtils.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QFileInfo>
#include <QDir>

Agent::Agent(const QString &modelPath, QObject *parent)
    : QObject(parent)
    , m_modelPath(modelPath)
    , m_chatModel()
    , m_mcp(this)
    , m_commands(this)
{
    m_chat = new AgentChat(*this);

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

    const QString binDir = QCoreApplication::applicationDirPath();

    // ── MCP-Server registrieren ───────────────────────────────────────────
    // Wenn Incus aktiv und Container bekannt: Binaries im Container verwenden.
    // Sonst: lokale Binaries (bisheriges Verhalten).
    //
    // Transport-Entscheidung ist in McpManager::startServer() gekapselt:
    //   incusContainer leer    → lokaler QProcess
    //   incusContainer gesetzt → incus exec <container> -- <binary>

    const bool    useIncus  = cfg.incusEnabled() && !cfg.incusContainer().isEmpty();
    const QString container = cfg.incusContainer();
    const QString incusBin  = cfg.incusBinDir();  // /usr/lib/llamaqt-mcp

    if (useIncus) {
        emit appendTools(
            QString("<b>Incus:</b> Container <i>%1</i> aktiv — "
                    "MCP-Server laufen im Container.")
            .arg(container.toHtmlEscaped()), "system");
    }

    // filesystem / sysinfo / compile / websearch:
    // lokal oder im Container je nach Konfiguration
    auto addMcpServer = [&](const QString &name) {
        const QString localBin = binDir + "/mcp-servers/" + name + "/llamaqt-" + name;
        const QString contBin  = incusBin + "/llamaqt-" + name;
        m_mcp.addServer(useIncus ? contBin : localBin,
                        {},
                        useIncus ? container : QString{});
    };

    addMcpServer("filesystem");
    addMcpServer("sysinfo");
    addMcpServer("compile");
    addMcpServer("websearch");
    addMcpServer("workspace");

    // tree-sitter und clang laufen immer lokal —
    // sie brauchen Zugriff auf Host-Quelldateien (~/ai/LlamaQT/src/).
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

// ─── onIncusContainerChanged ──────────────────────────────────────────────────
// Wird von IncusDock::activeContainerChanged() ausgelöst wenn der User
// einen laufenden Container auswählt.
//
// Ablauf:
//   1. Container in AppConfig speichern
//   2. McpManager: alle Server auf neuen Container umschalten
//   3. startAll() neu aufrufen — startServer() löscht den alten Client
//      und erstellt einen neuen mit dem neuen Container
//   4. System-Prompt neu bauen (Tools können sich geändert haben)

void Agent::onIncusContainerChanged(const QString &containerName,
                                     const QString &ip)
{
    Q_UNUSED(ip)  // wird für SSE-Transport benötigt (Phase 2)

    AppConfig &cfg = AppConfig::instance();
    cfg.setIncusContainer(containerName);
    cfg.setIncusEnabled(!containerName.isEmpty());

    const QString incusBin = cfg.incusBinDir();

    emit appendTools(
        QString("<b>Incus:</b> Container → <i>%1</i><br>"
                "MCP-Server werden neu gestartet...")
        .arg(containerName.toHtmlEscaped()), "system");

    emit inputEnabled(false);
    emit statusChanged("MCP-Server werden neu gestartet...");

    // Container für alle 5 Container-Server setzen (0-4):
    // filesystem, sysinfo, compile, websearch, workspace
    // tree-sitter (5) und clang (6) bleiben lokal.
    m_mcp.setIncusContainerForRange(0, 4, containerName, incusBin);

    m_mcp.startAll([this](bool ok, QStringList errors) {
        if (!ok)
            for (const QString &e : errors)
                emit appendTools("MCP Fehler: " + e, "error");

        m_chatModel.setSystemPrompt(buildFullSystemPrompt());

        QString toolDebug = "<b>MCP Tools (Container):</b><br>";
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

        emit inputEnabled(true);
        emit statusChanged("Bereit");
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
            "Custom Chat-Template: Fallback ChatML.", "system");
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

    QString presetName   = ChatTemplate::presetName(detectedPreset);
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

    ToolCallFormat::Preset effective =
        (userChoice == ToolCallFormat::Preset::Auto) ? detectedFormat : userChoice;

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

            if (result.prompt.startsWith("__PLAN__:") ||
                result.prompt == "__EXECUTE__"         ||
                result.prompt.startsWith("__IMPORT__:")) {
                emit appendTools(
                    "<b>Hinweis:</b> Dieser Modus ist noch nicht implementiert.",
                    "system");
                emit inputEnabled(true);
                return;
            }

            if (result.prompt == "__SAVEDB__" ||
                result.prompt == "__LOADDB__" ||
                result.prompt == "__CODEASSEMBLE__") {
                emit appendTools(
                    "<b>Hinweis:</b> Datenbank-Funktionen nicht verfügbar.",
                    "system");
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
    m_generating        = false;
    m_continuationCount = 0;
    m_pendingToolCalls.clear();
    m_pendingToolIdx = 0;

    if (m_worker) m_worker->stopGeneration();
    m_toolFailCount.clear();

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
    m_pendingToolCalls.clear();
    m_pendingToolIdx = 0;

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
    m_continuationCount = 0;

    emit appendChat("Chat gelöscht.", "system");
    m_chat->emitStats();
    emit inputEnabled(true);
    emit statusChanged("Bereit");
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
    m_mode = AgentMode::Chat;
    emit modeChanged(m_mode);
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());
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

    if (m_summarizing) {
        m_summarizing = false;
        m_chatModel.clear();
        m_chatModel.setSystemPrompt(buildFullSystemPrompt());
        m_chatModel.addUserMessage(
            QString("[Summary:\n%1]").arg(fullResponse));
        m_chatModel.addAssistantMessage(
            "Understood. I have an overview of the previous conversation.");
        emit appendTools(
            QString("<b>Summary:</b><br><pre style='font-size:10px'>%1</pre>")
            .arg(fullResponse.left(500).toHtmlEscaped()), "system");
        emit inputEnabled(true);
        emit statusChanged("Summary complete");
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
