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
    tabs->addTab(createModelTab(),   "🧠 Modell");
    tabs->addTab(createSamplerTab(), "🎲 Sampler");
    tabs->addTab(createAgentTab(),   "🤖 Agent");
    tabs->addTab(createLoggingTab(), "📝 Logging");
    mainLayout->addWidget(tabs);

    // ─── Button-Box ───────────────────────────────────────────────────────
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

    // Pfad
    auto *pathGroup  = new QGroupBox("Modell-Pfad ⚠ (Neustart erforderlich)");
    auto *pathLayout = new QHBoxLayout(pathGroup);
    m_modelPath = new QLineEdit;
    m_modelPath->setPlaceholderText("/home/user/ai/models/model.gguf");
    auto *browseBtn = new QPushButton("Durchsuchen...");
    connect(browseBtn, &QPushButton::clicked, this, &ConfigDialog::onBrowseModel);
    pathLayout->addWidget(m_modelPath);
    pathLayout->addWidget(browseBtn);
    layout->addWidget(pathGroup);

    // Kontext
    auto *ctxGroup  = new QGroupBox("Kontext ⚠ (Neustart erforderlich)");
    auto *ctxForm   = new QFormLayout(ctxGroup);

    m_contextSize = makeSpinBox(2048, 256*1024, 128*1024);
    m_contextSize->setSingleStep(1024);
    m_contextSize->setSuffix(" Tokens");
    ctxForm->addRow("Context-Größe:", m_contextSize);

    m_batchSize = makeSpinBox(64, 4096, 512);
    m_batchSize->setSuffix(" Tokens");
    ctxForm->addRow("Batch-Size:", m_batchSize);

    // Kontextgröße-Hinweis
    auto *ctxHint = new QLabel(
        "<small style='color:#888'>128k Tokens ≈ ~96k Wörter. "
        "Mehr Kontext = mehr VRAM. Qwen3.5-9B: ~10GB bei 128k.</small>");
    ctxHint->setWordWrap(true);
    ctxForm->addRow("", ctxHint);

    layout->addWidget(ctxGroup);

    m_restartHint = new QLabel(
        "<b>⚠ Modell-Pfad und Kontext-Einstellungen werden erst nach "
        "dem nächsten Start wirksam.</b>");
    m_restartHint->setWordWrap(true);
    m_restartHint->setStyleSheet("color: #e37400; padding: 6px;");
    m_restartHint->hide();
    layout->addWidget(m_restartHint);

    layout->addStretch();

    // Änderungs-Tracking
    connect(m_modelPath,   &QLineEdit::textChanged, this, [this]{ m_restartNeeded = true; m_restartHint->show(); });
    connect(m_contextSize, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]{ m_restartNeeded = true; m_restartHint->show(); });
    connect(m_batchSize,   QOverload<int>::of(&QSpinBox::valueChanged), this, [this]{ m_restartNeeded = true; m_restartHint->show(); });

    return w;
}

// ─── Sampler Tab ─────────────────────────────────────────────────────────────
// Zwei Gruppen nebeneinander: Chat (links) und Tool (rechts).
// Sofort wirksam nach OK — kein Neustart nötig.
QWidget *ConfigDialog::createSamplerTab()
{
    auto *w      = new QWidget;
    auto *layout = new QHBoxLayout(w);
    layout->setSpacing(16);

    // ── Chat Profil ───────────────────────────────────────────────────────
    auto *chatGroup = new QGroupBox("Chat-Profil  (Temp 0.7, kreativ)");
    auto *chatForm  = new QFormLayout(chatGroup);

    m_chatTopK = makeSpinBox(1, 200, 40);
    m_chatTemp = makeDoubleSpinBox(0.0, 2.0, 0.05, 2, 0.7);
    m_chatTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.95);
    m_chatMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);

    chatForm->addRow("Top-K:",    m_chatTopK);
    chatForm->addRow("Temp:",     m_chatTemp);
    chatForm->addRow("Top-P:",    m_chatTopP);
    chatForm->addRow("Min-P:",    m_chatMinP);

    auto *chatHint = new QLabel(
        "<small style='color:#888'>Höhere Temp = kreativer/zufälliger.<br>"
        "Top-K begrenzt die Auswahl auf K Token.</small>");
    chatHint->setWordWrap(true);
    chatForm->addRow("", chatHint);
    layout->addWidget(chatGroup);

    // ── Tool Profil ───────────────────────────────────────────────────────
    auto *toolGroup = new QGroupBox("Tool-Profil  (Temp 0.1, deterministisch)");
    auto *toolForm  = new QFormLayout(toolGroup);

    m_toolTopK = makeSpinBox(1, 100, 20);
    m_toolTemp = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.1);
    m_toolTopP = makeDoubleSpinBox(0.0, 1.0, 0.05, 2, 0.5);
    m_toolMinP = makeDoubleSpinBox(0.0, 1.0, 0.01, 2, 0.05);

    toolForm->addRow("Top-K:",    m_toolTopK);
    toolForm->addRow("Temp:",     m_toolTemp);
    toolForm->addRow("Top-P:",    m_toolTopP);
    toolForm->addRow("Min-P:",    m_toolMinP);

    auto *toolHint = new QLabel(
        "<small style='color:#888'>Niedrige Temp für valides JSON.<br>"
        "Wird für alle Tool-Calls verwendet.</small>");
    toolHint->setWordWrap(true);
    toolForm->addRow("", toolHint);
    layout->addWidget(toolGroup);

    // Änderungs-Tracking
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

    // Kontext-Management
    auto *ctxGroup = new QGroupBox("Kontext-Management");
    auto *ctxForm  = new QFormLayout(ctxGroup);

    m_summarizeThreshold = makeSpinBox(50, 99, 80);
    m_summarizeThreshold->setSuffix(" %");
    ctxForm->addRow("Auto-Summarize bei:", m_summarizeThreshold);

    auto *ctxHint = new QLabel(
        "<small style='color:#888'>Bei dieser Kontext-Auslastung wird die "
        "Konversation automatisch zusammengefasst.</small>");
    ctxHint->setWordWrap(true);
    ctxForm->addRow("", ctxHint);
    layout->addWidget(ctxGroup);

    // Generierungs-Limits
    auto *genGroup = new QGroupBox("Generierungs-Limits");
    auto *genForm  = new QFormLayout(genGroup);

    m_maxContinuations = makeSpinBox(1, 10, 3);
    genForm->addRow("Max. Continuations:", m_maxContinuations);

    m_maxToolResultChars = makeSpinBox(1000, 50000, 6000);
    m_maxToolResultChars->setSingleStep(500);
    m_maxToolResultChars->setSuffix(" Zeichen");
    genForm->addRow("Max. Tool-Ergebnis:", m_maxToolResultChars);

    auto *genHint = new QLabel(
        "<small style='color:#888'>Längere Tool-Ergebnisse werden gekürzt "
        "um den Kontext zu schonen.</small>");
    genHint->setWordWrap(true);
    genForm->addRow("", genHint);
    layout->addWidget(genGroup);

    // Deadlock-Erkennung
    auto *dlGroup = new QGroupBox("Deadlock-Eskalation");
    auto *dlForm  = new QFormLayout(dlGroup);

    m_deadlockWarn     = makeSpinBox(1, 20, 3);
    m_deadlockRedirect = makeSpinBox(1, 20, 5);
    m_deadlockAbort    = makeSpinBox(1, 20, 7);

    dlForm->addRow("Warnung nach:",    m_deadlockWarn);
    dlForm->addRow("Umleitung nach:",  m_deadlockRedirect);
    dlForm->addRow("Abbruch nach:",    m_deadlockAbort);

    auto *dlHint = new QLabel(
        "<small style='color:#888'>Wenn dasselbe Tool N Mal hintereinander "
        "mit demselben Fehler scheitert.</small>");
    dlHint->setWordWrap(true);
    dlForm->addRow("", dlHint);
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

    // Chat-Logging
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

    auto *logHint = new QLabel(
        "<small style='color:#888'>Jede Session wird als eigene Datei gespeichert:<br>"
        "chat_YYYY-MM-DD_HH-mm-ss.md<br>"
        "Thinking wird als &lt;details&gt;-Block eingeklappt.</small>");
    logHint->setWordWrap(true);
    logLayout->addWidget(logHint);
    layout->addWidget(logGroup);

    // Web-Search
    auto *wsGroup  = new QGroupBox("Web-Search (Tavily)");
    auto *wsLayout = new QFormLayout(wsGroup);

    m_tavilyApiKey = new QLineEdit;
    m_tavilyApiKey->setEchoMode(QLineEdit::Password);  // API Key verbergen
    m_tavilyApiKey->setPlaceholderText("tvly-...");

    auto *showKey = new QCheckBox("Anzeigen");
    connect(showKey, &QCheckBox::toggled, this, [this](bool show){
        m_tavilyApiKey->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });

    auto *keyLayout = new QHBoxLayout;
    keyLayout->addWidget(m_tavilyApiKey);
    keyLayout->addWidget(showKey);

    wsLayout->addRow("API Key:", keyLayout);

    auto *wsHint = new QLabel(
        "<small style='color:#888'>Kostenlos bei tavily.com — "
        "1000 Anfragen/Monat im Free-Tier.<br>"
        "Wird als Umgebungsvariable TAVILY_API_KEY weitergegeben.</small>");
    wsHint->setWordWrap(true);
    wsLayout->addRow("", wsHint);
    layout->addWidget(wsGroup);

    layout->addStretch();

    // Logging-Tracking
    connect(m_chatLoggingEnabled, &QCheckBox::toggled, this, [this]{ m_loggingChanged = true; });

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

    // Flags zurücksetzen (load erzeugt keine Änderungen)
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

    // Deadlock-Werte: Reihenfolge erzwingen (warn < redirect < abort)
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
            "Modell-Pfad und Kontext-Einstellungen werden beim nächsten "
            "Programmstart übernommen.\n\n"
            "Sampler-Einstellungen sind bereits aktiv.");
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
    // Chat-Defaults
    m_chatTopK->setValue(40);
    m_chatTemp->setValue(0.7);
    m_chatTopP->setValue(0.95);
    m_chatMinP->setValue(0.05);
    // Tool-Defaults
    m_toolTopK->setValue(20);
    m_toolTemp->setValue(0.1);
    m_toolTopP->setValue(0.50);
    m_toolMinP->setValue(0.05);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────
QDoubleSpinBox *ConfigDialog::makeDoubleSpinBox(double min, double max,
                                                 double step, int decimals,
                                                 double value)
{
    auto *sb = new QDoubleSpinBox;
    sb->setRange(min, max);
    sb->setSingleStep(step);
    sb->setDecimals(decimals);
    sb->setValue(value);
    return sb;
}

QSpinBox *ConfigDialog::makeSpinBox(int min, int max, int value)
{
    auto *sb = new QSpinBox;
    sb->setRange(min, max);
    sb->setValue(value);
    return sb;
}
