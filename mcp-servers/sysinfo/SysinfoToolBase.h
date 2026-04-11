#pragma once
// ─── SysinfoToolBase ───────────────────────────────────────────────────────────
// Zwischenschicht zwischen ToolBase und den konkreten sysinfo Tools.
//
// Enthält:
//   1. Gemeinsame Hilfsfunktionen: runCmd(), toHuman()
//   2. Basisklasse SysinfoToolBase mit PathPolicy (vorbereitet)
//   3. Schema-Hilfsmethoden: prop(), req()
//
// Pattern analog zu TreeSitterToolBase.h

#include "../common/ToolBase.h"
#include "../common/PathPolicy.h"

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QPair>

// ─── Hilfsfunktionen ───────────────────────────────────────────────────────────

static QPair<QString,int> runCmd(const QString &cmd, const QStringList &args,
                                 int timeoutMs = 5000)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(cmd, args);
    if (!proc.waitForStarted(2000)) return {"", 127};
    if (!proc.waitForFinished(timeoutMs)) { proc.kill(); return {"timeout", 1}; }
    return {QString::fromUtf8(proc.readAll()).trimmed(), proc.exitCode()};
}

static QString toHumanBytes(qint64 bytes)
{
    const double GB = 1024.0*1024.0*1024.0, MB = 1024.0*1024.0, KB = 1024.0;
    if (bytes >= GB) return QString("%1 GB").arg(bytes/GB, 0,'f',1);
    if (bytes >= MB) return QString("%1 MB").arg(bytes/MB, 0,'f',1);
    if (bytes >= KB) return QString("%1 KB").arg(bytes/KB, 0,'f',1);
    return QString("%1 B").arg(bytes);
}

// ─── SysinfoToolBase ───────────────────────────────────────────────────────────
class SysinfoToolBase : public ToolBase
{
public:
    explicit SysinfoToolBase(PathPolicy *policy = nullptr)
        : m_policy(policy)
    {}

protected:
    PathPolicy *m_policy = nullptr;

    // ── Schema-Hilfsmethoden ─────────────────────────────────────────────────
    static QJsonObject prop(const QString &type, const QString &desc)
    {
        return QJsonObject{{"type", type}, {"description", desc}};
    }

    static QJsonArray req(std::initializer_list<const char*> fields)
    {
        QJsonArray arr;
        for (const char *f : fields) arr.append(f);
        return arr;
    }
};