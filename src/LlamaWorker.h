#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include "ChatModel.h"    // für QVector<ChatMessage>
#include "ChatTemplate.h" // für Fallback

// ─── LlamaWorker ──────────────────────────────────────────────────────────────
// llama.cpp Inference im Worker-Thread. Pattern: Active Object.
//
// Umbau: generate() bekommt jetzt QVector<ChatMessage> statt einen fertigen
// Prompt-String. Der Worker baut den Prompt intern via
// llama_chat_apply_template() — die Funktion liest das Chat-Template direkt
// aus den GGUF-Metadaten des geladenen Modells.
//
// Warum im Worker und nicht im Agent?
//   llama_chat_apply_template() braucht den llama_model* Pointer.
//   Der lebt im Worker-Thread. Würde Agent ihn benutzen, wäre das ein
//   Datenrace ohne Queue-Schutz.
//   Pattern: den Zugriff auf ressource-gebundene Daten dort lassen wo
//   die Ressource lebt — analog zum AVR wo du Peripherie-Register nur
//   aus dem richtigen ISR-Kontext anfasst.
//
// Fallback:
//   Falls llama_chat_apply_template() -1 zurückgibt (kein Template im GGUF,
//   unbekanntes Format), fällt applyTemplate() auf ChatModel::buildPrompt()
//   zurück — das bisherige Verhalten mit ChatTemplate-Struct.
//   ChatTemplate.h bleibt deshalb erhalten.

class LlamaWorker : public QObject {
    Q_OBJECT

public:
    enum class SamplerProfile { Chat, Tool };
    Q_ENUM(SamplerProfile)

    explicit LlamaWorker(QObject *parent = nullptr);
    ~LlamaWorker() override;

public slots:
    void initialize(const QString &modelPath);

    // Neu: bekommt rohe Nachrichten, baut Prompt intern via llama_chat_apply_template.
    // profile steuert welcher Sampler verwendet wird (Chat=kreativ, Tool=deterministisch).
    void generate(const QVector<ChatMessage> &messages,
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

    // Wird direkt vor modelLoaded() emittiert.
    // jinjaTemplate: roher Jinja2-String aus GGUF ("tokenizer.chat_template").
    //   Leer wenn kein Template eingebettet ist.
    // detectedPreset: Heuristisch erkanntes Preset (nie Preset::Auto).
    //   Wird für ConfigDialog-Anzeige und Fallback verwendet.
    void chatTemplateDetected(const QString &jinjaTemplate,
                              ChatTemplate::Preset detectedPreset);

private:
    // Baut den fertigen Prompt-String aus den Nachrichten.
    //
    // Strategie (zwei Stufen):
    //   1. llama_chat_apply_template() — nutzt Template aus GGUF
    //      Puffer-Größe: 2 × Summe aller Nachrichtenlängen (llama.cpp Empfehlung)
    //      Gibt -1 zurück wenn kein Template vorhanden oder unbekannt → Fallback
    //   2. Fallback: ChatModel::buildPromptFromMessages() mit m_detectedPreset
    //      Bisheriges Verhalten, garantiert immer einen String zu liefern.
    //
    // add_ass=true: öffnet den Assistant-Turn am Ende — llama.cpp generiert
    // ab dort. Entspricht dem bisherigen assistantStart am Ende von buildPrompt().
    QString applyTemplate(const QVector<ChatMessage> &messages) const;

    // Hilfsfunktion: ChatMessage::Role → C-String für llama_chat_message
    static const char *roleToStr(ChatMessage::Role role);

    void *m_model       = nullptr;
    void *m_ctx         = nullptr;
    void *m_samplerChat = nullptr;
    void *m_samplerTool = nullptr;
    void *m_sampler     = nullptr;

    std::atomic<bool> m_stopFlag{false};
    bool m_initialized = false;

    // Zuletzt erkanntes Preset — für Fallback in applyTemplate().
    // Wird in initialize() nach llama_model_chat_template() gesetzt.
    ChatTemplate::Preset m_detectedPreset = ChatTemplate::Preset::ChatML;

    QString tokenToString(int tokenId) const;
    void    cleanup();
    void   *buildChatSampler();
    void   *buildToolSampler();
};

Q_DECLARE_METATYPE(LlamaWorker::SamplerProfile)
