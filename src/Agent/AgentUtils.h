#pragma once
// ─── AgentUtils ───────────────────────────────────────────────────────────────
// Rein statische Hilfsfunktionen die von allen Agent-Teilklassen genutzt werden.
//
// Pattern: Utility Namespace — keine Instanz nötig, keine Qt-Abhängigkeit
//          außer QString / QJsonObject.
//
// Warum Namespace statt Klasse mit static-Methoden?
//   - Namespace kann nicht instanziiert werden (klarer als "= delete")
//   - Kein Header-Overhead für Qt-Metasystem (kein Q_OBJECT)
//   - Funktionen können in .cpp ausgelagert werden (kürzere Compile-Zeit)
//
// Analogie AVR: wie eine utils.h mit Inline-Hilfsfunktionen die jede
// .c-Datei einbinden kann — kein globaler Zustand, keine ISR-Konflikte.

#include <QString>
#include <QJsonObject>

namespace AgentUtils {

// ─── repairJson ───────────────────────────────────────────────────────────────
// Versucht kaputtes JSON zu reparieren.
//
// Strategie (3 Ebenen):
//   1. Fehlende schließende Klammern ergänzen (öffnende - schließende zählen)
//   2. Trailing-Komma vor ] oder } entfernen
//   3. Einfache Anführungszeichen → doppelte ersetzen
//
// Gibt leeren String zurück wenn Reparatur fehlschlägt.
// Aufrufer muss mit leerem Ergebnis umgehen können.
QString repairJson(const QString &broken);

// ─── toolCallKey ──────────────────────────────────────────────────────────────
// Erzeugt einen eindeutigen Schlüssel aus Tool-Name + Argumenten.
// Wird für Deadlock-Erkennung genutzt: gleicher Key N-mal → Eskalation.
//
// Format: "toolName|{\"arg1\":val1}|{\"arg2\":val2}"
// Argumente alphabetisch sortiert → deterministisch.
QString toolCallKey(const QString &toolName, const QJsonObject &args);

// ─── deadlockEscalationPrompt ─────────────────────────────────────────────────
// Gibt den passenden Eskalations-Text für den aktuellen Fehlerzähler zurück.
//
// Stufen:
//   count < DEADLOCK_REDIRECT  → Warnung: Argumente prüfen
//   count < DEADLOCK_ABORT     → Umleitung: anderen Weg suchen
//   count >= DEADLOCK_ABORT    → Abbruch-Ankündigung
static constexpr int DEADLOCK_WARN     = 3;
static constexpr int DEADLOCK_REDIRECT = 5;
static constexpr int DEADLOCK_ABORT    = 7;

QString deadlockEscalationPrompt(const QString &toolName, int count);

// ─── computeDiffHtml ─────────────────────────────────────────────────────────
// Berechnet einen farbigen HTML-Diff zwischen zwei Texten (LCS-Algorithmus).
//
// Algorithmus: Longest Common Subsequence (klassischer DP-Ansatz).
// Komplexität: O(m*n) — nur für m,n <= 300 Zeilen sinnvoll.
// Bei größeren Dateien: Fallback auf Metadaten-Ausgabe.
//
// Ausgabe: HTML mit <span>-Tags für Added/Removed/Equal Zeilen.
// Kontext: CONTEXT=2 Zeilen um jede Änderung herum (wie git diff -U2).
QString computeDiffHtml(const QString &before, const QString &after,
                        const QString &filename);

} // namespace AgentUtils
