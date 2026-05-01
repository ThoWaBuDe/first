#include "CommandProcessor.h"

CommandProcessor::CommandProcessor(QObject *parent)
    : QObject(parent)
{}

bool CommandProcessor::isCommand(const QString &text)
{
    return text.startsWith('/');
}

CommandProcessor::ProcessResult CommandProcessor::process(const QString &text,
                                                           bool agentBusy)
{
    if (!isCommand(text)) return {};

    if (agentBusy) {
        ProcessResult r;
        r.handled        = true;
        r.notice         = "Agent ist beschäftigt — bitte warten.";
        r.noticeCssClass = "error";
        return r;
    }

    QStringList parts = text.mid(1).split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        ProcessResult r;
        r.handled        = true;
        r.notice         = helpText();
        r.noticeCssClass = "system";
        return r;
    }

    QString cmd  = parts.first().toLower();
    QStringList args = parts.mid(1);

    if (cmd == "init")      return handleInit(args);
    if (cmd == "build")     return handleBuild(args);
    if (cmd == "compile")   return handleCompile(args);
    if (cmd == "run")       return handleRun(args);
    if (cmd == "plan")      return handlePlan(args);
    if (cmd == "execute")   return handleExecute(args);
    if (cmd == "savedb")    return handleSaveDB(args);
    if (cmd == "loaddb")    return handleLoadDB(args);
    if (cmd == "summarize") return handleSummarize(args);
    if (cmd == "undo")      return handleUndo(args);
    if (cmd == "diff")      return handleDiff(args);
    if (cmd == "import")    return handleImport(args); // NEU

    ProcessResult r;
    r.handled        = true;
    r.notice         = QString("Unbekanntes Kommando: /%1\n%2").arg(cmd, helpText());
    r.noticeCssClass = "error";
    return r;
}

QString CommandProcessor::helpText()
{
    return
        "Verfügbare Kommandos:\n"
        "  /init [Projektname]  — Projekt + Git-Repo initialisieren\n"
        "  /build               — cmake configure + make\n"
        "  /compile             — nur make\n"
        "  /run                 — cmake + make + Binary starten\n"
        "  /plan <Auftrag>      — Projekt analysieren + Aufgabenplan erstellen\n"
        "  /execute             — Execute-Modus starten\n"
        "  /saveDB              — TaskTree + Thoughts in SQLite speichern\n"
        "  /loadDB              — TaskTree + Thoughts aus SQLite laden\n"
        "  /import <pfad>       — Quellcode in Node-Struktur importieren\n"
        "  /summarize           — Konversation manuell zusammenfassen\n"
        "  /undo [Datei]        — letzten git-commit rückgängig\n"
        "  /diff                — git diff anzeigen";
}

// ─── handleImport (NEU) ───────────────────────────────────────────────────────
// Startet den Import-Modus: Quellcode → Node-Baum.
// Funktioniert mit Einzeldateien und Verzeichnissen.
// Marker: "__IMPORT__:<pfad>" → Agent::onUserMessage() → AgentChat::handleImport()
CommandProcessor::ProcessResult CommandProcessor::handleImport(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;

    if (args.isEmpty()) {
        r.notice         = "Fehler: /import braucht einen Pfad.\n"
                           "Beispiel: /import src/TodoModel.cpp\n"
                           "          /import src/\n"
                           "          /import TodoApp/";
        r.noticeCssClass = "error";
        return r;
    }

    QString path = args.join(' ');
    r.prompt         = "__IMPORT__:" + path;
    r.notice         = QString("→ /import: %1").arg(path);
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleExecute(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled        = true;
    r.prompt         = "__EXECUTE__";
    r.notice         = "→ /execute — Execute-Modus wird gestartet...";
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleSaveDB(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled        = true;
    r.prompt         = "__SAVEDB__";
    r.notice         = "→ /saveDB — TaskTree + Thoughts werden gespeichert...";
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleLoadDB(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled        = true;
    r.prompt         = "__LOADDB__";
    r.notice         = "→ /loadDB — TaskTree + Thoughts werden geladen...";
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handlePlan(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;

    if (args.isEmpty()) {
        r.notice         = "Fehler: /plan braucht einen Auftrag.\n"
                           "Beispiel: /plan HTTP-Server mit Qt6 implementieren";
        r.noticeCssClass = "error";
        return r;
    }

    QString auftrag = args.join(' ');
    r.prompt         = "__PLAN__:" + auftrag;
    r.notice         = QString("→ /plan: %1").arg(auftrag);
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleInit(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;
    QString projectName = args.isEmpty() ? "MeinProjekt" : args.first();
    m_currentProject    = projectName;

    r.prompt = QString(
        "Initialisiere ein neues C++/Qt6 Projekt namens '%1' in der Sandbox.\n\n"
        "Schritte:\n"
        "1. Prüfe ob '%1/' existiert. Falls nicht: mkdir '%1'.\n"
        "2. Ermittle das aktuelle Datum via get_time.\n"
        "3. Lege '%1/.gitignore' an.\n"
        "4. Lege '%1/AGENT.md' an.\n"
        "5. Lege '%1/CMakeLists.txt' an falls nicht vorhanden.\n"
        "6. Lege '%1/src/' an.\n"
        "7. Zeige git_status und git_log.\n"
        "8. Bestätige mit Zusammenfassung.\n"
    ).arg(projectName);

    r.notice         = QString("→ /init %1").arg(projectName);
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleBuild(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled = true;
    QString project = m_currentProject.isEmpty() ? "." : m_currentProject;
    r.prompt = QString(
        "Führe einen vollständigen Build durch.\n\n"
        "1. Prüfe ob '%1/CMakeLists.txt' existiert.\n"
        "2. cmake_build: source_dir '%1', build_dir '%1/build'\n"
        "3. Berichte Erfolg oder Fehler.\n"
    ).arg(project);
    r.notice         = "→ /build";
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleCompile(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled = true;
    QString project = m_currentProject.isEmpty() ? "." : m_currentProject;
    r.prompt = QString(
        "Kompiliere das Projekt (nur make).\n\n"
        "1. cmake_build: source_dir '%1', build_dir '%1/build'\n"
        "2. Berichte Ergebnis.\n"
    ).arg(project);
    r.notice         = "→ /compile";
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleRun(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled = true;
    QString project = m_currentProject.isEmpty() ? "." : m_currentProject;
    r.prompt = QString(
        "Baue und starte das Projekt.\n\n"
        "1. cmake_build wenn nötig.\n"
        "2. Binary suchen: list_dir '%1/build/'.\n"
        "3. Binary starten: check_run mit danger_zone:true, timeout_ms:8000.\n"
        "4. Berichte Exit-Code, stdout, stderr.\n"
    ).arg(project);
    r.notice         = "→ /run";
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleSummarize(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled        = true;
    r.notice         = "→ /summarize — Konversation wird zusammengefasst...";
    r.noticeCssClass = "system";
    r.prompt         = "__SUMMARIZE__";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleUndo(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;
    QString file = args.isEmpty() ? "" : args.join(' ');

    if (file.isEmpty()) {
        r.prompt = "Mache die letzte Änderung rückgängig.\n\n"
                   "1. git_log (n=5)\n"
                   "2. git_checkout ref='HEAD~1'\n"
                   "3. git_status\n"
                   "4. Berichte was zurückgesetzt wurde.\n";
    } else {
        r.prompt = QString(
            "Mache letzte Änderung an '%1' rückgängig.\n\n"
            "1. git_log (n=3)\n"
            "2. git_checkout ref='HEAD~1', file='%1'\n"
            "3. git_diff\n"
            "4. Berichte Ergebnis.\n"
        ).arg(file);
    }

    r.notice         = QString("→ /undo%1").arg(file.isEmpty() ? "" : " " + file);
    r.noticeCssClass = "system";
    return r;
}

CommandProcessor::ProcessResult CommandProcessor::handleDiff(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;
    QString file = args.isEmpty() ? "" : args.join(' ');

    r.prompt = QString(
        "Zeige aktuelle Änderungen%1.\n\n"
        "1. git_status\n"
        "2. git_diff%2\n"
        "3. Erkläre kurz was sich geändert hat.\n"
    ).arg(
        file.isEmpty() ? "" : QString(" für '%1'").arg(file),
        file.isEmpty() ? "" : QString(" file='%1'").arg(file)
    );

    r.notice         = "→ /diff";
    r.noticeCssClass = "system";
    return r;
}
