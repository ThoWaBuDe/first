#pragma once
// ─── CodeAssembler ────────────────────────────────────────────────────────────
// Assembliert den TaskTree zu echten Dateien auf Disk.
//
// Prinzip:
//   Jeder H2-Node dessen title ein Dateiname ist → eine Datei.
//   H2->result        = Datei-Kopf (includes, Klassen-Deklaration, ...)
//   H3-Kinder->result = Methoden, sortiert nach order/insertionIdx
//   Assemblierte Datei = H2->result + concat(H3-Kinder)
//
// Konfigurierbar:
//   onlyDone = true  → nur Nodes mit status==Done assemblieren
//   onlyDone = false → alle Nodes mit nicht-leerem result
//
// Zielverzeichnis: AppConfig::executeSandboxProject()
//   Vollständiger Pfad: ~/llamatools/[executeSandboxProject]/
//   Unterverzeichnisse werden automatisch angelegt (QDir::mkpath).
//
// Bestehende Dateien werden immer überschrieben (kein Backup).
//
// Analogie AVR: wie ein Linker der Object-Files zu einem Binary zusammenfügt —
// jede Übersetzungseinheit (H2-Node) liefert ihren Beitrag,
// der Assembler fügt sie zur richtigen Stelle zusammen.

#include "TaskTree.h"
#include <QString>
#include <QStringList>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>

class CodeAssembler
{
public:
    // ── Ergebnis eines Assembly-Laufs ─────────────────────────────────────────
    struct AssemblyResult {
        int     filesWritten  = 0;   // erfolgreich geschriebene Dateien
        int     filesSkipped  = 0;   // übersprungen (kein result, falscher Status)
        int     filesFailed   = 0;   // Schreibfehler
        QStringList writtenPaths;    // absolute Pfade der geschriebenen Dateien
        QStringList errors;          // Fehlermeldungen
        bool    success() const { return filesFailed == 0; }
    };

    explicit CodeAssembler() = default;

    // ── Haupt-Methode ─────────────────────────────────────────────────────────
    // Traversiert den TaskTree und assembliert alle H2-Nodes.
    //
    // targetDir:  absoluter Pfad zum Zielverzeichnis
    // tree:       der TaskTree (read-only)
    // onlyDone:   true = nur Done-Nodes, false = alle mit result
    AssemblyResult assemble(const QString  &targetDir,
                            const TaskTree &tree,
                            bool            onlyDone = true)
    {
        AssemblyResult result;

        if (targetDir.isEmpty()) {
            result.errors << "Kein Zielverzeichnis angegeben.";
            result.filesFailed = 1;
            return result;
        }

        // Zielverzeichnis anlegen falls nicht vorhanden
        QDir dir(targetDir);
        if (!dir.exists()) {
            if (!QDir().mkpath(targetDir)) {
                result.errors << QString("Konnte Verzeichnis nicht anlegen: %1")
                                 .arg(targetDir);
                result.filesFailed = 1;
                return result;
            }
        }

        // Alle H2-Nodes suchen und assemblieren
        tree.traverse([&](const TaskNode *node) {
            if (node->level != static_cast<int>(TaskLevel::Class))
                return;  // nur H2-Nodes

            assembleNode(node, targetDir, onlyDone, result);
        });

        return result;
    }

    // ── Einzelnen H2-Node assemblieren ───────────────────────────────────────
    // Kann auch direkt aufgerufen werden (z.B. nach jedem abgeschlossenen Node).
    void assembleNode(const TaskNode  *node,
                      const QString   &targetDir,
                      bool             onlyDone,
                      AssemblyResult  &result)
    {
        if (!node) return;

        // Status-Filter
        if (onlyDone && node->status != TaskStatus::Done) {
            result.filesSkipped++;
            return;
        }

        // Kein Code vorhanden — auch Kinder prüfen
        QString assembled = buildFileContent(node, onlyDone);
        if (assembled.isEmpty()) {
            result.filesSkipped++;
            return;
        }

        // Zieldatei-Pfad bestimmen
        // title = Dateiname (z.B. "GameState.cpp" oder "src/GameState.cpp")
        // Falls title einen Unterverzeichnis-Pfad enthält → anlegen.
        QString relPath  = node->title.trimmed();
        QString fullPath = QDir(targetDir).filePath(relPath);
        QFileInfo fi(fullPath);

        // Unterverzeichnis anlegen
        if (!fi.absoluteDir().exists()) {
            if (!QDir().mkpath(fi.absolutePath())) {
                result.errors << QString("Konnte Unterverzeichnis nicht anlegen: %1")
                                 .arg(fi.absolutePath());
                result.filesFailed++;
                return;
            }
        }

        // Datei schreiben (immer überschreiben)
        // Analogie AVR: wie Flash-Programmierung — erst löschen, dann schreiben.
        QFile file(fullPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            result.errors << QString("Konnte Datei nicht schreiben: %1 — %2")
                             .arg(fullPath, file.errorString());
            result.filesFailed++;
            return;
        }

        QTextStream out(&file);
        out << assembled;
        file.close();

        result.filesWritten++;
        result.writtenPaths << fullPath;
    }

private:
    // ── Datei-Inhalt aus H2-Node + H3-Kindern zusammenbauen ──────────────────
    // Reihenfolge:
    //   1. H2->result (Datei-Kopf, includes, Klassen-Boilerplate)
    //   2. H3-Kinder sortiert nach order, dann insertionIdx
    //
    // Wenn H2->result leer aber H3-Kinder haben result → trotzdem assemblieren.
    // Wenn alles leer → leerer String (wird in assembleNode übersprungen).
    //
    // Trennzeile zwischen H2 und H3:
    //   Wenn H2->result nicht mit Newline endet → eine Leerzeile einfügen.
    //   Das verhindert dass der erste H3-Block direkt an den H2-Block klebt.
    QString buildFileContent(const TaskNode *node, bool onlyDone) const
    {
        QString content;

        // H2-Teil (Datei-Kopf)
        if (!node->result.isEmpty()) {
            if (shouldInclude(node, onlyDone))
                content = node->result;
        }

        // H3-Kinder sammeln und sortieren
        // Kinder sind bereits in sortChildren() nach order/insertionIdx sortiert —
        // wir vertrauen der bestehenden Reihenfolge im children-Vector.
        bool hasChildContent = false;
        QString childContent;

        for (const TaskNode *child : node->children) {
            if (child->level != static_cast<int>(TaskLevel::Impl))
                continue;  // nur H3-Nodes

            if (!shouldInclude(child, onlyDone))
                continue;

            if (child->result.isEmpty())
                continue;

            hasChildContent = true;

            // Trennzeile zwischen Methoden
            if (!childContent.isEmpty())
                childContent += "\n";

            childContent += child->result;

            // Sicherstellen dass jede Methode mit Newline endet
            if (!childContent.endsWith('\n'))
                childContent += '\n';
        }

        if (!hasChildContent && content.isEmpty())
            return {};  // nichts zu schreiben

        // H2 + H3 zusammenfügen
        if (!content.isEmpty() && hasChildContent) {
            // Leerzeile zwischen H2-Kopf und H3-Methoden einfügen
            if (!content.endsWith('\n'))
                content += '\n';
            content += '\n';  // eine Leerzeile als Trenner
        }

        content += childContent;
        return content;
    }

    // Prüft ob ein Node in die Assembly aufgenommen werden soll
    bool shouldInclude(const TaskNode *node, bool onlyDone) const
    {
        if (onlyDone)
            return node->status == TaskStatus::Done;
        return !node->result.isEmpty();
    }
};
