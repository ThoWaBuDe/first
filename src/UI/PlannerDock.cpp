#include "UI/PlannerDock.h"
#include "UI/TaskNodeDialog.h"
#include "Config/AppConfig.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QMenu>
#include <QAction>
#include <QMessageBox>

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

    connect(m_agent, &Agent::planReady,       this, &PlannerDock::onPlanReady);
    connect(m_agent, &Agent::taskTreeUpdated, this, &PlannerDock::onTreeUpdated);
    connect(m_agent, &Agent::modeChanged,     this, &PlannerDock::onModeChanged);
    connect(m_confirmButton, &QPushButton::clicked, m_agent, &Agent::onPlanApproved);
    connect(m_rejectButton,  &QPushButton::clicked, m_agent, &Agent::onPlanRejected);
}

// ─── setupUi ─────────────────────────────────────────────────────────────────
void PlannerDock::setupUi()
{
    auto *container  = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(container);
    mainLayout->setSpacing(4);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    m_statusLabel = new QLabel("Bereit — /plan <Auftrag> eingeben");
    m_statusLabel->setStyleSheet(
        "color: #555; font-size: 11px; font-style: italic; padding: 2px;");
    m_statusLabel->setWordWrap(true);
    mainLayout->addWidget(m_statusLabel);

    auto *splitter = new QSplitter(Qt::Vertical, container);

    // ── QTreeView ─────────────────────────────────────────────────────────
    m_treeView = new QTreeView(splitter);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setAlternatingRowColors(true);
    m_treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_treeView->setMinimumHeight(120);

    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_treeView, &QTreeView::customContextMenuRequested,
            this,       &PlannerDock::onContextMenu);
    connect(m_treeView, &QTreeView::activated,
            this,       &PlannerDock::onNodeActivated);

    m_model = new TaskTreeModel(
        const_cast<TaskTree*>(&m_agent->taskTree()), this);
    m_treeView->setModel(m_model);

    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &PlannerDock::onSelectionChanged);

    splitter->addWidget(m_treeView);

    // ── Kontext-Anzeige ───────────────────────────────────────────────────
    m_contextView = new QTextEdit(splitter);
    m_contextView->setReadOnly(true);
    m_contextView->setAcceptRichText(false);
    m_contextView->setFontFamily("monospace");
    m_contextView->setFontPointSize(10);
    m_contextView->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_contextView->setPlaceholderText(
        "Knoten auswählen — Doppelklick oder Rechtsklick zum Bearbeiten");
    m_contextView->setMinimumHeight(80);
    m_contextView->setMaximumHeight(200);
    splitter->addWidget(m_contextView);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    mainLayout->addWidget(splitter, 1);

    // ── Knoten-Zähler ─────────────────────────────────────────────────────
    m_nodeCountLabel = new QLabel("0 Knoten");
    m_nodeCountLabel->setStyleSheet("color: #888; font-size: 10px;");
    mainLayout->addWidget(m_nodeCountLabel);

    // ── Buttons ───────────────────────────────────────────────────────────
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

    // "Plan laden" — lädt gespeicherten Plan aus SQLite nach App-Neustart
    m_loadButton = new QPushButton("📂 Plan laden");
    m_loadButton->setToolTip(
        QString("Gespeicherten Plan laden aus:\n%1")
        .arg(AppConfig::instance().taskDbPath()));
    m_loadButton->setStyleSheet(
        "QPushButton { border-radius: 4px; padding: 4px 8px; }"
        "QPushButton:hover { background: #e8f0fe; }");

    connect(m_loadButton, &QPushButton::clicked, this, &PlannerDock::onLoadPlan);

    btnLayout->addWidget(m_confirmButton);
    btnLayout->addWidget(m_rejectButton);
    btnLayout->addStretch();
    btnLayout->addWidget(m_loadButton);
    mainLayout->addLayout(btnLayout);

    setWidget(container);
}

// ═════════════════════════════════════════════════════════════════════════════
// EXPAND-ZUSTAND
// ═════════════════════════════════════════════════════════════════════════════

// ─── saveExpandState ─────────────────────────────────────────────────────────
// Sammelt IDs aller aufgeklappten Knoten.
// Warum IDs statt QModelIndex?
//   QModelIndex ist nach beginResetModel() ungültig — IDs im TaskNode bleiben stabil.
// Analogie AVR: SREG in lokale Variable sichern bevor cli().
QSet<qint64> PlannerDock::saveExpandState() const
{
    QSet<qint64> expanded;
    std::function<void(const QModelIndex&)> collect =
        [&](const QModelIndex &parent) {
            for (int row = 0; row < m_model->rowCount(parent); ++row) {
                QModelIndex idx = m_model->index(row, 0, parent);
                if (m_treeView->isExpanded(idx)) {
                    TaskNode *n = m_model->node(idx);
                    if (n) expanded.insert(n->id);
                }
                collect(idx);
            }
        };
    collect(QModelIndex{});
    return expanded;
}

// ─── restoreExpandState ──────────────────────────────────────────────────────
// Stellt Expand-Zustand nach Model-Reset wieder her.
// Analogie AVR: SREG aus lokaler Variable wiederherstellen nach sei().
void PlannerDock::restoreExpandState(const QSet<qint64> &expanded)
{
    if (expanded.isEmpty()) return;
    std::function<void(const QModelIndex&)> restore =
        [&](const QModelIndex &parent) {
            for (int row = 0; row < m_model->rowCount(parent); ++row) {
                QModelIndex idx = m_model->index(row, 0, parent);
                TaskNode *n = m_model->node(idx);
                if (n && expanded.contains(n->id))
                    m_treeView->expand(idx);
                restore(idx);
            }
        };
    restore(QModelIndex{});
}

// ─── selectedNode ─────────────────────────────────────────────────────────────
TaskNode *PlannerDock::selectedNode() const
{
    QModelIndex idx = m_treeView->currentIndex();
    if (!idx.isValid() || !m_model) return nullptr;
    return m_model->node(idx);
}

// ═════════════════════════════════════════════════════════════════════════════
// EDIT-DIALOG
// ═════════════════════════════════════════════════════════════════════════════

// ─── openEditDialog ──────────────────────────────────────────────────────────
void PlannerDock::openEditDialog(TaskNode *node)
{
    if (!node) return;

    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());
    TaskNodeDialog dlg(tree, node, this);

    if (dlg.exec() == QDialog::Accepted) {
        dlg.applyToNode();

        auto expanded = saveExpandState();
        m_model->refresh();
        restoreExpandState(expanded);

        m_nodeCountLabel->setText(
            QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
        m_treeView->resizeColumnToContents(0);
        m_contextView->setPlainText(m_agent->taskTree().buildContext(node));
    }
}

// ─── onNodeActivated ─────────────────────────────────────────────────────────
void PlannerDock::onNodeActivated(const QModelIndex &index)
{
    if (!index.isValid() || !m_model) return;
    openEditDialog(m_model->node(index));
}

// ═════════════════════════════════════════════════════════════════════════════
// KONTEXTMENÜ
// ═════════════════════════════════════════════════════════════════════════════

void PlannerDock::onContextMenu(const QPoint &pos)
{
    QModelIndex idx = m_treeView->indexAt(pos);
    TaskNode *node  = idx.isValid() ? m_model->node(idx) : nullptr;

    QMenu menu(this);

    if (node) {
        QAction *editAct = menu.addAction("✏ Bearbeiten");
        connect(editAct, &QAction::triggered, this, &PlannerDock::onEditNode);

        menu.addSeparator();

        QAction *addChildAct = menu.addAction("➕ Kind hinzufügen");
        connect(addChildAct, &QAction::triggered, this, &PlannerDock::onAddChild);

        QAction *addSiblingAct = menu.addAction("➕ Geschwister hinzufügen");
        addSiblingAct->setEnabled(node->parent != nullptr);
        connect(addSiblingAct, &QAction::triggered, this, &PlannerDock::onAddSibling);

        menu.addSeparator();

        QAction *deleteAct = menu.addAction("🗑 Löschen");
        connect(deleteAct, &QAction::triggered, this, &PlannerDock::onDeleteNode);
    } else {
        // Kein Knoten → Wurzel anbieten wenn Tree leer
        if (m_agent->taskTree().isEmpty()) {
            QAction *newRootAct = menu.addAction("➕ Wurzelknoten hinzufügen");
            connect(newRootAct, &QAction::triggered, this, [this]() {
                TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());
                TaskNode *root = tree->createNode(
                    "Neues Ziel", "", static_cast<int>(TaskLevel::Goal),
                    TaskScope::External, 0, nullptr);
                onTreeUpdated();
                openEditDialog(root);
            });
        }
    }

    if (!menu.isEmpty())
        menu.exec(m_treeView->viewport()->mapToGlobal(pos));
}

void PlannerDock::onEditNode()
{
    openEditDialog(selectedNode());
}

// ─── onAddChild ──────────────────────────────────────────────────────────────
// Fügt leeren Kindknoten ein + öffnet sofort Dialog.
// level = parent->level + 1, scope = Internal als sicherer Default.
void PlannerDock::onAddChild()
{
    TaskNode *parent = selectedNode();
    if (!parent) return;

    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());
    TaskNode *child = tree->createNode(
        "Neue Aufgabe", "", parent->level + 1,
        TaskScope::Internal, 0, parent);

    auto expanded = saveExpandState();
    expanded.insert(parent->id);
    m_model->refresh();
    restoreExpandState(expanded);

    m_nodeCountLabel->setText(
        QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
    m_treeView->resizeColumnToContents(0);

    // Neuen Knoten im View selektieren
    std::function<QModelIndex(const QModelIndex&)> findIdx =
        [&](const QModelIndex &p) -> QModelIndex {
            for (int row = 0; row < m_model->rowCount(p); ++row) {
                QModelIndex i = m_model->index(row, 0, p);
                if (m_model->node(i) == child) return i;
                QModelIndex f = findIdx(i);
                if (f.isValid()) return f;
            }
            return {};
        };

    QModelIndex childIdx = findIdx(QModelIndex{});
    if (childIdx.isValid()) {
        m_treeView->setCurrentIndex(childIdx);
        m_treeView->scrollTo(childIdx);
    }

    openEditDialog(child);
}

// ─── onAddSibling ────────────────────────────────────────────────────────────
// Fügt Geschwisterknoten ein — gleicher level, gleicher Elter.
void PlannerDock::onAddSibling()
{
    TaskNode *sibling = selectedNode();
    if (!sibling || !sibling->parent) return;

    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());
    TaskNode *newNode = tree->createNode(
        "Neue Aufgabe", "", sibling->level,
        sibling->scope, sibling->order + 1, sibling->parent);

    auto expanded = saveExpandState();
    expanded.insert(sibling->parent->id);
    m_model->refresh();
    restoreExpandState(expanded);

    m_nodeCountLabel->setText(
        QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
    m_treeView->resizeColumnToContents(0);

    std::function<QModelIndex(const QModelIndex&)> findIdx =
        [&](const QModelIndex &p) -> QModelIndex {
            for (int row = 0; row < m_model->rowCount(p); ++row) {
                QModelIndex i = m_model->index(row, 0, p);
                if (m_model->node(i) == newNode) return i;
                QModelIndex f = findIdx(i);
                if (f.isValid()) return f;
            }
            return {};
        };

    QModelIndex newIdx = findIdx(QModelIndex{});
    if (newIdx.isValid()) {
        m_treeView->setCurrentIndex(newIdx);
        m_treeView->scrollTo(newIdx);
    }

    openEditDialog(newNode);
}

// ─── onDeleteNode ────────────────────────────────────────────────────────────
// Löscht Knoten + alle Kinder nach Sicherheitsabfrage.
void PlannerDock::onDeleteNode()
{
    TaskNode *node = selectedNode();
    if (!node) return;

    int childCount = static_cast<int>(node->children.size());
    QString msg = QString("Knoten <b>%1</b> löschen?")
                  .arg(node->title.toHtmlEscaped());
    if (childCount > 0) {
        msg += QString("<br><br><b>Achtung:</b> Dieser Knoten hat %1 direkte(s) Kind(er). "
                       "Alle werden ebenfalls gelöscht.")
               .arg(childCount);
    }

    if (QMessageBox::question(this, "Knoten löschen", msg,
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());
    auto expanded = saveExpandState();
    expanded.remove(node->id);

    tree->removeNode(node);

    m_model->refresh();
    restoreExpandState(expanded);

    m_contextView->clear();
    m_nodeCountLabel->setText(
        QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
    m_treeView->resizeColumnToContents(0);
}

// ─── onLoadPlan ──────────────────────────────────────────────────────────────
// Lädt gespeicherten Plan aus SQLite — nützlich nach App-Neustart.
// TaskTree::load() liest alle Knoten + dependsOn-IDs aus der DB.
void PlannerDock::onLoadPlan()
{
    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());
    bool ok = tree->load();

    if (!ok) {
        m_statusLabel->setText("Fehler: Plan konnte nicht geladen werden.");
        m_statusLabel->setStyleSheet(
            "color: #c5221f; font-size: 11px; font-weight: bold; padding: 2px;");
        return;
    }

    int count = tree->nodeCount();
    if (count == 0) {
        m_statusLabel->setText("Keine gespeicherten Pläne in der DB gefunden.");
        return;
    }

    m_model->refresh();
    m_treeView->expandAll();

    m_nodeCountLabel->setText(QString("%1 Knoten geladen").arg(count));
    m_statusLabel->setText(
        QString("Plan geladen: %1 Knoten — Doppelklick zum Bearbeiten.").arg(count));
    m_statusLabel->setStyleSheet(
        "color: #188038; font-size: 11px; font-weight: bold; padding: 2px;");

    show();
    raise();
}

// ═════════════════════════════════════════════════════════════════════════════
// AGENT-SIGNALE
// ═════════════════════════════════════════════════════════════════════════════

void PlannerDock::onPlanReady()
{
    m_model->refresh();
    m_treeView->expandAll();

    m_statusLabel->setText(
        "Plan bereit — prüfen, ggf. Bearbeiten (Doppelklick/Rechtsklick), dann bestätigen.");
    m_statusLabel->setStyleSheet(
        "color: #e37400; font-size: 11px; font-weight: bold; padding: 2px;");

    m_confirmButton->show();
    m_rejectButton->show();

    m_nodeCountLabel->setText(
        QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
    m_treeView->resizeColumnToContents(0);

    show();
    raise();
}

void PlannerDock::onTreeUpdated()
{
    if (!m_model) return;
    auto expanded = saveExpandState();
    m_model->refresh();
    restoreExpandState(expanded);

    m_nodeCountLabel->setText(
        QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
    m_treeView->resizeColumnToContents(0);
}

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
            m_statusLabel->setText("Execute-Modus — Bearbeiten weiterhin möglich.");
            m_statusLabel->setStyleSheet(
                "color: #188038; font-size: 11px; font-weight: bold; padding: 2px;");
            m_confirmButton->hide();
            m_rejectButton->hide();
            break;
    }
}

void PlannerDock::onSelectionChanged(const QModelIndex &current,
                                      const QModelIndex & /*previous*/)
{
    if (!current.isValid() || !m_model) {
        m_contextView->clear();
        return;
    }
    TaskNode *node = m_model->node(current);
    if (!node) { m_contextView->clear(); return; }
    m_contextView->setPlainText(m_agent->taskTree().buildContext(node));
}
