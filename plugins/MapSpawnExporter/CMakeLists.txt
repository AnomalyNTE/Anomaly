cmake_minimum_required(VERSION 3.22)

project(AnomalyMapSpawnExporter LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MapSpawnExporter targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)

anomaly_add_plugin(anomaly_map_spawn_exporter
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME MapSpawnExporter
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/MapSpawnExporter")

target_include_directories(anomaly_map_spawn_exporter BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(anomaly_map_spawn_exporter PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# `anomaly_add_plugin` installs the package into Anomaly/plugins/MapSpawnExporter.
