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
    ├── CommandProcessor — Slash-Kommandos
    ├── ChatLogger       — Markdown-Logging in Datei
    ├── AppConfig        — Singleton, QSettings, INI-Persistenz
    ├── TaskTree         — RAM-first Aufgabenbaum, SQLite-Persistenz
    ├── ExecuteMemory    — Thoughts/Kurzzeitgedächtnis, SQLite-Persistenz
    └── LlamaWorker      (Active Object, Worker-Thread)
          └── llama.cpp
```

### AgentMode (FSM)
```
Chat  ←→  Plan  →  Execute
            ↑          ↓
            └──── Chat (Fehler/Stop/Fertig)

Chat    — normaler Gesprächs-Modus
Plan    — Modell analysiert Projekt (Lese-Tools), gibt <plan>...</plan> aus
Execute — Modell implementiert Node für Node (Lichtkegel + Thoughts)
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
│   ├── CommandProcessor.h/.cpp
│   ├── ChatModel.h/.cpp
│   ├── ChatTemplate.h
│   ├── LlamaWorker.h/.cpp
│   ├── McpClient.h/.cpp
│   ├── McpManager.h/.cpp
│   ├── AppConfig.h/.cpp
│   ├── ChatLogger.h/.cpp
│   ├── ConfigDialog.h/.cpp       ← Execute-Tab neu
│   ├── EditorDock.h/.cpp
│   ├── PlannerDock.h/.cpp
│   ├── TaskNode.h                ← FileType-Erkennung neu
│   ├── TaskTree.h                ← side_output + build_prompt Schema
│   ├── TaskTreeModel.h
│   ├── TaskNodeDialog.h/.cpp
│   └── ExecuteMemory.h           ← parseFromLlmOutput() robust
└── mcp-servers/
    ├── common/McpServer.h + PathPolicy.h
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
H2 (level=2) — Einzelne Datei (.h oder .cpp oder CMakeLists.txt etc.)
H3 (level=3) — Implementierungsschritt (Methode, Algorithmus)
H4+          — Feinere Details (selten)
```

### Designprinzipien
- **.h Dateien** → immer H2-Blatt (Interface komplett in einem Stück)
- **.cpp Dateien** → H2 mit H3-Kindern (jede Methode ein eigener Node)
- **CMakeLists.txt** → H2-Blatt (CMake-Code, nicht C++)
- **Assembly**: H2->result + sortierte H3->result (nach `order`, dann `insertionIdx`)
- **Ein Node = eine Datei** (title = Dateiname, bestimmt auch den Dateityp)

### TaskNode Felder (v3)
```cpp
qint64     id           — eindeutige ID
int        level        — 0=Goal, 1=Files, 2=Class, 3=Impl
int        order        — Sortierung unter Geschwistern
int        insertionIdx — Tiebreaker bei gleichem order
TaskScope  scope        — Internal | External
QString    title        — Kurztitel (= Dateiname bei H2)
QString    description  — Aufgabe + Impl-Hints + Signaturen

// Execute-Output (getrennt seit v3):
QString    result       — NUR sauberer Code (aus <code>...</code>)
QString    sideOutput   — Thinking, Warnungen, Erklärungen des Modells
QString    buildPrompt  — vollständiger Prompt (Lichtkegel-Debugging)

TaskStatus status       — Pending/Running/Done/Failed/Blocked
QList<qint64> dependsOn — IDs abhängiger Nodes (horizontal)
```

### Dateityp-Erkennung (TaskNode::fileType())
```cpp
"GameState.cpp"   → FileType::Cpp      → "C++"
"GameState.h"     → FileType::CppHeader → "C++"
"CMakeLists.txt"  → FileType::CMake    → "CMake"
"script.py"       → FileType::Python   → "Python"
```
Wird von `buildExecuteSystemPrompt()` genutzt damit das Modell die
richtige Sprache für `<code>...</code>` verwendet.

### SQLite DB
- Pfad: `~/llamatools/tasks.sqlite`
- Tabelle `tasks`: alle TaskNode-Felder inkl. `side_output`, `build_prompt`
- Tabelle `execute_memory`: Thoughts (position, entry)
- **Migration**: `addColumnIfMissing()` fügt neue Spalten zu bestehenden DBs hinzu
- `save()` — dirty-Flag basiert
- `load()` — vollständiger Reload
- `clear()` — RAM-Tree leeren (DB bleibt)

### Lichtkegel-Prinzip ("Need to know")
```
Prompt für H3-Node enthält NUR:
  Vertikal:    H0 → H1 → H2 → H3 (aktuell)
  Horizontal:  dependsOn-Nodes (Interfaces + result falls schon vorhanden)
  Thoughts:    Kurzzeitgedächtnis (max N Einträge, konfigurierbar)
```
Das 9B-Modell bekommt genau was es braucht — nicht mehr, nicht weniger.

---

## Plan-Modus

### Ablauf
```
/plan <Auftrag>
  → startPlan() → m_taskTree.clear()
  → buildPlannerSystemPrompt() (H3-Regel!)
  → Modell: Lese-Tools → <plan>...</plan> JSON
  → handlePlanJson():
      parsePlanNode() [Erster Pass]  — Nodes + titleToId aufbauen
      dependsOn-Auflösung [Zweiter Pass] — Titel → IDs
      emit planReady()
  → PlannerDock: Tree + Bestätigen/Ablehnen
  → onPlanApproved() → save() → startExecute()
```

### H3-Regel im Planner-System-Prompt
```
.h Dateien  → H2-Blatt (Interface komplett in einem Stück)
.cpp Dateien → H2 + H3-Kinder (Methoden einzeln)
CMakeLists.txt → H2-Blatt (CMake, kein C++)
```

### Plan-Whitelist (nur Lese-Tools)
```
read_file, list_dir, get_symbol, get_project_index,
rebuild_index, get_time, sys_info, disk_free, get_pwd
```

---

## Execute-Modus

### Konzept
```
startExecute()
  → Thoughts aus DB laden
  → advanceExecute():
      nextPending()              — nächster Pending-Blatt-Node
      buildExecutePrompt(node)  — Lichtkegel:
            verticalContext()    — H0→H1→H2→H3 Pfad
            horizontalContext()  — dependsOn mit result (Interface!)
            Thoughts             — Kurzzeitgedächtnis
      buildPrompt in node speichern (Debugging)
      startGeneration(Chat)

onGenerationDone() [Execute-Zweig]:
  m_updatingThoughts == true:
    parseFromLlmOutput() → m_executeMemory.setEntries()
    → advanceExecute()

  m_mode == Execute:
    <tool_call>         → handleExecuteToolCall() (Whitelist)
    <code>...</code>    → handleExecuteCode()
    sonst               → Continuation
```

### Code-Output Format
```
<code>
...reiner Code (C++, CMake, Python, je nach Dateityp)...
</code>
```
- Kein Text vor `<code>`, kein Text nach `</code>`
- Alles außerhalb → `node->sideOutput`
- Code innerhalb → `node->result`
- Fallback: kein Tag → ganzer Response als Code, Warning ins sideOutput

### Assembly
```
H2: GameState.cpp->result  = "#include ..."        (Datei-Kopf)
  H3: makeMove()->result   = "bool GameState::..."  (order=0)
  H3: checkWin()->result   = "bool GameState::..."  (order=1)

Assemblierte Datei = concat(H2->result, H3-Kinder sortiert nach order/insertionIdx)
Ziel: ~/llamatools/[executeSandboxProject]/GameState.cpp
```
Assembly noch nicht implementiert — kommt in nächster Session.

### Execute-Whitelist (Lese-Tools + websearch)
```
read_file, list_dir, get_symbol, get_project_index,
get_function_body, get_class_members, get_includes,
check_syntax, read_multiple_files, grep_code, search_code,
web_search, get_time, sys_info, get_pwd, disk_free
```

### Thoughts (Kurzzeitgedächtnis)
- Format: QStringList — kurze Erkenntnisse ("Sprüche")
- Obergrenze: `AppConfig::executeMemoryMaxEntries()` (default 50)
- Nach jedem Node: LLM summarized → `parseFromLlmOutput()` bereinigt
- `parseFromLlmOutput()` entfernt: `<think>`-Blöcke, Nummerierung,
  Markdown-Bullets, zu kurze Zeilen (<4 Zeichen), englische Denkprozess-Artefakte
- Persistenz: SQLite Tabelle `execute_memory`
- Laden/Speichern: beim Execute-Start / nach jedem Node / `/saveDB` / `/loadDB`

---

## PlannerDock

```
PlannerDock (QDockWidget, links)
  ├── QTreeView + TaskTreeModel
  ├── QTextEdit (Kontext-Anzeige: buildContext())
  ├── Doppelklick / Enter / Rechtsklick → TaskNodeDialog
  ├── Kontextmenü:
  │     ✏ Bearbeiten | ➕ Kind | ➕ Geschwister | 🗑 Löschen
  ├── Expand-Fix: saveExpandState()/restoreExpandState()
  ├── Approval-Buttons: ✓ Bestätigen | ✗ Ablehnen
  └── 📂 Plan laden → TaskTree::load()
```

### TaskNodeDialog
- Felder: title, description, result, sideOutput (read-only), level, scope, status
- dependsOn: Transfer-Widget (verfügbar ↔ abhängig, Suchfeld)

---

## Slash-Kommandos

| Kommando              | Marker        | Aktion                                          |
|-----------------------|---------------|-------------------------------------------------|
| `/plan <Auftrag>`     | `__PLAN__:`   | Plan-Modus starten                              |
| `/execute`            | `__EXECUTE__` | Execute-Modus starten (nächster Pending-Node)   |
| `/saveDB`             | `__SAVEDB__`  | TaskTree + Thoughts in SQLite speichern         |
| `/loadDB`             | `__LOADDB__`  | TaskTree + Thoughts aus SQLite laden            |
| `/init [Name]`        | Prompt        | Verzeichnis + Git-Repo in Sandbox               |
| `/build`              | Prompt        | cmake + make                                    |
| `/compile`            | Prompt        | nur make                                        |
| `/run`                | Prompt        | cmake + make + Binary starten                   |
| `/summarize`          | `__SUMMARIZE__`| Konversation zusammenfassen                    |
| `/undo [Datei]`       | Prompt        | letzten git-commit rückgängig                   |
| `/diff`               | Prompt        | git diff anzeigen                               |

---

## Agenten-Loop

```
onUserMessage()
    → CommandProcessor::process()
    → Marker auswerten:
        __PLAN__:    → startPlan()
        __EXECUTE__  → startExecute()
        __SAVEDB__   → taskTree.save() + executeMemory.save()
        __LOADDB__   → taskTree.load() + executeMemory.load() + taskTreeUpdated()
        __SUMMARIZE__→ summarizeContext()
    → checkContextUsage() (>80% → auto-summarize)
    → startGeneration(Chat)
        → onGenerationDone()
            [Thoughts-Update]  m_updatingThoughts → parseFromLlmOutput() → advanceExecute()
            [Execute-Modus]    <tool_call> | <code>...</code> | Continuation
            [Plan-Modus]       <plan> | <tool_call> | Prosa
            [Chat-Modus]       <tool_call> | normale Antwort
```

---

## Sampler-Profile

| Profil | Top-K | Temp | Top-P | Min-P | Verwendung           |
|--------|-------|------|-------|-------|----------------------|
| Chat   | 40    | 0.7  | 0.95  | 0.05  | Konversation, Plan, Execute |
| Tool   | 20    | 0.1  | 0.50  | 0.05  | Tool-Calls           |

Seed: `/dev/urandom` vor jeder Generation.

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
| Execute       | memory_max_entries (default: 50)                       |
|               | auto_mode (default: true)                              |
|               | sandbox_project (Unterverzeichnis für Assembly)        |

---

## ConfigDialog Tabs

| Tab               | Inhalt                                                  |
|-------------------|---------------------------------------------------------|
| 🧠 Modell         | Modell-Pfad, n_ctx, Batch-Size (Neustart)               |
| 🎲 Sampler        | Chat + Tool Profile, Restore Defaults                   |
| 🤖 Agent          | Summarize-Schwelle, Continuations, Deadlock-Eskalation  |
| 📝 Logging        | Chat-Log, Tavily API Key                                |
| 💬 Template       | Chat-Template Preset, Custom, User System-Prompt        |
| ⚙ Execute        | Thoughts-Obergrenze, Auto-Modus, Sandbox-Projekt, DB-Info |

---

## MCP-Server & Tools (65 Tools)

### Execute-Whitelist
```
read_file, list_dir, get_symbol, get_project_index,
get_function_body, get_class_members, get_includes,
check_syntax, read_multiple_files, grep_code, search_code,
web_search, get_time, sys_info, get_pwd, disk_free
```

### Plan-Whitelist
```
read_file, list_dir, get_symbol, get_project_index,
rebuild_index, get_time, sys_info, disk_free, get_pwd
```

### Server-Übersicht
- **filesystem** (v2.3): read_file, write_file, str_replace, patch_file, list_dir,
  mkdir, tree, grep_code, search_code, find_files, read_multiple_files,
  move_file, copy_file, git_status, git_diff, git_log, git_checkout
- **treesitter** (v2.0): list_symbols, get_function_body, get_class_members,
  get_includes, get_class_hierarchy, get_call_graph, check_syntax,
  get_symbol, replace_symbol, get_project_index, rebuild_index
- **clang**: go_to_definition, find_references, type_hierarchy, find_callers,
  find_callees, type_errors, dead_code, generate_stub, generate_getters,
  generate_constructor, rename_symbol, extract_function, add_include, ...
- **compile** (v2.0): cmake_build, pkg_status, check_run
- **sysinfo** (v2.0): get_time, get_pwd, disk_free, sys_info, gpu_info,
  set_power_limit, process_list, networking, system_status, cuda_info
- **websearch** (v1.1): web_search

---

## Deadlock-Erkennung

| Fehlerzahl | Eskalation                     |
|------------|--------------------------------|
| 3          | Warnung — Argumente prüfen     |
| 5          | Umleitung — anderen Weg suchen |
| 7          | Abbruch                        |

---

## Build + Laufzeit

```bash
cd ~/ai/LlamaQT && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

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
- [x] Zwei Sampler-Profile + /dev/urandom Seed
- [x] Thinking-Filter, Tool-Deadlock, JSON-Repair
- [x] Stop-Bug-Fix (Session-ID Pattern)
- [x] CommandProcessor + alle Slash-Kommandos inkl. /plan /execute /saveDB /loadDB
- [x] Kontext-Management (80%-Auto + /summarize)
- [x] ChatLogger, AppConfig, ConfigDialog (6 Tabs inkl. Execute-Tab)
- [x] EditorDock (Syntax-Highlighting + QFileSystemWatcher)
- [x] KV-Cache Q8_0 + Flash Attention
- [x] tree-sitter MCP-Server v2.0 + clang MCP-Server
- [x] mcp-servers/common: McpServer.h + PathPolicy.h
- [x] **Plan-Modus**: /plan, TaskTree, PlannerDock, dependsOn-Auflösung
- [x] **TaskNodeDialog**: Edit-Dialog, Transfer-Widget für dependsOn
- [x] **PlannerDock**: Kontextmenü, Expand-Fix, Plan laden
- [x] **TaskTree v4**: clear(), removeNode(), Migration (addColumnIfMissing)
- [x] **TaskNode v3**: result + sideOutput + buildPrompt + FileType-Erkennung
- [x] **Execute-Modus**: startExecute(), advanceExecute(), buildExecutePrompt()
- [x] **Execute Code-Output**: `<code>...</code>` Tags, sideOutput trennen
- [x] **ExecuteMemory v2**: parseFromLlmOutput() — robust gegen Thinking-Artefakte
- [x] **Execute buildPrompt**: wird im Node gespeichert (Debugging)
- [x] **Dateityp-bewusster System-Prompt**: CMake ≠ C++

### In Implementierung / Nächste Schritte
- [ ] **Assembly**: Nodes → Dateien zusammenbauen
      H2->result + H3-Kinder (sortiert) → ~/llamatools/[project]/datei.cpp
      Slash-Kommando: `/assemble` oder Button im PlannerDock
- [ ] **Node-Status nach Execute**: Done/Failed korrekt propagieren
- [ ] **Execute-Stop**: onStop() im Execute-Modus → Node als Interrupted markieren

### Offen (spätere Sessions)
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

Aktuelle Aufgabe: Assembly implementieren.

Was bereits läuft:
- Plan-Modus + Execute-Modus funktionieren
- Execute schreibt Code in node->result (aus <code>-Tags)
- sideOutput + buildPrompt werden separat gespeichert
- Thoughts werden robust geparst (parseFromLlmOutput)
- /saveDB, /loadDB, /execute Kommandos vorhanden
- ConfigDialog hat Execute-Tab

Nächster Schritt — Assembly:
  Alle Done-Nodes eines H1-Zweigs → echte Dateien schreiben
  H2-Node title = Dateiname
  H2->result + H3-Kinder (sortiert nach order/insertionIdx) konkatenieren
  Ziel: ~/llamatools/[AppConfig::executeSandboxProject()]/

Implementierung:
  Agent::assembleProject() — traversiert Tree, schreibt Dateien via write_file MCP
  Slash-Kommando /assemble oder Button im PlannerDock
  Nur Done-Nodes assemblieren, Failed/Pending überspringen

Bitte Rückfragen stellen bevor implementiert wird.
```
