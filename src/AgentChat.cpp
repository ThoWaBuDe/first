#include "AgentChat.h"
#include "Agent.h"
#include "AgentUtils.h"
#include "AppConfig.h"
#include <QJsonDocument>
#include <QJsonObject>

// ─── handleToolCall ───────────────────────────────────────────────────────────
// Verarbeitet einen Tool-Call im normalen Chat-Modus.
// Alle Tools erlaubt (keine Whitelist wie Plan/Execute).
// Nach Tool-Ergebnis: mit Tool-Sampler weiter generieren.
void AgentChat::handleToolCall(const QString &fullResponse, uint32_t sessionId)
{
    int start     = fullResponse.indexOf("<tool_call>") + 11;
    int end       = fullResponse.indexOf("</tool_call>", start);
    QString block = fullResponse.mid(start, end - start).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);

    if (doc.isNull()) {
        emit m_agent.appendTools(
            QString("<b>JSON-Fehler:</b> %1<br><pre>%2</pre>")
            .arg(pe.errorString().toHtmlEscaped(), block.toHtmlEscaped()), "error");

        QString repaired = AgentUtils::repairJson(block);
        if (!repaired.isEmpty()) {
            doc = QJsonDocument::fromJson(repaired.toUtf8());
            emit m_agent.appendTools(
                QString("<b>JSON repariert:</b><br><pre>%1</pre>")
                .arg(repaired.toHtmlEscaped()), "tool");
        } else {
            QString errFeedback = QString(
                "[SYSTEM: Dein Tool-Call enthielt ungültiges JSON. Fehler: %1. "
                "Bitte sende den Tool-Call erneut mit korrektem JSON.]")
                .arg(pe.errorString());
            m_agent.m_chatModel.addToolResult("json_error", errFeedback);
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.m_currentResponse.clear();
            m_agent.m_thinkBuffer.clear();
            m_agent.m_inThinkBlock = false;
            emit m_agent.appendChat("<b>Assistent:</b> ", "assistant");
            emit m_agent.statusChanged("JSON-Fehler — nochmal...");
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
    }

    QString     toolName = doc.object().value("name").toString();
    QJsonObject toolArgs = doc.object().value("arguments").toObject();

    m_agent.m_logger.logToolCall(toolName,
        QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Indented)));

    emit m_agent.appendTools(
        QString("<b>Tool-Call: %1</b><br><pre>%2</pre>")
        .arg(toolName.toHtmlEscaped(),
             QString::fromUtf8(QJsonDocument(toolArgs)
                               .toJson(QJsonDocument::Indented)).toHtmlEscaped()),
        "tool");

    if (!m_agent.m_mcp.containsTool(toolName)) {
        QString errMsg = QString("Fehler: Unbekanntes Tool '%1'.").arg(toolName);
        emit m_agent.appendTools(errMsg, "error");
        m_agent.m_chatModel.addToolResult(toolName, errMsg);
        m_agent.m_generating      = true;
        m_agent.m_generatedTokens = 0;
        m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    emit m_agent.statusChanged(QString("Tool: %1...").arg(toolName));
    QString tKey = AgentUtils::toolCallKey(toolName, toolArgs);

    m_agent.m_mcp.callTool(toolName, toolArgs,
        [this, toolName, tKey, sessionId](QString result, QString error) {
            if (sessionId != m_agent.m_sessionId) return;

            QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);
            bool    isErr      = !error.isEmpty();

            // Token-Budget: lange Ergebnisse kürzen
            if (!isErr && toolResult.length() > AppConfig::instance().maxToolResultChars()) {
                int maxChars = AppConfig::instance().maxToolResultChars();
                int cut = toolResult.lastIndexOf('\n', maxChars);
                if (cut < maxChars / 2) cut = maxChars;
                toolResult = toolResult.left(cut)
                    + QString("\n\n[... gekürzt: %1 von %2 Zeichen angezeigt.]")
                      .arg(cut).arg(result.length());
            }

            if (isErr) {
                int &failCount = m_agent.m_toolFailCount[tKey];
                ++failCount;
                if (failCount >= AgentUtils::DEADLOCK_ABORT) {
                    emit m_agent.appendTools(
                        QString("<b>DEADLOCK ABBRUCH</b>: '%1' hat %2 Mal versagt.")
                        .arg(toolName.toHtmlEscaped()).arg(failCount), "error");
                    m_agent.m_chatModel.addToolResult(toolName,
                        AgentUtils::deadlockEscalationPrompt(toolName, failCount));
                    m_agent.m_toolFailCount.remove(tKey);
                    m_agent.m_generating      = true;
                    m_agent.m_generatedTokens = 0;
                    m_agent.m_currentResponse.clear();
                    m_agent.m_thinkBuffer.clear();
                    m_agent.m_inThinkBlock = false;
                    emit m_agent.appendChat("<b>Assistent:</b> ", "assistant");
                    emit m_agent.statusChanged("Abbruch...");
                    m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
                    return;
                }
                if (failCount == AgentUtils::DEADLOCK_WARN ||
                    failCount == AgentUtils::DEADLOCK_REDIRECT) {
                    emit m_agent.appendTools(
                        QString("<b>Deadlock-Warnung (Stufe %1):</b> '%2' fehlgeschlagen.")
                        .arg(failCount).arg(toolName.toHtmlEscaped()), "error");
                    toolResult += "\n\n" +
                        AgentUtils::deadlockEscalationPrompt(toolName, failCount);
                }
            } else {
                m_agent.m_toolFailCount.remove(tKey);
            }

            emit m_agent.appendTools(
                QString("<b>Ergebnis [%1]:</b><br><pre>%2</pre>")
                .arg(toolName.toHtmlEscaped(), toolResult.toHtmlEscaped()),
                isErr ? "error" : "tool");

            m_agent.m_logger.logToolResult(toolName, toolResult, isErr);

            // Git-Diff nach Schreib-Tools anzeigen
            static const QStringList fileWriteTools =
                {"str_replace", "write_file", "append_file"};
            if (!isErr && fileWriteTools.contains(toolName)) {
                QJsonObject diffArgs;
                diffArgs["stat_only"] = false;
                m_agent.m_mcp.callTool("git_diff", diffArgs,
                    [this, sessionId](QString diffResult, QString diffError) {
                        if (sessionId != m_agent.m_sessionId) return;
                        if (!diffError.isEmpty() || diffResult.isEmpty()) return;
                        QString html = "<b>Git Diff:</b><br>"
                                       "<pre style='font-size:10px'>";
                        for (const QString &line : diffResult.split('\n')) {
                            QString esc = line.toHtmlEscaped();
                            if (line.startsWith('+') && !line.startsWith("+++"))
                                html += QString("<span style='color:#188038;"
                                        "background:#e6f4ea'>%1</span>\n").arg(esc);
                            else if (line.startsWith('-') && !line.startsWith("---"))
                                html += QString("<span style='color:#c5221f;"
                                        "background:#fce8e6'>%1</span>\n").arg(esc);
                            else if (line.startsWith("@@"))
                                html += QString("<span style='color:#1a73e8'>"
                                        "%1</span>\n").arg(esc);
                            else
                                html += QString("<span style='color:#666'>"
                                        "%1</span>\n").arg(esc);
                        }
                        html += "</pre>";
                        emit m_agent.appendTools(html, "tool");
                    });
            }

            m_agent.m_chatModel.addToolResult(toolName, toolResult);
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.m_currentResponse.clear();
            m_agent.m_thinkBuffer.clear();
            m_agent.m_inThinkBlock = false;
            emit m_agent.appendChat("<b>Assistent:</b> ", "assistant");
            emit m_agent.statusChanged("Tool-Ergebnis verarbeiten...");
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
        });
}

// ─── filterToken ─────────────────────────────────────────────────────────────
// Zustandsautomat für <think>-Block-Erkennung.
//
// Zustand 0 (m_inThinkBlock = false):
//   Tokens akkumulieren bis <think> erkannt wird.
//   Alles vor <think> → chatView.
//   <think> → Zustand 1 wechseln.
//
// Zustand 1 (m_inThinkBlock = true):
//   Tokens akkumulieren bis </think> erkannt wird.
//   Thinking-Text → toolView (komprimiert).
//   </think> → Zustand 0 wechseln.
//
// MAX_TAG_LEN: Puffer-Fenster in dem wir auf Tag-Anfang warten.
// Größer als der längste Tag ("</think>" = 8 Zeichen) + Sicherheit.
void AgentChat::filterToken(const QString &token)
{
    static constexpr int MAX_TAG_LEN = 12;

    if (!m_agent.m_inThinkBlock) {
        m_agent.m_thinkBuffer += token;

        if (m_agent.m_thinkBuffer.endsWith("<think>")) {
            // <think> gefunden: alles davor ausgeben, Think-Modus aktivieren
            QString before = m_agent.m_thinkBuffer;
            before.chop(7);
            if (!before.isEmpty()) emit m_agent.appendChatToken(before);
            m_agent.m_inThinkBlock = true;
            m_agent.m_thinkBuffer.clear();

        } else if (m_agent.m_thinkBuffer.length() > MAX_TAG_LEN ||
                   !QString("<think>").startsWith(
                       m_agent.m_thinkBuffer.right(MAX_TAG_LEN))) {
            // Kein Tag-Anfang im Puffer → direkt ausgeben
            emit m_agent.appendChatToken(m_agent.m_thinkBuffer);
            m_agent.m_thinkBuffer.clear();
        }

    } else {
        m_agent.m_thinkBuffer += token;

        if (m_agent.m_thinkBuffer.endsWith("</think>")) {
            // </think> gefunden: Thinking-Text in toolView, Think-Modus deaktivieren
            QString thinkText = m_agent.m_thinkBuffer;
            thinkText.chop(8);
            emit m_agent.appendTools(
                QString("<b>Thinking:</b><br>"
                        "<span style='white-space:pre-wrap'>%1</span>")
                .arg(thinkText.trimmed().toHtmlEscaped()), "think");
            m_agent.m_logger.logThinking(thinkText.trimmed());
            m_agent.m_inThinkBlock = false;
            m_agent.m_thinkBuffer.clear();
        }
    }
}

// ─── summarizeContext ─────────────────────────────────────────────────────────
// Fasst die bisherige Konversation zusammen um Kontext freizumachen.
// Nach der Zusammenfassung: ChatModel geleert, Zusammenfassung als erste Nachricht.
//
// Warum ChatModel leeren statt alten Nachrichten löschen?
//   Einzelne Nachrichten zu löschen würde den Gesprächsfaden zerreißen.
//   Eine kompakte Zusammenfassung als "User"-Nachricht gibt dem Modell
//   einen kohärenten Ausgangspunkt.
void AgentChat::summarizeContext()
{
    if (m_agent.m_summarizing) return;
    m_agent.m_summarizing = true;

    // Gesprächshistorie als Text aufbauen
    QString history;
    for (const auto &msg : m_agent.m_chatModel.messages()) {
        switch (msg.role) {
            case ChatMessage::Role::System:    break;
            case ChatMessage::Role::User:
                history += "User: " + msg.content + "\n\n"; break;
            case ChatMessage::Role::Assistant:
                history += "Assistant: " + msg.content + "\n\n"; break;
            case ChatMessage::Role::Tool:
                history += "Tool: " + msg.content + "\n\n"; break;
        }
    }

    QString summarizePrompt = QString(
        "Fasse die folgende Konversation in maximal 300 Wörtern zusammen. "
        "Behalte alle wichtigen Fakten, getroffenen Entscheidungen, "
        "Dateipfade, Fehlermeldungen und offene Aufgaben. "
        "Schreibe nur die Zusammenfassung, keine Einleitung.\n\n"
        "--- Konversation ---\n%1\n--- Ende ---"
    ).arg(history.left(12000));

    m_agent.m_chatModel.addUserMessage(summarizePrompt);
    m_agent.m_currentResponse.clear();
    m_agent.m_thinkBuffer.clear();
    m_agent.m_inThinkBlock    = false;
    m_agent.m_generatedTokens = 0;
    m_agent.m_generating      = true;
    m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
}

// ─── emitStats ────────────────────────────────────────────────────────────────
void AgentChat::emitStats()
{
    emit m_agent.statsUpdated(
        m_agent.m_promptTokens,
        m_agent.m_generatedTokens,
        m_agent.m_totalTokens,
        m_agent.m_ctxSize);
}

// ─── checkContextUsage ────────────────────────────────────────────────────────
void AgentChat::checkContextUsage()
{
    if (m_agent.m_summarizing || m_agent.m_ctxSize == 0) return;
    int used = m_agent.m_promptTokens + m_agent.m_generatedTokens;
    int pct  = (used * 100) / m_agent.m_ctxSize;
    if (pct >= AppConfig::instance().summarizeThreshold()) {
        emit m_agent.appendTools(
            QString("<b>Kontext bei %1% — automatisches Zusammenfassen...</b>")
            .arg(pct), "system");
        summarizeContext();
    }
}
