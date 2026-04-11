#pragma once
// ─── GitHelper ────────────────────────────────────────────────────────────────
// Kapselt alle git-Operationen für den filesystem MCP-Server.
//
// Pattern: Facade
//   GitHelper ist eine dünne Schicht über QProcess+git. Die Tool-Klassen
//   (GitStatusTool, GitDiffTool, ...) sehen nur autoCommit(), run() und
//   repoFor() — sie wissen nichts von QProcess oder git-Kommandozeilen.
//
// Konstruktor-Injektion:
//   GitHelper bekommt PathPolicy im Konstruktor. Das primarySandbox() liefert
//   den Wurzelpfad für git-Repos. Die Tool-Klassen bekommen GitHelper* im
//   eigenen Konstruktor — Chain of Dependency Injection.
//
// Git-Repos:
//   Jedes Unterverzeichnis der Sandbox kann ein eigenes Repo sein (/init).
//   gitRepoFor(path) sucht das nächste .git-Verzeichnis aufwärts bis zur
//   Sandbox-Root. Fallback: Sandbox-Root selbst.
//
// Sicherheit:
//   Remote-Operationen (push/pull/remote/fetch/clone) sind gesperrt.
//   Das ist bewusst: der Agent soll nur lokal arbeiten.

#include <QString>
#include <QStringList>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QPair>
#include "PathPolicy.h"

class GitHelper
{
public:
    // policy wird per Referenz gehalten — GitHelper darf PathPolicy nicht
    // überleben. In main() leben beide auf dem Stack, das ist sicher.
    explicit GitHelper(const PathPolicy &policy)
        : m_policy(policy)
    {}

    // Findet das git-Repo für einen absoluten Dateipfad.
    // Sucht .git-Verzeichnis aufwärts bis zur Sandbox-Root.
    // Fallback: Sandbox-Root selbst (für neue Projekte).
    QString repoFor(const QString &absPath) const
    {
        QString sandbox = m_policy.primarySandbox();
        QDir dir(QFileInfo(absPath).isDir() ? absPath : QFileInfo(absPath).absolutePath());

        while (dir.absolutePath().startsWith(sandbox)) {
            if (QDir(dir.absolutePath() + "/.git").exists())
                return dir.absolutePath();
            if (!dir.cdUp()) break;
        }

        // Fallback: erstes Unterverzeichnis als Projekt-Root
        QString rel = absPath;
        if (rel.startsWith(sandbox + "/"))
            rel = rel.mid(sandbox.length() + 1);
        QString projectName = rel.section('/', 0, 0);
        if (!projectName.isEmpty() && !projectName.contains('.'))
            return sandbox + "/" + projectName;

        return sandbox;
    }

    // Stellt sicher dass im angegebenen Verzeichnis ein git-Repo existiert.
    // Legt user.name/user.email lokal an falls kein globales config vorhanden.
    bool ensureRepo(const QString &repoPath) const
    {
        if (QDir(repoPath + "/.git").exists()) return true;

        QDir().mkpath(repoPath);
        QProcess proc;
        proc.setWorkingDirectory(repoPath);
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start("git", {"init"});
        if (!proc.waitForStarted(3000) || !proc.waitForFinished(5000)) return false;

        auto checkGlobal = [](const QString &key) -> bool {
            QProcess p;
            p.start("git", {"config", "--global", key});
            p.waitForFinished(2000);
            return p.exitCode() == 0
                && !QString::fromUtf8(p.readAll()).trimmed().isEmpty();
        };

        if (!checkGlobal("user.name")) {
            QProcess p;
            p.setWorkingDirectory(repoPath);
            p.start("git", {"config", "user.name", "LlamaQt"});
            p.waitForFinished(2000);
        }
        if (!checkGlobal("user.email")) {
            QProcess p;
            p.setWorkingDirectory(repoPath);
            p.start("git", {"config", "user.email", "llamaqt@local"});
            p.waitForFinished(2000);
        }
        return true;
    }

    // Führt ein git-Kommando im angegebenen Repo-Verzeichnis aus.
    // Gibt {stdout+stderr, exitCode} zurück.
    // Remote-Operationen werden hier geblockt — nicht erst im Client.
    QPair<QString,int> run(const QString &repoPath, const QStringList &args) const
    {
        static const QStringList blocked = {"push","pull","remote","fetch","clone"};
        if (!args.isEmpty() && blocked.contains(args.first().toLower()))
            return {QString("Error: git %1 is not allowed.").arg(args.first()), 1};

        QProcess proc;
        proc.setWorkingDirectory(repoPath);
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start("git", args);
        if (!proc.waitForStarted(3000))
            return {"Error: could not start git.", 1};
        if (!proc.waitForFinished(15000)) {
            proc.kill();
            return {"Error: git timeout.", 1};
        }
        return {QString::fromUtf8(proc.readAll()).trimmed(), proc.exitCode()};
    }

    // Commit aller Änderungen im Repo des angegebenen Pfads.
    // Wird vor jedem schreibenden Tool-Call aufgerufen (Undo-System).
    // Tut nichts wenn es keine staged Änderungen gibt (diff --cached --quiet).
    void autoCommit(const QString &absFilePath, const QString &message) const
    {
        QString repoPath = repoFor(absFilePath);
        if (!ensureRepo(repoPath)) return;
        run(repoPath, {"add", "-A"});
        // Prüfen ob es überhaupt etwas zu committen gibt
        auto [diffOut, diffCode] = run(repoPath, {"diff", "--cached", "--quiet"});
        Q_UNUSED(diffOut)
        if (diffCode == 0) return;   // 0 = kein Unterschied → kein Commit nötig
        run(repoPath, {"commit", "-m", QString("auto: %1").arg(message)});
    }

    // Zugriff auf die Policy (für Tool-Klassen die primarySandbox() brauchen)
    const PathPolicy &policy() const { return m_policy; }

private:
    const PathPolicy &m_policy;
};
