# LlamaQt — Qt6 llama.cpp Chat Interface

Lokale LLM-Chat-Oberfläche mit Tool-Use via MCP-Server.
Entwickelt für Qwen3 auf Debian Trixie.

## Abhängigkeiten

```bash
sudo apt install qt6-base-dev cmake build-essential
```

## Build

```bash
mkdir -p ~/llamaqt/build && cd ~/llamaqt/build

cmake .. \
  -DLLAMA_BUILD_DIR=$HOME/ai/qLP/build \
  -DLLAMA_SRC_DIR=$HOME/ai/qLP \
  -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)
```

## Starten

```bash
# Mit Web-Search (Tavily API):
export TAVILY_API_KEY="tvly-..."
./LlamaQt

# Ohne Web-Search:
./LlamaQt
```

## Modell-Pfad

In `src/MainWindow.h`:
```cpp
static constexpr const char *MODEL_PATH =
    "/home/thomas/ai/models/Qwen3.5-9B-Q6_K.gguf";
```

## Tool-Sandbox

Das Modell kann nur auf `~/llamatools/` zugreifen.
Symlinks werden auf jedem Pfad-Level geprüft (kein Sandbox-Escape).

```bash
mkdir ~/llamatools
echo "Hallo Welt" > ~/llamatools/test.txt
```

## Slash-Kommandos

| Kommando              | Aktion                                              |
|-----------------------|-----------------------------------------------------|
| `/init [Projektname]` | Verzeichnis + AGENT.md in Sandbox anlegen           |
| `/build`              | cmake configure + make                              |
| `/compile`            | nur make (kein Re-Configure)                        |
| `/run`                | cmake (falls nötig) + make + Binary starten         |

Slash-Kommandos erzeugen einen präzisen Prompt der ans LLM geschickt wird.
Das LLM führt die eigentliche Arbeit via Tools aus.

## Eingabe

- **Enter** — Nachricht senden
- **Shift+Enter** — Zeilenumbruch in der Eingabe

## Architektur

```
GUI-Thread                      Worker-Thread
────────────────────────────    ──────────────────────────
MainWindow (View/MVP)           LlamaWorker (Active Object)
  └── Agent (Presenter/MVP)       ├── llama_model*
        ├── ChatModel             ├── llama_context*
        ├── McpManager            ├── llama_sampler* Chat
        │     └── McpClient(s)    └── llama_sampler* Tool
        └── CommandProcessor

Signal-Slot (QueuedConnection = thread-sichere Queue):
  tokenGenerated  →  onTokenReceived  →  filterToken
  generationDone  →  onGenerationDone →  handleToolCall / fertig
  modelLoaded     →  onModelLoaded

Patterns:
  MVP              - MainWindow (View) / Agent (Presenter) / ChatModel (Model)
  Active Object    - LlamaWorker im eigenen Thread
  Producer/Consumer- Token-Streaming via Signals
  Command          - CommandProcessor (Slash-Kommandos)
  Facade           - McpManager über mehrere McpClients
  State Machine    - filterToken (<think>...</think>)
  Generation Stamp - Session-ID für sicheres onStop()
```

## MCP-Server

| Server       | Tools                                        |
|--------------|----------------------------------------------|
| filesystem   | read_file, write_file, append_file, str_replace, list_dir, list_symbols, mkdir |
| sysinfo      | get_time, get_pwd, disk_free, sys_info       |
| compile      | cmake_build, pkg_status, check_run           |
| websearch    | web_search                                   |

## Sampler-Profile

| Profil | Top-K | Temp | Top-P | Verwendung               |
|--------|-------|------|-------|--------------------------|
| Chat   | 40    | 0.7  | 0.95  | Normale Konversation     |
| Tool   | 20    | 0.1  | 0.50  | Tool-Calls, nach Tools   |

## Deadlock-Erkennung

Wenn dasselbe Tool wiederholt mit demselben Fehler scheitert:

| Fehlerzahl | Eskalation                                    |
|------------|-----------------------------------------------|
| 3          | Warnung — LLM zur Überprüfung auffordern      |
| 5          | Umleitung — anderen Weg suchen                |
| 7          | Abbruch — LLM erklärt dem Nutzer den Fehler   |

## Stop-Mechanismus

`onStop()` inkrementiert eine Session-ID. Alle laufenden MCP-Callbacks
und Continuations prüfen ihre gespeicherte ID — veraltete werden verworfen.
Das verhindert dass Continuations nach dem Stop weiterlaufen.

## Projektgedächtnis für Claude

`AGENT.md` im Projektverzeichnis enthält Architektur, Konventionen und
aktuellen Stand — analog zu Claude Code. Claude liest es am Sessionbeginn.
