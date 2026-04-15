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
    src/MainWindow.cpp
    src/Agent.cpp
    src/CommandProcessor.cpp
    src/LlamaWorker.cpp
    src/McpClient.cpp
    src/McpManager.cpp
    src/ChatModel.cpp
    src/AppConfig.cpp
    src/ChatLogger.cpp
    src/ConfigDialog.cpp
    src/EditorDock.cpp
    src/PlannerDock.cpp
    src/TaskNodeDialog.cpp
)

set(HEADERS
    src/MainWindow.h
    src/Agent.h
    src/CommandProcessor.h
    src/LlamaWorker.h
    src/McpClient.h
    src/McpManager.h
    src/ChatModel.h
    src/AppConfig.h
    src/ChatLogger.h
    src/ConfigDialog.h
    src/EditorDock.h
    src/PlannerDock.h
    src/TaskNode.h
    src/TaskTree.h
    src/TaskTreeModel.h
    src/TaskNodeDialog.h
    src/ExecuteMemory.h
    src/CodeAssembler.h
    src/NodeGraphView.h
)

add_executable(LlamaQt ${SOURCES} ${HEADERS}
    src/MainWindow.ui)

# ─── Include-Pfade ──────────────────────────────────────────────────────────
# Hinweis: PkgConfig::LLAMA bringt Include-Pfade und RPATH automatisch mit.
# Wir fügen nur src/ manuell hinzu.
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
