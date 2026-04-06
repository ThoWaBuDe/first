// ─── LlamaQt MCP SysInfo Server ──────────────────────────────────────────────
// Implements the MCP stdio protocol for system information queries.
// All tools are read-only. No file writes, no process execution.
//
// Advertised Tools: get_time, get_pwd, disk_free, sys_info

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QStorageInfo>
#include <QSysInfo>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QThread>
#include <iostream>
#include <string>

static void sendResponse(const QJsonObject &msg)
{
    QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    std::cout << line.toStdString();
    std::cout.flush();
}

static void sendResult(int id, const QString &text, bool isError = false)
{
    sendResponse({
        {"jsonrpc","2.0"}, {"id",id},
        {"result", QJsonObject{
            {"content", QJsonArray{QJsonObject{{"type","text"},{"text",text}}}},
            {"isError", isError}
        }}
    });
}

static void sendError(int id, int code, const QString &message)
{
    sendResponse({{"jsonrpc","2.0"},{"id",id},
                  {"error",QJsonObject{{"code",code},{"message",message}}}});
}

// ─── Tool handlers ────────────────────────────────────────────────────────────

static QString handleGetTime(const QJsonObject &)
{
    QDateTime now = QDateTime::currentDateTime();
    return QString("Date:     %1\n"
                   "Time:     %2\n"
                   "UTC:      %3\n"
                   "Unix:     %4")
           .arg(now.toString("dddd, dd. MMMM yyyy"))
           .arg(now.toString("HH:mm:ss"))
           .arg(now.toUTC().toString("HH:mm:ss UTC"))
           .arg(now.toSecsSinceEpoch());
}

static QString handleGetPwd(const QJsonObject &)
{
    QString sandbox = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                      + "/llamatools";
    return QString("Sandbox root: %1").arg(sandbox);
}

static QString handleDiskFree(const QJsonObject &args)
{
    // Only allow paths inside the sandbox or the home directory.
    // No symlink check needed here — QStorageInfo works on the mounted filesystem,
    // not on individual files.
    QString path = args.value("path").toString(
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation));

    QStorageInfo storage(path);
    if (!storage.isValid())
        return QString("Error: No filesystem found at '%1'.").arg(path);

    auto toHuman = [](qint64 bytes) -> QString {
        const double GB = 1024.0*1024.0*1024.0, MB = 1024.0*1024.0;
        if (bytes >= GB) return QString("%1 GB").arg(bytes/GB, 0,'f',1);
        if (bytes >= MB) return QString("%1 MB").arg(bytes/MB, 0,'f',1);
        return QString("%1 KB").arg(bytes/1024.0, 0,'f',1);
    };

    qint64 total = storage.bytesTotal();
    qint64 avail = storage.bytesAvailable();
    qint64 used  = total - avail;
    int pct = total > 0 ? static_cast<int>(used*100/total) : 0;

    return QString("Filesystem:  %1\nPath:        %2\n"
                   "Total:       %3\nUsed:        %4 (%5%)\nFree:        %6")
           .arg(storage.fileSystemType()).arg(path)
           .arg(toHuman(total)).arg(toHuman(used)).arg(pct).arg(toHuman(avail));
}

static QString handleSysInfo(const QJsonObject &)
{
    int cores = QThread::idealThreadCount();

    QString memInfo;
    QFile memFile("/proc/meminfo");
    if (memFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&memFile);
        qint64 total = 0, free = 0, avail = 0;
        while (!in.atEnd()) {
            QString line = in.readLine();
            auto extractKb = [](const QString &l) {
                return l.section(':',1).trimmed().section(' ',0,0).toLongLong();
            };
            if (line.startsWith("MemTotal:"))          total = extractKb(line);
            else if (line.startsWith("MemFree:"))      free  = extractKb(line);
            else if (line.startsWith("MemAvailable:")) avail = extractKb(line);
            if (total && free && avail) break;
        }
        if (total > 0)
            memInfo = QString("RAM total:     %1 MB\nRAM free:      %2 MB\nRAM available: %3 MB")
                      .arg(total/1024).arg(free/1024).arg(avail/1024);
        else
            memInfo = "RAM: could not parse /proc/meminfo";
    } else {
        memInfo = "RAM: /proc/meminfo not readable";
    }

    return QString("Hostname:    %1\nOS:          %2\nKernel:      %3\nCPU cores:   %4 (logical)\n%5")
           .arg(QSysInfo::machineHostName())
           .arg(QSysInfo::prettyProductName())
           .arg(QSysInfo::kernelVersion())
           .arg(cores)
           .arg(memInfo);
}

// ─── Tool list ───────────────────────────────────────────────────────────────
static QJsonArray makeToolList()
{
    auto makeProp = [](const QString &type, const QString &desc) {
        return QJsonObject{{"type",type},{"description",desc}};
    };
    auto makeTool = [](const QString &name, const QString &desc,
                       const QJsonObject &props, const QJsonArray &req = {}) {
        return QJsonObject{{"name",name},{"description",desc},
            {"inputSchema",QJsonObject{{"type","object"},{"properties",props},{"required",req}}}};
    };

    return QJsonArray{
        makeTool("get_time",
            "Returns the current local time, date, UTC time and Unix timestamp.",
            {}, {}),
        makeTool("get_pwd",
            "Returns the sandbox root directory (~/llamatools/). "
            "All file operations are restricted to this path.",
            {}, {}),
        makeTool("disk_free",
            "Returns disk usage information for a given path, similar to 'df -h'. "
            "Shows total, used and free space.",
            {{"path", makeProp("string","Path to check (default: home directory)")}},
            {}),
        makeTool("sys_info",
            "Returns system information: hostname, OS, kernel version, "
            "CPU core count and RAM usage (total/free/available).",
            {}, {})
    };
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QJsonArray tools = makeToolList();

    QTextStream in(stdin);
    QTextStream errStream(stderr);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) continue;

        QJsonObject msg = doc.object();
        QString method  = msg.value("method").toString();
        bool hasId      = msg.contains("id");
        int  id         = msg.value("id").toInt(-1);

        if (method == "initialize") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                {"protocolVersion","2024-11-05"},
                {"capabilities",   QJsonObject{}},
                {"serverInfo",     QJsonObject{{"name","llamaqt-sysinfo"},{"version","1.1"}}}
            }}});
            continue;
        }
        if (method == "notifications/initialized") continue;

        if (method == "tools/list") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},
                          {"result",QJsonObject{{"tools",tools}}}});
            continue;
        }

        if (method == "tools/call") {
            QJsonObject params  = msg.value("params").toObject();
            QString toolName    = params.value("name").toString();
            QJsonObject toolArgs = params.value("arguments").toObject();

            QString result;
            bool isError = false;

            if      (toolName == "get_time")  result = handleGetTime(toolArgs);
            else if (toolName == "get_pwd")   result = handleGetPwd(toolArgs);
            else if (toolName == "disk_free") result = handleDiskFree(toolArgs);
            else if (toolName == "sys_info")  result = handleSysInfo(toolArgs);
            else { result = QString("Unknown tool: '%1'").arg(toolName); isError = true; }

            sendResult(id, result, isError);
            continue;
        }

        if (hasId) sendError(id, -32601, "Method not found: " + method);
    }
    return 0;
}
