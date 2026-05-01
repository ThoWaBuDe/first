#pragma once
// ─── AgentPlan ────────────────────────────────────────────────────────────────
// Zuständig für den Plan-Modus.
//
// NEU (Punkt G): Iterativer Plan-Aufbau
//   Statt einem großen JSON liefert das Modell den Graph Node für Node
//   über interne Tools:
//     create_node    — neuen Node anlegen
//     set_depends_on — Abhängigkeit setzen
//     plan_done      — Plan abgeschlossen
//
//   Warum iterativ statt einem großen JSON?
//   - Das 9B-Modell verliert bei >20 Nodes den Überblick
//   - Jeder Tool-Call ist einfach: ein Node, klare Parameter
//   - Symbol-Awareness ist einfacher: nach jedem create_node prüfen
//     ob das Symbol schon existiert → Duplikat sofort verhindern
//   - Fehler sind lokal: ein falscher Node → eine Korrektur
//
//   Kompatibilität: Das alte JSON-Format bleibt als Fallback erhalten.
//   Wenn das Modell doch <plan>...</plan> liefert, wird es wie bisher
//   verarbeitet. So sind alte Prompts noch nutzbar.

#include <QString>
#include <QJsonObject>
#include <QHash>
#include <cstdint>
#include "Chat/ToolCallFormat.h"

class Agent;
struct TaskNode;

class AgentPlan
{
public:
    explicit AgentPlan(Agent &agent) : m_agent(agent) {}

    // ── Öffentliche Schnittstelle ─────────────────────────────────────────────

    // Plan-Modus starten (iterativer Modus als Standard)
    void startPlan(const QString &auftrag);

    // Verarbeitet einen Tool-Call im Plan-Modus
    // Dispatcht auf interne Tools (create_node etc.) ODER externe Lese-Tools
    void handlePlanToolCall(const QString &fullResponse, uint32_t sessionId);

    // Fallback: <plan>...</plan> JSON parsen
    void handlePlanJson(const QString &fullResponse, uint32_t sessionId);

private:
    Agent &m_agent;

    // ── Interne Plan-Tools (Punkt G) ──────────────────────────────────────────
    // Diese Tools manipulieren direkt den TaskTree im GUI-Thread.
    // Kein MCP-Server nötig — analogie zu PlanOptimizer.
    //
    // Rückgabe: Tool-Ergebnis als String (wird als tool_result ins ChatModel)
    QString handleCreateNode(const QJsonObject &args);
    QString handleSetDependsOn(const QJsonObject &args);
    QString handleGetNodes(const QJsonObject &args);
    QString handlePlanDone(const QJsonObject &args, uint32_t sessionId);

    // Prüft ob ein Tool-Call ein internes Plan-Tool ist
    bool isInternalPlanTool(const QString &toolName) const;

    // ── Hilfsmethoden ─────────────────────────────────────────────────────────
    QString buildPlannerSystemPrompt(const QString &auftrag) const;

    // Fallback: JSON-Parser (alter Modus)
    int parsePlanNode(const QJsonObject &obj,
                      TaskNode         *parent,
                      int               depth,
                      QHash<QString, qint64>     &titleToId,
                      QHash<qint64, QStringList> &pendingDeps);

    // Hilfsfunktion: Tool-Ergebnis → ChatModel → nächste Generation
    void sendToolResult(const QString &toolName,
                        const QString &result,
                        bool           isPlanDone = false,
                        uint32_t       sessionId  = 0);
};
