#include "AgentPlan.h"
#include "Agent.h"
#include "AgentUtils.h"
#include "AppConfig.h"
#include <QJsonDocument>
#include <QJsonArray>

// ─── Interne Tool-Namen ───────────────────────────────────────────────────────
static const QString TOOL_CREATE_NODE    = "create_node";
static const QString TOOL_SET_DEPENDS_ON = "set_depends_on";
static const QString TOOL_GET_NODES      = "get_nodes";
static const QString TOOL_PLAN_DONE      = "plan_done";

static const QStringList INTERNAL_PLAN_TOOLS = {
    TOOL_CREATE_NODE, TOOL_SET_DEPENDS_ON,
    TOOL_GET_NODES,   TOOL_PLAN_DONE,
};

// ─── startPlan ────────────────────────────────────────────────────────────────
void AgentPlan::startPlan(const QString &auftrag)
{
    m_agent.m_mode = AgentMode::Plan;
    emit m_agent.modeChanged(m_agent.m_mode);

    // Frischen Tree starten — jeder /plan-Aufruf beginnt von vorne.
    // Analogie AVR: DMA-Buffer vor neuem Transfer leeren.
    m_agent.m_taskTree.clear();
    m_agent.m_planRetryCount    = 0;
    m_agent.m_continuationCount = 0;

    m_agent.m_chatModel.clear();
    m_agent.m_chatModel.setSystemPrompt(buildPlannerSystemPrompt(auftrag));

    m_agent.m_currentResponse.clear();
    m_agent.m_thinkBuffer.clear();
    m_agent.m_inThinkBlock    = false;
    m_agent.m_generatedTokens = 0;
    m_agent.m_generating      = true;

    m_agent.m_chatModel.addUserMessage(
        QString("Analysiere das Projekt und erstelle einen Plan für: %1\n\n"
                "Nutze create_node um den Aufgabenbaum aufzubauen. "
                "Beginne mit dem H0-Zielknoten, dann H1-Gruppen, "
                "dann H2-Dateien, dann H3-Methoden. "
                "Nutze set_depends_on für Abhängigkeiten. "
                "Schließe mit plan_done ab.")
        .arg(auftrag));

    emit m_agent.appendChat("<b>Assistent (Plan-Analyse):</b> ", "assistant");
    emit m_agent.appendTools(
        QString("<b>Plan-Modus gestartet:</b> %1<br>"
                "<small>Iterativer Modus — Modell baut Graph Node für Node</small>")
        .arg(auftrag.toHtmlEscaped()), "system");

    emit m_agent.inputEnabled(false);
    emit m_agent.statusChanged("Plan-Analyse läuft...");
    m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
}

// ─── buildPlannerSystemPrompt ─────────────────────────────────────────────────
// Iterativer Modus: Modell bekommt interne Tools statt JSON-Format.
//
// Das Modell kennt jetzt:
//   1. Lese-Tools (wie bisher): um das Projekt zu verstehen
//   2. Interne Plan-Tools: um den Graph aufzubauen
//
// Symbol-Awareness (Punkt F) ist eingebaut:
//   create_node verlangt ein "symbol"-Feld.
//   Wenn das Symbol schon existiert → Tool gibt Hinweis zurück.
QString AgentPlan::buildPlannerSystemPrompt(const QString &auftrag) const
{
    Q_UNUSED(auftrag)
    return QString(
        "Du bist ein Planungs-Agent fuer C++/Qt6 Projekte.\n"
        "\n"
        "DEINE AUFGABE:\n"
        "1. Analysiere das Projekt mit Lese-Tools\n"
        "2. Baue den Aufgabenbaum mit Plan-Tools auf\n"
        "3. Schließe mit plan_done ab\n"
        "\n"
        "── LESE-TOOLS ────────────────────────────────────────────────────────\n"
        "read_file, list_dir, get_symbol, get_project_index,\n"
        "get_time, sys_info, disk_free, get_pwd\n"
        "\n"
        "── PLAN-TOOLS (zum Aufbau des Aufgabenbaums) ────────────────────────\n"
        "\n"
        "create_node — Neuen Knoten anlegen\n"
        "  <tool_call>{\"name\": \"create_node\", \"arguments\": {\n"
        "    \"title\":       \"GameState.cpp\",\n"
        "    \"level\":       2,\n"
        "    \"scope\":       \"internal\",\n"
        "    \"description\": \"Implementiert alle GameState-Methoden\",\n"
        "    \"symbol\":      \"GameState\",\n"
        "    \"parent_id\":   3\n"
        "  }}</tool_call>\n"
        "  Felder: title (required), level (0-4), scope (internal/external),\n"
        "          description, symbol (C++-Symbolname, z.B. 'Timer::start()'),\n"
        "          parent_id (id eines bestehenden Knotens, -1 fuer Wurzel)\n"
        "  Antwort: {\"id\": N, \"title\": \"...\"} oder Fehler wenn Symbol-Duplikat\n"
        "\n"
        "set_depends_on — Abhängigkeit zwischen Knoten setzen\n"
        "  <tool_call>{\"name\": \"set_depends_on\", \"arguments\": {\n"
        "    \"from_id\": 5, \"to_id\": 2\n"
        "  }}</tool_call>\n"
        "  Bedeutung: Knoten from_id benoetigt Knoten to_id\n"
        "\n"
        "get_nodes — Aktuellen Baum ansehen\n"
        "  <tool_call>{\"name\": \"get_nodes\", \"arguments\": {}}</tool_call>\n"
        "  Zeigt alle bisher erstellten Knoten mit IDs\n"
        "\n"
        "plan_done — Plan abschließen\n"
        "  <tool_call>{\"name\": \"plan_done\", \"arguments\": {\n"
        "    \"summary\": \"Kurze Zusammenfassung des Plans\"\n"
        "  }}</tool_call>\n"
        "\n"
        "── HIERARCHIE-REGELN ─────────────────────────────────────────────────\n"
        "level 0 (H0) — Gesamtziel (genau 1x, parent_id=-1)\n"
        "level 1 (H1) — Dateigruppe oder Modul\n"
        "level 2 (H2) — Einzelne Datei (title = Dateiname)\n"
        "level 3 (H3) — Implementierungsschritt (eine Methode)\n"
        "\n"
        "H3-REGEL:\n"
        "  .h Dateien      → H2-Blatt (kein H3 noetig)\n"
        "  .cpp Dateien    → H2 + H3-Kinder (jede Methode ein Node)\n"
        "  CMakeLists.txt  → H2-Blatt\n"
        "\n"
        "SYMBOL-REGEL (verhindert Duplikate!):\n"
        "  Jedes C++-Symbol darf nur in EINEM Node implementiert werden.\n"
        "  Wenn create_node ein bekanntes Symbol meldet → set_depends_on\n"
        "  statt einen neuen Node anlegen.\n"
        "  Beispiele fuer symbol-Feld:\n"
        "    H2 .h:   'Timer'              (Klassen-Interface)\n"
        "    H2 .cpp: 'Timer'              (Datei insgesamt)\n"
        "    H3:      'Timer::start()'     (Methode)\n"
        "    H3:      'Timer::m_interval'  (Member)\n"
        "\n"
        "── REIHENFOLGE ───────────────────────────────────────────────────────\n"
        "1. Erst Lese-Tools um Projekt zu verstehen\n"
        "2. H0 anlegen (Wurzel)\n"
        "3. H1-Gruppen anlegen\n"
        "4. H2-Dateien anlegen\n"
        "5. H3-Methoden anlegen\n"
        "6. set_depends_on fuer alle Abhaengigkeiten\n"
        "7. get_nodes zum Ueberpruefen\n"
        "8. plan_done\n"
        "\n"
        "Antworte auf Deutsch."
    );
}

// ─── isInternalPlanTool ───────────────────────────────────────────────────────
bool AgentPlan::isInternalPlanTool(const QString &toolName) const
{
    return INTERNAL_PLAN_TOOLS.contains(toolName);
}

// ─── handlePlanToolCall ───────────────────────────────────────────────────────
// Dispatcht auf interne Plan-Tools ODER externe Lese-Tools.
void AgentPlan::handlePlanToolCall(const QString &fullResponse, uint32_t sessionId)
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
                QString("[SYSTEM: Ungültiges JSON. Fehler: %1. "
                        "Bitte korrektes JSON verwenden.]")
                .arg(pe.errorString()));
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.m_currentResponse.clear();
            m_agent.m_thinkBuffer.clear();
            m_agent.m_inThinkBlock = false;
            emit m_agent.appendChat(
                "<b>Assistent (Plan-Analyse):</b> ", "assistant");
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
    }

    QString     toolName = doc.object().value("name").toString();
    QJsonObject toolArgs = doc.object().value("arguments").toObject();

    emit m_agent.appendTools(
        QString("<b>Plan-Tool: %1</b><br><pre>%2</pre>")
        .arg(toolName.toHtmlEscaped(),
             QString::fromUtf8(QJsonDocument(toolArgs)
                               .toJson(QJsonDocument::Indented)).toHtmlEscaped()),
        "tool");

    // ── Interne Plan-Tools ────────────────────────────────────────────────
    if (isInternalPlanTool(toolName)) {
        QString result;
        bool    isDone = false;

        if (toolName == TOOL_CREATE_NODE)
            result = handleCreateNode(toolArgs);
        else if (toolName == TOOL_SET_DEPENDS_ON)
            result = handleSetDependsOn(toolArgs);
        else if (toolName == TOOL_GET_NODES)
            result = handleGetNodes(toolArgs);
        else if (toolName == TOOL_PLAN_DONE) {
            result = handlePlanDone(toolArgs, sessionId);
            isDone = true;
        }

        emit m_agent.appendTools(
            QString("<b>Plan-Tool Ergebnis:</b><br><pre>%1</pre>")
            .arg(result.toHtmlEscaped()), "tool");

        if (!isDone)
            sendToolResult(toolName, result, false, sessionId);
        // Bei plan_done: handlePlanDone hat bereits alles erledigt
        return;
    }

    // ── Externe Lese-Tools ────────────────────────────────────────────────
    if (!Agent::PLAN_ALLOWED_TOOLS.contains(toolName)) {
        QString errMsg = QString(
            "[SYSTEM: Tool '%1' ist im Plan-Modus VERBOTEN. "
            "Nur Lese-Tools und Plan-Tools erlaubt.]")
            .arg(toolName);
        emit m_agent.appendTools(
            QString("Plan-Whitelist: <b>%1</b> verboten.")
            .arg(toolName.toHtmlEscaped()), "error");
        sendToolResult(toolName, errMsg, false, sessionId);
        return;
    }

    if (!m_agent.m_mcp.containsTool(toolName)) {
        sendToolResult(toolName,
            QString("Fehler: Tool '%1' nicht verfügbar.").arg(toolName),
            false, sessionId);
        return;
    }

    emit m_agent.statusChanged(QString("Plan-Tool: %1...").arg(toolName));
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
                        QString("<b>Plan: DEADLOCK ABBRUCH</b> '%1'")
                        .arg(toolName.toHtmlEscaped()), "error");
                    m_agent.m_toolFailCount.remove(tKey);
                    m_agent.m_mode = AgentMode::Chat;
                    emit m_agent.modeChanged(m_agent.m_mode);
                    m_agent.m_chatModel.setSystemPrompt(
                        m_agent.buildFullSystemPrompt());
                    emit m_agent.inputEnabled(true);
                    emit m_agent.statusChanged("Plan fehlgeschlagen");
                    return;
                }
                toolResult += "\n\n" +
                    AgentUtils::deadlockEscalationPrompt(toolName, failCount);
            } else {
                m_agent.m_toolFailCount.remove(tKey);
            }

            emit m_agent.appendTools(
                QString("<b>Plan-Ergebnis [%1]:</b><br><pre>%2</pre>")
                .arg(toolName.toHtmlEscaped(),
                     toolResult.left(600).toHtmlEscaped() +
                     (toolResult.length() > 600 ? "\n..." : "")),
                isErr ? "error" : "tool");

            sendToolResult(toolName, toolResult, false, sessionId);
        });
}

// ─── handleCreateNode ─────────────────────────────────────────────────────────
// Legt einen neuen Node im TaskTree an.
//
// Symbol-Duplikat-Prüfung (Punkt H + G):
//   Wenn das übergebene Symbol schon existiert, wird kein neuer Node angelegt.
//   Stattdessen: Hinweis mit der ID des bestehenden Nodes → Modell soll
//   set_depends_on verwenden.
QString AgentPlan::handleCreateNode(const QJsonObject &args)
{
    QString title       = args.value("title").toString().trimmed();
    int     level       = args.value("level").toInt(3);
    QString scopeStr    = args.value("scope").toString("internal");
    QString description = args.value("description").toString();
    QString symbol      = args.value("symbol").toString().trimmed();
    qint64  parentId    = args.value("parent_id").toInteger(-1);
    int     order       = args.value("order").toInt(0);

    if (title.isEmpty())
        return R"({"error": "title ist required"})";

    // ── Symbol-Duplikat-Prüfung ───────────────────────────────────────────
    if (!symbol.isEmpty() && m_agent.m_taskTree.symbolExists(symbol)) {
        TaskNode *existing = m_agent.m_taskTree.findBySymbol(symbol);
        return QString(
            "{\"warning\": \"Symbol '%1' existiert bereits in Node %2 ('%3'). "
            "Verwende set_depends_on statt einen neuen Node anzulegen.\", "
            "\"existing_id\": %2}")
            .arg(symbol)
            .arg(existing ? existing->id : -1)
            .arg(existing ? existing->title : "?");
    }

    // ── Elternknoten suchen ───────────────────────────────────────────────
    TaskNode *parent = nullptr;
    if (parentId >= 0) {
        parent = m_agent.m_taskTree.findById(parentId);
        if (!parent)
            return QString(
                "{\"error\": \"parent_id %1 nicht gefunden. "
                "Verwende get_nodes um gültige IDs zu sehen.\"}")
                .arg(parentId);
    }

    TaskScope scope = (scopeStr == "external")
                      ? TaskScope::External : TaskScope::Internal;

    TaskNode *node = m_agent.m_taskTree.createNode(
        title, description, level, scope, order, parent, symbol);

    emit m_agent.taskTreeUpdated();

    return QString(
        "{\"id\": %1, \"title\": \"%2\", \"level\": %3%4}")
        .arg(node->id)
        .arg(node->title)
        .arg(node->level)
        .arg(symbol.isEmpty() ? ""
             : QString(", \"symbol\": \"%1\"").arg(symbol));
}

// ─── handleSetDependsOn ───────────────────────────────────────────────────────
QString AgentPlan::handleSetDependsOn(const QJsonObject &args)
{
    qint64 fromId = args.value("from_id").toInteger(-1);
    qint64 toId   = args.value("to_id").toInteger(-1);

    if (fromId < 0 || toId < 0)
        return R"({"error": "from_id und to_id sind required"})";

    TaskNode *from = m_agent.m_taskTree.findById(fromId);
    TaskNode *to   = m_agent.m_taskTree.findById(toId);

    if (!from)
        return QString(R"({"error": "from_id %1 nicht gefunden"})").arg(fromId);
    if (!to)
        return QString(R"({"error": "to_id %1 nicht gefunden"})").arg(toId);
    if (fromId == toId)
        return R"({"error": "Knoten kann nicht von sich selbst abhängen"})";

    m_agent.m_taskTree.addDependency(from, to);

    return QString(
        "{\"ok\": true, \"message\": \"%1 haengt jetzt ab von %2\"}")
        .arg(from->title).arg(to->title);
}

// ─── handleGetNodes ───────────────────────────────────────────────────────────
// Gibt den aktuellen Baum als kompakte Liste aus.
// Das Modell kann damit prüfen was schon da ist.
QString AgentPlan::handleGetNodes(const QJsonObject &args)
{
    Q_UNUSED(args)

    if (m_agent.m_taskTree.isEmpty())
        return R"({"nodes": [], "message": "Baum ist noch leer"})";

    QJsonArray nodes;
    m_agent.m_taskTree.traverse([&](const TaskNode *n) {
        QJsonObject obj;
        obj["id"]    = n->id;
        obj["title"] = n->title;
        obj["level"] = n->level;
        if (!n->symbol.isEmpty())
            obj["symbol"] = n->symbol;
        if (n->parent)
            obj["parent_id"] = n->parent->id;
        if (!n->dependsOn.isEmpty()) {
            QJsonArray deps;
            for (qint64 d : n->dependsOn) deps.append(d);
            obj["depends_on"] = deps;
        }
        nodes.append(obj);
    });

    QJsonObject result;
    result["nodes"] = nodes;
    result["count"] = nodes.size();
    result["symbols_used"] = QJsonArray::fromStringList(
        m_agent.m_taskTree.allSymbols());

    return QString::fromUtf8(
        QJsonDocument(result).toJson(QJsonDocument::Indented));
}

// ─── handlePlanDone ───────────────────────────────────────────────────────────
// Plan ist abgeschlossen — Baum anzeigen, Bestätigung abwarten.
QString AgentPlan::handlePlanDone(const QJsonObject &args, uint32_t sessionId)
{
    Q_UNUSED(sessionId)
    QString summary = args.value("summary").toString("Plan erstellt.");

    int nodeCount = m_agent.m_taskTree.nodeCount();

    if (nodeCount == 0) {
        return R"({"error": "Plan ist leer. Bitte create_node verwenden um Knoten anzulegen."})";
    }

    // ── Duplikat-Check über alle Nodes ────────────────────────────────────
    // Prüft ob Nodes ohne Symbol auf bekannte Symbole hindeuten.
    // Einfache Heuristik: Titel-Vergleich.
    int warnCount = 0;
    QString warnings;
    QStringList knownSymbols = m_agent.m_taskTree.allSymbols();

    m_agent.m_taskTree.traverse([&](const TaskNode *n) {
        if (n->level < static_cast<int>(TaskLevel::Impl)) return;
        if (!n->symbol.isEmpty()) return; // hat Symbol → ok
        // H3 ohne Symbol → Warnung
        ++warnCount;
        warnings += QString("  Node %1 '%2' hat kein Symbol\n")
                    .arg(n->id).arg(n->title);
    });

    emit m_agent.appendTools(
        QString("<b>Plan abgeschlossen:</b> %1 Knoten, %2 Symbole%3")
        .arg(nodeCount)
        .arg(knownSymbols.size())
        .arg(warnCount > 0
             ? QString("<br><small>⚠ %1 H3-Nodes ohne Symbol</small>")
               .arg(warnCount)
             : ""),
        "system");

    emit m_agent.taskTreeUpdated();
    emit m_agent.planReady();
    emit m_agent.inputEnabled(true);
    emit m_agent.statusChanged("Plan bereit — bitte bestätigen oder ablehnen");

    return QString("{\"ok\": true, \"nodes\": %1, \"summary\": \"%2\"}")
           .arg(nodeCount).arg(summary);
}

// ─── sendToolResult ───────────────────────────────────────────────────────────
// Fügt Tool-Ergebnis ins ChatModel ein und startet nächste Generation.
void AgentPlan::sendToolResult(const QString &toolName,
                                const QString &result,
                                bool           isPlanDone,
                                uint32_t       sessionId)
{
    Q_UNUSED(isPlanDone)
    Q_UNUSED(sessionId)

    m_agent.m_chatModel.addToolResult(toolName, result);
    m_agent.m_generating      = true;
    m_agent.m_generatedTokens = 0;
    m_agent.m_currentResponse.clear();
    m_agent.m_thinkBuffer.clear();
    m_agent.m_inThinkBlock = false;
    emit m_agent.appendChat("<b>Assistent (Plan-Analyse):</b> ", "assistant");
    emit m_agent.statusChanged("Plan-Analyse läuft...");
    m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
}

// ─── handlePlanJson ───────────────────────────────────────────────────────────
// Fallback: altes JSON-Format verarbeiten (Kompatibilität).
// Identisch mit der ursprünglichen Implementierung, plus Symbol-Index aufbauen.
void AgentPlan::handlePlanJson(const QString &fullResponse, uint32_t sessionId)
{
    int planStart = fullResponse.indexOf("<plan>") + 6;
    int planEnd   = fullResponse.indexOf("</plan>", planStart);
    QString planJson = fullResponse.mid(planStart, planEnd - planStart).trimmed();

    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(planJson.toUtf8(), &pe);

    if (doc.isNull()) {
        emit m_agent.appendTools(
            QString("<b>Plan JSON-Fehler:</b> %1<br><pre>%2</pre>")
            .arg(pe.errorString().toHtmlEscaped(),
                 planJson.left(300).toHtmlEscaped()), "error");

        QString repaired = AgentUtils::repairJson(planJson);
        if (!repaired.isEmpty()) {
            doc = QJsonDocument::fromJson(repaired.toUtf8());
            emit m_agent.appendTools(
                "<b>Plan JSON repariert.</b>", "system");
        } else {
            ++m_agent.m_planRetryCount;
            if (m_agent.m_planRetryCount > Agent::MAX_PLAN_RETRIES) {
                emit m_agent.appendTools(
                    "<b>Plan fehlgeschlagen:</b> JSON ungültig.", "error");
                m_agent.m_mode = AgentMode::Chat;
                emit m_agent.modeChanged(m_agent.m_mode);
                m_agent.m_chatModel.setSystemPrompt(
                    m_agent.buildFullSystemPrompt());
                emit m_agent.inputEnabled(true);
                emit m_agent.statusChanged("Plan fehlgeschlagen");
                return;
            }
            QString errFeedback = QString(
                "[SYSTEM: Ungültiges JSON in <plan>. Fehler: %1. "
                "Bitte verwende stattdessen die create_node / plan_done Tools.]")
                .arg(pe.errorString());
            m_agent.m_chatModel.addAssistantMessage(fullResponse);
            m_agent.m_chatModel.addToolResult("plan_json_error", errFeedback);
            m_agent.m_generating      = true;
            m_agent.m_generatedTokens = 0;
            m_agent.m_currentResponse.clear();
            m_agent.m_thinkBuffer.clear();
            m_agent.m_inThinkBlock = false;
            emit m_agent.appendChat(
                "<b>Assistent (Plan-Korrektur):</b> ", "assistant");
            emit m_agent.statusChanged("Plan-JSON wird korrigiert...");
            m_agent.startGeneration(LlamaWorker::SamplerProfile::Tool);
            return;
        }
    }

    // ── JSON valide → wie bisher verarbeiten ─────────────────────────────
    QJsonObject root = doc.object();
    QString goalTitle = root.value("goal").toString("Unbenanntes Ziel");

    TaskNode *goalNode = m_agent.m_taskTree.createNode(
        goalTitle, "", static_cast<int>(TaskLevel::Goal),
        TaskScope::External, 0, nullptr);
    goalNode->status = TaskStatus::Pending;

    QHash<QString, qint64>     titleToId;
    QHash<qint64, QStringList> pendingDeps;
    titleToId.insert(goalTitle, goalNode->id);

    QJsonArray children = root.value("children").toArray();
    int nodeCount = 1;
    for (int i = 0; i < children.size(); ++i)
        nodeCount += parsePlanNode(children[i].toObject(), goalNode, 1,
                                   titleToId, pendingDeps);

    // dependsOn auflösen (2. Pass)
    int resolvedDeps = 0, unresolvedDeps = 0;
    for (auto it = pendingDeps.constBegin(); it != pendingDeps.constEnd(); ++it) {
        TaskNode *node = m_agent.m_taskTree.findById(it.key());
        if (!node) continue;
        for (const QString &depTitle : it.value()) {
            qint64 depId = titleToId.value(depTitle, -1);
            if (depId < 0) {
                for (auto jt = titleToId.constBegin();
                     jt != titleToId.constEnd(); ++jt) {
                    bool exactFilename =
                        jt.key().contains('.') &&
                        (jt.key() == depTitle ||
                         depTitle.contains(jt.key(), Qt::CaseInsensitive));
                    if (exactFilename) { depId = jt.value(); break; }
                }
            }
            if (depId >= 0 && depId != node->id) {
                m_agent.m_taskTree.addDependency(node,
                    m_agent.m_taskTree.findById(depId));
                ++resolvedDeps;
            } else {
                ++unresolvedDeps;
            }
        }
    }

    emit m_agent.appendTools(
        QString("<b>Plan (JSON-Fallback):</b> %1 Knoten, "
                "%2 Abhängigkeiten aufgelöst.")
        .arg(nodeCount).arg(resolvedDeps), "system");

    emit m_agent.taskTreeUpdated();
    emit m_agent.planReady();
    emit m_agent.inputEnabled(true);
    emit m_agent.statusChanged("Plan bereit — bitte bestätigen oder ablehnen");
}

// ─── parsePlanNode ────────────────────────────────────────────────────────────
int AgentPlan::parsePlanNode(const QJsonObject &obj,
                              TaskNode *parent, int depth,
                              QHash<QString, qint64>     &titleToId,
                              QHash<qint64, QStringList> &pendingDeps)
{
    QString title = obj.value("title").toString(
        QString("Unbenannte Aufgabe (H%1)").arg(depth));
    int level = obj.contains("level")
                ? obj.value("level").toInt(depth) : depth;
    TaskScope scope = (obj.value("scope").toString() == "external")
                      ? TaskScope::External : TaskScope::Internal;
    QString description = obj.value("description").toString();
    QString symbol      = obj.value("symbol").toString();
    int order = obj.value("order").toInt(0);

    TaskNode *node = m_agent.m_taskTree.createNode(
        title, description, level, scope, order, parent, symbol);

    if (!titleToId.contains(title))
        titleToId.insert(title, node->id);

    QJsonArray deps = obj.value("dependsOn").toArray();
    if (!deps.isEmpty()) {
        QStringList depTitles;
        for (const QJsonValue &v : deps) depTitles << v.toString();
        if (!depTitles.isEmpty())
            pendingDeps.insert(node->id, depTitles);
    }

    QJsonArray children = obj.value("children").toArray();
    int count = 1;
    for (int i = 0; i < children.size(); ++i)
        count += parsePlanNode(children[i].toObject(), node, depth + 1,
                               titleToId, pendingDeps);
    return count;
}
