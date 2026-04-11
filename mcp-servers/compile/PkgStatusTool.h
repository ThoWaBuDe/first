#pragma once
// ─── PkgStatusTool ───────────────────────────────────────────────────────────

#include "CompileToolBase.h"

class PkgStatusTool : public CompileToolBase
{
public:
    using CompileToolBase::CompileToolBase;

    QString name() const override { return "pkg_status"; }

    QString description() const override
    {
        return "List installed development packages (cmake, g++, qt6-base-dev, etc.).";
    }

    QJsonObject properties() const override { return {}; }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &) override
    {
        QStringList packages = {
            "cmake", "g++", "clang", "make", "ninja-build",
            "qt6-base-dev", "libgl1-mesa-dev", "pkg-config"
        };
        QString result;
        for (const QString &pkg : packages) {
            auto [out, failed] = runProcess(
                "dpkg-query", {"-W", "-f=${Package} ${Version}\n", pkg}, "/tmp", 5000);
            result += failed
                ? QString("[ ] %1 - not installed\n").arg(pkg)
                : QString("[x] %1\n").arg(out.trimmed());
        }
        return ToolResult::ok(result.trimmed());
    }
};