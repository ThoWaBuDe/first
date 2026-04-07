#pragma once
#include <QObject>
#include <QString>
#include <atomic>

// ─── LlamaWorker ──────────────────────────────────────────────────────────────
// llama.cpp Inference im Worker-Thread. Pattern: Active Object.
//
// Sampler-Parameter kommen jetzt aus AppConfig — rebuildSamplers() übernimmt
// Änderungen live ohne Modell neu zu laden.
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

    // Sampler mit aktuellen AppConfig-Werten neu bauen (sofort wirksam)
    void rebuildSamplers();

signals:
    void tokenGenerated(const QString &token);
    void generationDone(const QString &fullResponse);
    void statsUpdate(int promptTokens, int ctxSize);
    void errorOccurred(const QString &error);
    void modelLoaded();
    void samplersRebuilt();  // nach rebuildSamplers() — für UI-Bestätigung

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
