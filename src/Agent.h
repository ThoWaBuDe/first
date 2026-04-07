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

// ─── Agent ────────────────────────────────────────────────────────────────────
// Pattern: Presenter aus MVP (Model-View-Presenter).
//
// Der Agent ist der Dirigent der gesamten Anwendungslogik:
//   - Er besitzt ChatModel, McpManager, LlamaWorker, CommandProcessor
//   - Er steuert den kompletten Agenten-Loop:
//       User-Input → Kommando-Check → Generierung → Tool-Call-Erkennung →
//       MCP-Dispatch → Ergebnis → nächste Generierung
//   - Er kommuniziert mit dem MainWindow NUR über Signals (nach oben)
//     und Slots (nach unten, von der UI)
//
// ─── Stop-Mechanismus ────────────────────────────────────────────────────────
// Problem: m_generating = false reicht nicht — laufende MCP-Callbacks und
// eingereihte invokeMethod-Aufrufe laufen danach noch durch.
//
// Lösung: m_sessionId (uint32, monoton steigend).
// Jede Generierungs-Session bekommt eine eindeutige ID.
// Alle Callbacks und Continuations prüfen ob ihre gespeicherte ID noch der
// aktuellen entspricht. Falls nicht → verwerfen (Stale Callback).
//
// Pattern: Generation Stamp / Token (ähnlich wie Cancel-Token in .NET).
// Analogie AVR: wie ein "Sequence Number" bei UART-Protokollen —
// veraltete Pakete werden anhand der Sequenznummer verworfen.
//
// ─── Tool-Deadlock-Erkennung ─────────────────────────────────────────────────
// Erkennt wenn dasselbe Tool wiederholt mit demselben Fehler scheitert.
// Eskalation: 3x → Warnung, 5x → andere Lösung, 7x → Abbruch.
// Schlüssel: "toolname|arg1=val1|arg2=val2" (kanonische Argumentdarstellung).

class Agent : public QObject {
    Q_OBJECT

public:
    explicit Agent(const QString &modelPath, QObject *parent = nullptr);
    ~Agent() override;

    void start();

    // Zugriff auf Worker für MainWindow (rebuildSamplers via invokeMethod)
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

private:
    void startGeneration(LlamaWorker::SamplerProfile profile);
    void handleToolCall(const QString &fullResponse, uint32_t sessionId);
    void filterToken(const QString &token);
    void emitStats();

    // ─── Kontext-Management ───────────────────────────────────────────────
    void checkContextUsage();   // >80% → auto-summarize
    void summarizeContext();     // fasst Konversation zusammen, kürzt ChatModel

    // ─── Diff-Ansicht (LCS) ───────────────────────────────────────────────
    QString computeDiffHtml(const QString &before, const QString &after,
                            const QString &filename) const;

    // Deadlock-Erkennung:
    // Kanonischer Schlüssel aus Tool-Name + Argumenten (sortiert)
    QString toolCallKey(const QString &toolName, const QJsonObject &args) const;
    // Eskalations-Prompt abhängig von Fehlerzähler
    QString deadlockEscalationPrompt(const QString &toolName, int count) const;

    // JSON-Repair: versucht kaputtes JSON zu reparieren
    // Gibt reparierten String zurück oder QString() bei Misserfolg
    QString repairJson(const QString &broken) const;

    // ─── Owned Objects ────────────────────────────────────────────────────
    QString           m_modelPath;
    ChatModel         m_chatModel;
    McpManager        m_mcp;
    CommandProcessor  m_commands;
    ChatLogger        m_logger;
    QThread           m_workerThread;
    LlamaWorker      *m_worker = nullptr;

    // ─── Session-ID (Stop-Mechanismus) ───────────────────────────────────
    // Wird bei jedem onStop() und onClearChat() inkrementiert.
    // Callbacks speichern ihre ID bei Erstellung — veraltet = verwerfen.
    uint32_t m_sessionId = 0;

    // ─── Generierungs-State ───────────────────────────────────────────────
    bool    m_generating        = false;
    QString m_currentResponse;
    QString m_thinkBuffer;
    bool    m_inThinkBlock      = false;
    int     m_continuationCount = 0;
    static constexpr int MAX_CONTINUATIONS = 3;

    // ─── Token-Budget ─────────────────────────────────────────────────────
    // Maximale Länge eines Tool-Ergebnisses bevor es gekürzt wird.
    // 6000 Zeichen ≈ ~1500 Tokens — großzügig aber nicht kontextflutend.
    static constexpr int MAX_TOOL_RESULT_CHARS = 6000;

    // ─── Deadlock-Tracking ────────────────────────────────────────────────
    // Zählt aufeinanderfolgende Fehler pro Tool-Call-Schlüssel.
    // Wird bei jedem erfolgreichen Tool-Call geleert.
    QHash<QString, int> m_toolFailCount;

    static constexpr int DEADLOCK_WARN     = 3;
    static constexpr int DEADLOCK_REDIRECT = 5;
    static constexpr int DEADLOCK_ABORT    = 7;

    // ─── Kontext-Management ───────────────────────────────────────────────
    bool m_summarizing = false;
    static constexpr int CTX_SUMMARIZE_THRESHOLD = 80;  // % Auslastung

    // ─── Token-Statistik ──────────────────────────────────────────────────
    int m_generatedTokens = 0;
    int m_totalTokens     = 0;
    int m_promptTokens    = 0;
    int m_ctxSize         = 0;
};
