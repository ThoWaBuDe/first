#pragma once
// ─── WorkspaceToolBase ────────────────────────────────────────────────────────
// Zwischenschicht zwischen ToolBase und den konkreten Workspace-Tools.
//
// Der llamaqt-workspace Server läuft als User 'llamaqt' im Incus-Container.
// Er hat Zugriff auf:
//   - /home/llamaqt/workplace/  (Sandbox, read+write)
//   - sudo apt-get              (via sudoers NOPASSWD)
//   - pip install --user        (ohne sudo)
//   - bash/python3              (direkte Ausführung)
//   - HTTP/HTTPS                (Qt6::Network)
//
// Warum ein eigener Server statt Erweiterung der bestehenden?
//   Die bestehenden Server (filesystem, sysinfo, compile) laufen auch lokal.
//   Workspace-Tools sind explizit Container-only — bash_exec auf dem Host
//   wäre eine massive Sicherheitslücke.

#include "../common/ToolBase.h"
#include "../common/PathPolicy.h"

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QDir>
#include <QStandardPaths>
#include <QPair>
#include <QElapsedTimer>

// ─── Hilfsfunktion: Prozess starten und Ausgabe lesen ─────────────────────────
// Gibt {stdout+stderr, exitCode} zurück.
// timeoutMs: wie lange wir maximal warten.
static QPair<QString, int> runWorkspaceCmd(
    const QString &program,
    const QStringList &args,
    const QString &workDir = {},
    int timeoutMs = 30000,
    const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment())
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    if (!workDir.isEmpty()) proc.setWorkingDirectory(workDir);
    proc.setProcessEnvironment(env);
    proc.start(program, args);

    if (!proc.waitForStarted(5000))
        return {QString("Fehler: '%1' konnte nicht gestartet werden.").arg(program), 1};

    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {QString("Fehler: Timeout nach %1s.\n%2")
                .arg(timeoutMs / 1000)
                .arg(QString::fromUtf8(proc.readAll())), 1};
    }

    QString output = QString::fromUtf8(proc.readAll());
    return {output.isEmpty() ? "(keine Ausgabe)" : output, proc.exitCode()};
}

// ─── Blacklist für bash_exec ──────────────────────────────────────────────────
// Verhindert irreversibel destruktive Kommandos.
// Der Container ist die eigentliche Sicherheitsgrenze — diese Liste
// schützt nur gegen offensichtliche Selbstzerstörung.
static bool isBlacklisted(const QString &cmd)
{
    static const QStringList patterns = {
        "rm -rf /",
        "rm -rf /*",
        "mkfs",
        "dd if=",
        "> /dev/sd",
        ":(){ :|:& };:",  // fork bomb
        "chmod -R 000 /",
        "chown -R",
    };
    for (const QString &p : patterns) {
        if (cmd.contains(p)) return true;
    }
    return false;
}

// ─── WorkspaceToolBase ────────────────────────────────────────────────────────
class WorkspaceToolBase : public ToolBase
{
public:
    explicit WorkspaceToolBase(PathPolicy *policy = nullptr)
        : m_policy(policy)
    {}

protected:
    PathPolicy *m_policy = nullptr;

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

    // Workplace-Verzeichnis des llamaqt-Users
    static QString workplacePath()
    {
        // Im Container läuft dieser Prozess als 'llamaqt'
        // HOME ist /home/llamaqt
        const QString home = qEnvironmentVariable("HOME", "/home/llamaqt");
        return home + "/workplace";
    }
};
