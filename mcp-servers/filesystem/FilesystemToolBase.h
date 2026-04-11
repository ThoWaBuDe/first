#pragma once
// ─── FilesystemToolBase ───────────────────────────────────────────────────────
// Zwischenschicht zwischen ToolBase und den konkreten filesystem-Tools.
//
// Pattern: Template Method (Schablonenmethode)
//   FilesystemToolBase implementiert die gemeinsame Infrastruktur:
//   - Konstruktor-Injektion von PathPolicy und GitHelper
//   - Hilfsmethoden resolve(), prop(), requiredArray()
//   Die konkreten Tools (ReadFileTool etc.) erben davon und implementieren
//   nur noch name(), description(), properties(), required(), execute().
//
// Warum eine Zwischenschicht?
//   Ohne sie müsste jede der ~20 Tool-Klassen denselben Konstruktor und
//   dieselben Hilfsmethoden wiederholen — DRY-Verletzung.
//   Mit der Zwischenschicht: gemeinsamer Code einmal, konkrete Logik je Tool.
//
// Analogie (AVR):
//   FilesystemToolBase ist wie ein BSP (Board Support Package) das UART-Init,
//   GPIO-Zugriff etc. kapselt. Die konkreten "Treiber" nutzen das BSP,
//   ohne die Hardware-Details zu kennen.

#include "../common/ToolBase.h"    // ToolBase + ToolResult (via ToolResult.h)
#include "../common/PathPolicy.h"
#include "GitHelper.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QString>

// Größenbegrenzungen — zentral hier, nicht in jeder Tool-Klasse wiederholen.
static constexpr int MAX_READ_CHARS     = 8192;
static constexpr int MAX_WRITE_CHARS    = 16384;
static constexpr int MAX_GREP_HITS      = 200;
static constexpr int MAX_TREE_DEPTH     = 6;
static constexpr int MAX_MULTIPLE_FILES = 15;

class FilesystemToolBase : public ToolBase
{
public:
    // Konstruktor-Injektion: Policy und Git werden als Zeiger übergeben.
    // Zeiger statt Referenz weil Tool-Objekte in unique_ptr leben und
    // verschoben werden können — Referenzen darf man nicht verschieben.
    // Die Objekte selbst (policy, git) leben in main() auf dem Stack und
    // überleben den Server garantiert.
    FilesystemToolBase(PathPolicy *policy, GitHelper *git)
        : m_policy(policy), m_git(git)
    {}

protected:
    PathPolicy *m_policy;
    GitHelper  *m_git;

    // ── Hilfsmethoden für Schema-Konstruktion ─────────────────────────────────

    // Erzeugt ein JSON-Schema-Property-Objekt.
    // Verwendung: properties() überschreiben und prop() aufrufen.
    static QJsonObject prop(const QString &type, const QString &desc)
    {
        return QJsonObject{{"type", type}, {"description", desc}};
    }

    // Erzeugt ein QJsonArray aus einer Initializer-Liste von Strings.
    // Verwendung: required() überschreiben → return req({"path","content"});
    static QJsonArray req(std::initializer_list<const char*> fields)
    {
        QJsonArray arr;
        for (const char *f : fields) arr.append(f);
        return arr;
    }

    // ── Pfad-Auflösung ────────────────────────────────────────────────────────

    // Löst einen Pfad für Lesezugriff auf.
    // Gibt {"", Fehlermeldung} zurück wenn der Pfad ungültig ist.
    // Gibt {absPath, ""} zurück wenn alles ok ist.
    // Analogie (AVR): wie ein Adress-Decoder der prüft ob eine Adresse
    // im gültigen Speicherbereich liegt bevor du darauf zugreifst.
    QPair<QString,QString> resolveRead(const QString &path) const
    {
        auto rp = m_policy->resolveRead(path);
        if (!rp.valid) return {"", rp.error};
        return {rp.absPath, ""};
    }

    // Löst einen Pfad für Schreibzugriff auf (nur writable Roots).
    QPair<QString,QString> resolveWrite(const QString &path) const
    {
        auto rp = m_policy->resolveWrite(path);
        if (!rp.valid) return {"", rp.error};
        return {rp.absPath, ""};
    }
};
