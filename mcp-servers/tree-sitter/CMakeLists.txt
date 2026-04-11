cmake_minimum_required(VERSION 3.16)
project(LlamaMcpTreeSitter)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

# ── tree-sitter core runtime ──────────────────────────────────────────────────
# lib.c ist ein "unity build" — eine einzige .c Datei die alle anderen inkludiert.
# Wir kompilieren sie als C (nicht C++) daher add_library mit separater Sprache.
add_library(tree-sitter-core STATIC
    vendor/tree-sitter/lib/src/lib.c
)
target_include_directories(tree-sitter-core PUBLIC
    vendor/tree-sitter/lib/include
    vendor/tree-sitter/lib/src        # interne headers (alloc.h etc.)
)
set_target_properties(tree-sitter-core PROPERTIES
    C_STANDARD 11
    POSITION_INDEPENDENT_CODE ON
)

# ── C grammar wird nicht benötigt ────────────────────────────────────────────
# C ist ein Subset von C++ — der C++ Parser versteht beides.
# tree-sitter-c wird nicht kompiliert und nicht gelinkt.

# ── Shim für tree_sitter/alloc.h ──────────────────────────────────────────────
# scanner.c der Grammars macht #include <tree_sitter/alloc.h> etc.
# Diese Dateien liegen in vendor/tree-sitter/lib/src/ — dort gibt es aber
# keinen Unterordner "tree_sitter/". In lib/include/ liegen sie auch nicht.
# Lösung: configure_file kopiert alle internen Headers zur Build-Zeit
# in ${CMAKE_BINARY_DIR}/tree_sitter_shim/tree_sitter/ — kein Touch der
# vendor-Quellen, git pull bleibt sauber.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tree_sitter_shim/tree_sitter)
configure_file(
    vendor/tree-sitter/lib/src/alloc.h
    ${CMAKE_BINARY_DIR}/tree_sitter_shim/tree_sitter/alloc.h
    COPYONLY
)
configure_file(
    vendor/tree-sitter/lib/src/array.h
    ${CMAKE_BINARY_DIR}/tree_sitter_shim/tree_sitter/array.h
    COPYONLY
)
configure_file(
    vendor/tree-sitter/lib/src/parser.h
    ${CMAKE_BINARY_DIR}/tree_sitter_shim/tree_sitter/parser.h
    COPYONLY
)
configure_file(
    vendor/tree-sitter/lib/src/point.h
    ${CMAKE_BINARY_DIR}/tree_sitter_shim/tree_sitter/point.h
    COPYONLY
)
configure_file(
    vendor/tree-sitter/lib/src/atomic.h
    ${CMAKE_BINARY_DIR}/tree_sitter_shim/tree_sitter/atomic.h
    COPYONLY
)

# ── C++ grammar ───────────────────────────────────────────────────────────────
# scanner.c ist handgeschrieben (erkennt z.B. raw strings, template-Klammern)
add_library(tree-sitter-cpp STATIC
    vendor/tree-sitter-cpp/src/parser.c
    vendor/tree-sitter-cpp/src/scanner.c
)
target_include_directories(tree-sitter-cpp PUBLIC
    vendor/tree-sitter/lib/include
    ${CMAKE_BINARY_DIR}/tree_sitter_shim
)
set_target_properties(tree-sitter-cpp PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

# ── CMake grammar ─────────────────────────────────────────────────────────────
add_library(tree-sitter-cmake STATIC
    vendor/tree-sitter-cmake/src/parser.c
    vendor/tree-sitter-cmake/src/scanner.c
)
target_include_directories(tree-sitter-cmake PUBLIC
    vendor/tree-sitter/lib/include
    ${CMAKE_BINARY_DIR}/tree_sitter_shim
)
set_target_properties(tree-sitter-cmake PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

# ── MCP server executable ─────────────────────────────────────────────────────
add_executable(llamaqt-treesitter main.cpp)

target_include_directories(llamaqt-treesitter PRIVATE
    vendor/tree-sitter/lib/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common   # McpServer.h + PathPolicy.h
)

target_link_libraries(llamaqt-treesitter PRIVATE
    Qt6::Core
    tree-sitter-core
    tree-sitter-cpp
    tree-sitter-cmake
)
