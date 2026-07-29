#include "anomaly/bootstrap_client.hpp"
#include "anomaly/runtime_launch.hpp"
#include "proxy_exports.hpp"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace {

HMODULE g_original_dwmapi{};
HMODULE g_proxy_module{};
HMODULE g_anomaly_core{};

std::filesystem::path ModuleDirectory(HMODULE module) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool LoadOriginalDwmapi() {
    std::wstring system_directory(32768, L'\0');
    const UINT length = GetSystemDirectoryW(
        system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) return false;
    system_directory.resize(length);
    g_original_dwmapi = LoadLibraryW(
        (std::filesystem::path(system_directory) / L"dwmapi.dll").c_str());
    if (g_original_dwmapi == nullptr) return false;
    static_cast<void>(ResolveProxyExports(g_original_dwmapi));
    return true;
}

DWORD WINAPI BootstrapThread(void*) {
    const auto runtime_root = ModuleDirectory(g_proxy_module) / L"Anomaly";
    const auto log_directory = runtime_root / L"logs";
    const auto selected = anomaly::ResolveRuntimeLaunch({
        .installation_root = runtime_root,
    });
    if (!selected.Ok()) {
        OutputDebugStringW(
            (L"Anomaly: Runtime launch failed: " +
             std::wstring(selected.message.begin(), selected.message.end()) + L"\n")
                .c_str());
        return ERROR_INVALID_STATE;
    }
    const auto& core_path = selected.core_path;
    const AnomalyStartInfo start_info{
        .struct_size = ANOMALY_START_INFO_V1_SIZE,
        .bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION,
        .bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_DWMAPI_PROXY,
        .flags = 0,
        .bootstrap_module = g_proxy_module,
        .game_module = GetModuleHandleW(nullptr),
        .runtime_root = runtime_root.c_str(),
        .log_directory = log_directory.c_str(),
        .external_stop_event = nullptr,
    };
    const anomaly::BootstrapRuntimeResult result =
        anomaly::StartRuntimeCore(core_path, start_info);
    if (!result) {
        OutputDebugStringW(
            (L"Anomaly: core start failed: " + core_path.wstring() + L" error=" +
             std::to_wstring(result.error) + L"\n")
                .c_str());
        return result.error;
    }
    g_anomaly_core = result.module;
    return ERROR_SUCCESS;
}

}  // namespace

extern "C" std::uintptr_t WINAPI ProxyMissingExport() {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return static_cast<std::uintptr_t>(E_NOTIMPL);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_proxy_module = module;
        DisableThreadLibraryCalls(module);
        if (!LoadOriginalDwmapi()) return FALSE;
        const HANDLE thread = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr && g_original_dwmapi != nullptr) {
        FreeLibrary(g_original_dwmapi);
        g_original_dwmapi = nullptr;
    }
    return TRUE;
}
