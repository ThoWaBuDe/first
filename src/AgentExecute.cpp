#include "AgentExecute.h"
#include "Agent.h"
#include "AgentUtils.h"
#include "AppConfig.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDir>

// ─── startExecute ─────────────────────────────────────────────────────────────
void AgentExecute::startExecute()
{
    m_agent.m_mode = AgentMode::Execute;
    emit m_agent.modeChanged(m_agent.m_mode);

    const AppConfig &cfg = AppConfig::instance();
    m_agent.m_executeMemory.setDbPath(cfg.taskDbPath());
    m_agent.m_executeMemory.setMaxEntries(cfg.executeMemoryMaxEntries());
    m_agent.m_executeMemory.load();

    emit m_agent.appendTools(
        QString("<b>Execute-Modus gestartet.</b> Thoughts: %1 Einträge.")
        .arg(m_agent.m_executeMemory.count()), "system");

    if (!advanceExecute()) {
        emit m_agent.appendChat(
            "<b>[System]</b> Alle Nodes bereits erledigt.", "system");
        m_agent.m_mode = AgentMode::Chat;
        emit m_agent.modeChanged(m_agent.m_mode);
        m_agent.m_chatModel.setSystemPrompt(m_agent.buildFullSystemPrompt());
        emit m_agent.inputEnabled(true);
        emit m_agent.statusChanged("Bereit");
        emit m_agent.executeFinished();
    }
}

// ─── advanceExecute ───────────────────────────────────────────────────────────
// FIX A: startGeneration() wird korrekt aufgerufen (kein toter Code).
// NEU: Erkennt Validierungs-Nodes und baut passenden Prompt.
bool AgentExecute::advanceExecute()
{
    TaskNode *node = m_agent.m_taskTree.nextPending();

    if (!node) {
        emit m_agent.appendTools(
            "<b>Execute: alle Nodes abgearbeitet!</b>", "system");
        emit m_agent.executeFinished();
        m_agent.m_executeMemory.save();

        if (!AppConfig::instance().executeSandboxProject().isEmpty())
            m_agent.m_assembler.assemble(
                QDir::homePath() + "/llamatools/" +
                AppConfig::instance().executeSandboxProject(),
                m_agent.m_taskTree,
                AppConfig::instance().assembleOnlyDone());

        m_agent.m_mode = AgentMode::Chat;
        emit m_agent.modeChanged(m_agent.m_mode);
        m_agent.m_chatModel.setSystemPrompt(m_agent.buildFullSystemPrompt());
        emit m_agent.inputEnabled(true);
        emit m_agent.statusChanged("Execute abgeschlossen");
        return false;
    }

    m_agent.m_currentNode = node;
    m_agent.m_taskTree.setStatus(node, TaskStatus::Running);
    emit m_agent.taskTreeUpdated();
    emit m_agent.executeNodeStarted(node->id);

    // ── Prompt aufbauen ───────────────────────────────────────────────────
    QString prompt;
    QString systemPrompt;

    if (node->isValidationNode()) {
        // H4-Validierungs-Node: eigener Prompt und System-Prompt
        prompt       = buildValidationPrompt(node);
        systemPrompt = QString(
            "Du bist ein Code-Review-Agent.\n"
            "Prüfe die Implementierung und antworte mit GENAU EINER ZEILE:\n"
            "  'ok' — wenn alles korrekt ist\n"
            "  '<kurze Fehlerbeschreibung>' — wenn ein Problem gefunden wurde\n"
            "Kein anderer Text. Kein Markdown. Nur eine Zeile.");
        emit m_agent.appendChat(
            QString("<b>Validierung:</b> %1")
            .arg(node->title.toHtmlEscaped()), "system");
    } else {
        // Normaler Impl-Node
        prompt       = buildExecutePrompt(node);
        systemPrompt = buildExecuteSystemPrompt();
        emit m_agent.appendChat(
            QString("<b>Execute Node:</b> %1 [%2]")
            .arg(node->title.toHtmlEscaped(),
                 TaskNode::levelName(node->level).toHtmlEscaped()),
            "system");
    }

    m_agent.m_taskTree.setBuildPrompt(node, prompt);

    m_agent.m_chatModel.clear();
    m_agent.m_chatModel.setSystemPrompt(systemPrompt);
    m_agent.m_chatModel.addUserMessage(prompt);

    m_agent.m_currentResponse.clear();
    m_agent.m_thinkBuffer.clear();
    m_agent.m_inThinkBlock      = false;
    m_agent.m_generatedTokens   = 0;
    m_agent.m_generating        = true;
    m_agent.m_continuationCount = 0;

    emit m_agent.appendChat("<b>Assistent (Execute):</b> ", "assistant");
    emit m_agent.statusChanged(
        node->isValidationNode()
        ? QString("Validiere: %1...").arg(node->parent
            ? node->parent->title : node->title)
        : QString("Execute: %1...").arg(node->title));

    // Validierungs-Nodes mit Chat-Sampler (kurze Antwort, kein Code)
    // Impl-Nodes mit Execute-Sampler
    m_agent.startGeneration(
        node->isValidationNode()
        ? LlamaWorker::SamplerProfile::Chat
        : LlamaWorker::SamplerProfile::Execute);

    return true;
}

// ─── isCurrentNodeValidation ─────────────────────────────────────────────────
bool AgentExecute::isCurrentNodeValidation() const
{
    return m_agent.m_currentNode &&
           m_agent.m_currentNode->isValidationNode();
}

// ─── buildValidationPrompt ────────────────────────────────────────────────────
// Zeigt dem Modell den Code seines H3-Elters + die Aufgabe.
// Das Modell soll NUR "ok" oder eine Fehlerbeschreibung ausgeben.
QString AgentExecute::buildValidationPrompt(const TaskNode *valNode) const
{
    const TaskNode *implNode = valNode->parent;
    if (!implNode)
        return "Fehler: Validierungs-Node hat keinen Elter.";

    QString prompt;

    // Kontext: was wurde implementiert?
    prompt += QString("## Zu prüfende Implementierung\n\n");
    prompt += QString("Titel: **%1**\n").arg(implNode->title);
    if (!implNode->symbol.isEmpty())
        prompt += QString("Symbol: `%1`\n").arg(implNode->symbol);
    prompt += "\n";

    // Beschreibung / Aufgabe
    if (!implNode->description.isEmpty()) {
        prompt += "### Aufgabe war:\n";
        prompt += implNode->description + "\n\n";
    }

    // Der tatsächlich generierte Code
    if (!implNode->result.isEmpty()) {
        prompt += "### Generierter Code:\n";
        prompt += "```\n" + implNode->result + "\n```\n\n";
    } else {
        prompt += "⚠ Kein Code vorhanden — Node hat kein result.\n\n";
    }

    // Interface aus dependsOn (zum Vergleich)
    if (!implNode->dependsOn.isEmpty()) {
        prompt += "### Interface (dependsOn):\n";
        for (qint64 depId : implNode->dependsOn) {
            const TaskNode *dep = m_agent.m_taskTree.findById(depId);
            if (!dep || dep->result.isEmpty()) continue;
            prompt += QString("**%1:**\n").arg(dep->title);
            prompt += "```\n" + dep->result + "\n```\n\n";
        }
    }

    prompt += "---\n\n";
    prompt += "## Deine Aufgabe\n\n";
    prompt += "Prüfe den Code auf:\n";
    prompt += "- Korrekte Signatur (passt zum Interface)\n";
    prompt += "- Vollständigkeit (keine fehlenden return-Statements)\n";
    prompt += "- Offensichtliche Logikfehler\n";
    prompt += "- Undefinierte Variablen oder Symbole\n\n";
    prompt += "Antworte mit genau einer Zeile:\n";
    prompt += "  **'ok'** — wenn alles korrekt ist\n";
    prompt += "  **'<Fehlerbeschreibung>'** — wenn ein Problem gefunden wurde\n\n";
    prompt += "Nur eine Zeile. Kein anderer Text.";

    return prompt;
}

// ─── handleValidationResult ───────────────────────────────────────────────────
// Verarbeitet die Antwort des Modells auf einen Validierungs-Node.
//
// "ok" → Node als Done markieren, weiter
// sonst → Fehlerbeschreibung speichern, H3-Elter als Failed markieren
//
// Warum H3 als Failed und nicht nur H4?
//   Der H3-Node ist der eigentliche Impl-Node. Wenn seine Validierung
//   fehlschlägt, ist er nicht wirklich Done. Failed-Status → Optimizer
//   kann ihn neu bearbeiten (Punkt J folgt).
void AgentExecute::handleValidationResult(const QString &fullResponse,
                                           uint32_t sessionId)
{
    Q_UNUSED(sessionId)
    if (!m_agent.m_currentNode || !m_agent.m_currentNode->isValidationNode())
        return;

    TaskNode *valNode  = m_agent.m_currentNode;
    TaskNode *implNode = valNode->parent;

    // Response bereinigen
    QString answer = fullResponse.trimmed();

    // <think>-Blöcke entfernen
    static const QRegularExpression thinkRe(
        "<think>.*?</think>",
        QRegularExpression::DotMatchesEverythingOption);
    answer.remove(thinkRe);
    answer = answer.trimmed();

    // Nur erste Zeile nehmen (Modell schreibt manchmal mehr)
    int newline = answer.indexOf('\n');
    if (newline > 0) answer = answer.left(newline).trimmed();

    bool isOk = answer.toLower() == "ok" ||
                answer.toLower().startsWith("ok ");

    // Validierungsergebnis speichern
    m_agent.m_taskTree.setValidationResult(valNode, answer);
    m_agent.m_taskTree.setStatus(valNode,
        isOk ? TaskStatus::Done : TaskStatus::Failed);

    if (isOk) {
        emit m_agent.appendTools(
            QString("<b>Validierung ✓:</b> %1")
            .arg(implNode ? implNode->title.toHtmlEscaped() : "?"),
            "system");
    } else {
        emit m_agent.appendTools(
            QString("<b>Validierung ✗:</b> %1<br>"
                    "<small>%2</small>")
            .arg(implNode ? implNode->title.toHtmlEscaped() : "?")
            .arg(answer.toHtmlEscaped()),
            "error");

        // H3-Elter als Failed markieren
        if (implNode) {
            m_agent.m_taskTree.setStatus(implNode, TaskStatus::Failed);
            emit m_agent.appendTools(
                QString("<b>Node als Failed markiert:</b> %1<br>"
                        "<small>Validierungsfehler: %2</small>")
                .arg(implNode->title.toHtmlEscaped())
                .arg(answer.toHtmlEscaped()),
                "error");
        }
    }

    m_agent.m_taskTree.save();
    emit m_agent.taskTreeUpdated();

    // Weiter mit nächstem Node
    updateThoughts(valNode, m_agent.m_sessionId);
}

// ─── handleExecuteCode ────────────────────────────────────────────────────────
// Extrahiert Code aus <code>-Tags.
// NEU: Nach erfolgreichem H3-Code → Validierungs-Node anlegen (Punkt N).
void AgentExecute::handleExecuteCode(const QString &fullResponse,
                                      uint32_t sessionId)
{
    Q_UNUSED(sessionId)
    if (!m_agent.m_currentNode) return;

    // Validierungs-Nodes haben keinen <code>-Output → handleValidationResult
    if (m_agent.m_currentNode->isValidationNode()) {
        handleValidationResult(fullResponse, sessionId);
        return;
    }

    QString code;
    QString sideOut;

    int codeStart = fullResponse.indexOf("<code>");
    int codeEnd   = fullResponse.indexOf("</code>");

    if (codeStart >= 0 && codeEnd > codeStart) {
        sideOut = fullResponse.left(codeStart).trimmed();
        code    = fullResponse.mid(codeStart + 6,
                                   codeEnd - codeStart - 6).trimmed();
        QString after = fullResponse.mid(codeEnd + 7).trimmed();
        if (!after.isEmpty()) {
            if (!sideOut.isEmpty()) sideOut += "\n\n";
            sideOut += after;
        }
    } else {
        code = fullResponse.trimmed();
        if (code.startsWith("```")) {
            int firstNewline = code.indexOf('\n');
            if (firstNewline > 0) code = code.mid(firstNewline + 1);
            if (code.endsWith("```")) code.chop(3);
            code = code.trimmed();
        }
        sideOut = "[Hinweis: Keine <code>-Tags. Gesamter Output als Code.]";
        emit m_agent.appendTools(
            "<b>Execute:</b> Keine &lt;code&gt;-Tags — Fallback.", "system");
    }

    // <think>-Blöcke aus sideOutput
    static const QRegularExpression thinkRe(
        "<think>.*?</think>",
        QRegularExpression::DotMatchesEverythingOption);
    sideOut.remove(thinkRe);
    sideOut = sideOut.trimmed();

    m_agent.m_taskTree.setResult(m_agent.m_currentNode, code);
    m_agent.m_taskTree.setSideOutput(m_agent.m_currentNode, sideOut);
    m_agent.m_taskTree.setStatus(m_agent.m_currentNode, TaskStatus::Done);

    // ── NEU (Punkt N): Validierungs-Node anlegen für H3-Nodes ─────────────
    // Nur für H3 (Impl-Level) — nicht für H2-Blätter oder CMakeLists etc.
    bool isImplNode = (m_agent.m_currentNode->level >=
                       static_cast<int>(TaskLevel::Impl));
    bool isCodeFile = (m_agent.m_currentNode->fileType() ==
                       TaskNode::FileType::Cpp ||
                       m_agent.m_currentNode->fileType() ==
                       TaskNode::FileType::Unknown);

    if (isImplNode && isCodeFile && !code.isEmpty()) {
        TaskNode *valNode = m_agent.m_taskTree.createValidationNode(
            m_agent.m_currentNode);
        if (valNode) {
            emit m_agent.appendTools(
                QString("<b>Validierungs-Node erstellt:</b> %1")
                .arg(valNode->title.toHtmlEscaped()), "system");
        }
    }

    m_agent.m_taskTree.save();

    emit m_agent.appendTools(
        QString("<b>Node fertig:</b> %1<br>"
                "<small>%2 Zeichen Code | %3 Zeichen sideOutput</small>")
        .arg(m_agent.m_currentNode->title.toHtmlEscaped())
        .arg(code.length())
        .arg(sideOut.length()),
        "system");

    emit m_agent.executeNodeDone(m_agent.m_currentNode->id);
    emit m_agent.taskTreeUpdated();

    updateThoughts(m_agent.m_currentNode, m_agent.m_sessionId);
}

// ─── buildExecuteSystemPrompt ─────────────────────────────────────────────────
QString AgentExecute::buildExecuteSystemPrompt() const
{
    QString lang = "Code";
    QString fileTypeHint;

    if (m_agent.m_currentNode) {
        const TaskNode *fileNode = m_agent.m_currentNode;
        if (fileNode->level >= static_cast<int>(TaskLevel::Impl) &&
            fileNode->parent &&
            fileNode->parent->level == static_cast<int>(TaskLevel::Class))
            fileNode = fileNode->parent;

        lang = fileNode->languageName();
        fileTypeHint = QString(
            "Du implementierst eine %1.\n"
            "Ausgabe: NUR %2-Code.\n\n"
        ).arg(fileNode->fileTypeDescription(), lang);
    }

    return QString(
        "Du bist ein Implementierungs-Agent fuer C++/Qt6 Projekte.\n"
        "\n"
        "%1"
        "AUSGABE-FORMAT:\n"
        "<code>\n"
        "...dein %2-Code hier...\n"
        "</code>\n"
        "\n"
        "Kein Text vor <code>, kein Text nach </code>.\n"
        "Kein Markdown. Nur der reine Code zwischen den Tags.\n"
        "\n"
        "WICHTIG: Du schreibst NUR das was in deiner Aufgabe steht.\n"
        "  - Bei einer Methode: NUR den Methodenkörper\n"
        "  - Keine vollständige Datei wenn du nur eine Methode schreibst\n"
        "\n"
        "LESE-TOOLS: read_file, get_function_body, get_class_members,\n"
        "  get_includes, list_dir, get_symbol, search_code, grep_code,\n"
        "  read_multiple_files, web_search, get_time, sys_info\n"
        "\n"
        "VERBOTEN: write_file, str_replace, append_file, cmake_build\n"
        "\n"
        "Antworte auf Deutsch wenn du Text ausgibst.\n"
        "Fehler-Text AUSSERHALB der <code>-Tags schreiben.\n"
    ).arg(fileTypeHint, lang);
}

// ─── buildExecutePrompt ───────────────────────────────────────────────────────
// FIX B: Expliziter (aktuell)-Marker.
QString AgentExecute::buildExecutePrompt(const TaskNode *node) const
{
    QString prompt;

    if (!m_agent.m_executeMemory.isEmpty()) {
        prompt += "## Bisherige Erkenntnisse (Thoughts)\n\n";
        prompt += m_agent.m_executeMemory.toPromptString();
        prompt += "\n\n";
    }

    // Vertikaler Kontext mit explizitem Marker (FIX B)
    prompt += "## Aufgaben-Kontext\n\n";
    for (const TaskNode *n : node->pathFromRoot()) {
        bool isCurrent = (n->id == node->id);
        prompt += QString("%1%2: %3\n")
                  .arg(TaskNode::levelName(n->level))
                  .arg(isCurrent ? " **(aktuell)**" : "")
                  .arg(n->title);
    }
    if (!node->symbol.isEmpty())
        prompt += QString("Symbol: `%1`\n").arg(node->symbol);
    if (!node->description.isEmpty())
        prompt += "---\n" + node->description + "\n";
    prompt += "\n";

    // Horizontaler Kontext
    if (!node->dependsOn.isEmpty()) {
        prompt += "\n## Abhängigkeiten\n\n";
        for (qint64 depId : node->dependsOn) {
            const TaskNode *dep = m_agent.m_taskTree.findById(depId);
            if (!dep) continue;
            prompt += QString("### %1 [%2]\n")
                      .arg(dep->title, TaskNode::levelName(dep->level));
            if (!dep->symbol.isEmpty())
                prompt += QString("Symbol: `%1`\n").arg(dep->symbol);
            if (!dep->description.isEmpty())
                prompt += dep->description + "\n";
            if (!dep->result.isEmpty())
                prompt += "\n```\n" + dep->result + "\n```\n";
            prompt += "\n";
        }
    }

    // Geschwister (verhindert Duplikate)
    if (node->parent && !node->parent->children.empty()) {
        bool hasFinished = false;
        QString sibCtx;
        for (const TaskNode *sib : node->parent->children) {
            if (sib->id == node->id) break;
            if (sib->status != TaskStatus::Done || sib->result.isEmpty()) continue;
            if (!hasFinished) {
                sibCtx += "\n## Bereits implementierte Geschwister\n\n";
                sibCtx += "(Nicht wiederholen — baue darauf auf)\n\n";
                hasFinished = true;
            }
            sibCtx += QString("### %1 (fertig)\n").arg(sib->title);
            if (!sib->symbol.isEmpty())
                sibCtx += QString("Symbol: `%1`\n").arg(sib->symbol);
            sibCtx += "```\n" + sib->result + "\n```\n\n";
        }
        if (hasFinished) prompt += sibCtx;
    }

    prompt += "## Deine Aufgabe\n\n";
    prompt += QString("Implementiere: **%1**\n\n").arg(node->title);
    if (!node->description.isEmpty())
        prompt += node->description + "\n\n";

    if (node->level == static_cast<int>(TaskLevel::Class)) {
        prompt += node->children.empty()
            ? "Dieser Node ist ein Blatt — produziere die **vollständige Datei**.\n\n"
            : "Container-Node — produziere nur den **Datei-Kopf**.\n\n";
    }

    if (node->level >= static_cast<int>(TaskLevel::Impl)) {
        prompt += "**WICHTIG:** NUR diese eine Methode. "
                  "Keine vollständige Datei, keine anderen Methoden.\n\n";
    }

    prompt += "Gib den Code zwischen <code>...</code> aus.";
    return prompt;
}

// ─── handleExecuteToolCall ────────────────────────────────────────────────────
void AgentExecute::handleExecuteToolCall(const QString &fullResponse,
                                          uint32_t sessionId)
{
    int start     = fullResponse.indexOf("<tool_call>") + 11;
    int end       = fullResponse.indexOf("</tool_call>", start);
    QString block = fullResponse.mid(start, end - start).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);

    if (doc.isNull()) {
        QString repaired = AgentUtils::repairJson(block);
        if (!repaired.isEmpty())
            doc = QJsonDocument::fromJson(repaired.toUtf8());
        else {
            m_agent.m_chatModel.addToolResult("json_error",
                QString("[SYSTEM: Ungültiges JSON. Fehler: %1]")
                .arg(pe.errorString()));
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.m_currentResponse.clear();
            m_agent.m_thinkBuffer.clear();
            m_agent.m_inThinkBlock = false;
            emit m_agent.appendChat(
                "<b>Assistent (Execute):</b> ", "assistant");
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Execute);
            return;
        }
    }

    QString     toolName = doc.object().value("name").toString();
    QJsonObject toolArgs = doc.object().value("arguments").toObject();

    emit m_agent.appendTools(
        QString("<b>Execute-Tool: %1</b><br><pre>%2</pre>")
        .arg(toolName.toHtmlEscaped(),
             QString::fromUtf8(QJsonDocument(toolArgs)
                               .toJson(QJsonDocument::Indented)).toHtmlEscaped()),
        "tool");

    if (!Agent::EXECUTE_ALLOWED_TOOLS.contains(toolName)) {
        m_agent.m_chatModel.addToolResult(toolName,
            QString("[SYSTEM: Tool '%1' im Execute-Modus VERBOTEN.]")
            .arg(toolName));
        m_agent.m_generating      = true;
        m_agent.m_generatedTokens = 0;
        m_agent.m_currentResponse.clear();
        m_agent.m_thinkBuffer.clear();
        m_agent.m_inThinkBlock = false;
        emit m_agent.appendChat("<b>Assistent (Execute):</b> ", "assistant");
        m_agent.startGeneration(LlamaWorker::SamplerProfile::Execute);
        return;
    }

    if (!m_agent.m_mcp.containsTool(toolName)) {
        m_agent.m_chatModel.addToolResult(toolName,
            QString("Tool '%1' nicht verfügbar.").arg(toolName));
        m_agent.m_generating      = true;
        m_agent.m_generatedTokens = 0;
        m_agent.startGeneration(LlamaWorker::SamplerProfile::Execute);
        return;
    }

    emit m_agent.statusChanged(
        QString("Execute-Tool: %1...").arg(toolName));
    QString tKey = AgentUtils::toolCallKey(toolName, toolArgs);

    m_agent.m_mcp.callTool(toolName, toolArgs,
        [this, toolName, tKey, sessionId](QString result, QString error) {
            if (sessionId != m_agent.m_sessionId) return;

            QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);
            bool    isErr      = !error.isEmpty();

            if (!isErr &&
                toolResult.length() > AppConfig::instance().maxToolResultChars()) {
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
                        QString("<b>Execute: DEADLOCK ABBRUCH</b> '%1'")
                        .arg(toolName.toHtmlEscaped()), "error");
                    m_agent.m_toolFailCount.remove(tKey);
                    if (m_agent.m_currentNode) {
                        m_agent.m_taskTree.setStatus(
                            m_agent.m_currentNode, TaskStatus::Failed);
                        m_agent.m_taskTree.save();
                        emit m_agent.taskTreeUpdated();
                    }
                    advanceExecute();
                    return;
                }
                toolResult += "\n\n" +
                    AgentUtils::deadlockEscalationPrompt(toolName, failCount);
            } else {
                m_agent.m_toolFailCount.remove(tKey);
            }

            emit m_agent.appendTools(
                QString("<b>Execute-Ergebnis [%1]:</b><br><pre>%2</pre>")
                .arg(toolName.toHtmlEscaped(),
                     toolResult.left(800).toHtmlEscaped() +
                     (toolResult.length() > 800 ? "\n..." : "")),
                isErr ? "error" : "tool");

            m_agent.m_chatModel.addToolResult(toolName, toolResult);
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.m_currentResponse.clear();
            m_agent.m_thinkBuffer.clear();
            m_agent.m_inThinkBlock = false;
            emit m_agent.appendChat(
                "<b>Assistent (Execute):</b> ", "assistant");
            emit m_agent.statusChanged("Execute läuft...");
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Execute);
        });
}

// ─── updateThoughts ───────────────────────────────────────────────────────────
// FIX C: Nur parseFromLlmOutput() — kein doppelter Block.
void AgentExecute::updateThoughts(const TaskNode *node, uint32_t sessionId)
{
    Q_UNUSED(sessionId)
    m_agent.m_updatingThoughts = true;

    QString summarizePrompt = QString(
        "Du hast gerade folgenden Node bearbeitet:\n"
        "Titel: %1\n"
        "%2"
        "\n"
        "Bisherige Thoughts:\n%3\n\n"
        "Aufgabe: Aktualisiere die Thoughts-Liste.\n"
        "- Behalte wichtige Erkenntnisse\n"
        "- Füge neue Erkenntnisse hinzu\n"
        "- Entferne überholte Einträge\n"
        "- Maximal %4 Einträge\n"
        "- Eine Erkenntnis pro Zeile\n"
        "- KEINE Nummerierung, KEIN Markdown, NUR die Liste"
    ).arg(
        node->title,
        node->symbol.isEmpty() ? ""
            : QString("Symbol: %1\n").arg(node->symbol),
        m_agent.m_executeMemory.toPromptString(),
        QString::number(AppConfig::instance().executeMemoryMaxEntries())
    );

    ChatModel summarizeModel;
    summarizeModel.setChatTemplate(m_agent.m_chatModel.chatTemplate());
    summarizeModel.setSystemPrompt(
        "Komprimiere Kurzzeitgedächtnis-Listen. "
        "NUR die Liste. Eine Erkenntnis pro Zeile. "
        "Keine Nummerierung. Kein Markdown.");
    summarizeModel.addUserMessage(summarizePrompt);

    m_agent.m_currentResponse.clear();
    m_agent.m_thinkBuffer.clear();
    m_agent.m_inThinkBlock    = false;
    m_agent.m_generatedTokens = 0;
    m_agent.m_generating      = true;

    emit m_agent.statusChanged("Thoughts werden aktualisiert...");
    m_agent.m_chatModel = std::move(summarizeModel);
    m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
}

// ─── isExecuteToolCall ────────────────────────────────────────────────────────
bool AgentExecute::isExecuteToolCall(const QString &response) const
{
    return response.contains("<tool_call>") &&
           response.contains("</tool_call>");
}
