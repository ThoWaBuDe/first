#pragma once
// ─── WebsearchToolBase ───────────────────────────────────────────────────────

#include "../common/ToolBase.h"
#include "../common/PathPolicy.h"

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>

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

        if (content.length() > 500)
            content = content.left(500) + "...";

        output += QString("[%1] %2\n%3\n%4\n\n")
                  .arg(num++).arg(title).arg(url).arg(content);
    }

    return output.trimmed();
}

class WebsearchToolBase : public ToolBase
{
public:
    explicit WebsearchToolBase(PathPolicy *policy, const QString &apiKey)
        : m_policy(policy), m_apiKey(apiKey)
    {}

protected:
    PathPolicy *m_policy;
    QString m_apiKey;

    static QJsonObject prop(const QString &type, const QString &desc)
    {
        return QJsonObject{{"type", type}, {"description", desc}};
    }
};