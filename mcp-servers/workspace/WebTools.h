#pragma once
// ─── WebTools ─────────────────────────────────────────────────────────────────
// web_fetch: Rohen HTML-Inhalt einer URL abrufen.
// web_scrape: URL abrufen und lesbaren Text extrahieren (kein HTML).
//
// Transport: QNetworkAccessManager (Qt6::Network).
// Warum synchron (QEventLoop)?
//   MCP-Server sind single-threaded stdio-Programme.
//   Wir blockieren bis die Antwort da ist — kein Callback-Chaos.
//   Timeout: 30 Sekunden.
//
// HTML-zu-Text Algorithmus (web_scrape):
//   1. Script/Style-Blöcke entfernen (Regex)
//   2. HTML-Tags durch Leerzeichen ersetzen
//   3. HTML-Entities dekodieren (&amp; → &, &lt; → <, etc.)
//   4. Mehrfache Leerzeichen/Zeilenumbrüche normalisieren
//   Ergebnis: lesbarer Fließtext, ähnlich wie lynx --dump.

#include "WorkspaceToolBase.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>
#include <QUrl>

// ─── Hilfsfunktion: HTML → lesbarer Text ─────────────────────────────────────
static QString htmlToText(const QString &html)
{
    QString text = html;

    // 1. <script>...</script> und <style>...</style> entfernen
    static const QRegularExpression scriptRe(
        "<script[^>]*>[\\s\\S]*?</script>",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression styleRe(
        "<style[^>]*>[\\s\\S]*?</style>",
        QRegularExpression::CaseInsensitiveOption);
    text.remove(scriptRe);
    text.remove(styleRe);

    // 2. Block-Elemente durch Zeilenumbruch ersetzen
    static const QRegularExpression blockRe(
        "<(br|p|div|h[1-6]|li|tr|blockquote)[^>]*>",
        QRegularExpression::CaseInsensitiveOption);
    text.replace(blockRe, "\n");

    // 3. Alle verbleibenden Tags entfernen
    static const QRegularExpression tagRe("<[^>]+>");
    text.remove(tagRe);

    // 4. HTML-Entities dekodieren
    text.replace("&amp;",  "&");
    text.replace("&lt;",   "<");
    text.replace("&gt;",   ">");
    text.replace("&quot;", "\"");
    text.replace("&apos;", "'");
    text.replace("&nbsp;", " ");
    text.replace("&#39;",  "'");

    // 5. Mehrfache Leerzeichen und Leerzeilen normalisieren
    static const QRegularExpression multiSpaceRe("[ \\t]+");
    text.replace(multiSpaceRe, " ");
    static const QRegularExpression multiLineRe("\\n{3,}");
    text.replace(multiLineRe, "\n\n");

    return text.trimmed();
}

// ─── Hilfsfunktion: HTTP GET ──────────────────────────────────────────────────
static QPair<QString, int> httpGet(const QString &url, int timeoutMs = 30000)
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (compatible; LlamaQt/1.0)");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = nam.get(req);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(timeoutMs);

    QObject::connect(reply,  &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout,         &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    } else {
        reply->abort();
        reply->deleteLater();
        return {"Error: Timeout nach " + QString::number(timeoutMs / 1000) + "s.", 1};
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        reply->deleteLater();
        return {"Error: " + err, 1};
    }

    // Encoding aus Content-Type lesen
    QString contentType = reply->header(
        QNetworkRequest::ContentTypeHeader).toString();
    QByteArray data = reply->readAll();
    reply->deleteLater();

    // Einfache Encoding-Erkennung
    QString text;
    if (contentType.contains("utf-8", Qt::CaseInsensitive) ||
        contentType.contains("utf8",  Qt::CaseInsensitive)) {
        text = QString::fromUtf8(data);
    } else if (contentType.contains("latin") ||
               contentType.contains("iso-8859")) {
        text = QString::fromLatin1(data);
    } else {
        text = QString::fromUtf8(data);  // UTF-8 als Fallback
    }

    return {text, 0};
}

// ─── WebFetchTool ─────────────────────────────────────────────────────────────
class WebFetchTool : public WorkspaceToolBase
{
public:
    using WorkspaceToolBase::WorkspaceToolBase;

    QString name() const override { return "web_fetch"; }

    QString description() const override
    {
        return
            "Fetch the raw HTML content of a URL. "
            "Returns the complete HTML source. "
            "Use web_scrape if you want readable text without HTML tags. "
            "Use web_fetch if you need to inspect links, forms, or HTML structure. "
            "Max response size: 500KB. Timeout: 30 seconds.";
    }

    QJsonObject properties() const override
    {
        return {
            {"url",      prop("string",  "URL to fetch (must start with http:// or https://)")},
            {"max_kb",   prop("integer", "Max response size in KB (default: 500, max: 2000)")}
        };
    }

    QJsonArray required() const override { return req({"url"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString url  = args.value("url").toString().trimmed();
        int maxKb    = qBound(1, args.value("max_kb").toInt(500), 2000);

        if (url.isEmpty())
            return ToolResult::err("Error: 'url' is required.");
        if (!url.startsWith("http://") && !url.startsWith("https://"))
            return ToolResult::err("Error: URL must start with http:// or https://");

        auto [html, code] = httpGet(url);
        if (code != 0) return ToolResult::err(html);

        // Größe begrenzen
        const int maxChars = maxKb * 1024;
        if (html.length() > maxChars) {
            html = html.left(maxChars)
                + QString("\n\n[... gekürzt bei %1 KB]").arg(maxKb);
        }

        return ToolResult::ok(html);
    }
};

// ─── WebScrapeTool ────────────────────────────────────────────────────────────
class WebScrapeTool : public WorkspaceToolBase
{
public:
    using WorkspaceToolBase::WorkspaceToolBase;

    QString name() const override { return "web_scrape"; }

    QString description() const override
    {
        return
            "Fetch a URL and extract readable text (no HTML tags). "
            "Scripts, styles, and HTML markup are removed. "
            "Returns clean, readable text similar to 'lynx --dump'. "
            "Use this to read articles, documentation, or any web content. "
            "Use web_fetch if you need the raw HTML structure. "
            "Max response size: 500KB. Timeout: 30 seconds.";
    }

    QJsonObject properties() const override
    {
        return {
            {"url",      prop("string",  "URL to scrape (must start with http:// or https://)")},
            {"max_kb",   prop("integer", "Max response size in KB (default: 500, max: 2000)")}
        };
    }

    QJsonArray required() const override { return req({"url"}); }

    ToolResult execute(const QJsonObject &args) override
    {
        QString url  = args.value("url").toString().trimmed();
        int maxKb    = qBound(1, args.value("max_kb").toInt(500), 2000);

        if (url.isEmpty())
            return ToolResult::err("Error: 'url' is required.");
        if (!url.startsWith("http://") && !url.startsWith("https://"))
            return ToolResult::err("Error: URL must start with http:// or https://");

        auto [html, code] = httpGet(url);
        if (code != 0) return ToolResult::err(html);

        QString text = htmlToText(html);

        // Größe begrenzen
        const int maxChars = maxKb * 1024;
        if (text.length() > maxChars) {
            text = text.left(maxChars)
                + QString("\n\n[... gekürzt bei %1 KB]").arg(maxKb);
        }

        return ToolResult::ok(text);
    }
};
