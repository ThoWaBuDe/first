# LlamaQt — AGENT.md
> Dieses Dokument ist das Projektgedächtnis für Claude (analog zu Claude Code).
> Es beschreibt Architektur, Konventionen und aktuellen Stand so dass Claude
> nach einer Session-Pause sofort produktiv ist.

---

## Projektübersicht

**LlamaQt** ist eine lokale LLM-Chat-Oberfläche in C++/Qt6.
- Inference: llama.cpp (Qwen3.5-9B-Q6_K, lokal)
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
    ├── ChatModel        — Gesprächshistorie, ChatML-Prompt-Builder
    ├── McpManager       — Facade über mehrere McpClients
    │     ├── McpClient filesystem  (llamaqt-filesystem)
    │     ├── McpClient sysinfo     (llamaqt-sysinfo)
    │     ├── McpClient compile     (llamaqt-compile)
    │     └── McpClient websearch   (llamaqt-websearch)
    ├── CommandProcessor — Slash-Kommandos (/init, /build, /compile, /run)
    └── LlamaWorker      (Active Object, Worker-Thread)
          └── llama.cpp  (llama_model, llama_context, llama_sampler)
```

### Threading
- **GUI-Thread**: MainWindow, Agent, McpManager, McpClient (QProcess-Callbacks)
- **Worker-Thread**: LlamaWorker (blockierende llama.cpp Inference)
- Kommunikation: Qt Signals/Slots mit QueuedConnection (thread-safe Queue)

---

## Dateistruktur

```
LlamaQt/
├── CMakeLists.txt
├── AGENT.md                  ← dieses Dokument
├── README.md
├── src/
│   ├── main.cpp
│   ├── MainWindow.h/.cpp/.ui
│   ├── Agent.h/.cpp          — Presenter, Agenten-Loop
│   ├── CommandProcessor.h/.cpp — Slash-Kommandos
│   ├── ChatModel.h/.cpp      — ChatML Prompt-Builder
│   ├── LlamaWorker.h/.cpp    — llama.cpp Inference-Thread
│   ├── McpClient.h/.cpp      — JSON-RPC 2.0 stdio Client
│   └── McpManager.h/.cpp     — Facade, Tool-Routing
└── mcp-servers/
    ├── filesystem/            — read_file, write_file, str_replace, ...
    ├── sysinfo/               — get_time, get_pwd, disk_free, sys_info
    ├── compile/               — cmake_build, pkg_status, check_run
    └── websearch/             — web_search (Tavily API)
```

---

## MCP-Server

| Server       | Binary                  | Tools                                                    |
|--------------|-------------------------|----------------------------------------------------------|
| filesystem   | llamaqt-filesystem      | read_file, write_file, append_file, str_replace, list_dir, list_symbols, mkdir |
| sysinfo      | llamaqt-sysinfo         | get_time, get_pwd, disk_free, sys_info                   |
| compile      | llamaqt-compile         | cmake_build, pkg_status, check_run                       |
| websearch    | llamaqt-websearch       | web_search                                               |

**Sandbox**: `~/llamatools/` — alle Dateizugriffe nur hier erlaubt.
**Symlink-Schutz**: Jede Pfadkomponente wird auf Symlinks geprüft (kein Sandbox-Escape).

---

## Sampler-Profile (LlamaWorker)

| Profil | Top-K | Temp | Top-P | Verwendung              |
|--------|-------|------|-------|-------------------------|
| Chat   | 40    | 0.7  | 0.95  | Normale Konversation    |
| Tool   | 20    | 0.1  | 0.50  | Tool-Calls, nach Tools  |

**Hinweis**: Lazy-Grammar (llama_sampler_init_grammar_lazy_patterns) ist für
Qwen3 + Thinking noch nicht stabil — GGML_ASSERT bei Trigger-Token.
Stattdessen: Post-hoc JSON-Validierung in Agent::handleToolCall().

---

## Tool-Call Format (Qwen3 Hermes)

```
<tool_call>
{"name": "tool_name", "arguments": {"key": "value"}}
</tool_call>
```

Thinking läuft durch `<think>...</think>` Blöcke davor.

---

## Agenten-Loop (Agent.cpp)

```
onUserMessage()
    → startGeneration(Chat)
        → onTokenReceived()  [filterToken: think vs. sichtbar]
        → onGenerationDone()
            ├── Fall A: offener <tool_call> ohne </tool_call>
            │     → Continuation (max 3x, SamplerProfile::Tool)
            ├── Fall B: vollständiger <tool_call>...</tool_call>
            │     → handleToolCall()
            │           → JSON validieren / reparieren
            │           → m_mcp.callTool()
            │           → Tool-Ergebnis in ChatModel
            │           → startGeneration(Tool)
            └── Fall C: normale Antwort → fertig
```

**Deadlock-Erkennung** (seit Patch 2):
- Gleiches Tool + gleiche Argumente schlägt N mal fehl
- Eskalation: 3x → Warnung ans LLM, 5x → andere Lösung suchen, 7x → Abbruch

---

## Slash-Kommandos (CommandProcessor)

| Kommando              | Aktion                                                        |
|-----------------------|---------------------------------------------------------------|
| `/init [Projektname]` | Verzeichnis + AGENT.md in Sandbox anlegen, Modell einweisen  |
| `/build`              | cmake + make (via cmake_build Tool)                          |
| `/compile`            | nur make (via cmake_build Tool, kein Re-Configure)           |
| `/run`                | ggf. cmake + make, dann check_run mit danger_zone:true        |

---

## Bekannte Probleme / TODOs

- [ ] Lazy-Grammar für Tool-Profil sobald llama.cpp API stabil
- [ ] `/run` Live-Output (check_run gibt aktuell nur Gesamtausgabe zurück)
- [ ] Kontext-Management: automatisches Zusammenfassen bei >80% Auslastung

---

## Konventionen

- **Sprache**: Code-Kommentare auf Deutsch, Tool-Descriptions auf Englisch
- **Pfade**: immer relativ zur Sandbox-Root, kein automatischer `/build/`-Prefix
- **Patterns**: MVP, Active Object, Command Dispatcher, Facade, State Machine
- **Qt**: Signals/Slots für alles, kein direkter Thread-Zugriff
- **Fehler**: immer per Signal nach oben, nie silent schlucken

---

## Build

```bash
cd ~/llamaqt/build
cmake .. -DLLAMA_BUILD_DIR=$HOME/ai/qLP/build -DLLAMA_SRC_DIR=$HOME/ai/qLP
make -j$(nproc)
```

## Modell-Pfad

`src/MainWindow.h` → `MODEL_PATH`
