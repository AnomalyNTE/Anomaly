cmake_minimum_required(VERSION 3.22)

project(AnomalyFreeCamera LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Free Camera targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

set(FREE_CAMERA_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package/FreeCamera")

anomaly_add_plugin(anomaly_free_camera
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME FreeCamera
    OUTPUT_DIRECTORY "${FREE_CAMERA_PACKAGE_DIRECTORY}")

target_compile_definitions(anomaly_free_camera PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(anomaly_free_camera PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")

add_custom_command(TARGET anomaly_free_camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/locales"
        "$<TARGET_FILE_DIR:anomaly_free_camera>/locales")

install(TARGETS anomaly_free_camera
    RUNTIME DESTINATION FreeCamera
    LIBRARY DESTINATION FreeCamera)
install(FILES manifest.json DESTINATION FreeCamera)
install(DIRECTORY locales/ DESTINATION FreeCamera/locales)
