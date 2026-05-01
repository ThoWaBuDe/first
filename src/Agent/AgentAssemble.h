#pragma once
// ─── AgentAssemble ────────────────────────────────────────────────────────────
// Zuständig für die Assembly der Nodes zu Dateien.
// Dünner Wrapper um CodeAssembler — kapselt die Zielverzeichnis-Logik
// und das Feedback an die UI.

#include <QString>

class Agent;

class AgentAssemble
{
public:
    explicit AgentAssemble(Agent &agent) : m_agent(agent) {}

    // Assembliert alle H2-Nodes zu Dateien im Sandbox-Verzeichnis.
    // Ziel: ~/llamatools/[executeSandboxProject]/
    void assembleProject();

private:
    Agent &m_agent;
};
