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
    src/game/ue5/ue5_nte_adapter.cpp
    src/game/ue5/game_tick_hook.cpp
    src/game/ue5/ue5_outbound_bit_count_probe.cpp)
target_include_directories(anomaly_adapter_services PUBLIC include)
target_compile_definitions(
    anomaly_adapter_services PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(anomaly_adapter_services PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(
    anomaly_adapter_services
    PUBLIC anomaly_symbol_resolver anomaly_hook_manager anomaly_minhook_backend)

add_library(anomaly_nte_profile_runtime STATIC src/game/nte/nte_profile_runtime.cpp)
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

option(ANOMALY_BUILD_PLUGIN_MANAGER_FIXTURE
    "Build the integration-test plugin manager fixture" ON)
if(ANOMALY_BUILD_PLUGIN_MANAGER_FIXTURE)
    add_library(anomaly_plugin_manager_fixture SHARED
        tests/fixtures/plugin_manager_fixture.cpp)
    target_include_directories(anomaly_plugin_manager_fixture PRIVATE include)
    target_compile_definitions(anomaly_plugin_manager_fixture PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly_plugin_manager_fixture PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(anomaly_plugin_manager_fixture PRIVATE anomaly_imgui)
    set_target_properties(anomaly_plugin_manager_fixture PROPERTIES
        OUTPUT_NAME "plugin"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-manager/plugins/PluginManagerFixture"
        LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/tests/plugin-manager/plugins/PluginManagerFixture"
        ARCHIVE_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-manager/plugins/PluginManagerFixture"
        PDB_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-manager/plugins/PluginManagerFixture")
    add_custom_command(TARGET anomaly_plugin_manager_fixture PRE_BUILD
        COMMAND "${CMAKE_COMMAND}" -E remove_directory
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-manager/plugins"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:anomaly_plugin_manager_fixture>")
    add_custom_command(TARGET anomaly_plugin_manager_fixture POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/tests/fixtures/plugin_manager_fixture_manifest.json"
                "$<TARGET_FILE_DIR:anomaly_plugin_manager_fixture>/manifest.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/tests/fixtures/plugin_manager_fixture_watch.txt"
                "$<TARGET_FILE_DIR:anomaly_plugin_manager_fixture>/watch.txt")
endif()

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
    target_link_libraries(anomaly_builtin_fake_uid PRIVATE nlohmann_json::nlohmann_json)

    foreach(builtin_target IN ITEMS
            anomaly_builtin_nte_position anomaly_builtin_nte_teleport
            anomaly_builtin_entity_esp anomaly_builtin_pink_paw_heist_esp
            anomaly_builtin_fake_uid)
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
    anomaly_copy_builtin_locales(anomaly_builtin_entity_esp EntityESP)
    anomaly_copy_builtin_locales(anomaly_builtin_pink_paw_heist_esp PinkPawHeistESP)
    anomaly_copy_builtin_locales(anomaly_builtin_fake_uid FakeUID)
    set_target_properties(anomaly_builtin_nte_position PROPERTIES PDB_NAME NtePosition)
    set_target_properties(anomaly_builtin_nte_teleport PROPERTIES PDB_NAME NteTeleport)
    set_target_properties(anomaly_builtin_entity_esp PROPERTIES PDB_NAME EntityESP)
    set_target_properties(anomaly_builtin_pink_paw_heist_esp PROPERTIES PDB_NAME PinkPawHeistESP)
    set_target_properties(anomaly_builtin_fake_uid PROPERTIES PDB_NAME FakeUID)
    if(MSVC)
        target_compile_options(anomaly_builtin_pink_paw_heist_esp PRIVATE /utf-8)
    endif()
    file(GLOB_RECURSE ANOMALY_PINK_PAW_HEIST_ESP_ASSETS CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/plugins/PinkPawHeistESP/assets/*")
    set_property(SOURCE plugins/PinkPawHeistESP/plugin.cpp APPEND PROPERTY
        OBJECT_DEPENDS "${ANOMALY_PINK_PAW_HEIST_ESP_ASSETS}")
    add_custom_command(TARGET anomaly_builtin_pink_paw_heist_esp POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/plugins/PinkPawHeistESP/assets"
                "$<TARGET_FILE_DIR:anomaly_builtin_pink_paw_heist_esp>/assets"
        VERBATIM)
    add_dependencies(anomaly_test_host
        anomaly_builtin_nte_position anomaly_builtin_nte_teleport
        anomaly_builtin_entity_esp anomaly_builtin_pink_paw_heist_esp
        anomaly_builtin_fake_uid)
    add_dependencies(anomaly-platform-preview
        anomaly_builtin_nte_position anomaly_builtin_nte_teleport
        anomaly_builtin_entity_esp anomaly_builtin_pink_paw_heist_esp
        anomaly_builtin_fake_uid)
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
anomaly_stage_platform_preview_plugin(anomaly_builtin_entity_esp EntityESP)
anomaly_stage_platform_preview_plugin(anomaly_builtin_pink_paw_heist_esp PinkPawHeistESP)
anomaly_stage_platform_preview_plugin(anomaly_builtin_fake_uid FakeUID)

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

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME project_version_resolution
        COMMAND "${CMAKE_COMMAND}"
            -P "${CMAKE_SOURCE_DIR}/tests/cmake/anomaly_version_tests.cmake")

    find_program(ANOMALY_POWERSHELL_EXECUTABLE NAMES pwsh.exe)
    if(NOT ANOMALY_POWERSHELL_EXECUTABLE)
        message(FATAL_ERROR "PowerShell 7+ (pwsh.exe) is required to run Anomaly test tools")
    endif()

    add_executable(core_api_c_contract tests/contract/core_api_c_contract.c)
    target_include_directories(core_api_c_contract PRIVATE include)
    target_compile_definitions(
        core_api_c_contract PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(core_api_c_contract PRIVATE /W4)
    set_target_properties(core_api_c_contract PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME core_api_c_contract COMMAND core_api_c_contract)

    add_executable(core_api_cpp_contract tests/contract/core_api_cpp_contract.cpp)
    target_include_directories(core_api_cpp_contract PRIVATE include)
    target_compile_definitions(
        core_api_cpp_contract PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(core_api_cpp_contract PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME core_api_cpp_contract COMMAND core_api_cpp_contract)

    add_library(bootstrap_contract_fixture SHARED
        tests/fixtures/bootstrap_contract_fixture.cpp)
    target_include_directories(bootstrap_contract_fixture PRIVATE include)
    target_compile_definitions(
        bootstrap_contract_fixture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(bootstrap_contract_fixture PRIVATE /W4 /permissive- /EHsc)
    set_target_properties(bootstrap_contract_fixture PROPERTIES PREFIX "")

    add_executable(bootstrap_client_tests tests/unit/bootstrap_client_tests.cpp)
    target_include_directories(bootstrap_client_tests PRIVATE include)
    target_compile_definitions(
        bootstrap_client_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(bootstrap_client_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(bootstrap_client_tests PRIVATE anomaly_bootstrap_client)
    add_dependencies(bootstrap_client_tests bootstrap_contract_fixture)
    add_test(
        NAME bootstrap_client_tests
        COMMAND bootstrap_client_tests $<TARGET_FILE:bootstrap_contract_fixture>)

    add_executable(runtime_session_tests tests/unit/runtime_session_tests.cpp)
    target_include_directories(runtime_session_tests PRIVATE include)
    target_compile_definitions(
        runtime_session_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(runtime_session_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(runtime_session_tests PRIVATE anomaly_runtime_kernel)
    add_test(NAME runtime_session_tests COMMAND runtime_session_tests)

    add_executable(launcher_proxy_installation_tests
        tests/unit/launcher_proxy_installation_tests.cpp)
    target_compile_definitions(launcher_proxy_installation_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(launcher_proxy_installation_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(launcher_proxy_installation_tests PRIVATE anomaly_launcher_proxy)
    add_test(NAME launcher_proxy_installation_tests COMMAND launcher_proxy_installation_tests)

    add_library(manual_map_core_fixture SHARED
        tests/fixtures/manual_map_core_fixture.cpp)
    set_target_properties(manual_map_core_fixture PROPERTIES
        PREFIX "" OUTPUT_NAME "manual-map-core-fixture")
    target_include_directories(manual_map_core_fixture PRIVATE include)
    target_compile_definitions(manual_map_core_fixture PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(manual_map_core_fixture PRIVATE /W4 /permissive- /EHsc)

    add_library(manual_map_static_tls_fixture SHARED
        tests/fixtures/manual_map_static_tls_fixture.cpp)
    set_target_properties(manual_map_static_tls_fixture PROPERTIES
        PREFIX "" OUTPUT_NAME "manual-map-static-tls-fixture")
    target_include_directories(manual_map_static_tls_fixture PRIVATE include)
    target_compile_definitions(manual_map_static_tls_fixture PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(
        manual_map_static_tls_fixture PRIVATE /W4 /permissive- /EHsc)

    add_library(manual_map_dwmapi_collision_fixture SHARED
        tests/fixtures/manual_map_dwmapi_collision.cpp)
    set_target_properties(manual_map_dwmapi_collision_fixture PROPERTIES
        PREFIX ""
        OUTPUT_NAME "dwmapi"
        ARCHIVE_OUTPUT_NAME "manual_map_dwmapi_collision"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/manual-map-collision")
    target_compile_definitions(manual_map_dwmapi_collision_fixture PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(
        manual_map_dwmapi_collision_fixture PRIVATE /W4 /permissive- /EHsc)

    add_executable(manual_map_target tests/fixtures/manual_map_target.cpp)
    set_target_properties(manual_map_target PROPERTIES OUTPUT_NAME "manual-map-target")
    target_compile_definitions(manual_map_target PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(manual_map_target PRIVATE /W4 /permissive- /EHsc)

    add_executable(manual_map_tests tests/integration/manual_map_tests.cpp)
    target_compile_definitions(manual_map_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(manual_map_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(manual_map_tests PRIVATE anomaly_manual_map)
    add_dependencies(manual_map_tests
        manual_map_core_fixture manual_map_static_tls_fixture manual_map_target
        manual_map_dwmapi_collision_fixture)
    add_test(NAME manual_map_tests COMMAND manual_map_tests
        $<TARGET_FILE:manual_map_core_fixture> $<TARGET_FILE:manual_map_target>
        $<TARGET_FILE:manual_map_dwmapi_collision_fixture>
        $<TARGET_FILE:manual_map_static_tls_fixture>)
    set_tests_properties(manual_map_tests PROPERTIES TIMEOUT 30)
    if(ANOMALY_ENABLE_ASAN)
        # MSVC AddressSanitizer adds loader-managed TLS to instrumented DLLs,
        # so those images are intentionally invalid manual-map inputs.
        set_tests_properties(manual_map_tests PROPERTIES DISABLED TRUE)
    endif()

    add_executable(launcher_manifest_tests tests/unit/launcher_manifest_tests.cpp)
    target_compile_definitions(launcher_manifest_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(launcher_manifest_tests PRIVATE /W4 /permissive- /EHsc)
    add_dependencies(launcher_manifest_tests anomaly_launcher anomaly_core)
    add_test(NAME launcher_manifest_tests COMMAND launcher_manifest_tests
        $<TARGET_FILE:anomaly_launcher> $<TARGET_FILE:anomaly_core>)

    add_executable(runtime_launch_tests tests/unit/runtime_launch_tests.cpp)
    target_include_directories(runtime_launch_tests PRIVATE include)
    target_compile_definitions(
        runtime_launch_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(runtime_launch_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(runtime_launch_tests PRIVATE anomaly_runtime_launch)
    add_test(NAME runtime_launch_tests COMMAND runtime_launch_tests)

    add_executable(startup_fault_matrix tests/fixtures/startup_fault_matrix.cpp)
    target_include_directories(startup_fault_matrix PRIVATE include)
    target_compile_definitions(
        startup_fault_matrix PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(startup_fault_matrix PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(startup_fault_matrix PRIVATE anomaly_runtime_kernel)
    add_test(NAME startup_fault_matrix COMMAND startup_fault_matrix)
    set_tests_properties(startup_fault_matrix PROPERTIES TIMEOUT 30 LABELS "stability;fault")

    add_executable(dispatcher_tests tests/unit/dispatcher_tests.cpp)
    target_include_directories(dispatcher_tests PRIVATE include)
    target_compile_definitions(
        dispatcher_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(dispatcher_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(dispatcher_tests PRIVATE anomaly_dispatcher)
    add_test(NAME dispatcher_tests COMMAND dispatcher_tests)

    add_executable(runtime_dispatchers_tests tests/unit/runtime_dispatchers_tests.cpp)
    target_include_directories(runtime_dispatchers_tests PRIVATE include)
    target_compile_definitions(
        runtime_dispatchers_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(runtime_dispatchers_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(runtime_dispatchers_tests PRIVATE anomaly_runtime_dispatchers)
    add_test(NAME runtime_dispatchers_tests COMMAND runtime_dispatchers_tests)

    add_executable(game_dispatcher_fixture tests/fixtures/game_dispatcher_fixture.cpp)
    target_include_directories(game_dispatcher_fixture PRIVATE include)
    target_compile_definitions(
        game_dispatcher_fixture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(game_dispatcher_fixture PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(game_dispatcher_fixture PRIVATE anomaly_runtime_kernel)
    add_test(NAME game_dispatcher_fixture COMMAND game_dispatcher_fixture)

    add_executable(structured_logger_tests tests/unit/structured_logger_tests.cpp)
    target_include_directories(structured_logger_tests PRIVATE include)
    target_compile_definitions(
        structured_logger_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(structured_logger_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(structured_logger_tests PRIVATE anomaly_structured_logger)
    add_test(NAME structured_logger_tests COMMAND structured_logger_tests)

    add_executable(reliable_storage_tests tests/unit/reliable_storage_tests.cpp)
    target_include_directories(reliable_storage_tests PRIVATE include)
    target_compile_definitions(
        reliable_storage_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(reliable_storage_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(reliable_storage_tests PRIVATE anomaly_reliable_storage)
    add_test(NAME reliable_storage_tests COMMAND reliable_storage_tests)

    add_executable(runtime_recovery_tests tests/unit/runtime_recovery_tests.cpp)
    target_include_directories(runtime_recovery_tests PRIVATE include)
    target_compile_definitions(
        runtime_recovery_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(runtime_recovery_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(runtime_recovery_tests PRIVATE anomaly_runtime_recovery)
    add_test(NAME runtime_recovery_tests COMMAND runtime_recovery_tests)

    add_executable(runtime_crash_coordinator_tests
        tests/integration/runtime_crash_coordinator_tests.cpp)
    target_include_directories(runtime_crash_coordinator_tests PRIVATE include)
    target_compile_definitions(runtime_crash_coordinator_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(runtime_crash_coordinator_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(runtime_crash_coordinator_tests PRIVATE
        anomaly_runtime_crash_coordinator anomaly_runtime_recovery)
    add_dependencies(runtime_crash_coordinator_tests anomaly_crash_coordinator)
    add_test(NAME runtime_crash_coordinator_tests
        COMMAND runtime_crash_coordinator_tests $<TARGET_FILE:anomaly_crash_coordinator>)
    set_tests_properties(runtime_crash_coordinator_tests PROPERTIES TIMEOUT 30)

    add_executable(scoped_platform_services_tests tests/unit/scoped_platform_services_tests.cpp)
    target_include_directories(scoped_platform_services_tests PRIVATE include)
    target_compile_definitions(
        scoped_platform_services_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        "ANOMALY_EXPECTED_RELEASE_VERSION=\"${ANOMALY_VERSION_STRING}\"")
    target_compile_options(scoped_platform_services_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(scoped_platform_services_tests PRIVATE anomaly_scoped_platform)
    add_test(NAME scoped_platform_services_tests COMMAND scoped_platform_services_tests)

    add_executable(crash_reporter_tests tests/unit/crash_reporter_tests.cpp)
    target_include_directories(crash_reporter_tests PRIVATE include)
    target_compile_definitions(
        crash_reporter_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(crash_reporter_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(crash_reporter_tests PRIVATE anomaly_crash_reporter)
    add_test(NAME crash_reporter_tests COMMAND crash_reporter_tests)
    set_tests_properties(crash_reporter_tests PROPERTIES TIMEOUT 45)

    add_executable(service_graph_tests tests/unit/service_graph_tests.cpp)
    target_include_directories(service_graph_tests PRIVATE include)
    target_compile_definitions(
        service_graph_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(service_graph_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(service_graph_tests PRIVATE anomaly_service_graph ue5mem_core)
    add_test(NAME service_graph_tests COMMAND service_graph_tests)

    add_executable(service_affinity_tests tests/unit/service_affinity_tests.cpp)
    target_include_directories(service_affinity_tests PRIVATE include)
    target_compile_definitions(
        service_affinity_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(service_affinity_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(service_affinity_tests PRIVATE anomaly_service_graph)
    add_test(NAME service_affinity_tests COMMAND service_affinity_tests)

    add_executable(plugin_manifest_tests tests/unit/plugin_manifest_tests.cpp)
    target_include_directories(plugin_manifest_tests PRIVATE include)
    target_compile_definitions(
        plugin_manifest_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        "ANOMALY_PLUGIN_MANIFEST_SCHEMA_FILE=\"${CMAKE_SOURCE_DIR}/schemas/plugin-manifest.schema.json\""
        "ANOMALY_NTE_TELEPORT_MANIFEST_FILE=\"${CMAKE_SOURCE_DIR}/plugins/NteTeleport/manifest.json\"")
    target_compile_options(plugin_manifest_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_manifest_tests PRIVATE anomaly_plugin_manifest)
    add_test(NAME plugin_manifest_tests COMMAND plugin_manifest_tests)

    add_executable(ipc_registry_tests tests/unit/ipc_registry_tests.cpp)
    target_include_directories(ipc_registry_tests PRIVATE include)
    target_compile_definitions(
        ipc_registry_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ipc_registry_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(ipc_registry_tests PRIVATE anomaly_plugin_runtime)
    add_test(NAME ipc_registry_tests COMMAND ipc_registry_tests)

    add_executable(ipc_abi_contract tests/contract/ipc_abi_contract.c)
    target_include_directories(ipc_abi_contract PRIVATE include)
    target_compile_options(ipc_abi_contract PRIVATE /W4)
    set_target_properties(ipc_abi_contract PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME ipc_abi_contract COMMAND ipc_abi_contract)

    add_executable(plugin_capability_policy_tests tests/unit/plugin_capability_policy_tests.cpp)
    target_include_directories(plugin_capability_policy_tests PRIVATE include)
    target_compile_definitions(
        plugin_capability_policy_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_capability_policy_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_capability_policy_tests PRIVATE anomaly_plugin_manifest)
    add_test(NAME plugin_capability_policy_tests COMMAND plugin_capability_policy_tests)

    add_executable(plugin_compatibility_tests tests/unit/plugin_compatibility_tests.cpp)
    target_include_directories(plugin_compatibility_tests PRIVATE include)
    target_compile_definitions(
        plugin_compatibility_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_compatibility_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_compatibility_tests PRIVATE anomaly_plugin_compatibility)
    add_test(NAME plugin_compatibility_tests COMMAND plugin_compatibility_tests)

    add_executable(plugin_package_tests tests/unit/plugin_package_tests.cpp)
    target_include_directories(plugin_package_tests PRIVATE include)
    target_compile_definitions(
        plugin_package_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_package_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_package_tests PRIVATE anomaly_plugin_package)
    add_test(NAME plugin_package_tests COMMAND plugin_package_tests)

    add_executable(repository_update_tests tests/unit/repository_update_tests.cpp)
    target_include_directories(repository_update_tests PRIVATE include)
    target_compile_definitions(
        repository_update_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(repository_update_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(repository_update_tests PRIVATE
        anomaly_update_transaction nlohmann_json::nlohmann_json bcrypt)
    add_test(NAME repository_update_tests COMMAND repository_update_tests)

    add_executable(repository_configuration_tests
        tests/unit/repository_configuration_tests.cpp)
    target_include_directories(repository_configuration_tests PRIVATE include)
    target_compile_definitions(
        repository_configuration_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(repository_configuration_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(
        repository_configuration_tests PRIVATE anomaly_repository_configuration)
    add_test(NAME repository_configuration_tests COMMAND repository_configuration_tests)

    add_executable(repository_policy_tests tests/unit/repository_policy_tests.cpp)
    target_include_directories(repository_policy_tests PRIVATE include)
    target_compile_definitions(
        repository_policy_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(repository_policy_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(
        repository_policy_tests PRIVATE anomaly_repository_policy
        nlohmann_json::nlohmann_json bcrypt)
    add_test(NAME repository_policy_tests COMMAND repository_policy_tests)

    add_executable(repository_network_tests tests/unit/repository_network_tests.cpp)
    target_include_directories(repository_network_tests PRIVATE include)
    target_compile_definitions(
        repository_network_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(repository_network_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(repository_network_tests PRIVATE anomaly_repository_network)
    add_test(NAME repository_network_tests COMMAND repository_network_tests)

    add_executable(repository_coordinator_tests
        tests/unit/repository_coordinator_tests.cpp)
    target_include_directories(repository_coordinator_tests PRIVATE include)
    target_compile_definitions(
        repository_coordinator_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(repository_coordinator_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(repository_coordinator_tests PRIVATE
        anomaly_repository_coordinator nlohmann_json::nlohmann_json)
    add_test(NAME repository_coordinator_tests COMMAND repository_coordinator_tests)

    add_test(NAME diagnostics_bundle_tests
        COMMAND "${ANOMALY_POWERSHELL_EXECUTABLE}"
            -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/fixtures/diagnostics_bundle_tests.ps1"
            -Collector "${CMAKE_SOURCE_DIR}/tools/collect_diagnostics.ps1")
    set_tests_properties(diagnostics_bundle_tests PROPERTIES
        TIMEOUT 60 LABELS "stability;privacy")

    add_executable(plugin_catalog_resolver_tests tests/unit/plugin_catalog_resolver_tests.cpp)
    target_include_directories(plugin_catalog_resolver_tests PRIVATE include)
    target_compile_definitions(
        plugin_catalog_resolver_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_catalog_resolver_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(
        plugin_catalog_resolver_tests PRIVATE anomaly_plugin_dependency_resolver)
    add_test(NAME plugin_catalog_resolver_tests COMMAND plugin_catalog_resolver_tests)

    add_executable(platform_ui_window_registry_tests
        tests/integration/platform_ui_window_registry_tests.cpp
        src/ui/platform_host.cpp
        "${PLATFORM_UI_RESOURCES_RC}"
        src/plugin/plugin_manager.cpp)
    target_include_directories(platform_ui_window_registry_tests PRIVATE include src)
    target_compile_definitions(
        platform_ui_window_registry_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(platform_ui_window_registry_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(
        platform_ui_window_registry_tests PRIVATE
        ue5mem_core anomaly_imgui anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_platform_ui_model anomaly_plugin_enablement
        anomaly_platform_settings
        anomaly_adapter_services anomaly_plugin_runtime anomaly_structured_logger
        anomaly_scoped_platform anomaly_ui_resources nlohmann_json::nlohmann_json)
    add_test(NAME platform_ui_window_registry_tests COMMAND platform_ui_window_registry_tests)

    add_executable(plugin_shadow_watcher_tests tests/unit/plugin_shadow_watcher_tests.cpp)
    target_include_directories(plugin_shadow_watcher_tests PRIVATE include)
    target_compile_definitions(
        plugin_shadow_watcher_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_shadow_watcher_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_shadow_watcher_tests PRIVATE anomaly_plugin_shadow)
    add_test(NAME plugin_shadow_watcher_tests COMMAND plugin_shadow_watcher_tests)

    add_executable(plugin_api_v1_c_contract tests/contract/plugin_api_v1_contract.c)
    target_include_directories(plugin_api_v1_c_contract PRIVATE include)
    target_compile_definitions(
        plugin_api_v1_c_contract PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_api_v1_c_contract PRIVATE /W4)
    set_target_properties(plugin_api_v1_c_contract PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME plugin_api_v1_c_contract COMMAND plugin_api_v1_c_contract)

    add_executable(plugin_runtime_v1_tests tests/unit/plugin_runtime_v1_tests.cpp)
    target_include_directories(plugin_runtime_v1_tests PRIVATE include)
    target_compile_definitions(
        plugin_runtime_v1_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_runtime_v1_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_runtime_v1_tests PRIVATE anomaly_plugin_runtime)
    add_test(NAME plugin_runtime_v1_tests COMMAND plugin_runtime_v1_tests)

    add_executable(hook_manager_tests tests/unit/hook_manager_tests.cpp)
    target_include_directories(hook_manager_tests PRIVATE include)
    target_compile_definitions(
        hook_manager_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(hook_manager_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(hook_manager_tests PRIVATE anomaly_hook_manager)
    add_test(NAME hook_manager_tests COMMAND hook_manager_tests)

    add_executable(ui_resource_registry_tests tests/unit/ui_resource_registry_tests.cpp)
    target_include_directories(ui_resource_registry_tests PRIVATE include)
    target_compile_definitions(
        ui_resource_registry_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ui_resource_registry_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(ui_resource_registry_tests PRIVATE anomaly_ui_resources)
    add_test(NAME ui_resource_registry_tests COMMAND ui_resource_registry_tests)

    add_executable(input_service_tests tests/unit/input_service_tests.cpp)
    target_include_directories(input_service_tests PRIVATE include)
    target_compile_definitions(
        input_service_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(input_service_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(input_service_tests PRIVATE anomaly_ui_resources)
    add_test(NAME input_service_tests COMMAND input_service_tests)

    set(ANOMALY_IPC_PLUGIN_FIXTURE_DIRECTORY
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/ipc-plugin-fixture")
    anomaly_add_plugin(ipc_c_provider C_ONLY
        SOURCES tests/fixtures/ipc_c_provider.c
        MANIFEST tests/fixtures/ipc_c_provider_manifest.json
        PACKAGE_NAME CProvider
        OUTPUT_DIRECTORY "${ANOMALY_IPC_PLUGIN_FIXTURE_DIRECTORY}/CProvider")
    anomaly_add_plugin(ipc_cpp_consumer
        SOURCES tests/fixtures/ipc_cpp_consumer.cpp
        MANIFEST tests/fixtures/ipc_cpp_consumer_manifest.json
        PACKAGE_NAME CppConsumer
        OUTPUT_DIRECTORY "${ANOMALY_IPC_PLUGIN_FIXTURE_DIRECTORY}/CppConsumer")
    anomaly_add_plugin(ipc_cpp_provider
        SOURCES tests/fixtures/ipc_cpp_provider.cpp
        MANIFEST tests/fixtures/ipc_cpp_provider_manifest.json
        PACKAGE_NAME CppProvider
        OUTPUT_DIRECTORY "${ANOMALY_IPC_PLUGIN_FIXTURE_DIRECTORY}/CppProvider")
    anomaly_add_plugin(ipc_c_consumer C_ONLY
        SOURCES tests/fixtures/ipc_c_consumer.c
        MANIFEST tests/fixtures/ipc_c_consumer_manifest.json
        PACKAGE_NAME CConsumer
        OUTPUT_DIRECTORY "${ANOMALY_IPC_PLUGIN_FIXTURE_DIRECTORY}/CConsumer")

    add_executable(ipc_plugin_integration_tests
        tests/integration/ipc_plugin_integration_tests.cpp src/plugin/plugin_manager.cpp)
    target_include_directories(ipc_plugin_integration_tests PRIVATE include)
    target_compile_definitions(
        ipc_plugin_integration_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ipc_plugin_integration_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(ipc_plugin_integration_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform)
    add_dependencies(ipc_plugin_integration_tests
        ipc_c_provider ipc_cpp_consumer ipc_cpp_provider ipc_c_consumer)
    add_test(NAME ipc_plugin_integration_tests
        COMMAND ipc_plugin_integration_tests "${ANOMALY_IPC_PLUGIN_FIXTURE_DIRECTORY}")

    anomaly_add_plugin(faulting_plugin
        SOURCES tests/fixtures/faulting_plugin.cpp
        MANIFEST tests/fixtures/faulting_plugin_manifest.json
        PACKAGE_NAME FaultingPlugin
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/faulting-plugin/Faulting")

    add_executable(plugin_fault_isolation_tests
        tests/integration/plugin_fault_isolation_tests.cpp src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_fault_isolation_tests PRIVATE include)
    target_compile_definitions(
        plugin_fault_isolation_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_fault_isolation_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_fault_isolation_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform)
    add_dependencies(plugin_fault_isolation_tests faulting_plugin)
    add_test(NAME plugin_fault_isolation_tests
        COMMAND plugin_fault_isolation_tests
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/faulting-plugin")

    anomaly_add_plugin(stop_timeout_plugin
        SOURCES tests/fixtures/stop_timeout_plugin.cpp
        MANIFEST tests/fixtures/stop_timeout_plugin_manifest.json
        PACKAGE_NAME StopTimeoutPlugin
        OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/stop-timeout-plugin/StopTimeout")

    add_executable(plugin_stop_quarantine_tests
        tests/integration/plugin_stop_quarantine_tests.cpp src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_stop_quarantine_tests PRIVATE include)
    target_compile_definitions(
        plugin_stop_quarantine_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_stop_quarantine_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_stop_quarantine_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform)
    add_dependencies(plugin_stop_quarantine_tests stop_timeout_plugin)
    add_test(NAME plugin_stop_quarantine_tests
        COMMAND plugin_stop_quarantine_tests
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/stop-timeout-plugin")

    set(ANOMALY_STOP_DEADLINE_FIXTURE_DIRECTORY
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/stop-deadline-plugins")
    anomaly_add_plugin(stop_deadline_fast_plugin
        SOURCES tests/fixtures/stop_deadline_plugin.cpp
        MANIFEST tests/fixtures/stop_deadline_fast_manifest.json
        PACKAGE_NAME Fast
        OUTPUT_DIRECTORY "${ANOMALY_STOP_DEADLINE_FIXTURE_DIRECTORY}/Fast")
    target_compile_definitions(stop_deadline_fast_plugin PRIVATE
        ANOMALY_STOP_FIXTURE_ID="anomaly.fixture.stop-deadline.fast"
        ANOMALY_STOP_FIXTURE_DELAY_MS=0)

    anomaly_add_plugin(stop_deadline_slow_plugin
        SOURCES tests/fixtures/stop_deadline_plugin.cpp
        MANIFEST tests/fixtures/stop_deadline_slow_manifest.json
        PACKAGE_NAME Slow
        OUTPUT_DIRECTORY "${ANOMALY_STOP_DEADLINE_FIXTURE_DIRECTORY}/Slow")
    target_compile_definitions(stop_deadline_slow_plugin PRIVATE
        ANOMALY_STOP_FIXTURE_ID="anomaly.fixture.stop-deadline.slow"
        ANOMALY_STOP_FIXTURE_DELAY_MS=100)

    add_executable(plugin_stop_deadline_tests
        tests/integration/plugin_stop_deadline_tests.cpp src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_stop_deadline_tests PRIVATE include)
    target_compile_definitions(
        plugin_stop_deadline_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_stop_deadline_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_stop_deadline_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services
        anomaly_plugin_runtime anomaly_structured_logger anomaly_scoped_platform)
    add_dependencies(
        plugin_stop_deadline_tests stop_deadline_fast_plugin stop_deadline_slow_plugin)
    add_test(NAME plugin_stop_deadline_tests
        COMMAND plugin_stop_deadline_tests "${ANOMALY_STOP_DEADLINE_FIXTURE_DIRECTORY}")

    add_executable(input_fuzz_tests tests/fuzz/input_fuzz_tests.cpp)
    target_include_directories(input_fuzz_tests PRIVATE include)
    target_compile_definitions(
        input_fuzz_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(input_fuzz_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(input_fuzz_tests PRIVATE
        anomaly_plugin_manifest anomaly_build_profile anomaly_plugin_package ue5mem_core)
    add_test(NAME input_fuzz_tests COMMAND input_fuzz_tests --iterations 1000)
    set_tests_properties(input_fuzz_tests PROPERTIES TIMEOUT 60 LABELS "stability;fuzz")

    add_executable(runtime_performance_tests tests/performance/runtime_performance_tests.cpp)
    target_include_directories(runtime_performance_tests PRIVATE include)
    target_compile_definitions(
        runtime_performance_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(runtime_performance_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(runtime_performance_tests PRIVATE
        anomaly_runtime_kernel anomaly_plugin_runtime anomaly_structured_logger
        anomaly_plugin_shadow Psapi)
    set_target_properties(runtime_performance_tests PROPERTIES
        OUTPUT_NAME "anomaly-performance-tests")
    add_test(NAME runtime_performance_tests COMMAND runtime_performance_tests)
    set_tests_properties(runtime_performance_tests PROPERTIES
        TIMEOUT 30 LABELS "stability;performance")

    add_test(
        NAME render_fixture
        COMMAND anomaly_render_fixture --frames 1000 --resizes 50 --rebuilds 10 --device-rebuilds 10)
    set_tests_properties(render_fixture PROPERTIES
        TIMEOUT 60 LABELS "stability;performance;render")

    add_executable(memory_tests tests/unit/memory_tests.cpp src/platform/windows/memory.cpp src/platform/windows/pattern.cpp)
    target_include_directories(memory_tests PRIVATE include)
    target_compile_definitions(memory_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(memory_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(memory_tests PRIVATE Psapi)
    add_test(NAME memory_tests COMMAND memory_tests)

    add_executable(internal_memory_services_tests tests/unit/internal_memory_services_tests.cpp)
    target_include_directories(internal_memory_services_tests PRIVATE include)
    target_compile_definitions(
        internal_memory_services_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(internal_memory_services_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(internal_memory_services_tests PRIVATE ue5mem_core)
    add_test(NAME internal_memory_services_tests COMMAND internal_memory_services_tests)

    add_executable(pipe_stop_tests tests/unit/pipe_stop_tests.cpp)
    target_include_directories(pipe_stop_tests PRIVATE include)
    target_compile_definitions(pipe_stop_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pipe_stop_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(
        pipe_stop_tests PRIVATE ue5mem_core nlohmann_json::nlohmann_json)
    add_test(NAME pipe_stop_tests COMMAND pipe_stop_tests)
    set_tests_properties(pipe_stop_tests PROPERTIES TIMEOUT 15)

    add_executable(anomaly_cli_client_tests tests/integration/anomaly_cli_client_tests.cpp)
    target_compile_definitions(
        anomaly_cli_client_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly_cli_client_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(
        anomaly_cli_client_tests PRIVATE ue5mem_core nlohmann_json::nlohmann_json)
    add_dependencies(anomaly_cli_client_tests anomaly_cli)
    add_test(
        NAME anomaly_cli_client_tests
        COMMAND anomaly_cli_client_tests $<TARGET_FILE:anomaly_cli>)
    set_tests_properties(anomaly_cli_client_tests PROPERTIES TIMEOUT 20)

    add_executable(runtime_core_integration_tests tests/integration/runtime_core_integration_tests.cpp)
    target_include_directories(runtime_core_integration_tests PRIVATE include)
    target_compile_definitions(
        runtime_core_integration_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(runtime_core_integration_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(runtime_core_integration_tests PRIVATE
        anomaly_runtime_recovery nlohmann_json::nlohmann_json)
    set_target_properties(runtime_core_integration_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/runtime-core")
    add_dependencies(runtime_core_integration_tests anomaly_core)
    add_test(
        NAME runtime_core_integration_tests
        COMMAND runtime_core_integration_tests $<TARGET_FILE:anomaly_core>)

    add_executable(proxy_smoke tests/integration/proxy_smoke.cpp)
    target_include_directories(proxy_smoke PRIVATE include)
    target_compile_definitions(proxy_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(proxy_smoke PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(proxy_smoke PRIVATE nlohmann_json::nlohmann_json)
    add_dependencies(proxy_smoke anomaly_core)
    add_test(NAME proxy_smoke COMMAND proxy_smoke $<TARGET_FILE:dwmapi>)

    add_library(raw_memory_capability_fixture SHARED
        tests/fixtures/raw_memory_capability_fixture.c)
    target_link_libraries(raw_memory_capability_fixture PRIVATE Anomaly::sdk)
    target_compile_options(raw_memory_capability_fixture PRIVATE /W4)
    set_target_properties(raw_memory_capability_fixture PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        OUTPUT_NAME "plugin"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-manager/raw-memory-capability-fixture"
        LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/tests/plugin-manager/raw-memory-capability-fixture"
        ARCHIVE_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-manager/raw-memory-capability-fixture"
        PDB_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-manager/raw-memory-capability-fixture")

    add_library(config_persistence_fixture SHARED
        tests/fixtures/config_persistence_fixture.c)
    target_link_libraries(config_persistence_fixture PRIVATE Anomaly::sdk)
    target_compile_options(config_persistence_fixture PRIVATE /W4)
    set_target_properties(config_persistence_fixture PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        OUTPUT_NAME "plugin"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-config-persistence-fixture"
        LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/tests/plugin-config-persistence-fixture"
        ARCHIVE_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-config-persistence-fixture"
        PDB_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-config-persistence-fixture")

    add_executable(plugin_manager_tests tests/integration/plugin_manager_tests.cpp src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_manager_tests PRIVATE include)
    target_compile_definitions(plugin_manager_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_manager_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(
        plugin_manager_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform nlohmann_json::nlohmann_json)
    add_dependencies(plugin_manager_tests
        anomaly_plugin_manager_fixture raw_memory_capability_fixture)
    add_test(
        NAME plugin_manager_tests
        COMMAND plugin_manager_tests
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-manager")

    add_executable(
        plugin_ui_window_persistence_tests
        tests/integration/plugin_ui_window_persistence_tests.cpp
        src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_ui_window_persistence_tests PRIVATE include)
    target_compile_definitions(
        plugin_ui_window_persistence_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_ui_window_persistence_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_ui_window_persistence_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform)
    add_test(
        NAME plugin_ui_window_persistence_tests
        COMMAND plugin_ui_window_persistence_tests
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-ui-window-persistence")

    add_executable(
        plugin_config_persistence_tests
        tests/integration/plugin_config_persistence_tests.cpp
        src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_config_persistence_tests PRIVATE include)
    target_compile_definitions(
        plugin_config_persistence_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_config_persistence_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_config_persistence_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform)
    add_dependencies(plugin_config_persistence_tests config_persistence_fixture)
    add_test(
        NAME plugin_config_persistence_tests
        COMMAND plugin_config_persistence_tests "$<TARGET_FILE:config_persistence_fixture>")

    add_library(ui_frame_scope_plugin SHARED tests/fixtures/ui_frame_scope_plugin.cpp)
    target_link_libraries(ui_frame_scope_plugin PRIVATE Anomaly::sdk)
    target_compile_options(ui_frame_scope_plugin PRIVATE /W4 /permissive- /EHsc)

    add_executable(
        plugin_ui_frame_scope_tests
        tests/integration/plugin_ui_frame_scope_tests.cpp
        src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_ui_frame_scope_tests PRIVATE include)
    target_compile_definitions(
        plugin_ui_frame_scope_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_ui_frame_scope_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_ui_frame_scope_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform anomaly_ui_resources nlohmann_json::nlohmann_json)
    add_dependencies(plugin_ui_frame_scope_tests ui_frame_scope_plugin)
    add_test(
        NAME plugin_ui_frame_scope_tests
        COMMAND plugin_ui_frame_scope_tests "$<TARGET_FILE:ui_frame_scope_plugin>")

    add_library(ui_draw_fault_plugin SHARED tests/fixtures/ui_draw_fault_plugin.cpp)
    target_link_libraries(ui_draw_fault_plugin PRIVATE Anomaly::sdk)
    target_compile_options(ui_draw_fault_plugin PRIVATE /W4 /permissive- /EHsc)
    set_target_properties(ui_draw_fault_plugin PROPERTIES
        OUTPUT_NAME "plugin"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/ui-draw-fault-plugin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/tests/ui-draw-fault-plugin")

    add_executable(
        plugin_ui_draw_fault_tests
        tests/integration/plugin_ui_draw_fault_tests.cpp
        src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_ui_draw_fault_tests PRIVATE include)
    target_compile_definitions(
        plugin_ui_draw_fault_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_ui_draw_fault_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_ui_draw_fault_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform anomaly_ui_resources nlohmann_json::nlohmann_json)
    add_dependencies(plugin_ui_draw_fault_tests ui_draw_fault_plugin)
    add_test(
        NAME plugin_ui_draw_fault_tests
        COMMAND plugin_ui_draw_fault_tests "$<TARGET_FILE:ui_draw_fault_plugin>")

    add_library(native_dependency_private SHARED
        tests/fixtures/native_dependency_private.cpp)
    target_compile_definitions(
        native_dependency_private PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(native_dependency_private PRIVATE /W4 /permissive- /EHsc)
    set_target_properties(native_dependency_private PROPERTIES
        OUTPUT_NAME "anomaly_fixture_private_dependency"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/native-dependency-fixture"
        LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/tests/native-dependency-fixture")

    add_library(native_dependency_plugin SHARED
        tests/fixtures/native_dependency_plugin.cpp)
    target_link_libraries(
        native_dependency_plugin PRIVATE Anomaly::sdk native_dependency_private)
    target_compile_definitions(
        native_dependency_plugin PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(native_dependency_plugin PRIVATE /W4 /permissive- /EHsc)
    set_target_properties(native_dependency_plugin PROPERTIES
        OUTPUT_NAME "plugin"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/native-dependency-fixture"
        LIBRARY_OUTPUT_DIRECTORY
            "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/tests/native-dependency-fixture")

    add_executable(plugin_native_dependency_tests
        tests/integration/plugin_native_dependency_tests.cpp src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_native_dependency_tests PRIVATE include)
    target_compile_definitions(
        plugin_native_dependency_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_native_dependency_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_native_dependency_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services
        anomaly_plugin_runtime anomaly_structured_logger anomaly_scoped_platform)
    add_dependencies(
        plugin_native_dependency_tests native_dependency_plugin native_dependency_private)
    add_test(
        NAME plugin_native_dependency_tests
        COMMAND plugin_native_dependency_tests
            "$<TARGET_FILE:native_dependency_plugin>"
            "$<TARGET_FILE:native_dependency_private>")

    foreach(enablement_fixture IN ITEMS stable base child)
        add_library(enablement_${enablement_fixture}_plugin SHARED
            tests/fixtures/enablement_fixture_plugin.cpp)
        target_link_libraries(enablement_${enablement_fixture}_plugin PRIVATE Anomaly::sdk)
        target_compile_definitions(enablement_${enablement_fixture}_plugin PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
            "ANOMALY_ENABLEMENT_FIXTURE_ID=\"anomaly.fixture.enablement-${enablement_fixture}\"")
        target_compile_options(enablement_${enablement_fixture}_plugin PRIVATE /W4 /permissive- /EHsc)
        set_target_properties(enablement_${enablement_fixture}_plugin PROPERTIES
            OUTPUT_NAME "enablement-${enablement_fixture}"
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/plugin-enablement-fixtures"
            LIBRARY_OUTPUT_DIRECTORY
                "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/tests/plugin-enablement-fixtures")
    endforeach()
    add_executable(plugin_enablement_reconcile_tests
        tests/integration/plugin_enablement_reconcile_tests.cpp
        src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_enablement_reconcile_tests PRIVATE include)
    target_compile_definitions(plugin_enablement_reconcile_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_enablement_reconcile_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_enablement_reconcile_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services
        anomaly_plugin_runtime anomaly_structured_logger anomaly_scoped_platform)
    add_dependencies(plugin_enablement_reconcile_tests
        enablement_stable_plugin enablement_base_plugin enablement_child_plugin
        stop_timeout_plugin)
    add_test(NAME plugin_enablement_reconcile_tests
        COMMAND plugin_enablement_reconcile_tests
            "$<TARGET_FILE:enablement_stable_plugin>"
            "$<TARGET_FILE:enablement_base_plugin>"
            "$<TARGET_FILE:enablement_child_plugin>"
            "$<TARGET_FILE:stop_timeout_plugin>")

    add_executable(plugin_reload_rollback_tests
        tests/integration/plugin_reload_rollback_tests.cpp src/plugin/plugin_manager.cpp)
    target_include_directories(plugin_reload_rollback_tests PRIVATE include)
    target_compile_definitions(
        plugin_reload_rollback_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(plugin_reload_rollback_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(plugin_reload_rollback_tests PRIVATE
        ue5mem_core anomaly_plugin_shadow anomaly_plugin_dependency_resolver
        anomaly_plugin_enablement anomaly_adapter_services anomaly_plugin_runtime
        anomaly_structured_logger anomaly_scoped_platform)
    add_dependencies(plugin_reload_rollback_tests anomaly_plugin_manager_fixture)
    add_test(NAME plugin_reload_rollback_tests
        COMMAND plugin_reload_rollback_tests "$<TARGET_FILE_DIR:anomaly_plugin_manager_fixture>")

    foreach(plugin_manager_consumer IN ITEMS
            platform_ui_window_registry_tests
            ipc_plugin_integration_tests
            plugin_fault_isolation_tests
            plugin_stop_quarantine_tests
            plugin_stop_deadline_tests
            plugin_manager_tests
            plugin_ui_window_persistence_tests
            plugin_config_persistence_tests
            plugin_ui_frame_scope_tests
            plugin_ui_draw_fault_tests
            plugin_native_dependency_tests
            plugin_enablement_reconcile_tests
            plugin_reload_rollback_tests)
        target_link_libraries(
            ${plugin_manager_consumer} PRIVATE
            anomaly_ui_resources anomaly_i18n nlohmann_json::nlohmann_json)
    endforeach()

    add_executable(build_profile_tests tests/unit/build_profile_tests.cpp)
    target_include_directories(build_profile_tests PRIVATE include)
    target_compile_definitions(
        build_profile_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(build_profile_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(build_profile_tests PRIVATE anomaly_build_profile)
    add_test(NAME build_profile_tests COMMAND build_profile_tests)

    add_executable(pink_paw_rob_bank_runtime_tests
        tests/unit/pink_paw_rob_bank_runtime_tests.cpp
        plugins/PinkPawHeistESP/rob_bank_runtime.cpp)
    target_include_directories(pink_paw_rob_bank_runtime_tests PRIVATE
        include "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(pink_paw_rob_bank_runtime_tests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pink_paw_rob_bank_runtime_tests PRIVATE
        /W4 /permissive- /EHsc)
    target_link_libraries(pink_paw_rob_bank_runtime_tests PRIVATE Anomaly::sdk)
    add_test(NAME pink_paw_rob_bank_runtime_tests
        COMMAND pink_paw_rob_bank_runtime_tests)

    if(TARGET anomaly_builtin_fake_uid)
        add_executable(fake_uid_plugin_tests
            tests/integration/fake_uid_plugin_tests.cpp)
        target_link_libraries(fake_uid_plugin_tests PRIVATE Anomaly::sdk)
        target_compile_definitions(fake_uid_plugin_tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(fake_uid_plugin_tests PRIVATE /W4 /permissive- /EHsc)
        add_dependencies(fake_uid_plugin_tests anomaly_builtin_fake_uid)
        add_test(NAME fake_uid_plugin_tests
            COMMAND fake_uid_plugin_tests $<TARGET_FILE:anomaly_builtin_fake_uid>)
    endif()

    add_executable(symbol_resolver_tests tests/unit/symbol_resolver_tests.cpp)
    target_include_directories(symbol_resolver_tests PRIVATE include)
    target_compile_definitions(
        symbol_resolver_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(symbol_resolver_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(symbol_resolver_tests PRIVATE anomaly_symbol_resolver)
    add_test(NAME symbol_resolver_tests COMMAND symbol_resolver_tests)

    add_executable(ue5_nte_adapter_tests tests/unit/ue5_nte_adapter_tests.cpp)
    target_include_directories(ue5_nte_adapter_tests PRIVATE include)
    target_compile_definitions(
        ue5_nte_adapter_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ue5_nte_adapter_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(ue5_nte_adapter_tests PRIVATE anomaly_adapter_services)
    add_test(NAME ue5_nte_adapter_tests COMMAND ue5_nte_adapter_tests)

    add_executable(game_tick_hook_tests tests/unit/game_tick_hook_tests.cpp)
    target_include_directories(game_tick_hook_tests PRIVATE include)
    target_compile_definitions(
        game_tick_hook_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(game_tick_hook_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(game_tick_hook_tests PRIVATE anomaly_adapter_services)
    add_test(NAME game_tick_hook_tests COMMAND game_tick_hook_tests)

    add_executable(
        ue5_outbound_bit_count_probe_tests
        tests/unit/ue5_outbound_bit_count_probe_tests.cpp)
    target_include_directories(ue5_outbound_bit_count_probe_tests PRIVATE include)
    target_compile_definitions(
        ue5_outbound_bit_count_probe_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ue5_outbound_bit_count_probe_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(
        ue5_outbound_bit_count_probe_tests PRIVATE anomaly_adapter_services)
    add_test(
        NAME ue5_outbound_bit_count_probe_tests
        COMMAND ue5_outbound_bit_count_probe_tests)

    add_executable(nte_profile_runtime_tests tests/unit/nte_profile_runtime_tests.cpp)
    target_include_directories(nte_profile_runtime_tests PRIVATE include)
    target_compile_definitions(
        nte_profile_runtime_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(nte_profile_runtime_tests PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(nte_profile_runtime_tests PRIVATE anomaly_nte_profile_runtime)
    add_test(NAME nte_profile_runtime_tests COMMAND nte_profile_runtime_tests)

    add_executable(sdk_cpp_contract tests/contract/sdk_cpp_contract.cpp)
    target_link_libraries(sdk_cpp_contract PRIVATE Anomaly::sdk)
    target_compile_options(sdk_cpp_contract PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME sdk_cpp_contract COMMAND sdk_cpp_contract)

    add_executable(sdk_service_version_contract tests/contract/sdk_service_version_contract.c)
    target_link_libraries(sdk_service_version_contract PRIVATE Anomaly::sdk)
    target_compile_options(sdk_service_version_contract PRIVATE /W4)
    set_target_properties(sdk_service_version_contract PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME sdk_service_version_contract COMMAND sdk_service_version_contract)

    add_library(service_query_plugin SHARED tests/fixtures/service_query_plugin.c)
    target_link_libraries(service_query_plugin PRIVATE Anomaly::sdk)
    target_compile_options(service_query_plugin PRIVATE /W4)
    set_target_properties(service_query_plugin PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        OUTPUT_NAME "plugin"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/service-query"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/tests/service-query"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/service-query"
        PDB_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests/service-query")
    add_custom_command(TARGET service_query_plugin POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/tests/fixtures/service_query_manifest.json"
                "$<TARGET_FILE_DIR:service_query_plugin>/manifest.json")
    add_dependencies(anomaly_test_host service_query_plugin)
    add_test(NAME service_version_runtime_contract
        COMMAND anomaly_test_host
            --plugin "$<TARGET_FILE_DIR:service_query_plugin>" --reload 2 --ticks 2)

    add_test(NAME sdk_abi_snapshot
        COMMAND anomaly_abi_snapshot --check
            "${CMAKE_SOURCE_DIR}/abi/anomaly-sdk-v1-windows-x64.json")

    if(ANOMALY_BUILD_SDK_EXAMPLES)
        set(ANOMALY_RELOAD_PRIVATE_GROWTH_BUDGET_BYTES 10485760)
        if(ANOMALY_ENABLE_ASAN)
            set(ANOMALY_RELOAD_PRIVATE_GROWTH_BUDGET_BYTES 67108864)
        endif()
        add_test(NAME anomaly_reload_soak
            COMMAND anomaly_test_host --plugin "$<TARGET_FILE_DIR:anomaly_example_hello_ui>"
                --reload 100 --ticks 4
                --private-growth-budget-bytes ${ANOMALY_RELOAD_PRIVATE_GROWTH_BUDGET_BYTES})
        set_tests_properties(anomaly_reload_soak PROPERTIES
            TIMEOUT 120 LABELS "stability;soak")
        add_dependencies(anomaly_test_host anomaly_example_hello_ui anomaly_example_tick_counter
            anomaly_example_reliable_config anomaly_example_nte_inspector)
    endif()


    if(IS_ABSOLUTE "${CMAKE_MAKE_PROGRAM}")
        set(ANOMALY_SDK_TEST_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
    else()
        find_program(ANOMALY_SDK_TEST_MAKE_PROGRAM NAMES "${CMAKE_MAKE_PROGRAM}" REQUIRED)
    endif()
    add_test(NAME sdk_external_consumer
        COMMAND "${CMAKE_COMMAND}"
            "-DANOMALY_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DANOMALY_BINARY_DIR=${CMAKE_BINARY_DIR}"
            "-DANOMALY_CMAKE=${CMAKE_COMMAND}"
            "-DANOMALY_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DANOMALY_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DANOMALY_LINKER=${CMAKE_LINKER}"
            "-DANOMALY_RC_COMPILER=${CMAKE_RC_COMPILER}"
            "-DANOMALY_MT=${CMAKE_MT}"
            "-DANOMALY_MAKE_PROGRAM=${ANOMALY_SDK_TEST_MAKE_PROGRAM}"
            "-DANOMALY_GENERATOR=${CMAKE_GENERATOR}"
            "-DANOMALY_CONFIG=$<CONFIG>"
            "-DANOMALY_PLUGIN_TOOL=$<TARGET_FILE:anomaly-plugin>"
            "-DANOMALY_TEST_HOST=$<TARGET_FILE:anomaly_test_host>"
            -P "${CMAKE_SOURCE_DIR}/tests/fixtures/sdk_consumer_test.cmake")
    set_tests_properties(sdk_external_consumer PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

    if(ANOMALY_ENABLE_ASAN)
        get_property(ANOMALY_CTEST_TESTS DIRECTORY PROPERTY TESTS)
        set_property(TEST ${ANOMALY_CTEST_TESTS} APPEND PROPERTY
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${ANOMALY_ASAN_RUNTIME_DIRECTORY}")
    endif()
endif()

set(ANOMALY_RELEASE_PE_TARGETS
    dwmapi anomaly_core anomaly_launcher anomaly_crash_coordinator
    anomaly-plugin anomaly_abi_snapshot anomaly_test_host
    anomaly-profile anomaly_cli anomaly_inspect)
foreach(runtime_plugin_target IN ITEMS
        anomaly_example_hello_ui anomaly_example_tick_counter
        anomaly_example_reliable_config anomaly_example_nte_inspector
        anomaly_builtin_nte_position anomaly_builtin_nte_teleport
        anomaly_builtin_entity_esp anomaly_builtin_pink_paw_heist_esp
        anomaly_builtin_fake_uid)
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
    install(DIRECTORY plugins/PinkPawHeistESP/assets/
        DESTINATION Anomaly/plugins/PinkPawHeistESP/assets COMPONENT GameRuntime)
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
