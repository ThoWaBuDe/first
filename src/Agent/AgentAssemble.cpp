#include "Agent/AgentAssemble.h"
#include "Agent/Agent.h"
#include "Config/AppConfig.h"
#include <QDir>
#include <QFileInfo>

// ─── assembleProject ──────────────────────────────────────────────────────────
// Zielverzeichnis: ~/llamatools/[executeSandboxProject]/
// Wenn leer → Fehler mit Hinweis auf Einstellungen-Dialog.
void AgentAssemble::assembleProject()
{
    const AppConfig &cfg = AppConfig::instance();
    QString project = cfg.executeSandboxProject().trimmed();

    if (project.isEmpty()) {
        emit m_agent.appendTools(
            "<b>Assembly fehlgeschlagen:</b> Kein Sandbox-Projekt konfiguriert.<br>"
            "Bitte im Einstellungen-Dialog unter <b>⚙ Execute → Projekt</b> "
            "ein Unterverzeichnis angeben (z.B. 'MeinProjekt').",
            "error");
        return;
    }

    QString targetDir = QDir::homePath() + "/llamatools/" + project;

    emit m_agent.appendTools(
        QString("<b>Assembly gestartet:</b> Ziel: <code>%1</code>")
        .arg(targetDir.toHtmlEscaped()),
        "system");

    CodeAssembler::AssemblyResult result =
        m_agent.m_assembler.assemble(targetDir, m_agent.m_taskTree,
                                      cfg.assembleOnlyDone());

    if (result.filesWritten > 0) {
        QString html = QString(
            "<b>Assembly abgeschlossen:</b> "
            "%1 Datei(en) geschrieben, %2 übersprungen<br>")
            .arg(result.filesWritten)
            .arg(result.filesSkipped);

        html += "<small>";
        int shown = 0;
        for (const QString &path : result.writtenPaths) {
            if (shown >= 10) {
                html += QString("... und %1 weitere<br>")
                        .arg(result.writtenPaths.size() - shown);
                break;
            }
            html += QString("&nbsp;✓ <code>%1</code><br>")
                    .arg(QFileInfo(path).fileName().toHtmlEscaped());
            ++shown;
        }
        html += "</small>";

        emit m_agent.appendTools(html, "system");
        emit m_agent.appendChat(
            QString("<b>[Assembly]</b> %1 Datei(en) in <code>%2</code> geschrieben.")
            .arg(result.filesWritten).arg(targetDir.toHtmlEscaped()),
            "system");
    } else {
        emit m_agent.appendTools(
            QString("<b>Assembly:</b> Keine Dateien geschrieben "
                    "(%1 übersprungen).<br>"
                    "<small>Haben alle Nodes einen result-Inhalt?</small>")
            .arg(result.filesSkipped),
            "system");
    }

    for (const QString &err : result.errors) {
        emit m_agent.appendTools(
            QString("<b>Assembly-Fehler:</b> %1").arg(err.toHtmlEscaped()),
            "error");
    }

    emit m_agent.statusChanged(
        result.success() ? "Assembly abgeschlossen" : "Assembly mit Fehlern");
}
