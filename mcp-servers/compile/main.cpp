// ─── LlamaQt MCP Compile Server ──────────────────────────────────────────────
// Builds CMake projects and validates/runs binaries inside the sandbox.
//
// Security:
//   - All paths are resolved relative to ~/llamatools/ (sandbox root)
//   - Symlinks are rejected at every path check (sandbox escape prevention)
//   - check_run only executes binaries inside the sandbox
//
// Advertised Tools:
//   cmake_build  — configure and build a CMake project
//   pkg_status   — check installed development packages
//   check_run    — verify and optionally test-run a sandbox binary

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

// ─── Sandbox ─────────────────────────────────────────────────────────────────
static QString sandboxRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
           + "/llamatools";
}

// ─── Symlink check ───────────────────────────────────────────────────────────
// Walk every path component and reject if any is a symlink.
// A directory symlink at any level could redirect the entire subtree
// outside the sandbox (e.g. ln -s /etc ~/llamatools/etc).
static bool isSymlinkInPath(const QString &absolutePath)
{
    QStringList parts = absolutePath.split('/', Qt::SkipEmptyParts);
    QString current;
    for (const QString &part : parts) {
        current += "/" + part;
        if (QFileInfo(current).isSymLink())
            return true;
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
        reason = QString("Symlink detected — not allowed (sandbox escape risk): %1").arg(path);
        return false;
    }
    return true;
}

// ─── JSON-RPC helpers ────────────────────────────────────────────────────────
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

// ─── runProcess ──────────────────────────────────────────────────────────────
// Run an external process, capturing merged stdout+stderr.
// Returns {output, failed}.
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

// ─── Tool handlers ────────────────────────────────────────────────────────────

// cmake_build: configure and build a CMake project inside the sandbox.
//
// Paths (build_dir, source_dir) are relative to sandbox root unless absolute.
// Absolute paths must still be inside the sandbox.
// Symlinks are rejected.
static std::pair<QString,bool> handleCmakeBuild(const QJsonObject &args)
{
    QString buildDir  = args.value("build_dir").toString("build");
    QString sourceDir = args.value("source_dir").toString(".");
    QString compiler  = args.value("compiler").toString();

    // Resolve relative paths against sandbox root
    if (!buildDir.startsWith('/'))  buildDir  = sandboxRoot() + "/" + buildDir;
    if (!sourceDir.startsWith('/')) sourceDir = sandboxRoot() + "/" + sourceDir;

    QString reason;
    if (!isPathAllowed(buildDir, reason))   return {reason, true};
    if (!isPathAllowed(sourceDir, reason))  return {reason, true};

    QDir().mkpath(buildDir);

    // Clear CMakeCache when switching compilers to avoid stale config
    if (!compiler.isEmpty()) {
        QString cache = buildDir + "/CMakeCache.txt";
        if (QFile::exists(cache)) QFile::remove(cache);
    }

    // Step 1: cmake configure
    QStringList cmakeArgs = {sourceDir, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
    if (compiler == "clang++")
        cmakeArgs << "-DCMAKE_CXX_COMPILER=clang++" << "-DCMAKE_C_COMPILER=clang";
    else if (compiler == "g++")
        cmakeArgs << "-DCMAKE_CXX_COMPILER=g++" << "-DCMAKE_C_COMPILER=gcc";

    auto [confOut, confFailed] = runProcess("cmake", cmakeArgs, buildDir);
    if (confFailed)
        return {"CMake configuration failed:\n" + confOut, true};

    // Step 2: cmake --build
    int cores = QThread::idealThreadCount();
    auto [buildOut, buildFailed] = runProcess(
        "cmake", {"--build", ".", "-j", QString::number(cores)}, buildDir);

    QString summary = QString("Build dir: %1\nCores: %2\n\n%3")
                      .arg(buildDir).arg(cores).arg(buildOut);
    return {summary, buildFailed};
}

// pkg_status: list installed development packages.
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

// check_run: verify a sandbox binary and optionally run it.
//
// Path resolution:
//   - 'binary' is relative to sandbox root (~/llamatools/)
//   - Absolute paths are accepted if inside the sandbox
//   - Symlinks are rejected
//
// With danger_zone:true a timed test run is performed.
// Reports: exit code, actual runtime (ms), timeout limit, stdout, stderr.
static std::pair<QString,bool> handleCheckRun(const QJsonObject &args)
{
    QString binary     = args.value("binary").toString();
    bool    dangerZone = args.value("danger_zone").toBool(false);
    int     timeoutMs  = args.value("timeout_ms").toInt(5000);

    if (binary.isEmpty())
        return {"Error: 'binary' is required.", true};

    // Resolve path: relative → sandbox root (no automatic /build/ prefix)
    QString fullPath = binary.startsWith('/')
                       ? binary
                       : sandboxRoot() + "/" + binary;

    QString reason;
    if (!isPathAllowed(fullPath, reason))
        return {reason, true};

    QFileInfo fi(fullPath);
    if (!fi.exists())
        return {QString("Error: binary not found: %1\n"
                        "(resolved to: %2)").arg(binary, fullPath), true};
    if (!fi.isExecutable())
        return {QString("Error: not executable: %1").arg(fullPath), true};

    QString info = QString("Binary:   %1\nSize:     %2 bytes\n")
                   .arg(fullPath).arg(fi.size());

    if (!dangerZone) {
        info += QString("Status:   present and executable\n"
                        "(set danger_zone:true to run it)");
        return {info, false};
    }

    // ─── Timed test run ───────────────────────────────────────────────────
    // Run the binary with separate stdout/stderr capture.
    // QElapsedTimer measures actual wall-clock runtime in milliseconds.
    //
    // Outcome A: binary exits before timeout
    //   → report exit code (0 = success, != 0 = failure), runtime, output
    //
    // Outcome B: binary still running after timeout
    //   → send SIGTERM, wait 1s, then SIGKILL if needed
    //   → report "still running after Nms" as success indicator
    QProcess proc;
    proc.setProgram(fullPath);
    // Keep stdout and stderr separate so the model can distinguish them
    proc.setProcessChannelMode(QProcess::SeparateChannels);

    QElapsedTimer timer;
    timer.start();
    proc.start();

    if (!proc.waitForStarted(3000)) {
        return {info + "Error: could not start process.", true};
    }

    bool exitedInTime = proc.waitForFinished(timeoutMs);
    qint64 elapsedMs  = timer.elapsed();

    QString stdoutStr = QString::fromUtf8(proc.readAllStandardOutput());
    QString stderrStr = QString::fromUtf8(proc.readAllStandardError());

    // Truncate very long output so the model context is not flooded
    auto truncate = [](const QString &s, int maxChars) -> QString {
        if (s.length() <= maxChars) return s;
        return s.left(maxChars) + QString("\n[... truncated at %1 chars]").arg(maxChars);
    };
    stdoutStr = truncate(stdoutStr, 2000);
    stderrStr = truncate(stderrStr, 2000);

    QString report = info;
    report += QString("Timeout limit: %1 ms\n").arg(timeoutMs);
    report += QString("Actual runtime: %1 ms\n").arg(elapsedMs);

    if (exitedInTime) {
        int exitCode = proc.exitCode();
        report += QString("Exit code: %1 (%2)\n")
                  .arg(exitCode)
                  .arg(exitCode == 0 ? "success" : "failure");
        if (!stdoutStr.isEmpty())
            report += QString("\n--- stdout ---\n%1").arg(stdoutStr);
        if (!stderrStr.isEmpty())
            report += QString("\n--- stderr ---\n%1").arg(stderrStr);

        bool failed = (exitCode != 0);
        return {report, failed};

    } else {
        // Binary did not exit within the timeout — terminate it
        proc.terminate();
        if (!proc.waitForFinished(1000))
            proc.kill();

        report += QString("Result: still running after %1 ms — terminated\n").arg(timeoutMs);
        report += "(This is normal for interactive or server processes)\n";
        if (!stdoutStr.isEmpty())
            report += QString("\n--- stdout (partial) ---\n%1").arg(stdoutStr);
        if (!stderrStr.isEmpty())
            report += QString("\n--- stderr (partial) ---\n%1").arg(stderrStr);

        return {report, false};  // "still running" is not an error
    }
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
        makeTool("cmake_build",
            "Configure and build a CMake project inside the sandbox (~/llamatools/). "
            "Relative paths are resolved against the sandbox root. "
            "Returns build output. Symlinks are rejected.",
            {{"build_dir",  makeProp("string",  "Build directory relative to sandbox root (default: build)")},
             {"source_dir", makeProp("string",  "Source directory containing CMakeLists.txt (default: .)")},
             {"compiler",   makeProp("string",  "Compiler to use: g++ or clang++ (optional)")}},
            {}),

        makeTool("pkg_status",
            "List installed development packages: cmake, g++, clang, qt6-base-dev, etc. "
            "Useful to check the build environment before attempting a build.",
            {}, {}),

        makeTool("check_run",
            "Verify that a sandbox binary exists and is executable. "
            "With danger_zone:true, runs the binary for up to timeout_ms milliseconds "
            "and reports: exit code, actual runtime, stdout and stderr. "
            "Path is relative to sandbox root (~/llamatools/) — no automatic /build/ prefix. "
            "Example: 'build/MyApp' resolves to ~/llamatools/build/MyApp. "
            "Symlinks are rejected.",
            {{"binary",      makeProp("string",  "Binary path relative to sandbox root")},
             {"danger_zone", makeProp("boolean", "true = run the binary (default: false)")},
             {"timeout_ms",  makeProp("integer", "Max run time in ms (default: 5000)")}},
            {"binary"})
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
                {"capabilities",   QJsonObject{}},
                {"serverInfo",     QJsonObject{{"name","llamaqt-compile"},{"version","1.1"}}}
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

            std::pair<QString,bool> result;
            if      (toolName == "cmake_build") result = handleCmakeBuild(toolArgs);
            else if (toolName == "pkg_status")  result = handlePkgStatus(toolArgs);
            else if (toolName == "check_run")   result = handleCheckRun(toolArgs);
            else result = {QString("Unknown tool: '%1'").arg(toolName), true};

            sendResult(id, result.first, result.second);
            continue;
        }

        if (hasId) sendError(id, -32601, "Method not found: " + method);
    }
    return 0;
}
