#pragma once
#include <QMainWindow>
#include <QThread>
#include "ChatModel.h"
#include "McpManager.h"

namespace Ui { class MainWindow; }

class LlamaWorker;

// ─── MainWindow ───────────────────────────────────────────────────────────────
// Haupt-UI. Koordiniert Worker-Thread, ChatModel und McpManager.
//
// UI-Struktur (MainWindow.ui):
//   centralWidget  → chatView (Chat), statusLabel, inputLine, Buttons
//   toolDock       → QDockWidget rechts: toolView (Tools+Thinking), statsLabel
//
// Thinking-Filter:
//   <think>...</think> Tokens werden NICHT in chatView angezeigt,
//   aber in m_currentResponse gespeichert (Modell braucht sie in der History).
//   Im toolView werden sie als einklappbarer Block dargestellt.
//
// Token-Statistik:
//   m_generatedTokens  — Tokens dieser Generierung
//   m_totalTokens      — Tokens gesamt seit Start
//   m_promptTokens     — Tokens des letzten Prompts (aus generate() Signal)
//   m_ctxSize          — n_ctx des Modells (aus modelLoaded Signal)
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onSendClicked();
    void onStopClicked();
    void onClearClicked();
    void onClearToolsClicked();

    void onTokenReceived(const QString &token);
    void onGenerationDone(const QString &fullResponse);
    void onModelLoaded();
    void onStatsUpdate(int promptTokens, int ctxSize);
    void onError(const QString &error);

private:
    void setupWorker();
    void sendMessage(const QString &userText);

    // Chat-View: normaler Antwort-Text
    void appendToChat(const QString &text, const QString &cssClass);

    // Tool-View: Tool-Calls, Thinking, System-Meldungen
    void appendToTools(const QString &text, const QString &cssClass);

    void setInputEnabled(bool enabled);
    void updateStats();

    // ─── UI ──────────────────────────────────────────────────────────────
    Ui::MainWindow *ui;

    // ─── Backend ─────────────────────────────────────────────────────────
    QThread        m_workerThread;
    LlamaWorker   *m_worker = nullptr;
    McpManager     m_mcp;
    ChatModel      m_chatModel;

    // ─── Generierungs-State ───────────────────────────────────────────────
    QString m_currentResponse;   // vollständige Antwort inkl. <think>-Blöcken
    QString m_thinkBuffer;       // aktueller Thinking-Block (zwischen Tags)
    bool    m_inThinkBlock = false;
    bool    m_generating   = false;

    int m_continuationCount = 0;
    static constexpr int MAX_CONTINUATIONS = 3;

    // ─── Token-Statistik ─────────────────────────────────────────────────
    int m_generatedTokens = 0;   // Tokens dieser Generierung
    int m_totalTokens     = 0;   // Tokens gesamt seit Modell-Start
    int m_promptTokens    = 0;   // Tokens des letzten Prompts
    int m_ctxSize         = 0;   // n_ctx des Modells

    static constexpr const char *MODEL_PATH =
        "/home/thomas/ai/models/Qwen3.5-9B-Q6_K.gguf";
};
