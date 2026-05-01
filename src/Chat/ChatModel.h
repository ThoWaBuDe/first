#pragma once
#include <QString>
#include <QVector>
#include <QMetaType>
#include "ChatTemplate.h"

// ─── ChatMessage ─────────────────────────────────────────────────────────────
struct ChatMessage {
    enum class Role { System, User, Assistant, Tool };
    Role    role;
    QString content;
};

// QVector<ChatMessage> muss als Qt-Metatyp registriert sein damit
// Agent::startGeneration() es per invokeMethod() über die Thread-Grenze
// schicken kann. Qt macht dabei eine tiefe Kopie — thread-sicher.
// Q_DECLARE_METATYPE registriert den Typ zur Compile-Zeit,
// qRegisterMetaType() (in Agent.cpp) macht ihn zur Laufzeit bekannt.
Q_DECLARE_METATYPE(QVector<ChatMessage>)

// ─── ChatModel ───────────────────────────────────────────────────────────────
// Hält die Gesprächshistorie. buildPrompt() wird nur noch als Fallback
// in LlamaWorker::applyTemplate() genutzt wenn llama_chat_apply_template()
// fehlschlägt.
class ChatModel {
public:
    explicit ChatModel(const QString &systemPrompt = {});

    void setChatTemplate(const ChatTemplate &tmpl) { m_template = tmpl; }
    const ChatTemplate &chatTemplate() const { return m_template; }

    void setSystemPrompt(const QString &prompt);
    void addUserMessage(const QString &text);
    void addAssistantMessage(const QString &text);
    void addToolResult(const QString &toolName, const QString &result);

    // Primär: wird von LlamaWorker::applyTemplate() als Fallback genutzt.
    // Baut den Prompt mit dem gesetzten ChatTemplate-Struct.
    QString buildPrompt() const;

    const QVector<ChatMessage> &messages() const { return m_messages; }

    void clear();

private:
    QVector<ChatMessage> m_messages;
    ChatTemplate         m_template = ChatTemplate::chatML();

    QString formatMessage(const ChatMessage &msg) const;
};
