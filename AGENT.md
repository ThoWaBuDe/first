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
    │     ├── McpClient websearch   (llamaqt-websearch)
    │     └── McpClient treesitter  (llamaqt-treesitter)
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
    ├── common/              ← NEU: gemeinsame Infrastruktur (header-only)
    │   ├── McpServer.h      ← JSON-RPC Loop + Tool-Registry
    │   └── PathPolicy.h     ← Pfad-Auflösung + Sandbox-Sicherheit
    ├── filesystem/   — Dateioperationen + Git (v2.3, Umbau ausstehend)
    ├── sysinfo/      — Systeminfo + GPU (v2.0)
    ├── compile/      — Build + Retry-Loop (v2.0)
    ├── websearch/    — Tavily API (v1.1)
    └── tree-sitter/  — AST-Analyse C/C++/CMake (v2.0, NEU)
        ├── CMakeLists.txt
        ├── main.cpp         ← verwendet McpServer.h + PathPolicy.h
        └── vendor/          ← git submodules
            ├── tree-sitter/
            ├── tree-sitter-c/   (nicht genutzt, C++ Grammar reicht)
            ├── tree-sitter-cpp/
            └── tree-sitter-cmake/
```

---

## MCP-Common Bibliothek (header-only)

### PathPolicy (`common/PathPolicy.h`)
Gemeinsame Pfad-Sicherheitslogik für alle Server.

```cpp
struct Root { QString path; bool writable; };

PathPolicy policy;
policy.addRoot("/home/thomas/llamatools",  true);   // Sandbox: lesen+schreiben
policy.addRoot("/home/thomas/ai/LlamaQT", false);   // Sources: nur lesen

auto rp = policy.resolveRead("src/Agent.h");   // → absoluter Pfad
auto rp = policy.resolveWrite("LlamaQT/x.cpp"); // → nur wenn in writable Root
// rp.valid, rp.absPath, rp.writable, rp.error
```

Sicherheit: Symlink-Check auf jeder Pfad-Komponente, Sandbox-Escape verhindert.
Pfad-Auflösung: relativ → erste Root in der Pfad existiert gewinnt; absolut → Root-Zugehörigkeit prüfen.

### McpServer (`common/McpServer.h`)
JSON-RPC 2.0 Loop + Tool-Registry. Pattern: Template Method + Command + Registry.

```cpp
McpServer server("llamaqt-xyz", "1.0");

server.registerTool({
    "tool_name", "Beschreibung",
    QJsonObject{{"path", ...}},   // properties
    {"path"},                      // required
    [&](const QJsonObject &args) -> ToolResult {
        return ToolResult::ok("Ergebnis");
        // oder: return ToolResult::err("Fehlermeldung");
    }
});

server.run();  // blockiert bis stdin EOF
```

**WICHTIG — Nächster Ausbauschritt (noch nicht implementiert):**
Tool-Lambdas sollen durch echte Klassen ersetzt werden:
```cpp
// Geplant — noch NICHT im Code:
class ReadFileTool : public ToolBase {
public:
    ReadFileTool(PathPolicy &policy) : m_policy(policy) {}
    ToolResult execute(const QJsonObject &args) override;
private:
    PathPolicy &m_policy;  // Dependency Injection
};
```
Vorteil: eine .cpp Datei pro Tool (eigene Übersetzungseinheit), testbar,
gemeinsame Ressourcen via Konstruktor injizierbar.

---

## MCP-Server & Tools

### Tool-Priorität für Code-Analyse
**IMMER in dieser Reihenfolge:**
1. `llamaqt-treesitter` zuerst — strukturelle Analyse
2. `llamaqt-filesystem` danach — nur was tree-sitter nicht kann

```
list_symbols(path)              → Überblick: was ist in dieser Datei?
get_class_hierarchy(path)       → Vererbung verstehen
get_includes(path)              → Abhängigkeiten
get_class_members(path, class)  → Klasse verstehen vor Änderung
get_function_body(path, func)   → gezielt eine Funktion lesen
check_syntax(path)              → nach jeder Änderung prüfen
```

**Pfade:** tree-sitter akzeptiert absolut UND relativ zur Sandbox.
`get_pwd` (sysinfo) liefert den Sandbox-Root.
LlamaQT-Quellcode liegt in `/home/thomas/ai/LlamaQT/` (read-only für Modell).

### filesystem (llamaqt-filesystem v2.3)
**Status: Umbau auf McpServer+PathPolicy ausstehend (nächste Session)**

Aktuelle Einschränkung: akzeptiert nur relative Pfade (absolute werden abgelehnt).
Nach Umbau: absolute Pfade erlaubt wenn in bekannter Root.

Sandbox: `~/llamatools/` — Symlink-Schutz auf jeder Pfadebene.
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
| mkdir               | Verzeichnis anlegen                                   |
| grep_code           | Regex-Suche rekursiv (max 100 Treffer)                |
| search_code         | Regex-Suche mit Kontext-Zeilen                        |
| tree                | Rekursiver Verzeichnisbaum (max depth 6)              |
| find_files          | Glob-Pattern Suche (rekursiv)                         |
| read_multiple_files | Bis zu 15 Dateien auf einmal lesen                    |
| move_file           | Verschieben/Umbenennen (Ziel → Papierkorb)            |
| copy_file           | Kopieren (Ziel darf nicht existieren)                 |
| git_status          | git status in Sandbox                                 |
| git_diff            | git diff (working tree gegen HEAD)                    |
| git_log             | git log (letzte N Commits)                            |
| git_checkout        | git checkout (Datei/Commit) — kein remote             |

### treesitter (llamaqt-treesitter v2.0) — NEU
Zwei Roots (Umgebungsvariablen):
- `LLAMAQT_SANDBOX` (Standard: `~/llamatools`) — read+write
- `LLAMAQT_SOURCES` (Standard: `~/ai/LlamaQT`) — read-only

Grammars: C++ (für .c .cpp .h .hpp), CMake.
Qt-Macros (Q_OBJECT, signals:, slots:) werden vor dem Parsen neutralisiert.

| Tool                | Beschreibung                                          |
|---------------------|-------------------------------------------------------|
| list_symbols        | Funktionen, Klassen, Structs, Enums + Zeilennummern   |
| get_function_body   | Kompletten Funktionsrumpf extrahieren                 |
| get_class_members   | Members + Methoden einer Klasse/Struct                |
| get_includes        | #include Liste mit Zeilennummern                      |
| get_class_hierarchy | Vererbungshierarchie (class Foo : public Bar)         |
| get_call_graph      | Welche Funktionen ruft foo() auf? (eine Ebene)        |
| check_syntax        | Syntaxfehler mit Zeile + Spalte                       |

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
ctxParams.type_k = GGML_TYPE_Q8_0;
ctxParams.type_v = GGML_TYPE_Q8_0;
ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
```

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
   aus GGUF-Metadaten (`tokenizer.chat_template`).
2. **Manuell**: Preset-Auswahl im ConfigDialog (ChatML / Llama3 / Gemma / Mistral)

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

---

## EditorDock

Andockbares Code-Editor-Fenster (`QDockWidget`):
- QPlainTextEdit mit C++ Syntax-Highlighting (CppHighlighter)
- QFileSystemWatcher: externe Änderungen (durch Modell) werden markiert
- Dirty-Flag: `*` im Titel, Extern-Flag: `!`
- Ctrl+S → `fileSavedByUser()` Signal → Systemnachricht in Chat

---

## Deadlock-Erkennung

| Fehlerzahl | Eskalation                         |
|------------|------------------------------------|
| 3          | Warnung — Argumente prüfen         |
| 5          | Umleitung — anderen Weg suchen     |
| 7          | Abbruch — Erklärung an User        |

---

## LlamaWorker — generate()-Varianten

```cpp
void generate(messages, profile);       // Cache leeren (stabil)
void generateDelta(messages, profile);  // Cache behalten (vorbereitet, nicht aktiv)
void doGenerate(messages, profile, clearCache);  // gemeinsamer Kern
```

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
- [x] CommandProcessor (/init /build /compile /run /summarize /undo /diff)
- [x] Kontext-Management (80%-Auto + /summarize)
- [x] Git-Integration im filesystem MCP
- [x] grep_code + search_code + tree + find_files Tools
- [x] read_multiple_files, patch_file, move_file, copy_file
- [x] Compile-Retry-Loop (3x, Eskalation)
- [x] git_diff Anzeige nach Datei-Änderungen (farbig)
- [x] Token-Budget (Tool-Ergebnisse kürzen)
- [x] ChatLogger (Markdown, pro Session)
- [x] AppConfig / QSettings (Singleton, INI-Format)
- [x] gpu_info + set_power_limit Tools
- [x] ConfigDialog (Tabs: Modell/Sampler/Agent/Logging/Template)
- [x] Chat-Template: Auto aus GGUF + manuelle Auswahl
- [x] EditorDock: Code-Editor mit Syntax-Highlighting + QFileSystemWatcher
- [x] KV-Cache Quantisierung (Q8_0)
- [x] randomSeed() via /dev/urandom
- [x] doGenerate() + generateDelta() — Vorbereitung Delta-Encoding
- [x] **tree-sitter MCP-Server v2.0** (list_symbols, get_function_body,
      get_class_members, get_includes, get_class_hierarchy, get_call_graph,
      check_syntax) — Qt-Macro Neutralisierung, zwei Roots, C++ Grammar
- [x] **mcp-servers/common**: McpServer.h + PathPolicy.h (header-only)
- [x] tree-sitter vendor als git submodules

### Offen — nächste Schritte (Priorität)

**MCP-Infrastruktur (nächste Session):**
- [ ] **filesystem-Server auf McpServer+PathPolicy umbauen**
      - Absolute Pfade für Lesen erlauben (in bekannter Root)
      - Absolute Pfade für Schreiben: nur wenn in writable Root
      - `list_symbols` Tool entfernen (tree-sitter macht das besser)
      - Trash-Pfad via Umgebungsvariable konfigurierbar
      - Tool-Klassen statt Lambdas (ToolBase Basisklasse, Dependency Injection)
- [ ] **Tool-Klassen Architektur** (für alle Server):
      ```
      class ToolBase {
          virtual ToolResult execute(const QJsonObject &args) = 0;
          virtual QJsonObject schema() const = 0;
      };
      class ReadFileTool : public ToolBase { ... };
      ```
      Vorteil: eine .cpp pro Tool, testbar, klar strukturiert
- [ ] **LRU-Cache im tree-sitter Server** (parsed denselben Header oft)
      Schlüssel: Dateipfad + mtime, Wert: ParseResult
      Größe: ~20 Einträge reichen
- [ ] **MCP-Server Konfigurationsdatei**
      `~/.config/LlamaQt/mcp-servers.conf` (INI-Format, QSettings)
      Sandbox-Pfad, Sources-Pfad, Limits — unabhängig vom Modell konfigurierbar
      Kein stdio-Schreiben für Config (würde JSON-RPC stören)
- [ ] sysinfo + compile + websearch auf McpServer+PathPolicy umbauen

**LlamaQt Core:**
- [ ] generateDelta() aktivieren: n_past-Tracking in LlamaWorker
- [ ] KV-Cache Rollback: llama_kv_cache_seq_rm nach Tool-Fehler
- [ ] maxNewTokens in AppConfig (Chat: 8192, Tool: 2048)
- [ ] Planner/Executor-Trennung (Meilensteine, Teilaufgaben)
- [ ] Live-Output bei /run (stdout streaming)
- [ ] MCP-Server Neustart bei Absturz
- [ ] Persistentes Konversationsgedächtnis zwischen Sessions
- [ ] XTC-Sampler Option (konfigurierbar, nur Chat-Profil)
- [ ] DRY-Sampler Option (gegen logische Endlosschleifen)
- [ ] Mehrere Modelle (klein für Tool-Calls, groß für Planung)

---

## Übergabe-Prompt für neue Session

```
Lies zuerst AGENT.md komplett ein. Dann:

Aktuelle Aufgabe: filesystem MCP-Server umbauen.

Kontext:
- mcp-servers/common/McpServer.h und PathPolicy.h existieren bereits
- tree-sitter Server (mcp-servers/tree-sitter/main.cpp) ist bereits
  auf diese Infrastruktur umgebaut — als Referenz verwenden
- filesystem Server (mcp-servers/filesystem/main.cpp) ist noch alt (v2.3)

Ziele für diese Session:
1. filesystem-Server auf McpServer+PathPolicy umbauen
2. Absolute Pfade für Lesen erlauben (wenn in bekannter Root)
3. Schreiben nur in writable Roots (Sandbox)
4. list_symbols Tool entfernen (tree-sitter macht das besser)
5. Trash-Pfad via LLAMAQT_TRASH Umgebungsvariable konfigurierbar
6. Tool-Basisklasse ToolBase einführen, ReadFileTool etc. ableiten
7. README.md aktualisieren

Bitte lies zuerst die aktuellen Dateien:
list_symbols("mcp-servers/filesystem/main.cpp")
list_symbols("mcp-servers/common/McpServer.h")
list_symbols("mcp-servers/common/PathPolicy.h")
Dann stelle Rückfragen bevor du implementierst.
```

---

## Build

```bash
cd ~/ai/LlamaQT
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Laufzeit

```bash
export TAVILY_API_KEY="tvly-..."        # websearch
export LLAMAQT_SANDBOX="$HOME/llamatools"   # optional, das ist der Default
export LLAMAQT_SOURCES="$HOME/ai/LlamaQT"  # optional, das ist der Default
./LlamaQt
```
