#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include "ChatModel.h"
#include "ChatTemplate.h"

// ─── LlamaWorker ──────────────────────────────────────────────────────────────
// Active Object im Worker-Thread. Führt blockierende llama.cpp-Inference durch.
//
// NEU (Punkt I): SamplerProfile::Execute
//   Drittes Profil zwischen Chat und Tool:
//   - Chat  (Top-K 40, Temp 0.7): Konversation, Plan — kreativ
//   - Execute (Top-K 20, Temp 0.2): Code-Generierung — deterministisch aber flexibel
//   - Tool  (Top-K 20, Temp 0.1): JSON-Tool-Calls — maximal deterministisch
//
// Warum Execute zwischen Chat und Tool?
//   Tool-Calls brauchen exaktes JSON → niedrigste Temp.
//   Code-Generierung braucht korrekte Syntax UND algorithmische Kreativität.
//   Zu niedrige Temp → repetitiver, schablonenhafter Code.
//   Zu hohe Temp → Halluzination von Symbolen/APIs.
//   Temp 0.2 ist empirisch ein guter Mittelwert für Code-LLMs.

class LlamaWorker : public QObject {
    Q_OBJECT

public:
    enum class SamplerProfile {
        Chat,    // Konversation, Plan
        Execute, // Code-Generierung (NEU — Punkt I)
        Tool     // JSON Tool-Calls
    };
    Q_ENUM(SamplerProfile)

    explicit LlamaWorker(QObject *parent = nullptr);
    ~LlamaWorker() override;

public slots:
    void initialize(const QString &modelPath);

    // KV-Cache wird geleert — bisheriges Verhalten
    void generate(const QVector<ChatMessage> &messages,
                  LlamaWorker::SamplerProfile profile =
                      LlamaWorker::SamplerProfile::Chat);

    // Cache bleibt — Vorbereitung für Delta-Encoding (noch nicht aktiv)
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

private:
    void doGenerate(const QVector<ChatMessage> &messages,
                    SamplerProfile profile,
                    bool clearCache);

    // Baut die drei Sampler-Ketten
    void *buildChatSampler();
    void *buildExecuteSampler(); // NEU
    void *buildToolSampler();

    // Tauscht Dist-Sampler (Index 4) gegen neuen mit frischem Seed
    void refreshDistSampler(void *chain);

    QString applyTemplate(const QVector<ChatMessage> &messages) const;
    static const char *roleToStr(ChatMessage::Role role);
    QString tokenToString(int tokenId) const;
    void    cleanup();

    void *m_model         = nullptr;
    void *m_ctx           = nullptr;
    void *m_samplerChat    = nullptr;
    void *m_samplerExecute = nullptr; // NEU
    void *m_samplerTool    = nullptr;
    void *m_sampler        = nullptr; // aktiver Sampler (Zeiger auf eines der drei)

    std::atomic<bool> m_stopFlag{false};
    bool m_initialized = false;

    ChatTemplate::Preset m_detectedPreset = ChatTemplate::Preset::ChatML;
};

Q_DECLARE_METATYPE(LlamaWorker::SamplerProfile)
