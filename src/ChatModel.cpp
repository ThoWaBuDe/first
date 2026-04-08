#include "ChatModel.h"

// ─── ChatModel ───────────────────────────────────────────────────────────────
// Prompt-Aufbau mit injiziertem ChatTemplate.
//
// Vorher: hardcoded ChatML Konstanten (<|im_start|> etc.)
// Jetzt:  m_template.systemStart / userStart / etc. werden verwendet.
//
// Das Prinzip bleibt dasselbe wie beim AVR-UART-Treiber:
// Die Struktur der Übertragung (erst Header, dann Inhalt, dann Ende)
// ändert sich nicht — nur die konkreten Bytes (Token-Strings) kommen
// jetzt aus einer austauschbaren Konfiguration.

ChatModel::ChatModel(const QString &systemPrompt)
{
    if (!systemPrompt.isEmpty()) {
        m_messages.push_back({ChatMessage::Role::System, systemPrompt});
    }
}

void ChatModel::setSystemPrompt(const QString &prompt)
{
    // System-Prompt ist immer Index 0. Falls bereits vorhanden: ersetzen.
    // Falls nicht: vorne einfügen.
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
    // Tool-Ergebnisse: Format hängt von toolRole im Template ab.
    // Wenn toolRole == "tool" → echte tool-Rolle (Llama3)
    // Wenn toolRole == "user" oder leer → als user-Nachricht (ChatML, Mistral)
    // ChatModel speichert es immer als Role::Tool — formatMessage() entscheidet
    // wie es gerendert wird.
    QString content = QString("[Tool: %1]\n%2").arg(toolName, result);
    m_messages.push_back({ChatMessage::Role::Tool, content});
}

// ─── formatMessage ───────────────────────────────────────────────────────────
// Wandelt eine ChatMessage in einen Template-formatierten String um.
//
// Pattern: Strategy — das konkrete Format kommt aus m_template (Strategy-Objekt),
// diese Methode ist der Kontext der die Strategy anwendet.
//
// Gemma-Sonderfall: Gemma hat keine eigene system-Rolle.
// systemStart ist identisch mit userStart. Das ist korrekt — Gemma erwartet
// den System-Prompt als erste user-Nachricht. Das Template-Struct bildet
// das already ab (gemma() setzt systemStart = userStart).
//
// Mistral-Sonderfall: assistantEnd ist "</s>\n" — Mistral schließt
// Assistant-Turns explizit. Das Modell generiert also bis <|im_end|> (ChatML)
// oder </s> (Mistral) — wir setzen es trotzdem, schadet nicht.
QString ChatModel::formatMessage(const ChatMessage &msg) const
{
    switch (msg.role) {
        case ChatMessage::Role::System:
            return m_template.systemStart + msg.content + m_template.systemEnd;

        case ChatMessage::Role::User:
            return m_template.userStart + msg.content + m_template.userEnd;

        case ChatMessage::Role::Assistant:
            return m_template.assistantStart + msg.content + m_template.assistantEnd;

        case ChatMessage::Role::Tool: {
            // Tool-Ergebnisse: Rolle hängt vom Template ab.
            // "tool"  → eigener tool-Header (Llama3)
            // "user"  → als user-Nachricht formatieren (ChatML, Gemma, Mistral)
            // ""      → Fallback: user
            const QString &role = m_template.toolRole;
            if (role == "tool") {
                // Llama3-Format: eigener tool-Header
                // <|start_header_id|>tool<|end_header_id|>\n\n...<|eot_id|>
                // Wir bauen das aus den user-Tags mit ersetzter Rolle —
                // das ist für Llama3 korrekt weil die Tags symmetrisch sind.
                QString toolStart = m_template.userStart;
                toolStart.replace("user", "tool");
                return toolStart + msg.content + m_template.userEnd;
            }
            // Default: als user-Nachricht
            return m_template.userStart + msg.content + m_template.userEnd;
        }
    }
    return {};
}

// ─── buildPrompt ─────────────────────────────────────────────────────────────
// Konkateniert alle Nachrichten + öffnet den Assistant-Turn.
// llama.cpp generiert ab dem letzten assistantStart.
//
// Analogie AVR: wir bauen das komplette Paket das gesendet wird —
// Header + Payload-Teile + offener Empfänger-Turn am Ende.
QString ChatModel::buildPrompt() const
{
    QString prompt;
    for (const auto &msg : m_messages) {
        prompt += formatMessage(msg);
    }
    // Assistant-Turn öffnen — llama.cpp füllt ab hier auf.
    // assistantEnd wird bewusst NICHT angehängt (Modell generiert bis EOS).
    prompt += m_template.assistantStart;
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
