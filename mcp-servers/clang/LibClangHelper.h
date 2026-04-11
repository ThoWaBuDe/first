#pragma once
// ─── LibClangHelper ───────────────────────────────────────────────────────────
// Wrapper für libclang C-API.
// Bietet:
//   - Index erstellen und TranslationUnit parsen
//   - Cursor/Type-Hilfsfunktionen
//   - USR-basierte Symbol-Identifikation
//   - Suchfunktionen (find_references, find_definitions, etc.)

#include <clang-c/Index.h>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QPair>
#include <QMap>
#include <QSet>
#include <QFileInfo>
#include <QDir>

struct ClangCursor {
    CXCursor cursor;
    QString kind;
    QString spelling;
    QString usr;
    QString displayName;
    QString file;
    int line;
    int column;
};

struct ClangLocation {
    QString file;
    int line;
    int column;
};

struct ClangSymbol {
    QString name;
    QString kind;
    QString usr;
    QString displayName;
    QString file;
    int line;
    int column;
    QString container;
};

class LibClangHelper
{
public:
    LibClangHelper();
    ~LibClangHelper();

    bool parseFile(const QString &filePath, const QStringList &args = {});

    QStringList diagnostics() const { return m_diagnostics; }
    bool isValid() const { return m_tu != nullptr; }

    ClangCursor getCursorAt(const QString &file, int line, int column);
    ClangCursor getCursorForUSR(const QString &usr);

    QVector<ClangSymbol> findReferences(const QString &symbol, const QString &usr);
    QVector<ClangSymbol> findDefinitions(const QString &symbol, const QString &usr);
    QVector<ClangSymbol> findDeclarations(const QString &usr);

    QVector<ClangSymbol> getCallers(const QString &usr);
    QVector<ClangSymbol> getCallees(const QString &usr);

    QVector<ClangSymbol> getAllFunctions();
    QVector<ClangSymbol> getAllClasses();
    QVector<ClangSymbol> getAllVariables();

    QVector<ClangSymbol> getAllIncludes();
    QVector<ClangSymbol> getBaseClasses(const QString &className);
    QVector<ClangSymbol> getDerivedClasses(const QString &className);
    QVector<ClangSymbol> getVirtualMethods(const QString &className);

    QString getTypeSpelling(CXType type);
    QString getCursorKindName(CXCursorKind kind);

    ClangSymbol cursorToSymbol(CXCursor cursor);
    static ClangSymbol cursorToSymbolStatic(CXCursor cursor);

    QString currentFile() const { return m_currentFile; }

private:
    CXIndex m_index = nullptr;
    CXTranslationUnit m_tu = nullptr;
    QString m_currentFile;
    QStringList m_diagnostics;

public:
    static QString clocToQString(CXString s);
private:
    static QString clocToFile(CXSourceLocation loc, unsigned int &line, unsigned int &col);
    static QString getUSR(CXCursor cursor);
    static QString getCursorSpelling(CXCursor cursor);
    static QString getDisplayName(CXCursor cursor);
    static QString getCursorKindSpelling(CXCursorKind kind);
};

inline QString LibClangHelper::clocToQString(CXString s)
{
    QString result = QString::fromUtf8(clang_getCString(s));
    clang_disposeString(s);
    return result;
}

inline QString LibClangHelper::clocToFile(CXSourceLocation loc, unsigned int &line, unsigned int &col)
{
    CXFile file;
    clang_getFileLocation(loc, &file, &line, &col, nullptr);
    if (!file) return QString();
    return clocToQString(clang_getFileName(file));
}

inline QString LibClangHelper::getUSR(CXCursor cursor)
{
    if (clang_isDeclaration(clang_getCursorKind(cursor))) {
        CXString usr = clang_getCursorUSR(cursor);
        return clocToQString(usr);
    }
    return QString();
}

inline QString LibClangHelper::getCursorSpelling(CXCursor cursor)
{
    return clocToQString(clang_getCursorSpelling(cursor));
}

inline QString LibClangHelper::getDisplayName(CXCursor cursor)
{
    return clocToQString(clang_getCursorDisplayName(cursor));
}

inline QString LibClangHelper::getCursorKindSpelling(CXCursorKind kind)
{
    return clocToQString(clang_getCursorKindSpelling(kind));
}