#pragma once
// ─── GetProjectIndexTool ──────────────────────────────────────────────────────
// Gibt den gecachten Projekt-Index zurück (oder generiert ihn neu).
//
// Das ist das "on-demand" Gegenstück zum statischen System-Prompt-Index.
// Das Modell ruft dieses Tool wenn es einen detaillierteren Überblick braucht
// als der statische Index bietet.
//
// Cache-Logik: make-Prinzip — nur neu generieren wenn Sandbox-Dateien
// neuer sind als der Cache. Sonst: Cache direkt zurückgeben (schnell).

#include "TreeSitterToolBase.h"
#include "ProjectIndexer.h"
#include "../common/McpConfig.h"

class GetProjectIndexTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "get_project_index"; }

    QString description() const override
    {
        return
            "Get a structured Markdown index of the project: "
            "directory tree, all classes with fields and methods, "
            "and free functions — for both the sandbox and LlamaQt sources. "
            "Use this to orient yourself at the start of a session or when "
            "you need to find where a class or function is defined. "
            "The index is cached and rebuilt automatically when sandbox files change. "
            "For the source code of a specific symbol, use get_symbol instead.";
    }

    QJsonObject properties() const override { return {}; }
    QJsonArray  required()   const override { return req({}); }

    ToolResult execute(const QJsonObject &) override
    {
        McpConfig cfg;
        ProjectIndexer indexer(cfg.sandboxRoot(), cfg.sourceRoot(), cfg.cachePath());
        QString index = indexer.get();
        return index.isEmpty()
            ? ToolResult::err("Error: Could not generate project index.")
            : ToolResult::ok(index);
    }
};


// ─── RebuildIndexTool ─────────────────────────────────────────────────────────
// Löscht den Cache und generiert den Projekt-Index neu.
//
// Wann braucht man das?
//   - Neue Dateien wurden zur Sandbox hinzugefügt (mkdir + write_file)
//   - Die Verzeichnisstruktur hat sich geändert
//   - Man will sicherstellen dass der Index aktuell ist
//
// Normalerweise passiert das automatisch (Timestamp-Check), aber manchmal
// will man explizit einen frischen Index — z.B. nach einem großen Refactoring.

class RebuildIndexTool : public TreeSitterToolBase
{
public:
    using TreeSitterToolBase::TreeSitterToolBase;

    QString name() const override { return "rebuild_index"; }

    QString description() const override
    {
        return
            "Force a rebuild of the project index cache. "
            "Use this after adding new files or restructuring the sandbox. "
            "Normally the index is rebuilt automatically when files change. "
            "Returns the freshly generated index.";
    }

    QJsonObject properties() const override { return {}; }
    QJsonArray  required()   const override { return req({}); }

    ToolResult execute(const QJsonObject &) override
    {
        McpConfig cfg;
        ProjectIndexer indexer(cfg.sandboxRoot(), cfg.sourceRoot(), cfg.cachePath());
        QString index = indexer.rebuild();
        return index.isEmpty()
            ? ToolResult::err("Error: Could not rebuild project index.")
            : ToolResult::ok("Index rebuilt.\n\n" + index);
    }
};
