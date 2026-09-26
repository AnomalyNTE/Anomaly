cmake_minimum_required(VERSION 3.22)

project(AnomalyNtePickupDemo LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NtePickupDemo targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)

set(NTE_PICKUP_DEMO_PACKAGE_DIRECTORY
    "${CMAKE_BINARY_DIR}/package/NtePickupDemo")

anomaly_add_plugin(anomaly_nte_pickup_demo
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME NtePickupDemo
    OUTPUT_DIRECTORY "${NTE_PICKUP_DEMO_PACKAGE_DIRECTORY}")

target_include_directories(anomaly_nte_pickup_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(anomaly_nte_pickup_demo PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# `anomaly_add_plugin` mirrors the manifest and the `locales` directory next to it into the
# package directory, and installs the package into Anomaly/plugins/NtePickupDemo.
