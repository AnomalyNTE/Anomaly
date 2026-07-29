#include "anomaly/runtime_launch.hpp"

#include "anomaly/sdk/version.h"

#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace anomaly {
namespace {

constexpr wchar_t kCoreFile[] = L"Anomaly.Core.dll";

RuntimeLaunchResult Failure(RuntimeLaunchError error, std::string message) {
    return {error, std::move(message)};
}

bool IsReparsePoint(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsRegularFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
        !IsReparsePoint(path);
}

std::optional<std::filesystem::path> AbsoluteRoot(
    const std::filesystem::path& root) noexcept {
    try {
        if (root.empty()) return std::nullopt;
        const auto absolute = std::filesystem::absolute(root);
        std::error_code error;
        if (!std::filesystem::is_directory(absolute, error) || error ||
            IsReparsePoint(absolute)) {
            return std::nullopt;
        }
        return absolute;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

RuntimeLaunchResult ResolveRuntimeLaunch(const RuntimeLaunchOptions& options) noexcept {
    try {
        const auto root = AbsoluteRoot(options.installation_root);
        if (!root) {
            return Failure(RuntimeLaunchError::InvalidRoot, "Runtime installation root is invalid");
        }
        const auto core = *root / kCoreFile;
        if (!IsRegularFile(core)) {
            return Failure(
                RuntimeLaunchError::CoreUnavailable,
                "Runtime does not contain Anomaly.Core.dll");
        }
        RuntimeLaunchResult result;
        result.core_path = core;
        result.runtime_root = *root;
        result.version = ANOMALY_SDK_VERSION_STRING;
        result.message = "current Runtime selected";
        return result;
    } catch (const std::exception& error) {
        return Failure(RuntimeLaunchError::CoreUnavailable, error.what());
    } catch (...) {
        return Failure(
            RuntimeLaunchError::CoreUnavailable,
            "Runtime launch resolution failed unexpectedly");
    }
}

const char* RuntimeLaunchErrorName(RuntimeLaunchError error) noexcept {
    switch (error) {
    case RuntimeLaunchError::None: return "none";
    case RuntimeLaunchError::InvalidRoot: return "invalid-root";
    case RuntimeLaunchError::CoreUnavailable: return "core-unavailable";
    }
    return "unknown";
}

}  // namespace anomaly
