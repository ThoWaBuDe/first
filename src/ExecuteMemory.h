#pragma once
// ─── ExecuteMemory ────────────────────────────────────────────────────────────
// Kurzzeitgedächtnis des Execute-Agenten ("Thoughts").
//
// Neu in v2:
//   parseFromLlmOutput() — robustes Parsing:
//     - <think>...</think> Blöcke entfernen
//     - Nummerierung entfernen ("1. ", "2. ", ...)
//     - Leere Zeilen überspringen
//     - Zeilen unter 4 Zeichen überspringen (Artefakte)
//     - Harte Obergrenze durchsetzen

#include <QString>
#include <QStringList>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QRegularExpression>
#include <QDebug>

class ExecuteMemory
{
public:
    explicit ExecuteMemory(int maxEntries = 50)
        : m_maxEntries(maxEntries)
    {}

    // ── Einträge direkt setzen (nach robustem Parse) ──────────────────────────
    void setEntries(const QStringList &entries)
    {
        m_entries = entries;
        while (m_entries.size() > m_maxEntries)
            m_entries.removeFirst();
        m_dirty = true;
    }

    // ── LLM-Output parsen → saubere Thoughts-Liste ───────────────────────────
    // Das ist der zentrale Fix gegenüber v1.
    //
    // Probleme in v1:
    //   - <think>...</think> Blöcke landeten als Einträge in der Liste
    //   - Nummerierungen wie "1.", "2." wurden mitgespeichert
    //   - Das Modell schrieb seinen Denkprozess als Eintrag
    //   - Obergrenze griff nicht weil Parsing falsch war
    //
    // Lösung hier:
    //   1. Thinking-Blöcke komplett entfernen
    //   2. Pro Zeile: Nummerierung/Prefix entfernen
    //   3. Leere + zu kurze Zeilen verwerfen
    //   4. Harte Obergrenze danach anwenden
    //
    // Analogie AVR: wie ein UART-Empfangspuffer der nur gültige Pakete
    // durchlässt — Framing-Fehler werden verworfen, kein Buffer-Overflow.
    void parseFromLlmOutput(const QString &llmResponse)
    {
        QString text = llmResponse;

        // Schritt 1: <think>...</think> Blöcke entfernen
        // Das Modell schreibt seinen Denkprozess in diese Blöcke —
        // wir wollen nur das Ergebnis, nicht den Prozess.
        static const QRegularExpression thinkRe(
            "<think>.*?</think>",
            QRegularExpression::DotMatchesEverythingOption);
        text.remove(thinkRe);

        // Schritt 2: Markdown-Fences entfernen (```...```)
        static const QRegularExpression fenceRe(
            "```[a-zA-Z]*\\n?");
        text.remove(fenceRe);
        text.remove("```");

        // Schritt 3: Zeilen parsen
        QStringList result;
        static const QRegularExpression numberPrefixRe("^\\d+[.)\\s]+");

        for (const QString &rawLine : text.split('\n')) {
            QString line = rawLine.trimmed();

            // Leere Zeilen verwerfen
            if (line.isEmpty()) continue;

            // Zu kurze Zeilen verwerfen (Artefakte wie "1.", "-", "*")
            if (line.length() < 4) continue;

            // Nummerierung entfernen: "1. ", "1) ", "42. "
            line.remove(numberPrefixRe);
            line = line.trimmed();

            // Markdown-Bullets entfernen: "- ", "* ", "• "
            if (line.startsWith("- ") || line.startsWith("* ") || line.startsWith("• "))
                line = line.mid(2).trimmed();

            // Nochmal Längencheck nach Bereinigung
            if (line.length() < 4) continue;

            // Zeilen die nach Denkprozess aussehen verwerfen
            // (Modell schreibt manchmal "I need to...", "Let me...")
            if (line.startsWith("I ") || line.startsWith("Let ") ||
                line.startsWith("Wait") || line.startsWith("Actually"))
                continue;

            result << line;
        }

        // Schritt 4: Harte Obergrenze
        while (result.size() > m_maxEntries)
            result.removeFirst();

        m_entries = result;
        m_dirty   = true;
    }

    const QStringList &entries() const { return m_entries; }
    int count() const      { return m_entries.size(); }
    int maxEntries() const { return m_maxEntries; }
    void setMaxEntries(int n) { m_maxEntries = n; }
    bool isEmpty() const   { return m_entries.isEmpty(); }

    // Formatiert Thoughts als nummerierten String für den Prompt
    QString toPromptString() const
    {
        if (m_entries.isEmpty()) return "(noch keine Thoughts)";
        QString result;
        for (int i = 0; i < m_entries.size(); ++i)
            result += QString("%1. %2\n").arg(i + 1).arg(m_entries[i]);
        return result.trimmed();
    }

    // ── Persistenz ────────────────────────────────────────────────────────────
    void setDbPath(const QString &path)
    {
        if (m_db.isOpen()) m_db.close();
        m_dbPath = path;
    }

    bool save()
    {
        if (!openDb()) return false;
        QSqlQuery q(m_db);
        m_db.transaction();
        q.exec("DELETE FROM execute_memory");
        q.prepare("INSERT INTO execute_memory (position, entry) VALUES (:pos, :entry)");
        for (int i = 0; i < m_entries.size(); ++i) {
            q.bindValue(":pos",   i);
            q.bindValue(":entry", m_entries[i]);
            if (!q.exec())
                qWarning() << "ExecuteMemory::save:" << q.lastError().text();
        }
        m_db.commit();
        m_dirty = false;
        return true;
    }

    bool load()
    {
        if (!openDb()) return false;
        m_entries.clear();
        QSqlQuery q("SELECT entry FROM execute_memory ORDER BY position", m_db);
        while (q.next())
            m_entries << q.value(0).toString();
        m_dirty = false;
        return true;
    }

    bool isDirty() const { return m_dirty; }

private:
    QString      m_dbPath;
    QSqlDatabase m_db;
    QStringList  m_entries;
    int          m_maxEntries;
    bool         m_dirty = false;

    bool openDb()
    {
        if (m_db.isOpen()) return true;
        if (m_dbPath.isEmpty()) {
            qWarning() << "ExecuteMemory: kein DB-Pfad";
            return false;
        }
        const QString connName = "exmem_" + m_dbPath;
        if (QSqlDatabase::contains(connName)) {
            m_db = QSqlDatabase::database(connName);
        } else {
            m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
            m_db.setDatabaseName(m_dbPath);
        }
        if (!m_db.open()) {
            qWarning() << "ExecuteMemory:" << m_db.lastError().text();
            return false;
        }
        QSqlQuery q(m_db);
        q.exec(
            "CREATE TABLE IF NOT EXISTS execute_memory ("
            "  position INTEGER PRIMARY KEY,"
            "  entry    TEXT NOT NULL"
            ")"
        );
        return true;
    }
};
