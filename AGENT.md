# LlamaQt — AGENT.md
> Projektgedächtnis für Claude und Qwen.

---

## Projektübersicht

**LlamaQt** ist eine lokale LLM-Chat-Oberfläche mit Tool-Use via MCP-Server.
Entwickelt für Qwen3 auf Debian Trixie.

- Inference: llama.cpp (Qwen3.5-9B-Q6_K oder 27B Q4, lokal)
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
    ├── AgentUtils     — Namespace: repairJson, toolCallKey, ...
    ├── ChatModel      — Gesprächshistorie
    ├── McpManager     — Facade + Auto-Restart mit Backoff
    ├── CommandProcessor
    ├── ChatLogger
    ├── AppConfig      — Singleton, 2 Sampler-Profile (Chat, Tool)
    └── LlamaWorker    (Active Object, Worker-Thread)
          └── llama.cpp
```

---

## Sampler-Profile (2 Profile)

| Profil | Top-K | Temp | Verwendung              |
|--------|-------|------|-------------------------|
| Chat   | 40    | 0.7  | Konversation            |
| Tool   | 20    | 0.1  | JSON Tool-Calls         |

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

## Tool-Call-Format Detection

```
GGUF Jinja-Template → ToolCallFormat::detectFromJinja()
  → Auto: Modellname → ToolCallFormat::detectFromModelName()
  → Agent::onToolFormatDetected() → m_activeToolFormat
  → McpManager::buildToolsSystemPrompt(m_activeToolFormat)
```

Unterstützte Formate: QwenXmlTags, MistralNative, Gemma4Google,
Llama3ToolUse, Generic.

---

## Entscheidungen & Learnings

### Node-Konzept (eingefroren Mai 2026)

TaskTree/TaskNode/Plan/Execute/Assemble wurde entwickelt und dann
bewusst eingefroren. Grund:

- Kleine lokale Modelle (9B-27B) verlieren Konsistenz über viele
  Tool-Call-Sequenzen
- Header↔CPP-Konsistenz nicht zuverlässig herstellbar
- Code-Assembler hatte zu viele Sonderfälle (keine generische Lösung)
- Overhead enorm im Verhältnis zum erzielbaren Nutzen

Der Code liegt im Git-History (Commit: "Node Konzept derzeit nicht
weiter verfolgt, für kleine LLM zu komplex").

### Nächste Richtung: Symbol-basiertes Editing

Statt Nodes generieren → Code assemblen:
- LlamaQt holt eine Funktion via get_function_body / clangd
- LLM bekommt NUR diese eine Funktion als Kontext
- LLM schreibt modifizierte Version
- LlamaQt schreibt sie zurück via replace_symbol
- Git als Undo

Ein Schritt, isolierter Kontext, deterministisch kontrolliert.
Passt zu lokalen 14B Modellen.

### Modell-Grenzen (empirisch)

- Qwen3.5-9B-Q6_K: Tool-Calling funktioniert, kleine Aufgaben OK
- Qwen3.6-27B-Q4_K_M: Tool-Calling gut, größere Aufgaben möglich
- Qwen2.5-Coder-14B-Q6_K: Tool-Calling funktioniert NICHT mit
  <tool_call>-Format (generiert JSON in Code-Block ohne Tags)
- Devstral-Small-2-24B: Mistral-Format, funktioniert
- TicTacToe mit 14B: 4/10 Versuche erfolgreich (mit Tool-Use)
- Mehrstufige Planung über viele Dateien: nicht zuverlässig möglich

### Deterministische Seeds → Loops

Feste Seeds führen zu Endlosschleifen bei wiederholten Fehlern.
Immer /dev/urandom Seeds, vor jeder Generation neu ziehen.

### GBNF Grammar mit Thinking-Modellen

llama_sampler_init_grammar_lazy_patterns() crasht mit Qwen Thinking.
Lösung: Post-hoc JSON-Validierung + </tool_call> Stop-Sequenz.

### llama_kv_cache_seq_rm

In aktueller llama.cpp Version umbenannt zu llama_memory_seq_rm.

---

## Offene TODOs

### Infrastruktur
- Q — generateDelta() aktivieren (n_past-Tracking in LlamaWorker)
- R — KV-Cache Rollback (llama_memory_seq_rm nach Tool-Fehler)

### Features
- Symbol-basiertes Editing (nächste Hauptrichtung)
- Incus Container Integration (INCUS_TODO.md)
- Persistent conversation memory zwischen Sessions
- XTC/DRY Sampler-Optionen
- Multiple Modell-Support (klein für Tool-Calls, groß für Planung)

---

## Build

```bash
cd ~/ai/LlamaQT && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

## Key Source Locations

- Main project: `~/ai/LlamaQT/`
- Sandbox: `~/llamatools/`
- Chat logs: `~/llamatools/chat_log/`
- MCP servers: `LlamaQt/mcp-servers/{filesystem,sysinfo,compile,websearch,tree-sitter,clang}/`
