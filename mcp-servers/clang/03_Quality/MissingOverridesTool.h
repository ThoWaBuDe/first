#pragma once
#include "../ClangToolBase.h"

class MissingOverridesTool : public ClangToolBase {
public:
    using ClangToolBase::ClangToolBase;

    QString name() const override { return "missing_overrides"; }

    QString description() const override
    {
        return "Find virtual methods overriding base methods that are missing override keyword.";
    }

    QJsonObject properties() const override
    {
        return {{"file", prop("string", "File")}};
    }

    QJsonArray required() const override { return {"file"}; }

    ToolResult execute(const QJsonObject &args) override {
        QString file = args.value("file").toString();
        if (file.isEmpty()) return ToolResult::err("Error: 'file' is required.");
        if (!parseFile(file)) return ToolResult::err("Error: Could not parse file.");

        auto virtualMethods = m_clang->getVirtualMethods(QString());
        if (virtualMethods.isEmpty()) {
            return ToolResult::ok("No virtual methods found.");
        }

        QString result = "Virtual methods found:\n";
        for (const auto &m : virtualMethods) {
            result += QString("  %1 - %2:%3\n")
                .arg(m.displayName).arg(m.file).arg(m.line);
        }

        return ToolResult::ok(result.trimmed());
    }
};