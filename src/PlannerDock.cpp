#include "PlannerDock.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>

// ─── Konstruktor ──────────────────────────────────────────────────────────────
PlannerDock::PlannerDock(Agent *agent, QWidget *parent)
    : QDockWidget("📋 Aufgabenplan", parent)
    , m_agent(agent)
{
    setObjectName("plannerDock");
    setFeatures(QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetMovable  |
                QDockWidget::DockWidgetFloatable);

    setupUi();

    // ─── Connections: Agent → Dock ─────────────────────────────────────────
    connect(m_agent, &Agent::planReady,
            this,    &PlannerDock::onPlanReady);
    connect(m_agent, &Agent::taskTreeUpdated,
            this,    &PlannerDock::onTreeUpdated);
    connect(m_agent, &Agent::modeChanged,
            this,    &PlannerDock::onModeChanged);

    // ─── Connections: Buttons → Agent ─────────────────────────────────────
    connect(m_confirmButton, &QPushButton::clicked,
            m_agent,         &Agent::onPlanApproved);
    connect(m_rejectButton,  &QPushButton::clicked,
            m_agent,         &Agent::onPlanRejected);
}

// ─── setupUi ─────────────────────────────────────────────────────────────────
void PlannerDock::setupUi()
{
    auto *container = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(container);
    mainLayout->setSpacing(4);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    // ─── Status-Label ─────────────────────────────────────────────────────
    // Zeigt aktuellen Modus und Kurzinfo.
    m_statusLabel = new QLabel("Bereit — /plan <Auftrag> eingeben");
    m_statusLabel->setStyleSheet(
        "color: #555; font-size: 11px; font-style: italic; padding: 2px;");
    m_statusLabel->setWordWrap(true);
    mainLayout->addWidget(m_statusLabel);

    // ─── QSplitter: oben Tree, unten Kontext ──────────────────────────────
    // Splitter erlaubt dem User die Größenverhältnisse anzupassen.
    // Analogie AVR: wie ein variabler Spannungsteiler — zwei Bereiche
    // teilen den verfügbaren Platz, User bestimmt die Aufteilung.
    auto *splitter = new QSplitter(Qt::Vertical, container);

    // ─── QTreeView ────────────────────────────────────────────────────────
    m_treeView = new QTreeView(splitter);
    m_treeView->setUniformRowHeights(true);   // Performance bei großen Trees
    m_treeView->setAlternatingRowColors(true);
    m_treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_treeView->setMinimumHeight(120);

    // Initiales Model (leerer Tree) — wird in onTreeUpdated() ersetzt
    // wenn Agent::taskTreeUpdated() feuert.
    // Wir zeigen den Tree immer über den Agent-eigenen TaskTree —
    // kein Kopieren, nur ein Zeiger.
    // Cast nötig weil taskTree() const& zurückgibt:
    m_model = new TaskTreeModel(
        const_cast<TaskTree*>(&m_agent->taskTree()), this);
    m_treeView->setModel(m_model);

    // Selektion → Kontext-Anzeige aktualisieren
    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &PlannerDock::onSelectionChanged);

    splitter->addWidget(m_treeView);

    // ─── Kontext-Anzeige ──────────────────────────────────────────────────
    // Zeigt buildContext() des selektierten Knotens.
    // Read-only, monospace, Wortumbruch.
    m_contextView = new QTextEdit(splitter);
    m_contextView->setReadOnly(true);
    m_contextView->setAcceptRichText(false);
    m_contextView->setFontFamily("monospace");
    m_contextView->setFontPointSize(10);
    m_contextView->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_contextView->setPlaceholderText("Knoten auswählen um Kontext zu sehen...");
    m_contextView->setMinimumHeight(80);
    m_contextView->setMaximumHeight(200);
    splitter->addWidget(m_contextView);

    splitter->setStretchFactor(0, 3);  // Tree bekommt 3x mehr Platz als Kontext
    splitter->setStretchFactor(1, 1);
    mainLayout->addWidget(splitter, 1);  // stretch=1 → nimmt verfügbaren Platz

    // ─── Knoten-Zähler ────────────────────────────────────────────────────
    m_nodeCountLabel = new QLabel("0 Knoten");
    m_nodeCountLabel->setStyleSheet("color: #888; font-size: 10px;");
    mainLayout->addWidget(m_nodeCountLabel);

    // ─── Approval-Buttons ─────────────────────────────────────────────────
    // Anfangs ausgeblendet — werden sichtbar wenn planReady() feuert.
    // Layout: [Bestätigen ✓] [Ablehnen ✗] mit Spacer dazwischen
    auto *btnLayout = new QHBoxLayout;
    btnLayout->setSpacing(6);

    m_confirmButton = new QPushButton("✓ Plan bestätigen");
    m_confirmButton->setStyleSheet(
        "QPushButton { background: #188038; color: white; "
        "border-radius: 4px; padding: 4px 12px; font-weight: bold; }"
        "QPushButton:hover { background: #145c2c; }"
        "QPushButton:disabled { background: #ccc; color: #888; }");
    m_confirmButton->hide();

    m_rejectButton = new QPushButton("✗ Ablehnen");
    m_rejectButton->setStyleSheet(
        "QPushButton { background: #c5221f; color: white; "
        "border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background: #9c1a18; }"
        "QPushButton:disabled { background: #ccc; color: #888; }");
    m_rejectButton->hide();

    btnLayout->addWidget(m_confirmButton);
    btnLayout->addWidget(m_rejectButton);
    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    setWidget(container);
}

// ─── onPlanReady ─────────────────────────────────────────────────────────────
// Agent hat <plan>...</plan> geparst und TaskTree aufgebaut.
// Tree aktualisieren + Buttons einblenden.
void PlannerDock::onPlanReady()
{
    // Tree erst aktualisieren, dann Buttons zeigen
    onTreeUpdated();

    m_statusLabel->setText(
        "Plan bereit — bitte prüfen und bestätigen oder ablehnen.");
    m_statusLabel->setStyleSheet(
        "color: #e37400; font-size: 11px; font-weight: bold; padding: 2px;");

    m_confirmButton->show();
    m_rejectButton->show();

    // Alle Knoten aufklappen damit der User den vollen Plan sieht
    m_treeView->expandAll();

    // Dock einblenden falls ausgeblendet
    show();
    raise();
}

// ─── onTreeUpdated ───────────────────────────────────────────────────────────
// TaskTree hat sich verändert → QTreeView neu aufbauen.
//
// Pattern: Observer-Update — wir rufen nur refresh() auf dem Model auf.
// Das Model zeigt auf den Agent-eigenen TaskTree (Zeiger, kein Kopieren).
// beginResetModel/endResetModel in refresh() löst Qt's View-Neuaufbau aus.
//
// Analogie AVR: wie ein DMA-Transfer der fertig ist und einen IRQ auslöst —
// der View "wacht auf" und liest den neuen Speicherinhalt.
void PlannerDock::onTreeUpdated()
{
    if (!m_model) return;
    m_model->refresh();

    int count = m_agent->taskTree().nodeCount();
    m_nodeCountLabel->setText(QString("%1 Knoten").arg(count));

    // Spaltenbreite anpassen
    m_treeView->resizeColumnToContents(0);
}

// ─── onModeChanged ───────────────────────────────────────────────────────────
// Agent hat den Modus gewechselt.
// Wir passen Titel, Status und Button-Sichtbarkeit an.
void PlannerDock::onModeChanged(AgentMode mode)
{
    switch (mode) {
        case AgentMode::Chat:
            setWindowTitle("📋 Aufgabenplan");
            m_statusLabel->setText("Chat-Modus — /plan <Auftrag> für neuen Plan");
            m_statusLabel->setStyleSheet(
                "color: #555; font-size: 11px; font-style: italic; padding: 2px;");
            m_confirmButton->hide();
            m_rejectButton->hide();
            break;

        case AgentMode::Plan:
            setWindowTitle("📋 Plan-Analyse läuft...");
            m_statusLabel->setText("Modell analysiert Projekt...");
            m_statusLabel->setStyleSheet(
                "color: #1a73e8; font-size: 11px; font-style: italic; padding: 2px;");
            m_confirmButton->hide();
            m_rejectButton->hide();
            break;

        case AgentMode::Execute:
            setWindowTitle("📋 Plan — Ausführung");
            m_statusLabel->setText("Execute-Modus — Tasks werden abgearbeitet...");
            m_statusLabel->setStyleSheet(
                "color: #188038; font-size: 11px; font-weight: bold; padding: 2px;");
            m_confirmButton->hide();
            m_rejectButton->hide();
            break;
    }
}

// ─── onSelectionChanged ──────────────────────────────────────────────────────
// User hat einen Knoten im Tree ausgewählt.
// Zeigt buildContext() dieses Knotens in der Kontext-Anzeige.
//
// buildContext() liefert vertikalen Pfad + horizontale Abhängigkeiten.
// Das ist genau das was das Modell als Prompt bekommt — User kann so
// prüfen ob der Kontext sinnvoll ist.
void PlannerDock::onSelectionChanged(const QModelIndex &current,
                                      const QModelIndex & /*previous*/)
{
    if (!current.isValid() || !m_model) {
        m_contextView->clear();
        return;
    }

    TaskNode *node = m_model->node(current);
    if (!node) {
        m_contextView->clear();
        return;
    }

    // buildContext() aus dem Agent-eigenen TaskTree
    QString ctx = m_agent->taskTree().buildContext(node);
    m_contextView->setPlainText(ctx);
}
