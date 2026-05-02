# LlamaQt — Incus Container Integration

> Stand: Mai 2026 — noch nicht implementiert, Phase 1 ausstehend
> Ziel: LLM-Tools laufen in einem isolierten Incus-Container.
> Host-System und LlamaQt bleiben unberührt, auch wenn das LLM "zerbombt".

---

## Architektur-Entscheidungen (festgelegt)

### Transport: Weg B — stdio-Proxy über `incus exec`

```
LlamaQt (Host)
  └── McpClient (stdio, wie bisher)
        └── QProcess: "incus exec <container> -- /opt/llamaqt/<mcp-server>"
```

**Begründung:**
- Null Aufwand im McpClient — nur der gestartete Prozess ändert sich
- Kein Cross-Compile nötig: MCP-Server-Binary läuft nativ im Container
- Funktioniert auf beliebigen Architekturen (RPI, x86, ARM)
- Remote-Maschinen per `incus remote` erreichbar — ohne SSH-Tunnel

### Container-Administration: LlamaQt, NICHT das LLM

Das LLM hat **keinen Zugriff** auf Incus-Administration.

**Begründung:**
- Incus ist Infrastruktur, keine Werkzeug-Ebene
- Ein LLM mit Container-Admin-Rechten könnte Volumes mounten
  und damit die Isolation umgehen (`incus config device add` mit
  falschem Pfad → Host-Home im Container sichtbar)
- Dasselbe Prinzip wie PathPolicy: das LLM entscheidet nicht
  wo seine Sandbox ist, sondern arbeitet darin
- LlamaQt entscheidet: welcher Container, wann Snapshot,
  wann Restore — das LLM merkt davon nichts

### MCP-Server laufen als normaler User

Kein root, kein sudo. Die Container-Isolation ist die Sicherheitsschicht,
nicht Unix-Permissions innerhalb des Containers.

---

## Neue Komponenten

### 1. IncusManager (in LlamaQt, GUI-Thread)

Zuständig für die gesamte Container-Lebenszeit.
Das LLM wird davon nicht informiert.

```
IncusManager
  ├── listContainers()       → verfügbare Container anzeigen
  ├── startContainer()       → Container starten
  ├── stopContainer()        → Container stoppen
  ├── snapshotBefore()       → Snapshot vor Execute-Lauf
  ├── restoreSnapshot()      → Zurücksetzen nach Katastrophe
  └── execBinary(path)       → gibt "incus exec <c> -- <path>" zurück
                               → wird von McpManager als Prozess gestartet
```

REST API von Incus direkt aus Qt Network — kein CLI-Wrapper,
kein separater Prozess. Incus REST ist vollständig dokumentiert
und stabil.

**UI-Integration:**
- Neues Dock-Widget: Container-Status, Start/Stop, Snapshot/Restore
- `incus console` als Terminal-Emulator im Dock (QProcess + ANSI)
- Automatischer Snapshot vor jedem `/execute`-Lauf (konfigurierbar)

### 2. Container-MCP-Server (neues Binary, läuft im Container)

Ersetzt und erweitert die bestehenden Host-MCP-Server.
Erkennt beim Start ob er im Container läuft — verweigert sonst den Start.

**Container-Erkennung:**
```cpp
bool isInContainer() {
    return QFile::exists("/run/.containerenv") ||    // Incus/Podman
           QFile::exists("/.dockerenv") ||            // Docker
           QFile::exists("/run/systemd/container");   // systemd-nspawn
}
```

**Tools die nur im Container erlaubt sind:**

| Tool | Beschreibung |
|------|--------------|
| `bash_exec` | Beliebige Shell-Kommandos ausführen |
| `python_exec` | Python-Code ausführen (inkl. pip install) |
| `pip_install` | Pakete installieren |
| `apt_install` | System-Pakete installieren (sudo im Container ok) |
| `web_fetch` | HTTP/HTTPS ohne API-Key (curl/requests) |
| `web_search` | Scraping oder Tavily (optional) |
| `cmake_build` | Build direkt im Container |
| `run_binary` | Kompiliertes Binary starten und Ausgabe streamen |

**Bestehende Tools bleiben erhalten:**
- `read_file`, `write_file`, `str_replace` etc. — ohne PathPolicy-Einschränkung
  (der Container *ist* die Sandbox)
- `git_*` — funktioniert wie bisher
- `check_syntax`, `list_symbols` etc. — tree-sitter lokal im Container

### 3. McpManager-Erweiterung

```cpp
// Neuer Parameter: optional über incus exec proxen
void addServer(const QString &binary,
               const QStringList &args = {},
               const QString &incusContainer = {});  // NEU

// Intern:
// wenn incusContainer nicht leer:
//   QProcess startet: "incus exec <container> -- <binary> <args>"
// sonst: bisheriges Verhalten (lokaler Prozess)
```

Minimale Änderung — ein Parameter, ein if-Block. Kein neues Protokoll.

---

## Sicherheits-Modell

```
┌─────────────────────────────────────────────────────┐
│  Host                                               │
│  ┌─────────────────┐                                │
│  │   LlamaQt       │  IncusManager (REST)           │
│  │   GUI-Thread    │──────────────────────────────► │ Incus Daemon
│  │                 │                                │
│  │   McpManager   ──── incus exec container ──────► │
│  └─────────────────┘                                │
│                           │                         │
│              ┌────────────▼────────────┐            │
│              │  Incus Container        │            │
│              │  (isoliert)             │            │
│              │                         │            │
│              │  llamaqt-container-mcp  │            │
│              │  ├── bash_exec          │            │
│              │  ├── python_exec        │            │
│              │  ├── apt_install        │            │
│              │  ├── web_fetch          │            │
│              │  └── cmake_build        │            │
│              │                         │            │
│              │  läuft als: user        │            │
│              │  kein Zugriff auf Host  │            │
│              └─────────────────────────┘            │
└─────────────────────────────────────────────────────┘
```

**Was das LLM KANN (im Container):**
- Beliebige Dateien lesen/schreiben (Container-Filesystem)
- Shell-Kommandos, Python, pip, apt
- Web-Zugriff (HTTP/HTTPS)
- Code kompilieren und ausführen
- Git-Repos klonen und committen

**Was das LLM NICHT KANN:**
- Host-Dateisystem sehen (kein Volume gemountet)
- Incus-Container administrieren
- Neue Container erstellen oder Volumes mounten
- LlamaQt-Konfiguration lesen (liegt auf dem Host)
- Kreditkartennummern finden (nicht im Container)

**Snapshot-Strategie:**
- Automatisch vor jedem `/execute`-Lauf
- Manuell über IncusManager-Dock jederzeit
- Restore per Knopfdruck — Container zurück auf sauberen Stand
- Kein git-Undo mehr nötig für kritische Operationen

---

## Implementierungs-Reihenfolge

### Phase 1 — Minimaler Proof of Concept (klein, sofort testbar)
- McpManager: `addServer()` mit optionalem `incusContainer`-Parameter
- Bestehenden `llamaqt-filesystem` Server im Container starten
- Testen: funktioniert stdio-Proxy über `incus exec`?

### Phase 2 — Container-MCP-Server
- `llamaqt-container` neues Binary (Qt6, wie die anderen)
- `bash_exec` und `python_exec` als erste Tools
- Container-Erkennung beim Start
- `pip_install`, `apt_install`, `web_fetch`

### Phase 3 — IncusManager in LlamaQt
- REST-Client für Incus API (Qt Network)
- Dock-Widget: Container-Liste, Start/Stop
- Snapshot vor Execute, Restore-Button
- `incus console` als Terminal im Dock (optional)

### Phase 4 — Produktionsreif
- Container-Initialisierung aus LlamaQt heraus (neues Image aufsetzen)
- Mehrere Container parallel (z.B. einer pro Projekt)
- Remote-Container über `incus remote` (RPI, andere Maschinen)
- Automatische MCP-Server-Installation im frischen Container

---

## Offene Fragen

- Welche Incus-Image-Basis? (Ubuntu 24.04 LTS empfohlen — gleiche Basis wie Trixie-Pakete)
- Qt6 im Container statisch oder als Paket? (Paket einfacher, statisch portabler)
- Tavily API-Key im Container? (Umgebungsvariable via `incus exec --env`)
- Terminal-Emulator für `incus console`: eigene Implementierung oder externes Widget?
