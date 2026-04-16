#pragma once
// ─── AgentExecute ─────────────────────────────────────────────────────────────
// Execute-Modus: Node für Node implementieren lassen.
//
// NEU (Punkt N): Validierungs-Node (H4)
//   Nach jedem H3-Node wird automatisch ein H4-Validierungs-Node eingefügt.
//   Das Modell prüft seine eigene Implementierung:
//     "ok"           → nächster Node
//     "<Fehler>"     → Node als Failed markieren, Optimizer kann eingreifen
//
//   Warum selbst validieren statt sofort kompilieren?
//   - compile (check_syntax) ist teuer (cmake + clang)
//   - Das Modell kennt seinen eigenen Code besser als ein Compiler
//   - Viele Fehler (falsche Signatur, vergessene returns) sind ohne
//     Kompilierung erkennbar
//   - check_syntax als zweite Stufe ist trotzdem möglich (Punkt J)

#include <QString>
#include <cstdint>

class Agent;
struct TaskNode;

class AgentExecute
{
public:
    explicit AgentExecute(Agent &agent) : m_agent(agent) {}

    void startExecute();
    bool advanceExecute();
    void handleExecuteToolCall(const QString &fullResponse, uint32_t sessionId);
    void handleExecuteCode(const QString &fullResponse, uint32_t sessionId);
    void updateThoughts(const TaskNode *node, uint32_t sessionId);
    bool isExecuteToolCall(const QString &response) const;

    // NEU (Punkt N): Validierungs-Node verarbeiten
    void handleValidationResult(const QString &fullResponse, uint32_t sessionId);

private:
    Agent &m_agent;

    // Prüft ob der aktuelle Node ein Validierungs-Node ist
    bool isCurrentNodeValidation() const;

    QString buildExecuteSystemPrompt() const;
    QString buildExecutePrompt(const TaskNode *node) const;

    // NEU: Prompt für Validierungs-Node
    QString buildValidationPrompt(const TaskNode *valNode) const;
};
