#pragma once
// ─── TaskTreeModel ────────────────────────────────────────────────────────────
// QAbstractItemModel für QTreeView. Zeigt TaskTree im UI.
//
// Spalten: Titel | Status | Ebene | Scope | Erstellt
// Tooltip: vollständiger buildContext() inkl. horizontaler Abhängigkeiten

#include "TaskTree.h"
#include <QAbstractItemModel>
#include <QColor>
#include <QFont>

class TaskTreeModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit TaskTreeModel(TaskTree *tree, QObject *parent = nullptr)
        : QAbstractItemModel(parent), m_tree(tree)
    {}

    int rowCount(const QModelIndex &parent = {}) const override
    {
        if (!parent.isValid())
            return static_cast<int>(m_tree->roots().size());
        TaskNode *n = node(parent);
        return n ? static_cast<int>(n->children.size()) : 0;
    }

    int columnCount(const QModelIndex & = {}) const override { return 5; }

    QVariant data(const QModelIndex &idx, int role = Qt::DisplayRole) const override
    {
        if (!idx.isValid()) return {};
        TaskNode *n = node(idx);
        if (!n) return {};

        if (role == Qt::DisplayRole) {
            switch (idx.column()) {
                case 0: return n->title;
                case 1: return TaskNode::statusName(n->status);
                case 2: return TaskNode::levelName(n->level);
                case 3: return TaskNode::scopeName(n->scope);
                case 4: return n->createdAt.toString("dd.MM. hh:mm");
            }
        }

        if (role == Qt::ForegroundRole && idx.column() == 1) {
            switch (n->status) {
                case TaskStatus::Pending: return QColor(Qt::gray);
                case TaskStatus::Running: return QColor(Qt::darkYellow);
                case TaskStatus::Done:    return QColor(Qt::darkGreen);
                case TaskStatus::Failed:  return QColor(Qt::red);
                case TaskStatus::Blocked: return QColor(255, 140, 0);
            }
        }

        if (role == Qt::FontRole && n->status == TaskStatus::Running) {
            QFont f; f.setBold(true); return f;
        }

        // Tooltip zeigt vollständigen Kontext inkl. dependsOn
        if (role == Qt::ToolTipRole && idx.column() == 0)
            return m_tree->buildContext(n);

        if (role == Qt::UserRole)
            return QVariant::fromValue(static_cast<void*>(n));

        return {};
    }

    QVariant headerData(int section, Qt::Orientation o,
                        int role = Qt::DisplayRole) const override
    {
        if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
            case 0: return "Aufgabe";
            case 1: return "Status";
            case 2: return "Ebene";
            case 3: return "Scope";
            case 4: return "Erstellt";
        }
        return {};
    }

    QModelIndex index(int row, int col,
                      const QModelIndex &parent = {}) const override
    {
        if (!hasIndex(row, col, parent)) return {};
        TaskNode *n = nullptr;
        if (!parent.isValid()) {
            if (row < static_cast<int>(m_tree->roots().size()))
                n = m_tree->roots()[row];
        } else {
            TaskNode *p = node(parent);
            if (p && row < static_cast<int>(p->children.size()))
                n = p->children[row];
        }
        return n ? createIndex(row, col, n) : QModelIndex{};
    }

    QModelIndex parent(const QModelIndex &child) const override
    {
        if (!child.isValid()) return {};
        TaskNode *n = node(child);
        TaskNode *p = n ? n->parent : nullptr;
        if (!p) return {};
        return createIndex(rowOf(p), 0, p);
    }

    // Kompletten Neuaufbau des Views anstoßen
    void refresh() { beginResetModel(); endResetModel(); }

    TaskNode *node(const QModelIndex &idx) const {
        return idx.isValid()
            ? static_cast<TaskNode*>(idx.internalPointer())
            : nullptr;
    }

private:
    TaskTree *m_tree;

    int rowOf(const TaskNode *n) const {
        if (!n) return 0;
        const auto &siblings = n->parent ? n->parent->children : m_tree->roots();
        for (int i = 0; i < static_cast<int>(siblings.size()); ++i)
            if (siblings[i] == n) return i;
        return 0;
    }
};
