# LlamaQt — Verbesserungsliste & Prioritäten

> Stand: Session April 2026  
> Basis: Gespräch + Code-Review Agent.cpp / TaskTree / CodeAssembler

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

## GRUPPE 1: Kritische Bugs (sofort)

### A — advanceExecute() toter Code / Execute startet nie
**Problem:** Nach `emit appendChat(...)` folgt versehentlich der "alle Nodes fertig"-Block.
`startGeneration()` liegt hinter einem `return false` — wird **nie erreicht**.
Execute-Modus startet also, tut aber nichts.

```cpp
// FALSCH (aktuell):
emit appendChat(...);
emit appendTools("<b>Execute: alle Nodes abgearbeitet!</b>"); // FALSCH
emit executeFinished();   // FALSCH
return false;             // toter Pfad
startGeneration(...);     // nie erreicht
return true;
```

**Fix:** Toten Block entfernen, `startGeneration()` korrekt aufrufen.  
**Aufwand:** S | **Priorität:** 🔴

---

### B — buildExecutePrompt() falscher "aktuell"-Marker
**Problem:** `verticalContext()` markiert H2 als "(aktuell)" statt H3.
Das Modell denkt es soll die Datei schreiben, nicht die Methode.  
**Fix:** Patch in Agent_bugfix.cpp vorhanden, einbauen.  
**Aufwand:** S | **Priorität:** 🔴

---

### C — onGenerationDone() doppelte Thoughts-Update-Logik
**Problem:** `m_updatingThoughts`-Block existiert **zweimal** in `onGenerationDone()` —
einmal ganz oben (korrekt mit `parseFromLlmOutput()`), einmal weiter unten
(veraltet, nur splitLines). Der zweite Block wird durch `if (m_mode == Execute)`
erst danach geprüft — je nach Reihenfolge kann der falsche Block feuern.  
**Fix:** Zweiten veralteten Block entfernen.  
**Aufwand:** S | **Priorität:** 🔴

---

### D — dependsOn Substring-Match zu aggressiv
**Problem:** `"Timer"` matcht `"AnimationTimer"` und `"TimerWidget"`.
Falsche Abhängigkeiten werden eingetragen.  
**Fix:** Substring nur als Fallback wenn exakter Match fehlschlägt,
zusätzlich Wortgrenzen prüfen.  
**Aufwand:** S | **Priorität:** 🔴

---

## GRUPPE 2: Architektur (nächste Session)

### E — Agent.cpp God Object aufteilen
**Problem:** Agent.cpp ist >1200 Zeilen, mischt Chat/Plan/Execute/Assemble.
Schwer zu lesen, schwer zu testen, schwer zu erweitern.

**Lösung — Komposition mit friend:**
```
Agent.h/.cpp         — Koordination, onUserMessage, onGenerationDone
AgentChat.h/.cpp     — handleToolCall, filterToken, summarizeContext
AgentPlan.h/.cpp     — startPlan, buildPlannerSystemPrompt,
                        handlePlanToolCall, handlePlanJson, parsePlanNode
AgentExecute.h/.cpp  — startExecute, advanceExecute, buildExecutePrompt,
                        buildExecuteSystemPrompt, handleExecuteCode,
                        handleExecuteToolCall, updateThoughts
AgentAssemble.h/.cpp — assembleProject
AgentHelpers.h/.cpp  — repairJson, toolCallKey, deadlockEscalationPrompt,
                        computeDiffHtml (Namespace AgentUtils)
```

`Agent` deklariert alle Teilklassen als `friend`.
Teilklassen erhalten Referenz `Agent &m_agent` im Konstruktor.  
**Aufwand:** L | **Priorität:** 🟠

---

### F — Planner-Prompt: Symbol-Awareness
**Problem:** Planner denkt in Dateien/Methoden, nicht in Symbolen.
Gleiche Funktion kann in zwei Nodes landen (z.B. Timer in main.cpp UND timer.cpp).

**Lösung:** Planner-Prompt um Regel erweitern:
> "Jedes Symbol (Klasse, Methode, Variable) darf nur in EINEM Node als
> `result` existieren. Alle anderen Nodes die es brauchen: `dependsOn`."

**Aufwand:** S | **Priorität:** 🟠

---

### G — Iterativer Plan-Aufbau statt einem großen JSON
**Problem:** Komplettes H0-H2 JSON auf einmal überfordert das 9B-Modell.
Duplikate entstehen weil das Modell den Überblick verliert.

**Lösung:** Modell baut Graph iterativ mit Tools:
```
create_node, edit_node, delete_node, set_depends_on, plan_done
```
Analog zu PlanOptimizer — aber für den initialen Plan.
Jeder Tool-Call = ein Node. Weniger Komplexität pro Aufruf.

**Aufwand:** M | **Priorität:** 🟠

---

### H — Symbol-Feld in TaskNode
**Problem:** Kein eindeutiger Symbol-Schlüssel → Duplikat-Erkennung nur per Titel-String.

**Lösung:** Neues Feld `QString symbol` in TaskNode:
```
"Timer::start()"   — eindeutig im Scope
"GameState::m_board" — Member
```
Optimizer prüft: existiert dieses Symbol schon? → `dependsOn` statt Reimplementierung.

**Aufwand:** M | **Priorität:** 🟠

---

## GRUPPE 3: Execute-Qualität

### I — Sampler-Profile pro Modus
**Problem:** Alle Modi (Plan, Execute, Chat, Merge) nutzen Chat-Sampler oder Tool-Sampler.
Aber die optimalen Parameter sind verschieden:

| Modus | Empfehlung |
|-------|-----------|
| Plan | Temp 0.7, kreativ |
| Execute (Methode codieren) | Temp 0.1-0.3, deterministisch |
| Optimize | Temp 0.1, Tool-Sampler |
| Merge/Assembly | Temp 0.2 |

**Lösung:** `SamplerProfile` um `Execute` und `Merge` erweitern.  
**Aufwand:** S | **Priorität:** 🟡

---

### J — Feedback-Schleife Execute → Optimizer
**Problem:** Execute ist Single-Pass. Fehler im generierten Code bleiben bis Assembly.

**Lösung:**
```
Node fertig → compile (check_syntax MCP) → Fehler?
  → zurück zum Optimizer mit Fehlermeldung
  → Optimizer repariert Node
  → Execute nochmal
```
`check_syntax` ist bereits in der Execute-Whitelist — der Weg ist frei.  
**Aufwand:** M | **Priorität:** 🟡

---

### K — LLM-basierter Semantic Merge
**Problem:** CodeAssembler concateniert Text — Duplikate, fehlende Klammern,
widersprüchliche Includes.

**Lösung:** Nach Assembly pro H2-Datei ein LLM-Pass:
```
Prompt: "Hier sind alle Fragmente für GameState.cpp.
         Aufgabe: GameState.h (Interface), diese Fragmente.
         Integriere zu einer syntaktisch korrekten .cpp Datei."
```
Das Modell sieht den vollständigen Kontext einer Datei.
Kleines Modell muss nur noch "zusammenfügen", nicht "erfinden".  
**Aufwand:** M | **Priorität:** 🟡

---

### L — Clang-AST Rückkopplung nach Execute
**Problem:** Nach Execute keine semantische Prüfung ob Symbole wirklich eindeutig sind.

**Lösung:** `llamaqt-clang` MCP nach Assembly nutzen:
```
type_errors → Fehler an Optimizer zurückmelden
find_references → doppelte Definitionen finden
```
Clang kennt Scope — `Timer::start` ≠ `Clock::start`.  
**Aufwand:** L | **Priorität:** 🟡

---

### M — Pre-Execute Dry-Run (Struktur-Validierung)
**Problem:** Strukturfehler (fehlende Header, falsche CMake-Targets) werden erst
beim echten Build sichtbar — nach viel Rechenzeit.

**Lösung:** Aus Signaturen + dependsOn leere Stubs generieren:
```cpp
// Nur Klassenkopf + leere Methodenrümpfe
class GameState {
    bool makeMove(int row, int col) { return false; }
};
```
`cmake_build` + `check_syntax` prüfen die Struktur.
Fehler → Optimizer bevor Execute startet.  
**Aufwand:** L | **Priorität:** 🟡

---

### N — Validierungs-Node (H4) nach jedem H3
**Problem:** Das Modell prüft seinen eigenen Output nicht.

**Lösung:** Nach jedem H3-Node automatisch einen H4-Validierungs-Node erzeugen:
```
Prompt: "Du hast makeMove() implementiert.
         Prüfe: Ist der Code vollständig? Korrekte Signatur?
         Gibt es offensichtliche Fehler?
         Falls ja: beschreibe den Fehler für den Optimizer."
```
Node-Status: Done (ok) oder Failed (Optimizer greift ein).  
**Aufwand:** M | **Priorität:** 🟡

---

### O — Prompt-Schichtung: expliziter Skelett-Hinweis
**Problem:** Modell schreibt vollständige Dateien statt Methoden-Fragmente,
weil der Prompt nicht explizit genug ist.

**Lösung — Ansatz C (Hybrid):**
- Code-Generator erzeugt Klassen-Gerüst (.h) deterministisch aus Symbol-Plan
- LLM codiert NUR Methodenkörper
- Prompt: "Du schreibst NUR den Körper von `makeMove()`.
  Signatur ist bereits vorgegeben. Keine #include, keine Klassen-Deklaration."

**Aufwand:** M | **Priorität:** 🟡

---

## GRUPPE 4: Infrastruktur

### P — MCP-Server für Node-Manipulation (optional)
**Frage:** Integriert (aktuell) vs. MCP-Server für TaskTree-Operationen.

**Aktuell:** PlanOptimizer direkt im GUI-Thread auf TaskTree — schnell, einfach.
**MCP-Option:** Externe Datenquelle möglich (CSV, andere DB), aber Overhead.

**Empfehlung:** Integriert lassen bis ein konkreter Bedarf für externe Datenquellen entsteht.  
**Aufwand:** XL | **Priorität:** 🟢

---

### Q — generateDelta() aktivieren (n_past-Tracking)
**Problem:** Jede Generation leert den KV-Cache — gesamter Prompt wird
jedes Mal neu durch `llama_decode()`. Bei langen Kontexten teuer.

**Lösung:** n_past tracken, nur neue Tokens encoden.
Vorbereitung ist in LlamaWorker bereits vorhanden.  
**Aufwand:** M | **Priorität:** 🟢

---

### R — KV-Cache Rollback nach Tool-Fehler
**Lösung:** `llama_kv_cache_seq_rm()` nach fehlgeschlagenem Tool-Call.
Setzt den Cache auf den Zustand vor dem fehlerhaften Token zurück.  
**Aufwand:** M | **Priorität:** 🟢

---

### S — MCP-Server Auto-Restart bei Absturz
**Problem:** Wenn ein MCP-Server crasht, sind alle seine Tools für immer weg.
`serverDied` Signal existiert, aber kein Restart-Handler.  
**Aufwand:** S | **Priorität:** 🟢

---

### T — NodeGraphView Layout-Verbesserung
**Problem:** Einfaches Spalten-Layout — Nodes überlappen bei großen Plänen.
**Lösung:** Sugiyama-Algorithmus (hierarchisches Layout) oder
einfacher Baum-Layout-Algorithmus.  
**Aufwand:** L | **Priorität:** 🟢

---

## Zusammenfassung nach Priorität

| # | Punkt | Gruppe | Aufwand | Prio |
|---|-------|--------|---------|------|
| 1 | A — advanceExecute() toter Code | Bug | S | 🔴 |
| 2 | B — falscher aktuell-Marker | Bug | S | 🔴 |
| 3 | C — doppelter Thoughts-Block | Bug | S | 🔴 |
| 4 | D — dependsOn Substring-Match | Bug | S | 🔴 |
| 5 | E — Agent God Object aufteilen | Architektur | L | 🟠 |
| 6 | F — Planner Symbol-Awareness | Prompting | S | 🟠 |
| 7 | G — Iterativer Plan-Aufbau | Architektur | M | 🟠 |
| 8 | H — Symbol-Feld in TaskNode | Datenmodell | M | 🟠 |
| 9 | I — Sampler-Profile pro Modus | Execute | S | 🟡 |
| 10 | J — Feedback Execute→Optimizer | Execute | M | 🟡 |
| 11 | K — Semantic Merge LLM | Assembly | M | 🟡 |
| 12 | L — Clang-AST Rückkopplung | Assembly | L | 🟡 |
| 13 | M — Pre-Execute Dry-Run | Execute | L | 🟡 |
| 14 | N — Validierungs-Node H4 | Execute | M | 🟡 |
| 15 | O — Skelett-Hinweis im Prompt | Prompting | M | 🟡 |
| 16 | P — MCP für Node-Manipulation | Infra | XL | 🟢 |
| 17 | Q — generateDelta() aktivieren | Infra | M | 🟢 |
| 18 | R — KV-Cache Rollback | Infra | M | 🟢 |
| 19 | S — MCP Auto-Restart | Infra | S | 🟢 |
| 20 | T — NodeGraphView Layout | UI | L | 🟢 |

---

## Empfohlene Reihenfolge (nächste 3 Sessions)

**Session 1 (diese):** A + B + C + D + E (Bugs + Refactoring)  
**Session 2:** F + G + H (Planner verbessern, Symbol-Awareness)  
**Session 3:** I + J + N (Execute-Qualität, Validierung)  
