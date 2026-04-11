#pragma once
// ─── DiskIoTool ────────────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"
#include <QFile>

class DiskIoTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "disk_io"; }

    QString description() const override
    {
        return "Returns disk I/O statistics (read/write bytes per device). "
               "Useful to monitor disk activity during model loading.";
    }

    QJsonObject properties() const override
    {
        return {
            {"refresh", prop("boolean", "Force fresh read (default: cached)")}
        };
    }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &args) override
    {
        Q_UNUSED(args);

        QFile diskStats("/proc/diskstats");
        if (!diskStats.open(QIODevice::ReadOnly))
            return ToolResult::err("Cannot read /proc/diskstats");

        QString content = QString::fromUtf8(diskStats.readAll());
        QStringList lines = content.split('\n', Qt::SkipEmptyParts);

        QString result = "Disk I/O Statistics:\n";
        result += "────────────────────────────────────────────────────\n";
        result += "DEVICE        READS    READ(B)    WRITES   WRITE(B)\n";
        result += "────────────────────────────────────────────────────\n";

        for (const QString &line : lines) {
            QStringList fields = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (fields.size() < 14) continue;

            QString device = fields[2];
            if (device.startsWith("ram") || device.startsWith("loop")) continue;

            qint64 reads = fields[3].toLongLong();
            qint64 readSectors = fields[5].toLongLong();
            qint64 writes = fields[7].toLongLong();
            qint64 writeSectors = fields[9].toLongLong();

            if (reads == 0 && writes == 0) continue;

            result += QString("%1 %2 %3 %4 %5\n")
                .arg(device.leftJustified(12, ' '))
                .arg(QString::number(reads).rightJustified(8, ' '))
                .arg(toHumanBytes(readSectors * 512).leftJustified(12, ' '))
                .arg(QString::number(writes).rightJustified(8, ' '))
                .arg(toHumanBytes(writeSectors * 512));
        }

        result += "\nNote: Values are since system boot.";
        return ToolResult::ok(result);
    }
};

#include <QRegularExpression>
#include <QIODevice>