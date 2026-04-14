#pragma once
#include <QDockWidget>
#include <QTreeView>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>
#include <QSet>
#include "TaskTree.h"
#include "TaskTreeModel.h"
#include "Agent.h"

// ─── PlannerDock ──────────────────────────────────────────────────────────────
// QDockWidget das den TaskTree als QTreeView anzeigt.
//
// Funktionen:
//   - Doppelklick / Kontextmenü "Bearbeiten"  → TaskNodeDialog
//   - Kontextmenü "Kind hinzufügen"            → neuer Kindknoten + Dialog
//   - Kontextmenü "Geschwister hinzufügen"     → neuer Geschwisterknoten + Dialog
//   - Kontextmenü "Löschen"                    → Sicherheitsabfrage + removeNode()
//
// Tree-Expand-Problem:
//   beginResetModel()/endResetModel() wirft den kompletten View-Zustand weg.
//   Lösung: saveExpandState() / restoreExpandState() speichern die IDs aller
//   aufgeklappten Knoten (als QSet<qint64>) und stellen sie danach wieder her.
//   Analogie AVR: wie Sichern/Wiederherstellen des SREG um cli/sei herum.

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
    void onNodeActivated(const QModelIndex &index);  // Doppelklick / Enter

    // Kontextmenü
    void onContextMenu(const QPoint &pos);
    void onEditNode();
    void onAddChild();
    void onAddSibling();
    void onDeleteNode();

private:
    void setupUi();

    // Edit-Dialog öffnen + bei OK: applyToNode() + refresh mit Expand-Erhalt
    void openEditDialog(TaskNode *node);

    // Expand-Zustand sichern / wiederherstellen
    QSet<qint64> saveExpandState() const;
    void restoreExpandState(const QSet<qint64> &expanded);

    // Aktuell selektierter Knoten (nullptr wenn keiner selektiert)
    TaskNode *selectedNode() const;

    Agent          *m_agent;
    TaskTreeModel  *m_model  = nullptr;

    QTreeView   *m_treeView;
    QTextEdit   *m_contextView;
    QLabel      *m_statusLabel;
    QPushButton *m_confirmButton;
    QPushButton *m_rejectButton;
    QLabel      *m_nodeCountLabel;
};
