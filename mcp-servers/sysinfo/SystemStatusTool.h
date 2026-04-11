#pragma once
// ─── SystemStatusTool ──────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"
#include <QFile>
#include <QTextStream>

class SystemStatusTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "system_status"; }

    QString description() const override
    {
        return "Returns system uptime, load average, and logged-in users. "
               "Useful to check system health and load.";
    }

    QJsonObject properties() const override { return {}; }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &) override
    {
        QString result;

        QFile uptimeFile("/proc/uptime");
        if (uptimeFile.open(QIODevice::ReadOnly)) {
            QString content = QString::fromUtf8(uptimeFile.readAll()).trimmed();
            double upSecs = content.section(' ', 0, 0).toDouble();
            int days = static_cast<int>(upSecs / 86400);
            int hours = static_cast<int>((upSecs / 3600)) % 24;
            int mins = static_cast<int>(upSecs / 60) % 60;
            result += QString("Uptime: %1 days, %2h %3m\n").arg(days).arg(hours).arg(mins);
        }

        QFile loadFile("/proc/loadavg");
        if (loadFile.open(QIODevice::ReadOnly)) {
            QString content = QString::fromUtf8(loadFile.readAll()).trimmed();
            QStringList parts = content.split(' ');
            if (parts.size() >= 5) {
                result += QString("Load average: %1 (1m)  %2 (5m)  %3 (15m)\n")
                          .arg(parts[0]).arg(parts[1]).arg(parts[2]);
            }
        }

        auto [whoOut, whoCode] = runCmd("who", {"-q"});
        if (whoCode == 0) {
            result += "\nLogged in users: ";
            result += whoOut.trimmed();
            result += "\n";
        }

        auto [uptimeCmdOut, uptimeCmdCode] = runCmd("uptime", {"-p"});
        if (uptimeCmdCode == 0) {
            result += QString("\nPretty uptime: %1").arg(uptimeCmdOut.trimmed());
        }

        return ToolResult::ok(result.trimmed());
    }
};