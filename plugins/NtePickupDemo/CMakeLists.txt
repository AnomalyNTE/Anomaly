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

add_custom_command(TARGET anomaly_nte_pickup_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/locales"
        "$<TARGET_FILE_DIR:anomaly_nte_pickup_demo>/locales")

install(TARGETS anomaly_nte_pickup_demo
    RUNTIME DESTINATION NtePickupDemo
    LIBRARY DESTINATION NtePickupDemo)
install(FILES manifest.json DESTINATION NtePickupDemo)
install(DIRECTORY locales/ DESTINATION NtePickupDemo/locales)
