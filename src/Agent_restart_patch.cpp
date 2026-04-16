// ─── PATCH Agent.cpp — start() ────────────────────────────────────────────────
// Nach dem connect(&m_mcp, &McpManager::serverDied, ...) Block,
// folgende connects hinzufügen:

/*
    // MCP Auto-Restart Feedback (Punkt S)
    connect(&m_mcp, &McpManager::serverRestarting,
            this, [this](const QString &name, int attempt, int delaySec) {
        emit appendTools(
            QString("<b>MCP Neustart:</b> <i>%1</i> — "
                    "Versuch %2/%3 in %4s...")
            .arg(name.toHtmlEscaped())
            .arg(attempt)
            .arg(McpManager::MAX_RESTART_ATTEMPTS)  // oder hardcode 3
            .arg(delaySec),
            "system");
    });

    connect(&m_mcp, &McpManager::serverRestored,
            this, [this](const QString &name) {
        emit appendTools(
            QString("<b>MCP wiederhergestellt:</b> <i>%1</i> — "
                    "Tools wieder verfügbar.")
            .arg(name.toHtmlEscaped()),
            "system");
    });

    connect(&m_mcp, &McpManager::serverGaveUp,
            this, [this](const QString &name) {
        emit appendTools(
            QString("<b>MCP aufgegeben:</b> <i>%1</i> — "
                    "Alle Neustart-Versuche fehlgeschlagen. "
                    "Bitte LlamaQt neu starten.")
            .arg(name.toHtmlEscaped()),
            "error");
    });
*/

// ─── PATCH AgentExecute.cpp — advanceExecute() ───────────────────────────────
// Letzte Zeile in advanceExecute() ändern:
//
// ALT:
//     m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
//
// NEU:
//     m_agent.startGeneration(LlamaWorker::SamplerProfile::Execute);
//
// ─── PATCH AgentExecute.cpp — handleExecuteToolCall() ────────────────────────
// Am Ende des Callbacks, startGeneration ändern:
//
// ALT:
//     m_agent.startGeneration(LlamaWorker::SamplerProfile::Chat);
//
// NEU:
//     // Im Execute-Modus nach Tool-Ergebnis weiter mit Execute-Sampler
//     m_agent.startGeneration(LlamaWorker::SamplerProfile::Execute);
//
// HINWEIS: updateThoughts() bleibt auf Chat-Sampler —
// Thoughts-Update ist Konversation, kein Code-Generierung.
