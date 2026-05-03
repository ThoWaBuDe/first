#pragma once
// ─── ToolCallFormat ───────────────────────────────────────────────────────────
// Definiert alle unterstützten Tool-Call-Formate und den Parser.
//
// Zwei Ebenen analog zu ChatTemplate:
//   1. ToolCallFormat::Preset — welches Format soll verwendet werden
//   2. ToolCallParser         — parst einen LLM-Output in ParsedToolCall(s)
//
// Warum eine eigene Datei?
//   ToolCallParser wird von AgentChat, AgentExecute, AgentPlan gebraucht.
//   McpManager braucht ToolCallFormat für toolCallHeader().
//   Keine zirkulären Abhängigkeiten wenn alles in dieser einen Header-Datei ist.
//
// Formate:
//
//   QwenXmlTags (Standard, bisher hardcoded):
//     <tool_call>
//     {"name": "read_file", "arguments": {"path": "foo.cpp"}}
//     </tool_call>
//
//   MistralNative (Devstral, Mistral-Modelle):
//     [TOOL_CALLS] [{"name": "read_file", "arguments": {"path": "foo.cpp"}}]
//     Besonderheit: ARRAY — mehrere Tool-Calls möglich pro Antwort!
//
//   Gemma4Google (Gemma4, Google-Modelle):
//     ```json
//     {"function_call": {"name": "read_file", "arguments": {"path": "foo.cpp"}}}
//     ```
//     Alternativ auch ohne function_call wrapper direkt als JSON.
//
//   Llama3ToolUse (Meta Llama3.x mit Tool-Use Fine-Tune):
//     <|python_tag|>{"name": "read_file", "arguments": {"path": "foo.cpp"}}<|eom_id|>
//
//   Generic (Fallback, alle Modelle):
//     <tool_call>...</tool_call>  wie QwenXmlTags
//
// Sequenzielles Ausführen von Arrays (Mistral):
//   ToolCallParser::parseAll() gibt QVector<ParsedToolCall> zurück.
//   Agent iteriert über alle und führt sie nacheinander aus.
//   Analogie AVR: FIFO-Queue — Eingang in Reihenfolge abarbeiten.

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QVector>
#include <QRegularExpression>

// ─── ParsedToolCall ───────────────────────────────────────────────────────────
// Ein einzelner geparster Tool-Call.
// Mehrere entstehen beim Mistral-Array-Format.
struct ParsedToolCall {
    QString     name;
    QJsonObject arguments;
    bool        valid = false;
    QString     error;

    static ParsedToolCall err(const QString &msg) {
        ParsedToolCall r;
        r.valid = false;
        r.error = msg;
        return r;
    }
    static ParsedToolCall ok(const QString &name, const QJsonObject &args) {
        ParsedToolCall r;
        r.name      = name;
        r.arguments = args;
        r.valid     = true;
        return r;
    }
};

// ─── ToolCallFormat ───────────────────────────────────────────────────────────
struct ToolCallFormat {

    enum class Preset {
        Auto,          // aus GGUF/Modellname erkennen
        QwenXmlTags,   // <tool_call>...</tool_call>
        MistralNative, // [TOOL_CALLS] [...]
        Gemma4Google,  // ```json\n{"function_call":...}```
        Llama3ToolUse, // <|python_tag|>...<|eom_id|>
        Generic,       // Fallback = QwenXmlTags
    };

    // ── Preset → Anzeigename ──────────────────────────────────────────────────
    static QString presetName(Preset p) {
        switch (p) {
        case Preset::Auto:          return "Auto (aus Modell erkennen)";
        case Preset::QwenXmlTags:   return "Qwen XML Tags (<tool_call>)";
        case Preset::MistralNative: return "Mistral Native ([TOOL_CALLS])";
        case Preset::Gemma4Google:  return "Gemma4 / Google (```json)";
        case Preset::Llama3ToolUse: return "Llama3 Tool Use (<|python_tag|>)";
        case Preset::Generic:       return "Generic (Fallback)";
        }
        return "Generic";
    }

    // ── Auto-Detection aus Modellname / GGUF-Metadaten ───────────────────────
    // Analogie zu ChatTemplate::detectFromJinja() —
    // heuristischer Substring-Match, kein vollständiger Parser.
    //
    // Quellen für den Erkennungsstring:
    //   1. Modell-Dateiname (z.B. "devstral-small-2-24b-q4_k_m.gguf")
    //   2. GGUF general.name Metadaten (falls llama.cpp sie exponiert)
    //   3. Jinja Chat-Template (Tool-Call-Marker im Template-String)
    static Preset detectFromModelName(const QString &modelPath) {
        QString lower = modelPath.toLower();

        // Mistral-Familie: devstral, mistral, mixtral, mathstral
        if (lower.contains("devstral") ||
            lower.contains("mistral")  ||
            lower.contains("mixtral")  ||
            lower.contains("mathstral"))
            return Preset::MistralNative;

        // Gemma-Familie: gemma4, gemma-4, gemma 4
        if (lower.contains("gemma4") ||
            lower.contains("gemma-4") ||
            lower.contains("gemma_4"))
            return Preset::Gemma4Google;

        // Gemma2/3 — ältere Gemma ohne native Tool-Calls → Generic
        if (lower.contains("gemma"))
            return Preset::Generic;

        // Llama3-Tool-Use Fine-Tunes
        if ((lower.contains("llama") && lower.contains("tool")) ||
            lower.contains("hermes") ||
            lower.contains("functionary"))
            return Preset::Llama3ToolUse;

        // Qwen-Familie: qwen, qwq
        if (lower.contains("qwen") || lower.contains("qwq"))
            return Preset::QwenXmlTags;

        // Unbekannt → Generic (= QwenXmlTags als Fallback)
        return Preset::Generic;
    }

    // Aus Jinja-Template detektieren (zweite Erkennungsebene)
    static Preset detectFromJinja(const QString &jinja) {
        if (jinja.isEmpty()) return Preset::Auto; // kein Template → weiter mit Modellname

        if (jinja.contains("[TOOL_CALLS]"))
            return Preset::MistralNative;
        if (jinja.contains("function_call") || jinja.contains("gemma"))
            return Preset::Gemma4Google;
        if (jinja.contains("<|python_tag|>"))
            return Preset::Llama3ToolUse;
        if (jinja.contains("<tool_call>"))
            return Preset::QwenXmlTags;

        return Preset::Auto; // Template vorhanden aber kein Tool-Call-Marker → Modellname nutzen
    }

    // ── System-Prompt Header pro Format ──────────────────────────────────────
    // Wird von McpManager::toolCallHeader(format) ausgegeben.
    // Das Modell bekommt sein eigenes Format als Beispiel — keine Übersetzung nötig.
    static QString systemPromptHeader(Preset preset) {
        switch (preset) {

        case Preset::MistralNative:
            return
                "You may call one or more functions to assist with the user query.\n\n"
                "For each function call, return a JSON array prefixed with [TOOL_CALLS]:\n"
                "[TOOL_CALLS] [{\"name\": <function-name>, \"arguments\": <args-json-object>}]\n\n"
                "Multiple calls are allowed in one response as an array:\n"
                "[TOOL_CALLS] [{\"name\": \"func1\", \"arguments\": {...}}, "
                "{\"name\": \"func2\", \"arguments\": {...}}]\n\n"
                "Wait for all results before continuing.\n"
                "Available tools:\n";

        case Preset::Gemma4Google:
            return
                "You may call functions to assist with the user query.\n\n"
                "For each function call, return a JSON code block:\n"
                "```json\n"
                "{\"function_call\": {\"name\": <function-name>, "
                "\"arguments\": <args-json-object>}}\n"
                "```\n\n"
                "Wait for the result before making the next function call.\n"
                "Available tools:\n";

        case Preset::Llama3ToolUse:
            return
                "You may call functions to assist with the user query.\n\n"
                "For each function call use this format:\n"
                "<|python_tag|>{\"name\": <function-name>, "
                "\"arguments\": <args-json-object>}<|eom_id|>\n\n"
                "Wait for the result before the next call.\n"
                "Available tools:\n";

        case Preset::QwenXmlTags:
        case Preset::Generic:
        case Preset::Auto:
        default:
            return
                "You are a helpful assistant with access to tools.\n\n"
                "To call a tool, emit a JSON object inside <tool_call> XML tags.\n"
                "Every string value — including the tool name — must be in double quotes.\n"
                "Format:\n"
                "<tool_call>\n"
                "{\"name\": \"tool_name\", \"arguments\": {\"param\": \"value\"}}\n"
                "</tool_call>\n\n"
                "Wait for the result before making the next tool call.\n"
                "Available tools:\n";
        }
    }

    // ── Stop-Sequenzen pro Format ─────────────────────────────────────────────
    // LlamaWorker::doGenerate() stoppt früh wenn diese Sequenz im Output erscheint.
    // Analogie AVR: UART stoppt beim ETX-Byte, nicht erst beim Puffer-Ende.
    static QString stopSequence(Preset preset) {
        switch (preset) {
        case Preset::MistralNative: return QString(); // Array-Ende schwer zu detektieren → kein früher Stop
        case Preset::Gemma4Google:  return "```";     // Code-Block-Ende
        case Preset::Llama3ToolUse: return "<|eom_id|>";
        default:                    return "</tool_call>";
        }
    }

    // ── Erkennung ob Tool-Call im Response vorhanden ──────────────────────────
    static bool containsToolCall(const QString &response, Preset preset) {
        switch (preset) {
        case Preset::MistralNative: return response.contains("[TOOL_CALLS]");
        case Preset::Gemma4Google:  return response.contains("\"function_call\"") ||
                                           response.contains("function_call");
        case Preset::Llama3ToolUse: return response.contains("<|python_tag|>");
        default:                    return response.contains("<tool_call>") &&
                                           response.contains("</tool_call>");
        }
    }

    // ── Vollständiger Tool-Call (nicht abgeschnitten) ─────────────────────────
    static bool isCompleteToolCall(const QString &response, Preset preset) {
        switch (preset) {
        case Preset::MistralNative:
            // Array muss mit ] enden — heuristisch
            if (!response.contains("[TOOL_CALLS]")) return false;
            {
                int start = response.indexOf("[TOOL_CALLS]") + 12;
                QString rest = response.mid(start).trimmed();
                return rest.startsWith('[') && rest.contains(']');
            }
        case Preset::Gemma4Google:
            return response.contains("\"function_call\"") &&
                   response.count("```") >= 2;
        case Preset::Llama3ToolUse:
            return response.contains("<|python_tag|>") &&
                   response.contains("<|eom_id|>");
        default:
            return response.contains("<tool_call>") &&
                   response.contains("</tool_call>");
        }
    }
};

// ─── ToolCallParser ───────────────────────────────────────────────────────────
// Parst einen LLM-Response-String in einen oder mehrere ParsedToolCall(s).
//
// Hauptmethode: parseAll() — gibt immer einen QVector zurück.
//   Leer  → kein Tool-Call gefunden
//   1     → normaler Fall (alle Formate außer Mistral-Array)
//   >1    → Mistral-Array mit mehreren Calls
//
// Agent iteriert über den Vector und führt alle sequenziell aus.
// Analogie AVR: FIFO — erstes Element zuerst, dann nächstes.
class ToolCallParser
{
public:
    // Parst alle Tool-Calls aus einem Response.
    // Gibt leeren Vector zurück wenn kein Tool-Call gefunden.
    static QVector<ParsedToolCall> parseAll(const QString &response,
                                             ToolCallFormat::Preset preset)
    {
        switch (preset) {
        case ToolCallFormat::Preset::MistralNative:
            return parseMistral(response);
        case ToolCallFormat::Preset::Gemma4Google:
            return parseGemma4(response);
        case ToolCallFormat::Preset::Llama3ToolUse:
            return parseLlama3(response);
        default:
            return parseQwenXml(response);
        }
    }

    // Convenience: ersten Tool-Call holen (für Nicht-Array-Formate)
    static ParsedToolCall parseFirst(const QString &response,
                                      ToolCallFormat::Preset preset)
    {
        auto all = parseAll(response, preset);
        if (all.isEmpty()) return ParsedToolCall::err("Kein Tool-Call gefunden.");
        return all.first();
    }

private:
    // ── Qwen / Generic: <tool_call>...</tool_call> ────────────────────────────
    static QVector<ParsedToolCall> parseQwenXml(const QString &response)
    {
        QVector<ParsedToolCall> result;
        QString text = response;
        int searchFrom = 0;

        while (true) {
            int start = text.indexOf("<tool_call>", searchFrom);
            if (start < 0) break;
            int end = text.indexOf("</tool_call>", start + 11);
            if (end < 0) break;

            QString block = text.mid(start + 11, end - start - 11).trimmed();
            result.append(parseJsonBlock(block));
            searchFrom = end + 12;
        }
        return result;
    }

    // ── Mistral Native: [TOOL_CALLS] [{...}, {...}] ───────────────────────────
    // Das Array kann einen oder mehrere Tool-Calls enthalten.
    // [TOOL_CALLS] ist ein spezielles Prefix-Token das Mistral ausgibt.
    static QVector<ParsedToolCall> parseMistral(const QString &response)
    {
        QVector<ParsedToolCall> result;

        int marker = response.indexOf("[TOOL_CALLS]");
        if (marker < 0) return result;

        // Alles nach [TOOL_CALLS] bis zum Ende der Zeile / nächsten Absatz
        QString rest = response.mid(marker + 12).trimmed();

        // Array-Start finden
        int arrayStart = rest.indexOf('[');
        if (arrayStart < 0) return result;

        // Array-Ende: balancierte Klammern zählen
        // Analogie AVR: Klammer-Stack wie ein Hardware-Tiefenzähler
        int depth = 0;
        int arrayEnd = -1;
        for (int i = arrayStart; i < rest.length(); ++i) {
            if (rest[i] == '[') ++depth;
            else if (rest[i] == ']') {
                --depth;
                if (depth == 0) { arrayEnd = i; break; }
            }
        }
        if (arrayEnd < 0) {
            // Unvollständig — versuche es trotzdem mit repair
            result.append(ParsedToolCall::err("Unvollständiges Mistral-Array."));
            return result;
        }

        QString arrayStr = rest.mid(arrayStart, arrayEnd - arrayStart + 1);
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(arrayStr.toUtf8(), &pe);

        if (doc.isNull() || !doc.isArray()) {
            result.append(ParsedToolCall::err(
                QString("Mistral-Array: JSON-Fehler: %1").arg(pe.errorString())));
            return result;
        }

        // Array iterieren — jedes Element ist ein Tool-Call
        for (const QJsonValue &val : doc.array()) {
            QJsonObject obj = val.toObject();
            QString name = obj.value("name").toString();
            if (name.isEmpty()) {
                result.append(ParsedToolCall::err("Mistral-Array: 'name' fehlt."));
                continue;
            }
            // Mistral nutzt "arguments" oder "parameters" — beide prüfen
            QJsonObject args = obj.contains("arguments")
                ? obj.value("arguments").toObject()
                : obj.value("parameters").toObject();
            result.append(ParsedToolCall::ok(name, args));
        }
        return result;
    }

    // ── Gemma4 / Google: ```json\n{"function_call": {...}}\n``` ──────────────
    static QVector<ParsedToolCall> parseGemma4(const QString &response)
    {
        QVector<ParsedToolCall> result;

        // Variante 1: Code-Block mit function_call wrapper
        static const QRegularExpression codeBlockRe(
            "```(?:json)?\\s*\\n?([\\s\\S]*?)\\n?```",
            QRegularExpression::MultilineOption);

        QRegularExpressionMatchIterator it = codeBlockRe.globalMatch(response);
        while (it.hasNext()) {
            auto m = it.next();
            QString block = m.captured(1).trimmed();
            QJsonParseError pe;
            QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);
            if (doc.isNull()) continue;

            QJsonObject obj = doc.object();

            // function_call wrapper (Gemma4-Standard)
            if (obj.contains("function_call")) {
                QJsonObject fc = obj.value("function_call").toObject();
                QString name   = fc.value("name").toString();
                QJsonObject args;
                // arguments kann String (serialisiertes JSON) oder Object sein
                QJsonValue argsVal = fc.value("arguments");
                if (argsVal.isObject()) {
                    args = argsVal.toObject();
                } else if (argsVal.isString()) {
                    QJsonDocument argDoc = QJsonDocument::fromJson(
                        argsVal.toString().toUtf8());
                    if (argDoc.isObject()) args = argDoc.object();
                }
                if (!name.isEmpty())
                    result.append(ParsedToolCall::ok(name, args));
                continue;
            }

            // Variante 2: direkt {"name": ..., "arguments": ...} ohne wrapper
            if (obj.contains("name")) {
                result.append(parseJsonBlock(block));
                continue;
            }
        }

        // Variante 3: kein Code-Block aber function_call als Plaintext-JSON
        if (result.isEmpty() && response.contains("\"function_call\"")) {
            int fcStart = response.indexOf("\"function_call\"");
            // Rückwärts zum öffnenden { suchen
            int objStart = response.lastIndexOf('{', fcStart);
            if (objStart >= 0) {
                // Balancierte Klammern finden
                int depth = 0, objEnd = -1;
                for (int i = objStart; i < response.length(); ++i) {
                    if (response[i] == '{') ++depth;
                    else if (response[i] == '}') {
                        --depth;
                        if (depth == 0) { objEnd = i; break; }
                    }
                }
                if (objEnd > 0) {
                    QString block = response.mid(objStart, objEnd - objStart + 1);
                    QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8());
                    if (!doc.isNull()) {
                        QJsonObject fc = doc.object()
                            .value("function_call").toObject();
                        QString name = fc.value("name").toString();
                        QJsonObject args = fc.value("arguments").toObject();
                        if (!name.isEmpty())
                            result.append(ParsedToolCall::ok(name, args));
                    }
                }
            }
        }

        return result;
    }

    // ── Llama3 Tool-Use: <|python_tag|>...<|eom_id|> ─────────────────────────
    static QVector<ParsedToolCall> parseLlama3(const QString &response)
    {
        QVector<ParsedToolCall> result;
        QString text = response;
        int searchFrom = 0;

        static const QString startTag = "<|python_tag|>";
        static const QString endTag   = "<|eom_id|>";

        while (true) {
            int start = text.indexOf(startTag, searchFrom);
            if (start < 0) break;
            int end = text.indexOf(endTag, start + startTag.length());
            if (end < 0) break;

            QString block = text.mid(start + startTag.length(),
                                      end - start - startTag.length()).trimmed();
            result.append(parseJsonBlock(block));
            searchFrom = end + endTag.length();
        }
        return result;
    }

    // ── Gemeinsamer JSON-Block-Parser ─────────────────────────────────────────
    // Erwartet {"name": "...", "arguments": {...}}
    static ParsedToolCall parseJsonBlock(const QString &block)
    {
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &pe);
        if (doc.isNull())
            return ParsedToolCall::err(
                QString("JSON-Fehler: %1 in Block: %2")
                .arg(pe.errorString(), block.left(80)));

        QJsonObject obj = doc.object();
        QString name    = obj.value("name").toString();
        if (name.isEmpty())
            return ParsedToolCall::err("'name' fehlt im Tool-Call.");

        QJsonObject args = obj.value("arguments").toObject();
        return ParsedToolCall::ok(name, args);
    }
};
