#pragma once
// ─── CompileToolBase ─────────────────────────────────────────────────────────
// Zwischenschicht zwischen ToolBase und den konkreten compile Tools.
//
// Enthält:
//   1. Gemeinsame Hilfsfunktionen: runProcess(), sandboxRoot(), isPathAllowed()
//   2. Basisklasse CompileToolBase mit PathPolicy

#include "../common/ToolBase.h"
#include "../common/PathPolicy.h"

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QPair>
#include <QElapsedTimer>

static QPair<QString,bool> runProcess(const QString &cmd,
                                       const QStringList &args,
                                       const QString &workDir,
                                       int timeoutMs = 120000)
{
    QProcess proc;
    proc.setWorkingDirectory(workDir);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(cmd, args);

    if (!proc.waitForStarted(5000))
        return {QString("Error: could not start '%1'.").arg(cmd), true};
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {QString("Error: timeout after %1s.\n%2")
                .arg(timeoutMs/1000)
                .arg(QString::fromUtf8(proc.readAll())), true};
    }

    QString output = QString::fromUtf8(proc.readAll());
    bool failed = (proc.exitCode() != 0);
    return {output.isEmpty() ? "(no output)" : output, failed};
}

// ─── CompileToolBase ─────────────────────────────────────────────────────────
class CompileToolBase : public ToolBase
{
public:
    explicit CompileToolBase(PathPolicy *policy)
        : m_policy(policy)
    {}

protected:
    PathPolicy *m_policy;

    static QJsonObject prop(const QString &type, const QString &desc)
    {
        return QJsonObject{{"type", type}, {"description", desc}};
    }

    static QJsonArray req(std::initializer_list<const char*> fields)
    {
        QJsonArray arr;
        for (const char *f : fields) arr.append(f);
        return arr;
    }

    QString sandboxRoot() const
    {
        if (m_policy && !m_policy->primarySandbox().isEmpty())
            return m_policy->primarySandbox();
        return QDir::homePath() + "/llamatools";
    }
};