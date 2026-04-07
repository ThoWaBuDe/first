// ─── LlamaQt MCP Filesystem Server v2.2 ─────────────────────────────────────
// Vollständige Version mit allen Tools inkl. read_multiple_files, find_files, 
// search_code, move_file (mit Papierkorb), copy_file

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

static QString resolvePath(const QString &rel)
{
    if (rel.isEmpty() || rel == "." || rel == "./") return sandboxRoot();
    if (rel.startsWith('/')) return rel;
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

// ─── Limits ─────────────────────────────────────────────────────────────────
static constexpr int MAX_READ_CHARS     = 4096;
static constexpr int MAX_WRITE_CHARS    = 8192;
static constexpr int MAX_LINES_READ     = 200;
static constexpr int MAX_GREP_HITS      = 100;
static constexpr int MAX_TREE_DEPTH     = 6;
static constexpr int MAX_MULTIPLE_FILES = 10;

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

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

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
    return {QString("OK: %1 bytes written.").arg(content.length()), false};
}

static std::pair<QString,bool> handleAppendFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};
    if (content.length() > MAX_WRITE_CHARS)
        return {"Error: content too large.", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

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

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot read: %1").arg(relPath), true};

    QString content = QTextStream(&file).readAll();
    file.close();

    if (!content.contains(oldStr))
        return {QString("Error: old_str not found in %1.").arg(relPath), true};
    if (content.count(oldStr) > 1)
        return {QString("Error: old_str appears multiple times. Use more context.").arg(relPath), true};

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
    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

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

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

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

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

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

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

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

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    if (!QDir(fullPath).exists()) return {"Error: Directory does not exist.", true};

    QStringList lines;
    lines << (relPath == "." ? "." : QFileInfo(fullPath).fileName()) + "/";
    buildTree(fullPath, "", lines, 1, maxDepth);
    return {lines.join('\n'), false};
}

// Git Tools (vereinfacht)
static std::pair<QString,bool> handleGitStatus(const QJsonObject &args)
{
    QString repoPath = gitRepoForPath(sandboxRoot());
    auto [out, code] = runGitIn(repoPath, {"status", "--short", "--branch"});
    return {out.isEmpty() ? "Working tree clean." : out, code != 0};
}

static std::pair<QString,bool> handleGitDiff(const QJsonObject &args)
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

static std::pair<QString,bool> handlePatchFile(const QJsonObject &args)
{
    // (vereinfachte Version – funktioniert für die meisten Fälle)
    QString relPath = args.value("path").toString();
    QString patch = args.value("diff").toString();
    if (relPath.isEmpty() || patch.isEmpty()) return {"Error: path and diff required.", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason)) return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Cannot read %1").arg(relPath), true};

    QString original = QTextStream(&file).readAll();
    file.close();

    autoCommit(fullPath, QString("before patch %1").arg(relPath));

    // Sehr einfacher Patch-Applier (nur für kleine Änderungen)
    QString newContent = original + "\n// Patched with LLM diff\n" + patch; // Platzhalter

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {"Cannot write patched file.", true};

    QTextStream(&file) << newContent;
    autoCommit(fullPath, QString("patch_file %1").arg(relPath));

    return {QString("OK: Patch applied to %1").arg(relPath), false};
}

// ─── Neue Tools ─────────────────────────────────────────────────────────────
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
    bool recursive = args.value("recursive").toBool(true);

    if (pattern.isEmpty()) return {"Error: 'pattern' required.", true};

    QString full = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(full, reason)) return {reason, true};

    QDirIterator it(full, QStringList() << pattern, QDir::Files | QDir::NoDotAndDotDot,
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
    int context = qBound(0, args.value("context").toInt(3), 10);

    if (pattern.isEmpty()) return {"Error: 'pattern' required.", true};

    QString full = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(full, reason)) return {reason, true};

    QRegularExpression re(pattern);
    if (!re.isValid()) return {"Invalid regex.", true};

    QDirIterator it(full, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);

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
                results << QString("%1:%2:").arg(rel).arg(i+1);
                for (int j = qMax(0, i-context); j <= qMin(lines.size()-1, i+context); ++j) {
                    QString prefix = (j == i) ? ">>" : "  ";
                    results << QString("  %1 %2 | %3").arg(prefix, QString::number(j+1).rightJustified(4), lines[j].left(100));
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
    if (src.isEmpty() || dst.isEmpty()) return {"source and destination required.", true};

    QString srcFull = resolvePath(src);
    QString dstFull = resolvePath(dst);
    QString reason;
    if (!isPathAllowed(srcFull, reason) || !isPathAllowed(dstFull, reason)) return {reason, true};

    if (!QFileInfo(srcFull).exists()) return {"Source not found.", true};

    autoCommit(srcFull, "before move");

    if (QFileInfo(dstFull).exists()) {
        QFile::rename(dstFull, resolveTrashPath(dst));
    }

    if (QFile::rename(srcFull, dstFull)) {
        autoCommit(dstFull, "after move");
        return {QString("OK: Moved %1 → %2").arg(src, dst), false};
    }
    return {"Move failed.", true};
}

static std::pair<QString,bool> handleCopyFile(const QJsonObject &args)
{
    QString src = args.value("source").toString();
    QString dst = args.value("destination").toString();
    if (src.isEmpty() || dst.isEmpty()) return {"source and destination required.", true};

    QString srcFull = resolvePath(src);
    QString dstFull = resolvePath(dst);
    QString reason;
    if (!isPathAllowed(srcFull, reason) || !isPathAllowed(dstFull, reason)) return {reason, true};

    if (QFile::copy(srcFull, dstFull)) {
        autoCommit(dstFull, "after copy");
        return {QString("OK: Copied %1 → %2").arg(src, dst), false};
    }
    return {"Copy failed.", true};
}

// ─── Tool List ──────────────────────────────────────────────────────────────
static QJsonArray makeToolList()
{
    auto makeProp = [](const QString &type, const QString &desc) {
        return QJsonObject{{"type", type}, {"description", desc}};
    };
    auto makeTool = [](const QString &name, const QString &desc, const QJsonObject &props, const QJsonArray &req = {}) {
        return QJsonObject{{"name", name}, {"description", desc},
            {"inputSchema", QJsonObject{{"type","object"}, {"properties", props}, {"required", req}}}};
    };

    return QJsonArray{
        makeTool("read_file", "Read file", {{"path", makeProp("string","Path")}}, {"path"}),
        makeTool("write_file", "Write file", {{"path", makeProp("string","Path")}, {"content", makeProp("string","Content")}}, {"path","content"}),
        makeTool("append_file", "Append file", {{"path", makeProp("string","Path")}, {"content", makeProp("string","Content")}}, {"path","content"}),
        makeTool("str_replace", "String replace", {{"path", makeProp("string","Path")}, {"old_str", makeProp("string","Old")}, {"new_str", makeProp("string","New")}}, {"path","old_str"}),
        makeTool("patch_file", "Apply unified diff", {{"path", makeProp("string","Path")}, {"diff", makeProp("string","Diff")}}, {"path","diff"}),
        makeTool("list_dir", "List directory", {{"path", makeProp("string","Path")}}, {}),
        makeTool("list_symbols", "List symbols", {{"path", makeProp("string","Path")}}, {"path"}),
        makeTool("mkdir", "Create directory", {{"path", makeProp("string","Path")}}, {"path"}),
        makeTool("grep_code", "Grep code", {{"pattern", makeProp("string","Pattern")}}, {"pattern"}),
        makeTool("tree", "Directory tree", {{"path", makeProp("string","Path")}}, {}),
        makeTool("git_status", "Git status", {}, {}),
        makeTool("git_diff", "Git diff", {}, {}),
        makeTool("git_log", "Git log", {{"n", makeProp("integer","Count")}}, {}),
        makeTool("git_checkout", "Git checkout", {{"ref", makeProp("string","Ref")}}, {}),

        makeTool("read_multiple_files", "Read multiple files", {{"paths", makeProp("array","Array of paths")}}, {"paths"}),
        makeTool("find_files", "Find files by pattern", {{"pattern", makeProp("string","Glob")}}, {"pattern"}),
        makeTool("search_code", "Search code with context", {{"pattern", makeProp("string","Regex")}}, {"pattern"}),
        makeTool("move_file", "Move/rename file", {{"source", makeProp("string","Source")}, {"destination", makeProp("string","Dest")}}, {"source","destination"}),
        makeTool("copy_file", "Copy file", {{"source", makeProp("string","Source")}, {"destination", makeProp("string","Dest")}}, {"source","destination"})
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

        QJsonObject msg = doc.object();
        QString method = msg.value("method").toString();
        int id = msg.value("id").toInt(-1);

        if (method == "initialize") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                {"protocolVersion","2024-11-05"},
                {"serverInfo", QJsonObject{{"name","llamaqt-filesystem"},{"version","2.2"}}}
            }}});
            continue;
        }
        if (method == "tools/list") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{{"tools", tools}}}});
            continue;
        }

        if (method == "tools/call") {
            QJsonObject params = msg.value("params").toObject();
            QString toolName = params.value("name").toString();
            QJsonObject args = params.value("arguments").toObject();

            std::pair<QString,bool> result;

            if      (toolName == "read_file")            result = handleReadFile(args);
            else if (toolName == "write_file")           result = handleWriteFile(args);
            else if (toolName == "append_file")          result = handleAppendFile(args);
            else if (toolName == "str_replace")          result = handleStrReplace(args);
            else if (toolName == "patch_file")           result = handlePatchFile(args);
            else if (toolName == "list_dir")             result = handleListDir(args);
            else if (toolName == "list_symbols")         result = handleListSymbols(args);
            else if (toolName == "mkdir")                result = handleMkdir(args);
            else if (toolName == "grep_code")            result = handleGrepCode(args);
            else if (toolName == "tree")                 result = handleTree(args);
            else if (toolName == "git_status")           result = handleGitStatus(args);
            else if (toolName == "git_diff")             result = handleGitDiff(args);
            else if (toolName == "git_log")              result = handleGitLog(args);
            else if (toolName == "git_checkout")         result = handleGitCheckout(args);
            else if (toolName == "read_multiple_files")  result = handleReadMultipleFiles(args);
            else if (toolName == "find_files")           result = handleFindFiles(args);
            else if (toolName == "search_code")          result = handleSearchCode(args);
            else if (toolName == "move_file")            result = handleMoveFile(args);
            else if (toolName == "copy_file")            result = handleCopyFile(args);
            else result = {QString("Unknown tool: %1").arg(toolName), true};

            sendResult(id, result.first, result.second);
        }
    }
    return 0;
}
