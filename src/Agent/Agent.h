#pragma once
#include <QObject>
#include <QThread>
#include <QHash>
#include <QJsonObject>
#include <cstdint>
#include "Chat/ChatModel.h"
#include "MCP/McpManager.h"
#include "AI/LlamaWorker.h"
#include "Task/CommandProcessor.h"
#include "Chat/ChatLogger.h"
#include "Config/AppConfig.h"
#include "Chat/ChatTemplate.h"
#include "Chat/ToolCallFormat.h"
#include "Task/TaskTree.h"
#include "AI/ExecuteMemory.h"
#include "Task/CodeAssembler.h"

class AgentChat;
class AgentPlan;
class AgentExecute;
class AgentAssemble;
class AgentImport;   // NEU

enum class AgentMode { Chat, Plan, Execute };

class Agent : public QObject {
    Q_OBJECT

    friend class AgentChat;
    friend class AgentPlan;
    friend class AgentExecute;
    friend class AgentAssemble;
    friend class AgentImport;   // NEU

public:
    explicit Agent(const QString &modelPath, QObject *parent = nullptr);
    ~Agent() override;

    void start();

    LlamaWorker *worker() const { return m_worker; }
    const TaskTree      &taskTree()      const { return m_taskTree; }
    const ExecuteMemory &executeMemory() const { return m_executeMemory; }
    AgentMode mode() const { return m_mode; }

    ToolCallFormat::Preset activeToolFormat() const { return m_activeToolFormat; }

    // NEU: Zugriff auf Import-Agent für onGenerationDone()
    AgentImport *importAgent() const { return m_import; }

    static const QStringList PLAN_ALLOWED_TOOLS;
    static const QStringList EXECUTE_ALLOWED_TOOLS;

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
    void onToolFormatDetected(ToolCallFormat::Preset detectedFormat,
                               const QString &source);

private:
    void startGeneration(LlamaWorker::SamplerProfile profile);
    QString buildFullSystemPrompt() const;
    void    applyChatTemplate();

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

    AgentChat    *m_chat     = nullptr;
    AgentPlan    *m_plan     = nullptr;
    AgentExecute *m_execute  = nullptr;
    AgentAssemble*m_assemble = nullptr;
    AgentImport  *m_import   = nullptr;  // NEU

    ToolCallFormat::Preset m_activeToolFormat = ToolCallFormat::Preset::QwenXmlTags;

    QVector<ParsedToolCall> m_pendingToolCalls;
    int                     m_pendingToolIdx = 0;

    uint32_t m_sessionId         = 0;
    bool     m_generating        = false;
    QString  m_currentResponse;
    QString  m_thinkBuffer;
    bool     m_inThinkBlock      = false;
    int      m_continuationCount = 0;

    int m_planRetryCount = 0;

    QHash<QString, int> m_toolFailCount;

    bool m_summarizing = false;

    int m_generatedTokens = 0;
    int m_totalTokens     = 0;
    int m_promptTokens    = 0;
    int m_ctxSize         = 0;

    ChatTemplate::Preset m_detectedPreset = ChatTemplate::Preset::ChatML;
};
