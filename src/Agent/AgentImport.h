#pragma once
// ─── AgentImport ──────────────────────────────────────────────────────────────
// Import-Modus: Quellcode-Dateien in den TaskTree importieren.
//
// Zwei-Phasen-Ansatz:
//
//   Phase 1 — Modell plant (kurzer Context):
//     list_dir → create_node H2 pro Datei (mit import_strategy) → plan_done
//     Das Modell entscheidet welche Strategie pro Datei verwendet wird.
//
//   Zwischen Phase 1 und 2 — LlamaQt gruppiert:
//     groupByBasename(): Agent.h + Agent.cpp → H1: "Agent"
//     Das Modell kann danach noch Korrekturen vornehmen (optional).
//
//   Phase 2 — LlamaQt iteriert, Modell analysiert (Context-Reset pro Datei):
//     für jeden H2-Node mit Pending + ImportStrategy != None:
//       - Strategy::Header   → read_file, result = Header-Inhalt
//       - Strategy::Symbols  → list_symbols + get_function_body → H3-Nodes
//       - Strategy::ReadFile → read_file, result = Datei-Inhalt
//       - Strategy::Skip     → überspringen
//     Context wird nach jeder Datei geleert.
//
// Pattern: Active Object (Koordination) + Strategy (ImportStrategy pro Node)
// Analogie AVR: wie ein DMA-Controller der Speicherblöcke sequenziell
// überträgt — jeder Block hat seinen eigenen Transfer-Modus.

#include <QString>
#include <cstdint>
#include <QJsonObject>

class Agent;
struct TaskNode;

class AgentImport
{
public:
    explicit AgentImport(Agent &agent) : m_agent(agent) {}

    // ── Phase 1: Import starten ───────────────────────────────────────────────
    // Bereitet den TaskTree vor und startet das Modell für die Planung.
    // Das Modell bekommt list_dir + create_node + plan_done Tools.
    void startImport(const QString &path);

    // ── Zwischen Phase 1 und 2: Gruppierung + optionale Modell-Korrektur ──────
    // Wird von Agent::onGenerationDone() aufgerufen wenn plan_done erkannt wird.
    void onPhase1Done(uint32_t sessionId);

    // ── Phase 2: nächste Datei analysieren ───────────────────────────────────
    // Analog zu AgentExecute::advanceExecute().
    // Gibt false zurück wenn alle Dateien abgearbeitet sind.
    bool advanceImport();

    // ── Tool-Call im Import-Modus verarbeiten ─────────────────────────────────
    // Phase 1: interne Plan-Tools (create_node, plan_done) + list_dir
    // Phase 2: list_symbols, get_function_body, read_file → H3-Nodes anlegen
    void handleImportToolCall(const QString &fullResponse, uint32_t sessionId);

    // ── Ergebnis einer Phase-2-Analyse verarbeiten ────────────────────────────
    // Wird aufgerufen wenn das Modell den Analyse-Output liefert.
    void handleImportResult(const QString &fullResponse, uint32_t sessionId);

    // ── Prüft ob der aktuelle Node im Import-Modus ist ────────────────────────
    bool isImportMode() const { return m_importActive; }

    // ── Prüft ob Response ein Import-Tool-Call ist ────────────────────────────
    bool isImportToolCall(const QString &response) const;

private:
    Agent &m_agent;

    bool    m_importActive  = false;
    bool    m_phase1Done    = false;
    QString m_importPath;

    // ── Prompts ───────────────────────────────────────────────────────────────
    QString buildPhase1SystemPrompt(const QString &path) const;
    QString buildPhase2SystemPrompt(const TaskNode *node) const;
    QString buildPhase2Prompt(const TaskNode *node) const;

    // ── Interne Tool-Handler (Phase 1) ────────────────────────────────────────
    // Analog zu AgentPlan — direkte TaskTree-Manipulation ohne MCP
    QString handleCreateNode(const QJsonObject &args);
    QString handlePlanDone(const QJsonObject &args, uint32_t sessionId);

    // ── H3-Node aus Tool-Ergebnis anlegen (Phase 2) ───────────────────────────
    void createH3FromSymbols(TaskNode *h2Node, const QString &symbolsResult);
    void createH3FromFunctionBody(TaskNode *h2Node,
                                   const QString &symbol,
                                   const QString &body);

    // Sendet Tool-Ergebnis ins ChatModel und startet nächste Generation
    void sendToolResult(const QString &toolName,
                        const QString &result,
                        uint32_t sessionId);
};
