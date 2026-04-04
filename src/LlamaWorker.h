#pragma once
#include <QThread>
#include <QString>
#include <atomic>

// ─── LlamaWorker ──────────────────────────────────────────────────────────────
// Führt llama.cpp Inference in einem separaten Thread aus.
//
// WARUM ein eigener Thread?
//   llama_decode() ist eine blocking-Funktion — sie läuft so lange bis alle
//   Tokens generiert sind. Im GUI-Thread würde das die UI einfrieren.
//   Der Worker-Thread generiert Token für Token und schickt jeden via Signal
//   an den GUI-Thread. Das ist das klassische Producer/Consumer Pattern mit
//   Qt Signals/Slots als thread-sichere Kommunikation.
//
// Pattern: Active Object — ein Objekt das seine eigene Ausführungseinheit
//          (Thread) besitzt und Anfragen asynchron abarbeitet.
//
// Lebenszyklus:
//   1. LlamaWorker erzeugen
//   2. moveToThread(&m_thread) — Objekt lebt im Worker-Thread
//   3. m_thread.start()
//   4. generate() via Signal aufrufen → läuft im Worker-Thread
//   5. tokenGenerated() Signals kommen im Worker-Thread an,
//      Qt überträgt sie thread-sicher in den GUI-Thread
class LlamaWorker : public QObject {
    Q_OBJECT

public:
    explicit LlamaWorker(QObject *parent = nullptr);
    ~LlamaWorker() override;

public slots:
    // Initialisiert llama.cpp (Modell laden, Kontext erstellen).
    // Muss als erstes aufgerufen werden.
    void initialize(const QString &modelPath);

    // Startet Inference für den gegebenen Prompt.
    // Emittiert tokenGenerated() für jeden Token, dann generationDone().
    void generate(const QString &prompt);

    // Bricht laufende Generierung ab (thread-sicher via atomic flag)
    void stopGeneration();

signals:
    // Wird für jeden generierten Token emittiert (Streaming)
    void tokenGenerated(const QString &token);

    // Wird nach vollständiger Antwort emittiert
    void generationDone(const QString &fullResponse);

    // Fehlermeldung
    void errorOccurred(const QString &error);

    // Modell erfolgreich geladen
    void modelLoaded();

private:
    // llama.cpp Handles — als void* damit der Header kein llama.h braucht
    // (Forward-Declaration funktioniert bei typedef struct nicht direkt)
    void *m_model   = nullptr;   // llama_model*
    void *m_ctx     = nullptr;   // llama_context*
    void *m_sampler = nullptr;   // llama_sampler*

    // Atomic Flag für thread-sicheren Abbruch.
    // std::atomic<bool> kann von einem anderen Thread gesetzt werden
    // ohne Mutex — das ist genau der Anwendungsfall hier.
    std::atomic<bool> m_stopFlag{false};

    bool m_initialized = false;

    // Hilfsfunktion: Token-ID → String
    QString tokenToString(int tokenId) const;

    void cleanup();
};
