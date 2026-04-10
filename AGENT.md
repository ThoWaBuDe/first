# LlamaQt — AGENT.md
> Projektgedächtnis für Claude und Qwen.
> Nach einer Session-Pause hier einlesen um sofort produktiv zu sein.

---

## Projektübersicht

**LlamaQt** ist eine lokale LLM-Chat-Oberfläche die zu einem vollständigen
**lokalen Coding-Agenten** ausgebaut wird.

- Inference: llama.cpp (Qwen3.5-9B-Q6_K, lokal, kein Cloud)
- Tool-Use: MCP-Server via stdio JSON-RPC 2.0
- GUI: Qt6 Widgets (QMainWindow, QDockWidget)
- Build: CMake, Debian Trixie

---

## Architektur (MVP-Pattern)

```
MainWindow  (View)
    │  signals/slots
    ▼
Agent       (Presenter)  ← Herzstück, GUI-Thread
    ├── ChatModel        — Gesprächshistorie, Prompt-Builder (Fallback)
    ├── McpManager       — Facade über mehrere McpClients
    │     ├── McpClient filesystem  (llamaqt-filesystem)
    │     ├── McpClient sysinfo     (llamaqt-sysinfo)
    │     ├── McpClient compile     (llamaqt-compile)
    │     └── McpClient websearch   (llamaqt-websearch)
    ├── CommandProcessor — Slash-Kommandos
    ├── ChatLogger       — Markdown-Logging in Datei
    ├── AppConfig        — Singleton, QSettings, INI-Persistenz
    └── LlamaWorker      (Active Object, Worker-Thread)
          └── llama.cpp
```

### Threading
- **GUI-Thread**: MainWindow, Agent, McpManager, McpClient
- **Worker-Thread**: LlamaWorker (blockierende llama.cpp Inference)
- Kommunikation: Qt Signals/Slots mit QueuedConnection
- Pattern: Generation Stamp (Session-ID) für sicheres onStop()

---

## Dateistruktur

```
LlamaQt/
├── CMakeLists.txt
├── AGENT.md
├── README.md
├── PROJECT_OVERVIEW.md      ← von Qwen generiert, Klassen + Abhängigkeiten
├── src/
│   ├── main.cpp
│   ├── MainWindow.h/.cpp/.ui
│   ├── Agent.h/.cpp
│   ├── CommandProcessor.h/.cpp
│   ├── ChatModel.h/.cpp
│   ├── ChatTemplate.h       ← Value Object, kein .cpp
│   ├── LlamaWorker.h/.cpp
│   ├── McpClient.h/.cpp
│   ├── McpManager.h/.cpp
│   ├── AppConfig.h/.cpp     ← Singleton, QSettings
│   ├── ChatLogger.h/.cpp    ← Markdown-Logging
│   ├── ConfigDialog.h/.cpp  ← modaler Settings-Dialog
│   └── EditorDock.h/.cpp    ← Code-Editor mit Syntax-Highlighting
└── mcp-servers/
    ├── filesystem/   — Dateioperationen + Git (v2.3)
    ├── sysinfo/      — Systeminfo + GPU (v2.0)
    ├── compile/      — Build + Retry-Loop (v2.0)
    └── websearch/    — Tavily API (v1.1)
```

---

## MCP-Server & Tools

### filesystem (llamaqt-filesystem v2.3)
Sandbox: `~/llamatools/` — Symlink-Schutz auf jeder Pfadebene.
Absolute Pfade werden in `resolvePath()` abgelehnt.
Git-Repo wird durch `/init` initialisiert. Vor jedem schreibenden
Zugriff: auto-commit. Remote-Operationen gesperrt.
Papierkorb: `~/.llamatools_trash/` für move_file bei Ziel-Konflikt.

| Tool                | Beschreibung                                          |
|---------------------|-------------------------------------------------------|
| read_file           | Datei lesen, optional Zeilenbereich (max 4096 Zeichen)|
| write_file          | Datei schreiben (auto-commit vorher, max 8192 Zeichen)|
| append_file         | Anhängen (auto-commit vorher)                         |
| str_replace         | Eindeutiger Replace (auto-commit vorher)              |
| patch_file          | Unified-Diff anwenden (multi-hunk, auto-commit)       |
| list_dir            | Verzeichnis auflisten ([D]/[F]/[L] Tags)              |
| list_symbols        | C++ Klassen/Methoden aus Quelldatei                   |
| mkdir               | Verzeichnis anlegen                                   |
| grep_code           | Regex-Suche rekursiv (max 100 Treffer)                |
| search_code         | Regex-Suche mit Kontext-Zeilen                        |
| tree                | Rekursiver Verzeichnisbaum (max depth 6)              |
| find_files          | Glob-Pattern Suche (rekursiv)                         |
| read_multiple_files | Bis zu 10 Dateien auf einmal lesen                    |
| move_file           | Verschieben/Umbenennen (Ziel → Papierkorb)            |
| copy_file           | Kopieren (Ziel darf nicht existieren)                 |
| git_status          | git status in Sandbox                                 |
| git_diff            | git diff (working tree gegen HEAD)                    |
| git_log             | git log (letzte N Commits)                            |
| git_checkout        | git checkout (Datei/Commit) — kein remote             |

### compile (llamaqt-compile v2.0)
| Tool        | Beschreibung                                               |
|-------------|------------------------------------------------------------|
| cmake_build | cmake + make, auto Retry-Loop (3x, Eskalation: Cache/Clean)|
| pkg_status  | installierte Pakete prüfen                                 |
| check_run   | Binary prüfen/starten (danger_zone:true = ausführen)       |

### sysinfo (llamaqt-sysinfo v2.0)
| Tool            | Beschreibung                                       |
|-----------------|----------------------------------------------------|
| get_time        | Datum, Uhrzeit, UTC, Unix-Timestamp                |
| get_pwd         | Sandbox-Root-Verzeichnis                           |
| disk_free       | Speicher-Status (df-Stil)                          |
| sys_info        | CPU-Kerne, Temp, RAM, OS, Kernel                   |
| gpu_info        | NVIDIA GPU: VRAM, Auslastung, Temp, Power, Clocks  |
| set_power_limit | GPU Power Limit setzen (nvidia-smi, mit Limits-Check)|

### websearch (llamaqt-websearch v1.1)
| Tool       | Beschreibung                                          |
|------------|-------------------------------------------------------|
| web_search | Tavily API: Title + URL + Snippet, max 10 Ergebnisse  |

---

## Sampler-Profile

| Profil | Top-K | Temp | Top-P | Min-P | Seed          | Verwendung           |
|--------|-------|------|-------|-------|---------------|----------------------|
| Chat   | 40    | 0.7  | 0.95  | 0.05  | /dev/urandom  | Normale Konversation |
| Tool   | 20    | 0.1  | 0.50  | 0.05  | /dev/urandom  | Tool-Calls           |

**Wichtig:** Seed wird vor **jeder** Generation neu aus `/dev/urandom` gezogen
(`refreshDistSampler()`). Kein fester Seed → kein deterministischer Loop.
Sampler-Parameter sind in AppConfig konfigurierbar und live neu baubar
(`rebuildSamplers()`).

### KV-Cache Konfiguration
```cpp
ctxParams.type_k = GGML_TYPE_Q8_0;  // K-Cache quantisiert
ctxParams.type_v = GGML_TYPE_Q8_0;  // V-Cache quantisiert
ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
```
Qwen3.5 hat hybride Architektur: Transformer-Attention-Layer + Recurrent-Layer
(Mamba/SSM). Recurrent-Layer brauchen keinen KV-Cache → VRAM-Ersparnis.

---

## Agenten-Loop

```
onUserMessage()
    → CommandProcessor::process()   (Slash-Kommando?)
    → checkContextUsage()           (>80% → auto-summarize)
    → startGeneration(Chat)
        → onTokenReceived()         [filterToken: think vs. sichtbar]
        → onGenerationDone()
            ├── Fall A: offener <tool_call>  → Continuation (max 3x)
            ├── Fall B: vollständiger Tool-Call
            │     → JSON validieren / reparieren
            │     → Deadlock-Check (3/5/7 Eskalation)
            │     → m_mcp.callTool()
            │     → git_diff anzeigen (bei Datei-Änderungen)
            │     → Tool-Ergebnis → startGeneration(Tool)
            └── Fall C: normale Antwort → fertig
```

---

## Slash-Kommandos (CommandProcessor)

| Kommando              | Aktion                                                    |
|-----------------------|-----------------------------------------------------------|
| `/init [Name]`        | Verzeichnis + Git-Repo + AGENT.md in Sandbox anlegen      |
| `/build`              | cmake + make (mit Retry-Loop)                             |
| `/compile`            | nur make                                                  |
| `/run`                | cmake (falls nötig) + make + Binary starten               |
| `/summarize`          | Konversation manuell zusammenfassen                       |
| `/undo [Datei]`       | letzten git-commit rückgängig (optional: nur eine Datei)  |
| `/diff`               | git diff anzeigen (letzte Änderungen)                     |

---

## Kontext-Management

- **80%-Schwelle**: bei >80% Auslastung automatisch zusammenfassen
- **`/summarize`**: manuell auslösen
- **Strategie**: LLM fasst bisherige Konversation zusammen.
  Neuer Kontext: System-Prompt + Zusammenfassung + aktuelle Aufgabe.

---

## Git als Undo-System

Sandbox = Git-Repo (eines pro Projekt-Unterverzeichnis).
Vor jedem schreibenden Tool-Call: auto-commit.
```
git add -A && git commit -m "auto: str_replace datei.cpp"
```
- `/undo`  → git checkout HEAD~1
- `/diff`  → git diff HEAD
**Gesperrt**: push / pull / remote / fetch / clone

---

## Chat-Template

Zwei Quellen, konfigurierbar in AppConfig / ConfigDialog:
1. **Auto** (Standard): `llama_chat_apply_template()` liest Template direkt
   aus GGUF-Metadaten (`tokenizer.chat_template`). Worker emittiert
   `chatTemplateDetected()` Signal mit erkanntem Preset.
2. **Manuell**: Preset-Auswahl im ConfigDialog (ChatML / Llama3 / Gemma / Mistral)
   oder Custom JSON.

Fallback-Kette:
```
llama_chat_apply_template() → Puffer zu klein → resize → retry
    → fehlgeschlagen → ChatModel::buildPrompt() mit ChatTemplate-Struct
```

---

## AppConfig (Singleton)

Persistiert in `~/.config/LlamaQt/LlamaQt.conf` (INI-Format).

| Gruppe      | Parameter                                          |
|-------------|----------------------------------------------------|
| Model       | path, context_size, batch_size, chat_template      |
| SamplerChat | top_k, temp, top_p, min_p                          |
| SamplerTool | top_k, temp, top_p, min_p                          |
| Agent       | summarize_threshold, max_continuations, deadlock_* |
|             | max_tool_result_chars, sandbox_path                |
| WebSearch   | tavily_api_key                                     |
| Logging     | chat_enabled, chat_log_dir                         |

Zwei Klassen von Parametern:
- **Sofort wirksam**: Sampler, Schwellen, Logging
- **Neustart nötig**: Modellpfad, n_ctx, Chat-Template

---

## EditorDock

Andockbares Code-Editor-Fenster (`QDockWidget`):
- QPlainTextEdit mit C++ Syntax-Highlighting (CppHighlighter)
- QFileSystemWatcher: externe Änderungen (durch Modell) werden markiert
- Dirty-Flag: User-Änderungen werden mit `*` im Tab-Titel angezeigt
- Extern-Flag: Modell-Änderungen werden mit `!` angezeigt
- Bei manuellem Speichern (Ctrl+S): `fileSavedByUser()` Signal →
  Systemnachricht in Chat-Kontext (Modell wird informiert)
- Öffnet Dateien aus `~/llamatools/` Sandbox

---

## ChatLogger

Schreibt Konversation in `~/llamatools/chat_log/chat_YYYY-MM-DD_HH-mm-ss.md`.
Jede Session = eigene Datei. Format: Markdown mit aufklappbaren Thinking-Blöcken.
Aktivierbar in AppConfig / ConfigDialog.

---

## Deadlock-Erkennung

| Fehlerzahl | Eskalation                         |
|------------|------------------------------------|
| 3          | Warnung — Argumente prüfen         |
| 5          | Umleitung — anderen Weg suchen     |
| 7          | Abbruch — Erklärung an User        |

Schlüssel: `toolName + JSON-Arguments` → Counter. Bei Erfolg: Counter gelöscht.

---

## LlamaWorker — generate()-Varianten

```cpp
// Cache leeren vor Encode (bisheriges Verhalten, stabil):
void generate(messages, profile);

// Cache behalten — Delta-Encoding (Vorbereitung, noch nicht aktiv):
void generateDelta(messages, profile);

// Gemeinsamer Kern:
void doGenerate(messages, profile, clearCache);
```

`generateDelta()` ist vorbereitet aber noch nicht im Agent verdrahtet.
Nächste Ausbaustufe: n_past-Tracking für echtes Delta-Encoding.

---

## Offene TODOs

### Implementiert ✓
- [x] MVP-Architektur (MainWindow/Agent/ChatModel)
- [x] MCP stdio JSON-RPC (McpClient/McpManager)
- [x] Zwei Sampler-Profile (Chat/Tool)
- [x] Thinking-Filter (<think>...</think>)
- [x] Tool-Deadlock-Erkennung (3/5/7 Eskalation)
- [x] JSON-Repair (kaputte Tool-Calls reparieren)
- [x] Stop-Bug-Fix (Session-ID Pattern)
- [x] Smart-Autoscroll (beide QTextEdit)
- [x] Word-Wrap in toolView
- [x] QTextEdit Eingabe (Shift+Enter = Umbruch)
- [x] CommandProcessor (/init /build /compile /run /summarize /undo /diff)
- [x] AGENT.md Projektgedächtnis
- [x] Kontext-Management (80%-Auto + /summarize)
- [x] Git-Integration im filesystem MCP (pro Projekt-Verzeichnis)
- [x] grep_code + search_code + tree + find_files Tools
- [x] read_multiple_files (bis zu 10 Dateien parallel)
- [x] patch_file Tool (unified diff, multi-hunk)
- [x] move_file + copy_file Tools
- [x] Compile-Retry-Loop (3x, Eskalation)
- [x] git_diff Anzeige nach Datei-Änderungen (farbig, im toolView)
- [x] /undo + /diff Kommandos
- [x] Token-Budget (Tool-Ergebnisse kürzen, konfigurierbar)
- [x] ChatLogger (Markdown, pro Session)
- [x] AppConfig / QSettings (Singleton, INI-Format)
- [x] gpu_info + set_power_limit Tools (nvidia-smi)
- [x] CPU-Temperatur in sys_info (/sys/class/thermal)
- [x] ConfigDialog (Tabs: Modell/Sampler/Agent/Logging/Template)
- [x] LlamaWorker liest Sampler aus AppConfig, rebuildSamplers() live
- [x] Chat-Template: Auto aus GGUF + manuelle Auswahl in ConfigDialog
- [x] EditorDock: Code-Editor mit Syntax-Highlighting + QFileSystemWatcher
- [x] KV-Cache Quantisierung (Q8_0 für K und V)
- [x] randomSeed() via /dev/urandom — kein deterministischer Loop mehr
- [x] doGenerate() + generateDelta() — Vorbereitung Delta-Encoding
- [x] PROJECT_OVERVIEW.md durch Qwen generiert

### Offen — nächste Schritte
- [ ] generateDelta() aktivieren: n_past-Tracking in LlamaWorker
- [ ] KV-Cache Rollback: llama_kv_cache_seq_rm nach Tool-Fehler
- [ ] maxNewTokens in AppConfig (Chat: 8192, Tool: 2048)
- [ ] AST-Integration: clangd/tree-sitter für Projektanalyse
- [ ] Planner/Executor-Trennung (Meilensteine, Teilaufgaben)
- [ ] Live-Output bei /run (stdout streaming)
- [ ] Automatische AGENT.md-Aktualisierung nach Session
- [ ] Mehrere Modelle (klein für Tool-Calls, groß für Planung)
- [ ] MCP-Server Neustart bei Absturz
- [ ] Persistentes Konversationsgedächtnis zwischen Sessions
- [ ] XTC-Sampler Option (konfigurierbar, nur Chat-Profil)
- [ ] DRY-Sampler Option (gegen logische Endlosschleifen)

---

## Build

```bash
cd ~/llamaqt/build
cmake .. \
  -DLLAMA_BUILD_DIR=$HOME/ai/qLP/build \
  -DLLAMA_SRC_DIR=$HOME/ai/qLP \
  -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Laufzeit

```bash
export TAVILY_API_KEY="tvly-..."   # optional
./LlamaQt
```
