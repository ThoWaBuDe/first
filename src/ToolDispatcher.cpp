#include "ToolDispatcher.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QStorageInfo>
#include <QSysInfo>
#include <QStandardPaths>
#include <QThread>

// ─── Tool-Call Tags ───────────────────────────────────────────────────────────
static constexpr const char *TOOL_CALL_OPEN  = "<tool_call>";
static constexpr const char *TOOL_CALL_CLOSE = "</tool_call>";

// ─── Limits ───────────────────────────────────────────────────────────────────
static constexpr int MAX_READ_CHARS   = 4096;  // max Zeichen pro read_file
static constexpr int MAX_WRITE_CHARS  = 8192;  // max Zeichen pro write_file
static constexpr int MAX_LINES_READ   = 200;   // max Zeilen bei Range-Read

// ─── Konstruktor ─────────────────────────────────────────────────────────────
ToolDispatcher::ToolDispatcher(QObject *parent)
    : QObject(parent)
{
    m_sandboxRoot = QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
                    + "/llamatools";
    QDir().mkpath(m_sandboxRoot);
}

// ─── resolvePath ─────────────────────────────────────────────────────────────
// Wandelt einen relativen Pfad in einen absoluten Sandbox-Pfad um.
// Sonderfall "./" oder "" → Sandbox-Root selbst.
QString ToolDispatcher::resolvePath(const QString &relPath) const
{
    if (relPath.isEmpty() || relPath == "." || relPath == "./")
        return m_sandboxRoot;
    // Absoluter Pfad → direkt verwenden (wird dann von isPathAllowed geprüft)
    if (relPath.startsWith('/'))
        return relPath;
    return m_sandboxRoot + "/" + relPath;
}

// ─── isPathAllowed ────────────────────────────────────────────────────────────
// Sicherheitsfunktion: löst ../ via QFileInfo::absolutePath() auf.
// Nur Pfade innerhalb m_sandboxRoot sind erlaubt.
bool ToolDispatcher::isPathAllowed(const QString &path) const
{
    QFileInfo fi(path);
    // absolutePath() = Parent-Verzeichnis, löst ../ auf
    // absoluteFilePath() würde bei nicht-existenter Datei "" zurückgeben
    QString absDir = fi.absolutePath();
    QString absFile = fi.absoluteFilePath();

    // Datei selbst oder ihr Verzeichnis muss innerhalb der Sandbox liegen
    return absFile.startsWith(m_sandboxRoot) || absDir.startsWith(m_sandboxRoot);
}

// ─── containsToolCall ─────────────────────────────────────────────────────────
bool ToolDispatcher::containsToolCall(const QString &text) const
{
    return text.contains(TOOL_CALL_OPEN) && text.contains(TOOL_CALL_CLOSE);
}

// ─── extractToolCallBlock ─────────────────────────────────────────────────────
QString ToolDispatcher::extractToolCallBlock(const QString &text) const
{
    int start = text.indexOf(TOOL_CALL_OPEN);
    if (start < 0) return {};
    int contentStart = start + QString(TOOL_CALL_OPEN).length();
    int end = text.indexOf(TOOL_CALL_CLOSE, contentStart);
    if (end < 0) return {};
    return text.mid(contentStart, end - contentStart).trimmed();
}

// ─── dispatch ────────────────────────────────────────────────────────────────
// Pattern: Command Dispatcher / Table-Driven Dispatch
// Mappt Tool-Namen auf Handler-Methoden.
QString ToolDispatcher::dispatch(const QString &jsonBlock, QString &toolName)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(jsonBlock.toUtf8(), &err);

    if (err.error != QJsonParseError::NoError)
        return QString("JSON Fehler: %1").arg(err.errorString());

    QJsonObject obj  = doc.object();
    toolName         = obj.value("name").toString();
    QJsonObject args = obj.value("arguments").toObject();

    // ─── Filesystem ───────────────────────────────────────────────────────
    if (toolName == "read_file")    return handleReadFile(args);
    if (toolName == "write_file")   return handleWriteFile(args);
    if (toolName == "str_replace")  return handleStrReplace(args);
    if (toolName == "list_dir")     return handleListDir(args);
    if (toolName == "list_symbols") return handleListSymbols(args);

    // ─── System ───────────────────────────────────────────────────────────
    if (toolName == "get_time")     return handleGetTime(args);
    if (toolName == "get_pwd")      return handleGetPwd(args);
    if (toolName == "disk_free")    return handleDiskFree(args);
    if (toolName == "sys_info")     return handleSysInfo(args);

    return QString("Unbekanntes Tool: '%1'").arg(toolName);
}

// ═════════════════════════════════════════════════════════════════════════════
// FILESYSTEM TOOLS
// ═════════════════════════════════════════════════════════════════════════════

// ─── handleReadFile ──────────────────────────────────────────────────────────
// Liest eine Datei, optional nur einen Zeilenbereich (start_line / end_line).
// Zeilennummern sind 1-basiert (wie in Editoren üblich).
//
// Ohne Range: max MAX_READ_CHARS Zeichen
// Mit Range:  max MAX_LINES_READ Zeilen
QString ToolDispatcher::handleReadFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return "Fehler: 'path' fehlt.";

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return QString("Fehler: Pfad außerhalb Sandbox: %1").arg(relPath);

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString("Fehler: Datei nicht lesbar: %1").arg(relPath);

    // ─── Optionaler Zeilenbereich ─────────────────────────────────────────
    // start_line / end_line sind optional. Wenn vorhanden → Range-Read.
    // Ist das Modell-Äquivalent zu einem Editor der einen Bereich markiert.
    bool hasRange   = args.contains("start_line");
    int  startLine  = args.value("start_line").toInt(1);   // 1-basiert
    int  endLine    = args.value("end_line").toInt(startLine + MAX_LINES_READ - 1);

    if (startLine < 1) startLine = 1;
    // Sicherheitsgrenze: nicht mehr als MAX_LINES_READ auf einmal
    if (endLine - startLine + 1 > MAX_LINES_READ)
        endLine = startLine + MAX_LINES_READ - 1;

    QTextStream in(&file);
    QString result;

    if (hasRange) {
        // Range-Modus: Zeile für Zeile lesen, nur gewünschten Bereich ausgeben
        // Analogie AVR: wie ein UART-Empfangspuffer mit Offset-Zugriff
        int currentLine = 0;
        result += QString("// %1 [Zeilen %2-%3]\n").arg(relPath).arg(startLine).arg(endLine);
        while (!in.atEnd()) {
            ++currentLine;
            QString line = in.readLine();
            if (currentLine >= startLine && currentLine <= endLine) {
                // Zeilennummer mit ausgeben — hilft dem Modell bei str_replace
                result += QString("%1: %2\n").arg(currentLine, 4).arg(line);
            }
            if (currentLine > endLine) break;
        }
        if (currentLine < startLine)
            return QString("Fehler: Datei hat nur %1 Zeilen (start_line=%2).")
                   .arg(currentLine).arg(startLine);
    } else {
        // Vollständiger Read mit Zeichenlimit
        result = in.readAll();
        if (result.length() > MAX_READ_CHARS) {
            // Abschneiden und Hinweis ausgeben wie man weiterliest
            int lastNewline = result.lastIndexOf('\n', MAX_READ_CHARS);
            int cutAt = (lastNewline > 0) ? lastNewline : MAX_READ_CHARS;
            int linesRead = result.left(cutAt).count('\n') + 1;
            result = result.left(cutAt);
            result += QString("\n[... abgeschnitten nach %1 Zeichen / %2 Zeilen."
                              " Weiter lesen mit start_line=%3]")
                      .arg(cutAt).arg(linesRead).arg(linesRead + 1);
        }
    }

    return result;
}

// ─── handleWriteFile ─────────────────────────────────────────────────────────
// Schreibt/überschreibt eine Datei.
// Content-Limit: MAX_WRITE_CHARS — für größere Dateien str_replace verwenden.
QString ToolDispatcher::handleWriteFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();

    if (relPath.isEmpty()) return "Fehler: 'path' fehlt.";

    if (content.length() > MAX_WRITE_CHARS)
        return QString("Fehler: content zu groß (%1 Zeichen, max %2)."
                       " Verwende str_replace für Änderungen an bestehenden Dateien"
                       " oder schreibe in kleineren Blöcken.")
               .arg(content.length()).arg(MAX_WRITE_CHARS);

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return QString("Fehler: Pfad außerhalb Sandbox: %1").arg(relPath);

    // Übergeordnete Verzeichnisse anlegen
    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    QFile file(fullPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString("Fehler: Datei nicht schreibbar: %1").arg(relPath);

    QTextStream out(&file);
    out << content;

    int lines = content.count('\n') + 1;
    return QString("OK: %1 Bytes (%2 Zeilen) in '%3' geschrieben.")
           .arg(content.length()).arg(lines).arg(relPath);
}

// ─── handleStrReplace ────────────────────────────────────────────────────────
// Ersetzt einen eindeutigen Textblock in einer Datei.
//
// Pattern: Find-and-Replace mit Eindeutigkeitsprüfung.
// Der old_str muss GENAU EINMAL in der Datei vorkommen — sonst Fehler.
// Das verhindert versehentliche Mehrfach-Ersetzungen.
//
// Vorteil gegenüber Zeilennummern: Zeilen können sich verschieben,
// der zu ersetzende Text bleibt als Anker gültig.
QString ToolDispatcher::handleStrReplace(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString oldStr  = args.value("old_str").toString();
    QString newStr  = args.value("new_str").toString();

    if (relPath.isEmpty()) return "Fehler: 'path' fehlt.";
    if (oldStr.isEmpty())  return "Fehler: 'old_str' fehlt.";
    // new_str darf leer sein (= Löschen)

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return QString("Fehler: Pfad außerhalb Sandbox: %1").arg(relPath);

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString("Fehler: Datei nicht lesbar: %1").arg(relPath);

    QString content = QTextStream(&file).readAll();
    file.close();

    // ─── Eindeutigkeitsprüfung ────────────────────────────────────────────
    // Zählt wie oft old_str vorkommt.
    // 0 → nicht gefunden (Tippfehler? falscher Kontext?)
    // 1 → eindeutig → OK
    // >1 → mehrdeutig → Fehler, Modell soll mehr Kontext in old_str geben
    int count = 0;
    int pos   = 0;
    while ((pos = content.indexOf(oldStr, pos)) != -1) {
        ++count;
        pos += oldStr.length();
    }

    if (count == 0) {
        // Hilfreich: zeige die ersten 80 Zeichen des gesuchten Strings
        QString preview = oldStr.left(80).replace('\n', "↵");
        return QString("Fehler: 'old_str' nicht gefunden in '%1'.\n"
                       "Gesuchter Text (Anfang): %2")
               .arg(relPath).arg(preview);
    }
    if (count > 1) {
        return QString("Fehler: 'old_str' kommt %1x vor in '%2' — nicht eindeutig.\n"
                       "Gib mehr Kontext in 'old_str' an (mehr umgebende Zeilen).")
               .arg(count).arg(relPath);
    }

    // Ersetzen
    content.replace(oldStr, newStr);

    // Zurückschreiben
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString("Fehler: Datei nicht schreibbar: %1").arg(relPath);
    QTextStream(&file) << content;

    int oldLines = oldStr.count('\n') + 1;
    int newLines = newStr.count('\n') + 1;
    return QString("OK: %1 Zeile(n) ersetzt durch %2 Zeile(n) in '%3'.")
           .arg(oldLines).arg(newLines).arg(relPath);
}

// ─── handleListDir ────────────────────────────────────────────────────────────
QString ToolDispatcher::handleListDir(const QJsonObject &args)
{
    QString relPath  = args.value("path").toString(".");
    QString fullPath = resolvePath(relPath);

    if (!isPathAllowed(fullPath))
        return "Fehler: Pfad außerhalb Sandbox.";

    QDir dir(fullPath);
    if (!dir.exists())
        return QString("Fehler: Verzeichnis existiert nicht: %1").arg(relPath);

    QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                        QDir::Name | QDir::DirsFirst);
    if (entries.isEmpty()) return "(leer)";

    // Typ-Prefix: [D] für Directory, [F] für File
    QStringList result;
    for (const QString &name : entries) {
        QFileInfo fi(fullPath + "/" + name);
        QString prefix = fi.isDir() ? "[D]" : "[F]";
        QString size   = fi.isFile()
                         ? QString(" (%1 Bytes)").arg(fi.size())
                         : QString();
        result << QString("%1 %2%3").arg(prefix, name, size);
    }
    return result.join('\n');
}

// ─── handleListSymbols ───────────────────────────────────────────────────────
// Extrahiert C++ Klassen- und Methodennamen aus einer Quelldatei.
//
// Implementierung: reines Regex/String-Matching ohne externen Parser.
// Erkennt:
//   - Klassendefinitionen:   "class Foo"  / "struct Foo"
//   - Methodendefinitionen:  "Typ Klasse::Methode(" am Zeilenanfang
//   - Freie Funktionen:      "Typ name(" am Zeilenanfang (kein ::)
//
// Einschränkung: Template-Funktionen und Lambda-Closures werden nicht erkannt.
// Für vollständige Analyse wäre tree-sitter nötig (Stufe 2).
QString ToolDispatcher::handleListSymbols(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return "Fehler: 'path' fehlt.";

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return QString("Fehler: Pfad außerhalb Sandbox: %1").arg(relPath);

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString("Fehler: Datei nicht lesbar: %1").arg(relPath);

    QTextStream in(&file);
    QStringList symbols;
    int lineNum = 0;

    // Regex-Patterns (compiliert einmalig hier, nicht im Loop):
    //
    // classPattern:  "class" oder "struct" gefolgt von Bezeichner, optional ":"
    //   Gruppe 1 = "class"/"struct", Gruppe 2 = Name
    //
    // methodPattern: Rückgabetyp + optionaler Klassenname:: + Methodenname + "("
    //   Erkennt nur Definitionen (im .cpp), nicht Deklarationen (im .h).
    //   Am Zeilenanfang verankert (^\S) um lokale Lambdas zu ignorieren.
    //
    // Das ist kein vollständiger C++ Parser — reicht aber für Navigation.
    QRegularExpression classPattern(
        R"(^\s*(?:class|struct)\s+(\w+))",
        QRegularExpression::MultilineOption);

    QRegularExpression methodPattern(
        R"(^(\w[\w\s\*&:<>]*)\s+(\w+::)?(\w+)\s*\()",
        QRegularExpression::MultilineOption);

    // Schlüsselwörter die keine Funktionen sind (Kontrollfluss etc.)
    static const QStringList keywords = {
        "if", "while", "for", "switch", "return", "else",
        "catch", "case", "do", "sizeof", "alignof"
    };

    while (!in.atEnd()) {
        ++lineNum;
        QString line = in.readLine();

        // Klassen/Structs
        auto classMatch = classPattern.match(line);
        if (classMatch.hasMatch()) {
            symbols << QString("L%1  [class]  %2")
                       .arg(lineNum, 4).arg(classMatch.captured(1));
            continue;
        }

        // Methoden/Funktionen: nur Zeilen die nicht mit Leerzeichen beginnen
        // (Definition im .cpp beginnt am Zeilenanfang, Deklaration im .h mit Indent)
        if (!line.isEmpty() && !line[0].isSpace() && line.contains('(')) {
            auto methodMatch = methodPattern.match(line);
            if (methodMatch.hasMatch()) {
                QString name = methodMatch.captured(3);
                if (!keywords.contains(name)) {
                    QString scope = methodMatch.captured(2); // "Klasse::" oder ""
                    QString type  = scope.isEmpty() ? "[func]" : "[method]";
                    symbols << QString("L%1  %2  %3%4")
                               .arg(lineNum, 4).arg(type)
                               .arg(scope).arg(name);
                }
            }
        }
    }

    if (symbols.isEmpty())
        return QString("Keine Symbole gefunden in '%1'.\n"
                       "(Nur .h/.cpp Dateien werden analysiert)").arg(relPath);

    return QString("Symbole in '%1':\n%2").arg(relPath).arg(symbols.join('\n'));
}

// ═════════════════════════════════════════════════════════════════════════════
// SYSTEM TOOLS
// ═════════════════════════════════════════════════════════════════════════════

// ─── handleGetTime ───────────────────────────────────────────────────────────
QString ToolDispatcher::handleGetTime(const QJsonObject &)
{
    QDateTime now = QDateTime::currentDateTime();
    return QString("Datum:    %1\n"
                   "Uhrzeit:  %2\n"
                   "UTC:      %3\n"
                   "Unix:     %4")
           .arg(now.toString("dddd, dd. MMMM yyyy"))
           .arg(now.toString("HH:mm:ss"))
           .arg(now.toUTC().toString("HH:mm:ss UTC"))
           .arg(now.toSecsSinceEpoch());
}

// ─── handleGetPwd ────────────────────────────────────────────────────────────
// Gibt das Sandbox-Verzeichnis zurück (entspricht "pwd" aus Modell-Perspektive)
QString ToolDispatcher::handleGetPwd(const QJsonObject &)
{
    return QString("Sandbox-Root: %1\n"
                   "Existiert:    %2")
           .arg(m_sandboxRoot)
           .arg(QDir(m_sandboxRoot).exists() ? "ja" : "nein");
}

// ─── handleDiskFree ──────────────────────────────────────────────────────────
// Freier Speicher auf dem Dateisystem — verwendet QStorageInfo (Qt intern,
// kein externer Prozess nötig). Entspricht "df -h" für einen Pfad.
QString ToolDispatcher::handleDiskFree(const QJsonObject &args)
{
    // Optionaler Pfad — default: Sandbox selbst
    QString path = args.value("path").toString(m_sandboxRoot);

    // QStorageInfo kapselt statfs() intern — keine Shell, keine Pipes
    QStorageInfo storage(path);
    if (!storage.isValid())
        return QString("Fehler: Kein Dateisystem unter '%1' gefunden.").arg(path);

    // Bytes in lesbare Einheit umrechnen
    auto toHuman = [](qint64 bytes) -> QString {
        const double GB = 1024.0 * 1024.0 * 1024.0;
        const double MB = 1024.0 * 1024.0;
        if (bytes >= GB) return QString("%1 GB").arg(bytes / GB, 0, 'f', 1);
        if (bytes >= MB) return QString("%1 MB").arg(bytes / MB, 0, 'f', 1);
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    };

    qint64 total     = storage.bytesTotal();
    qint64 avail     = storage.bytesAvailable();
    qint64 used      = total - avail;
    int    usedPct   = (total > 0) ? static_cast<int>(used * 100 / total) : 0;

    return QString("Dateisystem: %1\n"
                   "Pfad:        %2\n"
                   "Gesamt:      %3\n"
                   "Belegt:      %4 (%5%)\n"
                   "Frei:        %6")
           .arg(storage.fileSystemType())
           .arg(path)
           .arg(toHuman(total))
           .arg(toHuman(used)).arg(usedPct)
           .arg(toHuman(avail));
}

// ─── handleSysInfo ───────────────────────────────────────────────────────────
// Systeminformationen über QSysInfo (Qt intern) und /proc (Linux-spezifisch).
// Kein fork(), kein system() — alles direkt gelesen.
QString ToolDispatcher::handleSysInfo(const QJsonObject &)
{
    // CPU-Kerne: QThread::idealThreadCount() = logische Kerne (mit HT)
    int cores = QThread::idealThreadCount();

    // RAM: /proc/meminfo lesen (Linux) — kein externer Prozess
    QString memInfo;
    QFile memFile("/proc/meminfo");
    if (memFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&memFile);
        qint64 memTotal = 0, memFree = 0, memAvail = 0;

        // /proc/meminfo Format: "MemTotal:        9437184 kB"
        // QString::section() ist robuster als split() für dieses Format:
        //   section(':', 1)  → "        9437184 kB"
        //   trimmed()        → "9437184 kB"
        //   section(' ', 0, 0) → "9437184"
        // Kein Regex nötig, keine Abhängigkeit von Whitespace-Anzahl.
        while (!in.atEnd()) {
            QString line = in.readLine();
            auto extractKb = [&](const QString &l) -> qint64 {
                return l.section(':', 1).trimmed().section(' ', 0, 0).toLongLong();
            };
            if (line.startsWith("MemTotal:"))     memTotal = extractKb(line);
            else if (line.startsWith("MemFree:")) memFree  = extractKb(line);
            else if (line.startsWith("MemAvailable:")) memAvail = extractKb(line);
            if (memTotal && memFree && memAvail) break;
        }
        // /proc/meminfo liefert kB → in MB umrechnen
        if (memTotal > 0) {
            memInfo = QString("RAM gesamt:  %1 MB\n"
                              "RAM frei:    %2 MB\n"
                              "RAM verfügb: %3 MB")
                      .arg(memTotal / 1024).arg(memFree / 1024).arg(memAvail / 1024);
        } else {
            memInfo = "RAM: Parsing fehlgeschlagen";
        }
    } else {
        memInfo = "RAM: nicht lesbar (/proc/meminfo)";
    }

    return QString("Hostname:    %1\n"
                   "OS:          %2\n"
                   "Kernel:      %3\n"
                   "CPU-Kerne:   %4 (logisch)\n"
                   "%5")
           .arg(QSysInfo::machineHostName())
           .arg(QSysInfo::prettyProductName())
           .arg(QSysInfo::kernelVersion())
           .arg(cores)
           .arg(memInfo);
}

// ═════════════════════════════════════════════════════════════════════════════
// SYSTEM-PROMPT BESCHREIBUNG
// ═════════════════════════════════════════════════════════════════════════════

QString ToolDispatcher::toolsDescription()
{
    return R"(Du hast Zugriff auf folgende Tools. Rufe sie bei Bedarf so auf:
<tool_call>
{"name": "TOOLNAME", "arguments": {...}}
</tool_call>

Warte nach jedem Tool-Call auf das Ergebnis bevor du weitermachst.

━━━ FILESYSTEM (Sandbox: ~/llamatools/) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. read_file — Datei lesen
   {"path": "datei.cpp"}
   {"path": "datei.cpp", "start_line": 40, "end_line": 80}
   Hinweis: Ohne Range max 4096 Zeichen. Mit Range max 200 Zeilen.
   Bei "[... abgeschnitten]" weiter lesen mit start_line=N.

2. write_file — Datei schreiben (neu/überschreiben)
   {"path": "datei.txt", "content": "Inhalt..."}
   Hinweis: Max 8192 Zeichen. Für Änderungen an bestehenden Dateien
   besser str_replace verwenden.

3. str_replace — Text in Datei ersetzen (bevorzugt für Edits!)
   {"path": "datei.cpp", "old_str": "alter Text\nmehrere Zeilen ok", "new_str": "neuer Text"}
   Hinweis: old_str muss GENAU EINMAL in der Datei vorkommen.
   Bei Mehrdeutigkeit: mehr Kontext (mehr umgebende Zeilen) in old_str angeben.
   new_str darf leer sein (= Löschen).

4. list_dir — Verzeichnis auflisten
   {"path": "."}

5. list_symbols — C++ Symbole einer Datei
   {"path": "src/MainWindow.cpp"}
   Gibt Zeilennummern, Klassen, Methoden und Funktionen zurück.
   Nützlich um Überblick zu bekommen bevor read_file mit Range.

━━━ SYSTEM (read-only) ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

6. get_time — aktuelle Uhrzeit und Datum
   {}

7. get_pwd — aktuelles Arbeitsverzeichnis (Sandbox-Root)
   {}

8. disk_free — freier Speicher
   {}
   {"path": "/home"}

9. sys_info — Systeminformationen (CPU, RAM, OS)
   {}

━━━ WORKFLOW-TIPPS ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Für Datei untersuchen:  list_symbols → read_file mit Range
Für Datei editieren:    read_file (relevanter Bereich) → str_replace
Neue kleine Datei:      write_file
Große neue Datei:       write_file in Blöcken + str_replace zum Ergänzen

Antworte auf Deutsch.)";
}
