#include "LlamaWorker.h"

#include "llama.h"
#include "ggml.h"

#include <QDebug>
#include <vector>

#define AS_MODEL(p)   reinterpret_cast<llama_model*>(p)
#define AS_CTX(p)     reinterpret_cast<llama_context*>(p)
#define AS_SAMPLER(p) reinterpret_cast<llama_sampler*>(p)

// Hinweis: GBNF Grammar fuer Tool-Calls wurde versucht (llama_sampler_init_grammar_lazy_patterns),
// aber die Lazy-Grammar-API ist fuer Qwen3 + Thinking noch nicht stabil.
// Der Trigger-Token wird selbst durch die Grammar gejagt und loest einen Assert aus.
// Stattdessen: Post-hoc JSON-Validierung in MainWindow (QJsonDocument::fromJson).

LlamaWorker::LlamaWorker(QObject *parent)
    : QObject(parent)
{}

LlamaWorker::~LlamaWorker()
{
    cleanup();
}

// ─── cleanup ─────────────────────────────────────────────────────────────────
// Gibt alle llama.cpp Ressourcen frei. Reihenfolge: Sampler → Context → Model.
// m_sampler ist nur ein Zeiger (kein Owner) — nicht freigeben.
void LlamaWorker::cleanup()
{
    m_sampler = nullptr;

    if (m_samplerChat) {
        llama_sampler_free(AS_SAMPLER(m_samplerChat));
        m_samplerChat = nullptr;
    }
    if (m_samplerTool) {
        llama_sampler_free(AS_SAMPLER(m_samplerTool));
        m_samplerTool = nullptr;
    }
    if (m_ctx) {
        llama_free(AS_CTX(m_ctx));
        m_ctx = nullptr;
    }
    if (m_model) {
        llama_model_free(AS_MODEL(m_model));
        m_model = nullptr;
    }
}

// ─── buildChatSampler ────────────────────────────────────────────────────────
// Chat-Profil: breit und kreativ.
// Kein Grammar-Zustand — reset() setzt nur Dist-RNG und eventuelle
// Penalties zurueck. Schnell und einfach.
//
// Chain: Top-K 40 → Temp 0.7 → Top-P 0.95 → Min-P 0.05 → Dist(seed=42)
void *LlamaWorker::buildChatSampler()
{
    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);

    llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(42));

    return chain;
}

// ─── buildToolSampler ────────────────────────────────────────────────────────
// Tool-Profil: eng und deterministisch.
//
// Chain: Top-K 20 → Temp 0.1 → Top-P 0.50 → Min-P 0.05 → Dist(seed=1337)
//
// Warum keine Grammar?
// llama_sampler_init_grammar_lazy_patterns ist fuer Qwen3 + Thinking noch
// nicht stabil — der Trigger-Token selbst wird durch die Grammar gejagt
// und loest einen GGML_ASSERT aus ("Unexpected empty grammar stack").
// Die llama.cpp-Entwickler bestaetigen dass es noch keinen zuverlaessigen
// "Anti-Trigger" gibt um Grammar nach </tool_call> wieder zu deaktivieren.
//
// Stattdessen: JSON-Validierung nach der Generierung in MainWindow.cpp
// (QJsonDocument::fromJson). Bei Fehler: Continuation-Mechanismus greift.
// Das ist robuster als Grammar-Enforcement waehrend der Generierung.
void *LlamaWorker::buildToolSampler()
{
    llama_sampler_chain_params p = llama_sampler_chain_default_params();
    llama_sampler *chain = llama_sampler_chain_init(p);

    llama_sampler_chain_add(chain, llama_sampler_init_top_k(20));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(0.1f));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.50f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(1337));

    return chain;
}

// ─── initialize ──────────────────────────────────────────────────────────────
// Laedt Modell, erstellt Context, baut beide Sampler einmalig.
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

    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx           = 128 * 1024;
    ctxParams.n_batch         = 512;
    ctxParams.n_ubatch        = 512;
    ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;

    m_ctx = llama_init_from_model(AS_MODEL(m_model), ctxParams);
    if (!m_ctx) {
        emit errorOccurred("Kontext konnte nicht erstellt werden.");
        cleanup();
        return;
    }

    // Beide Sampler einmalig bauen. Leben bis cleanup().
    m_samplerChat = buildChatSampler();
    m_samplerTool = buildToolSampler();
    m_sampler     = m_samplerChat;  // Default: Chat

    m_initialized = true;
    emit modelLoaded();
}

// ─── generate ────────────────────────────────────────────────────────────────
// Ablauf:
//   1. Sampler-Profil per Pointer-Swap setzen
//   2. llama_sampler_reset() — Zustand des aktiven Samplers zuruecksetzen
//      (Dist-RNG, Lazy-Grammar-Trigger-Zustand, etc.)
//   3. Tokenisieren, Overflow-Check, KV-Cache leeren
//   4. Prompt dekodieren, dann Token-Streaming-Loop
//
// Reset-Strategie (Pattern: Reset before reuse):
//   Jeder Sampler wird vor der Nutzung resettet, nicht danach.
//   Vorteil: falls generate() vorzeitig abbricht (StopFlag, Fehler),
//   hinterlaesst er keinen schmutzigen Zustand — der naechste Aufruf
//   resettet sowieso.
void LlamaWorker::generate(const QString &prompt, LlamaWorker::SamplerProfile profile)
{
    if (!m_initialized) {
        emit errorOccurred("Worker nicht initialisiert.");
        return;
    }

    m_stopFlag.store(false);

    // ─── Profil umschalten ────────────────────────────────────────────────
    // Pointer-Swap: O(1), kein Alloc, kein Free.
    switch (profile) {
        case SamplerProfile::Tool:
            m_sampler = m_samplerTool;
            break;
        case SamplerProfile::Chat:
        default:
            m_sampler = m_samplerChat;
            break;
    }

    // Reset: setzt alle Chain-Glieder auf Anfangszustand.
    // Fuer Chat: Dist-RNG zurueck auf Seed 42.
    // Fuer Tool: Dist-RNG zurueck auf Seed 1337, Lazy-Grammar wieder schlafend.
    // llama_sampler_reset() propagiert durch die gesamte Chain.
    llama_sampler_reset(AS_SAMPLER(m_sampler));

    // ─── Tokenisierung ────────────────────────────────────────────────────
    const std::string promptStr = prompt.toStdString();
    const llama_vocab *vocab    = llama_model_get_vocab(AS_MODEL(m_model));

    int maxTokens = static_cast<int>(promptStr.size()) + 128;
    std::vector<llama_token> promptTokens(maxTokens);

    int nTokens = llama_tokenize(
        vocab,
        promptStr.c_str(),
        static_cast<int32_t>(promptStr.size()),
        promptTokens.data(),
        static_cast<int32_t>(promptTokens.size()),
        true, true
    );

    if (nTokens < 0) {
        promptTokens.resize(-nTokens);
        llama_tokenize(vocab, promptStr.c_str(),
                       static_cast<int32_t>(promptStr.size()),
                       promptTokens.data(),
                       static_cast<int32_t>(promptTokens.size()),
                       true, true);
        nTokens = static_cast<int>(promptTokens.size());
    }
    promptTokens.resize(nTokens);

    // ─── Context-Overflow-Check ───────────────────────────────────────────
    const int n_ctx   = llama_n_ctx(AS_CTX(m_ctx));
    const int n_batch = llama_n_batch(AS_CTX(m_ctx));

    if (nTokens >= n_ctx) {
        emit errorOccurred(
            QString("Context-Overflow: Prompt hat %1 Tokens, Context-Window ist %2. "
                    "Chat-History mit 'Loeschen' zuruecksetzen.")
            .arg(nTokens).arg(n_ctx));
        return;
    }

    emit statsUpdate(nTokens, n_ctx);

    if ((n_ctx - nTokens) < n_ctx / 10) {
        emit tokenGenerated(
            QString("[WARNUNG: Kontext fast voll: %1 Tokens verbleiben]\n")
            .arg(n_ctx - nTokens));
    }

    // ─── KV-Cache leeren + Prompt dekodieren ─────────────────────────────
    // Kein llama_sampler_accept() fuer Prompt-Tokens noetig — der
    // Lazy-Grammar-Sampler braucht das nicht (im Gegensatz zum normalen
    // Grammar-Sampler der wissen muss wo in der Grammar er startet).
    llama_memory_t mem = llama_get_memory(AS_CTX(m_ctx));
    llama_memory_clear(mem, false);

    int processed = 0;
    while (processed < nTokens) {
        int chunkSize = std::min(n_batch, nTokens - processed);
        llama_batch chunk = llama_batch_get_one(
            promptTokens.data() + processed, chunkSize);
        if (llama_decode(AS_CTX(m_ctx), chunk) != 0) {
            emit errorOccurred(
                QString("llama_decode fehlgeschlagen (Prompt-Chunk bei Token %1/%2).")
                .arg(processed).arg(nTokens));
            return;
        }
        processed += chunkSize;
        if (m_stopFlag.load()) return;
    }

    // ─── Decode-Loop (Token-Streaming) ────────────────────────────────────
    // Im Tool-Profil: Stop-Sequenz "</tool_call>" erkennen und danach
    // sofort aufhoeren. Das verhindert dass das Modell nach dem JSON-Block
    // noch weitergeneriert und Muell produziert.
    //
    // Implementierung als Suffix-Check auf fullResponse:
    //   Nach jedem Token pruefen ob fullResponse mit "</tool_call>" endet.
    //   Das ist O(k) pro Token (k = Laenge des Stop-Strings) — vernachlaessigbar.
    //
    // Analogie AVR: wie ein UART-Empfaenger der auf ein Endezeichen wartet
    // und dann den Puffer schliesst.
    static const QString TOOL_STOP = "</tool_call>";

    QString fullResponse;
    const int maxNewTokens = 8192;

    for (int i = 0; i < maxNewTokens; ++i) {
        if (m_stopFlag.load()) break;

        llama_token newToken = llama_sampler_sample(
            AS_SAMPLER(m_sampler),
            AS_CTX(m_ctx),
            -1
        );

        if (llama_vocab_is_eog(vocab, newToken)) break;

        QString tokenStr = tokenToString(newToken);
        fullResponse += tokenStr;
        emit tokenGenerated(tokenStr);

        // Stop-Sequenz im Tool-Profil: nach </tool_call> aufhoeren.
        // endsWith() ist effizienter als contains() — prueft nur das Ende.
        if (profile == SamplerProfile::Tool && fullResponse.endsWith(TOOL_STOP))
            break;

        llama_batch nextBatch = llama_batch_get_one(&newToken, 1);
        if (llama_decode(AS_CTX(m_ctx), nextBatch) != 0) {
            emit errorOccurred(
                QString("llama_decode fehlgeschlagen nach %1 generierten Tokens "
                        "(Prompt: %2, Gesamt: %3/%4). Context-Overflow?")
                .arg(i).arg(nTokens).arg(nTokens + i).arg(n_ctx));
            break;
        }
    }

    emit generationDone(fullResponse);
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
