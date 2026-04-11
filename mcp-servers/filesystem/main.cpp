// ─── LlamaQt MCP Filesystem Server v2.3 ─────────────────────────────────────
// Vollständige Version mit allen Tools inkl. read_multiple_files, find_files,
// search_code, move_file (mit Papierkorb), copy_file, patch_file
//
// Änderungen v2.3:
//   - patch_file: ---/+++ Header-Zeilen werden korrekt übersprungen
//   - patch_file: '\ No newline at end of file' wird ignoriert
//   - patch_file: oldLen/newLen aus @@ Header werden zur Validierung genutzt
//   - patch_file: Leerzeilen im Hunk werden korrekt als Kontext behandelt
//   - patch_file: Diff-Größe begrenzt auf MAX_WRITE_CHARS * 2
//   - patch_file: Hunk-Nummer im Fehlertext statt Original-Zeilennummer
//   - resolvePath: absolute Pfade werden abgelehnt (Sandbox-Sicherheit)
//   - sendError: entfernt (war toter Code; JSON-RPC-Fehler via sendResult)
//   - copy_file: prüft jetzt Existenz der Quelldatei
//   - move_file: Trash-Move-Fehler wird geprüft
//   - makeToolList: vollständige, LLM-taugliche Beschreibungen für alle Tools

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDateTime>
#include <QProcess>
#include <QDirIterator>
#include <iostream>
#include <string>

// ─── Sandbox & Trash ────────────────────────────────────────────────────────
static QString sandboxRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/llamatools";
}

static QString trashRoot()
{
    QString trash = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.llamatools_trash";
    QDir().mkpath(trash);
    return trash;
}

// resolvePath akzeptiert nur relative Pfade.
// Absolute Pfade werden abgelehnt: ein neuer Aufrufer, der isPathAllowed()
// vergisst, hätte sonst sofort einen Sandbox-Escape.
static QString resolvePath(const QString &rel, QString &error)
{
    error.clear();
    if (rel.isEmpty() || rel == "." || rel == "./") return sandboxRoot();
    if (rel.startsWith('/')) {
        error = QString("Error: absolute paths are not allowed: %1").arg(rel);
        return {};
    }
    return sandboxRoot() + "/" + rel;
}

static QString resolveTrashPath(const QString &originalRel)
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString safeName = QFileInfo(originalRel).fileName().replace("/", "_");
    return trashRoot() + "/" + timestamp + "_" + safeName;
}

// ─── Security ───────────────────────────────────────────────────────────────
static bool isSymlinkInPath(const QString &absolutePath)
{
    QStringList parts = absolutePath.split('/', Qt::SkipEmptyParts);
    QString current;
    for (const QString &part : parts) {
        current += "/" + part;
        if (QFileInfo(current).isSymLink())
            return true;
    }
    return false;
}

static bool isPathAllowed(const QString &path, QString &reason)
{
    if (path.isEmpty()) {
        reason = "Error: empty path (absolute path rejected by resolvePath).";
        return false;
    }
    QFileInfo fi(path);
    QString absPath = fi.absoluteFilePath();
    QString absDir  = fi.absolutePath();

    if (!absPath.startsWith(sandboxRoot()) && !absDir.startsWith(sandboxRoot())) {
        reason = QString("Path outside sandbox: %1").arg(path);
        return false;
    }
    if (isSymlinkInPath(absPath)) {
        reason = QString("Symlink detected — not allowed: %1").arg(path);
        return false;
    }
    return true;
}

// Hilfsmakro: resolvePath + isPathAllowed in einem Schritt.
// Gibt false zurück und setzt 'reason', wenn der Pfad ungültig ist.
// 'fullPath' wird nur bei Erfolg gesetzt.
static bool resolveAndCheck(const QString &rel, QString &fullPath, QString &reason)
{
    fullPath = resolvePath(rel, reason);
    if (!reason.isEmpty()) return false;
    return isPathAllowed(fullPath, reason);
}

// ─── Limits ─────────────────────────────────────────────────────────────────
static constexpr int MAX_READ_CHARS     = 32768;
static constexpr int MAX_WRITE_CHARS    = 32768;
static constexpr int MAX_LINES_READ     = 600;
static constexpr int MAX_GREP_HITS      = 400;
static constexpr int MAX_TREE_DEPTH     = 12;
static constexpr int MAX_MULTIPLE_FILES = 12;

// ─── JSON-RPC helpers ───────────────────────────────────────────────────────
static void sendResponse(const QJsonObject &msg)
{
    QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    std::cout << line.toStdString();
    std::cout.flush();
}

static void sendResult(int id, const QString &text, bool isError = false)
{
    sendResponse({
        {"jsonrpc","2.0"}, {"id",id},
        {"result", QJsonObject{
            {"content", QJsonArray{QJsonObject{{"type","text"},{"text",text}}}},
            {"isError", isError}
        }}
    });
}

// ─── Git helpers ─────────────────────────────────────────────────────────────
static QString gitRepoForPath(const QString &path)
{
    QDir dir(path);
    while (dir.absolutePath().startsWith(sandboxRoot())) {
        if (QDir(dir.absolutePath() + "/.git").exists())
            return dir.absolutePath();
        if (!dir.cdUp()) break;
    }

    QString rel = path;
    if (rel.startsWith(sandboxRoot() + "/"))
        rel = rel.mid(sandboxRoot().length() + 1);
    QString projectName = rel.section('/', 0, 0);
    if (!projectName.isEmpty() && !projectName.contains('.'))
        return sandboxRoot() + "/" + projectName;

    return sandboxRoot();
}

static bool ensureGitRepo(const QString &repoPath)
{
    if (QDir(repoPath + "/.git").exists()) return true;

    QDir().mkpath(repoPath);
    QProcess proc;
    proc.setWorkingDirectory(repoPath);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start("git", {"init"});
    if (!proc.waitForStarted(3000) || !proc.waitForFinished(5000)) return false;

    auto checkGlobal = [&](const QString &key) -> bool {
        QProcess p; p.start("git", {"config", "--global", key});
        p.waitForFinished(2000);
        return p.exitCode() == 0 && !QString::fromUtf8(p.readAll()).trimmed().isEmpty();
    };

    if (!checkGlobal("user.name")) {
        QProcess p; p.setWorkingDirectory(repoPath);
        p.start("git", {"config", "user.name", "LlamaQt"}); p.waitForFinished(2000);
    }
    if (!checkGlobal("user.email")) {
        QProcess p; p.setWorkingDirectory(repoPath);
        p.start("git", {"config", "user.email", "llamaqt@local"}); p.waitForFinished(2000);
    }
    return true;
}

static std::pair<QString,int> runGitIn(const QString &repoPath, const QStringList &args)
{
    static const QStringList blocked = {"push","pull","remote","fetch","clone"};
    if (!args.isEmpty() && blocked.contains(args.first().toLower()))
        return {QString("Error: git %1 is not allowed.").arg(args.first()), 1};

    QProcess proc;
    proc.setWorkingDirectory(repoPath);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start("git", args);
    if (!proc.waitForStarted(3000)) return {"Error: could not start git.", 1};
    if (!proc.waitForFinished(15000)) { proc.kill(); return {"Error: git timeout.", 1}; }

    return {QString::fromUtf8(proc.readAll()).trimmed(), proc.exitCode()};
}

static void autoCommit(const QString &filePath, const QString &message)
{
    QString repoPath = gitRepoForPath(filePath);
    if (!ensureGitRepo(repoPath)) return;
    runGitIn(repoPath, {"add", "-A"});
    auto [diffOut, diffCode] = runGitIn(repoPath, {"diff", "--cached", "--quiet"});
    if (diffCode == 0) return;
    runGitIn(repoPath, {"commit", "-m", QString("auto: %1").arg(message)});
}

// ─── Tool Implementierungen ─────────────────────────────────────────────────

static std::pair<QString,bool> handleReadFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot open file: %1").arg(relPath), true};

    QTextStream in(&file);
    QString result = in.readAll();

    if (result.length() > MAX_READ_CHARS) {
        int cut = result.lastIndexOf('\n', MAX_READ_CHARS);
        if (cut < 0) cut = MAX_READ_CHARS;
        result = result.left(cut) + "\n[... truncated]";
    }
    return {result, false};
}

static std::pair<QString,bool> handleWriteFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};
    if (content.length() > MAX_WRITE_CHARS)
        return {QString("Error: content too large (max %1 chars).").arg(MAX_WRITE_CHARS), true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    autoCommit(fullPath, QString("before write_file %1").arg(relPath));

    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    QFile file(fullPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {QString("Error: Cannot write: %1").arg(relPath), true};

    QTextStream(&file) << content;
    autoCommit(fullPath, QString("write_file %1").arg(relPath));
    return {QString("OK: %1 bytes written.").arg(content.length()), false};
}

static std::pair<QString,bool> handleAppendFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};
    if (content.length() > MAX_WRITE_CHARS)
        return {"Error: content too large.", true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    QFile file(fullPath);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return {QString("Error: Cannot append to: %1").arg(relPath), true};

    QTextStream(&file) << content;
    autoCommit(fullPath, QString("append_file %1").arg(relPath));
    return {QString("OK: %1 bytes appended.").arg(content.length()), false};
}

static std::pair<QString,bool> handleStrReplace(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString oldStr  = args.value("old_str").toString();
    QString newStr  = args.value("new_str").toString();
    if (relPath.isEmpty() || oldStr.isEmpty()) return {"Error: path and old_str required.", true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot read: %1").arg(relPath), true};

    QString content = QTextStream(&file).readAll();
    file.close();

    if (!content.contains(oldStr))
        return {QString("Error: old_str not found in %1.").arg(relPath), true};
    if (content.count(oldStr) > 1)
        return {QString("Error: old_str appears multiple times in %1. Use more context.").arg(relPath), true};

    autoCommit(fullPath, QString("before str_replace %1").arg(relPath));

    content.replace(oldStr, newStr);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {"Error: Cannot write file.", true};
    QTextStream(&file) << content;

    autoCommit(fullPath, QString("str_replace %1").arg(relPath));
    return {"OK: Replacement done.", false};
}

static std::pair<QString,bool> handleListDir(const QJsonObject &args)
{
    QString relPath = args.value("path").toString(".");
    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    QDir dir(fullPath);
    if (!dir.exists()) return {"Error: Directory does not exist.", true};

    QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name | QDir::DirsFirst);
    if (entries.isEmpty()) return {"(empty directory)", false};

    QStringList result;
    for (const QString &name : entries) {
        QString ep = fullPath + "/" + name;
        QFileInfo fi(ep);
        QString tag = fi.isSymLink() ? "[L]" : (fi.isDir() ? "[D]" : "[F]");
        result << QString("%1 %2").arg(tag, name);
    }
    return {result.join('\n'), false};
}

static std::pair<QString,bool> handleListSymbols(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot read: %1").arg(relPath), true};

    QTextStream in(&file);
    QStringList symbols;
    int lineNum = 0;
    QRegularExpression classRe(R"(^\s*(class|struct)\s+(\w+))");
    QRegularExpression funcRe(R"(^(\w[\w\s\*&:<>]*)\s+(\w+)\s*\()");

    while (!in.atEnd()) {
        ++lineNum;
        QString line = in.readLine();
        auto cm = classRe.match(line);
        if (cm.hasMatch()) {
            symbols << QString("L%1 [class] %2").arg(lineNum, 4).arg(cm.captured(2));
            continue;
        }
        auto fm = funcRe.match(line);
        if (fm.hasMatch()) {
            symbols << QString("L%1 [func] %2").arg(lineNum, 4).arg(fm.captured(2));
        }
    }
    return {symbols.isEmpty() ? "No symbols found." : symbols.join('\n'), false};
}

static std::pair<QString,bool> handleMkdir(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    if (QDir(fullPath).exists())
        return {QString("OK: '%1' already exists.").arg(relPath), false};

    if (QDir().mkpath(fullPath))
        return {QString("OK: Directory created: %1").arg(relPath), false};

    return {QString("Error: Could not create %1").arg(relPath), true};
}

static std::pair<QString,bool> handleGrepCode(const QJsonObject &args)
{
    QString pattern = args.value("pattern").toString();
    QString relPath = args.value("path").toString(".");
    if (pattern.isEmpty()) return {"Error: 'pattern' is required.", true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    QRegularExpression re(pattern);
    if (!re.isValid()) return {QString("Invalid regex: %1").arg(re.errorString()), true};

    QDirIterator it(fullPath, QStringList() << "*", QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);

    QStringList results;
    int hits = 0;
    while (it.hasNext() && hits < MAX_GREP_HITS) {
        QString filePath = it.next();
        if (isSymlinkInPath(filePath)) continue;

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        QString rel = filePath.mid(sandboxRoot().length() + 1);
        QTextStream in(&file);
        int lineNum = 0;
        while (!in.atEnd()) {
            QString line = in.readLine();
            ++lineNum;
            if (re.match(line).hasMatch()) {
                results << QString("%1:%2: %3").arg(rel).arg(lineNum).arg(line.trimmed());
                ++hits;
                if (hits >= MAX_GREP_HITS) break;
            }
        }
    }
    return {results.isEmpty() ? "No matches." : results.join('\n'), false};
}

static void buildTree(const QString &dirPath, const QString &prefix, QStringList &lines, int depth, int maxDepth)
{
    if (depth > maxDepth) { lines << prefix + "..."; return; }

    QDir dir(dirPath);
    QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name | QDir::DirsFirst);
    entries.removeAll(".git");

    for (int i = 0; i < entries.size(); ++i) {
        bool isLast = (i == entries.size() - 1);
        QString name = entries[i];
        QString full = dirPath + "/" + name;
        QFileInfo fi(full);

        if (fi.isSymLink()) {
            lines << prefix + (isLast ? "└── " : "├── ") + name + " [symlink]";
            continue;
        }
        if (fi.isDir()) {
            lines << prefix + (isLast ? "└── " : "├── ") + name + "/";
            buildTree(full, prefix + (isLast ? "    " : "│   "), lines, depth + 1, maxDepth);
        } else {
            lines << prefix + (isLast ? "└── " : "├── ") + name;
        }
    }
}

static std::pair<QString,bool> handleTree(const QJsonObject &args)
{
    QString relPath = args.value("path").toString(".");
    int maxDepth = qBound(1, args.value("max_depth").toInt(MAX_TREE_DEPTH), 10);

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    if (!QDir(fullPath).exists()) return {"Error: Directory does not exist.", true};

    QStringList lines;
    lines << (relPath == "." ? "." : QFileInfo(fullPath).fileName()) + "/";
    buildTree(fullPath, "", lines, 1, maxDepth);
    return {lines.join('\n'), false};
}

// ─── Git Tools ───────────────────────────────────────────────────────────────
static std::pair<QString,bool> handleGitStatus(const QJsonObject &)
{
    QString repoPath = gitRepoForPath(sandboxRoot());
    auto [out, code] = runGitIn(repoPath, {"status", "--short", "--branch"});
    return {out.isEmpty() ? "Working tree clean." : out, code != 0};
}

static std::pair<QString,bool> handleGitDiff(const QJsonObject &)
{
    QString repoPath = gitRepoForPath(sandboxRoot());
    auto [out, code] = runGitIn(repoPath, {"diff", "HEAD"});
    return {out.isEmpty() ? "No differences." : out, code != 0};
}

static std::pair<QString,bool> handleGitLog(const QJsonObject &args)
{
    int n = qBound(1, args.value("n").toInt(10), 50);
    QString repoPath = gitRepoForPath(sandboxRoot());
    auto [out, code] = runGitIn(repoPath, {"log", "--oneline", QString("-%1").arg(n)});
    return {out.isEmpty() ? "No commits yet." : out, code != 0};
}

static std::pair<QString,bool> handleGitCheckout(const QJsonObject &args)
{
    QString ref = args.value("ref").toString("HEAD~1");
    QString repoPath = gitRepoForPath(sandboxRoot());
    auto [out, code] = runGitIn(repoPath, {"checkout", ref});
    return {code == 0 ? QString("OK: checked out %1").arg(ref) : out, code != 0};
}

// ─── patch_file ──────────────────────────────────────────────────────────────
//
// Algorithmus (unified diff, vereinfacht):
//
// Ein unified diff besteht aus einem oder mehreren "Hunks". Jeder Hunk
// beginnt mit einem Header wie:
//
//   @@ -10,6 +10,7 @@
//
// Das bedeutet: im alten File ab Zeile 10, 6 Zeilen lang; im neuen File
// ab Zeile 10, 7 Zeilen lang. Danach folgen die Hunk-Zeilen:
//   ' '  Kontextzeile (unverändert, gehört zu old UND new)
//   '-'  nur im alten File (wird gelöscht)
//   '+'  nur im neuen File (wird eingefügt)
//
// Wir lesen den gesamten Dateiinhalt in ein QStringList-Array ("patchedLines").
// Für jeden Hunk:
//   1. oldBlock und newBlock aus den Diff-Zeilen aufbauen
//   2. Validieren: oldBlock.size() == oldLen, newBlock.size() == newLen
//   3. Im patchedLines-Array prüfen, ob die Stelle mit oldBlock übereinstimmt
//   4. oldBlock ersetzen durch newBlock
//   5. offset aktualisieren (Zeilenverschiebung durch vorherige Hunks)
//
// Der offset ist nötig, weil jeder Hunk die absolute Zeilennummer aus dem
// Original-Diff verwendet, aber durch frühere Hunks das Array schon
// verschoben sein kann (ähnlich wie ein Zeigerversatz in C).

static std::pair<QString, bool> handlePatchFile(const QJsonObject &args)
{
    QString relPath     = args.value("path").toString();
    QString diffContent = args.value("diff").toString();

    if (relPath.isEmpty() || diffContent.isEmpty())
        return {"Error: 'path' and 'diff' are required.", true};

    // Größenbegrenzung für den Diff
    if (diffContent.length() > MAX_WRITE_CHARS * 2)
        return {QString("Error: diff too large (max %1 chars).").arg(MAX_WRITE_CHARS * 2), true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    // Datei einlesen
    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot read %1").arg(relPath), true};

    QStringList patchedLines;
    {
        QTextStream in(&file);
        while (!in.atEnd()) patchedLines << in.readLine();
    }
    file.close();

    autoCommit(fullPath, QString("before patch_file %1").arg(relPath));

    // Hunk-Header Regex: @@ -oldStart,oldLen +newStart,newLen @@
    // Das Komma und die Länge sind optional (z.B. "@@ -1 +1 @@" = 1 Zeile).
    // [ \t]+ statt \s für Robustheit gegen Tabs.
    QRegularExpression hunkRe(R"(^@@[ \t]+-(\d+),?(\d*)[ \t]+\+(\d+),?(\d*)[ \t]+@@)");

    QStringList diffLines = diffContent.split('\n');
    int offset   = 0;  // Zeilenversatz durch bereits angewandte Hunks
    int hunkNum  = 0;  // Zähler für Fehlermeldungen
    int i        = 0;

    while (i < diffLines.size()) {
        const QString &dline = diffLines[i];

        // Datei-Header-Zeilen ("--- a/foo" / "+++ b/foo") überspringen.
        // Sie beginnen mit "--- " oder "+++ " (mit Leerzeichen nach den Strichen).
        // Ohne diesen Check würden sie als gelöschte/neue Zeilen im Hunk landen.
        if (dline.startsWith("--- ") || dline.startsWith("+++ ")) {
            ++i;
            continue;
        }

        auto match = hunkRe.match(dline);
        if (!match.hasMatch()) {
            ++i;
            continue;
        }

        ++hunkNum;

        // Zeilennummern aus dem Header (1-basiert → 0-basiert für Array-Zugriff)
        int oldStart = match.captured(1).toInt() - 1;
        int oldLen   = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
        // newStart wird nicht für die Anwendung benötigt, nur newLen zur Validierung
        int newLen   = match.captured(4).isEmpty() ? 1 : match.captured(4).toInt();
        ++i;

        QStringList oldBlock;
        QStringList newBlock;

        // Hunk-Inhalt parsen bis zum nächsten @@ oder Ende
        while (i < diffLines.size() && !diffLines[i].startsWith("@@")) {
            const QString &dl = diffLines[i++];

            // Meta-Kommentar "\ No newline at end of file" ignorieren.
            // Beginnt mit Backslash, ist kein Diff-Inhalt.
            if (dl.startsWith('\\')) continue;

            if (dl.startsWith('-')) {
                // Gelöschte Zeile: gehört nur zu oldBlock
                oldBlock << dl.mid(1);
            } else if (dl.startsWith('+')) {
                // Neue Zeile: gehört nur zu newBlock
                newBlock << dl.mid(1);
            } else {
                // Kontextzeile: entweder ' ' + Inhalt, oder Leerzeile (= leere Kontextzeile).
                // Beide gehören zu old UND new.
                QString content = dl.startsWith(' ') ? dl.mid(1) : dl;
                oldBlock << content;
                newBlock << content;
            }
        }

        // Validierung: Blockgrößen müssen mit dem @@ Header übereinstimmen.
        // Abweichungen deuten auf einen kaputten Diff hin.
        if (oldBlock.size() != oldLen) {
            return {QString("Error: Hunk %1 header says oldLen=%2 but parsed %3 old lines.")
                        .arg(hunkNum).arg(oldLen).arg(oldBlock.size()), true};
        }
        if (newBlock.size() != newLen) {
            return {QString("Error: Hunk %1 header says newLen=%2 but parsed %3 new lines.")
                        .arg(hunkNum).arg(newLen).arg(newBlock.size()), true};
        }

        // Position im Array unter Berücksichtigung des Offsets berechnen.
        // Analogie: wie ein Zeiger in C, der durch vorherige Einfügungen
        // verschoben wurde.
        int applyAt = oldStart + offset;

        // Kontextprüfung: Stimmt der oldBlock mit dem aktuellen Dateiinhalt überein?
        if (applyAt < 0 || applyAt + oldBlock.size() > patchedLines.size()) {
            return {QString("Error: Hunk %1 position %2 is out of bounds (file has %3 lines).")
                        .arg(hunkNum).arg(applyAt + 1).arg(patchedLines.size()), true};
        }
        for (int j = 0; j < oldBlock.size(); ++j) {
            if (patchedLines[applyAt + j] != oldBlock[j]) {
                return {QString("Error: Hunk %1 does not match file at line %2.\n"
                                "  Expected: \"%3\"\n"
                                "  Found:    \"%4\"")
                            .arg(hunkNum)
                            .arg(applyAt + j + 1)
                            .arg(oldBlock[j])
                            .arg(patchedLines[applyAt + j]), true};
            }
        }

        // Anwenden: oldBlock aus dem Array entfernen, newBlock einfügen.
        // Qt5: QStringList hat kein remove(index, count) → erase() nutzen.
        // Qt5/Qt6 haben beide QList::erase(iterator, iterator).
        patchedLines.erase(patchedLines.begin() + applyAt,
                           patchedLines.begin() + applyAt + oldBlock.size());
        for (int j = 0; j < newBlock.size(); ++j)
            patchedLines.insert(applyAt + j, newBlock[j]);

        // Offset für den nächsten Hunk anpassen.
        // Wenn wir 3 Zeilen gelöscht und 5 eingefügt haben, sind alle
        // folgenden Zeilennummern um +2 verschoben.
        offset += (newBlock.size() - oldBlock.size());
    }

    if (hunkNum == 0)
        return {"Error: No valid hunks found in diff.", true};

    // Ergebnis zurückschreiben
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {"Error: Cannot write to file.", true};

    {
        QTextStream out(&file);
        for (const QString &l : patchedLines) out << l << "\n";
    }
    file.close();

    autoCommit(fullPath, QString("patch_file %1").arg(relPath));
    return {QString("OK: Applied %1 hunk(s) to %2.").arg(hunkNum).arg(relPath), false};
}

// ─── Weitere Tools ───────────────────────────────────────────────────────────
static std::pair<QString,bool> handleReadMultipleFiles(const QJsonObject &args)
{
    QJsonArray paths = args.value("paths").toArray();
    if (paths.isEmpty()) return {"Error: 'paths' array required (max 10).", true};
    if (paths.size() > MAX_MULTIPLE_FILES)
        return {QString("Error: Max %1 files allowed.").arg(MAX_MULTIPLE_FILES), true};

    QStringList output;
    for (const QJsonValue &v : paths) {
        QString p = v.toString().trimmed();
        if (p.isEmpty()) continue;
        auto [content, err] = handleReadFile({{"path", p}});
        output << QString("=== %1 ===").arg(p);
        output << (err ? "ERROR: " + content : content);
        output << "";
    }
    return {output.join('\n'), false};
}

static std::pair<QString,bool> handleFindFiles(const QJsonObject &args)
{
    QString pattern = args.value("pattern").toString();
    QString relPath = args.value("path").toString(".");
    bool recursive  = args.value("recursive").toBool(true);

    if (pattern.isEmpty()) return {"Error: 'pattern' required.", true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    QDirIterator it(fullPath, QStringList() << pattern, QDir::Files | QDir::NoDotAndDotDot,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);

    QStringList found;
    while (it.hasNext()) {
        QString f = it.next();
        if (isSymlinkInPath(f)) continue;
        found << f.mid(sandboxRoot().length() + 1);
    }
    return {found.isEmpty() ? "No files found." : found.join('\n'), false};
}

static std::pair<QString,bool> handleSearchCode(const QJsonObject &args)
{
    QString pattern = args.value("pattern").toString();
    QString relPath = args.value("path").toString(".");
    int context     = qBound(0, args.value("context").toInt(3), 10);

    if (pattern.isEmpty()) return {"Error: 'pattern' required.", true};

    QString fullPath, reason;
    if (!resolveAndCheck(relPath, fullPath, reason)) return {reason, true};

    QRegularExpression re(pattern);
    if (!re.isValid()) return {"Invalid regex.", true};

    QDirIterator it(fullPath, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);

    QStringList results;
    while (it.hasNext()) {
        QString filePath = it.next();
        if (isSymlinkInPath(filePath)) continue;

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        QString rel = filePath.mid(sandboxRoot().length() + 1);
        QStringList lines = QTextStream(&file).readAll().split('\n');

        for (int i = 0; i < lines.size(); ++i) {
            if (re.match(lines[i]).hasMatch()) {
                results << QString("%1:%2:").arg(rel).arg(i + 1);
                for (int j = qMax(0, i - context); j <= qMin(lines.size() - 1, i + context); ++j) {
                    QString prefix = (j == i) ? ">>" : "  ";
                    results << QString("  %1 %2 | %3").arg(prefix, QString::number(j + 1).rightJustified(4), lines[j].left(100));
                }
                results << "";
                break;
            }
        }
    }
    return {results.isEmpty() ? "No matches." : results.join('\n'), false};
}

static std::pair<QString,bool> handleMoveFile(const QJsonObject &args)
{
    QString src = args.value("source").toString();
    QString dst = args.value("destination").toString();
    if (src.isEmpty() || dst.isEmpty()) return {"Error: source and destination required.", true};

    QString srcFull, dstFull, reason;
    if (!resolveAndCheck(src, srcFull, reason)) return {reason, true};
    if (!resolveAndCheck(dst, dstFull, reason)) return {reason, true};

    if (!QFileInfo(srcFull).exists()) return {"Error: Source not found.", true};

    autoCommit(srcFull, QString("before move %1").arg(src));

    // Zieldatei in Papierkorb verschieben, falls vorhanden.
    // Fehler beim Trash-Move wird geprüft — wir brechen ab statt stillschweigend
    // die Zieldatei zu überschreiben.
    if (QFileInfo(dstFull).exists()) {
        QString trashPath = resolveTrashPath(dst);
        if (!QFile::rename(dstFull, trashPath))
            return {QString("Error: Could not move existing destination to trash: %1").arg(dst), true};
    }

    if (QFile::rename(srcFull, dstFull)) {
        autoCommit(dstFull, QString("move %1 to %2").arg(src, dst));
        return {QString("OK: Moved %1 → %2").arg(src, dst), false};
    }
    return {"Error: Move failed.", true};
}

static std::pair<QString,bool> handleCopyFile(const QJsonObject &args)
{
    QString src = args.value("source").toString();
    QString dst = args.value("destination").toString();
    if (src.isEmpty() || dst.isEmpty()) return {"Error: source and destination required.", true};

    QString srcFull, dstFull, reason;
    if (!resolveAndCheck(src, srcFull, reason)) return {reason, true};
    if (!resolveAndCheck(dst, dstFull, reason)) return {reason, true};

    // Existenz der Quelle prüfen — QFile::copy() gibt sonst nur "Copy failed."
    if (!QFileInfo(srcFull).exists())
        return {QString("Error: Source not found: %1").arg(src), true};

    if (QFile::copy(srcFull, dstFull)) {
        autoCommit(dstFull, QString("copy %1 to %2").arg(src, dst));
        return {QString("OK: Copied %1 → %2").arg(src, dst), false};
    }
    return {"Error: Copy failed (destination may already exist).", true};
}

// ─── Tool List ───────────────────────────────────────────────────────────────
static QJsonArray makeToolList()
{
    // makeProp erzeugt ein JSON-Objekt für eine einzelne Schema-Property.
    auto makeProp = [](const QString &type, const QString &desc) {
        return QJsonObject{{"type", type}, {"description", desc}};
    };

    // makeTool erzeugt den vollständigen Tool-Eintrag im MCP-Format.
    // 'desc' ist die Beschreibung, die das LLM sieht und zur Tool-Auswahl nutzt.
    // 'props' sind die Parameter, 'req' die Pflichtfelder.
    auto makeTool = [](const QString &name, const QString &desc,
                       const QJsonObject &props, const QJsonArray &req = {}) {
        return QJsonObject{
            {"name", name}, {"description", desc},
            {"inputSchema", QJsonObject{{"type","object"}, {"properties", props}, {"required", req}}}
        };
    };

    return QJsonArray{

        makeTool("read_file",
            "Read a text file from the sandbox and return its content. "
            "Paths are relative to the sandbox root (e.g. 'myproject/main.cpp'). "
            "Content is truncated at 4096 characters; use search_code or grep_code "
            "to locate specific sections in large files.",
            {{"path", makeProp("string", "Relative path to the file within the sandbox.")}},
            {"path"}),

        makeTool("write_file",
            "Write (overwrite) a file with new content. Creates parent directories "
            "automatically. Limited to 8192 characters. The previous state is "
            "committed to git before writing. Use append_file to add to an existing file.",
            {
                {"path",    makeProp("string", "Relative path to the file.")},
                {"content", makeProp("string", "Full content to write (max 8192 chars).")}
            },
            {"path", "content"}),

        makeTool("append_file",
            "Append text to the end of an existing file (or create it if it does not exist). "
            "Limited to 8192 characters per call.",
            {
                {"path",    makeProp("string", "Relative path to the file.")},
                {"content", makeProp("string", "Text to append (max 8192 chars).")}
            },
            {"path", "content"}),

        makeTool("str_replace",
            "Replace a unique string in a file with a new string. "
            "old_str must appear exactly once in the file — if it appears multiple times, "
            "add more surrounding lines as context until it is unique. "
            "new_str defaults to empty string (deletion) if omitted. "
            "Prefer this tool for small, targeted edits; use patch_file for larger changes.",
            {
                {"path",    makeProp("string", "Relative path to the file.")},
                {"old_str", makeProp("string", "Exact string to find (must be unique in the file).")},
                {"new_str", makeProp("string", "Replacement string. Omit to delete old_str.")}
            },
            {"path", "old_str"}),

        makeTool("patch_file",
            "Apply a unified diff patch to an existing file. "
            "The diff must be in standard unified diff format as produced by "
            "'diff -u' or 'git diff'. Include the '--- a/...' and '+++ b/...' "
            "header lines and one or more @@ hunks. "
            "Each hunk must have at least 2-3 context lines (' ' prefix) around "
            "the changes to allow unique matching. "
            "Lines starting with '-' are removed, '+' are inserted, ' ' are context. "
            "Use this tool for multi-location edits or when str_replace would require "
            "many separate calls. Changes are committed to git automatically. "
            "Example diff:\n"
            "--- a/foo.cpp\n"
            "+++ b/foo.cpp\n"
            "@@ -10,6 +10,7 @@\n"
            "     int x = 1;\n"
            "-    doOldThing();\n"
            "+    doNewThing();\n"
            "+    doExtraThing();\n"
            "     return x;",
            {
                {"path", makeProp("string", "Relative path to the file to patch.")},
                {"diff", makeProp("string",
                    "Unified diff content including --- and +++ headers and @@ hunks. "
                    "Max 16384 characters.")}
            },
            {"path", "diff"}),

        makeTool("list_dir",
            "List the contents of a directory. Returns one entry per line with a "
            "type tag: [D] directory, [F] file, [L] symlink. "
            "Defaults to the sandbox root if no path is given.",
            {{"path", makeProp("string", "Relative path to directory. Defaults to sandbox root.")}},
            {}),

        makeTool("list_symbols",
            "Extract class, struct, and function definitions from a C/C++ source file. "
            "Returns each symbol with its line number. Useful for getting an overview "
            "of a file's structure before editing.",
            {{"path", makeProp("string", "Relative path to a C/C++ source file.")}},
            {"path"}),

        makeTool("mkdir",
            "Create a directory (including all parent directories). "
            "Does nothing if the directory already exists.",
            {{"path", makeProp("string", "Relative path of the directory to create.")}},
            {"path"}),

        makeTool("grep_code",
            "Search all files in the sandbox (or a subdirectory) for lines matching "
            "a regular expression. Returns up to 100 matches in 'file:line: content' format. "
            "Use this for a quick overview of where a symbol or pattern appears. "
            "For results with surrounding context, use search_code instead.",
            {
                {"pattern", makeProp("string", "ECMAScript/PCRE regex to search for.")},
                {"path",    makeProp("string", "Subdirectory to search in. Defaults to sandbox root.")}
            },
            {"pattern"}),

        makeTool("tree",
            "Show the directory structure as an ASCII tree. "
            "Symlinks are shown but not followed. The .git directory is hidden.",
            {
                {"path",      makeProp("string",  "Relative path to the root directory. Defaults to sandbox root.")},
                {"max_depth", makeProp("integer", "Maximum depth to traverse (1-10, default 6).")}
            },
            {}),

        makeTool("git_status",
            "Show the current git status (branch and modified/untracked files) "
            "for the sandbox repository.",
            {}, {}),

        makeTool("git_diff",
            "Show the unified diff of all uncommitted changes relative to HEAD.",
            {}, {}),

        makeTool("git_log",
            "Show the last N git commits (one per line, short hash + message). "
            "Defaults to 10 commits.",
            {{"n", makeProp("integer", "Number of commits to show (1-50, default 10).")}},
            {}),

        makeTool("git_checkout",
            "Check out a git ref (commit hash, tag, or branch). "
            "Use 'HEAD~1' to go back one commit, 'HEAD~2' for two, etc. "
            "Network operations (push/pull/clone/fetch) are not allowed.",
            {{"ref", makeProp("string", "Git ref to check out (e.g. 'HEAD~1', a commit hash, or branch name).")}},
            {}),

        makeTool("read_multiple_files",
            "Read up to 10 files in one call. Returns each file's content separated "
            "by a '=== path ===' header. Files that cannot be read show an error "
            "instead of their content. Each file is still subject to the 4096-char "
            "truncation limit.",
            {{"paths", makeProp("array", "Array of relative file paths (max 10).")}},
            {"paths"}),

        makeTool("find_files",
            "Find files matching a glob pattern (e.g. '*.cpp', 'test_*.h'). "
            "Returns relative paths, one per line. Searches recursively by default.",
            {
                {"pattern",   makeProp("string",  "Glob pattern, e.g. '*.cpp' or 'CMakeLists.txt'.")},
                {"path",      makeProp("string",  "Subdirectory to search in. Defaults to sandbox root.")},
                {"recursive", makeProp("boolean", "Search subdirectories recursively (default: true).")}
            },
            {"pattern"}),

        makeTool("search_code",
            "Search for a regex pattern and return each match with surrounding context lines. "
            "Unlike grep_code, this shows only the first match per file with N lines of context "
            "above and below, making it easier to understand the surrounding code.",
            {
                {"pattern", makeProp("string",  "ECMAScript/PCRE regex to search for.")},
                {"path",    makeProp("string",  "Subdirectory to search in. Defaults to sandbox root.")},
                {"context", makeProp("integer", "Number of context lines above and below each match (0-10, default 3).")}
            },
            {"pattern"}),

        makeTool("move_file",
            "Move or rename a file or directory within the sandbox. "
            "If the destination already exists, it is moved to the trash "
            "(~/.llamatools_trash) before the move. The previous state is "
            "committed to git.",
            {
                {"source",      makeProp("string", "Relative path of the file to move.")},
                {"destination", makeProp("string", "Relative destination path.")}
            },
            {"source", "destination"}),

        makeTool("copy_file",
            "Copy a file to a new location within the sandbox. "
            "Fails if the destination already exists. "
            "The copy is committed to git automatically.",
            {
                {"source",      makeProp("string", "Relative path of the source file.")},
                {"destination", makeProp("string", "Relative path for the copy (must not already exist).")}
            },
            {"source", "destination"})
    };
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QDir().mkpath(sandboxRoot());

    QJsonArray tools = makeToolList();
    QTextStream in(stdin);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) continue;

        QJsonObject msg  = doc.object();
        QString method   = msg.value("method").toString();
        int id           = msg.value("id").toInt(-1);

        if (method == "initialize") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                {"protocolVersion","2024-11-05"},
                {"serverInfo", QJsonObject{{"name","llamaqt-filesystem"},{"version","2.3"}}}
            }}});
            continue;
        }
        if (method == "tools/list") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{{"tools", tools}}}});
            continue;
        }
        if (method == "tools/call") {
            QJsonObject params = msg.value("params").toObject();
            QString toolName   = params.value("name").toString();
            QJsonObject targs  = params.value("arguments").toObject();

            std::pair<QString,bool> result;

            if      (toolName == "read_file")           result = handleReadFile(targs);
            else if (toolName == "write_file")          result = handleWriteFile(targs);
            else if (toolName == "append_file")         result = handleAppendFile(targs);
            else if (toolName == "str_replace")         result = handleStrReplace(targs);
            else if (toolName == "patch_file")          result = handlePatchFile(targs);
            else if (toolName == "list_dir")            result = handleListDir(targs);
            else if (toolName == "list_symbols")        result = handleListSymbols(targs);
            else if (toolName == "mkdir")               result = handleMkdir(targs);
            else if (toolName == "grep_code")           result = handleGrepCode(targs);
            else if (toolName == "tree")                result = handleTree(targs);
            else if (toolName == "git_status")          result = handleGitStatus(targs);
            else if (toolName == "git_diff")            result = handleGitDiff(targs);
            else if (toolName == "git_log")             result = handleGitLog(targs);
            else if (toolName == "git_checkout")        result = handleGitCheckout(targs);
            else if (toolName == "read_multiple_files") result = handleReadMultipleFiles(targs);
            else if (toolName == "find_files")          result = handleFindFiles(targs);
            else if (toolName == "search_code")         result = handleSearchCode(targs);
            else if (toolName == "move_file")           result = handleMoveFile(targs);
            else if (toolName == "copy_file")           result = handleCopyFile(targs);
            else result = {QString("Error: Unknown tool: %1").arg(toolName), true};

            sendResult(id, result.first, result.second);
        }
    }
    return 0;
}
