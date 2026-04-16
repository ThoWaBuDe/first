#pragma once
#include <QDialog>
#include <QTabWidget>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>
#include "ChatTemplate.h"

// ─── ConfigDialog ─────────────────────────────────────────────────────────────
// Modaler QDialog mit Tabs für alle AppConfig-Parameter.
//
// Tabs:
//   Modell           — Pfad, n_ctx, Batch-Size
//   Sampler          — Chat und Tool Profile
//   Agent            — Schwellen, Continuations, Token-Budget
//   Logging          — Chat-Log, Tavily API Key
//   Template & Prompt — Chat-Template ComboBox + User System-Prompt  ← NEU
class ConfigDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConfigDialog(QWidget *parent = nullptr);

signals:
    void samplersChanged();
    void restartRequired();
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
    QWidget *createTemplateTab();   // ← NEU

    void loadFromConfig();
    void saveToConfig();

    QDoubleSpinBox *makeDoubleSpinBox(double min, double max, double step,
                                      int decimals, double value);
    QSpinBox       *makeSpinBox(int min, int max, int value);

    // ─── Modell Tab ───────────────────────────────────────────────────────
    QLineEdit *m_modelPath;
    QSpinBox  *m_contextSize;
    QSpinBox  *m_batchSize;
    QLabel    *m_restartHint;

    // ─── Sampler Tab ──────────────────────────────────────────────────────
    QSpinBox       *m_chatTopK;
    QDoubleSpinBox *m_chatTemp;
    QDoubleSpinBox *m_chatTopP;
    QDoubleSpinBox *m_chatMinP;
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

    // ─── Template & Prompt Tab ────────────────────────────────────────────
    QComboBox *m_chatTemplateCombo;     // Preset-Auswahl (Auto/ChatML/Llama3/...)
    QLabel    *m_detectedTemplateLabel; // zeigt was aus GGUF erkannt wurde
    QTextEdit *m_customTemplateEdit;    // nur aktiv bei "Custom"
    QLabel    *m_customTemplateLabel;   // Label dazu (zusammen ein/ausblenden)
    QTextEdit *m_userSystemPrompt;      // freier User-Text vor MCP-Prompts

    // Welche Gruppen wurden verändert
    bool m_samplersChanged  = false;
    bool m_restartNeeded    = false;
    bool m_loggingChanged   = false;
};
