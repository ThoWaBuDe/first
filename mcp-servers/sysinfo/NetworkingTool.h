#pragma once
// ─── NetworkingTool ───────────────────────────────────────────────────────────

#include "SysinfoToolBase.h"
#include <QFile>

class NetworkingTool : public SysinfoToolBase
{
public:
    using SysinfoToolBase::SysinfoToolBase;

    QString name() const override { return "networking"; }

    QString description() const override
    {
        return "Returns network interfaces, IP addresses, and connection status. "
               "Useful to check network configuration and active connections.";
    }

    QJsonObject properties() const override
    {
        return {
            {"detail", prop("boolean", "Show detailed stats (default: false)")}
        };
    }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &args) override
    {
        bool detail = args.value("detail").toBool(false);

        QString result;

        QFile ipAddr("/proc/net/dev");
        if (ipAddr.open(QIODevice::ReadOnly | QIODevice::Text)) {
            result += "Network Interfaces:\n";
            result += "────────────────────────────────────────────────────\n";

            QTextStream in(&ipAddr);
            in.readLine();
            in.readLine();

            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                if (line.isEmpty()) continue;

                QString iface = line.section(':', 0, 0).trimmed();
                if (iface == "lo") continue;

                QString stats = line.section(':', 1, 1).trimmed();
                QStringList fields = stats.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

                if (fields.size() >= 8) {
                    qint64 rx = fields[0].toLongLong();
                    qint64 tx = fields[8].toLongLong();

                    result += QString("● %1\n").arg(iface);
                    result += QString("   RX: %1  TX: %2\n")
                              .arg(toHumanBytes(rx))
                              .arg(toHumanBytes(tx));
                }
            }
        }

        auto [ipOut, ipCode] = runCmd("ip", {"addr", "show"});
        if (ipCode == 0) {
            result += "\nIP Addresses:\n";
            result += "────────────────────────────────────────────────────\n";

            QStringList lines = ipOut.split('\n');
            for (const QString &line : lines) {
                if (line.contains("inet ") && !line.contains("inet6")) {
                    QString iface = line.section(':', 0, 0).trimmed();
                    QString ip = line.section("inet ", 1, 1).section('/', 0, 0);
                    result += QString("%1: %2\n").arg(iface, ip);
                }
            }
        }

        if (detail) {
            auto [ssOut, ssCode] = runCmd("ss", {"-tun"}, 3000);
            if (ssCode == 0 && !ssOut.isEmpty()) {
                result += "\nActive Connections (TCP):\n";
                result += "────────────────────────────────────────────────────\n";
                result += ssOut;
            }
        }

        if (result.isEmpty())
            return ToolResult::err("Could not read network information");

        return ToolResult::ok(result.trimmed());
    }
};

#include <QRegularExpression>
#include <QIODevice>