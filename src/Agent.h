#pragma once
#include <QObject>
#include <QThread>
#include <QHash>
#include <QJsonObject>
#include <cstdint>
#include "ChatModel.h"
#include "McpManager.h"
#include "LlamaWorker.h"
#include "CommandProcessor.h"
#include "ChatLogger.h"
#include "AppConfig.h"
#include "ChatTemplate.h"

// ─── Agent ────────────────────────────────────────────────────────────────────
// Pattern: Presenter aus MVP.
//
// Umbau: startGeneration() übergibt nicht mehr einen fertigen Prompt-String
// an den Worker, sondern QVector<ChatMessage> — der Worker baut den Prompt
// intern via llama_chat_apply_template().
//
// Was sich geändert hat:
//   - startGeneration() ruft invokeMethod mit Q_ARG(QVector<ChatMessage>)
//   - ChatModel::buildPrompt() wird nicht mehr aufgerufen
//   - applyChatTemplate() / buildFullSystemPrompt() bleiben für ConfigDialog
//     und für den Fallback im Worker
//
// Was unverändert bleibt:
//   - Sampler-Umschaltung (Chat/Tool)
//   - Tool-Loop, Deadlock-Erkennung, JSON-Repair
//   - Stop-Mechanismus (Session-ID)
//   - Kontext-Management (Summarize)
//   - Chat-Template Detection + ConfigDialog-Anzeige

class Agent : public QObject {
    Q_OBJECT

public:
    explicit Agent(const QString &modelPath, QObject *parent = nullptr);
    ~Agent() override;

    void start();
    LlamaWorker *worker() const { return m_worker; }

public slots:
    void onUserMessage(const QString &text);
    void onStop();
    void onClearChat();
    void onFileSavedByUser(const QString &filePath);

signals:
    void appendChat(const QString &html, const QString &cssClass);
    void appendChatToken(const QString &text);
    void appendTools(const QString &html, const QString &cssClass);
    void statusChanged(const QString &text);
    void inputEnabled(bool enabled);
    void statsUpdated(int promptTokens, int generatedTokens,
                      int totalTokens,  int ctxSize);

private slots:
    void onTokenReceived(const QString &token);
    void onGenerationDone(const QString &fullResponse);
    void onModelLoaded();
    void onStatsUpdate(int promptTokens, int ctxSize);
    void onError(const QString &error);
    void onChatTemplateDetected(const QString &jinjaTemplate,
                                ChatTemplate::Preset detectedPreset);

private:
    // Übergibt m_chatModel.messages() an den Worker via invokeMethod.
    // profile bestimmt ob Chat- oder Tool-Sampler aktiv ist.
    void startGeneration(LlamaWorker::SamplerProfile profile);

    void handleToolCall(const QString &fullResponse, uint32_t sessionId);
    void filterToken(const QString &token);
    void emitStats();
    void checkContextUsage();
    void summarizeContext();

    QString computeDiffHtml(const QString &before, const QString &after,
                            const QString &filename) const;
    QString toolCallKey(const QString &toolName, const QJsonObject &args) const;
    QString deadlockEscalationPrompt(const QString &toolName, int count) const;
    QString repairJson(const QString &broken) const;

    QString buildFullSystemPrompt() const;
    void    applyChatTemplate();

    // ─── Owned Objects ────────────────────────────────────────────────────
    QString           m_modelPath;
    ChatModel         m_chatModel;
    McpManager        m_mcp;
    CommandProcessor  m_commands;
    ChatLogger        m_logger;
    QThread           m_workerThread;
    LlamaWorker      *m_worker = nullptr;

    uint32_t m_sessionId = 0;

    bool    m_generating        = false;
    QString m_currentResponse;
    QString m_thinkBuffer;
    bool    m_inThinkBlock      = false;
    int     m_continuationCount = 0;
    static constexpr int MAX_CONTINUATIONS = 3;
    static constexpr int MAX_TOOL_RESULT_CHARS = 6000;

    QHash<QString, int> m_toolFailCount;
    static constexpr int DEADLOCK_WARN     = 3;
    static constexpr int DEADLOCK_REDIRECT = 5;
    static constexpr int DEADLOCK_ABORT    = 7;

    bool m_summarizing = false;
    static constexpr int CTX_SUMMARIZE_THRESHOLD = 80;

    int m_generatedTokens = 0;
    int m_totalTokens     = 0;
    int m_promptTokens    = 0;
    int m_ctxSize         = 0;

    ChatTemplate::Preset m_detectedPreset = ChatTemplate::Preset::ChatML;
};
