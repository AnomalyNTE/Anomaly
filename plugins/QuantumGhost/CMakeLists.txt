cmake_minimum_required(VERSION 3.22)

project(QuantumGhost LANGUAGES CXX ASM_MASM)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Quantum Ghost targets Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 只面向运行时安装的 SDK，不回退到仓库内 include（头与运行时不匹配会导致服务协商失败）。
find_package(AnomalySDK CONFIG REQUIRED)

set(QUANTUM_GHOST_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package/QuantumGhost")

anomaly_add_plugin(quantum_ghost
    SOURCES
        plugin.cpp
        scale_stub.asm
    MANIFEST manifest.json
    PACKAGE_NAME QuantumGhost
    OUTPUT_DIRECTORY "${QUANTUM_GHOST_PACKAGE_DIRECTORY}")

target_compile_definitions(quantum_ghost PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# 界面文案为中文，显式声明源文件编码。
target_compile_options(quantum_ghost PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
