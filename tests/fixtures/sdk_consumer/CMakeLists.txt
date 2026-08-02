cmake_minimum_required(VERSION 3.22)
project(AnomalySdkConsumer LANGUAGES C CXX)
find_package(AnomalySDK CONFIG REQUIRED PATHS "${ANOMALY_SDK_ROOT}" NO_DEFAULT_PATH)
anomaly_add_plugin(external_sdk_plugin
    SOURCES plugin.cpp
    MANIFEST manifest.json
    PACKAGE_NAME ExternalSdkPlugin
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/ExternalSdkPlugin")
anomaly_add_plugin(external_c_sdk_plugin C_ONLY
    SOURCES plugin_c.c
    MANIFEST manifest_c.json
    PACKAGE_NAME ExternalCSdkPlugin
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/package/ExternalCSdkPlugin")
