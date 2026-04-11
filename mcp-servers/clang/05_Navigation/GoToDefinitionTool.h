#pragma once
// ─── GoToDefinitionTool ───────────────────────────────────────────────────────
// Navigation: Springe zur Definition eines Symbols.

#include "../ClangToolBase.h"

class GoToDefinitionTool : public ClangToolBase
{
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "go_to_definition"; }

    QString description() const override
    {
        return "Navigate to the definition of a function, class, variable, or type. "
               "Given a file, line, and column, finds where the symbol is defined.";
    }

    QJsonObject properties() const override
    {
        return {
            {"file", prop("string", "File path")},
            {"line", prop("integer", "Line number (1-based)")},
            {"column", prop("integer", "Column number (1-based)")},
            {"symbol_name", prop("string", "Symbol name (optional, for direct lookup)")}
        };
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override
    {
        QString file = args.value("file").toString();
        int line = args.value("line").toInt(1);
        int column = args.value("column").toInt(1);
        QString symbolName = args.value("symbol_name").toString();

        if (file.isEmpty())
            return ToolResult::err("Error: 'file' is required.");

        if (!parseFile(file))
            return ToolResult::err("Error: Could not parse file: " + m_clang->diagnostics().join("\n"));

        ClangCursor cursor;
        
        if (!symbolName.isEmpty()) {
            auto funcs = m_clang->getAllFunctions();
            for (const auto &f : funcs) {
                if (f.name == symbolName || f.displayName == symbolName) {
                    QString result = QString("Definition of %1:\n  %2:%3:%4")
                        .arg(symbolName).arg(f.file).arg(f.line).arg(f.column);
                    return ToolResult::ok(result);
                }
            }
            auto classes = m_clang->getAllClasses();
            for (const auto &c : classes) {
                if (c.name == symbolName || c.displayName == symbolName) {
                    QString result = QString("Definition of %1:\n  %2:%3:%4")
                        .arg(symbolName).arg(c.file).arg(c.line).arg(c.column);
                    return ToolResult::ok(result);
                }
            }
            return ToolResult::err(QString("Symbol '%1' not found.").arg(symbolName));
        }

        cursor = m_clang->getCursorAt(file, line, column);
        if (clang_isInvalid(cursor.cursor.kind))
            return ToolResult::err("No symbol at specified location.");

        CXCursor def = clang_getCursorDefinition(cursor.cursor);
        if (clang_Cursor_isNull(def)) {
            return ToolResult::ok(QString("No definition found. Declaration: %1")
                .arg(cursor.displayName));
        }

        CXSourceLocation loc = clang_getCursorLocation(def);
        unsigned int defLine, defCol;
        CXFile defFile;
        clang_getFileLocation(loc, &defFile, &defLine, &defCol, nullptr);
        
        if (!defFile) {
            return ToolResult::ok("Definition is in system header (not accessible).");
        }

        QString defFileName = LibClangHelper::clocToQString(clang_getFileName(defFile));
        
        return ToolResult::ok(QString("Definition of %1:\n  %2:%3:%4")
            .arg(cursor.displayName)
            .arg(defFileName)
            .arg(defLine + 1)
            .arg(defCol + 1));
    }
};