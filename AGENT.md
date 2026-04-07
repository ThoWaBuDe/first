# LlamaQt — AGENT.md
> Projektgedächtnis für Claude (analog zu Claude Code).
> Nach einer Session-Pause hier einlesen um sofort produktiv zu sein.
> Wird durch /init [Projektname] angelegt und durch den Agenten aktuell gehalten.

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
    ├── ChatModel        — Gesprächshistorie, ChatML-Prompt-Builder
    ├── McpManager       — Facade über mehrere McpClients
    │     ├── McpClient filesystem  (llamaqt-filesystem)
    │     ├── McpClient sysinfo     (llamaqt-sysinfo)
    │     ├── McpClient compile     (llamaqt-compile)
    │     └── McpClient websearch   (llamaqt-websearch)
    ├── CommandProcessor — Slash-Kommandos
    └── LlamaWorker      (Active Object, Worker-Thread)
          └── llama.cpp
```

### Threading
- **GUI-Thread**: MainWindow, Agent, McpManager, McpClient
- **Worker-Thread**: LlamaWorker (blockierende llama.cpp Inference)
- Kommunikation: Qt Signals/Slots mit QueuedConnection

---

## Dateistruktur

```
LlamaQt/
├── CMakeLists.txt
├── AGENT.md
├── README.md
├── src/
│   ├── main.cpp
│   ├── MainWindow.h/.cpp/.ui
│   ├── Agent.h/.cpp
│   ├── CommandProcessor.h/.cpp
│   ├── ChatModel.h/.cpp
│   ├── LlamaWorker.h/.cpp
│   ├── McpClient.h/.cpp
│   └── McpManager.h/.cpp
└── mcp-servers/
    ├── filesystem/   — Dateioperationen + Git
    ├── sysinfo/      — Systeminfo (read-only)
    ├── compile/      — Build + Retry-Loop
    └── websearch/    — Tavily API
```

---

## MCP-Server & Tools

### filesystem (llamaqt-filesystem)
Sandbox: `~/llamatools/` — Symlink-Schutz auf jeder Pfadebene.
Git-Repo wird durch `/init` initialisiert. Vor jedem schreibenden
Zugriff: auto-commit. Remote-Operationen gesperrt.

| Tool          | Beschreibung                                      |
|---------------|---------------------------------------------------|
| read_file     | Datei lesen, optional Zeilenbereich               |
| write_file    | Datei schreiben (auto-commit vorher)              |
| append_file   | Anhängen (auto-commit vorher)                     |
| str_replace   | Eindeutiger Replace (auto-commit vorher)          |
| list_dir      | Verzeichnis auflisten (Symlinks markiert)         |
| list_symbols  | C++ Klassen/Methoden aus Quelldatei               |
| mkdir         | Verzeichnis anlegen                               |
| grep_code     | Regulären Ausdruck in Dateien suchen (rekursiv)   |
| tree          | Rekursiver Verzeichnisbaum                        |
| git_status    | git status in Sandbox                             |
| git_diff      | git diff (working tree oder zwischen Commits)     |
| git_log       | git log (letzte N Commits)                        |
| git_checkout  | git checkout (Datei/Commit) — kein remote        |

### compile (llamaqt-compile)
| Tool        | Beschreibung                                          |
|-------------|-------------------------------------------------------|
| cmake_build | cmake + make, auto Retry-Loop (3x bei Fehler)        |
| pkg_status  | installierte Pakete prüfen                           |
| check_run   | Binary prüfen/starten (danger_zone:true = run)       |

### sysinfo / websearch — unverändert

---

## Sampler-Profile

| Profil | Top-K | Temp | Top-P | Verwendung              |
|--------|-------|------|-------|-------------------------|
| Chat   | 40    | 0.7  | 0.95  | Normale Konversation    |
| Tool   | 20    | 0.1  | 0.50  | Tool-Calls, nach Tools  |

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
            │     → Diff anzeigen (bei Datei-Änderungen)
            │     → Tool-Ergebnis → startGeneration(Tool)
            └── Fall C: normale Antwort → fertig
```

---

## Slash-Kommandos (CommandProcessor)

| Kommando              | Aktion                                                        |
|-----------------------|---------------------------------------------------------------|
| `/init [Name]`        | Verzeichnis + Git-Repo + AGENT.md in Sandbox anlegen         |
| `/build`              | cmake + make (mit Retry-Loop)                                |
| `/compile`            | nur make                                                      |
| `/run`                | cmake (falls nötig) + make + Binary starten                  |
| `/summarize`          | Konversation manuell zusammenfassen                          |
| `/undo`               | letzten git-commit rückgängig                                |
| `/diff`               | git diff anzeigen (letzte Änderungen)                        |

---

## Kontext-Management

- **80%-Schwelle**: bei >80% Auslastung automatisch zusammenfassen
- **`/summarize`**: manuell auslösen
- **Strategie**: LLM fasst bisherige Konversation zusammen.
  Neuer Kontext: System-Prompt + Zusammenfassung + letzte 2 Paare.

---

## Git als Undo-System

Sandbox = Git-Repo. Vor jedem schreibenden Tool-Call:
```
git add -A && git commit -m "auto: str_replace datei.cpp"
```
- `/undo`  → git checkout HEAD~1 -- <datei>
- `/diff`  → git diff HEAD~1
**Gesperrt**: push / pull / remote — nur für den User.

---

## Diff-Ansicht

Unified-Diff als farbiger HTML-Block im toolView nach Datei-Änderungen:
- Grün `.added`   — neue Zeilen
- Rot `.removed`  — entfernte Zeilen
- Grau `.context` — Kontext
LCS-Algorithmus in Agent.cpp, kein externes Tool.

---

## Deadlock-Erkennung

| Fehlerzahl | Eskalation                      |
|------------|---------------------------------|
| 3          | Warnung — Argumente prüfen      |
| 5          | Umleitung — anderen Weg suchen  |
| 7          | Abbruch — Erklärung an User     |

---

## Stop-Mechanismus

`onStop()` inkrementiert `m_sessionId`. Alle Callbacks prüfen ihre
gespeicherte ID — veraltete werden verworfen.
Pattern: Generation Stamp.

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
- [x] CommandProcessor (/init /build /compile /run)
- [x] AGENT.md Projektgedächtnis

### In Arbeit (diese Session) 🔨
- [x] Kontext-Management (80%-Auto + /summarize)
- [x] Git-Integration im filesystem MCP
- [x] grep_code Tool (rekursive Code-Suche)
- [x] tree Tool (rekursiver Verzeichnisbaum)
- [x] Compile-Retry-Loop (3x automatisch)
- [x] Diff-Ansicht im toolView (LCS, farbig)
- [x] /undo + /diff Kommandos
- [x] Token-Budget (Tool-Ergebnisse kürzen, konfigurierbar)
- [x] Chat-Logging in Markdown (ChatLogger)
- [x] AppConfig / QSettings (Singleton, INI-Format)
- [x] gpu_info Tool (nvidia-smi: VRAM, Temp, Power, Clocks)
- [x] set_power_limit Tool (nvidia-smi -pl, mit Grenzwert-Check)
- [x] CPU-Temperatur in sys_info (/sys/class/thermal)
- [x] Config-Dialog (Tabs: Modell/Sampler/Agent/Logging, Ctrl+,)
- [x] LlamaWorker liest Sampler-Parameter aus AppConfig
- [x] rebuildSamplers() — Sampler live neu bauen ohne Neustart
- [x] Git init automatisch pro Projektverzeichnis (kein Henne-Ei-Problem)
- [x] Git-Tools pfadbasiert (jedes Projekt eigenes Repo)

### Offen
- [ ] Planner/Executor-Trennung
- [ ] Token-Budget pro Tool-Call (intelligentes Kürzen)
- [ ] Mehrere Dateien parallel
- [ ] Automatische AGENT.md-Aktualisierung nach Session
- [ ] Mehrere Modelle (klein/groß)
- [ ] Live-Output bei /run
- [ ] Persistentes Konversationsgedächtnis zwischen Sessions

### Neue Ideen (noch nicht bewertet)

#### Config-System (AppConfig / QSettings)
- Zentrales Singleton `AppConfig` das alle Parameter hält und via `QSettings`
  im Home-Verzeichnis persistiert (`~/.config/LlamaQt/LlamaQt.conf`)
- Agent, LlamaWorker, McpManager werden mit Werten aus AppConfig initialisiert
- Konfigurierbare Parameter (Vorschlag):
  - Modellpfad
  - Sampler-Parameter (TopK, Temp, TopP, MinP, Seed je Profil)
  - Context-Größe (n_ctx), Batch-Size
  - Summarize-Schwelle (aktuell 80%)
  - Max Continuations (aktuell 3)
  - Deadlock-Schwellen (3/5/7)
  - Tavily API Key
  - Sandbox-Pfad (aktuell ~/llamatools/)
  - Logging-Einstellungen
- **Übernahme laufender Einstellungen**: Zwei Klassen von Parametern:
  - *Sofort wirksam*: Sampler-Werte, Schwellen, Logging → direkt übernehmen
  - *Neustart nötig*: Modellpfad, n_ctx, Sandbox-Pfad → Agent herunterfahren,
    neu aufsetzen (LlamaWorker::cleanup() + initialize())
- Config-Dialog: modaler QDialog mit Tabs (Modell / Sampler / Agent / Logging)

#### Chat-Logging
- Chat in Datei loggen (HTML mit aufklappbaren Blöcken für Thinking/Tools,
  oder plain Markdown)
- Allgemeines Logging (qDebug → Datei, Log-Level konfigurierbar)

#### NVIDIA / Hardware-Monitoring
- `sys_info` im sysinfo-MCP um GPU-Daten erweitern:
  - `nvidia-smi` via QProcess (immer verfügbar wenn Treiber installiert)
  - NVML (libnvidia-ml) direkt einbinden für:
    - GPU-Name, VRAM total/used/free
    - GPU-Auslastung (%), GPU-Temperatur
    - Power Draw (Watt) und Power Limit
    - Power Limit setzen via nvmlDeviceSetPowerManagementLimit()
      → nützlich für Thermal Management beim langen Inferenz-Betrieb
  - Fallback: wenn NVML nicht vorhanden → nvidia-smi parsen
  - Neues Tool: `gpu_info` — gibt alle GPU-Metriken zurück
  - Neues Tool: `set_power_limit` — setzt GPU Power Limit (Watt)
    Sicherheit: Minimum/Maximum aus NVML respektieren
- CPU-Metriken erweitern: /proc/stat für CPU-Auslastung (%), Temperatur via
  /sys/class/thermal/thermal_zone*/temp

#### Ollama Modell-Integration
- Ollama speichert Modelle in `~/.ollama/models/`:
  - `manifests/` — JSON mit Modell-Metadaten (Name, Parameter, Quantisierung)
  - `blobs/`     — eigentliche Gewichte (SHA256-benannt)
- Idee: beim Modell-Auswahl-Dialog den Ollama-Manifest-Ordner scannen
  und verfügbare Modelle anzeigen (Name, Größe, Quantisierung)
- OllamaScanner-Klasse (oder Teil von AppConfig):
  - `scanOllamaModels()` → Liste von {name, path, size, quantization}
  - Blob-Pfad aus Manifest auflösen → echter .gguf-Pfad für llama.cpp
  - Achtung: Ollama-Blobs sind nicht immer GGUF — Format prüfen
- Im Config-Dialog: "Aus Ollama importieren" Button

- Separates Fenster (`QMainWindow` oder `QDockWidget`) mit `QTabWidget`
- Jeder Tab = eine Datei aus der Sandbox (geladen via read_file MCP)
- Einfacher Text-Editor (QPlainTextEdit, Monospace, Zeilennummern)
- **Notify ans Modell**: wenn der User eine Datei im Editor ändert und speichert,
  geht eine Systemnachricht in den Chat-Context:
  `[User hat Datei 'foo.cpp' manuell bearbeitet. Bitte neu einlesen vor weiteren Änderungen.]`
- Syntax-Highlighting: QSyntaxHighlighter für C++ (Basis-Keywords reichen)
- Tab zeigt an ob Datei dirty (ungespeichert) ist


---

## Build

```bash
cd ~/llamaqt/build
cmake .. -DLLAMA_BUILD_DIR=$HOME/ai/qLP/build -DLLAMA_SRC_DIR=$HOME/ai/qLP
make -j$(nproc)
```
