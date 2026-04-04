#pragma once
#include <QString>
#include <QVector>

// ─── ChatMessage ─────────────────────────────────────────────────────────────
// Einfaches Value-Object (POD-ähnlich) das eine einzelne Nachricht hält.
// Pattern: Data Transfer Object (DTO)
struct ChatMessage {
    enum class Role { System, User, Assistant, Tool };

    Role    role;
    QString content;
};

// ─── ChatModel ───────────────────────────────────────────────────────────────
// Hält die gesamte Gesprächshistorie und baut daraus den llama.cpp-kompatiblen
// Prompt-String zusammen (ChatML Format, das Qwen3 erwartet).
//
// Pattern: Model aus MVC — reines Datenmodell, keine UI-Abhängigkeiten.
class ChatModel {
public:
    explicit ChatModel(const QString &systemPrompt = {});

    // System-Prompt nachtraeglich setzen (nach MCP-Server-Start)
    void setSystemPrompt(const QString &prompt);

    void addUserMessage(const QString &text);
    void addAssistantMessage(const QString &text);
    void addToolResult(const QString &toolName, const QString &result);

    // Gibt den vollständigen Prompt für llama.cpp zurück
    // Format: ChatML  (<|im_start|>role\ncontent<|im_end|>\n ...)
    QString buildPrompt() const;

    // Zugriff auf History (für UI-Darstellung)
    const QVector<ChatMessage> &messages() const { return m_messages; }

    void clear();

private:
    QVector<ChatMessage> m_messages;

    // Hilfsfunktion: eine einzelne Nachricht als ChatML formatieren
    static QString formatMessage(const ChatMessage &msg);
};
