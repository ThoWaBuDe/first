#pragma once
#include <QDockWidget>
#include <QTreeView>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>
#include <QSet>
#include "Task/TaskTree.h"
#include "Task/TaskTreeModel.h"
#include "Agent/Agent.h"

// ─── PlannerDock ──────────────────────────────────────────────────────────────
// QDockWidget das den TaskTree als QTreeView anzeigt.
//
// Funktionen:
//   - Doppelklick / Kontextmenü "Bearbeiten"  → TaskNodeDialog
//   - Kontextmenü "Kind hinzufügen"            → neuer Kindknoten + Dialog
//   - Kontextmenü "Geschwister hinzufügen"     → neuer Geschwisterknoten + Dialog
//   - Kontextmenü "Löschen"                    → Sicherheitsabfrage + removeNode()
//   - "Plan laden" Button                      → TaskTree::load() aus SQLite
//
// Expand-Fix:
//   saveExpandState() / restoreExpandState() sichern/stellen IDs aufgeklappter
//   Knoten wieder her — verhindert das Einklappen nach refresh().

class PlannerDock : public QDockWidget {
    Q_OBJECT

public:
    explicit PlannerDock(Agent *agent, QWidget *parent = nullptr);

public slots:
    void onPlanReady();
    void onTreeUpdated();
    void onModeChanged(AgentMode mode);

private slots:
    void onSelectionChanged(const QModelIndex &current, const QModelIndex &previous);
    void onNodeActivated(const QModelIndex &index);

    // Kontextmenü
    void onContextMenu(const QPoint &pos);
    void onEditNode();
    void onAddChild();
    void onAddSibling();
    void onDeleteNode();

    // Plan aus DB laden
    void onLoadPlan();

private:
    void setupUi();
    void openEditDialog(TaskNode *node);

    // Expand-Zustand sichern / wiederherstellen
    // Analogie AVR: SREG sichern/wiederherstellen um cli/sei herum
    QSet<qint64> saveExpandState() const;
    void restoreExpandState(const QSet<qint64> &expanded);

    TaskNode *selectedNode() const;

    Agent          *m_agent;
    TaskTreeModel  *m_model  = nullptr;

    QTreeView   *m_treeView;
    QTextEdit   *m_contextView;
    QLabel      *m_statusLabel;
    QPushButton *m_confirmButton;
    QPushButton *m_rejectButton;
    QPushButton *m_loadButton = nullptr;
    QLabel      *m_nodeCountLabel;
};
