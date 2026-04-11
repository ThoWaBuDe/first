#pragma once
// ─── GetTimeTool ───────────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"

class GetTimeTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "get_time"; }

    QString description() const override
    {
        return "Returns current local time, date, UTC and Unix timestamp.";
    }

    QJsonObject properties() const override { return {}; }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &) override
    {
        QDateTime now = QDateTime::currentDateTime();
        return ToolResult::ok(QString(
            "Date:     %1\n"
            "Time:     %2\n"
            "UTC:      %3\n"
            "Unix:     %4")
            .arg(now.toString("dddd, dd. MMMM yyyy"))
            .arg(now.toString("HH:mm:ss"))
            .arg(now.toUTC().toString("HH:mm:ss UTC"))
            .arg(now.toSecsSinceEpoch()));
    }
};