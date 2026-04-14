#pragma once
// ─── TaskNodeDialog ───────────────────────────────────────────────────────────
// Edit-Dialog für einen einzelnen TaskNode.
//
// Öffnet sich per Doppelklick im PlannerDock-TreeView.
// Felder: title, description, result, level, scope, status, dependsOn.
//
// dependsOn-Widget: Zwei-Listen-Pattern (klassisches "Transfer Widget"):
//   Links  — alle Nodes im Tree (außer dem aktuell editierten), filterbar
//   Rechts — aktuell abhängige Nodes
//   Buttons: → (hinzufügen), ← (entfernen)
//
// Pattern: Dialog (GoF) — modaler Dialog, gibt Ergebnis per accept()/reject()
//          zurück. Kein direktes Schreiben in den Node — erst bei OK.
//
// Warum kein inline-Editing im TreeView?
//   QTreeView inline-Editing ist für einfache Strings gut.
//   description ist mehrzeilig, dependsOn ist eine Liste von IDs —
//   beides braucht eigene Widgets. Ein Dialog ist sauberer.

#include <QDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include "TaskTree.h"

class TaskNodeDialog : public QDialog {
    Q_OBJECT

public:
    // tree     — der gesamte TaskTree (für dependsOn-Liste)
    // node     — der zu editierende Knoten
    // parent   — Qt-Eltern-Widget
    explicit TaskNodeDialog(TaskTree *tree, TaskNode *node,
                            QWidget *parent = nullptr);

    // Schreibt die Änderungen in den Node zurück.
    // Wird von PlannerDock nach accept() aufgerufen.
    void applyToNode();

private slots:
    // Suchfeld-Filter: blendet nicht-passende Einträge in der linken Liste aus
    void onFilterChanged(const QString &text);

    // Transfer-Buttons
    void onAddDep();     // markierte Einträge links → rechts
    void onRemoveDep();  // markierte Einträge rechts → links

private:
    void setupUi();
    void populateFields();

    // Füllt die linke Liste mit allen Nodes außer m_node selbst.
    // Format: "H2: MainWindow.h (id=3)"
    void populateAvailableList();

    // Hilfsfunktion: Anzeige-String für einen Node
    static QString nodeLabel(const TaskNode *n);

    TaskTree *m_tree;
    TaskNode *m_node;

    // ── Felder ────────────────────────────────────────────────────────────
    QLineEdit     *m_titleEdit;
    QPlainTextEdit *m_descEdit;
    QPlainTextEdit *m_resultEdit;
    QComboBox     *m_levelCombo;   // H0..H5+ (int)
    QComboBox     *m_scopeCombo;   // internal / external
    QComboBox     *m_statusCombo;  // pending / running / done / failed / blocked

    // ── dependsOn Transfer-Widget ─────────────────────────────────────────
    QLineEdit   *m_filterEdit;     // Suchfeld über linker Liste
    QListWidget *m_availableList;  // links: alle Nodes (gefiltert)
    QListWidget *m_depsList;       // rechts: abhängige Nodes
    QPushButton *m_addBtn;         // →
    QPushButton *m_removeBtn;      // ←
};
