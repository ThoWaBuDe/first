# llamaqt-filesystem MCP Server v3.0

Lokaler Dateisystem-MCP-Server für LlamaQt. Bietet dem LLM sicheren,
sandbox-beschränkten Zugriff auf das Dateisystem mit Git als Undo-System.

## Architektur

```
McpServer (JSON-RPC Loop)
    │  unique_ptr<ToolBase>
    ├── ReadFileTool
    ├── WriteFileTool
    ├── AppendFileTool
    ├── StrReplaceTool
    ├── PatchFileTool
    ├── ListDirTool
    ├── MkdirTool
    ├── TreeTool
    ├── GrepCodeTool
    ├── SearchCodeTool
    ├── FindFilesTool
    ├── ReadMultipleFilesTool
    ├── MoveFileTool
    ├── CopyFileTool
    ├── GitStatusTool
    ├── GitDiffTool
    ├── GitLogTool
    └── GitCheckoutTool

PathPolicy  — Pfad-Sicherheit (Symlink-Check, Root-Zugehörigkeit)
GitHelper   — git-Operationen (autoCommit, run, repoFor)
```

**Pattern:** Abstract Base Class + Dependency Injection + Facade

Jedes Tool erbt von `FilesystemToolBase` → `ToolBase`. Der Konstruktor
bekommt `PathPolicy*` und `GitHelper*` injiziert. `McpServer` kennt nur
`ToolBase*` — er weiß nichts von Pfaden oder git.

## Umgebungsvariablen

| Variable          | Default                   | Beschreibung                        |
|-------------------|---------------------------|-------------------------------------|
| `LLAMAQT_SANDBOX` | `~/llamatools`            | Schreibbare Sandbox (read+write)    |
| `LLAMAQT_SOURCES` | `~/ai/LlamaQT`            | LlamaQt-Quellcode (read-only)       |
| `LLAMAQT_TRASH`   | `~/.llamatools_trash`     | Papierkorb für move_file            |

## Tools

### Datei-Tools

| Tool           | Beschreibung                                                      |
|----------------|-------------------------------------------------------------------|
| `read_file`    | Datei lesen, optional Zeilenbereich (start_line/end_line)        |
| `write_file`   | Datei schreiben (auto-commit vorher, max 16384 Zeichen)          |
| `append_file`  | Text anhängen (auto-commit nachher)                              |
| `str_replace`  | Eindeutiger Replace (muss genau einmal vorkommen)                |
| `patch_file`   | Unified-Diff anwenden (multi-hunk, auto-commit)                  |

### Verzeichnis-Tools

| Tool        | Beschreibung                                          |
|-------------|-------------------------------------------------------|
| `list_dir`  | Verzeichnis auflisten ([D]/[F]/[L] Tags)              |
| `mkdir`     | Verzeichnis anlegen (inkl. Elternverzeichnisse)       |
| `tree`      | Rekursiver Verzeichnisbaum (max depth 6, .git hidden) |

### Such-Tools

| Tool                  | Beschreibung                                              |
|-----------------------|-----------------------------------------------------------|
| `grep_code`           | Regex-Suche rekursiv (max 200 Treffer, datei:zeile:inhalt)|
| `search_code`         | Regex-Suche mit N Kontext-Zeilen (erster Treffer/Datei)  |
| `find_files`          | Glob-Pattern Suche (rekursiv)                             |
| `read_multiple_files` | Bis zu 15 Dateien auf einmal lesen                        |

### Datei-Operationen

| Tool         | Beschreibung                                                    |
|--------------|-----------------------------------------------------------------|
| `move_file`  | Verschieben/Umbenennen (Ziel → Papierkorb wenn vorhanden)      |
| `copy_file`  | Kopieren (Quelle aus allen Roots, Ziel nur Sandbox)            |

### Git-Tools

| Tool            | Beschreibung                                          |
|-----------------|-------------------------------------------------------|
| `git_status`    | git status (Branch + geänderte Dateien)               |
| `git_diff`      | git diff (working tree gegen HEAD)                    |
| `git_log`       | git log (letzte N Commits)                            |
| `git_checkout`  | git checkout (Commit/Branch) — kein remote            |

**Entfernt gegenüber v2.3:** `list_symbols` → wird von `llamaqt-treesitter` besser abgedeckt.

## Sicherheit

- **Symlink-Schutz:** Jede Pfad-Komponente wird auf Symlinks geprüft (verhindert Sandbox-Escape via `/sandbox/evil -> /etc`)
- **Root-Validierung:** Absolute Pfade sind nur erlaubt wenn sie in einer bekannten Root liegen
- **Schreibschutz:** Schreiben nur in writable Roots (Sandbox) — LLAMAQT_SOURCES ist read-only
- **Git-Remote gesperrt:** push/pull/fetch/clone/remote werden in `GitHelper::run()` abgefangen

## Build

```bash
cd mcp-servers/filesystem
mkdir build && cd build
cmake .. && make -j$(nproc)
```

## Laufzeit

```bash
# Standard (Umgebungsvariablen optional)
./llamaqt-filesystem

# Mit eigenen Pfaden
LLAMAQT_SANDBOX=/mnt/projekte \
LLAMAQT_SOURCES=/home/user/src/LlamaQT \
LLAMAQT_TRASH=/tmp/llamatrash \
./llamaqt-filesystem
```

## Änderungen v3.0

- Tool-Logik vollständig in eigene Klassen ausgelagert (Header-only)
- `ToolBase` als gemeinsames Interface (`mcp-servers/common/ToolBase.h`)
- `McpServer` hält `unique_ptr<ToolBase>` — Ownership klar geregelt
- `GitHelper` als eigene Klasse mit `PathPolicy`-Referenz
- Absolute Pfade für Lesezugriff erlaubt (wenn in bekannter Root)
- Trash-Pfad via `LLAMAQT_TRASH` konfigurierbar
- `list_symbols` entfernt (tree-sitter übernimmt das)
- `read_file` unterstützt jetzt optionalen Zeilenbereich (start_line/end_line)
- `copy_file` liest Quelle aus allen Roots (auch read-only Sources)
