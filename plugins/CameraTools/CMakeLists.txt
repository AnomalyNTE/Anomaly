cmake_minimum_required(VERSION 3.22)

project(AnomalyCameraTools LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Camera Tools targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

set(CAMERA_TOOLS_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package/CameraTools")

anomaly_add_plugin(anomaly_camera_tools
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME CameraTools
    OUTPUT_DIRECTORY "${CAMERA_TOOLS_PACKAGE_DIRECTORY}")

target_compile_definitions(anomaly_camera_tools PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(anomaly_camera_tools PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")

add_custom_command(TARGET anomaly_camera_tools POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/locales"
        "$<TARGET_FILE_DIR:anomaly_camera_tools>/locales")

install(TARGETS anomaly_camera_tools
    RUNTIME DESTINATION CameraTools
    LIBRARY DESTINATION CameraTools)
install(FILES manifest.json DESTINATION CameraTools)
install(DIRECTORY locales/ DESTINATION CameraTools/locales)
