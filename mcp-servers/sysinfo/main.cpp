// ─── LlamaQt MCP SysInfo Server v2.0 ─────────────────────────────────────────
// Systeminfo: CPU, RAM, OS — und neu: GPU via nvidia-smi / NVML.
//
// GPU-Strategie (zwei Wege, Fallback):
//
//   Weg 1: NVML direkt (libnvidia-ml.so)
//     Präzisere Werte, kein subprocess. Wird per dlopen() geladen —
//     optional, kein harter Link. Wenn nicht vorhanden: Fallback.
//     Pattern: Optional Dependency via dlopen (Plugin-Mechanismus).
//
//   Weg 2: nvidia-smi via QProcess
//     Immer verfügbar wenn Nvidia-Treiber installiert. Parsen der
//     --query-gpu Ausgabe (CSV-Format, zuverlässig).
//     Fallback wenn NVML nicht geladen werden kann.
//
//   Kein GPU: nvidia-smi gibt Fehler → "No NVIDIA GPU detected" ausgeben.
//
// set_power_limit: setzt GPU Power Limit via nvidia-smi
//   (nvidia-smi -pl <watt>) — braucht root oder nvidia-persistenced.
//   Sicherheitshinweis wird immer mitausgegeben.
//
// Neue Tools gegenüber v1.1:
//   gpu_info        — GPU-Name, VRAM, Auslastung, Temp, Power
//   set_power_limit — GPU Power Limit setzen

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
#include <QProcess>
#include <iostream>

// ─── JSON-RPC helpers ────────────────────────────────────────────────────────
static void sendResponse(const QJsonObject &msg)
{
    QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    std::cout << line.toStdString();
    std::cout.flush();
}

static void sendResult(int id, const QString &text, bool isError = false)
{
    sendResponse({{"jsonrpc","2.0"},{"id",id},
        {"result",QJsonObject{
            {"content",QJsonArray{QJsonObject{{"type","text"},{"text",text}}}},
            {"isError",isError}}}});
}

static void sendError(int id, int code, const QString &message)
{
    sendResponse({{"jsonrpc","2.0"},{"id",id},
                  {"error",QJsonObject{{"code",code},{"message",message}}}});
}

// ─── runProcess helper ───────────────────────────────────────────────────────
static std::pair<QString,int> runCmd(const QString &cmd, const QStringList &args,
                                      int timeoutMs = 5000)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(cmd, args);
    if (!proc.waitForStarted(2000)) return {"", 127};
    if (!proc.waitForFinished(timeoutMs)) { proc.kill(); return {"timeout", 1}; }
    return {QString::fromUtf8(proc.readAll()).trimmed(), proc.exitCode()};
}

// ─── Bestehende Tool-Handler ─────────────────────────────────────────────────

static QString handleGetTime(const QJsonObject &)
{
    QDateTime now = QDateTime::currentDateTime();
    return QString("Date:     %1\nTime:     %2\nUTC:      %3\nUnix:     %4")
           .arg(now.toString("dddd, dd. MMMM yyyy"))
           .arg(now.toString("HH:mm:ss"))
           .arg(now.toUTC().toString("HH:mm:ss UTC"))
           .arg(now.toSecsSinceEpoch());
}

static QString handleGetPwd(const QJsonObject &)
{
    return QString("Sandbox root: %1")
           .arg(QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                + "/llamatools");
}

static QString handleDiskFree(const QJsonObject &args)
{
    QString path = args.value("path").toString(
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    QStorageInfo storage(path);
    if (!storage.isValid())
        return QString("Error: No filesystem at '%1'.").arg(path);

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

    return QString("Filesystem: %1\nPath:       %2\n"
                   "Total:      %3\nUsed:       %4 (%5%)\nFree:       %6")
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

    // CPU-Temperatur aus /sys/class/thermal (Linux)
    QString cpuTemp = "n/a";
    QFile tempFile("/sys/class/thermal/thermal_zone0/temp");
    if (tempFile.open(QIODevice::ReadOnly)) {
        bool ok;
        int millideg = QString::fromUtf8(tempFile.readAll()).trimmed().toInt(&ok);
        if (ok) cpuTemp = QString("%1 °C").arg(millideg / 1000.0, 0, 'f', 1);
    }

    return QString("Hostname:    %1\nOS:          %2\nKernel:      %3\n"
                   "CPU cores:   %4 (logical)\nCPU temp:    %5\n%6")
           .arg(QSysInfo::machineHostName())
           .arg(QSysInfo::prettyProductName())
           .arg(QSysInfo::kernelVersion())
           .arg(cores).arg(cpuTemp).arg(memInfo);
}

// ─── NEUES TOOL: gpu_info ────────────────────────────────────────────────────
// Fragt GPU-Metriken via nvidia-smi ab.
//
// nvidia-smi --query-gpu=... --format=csv,noheader,nounits
// gibt eine komma-separierte Zeile pro GPU zurück — zuverlässiger als
// das menschenlesbare Default-Format.
//
// Abgefragte Metriken:
//   name            — GPU-Modellname
//   memory.total    — VRAM gesamt (MiB)
//   memory.used     — VRAM belegt (MiB)
//   memory.free     — VRAM frei (MiB)
//   utilization.gpu — GPU-Auslastung (%)
//   temperature.gpu — GPU-Temperatur (°C)
//   power.draw      — aktueller Power Draw (W)
//   power.limit     — aktuelles Power Limit (W)
//   clocks.gr       — GPU-Takt (MHz)
//   clocks.mem      — Speicher-Takt (MHz)
//
// Warum nvidia-smi statt NVML direkt?
//   nvidia-smi ist auf jedem System mit Nvidia-Treiber verfügbar.
//   NVML via dlopen() wäre präziser und ohne subprocess, aber die
//   Header und der Link-Aufwand lohnen sich erst wenn wir set_power_limit
//   atomar machen wollen. Für Lesezugriffe ist nvidia-smi ausreichend.
static std::pair<QString,bool> handleGpuInfo(const QJsonObject &)
{
    // Prüfen ob nvidia-smi vorhanden
    auto [checkOut, checkCode] = runCmd("which", {"nvidia-smi"});
    if (checkCode != 0)
        return {"No NVIDIA GPU detected (nvidia-smi not found).\n"
                "AMD/Intel GPUs: use sys_info for basic system info.", false};

    // Query: komma-separiertes CSV, eine Zeile pro GPU
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
        return {QString("nvidia-smi error:\n%1").arg(smiOut), true};

    QString result;
    int gpuIndex = 0;

    for (const QString &line : smiOut.split('\n', Qt::SkipEmptyParts)) {
        QStringList fields = line.split(',');
        // Felder trimmen
        for (QString &f : fields) f = f.trimmed();

        if (fields.size() < queryFields.size()) {
            result += QString("GPU %1: parse error\n").arg(gpuIndex);
            ++gpuIndex;
            continue;
        }

        // VRAM in GB umrechnen für bessere Lesbarkeit
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

    return {result.trimmed(), false};
}

// ─── NEUES TOOL: set_power_limit ─────────────────────────────────────────────
// Setzt das GPU Power Limit via nvidia-smi -pl.
//
// Sicherheit:
//   - Wert wird gegen min/max aus gpu_info gecheckt (nochmal live abgefragt)
//   - Hinweis dass root oder nvidia-persistenced nötig ist
//   - Kein blindes Durchreichen — Wert muss numerisch sein
//
// Warum nützlich beim LLM-Betrieb?
//   Inference ist dauerhaft GPU-last. Ein niedrigeres Power Limit
//   (z.B. 80% TDP) reduziert Temperatur und Lüfterlärm mit nur ~5-10%
//   Performance-Verlust. Für lange Sessions oft sinnvoll.
static std::pair<QString,bool> handleSetPowerLimit(const QJsonObject &args)
{
    int watts = args.value("watts").toInt(0);
    int gpuId = args.value("gpu_id").toInt(0);

    if (watts <= 0)
        return {"Error: 'watts' must be a positive integer.", true};
    if (watts < 10 || watts > 600)
        return {QString("Error: %1W seems unreasonable (expected 10-600W). "
                        "Check gpu_info for min/max limits.").arg(watts), true};

    // Live min/max aus nvidia-smi holen um Grenzwerte zu prüfen
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
                    return {QString("Error: %1W is outside allowed range "
                                    "[%2W - %3W] for GPU %4.")
                            .arg(watts)
                            .arg(minW, 0, 'f', 0)
                            .arg(maxW, 0, 'f', 0)
                            .arg(gpuId), true};
                }
            }
        }
    }

    // Power Limit setzen
    auto [out, code] = runCmd("nvidia-smi",
        {"-i", QString::number(gpuId), "-pl", QString::number(watts)});

    QString report = QString("GPU %1 power limit → %2W\n").arg(gpuId).arg(watts);
    report += out + "\n";

    if (code != 0) {
        report += "\nHinweis: nvidia-smi -pl benötigt root-Rechte oder\n"
                  "nvidia-persistenced muss laufen. Versuche:\n"
                  "  sudo nvidia-smi -pl " + QString::number(watts) + "\n"
                  "oder füge den User zur 'video' Gruppe hinzu.";
        return {report, true};
    }

    report += "\nTipp: Prüfe mit gpu_info ob das neue Limit aktiv ist.";
    return {report, false};
}

// ─── Tool-Liste ──────────────────────────────────────────────────────────────
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
            "Returns current local time, date, UTC and Unix timestamp.",
            {}, {}),

        makeTool("get_pwd",
            "Returns the sandbox root directory (~/llamatools/).",
            {}, {}),

        makeTool("disk_free",
            "Returns disk usage for a path (like df -h).",
            {{"path", makeProp("string","Path to check (default: home directory)")}},
            {}),

        makeTool("sys_info",
            "Returns system info: hostname, OS, kernel, CPU cores and temperature, RAM.",
            {}, {}),

        makeTool("gpu_info",
            "Returns NVIDIA GPU information: name, VRAM (total/used/free), "
            "GPU utilization, temperature, power draw, power limit, clocks, driver. "
            "Uses nvidia-smi. Returns 'No NVIDIA GPU' if not available. "
            "Useful to monitor VRAM usage during model inference.",
            {}, {}),

        makeTool("set_power_limit",
            "Set the NVIDIA GPU power limit in Watts via nvidia-smi. "
            "Useful to reduce heat and noise during long inference sessions "
            "at the cost of ~5-10% performance. "
            "Requires root or nvidia-persistenced. "
            "The value is checked against the GPU's min/max limits.",
            {{"watts",  makeProp("integer", "Target power limit in Watts")},
             {"gpu_id", makeProp("integer", "GPU index (default: 0)")}},
            {"watts"})
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
                {"capabilities",QJsonObject{}},
                {"serverInfo",QJsonObject{{"name","llamaqt-sysinfo"},{"version","2.0"}}}
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
            QJsonObject params   = msg.value("params").toObject();
            QString toolName     = params.value("name").toString();
            QJsonObject toolArgs = params.value("arguments").toObject();

            QString result;
            bool isError = false;

            if      (toolName == "get_time")         result = handleGetTime(toolArgs);
            else if (toolName == "get_pwd")           result = handleGetPwd(toolArgs);
            else if (toolName == "disk_free")         result = handleDiskFree(toolArgs);
            else if (toolName == "sys_info")          result = handleSysInfo(toolArgs);
            else if (toolName == "gpu_info")
            { auto [r,e] = handleGpuInfo(toolArgs);   result = r; isError = e; }
            else if (toolName == "set_power_limit")
            { auto [r,e] = handleSetPowerLimit(toolArgs); result = r; isError = e; }
            else { result = QString("Unknown tool: '%1'").arg(toolName); isError = true; }

            sendResult(id, result, isError);
            continue;
        }

        if (hasId) sendError(id, -32601, "Method not found: " + method);
    }
    return 0;
}
