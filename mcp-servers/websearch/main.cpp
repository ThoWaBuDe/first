// ─── LlamaQt MCP WebSearch Server ────────────────────────────────────────────
// Web-Suche via Tavily API.
//
// API-Key wird aus der Umgebungsvariable TAVILY_API_KEY gelesen.
// Setzen vor dem Start: export TAVILY_API_KEY="tvly-..."
//
// Advertised Tools:
//   web_search  — Sucht im Web, gibt Titel + URL + Snippet zurueck

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTextStream>
#include <QProcessEnvironment>
#include <QUrl>
#include <iostream>

// ─── JSON-RPC Hilfsfunktionen ─────────────────────────────────────────────────
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

// ─── performSearch ────────────────────────────────────────────────────────────
// Sendet einen POST-Request an die Tavily Search API und gibt die Ergebnisse
// als formatierten String zurück.
//
// Tavily API: https://docs.tavily.com/docs/rest-api/api-reference
// Request:  POST https://api.tavily.com/search
//           Body: {"api_key":"...", "query":"...", "search_depth":"basic",
//                  "max_results":5}
// Response: {"results": [{"title":"...", "url":"...", "content":"..."}]}
//
// QNetworkAccessManager ist async — wir nutzen eine lokale QEventLoop
// um synchron zu warten (MCP-Server ist single-threaded blocking).
// Das ist das Qt-Äquivalent zu einem blocking HTTP-Client.
static QString performSearch(const QString &query, const QString &apiKey, int maxResults)
{
    QNetworkAccessManager manager;

    QNetworkRequest request(QUrl("https://api.tavily.com/search"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["api_key"]      = apiKey;
    body["query"]        = query;
    body["search_depth"] = "basic";
    body["max_results"]  = maxResults;

    QNetworkReply *reply = manager.post(request, QJsonDocument(body).toJson());

    // Lokale EventLoop: blockiert bis reply->finished() emittiert wird.
    // Kein QThread nötig — wir sind der einzige Prozess, der auf Antwort wartet.
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        reply->deleteLater();
        return "Netzwerk-Fehler: " + err;
    }

    QJsonDocument resDoc = QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();

    if (resDoc.isNull() || !resDoc.isObject())
        return "Fehler: Ungueltige API-Antwort.";

    QJsonArray results = resDoc.object().value("results").toArray();
    if (results.isEmpty())
        return "Keine Ergebnisse gefunden.";

    // Ergebnisse formatieren: Titel + URL + Snippet
    // Jedes Ergebnis durch Trennlinie abgegrenzt
    QString output;
    int num = 1;
    for (const QJsonValue &val : results) {
        QJsonObject item    = val.toObject();
        QString title   = item.value("title").toString();
        QString url     = item.value("url").toString();
        QString content = item.value("content").toString();

        // Snippet kürzen — Modell-Kontext schonen
        if (content.length() > 500)
            content = content.left(500) + "...";

        output += QString("[%1] %2\n%3\n%4\n\n")
                  .arg(num++).arg(title).arg(url).arg(content);
    }

    return output.trimmed();
}

// ─── Tool-Definitionen ───────────────────────────────────────────────────────
static QJsonArray makeToolList()
{
    auto makeProp = [](const QString &type, const QString &desc) {
        return QJsonObject{{"type",type},{"description",desc}};
    };
    auto makeTool = [](const QString &name, const QString &desc,
                       const QJsonObject &props, const QJsonArray &req = {}) {
        return QJsonObject{{"name",name},{"description",desc},
            {"inputSchema",QJsonObject{{"type","object"},{"properties",props},{"required",req}}}};
    };

    return QJsonArray{
        makeTool("web_search",
            "Sucht im Web via Tavily API. "
            "Gibt Titel, URL und Snippet der Top-Ergebnisse zurueck. "
            "Ideal fuer aktuelle Informationen, Qt6-Doku, C++ Fehlermeldungen.",
            {{"query",       makeProp("string",  "Suchbegriff oder Frage")},
             {"max_results", makeProp("integer", "Anzahl Ergebnisse (default: 5, max: 10)")}},
            {"query"})
    };
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // API-Key aus Umgebungsvariable lesen — nie hardcoden!
    // Setzen mit: export TAVILY_API_KEY="tvly-..."
    QString apiKey = QProcessEnvironment::systemEnvironment()
                     .value("TAVILY_API_KEY");

    QTextStream errStream(stderr);
    if (apiKey.isEmpty()) {
        errStream << "[llamaqt-websearch] WARNUNG: TAVILY_API_KEY nicht gesetzt.\n"
                  << "Web-Suche wird fehlschlagen. Setzen mit:\n"
                  << "  export TAVILY_API_KEY=\"tvly-...\"\n";
        errStream.flush();
        // Server läuft trotzdem — gibt bei Tool-Calls Fehlermeldung zurück
    }

    QJsonArray tools = makeToolList();
    QTextStream in(stdin);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            errStream << "JSON Parse Error: " << parseErr.errorString() << "\n";
            errStream.flush();
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
                {"serverInfo",     QJsonObject{{"name","llamaqt-websearch"},{"version","1.0"}}}
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

            if (toolName == "web_search") {
                if (apiKey.isEmpty()) {
                    sendResult(id,
                        "Fehler: TAVILY_API_KEY nicht gesetzt. "
                        "Server-Prozess mit 'export TAVILY_API_KEY=...' starten.", true);
                    continue;
                }

                QString query      = toolArgs.value("query").toString();
                int     maxResults = toolArgs.value("max_results").toInt(5);
                maxResults = qBound(1, maxResults, 10);  // 1..10

                if (query.isEmpty()) {
                    sendResult(id, "Fehler: 'query' fehlt.", true);
                    continue;
                }

                QString result = performSearch(query, apiKey, maxResults);
                sendResult(id, result, result.startsWith("Fehler:") || result.startsWith("Netzwerk-Fehler:"));
            } else {
                sendResult(id, QString("Unbekanntes Tool: '%1'").arg(toolName), true);
            }
            continue;
        }

        if (hasId) sendError(id, -32601, "Method not found: " + method);
    }

    return 0;
}
