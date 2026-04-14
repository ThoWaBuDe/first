#pragma once
// ─── TaskNode ─────────────────────────────────────────────────────────────────
// Ein Knoten im hierarchischen Aufgabenbaum des Coding-Agenten.
//
// Zwei Achsen:
//   Vertikal   — Eltern/Kind (H0→H4+, Aufgaben-Hierarchie)
//   Horizontal — dependsOn   (Abhängigkeiten zwischen beliebigen Knoten)
//
// dependsOn ist bewusst offen — kein Constraint auf H2 oder bestimmte Scopes.
// Emergenz entscheidet welche Dependency-Typen sinnvoll sind.
//
// Ownership: Jeder Knoten besitzt seine Kinder (Destruktor löscht rekursiv).
// Parent-Pointer ist nicht-owning.

#include <QString>
#include <QDateTime>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <vector>
#include <algorithm>
#include <functional>

// ─── TaskLevel ────────────────────────────────────────────────────────────────
// Benannte Konstanten für bekannte Ebenen.
// Member `level` bleibt int — Tiefe ist flexibel (H5, H6, ... möglich).
//
// Verwendung:
//   node->level = static_cast<int>(TaskLevel::Class);
//   if (node->level == static_cast<int>(TaskLevel::Impl)) { ... }
//   TaskNode::levelName(node->level)  → "H2:Klasse"
enum class TaskLevel : int {
    Goal       = 0,   // H0 — Gesamtziel
    Files      = 1,   // H1 — Dateiliste
    Class      = 2,   // H2 — Klasse / Übersetzungseinheit / Header
    Impl       = 3,   // H3 — Implementierung (variabel, aufspaltbar)
    Validation = 4,   // H4 — Validierung, Compilerfehler, Tests
    // H5+ via direktem int — kein Maximum
};

// ─── TaskScope ────────────────────────────────────────────────────────────────
// Klassenintern vs. öffentliches Interface.
// Primär für H2, aber bewusst für alle Ebenen offen.
//
// Internal: private/protected — Implementierungsdetail einer Klasse
// External: public — Interface das andere Klassen/Module nutzen
enum class TaskScope : int {
    Internal = 0,
    External = 1,
};

// ─── TaskStatus ───────────────────────────────────────────────────────────────
enum class TaskStatus : int {
    Pending  = 0,   // grau
    Running  = 1,   // gelb
    Done     = 2,   // grün
    Failed   = 3,   // rot
    Blocked  = 4,   // orange (Kinder haben Fehler)
};

// ─── TaskNode ─────────────────────────────────────────────────────────────────
struct TaskNode
{
    // ── Identität ─────────────────────────────────────────────────────────────
    qint64    id           = 0;
    int       level        = static_cast<int>(TaskLevel::Goal);
    int       order        = 0;
    int       insertionIdx = 0;

    // ── Klassifikation ────────────────────────────────────────────────────────
    TaskScope scope = TaskScope::Internal;

    // ── Inhalt ────────────────────────────────────────────────────────────────
    QString   title;        // Kurztitel (1 Zeile)
    QString   description;  // Aufgabe + Kontext + Impl-Hints + Signaturen
    QString   result;       // Was der Agent produziert hat

    // ── Horizontale Abhängigkeiten ────────────────────────────────────────────
    // IDs beliebiger anderer Knoten. Offen — keine Einschränkung auf H2.
    // Gesetzt vom Planner oder manuell im UI.
    // TaskTree::buildContext() löst diese IDs auf und hängt
    // Titel + Description der Zielknoten an den Modell-Kontext an.
    QList<qint64> dependsOn;

    // ── Status ────────────────────────────────────────────────────────────────
    TaskStatus status    = TaskStatus::Pending;
    QDateTime  createdAt;
    QDateTime  updatedAt;
    bool       dirty     = false;

    // ── Baum-Struktur ─────────────────────────────────────────────────────────
    TaskNode              *parent   = nullptr;   // nicht-owning
    std::vector<TaskNode*> children;              // owning

    // ── Konstruktor / Destruktor ───────────────────────────────────────────────
    TaskNode()
        : createdAt(QDateTime::currentDateTime())
        , updatedAt(QDateTime::currentDateTime())
    {}

    ~TaskNode() {
        for (TaskNode *child : children)
            delete child;
    }

    TaskNode(const TaskNode &) = delete;
    TaskNode &operator=(const TaskNode &) = delete;

    // ── Kinder-Verwaltung ─────────────────────────────────────────────────────

    void addChild(TaskNode *child) {
        child->parent       = this;
        child->insertionIdx = static_cast<int>(children.size());
        children.push_back(child);
        sortChildren();
        markDirty();
    }

    TaskNode *removeChild(TaskNode *child) {
        auto it = std::find(children.begin(), children.end(), child);
        if (it == children.end()) return nullptr;
        children.erase(it);
        child->parent = nullptr;
        markDirty();
        return child;
    }

    // ── Abfragen ──────────────────────────────────────────────────────────────

    bool allChildrenDone() const {
        if (children.empty()) return false;
        for (const TaskNode *c : children)
            if (c->status != TaskStatus::Done) return false;
        return true;
    }

    bool isLeaf() const { return children.empty(); }
    bool isRoot() const { return parent == nullptr; }

    // Bequemer Zugriff auf das enum
    TaskLevel taskLevel() const {
        return static_cast<TaskLevel>(level);
    }

    // Pfad von Wurzel bis zu diesem Knoten
    std::vector<const TaskNode*> pathFromRoot() const {
        std::vector<const TaskNode*> path;
        const TaskNode *n = this;
        while (n) { path.push_back(n); n = n->parent; }
        std::reverse(path.begin(), path.end());
        return path;
    }

    // Vertikaler Kontext (Pfad H0→aktuell).
    // Horizontaler Teil (dependsOn) wird von TaskTree::buildContext() ergänzt.
    QString verticalContext() const {
        QString ctx;
        for (const TaskNode *n : pathFromRoot()) {
            ctx += QString("%1%2: %3\n")
                   .arg(levelName(n->level))
                   .arg(n == this ? " (aktuell)" : "")
                   .arg(n->title);
        }
        if (!description.isEmpty())
            ctx += "---\n" + description;
        return ctx;
    }

    // ── String-Konvertierungen ────────────────────────────────────────────────

    static QString levelName(int l) {
        switch (l) {
            case static_cast<int>(TaskLevel::Goal):       return "H0:Ziel";
            case static_cast<int>(TaskLevel::Files):      return "H1:Dateien";
            case static_cast<int>(TaskLevel::Class):      return "H2:Klasse";
            case static_cast<int>(TaskLevel::Impl):       return "H3:Impl";
            case static_cast<int>(TaskLevel::Validation): return "H4:Validierung";
            default: return QString("H%1").arg(l);
        }
    }

    static QString scopeName(TaskScope s) {
        return s == TaskScope::External ? "external" : "internal";
    }
    static TaskScope scopeFromString(const QString &s) {
        return s == "external" ? TaskScope::External : TaskScope::Internal;
    }

    static QString statusName(TaskStatus s) {
        switch (s) {
            case TaskStatus::Pending: return "pending";
            case TaskStatus::Running: return "running";
            case TaskStatus::Done:    return "done";
            case TaskStatus::Failed:  return "failed";
            case TaskStatus::Blocked: return "blocked";
        }
        return "pending";
    }
    static TaskStatus statusFromString(const QString &s) {
        if (s == "running") return TaskStatus::Running;
        if (s == "done")    return TaskStatus::Done;
        if (s == "failed")  return TaskStatus::Failed;
        if (s == "blocked") return TaskStatus::Blocked;
        return TaskStatus::Pending;
    }

    // ── Serialisierung ────────────────────────────────────────────────────────
    QJsonObject toJson() const {
        QJsonArray deps;
        for (qint64 d : dependsOn) deps.append(d);
        return QJsonObject{
            {"id",            id},
            {"parent_id",     parent ? parent->id : qint64(-1)},
            {"level",         level},
            {"scope",         scopeName(scope)},
            {"order",         order},
            {"insertion_idx", insertionIdx},
            {"title",         title},
            {"description",   description},
            {"result",        result},
            {"depends_on",    deps},
            {"status",        statusName(status)},
            {"created_at",    createdAt.toString(Qt::ISODate)},
            {"updated_at",    updatedAt.toString(Qt::ISODate)},
        };
    }

private:
    void markDirty() {
        dirty     = true;
        updatedAt = QDateTime::currentDateTime();
    }

    void sortChildren() {
        std::stable_sort(children.begin(), children.end(),
            [](const TaskNode *a, const TaskNode *b) {
                if (a->order != b->order) return a->order < b->order;
                return a->insertionIdx < b->insertionIdx;
            });
    }
};
