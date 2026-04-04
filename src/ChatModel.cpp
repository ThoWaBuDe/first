#include "ChatModel.h"

// ChatML ist das Token-Format das Qwen3 (und viele andere) erwartet:
//   <|im_start|>system
//   Du bist ein Assistent.<|im_end|>
//   <|im_start|>user
//   Hallo<|im_end|>
//   <|im_start|>assistant
//   ← hier beginnt die Generierung

static constexpr const char *CHATML_START = "<|im_start|>";
static constexpr const char *CHATML_END   = "<|im_end|>\n";

ChatModel::ChatModel(const QString &systemPrompt)
{
    if (!systemPrompt.isEmpty()) {
        m_messages.push_back({ChatMessage::Role::System, systemPrompt});
    }
}

void ChatModel::setSystemPrompt(const QString &prompt)
{
    // System-Prompt ist immer Index 0. Falls bereits vorhanden: ersetzen.
    // Falls nicht: vorne einfuegen.
    ChatMessage msg{ChatMessage::Role::System, prompt};
    if (!m_messages.isEmpty() && m_messages[0].role == ChatMessage::Role::System)
        m_messages[0] = msg;
    else
        m_messages.prepend(msg);
}

void ChatModel::addUserMessage(const QString &text)
{
    m_messages.push_back({ChatMessage::Role::User, text});
}

void ChatModel::addAssistantMessage(const QString &text)
{
    m_messages.push_back({ChatMessage::Role::Assistant, text});
}

void ChatModel::addToolResult(const QString &toolName, const QString &result)
{
    // Tool-Ergebnisse werden als spezielle User-Nachrichten eingefügt
    // damit das Modell die Antwort sieht und weiter antworten kann.
    QString content = QString("[Tool: %1]\n%2").arg(toolName, result);
    m_messages.push_back({ChatMessage::Role::Tool, content});
}

// ─── formatMessage ───────────────────────────────────────────────────────────
// Wandelt eine ChatMessage in einen ChatML-String um.
// Pattern: Strategy / Template Method — jede Role hat ihr eigenes Tag.
QString ChatModel::formatMessage(const ChatMessage &msg)
{
    QString roleStr;
    switch (msg.role) {
        case ChatMessage::Role::System:    roleStr = "system";    break;
        case ChatMessage::Role::User:      roleStr = "user";      break;
        case ChatMessage::Role::Assistant: roleStr = "assistant"; break;
        case ChatMessage::Role::Tool:      roleStr = "user";      break; // Tool-Results als user
    }
    return QString("%1%2\n%3%4")
        .arg(CHATML_START)
        .arg(roleStr)
        .arg(msg.content)
        .arg(CHATML_END);
}

// ─── buildPrompt ─────────────────────────────────────────────────────────────
// Konkateniert alle Nachrichten + öffnet den Assistant-Turn.
// llama.cpp generiert ab dem letzten "<|im_start|>assistant\n".
QString ChatModel::buildPrompt() const
{
    QString prompt;
    for (const auto &msg : m_messages) {
        prompt += formatMessage(msg);
    }
    // Öffnet den Assistant-Turn — llama.cpp füllt ab hier auf
    prompt += QString("%1assistant\n").arg(CHATML_START);
    return prompt;
}

void ChatModel::clear()
{
    // System-Prompt (Index 0) behalten, Rest löschen
    if (!m_messages.isEmpty() && m_messages[0].role == ChatMessage::Role::System) {
        m_messages.resize(1);
    } else {
        m_messages.clear();
    }
}
