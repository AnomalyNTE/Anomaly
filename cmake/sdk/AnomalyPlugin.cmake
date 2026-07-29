include(CMakeParseArguments)

function(anomaly_add_plugin target)
    set(options C_ONLY)
    set(oneValueArgs MANIFEST PACKAGE_NAME OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ANOMALY "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
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
        RUNTIME_OUTPUT_DIRECTORY "$<1:${ANOMALY_OUTPUT_DIRECTORY}>"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${ANOMALY_OUTPUT_DIRECTORY}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${ANOMALY_OUTPUT_DIRECTORY}>"
        PDB_OUTPUT_DIRECTORY "$<1:${ANOMALY_OUTPUT_DIRECTORY}>")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
        if(NOT ANOMALY_C_ONLY)
            target_compile_options(${target} PRIVATE /EHsc)
        endif()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ANOMALY_MANIFEST}" "$<TARGET_FILE_DIR:${target}>/manifest.json"
        VERBATIM)
    set_property(TARGET ${target} PROPERTY ANOMALY_PACKAGE_DIRECTORY "${ANOMALY_OUTPUT_DIRECTORY}")
endfunction()
