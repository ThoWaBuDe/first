#pragma once
#include <QString>
#include <QJsonObject>
#include <cstdint>

#include "Chat/ToolCallFormat.h"

class Agent;

class AgentChat
{
public:
    explicit AgentChat(Agent &agent) : m_agent(agent) {}

    // Tool-Call verarbeiten (format-agnostisch, unterstützt Arrays)
    void handleToolCall(const QString &fullResponse, uint32_t sessionId);

    // Token-Filter: <think>...</think> abfangen
    void filterToken(const QString &token);

    // Kontext zusammenfassen
    void summarizeContext();

    // Statistiken emittieren
    void emitStats();

    // Kontext-Auslastung prüfen
    void checkContextUsage();

    // NEU: /import Command — Quellcode → Nodes
    void handleImport(const QString &path);

private:
    Agent &m_agent;

    // Einzelnen Tool-Call ausführen (aus Queue oder direkt)
    // queueSuffix: optionaler Hinweis "[2 weitere]" für Array-Anzeige
    void executeToolCall(const ParsedToolCall &call,
                         const QString &queueSuffix,
                         uint32_t sessionId);

    // Nächsten Call aus m_pendingToolCalls Queue holen und ausführen
    void executeNextPendingCall(uint32_t sessionId);
};
