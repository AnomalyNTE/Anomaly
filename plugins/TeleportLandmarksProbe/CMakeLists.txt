cmake_minimum_required(VERSION 3.22)

project(AnomalyTeleportLandmarksProbe LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "TeleportLandmarksProbe targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)

set(TELEPORT_LANDMARKS_PROBE_PACKAGE_DIRECTORY
    "${CMAKE_BINARY_DIR}/package/TeleportLandmarksProbe")

anomaly_add_plugin(anomaly_teleport_landmarks_probe
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME TeleportLandmarksProbe
    OUTPUT_DIRECTORY "${TELEPORT_LANDMARKS_PROBE_PACKAGE_DIRECTORY}")

target_compile_definitions(anomaly_teleport_landmarks_probe PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

install(TARGETS anomaly_teleport_landmarks_probe
    RUNTIME DESTINATION TeleportLandmarksProbe
    LIBRARY DESTINATION TeleportLandmarksProbe)
install(FILES manifest.json DESTINATION TeleportLandmarksProbe)
