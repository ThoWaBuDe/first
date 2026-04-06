// ─── LlamaQt MCP WebSearch Server ────────────────────────────────────────────
// Web search via Tavily API.
//
// The API key is read from the environment variable TAVILY_API_KEY.
// Set it before starting: export TAVILY_API_KEY="tvly-..."
//
// Advertised Tools:
//   web_search  — search the web, returns title + URL + snippet per result

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

// ─── performSearch ────────────────────────────────────────────────────────────
// POST to Tavily Search API, return formatted results.
//
// We use a local QEventLoop to block until the async QNetworkReply finishes.
// This is the Qt equivalent of a synchronous HTTP call — acceptable here
// because the MCP server is single-threaded and has no other work to do
// while waiting for the network response.
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

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        reply->deleteLater();
        return "Network error: " + err;
    }

    QJsonDocument resDoc = QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();

    if (resDoc.isNull() || !resDoc.isObject())
        return "Error: invalid API response.";

    QJsonArray results = resDoc.object().value("results").toArray();
    if (results.isEmpty())
        return "No results found.";

    QString output;
    int num = 1;
    for (const QJsonValue &val : results) {
        QJsonObject item    = val.toObject();
        QString title   = item.value("title").toString();
        QString url     = item.value("url").toString();
        QString content = item.value("content").toString();

        // Truncate snippet to keep model context manageable
        if (content.length() > 500)
            content = content.left(500) + "...";

        output += QString("[%1] %2\n%3\n%4\n\n")
                  .arg(num++).arg(title).arg(url).arg(content);
    }

    return output.trimmed();
}

// ─── Tool list ───────────────────────────────────────────────────────────────
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
            "Search the web using the Tavily API. "
            "Returns title, URL and a short snippet for each result. "
            "Useful for current information, Qt6 documentation, "
            "C++ error messages and library references.",
            {{"query",       makeProp("string",  "Search query or question")},
             {"max_results", makeProp("integer", "Number of results (default: 5, max: 10)")}},
            {"query"})
    };
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString apiKey = QProcessEnvironment::systemEnvironment()
                     .value("TAVILY_API_KEY");

    QTextStream errStream(stderr);
    if (apiKey.isEmpty()) {
        errStream << "[llamaqt-websearch] WARNING: TAVILY_API_KEY not set.\n"
                  << "Web search will fail. Set it with:\n"
                  << "  export TAVILY_API_KEY=\"tvly-...\"\n";
        errStream.flush();
    }

    QJsonArray tools = makeToolList();
    QTextStream in(stdin);

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            errStream << "JSON parse error: " << parseErr.errorString() << "\n";
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
                {"serverInfo",     QJsonObject{{"name","llamaqt-websearch"},{"version","1.1"}}}
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
                        "Error: TAVILY_API_KEY is not set. "
                        "Start the server process with: export TAVILY_API_KEY=tvly-...", true);
                    continue;
                }

                QString query      = toolArgs.value("query").toString();
                int     maxResults = toolArgs.value("max_results").toInt(5);
                maxResults = qBound(1, maxResults, 10);

                if (query.isEmpty()) {
                    sendResult(id, "Error: 'query' is required.", true);
                    continue;
                }

                QString result = performSearch(query, apiKey, maxResults);
                bool isErr = result.startsWith("Error:") || result.startsWith("Network error:");
                sendResult(id, result, isErr);
            } else {
                sendResult(id, QString("Unknown tool: '%1'").arg(toolName), true);
            }
            continue;
        }

        if (hasId) sendError(id, -32601, "Method not found: " + method);
    }
    return 0;
}
