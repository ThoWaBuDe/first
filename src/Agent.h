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
// Neu: Chat-Template und User-System-Prompt Integration.
//
//   buildFullSystemPrompt()  — kombiniert User-Text + MCP-Tools
//   applyChatTemplate()      — injiziert das richtige Template in ChatModel
//   onChatTemplateDetected() — Slot für LlamaWorker::chatTemplateDetected()
//
// Reihenfolge beim Start:
//   1. MCP-Server starten (async)
//   2. LlamaWorker::initialize()
//   3. chatTemplateDetected() → applyChatTemplate()
//   4. modelLoaded() → buildFullSystemPrompt() → ChatModel::setSystemPrompt()

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

    // ─── Neu: Chat-Template vom Worker ───────────────────────────────────
    // Empfängt das erkannte Template direkt vor modelLoaded().
    // Setzt m_detectedPreset und ruft applyChatTemplate() auf.
    void onChatTemplateDetected(const QString &jinjaTemplate,
                                ChatTemplate::Preset detectedPreset);

private:
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

    // ─── Neu: System-Prompt + Template ───────────────────────────────────

    // Baut den vollständigen System-Prompt:
    //   1. User-Text aus AppConfig::userSystemPrompt()  (leer = weggelassen)
    //   2. MCP Tool-Beschreibungen aus McpManager
    // Reihenfolge: User-Text zuerst — Modelle gewichten Prompt-Anfang stärker.
    QString buildFullSystemPrompt() const;

    // Liest das gewünschte Template aus AppConfig und injiziert es in ChatModel.
    // Bei Preset::Auto wird m_detectedPreset verwendet.
    // Bei Preset::Custom: Fallback ChatML bis Custom-Parsing implementiert ist.
    //
    // TODO Custom-Template: AppConfig::customChatTemplate() enthält einen
    // JSON-String mit den Format-Feldern. Dieser muss geparst und in ein
    // ChatTemplate-Struct umgewandelt werden. Aktuell Fallback auf ChatML.
    void applyChatTemplate();

    // ─── Owned Objects ────────────────────────────────────────────────────
    QString           m_modelPath;
    ChatModel         m_chatModel;
    McpManager        m_mcp;
    CommandProcessor  m_commands;
    ChatLogger        m_logger;
    QThread           m_workerThread;
    LlamaWorker      *m_worker = nullptr;

    // ─── Session-ID (Stop-Mechanismus) ───────────────────────────────────
    uint32_t m_sessionId = 0;

    // ─── Generierungs-State ───────────────────────────────────────────────
    bool    m_generating        = false;
    QString m_currentResponse;
    QString m_thinkBuffer;
    bool    m_inThinkBlock      = false;
    int     m_continuationCount = 0;
    static constexpr int MAX_CONTINUATIONS = 3;

    static constexpr int MAX_TOOL_RESULT_CHARS = 6000;

    // ─── Deadlock-Tracking ────────────────────────────────────────────────
    QHash<QString, int> m_toolFailCount;

    static constexpr int DEADLOCK_WARN     = 3;
    static constexpr int DEADLOCK_REDIRECT = 5;
    static constexpr int DEADLOCK_ABORT    = 7;

    // ─── Kontext-Management ───────────────────────────────────────────────
    bool m_summarizing = false;
    static constexpr int CTX_SUMMARIZE_THRESHOLD = 80;

    // ─── Token-Statistik ──────────────────────────────────────────────────
    int m_generatedTokens = 0;
    int m_totalTokens     = 0;
    int m_promptTokens    = 0;
    int m_ctxSize         = 0;

    // ─── Neu: Chat-Template State ─────────────────────────────────────────
    // Zuletzt vom GGUF erkanntes Preset (für Preset::Auto-Modus).
    // Wird in onChatTemplateDetected() gesetzt bevor modelLoaded() kommt.
    // Default ChatML — sicherer Fallback falls kein Modell geladen ist.
    ChatTemplate::Preset m_detectedPreset = ChatTemplate::Preset::ChatML;
};
