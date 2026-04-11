#pragma once
// ─── SetPowerLimitTool ─────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"

class SetPowerLimitTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "set_power_limit"; }

    QString description() const override
    {
        return "Set the NVIDIA GPU power limit in Watts via nvidia-smi. "
               "Useful to reduce heat and noise during long inference sessions "
               "at the cost of ~5-10% performance. "
               "Requires root or nvidia-persistenced. "
               "The value is checked against the GPU's min/max limits.";
    }

    QJsonObject properties() const override
    {
        return {
            {"watts",  prop("integer", "Target power limit in Watts")},
            {"gpu_id", prop("integer", "GPU index (default: 0)")}
        };
    }

    QJsonArray required() const override { return {"watts"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        int watts = args.value("watts").toInt(0);
        int gpuId = args.value("gpu_id").toInt(0);

        if (watts <= 0)
            return ToolResult::err("Error: 'watts' must be a positive integer.");
        if (watts < 10 || watts > 600)
            return ToolResult::err(QString("Error: %1W seems unreasonable (expected 10-600W). "
                            "Check gpu_info for min/max limits.").arg(watts));

        auto [limOut, limCode] = runCmd("nvidia-smi",
            {QString("--id=%1").arg(gpuId),
             "--query-gpu=power.min_limit,power.max_limit",
             "--format=csv,noheader,nounits"});

        if (limCode == 0 && !limOut.isEmpty()) {
            QStringList lims = limOut.split(',');
            if (lims.size() == 2) {
                bool okMin, okMax;
                double minW = lims[0].trimmed().toDouble(&okMin);
                double maxW = lims[1].trimmed().toDouble(&okMax);
                if (okMin && okMax) {
                    if (watts < minW || watts > maxW) {
                        return ToolResult::err(QString("Error: %1W is outside allowed range "
                                        "[%2W - %3W] for GPU %4.")
                                .arg(watts)
                                .arg(minW, 0, 'f', 0)
                                .arg(maxW, 0, 'f', 0)
                                .arg(gpuId));
                    }
                }
            }
        }

        auto [out, code] = runCmd("nvidia-smi",
            {"-i", QString::number(gpuId), "-pl", QString::number(watts)});

        QString report = QString("GPU %1 power limit → %2W\n").arg(gpuId).arg(watts);
        report += out + "\n";

        if (code != 0) {
            report += "\nHinweis: nvidia-smi -pl benötigt root-Rechte oder\n"
                      "nvidia-persistenced muss laufen. Versuche:\n"
                      "  sudo nvidia-smi -pl " + QString::number(watts) + "\n"
                      "oder füge den User zur 'video' Gruppe hinzu.";
            return ToolResult::err(report);
        }

        report += "\nTipp: Prüfe mit gpu_info ob das neue Limit aktiv ist.";
        return ToolResult::ok(report);
    }
};