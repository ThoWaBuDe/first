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
    │     ├── McpClient treesitter  (llamaqt-treesitter)
    │     └── McpClient clang       (llamaqt-clang)
    ├── CommandProcessor — Slash-Kommandos (/plan NEU)
    ├── ChatLogger       — Markdown-Logging in Datei
    ├── AppConfig        — Singleton, QSettings, INI-Persistenz
    ├── TaskTree         — RAM-first Aufgabenbaum, SQLite-Persistenz  ← NEU
    ├── ExecuteMemory    — Kurzzeitgedächtnis (Thoughts), SQLite       ← NEU (geplant)
    └── LlamaWorker      (Active Object, Worker-Thread)
          └── llama.cpp
```

### AgentMode (FSM)
```
Chat  ←→  Plan  →  Execute
            ↑          ↓
            └──── Chat (Fehler/Stop)

Chat    — normaler Gesprächs-Modus
Plan    — Modell analysiert Projekt (Lese-Tools), gibt <plan>...</plan> aus
Execute — Modell implementiert Node für Node (Thoughts-Kontext, roher Code-Output)
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
├── src/
│   ├── main.cpp
│   ├── MainWindow.h/.cpp/.ui
│   ├── Agent.h/.cpp
│   ├── CommandProcessor.h/.cpp    ← /plan Kommando
│   ├── ChatModel.h/.cpp
│   ├── ChatTemplate.h
│   ├── LlamaWorker.h/.cpp
│   ├── McpClient.h/.cpp
│   ├── McpManager.h/.cpp
│   ├── AppConfig.h/.cpp
│   ├── ChatLogger.h/.cpp
│   ├── ConfigDialog.h/.cpp
│   ├── EditorDock.h/.cpp
│   ├── PlannerDock.h/.cpp         ← TaskTree-View + Edit-Dialog + Load
│   ├── TaskNode.h                 ← Datenstruktur (header-only)
│   ├── TaskTree.h                 ← Container + SQLite-Persistenz (header-only)
│   ├── TaskTreeModel.h            ← QAbstractItemModel für QTreeView (header-only)
│   ├── TaskNodeDialog.h/.cpp      ← Edit-Dialog (Doppelklick im PlannerDock)
│   └── ExecuteMemory.h/.cpp       ← Thoughts/Kurzzeitgedächtnis (geplant)
└── mcp-servers/
    ├── common/
    │   ├── McpServer.h
    │   └── PathPolicy.h
    ├── filesystem/
    ├── sysinfo/
    ├── compile/
    ├── websearch/
    ├── tree-sitter/
    └── clang/
```

---

## TaskTree — Aufgabenbaum

### Hierarchie
```
H0 (level=0) — Gesamtziel (Wurzel, genau 1x)
H1 (level=1) — Dateigruppe / Modul
H2 (level=2) — Einzelne Datei (.h oder .cpp)
H3 (level=3) — Implementierungsschritt (Methode, Algorithmus)
H4+          — Feinere Details (selten)
```

### Designprinzipien
- **.h Dateien** → immer H2-Blatt (Interface komplett in einem Stück)
- **.cpp Dateien** → H2 mit H3-Kindern (jede Methode ein eigener Node)
- **Assembly**: H2->result + sortierte H3->result (nach `order`, dann `insertionIdx`)
- **Ein Node = eine Datei = ein Code-Block** (kein path-Attribut nötig, title = Dateiname)

### TaskNode Felder
```cpp
qint64     id           — eindeutige ID (SQLite Primary Key)
int        level        — Hierarchie-Ebene (0=Goal, 1=Files, 2=Class, 3=Impl)
int        order        — Sortierung unter Geschwistern
int        insertionIdx — Tiebreaker bei gleichem order
TaskScope  scope        — Internal (private/.cpp) | External (public/.h)
QString    title        — Kurztitel (= Dateiname bei H2)
QString    description  — Aufgabe + Impl-Hints + Signaturen
QString    result       — Generierter Code (vom Execute-Agent befüllt)
TaskStatus status       — Pending / Running / Done / Failed / Blocked
QList<qint64> dependsOn — IDs anderer Nodes (horizontale Abhängigkeiten)
```

### SQLite DB
- Pfad: `~/llamatools/tasks.sqlite`
- Tabelle: `tasks` (alle TaskNode-Felder)
- Tabelle: `execute_memory` (Thoughts — geplant)
- `save()` — dirty-Flag basiert (nur geänderte Nodes)
- `load()` — vollständiger Reload aus DB
- `clear()` — RAM-Tree leeren (DB bleibt)

### Lichtkegel-Prinzip ("Need to know")
```
Prompt für H3-Node enthält NUR:
  Vertikal:    H0 → H1 → H2 → H3 (aktuell)
  Horizontal:  dependsOn-Nodes (Interfaces die dieser Node braucht)
  Thoughts:    Kurzzeitgedächtnis (max N Einträge, konfigurierbar)
```
Nicht mehr, nicht weniger — das 9B-Modell wird nicht überlastet.

---

## Plan-Modus

### Ablauf
```
/plan <Auftrag>
  → CommandProcessor → Marker "__PLAN__:<text>"
  → Agent::startPlan()
      → m_taskTree.clear()               — frischer RAM-Tree
      → buildPlannerSystemPrompt()       — Lese-Tools + H3-Regel
      → Modell liest Projekt (Whitelist: read_file, list_dir, ...)
      → Modell gibt <plan>...</plan> JSON aus
      → handlePlanJson()
          → parsePlanNode() [Erster Pass]  — Knoten aufbauen + titleToId
          → dependsOn-Auflösung [Zweiter Pass] — Titel → IDs
          → emit planReady()
  → PlannerDock: Tree anzeigen + Bestätigen/Ablehnen
  → onPlanApproved(): save() → AgentMode::Execute (TODO)
```

### Plan JSON Format
```json
{
  "goal": "Gesamtziel",
  "children": [
    {
      "title": "H1-Gruppe",
      "level": 1, "scope": "external",
      "description": "...", "dependsOn": [], "children": [
        {
          "title": "GameState.h",
          "level": 2, "scope": "external",
          "description": "class GameState: ...", "dependsOn": [], "children": []
        },
        {
          "title": "GameState.cpp",
          "level": 2, "scope": "internal",
          "description": "Implementierung", "dependsOn": ["GameState.h"],
          "children": [
            {
              "title": "makeMove() implementieren",
              "level": 3, "scope": "internal",
              "description": "Prüft Gültigkeit, setzt Feld, wechselt Spieler",
              "dependsOn": ["GameState.h"], "children": []
            }
          ]
        }
      ]
    }
  ]
}
```

### Plan-Modus Whitelist
```
read_file, list_dir, get_symbol, get_project_index,
rebuild_index, get_time, sys_info, disk_free, get_pwd
```

---

## Execute-Modus (in Implementierung)

### Konzept
```
startExecute()
  → nextPending()              — nächster Pending-Blatt-Node
  → buildExecutePrompt(node)  — Lichtkegel aufbauen:
        verticalContext()      — H0→H1→H2→H3 Pfad
        horizontalContext()    — dependsOn-Nodes (Interfaces)
        Thoughts               — Kurzzeitgedächtnis
  → startGeneration(Chat)     — Modell generiert rohen C++ Code

onGenerationDone() [Execute-Zweig]
  → roher Code → node->result speichern
  → node->status = Done
  → updateThoughts()          — Thoughts nach jedem Node summarizen
  → nextPending() oder fertig
```

### Code-Output Format
Kein JSON, kein path-Attribut — roher C++ Code direkt:
```cpp
#include "GameState.h"

bool GameState::makeMove(int row, int col) {
    ...
}
```
Der Titel des H2-Nodes gibt die Zieldatei an.
Der Code landet in `node->result` (SQLite) — Assembly danach separat.

### Assembly
```
H2: GameState.cpp->result  = "#include ..."          (Datei-Kopf)
  H3: makeMove()->result   = "bool GameState::..."   (order=0)
  H3: checkWin()->result   = "bool GameState::..."   (order=1)

Assemblierte Datei = concat(H2->result, H3-Kinder sortiert nach order/insertionIdx)
```

### Execute-Modus Whitelist (Lese-Tools + websearch)
```
read_file, list_dir, get_symbol, get_project_index,
get_function_body, get_class_members, get_includes,
check_syntax, web_search, get_time, sys_info, get_pwd
```

### Thoughts (Kurzzeitgedächtnis)
- Format: QStringList ("Sprüche" / kurze Erkenntnisse)
- Obergrenze: konfigurierbar in AppConfig (`executeMemoryMaxEntries`)
- Nach jedem Node: LLM summarized → komprimiert Liste → Obergrenze einhalten
- Persistenz: SQLite Tabelle `execute_memory`
- RAM beim Start laden, beim Exit speichern
- Beispiele:
  - "QTimer muss im GUI-Thread leben"
  - "GameState hat keine Qt-Abhängigkeit"
  - "connect() über Thread-Grenzen: Qt::QueuedConnection"

---

## PlannerDock

```
PlannerDock (QDockWidget, links)
  ├── QTreeView + TaskTreeModel   — zeigt TaskTree
  ├── QTextEdit (Kontext-Anzeige) — buildContext() des selektierten Nodes
  ├── Doppelklick / Enter         → TaskNodeDialog (Edit)
  ├── Rechtsklick Kontextmenü:
  │     ✏ Bearbeiten              → TaskNodeDialog
  │     ➕ Kind hinzufügen        → createNode(level+1) + Dialog
  │     ➕ Geschwister hinzufügen → createNode(gleicher level) + Dialog
  │     🗑 Löschen               → Sicherheitsabfrage + removeNode()
  ├── Expand-Fix: saveExpandState()/restoreExpandState() — IDs statt Indizes
  ├── Buttons: ✓ Bestätigen | ✗ Ablehnen (nur im Plan-Modus sichtbar)
  └── Button: 📂 Plan laden       → TaskTree::load() aus SQLite
```

### TaskNodeDialog
- Felder: title, description, result, level (H0..H6), scope, status
- dependsOn: Transfer-Widget (verfügbar links ↔ abhängig rechts, Suchfeld)
- Doppelklick öffnet → bei OK: applyToNode() → refresh mit Expand-Erhalt

---

## Agenten-Loop (vollständig)

```
onUserMessage()
    → CommandProcessor::process()
        /plan → startPlan()        [Plan-Modus]
        andere → normaler Chat
    → checkContextUsage() (>80% → auto-summarize)
    → startGeneration(Chat)
        → onTokenReceived() [filterToken: think vs. sichtbar]
        → onGenerationDone()

            [Chat-Modus]
            ├── offener <tool_call>  → Continuation (max 3x)
            ├── <tool_call>...</tool_call> → handleToolCall()
            │     → JSON validieren/reparieren
            │     → Deadlock-Check (3/5/7)
            │     → m_mcp.callTool()
            │     → git_diff anzeigen (bei Datei-Änderungen)
            │     → startGeneration(Tool)
            └── normale Antwort → fertig

            [Plan-Modus]
            ├── <plan>...</plan>    → handlePlanJson()
            ├── <tool_call>        → handlePlanToolCall() (Whitelist!)
            └── Prosa              → Continuation

            [Execute-Modus] ← in Implementierung
            ├── roher Code         → handleExecuteCode()
            ├── <tool_call>        → handleExecuteToolCall() (Whitelist!)
            └── Prosa              → Continuation
```

---

## Slash-Kommandos

| Kommando              | Aktion                                                    |
|-----------------------|-----------------------------------------------------------|
| `/init [Name]`        | Verzeichnis + Git-Repo + AGENT.md in Sandbox              |
| `/build`              | cmake + make                                              |
| `/compile`            | nur make                                                  |
| `/run`                | cmake + make + Binary starten                             |
| `/plan <Auftrag>`     | Plan-Modus starten                                        |
| `/summarize`          | Konversation manuell zusammenfassen                       |
| `/undo [Datei]`       | letzten git-commit rückgängig                             |
| `/diff`               | git diff anzeigen                                         |

---

## Sampler-Profile

| Profil | Top-K | Temp | Top-P | Min-P | Verwendung           |
|--------|-------|------|-------|-------|----------------------|
| Chat   | 40    | 0.7  | 0.95  | 0.05  | Konversation + Plan  |
| Tool   | 20    | 0.1  | 0.50  | 0.05  | Tool-Calls           |

Seed: `/dev/urandom` vor jeder Generation (`refreshDistSampler()`).

---

## AppConfig Parameter

| Gruppe        | Parameter                                              |
|---------------|--------------------------------------------------------|
| Model         | path, context_size, batch_size, chat_template          |
| SamplerChat   | top_k, temp, top_p, min_p                              |
| SamplerTool   | top_k, temp, top_p, min_p                              |
| Agent         | summarize_threshold, max_continuations, deadlock_*     |
|               | max_tool_result_chars, sandbox_path                    |
| WebSearch     | tavily_api_key                                         |
| Logging       | chat_enabled, chat_log_dir                             |
| TaskTree      | db_path (default: ~/llamatools/tasks.sqlite)           |
| Execute       | memory_max_entries (Thoughts-Obergrenze) ← geplant     |
|               | auto_mode (Modell entscheidet Granularität) ← geplant  |

---

## MCP-Server & Tools (65 Tools)

### Execute-Whitelist (Lesen + websearch)
```
read_file, list_dir, get_symbol, get_project_index,
get_function_body, get_class_members, get_includes,
check_syntax, web_search, get_time, sys_info, get_pwd
```

### Plan-Whitelist (nur Lesen)
```
read_file, list_dir, get_symbol, get_project_index,
rebuild_index, get_time, sys_info, disk_free, get_pwd
```

### filesystem (llamaqt-filesystem v2.3)
read_file, write_file, append_file, str_replace, patch_file,
list_dir, mkdir, tree, grep_code, search_code, find_files,
read_multiple_files, move_file, copy_file,
git_status, git_diff, git_log, git_checkout

### treesitter (llamaqt-treesitter v2.0)
list_symbols, get_function_body, get_class_members, get_includes,
get_class_hierarchy, get_call_graph, check_syntax,
get_symbol, replace_symbol, get_project_index, rebuild_index

### clang (llamaqt-clang)
go_to_definition, find_references, go_to_declaration,
type_hierarchy, find_callers, find_callees, class_hierarchy,
variable_refs, unused_includes, missing_overrides, type_errors,
dead_code, generate_stub, generate_getters, generate_constructor,
generate_docs, rename_symbol, extract_function, move_method,
add_include

### compile (llamaqt-compile v2.0)
cmake_build, pkg_status, check_run

### sysinfo (llamaqt-sysinfo v2.0)
get_time, get_pwd, disk_free, sys_info, gpu_info,
set_power_limit, process_list, networking, system_status,
disk_io, cuda_info, battery

### websearch (llamaqt-websearch v1.1)
web_search

---

## Deadlock-Erkennung

| Fehlerzahl | Eskalation                     |
|------------|--------------------------------|
| 3          | Warnung — Argumente prüfen     |
| 5          | Umleitung — anderen Weg suchen |
| 7          | Abbruch — Erklärung an User    |

---

## Chat-Template

Auto (aus GGUF) oder manuell (ChatML/Llama3/Gemma/Mistral).
Fallback: `ChatModel::buildPrompt()` mit ChatTemplate-Struct.

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
export TAVILY_API_KEY="tvly-..."
export LLAMAQT_SANDBOX="$HOME/llamatools"
export LLAMAQT_SOURCES="$HOME/ai/LlamaQT"
./LlamaQt
```

---

## Offene TODOs

### Implementiert ✓
- [x] MVP-Architektur (MainWindow/Agent/ChatModel)
- [x] MCP stdio JSON-RPC (McpClient/McpManager)
- [x] Zwei Sampler-Profile (Chat/Tool) + /dev/urandom Seed
- [x] Thinking-Filter (<think>...</think>)
- [x] Tool-Deadlock-Erkennung (3/5/7 Eskalation)
- [x] JSON-Repair
- [x] Stop-Bug-Fix (Session-ID Pattern)
- [x] Smart-Autoscroll
- [x] CommandProcessor (/plan NEU)
- [x] Kontext-Management (80%-Auto + /summarize)
- [x] Git-Integration im filesystem MCP
- [x] ChatLogger, AppConfig, ConfigDialog
- [x] EditorDock (Syntax-Highlighting + QFileSystemWatcher)
- [x] KV-Cache Quantisierung (Q8_0) + Flash Attention
- [x] doGenerate() + generateDelta() (Delta vorbereitet)
- [x] tree-sitter MCP-Server v2.0
- [x] clang MCP-Server
- [x] mcp-servers/common: McpServer.h + PathPolicy.h
- [x] **Plan-Modus**: /plan, TaskTree, PlannerDock, dependsOn-Auflösung
- [x] **TaskNodeDialog**: Edit-Dialog mit Transfer-Widget für dependsOn
- [x] **PlannerDock**: Kontextmenü (Bearbeiten/Kind/Geschwister/Löschen)
- [x] **Expand-Fix**: saveExpandState()/restoreExpandState()
- [x] **TaskTree**: clear(), removeNode(), dirty-Flag Fix, load()
- [x] **Plan laden**: 📂 Button im PlannerDock

### In Implementierung
- [ ] **Execute-Modus**: startExecute(), buildExecutePrompt(), handleExecuteCode()
- [ ] **ExecuteMemory**: Thoughts (Kurzzeitgedächtnis), SQLite-Persistenz
- [ ] **Assembly**: Nodes → Dateien zusammenbauen
- [ ] **AppConfig**: executeMemoryMaxEntries, executeAutoMode

### Offen (nächste Sessions)
- [ ] generateDelta() aktivieren (n_past-Tracking)
- [ ] KV-Cache Rollback (llama_kv_cache_seq_rm)
- [ ] maxNewTokens in AppConfig
- [ ] filesystem-Server auf McpServer+PathPolicy umbauen
- [ ] MCP-Server Auto-Restart bei Absturz
- [ ] XTC-Sampler + DRY-Sampler Option
- [ ] Mehrere Modelle (klein für Tools, groß für Planung)
- [ ] vendor/-Ausschluss in ProjectIndexer
- [ ] TreeSitterToolBase protected → public

---

## Übergabe-Prompt für neue Session

```
Lies AGENT.md komplett ein. Dann:

Aktuelle Aufgabe: Execute-Modus implementieren.

Was bereits läuft:
- Plan-Modus funktioniert (getestet mit TicTacToe + Countdown-Timer)
- TaskTree mit H0-H3 Nodes, SQLite-Persistenz, dependsOn-Auflösung
- PlannerDock mit Edit-Dialog, Kontextmenü, Expand-Fix, Plan laden

Nächste Schritte:
1. ExecuteMemory.h/.cpp (Thoughts/Kurzzeitgedächtnis)
2. AppConfig: executeMemoryMaxEntries, executeAutoMode
3. Agent: startExecute(), buildExecutePrompt(), handleExecuteCode(),
          updateThoughts(), Execute-Zweig in onGenerationDone()
4. Agent::onPlanApproved() → AgentMode::Execute statt Chat

Designentscheidungen Execute:
- Lichtkegel: vertikal (H0→Hn) + horizontal (dependsOn) + Thoughts
- Code-Output: roher C++ Code direkt (kein JSON, kein path-Tag)
  title des H2-Nodes = Dateiname
  Code landet in node->result
- .h Nodes: immer H2-Blatt (komplett in einem Stück)
- .cpp Nodes: H2 + H3-Kinder (Methoden einzeln)
- Assembly: H2->result + H3-Kinder sortiert nach order/insertionIdx
- Whitelist: read_file, list_dir, get_symbol, get_project_index,
             get_function_body, get_class_members, get_includes,
             check_syntax, web_search, get_time, sys_info, get_pwd
- Thoughts: nach jedem Node summarizen, Obergrenze aus AppConfig

Bitte Rückfragen stellen bevor implementiert wird.
```
