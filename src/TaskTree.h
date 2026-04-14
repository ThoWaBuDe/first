#pragma once
// ─── TaskTree ─────────────────────────────────────────────────────────────────
// Container für den hierarchischen Aufgabenbaum. RAM-first.
//
// Neu in v3:
//   removeNode() — löscht Knoten + alle Kinder, bereinigt m_index
//   clear()      — löscht kompletten RAM-Tree (DB bleibt erhalten)

#include "TaskNode.h"

#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QString>
#include <QDebug>
#include <vector>
#include <functional>

class TaskTree
{
public:
    explicit TaskTree(const QString &dbPath = {})
        : m_dbPath(dbPath), m_nextId(1)
    {}

    ~TaskTree() {
        for (TaskNode *root : m_roots)
            delete root;
    }

    // ── Knoten erstellen ──────────────────────────────────────────────────────
    TaskNode *createNode(const QString    &title,
                         const QString    &description,
                         int               level,
                         TaskScope         scope  = TaskScope::Internal,
                         int               order  = 0,
                         TaskNode         *parent = nullptr)
    {
        TaskNode *node    = new TaskNode();
        node->id          = m_nextId++;
        node->title       = title;
        node->description = description;
        node->level       = level;
        node->scope       = scope;
        node->order       = order;
        node->dirty       = true;   // neu erstellte Knoten müssen gespeichert werden

        if (parent) {
            parent->addChild(node);
        } else {
            node->insertionIdx = static_cast<int>(m_roots.size());
            m_roots.push_back(node);
        }

        m_index[node->id] = node;
        m_dirty = true;
        return node;
    }

    // ── Abhängigkeit hinzufügen ───────────────────────────────────────────────
    void addDependency(TaskNode *from, TaskNode *to)
    {
        if (!from || !to) return;
        if (from->dependsOn.contains(to->id)) return;
        from->dependsOn.append(to->id);
        from->dirty = true;
        m_dirty     = true;
    }

    void removeDependency(TaskNode *from, qint64 toId)
    {
        if (!from) return;
        from->dependsOn.removeAll(toId);
        from->dirty = true;
        m_dirty     = true;
    }

    // ── Knoten löschen ───────────────────────────────────────────────────────
    // Löscht einen Knoten + alle Kinder rekursiv aus dem Tree.
    // Bereinigt m_index für alle gelöschten Knoten.
    //
    // Analogie AVR: wie das Freigeben einer verketteten Liste — erst alle
    // Folgeelemente (Kinder) aus dem Index entfernen, dann delete.
    void removeNode(TaskNode *node)
    {
        if (!node) return;

        // Schritt 1: alle Knoten aus m_index entfernen
        std::function<void(TaskNode*)> removeFromIndex = [&](TaskNode *n) {
            m_index.remove(n->id);
            for (TaskNode *child : n->children)
                removeFromIndex(child);
        };
        removeFromIndex(node);

        // Schritt 2: aus Elter oder m_roots aushängen
        if (node->parent) {
            node->parent->removeChild(node);
        } else {
            auto it = std::find(m_roots.begin(), m_roots.end(), node);
            if (it != m_roots.end())
                m_roots.erase(it);
        }

        // Schritt 3: löschen (Destruktor löscht Kinder rekursiv)
        delete node;
        m_dirty = true;
    }

    // ── Kompletten RAM-Tree löschen ───────────────────────────────────────────
    // DB bleibt unberührt — nur RAM-Zustand wird zurückgesetzt.
    // Wird in Agent::startPlan() aufgerufen wenn /plan ein zweites Mal
    // ausgeführt wird — frischer Start ohne alten Plan-Ballast.
    //
    // Analogie AVR: wie ein Buffer-Reset vor neuem DMA-Transfer.
    void clear()
    {
        for (TaskNode *root : m_roots)
            delete root;   // Destruktor löscht Kinder rekursiv
        m_roots.clear();
        m_index.clear();
        m_nextId = 1;
        m_dirty  = false;
        // m_dbPath und m_db bleiben erhalten — kein erneutes setDbPath() nötig
    }

    // ── Kontext für das Modell bauen ──────────────────────────────────────────
    QString buildContext(const TaskNode *node) const
    {
        if (!node) return {};

        QString ctx = node->verticalContext();

        if (!node->dependsOn.isEmpty()) {
            ctx += "\n--- Abhängigkeiten ---\n";
            for (qint64 depId : node->dependsOn) {
                const TaskNode *dep = m_index.value(depId, nullptr);
                if (!dep) continue;
                ctx += QString("[%1] %2: %3\n")
                       .arg(TaskNode::levelName(dep->level))
                       .arg(TaskNode::scopeName(dep->scope))
                       .arg(dep->title);
                if (!dep->description.isEmpty())
                    ctx += dep->description + "\n";
                ctx += "\n";
            }
        }
        return ctx;
    }

    // ── Status setzen + Propagation ───────────────────────────────────────────
    void setStatus(TaskNode *node, TaskStatus status)
    {
        if (!node) return;
        node->status    = status;
        node->dirty     = true;
        node->updatedAt = QDateTime::currentDateTime();
        propagateUp(node->parent);
        m_dirty = true;
    }

    void setResult(TaskNode *node, const QString &result)
    {
        if (!node) return;
        node->result    = result;
        node->dirty     = true;
        node->updatedAt = QDateTime::currentDateTime();
        m_dirty = true;
    }

    // ── Lookup ────────────────────────────────────────────────────────────────
    TaskNode *findById(qint64 id) const {
        return m_index.value(id, nullptr);
    }

    TaskNode *nextPending() const {
        for (TaskNode *root : m_roots) {
            TaskNode *found = findNextPending(root);
            if (found) return found;
        }
        return nullptr;
    }

    // ── Traversierung ─────────────────────────────────────────────────────────
    void traverse(const std::function<void(TaskNode*)> &cb) {
        for (TaskNode *root : m_roots)
            traverseNode(root, cb);
    }
    void traverse(const std::function<void(const TaskNode*)> &cb) const {
        for (const TaskNode *root : m_roots)
            traverseNodeConst(root, cb);
    }

    // ── Persistenz ────────────────────────────────────────────────────────────
    bool save()
    {
        if (!openDb()) return false;
        QSqlQuery q(m_db);
        m_db.transaction();
        traverse([&](TaskNode *node) {
            if (!node->dirty) return;
            upsertNode(q, node);
            node->dirty = false;
        });
        m_db.commit();
        m_dirty = false;
        return true;
    }

    bool load()
    {
        if (!openDb()) return false;

        for (TaskNode *r : m_roots) delete r;
        m_roots.clear();
        m_index.clear();
        m_nextId = 1;

        QSqlQuery q("SELECT id, parent_id, level, scope, `order`, insertion_idx, "
                    "title, description, result, depends_on, status, "
                    "created_at, updated_at "
                    "FROM tasks ORDER BY id", m_db);

        QHash<qint64, TaskNode*> loaded;

        while (q.next()) {
            TaskNode *node    = new TaskNode();
            node->id          = q.value(0).toLongLong();
            qint64 parentId   = q.value(1).toLongLong();
            node->level       = q.value(2).toInt();
            node->scope       = TaskNode::scopeFromString(q.value(3).toString());
            node->order       = q.value(4).toInt();
            node->insertionIdx= q.value(5).toInt();
            node->title       = q.value(6).toString();
            node->description = q.value(7).toString();
            node->result      = q.value(8).toString();
            node->status      = TaskNode::statusFromString(q.value(10).toString());
            node->createdAt   = QDateTime::fromString(q.value(11).toString(), Qt::ISODate);
            node->updatedAt   = QDateTime::fromString(q.value(12).toString(), Qt::ISODate);
            node->dirty       = false;

            const QString depsStr = q.value(9).toString();
            if (!depsStr.isEmpty()) {
                for (const QString &s : depsStr.split(',', Qt::SkipEmptyParts))
                    node->dependsOn.append(s.trimmed().toLongLong());
            }

            loaded[node->id]   = node;
            m_index[node->id]  = node;
            if (node->id >= m_nextId) m_nextId = node->id + 1;

            if (parentId < 0) {
                m_roots.push_back(node);
            } else {
                TaskNode *par = loaded.value(parentId, nullptr);
                if (par) {
                    node->parent = par;
                    par->children.push_back(node);
                } else {
                    qWarning() << "TaskTree::load: parent" << parentId
                               << "not found for node" << node->id;
                    m_roots.push_back(node);
                }
            }
        }

        traverse([](TaskNode *node) {
            std::stable_sort(node->children.begin(), node->children.end(),
                [](const TaskNode *a, const TaskNode *b) {
                    if (a->order != b->order) return a->order < b->order;
                    return a->insertionIdx < b->insertionIdx;
                });
        });

        return true;
    }

    // ── Zustand ───────────────────────────────────────────────────────────────
    bool isDirty()   const { return m_dirty; }
    bool isEmpty()   const { return m_roots.empty(); }
    int  nodeCount() const { return static_cast<int>(m_index.size()); }
    const std::vector<TaskNode*> &roots() const { return m_roots; }

    void setDbPath(const QString &path) {
        if (m_db.isOpen()) m_db.close();
        m_dbPath = path;
    }

private:
    QString                  m_dbPath;
    QSqlDatabase             m_db;
    std::vector<TaskNode*>   m_roots;
    QHash<qint64, TaskNode*> m_index;
    qint64                   m_nextId;
    bool                     m_dirty = false;

    void propagateUp(TaskNode *node)
    {
        if (!node) return;
        TaskStatus newStatus = node->status;
        if (node->allChildrenDone()) {
            newStatus = TaskStatus::Done;
        } else {
            bool anyFailed = false, anyRunning = false;
            for (const TaskNode *c : node->children) {
                if (c->status == TaskStatus::Failed ||
                    c->status == TaskStatus::Blocked)  anyFailed  = true;
                if (c->status == TaskStatus::Running)  anyRunning = true;
            }
            if (anyFailed)       newStatus = TaskStatus::Blocked;
            else if (anyRunning) newStatus = TaskStatus::Running;
        }
        if (newStatus != node->status) {
            node->status    = newStatus;
            node->dirty     = true;
            node->updatedAt = QDateTime::currentDateTime();
            propagateUp(node->parent);
        }
    }

    static TaskNode *findNextPending(TaskNode *node)
    {
        if (!node) return nullptr;
        if (node->status == TaskStatus::Blocked ||
            node->status == TaskStatus::Done)    return nullptr;
        if (node->isLeaf() && node->status == TaskStatus::Pending)
            return node;
        for (TaskNode *child : node->children) {
            TaskNode *found = findNextPending(child);
            if (found) return found;
        }
        return nullptr;
    }

    static void traverseNode(TaskNode *n,
                              const std::function<void(TaskNode*)> &cb)
    { cb(n); for (TaskNode *c : n->children) traverseNode(c, cb); }

    static void traverseNodeConst(const TaskNode *n,
                                   const std::function<void(const TaskNode*)> &cb)
    { cb(n); for (const TaskNode *c : n->children) traverseNodeConst(c, cb); }

    bool openDb()
    {
        if (m_db.isOpen()) return true;
        if (m_dbPath.isEmpty()) { qWarning() << "TaskTree: no db path"; return false; }
        const QString connName = "taskdb_" + m_dbPath;
        if (QSqlDatabase::contains(connName)) {
            m_db = QSqlDatabase::database(connName);
        } else {
            m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
            m_db.setDatabaseName(m_dbPath);
        }
        if (!m_db.open()) {
            qWarning() << "TaskTree:" << m_db.lastError().text();
            return false;
        }
        QSqlQuery q(m_db);
        q.exec(
            "CREATE TABLE IF NOT EXISTS tasks ("
            "  id            INTEGER PRIMARY KEY,"
            "  parent_id     INTEGER DEFAULT -1,"
            "  level         INTEGER NOT NULL,"
            "  scope         TEXT    DEFAULT 'internal',"
            "  `order`       INTEGER DEFAULT 0,"
            "  insertion_idx INTEGER DEFAULT 0,"
            "  title         TEXT    NOT NULL,"
            "  description   TEXT,"
            "  result        TEXT,"
            "  depends_on    TEXT,"
            "  status        TEXT    DEFAULT 'pending',"
            "  created_at    TEXT,"
            "  updated_at    TEXT"
            ")"
        );
        return true;
    }

    void upsertNode(QSqlQuery &q, const TaskNode *node)
    {
        QStringList depStrs;
        for (qint64 d : node->dependsOn) depStrs << QString::number(d);

        q.prepare(
            "INSERT OR REPLACE INTO tasks "
            "(id, parent_id, level, scope, `order`, insertion_idx, "
            " title, description, result, depends_on, status, "
            " created_at, updated_at) "
            "VALUES (:id,:pid,:lv,:sc,:ord,:ins,:ti,:desc,:res,:dep,:st,:cr,:up)"
        );
        q.bindValue(":id",   node->id);
        q.bindValue(":pid",  node->parent ? node->parent->id : qint64(-1));
        q.bindValue(":lv",   node->level);
        q.bindValue(":sc",   TaskNode::scopeName(node->scope));
        q.bindValue(":ord",  node->order);
        q.bindValue(":ins",  node->insertionIdx);
        q.bindValue(":ti",   node->title);
        q.bindValue(":desc", node->description);
        q.bindValue(":res",  node->result);
        q.bindValue(":dep",  depStrs.join(','));
        q.bindValue(":st",   TaskNode::statusName(node->status));
        q.bindValue(":cr",   node->createdAt.toString(Qt::ISODate));
        q.bindValue(":up",   node->updatedAt.toString(Qt::ISODate));

        if (!q.exec())
            qWarning() << "TaskTree::upsertNode:" << q.lastError().text();
    }
};
