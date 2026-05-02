#pragma once
// ─── TaskNode ─────────────────────────────────────────────────────────────────
// Ein Knoten im hierarchischen Aufgabenbaum.
//
// NEU (Punkt H): symbol-Feld
// NEU (Punkt N): validationResult-Feld
// NEU (Import):  importStrategy-Feld

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
    Skip     = 5
};

// ─── ImportStrategy ───────────────────────────────────────────────────────────
// Steuert wie AgentImport eine Datei in Phase 2 analysiert.
//
// None     — kein Import (normaler Plan-Node, Phase 2 überspringt ihn)
// Header   — read_file → result befüllen, keine H3-Kinder
//            Für .h/.hpp Dateien: Interface, Member, Slots sind relevant
// Symbols  — list_symbols + get_function_body → H3-Nodes pro Methode
//            Für .cpp Dateien: Implementierung aufteilen
// ReadFile — read_file → result befüllen, keine H3-Kinder
//            Für CMakeLists.txt, *.md, *.ui, *.py etc.
// Skip     — Datei nicht importieren
//
// Das LLM setzt die Strategie in Phase 1 via create_node "import_strategy"-Parameter.
// LlamaQt liest das Feld in advanceImport() und steuert Phase 2 entsprechend.
//
// Analogie AVR: wie ein Peripheral-Mode-Register —
// das LLM konfiguriert den Modus, LlamaQt führt ihn aus.
enum class ImportStrategy : int {
    None     = 0,
    Header   = 1,
    Symbols  = 2,
    ReadFile = 3,
    Skip     = 4,
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

    // ── Symbol (Punkt H) ──────────────────────────────────────────────────────
    QString   symbol;

    // ── Execute-Output ────────────────────────────────────────────────────────
    QString   result;
    QString   sideOutput;
    QString   buildPrompt;

    // ── Validierung (Punkt N) ─────────────────────────────────────────────────
    QString   validationResult;

    // ── Import-Strategie (NEU) ────────────────────────────────────────────────
    ImportStrategy importStrategy = ImportStrategy::None;

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
        for (TaskNode *child : children) delete child;
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

    bool isValidationNode() const {
        return level == static_cast<int>(TaskLevel::Validation);
    }

    const TaskNode *validatedNode() const {
        if (!isValidationNode() || !parent) return nullptr;
        return parent;
    }

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
            ctx += QString("%1: %2\n")
                   .arg(TaskNode::levelName(n->level))
                   .arg(n->title);
        }
        if (!description.isEmpty())
            ctx += "---\n" + description;
        return ctx;
    }

    // ── Dateityp-Erkennung ────────────────────────────────────────────────────
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

    // ── ImportStrategy Konvertierung ──────────────────────────────────────────
    // String-basiert damit das LLM leserliche Namen verwenden kann.
    // Analogie AVR: enum↔string Mapping wie in einer Konfigurations-Tabelle.
    static ImportStrategy importStrategyFromString(const QString &s) {
        if (s == "header")   return ImportStrategy::Header;
        if (s == "symbols")  return ImportStrategy::Symbols;
        if (s == "read_file") return ImportStrategy::ReadFile;
        if (s == "skip")     return ImportStrategy::Skip;
        return ImportStrategy::None;
    }

    static QString importStrategyToString(ImportStrategy s) {
        switch (s) {
            case ImportStrategy::Header:   return "header";
            case ImportStrategy::Symbols:  return "symbols";
            case ImportStrategy::ReadFile: return "read_file";
            case ImportStrategy::Skip:     return "skip";
            default:                       return "none";
        }
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
            {"id",                id},
            {"parent_id",         parent ? parent->id : qint64(-1)},
            {"level",             level},
            {"scope",             scopeName(scope)},
            {"order",             order},
            {"insertion_idx",     insertionIdx},
            {"title",             title},
            {"description",       description},
            {"symbol",            symbol},
            {"result",            result},
            {"side_output",       sideOutput},
            {"build_prompt",      buildPrompt},
            {"validation_result", validationResult},
            {"import_strategy",   importStrategyToString(importStrategy)}, // NEU
            {"depends_on",        deps},
            {"status",            statusName(status)},
            {"created_at",        createdAt.toString(Qt::ISODate)},
            {"updated_at",        updatedAt.toString(Qt::ISODate)},
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
