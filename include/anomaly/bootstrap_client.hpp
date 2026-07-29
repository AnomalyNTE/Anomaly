#pragma once

#include "anomaly/core_api.h"

#include <filesystem>

namespace anomaly {

struct BootstrapRuntimeResult {
    DWORD error{ERROR_SUCCESS};
    HMODULE module{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == ERROR_SUCCESS && module != nullptr;
    }
};

// Loads Core into the current process and invokes its versioned bootstrap
// entry. The caller owns the returned module reference after a successful start.
[[nodiscard]] BootstrapRuntimeResult StartRuntimeCore(
    const std::filesystem::path& core_path,
    const AnomalyStartInfo& start_info) noexcept;

}  // namespace anomaly
