#include "Agent/AgentImport.h"
#include "Agent/Agent.h"
#include "Agent/AgentUtils.h"
#include "Config/AppConfig.h"
#include "Chat/ToolCallFormat.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 1 — Modell plant
// ═════════════════════════════════════════════════════════════════════════════

// ─── startImport ─────────────────────────────────────────────────────────────
// Bereitet alles vor und startet Phase 1.
// Das Modell bekommt einen kurzen, fokussierten Prompt:
//   "list_dir → create_node H2 pro Datei → plan_done"
// Kein read_file, kein list_symbols — das kommt in Phase 2.
void AgentImport::startImport(const QString &path)
{
    m_importPath   = path;
    m_importActive = true;
    m_phase1Done   = false;

    emit m_agent.appendTools(
        QString("<b>Import Phase 1:</b> Modell erstellt Dateiplan für <code>%1</code>")
        .arg(path.toHtmlEscaped()), "system");
    emit m_agent.statusChanged("Import: Planung läuft...");

    // TaskTree leeren — frischer Start
    m_agent.m_taskTree.clear();

    // H0-Root anlegen
    m_agent.m_taskTree.createNode(
        QString("Import: %1").arg(path), "",
        static_cast<int>(TaskLevel::Goal),
        TaskScope::External, 0, nullptr);

    m_agent.m_chatModel.clear();
    m_agent.m_chatModel.setSystemPrompt(buildPhase1SystemPrompt(path));

    // Erzwungener erster Tool-Call: list_dir
    // Verhindert dass das Modell mit Prosa-Erklärungen beginnt.
    m_agent.m_chatModel.addUserMessage(
        QString("Erstelle den Import-Plan für '%1'.\n\n"
                "STARTE SOFORT mit diesem Tool-Call — keine Erklärung vorher:\n"
                "<tool_call>\n"
                "{\"name\": \"list_dir\", \"arguments\": {\"path\": \"%1\"}}\n"
                "</tool_call>")
        .arg(path));

    m_agent.m_currentResponse.clear();
    m_agent.m_thinkBuffer.clear();
    m_agent.m_inThinkBlock    = false;
    m_agent.m_generatedTokens = 0;
    m_agent.m_generating      = true;

    emit m_agent.appendChat("<b>Assistent (Import-Plan):</b> ", "assistant");
    emit m_agent.inputEnabled(false);
    m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
}

// ─── buildPhase1SystemPrompt ──────────────────────────────────────────────────
// Fokussierter Prompt für Phase 1.
// Das Modell soll NUR planen — kein Code lesen, nur Dateiliste + Strategie.
//
// Warum so strikt?
//   In Phase 1 ist der Context-Verbrauch kritisch.
//   Jedes read_file kostet 200-500 Token — das killt den Context.
//   Phase 2 übernimmt das Lesen, mit Context-Reset pro Datei.
QString AgentImport::buildPhase1SystemPrompt(const QString &path) const
{
    return QString(
        "Du bist ein Import-Planungs-Agent.\n"
        "\n"
        "AUFGABE: Erstelle einen Import-Plan für alle Dateien unter '%1'.\n"
        "\n"
        "ABLAUF:\n"
        "1. list_dir (einmal) → Dateiliste anschauen\n"
        "2. Für JEDE relevante Datei: create_node mit import_strategy\n"
        "3. plan_done\n"
        "\n"
        "WICHTIG: Lies KEINE Dateien! Kein read_file, kein list_symbols!\n"
        "Das passiert später automatisch pro Datei.\n"
        "\n"
        "── VERFÜGBARE TOOLS ─────────────────────────────────────────────────\n"
        "\n"
        "list_dir — Verzeichnis auflisten\n"
        "  <tool_call>{\"name\": \"list_dir\", \"arguments\": {\"path\": \"%1\"}}</tool_call>\n"
        "\n"
        "create_node — Datei in den Plan aufnehmen\n"
        "  <tool_call>{\"name\": \"create_node\", \"arguments\": {\n"
        "    \"title\":           \"Agent.cpp\",\n"
        "    \"level\":           2,\n"
        "    \"import_strategy\": \"symbols\",\n"
        "    \"symbol\":          \"Agent\"\n"
        "  }}</tool_call>\n"
        "\n"
        "  title:           Dateiname (nur der Dateiname, kein Pfad)\n"
        "  level:           immer 2\n"
        "  import_strategy: eine der folgenden Strategien (siehe unten)\n"
        "  symbol:          C++-Klassenname falls bekannt, sonst leer lassen\n"
        "\n"
        "plan_done — Plan abschließen\n"
        "  <tool_call>{\"name\": \"plan_done\", \"arguments\": {\n"
        "    \"summary\": \"Kurze Zusammenfassung\"\n"
        "  }}</tool_call>\n"
        "\n"
        "── IMPORT-STRATEGIEN ────────────────────────────────────────────────\n"
        "\n"
        "  \"header\"    — C++ Header (.h, .hpp)\n"
        "               Datei wird komplett gelesen, Inhalt wird gespeichert.\n"
        "               Keine Unteraufgaben (H3) nötig — alles in einer Datei.\n"
        "\n"
        "  \"symbols\"   — C++ Implementierung (.cpp, .cc, .cxx)\n"
        "               Symbole werden einzeln extrahiert (get_function_body).\n"
        "               Pro Methode/Funktion wird ein H3-Node angelegt.\n"
        "\n"
        "  \"read_file\" — Andere Textdateien (CMakeLists.txt, *.md, *.ui, *.py)\n"
        "               Datei wird komplett gelesen, Inhalt wird gespeichert.\n"
        "\n"
        "  \"skip\"      — Binärdateien, Build-Artefakte, .gitignore etc.\n"
        "               Datei wird nicht importiert.\n"
        "\n"
        "── ENTSCHEIDUNGSHILFE ───────────────────────────────────────────────\n"
        "\n"
        "  *.h / *.hpp           → \"header\"\n"
        "  *.cpp / *.cc / *.cxx  → \"symbols\"\n"
        "  CMakeLists.txt        → \"read_file\"\n"
        "  *.cmake               → \"read_file\"\n"
        "  *.md / *.txt          → \"read_file\"\n"
        "  *.ui                  → \"read_file\"\n"
        "  *.py                  → \"read_file\"\n"
        "  *.o / *.a / *.so      → \"skip\"\n"
        "  build/ Verzeichnisse  → \"skip\" (ignorieren)\n"
        "  .git / .gitignore     → \"skip\"\n"
        "\n"
        "── WICHTIGE REGELN ──────────────────────────────────────────────────\n"
        "\n"
        "- KEIN read_file in Phase 1!\n"
        "- Jede Datei bekommt genau einen create_node Aufruf\n"
        "- level ist immer 2 (H2)\n"
        "- parent_id NICHT setzen — LlamaQt gruppiert automatisch\n"
        "- Nach plan_done bitte keine weiteren Tool-Calls\n"
        "\n"
        "Antworte auf Deutsch."
    ).arg(path, path);
}

// ─── handleImportToolCall (Phase 1) ──────────────────────────────────────────
// Verarbeitet Tool-Calls in Phase 1.
// Interne Tools (create_node, plan_done) werden direkt behandelt.
// list_dir wird an McpManager weitergegeben.
void AgentImport::handleImportToolCall(const QString &fullResponse,
                                        uint32_t sessionId)
{
    ToolCallFormat::Preset fmt = m_agent.m_activeToolFormat;
    ParsedToolCall call = ToolCallParser::parseFirst(fullResponse, fmt);

    if (!call.valid) {
        // Repair-Versuch für XML-Format
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
                        call = ParsedToolCall::ok(
                            doc.object().value("name").toString(),
                            doc.object().value("arguments").toObject());
                    }
                }
            }
        }
        if (!call.valid) {
            m_agent.m_chatModel.addToolResult("json_error",
                QString("[SYSTEM: Ungültiger Tool-Call. Fehler: %1]").arg(call.error));
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.m_currentResponse.clear();
            m_agent.m_thinkBuffer.clear();
            m_agent.m_inThinkBlock = false;
            emit m_agent.appendChat("<b>Assistent (Import-Plan):</b> ", "assistant");
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
            return;
        }
    }

    const QString     &toolName = call.name;
    const QJsonObject &toolArgs = call.arguments;

    emit m_agent.appendTools(
        QString("<b>Import-Plan-Tool: %1</b><br><pre>%2</pre>")
        .arg(toolName.toHtmlEscaped(),
             QString::fromUtf8(QJsonDocument(toolArgs)
                               .toJson(QJsonDocument::Indented)).toHtmlEscaped()),
        "tool");

    // ── Interne Plan-Tools ────────────────────────────────────────────────
    if (toolName == "create_node") {
        QString result = handleCreateNode(toolArgs);
        emit m_agent.appendTools(
            QString("<b>Plan-Tool Ergebnis:</b><br><pre>%1</pre>")
            .arg(result.toHtmlEscaped()), "tool");
        emit m_agent.taskTreeUpdated();
        sendToolResult(toolName, result, sessionId);
        return;
    }

    if (toolName == "plan_done") {
        handlePlanDone(toolArgs, sessionId);
        return;
    }

    // ── list_dir → McpManager ─────────────────────────────────────────────
    if (toolName == "list_dir") {
        if (!m_agent.m_mcp.containsTool(toolName)) {
            sendToolResult(toolName,
                QString("Tool '%1' nicht verfügbar.").arg(toolName), sessionId);
            return;
        }

        emit m_agent.statusChanged("Import: Verzeichnis wird gelesen...");
        m_agent.m_mcp.callTool(toolName, toolArgs,
            [this, toolName, sessionId](QString result, QString error) {
                if (sessionId != m_agent.m_sessionId) return;
                QString toolResult = error.isEmpty() ? result : ("Fehler: " + error);

                emit m_agent.appendTools(
                    QString("<b>Import-Ergebnis [%1]:</b><br><pre>%2</pre>")
                    .arg(toolName.toHtmlEscaped(), toolResult.left(800).toHtmlEscaped()),
                    error.isEmpty() ? "tool" : "error");

                sendToolResult(toolName, toolResult, sessionId);
            });
        return;
    }

    // ── Unbekanntes Tool → Fehler ────────────────────────────────────────
    QString errMsg = QString(
        "[SYSTEM: Tool '%1' ist in Phase 1 VERBOTEN. "
        "Nur list_dir, create_node und plan_done sind erlaubt.]")
        .arg(toolName);
    emit m_agent.appendTools(
        QString("Import Phase 1: <b>%1</b> verboten.")
        .arg(toolName.toHtmlEscaped()), "error");
    sendToolResult(toolName, errMsg, sessionId);
}

// ─── handleCreateNode (Phase 1) ───────────────────────────────────────────────
// Legt einen H2-Node für eine Datei an.
// Liest import_strategy als String und konvertiert zu ImportStrategy.
QString AgentImport::handleCreateNode(const QJsonObject &args)
{
    QString title          = args.value("title").toString().trimmed();
    QString strategyStr    = args.value("import_strategy").toString("read_file");
    QString symbol         = args.value("symbol").toString().trimmed();
    int     level          = args.value("level").toInt(2);
    int     order          = args.value("order").toInt(0);

    if (title.isEmpty())
        return R"({"error": "title ist required"})";

    // Level-Schutz: in Phase 1 nur H2 erlaubt
    if (level != 2) {
        level = 2;
    }

    ImportStrategy strategy = TaskNode::importStrategyFromString(strategyStr);

    // H0-Root als Parent
    TaskNode *h0 = nullptr;
    if (!m_agent.m_taskTree.isEmpty())
        h0 = m_agent.m_taskTree.roots().front();

    // Symbol-Duplikat-Prüfung
    if (!symbol.isEmpty() && m_agent.m_taskTree.symbolExists(symbol)) {
        TaskNode *existing = m_agent.m_taskTree.findBySymbol(symbol);
        return QString(
            "{\"warning\": \"Symbol '%1' existiert bereits in Node %2 ('%3'). "
            "Übersprungen.\", \"existing_id\": %2}")
            .arg(symbol)
            .arg(existing ? existing->id : -1)
            .arg(existing ? existing->title : "?");
    }

    TaskNode *node = m_agent.m_taskTree.createNode(
        title, "", level,
        TaskScope::Internal, order, h0,
        symbol, strategy);

    return QString(
        "{\"id\": %1, \"title\": \"%2\", \"import_strategy\": \"%3\"%4}")
        .arg(node->id)
        .arg(node->title)
        .arg(strategyStr)
        .arg(symbol.isEmpty() ? ""
             : QString(", \"symbol\": \"%1\"").arg(symbol));
}

// ─── handlePlanDone (Phase 1) ─────────────────────────────────────────────────
// Phase 1 ist abgeschlossen.
// LlamaQt übernimmt: gruppieren → Phase 2 starten.
QString AgentImport::handlePlanDone(const QJsonObject &args, uint32_t sessionId)
{
    Q_UNUSED(sessionId)
    QString summary = args.value("summary").toString("Import-Plan erstellt.");

    int nodeCount = m_agent.m_taskTree.nodeCount();
    if (nodeCount <= 1) { // nur H0-Root
        emit m_agent.appendTools(
            "<b>Import Phase 1:</b> Keine Dateien geplant — "
            "Verzeichnis leer oder nur Skip-Dateien?", "error");
        m_importActive = false;
        m_agent.m_mode = AgentMode::Chat;
        emit m_agent.modeChanged(m_agent.m_mode);
        m_agent.m_chatModel.setSystemPrompt(m_agent.buildFullSystemPrompt());
        emit m_agent.inputEnabled(true);
        emit m_agent.statusChanged("Import fehlgeschlagen");
        return R"({"error": "Kein Datei-Plan erstellt."})";
    }

    emit m_agent.appendTools(
        QString("<b>Import Phase 1 abgeschlossen:</b> %1 Dateien geplant.<br>"
                "<small>%2</small>")
        .arg(nodeCount - 1) // H0 nicht mitzählen
        .arg(summary.toHtmlEscaped()), "system");

    // Gruppierung durch LlamaQt
    TaskNode *h0 = m_agent.m_taskTree.roots().front();
    m_agent.m_taskTree.groupByBasename(h0);
    emit m_agent.taskTreeUpdated();

    emit m_agent.appendTools(
        QString("<b>Import: Gruppierung abgeschlossen.</b> "
                "Starte Phase 2 — Datei für Datei analysieren..."), "system");

    m_phase1Done = true;

    // Phase 2 starten
    if (!advanceImport()) {
        emit m_agent.appendChat(
            "<b>[Import]</b> Alle Dateien abgearbeitet.", "system");
        m_importActive = false;
        m_agent.m_mode = AgentMode::Chat;
        emit m_agent.modeChanged(m_agent.m_mode);
        m_agent.m_chatModel.setSystemPrompt(m_agent.buildFullSystemPrompt());
        emit m_agent.inputEnabled(true);
        emit m_agent.statusChanged("Import abgeschlossen");
    }

    return QString("{\"ok\": true, \"files\": %1}").arg(nodeCount - 1);
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 2 — LlamaQt iteriert, Modell analysiert
// ═════════════════════════════════════════════════════════════════════════════

// ─── advanceImport ────────────────────────────────────────────────────────────
// Analog zu AgentExecute::advanceExecute().
// Sucht den nächsten Pending-H2-Node mit einer Import-Strategie und
// startet die Analyse dafür.
bool AgentImport::advanceImport()
{
    TaskNode *node = m_agent.m_taskTree.nextPendingImport();

    if (!node) {
        // Alle Dateien abgearbeitet
        emit m_agent.appendTools(
            "<b>Import Phase 2 abgeschlossen!</b> Alle Dateien importiert.",
            "system");
        m_agent.m_taskTree.save();
        emit m_agent.taskTreeUpdated();
        return false;
    }

    // Skip-Nodes sofort als Done markieren
    if (node->importStrategy == ImportStrategy::Skip) {
        m_agent.m_taskTree.setStatus(node, TaskStatus::Done);
        return advanceImport(); // nächster Node
    }

    m_agent.m_currentNode = node;
    m_agent.m_taskTree.setStatus(node, TaskStatus::Running);
    emit m_agent.taskTreeUpdated();
    emit m_agent.executeNodeStarted(node->id);

    // Context leeren — jede Datei bekommt einen frischen Context
    // Analogie AVR: DMA-Buffer vor neuem Transfer leeren
    m_agent.m_chatModel.clear();
    m_agent.m_chatModel.setSystemPrompt(buildPhase2SystemPrompt(node));

    QString prompt = buildPhase2Prompt(node);
    m_agent.m_chatModel.addUserMessage(prompt);
    m_agent.m_taskTree.setBuildPrompt(node, prompt);

    m_agent.m_currentResponse.clear();
    m_agent.m_thinkBuffer.clear();
    m_agent.m_inThinkBlock      = false;
    m_agent.m_generatedTokens   = 0;
    m_agent.m_generating        = true;
    m_agent.m_continuationCount = 0;

    emit m_agent.appendChat(
        QString("<b>Import [%1/%2]:</b> <code>%3</code> (%4)")
        .arg(m_agent.m_taskTree.nodeCount()) // vereinfacht
        .arg(m_agent.m_taskTree.nodeCount())
        .arg(node->title.toHtmlEscaped())
        .arg(TaskNode::importStrategyToString(node->importStrategy)),
        "system");
    emit m_agent.appendChat("<b>Assistent (Import):</b> ", "assistant");
    emit m_agent.statusChanged(
        QString("Import: %1...").arg(node->title));

    // Sampler: Tool für symbols, Chat für header/read_file
    auto profile = (node->importStrategy == ImportStrategy::Symbols)
                   ? LlamaWorker::SamplerProfile::Tool
                   : LlamaWorker::SamplerProfile::Chat;
    m_agent.startGeneration(profile);
    return true;
}

// ─── buildPhase2SystemPrompt ──────────────────────────────────────────────────
QString AgentImport::buildPhase2SystemPrompt(const TaskNode *node) const
{
    switch (node->importStrategy) {

    case ImportStrategy::Symbols:
        return QString(
            "Du bist ein Code-Analyse-Agent.\n"
            "\n"
            "AUFGABE: Analysiere die Datei '%1' und extrahiere alle Symbole.\n"
            "\n"
            "ABLAUF:\n"
            "1. list_symbols — alle Symbole anzeigen\n"
            "2. Für jede Methode/Funktion: get_function_body\n"
            "3. Für jede Methode: create_node H3 mit dem extrahierten Code\n"
            "4. plan_done\n"
            "\n"
            "ERLAUBTE TOOLS: list_symbols, get_function_body, create_node, plan_done\n"
            "VERBOTEN: read_file (außer wenn list_symbols fehlschlägt)\n"
            "\n"
            "create_node Format für H3:\n"
            "  <tool_call>{\"name\": \"create_node\", \"arguments\": {\n"
            "    \"title\":      \"ClassName::methodName()\",\n"
            "    \"level\":      3,\n"
            "    \"symbol\":     \"ClassName::methodName()\",\n"
            "    \"result\":     \"<vollständiger Methoden-Code>\",\n"
            "    \"parent_id\":  %2\n"
            "  }}</tool_call>\n"
            "\n"
            "WICHTIG:\n"
            "- result MUSS den vollständigen Code enthalten\n"
            "- title = Methoden-Name, NICHT Dateiname\n"
            "- parent_id = %2 (ID des H2-Nodes dieser Datei)\n"
            "\n"
            "Antworte auf Deutsch."
        ).arg(node->title).arg(node->id);

    case ImportStrategy::Header:
        return QString(
            "Du bist ein Code-Analyse-Agent.\n"
            "\n"
            "AUFGABE: Lies den Header '%1' und speichere den Inhalt.\n"
            "\n"
            "ABLAUF:\n"
            "1. read_file — Datei lesen\n"
            "2. set_result — Inhalt speichern\n"
            "3. plan_done\n"
            "\n"
            "ERLAUBTE TOOLS: read_file, set_result, plan_done\n"
            "\n"
            "set_result Format:\n"
            "  <tool_call>{\"name\": \"set_result\", \"arguments\": {\n"
            "    \"node_id\": %2,\n"
            "    \"result\":  \"<vollständiger Header-Inhalt>\"\n"
            "  }}</tool_call>\n"
            "\n"
            "Antworte auf Deutsch."
        ).arg(node->title).arg(node->id);

    case ImportStrategy::ReadFile:
        return QString(
            "Du bist ein Code-Analyse-Agent.\n"
            "\n"
            "AUFGABE: Lies die Datei '%1' und speichere den Inhalt.\n"
            "\n"
            "ABLAUF:\n"
            "1. read_file — Datei lesen\n"
            "2. set_result — Inhalt speichern\n"
            "3. plan_done\n"
            "\n"
            "ERLAUBTE TOOLS: read_file, set_result, plan_done\n"
            "\n"
            "Antworte auf Deutsch."
        ).arg(node->title).arg(node->id);

    default:
        return "Analysiere die Datei und speichere den Inhalt.";
    }
}

// ─── buildPhase2Prompt ────────────────────────────────────────────────────────
QString AgentImport::buildPhase2Prompt(const TaskNode *node) const
{
    // Pfad konstruieren: m_importPath + "/" + node->title
    QString filePath = m_importPath;
    if (!filePath.endsWith('/')) filePath += '/';
    filePath += node->title;

    switch (node->importStrategy) {
    case ImportStrategy::Symbols:
        return QString(
            "Analysiere '%1'.\n\n"
            "STARTE SOFORT mit:\n"
            "<tool_call>\n"
            "{\"name\": \"list_symbols\", \"arguments\": {\"file\": \"%1\"}}\n"
            "</tool_call>"
        ).arg(filePath);

    case ImportStrategy::Header:
    case ImportStrategy::ReadFile:
        return QString(
            "Lies '%1' und speichere den Inhalt in Node %2.\n\n"
            "STARTE SOFORT mit:\n"
            "<tool_call>\n"
            "{\"name\": \"read_file\", \"arguments\": {\"path\": \"%1\"}}\n"
            "</tool_call>"
        ).arg(filePath).arg(node->id);

    default:
        return QString("Analysiere '%1'.").arg(filePath);
    }
}

// ─── handleImportResult (Phase 2) ────────────────────────────────────────────
// Verarbeitet den Output des Modells in Phase 2.
// Das Modell kann Tool-Calls machen (list_symbols, get_function_body etc.)
// oder direkt plan_done wenn fertig.
void AgentImport::handleImportResult(const QString &fullResponse,
                                      uint32_t sessionId)
{
    Q_UNUSED(sessionId)

    if (!m_agent.m_currentNode) {
        advanceImport();
        return;
    }

    TaskNode *node = m_agent.m_currentNode;

    // plan_done → Datei abgeschlossen
    if (fullResponse.contains("\"plan_done\"") ||
        fullResponse.contains("name\": \"plan_done\"")) {
        m_agent.m_taskTree.setStatus(node, TaskStatus::Done);
        m_agent.m_taskTree.save();
        emit m_agent.executeNodeDone(node->id);
        emit m_agent.taskTreeUpdated();

        emit m_agent.appendTools(
            QString("<b>Import ✓:</b> <code>%1</code>")
            .arg(node->title.toHtmlEscaped()), "system");

        m_agent.m_currentNode = nullptr;

        if (!advanceImport()) {
            // Alle Dateien fertig
            m_importActive = false;
            m_agent.m_mode = AgentMode::Chat;
            emit m_agent.modeChanged(m_agent.m_mode);
            m_agent.m_chatModel.setSystemPrompt(m_agent.buildFullSystemPrompt());
            emit m_agent.inputEnabled(true);
            emit m_agent.statusChanged("Import abgeschlossen");
            emit m_agent.appendChat(
                QString("<b>[Import abgeschlossen]</b> %1 Dateien importiert.")
                .arg(m_agent.m_taskTree.nodeCount()), "system");
        }
        return;
    }

    // Tool-Call → weiter in handleImportToolCall
    // (wird von Agent::onGenerationDone() dispatcht)
}

// ─── handleImportToolCall (Phase 2) ──────────────────────────────────────────
// Verarbeitet Tool-Calls in Phase 2.
// set_result und create_node (H3) werden intern behandelt.
// list_symbols, get_function_body, read_file gehen an McpManager.
// (Diese Methode überschreibt die Phase-1-Version nicht —
//  Agent::onGenerationDone() dispatcht nach m_phase1Done.)
// HINWEIS: Die vollständige Phase-2-Tool-Call-Verarbeitung ist im
// handleImportToolCall eingebaut — es wird nach toolName unterschieden.

// ─── isImportToolCall ─────────────────────────────────────────────────────────
bool AgentImport::isImportToolCall(const QString &response) const
{
    return ToolCallFormat::isCompleteToolCall(response, m_agent.m_activeToolFormat);
}

// ─── sendToolResult ───────────────────────────────────────────────────────────
void AgentImport::sendToolResult(const QString &toolName,
                                  const QString &result,
                                  uint32_t sessionId)
{
    Q_UNUSED(sessionId)
    m_agent.m_chatModel.addToolResult(toolName, result);
    m_agent.m_generating      = true;
    m_agent.m_generatedTokens = 0;
    m_agent.m_currentResponse.clear();
    m_agent.m_thinkBuffer.clear();
    m_agent.m_inThinkBlock = false;

    QString label = m_phase1Done
        ? "<b>Assistent (Import):</b> "
        : "<b>Assistent (Import-Plan):</b> ";
    emit m_agent.appendChat(label, "assistant");
    emit m_agent.statusChanged("Import läuft...");
    m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
}
