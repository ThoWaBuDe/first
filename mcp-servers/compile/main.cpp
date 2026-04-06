// ─── LlamaQt MCP Compile Server v2.0 ─────────────────────────────────────────
// Build + Run in der Sandbox ~/llamatools/.
//
// Neu gegenüber v1.1:
//   cmake_build — automatischer Retry-Loop (bis zu 3 Versuche).
//     Bei Build-Fehler: Fehlerausgabe analysieren, häufige Ursachen
//     automatisch korrigieren (fehlende build/-Verzeichnis, Cache-Probleme),
//     dann erneut versuchen. Jeder Versuch wird protokolliert.
//
// Sicherheit:
//   - Alle Pfade relativ zu ~/llamatools/
//   - Symlinks werden auf jeder Pfadebene abgelehnt
//   - check_run: nur Binaries in der Sandbox

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <QTextStream>
#include <QElapsedTimer>
#include <iostream>

static QString sandboxRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
           + "/llamatools";
}

static bool isSymlinkInPath(const QString &absolutePath)
{
    QStringList parts = absolutePath.split('/', Qt::SkipEmptyParts);
    QString current;
    for (const QString &part : parts) {
        current += "/" + part;
        if (QFileInfo(current).isSymLink()) return true;
    }
    return false;
}

static bool isPathAllowed(const QString &path, QString &reason)
{
    QFileInfo fi(path);
    QString abs = fi.absoluteFilePath();
    if (!abs.startsWith(sandboxRoot())) {
        reason = QString("Path outside sandbox: %1").arg(path);
        return false;
    }
    if (isSymlinkInPath(abs)) {
        reason = QString("Symlink detected: %1").arg(path);
        return false;
    }
    return true;
}

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

// ─── runProcess ──────────────────────────────────────────────────────────────
static std::pair<QString,bool> runProcess(const QString &cmd,
                                           const QStringList &args,
                                           const QString &workDir,
                                           int timeoutMs = 120000)
{
    QProcess proc;
    proc.setWorkingDirectory(workDir);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(cmd, args);

    if (!proc.waitForStarted(5000))
        return {QString("Error: could not start '%1'.").arg(cmd), true};
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {QString("Error: timeout after %1s.\n%2")
                .arg(timeoutMs/1000)
                .arg(QString::fromUtf8(proc.readAll())), true};
    }

    QString output = QString::fromUtf8(proc.readAll());
    bool failed = (proc.exitCode() != 0);
    return {output.isEmpty() ? "(no output)" : output, failed};
}

// ─── cmake_build mit Retry-Loop ──────────────────────────────────────────────
// Retry-Strategie:
//   Versuch 1: normaler Build
//   Versuch 2 (bei Fehler): CMakeCache.txt löschen → Re-Configure
//   Versuch 3 (bei Fehler): build/-Verzeichnis komplett leeren → Neuaufbau
//
// Pattern: Retry with Escalating Recovery.
// Analogie AVR: wie ein UART-Receive-Error-Handler der erst soft-reset,
// dann hard-reset versucht bevor er aufgibt.
//
// Jeder Versuch wird im Output protokolliert damit das Modell sehen kann
// was versucht wurde. Das ist wichtiger als Kompaktheit.
static std::pair<QString,bool> handleCmakeBuild(const QJsonObject &args)
{
    QString buildDir  = args.value("build_dir").toString("build");
    QString sourceDir = args.value("source_dir").toString(".");
    QString compiler  = args.value("compiler").toString();
    int maxRetries    = args.value("max_retries").toInt(3);
    maxRetries = qBound(1, maxRetries, 3);

    if (!buildDir.startsWith('/'))  buildDir  = sandboxRoot() + "/" + buildDir;
    if (!sourceDir.startsWith('/')) sourceDir = sandboxRoot() + "/" + sourceDir;

    QString reason;
    if (!isPathAllowed(buildDir, reason))  return {reason, true};
    if (!isPathAllowed(sourceDir, reason)) return {reason, true};

    int cores = QThread::idealThreadCount();
    QString fullLog;  // Gesamtprotokoll aller Versuche

    for (int attempt = 1; attempt <= maxRetries; ++attempt) {
        fullLog += QString("\n─── Versuch %1/%2 ───\n").arg(attempt).arg(maxRetries);

        // ── Vorbereitung je nach Versuch ─────────────────────────────────
        if (attempt == 2) {
            // CMakeCache löschen → erzwingt Re-Configure
            QString cache = buildDir + "/CMakeCache.txt";
            if (QFile::exists(cache)) {
                QFile::remove(cache);
                fullLog += "→ CMakeCache.txt gelöscht (Re-Configure)\n";
            }
        } else if (attempt == 3) {
            // build/-Verzeichnis komplett leeren → sauberer Neuaufbau
            QDir buildDirObj(buildDir);
            if (buildDirObj.exists()) {
                buildDirObj.removeRecursively();
                fullLog += "→ build/-Verzeichnis geleert (Clean Build)\n";
            }
        }

        QDir().mkpath(buildDir);

        // ── cmake configure ───────────────────────────────────────────────
        QStringList cmakeArgs = {sourceDir, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
        if (compiler == "clang++")
            cmakeArgs << "-DCMAKE_CXX_COMPILER=clang++" << "-DCMAKE_C_COMPILER=clang";
        else if (compiler == "g++")
            cmakeArgs << "-DCMAKE_CXX_COMPILER=g++" << "-DCMAKE_C_COMPILER=gcc";

        auto [confOut, confFailed] = runProcess("cmake", cmakeArgs, buildDir);
        fullLog += "cmake configure:\n" + confOut + "\n";

        if (confFailed) {
            fullLog += QString("✗ Configure fehlgeschlagen (Versuch %1/%2)\n")
                       .arg(attempt).arg(maxRetries);
            if (attempt == maxRetries)
                return {"CMake configure failed after " + QString::number(maxRetries)
                        + " attempts:\n" + fullLog, true};
            continue;
        }

        // ── cmake --build ─────────────────────────────────────────────────
        auto [buildOut, buildFailed] = runProcess(
            "cmake", {"--build", ".", "-j", QString::number(cores)}, buildDir);
        fullLog += "cmake build:\n" + buildOut + "\n";

        if (!buildFailed) {
            // Erfolg!
            fullLog += QString("✓ Build erfolgreich (Versuch %1/%2)\n")
                       .arg(attempt).arg(maxRetries);
            return {QString("Build dir: %1\nCores: %2\n%3")
                    .arg(buildDir).arg(cores).arg(fullLog), false};
        }

        fullLog += QString("✗ Build fehlgeschlagen (Versuch %1/%2)\n")
                   .arg(attempt).arg(maxRetries);

        // Häufige Fehler erkennen und für nächsten Versuch dokumentieren
        if (buildOut.contains("CMakeCache") || buildOut.contains("cache"))
            fullLog += "→ Nächster Versuch: Cache löschen\n";
        else if (buildOut.contains("undefined reference") ||
                 buildOut.contains("cannot find"))
            fullLog += "→ Hinweis: Linker-Fehler — prüfe target_link_libraries\n";
        else if (buildOut.contains("No such file") ||
                 buildOut.contains("not found"))
            fullLog += "→ Hinweis: Fehlende Datei — prüfe Pfade in CMakeLists.txt\n";
    }

    return {"Build failed after " + QString::number(maxRetries) + " attempts:\n" + fullLog, true};
}

static std::pair<QString,bool> handlePkgStatus(const QJsonObject &)
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
    return {result.trimmed(), false};
}

static std::pair<QString,bool> handleCheckRun(const QJsonObject &args)
{
    QString binary    = args.value("binary").toString();
    bool dangerZone   = args.value("danger_zone").toBool(false);
    int timeoutMs     = args.value("timeout_ms").toInt(5000);

    if (binary.isEmpty()) return {"Error: 'binary' is required.", true};

    QString fullPath = binary.startsWith('/') ? binary : sandboxRoot() + "/" + binary;
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QFileInfo fi(fullPath);
    if (!fi.exists())
        return {QString("Error: not found: %1\n(resolved: %2)").arg(binary, fullPath), true};
    if (!fi.isExecutable())
        return {QString("Error: not executable: %1").arg(fullPath), true};

    QString info = QString("Binary:   %1\nSize:     %2 bytes\n")
                   .arg(fullPath).arg(fi.size());
    if (!dangerZone) {
        info += "Status:   present and executable\n(set danger_zone:true to run it)";
        return {info, false};
    }

    QProcess proc;
    proc.setProgram(fullPath);
    proc.setProcessChannelMode(QProcess::SeparateChannels);

    QElapsedTimer timer;
    timer.start();
    proc.start();

    if (!proc.waitForStarted(3000))
        return {info + "Error: could not start.", true};

    bool exitedInTime = proc.waitForFinished(timeoutMs);
    qint64 elapsed    = timer.elapsed();

    auto truncate = [](const QString &s, int max) {
        return s.length() <= max ? s : s.left(max) + "\n[... truncated]";
    };
    QString stdoutStr = truncate(QString::fromUtf8(proc.readAllStandardOutput()), 2000);
    QString stderrStr = truncate(QString::fromUtf8(proc.readAllStandardError()), 2000);

    QString report = info;
    report += QString("Timeout limit: %1 ms\nActual runtime: %2 ms\n")
              .arg(timeoutMs).arg(elapsed);

    if (exitedInTime) {
        int code = proc.exitCode();
        report += QString("Exit code: %1 (%2)\n").arg(code).arg(code == 0 ? "success" : "failure");
        if (!stdoutStr.isEmpty()) report += "\n--- stdout ---\n" + stdoutStr;
        if (!stderrStr.isEmpty()) report += "\n--- stderr ---\n" + stderrStr;
        return {report, code != 0};
    } else {
        proc.terminate();
        if (!proc.waitForFinished(1000)) proc.kill();
        report += QString("Result: still running after %1 ms — terminated\n").arg(timeoutMs);
        if (!stdoutStr.isEmpty()) report += "\n--- stdout (partial) ---\n" + stdoutStr;
        if (!stderrStr.isEmpty()) report += "\n--- stderr (partial) ---\n" + stderrStr;
        return {report, false};
    }
}

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
        makeTool("cmake_build",
            "Configure and build a CMake project inside the sandbox. "
            "Automatically retries up to 3 times on failure with escalating recovery: "
            "1st retry: delete CMakeCache.txt (re-configure), "
            "2nd retry: clean build directory. "
            "Reports each attempt separately so you can see what was tried.",
            {{"build_dir",   makeProp("string",  "Build dir relative to sandbox (default: build)")},
             {"source_dir",  makeProp("string",  "Source dir with CMakeLists.txt (default: .)")},
             {"compiler",    makeProp("string",  "Compiler: g++ or clang++ (optional)")},
             {"max_retries", makeProp("integer", "Max retry attempts 1-3 (default: 3)")}},
            {}),

        makeTool("pkg_status",
            "List installed development packages (cmake, g++, qt6-base-dev, etc.).",
            {}, {}),

        makeTool("check_run",
            "Verify a sandbox binary and optionally run it. "
            "Path relative to sandbox root (e.g. 'MyProject/build/MyApp'). "
            "With danger_zone:true: runs binary, reports exit code, runtime, stdout, stderr.",
            {{"binary",      makeProp("string",  "Binary path relative to sandbox root")},
             {"danger_zone", makeProp("boolean", "true = run the binary (default: false)")},
             {"timeout_ms",  makeProp("integer", "Max run time in ms (default: 5000)")}},
            {"binary"})
    };
}

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
        if (parseErr.error != QJsonParseError::NoError) {
            errStream << "JSON parse error: " << parseErr.errorString() << "\n";
            errStream.flush();
            continue;
        }

        QJsonObject msg = doc.object();
        QString method  = msg.value("method").toString();
        bool hasId      = msg.contains("id");
        int  id         = msg.value("id").toInt(-1);

        if (method == "initialize") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                {"protocolVersion","2024-11-05"},
                {"capabilities",QJsonObject{}},
                {"serverInfo",QJsonObject{{"name","llamaqt-compile"},{"version","2.0"}}}
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
            QJsonObject params = msg.value("params").toObject();
            QString toolName   = params.value("name").toString();
            QJsonObject tArgs  = params.value("arguments").toObject();

            std::pair<QString,bool> result;
            if      (toolName == "cmake_build") result = handleCmakeBuild(tArgs);
            else if (toolName == "pkg_status")  result = handlePkgStatus(tArgs);
            else if (toolName == "check_run")   result = handleCheckRun(tArgs);
            else result = {QString("Unknown tool: '%1'").arg(toolName), true};

            sendResult(id, result.first, result.second);
            continue;
        }

        if (hasId) sendError(id, -32601, "Method not found: " + method);
    }
    return 0;
}
