#pragma once
#include <QObject>
#include <QString>

// ─── CommandProcessor ─────────────────────────────────────────────────────────
// Verarbeitet Slash-Kommandos aus der Eingabezeile.
//
// Pattern: Command + Chain of Responsibility
//   - isCommand()  prüft ob ein Text ein Kommando ist
//   - process()    verarbeitet das Kommando und gibt einen Prompt zurück
//
// Kommandos:
//   /init [Name]    — Projekt initialisieren
//   /build          — cmake configure + make
//   /compile        — nur make
//   /run            — cmake + make + ausführen
//   /plan <Auftrag> — Plan-Modus: Modell analysiert Projekt + erstellt Plan
//   /summarize      — Konversation zusammenfassen
//   /undo [Datei]   — letzten git-commit rückgängig
//   /diff           — git diff anzeigen
//
// Ergebnis:
//   prompt           → ans LLM schicken (Sondermarker: "__SUMMARIZE__", "__PLAN__:...")
//   notice           → direkt im UI anzeigen
//   handled          → true wenn bekanntes Kommando

class CommandProcessor : public QObject {
    Q_OBJECT

public:
    explicit CommandProcessor(QObject *parent = nullptr);

    struct ProcessResult {
        bool    handled = false;
        QString prompt;           // ans LLM schicken
        QString notice;           // direkt im UI
        QString noticeCssClass;   // "system" oder "error"
    };

    static bool isCommand(const QString &text);
    ProcessResult process(const QString &text, bool agentBusy = false);
    static QString helpText();

private:
    ProcessResult handleInit(const QStringList &args);
    ProcessResult handleBuild(const QStringList &args);
    ProcessResult handleCompile(const QStringList &args);
    ProcessResult handleRun(const QStringList &args);
    ProcessResult handlePlan(const QStringList &args);   // NEU
    ProcessResult handleSummarize(const QStringList &args);
    ProcessResult handleUndo(const QStringList &args);
    ProcessResult handleDiff(const QStringList &args);

    QString m_currentProject;
};
