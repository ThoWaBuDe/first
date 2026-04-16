#pragma once
// ─── Agent ───────────────────────────────────────────────────────────────────
// Herzstück — Koordination und Zustandshaltung.
// Delegiert Fachlogik an Teilklassen:
//
//   AgentChat    — Chat-Modus (Tool-Calls, filterToken, summarize)
//   AgentPlan    — Plan-Modus (startPlan, handlePlanJson, parsePlanNode)
//   AgentExecute — Execute-Modus (advanceExecute, buildExecutePrompt)
//   AgentAssemble— Assembly (Nodes → Dateien)
//   AgentUtils   — Statische Hilfsfunktionen (repairJson, toolCallKey, ...)
//
// Pattern: Komposition + friend.
//   Die Teilklassen erhalten im Konstruktor eine Referenz auf Agent
//   und greifen über friend auf dessen private Member zu.
//   Agent selbst hat keinerlei Implementierungslogik mehr — nur Koordination.
//
// Threading:
//   Agent lebt im GUI-Thread.
//   LlamaWorker lebt im Worker-Thread.
//   Kommunikation: Qt Signals/Slots mit QueuedConnection.
//   Generation Stamp (m_sessionId) verhindert veraltete Callbacks.

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
#include "TaskTree.h"
#include "ExecuteMemory.h"
#include "CodeAssembler.h"

// Forward-Declarations der Teilklassen
class AgentChat;
class AgentPlan;
class AgentExecute;
class AgentAssemble;

enum class AgentMode { Chat, Plan, Execute };

class Agent : public QObject {
    Q_OBJECT

    // Teilklassen als friends — sie greifen direkt auf private Member zu.
    // Das ist bewusst gewählt: Kapselung gegenüber dem Rest der Welt
    // bleibt erhalten, aber interne Logik kann ausgelagert werden.
    friend class AgentChat;
    friend class AgentPlan;
    friend class AgentExecute;
    friend class AgentAssemble;

public:
    explicit Agent(const QString &modelPath, QObject *parent = nullptr);
    ~Agent() override;

    void start();

    LlamaWorker *worker() const { return m_worker; }
    const TaskTree      &taskTree()      const { return m_taskTree; }
    const ExecuteMemory &executeMemory() const { return m_executeMemory; }
    AgentMode mode() const { return m_mode; }

    // Whitelists (public damit Teilklassen sie ohne friend nutzen können)
    static const QStringList PLAN_ALLOWED_TOOLS;
    static const QStringList EXECUTE_ALLOWED_TOOLS;

    // Konstanten (public für Teilklassen)
    static constexpr int MAX_CONTINUATIONS = 3;
    static constexpr int MAX_PLAN_RETRIES  = 1;

public slots:
    void onUserMessage(const QString &text);
    void onStop();
    void onClearChat();
    void onFileSavedByUser(const QString &filePath);
    void onPlanApproved();
    void onPlanRejected();

signals:
    void appendChat(const QString &html, const QString &cssClass);
    void appendChatToken(const QString &text);
    void appendTools(const QString &html, const QString &cssClass);
    void statusChanged(const QString &text);
    void inputEnabled(bool enabled);
    void statsUpdated(int promptTokens, int generatedTokens,
                      int totalTokens, int ctxSize);
    void planReady();
    void modeChanged(AgentMode mode);
    void taskTreeUpdated();
    void executeNodeStarted(qint64 nodeId);
    void executeNodeDone(qint64 nodeId);
    void executeFinished();

private slots:
    void onTokenReceived(const QString &token);
    void onGenerationDone(const QString &fullResponse);
    void onModelLoaded();
    void onStatsUpdate(int promptTokens, int ctxSize);
    void onError(const QString &error);
    void onChatTemplateDetected(const QString &jinjaTemplate,
                                 ChatTemplate::Preset detectedPreset);

private:
    // ── Interne Hilfsmethoden (genutzt von Teilklassen) ──────────────────
    void startGeneration(LlamaWorker::SamplerProfile profile);
    QString buildFullSystemPrompt() const;
    void    applyChatTemplate();

    // ── Private Member (zugänglich für friend-Klassen) ────────────────────
    QString           m_modelPath;
    ChatModel         m_chatModel;
    McpManager        m_mcp;
    CommandProcessor  m_commands;
    ChatLogger        m_logger;
    QThread           m_workerThread;
    LlamaWorker      *m_worker = nullptr;

    TaskTree      m_taskTree;
    ExecuteMemory m_executeMemory;
    CodeAssembler m_assembler;

    AgentMode     m_mode        = AgentMode::Chat;
    TaskNode     *m_currentNode = nullptr;
    bool          m_updatingThoughts = false;

    // Teilklassen (Aggregation — Agent besitzt sie)
    AgentChat    *m_chat     = nullptr;
    AgentPlan    *m_plan     = nullptr;
    AgentExecute *m_execute  = nullptr;
    AgentAssemble*m_assemble = nullptr;

    // Generierungs-Zustand
    uint32_t m_sessionId         = 0;
    bool     m_generating        = false;
    QString  m_currentResponse;
    QString  m_thinkBuffer;
    bool     m_inThinkBlock      = false;
    int      m_continuationCount = 0;

    // Plan-Zustand
    int m_planRetryCount = 0;

    // Deadlock-Erkennung
    QHash<QString, int> m_toolFailCount;

    // Kontext-Management
    bool m_summarizing = false;

    // Token-Statistiken
    int m_generatedTokens = 0;
    int m_totalTokens     = 0;
    int m_promptTokens    = 0;
    int m_ctxSize         = 0;

    // Chat-Template
    ChatTemplate::Preset m_detectedPreset = ChatTemplate::Preset::ChatML;
};
