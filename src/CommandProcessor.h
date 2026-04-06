#pragma once
#include <QObject>
#include <QString>

// ─── CommandProcessor ─────────────────────────────────────────────────────────
// Verarbeitet Slash-Kommandos aus der Eingabezeile.
//
// Pattern: Command + Chain of Responsibility
//   - isCommand()  prüft ob ein Text ein Kommando ist
//   - process()    verarbeitet das Kommando und gibt einen Prompt zurück
//     der an das Modell geschickt wird (leer = kein LLM nötig)
//
// Designprinzip: Der CommandProcessor erledigt so wenig wie möglich selbst.
// Er baut einen präzisen deutschen Prompt und lässt das LLM die eigentliche
// Arbeit machen (Verzeichnis anlegen, AGENT.md schreiben, etc.) via Tools.
//
// Kommandos:
//   /init [Name]  — Projekt initialisieren (Verzeichnis + AGENT.md)
//   /build        — cmake configure + make
//   /compile      — nur make (kein Re-Configure)
//   /run          — cmake (falls nötig) + make (falls nötig) + ausführen
//
// Ergebnis von process():
//   - ProcessResult::prompt  → ans LLM schicken (kann leer sein)
//   - ProcessResult::notice  → direkt im UI anzeigen (Info/Fehler)
//   - ProcessResult::handled → true wenn es ein bekanntes Kommando war

class CommandProcessor : public QObject {
    Q_OBJECT

public:
    explicit CommandProcessor(QObject *parent = nullptr);

    struct ProcessResult {
        bool    handled = false;  // war es ein Kommando?
        QString prompt;           // ans LLM schicken (leer = nichts senden)
        QString notice;           // direkt im UI anzeigen
        QString noticeCssClass;   // CSS-Klasse für notice ("system", "error")
    };

    // Prüft ob text mit '/' beginnt
    static bool isCommand(const QString &text);

    // Verarbeitet das Kommando und gibt das Ergebnis zurück.
    // agentBusy: true wenn gerade eine Generierung läuft
    ProcessResult process(const QString &text, bool agentBusy = false);

    // Hilfstext für unbekannte Kommandos
    static QString helpText();

private:
    ProcessResult handleInit(const QStringList &args);
    ProcessResult handleBuild(const QStringList &args);
    ProcessResult handleCompile(const QStringList &args);
    ProcessResult handleRun(const QStringList &args);
    ProcessResult handleSummarize(const QStringList &args);
    ProcessResult handleUndo(const QStringList &args);
    ProcessResult handleDiff(const QStringList &args);

    // Aktuelles Projekt (gesetzt durch /init)
    QString m_currentProject;
};
