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

    if (cmd == "init")      return handleInit(args);
    if (cmd == "build")     return handleBuild(args);
    if (cmd == "compile")   return handleCompile(args);
    if (cmd == "run")       return handleRun(args);
    if (cmd == "summarize") return handleSummarize(args);
    if (cmd == "undo")      return handleUndo(args);
    if (cmd == "diff")      return handleDiff(args);

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
        "  /init [Projektname]  — Projekt + Git-Repo initialisieren\n"
        "  /build               — cmake configure + make\n"
        "  /compile             — nur make\n"
        "  /run                 — cmake + make + Binary starten\n"
        "  /summarize           — Konversation manuell zusammenfassen\n"
        "  /undo [Datei]        — letzten git-commit rückgängig\n"
        "  /diff                — git diff anzeigen";
}

// ─── handleInit ──────────────────────────────────────────────────────────────
// Initialisiert Projekt: Verzeichnis + Git-Repo + AGENT.md + CMakeLists.txt
// Git init geschieht implizit — der filesystem-Server macht auto-commit
// beim ersten write_file, was git init voraussetzt. Wir legen zuerst
// ein .gitignore an um das Repo zu erzwingen.
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
        "   Das erzwingt einen git-Commit und initialisiert das Repo.\n\n"
        "4. Lege '%1/AGENT.md' an (write_file) mit:\n"
        "   # %1 — AGENT.md\n"
        "   Erstellt: [Datum aus Schritt 2]\n"
        "   ## Beschreibung\n(kurz)\n"
        "   ## Verzeichnisstruktur\n(leer)\n"
        "   ## Build\ncmake + make im build/-Verzeichnis\n"
        "   ## TODOs\n- [ ] Projekt befüllen\n\n"
        "5. Lege '%1/CMakeLists.txt' an falls nicht vorhanden (write_file):\n"
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

// ─── handleSummarize ─────────────────────────────────────────────────────────
// Manueller Trigger für Kontext-Zusammenfassung.
// Der eigentliche Summarize-Mechanismus läuft in Agent::summarizeContext().
// Wir signalisieren das hier nur — Agent wertet m_commands.lastCommand() aus.
// Einfacherer Weg: wir geben einen leeren Prompt zurück und setzen ein Flag.
// Noch einfacher: wir emittieren das Signal direkt über den Prompt-Mechanismus.
CommandProcessor::ProcessResult CommandProcessor::handleSummarize(const QStringList &args)
{
    Q_UNUSED(args)
    ProcessResult r;
    r.handled        = true;
    r.notice         = "→ /summarize — Konversation wird zusammengefasst...";
    r.noticeCssClass = "system";
    // Sonderfall: kein Prompt ans LLM — Agent::onUserMessage() erkennt
    // das leere prompt und ruft direkt summarizeContext() auf.
    // Wir markieren es mit einem internen Marker-String.
    r.prompt = "__SUMMARIZE__";
    return r;
}

// ─── handleUndo ──────────────────────────────────────────────────────────────
// Macht den letzten auto-commit rückgängig via git_checkout.
// Optionales Argument: Dateiname (dann nur diese Datei zurücksetzen).
CommandProcessor::ProcessResult CommandProcessor::handleUndo(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;

    QString file = args.isEmpty() ? "" : args.join(' ');

    if (file.isEmpty()) {
        r.prompt = QString(
            "Mache die letzte Änderung in der Sandbox rückgängig.\n\n"
            "Schritte:\n"
            "1. Zeige git_log (n=5) damit wir sehen was rückgängig gemacht wird.\n"
            "2. Führe git_checkout aus mit ref='HEAD~1' ohne file-Argument\n"
            "   um den letzten Commit rückgängig zu machen.\n"
            "   Alternativ falls nur eine bestimmte Datei betroffen sein soll:\n"
            "   git_checkout mit ref='HEAD~1' und file='dateiname'.\n"
            "3. Zeige git_status nach dem Checkout.\n"
            "4. Berichte was zurückgesetzt wurde.\n"
        );
    } else {
        r.prompt = QString(
            "Mache die letzte Änderung an '%1' rückgängig.\n\n"
            "Schritte:\n"
            "1. Zeige git_log (n=3) für Kontext.\n"
            "2. Führe git_checkout aus: ref='HEAD~1', file='%1'.\n"
            "3. Zeige git_diff um zu bestätigen was sich geändert hat.\n"
            "4. Berichte das Ergebnis.\n"
        ).arg(file);
    }

    r.notice         = QString("→ /undo%1").arg(file.isEmpty() ? "" : " " + file);
    r.noticeCssClass = "system";
    return r;
}

// ─── handleDiff ──────────────────────────────────────────────────────────────
// Zeigt git diff der aktuellen Änderungen.
CommandProcessor::ProcessResult CommandProcessor::handleDiff(const QStringList &args)
{
    ProcessResult r;
    r.handled = true;

    QString file = args.isEmpty() ? "" : args.join(' ');

    r.prompt = QString(
        "Zeige die aktuellen Änderungen in der Sandbox%1.\n\n"
        "Schritte:\n"
        "1. Führe git_status aus um den Überblick zu geben.\n"
        "2. Führe git_diff aus%2.\n"
        "3. Erkläre kurz was sich geändert hat und warum.\n"
    ).arg(
        file.isEmpty() ? "" : QString(" für Datei '%1'").arg(file),
        file.isEmpty() ? "" : QString(" mit file='%1'").arg(file)
    );

    r.notice         = "→ /diff";
    r.noticeCssClass = "system";
    return r;
}

