#include "anomaly/bootstrap_client.hpp"

namespace anomaly {

BootstrapRuntimeResult StartRuntimeCore(
    const std::filesystem::path& core_path,
    const AnomalyStartInfo& start_info) noexcept {
    if (core_path.empty()) return {ERROR_INVALID_PARAMETER, nullptr};

    const HMODULE module = LoadLibraryExW(
        core_path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) return {GetLastError(), nullptr};

    const auto start = reinterpret_cast<AnomalyStartFn>(
        GetProcAddress(module, ANOMALY_CORE_START_ENTRY));
    if (start == nullptr) {
        FreeLibrary(module);
        return {ERROR_PROC_NOT_FOUND, nullptr};
    }

    const DWORD result = start(&start_info);
    if (result != ERROR_SUCCESS) {
        FreeLibrary(module);
        return {result, nullptr};
    }
    return {ERROR_SUCCESS, module};
}

}  // namespace anomaly
