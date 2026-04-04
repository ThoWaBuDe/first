cmake_minimum_required(VERSION 3.16)
project(LlamaQt VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)      # Für Qt MOC (Meta-Object Compiler)
set(CMAKE_AUTOUIC ON)      # Für .ui Dateien
set(CMAKE_AUTORCC ON)      # Für .qrc Ressourcen

# ─── Qt6 ────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# ─── llama.cpp Pfade ────────────────────────────────────────────────────────
# Passe diese Pfade an dein Build-Verzeichnis an!
set(LLAMA_BUILD_DIR "$ENV{HOME}/ai/qLP/build" CACHE PATH "llama.cpp build dir")
set(LLAMA_SRC_DIR   "$ENV{HOME}/ai/qLP"       CACHE PATH "llama.cpp source dir")

# ─── Sources ────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/LlamaWorker.cpp
    src/McpClient.cpp
    src/McpManager.cpp
    src/ChatModel.cpp
)

set(HEADERS
    src/MainWindow.h
    src/LlamaWorker.h
    src/McpClient.h
    src/McpManager.h
    src/ChatModel.h
)

add_executable(LlamaQt ${SOURCES} ${HEADERS}
    src/MainWindow.ui)

# ─── Include-Pfade ──────────────────────────────────────────────────────────
target_include_directories(LlamaQt PRIVATE
    src/
    ${LLAMA_SRC_DIR}/include          # llama.h
    ${LLAMA_SRC_DIR}/ggml/include     # ggml.h
    ${LLAMA_BUILD_DIR}/               # ggml-config.h (generiert)
)

# ─── Libraries ──────────────────────────────────────────────────────────────
# Sucht libllama.so im Build-Verzeichnis
find_library(LLAMA_LIB
    NAMES llama
    HINTS ${LLAMA_BUILD_DIR} ${LLAMA_BUILD_DIR}/src
    REQUIRED
)

target_link_libraries(LlamaQt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    ${LLAMA_LIB}
)

# RPATH setzen: damit die .so zur Laufzeit gefunden wird ohne LD_LIBRARY_PATH
set_target_properties(LlamaQt PROPERTIES
    BUILD_RPATH "${LLAMA_BUILD_DIR};${LLAMA_BUILD_DIR}/src"
    INSTALL_RPATH "${LLAMA_BUILD_DIR};${LLAMA_BUILD_DIR}/src"
)

# ─── MCP Server Subprojekte ──────────────────────────────────────────────────
add_subdirectory(mcp-servers/filesystem)
add_subdirectory(mcp-servers/sysinfo)
add_subdirectory(mcp-servers/compile)
