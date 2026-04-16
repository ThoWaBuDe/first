#include "AgentUtils.h"
#include <QJsonDocument>
#include <QStringList>
#include <QVector>

namespace AgentUtils {

// ─── repairJson ───────────────────────────────────────────────────────────────
QString repairJson(const QString &broken)
{
    QString s = broken.trimmed();
    if (!QJsonDocument::fromJson(s.toUtf8()).isNull()) return s;

    // Stufe 1: fehlende schließende Klammern ergänzen
    int openObj = s.count('{') - s.count('}');
    int openArr = s.count('[') - s.count(']');
    QString fixed = s;
    for (int i = 0; i < openArr; ++i) fixed += ']';
    for (int i = 0; i < openObj; ++i) fixed += '}';
    if (!QJsonDocument::fromJson(fixed.toUtf8()).isNull()) return fixed;

    // Stufe 2: Trailing-Komma entfernen
    // Sucht "," direkt vor ] oder } (nur Whitespace dazwischen)
    QString noTrailing = fixed;
    for (const QString &close : {QString("}"), QString("]")}) {
        int pos = noTrailing.lastIndexOf(close);
        while (pos > 0) {
            int comma = noTrailing.lastIndexOf(',', pos - 1);
            if (comma < 0) break;
            bool onlyWs = true;
            for (int i = comma + 1; i < pos; ++i) {
                if (!noTrailing[i].isSpace()) { onlyWs = false; break; }
            }
            if (onlyWs) { noTrailing.remove(comma, 1); pos = comma; }
            else break;
        }
    }
    if (!QJsonDocument::fromJson(noTrailing.toUtf8()).isNull()) return noTrailing;

    // Stufe 3: einfache → doppelte Anführungszeichen
    if (!s.contains('"') && s.contains('\'')) {
        QString withDouble = noTrailing;
        withDouble.replace('\'', '"');
        if (!QJsonDocument::fromJson(withDouble.toUtf8()).isNull())
            return withDouble;
    }

    return {}; // Reparatur fehlgeschlagen
}

// ─── toolCallKey ──────────────────────────────────────────────────────────────
QString toolCallKey(const QString &toolName, const QJsonObject &args)
{
    QStringList parts;
    parts << toolName;
    QStringList keys = args.keys();
    keys.sort(); // deterministisch
    for (const QString &k : keys) {
        QString val = QString::fromUtf8(
            QJsonDocument(QJsonObject{{k, args[k]}})
            .toJson(QJsonDocument::Compact));
        parts << val;
    }
    return parts.join('|');
}

// ─── deadlockEscalationPrompt ─────────────────────────────────────────────────
QString deadlockEscalationPrompt(const QString &toolName, int count)
{
    if (count >= DEADLOCK_ABORT)
        return QString(
            "[SYSTEM: Tool '%1' ist %2 Mal hintereinander fehlgeschlagen. "
            "ABBRUCH. Erkläre dem Nutzer was schiefgelaufen ist.]")
            .arg(toolName).arg(count);

    if (count >= DEADLOCK_REDIRECT)
        return QString(
            "[SYSTEM: Tool '%1' schlägt wiederholt fehl (%2 Mal). "
            "Suche einen ANDEREN Weg zum Ziel.]")
            .arg(toolName).arg(count);

    return QString(
        "[SYSTEM: Tool '%1' hat %2 Mal hintereinander den gleichen Fehler. "
        "Überprüfe deine Argumente sorgfältig.]")
        .arg(toolName).arg(count);
}

// ─── computeDiffHtml ─────────────────────────────────────────────────────────
QString computeDiffHtml(const QString &before, const QString &after,
                        const QString &filename)
{
    QStringList oldLines = before.split('\n');
    QStringList newLines = after.split('\n');
    int m = oldLines.size(), n = newLines.size();

    if (m > 300 || n > 300)
        return QString("<b>Diff %1</b> (zu groß für LCS, %2→%3 Zeilen)")
               .arg(filename.toHtmlEscaped()).arg(m).arg(n);

    // DP-Tabelle: LCS Längen
    // at(i,j) = LCS-Länge von oldLines[0..i-1] und newLines[0..j-1]
    std::vector<int> dp((m+1)*(n+1), 0);
    auto at = [&](int i, int j) -> int& { return dp[i*(n+1)+j]; };
    for (int i = 1; i <= m; ++i)
        for (int j = 1; j <= n; ++j)
            at(i,j) = (oldLines[i-1] == newLines[j-1])
                      ? at(i-1,j-1) + 1
                      : std::max(at(i-1,j), at(i,j-1));

    // Backtracking: Diff-Zeilen rekonstruieren
    struct DiffLine {
        enum Type { Equal, Added, Removed } type;
        QString text;
    };
    QVector<DiffLine> diffLines;
    int i = m, j = n;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && oldLines[i-1] == newLines[j-1])
            { diffLines.prepend({DiffLine::Equal,   oldLines[i-1]}); --i; --j; }
        else if (j > 0 && (i == 0 || at(i,j-1) >= at(i-1,j)))
            { diffLines.prepend({DiffLine::Added,   newLines[j-1]}); --j; }
        else
            { diffLines.prepend({DiffLine::Removed, oldLines[i-1]}); --i; }
    }

    // Kontext-Filter: nur Änderungen + CONTEXT Zeilen Umgebung anzeigen
    static constexpr int CONTEXT = 2;
    QVector<bool> changed(diffLines.size(), false);
    QVector<bool> show(diffLines.size(), false);
    for (int k = 0; k < diffLines.size(); ++k)
        if (diffLines[k].type != DiffLine::Equal) changed[k] = true;
    for (int k = 0; k < diffLines.size(); ++k) {
        if (!changed[k]) continue;
        for (int c = std::max(0, k-CONTEXT);
             c <= std::min((int)diffLines.size()-1, k+CONTEXT); ++c)
            show[c] = true;
    }

    // HTML aufbauen
    QString html = QString("<b>Diff: %1</b><br><pre style='font-size:10px'>")
                   .arg(filename.toHtmlEscaped());
    bool inGap = false;
    for (int k = 0; k < diffLines.size(); ++k) {
        if (!show[k]) {
            if (!inGap) { html += "<span style='color:#aaa'>...</span>\n"; inGap = true; }
            continue;
        }
        inGap = false;
        QString esc = diffLines[k].text.toHtmlEscaped();
        switch (diffLines[k].type) {
            case DiffLine::Added:
                html += QString("<span style='color:#188038;background:#e6f4ea'>"
                                "+ %1</span>\n").arg(esc); break;
            case DiffLine::Removed:
                html += QString("<span style='color:#c5221f;background:#fce8e6'>"
                                "- %1</span>\n").arg(esc); break;
            case DiffLine::Equal:
                html += QString("<span style='color:#666'>  %1</span>\n").arg(esc); break;
        }
    }
    html += "</pre>";
    return html;
}

} // namespace AgentUtils
