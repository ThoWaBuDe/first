cmake_minimum_required(VERSION 3.16)
project(LlamaMcpClang)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(llamaqt-clang
    main.cpp
    LibClangHelper.cpp
)

target_include_directories(llamaqt-clang PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    /usr/lib/llvm-19/include
)

target_link_libraries(llamaqt-clang PRIVATE
    Qt6::Core
    /usr/lib/llvm-19/lib/libclang.so
    Threads::Threads
)