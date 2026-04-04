// ─── LlamaQt MCP Build & Toolchain Server ──────────────────────────────────
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QDir>
#include <QStandardPaths>
#include <QThread>
#include <QFileInfo>
#include <iostream>
#include <string>

static QString sandboxRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/llamatools";
}

// Hilfsfunktion für Systembefehle
std::pair<QString, bool> runStep(const QString &cmd, const QStringList &args, const QString &wd = sandboxRoot()) {
    QProcess proc;
    proc.setWorkingDirectory(wd);
    proc.start(cmd, args);
    if (!proc.waitForFinished(60000)) {
        proc.kill();
        return {"Fehler: Timeout nach 60s.", true};
    }
    QString output = proc.readAllStandardOutput() + proc.readAllStandardError();
    return {output, proc.exitCode() != 0};
}

// --- Tool Handler ---

std::pair<QString, bool> handleCMake(const QJsonObject &args) {
    QString buildDir = sandboxRoot() + "/" + args.value("build_dir").toString("build");
    QString compiler = args.value("compiler").toString(""); // "g++" oder "clang++"
    QDir().mkpath(buildDir);

    if (!compiler.isEmpty()) QFile::remove(buildDir + "/CMakeCache.txt");

    QStringList cArgs = {"..", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
    if (compiler == "clang++") cArgs << "-DCMAKE_CXX_COMPILER=clang++" << "-DCMAKE_C_COMPILER=clang";
    else if (compiler == "g++") cArgs << "-DCMAKE_CXX_COMPILER=g++" << "-DCMAKE_C_COMPILER=gcc";

    auto conf = runStep("cmake", cArgs, buildDir);
    if (conf.second) return {"CMake Config Fehler:\n" + conf.first, true};

    return runStep("cmake", {"--build", ".", "-j", QString::number(QThread::idealThreadCount())}, buildDir);
}

std::pair<QString, bool> handlePkgStatus(const QJsonObject &) {
    return runStep("dpkg-query", {"-W", "*-dev", "cmake", "g++", "clang", "libgl1-mesa-dev"});
}

std::pair<QString, bool> handleCheckRun(const QJsonObject &args) {
    QString bin = args.value("binary").toString("StarAnimation");
    QString fullPath = sandboxRoot() + "/build/" + bin;
    QFileInfo fi(fullPath);

    if (!fi.exists() || !fi.isExecutable()) return {"Binary nicht gefunden oder nicht ausführbar.", true};
    if (!args.value("danger_zone").toBool(false)) return {"Check: Binary vorhanden. (Ausführung deaktiviert)", false};

    QProcess p;
    p.start(fullPath);
    if (!p.waitForStarted()) return {"Start fehlgeschlagen.", true};
    QThread::msleep(3000);
    if (p.state() == QProcess::Running) { p.terminate(); return {"Läuft stabil (3s Test).", false}; }
    return {"Crash beim Start. Exit: " + QString::number(p.exitCode()), true};
}

// --- JSON-RPC Boilerplate ---
void sendMsg(const QJsonObject &obj) {
    std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString() << std::endl;
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QJsonArray tools;
    tools.append(QJsonObject{{"name","cmake_build"},{"description","Baut Projekt. Args: build_dir, compiler (g++/clang++)"}});
    tools.append(QJsonObject{{"name","pkg_status"},{"description","Prüft installierte Header-Pakete."}});
    tools.append(QJsonObject{{"name","check_run"},{"description","Validiert Binary. danger_zone:true für 3s Testlauf."}});

    std::string line;
    while (std::getline(std::cin, line)) {
        QJsonObject msg = QJsonDocument::fromJson(QString::fromStdString(line).toUtf8()).object();
        int id = msg.value("id").toInt();
        QString method = msg.value("method").toString();

        if (method == "initialize") sendMsg({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{{"protocolVersion","2024-11-05"}}}});
        else if (method == "tools/list") sendMsg({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{{"tools",tools}}}});
        else if (method == "tools/call") {
            QJsonObject p = msg.value("params").toObject();
            QString n = p.value("name").toString();
            QJsonObject a = p.value("arguments").toObject();
            std::pair<QString,bool> r;
            if (n == "cmake_build") r = handleCMake(a);
            else if (n == "pkg_status") r = handlePkgStatus(a);
            else if (n == "check_run") r = handleCheckRun(a);
            
            QJsonObject res; res["content"] = QJsonArray{QJsonObject{{"type","text"},{"text",r.first}}};
            res["isError"] = r.second;
            sendMsg({{"jsonrpc","2.0"},{"id",id},{"result",res}});
        }
    }
    return 0;
}
