cmake_minimum_required(VERSION 3.22)

project(AnomalyNteNavmeshDemo LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "NteNavmeshDemo targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)

set(NTE_NAVMESH_DEMO_PACKAGE_DIRECTORY
    "${CMAKE_BINARY_DIR}/package/NteNavmeshDemo")

anomaly_add_plugin(anomaly_nte_navmesh_demo
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME NteNavmeshDemo
    OUTPUT_DIRECTORY "${NTE_NAVMESH_DEMO_PACKAGE_DIRECTORY}")

target_compile_definitions(anomaly_nte_navmesh_demo PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(anomaly_nte_navmesh_demo PRIVATE ntdll)

add_custom_command(TARGET anomaly_nte_navmesh_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/locales"
        "$<TARGET_FILE_DIR:anomaly_nte_navmesh_demo>/locales")

install(TARGETS anomaly_nte_navmesh_demo
    RUNTIME DESTINATION NteNavmeshDemo
    LIBRARY DESTINATION NteNavmeshDemo)
install(FILES manifest.json DESTINATION NteNavmeshDemo)
install(DIRECTORY locales/ DESTINATION NteNavmeshDemo/locales)
