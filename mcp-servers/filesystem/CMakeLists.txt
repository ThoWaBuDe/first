cmake_minimum_required(VERSION 3.16)
project(LlamaMcpFilesystem)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

# ── Quellen ───────────────────────────────────────────────────────────────────
# McpServer ist header-only — run() und registerTool() sind inline in McpServer.h.
# Alle Tool-Klassen sind ebenfalls Header-only.
# main.cpp ist die einzige zu kompilierende Quelldatei.
add_executable(llamaqt-filesystem
    main.cpp
)

# ── Include-Pfade ─────────────────────────────────────────────────────────────
# common/  — McpServer.h, PathPolicy.h, ToolBase.h
# .        — FilesystemToolBase.h, GitHelper.h, *Tool.h
target_include_directories(llamaqt-filesystem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(llamaqt-filesystem PRIVATE Qt6::Core)
