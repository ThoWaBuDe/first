#pragma once
#include "ClangToolBase.h"

class StubGeneratorTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "generate_stub"; }

    QString description() const override
    {
        return "Generate stub for virtual methods that need implementation.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"class_name", prop("string", "Class name")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        QString className = args.value("class_name").toString();

        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto virtualMethods = m_clang->getVirtualMethods(className);
        if (virtualMethods.isEmpty()) {
            return ToolResult::ok("No virtual methods found for " + className);
        }

        QString result = "Stubs to implement:\n\n";
        for (const auto &m : virtualMethods) {
            result += "virtual void " + m.name + "();\n";
        }

        return ToolResult::ok(result);
    }
};

class GetterSetterTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "generate_getters"; }

    QString description() const override
    {
        return "Generate getter/setter for member variables.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"class_name", prop("string", "Class name")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        QString className = args.value("class_name").toString();

        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto vars = m_clang->getAllVariables();
        if (vars.isEmpty()) {
            return ToolResult::ok("No member variables found.");
        }

        QString result = "Getters/Setters:\n\n";
        for (const auto &v : vars) {
            QString name = v.name;
            if (name.isEmpty()) continue;
            QString ucFirst = name.left(1).toUpper() + name.mid(1);
            result += "Q_PROPERTY(" + v.kind + " " + name + " READ " + name + " WRITE set" + ucFirst + ")\n";
            result += QString("Q_INVOKABLE %1 %2() const;\n").arg(v.kind).arg(name);
            result += QString("Q_INVOKABLE void set%1(const %2 &value);\n\n").arg(ucFirst).arg(v.kind);
        }

        return ToolResult::ok(result);
    }
};

class ConstructorGenTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "generate_constructor"; }

    QString description() const override
    {
        return "Generate constructor from member variables.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"class_name", prop("string", "Class name")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        QString className = args.value("class_name").toString();

        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto vars = m_clang->getAllVariables();
        if (vars.isEmpty()) {
            return ToolResult::ok("No member variables found.");
        }

        QString result = "Constructor:\n\n";
        result += className + "::" + className + "()\n";
        
        QStringList initList;
        QStringList params;
        for (const auto &v : vars) {
            if (v.name.isEmpty()) continue;
            initList.append("    m_" + v.name + "(" + v.name + ")");
            params.append(v.kind + " " + v.name);
        }

        if (!initList.isEmpty()) {
            result += "    : " + initList.join(",\n") + "\n";
        }
        result += "{\n}\n";

        return ToolResult::ok(result);
    }
};

class DocGeneratorTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "generate_docs"; }

    QString description() const override
    {
        return "Generate documentation comments for symbols.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"symbol_name", prop("string", "Symbol name")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        QString symbolName = args.value("symbol_name").toString();

        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto funcs = m_clang->getAllFunctions();
        auto classes = m_clang->getAllClasses();

        QString result = "Documentation templates:\n\n";

        for (const auto &f : funcs) {
            if (!symbolName.isEmpty() && f.name != symbolName) continue;
            result += "/**\n * @brief " + f.name + "\n *\n * @param \n * @return \n */\n";
            result += f.kind + " " + f.name + "();\n\n";
        }

        for (const auto &c : classes) {
            if (!symbolName.isEmpty() && c.name != symbolName) continue;
            result += "/**\n * @brief " + c.name + "\n *\n * " + c.kind + " class\n */\n";
            result += "class " + c.name + " {\n};\n\n";
        }

        if (symbolName.isEmpty()) {
            result += "// Run with symbol_name for specific symbol docs";
        }

        return ToolResult::ok(result.trimmed());
    }
};

class RenameSymbolTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "rename_symbol"; }

    QString description() const override
    {
        return "Rename function/variable/class across codebase.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"old_name", prop("string", "Old name")}, {"new_name", prop("string", "New name")}};
    }

    QJsonArray required() const override { return {"file", "old_name", "new_name"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        QString oldName = args.value("old_name").toString();
        QString newName = args.value("new_name").toString();

        if (file.isEmpty() || oldName.isEmpty() || newName.isEmpty())
            return ToolResult::err("Error: file, old_name, and new_name are required.");

        return ToolResult::ok(QString("Rename '%1' to '%2':\n\n"
            "Note: Use IDE refactoring for safe rename.\n"
            "Manual: sed -i 's/%1/%2/g' file.cpp\n\n"
            "This is a preview - actual renaming modifies files.")
            .arg(oldName).arg(newName));
    }
};

class ExtractFunctionTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "extract_function"; }

    QString description() const override
    {
        return "Extract code block as new function.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"start_line", prop("integer", "Start line")}, {"end_line", prop("integer", "End line")}, {"func_name", prop("string", "Function name")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        int startLine = args.value("start_line").toInt();
        int endLine = args.value("end_line").toInt();
        QString funcName = args.value("func_name").toString();

        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");

        return ToolResult::ok(QString("Extract function '%1' from lines %2-%3:\n\n"
            "Note: Use IDE refactoring to extract.\n\n"
            "void %1() {\n"
            "    // extracted code here\n"
            "}")
            .arg(funcName).arg(startLine).arg(endLine));
    }
};

class MoveMethodTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "move_method"; }

    QString description() const override
    {
        return "Move method to another class.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"method_name", prop("string", "Method name")}, {"target_class", prop("string", "Target class")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        QString methodName = args.value("method_name").toString();
        QString targetClass = args.value("target_class").toString();

        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");

        return ToolResult::ok(QString("Move method '%1' to class '%2':\n\n"
            "Note: Use IDE refactoring.\n\n"
            "1. Cut method from current class\n"
            "2. Paste into target class\n"
            "3. Update call sites")
            .arg(methodName).arg(targetClass));
    }
};

class AddIncludeTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "add_include"; }

    QString description() const override
    {
        return "Add #include directive to file.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}, {"header", prop("string", "Header to include")}};
    }

    QJsonArray required() const override { return {"file", "header"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        QString header = args.value("header").toString();

        if (file.isEmpty() || header.isEmpty())
            return ToolResult::err("Error: 'file' and 'header' are required.");

        QString includeLine;
        if (header.startsWith("<")) {
            includeLine = "#include " + header;
        } else if (header.endsWith(".h")) {
            includeLine = "#include \"" + header + "\"";
        } else {
            includeLine = "#include <" + header + ">";
        }

        return ToolResult::ok(QString("Add to %1:\n\n%2\n\n"
            "Place after other system includes, sorted alphabetically.")
            .arg(file).arg(includeLine));
    }
};