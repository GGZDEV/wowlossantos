# Azeroth Theft Auto - standalone protocol library, transport tests and bridge-cli.
# This does NOT build AzerothCore or the GTA adapter:
#   - mod-gamebridge is built inside AzerothCore's own module system (see modules/mod-gamebridge/README.md)
#   - the GTA SA adapter is a Win32 ASI built with the Plugin-SDK workflow (see adapters/gta-sa/README.md)
cmake_minimum_required(VERSION 3.16)
project(AzerothTheftAuto LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(gamebridge_protocol STATIC
    protocol/src/json.cpp
    protocol/src/framing.cpp
    protocol/src/protocol.cpp
    protocol/src/session.cpp
    protocol/src/net.cpp
    protocol/src/endpoint.cpp
)
target_include_directories(gamebridge_protocol PUBLIC protocol/include)
target_link_libraries(gamebridge_protocol PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(gamebridge_protocol PUBLIC ws2_32)
endif()
if(MSVC)
    target_compile_options(gamebridge_protocol PRIVATE /W4)
else()
    target_compile_options(gamebridge_protocol PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(ATA_BUILD_TOOLS "Build bridge-cli" ON)
if(ATA_BUILD_TOOLS)
    add_executable(bridge-cli tools/bridge-cli/main.cpp)
    target_link_libraries(bridge-cli PRIVATE gamebridge_protocol)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol_tests tests/protocol/test_main.cpp tests/protocol/test_protocol.cpp tests/protocol/test_endpoint.cpp)
    target_include_directories(protocol_tests PRIVATE tests/protocol)
    target_link_libraries(protocol_tests PRIVATE gamebridge_protocol)
    add_test(NAME protocol_tests COMMAND protocol_tests)
    set_tests_properties(protocol_tests PROPERTIES TIMEOUT 120)

    # GTA adapter logic (Plugin-SDK independent part) against a real loopback endpoint.
    add_executable(adapter_tests tests/protocol/test_main.cpp tests/adapter/test_adapter.cpp adapters/gta-sa/src/AdapterCore.cpp)
    target_include_directories(adapter_tests PRIVATE tests/protocol adapters/gta-sa/src)
    target_link_libraries(adapter_tests PRIVATE gamebridge_protocol)
    add_test(NAME adapter_tests COMMAND adapter_tests)
    set_tests_properties(adapter_tests PROPERTIES TIMEOUT 120)
endif()
