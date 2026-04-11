#pragma once
// ─── CudaInfoTool ─────────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"

class CudaInfoTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "cuda_info"; }

    QString description() const override
    {
        return "Returns CUDA version, compute capability, and driver info. "
               "Useful to verify CUDA compatibility for llama.cpp with CUDA.";
    }

    QJsonObject properties() const override { return {}; }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &) override
    {
        auto [checkOut, checkCode] = runCmd("which", {"nvidia-smi"});
        if (checkCode != 0)
            return ToolResult::ok("No NVIDIA GPU detected (CUDA requires NVIDIA).");

        auto [smiOut, smiCode] = runCmd("nvidia-smi",
            {"--query-gpu=driver_version,compute_cap,name",
             "--format=csv,noheader,nounits"});

        if (smiCode != 0 || smiOut.isEmpty())
            return ToolResult::err("Could not query CUDA info via nvidia-smi");

        QStringList lines = smiOut.split('\n', Qt::SkipEmptyParts);
        if (lines.isEmpty())
            return ToolResult::err("No GPU found");

        QStringList fields = lines[0].split(',');
        for (QString &f : fields) f = f.trimmed();

        if (fields.size() < 3)
            return ToolResult::err("Unexpected nvidia-smi output format");

        QString driver = fields[0];
        QString compute = fields[1];
        QString gpuName = fields[2];

        auto [nvccOut, nvccCode] = runCmd("nvcc", {"--version"});
        QString cudaVersion = "not found";
        if (nvccCode == 0 && !nvccOut.isEmpty()) {
            QStringList nvccLines = nvccOut.split('\n');
            for (const QString &l : nvccLines) {
                if (l.contains("release")) {
                    cudaVersion = l.section("release", 1, 1).section(',', 0, 0).trimmed();
                    break;
                }
            }
        }

        return ToolResult::ok(QString(
            "CUDA Information:\n"
            "────────────────────────────────────────────────────\n"
            "GPU:           %1\n"
            "Driver:        %2\n"
            "Compute Cap:   %3\n"
            "CUDA Toolkit:  %4\n"
            "\nNote: llama.cpp uses CUDA if compiled with CUDA support.")
            .arg(gpuName).arg(driver).arg(compute).arg(cudaVersion));
    }
};