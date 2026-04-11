#pragma once
// ─── DiskFreeTool ─────────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"
#include <QStorageInfo>

class DiskFreeTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "disk_free"; }

    QString description() const override
    {
        return "Returns disk usage for a path (like df -h).";
    }

    QJsonObject properties() const override
    {
        return {{"path", prop("string", "Path to check (default: home directory)")}};
    }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString path = args.value("path").toString(
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation));

        QStorageInfo storage(path);
        if (!storage.isValid())
            return ToolResult::err(QString("Error: No filesystem at '%1'.").arg(path));

        qint64 total = storage.bytesTotal();
        qint64 avail = storage.bytesAvailable();
        qint64 used  = total - avail;
        int pct = total > 0 ? static_cast<int>(used*100/total) : 0;

        return ToolResult::ok(QString(
            "Filesystem: %1\n"
            "Path:       %2\n"
            "Total:      %3\n"
            "Used:       %4 (%5%)\n"
            "Free:       %6")
            .arg(storage.fileSystemType())
            .arg(path)
            .arg(toHumanBytes(total))
            .arg(toHumanBytes(used)).arg(pct)
            .arg(toHumanBytes(avail)));
    }
};