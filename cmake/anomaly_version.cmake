function(anomaly_resolve_version release_version project_output version_output)
    if(NOT release_version MATCHES
       "^v?([0-9]+\\.[0-9]+\\.[0-9]+)([-+][0-9A-Za-z.+-]+)?$")
        message(FATAL_ERROR
            "Anomaly release version must start with vMAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH: ${release_version}")
    endif()

    set(${project_output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(${version_output} "${CMAKE_MATCH_1}${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
