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
#include "TaskTree.h"
#include "ExecuteMemory.h"
#include "CodeAssembler.h"

enum class AgentMode { Chat, Plan, Execute };

class Agent : public QObject {
    Q_OBJECT
public:
    explicit Agent(const QString &modelPath, QObject *parent = nullptr);
    ~Agent() override;
    void start();
    LlamaWorker *worker() const { return m_worker; }
    const TaskTree      &taskTree()      const { return m_taskTree; }
    const ExecuteMemory &executeMemory() const { return m_executeMemory; }
    AgentMode mode() const { return m_mode; }

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
    void statsUpdated(int promptTokens, int generatedTokens, int totalTokens, int ctxSize);
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
    void onChatTemplateDetected(const QString &jinjaTemplate, ChatTemplate::Preset detectedPreset);

private:
    void startGeneration(LlamaWorker::SamplerProfile profile);

    // Chat-Modus
    void handleToolCall(const QString &fullResponse, uint32_t sessionId);
    void filterToken(const QString &token);
    void emitStats();
    void checkContextUsage();
    void summarizeContext();

    // Plan-Modus
    void startPlan(const QString &auftrag);
    QString buildPlannerSystemPrompt(const QString &auftrag) const;
    void handlePlanToolCall(const QString &fullResponse, uint32_t sessionId);
    void handlePlanJson(const QString &fullResponse, uint32_t sessionId);
    int  parsePlanNode(const QJsonObject &obj, TaskNode *parent, int depth,
                       QHash<QString, qint64> &titleToId,
                       QHash<qint64, QStringList> &pendingDeps);

    // Execute-Modus
    void startExecute();
    bool advanceExecute();
    QString buildExecuteSystemPrompt() const;
    QString buildExecutePrompt(const TaskNode *node) const;
    void handleExecuteCode(const QString &fullResponse, uint32_t sessionId);
    void handleExecuteToolCall(const QString &fullResponse, uint32_t sessionId);
    void updateThoughts(const TaskNode *node, uint32_t sessionId);
    bool isExecuteToolCall(const QString &response) const;

    void assembleProject();

    // Hilfsmethoden
    QString computeDiffHtml(const QString &before, const QString &after, const QString &filename) const;
    QString toolCallKey(const QString &toolName, const QJsonObject &args) const;
    QString deadlockEscalationPrompt(const QString &toolName, int count) const;
    QString repairJson(const QString &broken) const;
    QString buildFullSystemPrompt() const;
    void    applyChatTemplate();

    // Member
    QString           m_modelPath;
    ChatModel         m_chatModel;
    McpManager        m_mcp;
    CommandProcessor  m_commands;
    ChatLogger        m_logger;
    QThread           m_workerThread;
    LlamaWorker      *m_worker = nullptr;

    TaskTree      m_taskTree;
    ExecuteMemory m_executeMemory;
    AgentMode     m_mode        = AgentMode::Chat;
    TaskNode     *m_currentNode = nullptr;
    bool          m_updatingThoughts = false;

    int m_planRetryCount = 0;
    static constexpr int MAX_PLAN_RETRIES = 1;

    static const QStringList PLAN_ALLOWED_TOOLS;
    static const QStringList EXECUTE_ALLOWED_TOOLS;

    uint32_t m_sessionId        = 0;
    bool     m_generating       = false;
    QString  m_currentResponse;
    QString  m_thinkBuffer;
    bool     m_inThinkBlock     = false;
    int      m_continuationCount = 0;
    static constexpr int MAX_CONTINUATIONS    = 3;
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

    CodeAssembler m_assembler;

    ChatTemplate::Preset m_detectedPreset = ChatTemplate::Preset::ChatML;
};
