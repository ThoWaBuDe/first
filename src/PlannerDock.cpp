#include "PlannerDock.h"
#include "TaskNodeDialog.h"
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

    connect(m_agent, &Agent::planReady,        this, &PlannerDock::onPlanReady);
    connect(m_agent, &Agent::taskTreeUpdated,  this, &PlannerDock::onTreeUpdated);
    connect(m_agent, &Agent::modeChanged,      this, &PlannerDock::onModeChanged);
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

    // Kontextmenü: Qt::CustomContextMenu aktivieren damit wir das Menü
    // selbst bauen können. Ohne dieses Flag liefert Qt kein contextMenuRequested.
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_treeView, &QTreeView::customContextMenuRequested,
            this,       &PlannerDock::onContextMenu);

    // Doppelklick / Enter → Bearbeiten
    // activated() statt doubleClicked() — reagiert auch auf Enter-Taste.
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

    // ── Approval-Buttons ──────────────────────────────────────────────────
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

// ═════════════════════════════════════════════════════════════════════════════
// EXPAND-ZUSTAND: SICHERN + WIEDERHERSTELLEN
// ═════════════════════════════════════════════════════════════════════════════

// ─── saveExpandState ─────────────────────────────────────────────────────────
// Traversiert alle Indizes im Model und sammelt die IDs der aufgeklappten Knoten.
//
// Warum IDs statt QModelIndex?
//   QModelIndex enthält internalPointer() auf TaskNode* — nach beginResetModel()
//   sind alle alten Indizes ungültig. IDs (qint64) bleiben stabil weil sie im
//   TaskNode selbst stecken, nicht im Model.
//
// Analogie AVR: wie das Sichern des SREG-Registers in eine lokale Variable
// bevor cli() aufgerufen wird — der Zustand überlebt den Reset.
QSet<qint64> PlannerDock::saveExpandState() const
{
    QSet<qint64> expanded;

    // Rekursive Hilfsfunktion als Lambda — durchsucht alle Indizes
    // Eine std::function ist nötig weil das Lambda sich selbst aufruft.
    std::function<void(const QModelIndex&)> collect =
        [&](const QModelIndex &parent) {
            for (int row = 0; row < m_model->rowCount(parent); ++row) {
                QModelIndex idx = m_model->index(row, 0, parent);
                if (m_treeView->isExpanded(idx)) {
                    TaskNode *n = m_model->node(idx);
                    if (n) expanded.insert(n->id);
                }
                // Auch nicht-aufgeklappte Knoten durchsuchen —
                // ihre Kinder könnten aufgeklappt sein.
                collect(idx);
            }
        };

    collect(QModelIndex{});
    return expanded;
}

// ─── restoreExpandState ──────────────────────────────────────────────────────
// Stellt den Expand-Zustand nach einem Model-Reset wieder her.
// Traversiert alle neuen Indizes und klappt die auf deren ID in `expanded` ist.
//
// Analogie AVR: wie das Wiederherstellen des SREG nach sei() —
// der Zustand wird aus der gesicherten Variable geladen.
void PlannerDock::restoreExpandState(const QSet<qint64> &expanded)
{
    if (expanded.isEmpty()) return;

    std::function<void(const QModelIndex&)> restore =
        [&](const QModelIndex &parent) {
            for (int row = 0; row < m_model->rowCount(parent); ++row) {
                QModelIndex idx = m_model->index(row, 0, parent);
                TaskNode *n = m_model->node(idx);
                if (n && expanded.contains(n->id)) {
                    m_treeView->expand(idx);
                }
                restore(idx);
            }
        };

    restore(QModelIndex{});
}

// ─── selectedNode ─────────────────────────────────────────────────────────────
// Gibt den aktuell selektierten Knoten zurück, oder nullptr.
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
// Öffnet TaskNodeDialog für den übergebenen Knoten.
// Bei OK: applyToNode() + Tree refresh mit Expand-Erhalt.
void PlannerDock::openEditDialog(TaskNode *node)
{
    if (!node) return;

    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());
    TaskNodeDialog dlg(tree, node, this);

    if (dlg.exec() == QDialog::Accepted) {
        dlg.applyToNode();

        // Expand-Zustand sichern → refresh → wiederherstellen
        // Das ist der Fix für das "Tree klappt ein"-Problem.
        auto expanded = saveExpandState();
        m_model->refresh();
        restoreExpandState(expanded);

        // Knoten-Zähler + Spaltenbreite
        int count = m_agent->taskTree().nodeCount();
        m_nodeCountLabel->setText(QString("%1 Knoten").arg(count));
        m_treeView->resizeColumnToContents(0);

        // Kontext-Anzeige aktualisieren
        m_contextView->setPlainText(m_agent->taskTree().buildContext(node));
    }
}

// ─── onNodeActivated ─────────────────────────────────────────────────────────
// Doppelklick oder Enter → Edit-Dialog öffnen.
void PlannerDock::onNodeActivated(const QModelIndex &index)
{
    if (!index.isValid() || !m_model) return;
    openEditDialog(m_model->node(index));
}

// ═════════════════════════════════════════════════════════════════════════════
// KONTEXTMENÜ
// ═════════════════════════════════════════════════════════════════════════════

// ─── onContextMenu ────────────────────────────────────────────────────────────
// Baut das Kontextmenü dynamisch auf.
//
// "Geschwister hinzufügen" ist nur aktiv wenn der Knoten einen Elternknoten hat
// (H0-Wurzelknoten hat keinen Elter → kein Geschwister möglich).
// "Löschen" ist immer verfügbar wenn ein Knoten selektiert ist.
//
// QMenu::exec(globalPos) öffnet das Menü modal an der Mausposition.
// Analogie AVR: wie ein Interrupt-Handler der kontextabhängig verschiedene
// Aktionen auslöst — welche Aktion, entscheidet der Zustand (selektierter Knoten).
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
        // Styling: rote Farbe für destruktive Aktion
        deleteAct->setIcon(QIcon::fromTheme("edit-delete"));
        connect(deleteAct, &QAction::triggered, this, &PlannerDock::onDeleteNode);
    } else {
        // Kein Knoten unter der Maus — nur "Neu" anbieten wenn Tree leer
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

// ─── onEditNode ──────────────────────────────────────────────────────────────
void PlannerDock::onEditNode()
{
    openEditDialog(selectedNode());
}

// ─── onAddChild ──────────────────────────────────────────────────────────────
// Fügt einen leeren Kindknoten ein und öffnet sofort den Edit-Dialog.
//
// level = parent->level + 1  — Kind ist eine Ebene tiefer.
// scope = Internal           — Implementierungsdetail als sicherer Default.
// order = 0                  — wird in addChild() via insertionIdx sortiert.
//
// Ablauf:
//   1. Leerer Knoten erstellen + in Tree einhängen
//   2. Tree-View aktualisieren (refresh mit Expand-Erhalt)
//   3. Neuen Knoten selektieren + aufklappen
//   4. Edit-Dialog öffnen
//   Bei Abbrechen: Knoten bleibt leer im Tree (User kann ihn per Löschen entfernen).
void PlannerDock::onAddChild()
{
    TaskNode *parent = selectedNode();
    if (!parent) return;

    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());

    // Leerer Knoten — Titel "Neue Aufgabe" als Platzhalter
    TaskNode *child = tree->createNode(
        "Neue Aufgabe",
        "",
        parent->level + 1,
        TaskScope::Internal,
        0,
        parent);

    // Expand-Zustand sichern + refresh
    auto expanded = saveExpandState();
    // Elternknoten soll aufgeklappt bleiben
    expanded.insert(parent->id);
    m_model->refresh();
    restoreExpandState(expanded);

    m_nodeCountLabel->setText(
        QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
    m_treeView->resizeColumnToContents(0);

    // Neuen Knoten im View selektieren
    // Wir suchen seinen Index über das Model.
    // QAbstractItemModel::match() wäre möglich aber aufwändig.
    // Einfacher: traverse und Index über internalPointer vergleichen.
    std::function<QModelIndex(const QModelIndex&)> findIndex =
        [&](const QModelIndex &parent) -> QModelIndex {
            for (int row = 0; row < m_model->rowCount(parent); ++row) {
                QModelIndex idx = m_model->index(row, 0, parent);
                if (m_model->node(idx) == child) return idx;
                QModelIndex found = findIndex(idx);
                if (found.isValid()) return found;
            }
            return {};
        };

    QModelIndex childIdx = findIndex(QModelIndex{});
    if (childIdx.isValid()) {
        m_treeView->setCurrentIndex(childIdx);
        m_treeView->scrollTo(childIdx);
    }

    openEditDialog(child);
}

// ─── onAddSibling ────────────────────────────────────────────────────────────
// Fügt einen Geschwisterknoten ein — gleicher level, gleicher Elter.
// Nur verfügbar wenn der selektierte Knoten einen Elternknoten hat.
void PlannerDock::onAddSibling()
{
    TaskNode *sibling = selectedNode();
    if (!sibling || !sibling->parent) return;

    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());

    TaskNode *newNode = tree->createNode(
        "Neue Aufgabe",
        "",
        sibling->level,         // gleiche Ebene wie Geschwister
        sibling->scope,         // gleicher Scope als sinnvoller Default
        sibling->order + 1,     // nach dem aktuellen Geschwister einsortieren
        sibling->parent);       // gleicher Elter

    auto expanded = saveExpandState();
    expanded.insert(sibling->parent->id);
    m_model->refresh();
    restoreExpandState(expanded);

    m_nodeCountLabel->setText(
        QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
    m_treeView->resizeColumnToContents(0);

    // Neuen Knoten selektieren
    std::function<QModelIndex(const QModelIndex&)> findIndex =
        [&](const QModelIndex &parent) -> QModelIndex {
            for (int row = 0; row < m_model->rowCount(parent); ++row) {
                QModelIndex idx = m_model->index(row, 0, parent);
                if (m_model->node(idx) == newNode) return idx;
                QModelIndex found = findIndex(idx);
                if (found.isValid()) return found;
            }
            return {};
        };

    QModelIndex newIdx = findIndex(QModelIndex{});
    if (newIdx.isValid()) {
        m_treeView->setCurrentIndex(newIdx);
        m_treeView->scrollTo(newIdx);
    }

    openEditDialog(newNode);
}

// ─── onDeleteNode ────────────────────────────────────────────────────────────
// Löscht den selektierten Knoten + alle Kinder nach Sicherheitsabfrage.
//
// Sicherheitsabfrage zeigt:
//   - Titel des Knotens
//   - Anzahl der Kinder (damit User weiß was gelöscht wird)
//
// removeNode() ist in TaskTree implementiert — löscht rekursiv + bereinigt m_index.
void PlannerDock::onDeleteNode()
{
    TaskNode *node = selectedNode();
    if (!node) return;

    // Sicherheitsabfrage aufbauen
    int childCount = static_cast<int>(node->children.size());
    QString msg = QString("Knoten <b>%1</b> löschen?")
                  .arg(node->title.toHtmlEscaped());
    if (childCount > 0) {
        msg += QString("<br><br><b>Achtung:</b> Dieser Knoten hat %1 direkte(s) Kind(er). "
                       "Alle werden ebenfalls gelöscht.")
               .arg(childCount);
    }

    QMessageBox::StandardButton answer = QMessageBox::question(
        this,
        "Knoten löschen",
        msg,
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (answer != QMessageBox::Yes) return;

    TaskTree *tree = const_cast<TaskTree*>(&m_agent->taskTree());

    // Expand-Zustand sichern bevor wir löschen
    auto expanded = saveExpandState();
    expanded.remove(node->id);  // gelöschten Knoten aus expanded entfernen

    tree->removeNode(node);  // löscht Knoten + Kinder + bereinigt m_index

    m_model->refresh();
    restoreExpandState(expanded);

    m_contextView->clear();
    m_nodeCountLabel->setText(
        QString("%1 Knoten").arg(m_agent->taskTree().nodeCount()));
    m_treeView->resizeColumnToContents(0);
}

// ═════════════════════════════════════════════════════════════════════════════
// AGENT-SIGNALE
// ═════════════════════════════════════════════════════════════════════════════

void PlannerDock::onPlanReady()
{
    auto expanded = saveExpandState();
    m_model->refresh();
    // Bei planReady: alles aufklappen damit User den vollen Plan sieht
    m_treeView->expandAll();
    // expandAll() überschreibt restoreExpandState — hier bewusst nicht aufrufen

    m_statusLabel->setText(
        "Plan bereit — prüfen, ggf. Bearbeiten (Doppelklick/Rechtsklick), dann bestätigen.");
    m_statusLabel->setStyleSheet(
        "color: #e37400; font-size: 11px; font-weight: bold; padding: 2px;");

    m_confirmButton->show();
    m_rejectButton->show();

    int count = m_agent->taskTree().nodeCount();
    m_nodeCountLabel->setText(QString("%1 Knoten").arg(count));
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

    int count = m_agent->taskTree().nodeCount();
    m_nodeCountLabel->setText(QString("%1 Knoten").arg(count));
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
