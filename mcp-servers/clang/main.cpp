// ─── LlamaQt MCP Clang Server v1.0 ─────────────────────────────────────────────

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>
#include <memory>

#include "../common/McpServer.h"
#include "../common/PathPolicy.h"

#include "ClangToolBase.h"
#include "LibClangHelper.h"
#include "AllStubs.h"

#include "05_Navigation/GoToDefinitionTool.h"
#include "05_Navigation/FindReferencesTool.h"
#include "05_Navigation/GoToDeclarationTool.h"
#include "05_Navigation/TypeHierarchyTool.h"

#include "01_Analysis/FindCallersTool.h"
#include "01_Analysis/FindCalleesTool.h"
#include "01_Analysis/ClassHierarchyTool.h"
#include "01_Analysis/VariableRefsTool.h"

#include "03_Quality/UnusedIncludesTool.h"
#include "03_Quality/MissingOverridesTool.h"
#include "03_Quality/TypeErrorsTool.h"
#include "03_Quality/DeadCodeTool.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString projectRoot = QDir::homePath() + "/llamatools";
    PathPolicy policy;
 //   policy.addRoot(projectRoot, true);

    QTextStream err(stderr);
    err << "[llamaqt-clang v1.0]\n";
    err << "  Project root: " << projectRoot << "\n";
    err.flush();

    McpServer server("llamaqt-clang", "1.0");

    server.registerTool(std::make_unique<GoToDefinitionTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<FindReferencesTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<GoToDeclarationTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<TypeHierarchyTool>(&policy, projectRoot));

    server.registerTool(std::make_unique<FindCallersTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<FindCalleesTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<ClassHierarchyTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<VariableRefsTool>(&policy, projectRoot));

    server.registerTool(std::make_unique<UnusedIncludesTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<MissingOverridesTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<TypeErrorsTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<DeadCodeTool>(&policy, projectRoot));

    // Generation
    server.registerTool(std::make_unique<StubGeneratorTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<GetterSetterTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<ConstructorGenTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<DocGeneratorTool>(&policy, projectRoot));

    // Refactoring
    server.registerTool(std::make_unique<RenameSymbolTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<ExtractFunctionTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<MoveMethodTool>(&policy, projectRoot));
    server.registerTool(std::make_unique<AddIncludeTool>(&policy, projectRoot));

    server.run();
    return 0;
}
