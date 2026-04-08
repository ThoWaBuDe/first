#pragma once
#include <QObject>
#include <QString>
#include <QSettings>
#include "ChatTemplate.h"

// ─── AppConfig ────────────────────────────────────────────────────────────────
// Zentrales Konfigurations-Singleton.
//
// Pattern: Singleton — eine Instanz, global erreichbar via AppConfig::instance().
// Persistenz: QSettings im INI-Format unter ~/.config/LlamaQt/LlamaQt.conf
//
// Zwei Klassen von Parametern:
//   Sofort wirksam  — Schwellen, Sampler-Werte, Logging → Agent übernimmt live
//   Neustart nötig  — Modellpfad, n_ctx, Chat-Template → LlamaWorker::cleanup()+initialize()
//
// Verwendung:
//   AppConfig::instance().modelPath()         // lesen
//   AppConfig::instance().setModelPath(p);    // setzen + sofort in Datei speichern

class AppConfig : public QObject {
    Q_OBJECT

public:
    static AppConfig &instance();

    void load();
    void save();

    // ─── Modell ───────────────────────────────────────────────────────────
    QString modelPath() const          { return m_modelPath; }
    void setModelPath(const QString &v){ m_modelPath = v; save(); emit modelPathChanged(v); }

    // ─── Sampler Chat ─────────────────────────────────────────────────────
    int     chatTopK() const           { return m_chatTopK; }
    float   chatTemp() const           { return m_chatTemp; }
    float   chatTopP() const           { return m_chatTopP; }
    float   chatMinP() const           { return m_chatMinP; }
    void setChatTopK(int v)            { m_chatTopK = v; save(); }
    void setChatTemp(float v)          { m_chatTemp = v; save(); }
    void setChatTopP(float v)          { m_chatTopP = v; save(); }
    void setChatMinP(float v)          { m_chatMinP = v; save(); }

    // ─── Sampler Tool ─────────────────────────────────────────────────────
    int     toolTopK() const           { return m_toolTopK; }
    float   toolTemp() const           { return m_toolTemp; }
    float   toolTopP() const           { return m_toolTopP; }
    float   toolMinP() const           { return m_toolMinP; }
    void setToolTopK(int v)            { m_toolTopK = v; save(); }
    void setToolTemp(float v)          { m_toolTemp = v; save(); }
    void setToolTopP(float v)          { m_toolTopP = v; save(); }
    void setToolMinP(float v)          { m_toolMinP = v; save(); }

    // ─── Kontext ──────────────────────────────────────────────────────────
    int     contextSize() const        { return m_contextSize; }
    int     batchSize() const          { return m_batchSize; }
    void setContextSize(int v)         { m_contextSize = v; save(); }
    void setBatchSize(int v)           { m_batchSize = v; save(); }

    // ─── Agent-Verhalten ──────────────────────────────────────────────────
    int     summarizeThreshold() const { return m_summarizeThreshold; }
    int     maxContinuations() const   { return m_maxContinuations; }
    int     deadlockWarn() const       { return m_deadlockWarn; }
    int     deadlockRedirect() const   { return m_deadlockRedirect; }
    int     deadlockAbort() const      { return m_deadlockAbort; }
    int     maxToolResultChars() const { return m_maxToolResultChars; }
    void setSummarizeThreshold(int v)  { m_summarizeThreshold = v; save(); }
    void setMaxContinuations(int v)    { m_maxContinuations = v; save(); }
    void setMaxToolResultChars(int v)  { m_maxToolResultChars = v; save(); }
    void setDeadlockWarn(int v)        { m_deadlockWarn = v; save(); }
    void setDeadlockRedirect(int v)    { m_deadlockRedirect = v; save(); }
    void setDeadlockAbort(int v)       { m_deadlockAbort = v; save(); }

    // ─── Sandbox ──────────────────────────────────────────────────────────
    QString sandboxPath() const        { return m_sandboxPath; }
    void setSandboxPath(const QString &v){ m_sandboxPath = v; save(); }

    // ─── Web-Search ───────────────────────────────────────────────────────
    QString tavilyApiKey() const       { return m_tavilyApiKey; }
    void setTavilyApiKey(const QString &v){ m_tavilyApiKey = v; save(); }

    // ─── Logging ──────────────────────────────────────────────────────────
    bool    chatLoggingEnabled() const { return m_chatLoggingEnabled; }
    QString chatLogDir() const         { return m_chatLogDir; }
    void setChatLoggingEnabled(bool v) { m_chatLoggingEnabled = v; save();
                                         emit chatLoggingChanged(v); }
    void setChatLogDir(const QString &v){ m_chatLogDir = v; save(); }

    // ─── Chat-Template ────────────────────────────────────────────────────
    // Neustart nötig wenn geändert.
    // detectedJinjaTemplate: vom LlamaWorker nach Modell-Laden gesetzt,
    // wird NICHT persistiert (kommt immer neu aus dem GGUF).
    ChatTemplate::Preset chatTemplatePreset() const  { return m_chatTemplatePreset; }
    QString customChatTemplate()  const              { return m_customChatTemplate; }
    QString detectedJinjaTemplate() const            { return m_detectedJinjaTemplate; }

    void setChatTemplatePreset(ChatTemplate::Preset v) {
        m_chatTemplatePreset = v; save(); emit chatTemplateChanged();
    }
    void setCustomChatTemplate(const QString &v) {
        m_customChatTemplate = v; save(); emit chatTemplateChanged();
    }
    // Kein save() — kommt aus GGUF, nur RAM, nicht persistiert
    void setDetectedJinjaTemplate(const QString &v) { m_detectedJinjaTemplate = v; }

    // ─── User System-Prompt ───────────────────────────────────────────────
    // Sofort wirksam nach /clear oder Neustart.
    // Wird dem MCP-Tool-Prompt vorangestellt (User-Wünsche haben Vorrang).
    QString userSystemPrompt() const           { return m_userSystemPrompt; }
    void setUserSystemPrompt(const QString &v) { m_userSystemPrompt = v; save(); }

signals:
    void chatLoggingChanged(bool enabled);
    void modelPathChanged(const QString &path);

    // Chat-Template oder User-Prompt geändert → Neustart-Hinweis im UI
    void chatTemplateChanged();

private:
    explicit AppConfig(QObject *parent = nullptr);

    QSettings m_settings;

    // ─── Werte mit Defaults ───────────────────────────────────────────────
    QString m_modelPath       = "/home/thomas/ai/models/Qwen3.5-9B-Q6_K.gguf";

    int     m_chatTopK        = 40;
    float   m_chatTemp        = 0.7f;
    float   m_chatTopP        = 0.95f;
    float   m_chatMinP        = 0.05f;

    int     m_toolTopK        = 20;
    float   m_toolTemp        = 0.1f;
    float   m_toolTopP        = 0.50f;
    float   m_toolMinP        = 0.05f;

    int     m_contextSize     = 128 * 1024;
    int     m_batchSize       = 512;

    int     m_summarizeThreshold  = 80;
    int     m_maxContinuations    = 3;
    int     m_deadlockWarn        = 3;
    int     m_deadlockRedirect    = 5;
    int     m_deadlockAbort       = 7;
    int     m_maxToolResultChars  = 6000;

    QString m_sandboxPath     = "";
    QString m_tavilyApiKey    = "";

    bool    m_chatLoggingEnabled = false;
    QString m_chatLogDir      = "";

    // ─── Chat-Template ────────────────────────────────────────────────────
    ChatTemplate::Preset m_chatTemplatePreset   = ChatTemplate::Preset::Auto;
    QString              m_customChatTemplate   = "";
    QString              m_detectedJinjaTemplate = "";  // nicht persistiert

    // ─── User System-Prompt ───────────────────────────────────────────────
    QString m_userSystemPrompt = "";
};
