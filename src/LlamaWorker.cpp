#include "LlamaWorker.h"
#include "ChatModel.h"
#include "AppConfig.h"

#include "llama.h"
#include "ggml.h"

#include <QDebug>
#include <vector>
#include <string>
#include <numeric>

#include <fcntl.h>
#include <unistd.h>

#define AS_MODEL(p)   reinterpret_cast<llama_model*>(p)
#define AS_CTX(p)     reinterpret_cast<llama_context*>(p)
#define AS_SAMPLER(p) reinterpret_cast<llama_sampler*>(p)

// ─── randomSeed ──────────────────────────────────────────────────────────────
// /dev/urandom — blockiert nie, ausreichend für Sampler-Seed.
static uint32_t randomSeed()
{
    uint32_t seed = 0;
    int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { ::read(fd, &seed, sizeof(seed)); ::close(fd); }
    if (seed == 0) seed = static_cast<uint32_t>(time(nullptr));
    return seed;
}

LlamaWorker::LlamaWorker(QObject *parent) : QObject(parent) {}
LlamaWorker::~LlamaWorker() { cleanup(); }

void LlamaWorker::cleanup()
{
    m_sampler = nullptr;
    if (m_samplerChat)    { llama_sampler_free(AS_SAMPLER(m_samplerChat));    m_samplerChat    = nullptr; }
    if (m_samplerExecute) { llama_sampler_free(AS_SAMPLER(m_samplerExecute)); m_samplerExecute = nullptr; }
    if (m_samplerTool)    { llama_sampler_free(AS_SAMPLER(m_samplerTool));    m_samplerTool    = nullptr; }
    if (m_ctx)            { llama_free(AS_CTX(m_ctx));                        m_ctx            = nullptr; }
    if (m_model)          { llama_model_free(AS_MODEL(m_model));              m_model          = nullptr; }
}

// ─── buildChatSampler ────────────────────────────────────────────────────────
// Chat: kreativ — top_k=40, temp=0.7
void *LlamaWorker::buildChatSampler()
{
    const AppConfig &cfg = AppConfig::instance();
    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(cfg.chatTopK()));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(cfg.chatTemp()));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(cfg.chatTopP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(cfg.chatMinP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(0)); // Seed → refreshDistSampler()
    return chain;
}

// ─── buildExecuteSampler (NEU — Punkt I) ─────────────────────────────────────
// Execute: Code-Generierung — top_k=20, temp=0.2
// Zwischen Chat (kreativ) und Tool (deterministisch).
// Konfigurierbar über AppConfig::SamplerExecute-Gruppe.
void *LlamaWorker::buildExecuteSampler()
{
    const AppConfig &cfg = AppConfig::instance();
    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(cfg.executeTopK()));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(cfg.executeTemp()));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(cfg.executeTopP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(cfg.executeMinP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(0));
    return chain;
}

// ─── buildToolSampler ────────────────────────────────────────────────────────
// Tool: deterministisch — top_k=20, temp=0.1
void *LlamaWorker::buildToolSampler()
{
    const AppConfig &cfg = AppConfig::instance();
    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(cfg.toolTopK()));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(cfg.toolTemp()));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(cfg.toolTopP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(cfg.toolMinP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(0));
    return chain;
}

// ─── refreshDistSampler ──────────────────────────────────────────────────────
// Tauscht Dist-Sampler (letzter in der Kette, Index 4) gegen neuen mit Seed.
// Analogie AVR: Timer-Reload-Register — nur Startwert neu, Hardware läuft weiter.
void LlamaWorker::refreshDistSampler(void *chain)
{
    llama_sampler *old = llama_sampler_chain_remove(AS_SAMPLER(chain), 4);
    if (old) llama_sampler_free(old);
    llama_sampler_chain_add(AS_SAMPLER(chain),
                            llama_sampler_init_dist(randomSeed()));
}

// ─── rebuildSamplers ─────────────────────────────────────────────────────────
// Alle drei Sampler neu bauen — nach Einstellungsänderung.
void LlamaWorker::rebuildSamplers()
{
    if (m_samplerChat)    { llama_sampler_free(AS_SAMPLER(m_samplerChat));    m_samplerChat    = nullptr; }
    if (m_samplerExecute) { llama_sampler_free(AS_SAMPLER(m_samplerExecute)); m_samplerExecute = nullptr; }
    if (m_samplerTool)    { llama_sampler_free(AS_SAMPLER(m_samplerTool));    m_samplerTool    = nullptr; }

    m_samplerChat    = buildChatSampler();
    m_samplerExecute = buildExecuteSampler();
    m_samplerTool    = buildToolSampler();
    m_sampler        = m_samplerChat;
    emit samplersRebuilt();
}

// ─── initialize ──────────────────────────────────────────────────────────────
void LlamaWorker::initialize(const QString &modelPath)
{
    cleanup();
    llama_backend_init();

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = -1;

    m_model = llama_model_load_from_file(
        modelPath.toLocal8Bit().constData(), modelParams);
    if (!m_model) {
        emit errorOccurred(
            QString("Modell konnte nicht geladen werden: %1").arg(modelPath));
        return;
    }

    const AppConfig &cfg = AppConfig::instance();
    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx           = cfg.contextSize();
    ctxParams.n_batch         = cfg.batchSize();
    ctxParams.n_ubatch        = cfg.batchSize();
    ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;

    m_ctx = llama_init_from_model(AS_MODEL(m_model), ctxParams);
    if (!m_ctx) {
        emit errorOccurred("Kontext konnte nicht erstellt werden.");
        cleanup();
        return;
    }

    m_samplerChat    = buildChatSampler();
    m_samplerExecute = buildExecuteSampler();
    m_samplerTool    = buildToolSampler();
    m_sampler        = m_samplerChat;
    m_initialized    = true;

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
QString LlamaWorker::applyTemplate(const QVector<ChatMessage> &messages) const
{
    if (!m_model) return {};

    std::vector<std::string>        contents;
    std::vector<llama_chat_message> msgs;
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
               << "Fallback auf Preset:"
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
// Gemeinsamer Kern für generate() und generateDelta().
// clearCache=true → KV-Cache leeren (bisheriges Verhalten).
// clearCache=false → Delta-Encoding Vorbereitung (TODO: n_past).
void LlamaWorker::doGenerate(const QVector<ChatMessage> &messages,
                              SamplerProfile profile,
                              bool clearCache)
{
    if (!m_initialized) {
        emit errorOccurred("Worker nicht initialisiert.");
        return;
    }
    m_stopFlag.store(false);

    // ── Sampler-Profil wählen (NEU: Execute) ─────────────────────────────
    switch (profile) {
        case SamplerProfile::Execute: m_sampler = m_samplerExecute; break;
        case SamplerProfile::Tool:    m_sampler = m_samplerTool;    break;
        default:                      m_sampler = m_samplerChat;    break;
    }

    // Frischer Seed vor jeder Generation → kein deterministischer Loop
    refreshDistSampler(m_sampler);

    QString promptQStr = applyTemplate(messages);
    if (promptQStr.isEmpty()) {
        emit errorOccurred("Prompt konnte nicht gebaut werden.");
        return;
    }

    const std::string  promptStr = promptQStr.toStdString();
    const llama_vocab *vocab     = llama_model_get_vocab(AS_MODEL(m_model));

    int maxTokens = static_cast<int>(promptStr.size()) + 128;
    std::vector<llama_token> promptTokens(maxTokens);

    int nTokens = llama_tokenize(
        vocab, promptStr.c_str(),
        static_cast<int32_t>(promptStr.size()),
        promptTokens.data(), static_cast<int32_t>(promptTokens.size()),
        true, true);
    if (nTokens < 0) {
        promptTokens.resize(-nTokens);
        nTokens = static_cast<int>(promptTokens.size());
        llama_tokenize(vocab, promptStr.c_str(),
                       static_cast<int32_t>(promptStr.size()),
                       promptTokens.data(),
                       static_cast<int32_t>(promptTokens.size()),
                       true, true);
    }
    promptTokens.resize(nTokens);

    const int n_ctx   = llama_n_ctx(AS_CTX(m_ctx));
    const int n_batch = llama_n_batch(AS_CTX(m_ctx));

    if (nTokens >= n_ctx) {
        emit errorOccurred(
            QString("Context-Overflow: Prompt hat %1 Tokens, Window ist %2.")
            .arg(nTokens).arg(n_ctx));
        return;
    }

    emit statsUpdate(nTokens, n_ctx);

    if ((n_ctx - nTokens) < n_ctx / 10)
        emit tokenGenerated(
            QString("[WARNUNG: Kontext fast voll: %1 Tokens]\n")
            .arg(n_ctx - nTokens));

    if (clearCache) {
        llama_memory_t mem = llama_get_memory(AS_CTX(m_ctx));
        llama_memory_clear(mem, false);
    }

    // ── Prefill ──────────────────────────────────────────────────────────
    int processed = 0;
    while (processed < nTokens) {
        int chunkSize = std::min(n_batch, nTokens - processed);
        llama_batch chunk =
            llama_batch_get_one(promptTokens.data() + processed, chunkSize);
        if (llama_decode(AS_CTX(m_ctx), chunk) != 0) {
            emit errorOccurred(
                QString("llama_decode fehlgeschlagen bei Token %1/%2.")
                .arg(processed).arg(nTokens));
            return;
        }
        processed += chunkSize;
        if (m_stopFlag.load()) return;
    }

    // ── Token-Sampling-Loop ───────────────────────────────────────────────
    static const QString TOOL_STOP = "</tool_call>";
    static const QString CODE_STOP = "</code>";
    QString fullResponse;
    const int maxNewTokens = 8192;

    for (int i = 0; i < maxNewTokens; ++i) {
        if (m_stopFlag.load()) break;

        llama_token newToken =
            llama_sampler_sample(AS_SAMPLER(m_sampler), AS_CTX(m_ctx), -1);
        if (llama_vocab_is_eog(vocab, newToken)) break;

        QString tokenStr = tokenToString(newToken);
        fullResponse += tokenStr;
        emit tokenGenerated(tokenStr);

        // Tool-Calls früh stoppen
        if ((profile == SamplerProfile::Tool ||
             profile == SamplerProfile::Execute) &&
            fullResponse.endsWith(TOOL_STOP))
            break;

        // FIX: Execute-Modus stoppt sobald </code> vollständig im Response.
        // Das verhindert den Halluzinations-Loop nach dem Code-Block.
        // Analogie AVR: UART-Empfang stoppt bei ETX-Byte, nicht erst bei Puffer-Ende.
        if (profile == SamplerProfile::Execute &&
            fullResponse.contains(CODE_STOP))
            break;

        llama_batch nextBatch = llama_batch_get_one(&newToken, 1);
        if (llama_decode(AS_CTX(m_ctx), nextBatch) != 0) {
            emit errorOccurred(
                QString("llama_decode fehlgeschlagen nach %1 Tokens.").arg(i));
            break;
        }
    }

    emit generationDone(fullResponse);
}

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

void LlamaWorker::stopGeneration()
{
    m_stopFlag.store(true);
}

QString LlamaWorker::tokenToString(int tokenId) const
{
    const llama_vocab *vocab = llama_model_get_vocab(AS_MODEL(m_model));
    char buf[256] = {0};
    int len = llama_token_to_piece(vocab, tokenId, buf, sizeof(buf) - 1, 0, false);
    if (len <= 0) return {};
    return QString::fromUtf8(buf, len);
}
