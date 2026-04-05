// ─── LlamaQt MCP Compile Server ──────────────────────────────────────────────
// Baut CMake-Projekte und validiert Binaries aus der Sandbox.
//
// Advertised Tools:
//   cmake_build  — CMake konfigurieren + bauen
//   pkg_status   — installierte Dev-Pakete prüfen
//   check_run    — Binary auf Existenz und Startfähigkeit prüfen

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
#include <iostream>

// ─── Sandbox ─────────────────────────────────────────────────────────────────
static QString sandboxRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
           + "/llamatools";
}

// ─── JSON-RPC Hilfsfunktionen ─────────────────────────────────────────────────
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
// Führt einen externen Prozess aus und gibt stdout+stderr zurück.
// timeout_ms: maximale Wartezeit in Millisekunden.
// isError wird true wenn exitCode != 0 oder Timeout.
static std::pair<QString,bool> runProcess(const QString &cmd,
                                           const QStringList &args,
                                           const QString &workDir,
                                           int timeoutMs = 120000)
{
    QProcess proc;
    proc.setWorkingDirectory(workDir);
    proc.setProcessChannelMode(QProcess::MergedChannels); // stdout+stderr gemeinsam
    proc.start(cmd, args);

    if (!proc.waitForStarted(5000))
        return {QString("Fehler: '%1' konnte nicht gestartet werden.").arg(cmd), true};

    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {QString("Fehler: Timeout nach %1s.\n%2")
                .arg(timeoutMs/1000)
                .arg(QString::fromUtf8(proc.readAll())), true};
    }

    QString output = QString::fromUtf8(proc.readAll());
    bool failed = (proc.exitCode() != 0);
    return {output.isEmpty() ? "(keine Ausgabe)" : output, failed};
}

// ─── Tool-Handler ─────────────────────────────────────────────────────────────

// cmake_build: konfiguriert und baut ein CMake-Projekt.
//
// build_dir: Pfad zum Build-Verzeichnis (relativ zur Sandbox oder absolut).
//            Default: <sandbox>/build
// source_dir: Pfad zum Source-Verzeichnis (wo CMakeLists.txt liegt).
//             Default: <sandbox>
// compiler:   "g++" oder "clang++" — leer = CMake-Default
//
// Ablauf: cmake .. → cmake --build . -jN
static std::pair<QString,bool> handleCmakeBuild(const QJsonObject &args)
{
    // Pfade auflösen — absolut oder relativ zur Sandbox
    QString buildDir  = args.value("build_dir").toString("build");
    QString sourceDir = args.value("source_dir").toString(".");
    QString compiler  = args.value("compiler").toString();

    // Relative Pfade relativ zur Sandbox
    if (!buildDir.startsWith('/'))  buildDir  = sandboxRoot() + "/" + buildDir;
    if (!sourceDir.startsWith('/')) sourceDir = sandboxRoot() + "/" + sourceDir;

    QDir().mkpath(buildDir);

    // Bei Compiler-Wechsel CMakeCache löschen — sonst ignoriert CMake den Wechsel
    if (!compiler.isEmpty()) {
        QString cache = buildDir + "/CMakeCache.txt";
        if (QFile::exists(cache)) QFile::remove(cache);
    }

    // ─── Schritt 1: cmake konfigurieren ─────────────────────────────────
    QStringList cmakeArgs = {sourceDir, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
    if (compiler == "clang++")
        cmakeArgs << "-DCMAKE_CXX_COMPILER=clang++" << "-DCMAKE_C_COMPILER=clang";
    else if (compiler == "g++")
        cmakeArgs << "-DCMAKE_CXX_COMPILER=g++" << "-DCMAKE_C_COMPILER=gcc";

    auto [confOut, confFailed] = runProcess("cmake", cmakeArgs, buildDir);
    if (confFailed)
        return {"CMake Konfiguration fehlgeschlagen:\n" + confOut, true};

    // ─── Schritt 2: cmake --build ────────────────────────────────────────
    int cores = QThread::idealThreadCount();
    auto [buildOut, buildFailed] = runProcess(
        "cmake", {"--build", ".", "-j", QString::number(cores)}, buildDir);

    QString summary = QString("Build-Verzeichnis: %1\nCores: %2\n\n%3")
                      .arg(buildDir).arg(cores).arg(buildOut);
    return {summary, buildFailed};
}

// pkg_status: listet installierte Entwicklungspakete auf.
// Nützlich um zu prüfen ob Qt6-dev, cmake, g++ etc. vorhanden sind.
static std::pair<QString,bool> handlePkgStatus(const QJsonObject &)
{
    // dpkg-query -W gibt Name + Version für installierte Pakete zurück
    QStringList packages = {
        "cmake", "g++", "clang", "make", "ninja-build",
        "qt6-base-dev", "libgl1-mesa-dev", "pkg-config"
    };

    QString result;
    for (const QString &pkg : packages) {
        auto [out, failed] = runProcess("dpkg-query", {"-W", "-f=${Package} ${Version}\n", pkg}, "/tmp", 5000);
        result += failed
            ? QString("[ ] %1 - nicht installiert\n").arg(pkg)
            : QString("[x] %1\n").arg(out.trimmed());
    }
    return {result.trimmed(), false};
}

// check_run: prüft ob ein Binary existiert und ausführbar ist.
// Mit danger_zone:true wird ein 3-Sekunden-Testlauf durchgeführt.
// Sicherheit: Binary muss sich in sandbox/build/ befinden.
static std::pair<QString,bool> handleCheckRun(const QJsonObject &args)
{
    QString binary   = args.value("binary").toString();
    bool    dangerZone = args.value("danger_zone").toBool(false);

    if (binary.isEmpty())
        return {"Fehler: 'binary' fehlt.", true};

    // Pfad auflösen — nur innerhalb sandbox/build erlaubt
    QString fullPath = binary.startsWith('/')
                       ? binary
                       : sandboxRoot() + "/build/" + binary;

    QFileInfo fi(fullPath);
    if (!fi.absoluteFilePath().startsWith(sandboxRoot()))
        return {"Fehler: Binary ausserhalb der Sandbox.", true};

    if (!fi.exists())
        return {QString("Binary nicht gefunden: %1").arg(fullPath), true};
    if (!fi.isExecutable())
        return {QString("Binary nicht ausfuehrbar: %1").arg(fullPath), true};

    if (!dangerZone)
        return {QString("OK: Binary vorhanden und ausfuehrbar.\nPfad: %1\n"
                        "Groesse: %2 Bytes\n"
                        "(Testlauf deaktiviert - danger_zone:true zum Starten)")
                .arg(fullPath).arg(fi.size()), false};

    // ─── 3-Sekunden Testlauf ─────────────────────────────────────────────
    // QProcess::start() + 3s warten + terminate()
    // waitForFinished(1000) danach — falls terminate() nicht reicht: kill()
    QProcess proc;
    proc.start(fullPath);
    if (!proc.waitForStarted(3000))
        return {"Testlauf: Start fehlgeschlagen.", true};

    // 3 Sekunden laufen lassen
    bool stillRunning = !proc.waitForFinished(3000);

    if (stillRunning) {
        proc.terminate();
        if (!proc.waitForFinished(1000))
            proc.kill();
        return {QString("Testlauf OK: Binary lief 3s stabil.\nPfad: %1").arg(fullPath), false};
    } else {
        return {QString("Testlauf: Binary beendet nach < 3s.\n"
                        "Exit-Code: %1\n%2")
                .arg(proc.exitCode())
                .arg(QString::fromUtf8(proc.readAll())),
                proc.exitCode() != 0};
    }
}

// ─── Tool-Definitionen ───────────────────────────────────────────────────────
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
            "Konfiguriert und baut ein CMake-Projekt. "
            "Gibt Build-Ausgabe zurueck. Relative Pfade zur Sandbox.",
            {{"build_dir",  makeProp("string","Build-Verzeichnis (default: build)")},
             {"source_dir", makeProp("string","Source-Verzeichnis mit CMakeLists.txt (default: .)")},
             {"compiler",   makeProp("string","Compiler: g++ oder clang++ (optional)")}},
            {}),

        makeTool("pkg_status",
            "Listet installierte Entwicklungspakete (cmake, g++, qt6-base-dev, ...).",
            {}, {}),

        makeTool("check_run",
            "Prueft ob ein Binary existiert und ausfuehrbar ist. "
            "Mit danger_zone:true wird ein 3-Sekunden-Testlauf gemacht.",
            {{"binary",     makeProp("string","Binary-Name oder Pfad (relativ zu sandbox/build/)")},
             {"danger_zone",makeProp("boolean","true = 3s Testlauf (default: false)")}},
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
            errStream << "JSON Parse Error: " << parseErr.errorString() << "\n";
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
                {"serverInfo",     QJsonObject{{"name","llamaqt-compile"},{"version","1.0"}}}
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
            else result = {QString("Unbekanntes Tool: '%1'").arg(toolName), true};

            sendResult(id, result.first, result.second);
            continue;
        }

        if (hasId) sendError(id, -32601, "Method not found: " + method);
    }

    return 0;
}
