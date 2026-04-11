#pragma once
// ─── CmakeBuildTool ───────────────────────────────────────────────────────────

#include "CompileToolBase.h"
#include <QFile>

class CmakeBuildTool : public CompileToolBase
{
public:
    using CompileToolBase::CompileToolBase;

    QString name() const override { return "cmake_build"; }

    QString description() const override
    {
        return "Configure and build a CMake project inside the sandbox. "
               "Automatically retries up to 3 times on failure with escalating recovery: "
               "1st retry: delete CMakeCache.txt (re-configure), "
               "2nd retry: clean build directory. "
               "Reports each attempt separately so you can see what was tried.";
    }

    QJsonObject properties() const override
    {
        return {
            {"build_dir",   prop("string",  "Build dir relative to sandbox (default: build)")},
            {"source_dir",  prop("string",  "Source dir with CMakeLists.txt (default: .)")},
            {"compiler",    prop("string",  "Compiler: g++ or clang++ (optional)")},
            {"max_retries", prop("integer", "Max retry attempts 1-3 (default: 3)")}
        };
    }

    QJsonArray required() const override { return {}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString buildDir  = args.value("build_dir").toString("build");
        QString sourceDir = args.value("source_dir").toString(".");
        QString compiler  = args.value("compiler").toString();
        int maxRetries    = args.value("max_retries").toInt(3);
        maxRetries = qBound(1, maxRetries, 3);

        if (!buildDir.startsWith('/'))  buildDir  = sandboxRoot() + "/" + buildDir;
        if (!sourceDir.startsWith('/')) sourceDir = sandboxRoot() + "/" + sourceDir;

        int cores = QThread::idealThreadCount();
        QString fullLog;

        for (int attempt = 1; attempt <= maxRetries; ++attempt) {
            fullLog += QString("\n─── Versuch %1/%2 ───\n").arg(attempt).arg(maxRetries);

            if (attempt == 2) {
                QString cache = buildDir + "/CMakeCache.txt";
                if (QFile::exists(cache)) {
                    QFile::remove(cache);
                    fullLog += "→ CMakeCache.txt gelöscht (Re-Configure)\n";
                }
            } else if (attempt == 3) {
                QDir buildDirObj(buildDir);
                if (buildDirObj.exists()) {
                    buildDirObj.removeRecursively();
                    fullLog += "→ build/-Verzeichnis geleert (Clean Build)\n";
                }
            }

            QDir().mkpath(buildDir);

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
                    return ToolResult::err("CMake configure failed after "
                        + QString::number(maxRetries) + " attempts:\n" + fullLog);
                continue;
            }

            auto [buildOut, buildFailed] = runProcess(
                "cmake", {"--build", ".", "-j", QString::number(cores)}, buildDir);
            fullLog += "cmake build:\n" + buildOut + "\n";

            if (!buildFailed) {
                fullLog += QString("✓ Build erfolgreich (Versuch %1/%2)\n")
                           .arg(attempt).arg(maxRetries);
                return ToolResult::ok(QString("Build dir: %1\nCores: %2\n%3")
                        .arg(buildDir).arg(cores).arg(fullLog));
            }

            fullLog += QString("✗ Build fehlgeschlagen (Versuch %1/%2)\n")
                       .arg(attempt).arg(maxRetries);

            if (buildOut.contains("CMakeCache") || buildOut.contains("cache"))
                fullLog += "→ Nächster Versuch: Cache löschen\n";
            else if (buildOut.contains("undefined reference") ||
                     buildOut.contains("cannot find"))
                fullLog += "→ Hinweis: Linker-Fehler — prüfe target_link_libraries\n";
            else if (buildOut.contains("No such file") ||
                     buildOut.contains("not found"))
                fullLog += "→ Hinweis: Fehlende Datei — prüfe Pfade in CMakeLists.txt\n";
        }

        return ToolResult::err("Build failed after " + QString::number(maxRetries)
            + " attempts:\n" + fullLog);
    }
};