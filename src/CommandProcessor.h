#pragma once
#include <QObject>
#include <QString>

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
    ProcessResult handleExecute(const QStringList &args);
    ProcessResult handleSaveDB(const QStringList &args);
    ProcessResult handleLoadDB(const QStringList &args);
    ProcessResult handleSummarize(const QStringList &args);
    ProcessResult handleUndo(const QStringList &args);
    ProcessResult handleDiff(const QStringList &args);
    ProcessResult handleImport(const QStringList &args); // NEU

    QString m_currentProject;
};
