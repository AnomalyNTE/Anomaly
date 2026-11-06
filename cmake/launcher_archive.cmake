# The mod downloader ships its decoders; it does not require a system archiver.
# Keep third-party options local to this function and out of the Core build.
function(anomaly_add_launcher_archive)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    # Preserve local overrides when older dependencies first create cache entries.
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_BUILD_STATIC ON)
    set(ZLIB_INSTALL OFF)
    FetchContent_Declare(anomaly_zlib
        URL https://zlib.net/zlib-1.4.tar.gz
        URL_HASH SHA256=02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR anomaly-no-cmake)
    FetchContent_Declare(anomaly_lzma
        URL https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.xz
        URL_HASH SHA256=4ce24038fd4221e0d13bc1a2de7a4db56e90b92b3bf75321f6c14be73f65de4b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR anomaly-no-cmake)
    FetchContent_Declare(anomaly_libarchive
        URL https://github.com/libarchive/libarchive/releases/download/v3.8.11/libarchive-3.8.11.tar.xz
        URL_HASH SHA256=a3dc0872b1b39da34dc18ef1b944f0f11ffd37425d70ae04be679279f1cf43d2
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR anomaly-no-cmake)
    FetchContent_MakeAvailable(anomaly_zlib anomaly_lzma anomaly_libarchive)
    if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "MSVC"
            AND CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher_archive_msvc.cmake")
    endif()
    add_subdirectory("${anomaly_zlib_SOURCE_DIR}" "${anomaly_zlib_BINARY_DIR}" EXCLUDE_FROM_ALL)
    foreach(option IN ITEMS XZ_TOOL_XZ XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO
            XZ_TOOL_SCRIPTS XZ_DOC XZ_NLS)
        set(${option} OFF)
    endforeach()
    add_subdirectory("${anomaly_lzma_SOURCE_DIR}" "${anomaly_lzma_BINARY_DIR}" EXCLUDE_FROM_ALL)

    # These dependencies are source targets, not installed libraries that find_package can open.
    # Their static C ABI is fixed by the options above; don't link-probe an unbuilt target.
    set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
    set(ZLIB_FOUND TRUE)
    set(ZLIB_INCLUDE_DIR "${anomaly_zlib_SOURCE_DIR};${anomaly_zlib_BINARY_DIR}")
    set(ZLIB_LIBRARIES zlibstatic)
    set(ZLIB_WINAPI FALSE)
    set(ZLIB_DLL FALSE)
    set(WITHOUT_ZLIB_DLL TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_LibLZMA TRUE)
    set(LIBLZMA_FOUND TRUE)
    set(LIBLZMA_INCLUDE_DIR "${anomaly_lzma_SOURCE_DIR}/src/liblzma/api")
    set(LIBLZMA_INCLUDE_DIRS "${LIBLZMA_INCLUDE_DIR}")
    set(LIBLZMA_LIBRARIES liblzma)
    set(WITHOUT_LZMA_API_STATIC FALSE)
    set(LZMA_API_STATIC TRUE)
    set(HAVE_LZMA_STREAM_ENCODER_MT FALSE)
    set(POSIX_REGEX_LIB NONE)
    set(ENABLE_ZLIB ON)
    set(ENABLE_LZMA ON)
    foreach(option IN ITEMS ENABLE_TEST ENABLE_INSTALL ENABLE_WERROR ENABLE_COVERAGE
            ENABLE_TAR ENABLE_CPIO ENABLE_CAT ENABLE_UNZIP ENABLE_OPENSSL ENABLE_MBEDTLS ENABLE_MD
            ENABLE_NETTLE ENABLE_LIBB2 ENABLE_LZ4 ENABLE_LZO ENABLE_ZSTD ENABLE_BZip2
            ENABLE_LIBXML2 ENABLE_BSDXML ENABLE_EXPAT ENABLE_WIN32_XMLLITE
            ENABLE_PCREPOSIX ENABLE_PCRE2POSIX ENABLE_LIBGCC ENABLE_ICONV ENABLE_ACL ENABLE_XATTR)
        set(${option} OFF)
    endforeach()
    add_subdirectory("${anomaly_libarchive_SOURCE_DIR}" "${anomaly_libarchive_BINARY_DIR}" EXCLUDE_FROM_ALL)
    add_library(anomaly_launcher_archive STATIC src/launcher/mod_archive.cpp)
    target_include_directories(anomaly_launcher_archive PUBLIC include)
    target_compile_definitions(anomaly_launcher_archive PRIVATE
        LIBARCHIVE_STATIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(anomaly_launcher_archive PRIVATE /utf-8 /W4 /permissive- /EHsc)
    target_link_libraries(anomaly_launcher_archive PRIVATE archive_static anomaly_plugin_package ole32)
endfunction()

anomaly_add_launcher_archive()
