include(CMakeParseArguments)

# The SDK distribution installs the bridge source next to this file
# (share/anomaly/cpp), so installed-package consumers find it through the same
# relative path as the in-tree build.
set(ANOMALY_CXX_THROW_BRIDGE "${CMAKE_CURRENT_LIST_DIR}/../../sdk/cpp/plugin_cxx_throw_bridge.cpp"
    CACHE FILEPATH "Plugin C++ exception bridge source compiled into every plugin package")

# anomaly_add_plugin(<target>
#     SOURCES <source>...
#     MANIFEST <manifest.json>
#     [PACKAGE_NAME <name>] [OUTPUT_DIRECTORY <directory>]
#     [C_ONLY] [NO_RELEASE] [TEST_PLUGIN])
#
# Builds one plugin package and owns its packaging rules, so a package can never be
# missing from a runtime tree: `plugin.dll` and the manifest land in the package
# directory, and the same package directory is what gets installed.
#
#   - the package installs into `Anomaly/plugins/<PACKAGE_NAME>` of the GameRuntime
#     component, and the `locales` directory next to the manifest is mirrored into the
#     package directory and installed with it;
#   - TEST_PLUGIN marks a developer test package: it is built and installed only when
#     ANOMALY_BUILD_TEST_PLUGINS is ON, and it then installs into the TestPlugins component
#     instead of GameRuntime;
#   - NO_RELEASE builds the package without installing it at all.
#
# C++ packages additionally compile the SDK exception bridge
# (`sdk/cpp/plugin_cxx_throw_bridge.cpp` unless ANOMALY_CXX_THROW_BRIDGE overrides the
# path) because the runtime maps plugins without the Windows loader, whose services
# vcruntime's _CxxThrowException depends on.
function(anomaly_add_plugin target)
    set(options C_ONLY NO_RELEASE TEST_PLUGIN)
    set(oneValueArgs MANIFEST PACKAGE_NAME OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ANOMALY "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    # A test package is opt-in: without the switch it is not built at all, so nothing has to
    # depend on, install or validate a target that does not exist. ANOMALY_BUILD_TEST_PLUGINS
    # is declared by the Anomaly build; a project that does not define it builds no test
    # package.
    if(ANOMALY_TEST_PLUGIN AND NOT ANOMALY_BUILD_TEST_PLUGINS)
        return()
    endif()
    if(NOT ANOMALY_SOURCES)
        message(FATAL_ERROR "anomaly_add_plugin(${target}) requires SOURCES")
    endif()
    if(NOT ANOMALY_MANIFEST)
        message(FATAL_ERROR "anomaly_add_plugin(${target}) requires MANIFEST")
    endif()
    if(NOT IS_ABSOLUTE "${ANOMALY_MANIFEST}")
        set(ANOMALY_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/${ANOMALY_MANIFEST}")
    endif()
    if(NOT EXISTS "${ANOMALY_MANIFEST}")
        message(FATAL_ERROR "plugin manifest does not exist: ${ANOMALY_MANIFEST}")
    endif()
    if(NOT ANOMALY_PACKAGE_NAME)
        set(ANOMALY_PACKAGE_NAME "${target}")
    endif()
    if(NOT ANOMALY_OUTPUT_DIRECTORY)
        set(ANOMALY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/${ANOMALY_PACKAGE_NAME}")
    endif()
    add_library(${target} SHARED ${ANOMALY_SOURCES})
    target_link_libraries(${target} PRIVATE Anomaly::sdk)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "plugin"
        # Every plugin package produces `plugin.dll`, so the symbols are named after the
        # package instead: a shared PDB name would make the packages indistinguishable in
        # the Symbols component.
        PDB_NAME "${ANOMALY_PACKAGE_NAME}"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${ANOMALY_OUTPUT_DIRECTORY}>"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${ANOMALY_OUTPUT_DIRECTORY}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${ANOMALY_OUTPUT_DIRECTORY}>"
        PDB_OUTPUT_DIRECTORY "$<1:${ANOMALY_OUTPUT_DIRECTORY}>")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
        if(NOT ANOMALY_C_ONLY)
            # The runtime maps plugin images without the Windows loader, so a plugin
            # image must not depend on loader services:
            #   - /EHsc C++ plugins throw through the SDK exception bridge source below
            #     instead of vcruntime's loader-assisted _CxxThrowException;
            #   - /Zc:threadSafeInit- keeps function-local static guards out of the
            #     loader-managed TLS slot the mapper cannot reserve.
            target_compile_options(${target} PRIVATE /EHsc /Zc:threadSafeInit-)
            target_sources(${target} PRIVATE "${ANOMALY_CXX_THROW_BRIDGE}")
        endif()
    endif()
    # The manifest sits in the plugin source directory, so that directory describes the
    # package contents: the manifest itself, plus `locales` when the plugin ships
    # localized catalogs. Both are mirrored into the build-tree package and installed.
    get_filename_component(anomaly_package_source_directory "${ANOMALY_MANIFEST}" DIRECTORY)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ANOMALY_MANIFEST}" "$<TARGET_FILE_DIR:${target}>/manifest.json"
        VERBATIM)
    if(EXISTS "${anomaly_package_source_directory}/locales")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${anomaly_package_source_directory}/locales"
                    "$<TARGET_FILE_DIR:${target}>/locales"
            VERBATIM)
    endif()
    set_property(TARGET ${target} PROPERTY ANOMALY_PACKAGE_DIRECTORY "${ANOMALY_OUTPUT_DIRECTORY}")
    if(ANOMALY_NO_RELEASE)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY ANOMALY_RELEASE_PLUGIN_TARGETS "${target}")
    if(ANOMALY_TEST_PLUGIN)
        set(anomaly_package_component TestPlugins)
    else()
        set(anomaly_package_component GameRuntime)
    endif()
    set(anomaly_package_directory "Anomaly/plugins/${ANOMALY_PACKAGE_NAME}")
    install(TARGETS ${target}
        RUNTIME DESTINATION "${anomaly_package_directory}" COMPONENT ${anomaly_package_component}
        LIBRARY DESTINATION "${anomaly_package_directory}" COMPONENT ${anomaly_package_component})
    install(FILES "${ANOMALY_MANIFEST}"
        DESTINATION "${anomaly_package_directory}" COMPONENT ${anomaly_package_component})
    if(EXISTS "${anomaly_package_source_directory}/locales")
        install(DIRECTORY "${anomaly_package_source_directory}/locales/"
            DESTINATION "${anomaly_package_directory}/locales"
            COMPONENT ${anomaly_package_component})
    endif()
endfunction()
