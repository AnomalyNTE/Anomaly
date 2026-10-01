cmake_minimum_required(VERSION 3.22)

project(AnomalyRuntimeProfiler LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Runtime Profiler targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)

anomaly_add_plugin(anomaly_runtime_profiler
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME RuntimeProfiler)

target_compile_definitions(anomaly_runtime_profiler PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(anomaly_runtime_profiler PRIVATE
    psapi)
