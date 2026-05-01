#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include "Chat/ChatModel.h"
#include "Chat/ChatTemplate.h"
#include "Chat/ToolCallFormat.h"

// ─── LlamaWorker ──────────────────────────────────────────────────────────────
// Active Object im Worker-Thread.
//
// NEU: toolCallFormatDetected Signal
//   Nach dem Laden des Modells wird das Tool-Call-Format erkannt:
//   1. Aus dem Jinja-Template im GGUF (genauer)
//   2. Aus dem Modell-Dateinamen (Fallback)
//   Analogie zu chatTemplateDetected — gleiche Erkennungslogik, zweite Ebene.

class LlamaWorker : public QObject {
    Q_OBJECT

public:
    enum class SamplerProfile {
        Chat,
        Execute,
        Tool
    };
    Q_ENUM(SamplerProfile)

    explicit LlamaWorker(QObject *parent = nullptr);
    ~LlamaWorker() override;

public slots:
    void initialize(const QString &modelPath);

    void generate(const QVector<ChatMessage> &messages,
                  LlamaWorker::SamplerProfile profile =
                      LlamaWorker::SamplerProfile::Chat);

    void generateDelta(const QVector<ChatMessage> &messages,
                       LlamaWorker::SamplerProfile profile =
                           LlamaWorker::SamplerProfile::Chat);

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
    // NEU: Tool-Call-Format nach Modell-Load
    // source: "GGUF-Template" oder "Modellname" — für UI-Anzeige
    void toolCallFormatDetected(ToolCallFormat::Preset detectedFormat,
                                const QString &source);

private:
    void doGenerate(const QVector<ChatMessage> &messages,
                    SamplerProfile profile,
                    bool clearCache);

    void *buildChatSampler();
    void *buildExecuteSampler();
    void *buildToolSampler();
    void  refreshDistSampler(void *chain);

    QString applyTemplate(const QVector<ChatMessage> &messages) const;
    static const char *roleToStr(ChatMessage::Role role);
    QString tokenToString(int tokenId) const;
    void    cleanup();

    void *m_model         = nullptr;
    void *m_ctx           = nullptr;
    void *m_samplerChat    = nullptr;
    void *m_samplerExecute = nullptr;
    void *m_samplerTool    = nullptr;
    void *m_sampler        = nullptr;

    std::atomic<bool> m_stopFlag{false};
    bool m_initialized = false;

    ChatTemplate::Preset   m_detectedPreset     = ChatTemplate::Preset::ChatML;
    ToolCallFormat::Preset m_detectedToolFormat  = ToolCallFormat::Preset::Generic; // NEU

    QString m_modelPath; // NEU: für Fallback-Detection aus Dateiname
};

Q_DECLARE_METATYPE(LlamaWorker::SamplerProfile)
