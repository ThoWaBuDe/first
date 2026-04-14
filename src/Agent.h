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

// ─── AgentMode ────────────────────────────────────────────────────────────────
// Drei Modi des Agenten.
//
// Chat    — normaler Gesprächs-Modus (bisheriges Verhalten, unverändert)
// Plan    — Modell liest Projekt via Lese-Tools, gibt dann <plan>...</plan> aus
// Execute — Modell arbeitet TaskTree-Knoten ab (kommt in einer späteren Session)
//
// Analogie AVR: wie ein Zustandsautomat (FSM) mit drei Zuständen.
// Übergänge:
//   Chat  → Plan     via /plan <Auftrag>
//   Plan  → Chat     via Ablehnen-Button oder Fehler
//   Plan  → Execute  via Bestätigen-Button (noch nicht implementiert)
//   Execute → Chat   via Stop oder alle Tasks Done
enum class AgentMode {
    Chat,
    Plan,
    Execute,  // Vorbereitung — wird in nächster Session aktiviert
};

// ─── Agent ────────────────────────────────────────────────────────────────────
// Pattern: Presenter aus MVP.
//
// Neu in dieser Version:
//   - AgentMode: Chat / Plan / Execute
//   - Plan-Modus: Hybrid (Lese-Tools + <plan>...</plan> JSON)
//   - TaskTree: wird im Plan-Modus aufgebaut, persistiert in SQLite
//   - handlePlanToolCall(): wie handleToolCall() aber nur Lese-Tools erlaubt
//   - handlePlanJson(): parst <plan>...</plan>, baut m_taskTree auf
//   - buildPlannerSystemPrompt(): System-Prompt für Plan-Modus
//
// Was unverändert bleibt:
//   - startGeneration(), filterToken(), handleToolCall() — nicht angefasst
//   - Stop-Mechanismus (Session-ID)
//   - Kontext-Management (Summarize)
//   - Sampler-Umschaltung

class Agent : public QObject {
    Q_OBJECT

public:
    explicit Agent(const QString &modelPath, QObject *parent = nullptr);
    ~Agent() override;

    void start();
    LlamaWorker *worker() const { return m_worker; }

    // Zugriff für PlannerDock (read-only)
    const TaskTree &taskTree() const { return m_taskTree; }
    AgentMode mode() const           { return m_mode; }

public slots:
    void onUserMessage(const QString &text);
    void onStop();
    void onClearChat();
    void onFileSavedByUser(const QString &filePath);

    // Plan-Approval: wird von PlannerDock aufgerufen
    void onPlanApproved();
    void onPlanRejected();

signals:
    void appendChat(const QString &html, const QString &cssClass);
    void appendChatToken(const QString &text);
    void appendTools(const QString &html, const QString &cssClass);
    void statusChanged(const QString &text);
    void inputEnabled(bool enabled);
    void statsUpdated(int promptTokens, int generatedTokens,
                      int totalTokens,  int ctxSize);

    // Plan-Modus Signale → PlannerDock
    // planReady: Modell hat <plan>...</plan> geliefert, Tree ist aufgebaut.
    //            PlannerDock zeigt Tree + Bestätigen/Ablehnen Buttons.
    void planReady();

    // modeChanged: informiert MainWindow wenn Modus wechselt
    // (z.B. für Status-Anzeige, Button-Enable/Disable)
    void modeChanged(AgentMode mode);

    // taskTreeUpdated: ein Knoten-Status hat sich geändert → PlannerDock refresh
    void taskTreeUpdated();

private slots:
    void onTokenReceived(const QString &token);
    void onGenerationDone(const QString &fullResponse);
    void onModelLoaded();
    void onStatsUpdate(int promptTokens, int ctxSize);
    void onError(const QString &error);
    void onChatTemplateDetected(const QString &jinjaTemplate,
                                ChatTemplate::Preset detectedPreset);

private:
    void startGeneration(LlamaWorker::SamplerProfile profile);

    // ─── Chat-Modus (unverändert) ──────────────────────────────────────────
    void handleToolCall(const QString &fullResponse, uint32_t sessionId);
    void filterToken(const QString &token);
    void emitStats();
    void checkContextUsage();
    void summarizeContext();

    // ─── Plan-Modus (NEU) ─────────────────────────────────────────────────
    // startPlan(): richtet Plan-Modus ein + startet erste Generierung
    void startPlan(const QString &auftrag);

    // buildPlannerSystemPrompt(): System-Prompt für Plan-Modus.
    //   Enthält NUR Lese-Tools, kein write_file, kein str_replace etc.
    //   Erklärt dem Modell das <plan>...</plan> Format.
    QString buildPlannerSystemPrompt(const QString &auftrag) const;

    // handlePlanToolCall(): wie handleToolCall() aber mit Whitelist.
    //   Nur Lese-Tools erlaubt: read_file, list_dir, get_symbol,
    //   get_project_index, get_time, sys_info.
    //   Schreib-Tools → Fehler-Nachricht ans Modell (kein Absturz).
    void handlePlanToolCall(const QString &fullResponse, uint32_t sessionId);

    // handlePlanJson(): parst <plan>...</plan> Block.
    //   Baut m_taskTree aus dem JSON auf.
    //   Bei Fehler: JSON-Repair + 1x Retry (wie bei handleToolCall).
    //   Bei Erfolg: emit planReady().
    void handlePlanJson(const QString &fullResponse, uint32_t sessionId);

    // parsePlanNode(): rekursiver JSON-Parser für einen Knoten + seine Kinder.
    //   Gibt Anzahl der erstellten Knoten zurück (für Fehlerdiagnose).
    int parsePlanNode(const QJsonObject &obj, TaskNode *parent, int depth);

    // ─── Hilfsmethoden ────────────────────────────────────────────────────
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

    // ─── TaskTree (NEU) ───────────────────────────────────────────────────
    // RAM-first — DB-Pfad kommt aus AppConfig::taskDbPath().
    // Wird in startPlan() initialisiert (DB-Pfad gesetzt).
    TaskTree    m_taskTree;
    AgentMode   m_mode        = AgentMode::Chat;
    TaskNode   *m_currentNode = nullptr;   // aktuell bearbeiteter Knoten (Execute)

    // Plan-Retry: wie viele Male wurde der Plan-JSON-Repair versucht?
    // Max 1 Retry dann Abbruch (wie bei handleToolCall JSON-Repair).
    int m_planRetryCount = 0;
    static constexpr int MAX_PLAN_RETRIES = 1;

    // ─── Whitelist: erlaubte Tools im Plan-Modus ──────────────────────────
    // Schreib-Tools sind im Plan-Modus verboten — das Modell soll nur lesen.
    // Die Liste ist static const — wird einmal gebaut, nie verändert.
    // Analogie AVR: wie eine Lookup-Table im Flash (read-only, schnell).
    static const QStringList PLAN_ALLOWED_TOOLS;

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
