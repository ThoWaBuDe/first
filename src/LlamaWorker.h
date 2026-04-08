#pragma once
#include <QObject>
#include <QString>
#include <atomic>
#include "ChatTemplate.h"

// ─── LlamaWorker ──────────────────────────────────────────────────────────────
// llama.cpp Inference im Worker-Thread. Pattern: Active Object.
//
// Neu: Nach dem Laden des Modells wird das eingebettete Chat-Template
// aus den GGUF-Metadaten ausgelesen und per Signal gemeldet.
// Agent verbindet dieses Signal und aktualisiert AppConfig + ChatModel.
class LlamaWorker : public QObject {
    Q_OBJECT

public:
    enum class SamplerProfile { Chat, Tool };
    Q_ENUM(SamplerProfile)

    explicit LlamaWorker(QObject *parent = nullptr);
    ~LlamaWorker() override;

public slots:
    void initialize(const QString &modelPath);
    void generate(const QString &prompt,
                  LlamaWorker::SamplerProfile profile = LlamaWorker::SamplerProfile::Chat);
    void stopGeneration();
    void rebuildSamplers();

signals:
    void tokenGenerated(const QString &token);
    void generationDone(const QString &fullResponse);
    void statsUpdate(int promptTokens, int ctxSize);
    void errorOccurred(const QString &error);
    void modelLoaded();
    void samplersRebuilt();

    // ─── Neu: Chat-Template aus GGUF ──────────────────────────────────────
    // Wird direkt vor modelLoaded() emittiert.
    // jinjaTemplate: roher Jinja2-String aus GGUF-Feld "tokenizer.chat_template".
    //   Leer wenn kein Template eingebettet ist.
    // detectedPreset: Heuristisch erkanntes Preset (nie Preset::Auto).
    //   ChatML als Fallback wenn kein Template oder unbekanntes Format.
    //
    // Reihenfolge der Signale:
    //   1. chatTemplateDetected(...)  ← Agent stellt Template ein
    //   2. modelLoaded()             ← Agent zeigt "Modell bereit" an
    //
    // Warum vor modelLoaded()?
    //   Agent::onModelLoaded() baut den System-Prompt und gibt ihn an
    //   ChatModel weiter. ChatModel muss vorher das richtige Template
    //   kennen damit buildPrompt() korrekt formatiert.
    void chatTemplateDetected(const QString &jinjaTemplate,
                              ChatTemplate::Preset detectedPreset);

private:
    void *m_model       = nullptr;
    void *m_ctx         = nullptr;
    void *m_samplerChat = nullptr;
    void *m_samplerTool = nullptr;
    void *m_sampler     = nullptr;

    std::atomic<bool> m_stopFlag{false};
    bool m_initialized = false;

    QString tokenToString(int tokenId) const;
    void    cleanup();
    void   *buildChatSampler();
    void   *buildToolSampler();
};

Q_DECLARE_METATYPE(LlamaWorker::SamplerProfile)
