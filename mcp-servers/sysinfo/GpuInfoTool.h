#pragma once
// ─── GpuInfoTool ──────────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"

class GpuInfoTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "gpu_info"; }

    QString description() const override
    {
        return "Returns NVIDIA GPU information: name, VRAM (total/used/free), "
               "GPU utilization, temperature, power draw, power limit, clocks, driver. "
               "Uses nvidia-smi. Returns 'No NVIDIA GPU' if not available. "
               "Useful to monitor VRAM usage during model inference.";
    }

    QJsonObject properties() const override { return {}; }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &) override
    {
        auto [checkOut, checkCode] = runCmd("which", {"nvidia-smi"});
        if (checkCode != 0)
            return ToolResult::ok(
                "No NVIDIA GPU detected (nvidia-smi not found).\n"
                "AMD/Intel GPUs: use sys_info for basic system info.");

        QStringList queryFields = {
            "index", "name",
            "memory.total", "memory.used", "memory.free",
            "utilization.gpu",
            "temperature.gpu",
            "power.draw", "power.limit",
            "power.min_limit", "power.max_limit",
            "clocks.gr", "clocks.mem",
            "driver_version"
        };

        auto [smiOut, smiCode] = runCmd("nvidia-smi",
            {"--query-gpu=" + queryFields.join(','),
             "--format=csv,noheader,nounits"});

        if (smiCode != 0 || smiOut.isEmpty())
            return ToolResult::err(QString("nvidia-smi error:\n%1").arg(smiOut));

        QString result;
        int gpuIndex = 0;

        for (const QString &line : smiOut.split('\n', Qt::SkipEmptyParts)) {
            QStringList fields = line.split(',');
            for (QString &f : fields) f = f.trimmed();

            if (fields.size() < queryFields.size()) {
                result += QString("GPU %1: parse error\n").arg(gpuIndex);
                ++gpuIndex;
                continue;
            }

            auto toGb = [](const QString &mib) -> QString {
                bool ok;
                double v = mib.toDouble(&ok);
                return ok ? QString("%1 MiB (%2 GB)").arg(mib).arg(v/1024.0, 0,'f',1)
                          : mib;
            };

            result += QString("─── GPU %1: %2 ───\n").arg(fields[0], fields[1]);
            result += QString("VRAM total:    %1\n").arg(toGb(fields[2]));
            result += QString("VRAM used:     %1\n").arg(toGb(fields[3]));
            result += QString("VRAM free:     %1\n").arg(toGb(fields[4]));
            result += QString("GPU util:      %1 %%\n").arg(fields[5]);
            result += QString("Temperature:   %1 °C\n").arg(fields[6]);
            result += QString("Power draw:    %1 W\n").arg(fields[7]);
            result += QString("Power limit:   %1 W\n").arg(fields[8]);
            result += QString("Power min/max: %1 W / %2 W\n").arg(fields[9], fields[10]);
            result += QString("GPU clock:     %1 MHz\n").arg(fields[11]);
            result += QString("Mem clock:     %1 MHz\n").arg(fields[12]);
            result += QString("Driver:        %1\n").arg(fields[13]);
            result += "\n";
            ++gpuIndex;
        }

        return ToolResult::ok(result.trimmed());
    }
};