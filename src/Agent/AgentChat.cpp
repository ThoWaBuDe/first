#include <QJsonDocument>
#include <QJsonObject>

#include "AgentChat.h"
#include "Agent.h"
#include "AgentUtils.h"
#include "Config/AppConfig.h"

// ─── handleToolCall ───────────────────────────────────────────────────────────
// NEU: format-agnostisch via ToolCallParser.
// Unterstützt Mistral-Arrays: mehrere Tool-Calls sequenziell ausführen.
void AgentChat::handleToolCall(const QString &fullResponse, uint32_t sessionId)
{
    ToolCallFormat::Preset fmt = m_agent.m_activeToolFormat;
    QVector<ParsedToolCall> calls = ToolCallParser::parseAll(fullResponse, fmt);

    if (calls.isEmpty()) {
        emit m_agent.appendTools(
            QString("<b>Tool-Call:</b> Kein gültiger Call im Format '%1' gefunden.")
            .arg(ToolCallFormat::presetName(fmt).toHtmlEscaped()), "error");
        m_agent.m_chatModel.addToolResult("parse_error",
            QString("[SYSTEM: Kein Tool-Call im Format '%1' gefunden. "
                    "Bitte erneut senden.]")
            .arg(ToolCallFormat::presetName(fmt)));
        m_agent.m_generating      = true;
        m_agent.m_generatedTokens = 0;
        m_agent.m_currentResponse.clear();
        m_agent.m_thinkBuffer.clear();
        m_agent.m_inThinkBlock = false;
        emit m_agent.appendChat("<b>Assistent:</b> ", "assistant");
        m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    // JSON-Fehler in einzelnen Calls abfangen
    for (const ParsedToolCall &call : calls) {
        if (!call.valid) {
            emit m_agent.appendTools(
                QString("<b>JSON-Fehler:</b> %1").arg(call.error.toHtmlEscaped()),
                "error");

            // Repair-Versuch (nur für XML-Format sinnvoll)
            if (fmt == ToolCallFormat::Preset::QwenXmlTags ||
                fmt == ToolCallFormat::Preset::Generic) {
                int start = fullResponse.indexOf("<tool_call>") + 11;
                int end   = fullResponse.indexOf("</tool_call>", start);
                if (start > 10 && end > start) {
                    QString block = fullResponse.mid(start, end - start).trimmed();
                    QString repaired = AgentUtils::repairJson(block);
                    if (!repaired.isEmpty()) {
                        QJsonDocument doc = QJsonDocument::fromJson(repaired.toUtf8());
                        if (!doc.isNull()) {
                            emit m_agent.appendTools("<b>JSON repariert.</b>", "tool");
                            executeToolCall(
                                ParsedToolCall::ok(
                                    doc.object().value("name").toString(),
                                    doc.object().value("arguments").toObject()),
                                {}, sessionId);
                            return;
                        }
                    }
                }
            }

            m_agent.m_chatModel.addToolResult("json_error",
                QString("[SYSTEM: Ungültiges JSON. Fehler: %1. "
                        "Bitte korrektes JSON verwenden.]").arg(call.error));
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.m_currentResponse.clear();
            m_agent.m_thinkBuffer.clear();
            m_agent.m_inThinkBlock = false;
            emit m_agent.appendChat("<b>Assistent:</b> ", "assistant");
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
    }

    // Sequenzielle Ausführung
    if (calls.size() == 1) {
        executeToolCall(calls.first(), {}, sessionId);
    } else {
        emit m_agent.appendTools(
            QString("<b>Tool-Array (%1 Calls):</b> sequenziell ausführen.")
            .arg(calls.size()), "system");
        m_agent.m_pendingToolCalls = calls;
        m_agent.m_pendingToolIdx   = 0;
        executeNextPendingCall(sessionId);
    }
}

// ─── executeNextPendingCall ───────────────────────────────────────────────────
void AgentChat::executeNextPendingCall(uint32_t sessionId)
{
    if (m_agent.m_pendingToolIdx >= m_agent.m_pendingToolCalls.size()) {
        m_agent.m_pendingToolCalls.clear();
        m_agent.m_pendingToolIdx = 0;
        m_agent.m_generating      = true;
        m_agent.m_generatedTokens = 0;
        m_agent.m_currentResponse.clear();
        m_agent.m_thinkBuffer.clear();
        m_agent.m_inThinkBlock = false;
        emit m_agent.appendChat("<b>Assistent:</b> ", "assistant");
        emit m_agent.statusChanged("Alle Tool-Calls abgeschlossen...");
        m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
        return;
    }

    const ParsedToolCall &call = m_agent.m_pendingToolCalls[m_agent.m_pendingToolIdx];
    ++m_agent.m_pendingToolIdx;

    int remaining = m_agent.m_pendingToolCalls.size() - m_agent.m_pendingToolIdx;
    executeToolCall(call,
        remaining > 0 ? QString(" [%1 weitere]").arg(remaining) : QString(),
        sessionId);
}

// ─── executeToolCall ──────────────────────────────────────────────────────────
void AgentChat::executeToolCall(const ParsedToolCall &call,
                                 const QString &queueSuffix,
                                 uint32_t sessionId)
{
    const QString &toolName   = call.name;
    const QJsonObject &toolArgs = call.arguments;

    m_agent.m_logger.logToolCall(toolName,
        QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Indented)));

    emit m_agent.appendTools(
        QString("<b>Tool-Call: %1</b>%2<br><pre>%3</pre>")
        .arg(toolName.toHtmlEscaped())
        .arg(queueSuffix.toHtmlEscaped())
        .arg(QString::fromUtf8(QJsonDocument(toolArgs)
                               .toJson(QJsonDocument::Indented)).toHtmlEscaped()),
        "tool");

    if (!m_agent.m_mcp.containsTool(toolName)) {
        QString errMsg = QString("Fehler: Unbekanntes Tool '%1'.").arg(toolName);
        emit m_agent.appendTools(errMsg, "error");
        m_agent.m_chatModel.addToolResult(toolName, errMsg);
        if (!m_agent.m_pendingToolCalls.isEmpty()) {
            executeNextPendingCall(sessionId);
        } else {
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
        }
        return;
    }

    emit m_agent.statusChanged(QString("Tool: %1...").arg(toolName));
    QString tKey = AgentUtils::toolCallKey(toolName, toolArgs);

    m_agent.m_mcp.callTool(toolName, toolArgs,
        [this, toolName, tKey, sessionId](QString result, QString error) {
            if (sessionId != m_agent.m_sessionId) return;

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
                int &failCount = m_agent.m_toolFailCount[tKey];
                ++failCount;
                if (failCount >= AgentUtils::DEADLOCK_ABORT) {
                    emit m_agent.appendTools(
                        QString("<b>DEADLOCK ABBRUCH</b>: '%1' hat %2 Mal versagt.")
                        .arg(toolName.toHtmlEscaped()).arg(failCount), "error");
                    m_agent.m_chatModel.addToolResult(toolName,
                        AgentUtils::deadlockEscalationPrompt(toolName, failCount));
                    m_agent.m_toolFailCount.remove(tKey);
                    m_agent.m_pendingToolCalls.clear();
                    m_agent.m_pendingToolIdx = 0;
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
                        QString("<b>Deadlock-Warnung (Stufe %1):</b> '%2'")
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

            // Git-Diff nach Schreib-Tools
            static const QStringList fileWriteTools =
                {"str_replace", "write_file", "append_file"};
            if (!isErr && fileWriteTools.contains(toolName)) {
                QJsonObject diffArgs;
                m_agent.m_mcp.callTool("git_diff", diffArgs,
                    [this, sessionId](QString diffResult, QString diffError) {
                        if (sessionId != m_agent.m_sessionId) return;
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
                        emit m_agent.appendTools(html, "tool");
                    });
            }

            m_agent.m_chatModel.addToolResult(toolName, toolResult);

            // Bei Array: nächsten Call oder Generation starten
            if (!m_agent.m_pendingToolCalls.isEmpty() &&
                m_agent.m_pendingToolIdx < m_agent.m_pendingToolCalls.size()) {
                executeNextPendingCall(sessionId);
            } else {
                m_agent.m_pendingToolCalls.clear();
                m_agent.m_pendingToolIdx = 0;
                m_agent.m_generating      = true;
                m_agent.m_generatedTokens = 0;
                m_agent.m_currentResponse.clear();
                m_agent.m_thinkBuffer.clear();
                m_agent.m_inThinkBlock = false;
                emit m_agent.appendChat("<b>Assistent:</b> ", "assistant");
                emit m_agent.statusChanged("Tool-Ergebnis verarbeiten...");
                m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
            }
        });
}

// ─── filterToken ─────────────────────────────────────────────────────────────
void AgentChat::filterToken(const QString &token)
{
    static constexpr int MAX_TAG_LEN = 12;

    if (!m_agent.m_inThinkBlock) {
        m_agent.m_thinkBuffer += token;

        if (m_agent.m_thinkBuffer.endsWith("<think>")) {
            QString before = m_agent.m_thinkBuffer;
            before.chop(7);
            if (!before.isEmpty()) emit m_agent.appendChatToken(before);
            m_agent.m_inThinkBlock = true;
            m_agent.m_thinkBuffer.clear();
        } else if (m_agent.m_thinkBuffer.length() > MAX_TAG_LEN ||
                   !QString("<think>").startsWith(
                       m_agent.m_thinkBuffer.right(MAX_TAG_LEN))) {
            emit m_agent.appendChatToken(m_agent.m_thinkBuffer);
            m_agent.m_thinkBuffer.clear();
        }
    } else {
        m_agent.m_thinkBuffer += token;

        if (m_agent.m_thinkBuffer.endsWith("</think>")) {
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
void AgentChat::summarizeContext()
{
    if (m_agent.m_summarizing) return;
    m_agent.m_summarizing = true;

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
        "Behalte alle wichtigen Fakten, Dateipfade, Fehler und offene Aufgaben. "
        "Schreibe nur die Zusammenfassung.\n\n"
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
