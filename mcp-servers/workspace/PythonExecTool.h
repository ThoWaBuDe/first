#pragma once
// ─── PythonExecTool ───────────────────────────────────────────────────────────
// Führt Python3-Code im Container aus.
// Code wird in eine temporäre Datei geschrieben und via python3 ausgeführt.
// Warum temporäre Datei statt python3 -c?
//   python3 -c hat Probleme mit mehrzeiligem Code und Anführungszeichen.
//   Eine Datei ist robuster und zeigt korrekte Zeilennummern in Tracebacks.

#include "WorkspaceToolBase.h"
#include <QFile>
#include <QDir>
#include <QDateTime>

class PythonExecTool : public WorkspaceToolBase
{
public:
    using WorkspaceToolBase::WorkspaceToolBase;

    QString name() const override { return "python_exec"; }

    QString description() const override
    {
        return
            "Execute Python3 code in the container. "
            "Code is written to a temporary file and run with python3. "
            "stdout and stderr are merged and returned. "
            "Use pip_install to install packages before importing them. "
            "The working directory is ~/workplace/. "
            "Max timeout: 60 seconds.";
    }

    QJsonObject properties() const override
    {
        return {
            {"code",       prop("string",  "Python3 code to execute.")},
            {"timeout_s",  prop("integer", "Timeout in seconds (default: 60, max: 120).")}
        };
    }

    QJsonArray required() const override { return req({"code"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString code   = args.value("code").toString();
        int timeoutS   = qBound(1, args.value("timeout_s").toInt(60), 120);

        if (code.isEmpty())
            return ToolResult::err("Error: 'code' is required.");

        // Temporäre Datei anlegen
        const QString tmpDir  = workplacePath() + "/.tmp";
        QDir().mkpath(tmpDir);
        const QString tmpFile = tmpDir + "/exec_"
            + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".py";

        QFile f(tmpFile);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return ToolResult::err("Error: cannot create temp file.");
        f.write(code.toUtf8());
        f.close();

        // Umgebung: USER_BASE für pip --user
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PYTHONUNBUFFERED", "1");  // direkte Ausgabe, kein Puffern

        auto [output, exitCode] = runWorkspaceCmd(
            "/usr/bin/python3", {tmpFile}, workplacePath(), timeoutS * 1000, env);

        // Temporäre Datei aufräumen
        QFile::remove(tmpFile);

        return exitCode == 0
            ? ToolResult::ok(output)
            : ToolResult::err(output);
    }
};
