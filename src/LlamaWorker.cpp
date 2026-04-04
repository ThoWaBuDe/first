#include "LlamaWorker.h"

// llama.h hier einbinden — nicht im Header (sauberere Abhängigkeiten)
#include "llama.h"
#include "ggml.h"

#include <QDebug>
#include <vector>

// Cast-Helfer: void* → konkreter llama Typ
// Vermeidet reinterpret_cast-Wiederholung im Code.
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

// ─── cleanup ─────────────────────────────────────────────────────────────────
// Gibt llama.cpp Ressourcen in umgekehrter Reihenfolge frei.
// Wichtig: Sampler vor Context vor Model.
void LlamaWorker::cleanup()
{
    if (m_sampler) {
        llama_sampler_free(AS_SAMPLER(m_sampler));
        m_sampler = nullptr;
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

// ─── initialize ──────────────────────────────────────────────────────────────
// Lädt das GGUF-Modell und erstellt den Inference-Kontext.
// Wird im Worker-Thread ausgeführt (via Signal-Slot Verbindung).
void LlamaWorker::initialize(const QString &modelPath)
{
    cleanup(); // Falls neu initialisiert wird

    // llama.cpp Backend initialisieren (CUDA wird automatisch erkannt)
    llama_backend_init();

    // ─── Modell-Parameter ────────────────────────────────────────────────
    llama_model_params modelParams = llama_model_default_params();
    // GPU-Offload: alle Layer auf GPU (-1 = alle)
    modelParams.n_gpu_layers = -1;

    m_model = llama_model_load_from_file(modelPath.toLocal8Bit().constData(), modelParams);
    if (!m_model) {
        emit errorOccurred(QString("Modell konnte nicht geladen werden: %1").arg(modelPath));
        return;
    }

    // ─── Kontext-Parameter ───────────────────────────────────────────────
    llama_context_params ctxParams = llama_context_default_params();
    ctxParams.n_ctx    = 262144;   // Context-Window: 8K Tokens
    ctxParams.n_batch  = 512;    // Tokens pro Batch beim Prompt-Processing
    ctxParams.n_ubatch = 512;    // Micro-Batch für CUDA
    // flash_attn_type ist ein enum in Build 8626+
    // LLAMA_ATTENTION_TYPE_FLASH = Flash Attention (schneller auf Ada GPU)
    // LLAMA_ATTENTION_TYPE_NON_FLASH = Standard
    ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;

    // llama_new_context_with_model() ist deprecated seit Build ~8500
    // Nachfolger: llama_init_from_model() — gleiche Parameter, neuer Name
    m_ctx = llama_init_from_model(AS_MODEL(m_model), ctxParams);
    if (!m_ctx) {
        emit errorOccurred("Kontext konnte nicht erstellt werden.");
        cleanup();
        return;
    }

    // ─── Sampler-Chain ───────────────────────────────────────────────────
    // Der Sampler entscheidet welcher Token als nächstes gewählt wird.
    // Pattern: Chain of Responsibility — mehrere Filter hintereinandergeschaltet.
    //
    //   Temperature → Top-P → Min-P → Greedy
    //   |              |        |       |
    //   Skaliert     Filtert  Filtert  Wählt den
    //   Logits       Top-P%   unwahrsch. wahrscheinlichsten
    //                Tokens   Tokens    Token

    llama_sampler_chain_params samplerChainParams = llama_sampler_chain_default_params();
    m_sampler = llama_sampler_chain_init(samplerChainParams);

    // Temperatur 0.7: guter Mittelweg zwischen kreativ und kohärent
    llama_sampler_chain_add(AS_SAMPLER(m_sampler),
        llama_sampler_init_temp(0.7f));

    // Top-P 0.9: ignoriert die untersten 10% der Token-Wahrscheinlichkeiten
    llama_sampler_chain_add(AS_SAMPLER(m_sampler),
        llama_sampler_init_top_p(0.9f, 1));

    // Min-P 0.05: filtert Tokens raus die < 5% des Top-Token-Werts haben
    llama_sampler_chain_add(AS_SAMPLER(m_sampler),
        llama_sampler_init_min_p(0.05f, 1));

    // Dist-Sampler: zieht zufällig aus den verbleibenden Tokens (mit Seed)
    llama_sampler_chain_add(AS_SAMPLER(m_sampler),
        llama_sampler_init_dist(42));

    m_initialized = true;
    emit modelLoaded();
}

// ─── generate ────────────────────────────────────────────────────────────────
// Hauptfunktion: Tokenisiert den Prompt, führt Inference durch, streamt Tokens.
//
// Ablauf (analog zum AVR Interrupt-Service-Routine Konzept):
//   1. Prompt → Token-IDs (Tokenizer)
//   2. Token-IDs in den KV-Cache laden (llama_decode mit Batch)
//   3. Decode-Loop: Token samplen → emittieren → nächsten generieren
void LlamaWorker::generate(const QString &prompt)
{
    if (!m_initialized) {
        emit errorOccurred("Worker nicht initialisiert.");
        return;
    }

    m_stopFlag.store(false);

    // ─── Schritt 1: Tokenisierung ─────────────────────────────────────────
    // Der Prompt-String wird in eine Folge von Integer-IDs umgewandelt.
    // Jede ID entspricht einem "Token" (Wort-Teil, Zeichen, Sonderzeichen).
    const std::string promptStr = prompt.toStdString();
    const llama_vocab *vocab = llama_model_get_vocab(AS_MODEL(m_model));

    // Puffer-Größe schätzen: ~1.5x Zeichen als Tokens (Daumenregel)
    int maxTokens = static_cast<int>(promptStr.size()) + 128;
    std::vector<llama_token> promptTokens(maxTokens);

    int nTokens = llama_tokenize(
        vocab,
        promptStr.c_str(),
        static_cast<int32_t>(promptStr.size()),
        promptTokens.data(),
        static_cast<int32_t>(promptTokens.size()),
        /*add_special=*/true,    // BOS-Token hinzufügen
        /*parse_special=*/true   // Sonder-Tokens wie <|im_start|> erkennen
    );

    if (nTokens < 0) {
        // Puffer war zu klein — mit exakter Größe wiederholen
        promptTokens.resize(-nTokens);
        llama_tokenize(vocab, promptStr.c_str(),
                       static_cast<int32_t>(promptStr.size()),
                       promptTokens.data(),
                       static_cast<int32_t>(promptTokens.size()),
                       true, true);
        nTokens = static_cast<int>(promptTokens.size());
    }
    promptTokens.resize(nTokens);

    // ─── Schritt 2: Context-Overflow-Check ──────────────────────────────
    // Bevor wir den Prompt laden: prüfen ob er überhaupt in den KV-Cache passt.
    // n_ctx = konfiguriertes Context-Window (z.B. 262144)
    // nTokens = Tokens des gesamten Prompts (History + System-Prompt)
    // maxNewTokens = Tokens die wir noch generieren wollen
    //
    // Wenn Prompt + geplante Ausgabe > n_ctx → llama_decode() schlägt fehl.
    // Besser: frühzeitig abbrechen mit verständlicher Fehlermeldung.
    //
    // Analogie AVR: wie ein Stack-Overflow-Check vor einer rekursiven Funktion.
    const int n_ctx   = llama_n_ctx(AS_CTX(m_ctx));
    const int n_batch = llama_n_batch(AS_CTX(m_ctx));

    if (nTokens >= n_ctx) {
        emit errorOccurred(
            QString("Context-Overflow: Prompt hat %1 Tokens, "
                    "Context-Window ist %2. "
                    "Chat-History mit 'Loeschen' zuruecksetzen.")
            .arg(nTokens).arg(n_ctx));
        return;
    }

    // Warnung wenn weniger als 10% des Context-Windows für Ausgabe bleiben
    int remaining = n_ctx - nTokens;
    if (remaining < n_ctx / 10) {
        emit tokenGenerated(QString("[WARNUNG: Kontext fast voll: %1 Tokens verbleiben - Antwort abgeschnitten]\n").arg(remaining));
    }

    // ─── KV-Cache leeren + Prompt einlesen ───────────────────────────────
    // Build 8626: KV-Cache ist jetzt llama_memory_t (abstrahiertes Interface)
    // data_only=false: kompletter Reset inkl. Sequenz-Metadaten
    llama_memory_t mem = llama_get_memory(AS_CTX(m_ctx));
    llama_memory_clear(mem, false);

    // ─── Prompt in n_batch-Chunks aufteilen ──────────────────────────────
    // llama_decode() akzeptiert maximal n_batch Tokens pro Aufruf.
    // Analogie AVR: TX-Puffer mit 64 Byte — man sendet in Chunks.
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

    // ─── Schritt 3: Decode-Loop (Token-Streaming) ─────────────────────────
    // Analogie zur AVR-Welt: Das ist wie ein ISR der immer wieder triggert,
    // einen Wert aus einem Register liest und ihn weiterverarbeitet.
    //
    // Jede Iteration:
    //   a) Sampler wählt den nächsten Token (aus den Logits des letzten Decode)
    //   b) Ist es ein EOG-Token (End of Generation)? → Stop
    //   c) Token → String → Signal emittieren
    //   d) Token in neuen Batch packen → llama_decode → nächste Logits
    QString fullResponse;
    // Token-Limit für die Generierung.
    // 2048 war zu wenig für lange Tool-Call JSON-Blöcke — ein write_file
    // mit 200 Zeilen Code verbraucht leicht 1500-2000 Token nur für den Content.
    // 8192 gibt genug Puffer auch für große Antworten.
    const int maxNewTokens = 8192;

    for (int i = 0; i < maxNewTokens; ++i) {

        // Abbruch-Check: stopFlag kann vom GUI-Thread gesetzt werden
        if (m_stopFlag.load()) break;

        // Token samplen
        llama_token newToken = llama_sampler_sample(
            AS_SAMPLER(m_sampler),
            AS_CTX(m_ctx),
            -1  // -1 = letzter Token im Kontext
        );

        // EOG = End of Generation (EOS, <|im_end|>, etc.)
        if (llama_vocab_is_eog(vocab, newToken)) break;

        // Token-ID → String
        QString tokenStr = tokenToString(newToken);
        fullResponse += tokenStr;

        // Token an GUI-Thread senden (thread-sicherer Signal-Slot Mechanismus)
        emit tokenGenerated(tokenStr);

        // Neuen Token in den Kontext einlesen für nächsten Decode-Schritt
        llama_batch nextBatch = llama_batch_get_one(&newToken, 1);
        if (llama_decode(AS_CTX(m_ctx), nextBatch) != 0) {
            // Häufigste Ursache: KV-Cache voll (nTokens + i >= n_ctx).
            // Der Overflow-Check oben prüft nur den Prompt — bei sehr langen
            // Antworten kann der Cache während der Generierung volllaufen.
            int totalUsed = nTokens + i;
            emit errorOccurred(
                QString("llama_decode fehlgeschlagen nach %1 generierten Tokens "
                        "(Prompt: %2, Gesamt: %3/%4 Tokens). "
                        "Wahrscheinlich Context-Overflow - Chat zuruecksetzen.")
                .arg(i).arg(nTokens).arg(totalUsed).arg(n_ctx));
            // Generierung trotzdem sauber beenden — bisherige Antwort ausgeben
            break;
        }
    }

    emit generationDone(fullResponse);
}

// ─── stopGeneration ──────────────────────────────────────────────────────────
// Kann vom GUI-Thread aufgerufen werden — thread-sicher durch atomic.
void LlamaWorker::stopGeneration()
{
    m_stopFlag.store(true);
}

// ─── tokenToString ───────────────────────────────────────────────────────────
// Wandelt eine Token-ID in einen UTF-8 String um.
// llama_token_to_piece() füllt einen char-Puffer mit dem Stück.
QString LlamaWorker::tokenToString(int tokenId) const
{
    const llama_vocab *vocab = llama_model_get_vocab(AS_MODEL(m_model));
    char buf[256] = {0};
    int len = llama_token_to_piece(vocab, tokenId, buf, sizeof(buf) - 1,
                                    /*lstrip=*/0, /*special=*/false);
    if (len <= 0) return {};
    return QString::fromUtf8(buf, len);
}
