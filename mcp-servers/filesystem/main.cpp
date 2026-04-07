// ─── LlamaQt MCP Filesystem Server v2.1 ─────────────────────────────────────
// Dateioperationen + Git-Integration + patch_file (unified diff)
// Sicherheit: Alle Pfade relativ zu ~/llamatools/, Symlinks verboten
// Git: Auto-Checkpoint vor jeder schreibenden Operation

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

// ─── Sandbox ─────────────────────────────────────────────────────────────────
static QString sandboxRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
           + "/llamatools";
}

static QString resolvePath(const QString &rel)
{
    if (rel.isEmpty() || rel == "." || rel == "./") return sandboxRoot();
    if (rel.startsWith('/')) return rel;
    return sandboxRoot() + "/" + rel;
}

// ─── isSymlinkInPath ─────────────────────────────────────────────────────────
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

// ─── Limits ──────────────────────────────────────────────────────────────────
static constexpr int MAX_READ_CHARS  = 4096;
static constexpr int MAX_WRITE_CHARS = 8192;
static constexpr int MAX_LINES_READ  = 200;
static constexpr int MAX_GREP_HITS   = 100;
static constexpr int MAX_TREE_DEPTH  = 6;

// ─── JSON-RPC helpers ────────────────────────────────────────────────────────
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

static void sendError(int id, int code, const QString &message)
{
    sendResponse({{"jsonrpc","2.0"},{"id",id},
                  {"error",QJsonObject{{"code",code},{"message",message}}}});
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
    if (QDir(repoPath + "/.git").exists())
        return true;

    QDir().mkpath(repoPath);

    QProcess proc;
    proc.setWorkingDirectory(repoPath);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start("git", {"init"});
    if (!proc.waitForStarted(3000) || !proc.waitForFinished(5000))
        return false;

    auto checkGlobal = [&](const QString &key) -> bool {
        QProcess p;
        p.start("git", {"config", "--global", key});
        p.waitForFinished(2000);
        return p.exitCode() == 0 && !QString::fromUtf8(p.readAll()).trimmed().isEmpty();
    };

    if (!checkGlobal("user.name")) {
        QProcess p; p.setWorkingDirectory(repoPath);
        p.start("git", {"config", "user.name", "LlamaQt"});
        p.waitForFinished(2000);
    }
    if (!checkGlobal("user.email")) {
        QProcess p; p.setWorkingDirectory(repoPath);
        p.start("git", {"config", "user.email", "llamaqt@local"});
        p.waitForFinished(2000);
    }

    return QDir(repoPath + "/.git").exists();
}

static std::pair<QString,int> runGitIn(const QString &repoPath, const QStringList &args)
{
    static const QStringList blocked = {"push","pull","remote","fetch","clone"};
    if (!args.isEmpty() && blocked.contains(args.first().toLower())) {
        return {QString("Error: git %1 is not allowed.").arg(args.first()), 1};
    }

    QProcess proc;
    proc.setWorkingDirectory(repoPath);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start("git", args);

    if (!proc.waitForStarted(3000)) return {"Error: could not start git.", 1};
    if (!proc.waitForFinished(15000)) { proc.kill(); return {"Error: git timeout.", 1}; }

    return {QString::fromUtf8(proc.readAll()).trimmed(), proc.exitCode()};
}

static std::pair<QString,int> runGit(const QStringList &args)
{
    return runGitIn(sandboxRoot(), args);
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

// ─── Tool Handler ────────────────────────────────────────────────────────────

static std::pair<QString,bool> handleReadFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot open file: %1").arg(relPath), true};

    bool hasRange  = args.contains("start_line");
    int  startLine = args.value("start_line").toInt(1);
    int  endLine   = args.value("end_line").toInt(startLine + MAX_LINES_READ - 1);
    if (startLine < 1) startLine = 1;
    if (endLine - startLine + 1 > MAX_LINES_READ)
        endLine = startLine + MAX_LINES_READ - 1;

    QTextStream in(&file);
    QString result;

    if (hasRange) {
        int cur = 0;
        result += QString("// %1 [lines %2-%3]\n").arg(relPath).arg(startLine).arg(endLine);
        while (!in.atEnd()) {
            ++cur;
            QString line = in.readLine();
            if (cur >= startLine && cur <= endLine)
                result += QString("%1: %2\n").arg(cur, 4).arg(line);
            if (cur > endLine) break;
        }
        if (cur < startLine)
            return {QString("Error: File has only %1 lines.").arg(cur), true};
    } else {
        result = in.readAll();
        if (result.length() > MAX_READ_CHARS) {
            int cut = result.lastIndexOf('\n', MAX_READ_CHARS);
            if (cut < 0) cut = MAX_READ_CHARS;
            int lines = result.left(cut).count('\n') + 1;
            result = result.left(cut);
            result += QString("\n[... truncated. Continue with start_line=%1]").arg(lines + 1);
        }
    }
    return {result, false};
}

static std::pair<QString,bool> handleWriteFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};
    if (content.length() > MAX_WRITE_CHARS)
        return {QString("Error: content too large (%1 chars, max %2).")
                .arg(content.length()).arg(MAX_WRITE_CHARS), true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    autoCommit(fullPath, QString("before write_file %1").arg(relPath));

    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    QFile file(fullPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {QString("Error: Cannot write: %1").arg(relPath), true};

    QTextStream(&file) << content;
    autoCommit(fullPath, QString("write_file %1").arg(relPath));
    return {QString("OK: %1 bytes written to '%2'.").arg(content.length()).arg(relPath), false};
}

static std::pair<QString,bool> handleAppendFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};
    if (content.length() > MAX_WRITE_CHARS)
        return {QString("Error: content too large."), true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    QFile file(fullPath);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return {QString("Error: Cannot append to: %1").arg(relPath), true};

    QTextStream(&file) << content;
    qint64 total = file.size();
    autoCommit(fullPath, QString("append_file %1").arg(relPath));
    return {QString("OK: %1 bytes appended to '%2' (total: %3 bytes).")
            .arg(content.length()).arg(relPath).arg(total), false};
}

static std::pair<QString,bool> handleStrReplace(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString oldStr  = args.value("old_str").toString();
    QString newStr  = args.value("new_str").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};
    if (oldStr.isEmpty())  return {"Error: 'old_str' is required.", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot read: %1").arg(relPath), true};
    QString content = QTextStream(&file).readAll();
    file.close();

    int count = 0, pos = 0;
    while ((pos = content.indexOf(oldStr, pos)) != -1) { ++count; pos += oldStr.length(); }

    if (count == 0)
        return {QString("Error: 'old_str' not found in '%1'.").arg(relPath), true};
    if (count > 1)
        return {QString("Error: 'old_str' found %1 times — not unique. "
                        "Add more context.").arg(count), true};

    autoCommit(fullPath, QString("before str_replace %1").arg(relPath));

    content.replace(oldStr, newStr);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {"Error: Cannot write file.", true};
    QTextStream(&file) << content;

    autoCommit(fullPath, QString("str_replace %1").arg(relPath));
    return {QString("OK: replaced %1 line(s) with %2 line(s) in '%3'.")
            .arg(oldStr.count('\n')+1).arg(newStr.count('\n')+1).arg(relPath), false};
}

static std::pair<QString,bool> handleListDir(const QJsonObject &args)
{
    QString relPath  = args.value("path").toString(".");
    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QDir dir(fullPath);
    if (!dir.exists()) return {"Error: Directory does not exist.", true};

    QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                        QDir::Name | QDir::DirsFirst);
    if (entries.isEmpty()) return {"(empty)", false};

    QStringList result;
    for (const QString &name : entries) {
        QString ep = fullPath + "/" + name;
        QFileInfo fi(ep);
        QString tag   = fi.isSymLink() ? "[L]" : (fi.isDir() ? "[D]" : "[F]");
        QString extra = fi.isSymLink()
            ? QString(" -> %1 (symlink, access denied)").arg(fi.symLinkTarget())
            : (fi.isFile() ? QString(" (%1 bytes)").arg(fi.size()) : "");
        result << QString("%1 %2%3").arg(tag, name, extra);
    }
    return {result.join('\n'), false};
}

static std::pair<QString,bool> handleListSymbols(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot read: %1").arg(relPath), true};

    QTextStream in(&file);
    QStringList symbols;
    int lineNum = 0;

    QRegularExpression classPattern(R"(^\s*(?:class|struct)\s+(\w+))");
    QRegularExpression methodPattern(R"(^(\w[\w\s\*&:<>]*)\s+(\w+::)?(\w+)\s*\()");
    static const QStringList keywords = {"if","while","for","switch","return",
                                          "else","catch","case","do","sizeof"};
    while (!in.atEnd()) {
        ++lineNum;
        QString line = in.readLine();
        auto cm = classPattern.match(line);
        if (cm.hasMatch()) {
            symbols << QString("L%1  [class]  %2").arg(lineNum,4).arg(cm.captured(1));
            continue;
        }
        if (!line.isEmpty() && !line[0].isSpace() && line.contains('(')) {
            auto mm = methodPattern.match(line);
            if (mm.hasMatch()) {
                QString name = mm.captured(3);
                if (!keywords.contains(name)) {
                    QString scope = mm.captured(2);
                    symbols << QString("L%1  %2  %3%4")
                               .arg(lineNum,4)
                               .arg(scope.isEmpty() ? "[func]" : "[method]")
                               .arg(scope).arg(name);
                }
            }
        }
    }
    if (symbols.isEmpty())
        return {QString("No symbols found in '%1'.").arg(relPath), false};
    return {QString("Symbols in '%1':\n%2").arg(relPath).arg(symbols.join('\n')), false};
}

static std::pair<QString,bool> handleMkdir(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    if (QDir(fullPath).exists())
        return {QString("OK: '%1' already exists.").arg(relPath), false};
    if (QDir().mkpath(fullPath))
        return {QString("OK: directory '%1' created.").arg(relPath), false};
    return {QString("Error: Could not create '%1'.").arg(relPath), true};
}

static std::pair<QString,bool> handleGrepCode(const QJsonObject &args)
{
    QString pattern   = args.value("pattern").toString();
    QString relPath   = args.value("path").toString(".");
    QString extFilter = args.value("extension").toString();
    int maxHits       = args.value("max_hits").toInt(MAX_GREP_HITS);
    maxHits = qBound(1, maxHits, 500);

    if (pattern.isEmpty()) return {"Error: 'pattern' is required.", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QRegularExpression re(pattern);
    if (!re.isValid())
        return {QString("Error: invalid regex: %1").arg(re.errorString()), true};

    QStringList extensions;
    if (!extFilter.isEmpty()) {
        for (const QString &ext : extFilter.split(',', Qt::SkipEmptyParts))
            extensions << ("*." + ext.trimmed());
    }

    QStringList nameFilters = extensions.isEmpty()
        ? QStringList{"*"}
        : extensions;

    QDirIterator it(fullPath, nameFilters,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    QStringList results;
    int totalHits = 0;
    bool truncated = false;

    while (it.hasNext() && !truncated) {
        QString filePath = it.next();
        if (isSymlinkInPath(filePath)) continue;

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        QString relFilePath = filePath;
        if (relFilePath.startsWith(sandboxRoot() + "/"))
            relFilePath = relFilePath.mid(sandboxRoot().length() + 1);

        QTextStream in(&file);
        int lineNum = 0;

        while (!in.atEnd()) {
            ++lineNum;
            QString line = in.readLine();
            auto match = re.match(line);
            if (match.hasMatch()) {
                QString display = line.trimmed();
                if (display.length() > 120)
                    display = display.left(120) + "...";
                results << QString("%1:%2: %3")
                           .arg(relFilePath)
                           .arg(lineNum)
                           .arg(display);
                ++totalHits;
                if (totalHits >= maxHits) {
                    truncated = true;
                    break;
                }
            }
        }
    }

    if (results.isEmpty())
        return {QString("No matches for '%1'.").arg(pattern), false};

    QString output = results.join('\n');
    if (truncated)
        output += QString("\n[... truncated at %1 hits. Use max_hits or a "
                          "more specific pattern.]").arg(maxHits);
    output = QString("grep '%1' — %2 hit(s):\n%3")
             .arg(pattern).arg(totalHits).arg(output);
    return {output, false};
}

static void buildTree(const QString &dirPath, const QString &prefix,
                      QStringList &lines, int depth, int maxDepth)
{
    if (depth > maxDepth) {
        lines << prefix + "...";
        return;
    }

    QDir dir(dirPath);
    QStringList entries = dir.entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::Name | QDir::DirsFirst);
    entries.removeAll(".git");

    for (int i = 0; i < entries.size(); ++i) {
        bool isLast   = (i == entries.size() - 1);
        QString name  = entries[i];
        QString full  = dirPath + "/" + name;
        QFileInfo fi(full);

        if (fi.isSymLink()) {
            lines << prefix + (isLast ? "└── " : "├── ")
                     + name + " -> " + fi.symLinkTarget() + " [symlink]";
            continue;
        }

        if (fi.isDir()) {
            lines << prefix + (isLast ? "└── " : "├── ") + name + "/";
            QString childPrefix = prefix + (isLast ? "    " : "│   ");
            buildTree(full, childPrefix, lines, depth + 1, maxDepth);
        } else {
            QString sizeStr = QString(" (%1b)").arg(fi.size());
            lines << prefix + (isLast ? "└── " : "├── ") + name + sizeStr;
        }
    }
}

static std::pair<QString,bool> handleTree(const QJsonObject &args)
{
    QString relPath = args.value("path").toString(".");
    int maxDepth    = args.value("max_depth").toInt(MAX_TREE_DEPTH);
    maxDepth = qBound(1, maxDepth, 10);

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    if (!QDir(fullPath).exists())
        return {"Error: Directory does not exist.", true};

    QStringList lines;
    QString rootName = relPath == "." ? "." : QFileInfo(fullPath).fileName();
    lines << rootName + "/";
    buildTree(fullPath, "", lines, 1, maxDepth);

    return {lines.join('\n'), false};
}

static std::pair<QString,bool> handleGitStatus(const QJsonObject &args)
{
    QString relPath  = args.value("path").toString(".");
    QString fullPath = resolvePath(relPath);
    QString repoPath = gitRepoForPath(fullPath);

    if (!QDir(repoPath + "/.git").exists())
        return {QString("No git repository for '%1'. "
                        "Use /init or write a file to auto-init.").arg(relPath), true};

    auto [out, code] = runGitIn(repoPath, {"status", "--short", "--branch"});
    return {QString("Repo: %1\n%2").arg(repoPath, out.isEmpty() ? "Working tree clean." : out),
            code != 0};
}

static std::pair<QString,bool> handleGitDiff(const QJsonObject &args)
{
    QString relPath  = args.value("path").toString(".");
    QString fullPath = resolvePath(relPath);
    QString repoPath = gitRepoForPath(fullPath);

    if (!QDir(repoPath + "/.git").exists())
        return {"No git repository found.", true};

    QString from = args.value("from").toString();
    QString to   = args.value("to").toString();
    QString file = args.value("file").toString();
    bool statOnly = args.value("stat_only").toBool(false);

    QStringList gitArgs = {"diff"};
    if (statOnly) gitArgs << "--stat";

    if (!from.isEmpty() && !to.isEmpty())
        gitArgs << QString("%1..%2").arg(from, to);
    else if (!from.isEmpty())
        gitArgs << from;
    else
        gitArgs << "HEAD";

    if (!file.isEmpty()) {
        QString fullFile = resolvePath(file);
        QString reason;
        if (!isPathAllowed(fullFile, reason)) return {reason, true};
        gitArgs << "--" << fullFile;
    }

    auto [out, code] = runGitIn(repoPath, gitArgs);
    if (out.isEmpty()) return {"No differences found.", false};

    if (out.length() > 8000)
        out = out.left(8000) + "\n[... truncated. Use stat_only:true for summary.]";

    return {out, code != 0};
}

static std::pair<QString,bool> handleGitLog(const QJsonObject &args)
{
    QString relPath  = args.value("path").toString(".");
    QString fullPath = resolvePath(relPath);
    QString repoPath = gitRepoForPath(fullPath);

    if (!QDir(repoPath + "/.git").exists())
        return {"No git repository found.", true};

    int n = qBound(1, args.value("n").toInt(10), 50);

    auto [out, code] = runGitIn(repoPath, {"log", "--oneline", QString("-%1").arg(n)});
    if (out.isEmpty()) return {"No commits yet.", false};
    return {out, code != 0};
}

static std::pair<QString,bool> handleGitCheckout(const QJsonObject &args)
{
    QString relPath  = args.value("path").toString(".");
    QString fullPath = resolvePath(relPath);
    QString repoPath = gitRepoForPath(fullPath);

    if (!QDir(repoPath + "/.git").exists())
        return {"No git repository found.", true};

    QString ref  = args.value("ref").toString("HEAD~1");
    QString file = args.value("file").toString();

    if (ref.contains("origin") || ref.contains("upstream") || ref.startsWith("refs/remotes"))
        return {"Error: remote refs are not allowed.", true};

    QStringList gitArgs = {"checkout", ref};
    if (!file.isEmpty()) {
        QString fullFile = resolvePath(file);
        QString reason;
        if (!isPathAllowed(fullFile, reason)) return {reason, true};
        gitArgs << "--" << fullFile;
    }

    auto [out, code] = runGitIn(repoPath, gitArgs);
    QString msg = code == 0
        ? QString("OK: checked out '%1'%2 in %3.")
          .arg(ref, file.isEmpty() ? "" : " -- " + file, repoPath)
        : out;
    return {msg, code != 0};
}

// ─── NEUES TOOL: patch_file ─────────────────────────────────────────────────
static std::pair<QString,bool> handlePatchFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString patch   = args.value("diff").toString();

    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};
    if (patch.isEmpty())   return {"Error: 'diff' is required (unified diff).", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot read file: %1").arg(relPath), true};

    QString original = QTextStream(&file).readAll();
    file.close();

    // Auto-Commit VOR der Änderung
    autoCommit(fullPath, QString("before patch_file %1").arg(relPath));

    QStringList origLines = original.split('\n', Qt::KeepEmptyParts);
    QStringList patchedLines = origLines;
    QStringList patchLines = patch.split('\n', Qt::KeepEmptyParts);

    int i = 0;
    int appliedHunks = 0;
    int totalHunks = 0;

    while (i < patchLines.size()) {
        QString line = patchLines[i];

        // Überspringe Header-Zeilen wie --- +++ 
        if (line.startsWith("---") || line.startsWith("+++") || line.startsWith("index ")) {
            ++i;
            continue;
        }

        if (!line.startsWith("@@ -")) {
            ++i;
            continue;
        }

        totalHunks++;

        QRegularExpression re(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)");
        auto m = re.match(line);
        if (!m.hasMatch()) {
            ++i; continue;
        }

        int oldStart = m.captured(1).toInt() - 1;
        ++i;

        QStringList hunk;
        while (i < patchLines.size() && !patchLines[i].startsWith("@@ ")) {
            hunk << patchLines[i];
            ++i;
        }

        // Hunk anwenden mit strenger Context-Prüfung
        int cur = oldStart;
        bool hunkApplied = true;

        for (const QString &h : hunk) {
            if (h.startsWith(' ') || h.startsWith('-')) {        // Context oder Delete
                QString expected = h.mid(1);
                if (cur >= patchedLines.size() || patchedLines[cur] != expected) {
                    return {QString("Patch conflict in %1 at line %2\n"
                                    "Expected: %3\n"
                                    "Actual:   %4\n"
                                    "Hunk failed. Please generate a new diff with more/fresher context.")
                                .arg(relPath).arg(cur + 1)
                                .arg(expected.left(100))
                                .arg(patchedLines.value(cur).left(100)), true};
                }
                if (h.startsWith('-')) {
                    patchedLines.removeAt(cur);   // Zeile löschen
                    continue;
                }
                ++cur;
            }
            else if (h.startsWith('+')) {                         // Insert
                patchedLines.insert(cur, h.mid(1));
                ++cur;
            }
        }
        appliedHunks++;
    }

    if (totalHunks == 0)
        return {"Error: No valid @@ hunks found in the diff.", true};

    if (appliedHunks < totalHunks)
        return {QString("Partial failure: Only %1 of %2 hunks applied.")
                    .arg(appliedHunks).arg(totalHunks), true};

    // Erfolgreich → schreiben
    QString newContent = patchedLines.join('\n');

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {"Error: Cannot write patched file.", true};

    QTextStream(&file) << newContent;
    file.close();

    // Auto-Commit NACH der Änderung
    autoCommit(fullPath, QString("patch_file %1").arg(relPath));

    return {QString("OK: Successfully applied patch with %1 hunk(s) to '%2'\n"
                    "Lines: %3 → %4")
                .arg(appliedHunks).arg(relPath)
                .arg(origLines.size()).arg(patchedLines.size()), false};
}

// ─── Tool-Liste ──────────────────────────────────────────────────────────────
static QJsonArray makeToolList()
{
    auto makeProp = [](const QString &type, const QString &desc) {
        return QJsonObject{{"type", type}, {"description", desc}};
    };
    auto makeTool = [](const QString &name, const QString &desc,
                       const QJsonObject &props,
                       const QJsonArray &required = {}) {
        return QJsonObject{
            {"name", name}, {"description", desc},
            {"inputSchema", QJsonObject{
                {"type","object"}, {"properties", props}, {"required", required}
            }}
        };
    };

    return QJsonArray{
        makeTool("read_file",
            "Read a file from the sandbox (~/llamatools/). "
            "Optional line range. Without range: max 4096 chars. "
            "With range: max 200 lines. Symlinks rejected.",
            {{"path",       makeProp("string",  "Path relative to sandbox root")},
             {"start_line", makeProp("integer", "First line (1-based, optional)")},
             {"end_line",   makeProp("integer", "Last line (optional)")}},
            {"path"}),

        makeTool("write_file",
            "Write or overwrite a file. Max 8192 chars. "
            "Auto-commits to git before and after writing. "
            "Prefer str_replace for editing existing files.",
            {{"path",    makeProp("string", "Path relative to sandbox root")},
             {"content", makeProp("string", "File content")}},
            {"path","content"}),

        makeTool("append_file",
            "Append text to a file (or create it). Max 8192 chars per call. "
            "Auto-commits to git.",
            {{"path",    makeProp("string", "Path relative to sandbox root")},
             {"content", makeProp("string", "Content to append")}},
            {"path","content"}),

        makeTool("str_replace",
            "Replace a unique text block in a file. "
            "old_str must appear EXACTLY ONCE. Auto-commits to git. "
            "Preferred tool for editing existing files.",
            {{"path",    makeProp("string", "Path relative to sandbox root")},
             {"old_str", makeProp("string", "Text to replace (must be unique)")},
             {"new_str", makeProp("string", "Replacement (empty = delete)")}},
            {"path","old_str"}),

        makeTool("list_dir",
            "List files and directories. Symlinks shown as [L].",
            {{"path", makeProp("string", "Path relative to sandbox root (default: root)")}},
            {}),

        makeTool("list_symbols",
            "Extract C++ classes and methods from a source file. "
            "Returns line numbers for use with read_file start_line.",
            {{"path", makeProp("string", "Path to .h or .cpp file")}},
            {"path"}),

        makeTool("mkdir",
            "Create a directory including parents (like mkdir -p).",
            {{"path", makeProp("string", "Path relative to sandbox root")}},
            {"path"}),

        makeTool("grep_code",
            "Search for a regex pattern recursively in sandbox files. "
            "Returns filename:line:content for each match. "
            "Use extension to filter by file type (e.g. 'cpp' or 'h,cpp').",
            {{"pattern",   makeProp("string",  "Regular expression to search for")},
             {"path",      makeProp("string",  "Start directory (default: sandbox root)")},
             {"extension", makeProp("string",  "File extensions to search, comma-separated (e.g. 'cpp,h')")},
             {"max_hits",  makeProp("integer", "Max results (default: 100)")}},
            {"pattern"}),

        makeTool("tree",
            "Show a recursive directory tree of the sandbox. "
            "Useful to get an overview of a project structure. "
            ".git is hidden. Symlinks are shown but not followed.",
            {{"path",      makeProp("string",  "Start path (default: sandbox root)")},
             {"max_depth", makeProp("integer", "Max depth (default: 6, max: 10)")}},
            {}),

        makeTool("git_status",
            "Show git status of the sandbox repository.",
            {}, {}),

        makeTool("git_diff",
            "Show git diff in the sandbox. "
            "Default: working tree vs last commit (HEAD).",
            {{"from",      makeProp("string",  "Start ref (commit hash, HEAD~1, etc.)")},
             {"to",        makeProp("string",  "End ref (optional)")},
             {"file",      makeProp("string",  "Limit diff to this file (optional)")},
             {"stat_only", makeProp("boolean", "true = show only changed files summary")}},
            {}),

        makeTool("git_log",
            "Show recent git commits in the sandbox. "
            "Returns one-line format: hash + message.",
            {{"n", makeProp("integer", "Number of commits to show (default: 10, max: 50)")}},
            {}),

        makeTool("git_checkout",
            "Restore a file or commit in the sandbox. "
            "Remote refs (origin, upstream) are not allowed.",
            {{"ref",  makeProp("string", "Git ref: commit hash, HEAD~1, etc. (default: HEAD~1)")},
             {"file", makeProp("string", "File to restore (optional)")}},
            {}),

        // NEU: patch_file
        makeTool("patch_file",
            "Apply a unified diff (git diff / diff -u format) to a single file. "
            "Supports multiple hunks. Context must match exactly (safe). "
            "Auto-commits before and after. Best tool for complex edits by LLM.",
            {{"path", makeProp("string", "Path relative to sandbox root")},
             {"diff", makeProp("string", "The full unified diff text (including @@ hunks)")}},
            {"path", "diff"})
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
        if (parseErr.error != QJsonParseError::NoError) {
            std::cerr << "JSON parse error: " << parseErr.errorString().toStdString() << std::endl;
            continue;
        }

        QJsonObject msg = doc.object();
        QString method = msg.value("method").toString();
        int id = msg.value("id").toInt(-1);

        if (method == "initialize") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                {"protocolVersion","2024-11-05"},
                {"capabilities", QJsonObject{}},
                {"serverInfo", QJsonObject{{"name","llamaqt-filesystem"},{"version","2.1"}}}
            }}});
            continue;
        }
        if (method == "notifications/initialized") continue;

        if (method == "tools/list") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},
                          {"result",QJsonObject{{"tools",tools}}}});
            continue;
        }

        if (method == "tools/call") {
            QJsonObject params   = msg.value("params").toObject();
            QString toolName     = params.value("name").toString();
            QJsonObject toolArgs = params.value("arguments").toObject();

            std::pair<QString,bool> result;

            if      (toolName == "read_file")     result = handleReadFile(toolArgs);
            else if (toolName == "write_file")    result = handleWriteFile(toolArgs);
            else if (toolName == "append_file")   result = handleAppendFile(toolArgs);
            else if (toolName == "str_replace")   result = handleStrReplace(toolArgs);
            else if (toolName == "list_dir")      result = handleListDir(toolArgs);
            else if (toolName == "list_symbols")  result = handleListSymbols(toolArgs);
            else if (toolName == "mkdir")         result = handleMkdir(toolArgs);
            else if (toolName == "grep_code")     result = handleGrepCode(toolArgs);
            else if (toolName == "tree")          result = handleTree(toolArgs);
            else if (toolName == "git_status")    result = handleGitStatus(toolArgs);
            else if (toolName == "git_diff")      result = handleGitDiff(toolArgs);
            else if (toolName == "git_log")       result = handleGitLog(toolArgs);
            else if (toolName == "git_checkout")  result = handleGitCheckout(toolArgs);
            else if (toolName == "patch_file")    result = handlePatchFile(toolArgs);
            else result = {QString("Unknown tool: '%1'").arg(toolName), true};

            sendResult(id, result.first, result.second);
            continue;
        }

        if (id != -1)
            sendError(id, -32601, QString("Method not found: %1").arg(method));
    }
    return 0;
}
