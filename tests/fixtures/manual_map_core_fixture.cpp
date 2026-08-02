#include "anomaly/core_api.h"
#include "anomaly/thread_local_value.hpp"

#include <Windows.h>
#include <dwmapi.h>

#include <cstddef>
#include <cwchar>
#include <intrin.h>
#include <iterator>
#include <stdexcept>
#include <string_view>

#pragma comment(lib, "dwmapi.lib")

namespace {

volatile LONG g_dll_attached{};
anomaly::ThreadLocalScalar<LONG> g_thread_value;

bool CxxExceptionUnwindWorks() {
    try {
        throw std::runtime_error("manual-map-cxx-eh");
    } catch (const std::exception& error) {
        return std::string_view(error.what()) == "manual-map-cxx-eh";
    }
}

DWORD WINAPI VerifyThreadLocalValue(void*) {
    g_thread_value.Set(0x13572468);
    return g_thread_value.Get() == 0x13572468 && CxxExceptionUnwindWorks()
        ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

bool SystemDwmapiOwnsImport() {
    BOOL composition{};
    return DwmIsCompositionEnabled(&composition) !=
        static_cast<HRESULT>(0x81234567L);
}

}  // namespace

extern "C" IMAGE_DOS_HEADER __ImageBase;

extern "C" __declspec(dllexport) __declspec(noreturn) void WINAPI
AnomalyManualMapCxxThrow(void* exception_object, const void* throw_info) {
    constexpr DWORD kMsvcCxxException = 0xE06D7363UL;
    constexpr ULONG_PTR kMsvcEhMagicNumber1 = 0x19930520UL;
    const ULONG_PTR parameters[]{
        kMsvcEhMagicNumber1,
        reinterpret_cast<ULONG_PTR>(exception_object),
        reinterpret_cast<ULONG_PTR>(throw_info),
        reinterpret_cast<ULONG_PTR>(&__ImageBase),
    };
    RaiseException(
        kMsvcCxxException, EXCEPTION_NONCONTINUABLE,
        static_cast<DWORD>(std::size(parameters)), parameters);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

extern "C" __declspec(dllexport) DWORD WINAPI AnomalyStart(
    const AnomalyStartInfo* start_info) {
    if (start_info == nullptr || start_info->struct_size < ANOMALY_START_INFO_V1_SIZE ||
        start_info->bootstrap_abi_version != ANOMALY_BOOTSTRAP_ABI_VERSION ||
        start_info->bootstrap_type != ANOMALY_BOOTSTRAP_TYPE_EXTERNAL ||
        start_info->runtime_root == nullptr ||
        InterlockedCompareExchange(&g_dll_attached, 1, 1) != 1 ||
        !SystemDwmapiOwnsImport()) {
        return ERROR_INVALID_STATE;
    }
    const HANDLE thread = CreateThread(
        nullptr, 0, VerifyThreadLocalValue, nullptr, 0, nullptr);
    if (thread == nullptr) return GetLastError();
    const DWORD wait = WaitForSingleObject(thread, 5000);
    DWORD thread_result{ERROR_GEN_FAILURE};
    const BOOL exit_read = GetExitCodeThread(thread, &thread_result);
    CloseHandle(thread);
    if (wait != WAIT_OBJECT_0 || exit_read == FALSE || thread_result != ERROR_SUCCESS) {
        return wait == WAIT_FAILED ? GetLastError()
            : thread_result == ERROR_SUCCESS ? ERROR_TIMEOUT : thread_result;
    }
    const std::size_t root_length = std::wcslen(start_info->runtime_root);
    constexpr wchar_t suffix[] = L"\\manual-map-marker.txt";
    if (root_length + std::size(suffix) >= 32768) return ERROR_FILENAME_EXCED_RANGE;
    wchar_t marker[32768]{};
    std::wmemcpy(marker, start_info->runtime_root, root_length);
    std::wmemcpy(marker + root_length, suffix, std::size(suffix));
    static_cast<void>(CreateDirectoryW(start_info->runtime_root, nullptr));
    const HANDLE file = CreateFileW(
        marker, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    constexpr char content[] = "manual-map-ok";
    DWORD written{};
    const BOOL result = WriteFile(
        file, content, static_cast<DWORD>(sizeof(content) - 1), &written, nullptr);
    const DWORD error = result != FALSE && written == sizeof(content) - 1
        ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return error;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        InterlockedExchange(&g_dll_attached, 1);
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
