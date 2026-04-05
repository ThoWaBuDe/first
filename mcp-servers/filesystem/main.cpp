// ─── LlamaQt MCP Filesystem Server ───────────────────────────────────────────
// Implementiert das MCP stdio-Protokoll für Dateioperationen.
//
// Transport: newline-delimited JSON über stdin/stdout.
// Der Client (McpClient) startet diesen Prozess via QProcess und kommuniziert
// über die Standard-Streams. Kein TCP, kein HTTP.
//
// Advertised Tools:
//   read_file, write_file, append_file, str_replace, list_dir, list_symbols

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

static bool isPathAllowed(const QString &path)
{
    QFileInfo fi(path);
    QString abs = fi.absoluteFilePath();
    QString absDir = fi.absolutePath();
    return abs.startsWith(sandboxRoot()) || absDir.startsWith(sandboxRoot());
}

// ─── Tool-Handler ─────────────────────────────────────────────────────────────

static constexpr int MAX_READ_CHARS  = 4096;
static constexpr int MAX_WRITE_CHARS = 8192;
static constexpr int MAX_LINES_READ  = 200;

// Gibt {result, isError} zurück
static std::pair<QString,bool> handleReadFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Fehler: 'path' fehlt.", true};

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return {QString("Fehler: Pfad ausserhalb Sandbox: %1").arg(relPath), true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Fehler: Datei nicht lesbar: %1").arg(relPath), true};

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
        result += QString("// %1 [Zeilen %2-%3]\n").arg(relPath).arg(startLine).arg(endLine);
        while (!in.atEnd()) {
            ++currentLine;
            QString line = in.readLine();
            if (currentLine >= startLine && currentLine <= endLine)
                result += QString("%1: %2\n").arg(currentLine, 4).arg(line);
            if (currentLine > endLine) break;
        }
        if (currentLine < startLine)
            return {QString("Fehler: Datei hat nur %1 Zeilen.").arg(currentLine), true};
    } else {
        result = in.readAll();
        if (result.length() > MAX_READ_CHARS) {
            int cut = result.lastIndexOf('\n', MAX_READ_CHARS);
            if (cut < 0) cut = MAX_READ_CHARS;
            int lines = result.left(cut).count('\n') + 1;
            result = result.left(cut);
            result += QString("\n[... abgeschnitten. Weiter mit start_line=%1]").arg(lines + 1);
        }
    }
    return {result, false};
}

static std::pair<QString,bool> handleWriteFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();
    if (relPath.isEmpty()) return {"Fehler: 'path' fehlt.", true};
    if (content.length() > MAX_WRITE_CHARS)
        return {QString("Fehler: content zu gross (%1 Zeichen, max %2). "
                        "Verwende str_replace oder append_file.")
                .arg(content.length()).arg(MAX_WRITE_CHARS), true};

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return {QString("Fehler: Pfad ausserhalb Sandbox."), true};

    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    QFile file(fullPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {QString("Fehler: Datei nicht schreibbar: %1").arg(relPath), true};

    QTextStream(&file) << content;
    return {QString("OK: %1 Bytes in '%2' geschrieben.").arg(content.length()).arg(relPath), false};
}

static std::pair<QString,bool> handleAppendFile(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString content = args.value("content").toString();
    if (relPath.isEmpty()) return {"Fehler: 'path' fehlt.", true};
    if (content.length() > MAX_WRITE_CHARS)
        return {QString("Fehler: content zu gross (%1 Zeichen, max %2).")
                .arg(content.length()).arg(MAX_WRITE_CHARS), true};

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return {"Fehler: Pfad ausserhalb Sandbox.", true};

    QFileInfo fi(fullPath);
    QDir().mkpath(fi.absolutePath());

    // QIODevice::Append: schreibt ans Ende ohne bestehenden Inhalt zu loeschen
    QFile file(fullPath);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return {QString("Fehler: Datei nicht appendierbar: %1").arg(relPath), true};

    QTextStream(&file) << content;
    qint64 totalSize = file.size();
    return {QString("OK: %1 Bytes an '%2' angehaengt (Gesamt: %3 Bytes).")
            .arg(content.length()).arg(relPath).arg(totalSize), false};
}

static std::pair<QString,bool> handleStrReplace(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    QString oldStr  = args.value("old_str").toString();
    QString newStr  = args.value("new_str").toString();
    if (relPath.isEmpty()) return {"Fehler: 'path' fehlt.", true};
    if (oldStr.isEmpty())  return {"Fehler: 'old_str' fehlt.", true};

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return {"Fehler: Pfad ausserhalb Sandbox.", true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Fehler: Datei nicht lesbar: %1").arg(relPath), true};
    QString content = QTextStream(&file).readAll();
    file.close();

    int count = 0, pos = 0;
    while ((pos = content.indexOf(oldStr, pos)) != -1) { ++count; pos += oldStr.length(); }

    if (count == 0)
        return {QString("Fehler: 'old_str' nicht gefunden in '%1'.").arg(relPath), true};
    if (count > 1)
        return {QString("Fehler: 'old_str' %1x gefunden - nicht eindeutig. "
                        "Mehr Kontext angeben.").arg(count), true};

    content.replace(oldStr, newStr);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {QString("Fehler: Datei nicht schreibbar."), true};
    QTextStream(&file) << content;

    return {QString("OK: %1 Zeile(n) ersetzt durch %2 in '%3'.")
            .arg(oldStr.count('\n')+1).arg(newStr.count('\n')+1).arg(relPath), false};
}

static std::pair<QString,bool> handleListDir(const QJsonObject &args)
{
    QString relPath  = args.value("path").toString(".");
    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath)) return {"Fehler: Pfad ausserhalb Sandbox.", true};

    QDir dir(fullPath);
    if (!dir.exists()) return {QString("Fehler: Verzeichnis existiert nicht."), true};

    QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                        QDir::Name | QDir::DirsFirst);
    if (entries.isEmpty()) return {"(leer)", false};

    QStringList result;
    for (const QString &name : entries) {
        QFileInfo fi(fullPath + "/" + name);
        result << QString("%1 %2%3")
                  .arg(fi.isDir() ? "[D]" : "[F]")
                  .arg(name)
                  .arg(fi.isFile() ? QString(" (%1 Bytes)").arg(fi.size()) : QString());
    }
    return {result.join('\n'), false};
}

static std::pair<QString,bool> handleListSymbols(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Fehler: 'path' fehlt.", true};

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath)) return {"Fehler: Pfad ausserhalb Sandbox.", true};

    QFile file(fullPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QString("Fehler: Datei nicht lesbar: %1").arg(relPath), true};

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
        return {QString("Keine Symbole in '%1'.").arg(relPath), false};
    return {QString("Symbole in '%1':\n%2").arg(relPath).arg(symbols.join('\n')), false};
}

// ─── handleMkdir ─────────────────────────────────────────────────────────────
// Erstellt ein Verzeichnis (inkl. alle Parent-Verzeichnisse — wie mkdir -p).
static std::pair<QString,bool> handleMkdir(const QJsonObject &args)
{
    QString relPath = args.value("path").toString();
    if (relPath.isEmpty()) return {"Fehler: 'path' fehlt.", true};

    QString fullPath = resolvePath(relPath);
    if (!isPathAllowed(fullPath))
        return {QString("Fehler: Pfad ausserhalb Sandbox: %1").arg(relPath), true};

    if (QDir(fullPath).exists())
        return {QString("OK: '%1' existiert bereits.").arg(relPath), false};

    if (QDir().mkpath(fullPath))
        return {QString("OK: '%1' erstellt.").arg(relPath), false};

    return {QString("Fehler: Konnte '%1' nicht erstellen.").arg(relPath), true};
}

// ─── Tool-Definitionen (advertised via tools/list) ───────────────────────────
static QJsonArray makeToolList()
{
    auto makeProp = [](const QString &type, const QString &desc) {
        return QJsonObject{{"type", type}, {"description", desc}};
    };

    auto makeTool = [](const QString &name, const QString &desc,
                       const QJsonObject &props,
                       const QJsonArray &required = {}) {
        return QJsonObject{
            {"name", name},
            {"description", desc},
            {"inputSchema", QJsonObject{
                {"type", "object"},
                {"properties", props},
                {"required", required}
            }}
        };
    };

    return QJsonArray{
        makeTool("read_file",
            "Liest eine Datei (optional nur einen Zeilenbereich). "
            "Ohne Range max 4096 Zeichen. Mit Range max 200 Zeilen. "
            "Bei [abgeschnitten] weiter lesen mit start_line=N.",
            {{"path",       makeProp("string", "Relativer Pfad zur Sandbox")},
             {"start_line", makeProp("integer","Erste Zeile (1-basiert, optional)")},
             {"end_line",   makeProp("integer","Letzte Zeile (optional)")}},
            {"path"}),

        makeTool("write_file",
            "Schreibt/ueberschreibt eine Datei. Max 8192 Zeichen. "
            "Fuer Aenderungen str_replace bevorzugen. "
            "Fuer grosse Dateien: write_file + append_file.",
            {{"path",    makeProp("string","Relativer Pfad")},
             {"content", makeProp("string","Dateiinhalt")}},
            {"path","content"}),

        makeTool("append_file",
            "Haengt Text an eine bestehende Datei an (oder erstellt sie neu). "
            "Max 8192 Zeichen pro Aufruf. Ideal fuer grosse Dateien in Bloecken.",
            {{"path",    makeProp("string","Relativer Pfad")},
             {"content", makeProp("string","Anzuhaengender Inhalt")}},
            {"path","content"}),

        makeTool("str_replace",
            "Ersetzt einen eindeutigen Textblock in einer Datei. "
            "old_str muss GENAU EINMAL vorkommen. "
            "Bevorzugtes Tool fuer Aenderungen an bestehenden Dateien. "
            "new_str darf leer sein (= Loeschen).",
            {{"path",    makeProp("string","Relativer Pfad")},
             {"old_str", makeProp("string","Zu ersetzender Text (eindeutig!)")},
             {"new_str", makeProp("string","Ersatztext (leer = loeschen)")}},
            {"path","old_str"}),

        makeTool("list_dir",
            "Listet Dateien und Verzeichnisse auf.",
            {{"path", makeProp("string","Relativer Pfad (default: Sandbox-Root)")}},
            {}),

        makeTool("list_symbols",
            "Extrahiert C++ Klassen und Methoden aus einer Quelldatei. "
            "Gibt Zeilennummern zurueck - direkt verwendbar als start_line fuer read_file.",
            {{"path", makeProp("string","Relativer Pfad zur .h oder .cpp Datei")}},
            {"path"}),

        makeTool("mkdir",
            "Erstellt ein Verzeichnis (inkl. Parent-Verzeichnisse, wie mkdir -p).",
            {{"path", makeProp("string","Relativer Pfad des neuen Verzeichnisses")}},
            {"path"})
    };
}

// ─── JSON-RPC Hilfsfunktionen ─────────────────────────────────────────────────

static void sendResponse(const QJsonObject &msg)
{
    QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    std::cout << line.toStdString();
    std::cout.flush();
}

static void sendResult(int id, const QString &text, bool isError = false)
{
    QJsonObject result{
        {"content", QJsonArray{QJsonObject{{"type","text"},{"text",text}}}},
        {"isError", isError}
    };
    sendResponse({{"jsonrpc","2.0"},{"id",id},{"result",result}});
}

static void sendError(int id, int code, const QString &message)
{
    sendResponse({
        {"jsonrpc","2.0"},
        {"id",id},
        {"error", QJsonObject{{"code",code},{"message",message}}}
    });
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Sandbox anlegen
    QDir().mkpath(sandboxRoot());

    QJsonArray tools = makeToolList();

    // stdin zeilenweise lesen — das ist die Server-Event-Loop
    // Kein QThread nötig: QTextStream über stdin ist blocking,
    // aber das ist der einzige Job dieses Prozesses.
    QTextStream in(stdin);
    QTextStream err(stderr);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            err << "JSON Parse Error: " << parseErr.errorString() << "\n";
            err.flush();
            continue;
        }

        QJsonObject msg = doc.object();
        QString method  = msg.value("method").toString();
        bool hasId      = msg.contains("id");
        int  id         = msg.value("id").toInt(-1);

        // ─── initialize ──────────────────────────────────────────────────
        if (method == "initialize") {
            sendResponse({
                {"jsonrpc","2.0"}, {"id",id},
                {"result", QJsonObject{
                    {"protocolVersion","2024-11-05"},
                    {"capabilities",   QJsonObject{}},
                    {"serverInfo",     QJsonObject{
                        {"name","llamaqt-filesystem"},
                        {"version","1.0"}
                    }}
                }}
            });
            continue;
        }

        // ─── notifications/initialized ───────────────────────────────────
        if (method == "notifications/initialized") {
            // Keine Antwort nötig (Notification)
            continue;
        }

        // ─── tools/list ──────────────────────────────────────────────────
        if (method == "tools/list") {
            sendResponse({
                {"jsonrpc","2.0"}, {"id",id},
                {"result", QJsonObject{{"tools", tools}}}
            });
            continue;
        }

        // ─── tools/call ──────────────────────────────────────────────────
        if (method == "tools/call") {
            QJsonObject params = msg.value("params").toObject();
            QString toolName   = params.value("name").toString();
            QJsonObject toolArgs = params.value("arguments").toObject();

            std::pair<QString,bool> result;

            if      (toolName == "read_file")    result = handleReadFile(toolArgs);
            else if (toolName == "write_file")   result = handleWriteFile(toolArgs);
            else if (toolName == "append_file")  result = handleAppendFile(toolArgs);
            else if (toolName == "str_replace")  result = handleStrReplace(toolArgs);
            else if (toolName == "list_dir")     result = handleListDir(toolArgs);
            else if (toolName == "list_symbols") result = handleListSymbols(toolArgs);
            else if (toolName == "mkdir")        result = handleMkdir(toolArgs);
            else result = {QString("Unbekanntes Tool: '%1'").arg(toolName), true};

            sendResult(id, result.first, result.second);
            continue;
        }

        // ─── Unbekannte Methode ───────────────────────────────────────────
        if (hasId) {
            sendError(id, -32601, QString("Method not found: %1").arg(method));
        }
    }

    return 0;
}
