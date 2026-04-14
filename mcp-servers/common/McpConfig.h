#pragma once
// ─── McpConfig ────────────────────────────────────────────────────────────────
// Leichtgewichtiger INI-Leser für MCP-Server-Prozesse.
//
// Problem: AppConfig ist ein Singleton der Hauptanwendung (Qt-Objekt, Signals).
// MCP-Server sind separate Prozesse — sie können AppConfig nicht verwenden.
//
// Lösung: McpConfig liest dieselbe INI-Datei direkt via QSettings,
// ohne Singleton, ohne QObject, ohne Signals.
//
// Analogie (AVR): wie ein zweiter Mikrocontroller der dieselbe EEPROM-Datei
// liest — er kennt das Singleton des ersten nicht, nur das Dateiformat.
//
// Verwendung in MCP-Server main():
//   McpConfig cfg;
//   QString sandbox = cfg.sandboxRoot();
//   QString sources = cfg.sourceRoot();
//   QString cache   = cfg.cachePath();

#include <QString>
#include <QSettings>
#include <QDir>
#include <QStandardPaths>

class McpConfig
{
public:
    // Liest beim Konstruieren einmalig die INI-Datei.
    // Danach sind alle Werte im RAM — kein weiterer Dateizugriff.
    McpConfig()
        : m_settings(QSettings::IniFormat,
                     QSettings::UserScope,
                     "LlamaQt", "LlamaQt")
    {
        const QString home = QDir::homePath();

        // ── Agent ──────────────────────────────────────────────────────────
        m_settings.beginGroup("Agent");
        m_sandboxPath = m_settings.value("sandbox_path",
                            home + "/llamatools").toString();
        m_settings.endGroup();

        // ── ProjectIndex ───────────────────────────────────────────────────
        m_settings.beginGroup("ProjectIndex");
        m_sourceRoot  ="";// m_settings.value("source_root",
                          //  home + "/ai/LlamaQT").toString();
        m_sandboxRoot = m_settings.value("sandbox_root",
                            home + "/llamatools").toString();
        m_cachePath   = m_settings.value("cache_path",
                            home + "/.cache/llamaqt/index.md").toString();
        m_autoRebuild = m_settings.value("auto_rebuild", true).toBool();
        m_settings.endGroup();

        // ── WebSearch ──────────────────────────────────────────────────────
        m_settings.beginGroup("WebSearch");
        m_tavilyApiKey = m_settings.value("tavily_api_key", "").toString();
        m_settings.endGroup();

        // Umgebungsvariablen überschreiben INI-Werte.
        // So können Docker/systemd-Umgebungen die Pfade übersteuern
        // ohne die INI zu ändern — dasselbe Pattern wie bisher.
        if (!qEnvironmentVariable("LLAMAQT_SANDBOX").isEmpty())
            m_sandboxRoot = qEnvironmentVariable("LLAMAQT_SANDBOX");
        if (!qEnvironmentVariable("LLAMAQT_SOURCES").isEmpty())
            m_sourceRoot = qEnvironmentVariable("LLAMAQT_SOURCES");
        if (!qEnvironmentVariable("LLAMAQT_TRASH").isEmpty())
            m_trashPath = qEnvironmentVariable("LLAMAQT_TRASH");
        else
            m_trashPath = home + "/.llamatools_trash";
    }

    // ── Getter ────────────────────────────────────────────────────────────
    QString sandboxPath()  const { return m_sandboxPath.isEmpty()
                                       ? m_sandboxRoot : m_sandboxPath; }
    QString sourceRoot()   const { return m_sourceRoot; }
    QString sandboxRoot()  const { return m_sandboxRoot; }
    QString cachePath()    const { return m_cachePath; }
    QString trashPath()    const { return m_trashPath; }
    QString tavilyApiKey() const { return m_tavilyApiKey; }
    bool    autoRebuild()  const { return m_autoRebuild; }

private:
    QSettings m_settings;

    QString m_sandboxPath;   // aus [Agent]/sandbox_path (legacy)
    QString m_sourceRoot;    // aus [ProjectIndex]/source_root
    QString m_sandboxRoot;   // aus [ProjectIndex]/sandbox_root
    QString m_cachePath;     // aus [ProjectIndex]/cache_path
    QString m_trashPath;     // aus Umgebungsvariable oder Default
    QString m_tavilyApiKey;  // aus [WebSearch]/tavily_api_key
    bool    m_autoRebuild = true;
};
