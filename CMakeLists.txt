cmake_minimum_required(VERSION 3.16)
project(LlamaQt VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ─── Qt6 ────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

# ─── Llama.cpp ──────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(LLAMA REQUIRED IMPORTED_TARGET llama)

# ─── Sources ────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/UI/MainWindow.cpp

    # Agent — aufgeteilt in Teilklassen (Refactoring)
    src/Agent/Agent.cpp
    src/Agent/AgentChat.cpp
    src/Agent/AgentPlan.cpp
    src/Agent/AgentExecute.cpp
    src/Agent/AgentAssemble.cpp
    src/Agent/AgentUtils.cpp

    src/Task/CommandProcessor.cpp
    src/AI/LlamaWorker.cpp
    src/MCP/McpClient.cpp
    src/MCP/McpManager.cpp
    src/Chat/ChatModel.cpp
    src/Config/AppConfig.cpp
    src/Chat/ChatLogger.cpp
    src/UI/ConfigDialog.cpp
    src/UI/EditorDock.cpp
    src/UI/PlannerDock.cpp
    src/UI/TaskNodeDialog.cpp
)

set(HEADERS
    src/UI/MainWindow.h
    src/Agent/Agent.h
    src/Agent/AgentChat.h
    src/Agent/AgentPlan.h
    src/Agent/AgentExecute.h
    src/Agent/AgentAssemble.h
    src/Agent/AgentUtils.h
    src/Task/CommandProcessor.h
    src/AI/LlamaWorker.h
    src/MCP/McpClient.h
    src/MCP/McpManager.h
    src/Chat/ChatModel.h
    src/Config/AppConfig.h
    src/Chat/ChatLogger.h
    src/UI/ConfigDialog.h
    src/UI/EditorDock.h
    src/UI/PlannerDock.h
    src/Task/TaskNode.h
    src/Task/TaskTree.h
    src/Task/TaskTreeModel.h
    src/UI/TaskNodeDialog.h
    src/AI/ExecuteMemory.h
    src/Task/CodeAssembler.h
    src/UI/NodeGraphView.h
    src/Chat/ChatTemplate.h
    src/UI/SearchBar.h
    src/Chat/ToolCallFormat.h
)

add_executable(LlamaQt ${SOURCES} ${HEADERS}
    src/UI/MainWindow.ui)

# ─── Include-Pfade ──────────────────────────────────────────────────────────
target_include_directories(LlamaQt PRIVATE
    src/
    ${LLAMA_INCLUDE_DIRS}
)

target_link_libraries(LlamaQt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Sql
    PkgConfig::LLAMA
)

# ─── MCP Server Subprojekte ──────────────────────────────────────────────────
add_subdirectory(mcp-servers/filesystem)
add_subdirectory(mcp-servers/sysinfo)
add_subdirectory(mcp-servers/compile)
add_subdirectory(mcp-servers/websearch)
add_subdirectory(mcp-servers/tree-sitter)
add_subdirectory(mcp-servers/clang)
