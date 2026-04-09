#include "Agent.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>
#include <QFileInfo>

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
    // QVector<ChatMessage> muss als Metatyp registriert sein damit
    // invokeMethod() es über die Thread-Grenze kopieren kann.
    // Qt kopiert den QVector vollständig — thread-sicher.
    qRegisterMetaType<QVector<ChatMessage>>("QVector<ChatMessage>");

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

    QString binDir = QCoreApplication::applicationDirPath();
    m_mcp.addServer(binDir + "/mcp-servers/filesystem/llamaqt-filesystem");
    m_mcp.addServer(binDir + "/mcp-servers/sysinfo/llamaqt-sysinfo");
    m_mcp.addServer(binDir + "/mcp-servers/compile/llamaqt-compile");
    m_mcp.addServer(binDir + "/mcp-servers/websearch/llamaqt-websearch");

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

// applyChatTemplate() wird weiterhin aufgerufen um ChatModel das richtige
// Template zu geben — der Fallback im Worker braucht es.
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

            if (result.prompt.isEmpty()) return;

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
    emit inputEnabled(true);
    emit statusChanged("Gestoppt");
}

void Agent::onClearChat()
{
    ++m_sessionId;
    m_generating = false;
    if (m_worker) m_worker->stopGeneration();

    m_chatModel.clear();
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock      = false;
    m_generatedTokens   = 0;
    m_totalTokens       = 0;
    m_promptTokens      = 0;
    m_toolFailCount.clear();

    emit appendChat("Chat gelöscht.", "system");
    emitStats();
    emit inputEnabled(true);
    emit statusChanged("Bereit");
}

void Agent::onFileSavedByUser(const QString &filePath)
{
    // Dateiname ohne vollen Pfad für die Nachricht (lesbarer)
    QString name = QFileInfo(filePath).fileName();

    // Systemnachricht in den Chat — sichtbar für User und Modell.
    // cssClass "system" = grau/kursiv (wie andere Systemhinweise).
    QString notice = QString(
                         "[System: User hat <b>%1</b> manuell gespeichert. "
                         "Bitte Datei vor weiteren Änderungen neu einlesen.]")
                         .arg(name.toHtmlEscaped());

    emit appendChat(notice, "system");

    // Auch in den ChatModel-Kontext injizieren damit das Modell es sieht.
    // Als Tool-Ergebnis formatiert: das Modell kennt dieses Format bereits.
    m_chatModel.addToolResult("editor_notify",
                              QString("[User hat '%1' manuell bearbeitet und gespeichert. "
                                      "Bitte read_file aufrufen bevor du str_replace oder "
                                      "write_file verwendest.]").arg(name));

    m_logger.logSystem(QString("User hat %1 manuell gespeichert.").arg(filePath));
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
// PRIVATE METHODEN
// ═════════════════════════════════════════════════════════════════════════════

// ─── startGeneration ─────────────────────────────────────────────────────────
// Kernänderung: übergibt m_chatModel.messages() statt buildPrompt().
//
// Qt kopiert den QVector<ChatMessage> vollständig beim invokeMethod() —
// da QueuedConnection eine tiefe Kopie macht. Damit ist der Worker-Thread
// nie von Änderungen im GUI-Thread betroffen während er generiert.
//
// Analogie AVR: wie du einen Puffer in den DMA-Bereich kopierst bevor
// du den DMA startest — der ursprüngliche Puffer kann danach verändert
// werden ohne die laufende Übertragung zu stören.
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
