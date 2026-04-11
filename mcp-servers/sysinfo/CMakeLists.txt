cmake_minimum_required(VERSION 3.16)
project(LlamaMcpSysinfo)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(llamaqt-sysinfo main.cpp)

target_include_directories(llamaqt-sysinfo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(llamaqt-sysinfo PRIVATE Qt6::Core)