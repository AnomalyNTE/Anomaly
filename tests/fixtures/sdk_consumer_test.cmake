if(NOT DEFINED ANOMALY_SOURCE_DIR OR NOT DEFINED ANOMALY_BINARY_DIR OR
   NOT DEFINED ANOMALY_CMAKE OR NOT DEFINED ANOMALY_PLUGIN_TOOL OR
   NOT DEFINED ANOMALY_TEST_HOST OR NOT DEFINED ANOMALY_GENERATOR)
    message(FATAL_ERROR "missing SDK consumer test variables")
endif()
set(prefix "${ANOMALY_BINARY_DIR}/sdk-test-install")
set(build "${ANOMALY_BINARY_DIR}/sdk-consumer-build")
set(installed_examples_build "${ANOMALY_BINARY_DIR}/sdk-installed-examples-build")
set(local_runtime "${ANOMALY_BINARY_DIR}/sdk-local-runtime")
file(REMOVE_RECURSE "${prefix}" "${build}" "${installed_examples_build}" "${local_runtime}")
execute_process(
    COMMAND "${ANOMALY_CMAKE}" --install "${ANOMALY_BINARY_DIR}" --prefix "${prefix}" --component SDK
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "SDK install failed: ${result}")
endif()
execute_process(
    COMMAND "${ANOMALY_CMAKE}" -S "${ANOMALY_SOURCE_DIR}/tests/fixtures/sdk_consumer" -B "${build}"
        -G "${ANOMALY_GENERATOR}" "-DCMAKE_CXX_COMPILER=${ANOMALY_CXX_COMPILER}"
        "-DCMAKE_C_COMPILER=${ANOMALY_C_COMPILER}"
        "-DCMAKE_LINKER=${ANOMALY_LINKER}"
        "-DCMAKE_RC_COMPILER=${ANOMALY_RC_COMPILER}"
        "-DCMAKE_MT=${ANOMALY_MT}"
        "-DCMAKE_MAKE_PROGRAM=${ANOMALY_MAKE_PROGRAM}"
        "-DANOMALY_SDK_ROOT=${prefix}/lib/cmake/AnomalySDK"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "consumer configure failed: ${result}")
endif()
execute_process(
    COMMAND "${ANOMALY_CMAKE}" --build "${build}" --config "${ANOMALY_CONFIG}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "consumer build failed: ${result}")
endif()
if(NOT EXISTS "${build}/package/ExternalSdkPlugin/plugin.dll" OR
   NOT EXISTS "${build}/package/ExternalSdkPlugin/manifest.json" OR
   NOT EXISTS "${build}/package/ExternalCSdkPlugin/plugin.dll" OR
   NOT EXISTS "${build}/package/ExternalCSdkPlugin/manifest.json")
    message(FATAL_ERROR "external package output is incomplete")
endif()
set(packed "${build}/packed")
execute_process(
    COMMAND "${ANOMALY_PLUGIN_TOOL}" pack "${build}/package/ExternalSdkPlugin" --output "${packed}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "external package pack failed: ${result}")
endif()
set(packed_package "${packed}/example.external-sdk")
execute_process(COMMAND "${ANOMALY_PLUGIN_TOOL}" validate "${packed_package}" RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT EXISTS "${packed_package}/package.sha256")
    message(FATAL_ERROR "packed external package validation failed: ${result}")
endif()
set(local_plugins "${local_runtime}/Anomaly/plugins")
file(MAKE_DIRECTORY "${local_plugins}")
file(COPY "${packed_package}" DESTINATION "${local_plugins}")
set(installed_package "${local_plugins}/example.external-sdk")
execute_process(
    COMMAND "${ANOMALY_PLUGIN_TOOL}" validate "${installed_package}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "locally installed external package validation failed: ${result}")
endif()
execute_process(
    COMMAND "${ANOMALY_TEST_HOST}" --plugin "${installed_package}" --reload 2 --ticks 3
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "locally installed external package TestHost run failed: ${result}")
endif()

execute_process(
    COMMAND "${ANOMALY_TEST_HOST}"
        --plugin "${build}/package/ExternalCSdkPlugin" --reload 2 --ticks 3
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "external C package TestHost run failed: ${result}")
endif()

# This uses the installed example source and package config rather than the
# repository's source-tree targets.
execute_process(
    COMMAND "${ANOMALY_CMAKE}" -S "${prefix}/share/anomaly/examples" -B "${installed_examples_build}"
        -G "${ANOMALY_GENERATOR}" "-DCMAKE_CXX_COMPILER=${ANOMALY_CXX_COMPILER}"
        "-DCMAKE_C_COMPILER=${ANOMALY_C_COMPILER}"
        "-DCMAKE_LINKER=${ANOMALY_LINKER}"
        "-DCMAKE_RC_COMPILER=${ANOMALY_RC_COMPILER}"
        "-DCMAKE_MT=${ANOMALY_MT}"
        "-DCMAKE_MAKE_PROGRAM=${ANOMALY_MAKE_PROGRAM}"
        "-DAnomalySDK_DIR=${prefix}/lib/cmake/AnomalySDK"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed SDK examples configure failed: ${result}")
endif()
execute_process(
    COMMAND "${ANOMALY_CMAKE}" --build "${installed_examples_build}" --config "${ANOMALY_CONFIG}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed SDK examples build failed: ${result}")
endif()
foreach(example IN ITEMS HelloUi TickCounter ReliableConfig NteInspector)
    set(installed_example "${installed_examples_build}/packages/${example}")
    if(NOT EXISTS "${installed_example}/plugin.dll" OR
       NOT EXISTS "${installed_example}/manifest.json")
        message(FATAL_ERROR "installed ${example} package output is incomplete")
    endif()
    execute_process(
        COMMAND "${ANOMALY_PLUGIN_TOOL}" validate "${installed_example}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "installed ${example} package validation failed: ${result}")
    endif()
    execute_process(
        COMMAND "${ANOMALY_TEST_HOST}" --plugin "${installed_example}" --reload 2 --ticks 3
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "installed ${example} TestHost run failed: ${result}")
    endif()
endforeach()
