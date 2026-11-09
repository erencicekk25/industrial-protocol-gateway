cmake_minimum_required(VERSION 3.16)

project(industrial_protocol_gateway
    VERSION 1.0.0
    DESCRIPTION "Educational Modbus TCP to MQTT industrial gateway simulation"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(IPG_BUILD_TESTS "Build the self-contained test suite" ON)
option(IPG_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan (GCC/Clang on Linux/macOS)" OFF)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Shared compiler warnings (applied to every target we own)
# ---------------------------------------------------------------------------
add_library(ipg_warnings INTERFACE)
if(MSVC)
    target_compile_options(ipg_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(ipg_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor)
endif()

if(IPG_ENABLE_SANITIZERS AND NOT MSVC AND NOT WIN32)
    target_compile_options(ipg_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ipg_warnings INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Core library: protocol stacks, simulator and orchestration
# ---------------------------------------------------------------------------
add_library(ipg_core STATIC
    src/Logger.cpp
    src/TimeUtils.cpp
    src/TcpSocket.cpp
    src/VirtualModbusServer.cpp
    src/ModbusClient.cpp
    src/MqttPublisher.cpp
    src/GatewayEngine.cpp
)

target_include_directories(ipg_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ipg_core PUBLIC cxx_std_17)
target_link_libraries(ipg_core
    PUBLIC Threads::Threads
    PRIVATE ipg_warnings)

if(WIN32)
    target_link_libraries(ipg_core PUBLIC ws2_32)
    target_compile_definitions(ipg_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
endif()

# ---------------------------------------------------------------------------
# Command line application
# ---------------------------------------------------------------------------
add_executable(industrial_gateway src/main.cpp)
target_link_libraries(industrial_gateway PRIVATE ipg_core ipg_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(IPG_BUILD_TESTS)
    enable_testing()
    add_executable(ipg_tests tests/GatewayTests.cpp)
    target_link_libraries(ipg_tests PRIVATE ipg_core ipg_warnings)
    add_test(NAME ipg_tests COMMAND ipg_tests)
endif()
