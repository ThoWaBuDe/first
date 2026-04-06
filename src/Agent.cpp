#include "Agent.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>

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

    // ─── Slash-Kommando? ──────────────────────────────────────────────────
    if (CommandProcessor::isCommand(text)) {
        auto result = m_commands.process(text, m_generating);

        if (result.handled) {
            if (!result.notice.isEmpty())
                emit appendTools(result.notice.toHtmlEscaped(), result.noticeCssClass);

            // Sonderfall: /summarize → direkt summarizeContext()
            if (result.prompt == "__SUMMARIZE__") {
                emit inputEnabled(false);
                emit statusChanged("Zusammenfassen...");
                summarizeContext();
                return;
            }

            // Wenn kein Prompt → fertig (reine UI-Aktion)
            if (result.prompt.isEmpty())
                return;

            // Prompt vom CommandProcessor → ans LLM schicken
            // Das Kommando erscheint als User-Nachricht im Chat
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

    // ─── Normale Nachricht ────────────────────────────────────────────────
    // Kontext-Check bevor neue Nachricht hinzugefügt wird
    checkContextUsage();

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

// ─── onStop ──────────────────────────────────────────────────────────────────
// Fix für den Stop-Bug:
//
// Vorher: m_generating = false, aber laufende Lambda-Callbacks (MCP) und
// eingereihte invokeMethod-Aufrufe prüften m_generating nicht mehr und
// setzten die Continuation fort.
//
// Jetzt: m_sessionId wird inkrementiert. Alle Callbacks und Continuations
// haben ihre sessionId beim Erstellen gespeichert. Sie prüfen zu Beginn:
//   if (mySessionId != m_sessionId) return;  // veraltet → verwerfen
//
// Pattern: Generation Stamp. Analogie: wie ein Versionszähler bei
// optimistischem Locking — veraltete Schreiber erkennen den Konflikt.
void Agent::onStop()
{
    ++m_sessionId;           // invalidiert alle laufenden Callbacks
    m_generating = false;

    if (m_worker) m_worker->stopGeneration();

    m_toolFailCount.clear(); // Deadlock-Zähler zurücksetzen

    emit inputEnabled(true);
    emit statusChanged("Gestoppt");
}

void Agent::onClearChat()
{
    ++m_sessionId;  // laufende Callbacks ungültig machen
    m_generating = false;

    if (m_worker) m_worker->stopGeneration();

    m_chatModel.clear();
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

void Agent::onTokenReceived(const QString &token)
{
    ++m_generatedTokens;
    ++m_totalTokens;
    m_currentResponse += token;
    filterToken(token);
    if (m_generatedTokens % 10 == 0) emitStats();
}

// ─── onGenerationDone ────────────────────────────────────────────────────────
// Agenten-Loop. Wichtig: sessionId wird am Anfang gespeichert und bei
// jedem async Sprung weitergegeben. handleToolCall() bekommt sie als Parameter.
void Agent::onGenerationDone(const QString &fullResponse)
{
    m_generating = false;
    emitStats();

    uint32_t mySession = m_sessionId;

    // ─── Summarize-Pfad ───────────────────────────────────────────────────
    // Wenn wir gerade zusammengefasst haben: Ergebnis in ChatModel einbauen.
    // Strategie: System-Prompt behalten, alle alten Nachrichten löschen,
    // Zusammenfassung als erste User-Nachricht einfügen, dann die
    // letzten 2 echten Paare wiederherstellen wäre komplex — daher einfacher
    // Ansatz: clear() + Zusammenfassung als System-Ergänzung einfügen.
    if (m_summarizing) {
        m_summarizing = false;

        // ChatModel wurde durch summarizeContext() mit Summarize-Prompt befüllt.
        // Wir löschen jetzt alles und starten mit Zusammenfassung neu.
        m_chatModel.clear();

        // Zusammenfassung als erste User-Nachricht (das Modell "erinnert" sich)
        m_chatModel.addUserMessage(
            QString("[Zusammenfassung der bisherigen Konversation:\n%1]")
            .arg(fullResponse));
        m_chatModel.addAssistantMessage("Verstanden. Ich habe die bisherige Konversation im Überblick.");

        emit appendTools(
            QString("<b>Zusammenfassung erstellt:</b><br>"
                    "<pre style='font-size:10px'>%1</pre>")
            .arg(fullResponse.left(500).toHtmlEscaped() +
                 (fullResponse.length() > 500 ? "..." : "")),
            "system");

        emit inputEnabled(true);
        emit statusChanged("Zusammenfassung fertig — Kontext geleert");
        return;
    }  // Snapshot beim Eintreten

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
        handleToolCall(fullResponse, mySession);
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

void Agent::startGeneration(LlamaWorker::SamplerProfile profile)
{
    QMetaObject::invokeMethod(m_worker, "generate",
                              Qt::QueuedConnection,
                              Q_ARG(QString,                     m_chatModel.buildPrompt()),
                              Q_ARG(LlamaWorker::SamplerProfile, profile));
}

// ─── repairJson ──────────────────────────────────────────────────────────────
// Versucht häufige JSON-Fehler zu reparieren die LLMs produzieren:
//
//   1. Fehlende schließende Klammer  → "}" anhängen
//   2. Trailing Comma                → letztes Komma vor } oder ] entfernen
//   3. Einfache Anführungszeichen    → durch doppelte ersetzen
//
// Pattern: Best-Effort Repair — kein vollständiger Parser.
// Gibt reparierten String zurück, oder QString() wenn auch nach Reparatur
// kein gültiges JSON entstanden ist.
QString Agent::repairJson(const QString &broken) const
{
    QString s = broken.trimmed();

    // Versuch 1: direkt parsen (manchmal ist es doch gültig)
    if (!QJsonDocument::fromJson(s.toUtf8()).isNull())
        return s;

    // Versuch 2: fehlende schließende Klammer ergänzen
    // Zähle öffnende und schließende Klammern
    int openObj = s.count('{') - s.count('}');
    int openArr = s.count('[') - s.count(']');
    QString fixed = s;
    for (int i = 0; i < openArr; ++i) fixed += ']';
    for (int i = 0; i < openObj; ++i) fixed += '}';

    if (!QJsonDocument::fromJson(fixed.toUtf8()).isNull())
        return fixed;

    // Versuch 3: Trailing Comma entfernen (,} oder ,])
    // Regex-frei: letztes Komma vor schließender Klammer suchen
    QString noTrailing = fixed;
    for (const QString &close : {QString("}"), QString("]")}) {
        int pos = noTrailing.lastIndexOf(close);
        while (pos > 0) {
            int comma = noTrailing.lastIndexOf(',', pos - 1);
            if (comma < 0) break;
            // Prüfe ob zwischen Komma und Klammer nur Whitespace
            bool onlyWs = true;
            for (int i = comma + 1; i < pos; ++i) {
                if (!noTrailing[i].isSpace()) { onlyWs = false; break; }
            }
            if (onlyWs) {
                noTrailing.remove(comma, 1);
                pos = comma;
            } else {
                break;
            }
        }
    }

    if (!QJsonDocument::fromJson(noTrailing.toUtf8()).isNull())
        return noTrailing;

    // Versuch 4: Einfache Anführungszeichen → doppelte
    // Nur wenn kein doppeltes Anführungszeichen im String vorhanden
    if (!s.contains('"') && s.contains('\'')) {
        QString withDouble = noTrailing;
        withDouble.replace('\'', '"');
        if (!QJsonDocument::fromJson(withDouble.toUtf8()).isNull())
            return withDouble;
    }

    return {};  // Reparatur fehlgeschlagen
}

// ─── toolCallKey ─────────────────────────────────────────────────────────────
// Kanonischer Schlüssel für Deadlock-Erkennung.
// Format: "toolname|arg1=val1|arg2=val2" (Argumente alphabetisch sortiert).
// So werden Aufrufe mit gleichen Argumenten in anderer Reihenfolge
// als identisch erkannt.
QString Agent::toolCallKey(const QString &toolName, const QJsonObject &args) const
{
    QStringList parts;
    parts << toolName;

    QStringList keys = args.keys();
    keys.sort();  // kanonische Reihenfolge
    for (const QString &k : keys) {
        // Wert als kompaktes JSON — funktioniert für alle Typen
        QString val = QString::fromUtf8(
            QJsonDocument(QJsonObject{{k, args[k]}}).toJson(QJsonDocument::Compact));
        parts << val;
    }
    return parts.join('|');
}

// ─── deadlockEscalationPrompt ────────────────────────────────────────────────
// Gibt je nach Fehlerzähler eine Eskalations-Nachricht zurück
// die in den ChatModel-Context eingefügt wird.
QString Agent::deadlockEscalationPrompt(const QString &toolName, int count) const
{
    if (count >= DEADLOCK_ABORT) {
        return QString(
            "[SYSTEM: Tool '%1' ist %2 Mal hintereinander fehlgeschlagen. "
            "ABBRUCH. Erkläre dem Nutzer was schiefgelaufen ist und "
            "was er manuell tun kann.]"
        ).arg(toolName).arg(count);
    }
    if (count >= DEADLOCK_REDIRECT) {
        return QString(
            "[SYSTEM: Tool '%1' schlägt wiederholt fehl (%2 Mal). "
            "Suche einen ANDEREN Weg zum Ziel. Andere Tools, andere Argumente, "
            "oder erkläre warum das Ziel nicht erreichbar ist.]"
        ).arg(toolName).arg(count);
    }
    // DEADLOCK_WARN
    return QString(
        "[SYSTEM: Tool '%1' hat %2 Mal hintereinander den gleichen Fehler "
        "gemeldet. Überprüfe deine Argumente sorgfältig bevor du es erneut versuchst.]"
    ).arg(toolName).arg(count);
}

// ─── handleToolCall ──────────────────────────────────────────────────────────
// sessionId wird an den MCP-Callback übergeben.
// Der Callback prüft als erstes ob seine sessionId noch gültig ist.
//
// JSON-Repair: wenn QJsonDocument::fromJson() scheitert → repairJson() versuchen.
// Bei Erfolg: repariertes JSON verwenden, LLM informieren.
// Bei Misserfolg: Fehler melden, LLM kann mit besserem JSON neu versuchen.
void Agent::handleToolCall(const QString &fullResponse, uint32_t sessionId)
{
    int start     = fullResponse.indexOf("<tool_call>") + 11;
    int end       = fullResponse.indexOf("</tool_call>", start);
    QString block = fullResponse.mid(start, end - start).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);

    // ─── JSON-Repair ──────────────────────────────────────────────────────
    if (doc.isNull()) {
        emit appendTools(
            QString("<b>JSON-Fehler:</b> %1<br><pre>%2</pre>")
            .arg(pe.errorString().toHtmlEscaped(), block.toHtmlEscaped()),
            "error");

        QString repaired = repairJson(block);
        if (!repaired.isEmpty()) {
            doc = QJsonDocument::fromJson(repaired.toUtf8());
            emit appendTools(
                QString("<b>JSON repariert:</b><br><pre>%1</pre>")
                .arg(repaired.toHtmlEscaped()),
                "tool");
        } else {
            // Reparatur fehlgeschlagen → LLM informieren und nochmal lassen
            QString errFeedback = QString(
                "[SYSTEM: Dein Tool-Call enthielt ungültiges JSON. "
                "Fehler: %1. "
                "Bitte sende den Tool-Call erneut mit korrektem JSON.]"
            ).arg(pe.errorString());
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

    // ─── MCP-Callback mit Session-Check ───────────────────────────────────
    // Der Lambda-Callback speichert sessionId und toolCallKey bei seiner
    // Erstellung (Capture by value). Beim Aufruf (async, nach MCP-Antwort)
    // prüft er: bin ich noch aktuell?
    //
    // toolKey für Deadlock-Tracking — auch im Capture gespeichert.
    QString tKey = toolCallKey(toolName, toolArgs);

    m_mcp.callTool(toolName, toolArgs,
        [this, toolName, tKey, sessionId](QString result, QString error) {

            // ── Session-Check (Stop-Fix) ──────────────────────────────────
            // Wenn sessionId nicht mehr aktuell → diese Callback-Instanz
            // stammt aus einer abgebrochenen Session. Verwerfen.
            if (sessionId != m_sessionId) return;

            QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);
            bool    isErr      = !error.isEmpty();

            // ── Deadlock-Erkennung ────────────────────────────────────────
            // Bei Fehler: Zähler erhöhen und Eskalation prüfen.
            // Bei Erfolg: Zähler für diesen Schlüssel löschen.
            if (isErr) {
                int &failCount = m_toolFailCount[tKey];
                ++failCount;

                if (failCount >= DEADLOCK_ABORT) {
                    // Hard Abort: kein weiterer Tool-Call
                    emit appendTools(
                        QString("<b>DEADLOCK ABBRUCH</b>: '%1' hat %2 Mal "
                                "hintereinander versagt.")
                        .arg(toolName.toHtmlEscaped()).arg(failCount),
                        "error");
                    QString abortMsg = deadlockEscalationPrompt(toolName, failCount);
                    m_chatModel.addToolResult(toolName, abortMsg);
                    m_toolFailCount.remove(tKey);

                    // Eine letzte Generierung damit das LLM dem Nutzer erklärt
                    // was schiefgelaufen ist — aber keine weiteren Tool-Calls.
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
                    // Eskalations-Hinweis in den Context
                    QString escalation = deadlockEscalationPrompt(toolName, failCount);
                    emit appendTools(
                        QString("<b>Deadlock-Warnung (Stufe %1):</b> '%2' fehlgeschlagen.")
                        .arg(failCount).arg(toolName.toHtmlEscaped()),
                        "error");
                    // Hinweis wird zusätzlich zum Tool-Ergebnis eingefügt
                    toolResult = toolResult + "\n\n" + escalation;
                }

            } else {
                // Erfolg → Fehlerzähler für diesen Tool-Call löschen
                m_toolFailCount.remove(tKey);
            }

            emit appendTools(
                QString("<b>Ergebnis [%1]:</b><br><pre>%2</pre>")
                .arg(toolName.toHtmlEscaped(), toolResult.toHtmlEscaped()),
                isErr ? "error" : "tool");

            // ── Diff-Anzeige für dateiändernde Tools ─────────────────────
            // Bei str_replace, write_file, append_file: git diff anzeigen.
            // Wir fragen den filesystem-MCP nach dem aktuellen Diff.
            // Das ist asynchron — wir starten es nur wenn kein Fehler.
            static const QStringList fileWriteTools = {
                "str_replace", "write_file", "append_file"
            };
            if (!isErr && fileWriteTools.contains(toolName)) {
                // git_diff mit stat_only:false für vollständigen Diff
                QJsonObject diffArgs;
                diffArgs["stat_only"] = false;
                m_mcp.callTool("git_diff", diffArgs,
                    [this, sessionId](QString diffResult, QString diffError) {
                        if (sessionId != m_sessionId) return;
                        if (!diffError.isEmpty() || diffResult.isEmpty()) return;
                        // Rohen git diff als farbigen HTML-Block anzeigen
                        // Wir nutzen hier den git diff direkt (nicht LCS)
                        // weil git schon den perfekten Diff kennt
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

// ─── filterToken ─────────────────────────────────────────────────────────────
// State Machine: NORMAL ↔ THINK (unverändert)
void Agent::filterToken(const QString &token)
{
    static constexpr int MAX_TAG_LEN = 12;

    if (!m_inThinkBlock) {
        m_thinkBuffer += token;

        if (m_thinkBuffer.endsWith("<think>")) {
            QString before = m_thinkBuffer;
            before.chop(7);
            if (!before.isEmpty())
                emit appendChatToken(before);
            m_inThinkBlock = true;
            m_thinkBuffer.clear();

        } else if (m_thinkBuffer.length() > MAX_TAG_LEN ||
                   !QString("<think>").startsWith(
                       m_thinkBuffer.right(MAX_TAG_LEN))) {
            emit appendChatToken(m_thinkBuffer);
            m_thinkBuffer.clear();
        }

    } else {
        m_thinkBuffer += token;

        if (m_thinkBuffer.endsWith("</think>")) {
            QString thinkText = m_thinkBuffer;
            thinkText.chop(8);
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

void Agent::emitStats()
{
    emit statsUpdated(m_promptTokens, m_generatedTokens,
                      m_totalTokens,  m_ctxSize);
}

// ─── checkContextUsage ───────────────────────────────────────────────────────
// Wird am Anfang von onUserMessage() aufgerufen.
// Prüft ob der Kontext >80% voll ist und löst ggf. auto-summarize aus.
//
// Berechnung: (promptTokens + generatedTokens) / ctxSize * 100
// m_promptTokens wird von onStatsUpdate() aktuell gehalten.
//
// m_summarizing verhindert dass summarizeContext() sich selbst auslöst
// (Rekursionsschutz — analogie AVR: Interrupt-Disable während ISR).
void Agent::checkContextUsage()
{
    if (m_summarizing || m_ctxSize == 0) return;

    int used = m_promptTokens + m_generatedTokens;
    int pct  = (used * 100) / m_ctxSize;

    if (pct >= CTX_SUMMARIZE_THRESHOLD) {
        emit appendTools(
            QString("<b>Kontext bei %1% — automatisches Zusammenfassen...</b>")
            .arg(pct), "system");
        summarizeContext();
    }
}

// ─── summarizeContext ────────────────────────────────────────────────────────
// Fasst die aktuelle Konversation zusammen und kürzt den ChatModel-Context.
//
// Strategie (Pattern: Sliding Window + Summary):
//   1. Alle bisherigen Nachrichten als Text zusammenstellen
//   2. Einen Summarize-Prompt bauen und ans LLM schicken
//   3. Die Zusammenfassung als neue System-Nachricht einfügen
//   4. Alle alten Nachrichten außer den letzten 2 Paaren löschen
//
// "Letzten 2 Paare behalten" = 4 Nachrichten (User+Assistant je 2x).
// Das gibt dem Modell unmittelbaren Kontext ohne den ganzen Verlauf.
//
// Implementierung: Da wir keinen synchronen LLM-Aufruf haben, setzen
// wir m_summarizing = true und starten eine normale Generierung mit
// einem speziellen Summarize-Prompt. In onGenerationDone() wird der
// m_summarizing-Pfad separat behandelt.
void Agent::summarizeContext()
{
    if (m_summarizing) return;
    m_summarizing = true;

    // Alle Nachrichten als lesbaren Text zusammenstellen
    QString history;
    const auto &msgs = m_chatModel.messages();
    for (const auto &msg : msgs) {
        switch (msg.role) {
            case ChatMessage::Role::System:    break;  // System-Prompt überspringen
            case ChatMessage::Role::User:      history += "User: " + msg.content + "\n\n"; break;
            case ChatMessage::Role::Assistant: history += "Assistant: " + msg.content + "\n\n"; break;
            case ChatMessage::Role::Tool:      history += "Tool: " + msg.content + "\n\n"; break;
        }
    }

    // Summarize-Prompt: präzise Anweisung für eine kompakte Zusammenfassung
    // Kein Tool-Use erwünscht — nur Text.
    QString summarizePrompt = QString(
        "Fasse die folgende Konversation in maximal 300 Wörtern zusammen. "
        "Behalte alle wichtigen Fakten, getroffenen Entscheidungen, "
        "Dateipfade, Fehlermeldungen und offene Aufgaben. "
        "Schreibe nur die Zusammenfassung, keine Einleitung.\n\n"
        "--- Konversation ---\n%1\n--- Ende ---"
    ).arg(history.left(12000));  // Limit damit der Prompt nicht explodiert

    // Temporären ChatModel-State für Summarize bauen
    // Wir ersetzen den aktuellen Prompt mit dem Summarize-Prompt
    m_chatModel.addUserMessage(summarizePrompt);

    m_currentResponse.clear();
    m_thinkBuffer.clear();
    m_inThinkBlock    = false;
    m_generatedTokens = 0;
    m_generating      = true;

    startGeneration(LlamaWorker::SamplerProfile::Chat);
}

// ─── computeDiffHtml ─────────────────────────────────────────────────────────
// Berechnet einen visuellen Diff zwischen zwei Texten.
//
// Algorithmus: Longest Common Subsequence (LCS) auf Zeilenebene.
// LCS ist der klassische Diff-Algorithmus (auch in Unix diff verwendet).
//
// Idee:
//   - Teile beide Texte in Zeilen auf
//   - Berechne die längste gemeinsame Teilfolge (LCS)
//   - Alles in LCS = unverändert (grau)
//   - Im Original aber nicht in LCS = gelöscht (rot)
//   - Im Neu aber nicht in LCS = hinzugefügt (grün)
//
// Komplexität: O(m*n) Zeit und Speicher für m/n Zeilen.
// Für typische Dateiedits (<500 Zeilen) völlig akzeptabel.
//
// Analogie AVR: wie CRC-Vergleich zweier Speicherbereiche —
// wir finden die Unterschiede ohne den ganzen Inhalt zu übertragen.
QString Agent::computeDiffHtml(const QString &before, const QString &after,
                                const QString &filename) const
{
    QStringList oldLines = before.split('\n');
    QStringList newLines = after.split('\n');

    int m = oldLines.size();
    int n = newLines.size();

    // LCS-Tabelle aufbauen (Dynamic Programming).
    // dp[i][j] = Länge der LCS von oldLines[0..i-1] und newLines[0..j-1].
    // Speicher: (m+1) * (n+1) integers.
    // Für große Dateien: Limit setzen um Memory-Explosion zu vermeiden.
    if (m > 300 || n > 300) {
        // Fallback für sehr lange Dateien: zeilenweise Ausgabe ohne LCS
        return QString("<b>Diff %1</b> (Datei zu groß für LCS, %2→%3 Zeilen):<br>"
                       "<span style='color:#888'>Vorher: %2 Zeilen | Nachher: %3 Zeilen</span>")
               .arg(filename.toHtmlEscaped()).arg(m).arg(n);
    }

    // dp als flaches Array: dp[i*(n+1)+j]
    std::vector<int> dp((m+1) * (n+1), 0);
    auto at = [&](int i, int j) -> int& { return dp[i*(n+1)+j]; };

    for (int i = 1; i <= m; ++i)
        for (int j = 1; j <= n; ++j)
            at(i,j) = (oldLines[i-1] == newLines[j-1])
                      ? at(i-1,j-1) + 1
                      : std::max(at(i-1,j), at(i,j-1));

    // Rückverfolgung (Backtracking) um die Diff-Sequenz zu rekonstruieren.
    // Wir gehen von dp[m][n] zurück zu dp[0][0].
    // Pattern: Backtracking auf DP-Tabelle — klassisch für LCS/Diff.
    struct DiffLine { enum Type { Equal, Added, Removed } type; QString text; };
    QVector<DiffLine> diffLines;

    int i = m, j = n;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && oldLines[i-1] == newLines[j-1]) {
            diffLines.prepend({DiffLine::Equal, oldLines[i-1]});
            --i; --j;
        } else if (j > 0 && (i == 0 || at(i,j-1) >= at(i-1,j))) {
            diffLines.prepend({DiffLine::Added, newLines[j-1]});
            --j;
        } else {
            diffLines.prepend({DiffLine::Removed, oldLines[i-1]});
            --i;
        }
    }

    // HTML-Ausgabe bauen.
    // Nur veränderte Zeilen + 2 Zeilen Kontext darum (wie unified diff).
    // "Kontext-Fenster": zeige max. 2 unveränderte Zeilen vor/nach Änderung.
    static constexpr int CONTEXT = 2;

    // Markiere welche Zeilen verändert sind
    QVector<bool> changed(diffLines.size(), false);
    for (int k = 0; k < diffLines.size(); ++k)
        if (diffLines[k].type != DiffLine::Equal) changed[k] = true;

    // Kontext-Fenster aufbauen: zeige Zeile k wenn eine Änderung
    // innerhalb von CONTEXT Zeilen liegt
    QVector<bool> show(diffLines.size(), false);
    for (int k = 0; k < diffLines.size(); ++k) {
        if (!changed[k]) continue;
        for (int c = std::max(0, k-CONTEXT);
             c <= std::min((int)diffLines.size()-1, k+CONTEXT); ++c)
            show[c] = true;
    }

    QString html = QString("<b>Diff: %1</b><br>").arg(filename.toHtmlEscaped());
    html += "<pre style='font-size:10px; margin:2px 0;'>";

    bool inGap = false;
    for (int k = 0; k < diffLines.size(); ++k) {
        if (!show[k]) {
            if (!inGap) { html += "<span style='color:#aaa'>...</span>\n"; inGap = true; }
            continue;
        }
        inGap = false;

        const DiffLine &dl = diffLines[k];
        QString escaped = dl.text.toHtmlEscaped();

        switch (dl.type) {
            case DiffLine::Added:
                html += QString("<span style='color:#188038; background:#e6f4ea'>+ %1</span>\n")
                        .arg(escaped);
                break;
            case DiffLine::Removed:
                html += QString("<span style='color:#c5221f; background:#fce8e6'>- %1</span>\n")
                        .arg(escaped);
                break;
            case DiffLine::Equal:
                html += QString("<span style='color:#666'>  %1</span>\n").arg(escaped);
                break;
        }
    }
    html += "</pre>";
    return html;
}
