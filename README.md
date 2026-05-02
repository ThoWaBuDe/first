# LlamaQt — Qt6 llama.cpp Chat Interface

Lokale LLM-Chat-Oberfläche mit Tool-Use via MCP-Server.
Entwickelt für Qwen3 auf Debian Trixie.

## Abhängigkeiten

```bash
sudo apt install qt6-base-dev cmake build-essential
```

## Build

```bash
mkdir -p ~/ai/LlamaQT/build && cd ~/ai/LlamaQT/build
cmake .. -DCMAKE_BUILD_TYPE=Release
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
Chat-Template, Tool-Call-Format, Deadlock-Schwellen, Logging.

## Tool-Sandbox

Das Modell kann nur auf `~/llamatools/` zugreifen.
Symlinks werden auf jedem Pfad-Level geprüft (kein Sandbox-Escape).

```bash
mkdir ~/llamatools
```

## Slash-Kommandos

| Kommando              | Aktion                                    |
|-----------------------|-------------------------------------------|
| `/init [Projektname]` | Verzeichnis + Git-Repo + AGENT.md anlegen |
| `/build`              | cmake configure + make                    |
| `/compile`            | nur make                                  |
| `/run`                | cmake + make + Binary starten             |
| `/summarize`          | Konversation manuell zusammenfassen       |
| `/undo [Datei]`       | letzten git-commit rückgängig             |
| `/diff`               | git diff anzeigen                         |

## Eingabe

- **Enter** — Nachricht senden
- **Shift+Enter** — Zeilenumbruch in der Eingabe

## Architektur

```
GUI-Thread                          Worker-Thread
──────────────────────────────────  ──────────────────────────
MainWindow (View/MVP)               LlamaWorker (Active Object)
  └── Agent (Presenter/MVP)           ├── llama_model*
        ├── AgentChat                 ├── llama_context*
        ├── AgentUtils                ├── llama_sampler* Chat
        ├── ChatModel                 └── llama_sampler* Tool
        ├── McpManager
        │     └── McpClient(s)
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

| Server      | Tools (Auswahl)                                        |
|-------------|--------------------------------------------------------|
| filesystem  | read/write/str_replace, list_dir, grep_code, git_*     |
| sysinfo     | get_time, sys_info, gpu_info, set_power_limit          |
| compile     | cmake_build, check_run                                 |
| websearch   | web_search (Tavily API)                                |
| tree-sitter | list_symbols, get_function_body, replace_symbol        |
| clang       | find_references, rename_symbol, type_errors, go_to_def |

## Tool-Call-Format

Automatische Erkennung aus GGUF-Template + Modell-Dateiname.
Unterstützt: Qwen XML Tags, Mistral Native, Gemma4/Google,
Llama3 Tool-Use, Generic.

## Sampler-Profile

| Profil | Top-K | Temp | Verwendung      |
|--------|-------|------|-----------------|
| Chat   | 40    | 0.7  | Konversation    |
| Tool   | 20    | 0.1  | JSON Tool-Calls |

Seed wird vor **jeder** Generation neu aus /dev/urandom gezogen.

## Deadlock-Erkennung

| Fehlerzahl | Eskalation                               |
|------------|------------------------------------------|
| 3          | Warnung — LLM zur Überprüfung auffordern |
| 5          | Umleitung — anderen Weg suchen           |
| 7          | Abbruch — LLM erklärt den Fehler         |

## Code-Editor

EditorDock: andockbares Fenster mit C++ Syntax-Highlighting.
- Öffnet Dateien aus der Sandbox direkt im UI
- Erkennt externe Änderungen durch das Modell (`!` im Tab-Titel)
- Informiert das Modell bei manuellem Speichern automatisch

## Chat-Logging

Konversation wird in `~/llamatools/chat_log/` als Markdown gespeichert.
Aktivierbar in Einstellungen → Logging.

## Hardware-Anforderungen

Getestet mit: RTX 4000 SFF Ada (20GB VRAM), Ryzen 5 5600x, 48GB RAM.
Empfohlene Modelle: Qwen3.5-9B-Q6_K, Qwen3.6-27B-Q4_K_M, Devstral-Small-2-24B.
