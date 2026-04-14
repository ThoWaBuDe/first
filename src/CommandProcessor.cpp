#include "CommandProcessor.h"

CommandProcessor::CommandProcessor(QObject *parent)
    : QObject(parent)
{}

bool CommandProcessor::isCommand(const QString &text)
{
    return text.startsWith('/');
}

// ─── process ─────────────────────────────────────────────────────────────────
// Pattern: Command Dispatcher — String → Handler-Methode.
CommandProcessor::ProcessResult CommandProcessor::process(const QString &text,
                                                          bool agentBusy)
{
    if (!isCommand(text))
        return {};

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
    if (cmd == "plan")      return handlePlan(args);       // NEU
    if (cmd == "summarize") return handleSummarize(args);
    if (cmd == "undo")      return handleUndo(args);
    if (cmd == "diff")      return handleDiff(args);

    ProcessResult r;
    r.handled        = true;
    r.notice         = QString("Unbekanntes Kommando: /%1\n%2").arg(cmd, helpText());
    r.noticeCssClass = "error";
    return r;
}

// ─── helpText ────────────────────────────────────────────────────────────────
QString CommandProcessor::helpText()
{
    return
        "Verfügbare Kommandos:\n"
        "  /init [Projektname]  — Projekt + Git-Repo initialisieren\n"
        "  /build               — cmake configure + make\n"
        "  /compile             — nur make\n"
        "  /run                 — cmake + make + Binary starten\n"
        "  /plan <Auftrag>      — Projekt analysieren + Aufgabenplan erstellen\n"
        "  /summarize           — Konversation manuell zusammenfassen\n"
        "  /undo [Datei]        — letzten git-commit rückgängig\n"
        "  /diff                — git diff anzeigen";
}

// ─── handlePlan ──────────────────────────────────────────────────────────────
// Startet den Plan-Modus.
//
// Der Auftrag wird als "__PLAN__:<text>" Marker zurückgegeben.
// Agent::onUserMessage() erkennt diesen Marker und wechselt in AgentMode::Plan.
//
// Warum ein Marker statt einem eigenen Signal?
//   CommandProcessor kennt den Agent nicht (Dependency-Inversion).
//   Der Marker ist ein einfaches Protokoll über den bestehenden Kanal —
//   analog zu "__SUMMARIZE__" das bereits funktioniert.
//
// Ohne Auftrag: Fehlermeldung (Plan braucht ein Ziel).
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
    // Sondermarker — Agent::onUserMessage() wertet ihn aus
    r.prompt         = "__PLAN__:" + auftrag;
    r.notice         = QString("→ /plan: %1").arg(auftrag);
    r.noticeCssClass = "system";
    return r;
}

// ─── handleInit ──────────────────────────────────────────────────────────────
CommandProcessor::ProcessResult CommandProcessor::handleInit(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;

    QString projectName = args.isEmpty() ? "MeinProjekt" : args.first();
    m_currentProject    = projectName;

    r.prompt = QString(
        "Initialisiere ein neues C++/Qt6 Projekt namens '%1' in der Sandbox.\n\n"
        "Führe folgende Schritte der Reihe nach aus:\n\n"
        "1. Prüfe ob '%1/' existiert (list_dir root). Falls nicht: mkdir '%1'.\n\n"
        "2. Ermittle das aktuelle Datum via get_time.\n\n"
        "3. Lege '%1/.gitignore' an (write_file) mit Inhalt:\n"
        "   build/\n*.o\n*.a\n*.so\n*.user\n.DS_Store\n\n"
        "4. Lege '%1/AGENT.md' an (write_file) mit:\n"
        "   # %1 — AGENT.md\n"
        "   Erstellt: [Datum aus Schritt 2]\n"
        "   ## Beschreibung\n(kurz)\n"
        "   ## Verzeichnisstruktur\n(leer)\n"
        "   ## Build\ncmake + make im build/-Verzeichnis\n"
        "   ## TODOs\n- [ ] Projekt befüllen\n\n"
        "5. Lege '%1/CMakeLists.txt' an falls nicht vorhanden:\n"
        "   cmake_minimum_required(VERSION 3.16)\n"
        "   project(%1)\n"
        "   set(CMAKE_CXX_STANDARD 17)\n"
        "   find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)\n"
        "   add_executable(%1 src/main.cpp)\n"
        "   target_link_libraries(%1 PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)\n\n"
        "6. Lege '%1/src/' an (mkdir).\n\n"
        "7. Zeige git_status und git_log um zu bestätigen dass das Repo läuft.\n\n"
        "8. Bestätige mit einer Zusammenfassung was angelegt wurde.\n\n"
        "Sandbox-Root: ~/llamatools/ | Ziel: ~/llamatools/%1/\n"
    ).arg(projectName);

    r.notice         = QString("→ /init %1").arg(projectName);
    r.noticeCssClass = "system";
    return r;
}

// ─── handleBuild ─────────────────────────────────────────────────────────────
CommandProcessor::ProcessResult CommandProcessor::handleBuild(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled = true;
    QString project = m_currentProject.isEmpty() ? "." : m_currentProject;
    r.prompt = QString(
        "Führe einen vollständigen Build des Projekts durch.\n\n"
        "Schritte:\n"
        "1. Prüfe ob '%1/CMakeLists.txt' existiert.\n"
        "2. Führe cmake_build aus:\n"
        "   - source_dir: '%1'\n"
        "   - build_dir:  '%1/build'\n"
        "3. Berichte das Ergebnis: Erfolg oder Fehler mit Ursache.\n"
        "4. Bei Fehler: Analysiere die Ausgabe und schlage eine Lösung vor.\n"
    ).arg(project);
    r.notice         = "→ /build";
    r.noticeCssClass = "system";
    return r;
}

// ─── handleCompile ───────────────────────────────────────────────────────────
CommandProcessor::ProcessResult CommandProcessor::handleCompile(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled = true;
    QString project = m_currentProject.isEmpty() ? "." : m_currentProject;
    r.prompt = QString(
        "Kompiliere das Projekt (nur make, kein cmake Re-Configure).\n\n"
        "Schritte:\n"
        "1. Prüfe ob '%1/build/CMakeCache.txt' existiert.\n"
        "   Falls nicht: Weise darauf hin dass zuerst /build nötig ist.\n"
        "2. Falls ja: Führe cmake_build aus mit:\n"
        "   - source_dir: '%1'\n"
        "   - build_dir:  '%1/build'\n"
        "3. Berichte Erfolg oder Fehler.\n"
    ).arg(project);
    r.notice         = "→ /compile";
    r.noticeCssClass = "system";
    return r;
}

// ─── handleRun ───────────────────────────────────────────────────────────────
CommandProcessor::ProcessResult CommandProcessor::handleRun(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled = true;
    QString project = m_currentProject.isEmpty() ? "." : m_currentProject;
    r.prompt = QString(
        "Baue und starte das Projekt.\n\n"
        "Schritte:\n"
        "1. Prüfe ob '%1/build/CMakeCache.txt' existiert.\n"
        "   Falls nicht: cmake_build mit source_dir '%1', build_dir '%1/build'.\n"
        "2. Falls cmake_build nötig war: cmake_build erneut.\n"
        "3. Suche das gebaute Binary: list_dir '%1/build/'.\n"
        "4. Starte das Binary mit check_run:\n"
        "   - binary: '%1/build/[BinaryName]'\n"
        "   - danger_zone: true\n"
        "   - timeout_ms: 8000\n"
        "5. Berichte Exit-Code, Laufzeit, stdout und stderr.\n"
    ).arg(project);
    r.notice         = "→ /run";
    r.noticeCssClass = "system";
    return r;
}

// ─── handleSummarize ─────────────────────────────────────────────────────────
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

// ─── handleUndo ──────────────────────────────────────────────────────────────
CommandProcessor::ProcessResult CommandProcessor::handleUndo(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;
    QString file = args.isEmpty() ? "" : args.join(' ');

    if (file.isEmpty()) {
        r.prompt = QString(
            "Mache die letzte Änderung in der Sandbox rückgängig.\n\n"
            "Schritte:\n"
            "1. Zeige git_log (n=5).\n"
            "2. Führe git_checkout aus mit ref='HEAD~1'.\n"
            "3. Zeige git_status nach dem Checkout.\n"
            "4. Berichte was zurückgesetzt wurde.\n"
        );
    } else {
        r.prompt = QString(
            "Mache die letzte Änderung an '%1' rückgängig.\n\n"
            "Schritte:\n"
            "1. Zeige git_log (n=3).\n"
            "2. Führe git_checkout aus: ref='HEAD~1', file='%1'.\n"
            "3. Zeige git_diff.\n"
            "4. Berichte das Ergebnis.\n"
        ).arg(file);
    }

    r.notice         = QString("→ /undo%1").arg(file.isEmpty() ? "" : " " + file);
    r.noticeCssClass = "system";
    return r;
}

// ─── handleDiff ──────────────────────────────────────────────────────────────
CommandProcessor::ProcessResult CommandProcessor::handleDiff(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;
    QString file = args.isEmpty() ? "" : args.join(' ');

    r.prompt = QString(
        "Zeige die aktuellen Änderungen in der Sandbox%1.\n\n"
        "Schritte:\n"
        "1. Führe git_status aus.\n"
        "2. Führe git_diff aus%2.\n"
        "3. Erkläre kurz was sich geändert hat.\n"
    ).arg(
        file.isEmpty() ? "" : QString(" für Datei '%1'").arg(file),
        file.isEmpty() ? "" : QString(" mit file='%1'").arg(file)
    );

    r.notice         = "→ /diff";
    r.noticeCssClass = "system";
    return r;
}
