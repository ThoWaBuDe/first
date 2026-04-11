#pragma once
// ─── ProcessListTool ───────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"

class ProcessListTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "process_list"; }

    QString description() const override
    {
        return "Returns top processes by CPU or memory usage. "
               "Useful to monitor llama.cpp and system load. "
               "Default: top 10 by CPU.";
    }

    QJsonObject properties() const override
    {
        return {
            {"sort", prop("string", "Sort by: 'cpu' or 'memory' (default: cpu)")},
            {"limit", prop("integer", "Number of processes (default: 10)")}
        };
    }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString sortBy = args.value("sort").toString("cpu");
        int limit = args.value("limit").toInt(10);
        if (limit < 1) limit = 1;
        if (limit > 50) limit = 50;

        auto [out, code] = runCmd("ps", 
            {"aux", "--no-headers"}, 10000);

        if (code != 0)
            return ToolResult::err("Error: could not get process list");

        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        QList<QStringList> procs;

        for (const QString &line : lines) {
            QStringList fields = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (fields.size() >= 11) {
                bool okCpu, okMem;
                double cpu = fields[2].toDouble(&okCpu);
                double mem = fields[3].toDouble(&okMem);
                if (okCpu && okMem) {
                    procs.append(fields);
                }
            }
        }

        if (sortBy == "memory") {
            std::sort(procs.begin(), procs.end(), [](const QStringList &a, const QStringList &b) {
                return a[3].toDouble() > b[3].toDouble();
            });
        } else {
            std::sort(procs.begin(), procs.end(), [](const QStringList &a, const QStringList &b) {
                return a[2].toDouble() > b[2].toDouble();
            });
        }

        QString result = QString("Top %1 processes by %2:\n").arg(limit).arg(sortBy);
        result += "PID    USER       CPU%%   MEM%%   RSS     COMMAND\n";
        result += "────────────────────────────────────────────────────\n";

        for (int i = 0; i < qMin(limit, procs.size()); ++i) {
            const QStringList &p = procs[i];
            QString pid = p[1].leftJustified(7, ' ');
            QString user = p[0].leftJustified(9, ' ');
            QString cpu = p[2].leftJustified(6, ' ');
            QString mem = p[3].leftJustified(6, ' ');
            QString rss = QString::number(p[5].toInt() / 1024).leftJustified(7, ' ');
            QString cmd = p.mid(10).join(" ").left(30);
            result += QString("%1 %2 %3 %4 %5 %6\n").arg(pid, user, cpu, mem, rss, cmd);
        }

        return ToolResult::ok(result.trimmed());
    }
};

#include <QRegularExpression>