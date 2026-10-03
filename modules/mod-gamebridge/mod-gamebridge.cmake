# Included by AzerothCore's modules/CMakeLists.txt after the `modules` target exists
# (static module build only). Adds the standalone GameBridge protocol library
# sources (C++17-compatible, compiled here with the core's own standard).
get_filename_component(GAMEBRIDGE_MODULE_REAL "${CMAKE_SOURCE_DIR}/modules/mod-gamebridge" REALPATH)
set(GAMEBRIDGE_PROTOCOL_DIR "${GAMEBRIDGE_MODULE_REAL}/../../protocol" CACHE PATH "Path to the GameBridge protocol library")
get_filename_component(GAMEBRIDGE_PROTOCOL_DIR "${GAMEBRIDGE_PROTOCOL_DIR}" REALPATH)

if(NOT EXISTS "${GAMEBRIDGE_PROTOCOL_DIR}/include/gamebridge/protocol.h")
  message(FATAL_ERROR "mod-gamebridge: protocol library not found at ${GAMEBRIDGE_PROTOCOL_DIR}. "
                      "Link the module from the azeroth-theft-auto checkout (scripts/link-module.sh).")
endif()

target_sources(modules PRIVATE
  "${GAMEBRIDGE_PROTOCOL_DIR}/src/json.cpp"
  "${GAMEBRIDGE_PROTOCOL_DIR}/src/framing.cpp"
  "${GAMEBRIDGE_PROTOCOL_DIR}/src/protocol.cpp"
  "${GAMEBRIDGE_PROTOCOL_DIR}/src/session.cpp"
  "${GAMEBRIDGE_PROTOCOL_DIR}/src/net.cpp"
  "${GAMEBRIDGE_PROTOCOL_DIR}/src/endpoint.cpp")
target_include_directories(modules PRIVATE "${GAMEBRIDGE_PROTOCOL_DIR}/include")
if(WIN32)
  target_link_libraries(modules PRIVATE ws2_32)
endif()
message(STATUS "mod-gamebridge: protocol library from ${GAMEBRIDGE_PROTOCOL_DIR}")
