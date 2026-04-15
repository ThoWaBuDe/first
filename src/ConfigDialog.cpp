#include "ConfigDialog.h"
#include "AppConfig.h"

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

ConfigDialog::ConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("LlamaQt — Einstellungen");
    setMinimumWidth(520);
    setModal(true);
    setupUi();
    loadFromConfig();
}

// ─── setupUi ─────────────────────────────────────────────────────────────────
void ConfigDialog::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(createModelTab(),    "🧠 Modell");
    tabs->addTab(createSamplerTab(),  "🎲 Sampler");
    tabs->addTab(createAgentTab(),    "🤖 Agent");
    tabs->addTab(createLoggingTab(),  "📝 Logging");
    tabs->addTab(createTemplateTab(), "💬 Template & Prompt");
    tabs->addTab(createExecuteTab(),  "⚙ Execute");   // NEU
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

// ─── Modell Tab ──────────────────────────────────────────────────────────────
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

    auto *ctxGroup = new QGroupBox("Kontext ⚠ (Neustart erforderlich)");
    auto *ctxForm  = new QFormLayout(ctxGroup);
    m_contextSize = makeSpinBox(2048, 256*1024, 128*1024);
    m_contextSize->setSingleStep(1024);
    m_contextSize->setSuffix(" Tokens");
    ctxForm->addRow("Context-Größe:", m_contextSize);
    m_batchSize = makeSpinBox(64, 4096, 512);
    m_batchSize->setSuffix(" Tokens");
    ctxForm->addRow("Batch-Size:", m_batchSize);
    auto *ctxHint = new QLabel(
        "<small style='color:#888'>128k Tokens ≈ ~96k Wörter. Mehr Kontext = mehr VRAM.</small>");
    ctxHint->setWordWrap(true);
    ctxForm->addRow("", ctxHint);
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

// ─── Sampler Tab ─────────────────────────────────────────────────────────────
QWidget *ConfigDialog::createSamplerTab()
{
    auto *w      = new QWidget;
    auto *layout = new QHBoxLayout(w);
    layout->setSpacing(16);

    auto *chatGroup = new QGroupBox("Chat-Profil  (Temp 0.7, kreativ)");
    auto *chatForm  = new QFormLayout(chatGroup);
    m_chatTopK = makeSpinBox(1, 200, 40);
    m_chatTemp = makeDoubleSpinBox(0.0, 2.0, 0.05, 2, 0.7);
    m_chatTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.95);
    m_chatMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);
    chatForm->addRow("Top-K:", m_chatTopK);
    chatForm->addRow("Temp:",  m_chatTemp);
    chatForm->addRow("Top-P:", m_chatTopP);
    chatForm->addRow("Min-P:", m_chatMinP);
    layout->addWidget(chatGroup);

    auto *toolGroup = new QGroupBox("Tool-Profil  (Temp 0.1, deterministisch)");
    auto *toolForm  = new QFormLayout(toolGroup);
    m_toolTopK = makeSpinBox(1, 100, 20);
    m_toolTemp = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.1);
    m_toolTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.5);
    m_toolMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);
    toolForm->addRow("Top-K:", m_toolTopK);
    toolForm->addRow("Temp:",  m_toolTemp);
    toolForm->addRow("Top-P:", m_toolTopP);
    toolForm->addRow("Min-P:", m_toolMinP);
    layout->addWidget(toolGroup);

    auto markSamplers = [this]{ m_samplersChanged = true; };
    connect(m_chatTopK, QOverload<int>::of(&QSpinBox::valueChanged),          this, markSamplers);
    connect(m_chatTemp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, markSamplers);
    connect(m_chatTopP, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, markSamplers);
    connect(m_chatMinP, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, markSamplers);
    connect(m_toolTopK, QOverload<int>::of(&QSpinBox::valueChanged),          this, markSamplers);
    connect(m_toolTemp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, markSamplers);
    connect(m_toolTopP, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, markSamplers);
    connect(m_toolMinP, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, markSamplers);

    return w;
}

// ─── Agent Tab ───────────────────────────────────────────────────────────────
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

// ─── Logging Tab ─────────────────────────────────────────────────────────────
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
    wsLayout->addRow("API Key:", m_tavilyApiKey);
    layout->addWidget(wsGroup);

    connect(m_chatLoggingEnabled, &QCheckBox::toggled,
            this, [this]{ m_loggingChanged = true; });

    layout->addStretch();
    return w;
}

// ─── Template & Prompt Tab ───────────────────────────────────────────────────
QWidget *ConfigDialog::createTemplateTab()
{
    auto *w      = new QWidget;
    auto *layout = new QVBoxLayout(w);
    layout->setSpacing(12);

    // ── Chat-Template ─────────────────────────────────────────────────────
    auto *tmplGroup  = new QGroupBox("Chat-Template ⚠ (Neustart erforderlich)");
    auto *tmplLayout = new QVBoxLayout(tmplGroup);

    m_detectedTemplateLabel = new QLabel("Erkanntes Template aus GGUF: (wird nach Modell-Laden angezeigt)");
    m_detectedTemplateLabel->setStyleSheet("color: #1a73e8; font-size: 11px;");
    m_detectedTemplateLabel->setWordWrap(true);
    tmplLayout->addWidget(m_detectedTemplateLabel);

    auto *presetLayout = new QHBoxLayout;
    presetLayout->addWidget(new QLabel("Preset:"));
    m_chatTemplateCombo = new QComboBox;
    m_chatTemplateCombo->addItem("Auto (aus Modell)",   static_cast<int>(ChatTemplate::Preset::Auto));
    m_chatTemplateCombo->addItem("ChatML (Qwen, standard)", static_cast<int>(ChatTemplate::Preset::ChatML));
    m_chatTemplateCombo->addItem("Llama 3",             static_cast<int>(ChatTemplate::Preset::Llama3));
    m_chatTemplateCombo->addItem("Gemma",               static_cast<int>(ChatTemplate::Preset::Gemma));
    m_chatTemplateCombo->addItem("Mistral",             static_cast<int>(ChatTemplate::Preset::Mistral));
    m_chatTemplateCombo->addItem("Custom (manuell)",    static_cast<int>(ChatTemplate::Preset::Custom));
    presetLayout->addWidget(m_chatTemplateCombo);
    presetLayout->addStretch();
    tmplLayout->addLayout(presetLayout);

    m_customTemplateLabel = new QLabel("Custom Template (Jinja2-Format):");
    m_customTemplateLabel->hide();
    tmplLayout->addWidget(m_customTemplateLabel);

    m_customTemplateEdit = new QTextEdit;
    m_customTemplateEdit->setMaximumHeight(80);
    m_customTemplateEdit->setFont(QFont("monospace", 10));
    m_customTemplateEdit->hide();
    tmplLayout->addWidget(m_customTemplateEdit);

    connect(m_chatTemplateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                bool isCustom = (m_chatTemplateCombo->currentData().toInt() ==
                                 static_cast<int>(ChatTemplate::Preset::Custom));
                m_customTemplateLabel->setVisible(isCustom);
                m_customTemplateEdit->setVisible(isCustom);
                m_restartNeeded = true;
                m_restartHint->show();
            });

    layout->addWidget(tmplGroup);

    // ── User System-Prompt ────────────────────────────────────────────────
    auto *promptGroup  = new QGroupBox("User System-Prompt (sofort wirksam nach /clear)");
    auto *promptLayout = new QVBoxLayout(promptGroup);

    m_userSystemPrompt = new QTextEdit;
    m_userSystemPrompt->setAcceptRichText(false);
    m_userSystemPrompt->setMinimumHeight(100);
    m_userSystemPrompt->setPlaceholderText(
        "Optionaler Text der dem Modell vorangestellt wird.\n\n"
        "Beispiel:\n"
        "Du bist ein hilfreicher C++ Programmierer. "
        "Antworte präzise und auf Deutsch.\n\n"
        "Leer lassen für Standard-Verhalten.");
    promptLayout->addWidget(m_userSystemPrompt);

    auto *promptHint = new QLabel(
        "<small style='color:#888'>"
        "Der vollständige System-Prompt besteht aus:<br>"
        "<b>1.</b> Diesem Text &nbsp;← du bist hier<br>"
        "<b>2.</b> MCP Tool-Beschreibungen (automatisch)<br>"
        "Änderungen werden nach <b>/clear</b> oder Neustart übernommen."
        "</small>");
    promptHint->setWordWrap(true);
    promptLayout->addWidget(promptHint);

    layout->addWidget(promptGroup);
    layout->addStretch();

    return w;
}

// ─── Execute Tab ─────────────────────────────────────────────────────────────
// Parameter für den Execute-Modus.
//
// executeMemoryMaxEntries: Wie viele Thoughts maximal gespeichert werden.
//   Zu viel → Modell wird mit veraltetem Wissen überlastet
//   Zu wenig → wichtige Erkenntnisse gehen verloren
//   Default 50 (~2000 Token) ist ein guter Startpunkt für Qwen3.5-9B.
//
// executeAutoMode: Ob das Modell selbst entscheidet wann ein Node fertig ist.
//   true  → Modell schreibt <code>...</code> wenn es fertig ist
//   false → nicht implementiert, Platzhalter für manuelle Granularität
//
// executeSandboxProject: Welches Unterverzeichnis in ~/llamatools/ als
//   Ziel für das Assembly verwendet wird.
QWidget *ConfigDialog::createExecuteTab()
{
    auto *w      = new QWidget;
    auto *layout = new QVBoxLayout(w);
    layout->setSpacing(12);

    // ── Thoughts (Kurzzeitgedächtnis) ─────────────────────────────────────
    auto *memGroup = new QGroupBox("Thoughts — Kurzzeitgedächtnis");
    auto *memForm  = new QFormLayout(memGroup);

    m_executeMemoryMaxEntries = makeSpinBox(10, 200, 50);
    m_executeMemoryMaxEntries->setSuffix(" Einträge");
    m_executeMemoryMaxEntries->setToolTip(
        "Maximale Anzahl Thoughts die nach jedem Node gespeichert werden.\n"
        "Jeder Eintrag ≈ ~40 Token → 50 Einträge ≈ ~2000 Token.\n"
        "Mehr Einträge = mehr Kontext, aber mehr Token-Verbrauch pro Generierung.");
    memForm->addRow("Max. Thoughts:", m_executeMemoryMaxEntries);

    auto *memHint = new QLabel(
        "<small style='color:#888'>"
        "Thoughts sind kurze Erkenntnisse die das Modell aus jedem Node gewinnt.<br>"
        "Sie werden nach jedem Node automatisch komprimiert.<br>"
        "50 Einträge ≈ 2000 Token — passt gut für Qwen3.5-9B mit 128k Kontext."
        "</small>");
    memHint->setWordWrap(true);
    memForm->addRow("", memHint);
    layout->addWidget(memGroup);

    // ── Ausführungs-Modus ─────────────────────────────────────────────────
    auto *modeGroup = new QGroupBox("Ausführungs-Modus");
    auto *modeLayout = new QVBoxLayout(modeGroup);

    m_executeAutoMode = new QCheckBox(
        "Auto-Modus: Modell entscheidet wann ein Node abgeschlossen ist");
    m_executeAutoMode->setToolTip(
        "Auto: Modell schreibt <code>...</code> wenn es fertig ist.\n"
        "Der Agent wartet auf diese Tags und verarbeitet dann das Ergebnis.\n"
        "Deaktiviert: zukünftige manuelle Granularität (noch nicht implementiert).");
    modeLayout->addWidget(m_executeAutoMode);
    m_assembleOnlyDone = new QCheckBox(
            "Nur Done-Nodes assemblieren (empfohlen)");
    m_assembleOnlyDone->setToolTip(
            "Aktiviert: nur Nodes mit Status 'Done' werden zu Dateien.\n"
            "Deaktiviert: alle Nodes mit nicht-leerem result werden assembliert\n"
           "(auch laufende oder fehlgeschlagene Nodes).");
    modeLayout->addWidget(m_assembleOnlyDone);

    auto *modeHint = new QLabel(
        "<small style='color:#888'>"
        "Im Auto-Modus signalisiert das Modell das Ende der Implementierung "
        "durch &lt;code&gt;...&lt;/code&gt; Tags. "
        "Alles außerhalb der Tags wird als sideOutput gespeichert."
        "</small>");
    modeHint->setWordWrap(true);
    modeLayout->addWidget(modeHint);
    layout->addWidget(modeGroup);

    // ── Sandbox-Projekt ───────────────────────────────────────────────────
    auto *projGroup  = new QGroupBox("Sandbox-Projekt (für Assembly)");
    auto *projForm   = new QFormLayout(projGroup);

    m_executeSandboxProject = new QLineEdit;
    m_executeSandboxProject->setPlaceholderText(
        "Unterverzeichnis in ~/llamatools/ (z.B. MeinProjekt)");
    m_executeSandboxProject->setToolTip(
        "Das Unterverzeichnis in ~/llamatools/ in das die assemblierten Dateien "
        "geschrieben werden.\n"
        "Leer = kein Projekt ausgewählt (Assembly deaktiviert).");
    projForm->addRow("Projekt:", m_executeSandboxProject);

    auto *projHint = new QLabel(
        "<small style='color:#888'>"
        "Assembly: Nodes werden nach Abschluss zu echten Dateien zusammengebaut.<br>"
        "H2-Node 'GameState.cpp' → ~/llamatools/[Projekt]/GameState.cpp<br>"
        "H2-result + H3-Kinder (sortiert nach order) werden konkateniert."
        "</small>");
    projHint->setWordWrap(true);
    projForm->addRow("", projHint);
    layout->addWidget(projGroup);

    // ── DB-Pfad (info only) ───────────────────────────────────────────────
    auto *dbGroup = new QGroupBox("Datenbank");
    auto *dbForm  = new QFormLayout(dbGroup);
    auto *dbLabel = new QLabel(
        QString("<small style='color:#555'>%1</small>")
        .arg(AppConfig::instance().taskDbPath()));
    dbLabel->setWordWrap(true);
    dbForm->addRow("DB-Pfad:", dbLabel);

    auto *dbHint = new QLabel(
        "<small style='color:#888'>"
        "Enthält: tasks (Aufgabenbaum) + execute_memory (Thoughts).<br>"
        "Änderbar in ~/.config/LlamaQt/LlamaQt.conf unter [TaskTree] db_path."
        "</small>");
    dbHint->setWordWrap(true);
    dbForm->addRow("", dbHint);
    layout->addWidget(dbGroup);

    layout->addStretch();
    return w;
}

// ─── loadFromConfig ──────────────────────────────────────────────────────────
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

    // Template & Prompt
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
        m_detectedTemplateLabel->setText(
            "Erkanntes Template aus GGUF: (keines eingebettet oder Modell noch nicht geladen)");
    } else {
        ChatTemplate::Preset p = ChatTemplate::detectFromJinja(detected);
        m_detectedTemplateLabel->setText(
            QString("Erkanntes Template aus GGUF: %1")
            .arg(ChatTemplate::presetName(p)));
    }
    m_userSystemPrompt->setPlainText(cfg.userSystemPrompt());

    // Execute Tab
    m_executeMemoryMaxEntries->setValue(cfg.executeMemoryMaxEntries());
    m_executeAutoMode->setChecked(cfg.executeAutoMode());
    m_executeSandboxProject->setText(cfg.executeSandboxProject());

    m_assembleOnlyDone->setChecked(cfg.assembleOnlyDone());

    // Flags zurücksetzen
    m_samplersChanged = false;
    m_restartNeeded   = false;
    m_loggingChanged  = false;
    m_restartHint->hide();
}

// ─── saveToConfig ────────────────────────────────────────────────────────────
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
    m_deadlockWarn->setValue(w);
    m_deadlockRedirect->setValue(r);
    m_deadlockAbort->setValue(a);
    cfg.setDeadlockWarn(w);
    cfg.setDeadlockRedirect(r);
    cfg.setDeadlockAbort(a);

    cfg.setChatLoggingEnabled(m_chatLoggingEnabled->isChecked());
    cfg.setChatLogDir(m_chatLogDir->text());
    cfg.setTavilyApiKey(m_tavilyApiKey->text());

    // Template & Prompt
    auto preset = static_cast<ChatTemplate::Preset>(
        m_chatTemplateCombo->currentData().toInt());
    cfg.setChatTemplatePreset(preset);
    cfg.setCustomChatTemplate(m_customTemplateEdit->toPlainText());
    cfg.setUserSystemPrompt(m_userSystemPrompt->toPlainText());

    // Execute
    cfg.setExecuteMemoryMaxEntries(m_executeMemoryMaxEntries->value());
    cfg.setExecuteAutoMode(m_executeAutoMode->isChecked());
    cfg.setExecuteSandboxProject(m_executeSandboxProject->text());

    cfg.setAssembleOnlyDone(m_assembleOnlyDone->isChecked());

    cfg.save();
}

// ─── onAccepted ──────────────────────────────────────────────────────────────
void ConfigDialog::onAccepted()
{
    saveToConfig();

    if (m_samplersChanged)
        emit samplersChanged();

    if (m_restartNeeded) {
        emit restartRequired();
        QMessageBox::information(this, "Neustart erforderlich",
            "Modell-Pfad, Kontext und Chat-Template werden beim nächsten "
            "Programmstart übernommen.\n\n"
            "Sampler-Einstellungen und User System-Prompt sind bereits aktiv\n"
            "(System-Prompt nach /clear).");
    }

    if (m_loggingChanged)
        emit loggingChanged(m_chatLoggingEnabled->isChecked());

    accept();
}

// ─── Slots ───────────────────────────────────────────────────────────────────
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
    m_chatTopK->setValue(40);  m_chatTemp->setValue(0.7);
    m_chatTopP->setValue(0.95); m_chatMinP->setValue(0.05);
    m_toolTopK->setValue(20);  m_toolTemp->setValue(0.1);
    m_toolTopP->setValue(0.50); m_toolMinP->setValue(0.05);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────
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
