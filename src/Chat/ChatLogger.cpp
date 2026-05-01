#include "ChatLogger.h"
#include <QDir>
#include <QStandardPaths>

ChatLogger::ChatLogger(QObject *parent)
    : QObject(parent)
{
    m_logDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
               + "/llamatools/chat_log";
}

ChatLogger::~ChatLogger()
{
    if (m_file.isOpen()) {
        // Session-Ende markieren
        write("\n---\n*Session beendet: "
              + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
              + "*\n");
        m_file.close();
    }
}

void ChatLogger::setEnabled(bool enabled)
{
    m_enabled = enabled;
    if (enabled && !m_file.isOpen())
        startSession();
}

void ChatLogger::setLogDir(const QString &dir)
{
    m_logDir = dir;
}

// ─── startSession ────────────────────────────────────────────────────────────
// Öffnet eine neue Log-Datei mit Timestamp im Namen.
// Format: chat_2025-01-15_14-32-07.md
//
// Jede Session = eigene Datei. Kein Überschreiben alter Logs.
// Beim Öffnen wird ein Header geschrieben.
void ChatLogger::startSession()
{
    if (m_file.isOpen())
        m_file.close();

    QDir().mkpath(m_logDir);

    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    m_currentPath = m_logDir + "/chat_" + timestamp + ".md";

    m_file.setFileName(m_currentPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_enabled = false;
        return;
    }

    // QTextStream nach neuem open() neu setzen
    // (alter Stream ist an die alte Datei gebunden)
    m_stream.setDevice(&m_file);
    m_stream.setEncoding(QStringConverter::Utf8);

    write("# LlamaQt Chat\n");
    write("*Session gestartet: "
          + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
          + "*\n\n---\n\n");
}

// ─── write ───────────────────────────────────────────────────────────────────
// Schreibt einen Markdown-Block und flusht sofort.
// Flush nach jedem Block: bei Absturz gehen keine Einträge verloren.
// Analogie AVR: wie UART_flush() nach jeder wichtigen Ausgabe.
void ChatLogger::write(const QString &markdown)
{
    if (!m_file.isOpen()) return;
    m_stream << markdown;
    m_stream.flush();
}

void ChatLogger::ensureOpen()
{
    if (m_enabled && !m_file.isOpen())
        startSession();
}

// ─── Log-Methoden ────────────────────────────────────────────────────────────

void ChatLogger::logUser(const QString &text)
{
    if (!m_enabled) return;
    ensureOpen();
    write(QString("## 👤 User\n%1\n\n").arg(text));
}

void ChatLogger::logAssistant(const QString &text)
{
    if (!m_enabled) return;
    ensureOpen();
    // Thinking-Blöcke (<think>...</think>) im Log behalten aber markieren
    QString logged = text;
    logged.replace("<think>",  "\n> 💭 *Thinking:*\n> ");
    logged.replace("</think>", "\n");
    write(QString("## 🤖 Assistent\n%1\n\n").arg(logged));
}

void ChatLogger::logToolCall(const QString &toolName, const QString &argsJson)
{
    if (!m_enabled) return;
    ensureOpen();
    write(QString("### 🔧 Tool-Call: `%1`\n```json\n%2\n```\n\n")
          .arg(toolName, argsJson));
}

void ChatLogger::logToolResult(const QString &toolName,
                                const QString &result, bool isError)
{
    if (!m_enabled) return;
    ensureOpen();
    QString icon = isError ? "❌" : "✅";
    write(QString("### %1 Ergebnis: `%2`\n```\n%3\n```\n\n")
          .arg(icon, toolName, result));
}

void ChatLogger::logSystem(const QString &text)
{
    if (!m_enabled) return;
    ensureOpen();
    write(QString("> ℹ️ *System: %1*\n\n").arg(text));
}

void ChatLogger::logThinking(const QString &text)
{
    if (!m_enabled) return;
    ensureOpen();
    // Thinking kompakt — nur die ersten 200 Zeichen, Rest weglassen
    QString shortened = text.trimmed();
    if (shortened.length() > 200)
        shortened = shortened.left(200) + "...";
    write(QString("<details><summary>💭 Thinking</summary>\n\n%1\n\n</details>\n\n")
          .arg(shortened));
}
