cmake_minimum_required(VERSION 3.22)

set(ANOMALY_RELEASE_VERSION "1.0.0" CACHE STRING
    "Release version used to configure Anomaly")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/anomaly_version.cmake")
anomaly_resolve_version("${ANOMALY_RELEASE_VERSION}"
    ANOMALY_PROJECT_VERSION ANOMALY_VERSION_STRING)
project(Anomaly VERSION "${ANOMALY_PROJECT_VERSION}" LANGUAGES C CXX)

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(ANOMALY_GENERATED_INCLUDE_DIR "${GENERATED_DIR}/sdk-build/include")
set(ANOMALY_GENERATED_INSTALL_INCLUDE_DIR "${GENERATED_DIR}/sdk-install/include")
file(MAKE_DIRECTORY "${ANOMALY_GENERATED_INCLUDE_DIR}/anomaly/sdk")
file(MAKE_DIRECTORY "${ANOMALY_GENERATED_INSTALL_INCLUDE_DIR}/anomaly/sdk")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${ANOMALY_GENERATED_INCLUDE_DIR}/anomaly/sdk/configured_version.h"
    @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${ANOMALY_GENERATED_INSTALL_INCLUDE_DIR}/anomaly/sdk/version.h"
    @ONLY)
# In-tree SDK consumers include headers through several internal targets; keep
# the configured release data available to the source-tree version header.
include_directories(BEFORE "${ANOMALY_GENERATED_INCLUDE_DIR}")
add_compile_definitions(ANOMALY_USE_CONFIGURED_VERSION=1)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Anomaly currently targets Windows x64 only")
endif()

enable_language(ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_property(ANOMALY_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(ANOMALY_IS_MULTI_CONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

option(ANOMALY_ENABLE_ASAN "Instrument Anomaly targets with MSVC AddressSanitizer" OFF)
if(ANOMALY_ENABLE_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "ANOMALY_ENABLE_ASAN currently requires MSVC")
    endif()
    add_compile_definitions(ANOMALY_ENABLE_ASAN=1)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C>:/fsanitize=address>
        $<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address>)
    add_link_options(/INCREMENTAL:NO)
    get_filename_component(ANOMALY_ASAN_RUNTIME_DIRECTORY
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(ANOMALY_ASAN_RUNTIME_DLL
        "${ANOMALY_ASAN_RUNTIME_DIRECTORY}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${ANOMALY_ASAN_RUNTIME_DLL}")
        message(FATAL_ERROR
            "MSVC AddressSanitizer runtime was not found next to the configured compiler: "
            "${ANOMALY_ASAN_RUNTIME_DLL}")
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(anomaly_sdk INTERFACE)
add_library(Anomaly::sdk ALIAS anomaly_sdk)
set_target_properties(anomaly_sdk PROPERTIES EXPORT_NAME sdk)
target_compile_features(anomaly_sdk INTERFACE c_std_11 cxx_std_20)
target_compile_definitions(anomaly_sdk INTERFACE
    $<BUILD_INTERFACE:ANOMALY_USE_CONFIGURED_VERSION=1>)
target_include_directories(anomaly_sdk INTERFACE
    $<BUILD_INTERFACE:${ANOMALY_GENERATED_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/AnomalyPlugin.cmake")

include(FetchContent)
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(minhook)

set(JSON_BuildTests OFF)
set(JSON_Install OFF)
set(JSON_MultipleHeaders OFF)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
unset(JSON_BuildTests)
unset(JSON_Install)
unset(JSON_MultipleHeaders)

set(JSON_VALIDATOR_INSTALL OFF)
set(JSON_VALIDATOR_BUILD_TESTS OFF)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF)
set(JSON_VALIDATOR_SHARED_LIBS OFF)
FetchContent_Declare(
    json_schema_validator
    URL https://github.com/pboettch/json-schema-validator/archive/refs/tags/2.3.0.tar.gz
    URL_HASH SHA256=2c00b50023c7d557cdaa71c0777f5bcff996c4efd7a539e58beaa4219fa2a5e1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json_schema_validator)
unset(JSON_VALIDATOR_INSTALL)
unset(JSON_VALIDATOR_BUILD_TESTS)
unset(JSON_VALIDATOR_BUILD_EXAMPLES)
unset(JSON_VALIDATOR_SHARED_LIBS)

# miniz provides the ZIP inflate used to install Dalamud-style plugin packages.
FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniz)
add_library(anomaly_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(anomaly_miniz PUBLIC "${miniz_SOURCE_DIR}")
if(MSVC)
    target_compile_options(anomaly_miniz PRIVATE /w)
endif()

add_library(anomaly_imgui STATIC
    src/ui/host_ui_service.cpp
    src/ui/platform_ui_theme.cpp
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
)
target_include_directories(anomaly_imgui PUBLIC "${imgui_SOURCE_DIR}" PRIVATE include)
target_compile_definitions(anomaly_imgui PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_imgui PRIVATE /W3 /EHsc)
target_link_libraries(anomaly_imgui PUBLIC d3d11 d3d12 dxgi dxguid)

add_executable(proxy_generator tools/proxy_generator.cpp)
target_compile_definitions(proxy_generator PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

set(PROXY_DEF "${GENERATED_DIR}/dwmapi.def")
set(PROXY_ASM "${GENERATED_DIR}/dwmapi.asm")
set(PROXY_CPP "${GENERATED_DIR}/proxy_exports.cpp")
# This prefiltered 32px derivative keeps the one-level header texture close
# to its display size without imposing mip generation on generic UI assets.
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/logo-header.png" ANOMALY_PLATFORM_LOGO_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/logo.png" ANOMALY_ESC_MENU_LOGO_PATH)
file(TO_CMAKE_PATH
    "${CMAKE_SOURCE_DIR}/apps/launcher/AnomalyLauncher.ico"
    ANOMALY_LAUNCHER_ICON_PATH)
set(PLATFORM_UI_RESOURCES_RC "${GENERATED_DIR}/platform_ui_resources.rc")
set(LAUNCHER_RESOURCES_RC "${GENERATED_DIR}/AnomalyLauncher.rc")

file(MAKE_DIRECTORY "${GENERATED_DIR}")
set(ANOMALY_HOST_EN_US_CATALOG_FILE
    "${CMAKE_SOURCE_DIR}/locales/host/en-US.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ANOMALY_HOST_EN_US_CATALOG_FILE}")
file(READ "${ANOMALY_HOST_EN_US_CATALOG_FILE}" ANOMALY_HOST_EN_US_CATALOG_JSON)
set(ANOMALY_HOST_EN_US_CATALOG_LITERALS "")
string(LENGTH "${ANOMALY_HOST_EN_US_CATALOG_JSON}"
    ANOMALY_HOST_EN_US_CATALOG_LENGTH)
set(ANOMALY_HOST_EN_US_CATALOG_OFFSET 0)
while(ANOMALY_HOST_EN_US_CATALOG_OFFSET LESS ANOMALY_HOST_EN_US_CATALOG_LENGTH)
    math(EXPR ANOMALY_HOST_EN_US_CATALOG_REMAINING
        "${ANOMALY_HOST_EN_US_CATALOG_LENGTH} - ${ANOMALY_HOST_EN_US_CATALOG_OFFSET}")
    if(ANOMALY_HOST_EN_US_CATALOG_REMAINING GREATER 8000)
        set(ANOMALY_HOST_EN_US_CATALOG_CHUNK_LENGTH 8000)
    else()
        set(ANOMALY_HOST_EN_US_CATALOG_CHUNK_LENGTH
            ${ANOMALY_HOST_EN_US_CATALOG_REMAINING})
    endif()
    string(SUBSTRING "${ANOMALY_HOST_EN_US_CATALOG_JSON}"
        ${ANOMALY_HOST_EN_US_CATALOG_OFFSET}
        ${ANOMALY_HOST_EN_US_CATALOG_CHUNK_LENGTH}
        ANOMALY_HOST_EN_US_CATALOG_CHUNK)
    string(APPEND ANOMALY_HOST_EN_US_CATALOG_LITERALS
        "    R\"anomaly_catalog(${ANOMALY_HOST_EN_US_CATALOG_CHUNK})anomaly_catalog\"\n")
    math(EXPR ANOMALY_HOST_EN_US_CATALOG_OFFSET
        "${ANOMALY_HOST_EN_US_CATALOG_OFFSET} + ${ANOMALY_HOST_EN_US_CATALOG_CHUNK_LENGTH}")
endwhile()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/i18n_embedded_catalog.hpp.in"
    "${GENERATED_DIR}/anomaly/i18n_embedded_catalog.hpp"
    @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/schemas/plugin-manifest.schema.json"
    "${CMAKE_SOURCE_DIR}/schemas/build-profile.schema.json"
    "${CMAKE_SOURCE_DIR}/schemas/repository-cache.schema.json"
    "${CMAKE_SOURCE_DIR}/schemas/repository-config.schema.json"
    "${CMAKE_SOURCE_DIR}/schemas/repository-index.schema.json"
    "${CMAKE_SOURCE_DIR}/logo-header.png"
    "${CMAKE_SOURCE_DIR}/logo.png"
    "${CMAKE_SOURCE_DIR}/apps/launcher/AnomalyLauncher.ico")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/platform_ui_resources.rc.in"
    "${PLATFORM_UI_RESOURCES_RC}"
    @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/apps/launcher/AnomalyLauncher.rc.in"
    "${LAUNCHER_RESOURCES_RC}"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/plugin-manifest.schema.json"
    ANOMALY_PLUGIN_MANIFEST_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/plugin_manifest_schema.hpp.in"
    "${GENERATED_DIR}/plugin_manifest_schema.hpp"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/build-profile.schema.json"
    ANOMALY_BUILD_PROFILE_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/build_profile_schema.hpp.in"
    "${GENERATED_DIR}/build_profile_schema.hpp"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/repository-index.schema.json"
    ANOMALY_REPOSITORY_INDEX_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/repository_index_schema.hpp.in"
    "${GENERATED_DIR}/repository_index_schema.hpp"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/repository-config.schema.json"
    ANOMALY_REPOSITORY_CONFIG_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/repository_config_schema.hpp.in"
    "${GENERATED_DIR}/repository_config_schema.hpp"
    @ONLY)
file(READ
    "${CMAKE_SOURCE_DIR}/schemas/repository-cache.schema.json"
    ANOMALY_REPOSITORY_CACHE_SCHEMA_JSON)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/repository_cache_schema.hpp.in"
    "${GENERATED_DIR}/repository_cache_schema.hpp"
    @ONLY)
set(ANOMALY_SOURCE_COMMIT "")
set(ANOMALY_SOURCE_DIRTY true)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE ANOMALY_GIT_REVISION_RESULT
        OUTPUT_VARIABLE ANOMALY_SOURCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE ANOMALY_GIT_STATUS_RESULT
        OUTPUT_VARIABLE ANOMALY_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ANOMALY_GIT_REVISION_RESULT EQUAL 0 AND
       ANOMALY_GIT_STATUS_RESULT EQUAL 0 AND
       ANOMALY_GIT_STATUS STREQUAL "")
        set(ANOMALY_SOURCE_DIRTY false)
    endif()
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/build_provenance.hpp.in"
    "${GENERATED_DIR}/build_provenance.hpp"
    @ONLY)

add_custom_command(
    OUTPUT "${PROXY_DEF}" "${PROXY_ASM}" "${PROXY_CPP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND $<TARGET_FILE:proxy_generator>
            "${CMAKE_SOURCE_DIR}/config/dwmapi.exports" "${GENERATED_DIR}"
    DEPENDS proxy_generator "${CMAKE_SOURCE_DIR}/config/dwmapi.exports"
    VERBATIM
)

add_library(ue5mem_memory STATIC
    src/platform/windows/memory.cpp
    src/platform/windows/pattern.cpp)
target_include_directories(ue5mem_memory PUBLIC include)
target_compile_definitions(
    ue5mem_memory PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ue5mem_memory PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(ue5mem_memory PUBLIC Psapi)

add_library(anomaly_i18n STATIC src/i18n/i18n.cpp)
target_include_directories(
    anomaly_i18n PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_i18n PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_i18n PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_i18n PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_config STATIC src/platform/windows/config.cpp)
target_include_directories(anomaly_config PUBLIC include)
target_compile_definitions(
    anomaly_config PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_config PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_config PUBLIC anomaly_i18n)

add_library(anomaly_bootstrap_client STATIC src/bootstrap/bootstrap_client.cpp)
target_include_directories(anomaly_bootstrap_client PUBLIC include)
target_compile_definitions(
    anomaly_bootstrap_client PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_bootstrap_client PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_internal_memory_services STATIC
    src/platform/windows/module_memory_service.cpp
    src/platform/windows/pattern_service.cpp)
target_include_directories(anomaly_internal_memory_services PUBLIC include)
target_compile_definitions(
    anomaly_internal_memory_services PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_internal_memory_services PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_internal_memory_services PUBLIC ue5mem_memory)

add_library(anomaly_build_profile STATIC src/game/ue5/build_profile.cpp)
target_include_directories(
    anomaly_build_profile PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_build_profile PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_build_profile PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_build_profile
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator
            ue5mem_memory bcrypt Version)

add_library(anomaly_udp_packet_parser STATIC src/diagnostics/udp_packet_parser.cpp)
target_include_directories(anomaly_udp_packet_parser PUBLIC include)
target_compile_definitions(
    anomaly_udp_packet_parser PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_udp_packet_parser PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_npcap_capture STATIC src/diagnostics/npcap_capture.cpp)
target_include_directories(anomaly_npcap_capture PUBLIC include)
target_compile_definitions(
    anomaly_npcap_capture PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_npcap_capture PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_npcap_capture PUBLIC Ws2_32)

add_library(anomaly_ue5_bit_reader STATIC src/game/ue5/ue5_bit_reader.cpp)
target_include_directories(anomaly_ue5_bit_reader PUBLIC include)
target_compile_definitions(
    anomaly_ue5_bit_reader PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_ue5_bit_reader PRIVATE /W4 /permissive- /EHsc)

add_library(
    anomaly_ue5_network_deserializer STATIC src/game/ue5/ue5_network_deserializer.cpp)
target_include_directories(anomaly_ue5_network_deserializer PUBLIC include)
target_compile_definitions(
    anomaly_ue5_network_deserializer PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_ue5_network_deserializer PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_ue5_network_deserializer PUBLIC anomaly_ue5_bit_reader)
target_link_libraries(anomaly_build_profile PUBLIC anomaly_ue5_network_deserializer)

add_library(anomaly_symbol_resolver STATIC src/game/ue5/symbol_resolver.cpp)
target_include_directories(anomaly_symbol_resolver PUBLIC include)
target_compile_definitions(
    anomaly_symbol_resolver PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_symbol_resolver PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_symbol_resolver PUBLIC anomaly_build_profile anomaly_internal_memory_services)

add_library(anomaly_ue5_reflection_query STATIC src/game/ue5/ue5_reflection_query.cpp)
target_include_directories(anomaly_ue5_reflection_query PUBLIC include)
target_compile_definitions(
    anomaly_ue5_reflection_query PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_ue5_reflection_query PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_ue5_reflection_query PUBLIC anomaly_symbol_resolver)

add_library(anomaly_ue5_network_profile STATIC src/game/ue5/ue5_network_profile.cpp)
target_include_directories(anomaly_ue5_network_profile PUBLIC include)
target_compile_definitions(
    anomaly_ue5_network_profile PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_ue5_network_profile PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_ue5_network_profile
    PUBLIC anomaly_symbol_resolver anomaly_ue5_network_deserializer)

add_library(anomaly_adapter_services STATIC
    src/game/ue5/adapter_service_registry.cpp
    src/game/ue5/ue5_object_lookup.cpp
    src/game/ue5/ue5_process_event.cpp
    src/game/ue5/ue5_process_event_hook.cpp
    src/game/ue5/ue5_nte_adapter.cpp
    src/game/nte/nte_navigation_input_policy.cpp
    src/game/ue5/game_tick_hook.cpp
    src/game/ue5/ue5_outbound_bit_count_probe.cpp)
target_include_directories(anomaly_adapter_services PUBLIC include)
target_compile_definitions(
    anomaly_adapter_services PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_adapter_services PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_adapter_services
    PUBLIC anomaly_symbol_resolver anomaly_hook_manager anomaly_minhook_backend)

add_library(anomaly_websocket_server STATIC src/platform/windows/websocket_server.cpp)
target_include_directories(anomaly_websocket_server PUBLIC include)
target_compile_definitions(
    anomaly_websocket_server PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_websocket_server PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_websocket_server PUBLIC Ws2_32 bcrypt)

add_library(anomaly_nte_profile_runtime STATIC
    src/game/nte/nte_profile_runtime.cpp)
target_include_directories(anomaly_nte_profile_runtime PUBLIC include)
target_compile_definitions(
    anomaly_nte_profile_runtime PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_nte_profile_runtime PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_nte_profile_runtime PUBLIC anomaly_adapter_services anomaly_ue5_reflection_query)

add_library(ue5mem_core STATIC
    src/diagnostics/analyzer.cpp
    src/diagnostics/diagnostic_pipe_service.cpp
    src/diagnostics/json.cpp
    src/diagnostics/pipe_server.cpp
    src/runtime/service_graph_diagnostics.cpp)
target_include_directories(ue5mem_core PUBLIC include)
target_compile_definitions(ue5mem_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ue5mem_core PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    ue5mem_core
    PUBLIC anomaly_config anomaly_internal_memory_services ue5mem_memory Advapi32
    PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_runtime_kernel STATIC src/runtime/runtime_session.cpp)
target_include_directories(anomaly_runtime_kernel PUBLIC include)
target_compile_definitions(
    anomaly_runtime_kernel PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_runtime_kernel PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_dispatcher STATIC src/runtime/dispatcher.cpp)
target_include_directories(anomaly_dispatcher PUBLIC include)
target_compile_definitions(
    anomaly_dispatcher PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_dispatcher PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_runtime_dispatchers STATIC src/runtime/runtime_dispatchers.cpp)
target_include_directories(anomaly_runtime_dispatchers PUBLIC include)
target_compile_definitions(
    anomaly_runtime_dispatchers PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_runtime_dispatchers PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_runtime_dispatchers PUBLIC anomaly_dispatcher)

add_library(anomaly_structured_logger STATIC src/services/structured_logger.cpp)
target_include_directories(anomaly_structured_logger PUBLIC include)
target_compile_definitions(
    anomaly_structured_logger PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_structured_logger PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_crash_reporter STATIC src/diagnostics/crash_reporter.cpp)
target_include_directories(anomaly_crash_reporter PUBLIC include)
target_compile_definitions(
    anomaly_crash_reporter PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_crash_reporter PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_crash_reporter PUBLIC Dbghelp)

add_library(anomaly_reliable_storage STATIC src/services/reliable_storage.cpp)
target_include_directories(anomaly_reliable_storage PUBLIC include)
target_compile_definitions(
    anomaly_reliable_storage PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_reliable_storage PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_runtime_recovery STATIC src/diagnostics/runtime_recovery.cpp)
target_include_directories(anomaly_runtime_recovery PUBLIC include)
target_compile_definitions(
    anomaly_runtime_recovery PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_runtime_recovery PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_runtime_recovery
    PUBLIC anomaly_reliable_storage anomaly_semver
    PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_runtime_crash_coordinator STATIC
    src/diagnostics/runtime_crash_coordinator.cpp)
target_include_directories(anomaly_runtime_crash_coordinator PUBLIC include)
target_compile_definitions(
    anomaly_runtime_crash_coordinator PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_runtime_crash_coordinator PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_runtime_crash_coordinator
    PUBLIC anomaly_runtime_recovery anomaly_reliable_storage
    PRIVATE nlohmann_json::nlohmann_json)

add_executable(anomaly_crash_coordinator apps/crash_coordinator/main.cpp)
set_target_properties(anomaly_crash_coordinator PROPERTIES
    OUTPUT_NAME "AnomalyCrashCoordinator"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Anomaly")
target_include_directories(anomaly_crash_coordinator PRIVATE include)
target_compile_definitions(
    anomaly_crash_coordinator PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_crash_coordinator PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_crash_coordinator PRIVATE anomaly_runtime_crash_coordinator)

add_library(anomaly_artifact_crypto STATIC src/repository/artifact_crypto.cpp)
target_include_directories(anomaly_artifact_crypto PUBLIC include)
target_compile_definitions(
    anomaly_artifact_crypto PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_artifact_crypto PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_artifact_crypto PUBLIC bcrypt)

add_library(anomaly_launcher_proxy STATIC src/launcher/proxy_installation.cpp)
target_include_directories(anomaly_launcher_proxy PUBLIC include)
target_compile_definitions(
    anomaly_launcher_proxy PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_launcher_proxy PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_launcher_proxy PUBLIC anomaly_artifact_crypto)

add_library(anomaly_launcher_configuration STATIC src/launcher/configuration.cpp)
target_include_directories(anomaly_launcher_configuration PUBLIC include)
target_compile_definitions(
    anomaly_launcher_configuration PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_launcher_configuration PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_launcher_configuration
    PRIVATE nlohmann_json::nlohmann_json Advapi32)

add_library(anomaly_manual_map STATIC
    src/launcher/manual_map.cpp
    src/launcher/manual_map_remote.asm)
target_include_directories(anomaly_manual_map PUBLIC include)
target_compile_definitions(
    anomaly_manual_map PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_manual_map PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc>)
target_link_libraries(anomaly_manual_map PUBLIC Advapi32 Psapi)

add_library(anomaly_semver STATIC src/plugin/semver.cpp)
target_include_directories(anomaly_semver PUBLIC include)
target_compile_definitions(
    anomaly_semver PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_semver PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_plugin_manifest STATIC
    src/plugin/plugin_manifest.cpp
    src/plugin/plugin_capability_policy.cpp)
target_include_directories(
    anomaly_plugin_manifest PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_plugin_manifest PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_manifest PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_plugin_manifest
    PUBLIC anomaly_semver
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

add_library(anomaly_plugin_compatibility STATIC src/plugin/plugin_compatibility.cpp)
target_include_directories(anomaly_plugin_compatibility PUBLIC include)
target_compile_definitions(
    anomaly_plugin_compatibility PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_compatibility PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_plugin_compatibility PUBLIC anomaly_plugin_manifest)

add_library(anomaly_plugin_package STATIC src/plugin/plugin_package.cpp)
target_include_directories(anomaly_plugin_package PUBLIC include)
target_compile_definitions(
    anomaly_plugin_package PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_package PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_artifact_bundle STATIC src/repository/artifact_bundle.cpp)
target_include_directories(anomaly_artifact_bundle PUBLIC include)
target_compile_definitions(
    anomaly_artifact_bundle PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_artifact_bundle PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_artifact_bundle PUBLIC anomaly_artifact_crypto anomaly_plugin_package)

add_library(anomaly_repository_index STATIC src/repository/repository_index.cpp)
target_include_directories(
    anomaly_repository_index PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_repository_index PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_index PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_repository_index
    PUBLIC anomaly_semver anomaly_artifact_crypto
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

add_library(anomaly_repository_configuration STATIC
    src/repository/repository_configuration.cpp)
target_include_directories(
    anomaly_repository_configuration PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_repository_configuration PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_configuration PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_repository_configuration
    PUBLIC anomaly_repository_index
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

add_library(anomaly_repository_policy STATIC
    src/repository/repository_policy.cpp)
target_include_directories(
    anomaly_repository_policy PUBLIC include PRIVATE "${GENERATED_DIR}")
target_compile_definitions(
    anomaly_repository_policy PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_policy PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_repository_policy
    PUBLIC anomaly_repository_configuration
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

add_library(anomaly_runtime_launch STATIC
    src/repository/runtime_launch.cpp)
target_include_directories(anomaly_runtime_launch PUBLIC include)
target_compile_definitions(
    anomaly_runtime_launch PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_runtime_launch PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_update_transaction STATIC src/repository/update_transaction.cpp)
target_include_directories(anomaly_update_transaction PUBLIC include)
target_compile_definitions(
    anomaly_update_transaction PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_update_transaction PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_update_transaction PUBLIC
    anomaly_repository_index anomaly_artifact_bundle anomaly_build_profile anomaly_plugin_manifest
    PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_repository_network STATIC src/repository/repository_network.cpp)
target_include_directories(anomaly_repository_network PUBLIC include)
target_compile_definitions(
    anomaly_repository_network PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_network PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_repository_network PUBLIC winhttp Shlwapi)

add_library(anomaly_repository_coordinator STATIC
    src/repository/repository_coordinator.cpp
    src/repository/plugin_repository_config.cpp
    src/repository/plugin_list.cpp
    src/repository/safe_zip.cpp)
target_include_directories(anomaly_repository_coordinator PUBLIC include)
target_compile_definitions(
    anomaly_repository_coordinator PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_repository_coordinator PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_repository_coordinator
    PUBLIC anomaly_repository_network anomaly_semver anomaly_miniz
        nlohmann_json::nlohmann_json
    PRIVATE anomaly_artifact_crypto anomaly_plugin_manifest anomaly_plugin_package)

add_library(anomaly_plugin_catalog STATIC src/plugin/plugin_catalog.cpp)
target_include_directories(anomaly_plugin_catalog PUBLIC include)
target_compile_definitions(
    anomaly_plugin_catalog PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_catalog PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_plugin_catalog PUBLIC anomaly_plugin_compatibility anomaly_plugin_package)

add_library(anomaly_plugin_dependency_resolver STATIC src/plugin/plugin_dependency_resolver.cpp)
target_include_directories(anomaly_plugin_dependency_resolver PUBLIC include)
target_compile_definitions(
    anomaly_plugin_dependency_resolver PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_dependency_resolver PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_plugin_dependency_resolver PUBLIC anomaly_plugin_catalog)

add_library(anomaly_plugin_enablement STATIC src/plugin/plugin_enablement.cpp)
target_include_directories(anomaly_plugin_enablement PUBLIC include)
target_compile_definitions(
    anomaly_plugin_enablement PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_enablement PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_plugin_enablement
    PUBLIC anomaly_plugin_catalog PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_plugin_native_dependency STATIC src/plugin/plugin_native_dependency.cpp)
target_include_directories(anomaly_plugin_native_dependency PUBLIC include)
target_compile_definitions(
    anomaly_plugin_native_dependency PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_native_dependency PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_plugin_native_dependency PUBLIC Psapi)

add_library(anomaly_plugin_shadow STATIC
    src/plugin/plugin_shadow_store.cpp
    src/plugin/plugin_file_watcher.cpp)
target_include_directories(anomaly_plugin_shadow PUBLIC include)
target_compile_definitions(
    anomaly_plugin_shadow PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_shadow PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_plugin_shadow PUBLIC anomaly_plugin_catalog anomaly_plugin_native_dependency)

add_library(anomaly_plugin_runtime STATIC
    src/plugin/ipc_registry.cpp
    src/plugin/plugin_scope.cpp
    src/plugin/plugin_runtime.cpp)
target_include_directories(anomaly_plugin_runtime PUBLIC include)
target_compile_definitions(
    anomaly_plugin_runtime PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_plugin_runtime PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_ui_resources STATIC
    src/ui/input_service.cpp
    src/ui/ui_resource_decoder.cpp
    src/ui/ui_resource_registry.cpp)
target_include_directories(anomaly_ui_resources PUBLIC include)
target_compile_definitions(
    anomaly_ui_resources PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_ui_resources PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_ui_resources PUBLIC anomaly_plugin_runtime ole32)

add_library(anomaly_hook_manager STATIC src/hook/hook_manager.cpp)
target_include_directories(anomaly_hook_manager PUBLIC include)
target_compile_definitions(
    anomaly_hook_manager PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_hook_manager PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_hook_manager PUBLIC anomaly_plugin_runtime)

add_library(anomaly_ui_service STATIC src/ui/ui_service_registry.cpp)
target_include_directories(anomaly_ui_service PUBLIC include)
target_compile_definitions(
    anomaly_ui_service PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_ui_service PRIVATE /W4 /permissive- /EHsc)

add_library(anomaly_platform_ui_model STATIC src/ui/platform_ui_model.cpp)
target_include_directories(anomaly_platform_ui_model PUBLIC include)
target_compile_definitions(
    anomaly_platform_ui_model PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_platform_ui_model PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_platform_ui_model PUBLIC anomaly_plugin_dependency_resolver
    anomaly_plugin_enablement anomaly_repository_coordinator)

add_library(anomaly_platform_settings STATIC src/ui/platform_settings.cpp)
target_include_directories(anomaly_platform_settings PUBLIC include)
target_compile_definitions(
    anomaly_platform_settings PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_platform_settings PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_platform_settings PUBLIC anomaly_reliable_storage
    PRIVATE nlohmann_json::nlohmann_json)

add_library(anomaly_minhook_backend STATIC src/hook/minhook_backend.cpp)
target_include_directories(anomaly_minhook_backend PUBLIC include)
target_compile_definitions(
    anomaly_minhook_backend PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_minhook_backend PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_minhook_backend PUBLIC anomaly_hook_manager minhook)

add_library(anomaly_scoped_platform STATIC src/plugin/scoped_platform_services.cpp)
target_include_directories(anomaly_scoped_platform PUBLIC include)
target_compile_definitions(
    anomaly_scoped_platform PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_scoped_platform PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_scoped_platform
    PUBLIC anomaly_internal_memory_services anomaly_reliable_storage anomaly_minhook_backend
    PRIVATE nlohmann_json::nlohmann_json nlohmann_json_schema_validator::validator)

add_library(anomaly_service_graph STATIC src/runtime/service_graph.cpp)
target_include_directories(anomaly_service_graph PUBLIC include)
target_compile_definitions(
    anomaly_service_graph PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_service_graph PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_runtime_kernel PUBLIC anomaly_service_graph anomaly_runtime_dispatchers)

set(ANOMALY_STAGED_NTE_PROFILE_DIRECTORY
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Anomaly/profiles/nte")
add_custom_target(anomaly_stage_nte_profiles ALL
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "${ANOMALY_STAGED_NTE_PROFILE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/profiles/nte"
            "${ANOMALY_STAGED_NTE_PROFILE_DIRECTORY}"
    COMMENT "Synchronizing bundled NTE Profiles"
    VERBATIM)

add_library(anomaly_core SHARED
    src/runtime/core_main.cpp
    src/runtime/manual_map_exception.cpp
    src/ui/platform_host.cpp
    "${PLATFORM_UI_RESOURCES_RC}"
    src/render/dx12/embedded_bridge.cpp
    src/game/nte/nte_esc_menu_bridge.cpp
    src/render/dx12/embedded_discovery.cpp
    src/render/dx12/embedded_input.cpp
    src/render/dx12/embedded_renderer.cpp
    src/render/dx12/embedded_ui_resource_render_backend.cpp
    src/ui/embedded_ui.cpp
    src/plugin/plugin_manager.cpp
)
set_target_properties(anomaly_core PROPERTIES
    OUTPUT_NAME "Anomaly.Core"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Anomaly"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/Anomaly")
target_include_directories(anomaly_core PRIVATE include src "${GENERATED_DIR}")
target_compile_definitions(anomaly_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_core PRIVATE /W4 /permissive- /EHsc)

# The Core is manually mapped and cannot use MSVC's loader-managed TLS slot for
# function-local static guards. These targets initialize their process-scoped
# local statics on the lifecycle thread before publishing concurrent services.
set(ANOMALY_MANUAL_MAP_TLS_FREE_TARGETS
    anomaly_core
    anomaly_i18n
    anomaly_adapter_services
    anomaly_imgui
    anomaly_nte_profile_runtime
    anomaly_plugin_manifest
    anomaly_reliable_storage
    anomaly_repository_configuration
    anomaly_repository_index
    anomaly_repository_policy
    anomaly_service_graph
    nlohmann_json_schema_validator)
foreach(target IN LISTS ANOMALY_MANUAL_MAP_TLS_FREE_TARGETS)
    target_compile_options(${target} PRIVATE /Zc:threadSafeInit-)
endforeach()
target_link_libraries(
    anomaly_core PRIVATE
    ue5mem_core anomaly_runtime_kernel anomaly_imgui anomaly_minhook_backend anomaly_ui_service
    anomaly_platform_ui_model
    anomaly_platform_settings
    anomaly_scoped_platform
    anomaly_plugin_shadow anomaly_plugin_dependency_resolver anomaly_plugin_enablement
    anomaly_nte_profile_runtime
    anomaly_websocket_server
    anomaly_repository_coordinator
    anomaly_crash_reporter anomaly_runtime_crash_coordinator
    anomaly_structured_logger anomaly_ui_resources anomaly_i18n
    nlohmann_json::nlohmann_json)
add_dependencies(anomaly_core anomaly_stage_nte_profiles anomaly_crash_coordinator)
add_custom_command(TARGET anomaly_core POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/CONTACT"
            "$<TARGET_FILE_DIR:anomaly_core>/CONTACT"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/anomaly.ini"
            "$<TARGET_FILE_DIR:anomaly_core>/anomaly.ini"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/repository.json"
            "$<TARGET_FILE_DIR:anomaly_core>/repository.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/plugin-repositories.json"
            "$<TARGET_FILE_DIR:anomaly_core>/plugin-repositories.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/locales/host"
            "$<TARGET_FILE_DIR:anomaly_core>/locales/host"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:anomaly_core>/assets/fonts")

add_library(dwmapi SHARED
    src/bootstrap/dwmapi/proxy_main.cpp
    "${PROXY_CPP}"
    "${PROXY_ASM}"
    "${PROXY_DEF}"
)
set_target_properties(dwmapi PROPERTIES
    OUTPUT_NAME dwmapi
    ARCHIVE_OUTPUT_NAME anomaly_dwmapi
    PREFIX "")
target_include_directories(dwmapi PRIVATE include "${GENERATED_DIR}")
target_compile_definitions(dwmapi PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(dwmapi PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc>)
target_link_libraries(dwmapi PRIVATE anomaly_bootstrap_client anomaly_runtime_launch)
add_dependencies(dwmapi anomaly_core)

add_executable(anomaly_cli apps/cli/main.cpp)
set_target_properties(anomaly_cli PROPERTIES OUTPUT_NAME "anomaly-cli")
target_compile_definitions(anomaly_cli PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_cli PRIVATE /W4 /permissive- /EHsc)

add_executable(anomaly_inspect apps/cli/inspect_main.cpp)
set_target_properties(anomaly_inspect PROPERTIES OUTPUT_NAME "anomaly-inspect")
target_compile_definitions(anomaly_inspect PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_inspect PRIVATE /W4 /permissive- /EHsc)

add_executable(anomaly-profile apps/profile_tool/main.cpp)
target_include_directories(anomaly-profile PRIVATE include)
target_compile_definitions(
    anomaly-profile PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly-profile PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly-profile PRIVATE anomaly_build_profile)

add_executable(anomaly-plugin apps/plugin_tool/main.cpp)
set_target_properties(anomaly-plugin PROPERTIES OUTPUT_NAME "anomaly-plugin")
target_include_directories(anomaly-plugin PRIVATE include)
target_compile_definitions(anomaly-plugin PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly-plugin PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly-plugin PRIVATE
    anomaly_plugin_manifest anomaly_plugin_package anomaly_artifact_bundle bcrypt)

add_executable(anomaly_abi_snapshot apps/abi_snapshot/main.cpp)
set_target_properties(anomaly_abi_snapshot PROPERTIES OUTPUT_NAME "anomaly-abi-snapshot")
target_link_libraries(anomaly_abi_snapshot PRIVATE Anomaly::sdk)
target_compile_options(anomaly_abi_snapshot PRIVATE /W4 /permissive- /EHsc)

add_executable(anomaly_test_host apps/test_host/main.cpp src/plugin/plugin_manager.cpp)
set_target_properties(anomaly_test_host PROPERTIES OUTPUT_NAME "anomaly-test-host")
target_include_directories(anomaly_test_host PRIVATE include)
target_compile_definitions(anomaly_test_host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_test_host PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(anomaly_test_host PRIVATE
    ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver anomaly_plugin_enablement
    anomaly_adapter_services anomaly_plugin_runtime
    anomaly_structured_logger anomaly_scoped_platform anomaly_ui_resources
    anomaly_i18n nlohmann_json::nlohmann_json Psapi)

add_executable(anomaly-platform-preview WIN32
    apps/render_fixture/platform_preview.cpp
    src/ui/platform_host.cpp
    "${PLATFORM_UI_RESOURCES_RC}"
    src/plugin/plugin_manager.cpp)
set_target_properties(anomaly-platform-preview PROPERTIES
    OUTPUT_NAME "anomaly-platform-preview")
target_include_directories(anomaly-platform-preview PRIVATE include src)
target_compile_definitions(anomaly-platform-preview PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly-platform-preview PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly-platform-preview PRIVATE
    ue5mem_core anomaly_imgui anomaly_plugin_shadow anomaly_plugin_dependency_resolver
    anomaly_platform_ui_model anomaly_plugin_enablement anomaly_adapter_services
    anomaly_platform_settings
    anomaly_plugin_runtime
    anomaly_structured_logger anomaly_scoped_platform anomaly_ui_resources
    anomaly_i18n nlohmann_json::nlohmann_json)
add_dependencies(anomaly-platform-preview anomaly_stage_nte_profiles)

add_executable(anomaly_launcher WIN32
    apps/launcher/main.cpp
    apps/launcher/AnomalyLauncher.manifest
    "${LAUNCHER_RESOURCES_RC}")
set_target_properties(anomaly_launcher PROPERTIES OUTPUT_NAME "AnomalyLauncher")
target_include_directories(anomaly_launcher PRIVATE include)
target_compile_definitions(
    anomaly_launcher PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_launcher PRIVATE /W4 /permissive- /EHsc)
target_link_options(anomaly_launcher PRIVATE /MANIFESTUAC:NO /DELAYLOAD:dwmapi.dll)
target_link_libraries(anomaly_launcher PRIVATE
    anomaly_config anomaly_imgui anomaly_launcher_configuration anomaly_launcher_proxy
    anomaly_manual_map anomaly_runtime_recovery
    anomaly_runtime_launch anomaly_ui_resources
    ole32 shell32 delayimp)
add_dependencies(anomaly_launcher dwmapi anomaly_core)

add_executable(anomaly_render_fixture apps/render_fixture/main.cpp)
set_target_properties(anomaly_render_fixture PROPERTIES OUTPUT_NAME "anomaly-render-fixture")
target_compile_definitions(
    anomaly_render_fixture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_render_fixture PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_render_fixture PRIVATE anomaly_runtime_kernel d3d12 dxgi dxguid)

option(ANOMALY_BUILD_SDK_EXAMPLES "Build the Anomaly 1.0 SDK example plugins" ON)
option(ANOMALY_BUILD_BUILTIN_PLUGINS "Build Anomaly bundled plugins" ON)
set(ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/anomaly.ini"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/anomaly.ini"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/locales/host"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/locales/host"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/assets/fonts")
if(ANOMALY_BUILD_SDK_EXAMPLES)
    anomaly_add_plugin(anomaly_example_hello_ui C_ONLY
        SOURCES examples/hello_ui/plugin.c
        MANIFEST examples/hello_ui/manifest.json
        PACKAGE_NAME HelloUi
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk-examples/HelloUi")
    add_custom_command(TARGET anomaly_example_hello_ui POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/examples/hello_ui/locales"
                "$<TARGET_FILE_DIR:anomaly_example_hello_ui>/locales"
        VERBATIM)
    anomaly_add_plugin(anomaly_example_tick_counter
        SOURCES examples/tick_counter/plugin.cpp
        MANIFEST examples/tick_counter/manifest.json
        PACKAGE_NAME TickCounter
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk-examples/TickCounter")
    anomaly_add_plugin(anomaly_example_reliable_config
        SOURCES examples/reliable_config/plugin.cpp
        MANIFEST examples/reliable_config/manifest.json
        PACKAGE_NAME ReliableConfig
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk-examples/ReliableConfig")
    anomaly_add_plugin(anomaly_example_nte_inspector
        SOURCES examples/nte_inspector/plugin.cpp
        MANIFEST examples/nte_inspector/manifest.json
        PACKAGE_NAME NteInspector
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sdk-examples/NteInspector")
    foreach(example_target IN ITEMS anomaly_example_hello_ui anomaly_example_tick_counter
            anomaly_example_reliable_config anomaly_example_nte_inspector)
        target_compile_definitions(${example_target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    endforeach()
    set_target_properties(anomaly_example_hello_ui PROPERTIES PDB_NAME HelloUi)
    set_target_properties(anomaly_example_tick_counter PROPERTIES PDB_NAME TickCounter)
    set_target_properties(anomaly_example_reliable_config PROPERTIES PDB_NAME ReliableConfig)
    set_target_properties(anomaly_example_nte_inspector PROPERTIES PDB_NAME NteInspector)
    add_dependencies(anomaly-platform-preview
        anomaly_example_hello_ui
        anomaly_example_tick_counter
        anomaly_example_reliable_config
        anomaly_example_nte_inspector)
endif()

if(ANOMALY_BUILD_BUILTIN_PLUGINS)
    anomaly_add_plugin(anomaly_builtin_nte_position
        SOURCES plugins/NtePosition/plugin.cpp
        MANIFEST plugins/NtePosition/manifest.json
        PACKAGE_NAME NtePosition
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/NtePosition")
    anomaly_add_plugin(anomaly_builtin_nte_teleport
        SOURCES plugins/NteTeleport/plugin.cpp
        MANIFEST plugins/NteTeleport/manifest.json
        PACKAGE_NAME NteTeleport
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/NteTeleport")
    anomaly_add_plugin(anomaly_builtin_nte_navigation
        SOURCES plugins/NteNavmeshDemo/plugin.cpp
        MANIFEST plugins/NteNavmeshDemo/manifest.json
        PACKAGE_NAME NteNavmeshDemo
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/NteNavmeshDemo")
    anomaly_add_plugin(anomaly_builtin_nte_pickup_demo
        SOURCES plugins/NtePickupDemo/plugin.cpp
        MANIFEST plugins/NtePickupDemo/manifest.json
        PACKAGE_NAME NtePickupDemo
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/NtePickupDemo")
    anomaly_add_plugin(anomaly_builtin_entity_esp
        SOURCES plugins/EntityESP/plugin.cpp
        MANIFEST plugins/EntityESP/manifest.json
        PACKAGE_NAME EntityESP
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/EntityESP")
    anomaly_add_plugin(anomaly_builtin_pink_paw_heist_esp
        SOURCES
            plugins/PinkPawHeistESP/plugin.cpp
            plugins/PinkPawHeistESP/rob_bank_runtime.cpp
        MANIFEST plugins/PinkPawHeistESP/manifest.json
        PACKAGE_NAME PinkPawHeistESP
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/PinkPawHeistESP")

    anomaly_add_plugin(anomaly_builtin_fake_uid
        SOURCES plugins/FakeUID/plugin.cpp
        MANIFEST plugins/FakeUID/manifest.json
        PACKAGE_NAME FakeUID
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/FakeUID")
    anomaly_add_plugin(anomaly_builtin_camera_tools
        SOURCES plugins/CameraTools/plugin.cpp
        MANIFEST plugins/CameraTools/manifest.json
        PACKAGE_NAME CameraTools
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/builtin-plugins/CameraTools")
    target_link_libraries(anomaly_builtin_fake_uid PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(anomaly_builtin_camera_tools PRIVATE nlohmann_json::nlohmann_json)

    foreach(builtin_target IN ITEMS
            anomaly_builtin_nte_position anomaly_builtin_nte_teleport
            anomaly_builtin_nte_navigation anomaly_builtin_nte_pickup_demo
            anomaly_builtin_entity_esp anomaly_builtin_pink_paw_heist_esp
            anomaly_builtin_fake_uid anomaly_builtin_camera_tools)
        target_include_directories(${builtin_target} PRIVATE "${CMAKE_SOURCE_DIR}")
        target_compile_definitions(${builtin_target} PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    endforeach()

    function(anomaly_copy_builtin_locales target source_directory)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${CMAKE_SOURCE_DIR}/plugins/${source_directory}/locales"
                    "$<TARGET_FILE_DIR:${target}>/locales"
            VERBATIM)
    endfunction()
    anomaly_copy_builtin_locales(anomaly_builtin_nte_position NtePosition)
    anomaly_copy_builtin_locales(anomaly_builtin_nte_teleport NteTeleport)
    anomaly_copy_builtin_locales(anomaly_builtin_nte_navigation NteNavmeshDemo)
    anomaly_copy_builtin_locales(anomaly_builtin_nte_pickup_demo NtePickupDemo)
    anomaly_copy_builtin_locales(anomaly_builtin_entity_esp EntityESP)
    anomaly_copy_builtin_locales(anomaly_builtin_pink_paw_heist_esp PinkPawHeistESP)
    anomaly_copy_builtin_locales(anomaly_builtin_fake_uid FakeUID)
    anomaly_copy_builtin_locales(anomaly_builtin_camera_tools CameraTools)
    set_target_properties(anomaly_builtin_nte_position PROPERTIES PDB_NAME NtePosition)
    set_target_properties(anomaly_builtin_nte_teleport PROPERTIES PDB_NAME NteTeleport)
    set_target_properties(anomaly_builtin_nte_navigation PROPERTIES PDB_NAME NteNavmeshDemo)
    set_target_properties(anomaly_builtin_nte_pickup_demo PROPERTIES PDB_NAME NtePickupDemo)
    set_target_properties(anomaly_builtin_entity_esp PROPERTIES PDB_NAME EntityESP)
    set_target_properties(anomaly_builtin_pink_paw_heist_esp PROPERTIES PDB_NAME PinkPawHeistESP)
    set_target_properties(anomaly_builtin_fake_uid PROPERTIES PDB_NAME FakeUID)
    set_target_properties(anomaly_builtin_camera_tools PROPERTIES PDB_NAME CameraTools)
    if(MSVC)
        target_compile_options(anomaly_builtin_pink_paw_heist_esp PRIVATE /utf-8)
    endif()
    add_dependencies(anomaly_test_host
        anomaly_builtin_nte_position anomaly_builtin_nte_teleport
        anomaly_builtin_nte_navigation anomaly_builtin_nte_pickup_demo
        anomaly_builtin_entity_esp anomaly_builtin_pink_paw_heist_esp
        anomaly_builtin_fake_uid anomaly_builtin_camera_tools)
    add_dependencies(anomaly-platform-preview
        anomaly_builtin_nte_position anomaly_builtin_nte_teleport
        anomaly_builtin_nte_navigation anomaly_builtin_nte_pickup_demo
        anomaly_builtin_entity_esp anomaly_builtin_pink_paw_heist_esp
        anomaly_builtin_fake_uid anomaly_builtin_camera_tools)
endif()

function(anomaly_stage_platform_preview_plugin target package_name)
    if(NOT TARGET ${target})
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/plugins/${package_name}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "$<TARGET_FILE_DIR:${target}>"
                "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/plugins/${package_name}"
        VERBATIM)
    list(APPEND ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "$<TARGET_FILE_DIR:${target}>"
                "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/plugins/${package_name}")
    set(ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS
        "${ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS}" PARENT_SCOPE)
endfunction()
anomaly_stage_platform_preview_plugin(anomaly_example_hello_ui HelloUi)
anomaly_stage_platform_preview_plugin(anomaly_example_tick_counter TickCounter)
anomaly_stage_platform_preview_plugin(anomaly_example_reliable_config ReliableConfig)
anomaly_stage_platform_preview_plugin(anomaly_example_nte_inspector NteInspector)
anomaly_stage_platform_preview_plugin(anomaly_builtin_nte_position NtePosition)
anomaly_stage_platform_preview_plugin(anomaly_builtin_nte_teleport NteTeleport)
anomaly_stage_platform_preview_plugin(anomaly_builtin_nte_navigation NteNavmeshDemo)
anomaly_stage_platform_preview_plugin(anomaly_builtin_nte_pickup_demo NtePickupDemo)
anomaly_stage_platform_preview_plugin(anomaly_builtin_entity_esp EntityESP)
anomaly_stage_platform_preview_plugin(anomaly_builtin_pink_paw_heist_esp PinkPawHeistESP)
anomaly_stage_platform_preview_plugin(anomaly_builtin_fake_uid FakeUID)
anomaly_stage_platform_preview_plugin(anomaly_builtin_camera_tools CameraTools)

add_custom_command(TARGET anomaly-platform-preview POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/CONTACT"
            "$<TARGET_FILE_DIR:anomaly-platform-preview>/Anomaly/CONTACT"
    ${ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS}
    VERBATIM)

option(ANOMALY_BUILD_WINSOCK_TRACE_PROBES
    "Build opt-in bounded Winsock transport trace probe packages" ON)
if(ANOMALY_BUILD_WINSOCK_TRACE_PROBES)
    function(anomaly_add_winsock_trace_probe target operation plugin_id package_name)
        anomaly_add_plugin(${target}
            SOURCES tools/winsock_trace_probe/plugin.cpp
            MANIFEST tools/winsock_trace_probe/manifests/${package_name}.json
            PACKAGE_NAME ${package_name}
            OUTPUT_DIRECTORY
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/winsock-trace-probes/${package_name}")
        target_compile_definitions(${target} PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
            "ANOMALY_WINSOCK_TRACE_OPERATION=${operation}"
            "ANOMALY_WINSOCK_TRACE_PLUGIN_ID=\"${plugin_id}\"")
        target_link_libraries(${target} PRIVATE Ws2_32 Iphlpapi Bcrypt)
    endfunction()

    anomaly_add_winsock_trace_probe(
        anomaly_winsock_trace_send 1 anomaly.diagnostics.winsock.send WinsockSend)
    anomaly_add_winsock_trace_probe(
        anomaly_winsock_trace_sendto 2 anomaly.diagnostics.winsock.sendto WinsockSendTo)
    anomaly_add_winsock_trace_probe(
        anomaly_winsock_trace_wsasend 3 anomaly.diagnostics.winsock.wsasend WinsockWsaSend)
    anomaly_add_winsock_trace_probe(
        anomaly_winsock_trace_wsasendto 4 anomaly.diagnostics.winsock.wsasendto WinsockWsaSendTo)
    anomaly_add_winsock_trace_probe(
        anomaly_winsock_trace_recv 5 anomaly.diagnostics.winsock.recv WinsockRecv)
    anomaly_add_winsock_trace_probe(
        anomaly_winsock_trace_recvfrom 6 anomaly.diagnostics.winsock.recvfrom WinsockRecvFrom)
    anomaly_add_winsock_trace_probe(
        anomaly_winsock_trace_wsarecv 7 anomaly.diagnostics.winsock.wsarecv WinsockWsaRecv)
    anomaly_add_winsock_trace_probe(
        anomaly_winsock_trace_wsarecvfrom 8 anomaly.diagnostics.winsock.wsarecvfrom WinsockWsaRecvFrom)

    set(ANOMALY_WINSOCK_TRACE_PROBE_TARGETS
        anomaly_winsock_trace_send
        anomaly_winsock_trace_sendto
        anomaly_winsock_trace_wsasend
        anomaly_winsock_trace_wsasendto
        anomaly_winsock_trace_recv
        anomaly_winsock_trace_recvfrom
        anomaly_winsock_trace_wsarecv
        anomaly_winsock_trace_wsarecvfrom)
    add_dependencies(anomaly_test_host ${ANOMALY_WINSOCK_TRACE_PROBE_TARGETS})
endif()

option(ANOMALY_BUILD_BIDKING_OBJECT_CATALOG
    "Build the read-only NTE BidKing UObject catalog package" ON)
if(ANOMALY_BUILD_BIDKING_OBJECT_CATALOG)
    anomaly_add_plugin(anomaly_bidking_object_catalog
        SOURCES tools/bidking_object_catalog/plugin.cpp
        MANIFEST tools/bidking_object_catalog/manifest.json
        PACKAGE_NAME BidKingObjectCatalog
        OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/bidking-object-catalog/BidKingObjectCatalog")
    target_compile_definitions(anomaly_bidking_object_catalog PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_dependencies(anomaly_test_host anomaly_bidking_object_catalog)

endif()

option(ANOMALY_BUILD_BIDKING_ROUND_POOL_PROBE
    "Build the exact-build NTE BidKing response diagnostic package" ON)
if(ANOMALY_BUILD_BIDKING_ROUND_POOL_PROBE)
    anomaly_add_plugin(anomaly_bidking_round_pool_probe
        SOURCES tools/bidking_round_pool_probe/plugin.cpp
        MANIFEST tools/bidking_round_pool_probe/manifest.json
        PACKAGE_NAME BidKingRoundPoolProbe
        OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/bidking-round-pool-probe/BidKingRoundPoolProbe")
    target_compile_definitions(anomaly_bidking_round_pool_probe PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_dependencies(anomaly_test_host anomaly_bidking_round_pool_probe)
endif()
set(ANOMALY_RELEASE_PE_TARGETS
    dwmapi anomaly_core anomaly_launcher anomaly_crash_coordinator
    anomaly-plugin anomaly_abi_snapshot anomaly_test_host
    anomaly-profile anomaly_cli anomaly_inspect)
foreach(runtime_plugin_target IN ITEMS
        anomaly_example_hello_ui anomaly_example_tick_counter
        anomaly_example_reliable_config anomaly_example_nte_inspector
        anomaly_builtin_nte_position anomaly_builtin_nte_teleport
        anomaly_builtin_nte_navigation anomaly_builtin_nte_pickup_demo
        anomaly_builtin_entity_esp anomaly_builtin_pink_paw_heist_esp
        anomaly_builtin_fake_uid anomaly_builtin_camera_tools)
    if(TARGET ${runtime_plugin_target})
        list(APPEND ANOMALY_RELEASE_PE_TARGETS ${runtime_plugin_target})
    endif()
endforeach()
if(MSVC)
    # Keep the CodeView record embedded in each published PE independent of the
    # build-machine directory. The full PDB remains in the Symbols component.
    foreach(release_target IN LISTS ANOMALY_RELEASE_PE_TARGETS)
        target_link_options(${release_target} PRIVATE
            "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${release_target}>")
    endforeach()
endif()

install(TARGETS dwmapi
    RUNTIME DESTINATION . COMPONENT GameRuntime
    LIBRARY DESTINATION . COMPONENT GameRuntime)
install(TARGETS anomaly_launcher
    RUNTIME DESTINATION . COMPONENT GameRuntime)
install(TARGETS anomaly_core
    RUNTIME DESTINATION Anomaly COMPONENT GameRuntime
    LIBRARY DESTINATION Anomaly COMPONENT GameRuntime)
install(TARGETS anomaly_crash_coordinator
    RUNTIME DESTINATION Anomaly COMPONENT GameRuntime)
if(TARGET anomaly_builtin_nte_position)
    install(TARGETS anomaly_builtin_nte_position
        RUNTIME DESTINATION Anomaly/plugins/NtePosition COMPONENT GameRuntime
        LIBRARY DESTINATION Anomaly/plugins/NtePosition COMPONENT GameRuntime)
    install(FILES plugins/NtePosition/manifest.json
        DESTINATION Anomaly/plugins/NtePosition COMPONENT GameRuntime)
    install(DIRECTORY plugins/NtePosition/locales/
        DESTINATION Anomaly/plugins/NtePosition/locales COMPONENT GameRuntime)
endif()
if(TARGET anomaly_builtin_nte_teleport)
    install(TARGETS anomaly_builtin_nte_teleport
        RUNTIME DESTINATION Anomaly/plugins/NteTeleport COMPONENT GameRuntime
        LIBRARY DESTINATION Anomaly/plugins/NteTeleport COMPONENT GameRuntime)
    install(FILES plugins/NteTeleport/manifest.json
        DESTINATION Anomaly/plugins/NteTeleport COMPONENT GameRuntime)
    install(DIRECTORY plugins/NteTeleport/locales/
        DESTINATION Anomaly/plugins/NteTeleport/locales COMPONENT GameRuntime)
endif()
if(TARGET anomaly_builtin_nte_navigation)
    install(TARGETS anomaly_builtin_nte_navigation
        RUNTIME DESTINATION Anomaly/plugins/NteNavmeshDemo COMPONENT GameRuntime
        LIBRARY DESTINATION Anomaly/plugins/NteNavmeshDemo COMPONENT GameRuntime)
    install(FILES plugins/NteNavmeshDemo/manifest.json
        DESTINATION Anomaly/plugins/NteNavmeshDemo COMPONENT GameRuntime)
    install(DIRECTORY plugins/NteNavmeshDemo/locales/
        DESTINATION Anomaly/plugins/NteNavmeshDemo/locales COMPONENT GameRuntime)
endif()
if(TARGET anomaly_builtin_nte_pickup_demo)
    install(TARGETS anomaly_builtin_nte_pickup_demo
        RUNTIME DESTINATION Anomaly/plugins/NtePickupDemo COMPONENT GameRuntime
        LIBRARY DESTINATION Anomaly/plugins/NtePickupDemo COMPONENT GameRuntime)
    install(FILES plugins/NtePickupDemo/manifest.json
        DESTINATION Anomaly/plugins/NtePickupDemo COMPONENT GameRuntime)
    install(DIRECTORY plugins/NtePickupDemo/locales/
        DESTINATION Anomaly/plugins/NtePickupDemo/locales COMPONENT GameRuntime)
endif()
if(TARGET anomaly_builtin_entity_esp)
    install(TARGETS anomaly_builtin_entity_esp
        RUNTIME DESTINATION Anomaly/plugins/EntityESP COMPONENT GameRuntime
        LIBRARY DESTINATION Anomaly/plugins/EntityESP COMPONENT GameRuntime)
    install(FILES plugins/EntityESP/manifest.json
        DESTINATION Anomaly/plugins/EntityESP COMPONENT GameRuntime)
    install(DIRECTORY plugins/EntityESP/locales/
        DESTINATION Anomaly/plugins/EntityESP/locales COMPONENT GameRuntime)
endif()
if(TARGET anomaly_builtin_pink_paw_heist_esp)
    install(TARGETS anomaly_builtin_pink_paw_heist_esp
        RUNTIME DESTINATION Anomaly/plugins/PinkPawHeistESP COMPONENT GameRuntime
        LIBRARY DESTINATION Anomaly/plugins/PinkPawHeistESP COMPONENT GameRuntime)
    install(FILES plugins/PinkPawHeistESP/manifest.json
        DESTINATION Anomaly/plugins/PinkPawHeistESP COMPONENT GameRuntime)
    install(DIRECTORY plugins/PinkPawHeistESP/locales/
        DESTINATION Anomaly/plugins/PinkPawHeistESP/locales COMPONENT GameRuntime)
endif()
if(TARGET anomaly_builtin_fake_uid)
    install(TARGETS anomaly_builtin_fake_uid
        RUNTIME DESTINATION Anomaly/plugins/FakeUID COMPONENT GameRuntime
        LIBRARY DESTINATION Anomaly/plugins/FakeUID COMPONENT GameRuntime)
    install(FILES plugins/FakeUID/manifest.json
        DESTINATION Anomaly/plugins/FakeUID COMPONENT GameRuntime)
    install(DIRECTORY plugins/FakeUID/locales/
        DESTINATION Anomaly/plugins/FakeUID/locales COMPONENT GameRuntime)
endif()
if(TARGET anomaly_builtin_camera_tools)
    install(TARGETS anomaly_builtin_camera_tools
        RUNTIME DESTINATION Anomaly/plugins/CameraTools COMPONENT GameRuntime
        LIBRARY DESTINATION Anomaly/plugins/CameraTools COMPONENT GameRuntime)
    install(FILES plugins/CameraTools/manifest.json
        DESTINATION Anomaly/plugins/CameraTools COMPONENT GameRuntime)
    install(DIRECTORY plugins/CameraTools/locales/
        DESTINATION Anomaly/plugins/CameraTools/locales COMPONENT GameRuntime)
endif()
install(FILES CONTACT config/anomaly.ini config/repository.json config/plugin-repositories.json
    DESTINATION Anomaly COMPONENT GameRuntime)
install(DIRECTORY locales/host
    DESTINATION Anomaly/locales COMPONENT GameRuntime)
install(FILES assets/fonts/NotoSansCJKsc-Regular.ttf
    DESTINATION Anomaly/assets/fonts COMPONENT GameRuntime)
install(CODE [[
    file(REMOVE_RECURSE
        "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/Anomaly/profiles/nte")
]] COMPONENT GameRuntime)
install(DIRECTORY profiles/
    DESTINATION Anomaly/profiles COMPONENT GameRuntime)
install(TARGETS anomaly_cli anomaly_inspect anomaly-profile anomaly-plugin anomaly_abi_snapshot anomaly_test_host
    RUNTIME DESTINATION . COMPONENT Tools)
install(TARGETS anomaly_sdk EXPORT AnomalySDKTargets COMPONENT SDK)
install(EXPORT AnomalySDKTargets
    FILE AnomalySDKTargets.cmake
    NAMESPACE Anomaly::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AnomalySDK
    COMPONENT SDK)
install(DIRECTORY include/anomaly/sdk/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/anomaly/sdk COMPONENT SDK
    PATTERN "version.h" EXCLUDE)
install(FILES "${ANOMALY_GENERATED_INSTALL_INCLUDE_DIR}/anomaly/sdk/version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/anomaly/sdk COMPONENT SDK)
install(FILES schemas/plugin-manifest.schema.json schemas/build-profile.schema.json
    schemas/repository-config.schema.json
    schemas/repository-index.schema.json schemas/plugin-enablement.schema.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/anomaly/schemas COMPONENT SDK)
install(FILES abi/anomaly-sdk-v1-windows-x64.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/anomaly/abi COMPONENT SDK)
set(ANOMALY_SDK_EXAMPLES_DESTINATION "${CMAKE_INSTALL_DATADIR}/anomaly/examples")
install(FILES examples/CMakeLists.txt examples/README.md
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION} COMPONENT SDK)
install(FILES examples/hello_ui/plugin.c examples/hello_ui/manifest.json
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/hello_ui COMPONENT SDK)
install(DIRECTORY examples/hello_ui/locales
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/hello_ui COMPONENT SDK)
install(FILES examples/tick_counter/plugin.cpp examples/tick_counter/manifest.json
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/tick_counter COMPONENT SDK)
install(FILES examples/reliable_config/plugin.cpp examples/reliable_config/manifest.json
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/reliable_config COMPONENT SDK)
install(FILES examples/nte_inspector/plugin.cpp examples/nte_inspector/manifest.json
    DESTINATION ${ANOMALY_SDK_EXAMPLES_DESTINATION}/nte_inspector COMPONENT SDK)
configure_package_config_file(
    cmake/sdk/AnomalySDKConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AnomalySDKConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AnomalySDK)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AnomalySDKConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AnomalySDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AnomalySDKConfigVersion.cmake"
    cmake/sdk/AnomalyPlugin.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AnomalySDK COMPONENT SDK)
set(ANOMALY_THIRD_PARTY_LICENSE_FILES
    third_party/licenses/imgui-LICENSE.txt
    third_party/licenses/minhook-LICENSE.txt
    third_party/licenses/nlohmann-json-LICENSE.txt
    third_party/licenses/json-schema-validator-LICENSE.txt
    third_party/licenses/noto-sans-cjk-LICENSE.txt)
foreach(anomaly_release_component IN ITEMS GameRuntime SDK Tools Symbols)
    install(FILES LICENSE NOTICE
        DESTINATION . COMPONENT ${anomaly_release_component})
    install(FILES ${ANOMALY_THIRD_PARTY_LICENSE_FILES}
        DESTINATION third_party/licenses COMPONENT ${anomaly_release_component})
endforeach()
foreach(symbol_target IN LISTS ANOMALY_RELEASE_PE_TARGETS)
    install(FILES "$<TARGET_PDB_FILE:${symbol_target}>"
        DESTINATION symbols COMPONENT Symbols)
endforeach()
