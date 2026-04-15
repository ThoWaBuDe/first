#pragma once
// ─── TaskNode ─────────────────────────────────────────────────────────────────
// Ein Knoten im hierarchischen Aufgabenbaum des Coding-Agenten.
//
// Neu in v3:
//   result      — NUR sauberer Code (aus <code>...</code> extrahiert)
//   sideOutput  — Thinking-Blöcke, Warnungen, Erklärungen des Modells
//   buildPrompt — der vollständige Prompt der ans Modell geschickt wurde
//                 (Lichtkegel-Debugging: warum hat das Modell X ausgegeben?)

#include <QString>
#include <QDateTime>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <vector>
#include <algorithm>
#include <functional>

enum class TaskLevel : int {
    Goal       = 0,
    Files      = 1,
    Class      = 2,
    Impl       = 3,
    Validation = 4,
};

enum class TaskScope : int {
    Internal = 0,
    External = 1,
};

enum class TaskStatus : int {
    Pending  = 0,
    Running  = 1,
    Done     = 2,
    Failed   = 3,
    Blocked  = 4,
};

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
    QString   title;
    QString   description;

    // ── Execute-Output (getrennt) ─────────────────────────────────────────────
    // result:      NUR sauberer Code (aus <code>...</code> extrahiert)
    //              Leer = noch nicht implementiert
    // sideOutput:  Alles andere: Thinking, Warnungen, Erklärungen
    //              Nützlich um zu verstehen was das Modell "gedacht" hat
    // buildPrompt: Der vollständige Prompt (Lichtkegel: vertikal + horizontal + Thoughts)
    //              Für Debugging: man sieht genau welchen Kontext das Modell bekam
    //
    // Analogie AVR: wie drei separate Register — result ist der Ausgang,
    // sideOutput ist der Debug-Port, buildPrompt ist der Trace-Buffer.
    QString   result;
    QString   sideOutput;
    QString   buildPrompt;

    // ── Horizontale Abhängigkeiten ────────────────────────────────────────────
    QList<qint64> dependsOn;

    // ── Status ────────────────────────────────────────────────────────────────
    TaskStatus status    = TaskStatus::Pending;
    QDateTime  createdAt;
    QDateTime  updatedAt;
    bool       dirty     = false;

    // ── Baum-Struktur ─────────────────────────────────────────────────────────
    TaskNode              *parent   = nullptr;
    std::vector<TaskNode*> children;

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

    bool allChildrenDone() const {
        if (children.empty()) return false;
        for (const TaskNode *c : children)
            if (c->status != TaskStatus::Done) return false;
        return true;
    }

    bool isLeaf() const { return children.empty(); }
    bool isRoot() const { return parent == nullptr; }

    TaskLevel taskLevel() const { return static_cast<TaskLevel>(level); }

    std::vector<const TaskNode*> pathFromRoot() const {
        std::vector<const TaskNode*> path;
        const TaskNode *n = this;
        while (n) { path.push_back(n); n = n->parent; }
        std::reverse(path.begin(), path.end());
        return path;
    }

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

    // ── Dateityp-Erkennung ────────────────────────────────────────────────────
    // Wird von buildExecuteSystemPrompt() genutzt um dem Modell zu sagen
    // welche Sprache es ausgeben soll.
    enum class FileType { Cpp, CppHeader, CMake, Python, Text, Unknown };

    FileType fileType() const {
        QString t = title.toLower().trimmed();
        if (t.endsWith(".cpp") || t.endsWith(".cc") || t.endsWith(".cxx"))
            return FileType::Cpp;
        if (t.endsWith(".h") || t.endsWith(".hpp") || t.endsWith(".hxx"))
            return FileType::CppHeader;
        if (t == "cmakelists.txt" || t.endsWith(".cmake"))
            return FileType::CMake;
        if (t.endsWith(".py"))
            return FileType::Python;
        if (t.endsWith(".txt") || t.endsWith(".md"))
            return FileType::Text;
        return FileType::Unknown;
    }

    // Beschreibung des Dateityps für den System-Prompt
    QString fileTypeDescription() const {
        switch (fileType()) {
            case FileType::Cpp:       return "C++ Implementierungsdatei (.cpp)";
            case FileType::CppHeader: return "C++ Header-Datei (.h)";
            case FileType::CMake:     return "CMake Build-Konfigurationsdatei";
            case FileType::Python:    return "Python-Skript (.py)";
            case FileType::Text:      return "Textdatei";
            case FileType::Unknown:   return "Quellcode-Datei";
        }
        return "Quellcode-Datei";
    }

    // Sprachname für den System-Prompt ("C++", "CMake", ...)
    QString languageName() const {
        switch (fileType()) {
            case FileType::Cpp:
            case FileType::CppHeader: return "C++";
            case FileType::CMake:     return "CMake";
            case FileType::Python:    return "Python";
            case FileType::Text:      return "Text";
            case FileType::Unknown:   return "Code";
        }
        return "Code";
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
            {"side_output",   sideOutput},
            {"build_prompt",  buildPrompt},
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
