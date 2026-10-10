cmake_minimum_required(VERSION 3.22)

# Standalone Jelly plugin project. It is deliberately NOT part of the main Anomaly
# build graph: it consumes the installed SDK through find_package(AnomalySDK) and
# owns its own package directory, so nothing here changes a framework source file.
#
#   cmake -S plugins/Jelly -B plugins/Jelly/build -A x64 \
#         -DAnomalySDK_DIR=<sdk>/lib/cmake/AnomalySDK
#   cmake --build plugins/Jelly/build --config RelWithDebInfo
project(AnomalyJelly LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Jelly targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(JELLY_BUILD_TESTS "Build the offline logic and model fixture" ON)

find_package(AnomalySDK CONFIG REQUIRED)

# The package directory is per configuration on purpose. anomaly_add_plugin points
# every configuration at one output path, and a Debug build then overwrites the
# release package that is about to be shipped -- the release target even stays
# "up to date" afterwards because its DLL is newer than its own objects. Keeping
# the configurations apart makes that unrepresentable.
set(JELLY_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package/$<CONFIG>/Jelly")

anomaly_add_plugin(anomaly_jelly
    SOURCES
        plugin.cpp
        src/jelly_layout.cpp
        src/jelly_selection.cpp
        src/jelly_model.cpp
    MANIFEST manifest.json
    PACKAGE_NAME Jelly
    OUTPUT_DIRECTORY "${JELLY_PACKAGE_DIRECTORY}")

target_include_directories(anomaly_jelly PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(anomaly_jelly PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# The same runtime the framework's own plugins use: the release build must not
# depend on the debug CRT, which does not exist outside a development machine.
set_target_properties(anomaly_jelly PROPERTIES
    MSVC_RUNTIME_LIBRARY
        "$<$<CONFIG:Debug>:MultiThreadedDebugDLL>$<$<NOT:$<CONFIG:Debug>>:MultiThreadedDLL>")
if(MSVC)
    # The source carries Chinese comments and diagnostics; without this the compiler
    # reads them through the local code page and warns about every file.
    target_compile_options(anomaly_jelly PRIVATE /utf-8)
    # From Anomaly 2.3.0 on, plugins are mapped by the runtime's own image mapper
    # instead of the Windows loader. That mapper cannot reserve the loader-managed TLS
    # the thread-safe function-local static guard needs, so the guard is turned off, and
    # any C++ image that imports _CxxThrowException has to export the SDK exception
    # bridge the mapper rebinds that import to. A newer installed SDK does both of these
    # by itself: it sets ANOMALY_CXX_THROW_BRIDGE and adds /Zc:threadSafeInit-, so the
    # bridge source is only added when the installed SDK predates it.
    target_compile_options(anomaly_jelly PRIVATE /Zc:threadSafeInit-)
    if(NOT ANOMALY_CXX_THROW_BRIDGE)
        target_sources(anomaly_jelly PRIVATE src/plugin_cxx_throw_bridge.cpp)
    endif()
endif()

# The layout resource is plugin data, so it is not covered by anomaly_add_plugin's
# manifest and locales mirroring and has to be staged into the package explicitly.
add_custom_command(TARGET anomaly_jelly POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:anomaly_jelly>/data"
    VERBATIM)
install(DIRECTORY data/ DESTINATION Anomaly/plugins/Jelly/data COMPONENT GameRuntime)

if(JELLY_BUILD_TESTS)
    # Offline fixture: the pure logic plus the model module driven by a synthetic
    # UE image. It needs no SDK, no game and no network.
    add_executable(jelly_logic_tests
        tests/main.cpp
        tests/test_logic.cpp
        tests/test_model.cpp
        src/jelly_layout.cpp
        src/jelly_selection.cpp
        src/jelly_model.cpp)
    target_include_directories(jelly_logic_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(jelly_logic_tests PRIVATE
        JELLY_LAYOUT_RESOURCE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/jelly-layout.json"
        WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(
            jelly_logic_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
endif()
