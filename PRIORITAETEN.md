# LlamaQt — Verbesserungsliste & Prioritäten

> Stand: Mai 2026 — nach Node-Konzept Einfrierung
> Node-bezogene Punkte (A-O, T) entfernt — nicht mehr relevant.

---

## Legende

| Symbol | Bedeutung |
|--------|-----------|
| 🔴 | Kritisch — blockiert Funktion |
| 🟠 | Hoch — wichtige Verbesserung |
| 🟡 | Mittel — sinnvoll aber nicht dringend |
| 🟢 | Niedrig — nice-to-have |
| Aufwand S/M/L/XL | klein (<2h) / mittel (2-8h) / groß (1-2 Tage) / sehr groß (>2 Tage) |

---

## GRUPPE 1: Nächste Hauptrichtung

### Symbol-basiertes Editing
**Ziel:** LlamaQt holt eine einzelne Funktion/Methode, LLM modifiziert sie,
LlamaQt schreibt sie zurück. Ein Schritt, isolierter Kontext.

```
Benutzer wählt Symbol (z.B. via EditorDock oder /edit Kommando)
  → LlamaQt: get_function_body via tree-sitter MCP
  → LLM: sieht NUR diese Funktion + Aufgabe
  → LLM: schreibt neue Version
  → LlamaQt: replace_symbol → fertig
  → Git: auto-commit als Undo-Punkt
```

Tools die bereits vorhanden sind:
- `get_function_body`, `replace_symbol` — tree-sitter MCP ✓
- `find_references`, `rename_symbol` — clang MCP ✓
- `type_errors` — Validierung ✓
- `git_*` — Undo ✓

**Aufwand:** M | **Priorität:** 🔴

---

## GRUPPE 2: Infrastruktur

### Q — generateDelta() aktivieren
**Problem:** Jede Generation leert den KV-Cache — gesamter Prompt wird
neu encodiert. Bei langen Kontexten teuer.

**Lösung:** n_past tracken, nur neue Tokens encoden.
Vorbereitung in LlamaWorker bereits vorhanden (generateDelta() Stub).

**Aufwand:** M | **Priorität:** 🟠

---

### R — KV-Cache Rollback nach Tool-Fehler
**Lösung:** `llama_memory_seq_rm()` nach fehlgeschlagenem Tool-Call.
Setzt Cache auf Zustand vor dem fehlerhaften Token zurück.

**Hinweis:** In aktueller llama.cpp Version heißt die Funktion
`llama_memory_seq_rm` (nicht `llama_kv_cache_seq_rm`).

**Aufwand:** M | **Priorität:** 🟡

---

### Incus Container Integration
Siehe INCUS_TODO.md für Details.

Phase 1 (minimal):
- McpManager: `addServer()` mit optionalem `incusContainer`-Parameter
- Test: stdio-Proxy über `incus exec`

**Aufwand:** S (Phase 1) | **Priorität:** 🟡

---

## GRUPPE 3: Nice-to-have

### Persistent Conversation Memory
Gesprächs-Kontext zwischen Sessions erhalten.
AGENT.md als Projektgedächtnis ist ein erster Schritt.

**Aufwand:** M | **Priorität:** 🟢

---

### Multiple Modell-Support
Kleines Modell für Tool-Calls, großes für komplexe Aufgaben.
Setzt generateDelta() voraus für effizienten Switch.

**Aufwand:** L | **Priorität:** 🟢

---

### XTC/DRY Sampler-Optionen
Konfigurierbar in AppConfig + ConfigDialog.

**Aufwand:** S | **Priorität:** 🟢

---

### maxNewTokens in AppConfig
Chat: 8192, Tool: 2048 — aktuell hardcoded in LlamaWorker.

**Aufwand:** S | **Priorität:** 🟢

---

## Zusammenfassung

| # | Punkt | Aufwand | Prio |
|---|-------|---------|------|
| 1 | Symbol-basiertes Editing | M | 🔴 |
| 2 | generateDelta() | M | 🟠 |
| 3 | Incus Phase 1 | S | 🟡 |
| 4 | KV-Cache Rollback | M | 🟡 |
| 5 | Persistent Memory | M | 🟢 |
| 6 | Multiple Modelle | L | 🟢 |
| 7 | XTC/DRY Sampler | S | 🟢 |
| 8 | maxNewTokens Config | S | 🟢 |
