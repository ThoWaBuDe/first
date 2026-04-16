#pragma once
// ─── ExecuteMemory ────────────────────────────────────────────────────────────
// Kurzzeitgedächtnis des Execute-Agenten ("Thoughts").
//
// v3 Fixes:
//   parseFromLlmOutput() — zusätzlich:
//     - Duplikate und fast-identische Zeilen entfernen
//     - Zeilen die mit "nicht" enden verwerfen (sinnlose Negationen)
//     - Zeilen die keine Aussage enthalten verwerfen
//   updateThoughtsPrompt() — verbessert: explizite Verbote

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

    void setEntries(const QStringList &entries)
    {
        m_entries = entries;
        while (m_entries.size() > m_maxEntries)
            m_entries.removeFirst();
        m_dirty = true;
    }

    // ── LLM-Output parsen → saubere Thoughts-Liste ───────────────────────────
    // v3: Duplikat-Erkennung + Negations-Filter + Mindestqualität
    void parseFromLlmOutput(const QString &llmResponse)
    {
        QString text = llmResponse;

        // Schritt 1: <think>...</think> entfernen
        static const QRegularExpression thinkRe(
            "<think>.*?</think>",
            QRegularExpression::DotMatchesEverythingOption);
        text.remove(thinkRe);

        // Schritt 2: Markdown-Fences entfernen
        static const QRegularExpression fenceRe("```[a-zA-Z]*\\n?");
        text.remove(fenceRe);
        text.remove("```");

        // Schritt 3: Zeilen parsen
        QStringList result;
        QStringList resultLower;  // für Duplikat-Check (case-insensitive)
        static const QRegularExpression numberPrefixRe("^\\d+[.)\\s]+");

        for (const QString &rawLine : text.split('\n')) {
            QString line = rawLine.trimmed();

            if (line.isEmpty()) continue;
            if (line.length() < 4) continue;

            // Nummerierung entfernen
            line.remove(numberPrefixRe);
            line = line.trimmed();

            // Markdown-Bullets entfernen
            if (line.startsWith("- ") || line.startsWith("* ") ||
                line.startsWith("• ") || line.startsWith("+ "))
                line = line.mid(2).trimmed();

            if (line.length() < 4) continue;

            // Englische Denkprozess-Artefakte verwerfen
            if (line.startsWith("I ") || line.startsWith("Let ") ||
                line.startsWith("Wait") || line.startsWith("Actually") ||
                line.startsWith("So ") || line.startsWith("Now "))
                continue;

            // ── NEU: Sinnlose Negations-Zeilen verwerfen ──────────────────
            // "Timer-Objekt ist nicht konfigurierbar" → wertlos
            // "Signal-Verbindung ist nicht optional" → wertlos
            // Erkennungsmuster: Zeile enthält "nicht " und endet mit einem Adjektiv
            static const QRegularExpression negationRe(
                "\\b(nicht|kein|keine)\\b.*\\b(bar|lich|ig|los)$",
                QRegularExpression::CaseInsensitiveOption);
            if (negationRe.match(line).hasMatch()) continue;

            // ── NEU: Duplikat-Check (exakt + fast-identisch) ──────────────
            // Exakter Duplikat: überspringen
            QString lineLower = line.toLower();
            if (resultLower.contains(lineLower)) continue;

            // Fast-identisch: wenn ein bestehender Eintrag zu >80% ähnlich
            // ist, überspringen. Einfache Heuristik: erste 20 Zeichen gleich.
            // Das fängt Serien wie "Signal-Verbindung ist nicht X" ab.
            bool nearDuplicate = false;
            QString linePrefix = lineLower.left(20);
            for (const QString &existing : resultLower) {
                if (existing.left(20) == linePrefix) {
                    nearDuplicate = true;
                    break;
                }
            }
            if (nearDuplicate) continue;

            result << line;
            resultLower << lineLower;
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

    QString toPromptString() const
    {
        if (m_entries.isEmpty()) return "(noch keine Thoughts)";
        QString result;
        for (int i = 0; i < m_entries.size(); ++i)
            result += QString("%1. %2\n").arg(i + 1).arg(m_entries[i]);
        return result.trimmed();
    }

    // ── Verbesserter Summarize-Prompt ─────────────────────────────────────────
    // Gibt den Prompt-Text zurück den Agent::updateThoughts() verwenden soll.
    // Enthält explizite Verbote gegen Halluzinations-Muster.
    static QString buildSummarizePrompt(const QString &nodeTitle,
                                         const QString &nodeDescription,
                                         const QString &currentThoughts,
                                         int            maxEntries)
    {
        return QString(
            "Du hast gerade implementiert:\n"
            "Titel: %1\n"
            "Beschreibung: %2\n"
            "\n"
            "Bisherige Thoughts:\n"
            "%3\n"
            "\n"
            "Aktualisiere die Thoughts-Liste mit echten Erkenntnissen.\n"
            "\n"
            "REGELN:\n"
            "- Maximal %4 Eintraege\n"
            "- Eine Erkenntnis pro Zeile\n"
            "- KEINE Nummerierung\n"
            "- KEIN Markdown\n"
            "- Nur die Liste, kein anderer Text\n"
            "\n"
            "VERBOTEN (diese Muster erzeugen wertlose Listen):\n"
            "- Zeilen die mit 'nicht' enden (z.B. 'X ist nicht Y-bar')\n"
            "- Identische oder fast identische Zeilen\n"
            "- Zeilen ohne konkreten Informationsgehalt\n"
            "- Aufzaehlungen von API-Methoden ohne Kontext\n"
            "\n"
            "GUTE Erkenntnisse enthalten:\n"
            "- Warum etwas so gemacht wurde\n"
            "- Was bei diesem Projekt spezifisch wichtig ist\n"
            "- Konkrete Fehler die vermieden werden sollen\n"
            "- Abhaengigkeiten zwischen Klassen"
        ).arg(nodeTitle, nodeDescription, currentThoughts,
              QString::number(maxEntries));
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
        if (m_dbPath.isEmpty()) { qWarning() << "ExecuteMemory: kein DB-Pfad"; return false; }
        const QString connName = "exmem_" + m_dbPath;
        if (QSqlDatabase::contains(connName)) {
            m_db = QSqlDatabase::database(connName);
        } else {
            m_db = QSqlDatabase::addDatabase("QSQLITE", connName);
            m_db.setDatabaseName(m_dbPath);
        }
        if (!m_db.open()) { qWarning() << "ExecuteMemory:" << m_db.lastError().text(); return false; }
        QSqlQuery q(m_db);
        q.exec("CREATE TABLE IF NOT EXISTS execute_memory (position INTEGER PRIMARY KEY, entry TEXT NOT NULL)");
        return true;
    }
};
