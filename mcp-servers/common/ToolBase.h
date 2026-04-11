#pragma once
// ─── ToolBase ─────────────────────────────────────────────────────────────────
// Abstrakte Basisklasse für alle MCP-Tools.
//
// Pattern: Abstract Base Class (Schnittstelle) + Template Method
//
// Jedes konkrete Tool (ReadFileTool, WriteFileTool, ...) erbt von ToolBase
// und implementiert die vier reinen virtuellen Methoden.
// McpServer hält std::unique_ptr<ToolBase> und ruft diese Methoden direkt
// auf — kein Tool-Struct, kein toMcpTool()-Konvertierer nötig.
//
// Dependency Injection via Konstruktor:
//   Jede konkrete Tool-Klasse bekommt ihre Abhängigkeiten (PathPolicy,
//   GitHelper, ...) im Konstruktor übergeben. Dadurch ist jedes Tool-Objekt
//   ab dem Zeitpunkt der Konstruktion vollständig und sofort benutzbar.
//   Es gibt keinen "halbfertigen" Zustand wie bei Setter-Injection.
//
// Ownership:
//   McpServer besitzt die Tool-Objekte via unique_ptr. Beim Destruktor
//   des Servers werden alle Tools automatisch gelöscht — kein manuelles
//   delete nötig.
//
// Verwendung in main():
//
//   PathPolicy policy;
//   GitHelper  git(policy);
//   McpServer  server("mein-server", "1.0");
//
//   server.registerTool(std::make_unique<ReadFileTool>(&policy, &git));
//   server.registerTool(std::make_unique<WriteFileTool>(&policy, &git));
//
//   server.run();

#include "ToolResult.h"   // ToolResult — eigene Datei, kein Zirkel

#include <QString>
#include <QJsonObject>
#include <QJsonArray>

class ToolBase
{
public:
    // Name des Tools — wird für tools/list und tools/call verwendet.
    // Muss eindeutig innerhalb eines Servers sein.
    virtual QString     name()        const = 0;

    // Beschreibung für das LLM — erklärt wann und wie das Tool zu verwenden ist.
    // Wird in tools/list zurückgegeben und direkt vom Modell gelesen.
    virtual QString     description() const = 0;

    // JSON Schema: Parameter-Definitionen (name → {type, description}).
    // Beispiel: {{"path", {{"type","string"},{"description","..."}}}
    virtual QJsonObject properties()  const = 0;

    // Liste der Pflichtfelder — Subset der properties-Keys.
    // Beispiel: QJsonArray{"path", "content"}
    virtual QJsonArray  required()    const = 0;

    // Tool-Logik. Bekommt die JSON-Argumente aus dem tools/call Request.
    // Gibt ToolResult::ok(...) oder ToolResult::err(...) zurück.
    // Wird vom McpServer aufgerufen — nie direkt.
    virtual ToolResult  execute(const QJsonObject &args) = 0;

    // Virtueller Destruktor — wichtig bei Vererbung mit unique_ptr.
    // Ohne diesen würde beim delete nur der ToolBase-Destruktor aufgerufen,
    // nicht der der konkreten Klasse → Memory Leak / Undefined Behavior.
    virtual ~ToolBase() = default;

    // Konvertierung zu Tool-Struct für McpServer::toSchema().
    // Wird intern von McpServer::registerTool() genutzt.
    // Konkrete Klassen müssen das nicht überschreiben.
    QJsonObject toSchema() const
    {
        return QJsonObject{
            {"name",        name()},
            {"description", description()},
            {"inputSchema", QJsonObject{
                {"type",       "object"},
                {"properties", properties()},
                {"required",   required()}
            }}
        };
    }
};
