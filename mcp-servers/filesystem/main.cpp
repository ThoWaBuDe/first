// ─── LlamaQt MCP Filesystem Server ───────────────────────────────────────────
// Implements the MCP stdio protocol for file operations inside a sandbox.
//
// Transport: newline-delimited JSON over stdin/stdout.
// The client (McpClient) launches this process via QProcess and communicates
// over standard streams. No TCP, no HTTP.
//
// Security model:
//   - All paths are resolved relative to ~/llamatools/ (sandbox root)
//   - Absolute paths are rejected unless they start with the sandbox root
//   - Symlinks are NEVER followed — a symlink inside the sandbox could point
//     to /etc/passwd or any sensitive file outside the sandbox.
//     Every path component is checked with QFileInfo::isSymLink().
//
// Advertised Tools:
//   read_file, write_file, append_file, str_replace, list_dir,
//   list_symbols, mkdir

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
// Checks every component of the path for symlinks.
//
// Why check every component and not just the final path?
// A symlink can appear at any level:
//   ~/llamatools/subdir -> /etc/        (directory symlink)
//   ~/llamatools/subdir/passwd          (now reads /etc/passwd)
//
// QFileInfo::isSymLink() checks only the final component. We walk the full
// path component by component so no intermediate symlink is missed.
//
// Returns true if any component is a symlink (= path is unsafe).
static bool isSymlinkInPath(const QString &absolutePath)
{
    // Walk from root to the full path, checking each prefix.
    // Example: /home/thomas/llamatools/sub/file
    //   checks: /home
    //           /home/thomas
    //           /home/thomas/llamatools
    //           /home/thomas/llamatools/sub
    //           /home/thomas/llamatools/sub/file
    QStringList parts = absolutePath.split('/', Qt::SkipEmptyParts);
    QString current;
    for (const QString &part : parts) {
        current += "/" + part;
        QFileInfo fi(current);
        if (fi.isSymLink())
            return true;
    }
    return false;
}

// ─── isPathAllowed ────────────────────────────────────────────────────────────
// Two checks:
//   1. The resolved absolute path must be inside the sandbox root.
//   2. No symlink anywhere in the path.
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
        reason = QString("Symlink detected in path — following symlinks is "
                         "not allowed (potential sandbox escape): %1").arg(path);
        return false;
    }

    return true;
}

// ─── Limits ──────────────────────────────────────────────────────────────────
static constexpr int MAX_READ_CHARS  = 4096;
static constexpr int MAX_WRITE_CHARS = 8192;
static constexpr int MAX_LINES_READ  = 200;

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

// ─── Tool handlers ───────────────────────────────────────────────────────────

static std::pair<QString,bool> handleReadFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason))
        return {reason, true};

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
        int currentLine = 0;
        result += QString("// %1 [lines %2-%3]\n").arg(relPath).arg(startLine).arg(endLine);
        while (!in.atEnd()) {
            ++currentLine;
            QString line = in.readLine();
            if (currentLine >= startLine && currentLine <= endLine)
                result += QString("%1: %2\n").arg(currentLine, 4).arg(line);
            if (currentLine > endLine) break;
        }
        if (currentLine < startLine)
            return {QString("Error: File has only %1 lines.").arg(currentLine), true};
    } else {
        result = in.readAll();
        if (result.length() > MAX_READ_CHARS) {
            int cut = result.lastIndexOf('\n', MAX_READ_CHARS);
            if (cut < 0) cut = MAX_READ_CHARS;
            int lines = result.left(cut).count('\n') + 1;
            result = result.left(cut);
            result += QString("\n[... truncated. Continue reading with start_line=%1]")
                      .arg(lines + 1);
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
        return {QString("Error: content too large (%1 chars, max %2). "
                        "Use str_replace or append_file instead.")
                .arg(content.length()).arg(MAX_WRITE_CHARS), true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason))
        return {reason, true};

    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    QFile file(fullPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {QString("Error: Cannot write file: %1").arg(relPath), true};

    QTextStream(&file) << content;
    return {QString("OK: %1 bytes written to '%2'.").arg(content.length()).arg(relPath), false};
}

static std::pair<QString,bool> handleAppendFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();
    if (relPath.isEmpty()) return {"Error: 'path' is required.", true};
    if (content.length() > MAX_WRITE_CHARS)
        return {QString("Error: content too large (%1 chars, max %2).")
                .arg(content.length()).arg(MAX_WRITE_CHARS), true};

    QString fullPath = resolvePath(relPath);
    QString reason;
    if (!isPathAllowed(fullPath, reason))
        return {reason, true};

    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    QFile file(fullPath);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return {QString("Error: Cannot append to file: %1").arg(relPath), true};

    QTextStream(&file) << content;
    qint64 totalSize = file.size();
    return {QString("OK: %1 bytes appended to '%2' (total: %3 bytes).")
            .arg(content.length()).arg(relPath).arg(totalSize), false};
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
    if (!isPathAllowed(fullPath, reason))
        return {reason, true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Error: Cannot read file: %1").arg(relPath), true};
    QString content = QTextStream(&file).readAll();
    file.close();

    int count = 0, pos = 0;
    while ((pos = content.indexOf(oldStr, pos)) != -1) { ++count; pos += oldStr.length(); }

    if (count == 0)
        return {QString("Error: 'old_str' not found in '%1'.").arg(relPath), true};
    if (count > 1)
        return {QString("Error: 'old_str' found %1 times in '%2' — not unique. "
                        "Add more surrounding context.").arg(count).arg(relPath), true};

    content.replace(oldStr, newStr);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {"Error: Cannot write file.", true};
    QTextStream(&file) << content;

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
        QString entryPath = fullPath + "/" + name;
        QFileInfo fi(entryPath);

        // Mark symlinks explicitly — never follow them silently
        QString typeTag;
        if (fi.isSymLink())       typeTag = "[L]";  // symlink — do not use
        else if (fi.isDir())      typeTag = "[D]";
        else                      typeTag = "[F]";

        QString extra;
        if (fi.isSymLink())
            extra = QString(" -> %1 (symlink, access denied)").arg(fi.symLinkTarget());
        else if (fi.isFile())
            extra = QString(" (%1 bytes)").arg(fi.size());

        result << QString("%1 %2%3").arg(typeTag, name, extra);
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
        return {QString("Error: Cannot read file: %1").arg(relPath), true};

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
    if (!isPathAllowed(fullPath, reason))
        return {reason, true};

    if (QDir(fullPath).exists())
        return {QString("OK: '%1' already exists.").arg(relPath), false};

    if (QDir().mkpath(fullPath))
        return {QString("OK: directory '%1' created.").arg(relPath), false};

    return {QString("Error: Could not create '%1'.").arg(relPath), true};
}

// ─── Tool list ───────────────────────────────────────────────────────────────
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
            "Optionally read only a line range. "
            "Without range: max 4096 chars. With range: max 200 lines. "
            "If output is truncated, use start_line to continue. "
            "Symlinks are rejected for security.",
            {{"path",       makeProp("string",  "Path relative to sandbox root")},
             {"start_line", makeProp("integer", "First line to read (1-based, optional)")},
             {"end_line",   makeProp("integer", "Last line to read (optional)")}},
            {"path"}),

        makeTool("write_file",
            "Write or overwrite a file in the sandbox. Max 8192 chars. "
            "Prefer str_replace for editing existing files. "
            "For large files: write_file + append_file in chunks. "
            "Symlinks are rejected.",
            {{"path",    makeProp("string", "Path relative to sandbox root")},
             {"content", makeProp("string", "File content")}},
            {"path","content"}),

        makeTool("append_file",
            "Append text to an existing file (or create it). Max 8192 chars per call. "
            "Useful for writing large files in chunks. Symlinks are rejected.",
            {{"path",    makeProp("string", "Path relative to sandbox root")},
             {"content", makeProp("string", "Content to append")}},
            {"path","content"}),

        makeTool("str_replace",
            "Replace a unique text block in a file. "
            "old_str must appear EXACTLY ONCE in the file — otherwise an error is returned. "
            "Preferred tool for editing existing files. "
            "Set new_str to empty string to delete. Symlinks are rejected.",
            {{"path",    makeProp("string", "Path relative to sandbox root")},
             {"old_str", makeProp("string", "Text to replace (must be unique in file)")},
             {"new_str", makeProp("string", "Replacement text (empty = delete)")}},
            {"path","old_str"}),

        makeTool("list_dir",
            "List files and directories in the sandbox. "
            "Symlinks are shown as [L] and their targets are not accessible.",
            {{"path", makeProp("string", "Path relative to sandbox root (default: root)")}},
            {}),

        makeTool("list_symbols",
            "Extract C++ classes and methods from a source file. "
            "Returns line numbers usable as start_line for read_file. "
            "Useful for navigation before reading a specific function.",
            {{"path", makeProp("string", "Path to .h or .cpp file")}},
            {"path"}),

        makeTool("mkdir",
            "Create a directory including all parent directories (like mkdir -p). "
            "Symlinks in the path are rejected.",
            {{"path", makeProp("string", "Path relative to sandbox root")}},
            {"path"})
    };
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QDir().mkpath(sandboxRoot());

    QJsonArray tools = makeToolList();
    QTextStream in(stdin);
    QTextStream err(stderr);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            err << "JSON parse error: " << parseErr.errorString() << "\n";
            err.flush();
            continue;
        }

        QJsonObject msg = doc.object();
        QString method  = msg.value("method").toString();
        bool hasId      = msg.contains("id");
        int  id         = msg.value("id").toInt(-1);

        if (method == "initialize") {
            sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",QJsonObject{
                {"protocolVersion","2024-11-05"},
                {"capabilities",   QJsonObject{}},
                {"serverInfo",     QJsonObject{{"name","llamaqt-filesystem"},{"version","1.1"}}}
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
            if      (toolName == "read_file")    result = handleReadFile(toolArgs);
            else if (toolName == "write_file")   result = handleWriteFile(toolArgs);
            else if (toolName == "append_file")  result = handleAppendFile(toolArgs);
            else if (toolName == "str_replace")  result = handleStrReplace(toolArgs);
            else if (toolName == "list_dir")     result = handleListDir(toolArgs);
            else if (toolName == "list_symbols") result = handleListSymbols(toolArgs);
            else if (toolName == "mkdir")        result = handleMkdir(toolArgs);
            else result = {QString("Unknown tool: '%1'").arg(toolName), true};

            sendResult(id, result.first, result.second);
            continue;
        }

        if (hasId) sendError(id, -32601, QString("Method not found: %1").arg(method));
    }
    return 0;
}
