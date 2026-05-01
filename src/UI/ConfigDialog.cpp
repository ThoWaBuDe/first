#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QLabel>
#include <QFrame>

#include "UI/ConfigDialog.h"
#include "Config/AppConfig.h"


ConfigDialog::ConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("LlamaQt — Einstellungen");
    setMinimumWidth(560);
    setModal(true);
    setupUi();
    loadFromConfig();
}

void ConfigDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(createModelTab(),    "🧠 Modell");
    tabs->addTab(createSamplerTab(),  "🎲 Sampler");
    tabs->addTab(createAgentTab(),    "🤖 Agent");
    tabs->addTab(createLoggingTab(),  "📝 Logging");
    tabs->addTab(createTemplateTab(), "💬 Template & Format");
    mainLayout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ConfigDialog::onAccepted);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults),
            &QPushButton::clicked, this, &ConfigDialog::resetSamplerDefaults);
    mainLayout->addWidget(buttons);
}

QWidget *ConfigDialog::createModelTab()
{
    auto *w      = new QWidget;
    auto *layout = new QVBoxLayout(w);
    layout->setSpacing(12);

    auto *pathGroup  = new QGroupBox("Modell-Pfad ⚠ (Neustart erforderlich)");
    auto *pathLayout = new QHBoxLayout(pathGroup);
    m_modelPath = new QLineEdit;
    m_modelPath->setPlaceholderText("/home/user/ai/models/model.gguf");
    auto *browseBtn = new QPushButton("Durchsuchen...");
    connect(browseBtn, &QPushButton::clicked, this, &ConfigDialog::onBrowseModel);
    pathLayout->addWidget(m_modelPath);
    pathLayout->addWidget(browseBtn);
    layout->addWidget(pathGroup);

    auto *ctxGroup  = new QGroupBox("Kontext ⚠ (Neustart erforderlich)");
    auto *ctxForm   = new QFormLayout(ctxGroup);
    m_contextSize = makeSpinBox(2048, 256*1024, 128*1024);
    m_contextSize->setSingleStep(1024);
    m_contextSize->setSuffix(" Tokens");
    ctxForm->addRow("Context-Größe:", m_contextSize);
    m_batchSize = makeSpinBox(64, 4096, 512);
    m_batchSize->setSuffix(" Tokens");
    ctxForm->addRow("Batch-Size:", m_batchSize);
    layout->addWidget(ctxGroup);

    m_restartHint = new QLabel(
        "<b>⚠ Diese Einstellungen werden erst nach dem nächsten Start wirksam.</b>");
    m_restartHint->setWordWrap(true);
    m_restartHint->setStyleSheet("color: #e37400; padding: 6px;");
    m_restartHint->hide();
    layout->addWidget(m_restartHint);
    layout->addStretch();

    connect(m_modelPath,   &QLineEdit::textChanged,
            this, [this]{ m_restartNeeded = true; m_restartHint->show(); });
    connect(m_contextSize, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this]{ m_restartNeeded = true; m_restartHint->show(); });
    connect(m_batchSize,   QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this]{ m_restartNeeded = true; m_restartHint->show(); });

    return w;
}

QWidget *ConfigDialog::createSamplerTab()
{
    auto *w      = new QWidget;
    auto *layout = new QVBoxLayout(w);
    layout->setSpacing(12);

    auto *hLayout = new QHBoxLayout;
    hLayout->setSpacing(12);

    // Chat
    auto *chatGroup = new QGroupBox("Chat  (Temp 0.7 — kreativ)");
    auto *chatForm  = new QFormLayout(chatGroup);
    m_chatTopK = makeSpinBox(1, 200, 40);
    m_chatTemp = makeDoubleSpinBox(0.0, 2.0, 0.05, 2, 0.7);
    m_chatTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.95);
    m_chatMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);
    chatForm->addRow("Top-K:", m_chatTopK);
    chatForm->addRow("Temp:",  m_chatTemp);
    chatForm->addRow("Top-P:", m_chatTopP);
    chatForm->addRow("Min-P:", m_chatMinP);
    hLayout->addWidget(chatGroup);

    // Execute
    auto *execGroup = new QGroupBox("Execute  (Temp 0.2 — Code)");
    auto *execBox   = new QVBoxLayout(execGroup);
    auto *execForm  = new QFormLayout;
    m_executeTopK = makeSpinBox(1, 100, 20);
    m_executeTemp = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.2);
    m_executeTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.6);
    m_executeMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);
    execForm->addRow("Top-K:", m_executeTopK);
    execForm->addRow("Temp:",  m_executeTemp);
    execForm->addRow("Top-P:", m_executeTopP);
    execForm->addRow("Min-P:", m_executeMinP);
    execBox->addLayout(execForm);
    auto *execHint = new QLabel(
        "<small style='color:#888'>Für Code-Generierung im Execute-Modus.</small>");
    execBox->addWidget(execHint);
    hLayout->addWidget(execGroup);

    // Tool
    auto *toolGroup = new QGroupBox("Tool  (Temp 0.1 — JSON)");
    auto *toolForm  = new QFormLayout(toolGroup);
    m_toolTopK = makeSpinBox(1, 100, 20);
    m_toolTemp = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.1);
    m_toolTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.5);
    m_toolMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);
    toolForm->addRow("Top-K:", m_toolTopK);
    toolForm->addRow("Temp:",  m_toolTemp);
    toolForm->addRow("Top-P:", m_toolTopP);
    toolForm->addRow("Min-P:", m_toolMinP);
    hLayout->addWidget(toolGroup);

    layout->addLayout(hLayout);

    auto markSamplers = [this]{ m_samplersChanged = true; };
    for (auto *sb : {m_chatTopK, m_executeTopK, m_toolTopK})
        connect(sb, QOverload<int>::of(&QSpinBox::valueChanged), this, markSamplers);
    for (auto *sb : {m_chatTemp, m_chatTopP, m_chatMinP,
                     m_executeTemp, m_executeTopP, m_executeMinP,
                     m_toolTemp, m_toolTopP, m_toolMinP})
        connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, markSamplers);

    layout->addStretch();
    return w;
}

QWidget *ConfigDialog::createAgentTab()
{
    auto *w      = new QWidget;
    auto *layout = new QVBoxLayout(w);
    layout->setSpacing(12);

    auto *ctxGroup = new QGroupBox("Kontext-Management");
    auto *ctxForm  = new QFormLayout(ctxGroup);
    m_summarizeThreshold = makeSpinBox(50, 99, 80);
    m_summarizeThreshold->setSuffix(" %");
    ctxForm->addRow("Auto-Summarize bei:", m_summarizeThreshold);
    layout->addWidget(ctxGroup);

    auto *genGroup = new QGroupBox("Generierungs-Limits");
    auto *genForm  = new QFormLayout(genGroup);
    m_maxContinuations = makeSpinBox(1, 10, 3);
    genForm->addRow("Max. Continuations:", m_maxContinuations);
    m_maxToolResultChars = makeSpinBox(1000, 50000, 6000);
    m_maxToolResultChars->setSingleStep(500);
    m_maxToolResultChars->setSuffix(" Zeichen");
    genForm->addRow("Max. Tool-Ergebnis:", m_maxToolResultChars);
    layout->addWidget(genGroup);

    auto *dlGroup = new QGroupBox("Deadlock-Eskalation");
    auto *dlForm  = new QFormLayout(dlGroup);
    m_deadlockWarn     = makeSpinBox(1, 20, 3);
    m_deadlockRedirect = makeSpinBox(1, 20, 5);
    m_deadlockAbort    = makeSpinBox(1, 20, 7);
    dlForm->addRow("Warnung nach:",   m_deadlockWarn);
    dlForm->addRow("Umleitung nach:", m_deadlockRedirect);
    dlForm->addRow("Abbruch nach:",   m_deadlockAbort);
    layout->addWidget(dlGroup);

    layout->addStretch();
    return w;
}

QWidget *ConfigDialog::createLoggingTab()
{
    auto *w      = new QWidget;
    auto *layout = new QVBoxLayout(w);
    layout->setSpacing(12);

    auto *logGroup  = new QGroupBox("Chat-Logging");
    auto *logLayout = new QVBoxLayout(logGroup);
    m_chatLoggingEnabled = new QCheckBox("Chat-Verlauf in Markdown-Datei speichern");
    logLayout->addWidget(m_chatLoggingEnabled);

    auto *dirLayout = new QHBoxLayout;
    m_chatLogDir = new QLineEdit;
    m_chatLogDir->setPlaceholderText("Standard: ~/llamatools/chat_log/");
    auto *browseDirBtn = new QPushButton("...");
    browseDirBtn->setFixedWidth(30);
    connect(browseDirBtn, &QPushButton::clicked, this, &ConfigDialog::onBrowseLogDir);
    dirLayout->addWidget(new QLabel("Log-Verzeichnis:"));
    dirLayout->addWidget(m_chatLogDir);
    dirLayout->addWidget(browseDirBtn);
    logLayout->addLayout(dirLayout);
    layout->addWidget(logGroup);

    auto *wsGroup  = new QGroupBox("Web-Search (Tavily)");
    auto *wsLayout = new QFormLayout(wsGroup);
    m_tavilyApiKey = new QLineEdit;
    m_tavilyApiKey->setEchoMode(QLineEdit::Password);
    m_tavilyApiKey->setPlaceholderText("tvly-...");
    auto *showKey = new QCheckBox("Anzeigen");
    connect(showKey, &QCheckBox::toggled, this, [this](bool show){
        m_tavilyApiKey->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto *keyLayout = new QHBoxLayout;
    keyLayout->addWidget(m_tavilyApiKey);
    keyLayout->addWidget(showKey);
    wsLayout->addRow("API Key:", keyLayout);
    layout->addWidget(wsGroup);

    layout->addStretch();
    connect(m_chatLoggingEnabled, &QCheckBox::toggled, this, [this]{ m_loggingChanged = true; });
    return w;
}

QWidget *ConfigDialog::createTemplateTab()
{
    auto *w      = new QWidget;
    auto *layout = new QVBoxLayout(w);
    layout->setSpacing(12);

    // ── Chat-Template ─────────────────────────────────────────────────────
    auto *tmplGroup  = new QGroupBox("Chat-Template ⚠ (Neustart erforderlich)");
    auto *tmplLayout = new QVBoxLayout(tmplGroup);

    m_chatTemplateCombo = new QComboBox;
    m_chatTemplateCombo->addItem(ChatTemplate::presetName(ChatTemplate::Preset::Auto),
                                  static_cast<int>(ChatTemplate::Preset::Auto));
    m_chatTemplateCombo->addItem(ChatTemplate::presetName(ChatTemplate::Preset::ChatML),
                                  static_cast<int>(ChatTemplate::Preset::ChatML));
    m_chatTemplateCombo->addItem(ChatTemplate::presetName(ChatTemplate::Preset::Llama3),
                                  static_cast<int>(ChatTemplate::Preset::Llama3));
    m_chatTemplateCombo->addItem(ChatTemplate::presetName(ChatTemplate::Preset::Gemma),
                                  static_cast<int>(ChatTemplate::Preset::Gemma));
    m_chatTemplateCombo->addItem(ChatTemplate::presetName(ChatTemplate::Preset::Mistral),
                                  static_cast<int>(ChatTemplate::Preset::Mistral));
    m_chatTemplateCombo->addItem(ChatTemplate::presetName(ChatTemplate::Preset::Custom),
                                  static_cast<int>(ChatTemplate::Preset::Custom));
    tmplLayout->addWidget(m_chatTemplateCombo);

    m_detectedTemplateLabel = new QLabel("Erkanntes Template: (Modell noch nicht geladen)");
    m_detectedTemplateLabel->setStyleSheet("color: #555; font-size: 11px; font-style: italic;");
    m_detectedTemplateLabel->setWordWrap(true);
    tmplLayout->addWidget(m_detectedTemplateLabel);

    m_customTemplateLabel = new QLabel("Custom Template (JSON):");
    m_customTemplateEdit  = new QTextEdit;
    m_customTemplateEdit->setMaximumHeight(110);
    m_customTemplateEdit->setAcceptRichText(false);
    m_customTemplateEdit->setFontFamily("monospace");
    m_customTemplateLabel->setVisible(false);
    m_customTemplateEdit->setVisible(false);
    tmplLayout->addWidget(m_customTemplateLabel);
    tmplLayout->addWidget(m_customTemplateEdit);

    connect(m_chatTemplateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        auto preset = static_cast<ChatTemplate::Preset>(
            m_chatTemplateCombo->currentData().toInt());
        bool isCustom = (preset == ChatTemplate::Preset::Custom);
        m_customTemplateLabel->setVisible(isCustom);
        m_customTemplateEdit->setVisible(isCustom);
        m_restartNeeded = true;
        m_restartHint->show();
    });

    layout->addWidget(tmplGroup);

    // ── Tool-Call-Format (NEU) ─────────────────────────────────────────────
    // Analog zu Chat-Template — Auto als Default, manuell überschreibbar.
    // Wichtiger Unterschied: Mistral-Arrays werden sequenziell ausgeführt.
    auto *toolFmtGroup  = new QGroupBox("Tool-Call-Format ⚠ (Neustart erforderlich)");
    auto *toolFmtLayout = new QVBoxLayout(toolFmtGroup);

    m_toolFormatCombo = new QComboBox;
    using P = ToolCallFormat::Preset;
    m_toolFormatCombo->addItem(ToolCallFormat::presetName(P::Auto),
                                static_cast<int>(P::Auto));
    m_toolFormatCombo->addItem(ToolCallFormat::presetName(P::QwenXmlTags),
                                static_cast<int>(P::QwenXmlTags));
    m_toolFormatCombo->addItem(ToolCallFormat::presetName(P::MistralNative),
                                static_cast<int>(P::MistralNative));
    m_toolFormatCombo->addItem(ToolCallFormat::presetName(P::Gemma4Google),
                                static_cast<int>(P::Gemma4Google));
    m_toolFormatCombo->addItem(ToolCallFormat::presetName(P::Llama3ToolUse),
                                static_cast<int>(P::Llama3ToolUse));
    m_toolFormatCombo->addItem(ToolCallFormat::presetName(P::Generic),
                                static_cast<int>(P::Generic));
    toolFmtLayout->addWidget(m_toolFormatCombo);

    m_detectedToolFormatLabel = new QLabel("Aktives Tool-Format: (Modell noch nicht geladen)");
    m_detectedToolFormatLabel->setStyleSheet("color: #555; font-size: 11px; font-style: italic;");
    m_detectedToolFormatLabel->setWordWrap(true);
    toolFmtLayout->addWidget(m_detectedToolFormatLabel);

    auto *fmtHint = new QLabel(
        "<small style='color:#888'>"
        "<b>Auto:</b> Erkennung aus Modellname + GGUF-Template (empfohlen).<br>"
        "<b>Mistral Native:</b> Unterstützt Arrays — mehrere Tool-Calls werden "
        "sequenziell ausgeführt.<br>"
        "<b>Gemma4/Google:</b> JSON in Code-Blöcken (```json)."
        "</small>");
    fmtHint->setWordWrap(true);
    toolFmtLayout->addWidget(fmtHint);

    connect(m_toolFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { m_restartNeeded = true; m_restartHint->show(); });

    layout->addWidget(toolFmtGroup);

    // ── User System-Prompt ─────────────────────────────────────────────────
    auto *promptGroup  = new QGroupBox("User System-Prompt  (wirksam nach /clear)");
    auto *promptLayout = new QVBoxLayout(promptGroup);

    m_userSystemPrompt = new QTextEdit;
    m_userSystemPrompt->setAcceptRichText(false);
    m_userSystemPrompt->setMinimumHeight(80);
    m_userSystemPrompt->setPlaceholderText(
        "Optionaler Text der dem Modell vorangestellt wird.\n\n"
        "Leer lassen für Standard-Verhalten.");
    promptLayout->addWidget(m_userSystemPrompt);

    auto *promptHint = new QLabel(
        "<small style='color:#888'>"
        "System-Prompt = Dieser Text + MCP Tool-Beschreibungen (automatisch).<br>"
        "Änderungen nach <b>/clear</b> oder Neustart aktiv."
        "</small>");
    promptHint->setWordWrap(true);
    promptLayout->addWidget(promptHint);

    layout->addWidget(promptGroup);
    layout->addStretch();

    return w;
}

void ConfigDialog::loadFromConfig()
{
    const AppConfig &cfg = AppConfig::instance();

    m_modelPath->setText(cfg.modelPath());
    m_contextSize->setValue(cfg.contextSize());
    m_batchSize->setValue(cfg.batchSize());

    m_chatTopK->setValue(cfg.chatTopK());
    m_chatTemp->setValue(cfg.chatTemp());
    m_chatTopP->setValue(cfg.chatTopP());
    m_chatMinP->setValue(cfg.chatMinP());

    m_executeTopK->setValue(cfg.executeTopK());
    m_executeTemp->setValue(cfg.executeTemp());
    m_executeTopP->setValue(cfg.executeTopP());
    m_executeMinP->setValue(cfg.executeMinP());

    m_toolTopK->setValue(cfg.toolTopK());
    m_toolTemp->setValue(cfg.toolTemp());
    m_toolTopP->setValue(cfg.toolTopP());
    m_toolMinP->setValue(cfg.toolMinP());

    m_summarizeThreshold->setValue(cfg.summarizeThreshold());
    m_maxContinuations->setValue(cfg.maxContinuations());
    m_maxToolResultChars->setValue(cfg.maxToolResultChars());
    m_deadlockWarn->setValue(cfg.deadlockWarn());
    m_deadlockRedirect->setValue(cfg.deadlockRedirect());
    m_deadlockAbort->setValue(cfg.deadlockAbort());

    m_chatLoggingEnabled->setChecked(cfg.chatLoggingEnabled());
    m_chatLogDir->setText(cfg.chatLogDir());
    m_tavilyApiKey->setText(cfg.tavilyApiKey());

    // Chat-Template
    int savedPreset = static_cast<int>(cfg.chatTemplatePreset());
    for (int i = 0; i < m_chatTemplateCombo->count(); ++i) {
        if (m_chatTemplateCombo->itemData(i).toInt() == savedPreset) {
            m_chatTemplateCombo->setCurrentIndex(i);
            break;
        }
    }
    m_customTemplateEdit->setPlainText(cfg.customChatTemplate());

    QString detected = cfg.detectedJinjaTemplate();
    if (detected.isEmpty()) {
        m_detectedTemplateLabel->setText("Erkanntes Template: (keines im GGUF)");
    } else {
        ChatTemplate::Preset p = ChatTemplate::detectFromJinja(detected);
        m_detectedTemplateLabel->setText(
            QString("Erkanntes Template: %1").arg(ChatTemplate::presetName(p)));
    }

    // Tool-Call-Format (NEU)
    int savedToolFmt = static_cast<int>(cfg.toolCallFormatPreset());
    for (int i = 0; i < m_toolFormatCombo->count(); ++i) {
        if (m_toolFormatCombo->itemData(i).toInt() == savedToolFmt) {
            m_toolFormatCombo->setCurrentIndex(i);
            break;
        }
    }
    ToolCallFormat::Preset effective = cfg.effectiveToolCallFormat();
    m_detectedToolFormatLabel->setText(
        QString("Aktives Tool-Format: %1")
        .arg(ToolCallFormat::presetName(effective)));

    m_userSystemPrompt->setPlainText(cfg.userSystemPrompt());

    m_samplersChanged = false;
    m_restartNeeded   = false;
    m_loggingChanged  = false;
    m_restartHint->hide();
}

void ConfigDialog::saveToConfig()
{
    AppConfig &cfg = AppConfig::instance();

    cfg.setModelPath(m_modelPath->text());
    cfg.setContextSize(m_contextSize->value());
    cfg.setBatchSize(m_batchSize->value());

    cfg.setChatTopK(m_chatTopK->value());
    cfg.setChatTemp(static_cast<float>(m_chatTemp->value()));
    cfg.setChatTopP(static_cast<float>(m_chatTopP->value()));
    cfg.setChatMinP(static_cast<float>(m_chatMinP->value()));

    cfg.setExecuteTopK(m_executeTopK->value());
    cfg.setExecuteTemp(static_cast<float>(m_executeTemp->value()));
    cfg.setExecuteTopP(static_cast<float>(m_executeTopP->value()));
    cfg.setExecuteMinP(static_cast<float>(m_executeMinP->value()));

    cfg.setToolTopK(m_toolTopK->value());
    cfg.setToolTemp(static_cast<float>(m_toolTemp->value()));
    cfg.setToolTopP(static_cast<float>(m_toolTopP->value()));
    cfg.setToolMinP(static_cast<float>(m_toolMinP->value()));

    cfg.setSummarizeThreshold(m_summarizeThreshold->value());
    cfg.setMaxContinuations(m_maxContinuations->value());
    cfg.setMaxToolResultChars(m_maxToolResultChars->value());

    int w = m_deadlockWarn->value();
    int r = qMax(m_deadlockRedirect->value(), w + 1);
    int a = qMax(m_deadlockAbort->value(),    r + 1);
    cfg.setDeadlockWarn(w);
    cfg.setDeadlockRedirect(r);
    cfg.setDeadlockAbort(a);

    cfg.setChatLoggingEnabled(m_chatLoggingEnabled->isChecked());
    cfg.setChatLogDir(m_chatLogDir->text());
    cfg.setTavilyApiKey(m_tavilyApiKey->text());

    auto chatPreset = static_cast<ChatTemplate::Preset>(
        m_chatTemplateCombo->currentData().toInt());
    cfg.setChatTemplatePreset(chatPreset);
    cfg.setCustomChatTemplate(m_customTemplateEdit->toPlainText());

    // Tool-Call-Format (NEU)
    auto toolFmt = static_cast<ToolCallFormat::Preset>(
        m_toolFormatCombo->currentData().toInt());
    cfg.setToolCallFormatPreset(toolFmt);

    cfg.setUserSystemPrompt(m_userSystemPrompt->toPlainText());

    cfg.save();
}

void ConfigDialog::onAccepted()
{
    saveToConfig();

    if (m_samplersChanged)
        emit samplersChanged();

    if (m_restartNeeded) {
        emit restartRequired();
        QMessageBox::information(this, "Neustart erforderlich",
            "Modell-Pfad, Kontext, Chat-Template und Tool-Format werden beim "
            "nächsten Programmstart übernommen.\n\n"
            "Sampler-Einstellungen und User System-Prompt sind bereits aktiv\n"
            "(System-Prompt nach /clear).");
    }

    if (m_loggingChanged)
        emit loggingChanged(m_chatLoggingEnabled->isChecked());

    accept();
}

void ConfigDialog::onBrowseModel()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Modell auswählen",
        QFileInfo(m_modelPath->text()).absolutePath(),
        "GGUF Modelle (*.gguf);;Alle Dateien (*)");
    if (!path.isEmpty())
        m_modelPath->setText(path);
}

void ConfigDialog::onBrowseLogDir()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, "Log-Verzeichnis auswählen",
        m_chatLogDir->text().isEmpty()
            ? QDir::homePath() + "/llamatools/chat_log"
            : m_chatLogDir->text());
    if (!dir.isEmpty())
        m_chatLogDir->setText(dir);
}

void ConfigDialog::resetSamplerDefaults()
{
    m_chatTopK->setValue(40);    m_chatTemp->setValue(0.7);
    m_chatTopP->setValue(0.95);  m_chatMinP->setValue(0.05);
    m_executeTopK->setValue(20); m_executeTemp->setValue(0.2);
    m_executeTopP->setValue(0.60); m_executeMinP->setValue(0.05);
    m_toolTopK->setValue(20);    m_toolTemp->setValue(0.1);
    m_toolTopP->setValue(0.50);  m_toolMinP->setValue(0.05);
}

QDoubleSpinBox *ConfigDialog::makeDoubleSpinBox(double min, double max,
                                                 double step, int decimals, double value)
{
    auto *sb = new QDoubleSpinBox;
    sb->setRange(min, max); sb->setSingleStep(step);
    sb->setDecimals(decimals); sb->setValue(value);
    return sb;
}

QSpinBox *ConfigDialog::makeSpinBox(int min, int max, int value)
{
    auto *sb = new QSpinBox;
    sb->setRange(min, max); sb->setValue(value);
    return sb;
}
