include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/anomaly_version.cmake")

function(expect_version release_version expected_project_version expected_version)
    anomaly_resolve_version("${release_version}" actual_project_version actual_version)
    if(NOT actual_project_version STREQUAL expected_project_version OR
       NOT actual_version STREQUAL expected_version)
        message(FATAL_ERROR
            "release version ${release_version} resolved to project=${actual_project_version}, display=${actual_version}; expected project=${expected_project_version}, display=${expected_version}")
    endif()
endfunction()

expect_version("1.0.1" "1.0.1" "1.0.1")
expect_version("v2.3.4" "2.3.4" "2.3.4")
expect_version("v5.6.7-rc.2+build.9" "5.6.7" "5.6.7-rc.2+build.9")
