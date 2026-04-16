# LlamaQt — AGENT.md (aktualisiert)
> Projektgedächtnis für Claude und Qwen.

---

## Projektübersicht

**LlamaQt** ist eine lokale LLM-Chat-Oberfläche die zu einem vollständigen
**lokalen Coding-Agenten** ausgebaut wird.

- Inference: llama.cpp (Qwen3.5-9B-Q6_K oder 26B, lokal)
- Tool-Use: MCP-Server via stdio JSON-RPC 2.0
- GUI: Qt6 Widgets
- Build: CMake, Debian Trixie

---

## Architektur (MVP + Komposition)

```
MainWindow  (View)
    │
    ▼
Agent       (Koordinator, GUI-Thread)
    ├── AgentChat      — Chat-Modus (Tool-Calls, filterToken, summarize)
    ├── AgentPlan      — Plan-Modus (iterativ + JSON-Fallback)
    ├── AgentExecute   — Execute-Modus (Code + Validierung)
    ├── AgentAssemble  — Assembly (Nodes → Dateien)
    ├── AgentUtils     — Namespace: repairJson, toolCallKey, ...
    ├── ChatModel      — Gesprächshistorie
    ├── McpManager     — Facade + Auto-Restart mit Backoff
    ├── CommandProcessor
    ├── ChatLogger
    ├── AppConfig      — Singleton, 3 Sampler-Profile
    ├── TaskTree       — RAM-first + SQLite + Symbol-Index
    ├── ExecuteMemory  — Thoughts/Kurzzeitgedächtnis
    ├── CodeAssembler  — deduplicateIncludes
    └── LlamaWorker    (Active Object, Worker-Thread)
          └── llama.cpp
```

---

## TaskTree — Aufgabenbaum

### Hierarchie
```
H0 (level=0) — Gesamtziel
H1 (level=1) — Dateigruppe / Modul
H2 (level=2) — Einzelne Datei
H3 (level=3) — Implementierungsschritt (Methode)
H4 (level=4) — Validierungs-Node (automatisch nach H3)
```

### TaskNode Felder (v4)
```
id, level, order, insertionIdx, scope
title, description
symbol           — NEU: C++-Symbolname ("Timer::start()")
result           — sauberer Code
sideOutput       — Thinking, Warnungen
buildPrompt      — Debugging: was hat das Modell gesehen?
validationResult — NEU: "ok" oder Fehlerbeschreibung
dependsOn        — horizontale Abhängigkeiten
status, createdAt, updatedAt
```

### Symbol-Awareness
```
TaskTree::symbolExists("Timer::start()") → O(1)
TaskTree::findBySymbol("Timer::start()") → TaskNode*
TaskTree::setSymbol(node, "Timer::start()")
```

### Validierungs-Node (H4)
```
Nach jedem H3-Node automatisch:
  createValidationNode(implNode) → H4-Node als Kind

H4-Prompt: "Prüfe diesen Code. Antworte mit 'ok' oder '<Fehler>'"
H4-Ergebnis:
  "ok"     → H4 Done, weiter
  "<Fehler>" → H4 Failed, H3-Elter als Failed markieren
```

---

## Plan-Modus (iterativ — NEU)

### Ablauf
```
/plan <Auftrag>
  → startPlan() → TaskTree.clear()
  → Modell bekommt create_node / set_depends_on / get_nodes / plan_done Tools
  → Modell baut Graph Node für Node
  → plan_done → planReady() → PlannerDock

Fallback: Modell gibt <plan>...</plan> JSON → handlePlanJson() wie bisher
```

### Interne Plan-Tools
```
create_node     — Node anlegen (prüft Symbol-Duplikate!)
set_depends_on  — Abhängigkeit from_id → to_id
get_nodes       — aktuellen Baum anzeigen
plan_done       — Plan abschließen
```

### Symbol-Duplikat-Schutz
```
create_node mit bekanntem Symbol →
  {"warning": "Symbol 'Timer::start()' existiert bereits in Node 5",
   "existing_id": 5}
→ Modell verwendet set_depends_on statt neuen Node
```

---

## Sampler-Profile (3 Profile)

| Profil  | Top-K | Temp | Verwendung                    |
|---------|-------|------|-------------------------------|
| Chat    | 40    | 0.7  | Konversation, Plan-Analyse    |
| Execute | 20    | 0.2  | Code-Generierung (NEU)        |
| Tool    | 20    | 0.1  | JSON Tool-Calls, Optimize     |

Alle konfigurierbar in AppConfig + ConfigDialog Sampler-Tab.

---

## MCP Auto-Restart (Backoff)

```
Server crasht → serverDied Signal
  → scheduleRestart()
  → Backoff: 1s → 5s → 30s (3 Versuche)
  → onRestartTimer() → startServer() neu
  → serverRestored / serverGaveUp Signal → UI-Feedback
```

---

## Execute-Modus (mit Validierung)

```
advanceExecute()
  → nextPending() — Pending-Blatt-Node (H3 oder H4)

  H3-Node:
    → buildExecutePrompt() (Lichtkegel + Symbol + Geschwister)
    → startGeneration(Execute-Sampler)
    → handleExecuteCode() → result + sideOutput
    → createValidationNode() → H4-Node anlegen
    → updateThoughts()

  H4-Validierungs-Node:
    → buildValidationPrompt() (Code + Interface)
    → startGeneration(Chat-Sampler)
    → handleValidationResult()
      "ok"     → H4 Done → advanceExecute()
      "<Fehler>" → H4 Failed, H3 Failed → advanceExecute()
```

---

## Offene TODOs

### Abgehakt ✓
- A B C D — Bugs in advanceExecute, Marker, Thoughts, dependsOn
- E — Agent God Object aufgeteilt (AgentChat/Plan/Execute/Assemble/Utils)
- F — Symbol-Awareness im Planner-Prompt
- G — Iterativer Plan-Aufbau mit create_node Tools
- H — Symbol-Feld in TaskNode + Symbol-Index in TaskTree
- I — SamplerProfile::Execute (dritter Sampler)
- N — Validierungs-Node H4 nach jedem H3
- O — Skelett-Hinweis im Execute-Prompt
- S — MCP Auto-Restart mit Exponential Backoff

### Offen
- J — Feedback Execute → Optimizer (check_syntax nach Validierung)
- K — Semantic Merge LLM (Assembly-Qualität)
- L — Clang-AST Rückkopplung
- M — Pre-Execute Dry-Run
- P — MCP für Node-Manipulation (extern)
- Q — generateDelta() aktivieren
- R — KV-Cache Rollback
- T — NodeGraphView Layout-Verbesserung

---

## Build

```bash
cd ~/ai/LlamaQT && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```
