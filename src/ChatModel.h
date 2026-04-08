#pragma once
#include <QString>
#include <QVector>
#include "ChatTemplate.h"

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
// Prompt-String zusammen.
//
// Pattern: Model aus MVC — reines Datenmodell, keine UI-Abhängigkeiten.
//
// Neu: ChatTemplate-Injektion.
// ChatModel kennt jetzt das Chat-Template und formatiert jede Rolle damit.
// Das Template kommt von außen (von Agent) — ChatModel selbst trifft keine
// Entscheidung welches Template "richtig" ist.
//
// Pattern: Dependency Injection — ChatModel bekommt seine Abhängigkeit
// (ChatTemplate) von außen übergeben statt sie selbst zu erzeugen.
// Das macht ChatModel testbar ohne echte AppConfig oder llama.cpp.
//
// Analogie AVR: wie ein UART-Treiber der sein Baud-Rate-Register von
// außen bekommt statt es hardzukodieren.
class ChatModel {
public:
    explicit ChatModel(const QString &systemPrompt = {});

    // ─── Template-Injektion ───────────────────────────────────────────────
    // Setzt das Chat-Template das buildPrompt() für die Formatierung nutzt.
    // Default: ChatML (kompatibel mit bisherigem Verhalten).
    // Wird von Agent::start() nach dem MCP-Handshake gesetzt.
    void setChatTemplate(const ChatTemplate &tmpl) { m_template = tmpl; }
    const ChatTemplate &chatTemplate() const { return m_template; }

    // System-Prompt nachtraeglich setzen (nach MCP-Server-Start)
    void setSystemPrompt(const QString &prompt);

    void addUserMessage(const QString &text);
    void addAssistantMessage(const QString &text);
    void addToolResult(const QString &toolName, const QString &result);

    // Gibt den vollständigen Prompt für llama.cpp zurück.
    // Format: abhängig vom gesetzten ChatTemplate.
    // Öffnet am Ende den Assistant-Turn damit llama.cpp dort weiterschreibt.
    QString buildPrompt() const;

    // Zugriff auf History (für UI-Darstellung)
    const QVector<ChatMessage> &messages() const { return m_messages; }

    void clear();

private:
    QVector<ChatMessage> m_messages;
    ChatTemplate         m_template = ChatTemplate::chatML();  // sicherer Default

    // Hilfsfunktion: eine einzelne Nachricht mit dem aktuellen Template formatieren.
    // Pattern: Template Method — das konkrete Format kommt aus m_template,
    // die Struktur (welche Rolle bekommt welche Tags) bleibt hier.
    QString formatMessage(const ChatMessage &msg) const;
};
