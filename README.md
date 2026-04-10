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

## Konfiguration

Einstellungen unter `Datei → Einstellungen` (Ctrl+,) oder direkt in:
```
~/.config/LlamaQt/LlamaQt.conf
```

Konfigurierbar: Modellpfad, Sampler-Parameter, Context-Größe,
Chat-Template, Deadlock-Schwellen, Logging, Tavily API Key.

## Tool-Sandbox

Das Modell kann nur auf `~/llamatools/` zugreifen.
Symlinks werden auf jedem Pfad-Level geprüft (kein Sandbox-Escape).
Absolute Pfade werden abgelehnt.

```bash
mkdir ~/llamatools
```

## Slash-Kommandos

| Kommando              | Aktion                                              |
|-----------------------|-----------------------------------------------------|
| `/init [Projektname]` | Verzeichnis + Git-Repo + AGENT.md anlegen           |
| `/build`              | cmake configure + make (3x Retry)                  |
| `/compile`            | nur make (kein Re-Configure)                        |
| `/run`                | cmake + make + Binary starten                       |
| `/summarize`          | Konversation manuell zusammenfassen                 |
| `/undo [Datei]`       | letzten git-commit rückgängig                       |
| `/diff`               | git diff anzeigen                                   |

## Eingabe

- **Enter** — Nachricht senden
- **Shift+Enter** — Zeilenumbruch in der Eingabe

## Architektur

```
GUI-Thread                          Worker-Thread
──────────────────────────────────  ──────────────────────────
MainWindow (View/MVP)               LlamaWorker (Active Object)
  └── Agent (Presenter/MVP)           ├── llama_model*
        ├── ChatModel                 ├── llama_context*
        ├── McpManager                ├── llama_sampler* Chat
        │     └── McpClient(s)        └── llama_sampler* Tool
        ├── CommandProcessor
        ├── ChatLogger
        └── AppConfig (Singleton)

Signal-Slot (QueuedConnection = thread-sichere Queue):
  tokenGenerated  →  onTokenReceived  →  filterToken
  generationDone  →  onGenerationDone →  handleToolCall / fertig
  modelLoaded     →  onModelLoaded

Patterns:
  MVP              - MainWindow / Agent / ChatModel
  Active Object    - LlamaWorker im eigenen Thread
  Producer/Consumer- Token-Streaming via Signals
  Command          - CommandProcessor (Slash-Kommandos)
  Facade           - McpManager über mehrere McpClients
  State Machine    - filterToken (<think>...</think>)
  Generation Stamp - Session-ID für sicheres onStop()
  Singleton        - AppConfig
  Observer         - ChatLogger, EditorDock/QFileSystemWatcher
```

## MCP-Server

| Server     | Version | Tools                                                      |
|------------|---------|------------------------------------------------------------|
| filesystem | v2.3    | read/write/append/str_replace/patch, list_dir, list_symbols|
|            |         | mkdir, grep_code, search_code, tree, find_files            |
|            |         | read_multiple_files, move_file, copy_file                  |
|            |         | git_status/diff/log/checkout                               |
| sysinfo    | v2.0    | get_time, get_pwd, disk_free, sys_info                     |
|            |         | gpu_info, set_power_limit                                  |
| compile    | v2.0    | cmake_build (3x Retry), pkg_status, check_run              |
| websearch  | v1.1    | web_search (Tavily API)                                    |

## Sampler-Profile

| Profil | Top-K | Temp | Top-P | Min-P | Seed         |
|--------|-------|------|-------|-------|--------------|
| Chat   | 40    | 0.7  | 0.95  | 0.05  | /dev/urandom |
| Tool   | 20    | 0.1  | 0.50  | 0.05  | /dev/urandom |

Seed wird vor **jeder** Generation neu gezogen — kein deterministischer Loop.

## KV-Cache

```cpp
ctxParams.type_k = GGML_TYPE_Q8_0;
ctxParams.type_v = GGML_TYPE_Q8_0;
ctxParams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
```

Qwen3.5 hat hybride Architektur (Transformer + Recurrent/SSM-Layer).
Recurrent-Layer brauchen keinen KV-Cache → reduzierter VRAM-Verbrauch.

## Deadlock-Erkennung

| Fehlerzahl | Eskalation                                    |
|------------|-----------------------------------------------|
| 3          | Warnung — LLM zur Überprüfung auffordern      |
| 5          | Umleitung — anderen Weg suchen                |
| 7          | Abbruch — LLM erklärt dem Nutzer den Fehler   |

## Code-Editor

EditorDock: andockbares Fenster mit C++ Syntax-Highlighting.
- Öffnet Dateien aus der Sandbox direkt im UI
- Erkennt externe Änderungen durch das Modell (`!` im Tab-Titel)
- Informiert das Modell bei manuellem Speichern automatisch

## Chat-Logging

Konversation wird in `~/llamatools/chat_log/` als Markdown gespeichert.
Aktivierbar in Einstellungen → Logging.
