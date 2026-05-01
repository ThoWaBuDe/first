#pragma once
// ─── ExecuteDebugLogger ───────────────────────────────────────────────────────
// Konfigurierbare Debug-Logging Klasse für Plan/Optimize/Execute-Modus.
//
// Logt jeden Schritt des Agenten:
//   - Welcher Node wird bearbeitet
//   - Welcher Prompt geht rein (Lichtkegel)
//   - Was kommt raus (roher LLM-Output)
//   - Was landet in result / sideOutput nach Parsing
//   - Thoughts vor/nach Update
//
// Konfigurierbar: AppConfig::debugExecute() → bool
//   true  → alles loggen (in Datei + ToolPanel)
//   false → still (kein Overhead)
//
// Log-Format: Markdown-Datei pro Session
//   ~/llamatools/debug/debug_YYYY-MM-DD_HH-MM-SS.md
//
// Analogie AVR: wie ein Logic-Analyzer der man dazustecken und wieder
// abziehen kann — kein Einfluss auf das System wenn abgesteckt.

#include <QString>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDir>
#include <QDebug>
#include "TaskNode.h"

class ExecuteDebugLogger
{
public:
    explicit ExecuteDebugLogger() = default;

    // ── Aktivierung ───────────────────────────────────────────────────────────
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const        { return m_enabled; }

    void setLogDir(const QString &dir) { m_logDir = dir; }

    // Session starten — neue Log-Datei anlegen
    void startSession(const QString &goal)
    {
        if (!m_enabled) return;
        QString dir = m_logDir.isEmpty()
            ? QDir::homePath() + "/llamatools/debug"
            : m_logDir;
        QDir().mkpath(dir);

        QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
        m_logPath  = dir + "/debug_" + ts + ".md";

        m_file.setFileName(m_logPath);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning() << "ExecuteDebugLogger: Kann Log-Datei nicht öffnen:" << m_logPath;
            m_enabled = false;
            return;
        }
        m_stream.setDevice(&m_file);

        m_stream << "# LlamaQt Execute-Debug\n";
        m_stream << "*Session: " << ts << "*\n\n";
        m_stream << "## Ziel\n\n" << goal << "\n\n";
        m_stream << "---\n\n";
        m_stream.flush();
    }

    void endSession()
    {
        if (!m_enabled || !m_file.isOpen()) return;
        m_stream << "\n---\n*Session beendet: "
                 << QDateTime::currentDateTime().toString("HH:mm:ss")
                 << "*\n";
        m_stream.flush();
        m_file.close();
    }

    // ── Log-Methoden ──────────────────────────────────────────────────────────

    // Node-Start: welcher Node wird jetzt bearbeitet
    void logNodeStart(const TaskNode *node)
    {
        if (!m_enabled || !node) return;
        write(QString("## Node [id=%1] %2\n\n"
                      "**Level:** %3  \n"
                      "**Status:** %4  \n"
                      "**Beschreibung:** %5\n\n")
              .arg(node->id)
              .arg(node->title)
              .arg(TaskNode::levelName(node->level))
              .arg(TaskNode::statusName(node->status))
              .arg(node->description.left(200)));
    }

    // Prompt der ans Modell geschickt wird
    void logPromptIn(const QString &systemPrompt, const QString &userPrompt)
    {
        if (!m_enabled) return;
        write("### System-Prompt\n\n```\n" +
              systemPrompt.left(500) +
              (systemPrompt.length() > 500 ? "\n...[gekürzt]" : "") +
              "\n```\n\n");
        write("### User-Prompt (Lichtkegel)\n\n```\n" +
              userPrompt +
              "\n```\n\n");
    }

    // Roher LLM-Output (vor dem Parsing)
    void logRawOutput(const QString &raw)
    {
        if (!m_enabled) return;
        write("### Roher LLM-Output\n\n```\n" +
              raw.left(2000) +
              (raw.length() > 2000 ? "\n...[gekürzt]" : "") +
              "\n```\n\n");
    }

    // Parsing-Ergebnis: was landet wo
    void logParsingResult(const QString &code, const QString &sideOutput)
    {
        if (!m_enabled) return;
        write(QString("### Parsing-Ergebnis\n\n"
                      "**Code:** %1 Zeichen  \n"
                      "**SideOutput:** %2 Zeichen\n\n")
              .arg(code.length()).arg(sideOutput.length()));

        if (!code.isEmpty()) {
            write("**Code (Vorschau):**\n```\n" +
                  code.left(300) +
                  (code.length() > 300 ? "\n..." : "") +
                  "\n```\n\n");
        }
        if (!sideOutput.isEmpty()) {
            write("**SideOutput (Vorschau):**\n```\n" +
                  sideOutput.left(200) +
                  (sideOutput.length() > 200 ? "\n..." : "") +
                  "\n```\n\n");
        }
    }

    // Thoughts-Update
    void logThoughtsUpdate(const QStringList &before, const QStringList &after)
    {
        if (!m_enabled) return;
        write(QString("### Thoughts-Update\n\n"
                      "**Vorher:** %1 Einträge → **Nachher:** %2 Einträge\n\n")
              .arg(before.size()).arg(after.size()));

        // Neue Einträge markieren
        for (const QString &entry : after) {
            bool isNew = !before.contains(entry);
            write(QString("- %1%2\n")
                  .arg(isNew ? "🆕 " : "")
                  .arg(entry));
        }
        write("\n");
    }

    // Tool-Call im Execute/Optimize Modus
    void logToolCall(const QString &toolName, const QString &args,
                     const QString &result)
    {
        if (!m_enabled) return;
        write(QString("### Tool-Call: %1\n\n"
                      "**Args:** `%2`  \n"
                      "**Result:** %3 Zeichen\n\n")
              .arg(toolName, args.left(100))
              .arg(result.length()));
    }

    // Optimierer-Aktion
    void logOptimizerAction(const QString &action, qint64 nodeId,
                             const QString &details)
    {
        if (!m_enabled) return;
        write(QString("### Optimizer: %1 [node=%2]\n\n%3\n\n")
              .arg(action).arg(nodeId).arg(details.left(300)));
    }

    // Freie Nachricht
    void logInfo(const QString &msg)
    {
        if (!m_enabled) return;
        write("> " + msg + "\n\n");
    }

    QString logPath() const { return m_logPath; }

private:
    bool         m_enabled = false;
    QString      m_logDir;
    QString      m_logPath;
    QFile        m_file;
    QTextStream  m_stream;

    void write(const QString &text)
    {
        if (!m_file.isOpen()) return;
        m_stream << text;
        m_stream.flush();
    }
};
