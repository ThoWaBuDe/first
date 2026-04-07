#include "LlamaWorker.h"
#include "AppConfig.h"

#include "llama.h"
#include "ggml.h"

#include <QDebug>
#include <vector>

#define AS_MODEL(p)   reinterpret_cast<llama_model*>(p)
#define AS_CTX(p)     reinterpret_cast<llama_context*>(p)
#define AS_SAMPLER(p) reinterpret_cast<llama_sampler*>(p)

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

// ─── buildChatSampler ────────────────────────────────────────────────────────
// Liest Parameter aus AppConfig — werden beim nächsten rebuild() übernommen.
// Chain: Top-K → Temp → Top-P → Min-P → Dist
void *LlamaWorker::buildChatSampler()
{
    const AppConfig &cfg = AppConfig::instance();

    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);

    llama_sampler_chain_add(chain, llama_sampler_init_top_k(cfg.chatTopK()));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(cfg.chatTemp()));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(cfg.chatTopP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(cfg.chatMinP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(42));

    return chain;
}

// ─── buildToolSampler ────────────────────────────────────────────────────────
void *LlamaWorker::buildToolSampler()
{
    const AppConfig &cfg = AppConfig::instance();

    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);

    llama_sampler_chain_add(chain, llama_sampler_init_top_k(cfg.toolTopK()));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(cfg.toolTemp()));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(cfg.toolTopP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(cfg.toolMinP(), 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(1337));

    return chain;
}

// ─── rebuildSamplers ─────────────────────────────────────────────────────────
// Wird vom Config-Dialog aufgerufen wenn Sampler-Parameter geändert wurden.
// Gibt die alten Sampler frei und baut neue mit den aktuellen AppConfig-Werten.
// Muss im Worker-Thread aufgerufen werden (via QMetaObject::invokeMethod).
void LlamaWorker::rebuildSamplers()
{
    if (m_samplerChat) { llama_sampler_free(AS_SAMPLER(m_samplerChat)); m_samplerChat = nullptr; }
    if (m_samplerTool) { llama_sampler_free(AS_SAMPLER(m_samplerTool)); m_samplerTool = nullptr; }

    m_samplerChat = buildChatSampler();
    m_samplerTool = buildToolSampler();
    m_sampler     = m_samplerChat;  // default zurücksetzen

    emit samplersRebuilt();
}

// ─── initialize ──────────────────────────────────────────────────────────────
void LlamaWorker::initialize(const QString &modelPath)
{
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
    emit modelLoaded();
}

// ─── generate ────────────────────────────────────────────────────────────────
void LlamaWorker::generate(const QString &prompt, LlamaWorker::SamplerProfile profile)
{
    if (!m_initialized) {
        emit errorOccurred("Worker nicht initialisiert.");
        return;
    }

    m_stopFlag.store(false);

    switch (profile) {
        case SamplerProfile::Tool: m_sampler = m_samplerTool; break;
        default:                   m_sampler = m_samplerChat; break;
    }

    llama_sampler_reset(AS_SAMPLER(m_sampler));

    const std::string promptStr = prompt.toStdString();
    const llama_vocab *vocab    = llama_model_get_vocab(AS_MODEL(m_model));

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

    llama_memory_t mem = llama_get_memory(AS_CTX(m_ctx));
    llama_memory_clear(mem, false);

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

    static const QString TOOL_STOP = "</tool_call>";
    QString fullResponse;
    const int maxNewTokens = 8192;

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
