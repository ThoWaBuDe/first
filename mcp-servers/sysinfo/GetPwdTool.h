#pragma once
// ─── GetPwdTool ───────────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"
#include <QStandardPaths>

class GetPwdTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "get_pwd"; }

    QString description() const override
    {
        return "Returns the sandbox root directory (~/llamatools/).";
    }

    QJsonObject properties() const override { return {}; }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &) override
    {
        QString sandbox = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                        + "/llamatools";
        return ToolResult::ok(QString("Sandbox root: %1").arg(sandbox));
    }
};