#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include "ChatModel.h"
#include "ChatTemplate.h"

class LlamaWorker : public QObject {
    Q_OBJECT

public:
    enum class SamplerProfile { Chat, Tool };
    Q_ENUM(SamplerProfile)

    explicit LlamaWorker(QObject *parent = nullptr);
    ~LlamaWorker() override;

public slots:
    void initialize(const QString &modelPath);

    // Cache wird vor dem Encode geleert — bisheriges Verhalten.
    void generate(const QVector<ChatMessage> &messages,
                  LlamaWorker::SamplerProfile profile = LlamaWorker::SamplerProfile::Chat);

    // Cache bleibt erhalten — Vorbereitung für Delta-Encoding / KV-Rollback.
    // Noch nicht im Agent verdrahtet. n_past-Tracking folgt in nächster Ausbaustufe.
    void generateDelta(const QVector<ChatMessage> &messages,
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
    void chatTemplateDetected(const QString &jinjaTemplate,
                              ChatTemplate::Preset detectedPreset);

private:
    // Gemeinsamer Kern für generate() und generateDelta().
    // clearCache=true  → llama_memory_clear() vor Encode
    // clearCache=false → Cache bleibt, nur Delta encoden (TODO: n_past)
    void doGenerate(const QVector<ChatMessage> &messages,
                    SamplerProfile profile,
                    bool clearCache);

    // Tauscht den Dist-Sampler (letzter in der Kette, Index 4) gegen
    // einen neuen mit frischem Seed aus /dev/urandom.
    // Wird in doGenerate() vor jeder Generation aufgerufen.
    void refreshDistSampler(void *chain);

    QString applyTemplate(const QVector<ChatMessage> &messages) const;
    static const char *roleToStr(ChatMessage::Role role);

    void *m_model       = nullptr;
    void *m_ctx         = nullptr;
    void *m_samplerChat = nullptr;
    void *m_samplerTool = nullptr;
    void *m_sampler     = nullptr;

    std::atomic<bool> m_stopFlag{false};
    bool m_initialized = false;

    ChatTemplate::Preset m_detectedPreset = ChatTemplate::Preset::ChatML;

    QString tokenToString(int tokenId) const;
    void    cleanup();
    void   *buildChatSampler();
    void   *buildToolSampler();
};

Q_DECLARE_METATYPE(LlamaWorker::SamplerProfile)
