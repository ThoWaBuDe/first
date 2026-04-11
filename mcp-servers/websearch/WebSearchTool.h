#pragma once
// ─── WebSearchTool ───────────────────────────────────────────────────────────

#include "WebsearchToolBase.h"

class WebSearchTool : public WebsearchToolBase
{
public:
    using WebsearchToolBase::WebsearchToolBase;

    QString name() const override { return "web_search"; }

    QString description() const override
    {
        return "Search the web using the Tavily API. "
               "Returns title, URL and a short snippet for each result. "
               "Useful for current information, Qt6 documentation, "
               "C++ error messages and library references.";
    }

    QJsonObject properties() const override
    {
        return {
            {"query",       prop("string",  "Search query or question")},
            {"max_results", prop("integer", "Number of results (default: 5, max: 10)")}
        };
    }

    QJsonArray required() const override { return {"query"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        if (m_apiKey.isEmpty()) {
            return ToolResult::err(
                "Error: TAVILY_API_KEY is not set. "
                "Start the server process with: export TAVILY_API_KEY=tvly-...");
        }

        QString query      = args.value("query").toString();
        int     maxResults = args.value("max_results").toInt(5);
        maxResults = qBound(1, maxResults, 10);

        if (query.isEmpty()) {
            return ToolResult::err("Error: 'query' is required.");
        }

        QString result = performSearch(query, m_apiKey, maxResults);
        bool isErr = result.startsWith("Error:") || result.startsWith("Network error:");
        
        return isErr ? ToolResult::err(result) : ToolResult::ok(result);
    }
};