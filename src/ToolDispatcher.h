#pragma once
#include <QObject>
#include <QString>
#include <QJsonObject>

// ─── ToolDispatcher ───────────────────────────────────────────────────────────
// Zentrale Stelle für alle Tool-Calls des Modells.
//
// Pattern: Command Dispatcher — eingehende Tool-Namen werden auf konkrete
//          Handler-Methoden abgebildet.
//
// Tool-Call Format (Qwen3 / ChatML):
//   <tool_call>
//   {"name": "TOOLNAME", "arguments": {...}}
//   </tool_call>
//
// ─── Verfügbare Tools ────────────────────────────────────────────────────────
//
// FILESYSTEM (Sandbox: ~/llamatools/)
//   read_file    path, [start_line], [end_line]  → Dateiinhalt (optional Range)
//   write_file   path, content                   → Datei schreiben (neu/überschreiben)
//   str_replace  path, old_str, new_str          → Text in Datei ersetzen
//   list_dir     [path]                          → Verzeichnis auflisten
//   list_symbols path                            → C++ Symbole (Klassen/Methoden)
//
// SYSTEM (read-only, keine Sandbox)
//   get_time     –                               → aktuelle Uhrzeit + Datum
//   get_pwd      –                               → Sandbox-Wurzelverzeichnis
//   disk_free    [path]                          → freier Speicher (df-Stil)
//   sys_info     –                               → CPU-Kerne, RAM, Hostname

class ToolDispatcher : public QObject {
    Q_OBJECT

public:
    explicit ToolDispatcher(QObject *parent = nullptr);

    // Prüft ob text einen vollständigen Tool-Call enthält
    bool containsToolCall(const QString &text) const;

    // Extrahiert den JSON-Block aus <tool_call>...</tool_call>
    QString extractToolCallBlock(const QString &text) const;

    // Parsed JSON, dispatcht zum Handler, gibt Ergebnis zurück.
    // toolName wird als Output-Parameter gesetzt.
    QString dispatch(const QString &jsonBlock, QString &toolName);

    // System-Prompt Beschreibung aller Tools (wird in ChatModel injiziert)
    static QString toolsDescription();

private:
    // ─── Filesystem Tools ─────────────────────────────────────────────────
    QString handleReadFile(const QJsonObject &args);
    QString handleWriteFile(const QJsonObject &args);
    QString handleStrReplace(const QJsonObject &args);
    QString handleListDir(const QJsonObject &args);
    QString handleListSymbols(const QJsonObject &args);

    // ─── System Tools ─────────────────────────────────────────────────────
    QString handleGetTime(const QJsonObject &args);
    QString handleGetPwd(const QJsonObject &args);
    QString handleDiskFree(const QJsonObject &args);
    QString handleSysInfo(const QJsonObject &args);

    // Sicherheit: nur Pfade innerhalb m_sandboxRoot erlaubt
    bool    isPathAllowed(const QString &path) const;

    // Relativen Pfad in absoluten Sandbox-Pfad umwandeln
    QString resolvePath(const QString &relPath) const;

    QString m_sandboxRoot;
};
