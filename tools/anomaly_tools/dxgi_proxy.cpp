#include <Windows.h>

#include <cstddef>
#include <cstdint>

#pragma comment(linker, "/export:ApplyCompatResolutionQuirking=proxy_stub_0,@1")
#pragma comment(linker, "/export:CompatString=proxy_stub_1,@2")
#pragma comment(linker, "/export:CompatValue=proxy_stub_2,@3")
#pragma comment(linker, "/export:CreateDXGIFactory=proxy_stub_3,@10")
#pragma comment(linker, "/export:CreateDXGIFactory1=proxy_stub_4,@11")
#pragma comment(linker, "/export:CreateDXGIFactory2=proxy_stub_5,@12")
#pragma comment(linker, "/export:DXGID3D10CreateDevice=proxy_stub_6,@13")
#pragma comment(linker, "/export:DXGID3D10CreateLayeredDevice=proxy_stub_7,@14")
#pragma comment(linker, "/export:DXGID3D10GetLayeredDeviceSize=proxy_stub_8,@15")
#pragma comment(linker, "/export:DXGID3D10RegisterLayers=proxy_stub_9,@16")
#pragma comment(linker, "/export:DXGIDeclareAdapterRemovalSupport=proxy_stub_10,@17")
#pragma comment(linker, "/export:DXGIDisableVBlankVirtualization=proxy_stub_11,@18")
#pragma comment(linker, "/export:DXGIDumpJournal=proxy_stub_12,@4")
#pragma comment(linker, "/export:DXGIGetDebugInterface1=proxy_stub_13,@19")
#pragma comment(linker, "/export:DXGIReportAdapterConfiguration=proxy_stub_14,@20")
#pragma comment(linker, "/export:PIXBeginCapture=proxy_stub_15,@5")
#pragma comment(linker, "/export:PIXEndCapture=proxy_stub_16,@6")
#pragma comment(linker, "/export:PIXGetCaptureState=proxy_stub_17,@7")
#pragma comment(linker, "/export:SetAppCompatStringPointer=proxy_stub_18,@8")
#pragma comment(linker, "/export:UpdateHMDEmulationStatus=proxy_stub_19,@9")

namespace {

constexpr wchar_t kSystemModuleName[] = L"dxgi.dll";
constexpr wchar_t kDumperModuleName[] = L"Dumper-7.dll";
constexpr wchar_t kLogFileName[] = L"anomaly-tools.log";
constexpr std::size_t kExportCount = 20;

HMODULE g_proxy_module{};
HMODULE g_system_module{};
HMODULE g_dumper_module{};

template <typename Element, std::size_t Size>
constexpr std::size_t CountOf(const Element (&)[Size]) noexcept {
    return Size;
}

bool AppendFileName(
    wchar_t* path,
    const std::size_t capacity,
    std::size_t length,
    const wchar_t* file_name) noexcept {
    if (path == nullptr || file_name == nullptr || length >= capacity) return false;
    if (length != 0 && path[length - 1] != L'\\' && path[length - 1] != L'/') {
        if (length + 1 >= capacity) return false;
        path[length++] = L'\\';
    }
    for (std::size_t index = 0;; ++index) {
        if (length + index >= capacity) return false;
        path[length + index] = file_name[index];
        if (file_name[index] == L'\0') return true;
    }
}

bool SiblingPath(const wchar_t* file_name, wchar_t* path, const std::size_t capacity) noexcept {
    if (file_name == nullptr || path == nullptr || capacity == 0) return false;
    const DWORD length = GetModuleFileNameW(
        g_proxy_module, path, static_cast<DWORD>(capacity));
    if (length == 0 || length >= capacity) return false;

    std::size_t directory_length = length;
    while (directory_length != 0 && path[directory_length - 1] != L'\\' &&
           path[directory_length - 1] != L'/') {
        --directory_length;
    }
    return directory_length != 0 &&
           AppendFileName(path, capacity, directory_length, file_name);
}

void AppendLog(const wchar_t* message) noexcept {
    wchar_t log_path[32768]{};
    if (!SiblingPath(kLogFileName, log_path, CountOf(log_path))) return;

    const HANDLE log = CreateFileW(
        log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return;

    char utf8[512]{};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, message, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (size > 1) {
        DWORD written{};
        static_cast<void>(WriteFile(
            log, utf8, static_cast<DWORD>(size - 1), &written, nullptr));
    }
    CloseHandle(log);
}

}  // namespace

extern "C" std::uintptr_t g_dxgi_procs[kExportCount]{};

namespace {

bool ResolveSystemExports() noexcept {
    wchar_t system_path[32768]{};
    const UINT length = GetSystemDirectoryW(
        system_path, static_cast<UINT>(CountOf(system_path)));
    if (length == 0 || length >= CountOf(system_path) ||
        !AppendFileName(system_path, CountOf(system_path), length, kSystemModuleName)) {
        return false;
    }

    g_system_module = LoadLibraryExW(system_path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_system_module == nullptr || g_system_module == g_proxy_module) return false;

    constexpr const char* kExports[] = {
        "ApplyCompatResolutionQuirking",
        "CompatString",
        "CompatValue",
        "CreateDXGIFactory",
        "CreateDXGIFactory1",
        "CreateDXGIFactory2",
        "DXGID3D10CreateDevice",
        "DXGID3D10CreateLayeredDevice",
        "DXGID3D10GetLayeredDeviceSize",
        "DXGID3D10RegisterLayers",
        "DXGIDeclareAdapterRemovalSupport",
        "DXGIDisableVBlankVirtualization",
        "DXGIDumpJournal",
        "DXGIGetDebugInterface1",
        "DXGIReportAdapterConfiguration",
        "PIXBeginCapture",
        "PIXEndCapture",
        "PIXGetCaptureState",
        "SetAppCompatStringPointer",
        "UpdateHMDEmulationStatus",
    };
    static_assert(CountOf(kExports) == kExportCount);

    for (std::size_t index = 0; index < CountOf(kExports); ++index) {
        g_dxgi_procs[index] = reinterpret_cast<std::uintptr_t>(
            GetProcAddress(g_system_module, kExports[index]));
    }
    for (const auto address : g_dxgi_procs) {
        if (address == 0) return false;
    }
    return true;
}

DWORD WINAPI LoadDumperThread(void*) noexcept {
    wchar_t dumper_path[32768]{};
    if (!SiblingPath(kDumperModuleName, dumper_path, CountOf(dumper_path))) {
        AppendLog(L"Anomaly Tools DXGI: could not resolve the proxy directory.\r\n");
        return ERROR_BAD_PATHNAME;
    }

    g_dumper_module = LoadLibraryExW(
        dumper_path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (g_dumper_module == nullptr) {
        const DWORD error = GetLastError();
        AppendLog(L"Anomaly Tools DXGI: Dumper-7.dll failed to load.\r\n");
        return error;
    }
    AppendLog(L"Anomaly Tools DXGI: Dumper-7.dll loaded.\r\n");
    return ERROR_SUCCESS;
}

}  // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    g_proxy_module = module;
    DisableThreadLibraryCalls(module);
    if (!ResolveSystemExports()) {
        OutputDebugStringW(
            L"Anomaly Tools DXGI: failed to resolve the System32 exports.\n");
        return FALSE;
    }

    const HANDLE thread = CreateThread(nullptr, 0, LoadDumperThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        OutputDebugStringW(
            L"Anomaly Tools DXGI: failed to create the Dumper loader thread.\n");
    } else {
        CloseHandle(thread);
    }
    return TRUE;
}
