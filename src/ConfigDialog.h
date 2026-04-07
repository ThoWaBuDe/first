#pragma once
#include <QDialog>
#include <QTabWidget>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>

// ─── ConfigDialog ─────────────────────────────────────────────────────────────
// Modaler QDialog mit Tabs für alle AppConfig-Parameter.
//
// Tabs:
//   Modell   — Pfad, n_ctx, Batch-Size, Modell-Browser
//   Sampler  — Chat und Tool Profile nebeneinander
//   Agent    — Schwellen, Continuations, Token-Budget
//   Logging  — Chat-Log an/aus, Pfad, Tavily API Key
//
// Pattern: Model-View-Sync
//   loadFromConfig() → Widgets mit AppConfig-Werten befüllen
//   saveToConfig()   → Widget-Werte nach AppConfig schreiben
//   Klassen: "Sofort wirksam" vs "Neustart nötig" werden markiert
//
// Neustart-nötig Parameter bekommen ein ⚠ Icon und einen Hinweis.
// Sofort-wirksam Parameter werden direkt in AppConfig geschrieben —
// der Agent übernimmt sie beim nächsten Aufruf.

class ConfigDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConfigDialog(QWidget *parent = nullptr);

signals:
    // Emittiert wenn Sampler-Parameter geändert wurden → LlamaWorker::rebuildSamplers()
    void samplersChanged();
    // Emittiert wenn Neustart-nötig Parameter geändert wurden
    void restartRequired();
    // Emittiert wenn Logging-Status geändert wurde
    void loggingChanged(bool enabled);

private slots:
    void onAccepted();
    void onBrowseModel();
    void onBrowseLogDir();
    void resetSamplerDefaults();

private:
    void setupUi();
    QWidget *createModelTab();
    QWidget *createSamplerTab();
    QWidget *createAgentTab();
    QWidget *createLoggingTab();

    void loadFromConfig();
    void saveToConfig();

    // Helper: SpinBox mit Label erstellen
    QDoubleSpinBox *makeDoubleSpinBox(double min, double max, double step,
                                      int decimals, double value);
    QSpinBox       *makeSpinBox(int min, int max, int value);

    // ─── Modell Tab ───────────────────────────────────────────────────────
    QLineEdit *m_modelPath;
    QSpinBox  *m_contextSize;
    QSpinBox  *m_batchSize;
    QLabel    *m_restartHint;

    // ─── Sampler Tab ──────────────────────────────────────────────────────
    // Chat
    QSpinBox       *m_chatTopK;
    QDoubleSpinBox *m_chatTemp;
    QDoubleSpinBox *m_chatTopP;
    QDoubleSpinBox *m_chatMinP;
    // Tool
    QSpinBox       *m_toolTopK;
    QDoubleSpinBox *m_toolTemp;
    QDoubleSpinBox *m_toolTopP;
    QDoubleSpinBox *m_toolMinP;

    // ─── Agent Tab ────────────────────────────────────────────────────────
    QSpinBox *m_summarizeThreshold;
    QSpinBox *m_maxContinuations;
    QSpinBox *m_maxToolResultChars;
    QSpinBox *m_deadlockWarn;
    QSpinBox *m_deadlockRedirect;
    QSpinBox *m_deadlockAbort;

    // ─── Logging Tab ──────────────────────────────────────────────────────
    QCheckBox *m_chatLoggingEnabled;
    QLineEdit *m_chatLogDir;
    QLineEdit *m_tavilyApiKey;

    // Welche Gruppen wurden verändert (für Signals)
    bool m_samplersChanged  = false;
    bool m_restartNeeded    = false;
    bool m_loggingChanged   = false;
};
