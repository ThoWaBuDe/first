#pragma once
// ─── ToolResult ───────────────────────────────────────────────────────────────
// Rückgabetyp für alle Tool-Implementierungen.
//
// In eine eigene Datei ausgelagert damit:
//   - ToolBase.h  kann ToolResult.h inkludieren (kein McpServer.h nötig)
//   - McpServer.h kann ToolBase.h inkludieren   (kein Zirkel mehr)
//
// Vorher: McpServer.h <── ToolBase.h <── McpServer.h  (Zirkel!)
// Jetzt:  ToolResult.h <── ToolBase.h <── McpServer.h (azyklisch)

#include <QString>

struct ToolResult {
    QString text;
    bool    isError = false;

    static ToolResult ok(const QString &text)  { return {text, false}; }
    static ToolResult err(const QString &text) { return {text, true};  }
};
