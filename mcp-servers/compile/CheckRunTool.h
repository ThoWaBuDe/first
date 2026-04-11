#pragma once
// ─── CheckRunTool ─────────────────────────────────────────────────────────────

#include "CompileToolBase.h"

class CheckRunTool : public CompileToolBase
{
public:
    using CompileToolBase::CompileToolBase;

    QString name() const override { return "check_run"; }

    QString description() const override
    {
        return "Verify a sandbox binary and optionally run it. "
               "Path relative to sandbox root (e.g. 'MyProject/build/MyApp'). "
               "With danger_zone:true: runs binary, reports exit code, runtime, stdout, stderr.";
    }

    QJsonObject properties() const override
    {
        return {
            {"binary",      prop("string",  "Binary path relative to sandbox root")},
            {"danger_zone", prop("boolean", "true = run the binary (default: false)")},
            {"timeout_ms",  prop("integer", "Max run time in ms (default: 5000)")}
        };
    }

    QJsonArray required() const override { return {"binary"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString binary    = args.value("binary").toString();
        bool dangerZone   = args.value("danger_zone").toBool(false);
        int timeoutMs     = args.value("timeout_ms").toInt(5000);

        if (binary.isEmpty())
            return ToolResult::err("Error: 'binary' is required.");

        QString fullPath = binary.startsWith('/') ? binary : sandboxRoot() + "/" + binary;

        QFileInfo fi(fullPath);
        if (!fi.exists())
            return ToolResult::err(QString("Error: not found: %1\n(resolved: %2)")
                .arg(binary).arg(fullPath));
        if (!fi.isExecutable())
            return ToolResult::err(QString("Error: not executable: %1").arg(fullPath));

        QString info = QString("Binary:   %1\nSize:     %2 bytes\n")
                       .arg(fullPath).arg(fi.size());

        if (!dangerZone) {
            info += "Status:   present and executable\n(set danger_zone:true to run it)";
            return ToolResult::ok(info);
        }

        QProcess proc;
        proc.setProgram(fullPath);
        proc.setProcessChannelMode(QProcess::SeparateChannels);

        QElapsedTimer timer;
        timer.start();
        proc.start();

        if (!proc.waitForStarted(3000))
            return ToolResult::err(info + "Error: could not start.");

        bool exitedInTime = proc.waitForFinished(timeoutMs);
        qint64 elapsed    = timer.elapsed();

        auto truncate = [](const QString &s, int max) {
            return s.length() <= max ? s : s.left(max) + "\n[... truncated]";
        };
        QString stdoutStr = truncate(QString::fromUtf8(proc.readAllStandardOutput()), 2000);
        QString stderrStr = truncate(QString::fromUtf8(proc.readAllStandardError()), 2000);

        QString report = info;
        report += QString("Timeout limit: %1 ms\nActual runtime: %2 ms\n")
                  .arg(timeoutMs).arg(elapsed);

        if (exitedInTime) {
            int code = proc.exitCode();
            report += QString("Exit code: %1 (%2)\n").arg(code).arg(code == 0 ? "success" : "failure");
            if (!stdoutStr.isEmpty()) report += "\n--- stdout ---\n" + stdoutStr;
            if (!stderrStr.isEmpty()) report += "\n--- stderr ---\n" + stderrStr;
            return code == 0 ? ToolResult::ok(report) : ToolResult::err(report);
        } else {
            proc.terminate();
            if (!proc.waitForFinished(1000)) proc.kill();
            report += QString("Result: still running after %1 ms — terminated\n").arg(timeoutMs);
            if (!stdoutStr.isEmpty()) report += "\n--- stdout (partial) ---\n" + stdoutStr;
            if (!stderrStr.isEmpty()) report += "\n--- stderr (partial) ---\n" + stderrStr;
            return ToolResult::ok(report);
        }
    }
};