#pragma once
// ─── BashExecTool ─────────────────────────────────────────────────────────────
// Führt beliebige Shell-Kommandos im Container aus.
// Läuft als User 'llamaqt' — sudo ist nur für apt erlaubt.
//
// Sicherheit:
//   - Blacklist für offensichtlich destruktive Kommandos
//   - Timeout (Standard 30s, max 300s)
//   - Arbeitsverzeichnis ist ~/workplace/ (konfigurierbar)
//   - Der Container selbst ist die eigentliche Sicherheitsgrenze

#include "WorkspaceToolBase.h"

class BashExecTool : public WorkspaceToolBase
{
public:
    using WorkspaceToolBase::WorkspaceToolBase;

    QString name() const override { return "bash_exec"; }

    QString description() const override
    {
        return
            "Execute a bash command in the container. "
            "Runs as user 'llamaqt' in ~/workplace/ by default. "
            "Use sudo for apt commands (allowed via sudoers). "
            "Use pip install --user for Python packages (no sudo needed). "
            "stdout and stderr are merged and returned. "
            "Max timeout: 300 seconds. "
            "Avoid destructive commands like 'rm -rf /' — they are blocked.";
    }

    QJsonObject properties() const override
    {
        return {
            {"command",    prop("string",  "Bash command to execute.")},
            {"workdir",    prop("string",  "Working directory (default: ~/workplace/).")},
            {"timeout_s",  prop("integer", "Timeout in seconds (default: 30, max: 300).")}
        };
    }

    QJsonArray required() const override { return req({"command"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString command = args.value("command").toString().trimmed();
        QString workdir = args.value("workdir").toString();
        int timeoutS    = qBound(1, args.value("timeout_s").toInt(30), 300);

        if (command.isEmpty())
            return ToolResult::err("Error: 'command' is required.");

        if (isBlacklisted(command))
            return ToolResult::err(
                QString("Error: command blocked by safety filter: %1")
                .arg(command.left(80)));

        if (workdir.isEmpty())
            workdir = workplacePath();

        // ~ expandieren — QProcess kennt kein ~
        if (workdir.startsWith("~/"))
            workdir = QDir::homePath() + workdir.mid(1);
        else if (workdir == "~")
            workdir = QDir::homePath();

        // Sicherstellen dass das Arbeitsverzeichnis existiert
        QDir().mkpath(workdir);

        // /bin/bash absolut — PATH ist in runuser-Umgebung möglicherweise minimal
        auto [output, exitCode] = runWorkspaceCmd(
            "/bin/bash", {"-c", command}, workdir, timeoutS * 1000);

        QString result = QString("$ %1\n%2").arg(command, output);
        if (exitCode != 0)
            result += QString("\n[Exit-Code: %1]").arg(exitCode);

        return exitCode == 0
            ? ToolResult::ok(result)
            : ToolResult::err(result);
    }
};
