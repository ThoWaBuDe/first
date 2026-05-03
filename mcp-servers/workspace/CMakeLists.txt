cmake_minimum_required(VERSION 3.16)
project(LlamaMcpWorkspace)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_executable(llamaqt-workspace main.cpp)

target_include_directories(llamaqt-workspace PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(llamaqt-workspace PRIVATE
    Qt6::Core
    Qt6::Network
)
