cmake_minimum_required(VERSION 3.22)

project(PinkPawHeistEsp LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Pink Paw Heist ESP targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(AnomalySDK CONFIG REQUIRED)

set(PINK_PAW_HEIST_ESP_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package/PinkPawHeistESP")

anomaly_add_plugin(anomaly_pink_paw_heist_esp
    SOURCES plugin.cpp rob_bank_runtime.cpp
    MANIFEST manifest.json
    PACKAGE_NAME PinkPawHeistESP
    OUTPUT_DIRECTORY "${PINK_PAW_HEIST_ESP_PACKAGE_DIRECTORY}")

target_compile_definitions(anomaly_pink_paw_heist_esp PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    # The generated catalog contains UTF-8 Chinese display names.
    target_compile_options(anomaly_pink_paw_heist_esp PRIVATE /utf-8)
endif()

# anomaly_add_plugin copies the manifest only. Keep every package resource beside plugin.dll
# so the host's confined package-resource loader can resolve the relative paths below.
add_custom_command(TARGET anomaly_pink_paw_heist_esp POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:anomaly_pink_paw_heist_esp>/assets"
    VERBATIM)

install(TARGETS anomaly_pink_paw_heist_esp
    RUNTIME DESTINATION PinkPawHeistESP
    LIBRARY DESTINATION PinkPawHeistESP)
install(FILES manifest.json DESTINATION PinkPawHeistESP)
install(DIRECTORY assets/ DESTINATION PinkPawHeistESP/assets)
