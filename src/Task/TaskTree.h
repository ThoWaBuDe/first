#pragma once
// ─── TaskTree ─────────────────────────────────────────────────────────────────
// RAM-first Aufgabenbaum mit SQLite-Persistenz.
//
// NEU (Punkt H): Symbol-Index + symbolExists()
//   Schneller Lookup: "existiert dieses Symbol schon?" → O(1)
//   Duplikat-Erkennung für Optimizer und iterativen Plan-Aufbau.
//
// NEU (Punkt N): createValidationNode() + setValidationResult()
//   Erzeugt nach einem H3-Node automatisch einen H4-Validierungs-Node.

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
        for (TaskNode *root : m_roots) delete root;
    }

    // ── Knoten erstellen ──────────────────────────────────────────────────────
    TaskNode *createNode(const QString &title,
                         const QString &description,
                         int            level,
                         TaskScope      scope  = TaskScope::Internal,
                         int            order  = 0,
                         TaskNode      *parent = nullptr,
                         const QString &symbol = {})
    {
        TaskNode *node    = new TaskNode();
        node->id          = m_nextId++;
        node->title       = title;
        node->description = description;
        node->level       = level;
        node->scope       = scope;
        node->order       = order;
        node->symbol      = symbol;
        node->dirty       = true;

        if (parent) {
            parent->addChild(node);
        } else {
            node->insertionIdx = static_cast<int>(m_roots.size());
            m_roots.push_back(node);
        }

        m_index[node->id] = node;

        // Symbol-Index befüllen (NEU — Punkt H)
        if (!symbol.isEmpty())
            m_symbolIndex[symbol] = node->id;

        m_dirty = true;
        return node;
    }

    // ── Validierungs-Node erstellen (NEU — Punkt N) ───────────────────────────
    // Erzeugt einen H4-Node als Kind des übergebenen H3-Nodes.
    // Wird automatisch von AgentExecute nach handleExecuteCode() aufgerufen.
    //
    // Der Validierungs-Node bekommt:
    //   title:       "Validierung: <H3-Titel>"
    //   description: Was geprüft werden soll
    //   level:       TaskLevel::Validation (4)
    //   parent:      der H3-Node
    //
    // Warum als Kind des H3-Nodes statt als Geschwister?
    //   Der H3-Node ist erst "wirklich Done" wenn seine Validierung grün ist.
    //   Als Kind propagiert TaskStatus::Done nur wenn alle Kinder Done sind.
    //   Das erzwingt die Validierung automatisch im Execute-Ablauf.
    TaskNode *createValidationNode(TaskNode *implNode)
    {
        if (!implNode) return nullptr;

        QString desc = QString(
            "Prüfe die Implementierung von '%1'.\n\n"
            "Symbol: %2\n\n"
            "Prüfkriterien:\n"
            "- Korrekte Signatur laut Interface (dependsOn)\n"
            "- Keine offensichtlichen Logikfehler\n"
            "- Keine fehlenden return-Statements\n"
            "- Keine undefinierten Variablen\n"
            "- Passt zur Beschreibung der Aufgabe\n\n"
            "Antworte mit genau einer Zeile:\n"
            "  'ok' — wenn die Implementierung korrekt ist\n"
            "  '<Fehlerbeschreibung>' — wenn ein Problem gefunden wurde"
        ).arg(implNode->title,
              implNode->symbol.isEmpty() ? "(kein Symbol)" : implNode->symbol);

        TaskNode *valNode = createNode(
            QString("Validierung: %1").arg(implNode->title),
            desc,
            static_cast<int>(TaskLevel::Validation),
            TaskScope::Internal,
            implNode->order,  // gleiche Reihenfolge
            implNode          // Kind des H3-Nodes
        );

        return valNode;
    }

    // ── Knoten löschen ────────────────────────────────────────────────────────
    void removeNode(TaskNode *node)
    {
        if (!node) return;
        std::function<void(TaskNode*)> removeFromIndex = [&](TaskNode *n) {
            m_index.remove(n->id);
            if (!n->symbol.isEmpty()) m_symbolIndex.remove(n->symbol);
            for (TaskNode *child : n->children) removeFromIndex(child);
        };
        removeFromIndex(node);

        if (node->parent) {
            node->parent->removeChild(node);
        } else {
            auto it = std::find(m_roots.begin(), m_roots.end(), node);
            if (it != m_roots.end()) m_roots.erase(it);
        }
        delete node;
        m_dirty = true;
    }

    // ── RAM-Tree leeren ───────────────────────────────────────────────────────
    void clear()
    {
        for (TaskNode *root : m_roots) delete root;
        m_roots.clear();
        m_index.clear();
        m_symbolIndex.clear();  // NEU
        m_nextId = 1;
        m_dirty  = false;
    }

    // ── Symbol-Lookup (NEU — Punkt H) ─────────────────────────────────────────
    // Prüft ob ein Symbol bereits in einem Node implementiert wird.
    // O(1) dank Hash-Index.
    //
    // Verwendung im iterativen Plan-Aufbau (Punkt G):
    //   if (taskTree.symbolExists("Timer::start()"))
    //       → dependsOn statt neue Implementierung
    bool symbolExists(const QString &symbol) const {
        return !symbol.isEmpty() && m_symbolIndex.contains(symbol);
    }

    // Gibt den Node zurück der das Symbol implementiert, oder nullptr.
    TaskNode *findBySymbol(const QString &symbol) const {
        if (symbol.isEmpty()) return nullptr;
        qint64 id = m_symbolIndex.value(symbol, -1);
        if (id < 0) return nullptr;
        return m_index.value(id, nullptr);
    }

    // Symbol eines Nodes aktualisieren (z.B. nach Edit im Dialog)
    void setSymbol(TaskNode *node, const QString &symbol)
    {
        if (!node) return;
        // Alten Symbol-Eintrag entfernen
        if (!node->symbol.isEmpty())
            m_symbolIndex.remove(node->symbol);
        // Neuen Eintrag setzen
        node->symbol = symbol;
        if (!symbol.isEmpty())
            m_symbolIndex[symbol] = node->id;
        node->dirty     = true;
        node->updatedAt = QDateTime::currentDateTime();
        m_dirty = true;
    }

    // ── Validierungsergebnis setzen (NEU — Punkt N) ───────────────────────────
    void setValidationResult(TaskNode *node, const QString &result)
    {
        if (!node) return;
        node->validationResult = result;
        node->dirty     = true;
        node->updatedAt = QDateTime::currentDateTime();
        m_dirty = true;
    }

    // ── Abhängigkeiten ────────────────────────────────────────────────────────
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

    // ── Kontext für das Modell ────────────────────────────────────────────────
    QString buildContext(const TaskNode *node) const
    {
        if (!node) return {};
        QString ctx = node->verticalContext();

        if (!node->symbol.isEmpty())
            ctx += QString("\nSymbol: %1\n").arg(node->symbol);

        if (!node->dependsOn.isEmpty()) {
            ctx += "\n--- Abhängigkeiten ---\n";
            for (qint64 depId : node->dependsOn) {
                const TaskNode *dep = m_index.value(depId, nullptr);
                if (!dep) continue;
                ctx += QString("[%1] %2: %3\n")
                       .arg(TaskNode::levelName(dep->level))
                       .arg(TaskNode::scopeName(dep->scope))
                       .arg(dep->title);
                if (!dep->symbol.isEmpty())
                    ctx += QString("  Symbol: %1\n").arg(dep->symbol);
                if (!dep->description.isEmpty())
                    ctx += dep->description + "\n";
                ctx += "\n";
            }
        }
        return ctx;
    }

    // ── Status / Result setzen ────────────────────────────────────────────────
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

    void setSideOutput(TaskNode *node, const QString &sideOutput)
    {
        if (!node) return;
        node->sideOutput = sideOutput;
        node->dirty      = true;
        node->updatedAt  = QDateTime::currentDateTime();
        m_dirty = true;
    }

    void setBuildPrompt(TaskNode *node, const QString &prompt)
    {
        if (!node) return;
        node->buildPrompt = prompt;
        node->dirty       = true;
        node->updatedAt   = QDateTime::currentDateTime();
        m_dirty = true;
    }

    // ── Lookup ────────────────────────────────────────────────────────────────
    TaskNode *findById(qint64 id) const {
        return m_index.value(id, nullptr);
    }

    // nextPending überspringt Validierungs-Nodes NICHT —
    // sie werden wie normale Nodes ausgeführt (der Prompt ist anders).
    TaskNode *nextPending() const {
        for (TaskNode *root : m_roots) {
            TaskNode *found = findNextPending(root);
            if (found) return found;
        }
        return nullptr;
    }

    // ── Symbol-Statistiken (für Debug/UI) ────────────────────────────────────
    int symbolCount() const { return m_symbolIndex.size(); }

    QStringList allSymbols() const {
        return m_symbolIndex.keys();
    }

    // ── Traversierung ─────────────────────────────────────────────────────────
    void traverse(const std::function<void(TaskNode*)> &cb) {
        for (TaskNode *root : m_roots) traverseNode(root, cb);
    }
    void traverse(const std::function<void(const TaskNode*)> &cb) const {
        for (const TaskNode *root : m_roots) traverseNodeConst(root, cb);
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
        m_symbolIndex.clear(); // NEU
        m_nextId = 1;

        QSqlQuery q("SELECT id, parent_id, level, scope, `order`, insertion_idx, "
                    "title, description, symbol, result, side_output, build_prompt, "
                    "validation_result, depends_on, status, created_at, updated_at "
                    "FROM tasks ORDER BY id", m_db);

        QHash<qint64, TaskNode*> loaded;

        while (q.next()) {
            TaskNode *node      = new TaskNode();
            node->id            = q.value(0).toLongLong();
            qint64 parentId     = q.value(1).toLongLong();
            node->level         = q.value(2).toInt();
            node->scope         = TaskNode::scopeFromString(q.value(3).toString());
            node->order         = q.value(4).toInt();
            node->insertionIdx  = q.value(5).toInt();
            node->title         = q.value(6).toString();
            node->description   = q.value(7).toString();
            node->symbol        = q.value(8).toString();   // NEU
            node->result        = q.value(9).toString();
            node->sideOutput    = q.value(10).toString();
            node->buildPrompt   = q.value(11).toString();
            node->validationResult = q.value(12).toString(); // NEU
            node->status        = TaskNode::statusFromString(q.value(14).toString());
            node->createdAt     = QDateTime::fromString(q.value(15).toString(), Qt::ISODate);
            node->updatedAt     = QDateTime::fromString(q.value(16).toString(), Qt::ISODate);
            node->dirty         = false;

            const QString depsStr = q.value(13).toString();
            if (!depsStr.isEmpty()) {
                for (const QString &s : depsStr.split(',', Qt::SkipEmptyParts))
                    node->dependsOn.append(s.trimmed().toLongLong());
            }

            loaded[node->id]   = node;
            m_index[node->id]  = node;
            if (!node->symbol.isEmpty())
                m_symbolIndex[node->symbol] = node->id; // NEU

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
    QHash<QString, qint64>   m_symbolIndex; // NEU: Symbol → Node-ID
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
                    c->status == TaskStatus::Blocked) anyFailed  = true;
                if (c->status == TaskStatus::Running) anyRunning = true;
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
        if (QSqlDatabase::contains(connName))
            m_db = QSqlDatabase::database(connName);
        else {
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
            "  id                INTEGER PRIMARY KEY,"
            "  parent_id         INTEGER DEFAULT -1,"
            "  level             INTEGER NOT NULL,"
            "  scope             TEXT    DEFAULT 'internal',"
            "  `order`           INTEGER DEFAULT 0,"
            "  insertion_idx     INTEGER DEFAULT 0,"
            "  title             TEXT    NOT NULL,"
            "  description       TEXT,"
            "  symbol            TEXT,"
            "  result            TEXT,"
            "  side_output       TEXT,"
            "  build_prompt      TEXT,"
            "  validation_result TEXT,"
            "  depends_on        TEXT,"
            "  status            TEXT    DEFAULT 'pending',"
            "  created_at        TEXT,"
            "  updated_at        TEXT"
            ")"
        );

        // ── Migration: neue Spalten für bestehende DBs ────────────────────
        // Analogie AVR: Bootloader prüft Firmware-Version, lädt fehlende Teile.
        auto addColIfMissing = [&](const QString &col, const QString &type) {
            QSqlQuery check(m_db);
            check.exec(QString("SELECT %1 FROM tasks LIMIT 1").arg(col));
            if (check.lastError().isValid()) {
                QSqlQuery alter(m_db);
                alter.exec(QString("ALTER TABLE tasks ADD COLUMN %1 %2")
                           .arg(col, type));
            }
        };
        addColIfMissing("side_output",       "TEXT");
        addColIfMissing("build_prompt",      "TEXT");
        addColIfMissing("symbol",            "TEXT"); // NEU
        addColIfMissing("validation_result", "TEXT"); // NEU

        return true;
    }

    void upsertNode(QSqlQuery &q, const TaskNode *node)
    {
        QStringList depStrs;
        for (qint64 d : node->dependsOn) depStrs << QString::number(d);

        q.prepare(
            "INSERT OR REPLACE INTO tasks "
            "(id, parent_id, level, scope, `order`, insertion_idx, "
            " title, description, symbol, result, side_output, build_prompt, "
            " validation_result, depends_on, status, created_at, updated_at) "
            "VALUES (:id,:pid,:lv,:sc,:ord,:ins,:ti,:desc,:sym,:res,"
            "        :sout,:bprompt,:valres,:dep,:st,:cr,:up)"
        );
        q.bindValue(":id",      node->id);
        q.bindValue(":pid",     node->parent ? node->parent->id : qint64(-1));
        q.bindValue(":lv",      node->level);
        q.bindValue(":sc",      TaskNode::scopeName(node->scope));
        q.bindValue(":ord",     node->order);
        q.bindValue(":ins",     node->insertionIdx);
        q.bindValue(":ti",      node->title);
        q.bindValue(":desc",    node->description);
        q.bindValue(":sym",     node->symbol);            // NEU
        q.bindValue(":res",     node->result);
        q.bindValue(":sout",    node->sideOutput);
        q.bindValue(":bprompt", node->buildPrompt);
        q.bindValue(":valres",  node->validationResult);  // NEU
        q.bindValue(":dep",     depStrs.join(','));
        q.bindValue(":st",      TaskNode::statusName(node->status));
        q.bindValue(":cr",      node->createdAt.toString(Qt::ISODate));
        q.bindValue(":up",      node->updatedAt.toString(Qt::ISODate));

        if (!q.exec())
            qWarning() << "TaskTree::upsertNode:" << q.lastError().text();
    }
};
