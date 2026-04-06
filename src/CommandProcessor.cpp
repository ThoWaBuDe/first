#include "CommandProcessor.h"

CommandProcessor::CommandProcessor(QObject *parent)
    : QObject(parent)
{}

// ─── isCommand ───────────────────────────────────────────────────────────────
bool CommandProcessor::isCommand(const QString &text)
{
    return text.startsWith('/');
}

// ─── process ─────────────────────────────────────────────────────────────────
// Parst den Kommando-String und dispatcht zum Handler.
// Format: "/kommando [arg1] [arg2] ..."
//
// Pattern: Command Dispatcher — String → Handler-Methode.
CommandProcessor::ProcessResult CommandProcessor::process(const QString &text,
                                                          bool agentBusy)
{
    if (!isCommand(text))
        return {};  // handled = false

    if (agentBusy) {
        ProcessResult r;
        r.handled       = true;
        r.notice        = "Agent ist beschäftigt — bitte warten.";
        r.noticeCssClass = "error";
        return r;
    }

    // Kommando aufsplitten: "/build release" → ["build", "release"]
    QStringList parts = text.mid(1).split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        ProcessResult r;
        r.handled        = true;
        r.notice         = helpText();
        r.noticeCssClass = "system";
        return r;
    }

    QString cmd = parts.first().toLower();
    QStringList args = parts.mid(1);

    if (cmd == "init")    return handleInit(args);
    if (cmd == "build")   return handleBuild(args);
    if (cmd == "compile") return handleCompile(args);
    if (cmd == "run")     return handleRun(args);

    // Unbekanntes Kommando
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
        "  /init [Projektname]  — Projekt initialisieren\n"
        "  /build               — cmake configure + make\n"
        "  /compile             — nur make\n"
        "  /run                 — cmake (falls nötig) + make + ausführen";
}

// ─── handleInit ──────────────────────────────────────────────────────────────
// Baut einen Prompt der das LLM anweist:
//   1. Verzeichnis in ~/llamatools/ anlegen (mkdir Tool)
//   2. Eine AGENT.md erstellen (write_file Tool)
//   3. CMakeLists.txt Vorlage anlegen falls nicht vorhanden
//
// Das LLM erledigt die eigentliche Arbeit via Tools — wir geben nur
// die Aufgabe vor. Das ist robuster als C++-Code der Dateien anlegt,
// weil das LLM die AGENT.md sinnvoll befüllen kann.
CommandProcessor::ProcessResult CommandProcessor::handleInit(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;

    QString projectName = args.isEmpty() ? "MeinProjekt" : args.first();
    m_currentProject    = projectName;

    r.prompt = QString(
        "Initialisiere ein neues C++/Qt6 Projekt namens '%1' in der Sandbox.\n\n"
        "Führe folgende Schritte der Reihe nach aus:\n"
        "1. Prüfe ob das Verzeichnis '%1/' bereits existiert (list_dir).\n"
        "2. Falls nicht vorhanden: Lege es mit mkdir an.\n"
        "3. Lege '%1/AGENT.md' an mit write_file. Inhalt:\n"
        "   - Projektname und kurze Beschreibung\n"
        "   - Verzeichnisstruktur (noch leer)\n"
        "   - Build-Kommandos (cmake + make)\n"
        "   - Offene TODOs\n"
        "4. Falls '%1/CMakeLists.txt' nicht existiert: Lege eine minimale\n"
        "   CMakeLists.txt Vorlage für Qt6 an.\n"
        "5. Bestätige was du angelegt hast.\n\n"
        "Projekt: %1\n"
        "Sandbox-Root: ~/llamatools/\n"
        "Zielverzeichnis: ~/llamatools/%1/\n"
    ).arg(projectName);

    r.notice         = QString("→ /init %1").arg(projectName);
    r.noticeCssClass = "system";
    return r;
}

// ─── handleBuild ─────────────────────────────────────────────────────────────
// cmake configure + make via cmake_build Tool.
CommandProcessor::ProcessResult CommandProcessor::handleBuild(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled = true;

    QString project = m_currentProject.isEmpty() ? "." : m_currentProject;

    r.prompt = QString(
        "Führe einen vollständigen Build des Projekts durch.\n\n"
        "Schritte:\n"
        "1. Prüfe ob '%1/CMakeLists.txt' existiert (list_dir oder read_file).\n"
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
// Nur make, kein cmake Re-Configure.
// cmake_build mit einem bereits existierenden build/-Verzeichnis
// macht automatisch kein Re-Configure wenn CMakeCache.txt vorhanden.
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
        "   (cmake überspringt das Configure wenn CMakeCache.txt vorhanden)\n"
        "3. Berichte Erfolg oder Fehler.\n"
    ).arg(project);

    r.notice         = "→ /compile";
    r.noticeCssClass = "system";
    return r;
}

// ─── handleRun ───────────────────────────────────────────────────────────────
// cmake (falls nötig) + make (falls nötig) + check_run mit danger_zone:true.
// Das LLM entscheidet selbst ob Re-Configure nötig ist.
CommandProcessor::ProcessResult CommandProcessor::handleRun(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled = true;

    QString project = m_currentProject.isEmpty() ? "." : m_currentProject;

    r.prompt = QString(
        "Baue und starte das Projekt. Gehe intelligent vor:\n\n"
        "Schritte:\n"
        "1. Prüfe ob '%1/build/CMakeCache.txt' existiert.\n"
        "   Falls nicht: cmake_build mit source_dir '%1', build_dir '%1/build'.\n"
        "2. Falls cmake_build nötig war oder du unsicher bist ob Quellen\n"
        "   verändert wurden: cmake_build erneut (macht nur make wenn Cache da).\n"
        "3. Suche das gebaute Binary: list_dir '%1/build/' um es zu finden.\n"
        "   Typische Namen: Projektname, Executable aus CMakeLists.txt.\n"
        "4. Starte das Binary mit check_run:\n"
        "   - binary: '%1/build/[BinaryName]'\n"
        "   - danger_zone: true\n"
        "   - timeout_ms: 8000\n"
        "5. Berichte Exit-Code, Laufzeit, stdout und stderr.\n"
        "   Bei Fehler: Analysiere und schlage Lösung vor.\n"
    ).arg(project);

    r.notice         = "→ /run";
    r.noticeCssClass = "system";
    return r;
}
