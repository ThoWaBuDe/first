#pragma once
// ─── PipInstallTool ───────────────────────────────────────────────────────────
// Installiert Python-Pakete via pip install --user.
// Kein sudo nötig — Pakete landen in ~/.local/lib/python3.x/

#include "WorkspaceToolBase.h"

class PipInstallTool : public WorkspaceToolBase
{
public:
    using WorkspaceToolBase::WorkspaceToolBase;

    QString name() const override { return "pip_install"; }

    QString description() const override
    {
        return
            "Install Python packages using pip install --user. "
            "No sudo required — packages are installed to ~/.local/. "
            "Examples: pip_install('requests'), pip_install('beautifulsoup4 lxml'). "
            "Multiple packages can be specified space-separated.";
    }

    QJsonObject properties() const override
    {
        return {
            {"packages", prop("string", "Space-separated list of packages to install.")}
        };
    }

    QJsonArray required() const override { return req({"packages"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString packages = args.value("packages").toString().trimmed();
        if (packages.isEmpty())
            return ToolResult::err("Error: 'packages' is required.");

        // Paketnamen aufteilen und validieren (keine Shell-Injection)
        QStringList pkgList = packages.split(' ', Qt::SkipEmptyParts);
        for (const QString &pkg : pkgList) {
            // Einfache Validierung: nur alphanumerisch + - _ .
            for (const QChar &c : pkg) {
                if (!c.isLetterOrNumber() && c != '-' && c != '_'
                    && c != '.' && c != '=' && c != '>' && c != '<')
                    return ToolResult::err(
                        QString("Error: invalid package name: %1").arg(pkg));
            }
        }

        QStringList pipArgs = {"install", "--user"};
        pipArgs += pkgList;

        auto [output, exitCode] = runWorkspaceCmd(
            "/usr/bin/pip3", pipArgs, workplacePath(), 120000);

        return exitCode == 0
            ? ToolResult::ok(output)
            : ToolResult::err(output);
    }
};


// ─── AptInstallTool ───────────────────────────────────────────────────────────
// Installiert System-Pakete via sudo apt-get install.
// Erlaubt via sudoers NOPASSWD-Regel für den llamaqt-User.

class AptInstallTool : public WorkspaceToolBase
{
public:
    using WorkspaceToolBase::WorkspaceToolBase;

    QString name() const override { return "apt_install"; }

    QString description() const override
    {
        return
            "Install system packages using sudo apt-get install. "
            "Allowed without password via sudoers configuration. "
            "Examples: apt_install('git'), apt_install('curl wget'). "
            "Multiple packages can be specified space-separated. "
            "Use this for system-level dependencies that pip cannot provide.";
    }

    QJsonObject properties() const override
    {
        return {
            {"packages", prop("string", "Space-separated list of packages to install.")}
        };
    }

    QJsonArray required() const override { return req({"packages"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString packages = args.value("packages").toString().trimmed();
        if (packages.isEmpty())
            return ToolResult::err("Error: 'packages' is required.");

        QStringList pkgList = packages.split(' ', Qt::SkipEmptyParts);
        for (const QString &pkg : pkgList) {
            for (const QChar &c : pkg) {
                if (!c.isLetterOrNumber() && c != '-' && c != '_' && c != '.')
                    return ToolResult::err(
                        QString("Error: invalid package name: %1").arg(pkg));
            }
        }

        // apt-get update erst wenn nötig (optional, aber oft hilfreich)
        QStringList aptArgs = {"apt-get", "install", "-y", "-qq"};
        aptArgs += pkgList;

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("DEBIAN_FRONTEND", "noninteractive");

        auto [output, exitCode] = runWorkspaceCmd(
            "/usr/bin/sudo", aptArgs, "/tmp", 180000, env);

        return exitCode == 0
            ? ToolResult::ok(output.isEmpty()
                ? QString("OK: %1 installed.").arg(packages)
                : output)
            : ToolResult::err(output);
    }
};
