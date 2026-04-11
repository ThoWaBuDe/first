#pragma once
// ─── BatteryTool ──────────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"
#include <QDir>

class BatteryTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "battery"; }

    QString description() const override
    {
        return "Returns battery status: charge level, charging state, time remaining. "
               "Useful on laptops to monitor power during inference.";
    }

    QJsonObject properties() const override { return {}; }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &) override
    {
        QDir batteryDir("/sys/class/power_supply");
        QStringList entries = batteryDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QStringList batteries;

        for (const QString &entry : entries) {
            QFile typeFile(batteryDir.filePath(entry + "/type"));
            if (typeFile.open(QIODevice::ReadOnly)) {
                if (QString::fromUtf8(typeFile.readAll()).trimmed() == "Battery")
                    batteries.append(entry);
            }
        }

        if (batteries.isEmpty())
            return ToolResult::ok("No battery detected (desktop PC or battery not accessible).\n"
                                   "Use sys_info for other system metrics.");

        QString result = "Battery Status:\n";
        result += "────────────────────────────────────────────────────\n";

        for (const QString &batt : batteries) {
            QString path = batteryDir.filePath(batt);

            QString name = batt;
            QFile nameFile(path + "/name");
            if (nameFile.open(QIODevice::ReadOnly))
                name = QString::fromUtf8(nameFile.readAll()).trimmed();

            result += QString("─── %1 ───\n").arg(name);

            QFile capacityFile(path + "/capacity");
            int capacity = -1;
            if (capacityFile.open(QIODevice::ReadOnly))
                capacity = QString::fromUtf8(capacityFile.readAll()).trimmed().toInt();

            QFile statusFile(path + "/status");
            QString status = "Unknown";
            if (statusFile.open(QIODevice::ReadOnly))
                status = QString::fromUtf8(statusFile.readAll()).trimmed();

            result += QString("Charge:      %1%%\n").arg(capacity);
            result += QString("Status:      %1\n").arg(status);

            QFile voltageFile(path + "/voltage_now");
            if (voltageFile.open(QIODevice::ReadOnly)) {
                double voltage = voltageFile.readAll().toDouble() / 1e6;
                result += QString("Voltage:     %1 V\n").arg(voltage, 0, 'f', 2);
            }

            double power = 0;
            QFile powerFile(path + "/power_now");
            if (powerFile.open(QIODevice::ReadOnly)) {
                power = powerFile.readAll().toDouble() / 1e6;
                result += QString("Power:       %1 W\n").arg(power, 0, 'f', 2);
            }

            QFile energyFile(path + "/energy_now");
            QFile energyFullFile(path + "/energy_full");
            if (energyFile.open(QIODevice::ReadOnly) && energyFullFile.open(QIODevice::ReadOnly)) {
                double now = energyFile.readAll().toDouble() / 1e6;
                double full = energyFullFile.readAll().toDouble() / 1e6;
                if (full > 0) {
                    int percent = static_cast<int>(now / full * 100);
                    double hours = (status.contains("Charging"))
                        ? (full - now) / (power > 0 ? power : 1)
                        : now / (power > 0 ? power : 1);
                    int mins = static_cast<int>(hours * 60);
                    QString timeEst = (mins > 0)
                        ? QString("%1h %2m").arg(mins / 60).arg(mins % 60)
                        : "calculating...";
                    result += QString("Time remain: %1\n").arg(timeEst);
                }
            }

            result += "\n";
        }

        return ToolResult::ok(result.trimmed());
    }
};

#include <QIODevice>