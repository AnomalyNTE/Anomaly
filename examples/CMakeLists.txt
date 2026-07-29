cmake_minimum_required(VERSION 3.22)

project(AnomalySDKExamples LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Anomaly SDK examples target Windows x64")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)

set(ANOMALY_EXAMPLE_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/packages")

anomaly_add_plugin(anomaly_example_hello_ui C_ONLY
    SOURCES hello_ui/plugin.c
    MANIFEST hello_ui/manifest.json
    PACKAGE_NAME HelloUi
    OUTPUT_DIRECTORY "${ANOMALY_EXAMPLE_OUTPUT_ROOT}/HelloUi")

anomaly_add_plugin(anomaly_example_tick_counter
    SOURCES tick_counter/plugin.cpp
    MANIFEST tick_counter/manifest.json
    PACKAGE_NAME TickCounter
    OUTPUT_DIRECTORY "${ANOMALY_EXAMPLE_OUTPUT_ROOT}/TickCounter")

anomaly_add_plugin(anomaly_example_reliable_config
    SOURCES reliable_config/plugin.cpp
    MANIFEST reliable_config/manifest.json
    PACKAGE_NAME ReliableConfig
    OUTPUT_DIRECTORY "${ANOMALY_EXAMPLE_OUTPUT_ROOT}/ReliableConfig")

anomaly_add_plugin(anomaly_example_nte_inspector
    SOURCES nte_inspector/plugin.cpp
    MANIFEST nte_inspector/manifest.json
    PACKAGE_NAME NteInspector
    OUTPUT_DIRECTORY "${ANOMALY_EXAMPLE_OUTPUT_ROOT}/NteInspector")

foreach(example_target IN ITEMS anomaly_example_hello_ui anomaly_example_tick_counter
        anomaly_example_reliable_config anomaly_example_nte_inspector)
    target_compile_definitions(
        ${example_target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endforeach()
