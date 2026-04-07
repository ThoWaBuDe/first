#include "AppConfig.h"
#include <QStandardPaths>

// ─── Singleton ────────────────────────────────────────────────────────────────
// Statische lokale Variable: wird beim ersten Aufruf von instance() erzeugt
// und lebt bis Programmende. Thread-safe seit C++11 (magic statics).
AppConfig &AppConfig::instance()
{
    static AppConfig inst;
    return inst;
}

// ─── Konstruktor ──────────────────────────────────────────────────────────────
// QSettings mit INI-Format: ~/.config/LlamaQt/LlamaQt.conf
// INI statt native Format (Registry auf Windows) weil es lesbar und
// editierbar ist — der User kann Einstellungen direkt ändern.
AppConfig::AppConfig(QObject *parent)
    : QObject(parent)
    , m_settings(QSettings::IniFormat,
                 QSettings::UserScope,
                 "LlamaQt", "LlamaQt")
{
    load();
}

// ─── load ─────────────────────────────────────────────────────────────────────
// Liest alle Werte aus der .conf Datei.
// Falls ein Key nicht vorhanden ist → Default-Wert bleibt erhalten.
// Pattern: Defensive Read — fehlende Keys sind kein Fehler.
void AppConfig::load()
{
    m_settings.beginGroup("Model");
    m_modelPath   = m_settings.value("path",        m_modelPath).toString();
    m_contextSize = m_settings.value("context_size",m_contextSize).toInt();
    m_batchSize   = m_settings.value("batch_size",  m_batchSize).toInt();
    m_settings.endGroup();

    m_settings.beginGroup("SamplerChat");
    m_chatTopK = m_settings.value("top_k", m_chatTopK).toInt();
    m_chatTemp = m_settings.value("temp",  m_chatTemp).toFloat();
    m_chatTopP = m_settings.value("top_p", m_chatTopP).toFloat();
    m_chatMinP = m_settings.value("min_p", m_chatMinP).toFloat();
    m_settings.endGroup();

    m_settings.beginGroup("SamplerTool");
    m_toolTopK = m_settings.value("top_k", m_toolTopK).toInt();
    m_toolTemp = m_settings.value("temp",  m_toolTemp).toFloat();
    m_toolTopP = m_settings.value("top_p", m_toolTopP).toFloat();
    m_toolMinP = m_settings.value("min_p", m_toolMinP).toFloat();
    m_settings.endGroup();

    m_settings.beginGroup("Agent");
    m_summarizeThreshold = m_settings.value("summarize_threshold", m_summarizeThreshold).toInt();
    m_maxContinuations   = m_settings.value("max_continuations",   m_maxContinuations).toInt();
    m_deadlockWarn       = m_settings.value("deadlock_warn",       m_deadlockWarn).toInt();
    m_deadlockRedirect   = m_settings.value("deadlock_redirect",   m_deadlockRedirect).toInt();
    m_deadlockAbort      = m_settings.value("deadlock_abort",      m_deadlockAbort).toInt();
    m_maxToolResultChars = m_settings.value("max_tool_result_chars",m_maxToolResultChars).toInt();
    m_sandboxPath        = m_settings.value("sandbox_path",        m_sandboxPath).toString();
    m_settings.endGroup();

    m_settings.beginGroup("WebSearch");
    m_tavilyApiKey = m_settings.value("tavily_api_key", m_tavilyApiKey).toString();
    m_settings.endGroup();

    m_settings.beginGroup("Logging");
    m_chatLoggingEnabled = m_settings.value("chat_enabled", m_chatLoggingEnabled).toBool();
    m_chatLogDir         = m_settings.value("chat_log_dir", m_chatLogDir).toString();
    m_settings.endGroup();
}

// ─── save ─────────────────────────────────────────────────────────────────────
// Schreibt alle Werte in die .conf Datei.
// Wird nach jeder set*()-Methode aufgerufen → immer konsistent auf Disk.
// QSettings::sync() wird intern von QSettings beim Schreiben aufgerufen.
void AppConfig::save()
{
    m_settings.beginGroup("Model");
    m_settings.setValue("path",         m_modelPath);
    m_settings.setValue("context_size", m_contextSize);
    m_settings.setValue("batch_size",   m_batchSize);
    m_settings.endGroup();

    m_settings.beginGroup("SamplerChat");
    m_settings.setValue("top_k", m_chatTopK);
    m_settings.setValue("temp",  m_chatTemp);
    m_settings.setValue("top_p", m_chatTopP);
    m_settings.setValue("min_p", m_chatMinP);
    m_settings.endGroup();

    m_settings.beginGroup("SamplerTool");
    m_settings.setValue("top_k", m_toolTopK);
    m_settings.setValue("temp",  m_toolTemp);
    m_settings.setValue("top_p", m_toolTopP);
    m_settings.setValue("min_p", m_toolMinP);
    m_settings.endGroup();

    m_settings.beginGroup("Agent");
    m_settings.setValue("summarize_threshold",  m_summarizeThreshold);
    m_settings.setValue("max_continuations",    m_maxContinuations);
    m_settings.setValue("deadlock_warn",        m_deadlockWarn);
    m_settings.setValue("deadlock_redirect",    m_deadlockRedirect);
    m_settings.setValue("deadlock_abort",       m_deadlockAbort);
    m_settings.setValue("max_tool_result_chars",m_maxToolResultChars);
    m_settings.setValue("sandbox_path",         m_sandboxPath);
    m_settings.endGroup();    m_settings.beginGroup("WebSearch");
    m_settings.setValue("tavily_api_key", m_tavilyApiKey);
    m_settings.endGroup();

    m_settings.beginGroup("Logging");
    m_settings.setValue("chat_enabled", m_chatLoggingEnabled);
    m_settings.setValue("chat_log_dir", m_chatLogDir);
    m_settings.endGroup();

    m_settings.sync();
}
