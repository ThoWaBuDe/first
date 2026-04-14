#include "Agent.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>
#include <QFileInfo>
#include <QDir>

// ─── Whitelist: erlaubte Tools im Plan-Modus ──────────────────────────────────
// Nur Lese-Tools. Schreib-Tools (write_file, str_replace, cmake_build, ...)
// sind im Plan-Modus verboten — das Modell soll nur analysieren, nicht ändern.
//
// Analogie AVR: wie ein Read-only-Segment im Flash — zur Compile-Zeit fest,
// kann nicht versehentlich überschrieben werden.
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

    // ─── TaskTree DB-Pfad setzen ──────────────────────────────────────────
    // Das Verzeichnis muss existieren bevor SQLite die DB anlegt.
    // QDir::mkpath() ist idempotent (kein Fehler wenn Verzeichnis schon da).
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

            // ─── Plan-Modus Einstieg ───────────────────────────────────────
            // Marker: "__PLAN__:<Auftrag>"
            // Analogie AVR: wie eine ISR-Weiche — dieser Pfad verlässt den
            // normalen Hauptpfad und startet den Plan-FSM.
            if (result.prompt.startsWith("__PLAN__:")) {
                QString auftrag = result.prompt.mid(9);  // "__PLAN__:" = 9 Zeichen
                emit appendChat(
                    QString("<b>Plan-Modus:</b> %1").arg(text.toHtmlEscaped()), "user");
                startPlan(auftrag);
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

    // Bei Stop im Plan-Modus: zurück zu Chat
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

// ─── onPlanApproved ──────────────────────────────────────────────────────────
// User hat den Plan bestätigt.
// Speichert den TaskTree in SQLite.
// Execute-Modus wird in einer späteren Session implementiert —
// vorerst zurück zu Chat mit einer Zusammenfassung.
void Agent::onPlanApproved()
{
    if (m_mode != AgentMode::Plan) return;

    // TaskTree persistieren
    m_taskTree.save();
    emit appendTools(
        QString("<b>Plan gespeichert:</b> %1 Knoten in <code>%2</code>")
        .arg(m_taskTree.nodeCount())
        .arg(AppConfig::instance().taskDbPath().toHtmlEscaped()),
        "system");

    // Vorerst zurück zu Chat
    // TODO (nächste Session): m_mode = AgentMode::Execute; startExecute();
    m_mode = AgentMode::Chat;
    emit modeChanged(m_mode);
    m_chatModel.setSystemPrompt(buildFullSystemPrompt());

    emit appendChat(
        "<b>[System]</b> Plan bestätigt und gespeichert. "
        "Execute-Modus folgt in der nächsten Session.", "system");
    emit inputEnabled(true);
    emit statusChanged("Plan gespeichert — bereit");
}

// ─── onPlanRejected ──────────────────────────────────────────────────────────
// User hat den Plan abgelehnt — einfach zurück zu Chat.
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

// ─── onGenerationDone ────────────────────────────────────────────────────────
// Zentrale Weiche: Chat-Modus vs. Plan-Modus.
//
// Im Plan-Modus prüfen wir zuerst ob ein <plan>...</plan> Block da ist.
// Falls ja → handlePlanJson(). Falls nein → handlePlanToolCall() wie normal.
//
// Im Chat-Modus: unveränderte Logik.
void Agent::onGenerationDone(const QString &fullResponse)
{
    m_generating = false;
    emitStats();

    uint32_t mySession = m_sessionId;

    // ─── Plan-Modus ───────────────────────────────────────────────────────
    if (m_mode == AgentMode::Plan) {
        // Modell hat <plan>...</plan> ausgegeben → Plan parsen
        if (fullResponse.contains("<plan>") && fullResponse.contains("</plan>")) {
            handlePlanJson(fullResponse, mySession);
            return;
        }
        // Modell hat einen Tool-Call gemacht → im Plan-Modus nur Lese-Tools
        if (fullResponse.contains("<tool_call>") && fullResponse.contains("</tool_call>")) {
            m_chatModel.addAssistantMessage(fullResponse);
            handlePlanToolCall(fullResponse, mySession);
            return;
        }
        // Unvollständiger Tool-Call → Continuation (wie Chat-Modus)
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
        // Kein Tool-Call, kein Plan → Modell antwortet in Prosa.
        // Das kann passieren wenn Qwen erst erklärt was es tun will.
        // Wir fügen es als Assistent-Nachricht ein und warten auf nächsten Schritt.
        m_chatModel.addAssistantMessage(fullResponse);
        emit appendChat(fullResponse.toHtmlEscaped(), "assistant");
        // Modell soll weitermachen — nochmal generieren ohne neuen User-Input
        m_generating      = true;
        m_generatedTokens = 0;
        m_currentResponse.clear();
        m_thinkBuffer.clear();
        m_inThinkBlock = false;
        emit appendChat("<b>Assistent:</b> ", "assistant");
        startGeneration(LlamaWorker::SamplerProfile::Chat);
        return;
    }

    // ─── Chat-Modus (unverändert) ──────────────────────────────────────────
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
// Wechselt in Plan-Modus, setzt den Planner-System-Prompt und startet
// die erste Generierung.
//
// Ablauf:
//   1. m_mode = Plan
//   2. Alten TaskTree löschen (frischer Start)
//   3. ChatModel komplett neu aufsetzen mit Planner-System-Prompt
//   4. Erste User-Message = der Auftrag
//   5. startGeneration(Chat) — Chat-Sampler weil freie Analyse
void Agent::startPlan(const QString &auftrag)
{
    m_mode = AgentMode::Plan;
    emit modeChanged(m_mode);

    // Alten Tree verwerfen — jeder /plan-Aufruf startet frisch.
    // Gespeicherte DB bleibt erhalten (wird bei Bestätigung überschrieben).
    // Analogie AVR: wie ein Buffer-Reset vor neuem DMA-Transfer.
    // Wir löschen den RAM-Baum indem wir ein neues TaskTree-Objekt erstellen
    // ist nicht möglich da TaskTree kein clear() hat — wir rufen Nodes einzeln ab.
    // Pragmatisch: Wir setzen nur m_taskTree neu — dafür brauchen wir
    // move-assignment. TaskTree hat keinen. Wir geben einen klaren Kommentar
    // und überlassen das Löschen dem Destruktor wenn nötig.
    // TODO: TaskTree::clear() implementieren wenn Execute-Modus kommt.

    m_planRetryCount = 0;
    m_continuationCount = 0;

    // Frisches ChatModel für den Plan — kein alter Kontext
    m_chatModel.clear();
    m_chatModel.setSystemPrompt(buildPlannerSystemPrompt(auftrag));

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock    = false;
    m_generatedTokens = 0;
    m_generating      = true;

    // Auftrag als erste User-Message
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
// Erklärt dem Modell:
//   1. Welche Lese-Tools es hat
//   2. Das <plan>...</plan> Format
//   3. Was in den Feldern erwartet wird
//
// Warum ein eigener System-Prompt statt den normalen?
//   - Der normale enthält alle Tools incl. write_file, cmake_build etc.
//   - Das Modell soll im Plan-Modus NUR lesen
//   - Ein sauberer Prompt verhindert versehentliche Schreiboperationen
//   - Analogie AVR: wie separate Interrupt-Vektortabelle für verschiedene Modi
QString Agent::buildPlannerSystemPrompt(const QString &auftrag) const
{
    Q_UNUSED(auftrag)
    return R"(Du bist ein Planungs-Agent für C++/Qt6 Projekte.

DEINE AUFGABE:
1. Analysiere das Projekt mit den verfügbaren Lese-Tools
2. Erstelle danach GENAU EINEN <plan>...</plan> Block

ERLAUBTE TOOLS (nur Lesen, kein Schreiben!):

read_file — Datei lesen
  <tool_call>{"name": "read_file", "arguments": {"path": "datei.cpp"}}</tool_call>
  Mit Range: {"path": "datei.cpp", "start_line": 1, "end_line": 50}

list_dir — Verzeichnis auflisten
  <tool_call>{"name": "list_dir", "arguments": {"path": "."}}</tool_call>

get_symbol — Symbol in Datei suchen
  <tool_call>{"name": "get_symbol", "arguments": {"path": "datei.cpp", "symbol": "MyClass"}}</tool_call>

get_project_index — Projektübersicht (Markdown-Index)
  <tool_call>{"name": "get_project_index", "arguments": {}}</tool_call>

get_time, sys_info, disk_free, get_pwd — Systeminfos

VERBOTEN: write_file, str_replace, append_file, cmake_build, check_run

PLAN-FORMAT:
Wenn du genug analysiert hast, gib GENAU DIESEN Block aus:

<plan>
{
  "goal": "Kurzer Titel des Gesamtziels (H0)",
  "children": [
    {
      "title": "H1-Aufgabe (z.B. Dateistruktur)",
      "level": 1,
      "scope": "external",
      "description": "Was hier zu tun ist. Präzise.",
      "children": [
        {
          "title": "H2-Unteraufgabe (z.B. MainWindow.h)",
          "level": 2,
          "scope": "internal",
          "description": "Konkrete Impl-Hints, erwartete Signaturen, Abhängigkeiten.",
          "dependsOn": []
        }
      ]
    }
  ]
}
</plan>

REGELN FÜR DEN PLAN:
- level: 1 = Dateigruppe/Modul, 2 = einzelne Klasse/Datei, 3 = Impl-Detail
- scope: "external" = öffentliches Interface, "internal" = Implementierungsdetail
- dependsOn: Liste von Titeln anderer H2-Knoten die vorher fertig sein müssen (kann leer sein)
- description: Präzise — was genau implementiert werden muss, welche Signaturen, welche Patterns
- Keine Prosa nach dem </plan> Block

Antworte auf Deutsch.)";
}

// ─── handlePlanToolCall ───────────────────────────────────────────────────────
// Wie handleToolCall() im Chat-Modus, aber mit Whitelist.
//
// Schreib-Tools → Fehlermeldung ans Modell (kein Absturz, kein Modus-Wechsel).
// Das Modell bekommt: "[SYSTEM: Tool 'write_file' ist im Plan-Modus verboten...]"
// und soll dann einen Lese-Tool oder den <plan> Block verwenden.
//
// Deadlock-Schutz greift wie im Chat-Modus (gleicher m_toolFailCount).
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

    // ─── Whitelist-Prüfung ────────────────────────────────────────────────
    // Schreib-Tools sind im Plan-Modus verboten.
    // Analogie AVR: wie ein Schreibschutz-Register — Zugriff wird blockiert,
    // Fehler wird gemeldet, kein Absturz.
    if (!PLAN_ALLOWED_TOOLS.contains(toolName)) {
        QString errMsg = QString(
            "[SYSTEM: Tool '%1' ist im Plan-Modus VERBOTEN. "
            "Im Plan-Modus darf nur gelesen werden (read_file, list_dir, get_symbol, ...). "
            "Erstelle stattdessen den <plan>...</plan> Block wenn du genug analysiert hast.]")
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

    // ─── Erlaubtes Tool ausführen ─────────────────────────────────────────
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

            // Kürzen wenn zu lang
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
// Parst den <plan>...</plan> Block und baut m_taskTree auf.
//
// Fehlerbehandlung:
//   1. Versuch: JSON direkt parsen
//   2. Versuch: repairJson() + nochmal parsen
//   3. Falls noch Fehler: Retry-Generierung (max MAX_PLAN_RETRIES = 1)
//   4. Nach Retry-Erschöpfung: Fehler, zurück zu Chat
//
// Bei Erfolg: emit planReady() → PlannerDock zeigt Tree + Buttons.
void Agent::handlePlanJson(const QString &fullResponse, uint32_t sessionId)
{
    // JSON aus <plan>...</plan> extrahieren
    int planStart = fullResponse.indexOf("<plan>") + 6;  // "<plan>" = 6 Zeichen
    int planEnd   = fullResponse.indexOf("</plan>", planStart);
    QString planJson = fullResponse.mid(planStart, planEnd - planStart).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(planJson.toUtf8(), &pe);

    if (doc.isNull()) {
        emit appendTools(
            QString("<b>Plan JSON-Fehler:</b> %1<br><pre>%2</pre>")
            .arg(pe.errorString().toHtmlEscaped(),
                 planJson.left(300).toHtmlEscaped()), "error");

        // JSON-Repair versuchen
        QString repaired = repairJson(planJson);
        if (!repaired.isEmpty()) {
            doc = QJsonDocument::fromJson(repaired.toUtf8());
            emit appendTools("<b>Plan JSON repariert.</b>", "system");
        } else {
            // Retry: Modell soll Plan nochmal ausgeben
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
                "mit korrektem JSON. Achte auf korrekte Anführungszeichen und Kommas.]")
                .arg(pe.errorString());
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

    // ─── JSON valide → TaskTree aufbauen ─────────────────────────────────
    QJsonObject root = doc.object();
    QString goalTitle = root.value("goal").toString("Unbenanntes Ziel");

    // H0-Wurzel-Knoten erstellen
    // Analogie AVR: wie das Initialisieren des Stack-Pointers — erster Schritt
    // bevor irgendwas anderes passiert.
    TaskNode *goalNode = m_taskTree.createNode(
        goalTitle, "", static_cast<int>(TaskLevel::Goal),
        TaskScope::External, 0, nullptr);
    goalNode->status = TaskStatus::Pending;

    // Kinder rekursiv parsen
    QJsonArray children = root.value("children").toArray();
    int nodeCount = 1;  // goalNode zählt mit
    for (int i = 0; i < children.size(); ++i) {
        nodeCount += parsePlanNode(children[i].toObject(), goalNode, 1);
    }

    emit appendTools(
        QString("<b>Plan erstellt:</b> %1 Knoten, Ziel: <i>%2</i>")
        .arg(nodeCount).arg(goalTitle.toHtmlEscaped()), "system");

    // TaskTree-Modell aktualisieren → PlannerDock zeigt neuen Baum
    emit taskTreeUpdated();

    // Plan-Approval: User muss bestätigen
    // inputEnabled(true) damit User die Buttons klicken kann
    emit planReady();
    emit inputEnabled(true);
    emit statusChanged("Plan bereit — bitte bestätigen oder ablehnen");
}

// ─── parsePlanNode ────────────────────────────────────────────────────────────
// Rekursiver Aufbau des TaskTree aus einem JSON-Objekt.
//
// Pattern: Composite (GoF) — jeder Knoten kann wieder Kinder haben.
// Rekursionstiefe entspricht der Hierarchie-Tiefe (H1, H2, H3, ...).
//
// Fehlende Felder werden mit Defaults gefüllt:
//   title       → "Unbenannte Aufgabe"
//   level       → depth (aus Rekursionstiefe)
//   scope       → Internal
//   description → ""
//   dependsOn   → leer
//
// Warum depth statt level aus JSON?
//   Das Modell könnte falsche Level angeben. Wir trauen der Struktur
//   (Verschachtelung) mehr als dem expliziten level-Wert.
//   Falls level explizit angegeben ist, bevorzugen wir es trotzdem —
//   das Modell weiß manchmal mehr als die Struktur.
int Agent::parsePlanNode(const QJsonObject &obj, TaskNode *parent, int depth)
{
    QString title = obj.value("title").toString(
        QString("Unbenannte Aufgabe (H%1)").arg(depth));

    // level: aus JSON bevorzugt, sonst aus Rekursionstiefe
    int level = obj.contains("level")
                ? obj.value("level").toInt(depth)
                : depth;

    TaskScope scope = TaskScope::Internal;
    if (obj.value("scope").toString() == "external")
        scope = TaskScope::External;

    QString description = obj.value("description").toString();

    // order: Position unter den Geschwistern (für Sortierung)
    // Falls nicht im JSON: insertionIdx übernimmt die Sortierung
    int order = obj.value("order").toInt(0);

    TaskNode *node = m_taskTree.createNode(
        title, description, level, scope, order, parent);

    // dependsOn: Titel-Strings → IDs auflösen
    // Das Modell gibt Titel an (lesbar), wir wandeln in IDs um.
    // Auflösung jetzt ist nicht möglich (referenzierte Knoten vielleicht
    // noch nicht erstellt) — wir speichern die Titel und lösen nach
    // vollständigem Parsen auf. Hier erstmal überspringen.
    // TODO: dependsOn-Auflösung nach vollständigem Parsen (zweiter Pass)

    // Kinder rekursiv
    QJsonArray children = obj.value("children").toArray();
    int count = 1;  // dieser Knoten
    for (int i = 0; i < children.size(); ++i)
        count += parsePlanNode(children[i].toObject(), node, depth + 1);

    return count;
}

// ═════════════════════════════════════════════════════════════════════════════
// PRIVATE METHODEN (unverändert aus vorheriger Version)
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
