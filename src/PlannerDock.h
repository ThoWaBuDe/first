#pragma once
#include <QDockWidget>
#include <QTreeView>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>
#include "TaskTree.h"
#include "TaskTreeModel.h"
#include "Agent.h"

// ─── PlannerDock ──────────────────────────────────────────────────────────────
// QDockWidget das den TaskTree als QTreeView anzeigt.
//
// Zwei Zustände:
//   Analyse-Phase  — Tree leer/wächst, Buttons ausgeblendet,
//                    Statuslabel "Analyse läuft..."
//   Approval-Phase — Tree voll, Buttons "Bestätigen" / "Ablehnen" sichtbar,
//                    Kontext-Anzeige unten
//
// Connections:
//   Agent::planReady()       → onPlanReady()      (zeigt Buttons)
//   Agent::taskTreeUpdated() → onTreeUpdated()    (refresh Model)
//   Agent::modeChanged()     → onModeChanged()    (Titel, Status)
//   confirmButton::clicked   → Agent::onPlanApproved()
//   rejectButton::clicked    → Agent::onPlanRejected()
//
// Pattern: Observer (via Qt Signals/Slots).
// PlannerDock beobachtet Agent-Signale und aktualisiert die Anzeige.
// Es kennt Agent nur über Zeiger — kein direkter Zugriff auf Internals.

class PlannerDock : public QDockWidget {
    Q_OBJECT

public:
    explicit PlannerDock(Agent *agent, QWidget *parent = nullptr);

public slots:
    // Wird von Agent::planReady() ausgelöst: Buttons einblenden
    void onPlanReady();

    // Wird von Agent::taskTreeUpdated() ausgelöst: QTreeView neu aufbauen
    void onTreeUpdated();

    // Wird von Agent::modeChanged() ausgelöst: Titel und Status anpassen
    void onModeChanged(AgentMode mode);

private slots:
    void onSelectionChanged(const QModelIndex &current, const QModelIndex &previous);

private:
    void setupUi();

    Agent          *m_agent;
    TaskTreeModel  *m_model  = nullptr;

    QTreeView  *m_treeView;
    QTextEdit  *m_contextView;    // zeigt buildContext() des selektierten Knotens
    QLabel     *m_statusLabel;
    QPushButton *m_confirmButton; // "Plan bestätigen"
    QPushButton *m_rejectButton;  // "Plan ablehnen"
    QLabel     *m_nodeCountLabel; // "N Knoten"
};
