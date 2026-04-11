#pragma once
// ─── ClangToolBase ───────────────────────────────────────────────────────────
// Zwischenschicht zwischen ToolBase und den konkreten clang Tools.

#include "../common/ToolBase.h"
#include "../common/PathPolicy.h"
#include "LibClangHelper.h"

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>

class ClangToolBase : public ToolBase
{
public:
    ClangToolBase(PathPolicy *policy, const QString &projectRoot)
        : m_policy(policy), m_projectRoot(projectRoot)
    {
        m_clang = std::make_unique<LibClangHelper>();
    }

protected:
    PathPolicy *m_policy;
    QString m_projectRoot;
    std::unique_ptr<LibClangHelper> m_clang;

    bool parseFile(const QString &filePath)
    {
        return m_clang->parseFile(filePath);
    }

    static QJsonObject prop(const QString &type, const QString &desc)
    {
        return QJsonObject{{"type", type}, {"description", desc}};
    }

    static QJsonArray req(std::initializer_list<const char*> fields)
    {
        QJsonArray arr;
        for (const char *f : fields) arr.append(f);
        return arr;
    }

    QString projectRoot() const { return m_projectRoot; }

    QString formatSymbols(const QVector<ClangSymbol> &symbols, const QString &title)
    {
        if (symbols.isEmpty()) return title + ": (none)";
        
        QString result = title + ":\n";
        QSet<QString> seen;
        for (const auto &sym : symbols) {
            QString key = QString("%1:%2:%3").arg(sym.file).arg(sym.line).arg(sym.name);
            if (seen.contains(key)) continue;
            seen.insert(key);
            
            result += QString("  %1:%2  %3 (%4)\n")
                .arg(sym.file)
                .arg(sym.line)
                .arg(sym.name)
                .arg(sym.kind);
        }
        return result.trimmed();
    }
};