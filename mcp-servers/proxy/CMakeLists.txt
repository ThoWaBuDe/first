cmake_minimum_required(VERSION 3.16)
project(llamaqt-mcp-proxy VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_executable(llamaqt-mcp-proxy main.cpp)

target_link_libraries(llamaqt-mcp-proxy PRIVATE
    Qt6::Core
    Qt6::Network
)

# ─── Install-Regeln (für .deb) ───────────────────────────────────────────────
# WICHTIG: DESTINATION ohne führendes "usr/" — CPack hängt selbst
# den Prefix (/usr) davor. Mit "usr/lib/..." entsteht "/usr/usr/lib/...".

# Proxy-Binary
install(TARGETS llamaqt-mcp-proxy
        RUNTIME DESTINATION lib/llamaqt-mcp)

# MCP-Server Binaries — werden von build-in-container.sh unter /tmp/build-*/
# gebaut. Pfad muss mit dem Build-Script übereinstimmen.
foreach(SERVER filesystem sysinfo compile websearch)
    install(PROGRAMS /tmp/build-${SERVER}/llamaqt-${SERVER}
            DESTINATION lib/llamaqt-mcp
            OPTIONAL)
endforeach()

# systemd-Service Unit
install(FILES ${CMAKE_SOURCE_DIR}/llamaqt-mcp.service
        DESTINATION lib/systemd/system)

# ─── CPack: .deb erstellen ───────────────────────────────────────────────────
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "llamaqt-mcp")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "LlamaQt")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "LlamaQt MCP Server Bundle mit SSE-Proxy")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libqt6core6t64, libqt6network6")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")

# Postinst: systemd-Service aktivieren
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/debian/postinst"
    "${CMAKE_SOURCE_DIR}/debian/prerm")

include(CPack)
