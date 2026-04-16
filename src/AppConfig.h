#pragma once
#include <QObject>
#include <QString>
#include <QSettings>
#include "ChatTemplate.h"

// ─── AppConfig ────────────────────────────────────────────────────────────────
// Singleton — hält alle persistenten Einstellungen.
//
// NEU (Punkt I): SamplerExecute-Profil
//   Eigene Sampler-Parameter für den Execute-Modus.
//   Rationale: Execute codiert deterministisch (niedrige Temp),
//   braucht aber etwas mehr Kreativität als Tool-Calls (Temp 0.2 statt 0.1).
//   Optimale Werte: top_k=20, temp=0.2, top_p=0.60, min_p=0.05

class AppConfig : public QObject {
    Q_OBJECT
public:
    static AppConfig &instance();
    void load();
    void save();

    // ── Modell ────────────────────────────────────────────────────────────
    QString modelPath() const           { return m_modelPath; }
    void setModelPath(const QString &v) { m_modelPath = v; save();
                                          emit modelPathChanged(v); }

    // ── Sampler Chat ──────────────────────────────────────────────────────
    int   chatTopK() const { return m_chatTopK; }
    float chatTemp() const { return m_chatTemp; }
    float chatTopP() const { return m_chatTopP; }
    float chatMinP() const { return m_chatMinP; }
    void setChatTopK(int v)   { m_chatTopK = v; save(); }
    void setChatTemp(float v) { m_chatTemp = v; save(); }
    void setChatTopP(float v) { m_chatTopP = v; save(); }
    void setChatMinP(float v) { m_chatMinP = v; save(); }

    // ── Sampler Tool ──────────────────────────────────────────────────────
    int   toolTopK() const { return m_toolTopK; }
    float toolTemp() const { return m_toolTemp; }
    float toolTopP() const { return m_toolTopP; }
    float toolMinP() const { return m_toolMinP; }
    void setToolTopK(int v)   { m_toolTopK = v; save(); }
    void setToolTemp(float v) { m_toolTemp = v; save(); }
    void setToolTopP(float v) { m_toolTopP = v; save(); }
    void setToolMinP(float v) { m_toolMinP = v; save(); }

    // ── Sampler Execute (NEU — Punkt I) ───────────────────────────────────
    // Eigenes Profil für den Execute-Modus (Code-Generierung).
    // Bewusst zwischen Chat (kreativ) und Tool (deterministisch):
    //   - Niedrigere Temp als Chat → weniger Halluzination im Code
    //   - Höhere Temp als Tool → Algorithmen brauchen etwas Flexibilität
    int   executeTopK() const { return m_executeTopK; }
    float executeTemp() const { return m_executeTemp; }
    float executeTopP() const { return m_executeTopP; }
    float executeMinP() const { return m_executeMinP; }
    void setExecuteTopK(int v)   { m_executeTopK = v; save(); }
    void setExecuteTemp(float v) { m_executeTemp = v; save(); }
    void setExecuteTopP(float v) { m_executeTopP = v; save(); }
    void setExecuteMinP(float v) { m_executeMinP = v; save(); }

    // ── Kontext ───────────────────────────────────────────────────────────
    int contextSize() const    { return m_contextSize; }
    int batchSize() const      { return m_batchSize; }
    void setContextSize(int v) { m_contextSize = v; save(); }
    void setBatchSize(int v)   { m_batchSize = v; save(); }

    // ── Agent ─────────────────────────────────────────────────────────────
    int  summarizeThreshold() const   { return m_summarizeThreshold; }
    int  maxContinuations() const     { return m_maxContinuations; }
    int  deadlockWarn() const         { return m_deadlockWarn; }
    int  deadlockRedirect() const     { return m_deadlockRedirect; }
    int  deadlockAbort() const        { return m_deadlockAbort; }
    int  maxToolResultChars() const   { return m_maxToolResultChars; }
    void setSummarizeThreshold(int v) { m_summarizeThreshold = v; save(); }
    void setMaxContinuations(int v)   { m_maxContinuations = v; save(); }
    void setMaxToolResultChars(int v) { m_maxToolResultChars = v; save(); }
    void setDeadlockWarn(int v)       { m_deadlockWarn = v; save(); }
    void setDeadlockRedirect(int v)   { m_deadlockRedirect = v; save(); }
    void setDeadlockAbort(int v)      { m_deadlockAbort = v; save(); }

    QString sandboxPath() const           { return m_sandboxPath; }
    void setSandboxPath(const QString &v) { m_sandboxPath = v; save(); }

    // ── Web-Search ────────────────────────────────────────────────────────
    QString tavilyApiKey() const           { return m_tavilyApiKey; }
    void setTavilyApiKey(const QString &v) { m_tavilyApiKey = v; save(); }

    // ── Logging ───────────────────────────────────────────────────────────
    bool    chatLoggingEnabled() const   { return m_chatLoggingEnabled; }
    QString chatLogDir() const           { return m_chatLogDir; }
    void setChatLoggingEnabled(bool v)   { m_chatLoggingEnabled = v; save();
                                           emit chatLoggingChanged(v); }
    void setChatLogDir(const QString &v) { m_chatLogDir = v; save(); }

    // ── Chat-Template ─────────────────────────────────────────────────────
    ChatTemplate::Preset chatTemplatePreset() const { return m_chatTemplatePreset; }
    QString customChatTemplate() const              { return m_customChatTemplate; }
    QString detectedJinjaTemplate() const           { return m_detectedJinjaTemplate; }
    void setChatTemplatePreset(ChatTemplate::Preset v) {
        m_chatTemplatePreset = v; save(); emit chatTemplateChanged(); }
    void setCustomChatTemplate(const QString &v) {
        m_customChatTemplate = v; save(); emit chatTemplateChanged(); }
    void setDetectedJinjaTemplate(const QString &v) { m_detectedJinjaTemplate = v; }

    // ── User System-Prompt ────────────────────────────────────────────────
    QString userSystemPrompt() const           { return m_userSystemPrompt; }
    void setUserSystemPrompt(const QString &v) { m_userSystemPrompt = v; save(); }

    // ── Projekt-Index ─────────────────────────────────────────────────────
    QString indexSourceRoot() const            { return m_indexSourceRoot; }
    QString indexSandboxRoot() const           { return m_indexSandboxRoot; }
    QString indexCachePath() const             { return m_indexCachePath; }
    bool    indexAutoRebuild() const           { return m_indexAutoRebuild; }
    void setIndexSourceRoot(const QString &v)  { m_indexSourceRoot = v;  save(); }
    void setIndexSandboxRoot(const QString &v) { m_indexSandboxRoot = v; save(); }
    void setIndexCachePath(const QString &v)   { m_indexCachePath = v;   save(); }
    void setIndexAutoRebuild(bool v)           { m_indexAutoRebuild = v; save(); }

    // ── TaskTree ──────────────────────────────────────────────────────────
    QString taskDbPath() const           { return m_taskDbPath; }
    void setTaskDbPath(const QString &v) { m_taskDbPath = v; save(); }

    // ── Execute ───────────────────────────────────────────────────────────
    int     executeMemoryMaxEntries() const        { return m_executeMemoryMaxEntries; }
    void setExecuteMemoryMaxEntries(int v)         { m_executeMemoryMaxEntries = v; save(); }
    bool    executeAutoMode() const                { return m_executeAutoMode; }
    void setExecuteAutoMode(bool v)                { m_executeAutoMode = v; save(); }
    QString executeSandboxProject() const          { return m_executeSandboxProject; }
    void setExecuteSandboxProject(const QString &v){ m_executeSandboxProject = v; save(); }
    bool    assembleOnlyDone() const               { return m_assembleOnlyDone; }
    void setAssembleOnlyDone(bool v)               { m_assembleOnlyDone = v; save(); }

    // Debug-Logging Execute
    bool    debugExecute() const                   { return m_debugExecute; }
    void setDebugExecute(bool v)                   { m_debugExecute = v; save(); }
    QString debugLogDir() const                    { return m_debugLogDir; }
    void setDebugLogDir(const QString &v)          { m_debugLogDir = v; save(); }

signals:
    void chatLoggingChanged(bool enabled);
    void modelPathChanged(const QString &path);
    void chatTemplateChanged();

private:
    explicit AppConfig(QObject *parent = nullptr);
    QSettings m_settings;

    // ── Defaults ──────────────────────────────────────────────────────────
    QString m_modelPath   = "/home/thomas/ai/models/Qwen3.5-9B-Q6_K.gguf";

    // Chat: kreativ
    int   m_chatTopK = 40;    float m_chatTemp = 0.7f;
    float m_chatTopP = 0.95f; float m_chatMinP = 0.05f;

    // Tool: deterministisch (JSON-Ausgabe)
    int   m_toolTopK = 20;    float m_toolTemp = 0.1f;
    float m_toolTopP = 0.50f; float m_toolMinP = 0.05f;

    // Execute: zwischen Chat und Tool (Code-Generierung)
    // Rationale: temp=0.2 → weniger Halluzination als Chat,
    //            mehr Flexibilität als Tool (Algorithmen sind keine JSON-Strukturen)
    int   m_executeTopK = 20;    float m_executeTemp = 0.2f;
    float m_executeTopP = 0.60f; float m_executeMinP = 0.05f;

    int m_contextSize = 128 * 1024;
    int m_batchSize   = 512;

    int m_summarizeThreshold  = 80;
    int m_maxContinuations    = 3;
    int m_deadlockWarn        = 3;
    int m_deadlockRedirect    = 5;
    int m_deadlockAbort       = 7;
    int m_maxToolResultChars  = 6000;

    QString m_sandboxPath     = "";
    QString m_tavilyApiKey    = "";
    bool    m_chatLoggingEnabled = false;
    QString m_chatLogDir         = "";

    ChatTemplate::Preset m_chatTemplatePreset    = ChatTemplate::Preset::Auto;
    QString              m_customChatTemplate    = "";
    QString              m_detectedJinjaTemplate = "";
    QString m_userSystemPrompt = "";

    QString m_indexSourceRoot  = "";
    QString m_indexSandboxRoot = "";
    QString m_indexCachePath   = "";
    bool    m_indexAutoRebuild = true;

    QString m_taskDbPath = "";

    int     m_executeMemoryMaxEntries = 50;
    bool    m_executeAutoMode         = true;
    QString m_executeSandboxProject   = "";
    bool    m_assembleOnlyDone        = true;
    bool    m_debugExecute            = true;
    QString m_debugLogDir             = "/home/thomas/llamatools/debug";
};
