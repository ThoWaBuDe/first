#pragma once
#include <QObject>
#include <QString>

// ─── CommandProcessor ─────────────────────────────────────────────────────────
// Verarbeitet Slash-Kommandos aus der Eingabezeile.
//
// Neue Kommandos:
//   /saveDB   — TaskTree + ExecuteMemory in SQLite speichern
//   /loadDB   — TaskTree + ExecuteMemory aus SQLite laden
//   /execute  — Execute-Modus starten (ab nächstem Pending-Node)

class CommandProcessor : public QObject {
    Q_OBJECT

public:
    explicit CommandProcessor(QObject *parent = nullptr);

    struct ProcessResult {
        bool    handled = false;
        QString prompt;
        QString notice;
        QString noticeCssClass;
    };

    static bool isCommand(const QString &text);
    ProcessResult process(const QString &text, bool agentBusy = false);
    static QString helpText();

private:
    ProcessResult handleInit(const QStringList &args);
    ProcessResult handleBuild(const QStringList &args);
    ProcessResult handleCompile(const QStringList &args);
    ProcessResult handleRun(const QStringList &args);
    ProcessResult handlePlan(const QStringList &args);
    ProcessResult handleExecute(const QStringList &args);   // NEU
    ProcessResult handleSaveDB(const QStringList &args);    // NEU
    ProcessResult handleLoadDB(const QStringList &args);    // NEU
    ProcessResult handleSummarize(const QStringList &args);
    ProcessResult handleUndo(const QStringList &args);
    ProcessResult handleDiff(const QStringList &args);
    ProcessResult handleCodeAssemble(const QStringList &args);

    QString m_currentProject;
};
