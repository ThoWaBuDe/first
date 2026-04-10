#include "LlamaWorker.h"
#include "ChatModel.h"
#include "AppConfig.h"

#include "llama.h"
#include "ggml.h"

#include <QDebug>
#include <vector>
#include <string>
#include <numeric>      // std::accumulate

// POSIX — für open()/read()/close() auf /dev/urandom
#include <fcntl.h>
#include <unistd.h>

#define AS_MODEL(p)   reinterpret_cast<llama_model*>(p)
#define AS_CTX(p)     reinterpret_cast<llama_context*>(p)
#define AS_SAMPLER(p) reinterpret_cast<llama_sampler*>(p)

// ─── randomSeed ──────────────────────────────────────────────────────────────
// Liest 4 Byte aus /dev/urandom — dem Kernel-CSPRNG.
// Warum /dev/urandom und nicht /dev/random?
//   /dev/random blockiert wenn der Entropie-Pool leer ist (selten, aber möglich).
//   /dev/urandom blockiert nie und ist für unseren Zweck (Sampler-Seed) völlig
//   ausreichend — wir brauchen keine kryptographische Stärke, nur Nicht-Determinis-
//   mus. Seit Linux 4.8 sind beide intern identisch implementiert.
//
// Fallback: time(nullptr) — schlechter, aber besser als ein fester Wert.
// Pattern: Defense in Depth — jede Ebene hat einen Fallback.
static uint32_t randomSeed()
{
    uint32_t seed = 0;
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ::read(fd, &seed, sizeof(seed));
        ::close(fd);
    }
    if (seed == 0)                              // open() fehlgeschlagen
        seed = static_cast<uint32_t>(time(nullptr));
    return seed;
}

LlamaWorker::LlamaWorker(QObject *parent)
    : QObject(parent)
{}

LlamaWorker::~LlamaWorker()
{
    cleanup();
}

void LlamaWorker::cleanup()
{
    m_sampler = nullptr;
    if (m_samplerChat) { llama_sampler_free(AS_SAMPLER(m_samplerChat)); m_samplerChat = nullptr; }
    if (m_samplerTool) { llama_sampler_free(AS_SAMPLER(m_samplerTool)); m_samplerTool = nullptr; }
    if (m_ctx)         { llama_free(AS_CTX(m_ctx));                     m_ctx         = nullptr; }
    if (m_model)       { llama_model_free(AS_MODEL(m_model));           m_model       = nullptr; }
}

// ─── buildChatSampler / buildToolSampler ─────────────────────────────────────
// Der Dist-Sampler bekommt hier noch einen Platzhalter-Seed (0).
// Der echte Seed wird in doGenerate() kurz vor jeder Generation gesetzt —
// siehe refreshDistSampler(). So bleibt die Sampler-Kette stabil (kein
// rebuild nötig) aber der PRNG-Startzustand ist jedes Mal frisch.
void *LlamaWorker::buildChatSampler()
{
    const AppConfig &cfg = AppConfig::instance();
    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(cfg.chatTopK()));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(cfg.chatTemp()));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(cfg.chatTopP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(cfg.chatMinP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(0)); // Seed wird in doGenerate() ersetzt
    return chain;
}

void *LlamaWorker::buildToolSampler()
{
    const AppConfig &cfg = AppConfig::instance();
    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(cfg.toolTopK()));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(cfg.toolTemp()));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(cfg.toolTopP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(cfg.toolMinP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(0)); // Seed wird in doGenerate() ersetzt
    return chain;
}

// ─── refreshDistSampler ──────────────────────────────────────────────────────
// Tauscht den letzten Sampler in der Kette (= Dist) gegen einen neuen mit
// frischem Seed aus. Die anderen Sampler (TopK, Temp, TopP, MinP) bleiben
// unberührt — ihre Parameter ändern sich nicht.
//
// Warum den letzten Sampler tauschen und nicht die ganze Kette neu bauen?
//   - buildChatSampler() liest aus AppConfig — das ist okay, aber unnötig
//     wenn sich nur der Seed ändert.
//   - llama_sampler_chain_remove() + re-add wäre sauberer, aber die llama.cpp
//     C-API bietet das nicht direkt. Stattdessen: alten Dist freigeben,
//     neuen anlegen, an die Kette hängen.
//
// ACHTUNG: llama_sampler_chain_add() hängt immer ans Ende. Der Dist-Sampler
// muss also wirklich der letzte in der Kette sein — das ist in buildChatSampler/
// buildToolSampler so sichergestellt.
//
// Für Mikrocontroller-Denker: das ist wie ein Timer-Reload-Register — du
// schreibst nur den Startwert neu, die Timer-Hardware läuft dann von dort.
void LlamaWorker::refreshDistSampler(void *chain)
{
    // llama_sampler_chain_remove() entfernt per Index.
    // Index 4 = letzter Sampler (0=TopK, 1=Temp, 2=TopP, 3=MinP, 4=Dist).
    llama_sampler *old = llama_sampler_chain_remove(AS_SAMPLER(chain), 4);
    if (old) llama_sampler_free(old);
    llama_sampler_chain_add(AS_SAMPLER(chain), llama_sampler_init_dist(randomSeed()));
}

void LlamaWorker::rebuildSamplers()
{
    if (m_samplerChat) { llama_sampler_free(AS_SAMPLER(m_samplerChat)); m_samplerChat = nullptr; }
    if (m_samplerTool) { llama_sampler_free(AS_SAMPLER(m_samplerTool)); m_samplerTool = nullptr; }
    m_samplerChat = buildChatSampler();
    m_samplerTool = buildToolSampler();
    m_sampler     = m_samplerChat;
    emit samplersRebuilt();
}

// ─── initialize ──────────────────────────────────────────────────────────────
void LlamaWorker::initialize(const QString &modelPath)
{
    qDebug() << "ModellPfad:" << modelPath;
    cleanup();
    llama_backend_init();

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = -1;

    m_model = llama_model_load_from_file(modelPath.toLocal8Bit().constData(), modelParams);
    if (!m_model) {
        emit errorOccurred(QString("Modell konnte nicht geladen werden: %1").arg(modelPath));
        return;
    }

    const AppConfig &cfg = AppConfig::instance();
    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx           = cfg.contextSize();
    ctxParams.n_batch         = cfg.batchSize();
    ctxParams.n_ubatch        = cfg.batchSize();
    ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    ctxParams.type_k = GGML_TYPE_Q8_0;
    ctxParams.type_v = GGML_TYPE_Q8_0;

    m_ctx = llama_init_from_model(AS_MODEL(m_model), ctxParams);
    if (!m_ctx) {
        emit errorOccurred("Kontext konnte nicht erstellt werden.");
        cleanup();
        return;
    }

    m_samplerChat = buildChatSampler();
    m_samplerTool = buildToolSampler();
    m_sampler     = m_samplerChat;
    m_initialized = true;

    // ─── Chat-Template aus GGUF auslesen ─────────────────────────────────
    const char *rawTmpl = llama_model_chat_template(AS_MODEL(m_model), nullptr);
    QString jinjaTemplate = rawTmpl ? QString::fromUtf8(rawTmpl) : QString();
    m_detectedPreset = ChatTemplate::detectFromJinja(jinjaTemplate);

    emit chatTemplateDetected(jinjaTemplate, m_detectedPreset);
    emit modelLoaded();
}

// ─── roleToStr ───────────────────────────────────────────────────────────────
const char *LlamaWorker::roleToStr(ChatMessage::Role role)
{
    switch (role) {
    case ChatMessage::Role::System:    return "system";
    case ChatMessage::Role::User:      return "user";
    case ChatMessage::Role::Assistant: return "assistant";
    case ChatMessage::Role::Tool:      return "tool";
    }
    return "user";
}

// ─── applyTemplate ───────────────────────────────────────────────────────────
// Unverändert — siehe Original.
QString LlamaWorker::applyTemplate(const QVector<ChatMessage> &messages) const
{
    if (!m_model) return {};

    std::vector<std::string>         contents;
    std::vector<llama_chat_message>  msgs;
    contents.reserve(messages.size());
    msgs.reserve(messages.size());

    for (const ChatMessage &m : messages) {
        contents.push_back(m.content.toStdString());
        msgs.push_back({ roleToStr(m.role), contents.back().c_str() });
    }

    const char *tmpl = llama_model_chat_template(AS_MODEL(m_model), nullptr);

    size_t totalContent = std::accumulate(
        contents.begin(), contents.end(), size_t(0),
        [](size_t sum, const std::string &s) { return sum + s.size(); });
    size_t bufSize = totalContent * 2 + 1024;

    std::vector<char> buf(bufSize);
    int result = llama_chat_apply_template(
        tmpl, msgs.data(), msgs.size(),
        true, buf.data(), static_cast<int32_t>(buf.size()));

    if (result > 0)
        return QString::fromUtf8(buf.data(), result);

    if (result < 0 && bufSize < 512 * 1024) {
        bufSize = 512 * 1024;
        buf.resize(bufSize);
        result = llama_chat_apply_template(
            tmpl, msgs.data(), msgs.size(),
            true, buf.data(), static_cast<int32_t>(buf.size()));
        if (result > 0)
            return QString::fromUtf8(buf.data(), result);
    }

    qWarning() << "LlamaWorker: llama_chat_apply_template fehlgeschlagen,"
               << "Fallback auf ChatTemplate-Preset:"
               << ChatTemplate::presetName(m_detectedPreset);

    ChatModel fallbackModel;
    fallbackModel.setChatTemplate(ChatTemplate::forPreset(m_detectedPreset));
    for (const ChatMessage &m : messages) {
        switch (m.role) {
        case ChatMessage::Role::System:
            fallbackModel.setSystemPrompt(m.content); break;
        case ChatMessage::Role::User:
            fallbackModel.addUserMessage(m.content); break;
        case ChatMessage::Role::Assistant:
            fallbackModel.addAssistantMessage(m.content); break;
        case ChatMessage::Role::Tool:
            fallbackModel.addToolResult("tool", m.content); break;
        }
    }
    return fallbackModel.buildPrompt();
}

// ─── doGenerate ──────────────────────────────────────────────────────────────
// Private Hilfsmethode — enthält die gesamte Inference-Logik.
//
// Parameter clearCache:
//   true  → llama_memory_clear() vor dem Encode — bisheriges Verhalten.
//            Jeder generate()-Aufruf startet mit leerem KV-Cache.
//            Nachteil: der gesamte Prompt muss jedes Mal neu durch llama_decode().
//
//   false → Cache bleibt erhalten — generateDelta()-Verhalten (Vorbereitung).
//            Nur neue Tokens müssen encodiert werden.
//            Voraussetzung: der Prompt ist ein echter Suffix des vorherigen.
//            Noch nicht vollständig implementiert (n_past-Tracking fehlt),
//            daher erstmal als struktureller Platzhalter.
//
// Warum hier und nicht in generate()?
//   DRY-Prinzip (Don't Repeat Yourself). Der Token-Sampling-Loop ist ~40 Zeilen
//   die bei zwei separaten Methoden doppelt gepflegt werden müssten.
//   Eine private doGenerate() mit bool-Parameter ist das Standard-C++-Pattern
//   für "zwei öffentliche Methoden, ein gemeinsamer Kern".
void LlamaWorker::doGenerate(const QVector<ChatMessage> &messages,
                             SamplerProfile profile,
                             bool clearCache)
{
    if (!m_initialized) {
        emit errorOccurred("Worker nicht initialisiert.");
        return;
    }

    m_stopFlag.store(false);

    // Sampler-Profil wählen
    switch (profile) {
    case SamplerProfile::Tool: m_sampler = m_samplerTool; break;
    default:                   m_sampler = m_samplerChat; break;
    }

    // ── Frischer Seed vor jeder Generation ───────────────────────────────
    // Warum hier und nicht in buildChatSampler()?
    //   buildChatSampler() wird nur beim Start und bei rebuildSamplers()
    //   aufgerufen. Würden wir den Seed dort setzen, wäre er für alle
    //   Generationen dieser Session gleich.
    //   Hier, direkt vor dem encode, bekommt jede Generation einen eigenen
    //   Startzustand — auch Retry-Versuche nach Tool-Fehlern.
    refreshDistSampler(m_sampler);

    QString promptQStr = applyTemplate(messages);
    if (promptQStr.isEmpty()) {
        emit errorOccurred("Prompt konnte nicht gebaut werden.");
        return;
    }

    const std::string  promptStr = promptQStr.toStdString();
    const llama_vocab *vocab     = llama_model_get_vocab(AS_MODEL(m_model));

    // Tokenisierung: erst mit geschätzter Größe, bei Überlauf resize + retry.
    // Das ist das Standard-Pattern aus der llama.cpp Dokumentation.
    int maxTokens = static_cast<int>(promptStr.size()) + 128;
    std::vector<llama_token> promptTokens(maxTokens);

    int nTokens = llama_tokenize(vocab, promptStr.c_str(),
                                 static_cast<int32_t>(promptStr.size()),
                                 promptTokens.data(), static_cast<int32_t>(promptTokens.size()),
                                 true, true);
    if (nTokens < 0) {
        promptTokens.resize(-nTokens);
        llama_tokenize(vocab, promptStr.c_str(),
                       static_cast<int32_t>(promptStr.size()),
                       promptTokens.data(), static_cast<int32_t>(promptTokens.size()),
                       true, true);
        nTokens = static_cast<int>(promptTokens.size());
    }
    promptTokens.resize(nTokens);

    const int n_ctx   = llama_n_ctx(AS_CTX(m_ctx));
    const int n_batch = llama_n_batch(AS_CTX(m_ctx));

    if (nTokens >= n_ctx) {
        emit errorOccurred(
            QString("Context-Overflow: Prompt hat %1 Tokens, Context-Window ist %2.")
                .arg(nTokens).arg(n_ctx));
        return;
    }

    emit statsUpdate(nTokens, n_ctx);

    if ((n_ctx - nTokens) < n_ctx / 10)
        emit tokenGenerated(QString("[WARNUNG: Kontext fast voll: %1 Tokens]\n")
                                .arg(n_ctx - nTokens));

    // ── Cache-Handling ────────────────────────────────────────────────────
    if (clearCache) {
        // Bisheriges Verhalten: KV-Cache komplett leeren.
        // false = nur Metadaten löschen, Speicher bleibt allokiert (schneller).
        llama_memory_t mem = llama_get_memory(AS_CTX(m_ctx));
        llama_memory_clear(mem, false);
    }
    // else: Cache bleibt — generateDelta() Pfad.
    // TODO: n_past-Tracking für echtes Delta-Encoding (nächste Ausbaustufe).

    // ── Prompt encoden (Prefill) ──────────────────────────────────────────
    // Der Prompt wird in Batches durch llama_decode() geschoben.
    // llama_decode() füllt den KV-Cache und berechnet die Attention.
    // n_batch ist die maximale Chunk-Größe (aus AppConfig/ctxParams).
    // Producer/Consumer-Pattern: wir liefern Chunks, llama.cpp konsumiert sie.
    int processed = 0;
    while (processed < nTokens) {
        int chunkSize = std::min(n_batch, nTokens - processed);
        llama_batch chunk = llama_batch_get_one(promptTokens.data() + processed, chunkSize);
        if (llama_decode(AS_CTX(m_ctx), chunk) != 0) {
            emit errorOccurred(QString("llama_decode fehlgeschlagen bei Token %1/%2.")
                                   .arg(processed).arg(nTokens));
            return;
        }
        processed += chunkSize;
        if (m_stopFlag.load()) return;
    }

    // ── Token-Sampling-Loop (Autoregressive Decode) ───────────────────────
    // Jede Iteration: ein Token samplen, emittieren, in den Cache einspeisen.
    // Das Modell "sieht" seinen eigenen Output als Input für den nächsten Token.
    // Abbruch: EOG-Token, stopFlag, maxNewTokens, oder Tool-Stop-Sequenz.
    static const QString TOOL_STOP  = "</tool_call>";
    QString fullResponse;
    const int maxNewTokens = 16384;

    for (int i = 0; i < maxNewTokens; ++i) {
        if (m_stopFlag.load()) break;

        llama_token newToken = llama_sampler_sample(AS_SAMPLER(m_sampler), AS_CTX(m_ctx), -1);
        if (llama_vocab_is_eog(vocab, newToken)) break;

        QString tokenStr = tokenToString(newToken);
        fullResponse += tokenStr;
        emit tokenGenerated(tokenStr);

        if (profile == SamplerProfile::Tool && fullResponse.endsWith(TOOL_STOP))
            break;

        llama_batch nextBatch = llama_batch_get_one(&newToken, 1);
        if (llama_decode(AS_CTX(m_ctx), nextBatch) != 0) {
            emit errorOccurred(QString("llama_decode fehlgeschlagen nach %1 Tokens.").arg(i));
            break;
        }
    }

    emit generationDone(fullResponse);
}

// ─── generate / generateDelta ────────────────────────────────────────────────
// Zwei öffentliche Slots — thin wrapper um doGenerate().
// Agent ruft generate() wie bisher. generateDelta() ist für späteres
// KV-Cache-Rollback vorbereitet, aber noch nicht im Agent verdrahtet.

void LlamaWorker::generate(const QVector<ChatMessage> &messages,
                           LlamaWorker::SamplerProfile profile)
{
    doGenerate(messages, profile, /*clearCache=*/true);
}

void LlamaWorker::generateDelta(const QVector<ChatMessage> &messages,
                                LlamaWorker::SamplerProfile profile)
{
    doGenerate(messages, profile, /*clearCache=*/false);
}

// ─── stopGeneration ──────────────────────────────────────────────────────────
void LlamaWorker::stopGeneration()
{
    m_stopFlag.store(true);
}

// ─── tokenToString ───────────────────────────────────────────────────────────
QString LlamaWorker::tokenToString(int tokenId) const
{
    const llama_vocab *vocab = llama_model_get_vocab(AS_MODEL(m_model));
    char buf[256] = {0};
    int len = llama_token_to_piece(vocab, tokenId, buf, sizeof(buf) - 1, 0, false);
    if (len <= 0) return {};
    return QString::fromUtf8(buf, len);
}
