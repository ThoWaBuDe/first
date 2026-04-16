#pragma once
// ─── PlanOptimizer ────────────────────────────────────────────────────────────
// Optimierungs-Phase zwischen Plan und Execute.
//
// Konzept (hybrider Ansatz):
//   Phase 1: Planner  → Grundstruktur H0/H1/H2 (wie jetzt, JSON auf einmal)
//   Phase 2: Optimizer → LLM verfeinert den Tree inkrementell via Tools
//   Phase 3: Execute  → implementieren
//
// Der Optimizer bekommt:
//   - Den kompletten Tree als Markdown-Übersicht
//   - Tree-Manipulation-Tools (create/edit/delete/list nodes)
//   - Aufgabe: Überlappungen entfernen, H3-Nodes sinnvoll aufteilen,
//              dependsOn korrekt setzen
//
// MCP-Tools die der Optimizer nutzt (neue Whitelist):
//   list_nodes()              — alle Nodes als Übersicht
//   get_node(id)              — einzelnen Node lesen
//   create_node(...)          — neuen Node anlegen
//   edit_node(id, ...)        — Node bearbeiten
//   delete_node(id)           — Node löschen
//   set_depends_on(from, to)  — Abhängigkeit setzen
//   remove_depends_on(from, to)
//
// Diese Tools werden NICHT als MCP-Server implementiert (zu komplex),
// sondern direkt im Agent als interne Tool-Handler — wie eine interne
// "Sandbox-API" für den Optimizer.
//
// Implementierungsplan:
//   Agent::startOptimize()
//   Agent::buildOptimizerSystemPrompt()
//   Agent::buildOptimizerPrompt()     — Tree als Markdown
//   Agent::handleOptimizerToolCall()  — interne Tools dispatchen
//   Agent::handleOptimizerDone()      — fertig → startExecute()

// ─── Tree als Markdown für den Optimizer-Prompt ───────────────────────────────
// Gibt den kompletten Tree als lesbare Übersicht aus.
// Format:
//   ## TaskTree Übersicht
//   [id=1] H0: Gesamtziel (done)
//     [id=2] H1: Projektstruktur (done)
//       [id=3] H2: CMakeLists.txt (pending)
//       [id=4] H2: main.cpp (pending)
//         [id=8] H3: Timer initialisieren (pending) → depends: [3]

#include "TaskTree.h"
#include <QString>

class PlanOptimizer
{
public:
    // ── Tree als Markdown-Übersicht ────────────────────────────────────────────
    // Wird im Optimizer-Prompt verwendet damit das LLM den Tree "sieht".
    static QString treeToMarkdown(const TaskTree &tree)
    {
        if (tree.isEmpty()) return "(leerer Tree)\n";

        QString md = "## TaskTree Übersicht\n\n";
        md += "Format: [id=N] Level: Titel (status) → depends: [id1, id2]\n\n";

        std::function<void(const TaskNode*, int)> render =
            [&](const TaskNode *node, int indent) {
                QString pad(indent * 2, ' ');
                QString deps;
                if (!node->dependsOn.isEmpty()) {
                    QStringList depStrs;
                    for (qint64 d : node->dependsOn)
                        depStrs << QString::number(d);
                    deps = " → depends: [" + depStrs.join(", ") + "]";
                }
                md += QString("%1[id=%2] %3: %4 (%5)%6\n")
                      .arg(pad)
                      .arg(node->id)
                      .arg(TaskNode::levelName(node->level))
                      .arg(node->title)
                      .arg(TaskNode::statusName(node->status))
                      .arg(deps);
                for (const TaskNode *child : node->children)
                    render(child, indent + 1);
            };

        for (const TaskNode *root : tree.roots())
            render(root, 0);

        return md;
    }

    // ── Optimizer System-Prompt ───────────────────────────────────────────────
    static QString buildSystemPrompt()
    {
        return QString(
            "Du bist ein Plan-Optimierer fuer C++/Qt6 Projekte.\n"
            "\n"
            "Du bekommst einen Aufgabenbaum (TaskTree) der vom Planner erstellt wurde.\n"
            "Deine Aufgabe: Den Tree optimieren bevor die Implementierung beginnt.\n"
            "\n"
            "ZIELE DER OPTIMIERUNG:\n"
            "1. Ueberlappende H3-Nodes zusammenfuehren\n"
            "   (z.B. 'Timer initialisieren' + 'Timer starten' → ein Node)\n"
            "2. Fehlende H3-Nodes ergaenzen wenn H2 zu viel auf einmal macht\n"
            "3. dependsOn korrekt setzen (welche Nodes brauchen welche Interfaces)\n"
            "4. Sinnlose oder leere Nodes loeschen\n"
            "5. Reihenfolge (order) der H3-Nodes sinnvoll setzen\n"
            "\n"
            "VERFUEGBARE TOOLS:\n"
            "\n"
            "list_nodes — alle Nodes anzeigen\n"
            "  <tool_call>{\"name\": \"list_nodes\", \"arguments\": {}}</tool_call>\n"
            "\n"
            "get_node — einzelnen Node lesen\n"
            "  <tool_call>{\"name\": \"get_node\", \"arguments\": {\"id\": 5}}</tool_call>\n"
            "\n"
            "create_node — neuen Node anlegen\n"
            "  <tool_call>{\"name\": \"create_node\", \"arguments\": {\n"
            "    \"title\": \"Titel\",\n"
            "    \"level\": 3,\n"
            "    \"scope\": \"internal\",\n"
            "    \"description\": \"Was zu tun ist\",\n"
            "    \"parent_id\": 7,\n"
            "    \"order\": 0\n"
            "  }}</tool_call>\n"
            "\n"
            "edit_node — Node bearbeiten\n"
            "  <tool_call>{\"name\": \"edit_node\", \"arguments\": {\n"
            "    \"id\": 5,\n"
            "    \"title\": \"Neuer Titel\",\n"
            "    \"description\": \"Neue Beschreibung\",\n"
            "    \"order\": 1\n"
            "  }}</tool_call>\n"
            "\n"
            "delete_node — Node loeschen (loescht auch alle Kinder!)\n"
            "  <tool_call>{\"name\": \"delete_node\", \"arguments\": {\"id\": 5}}</tool_call>\n"
            "\n"
            "set_depends_on — Abhaengigkeit setzen\n"
            "  <tool_call>{\"name\": \"set_depends_on\", \"arguments\": {\n"
            "    \"from_id\": 7, \"to_id\": 3\n"
            "  }}</tool_call>\n"
            "\n"
            "remove_depends_on — Abhaengigkeit entfernen\n"
            "  <tool_call>{\"name\": \"remove_depends_on\", \"arguments\": {\n"
            "    \"from_id\": 7, \"to_id\": 3\n"
            "  }}</tool_call>\n"
            "\n"
            "optimize_done — Optimierung abschliessen\n"
            "  <tool_call>{\"name\": \"optimize_done\", \"arguments\": {}}</tool_call>\n"
            "\n"
            "REGELN:\n"
            "- Beginne mit list_nodes um den Tree zu verstehen\n"
            "- Mache nur notwendige Aenderungen\n"
            "- Begruende jede Aenderung kurz\n"
            "- Schliesse mit optimize_done ab\n"
            "- Antworte auf Deutsch"
        );
    }

    // ── Optimizer User-Prompt ─────────────────────────────────────────────────
    static QString buildUserPrompt(const TaskTree &tree)
    {
        return QString(
            "Hier ist der aktuelle TaskTree:\n\n"
            "%1\n"
            "Bitte analysiere den Tree und optimiere ihn.\n"
            "Beginne mit list_nodes fuer die vollstaendige Uebersicht."
        ).arg(treeToMarkdown(tree));
    }

    // ── Interne Tool-Handler ──────────────────────────────────────────────────
    // Diese Methoden werden von Agent::handleOptimizerToolCall() aufgerufen.
    // Sie manipulieren den TaskTree direkt (kein MCP-Server nötig).

    struct ToolResult {
        bool    success = true;
        QString message;
    };

    static ToolResult handleListNodes(const TaskTree &tree)
    {
        return { true, treeToMarkdown(tree) };
    }

    static ToolResult handleGetNode(const TaskTree &tree,
                                     const QJsonObject &args)
    {
        qint64 id = static_cast<qint64>(args.value("id").toDouble(-1));
        const TaskNode *node = tree.findById(id);
        if (!node)
            return { false, QString("Node id=%1 nicht gefunden.").arg(id) };

        QString info = QString(
            "[id=%1] %2: %3\n"
            "Status: %4\n"
            "Scope: %5\n"
            "Order: %6\n"
            "Beschreibung: %7\n"
            "dependsOn: [%8]\n"
            "Kinder: %9"
        ).arg(node->id)
         .arg(TaskNode::levelName(node->level))
         .arg(node->title)
         .arg(TaskNode::statusName(node->status))
         .arg(TaskNode::scopeName(node->scope))
         .arg(node->order)
         .arg(node->description)
         .arg([&]() {
             QStringList l;
             for (qint64 d : node->dependsOn) l << QString::number(d);
             return l.join(", ");
         }())
         .arg(node->children.size());

        return { true, info };
    }

    static ToolResult handleCreateNode(TaskTree &tree,
                                        const QJsonObject &args)
    {
        QString title       = args.value("title").toString();
        int     level       = args.value("level").toInt(3);
        QString scopeStr    = args.value("scope").toString("internal");
        QString description = args.value("description").toString();
        qint64  parentId    = static_cast<qint64>(args.value("parent_id").toDouble(-1));
        int     order       = args.value("order").toInt(0);

        if (title.isEmpty())
            return { false, "title fehlt." };

        TaskNode *parent = (parentId >= 0) ? tree.findById(parentId) : nullptr;
        if (parentId >= 0 && !parent)
            return { false, QString("parent_id=%1 nicht gefunden.").arg(parentId) };

        TaskScope scope = (scopeStr == "external")
                          ? TaskScope::External : TaskScope::Internal;

        TaskNode *node = tree.createNode(title, description, level,
                                          scope, order, parent);
        return { true, QString("Node erstellt: id=%1 '%2'")
                       .arg(node->id).arg(title) };
    }

    static ToolResult handleEditNode(TaskTree &tree,
                                      const QJsonObject &args)
    {
        qint64 id = static_cast<qint64>(args.value("id").toDouble(-1));
        TaskNode *node = tree.findById(id);
        if (!node)
            return { false, QString("Node id=%1 nicht gefunden.").arg(id) };

        if (args.contains("title"))
            node->title = args.value("title").toString();
        if (args.contains("description"))
            node->description = args.value("description").toString();
        if (args.contains("order"))
            node->order = args.value("order").toInt();
        if (args.contains("scope")) {
            node->scope = (args.value("scope").toString() == "external")
                          ? TaskScope::External : TaskScope::Internal;
        }
        node->dirty     = true;
        node->updatedAt = QDateTime::currentDateTime();

        return { true, QString("Node id=%1 bearbeitet.").arg(id) };
    }

    static ToolResult handleDeleteNode(TaskTree &tree,
                                        const QJsonObject &args)
    {
        qint64 id = static_cast<qint64>(args.value("id").toDouble(-1));
        TaskNode *node = tree.findById(id);
        if (!node)
            return { false, QString("Node id=%1 nicht gefunden.").arg(id) };

        int childCount = static_cast<int>(node->children.size());
        QString title  = node->title;
        tree.removeNode(node);

        return { true, QString("Node id=%1 '%2' gelöscht (+ %3 Kinder).")
                       .arg(id).arg(title).arg(childCount) };
    }

    static ToolResult handleSetDependsOn(TaskTree &tree,
                                          const QJsonObject &args)
    {
        qint64 fromId = static_cast<qint64>(args.value("from_id").toDouble(-1));
        qint64 toId   = static_cast<qint64>(args.value("to_id").toDouble(-1));

        TaskNode *from = tree.findById(fromId);
        TaskNode *to   = tree.findById(toId);

        if (!from) return { false, QString("from_id=%1 nicht gefunden.").arg(fromId) };
        if (!to)   return { false, QString("to_id=%1 nicht gefunden.").arg(toId) };
        if (fromId == toId) return { false, "from_id und to_id dürfen nicht gleich sein." };

        tree.addDependency(from, to);
        return { true, QString("Abhängigkeit gesetzt: %1 → %2")
                       .arg(fromId).arg(toId) };
    }

    static ToolResult handleRemoveDependsOn(TaskTree &tree,
                                             const QJsonObject &args)
    {
        qint64 fromId = static_cast<qint64>(args.value("from_id").toDouble(-1));
        qint64 toId   = static_cast<qint64>(args.value("to_id").toDouble(-1));

        TaskNode *from = tree.findById(fromId);
        if (!from) return { false, QString("from_id=%1 nicht gefunden.").arg(fromId) };

        tree.removeDependency(from, toId);
        return { true, QString("Abhängigkeit entfernt: %1 → %2")
                       .arg(fromId).arg(toId) };
    }

    // Dispatch: Tool-Name → Handler
    static ToolResult dispatch(const QString &toolName,
                                const QJsonObject &args,
                                TaskTree &tree)
    {
        if (toolName == "list_nodes")        return handleListNodes(tree);
        if (toolName == "get_node")          return handleGetNode(tree, args);
        if (toolName == "create_node")       return handleCreateNode(tree, args);
        if (toolName == "edit_node")         return handleEditNode(tree, args);
        if (toolName == "delete_node")       return handleDeleteNode(tree, args);
        if (toolName == "set_depends_on")    return handleSetDependsOn(tree, args);
        if (toolName == "remove_depends_on") return handleRemoveDependsOn(tree, args);
        if (toolName == "optimize_done")     return { true, "__OPTIMIZE_DONE__" };
        return { false, QString("Unbekanntes Tool: '%1'").arg(toolName) };
    }

    // Optimizer-Whitelist
    static bool isOptimizerTool(const QString &name)
    {
        static const QStringList tools = {
            "list_nodes", "get_node", "create_node", "edit_node",
            "delete_node", "set_depends_on", "remove_depends_on",
            "optimize_done"
        };
        return tools.contains(name);
    }
};
