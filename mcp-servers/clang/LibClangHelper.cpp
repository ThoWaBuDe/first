#include "LibClangHelper.h"
#include <QFile>
#include <QTextStream>

LibClangHelper::LibClangHelper()
{
    m_index = clang_createIndex(0, 0);
}

LibClangHelper::~LibClangHelper()
{
    if (m_tu) {
        clang_disposeTranslationUnit(m_tu);
    }
    if (m_index) {
        clang_disposeIndex(m_index);
    }
}

bool LibClangHelper::parseFile(const QString &filePath, const QStringList &args)
{
    if (m_tu) {
        clang_disposeTranslationUnit(m_tu);
        m_tu = nullptr;
    }

    m_currentFile = filePath;
    m_diagnostics.clear();

    QFileInfo fi(filePath);
    if (!fi.exists()) {
        m_diagnostics.append(QString("File not found: %1").arg(filePath));
        return false;
    }

    QVector<const char *> cargs;
    cargs.append("-fsyntax-only");
    cargs.append("-std=c++17");
    cargs.append("-I/usr/include");
    cargs.append("-I/usr/local/include");
    
    for (const QString &arg : args) {
        cargs.append(qstrdup(arg.toUtf8().constData()));
    }

    unsigned int options = CXTranslationUnit_PrecompiledPreamble | 
                           CXTranslationUnit_SkipFunctionBodies;

    m_tu = clang_parseTranslationUnit(
        m_index,
        filePath.toUtf8().constData(),
        cargs.data(),
        cargs.size(),
        nullptr,
        0,
        options
    );

    if (!m_tu) {
        m_diagnostics.append("Failed to parse translation unit");
        return false;
    }

    unsigned int diagCount = clang_getNumDiagnostics(m_tu);
    for (unsigned int i = 0; i < diagCount; ++i) {
        CXDiagnostic diag = clang_getDiagnostic(m_tu, i);
        CXString text = clang_getDiagnosticSpelling(diag);
        m_diagnostics.append(clocToQString(text));
        clang_disposeDiagnostic(diag);
    }

    return true;
}

ClangCursor LibClangHelper::getCursorAt(const QString &file, int line, int column)
{
    ClangCursor result;
    result.cursor = clang_getNullCursor();
    result.line = line;
    result.column = column;
    result.file = file;

    if (!m_tu || file.isEmpty()) return result;

    CXFile cxFile = clang_getFile(m_tu, file.toUtf8().constData());
    if (!cxFile) return result;

    CXSourceLocation loc = clang_getLocation(m_tu, cxFile, line, column);
    result.cursor = clang_getCursor(m_tu, loc);

    if (clang_isInvalid(result.cursor.kind)) {
        result.kind = "Invalid";
        return result;
    }

    result.kind = getCursorKindSpelling(clang_getCursorKind(result.cursor));
    result.spelling = getCursorSpelling(result.cursor);
    result.usr = getUSR(result.cursor);
    result.displayName = getDisplayName(result.cursor);

    return result;
}

QVector<ClangSymbol> LibClangHelper::findReferences(const QString &symbol, const QString &)
{
    QVector<ClangSymbol> results;

    if (!m_tu || symbol.isEmpty()) return results;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *results = static_cast<QVector<ClangSymbol>*>(data);
        if (clang_isReference(clang_getCursorKind(cursor))) {
            results->append(LibClangHelper::cursorToSymbolStatic(cursor));
        }
        return CXChildVisit_Recurse;
    };

    CXCursor targetCursor = clang_getNullCursor();
    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &results);

    return results;
}

QVector<ClangSymbol> LibClangHelper::getCallers(const QString &targetUSR)
{
    QVector<ClangSymbol> results;

    if (!m_tu) return results;

    QString searchUSR = targetUSR.isEmpty() ? QString() : targetUSR;

    struct CallerData {
        QVector<ClangSymbol> *results;
        QString targetUSR;
    } data{&results, searchUSR};

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *d = static_cast<CallerData*>(data);
        
        if (clang_getCursorKind(cursor) == CXCursor_CallExpr) {
            CXCursor called = clang_getCursorReferenced(cursor);
            if (!clang_Cursor_isNull(called)) {
                QString usr = getUSR(called);
                if (d->targetUSR.isEmpty() || usr == d->targetUSR) {
                    CXSourceLocation loc = clang_getCursorLocation(cursor);
                    unsigned int line, col;
                    QString file = clocToFile(loc, line, col);
                    
                    ClangSymbol sym;
                    sym.name = getCursorSpelling(called);
                    sym.kind = getCursorKindSpelling(clang_getCursorKind(called));
                    sym.usr = usr;
                    sym.file = file;
                    sym.line = line;
                    sym.column = col;
                    d->results->append(sym);
                }
            }
        }
        return CXChildVisit_Recurse;
    };

    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &data);

    return results;
}

QVector<ClangSymbol> LibClangHelper::getCallees(const QString &)
{
    QVector<ClangSymbol> results;

    if (!m_tu) return results;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *results = static_cast<QVector<ClangSymbol>*>(data);
        
        if (clang_getCursorKind(cursor) == CXCursor_CallExpr) {
            CXCursor called = clang_getCursorReferenced(cursor);
            if (!clang_Cursor_isNull(called)) {
                results->append(LibClangHelper::cursorToSymbolStatic(called));
            }
        }
        return CXChildVisit_Recurse;
    };

    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &results);

    return results;
}

QVector<ClangSymbol> LibClangHelper::getAllFunctions()
{
    QVector<ClangSymbol> results;
    if (!m_tu) return results;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *results = static_cast<QVector<ClangSymbol>*>(data);
        CXCursorKind kind = clang_getCursorKind(cursor);
        
        if (kind == CXCursor_FunctionDecl || 
            kind == CXCursor_CXXMethod ||
            kind == CXCursor_Constructor ||
            kind == CXCursor_Destructor) {
            results->append(LibClangHelper::cursorToSymbolStatic(cursor));
        }
        return CXChildVisit_Recurse;
    };

    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &results);
    return results;
}

QVector<ClangSymbol> LibClangHelper::getAllClasses()
{
    QVector<ClangSymbol> results;
    if (!m_tu) return results;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *results = static_cast<QVector<ClangSymbol>*>(data);
        CXCursorKind kind = clang_getCursorKind(cursor);
        
        if (kind == CXCursor_ClassDecl || 
            kind == CXCursor_StructDecl ||
            kind == CXCursor_ObjCInterfaceDecl ||
            kind == CXCursor_EnumDecl) {
            results->append(LibClangHelper::cursorToSymbolStatic(cursor));
        }
        return CXChildVisit_Recurse;
    };

    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &results);
    return results;
}

QVector<ClangSymbol> LibClangHelper::getAllVariables()
{
    QVector<ClangSymbol> results;
    if (!m_tu) return results;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *results = static_cast<QVector<ClangSymbol>*>(data);
        CXCursorKind kind = clang_getCursorKind(cursor);
        
        if (kind == CXCursor_VarDecl || 
            kind == CXCursor_FieldDecl ||
            kind == CXCursor_ParmDecl) {
            results->append(LibClangHelper::cursorToSymbolStatic(cursor));
        }
        return CXChildVisit_Recurse;
    };

    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &results);
    return results;
}

QString LibClangHelper::getTypeSpelling(CXType type)
{
    CXString typeSpelling = clang_getTypeSpelling(type);
    return clocToQString(typeSpelling);
}

QString LibClangHelper::getCursorKindName(CXCursorKind kind)
{
    return clocToQString(clang_getCursorKindSpelling(kind));
}

ClangSymbol LibClangHelper::cursorToSymbol(CXCursor cursor)
{
    return cursorToSymbolStatic(cursor);
}

ClangSymbol LibClangHelper::cursorToSymbolStatic(CXCursor cursor)
{
    ClangSymbol sym;
    sym.name = getCursorSpelling(cursor);
    sym.kind = getCursorKindSpelling(clang_getCursorKind(cursor));
    sym.usr = getUSR(cursor);
    sym.displayName = getDisplayName(cursor);

    CXSourceLocation loc = clang_getCursorLocation(cursor);
    unsigned int line, col;
    sym.file = clocToFile(loc, line, col);
    sym.line = line;
    sym.column = col;

    CXCursor parent = clang_getCursorSemanticParent(cursor);
    if (!clang_Cursor_isNull(parent)) {
        sym.container = getCursorSpelling(parent);
    }

    return sym;
}

QVector<ClangSymbol> LibClangHelper::getAllIncludes()
{
    QVector<ClangSymbol> results;
    if (!m_tu) return results;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *results = static_cast<QVector<ClangSymbol>*>(data);
        CXCursorKind kind = clang_getCursorKind(cursor);
        
        if (kind == CXCursor_InclusionDirective) {
            results->append(cursorToSymbolStatic(cursor));
        }
        return CXChildVisit_Recurse;
    };

    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &results);
    return results;
}

QVector<ClangSymbol> LibClangHelper::getBaseClasses(const QString &)
{
    QVector<ClangSymbol> results;
    if (!m_tu) return results;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *results = static_cast<QVector<ClangSymbol>*>(data);
        CXCursorKind kind = clang_getCursorKind(cursor);
        
        if (kind == CXCursor_CXXBaseSpecifier) {
            results->append(cursorToSymbolStatic(cursor));
        }
        return CXChildVisit_Recurse;
    };

    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &results);
    return results;
}

QVector<ClangSymbol> LibClangHelper::getDerivedClasses(const QString &)
{
    return getBaseClasses(QString());
}

QVector<ClangSymbol> LibClangHelper::getVirtualMethods(const QString &)
{
    QVector<ClangSymbol> results;
    if (!m_tu) return results;

    auto visitor = [](CXCursor cursor, CXCursor parent, CXClientData data) {
        auto *results = static_cast<QVector<ClangSymbol>*>(data);
        CXCursorKind kind = clang_getCursorKind(cursor);
        
        if (kind == CXCursor_CXXMethod) {
            unsigned int flags = clang_CXXMethod_isVirtual(cursor);
            if (flags) {
                results->append(cursorToSymbolStatic(cursor));
            }
        }
        return CXChildVisit_Recurse;
    };

    clang_visitChildren(clang_getTranslationUnitCursor(m_tu), visitor, &results);
    return results;
}