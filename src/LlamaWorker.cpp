#include "LlamaWorker.h"
#include "ChatModel.h"
#include "AppConfig.h"

#include "llama.h"
#include "ggml.h"

#include <QDebug>
#include <vector>
#include <string>
#include <numeric>  // für std::accumulate

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
QString LlamaWorker::applyTemplate(const QVector<ChatMessage> &messages) const
{
    if (!m_model) return {};

    // ── Stufe 1: llama_chat_apply_template ───────────────────────────────

    std::vector<std::string>         contents;
    std::vector<llama_chat_message>  msgs;
    contents.reserve(messages.size());
    msgs.reserve(messages.size());

    for (const ChatMessage &m : messages) {
        contents.push_back(m.content.toStdString());
        msgs.push_back({ roleToStr(m.role), contents.back().c_str() });
    }

    // Template aus dem Modell extrahieren
    const char * tmpl = llama_model_chat_template(AS_MODEL(m_model), nullptr);

    size_t totalContent = std::accumulate(
        contents.begin(), contents.end(), size_t(0),
        [](size_t sum, const std::string &s) { return sum + s.size(); });
    size_t bufSize = totalContent * 2 + 1024;

    std::vector<char> buf(bufSize);

    // KORREKTUR: Erster Parameter ist das Template (const char*), nicht das Model
    int result = llama_chat_apply_template(
        tmpl,
        msgs.data(),
        msgs.size(),
        true,         // add_ass: öffne Assistant-Turn am Ende
        buf.data(),
        static_cast<int32_t>(buf.size())
        );

    if (result > 0) {
        return QString::fromUtf8(buf.data(), result);
    }

    // Fallback bei zu kleinem Puffer
    if (result < 0 && bufSize < 512 * 1024) {
        bufSize = 512 * 1024;
        buf.resize(bufSize);
        result = llama_chat_apply_template(
            tmpl,
            msgs.data(), msgs.size(),
            true, buf.data(), static_cast<int32_t>(buf.size())
            );
        if (result > 0)
            return QString::fromUtf8(buf.data(), result);
    }

    // ── Stufe 2: Fallback auf ChatTemplate ───────────────────────────────
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

// ─── generate ────────────────────────────────────────────────────────────────
void LlamaWorker::generate(const QVector<ChatMessage> &messages,
                           LlamaWorker::SamplerProfile profile)
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

    QString promptQStr = applyTemplate(messages);
    if (promptQStr.isEmpty()) {
        emit errorOccurred("Prompt konnte nicht gebaut werden.");
        return;
    }

    const std::string promptStr = promptQStr.toStdString();
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
