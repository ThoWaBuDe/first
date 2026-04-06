#pragma once
#include <QObject>
#include <QThread>
#include "ChatModel.h"
#include "McpManager.h"
#include "LlamaWorker.h"

// ─── Agent ────────────────────────────────────────────────────────────────────
// Pattern: Presenter aus MVP (Model-View-Presenter).
//
// Der Agent ist der Dirigent der gesamten Anwendungslogik:
//   - Er besitzt ChatModel, McpManager und LlamaWorker (Aggregation)
//   - Er steuert den kompletten Agentenloop:
//       User-Input → Generierung → Tool-Call-Erkennung →
//       MCP-Dispatch → Ergebnis → nächste Generierung
//   - Er kommuniziert mit dem MainWindow NUR über Signals (nach oben)
//     und Slots (nach unten, von der UI)
//   - Er weiß NICHTS von Qt-Widgets, QTextEdit, QLabel etc.
//
// Threading:
//   - Agent selbst läuft im GUI-Thread (er macht keine blockierenden Ops)
//   - LlamaWorker läuft in m_workerThread (Active Object Pattern)
//   - MCP-Callbacks kommen per Qt-Event-Loop im GUI-Thread an

class Agent : public QObject {
    Q_OBJECT

public:
    explicit Agent(const QString &modelPath, QObject *parent = nullptr);
    ~Agent() override;

    // Startet MCP-Server und lädt dann das Modell.
    // Einmalig nach Konstruktion aufrufen.
    void start();

// ─── Slots: Eingaben von der UI ───────────────────────────────────────────────
public slots:
    void onUserMessage(const QString &text);
    void onStop();
    void onClearChat();

// ─── Signals: Ausgaben an die UI ──────────────────────────────────────────────
signals:
    // ── Chat-View: ZWEI verschiedene Signale für Streaming ────────────────
    //
    // appendChat:
    //   Öffnet einen neuen <p>-Block in chatView.
    //   Für strukturierte Einträge: "Du: ...", "Assistent:", Systemmeldungen.
    //   Implementierung in MainWindow: QTextEdit::append() → neuer Absatz.
    //
    // appendChatToken:
    //   Hängt Text an den LAUFENDEN Block an — KEIN neuer Absatz.
    //   Wird Token für Token während der Generierung emittiert.
    //   Implementierung in MainWindow: QTextCursor::insertText() am Ende.
    //
    // Warum zwei Signale?
    //   QTextEdit::append() erzeugt immer einen neuen <p>-Block (Absatz).
    //   Jeder Token einzeln per append() würde jeden Token in eine eigene
    //   Zeile setzen. QTextCursor::insertText() schreibt in den laufenden
    //   Absatz — genau das brauchen wir für Streaming.
    //   Analogie: append() = println(), insertText() = print() ohne \n.
    void appendChat(const QString &html, const QString &cssClass);
    void appendChatToken(const QString &text);

    // Tool-/Thinking-Ansicht (kein Streaming, immer komplette Blöcke)
    // CSS-Klassen: "tool", "think", "system", "error", "stats"
    void appendTools(const QString &html, const QString &cssClass);

    void statusChanged(const QString &text);
    void inputEnabled(bool enabled);

    // Rohdaten für die Statistik-Anzeige — View entscheidet Formatierung
    void statsUpdated(int promptTokens, int generatedTokens,
                      int totalTokens,  int ctxSize);

// ─── Private Slots: Worker-Callbacks ─────────────────────────────────────────
private slots:
    void onTokenReceived(const QString &token);
    void onGenerationDone(const QString &fullResponse);
    void onModelLoaded();
    void onStatsUpdate(int promptTokens, int ctxSize);
    void onError(const QString &error);

private:
    void startGeneration(LlamaWorker::SamplerProfile profile);
    void handleToolCall(const QString &fullResponse);

    // Thinking-Filter: State-Machine für <think>...</think>
    // Sichtbare Tokens  → emit appendChatToken()
    // Thinking-Tokens   → emit appendTools(..., "think")
    void filterToken(const QString &token);

    void emitStats();

    // ─── Owned Objects ────────────────────────────────────────────────────
    QString        m_modelPath;
    ChatModel      m_chatModel;
    McpManager     m_mcp;
    QThread        m_workerThread;
    LlamaWorker   *m_worker = nullptr;

    // ─── Generierungs-State ───────────────────────────────────────────────
    bool    m_generating        = false;
    QString m_currentResponse;   // inkl. <think>-Blöcke, für ChatModel
    QString m_thinkBuffer;       // Akkumulationspuffer für State-Machine
    bool    m_inThinkBlock      = false;
    int     m_continuationCount = 0;
    static constexpr int MAX_CONTINUATIONS = 3;

    // ─── Token-Statistik ──────────────────────────────────────────────────
    int m_generatedTokens = 0;
    int m_totalTokens     = 0;
    int m_promptTokens    = 0;
    int m_ctxSize         = 0;
};
