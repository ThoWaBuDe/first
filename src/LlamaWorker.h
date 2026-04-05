#pragma once
#include <QObject>
#include <QString>
#include <atomic>

// ─── LlamaWorker ──────────────────────────────────────────────────────────────
// Fuehrt llama.cpp Inference in einem separaten Thread aus.
// Pattern: Active Object — eigener Thread, async Kommunikation via Signals.
//
// ─── Zwei Sampler-Profile ─────────────────────────────────────────────────────
//
// Beide Sampler werden EINMALIG in initialize() gebaut und leben bis cleanup().
// Vor jeder Generierung wird llama_sampler_reset() aufgerufen — das setzt
// den internen Zustand aller Chain-Glieder zurueck:
//   - Dist-RNG:           zurueck auf Seed-Zustand
//   - Lazy-Grammar:       zurueck in "schlafend" (wartet auf Trigger-Pattern)
//
// Chat-Profil (SamplerProfile::Chat):
//   Top-K 40 → Temp 0.7 → Top-P 0.95 → Min-P 0.05 → Dist
//   Breit, kreativ — fuer normale Konversation.
//
// Tool-Profil (SamplerProfile::Tool):
//   Top-K 20 → Temp 0.1 → Top-P 0.50 → Min-P 0.05 → Dist → Lazy-Grammar
//   Eng, deterministisch — fuer Tool-Calls und Code.
//
//   Lazy-Grammar: schlaeft bis "<tool_call>" im Stream erscheint,
//   dann erzwingt sie gueltiges JSON fuer den Inhalt des Tool-Calls.
//   Thinking (<think>...</think>) vor dem Tool-Call laeuft ungehindert durch.
//   Pattern: Strategy mit Lazy Evaluation.
//
// Profilwechsel = Pointer-Swap auf m_sampler + reset() — kein Alloc/Free.
// Pattern: Strategy — zwei austauschbare Algorithmen hinter einer Schnittstelle.
class LlamaWorker : public QObject {
    Q_OBJECT

public:
    enum class SamplerProfile {
        Chat,   // Temp 0.7, breit — normale Konversation
        Tool    // Temp 0.1, eng + Lazy-Grammar — Tool-Calls und Code
    };
    Q_ENUM(SamplerProfile)

    explicit LlamaWorker(QObject *parent = nullptr);
    ~LlamaWorker() override;

public slots:
    void initialize(const QString &modelPath);

    void generate(const QString &prompt,
                  LlamaWorker::SamplerProfile profile = LlamaWorker::SamplerProfile::Chat);

    void stopGeneration();

signals:
    void tokenGenerated(const QString &token);
    void generationDone(const QString &fullResponse);
    void statsUpdate(int promptTokens, int ctxSize);
    void errorOccurred(const QString &error);
    void modelLoaded();

private:
    void *m_model       = nullptr;  // llama_model*
    void *m_ctx         = nullptr;  // llama_context*

    void *m_samplerChat = nullptr;  // llama_sampler* — dauerhaft, kein Grammar-Zustand
    void *m_samplerTool = nullptr;  // llama_sampler* — dauerhaft, Lazy-Grammar

    // Aktiver Zeiger — kein Owner, nie freigeben.
    // Zeigt auf m_samplerChat oder m_samplerTool.
    void *m_sampler     = nullptr;  // llama_sampler*

    std::atomic<bool> m_stopFlag{false};
    bool m_initialized = false;

    QString tokenToString(int tokenId) const;
    void    cleanup();

    // Factory Methods — geben llama_sampler* als void* zurueck.
    void *buildChatSampler();
    void *buildToolSampler();
};

Q_DECLARE_METATYPE(LlamaWorker::SamplerProfile)
