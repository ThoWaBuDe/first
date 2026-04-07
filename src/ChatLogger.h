#pragma once
#include <QObject>
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QDateTime>

// ─── ChatLogger ───────────────────────────────────────────────────────────────
// Schreibt die Konversation in eine Markdown-Datei mit.
//
// Pattern: Observer — wird von Agent über Ereignisse informiert und
//          schreibt sie in eine Datei. Keine Rückkopplung zum Agent.
//
// Dateiformat: Markdown
//   # LlamaQt Chat — 2025-01-15 14:32
//   ## User
//   Hallo
//   ## Assistent
//   Hallo! ...
//   ### Tool-Call: read_file
//   ```json
//   {"path": "foo.cpp"}
//   ```
//   ### Tool-Ergebnis
//   ```
//   int main() { ...
//   ```
//
// Warum Markdown statt HTML?
//   - Direkt lesbar in jedem Texteditor
//   - Von GitHub, VSCode, Obsidian gerendert
//   - Keine Parser-Abhängigkeit
//   - Einfach zu schreiben: kein Tag-Matching nötig
//
// Warum kein QSettings für den Pfad?
//   ChatLogger ist bewusst einfach gehalten. Der Pfad kommt von AppConfig
//   (wenn vorhanden) oder fällt auf ~/llamatools/chat_log/ zurück.

class ChatLogger : public QObject {
    Q_OBJECT

public:
    explicit ChatLogger(QObject *parent = nullptr);
    ~ChatLogger() override;

    // Logging aktivieren/deaktivieren (default: aus)
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Log-Verzeichnis setzen (default: ~/llamatools/chat_log/)
    void setLogDir(const QString &dir);

    // Neue Session starten (neue Datei, Timestamp im Namen)
    void startSession();

    // Einträge schreiben
    void logUser(const QString &text);
    void logAssistant(const QString &text);
    void logToolCall(const QString &toolName, const QString &argsJson);
    void logToolResult(const QString &toolName, const QString &result, bool isError);
    void logSystem(const QString &text);
    void logThinking(const QString &text);

    // Aktuellen Log-Dateipfad (für UI-Anzeige)
    QString currentLogPath() const { return m_currentPath; }

private:
    void write(const QString &markdown);
    void ensureOpen();

    bool        m_enabled     = false;
    QString     m_logDir;
    QString     m_currentPath;
    QFile       m_file;
    QTextStream m_stream;
};
