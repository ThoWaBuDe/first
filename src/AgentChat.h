#pragma once
// ─── AgentChat ────────────────────────────────────────────────────────────────
// Zuständig für den Chat-Modus des Agent:
//   - Tool-Calls im normalen Chat verarbeiten
//   - Token-Filter (<think>-Blöcke ausblenden)
//   - Kontext-Zusammenfassung
//
// Pattern: Komposition — AgentChat hält eine Referenz auf Agent und
//          greift über friend-Deklaration auf dessen private Member zu.
//
// Analogie AVR: wie ein UART-Handler der nur seinen eigenen Puffer kennt
//               und über eine gemeinsame Struct mit dem Hauptprogramm kommuniziert.

#include <QString>
#include <QJsonObject>
#include <cstdint>

class Agent; // Forward-Declaration — kein vollständiger Header nötig

class AgentChat
{
public:
    explicit AgentChat(Agent &agent) : m_agent(agent) {}

    // ── Öffentliche Schnittstelle (von Agent::onGenerationDone aufgerufen) ────

    // Normaler Chat-Tool-Call verarbeiten (Whitelist: alle Tools erlaubt)
    void handleToolCall(const QString &fullResponse, uint32_t sessionId);

    // Token-Filter: <think>...</think> Blöcke abfangen und in toolView umleiten
    // Zustandsbehaftet — m_agent.m_thinkBuffer / m_agent.m_inThinkBlock
    void filterToken(const QString &token);

    // Kontext zusammenfassen wenn Schwelle überschritten
    void summarizeContext();

    // Statistiken emittieren
    void emitStats();

    // Kontext-Auslastung prüfen, ggf. auto-summarize anstoßen
    void checkContextUsage();

private:
    Agent &m_agent;
};
