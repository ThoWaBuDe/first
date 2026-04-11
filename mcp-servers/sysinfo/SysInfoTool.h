#pragma once
// ─── SysInfoTool ──────────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"
#include <QThread>
#include <QFile>
#include <QDir>
#include <QSysInfo>
#include <QByteArray>
#include <QRegularExpression>

class SysInfoTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "sys_info"; }

    QString description() const override
    {
        return "Returns system info: hostname, OS, kernel, CPU cores and temperature, RAM.";
    }

    QJsonObject properties() const override { return {}; }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &) override
    {
        int cores = QThread::idealThreadCount();
        QString memInfo;

        QFile memFile("/proc/meminfo");
        QByteArray memContent;
        if (memFile.open(QIODevice::ReadOnly)) {
            memContent = memFile.readAll();
            memFile.close();
        }
        
        if (!memContent.isEmpty()) {
            QStringList lines = QString::fromUtf8(memContent).split('\n');
            
            qint64 memTotalKb = 0;
            qint64 memFreeKb = 0;
            qint64 memAvailKb = 0;
            
            for (const QString &line : lines) {
                QString key = line.section(':', 0, 0).trimmed();
                QString val = line.section(':', 1, 1).trimmed();
                
                if (key == "MemTotal") {
                    memTotalKb = val.section(' ', 0, 0).toLongLong();
                } else if (key == "MemFree") {
                    memFreeKb = val.section(' ', 0, 0).toLongLong();
                } else if (key == "MemAvailable") {
                    memAvailKb = val.section(' ', 0, 0).toLongLong();
                }
            }
            
            if (memTotalKb > 0) {
                memInfo = QString("RAM total:     %1 GB\nRAM free:      %2 GB\nRAM available: %3 GB")
                    .arg(memTotalKb / 1024.0 / 1024.0, 0, 'f', 1)
                    .arg(memFreeKb / 1024.0 / 1024.0, 0, 'f', 1)
                    .arg(memAvailKb / 1024.0 / 1024.0, 0, 'f', 1);
            } else {
                memInfo = "RAM: could not parse /proc/meminfo";
            }
        } else {
            memInfo = "RAM: /proc/meminfo not readable";
        }

        QString cpuTemp = "n/a";
        QString cpuTempSource;

        QDir thermalDir("/sys/class/thermal");
        QStringList zones = thermalDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &zone : zones) {
            if (!zone.startsWith("thermal_zone")) continue;
            QFile tempFile(thermalDir.filePath(zone + "/temp"));
            if (tempFile.open(QIODevice::ReadOnly)) {
                bool ok;
                int millideg = QString::fromUtf8(tempFile.readAll().trimmed()).toInt(&ok);
                tempFile.close();
                if (ok && millideg > 0 && millideg < 150000) {
                    cpuTemp = QString("%1 °C").arg(millideg / 1000.0, 0, 'f', 1);
                    cpuTempSource = "(sys/class/thermal)";
                    break;
                }
            }
        }

        auto [sensorsOut, sensorsCode] = runCmd("sensors", {});
        
        if (cpuTemp == "n/a" && sensorsCode == 0 && !sensorsOut.isEmpty()) {
            QStringList lines = sensorsOut.split('\n');
            for (const QString &line : lines) {
                if (line.contains("CPUTIN") || line.contains("Tctl") || line.contains("CPU temp")) {
                    QRegularExpression rx("([+-]?[0-9]+\\.?[0-9]*)°C");
                    QRegularExpressionMatch match = rx.match(line);
                    if (match.hasMatch()) {
                        cpuTemp = match.captured(1) + " °C";
                        cpuTempSource = "(sensors)";
                        break;
                    }
                }
            }
        }

        QString sensorsOutput;
        if (sensorsCode == 0 && !sensorsOut.isEmpty()) {
            sensorsOutput = "\n" + sensorsOut;
        }

        return ToolResult::ok(QString(
            "Hostname:    %1\n"
            "OS:          %2\n"
            "Kernel:      %3\n"
            "CPU cores:   %4 (logical)\n"
            "CPU temp:    %5 %6\n"
            "%7%8")
            .arg(QSysInfo::machineHostName())
            .arg(QSysInfo::prettyProductName())
            .arg(QSysInfo::kernelVersion())
            .arg(cores).arg(cpuTemp).arg(cpuTempSource).arg(memInfo).arg(sensorsOutput));
    }
};