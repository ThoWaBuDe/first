#include "AppConfig.h"
#include <QStandardPaths>
#include <QDir>

AppConfig &AppConfig::instance()
{
    static AppConfig inst;
    return inst;
}

AppConfig::AppConfig(QObject *parent)
    : QObject(parent)
    , m_settings(QSettings::IniFormat, QSettings::UserScope,
                 "LlamaQt", "LlamaQt")
{
    load();
}

void AppConfig::load()
{
    m_settings.beginGroup("Model");
    m_modelPath          = m_settings.value("path",         m_modelPath).toString();
    m_contextSize        = m_settings.value("context_size", m_contextSize).toInt();
    m_batchSize          = m_settings.value("batch_size",   m_batchSize).toInt();
    m_chatTemplatePreset = static_cast<ChatTemplate::Preset>(
        m_settings.value("chat_template_preset",
                         static_cast<int>(ChatTemplate::Preset::Auto)).toInt());
    m_customChatTemplate = m_settings.value("custom_chat_template", "").toString();
    // NEU: Tool-Call-Format
    m_toolCallFormat = static_cast<ToolCallFormat::Preset>(
        m_settings.value("tool_call_format",
                         static_cast<int>(ToolCallFormat::Preset::Auto)).toInt());
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

    m_settings.beginGroup("SamplerExecute");
    m_executeTopK = m_settings.value("top_k", m_executeTopK).toInt();
    m_executeTemp = m_settings.value("temp",  m_executeTemp).toFloat();
    m_executeTopP = m_settings.value("top_p", m_executeTopP).toFloat();
    m_executeMinP = m_settings.value("min_p", m_executeMinP).toFloat();
    m_settings.endGroup();

    m_settings.beginGroup("Agent");
    m_summarizeThreshold = m_settings.value("summarize_threshold",   m_summarizeThreshold).toInt();
    m_maxContinuations   = m_settings.value("max_continuations",     m_maxContinuations).toInt();
    m_deadlockWarn       = m_settings.value("deadlock_warn",         m_deadlockWarn).toInt();
    m_deadlockRedirect   = m_settings.value("deadlock_redirect",     m_deadlockRedirect).toInt();
    m_deadlockAbort      = m_settings.value("deadlock_abort",        m_deadlockAbort).toInt();
    m_maxToolResultChars = m_settings.value("max_tool_result_chars", m_maxToolResultChars).toInt();
    m_sandboxPath        = m_settings.value("sandbox_path",          m_sandboxPath).toString();
    m_userSystemPrompt   = m_settings.value("user_system_prompt",    "").toString();
    m_settings.endGroup();

    m_settings.beginGroup("WebSearch");
    m_tavilyApiKey = m_settings.value("tavily_api_key", m_tavilyApiKey).toString();
    m_settings.endGroup();

    m_settings.beginGroup("Logging");
    m_chatLoggingEnabled = m_settings.value("chat_enabled", m_chatLoggingEnabled).toBool();
    m_chatLogDir         = m_settings.value("chat_log_dir", m_chatLogDir).toString();
    m_settings.endGroup();

    const QString home = QDir::homePath();

    m_settings.beginGroup("ProjectIndex");
    m_indexSourceRoot  = "";
    m_indexSandboxRoot = m_settings.value("sandbox_root", home + "/llamatools").toString();
    m_indexCachePath   = m_settings.value("cache_path",
                         home + "/.cache/llamaqt/index.md").toString();
    m_indexAutoRebuild = m_settings.value("auto_rebuild", true).toBool();
    m_settings.endGroup();

    m_settings.beginGroup("TaskTree");
    m_taskDbPath = m_settings.value("db_path",
                   home + "/llamatools/tasks.sqlite").toString();
    m_settings.endGroup();

    m_settings.beginGroup("Execute");
    m_executeMemoryMaxEntries = m_settings.value("memory_max_entries",
                                m_executeMemoryMaxEntries).toInt();
    m_executeAutoMode         = m_settings.value("auto_mode",
                                m_executeAutoMode).toBool();
    m_executeSandboxProject   = m_settings.value("sandbox_project",
                                m_executeSandboxProject).toString();
    m_assembleOnlyDone        = m_settings.value("assemble_only_done",
                                m_assembleOnlyDone).toBool();
    m_debugExecute            = m_settings.value("debug_execute",
                                m_debugExecute).toBool();
    m_debugLogDir             = m_settings.value("debug_log_dir",
                                m_debugLogDir).toString();
    m_settings.endGroup();
}

void AppConfig::save()
{
    m_settings.beginGroup("Model");
    m_settings.setValue("path",                 m_modelPath);
    m_settings.setValue("context_size",         m_contextSize);
    m_settings.setValue("batch_size",           m_batchSize);
    m_settings.setValue("chat_template_preset", static_cast<int>(m_chatTemplatePreset));
    m_settings.setValue("custom_chat_template", m_customChatTemplate);
    // NEU: Tool-Call-Format
    m_settings.setValue("tool_call_format",     static_cast<int>(m_toolCallFormat));
    m_settings.endGroup();

    m_settings.beginGroup("SamplerChat");
    m_settings.setValue("top_k", m_chatTopK); m_settings.setValue("temp",  m_chatTemp);
    m_settings.setValue("top_p", m_chatTopP); m_settings.setValue("min_p", m_chatMinP);
    m_settings.endGroup();

    m_settings.beginGroup("SamplerTool");
    m_settings.setValue("top_k", m_toolTopK); m_settings.setValue("temp",  m_toolTemp);
    m_settings.setValue("top_p", m_toolTopP); m_settings.setValue("min_p", m_toolMinP);
    m_settings.endGroup();

    m_settings.beginGroup("SamplerExecute");
    m_settings.setValue("top_k", m_executeTopK); m_settings.setValue("temp",  m_executeTemp);
    m_settings.setValue("top_p", m_executeTopP); m_settings.setValue("min_p", m_executeMinP);
    m_settings.endGroup();

    m_settings.beginGroup("Agent");
    m_settings.setValue("summarize_threshold",   m_summarizeThreshold);
    m_settings.setValue("max_continuations",     m_maxContinuations);
    m_settings.setValue("deadlock_warn",         m_deadlockWarn);
    m_settings.setValue("deadlock_redirect",     m_deadlockRedirect);
    m_settings.setValue("deadlock_abort",        m_deadlockAbort);
    m_settings.setValue("max_tool_result_chars", m_maxToolResultChars);
    m_settings.setValue("sandbox_path",          m_sandboxPath);
    m_settings.setValue("user_system_prompt",    m_userSystemPrompt);
    m_settings.endGroup();

    m_settings.beginGroup("WebSearch");
    m_settings.setValue("tavily_api_key", m_tavilyApiKey);
    m_settings.endGroup();

    m_settings.beginGroup("Logging");
    m_settings.setValue("chat_enabled", m_chatLoggingEnabled);
    m_settings.setValue("chat_log_dir", m_chatLogDir);
    m_settings.endGroup();

    m_settings.beginGroup("ProjectIndex");
    m_settings.setValue("source_root",  m_indexSourceRoot);
    m_settings.setValue("sandbox_root", m_indexSandboxRoot);
    m_settings.setValue("cache_path",   m_indexCachePath);
    m_settings.setValue("auto_rebuild", m_indexAutoRebuild);
    m_settings.endGroup();

    m_settings.beginGroup("TaskTree");
    m_settings.setValue("db_path", m_taskDbPath);
    m_settings.endGroup();

    m_settings.beginGroup("Execute");
    m_settings.setValue("memory_max_entries", m_executeMemoryMaxEntries);
    m_settings.setValue("auto_mode",          m_executeAutoMode);
    m_settings.setValue("sandbox_project",    m_executeSandboxProject);
    m_settings.setValue("assemble_only_done", m_assembleOnlyDone);
    m_settings.setValue("debug_execute",      m_debugExecute);
    m_settings.setValue("debug_log_dir",      m_debugLogDir);
    m_settings.endGroup();

    m_settings.sync();
}
