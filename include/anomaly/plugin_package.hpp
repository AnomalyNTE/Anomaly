#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace anomaly {

enum class PluginPackageError : std::uint16_t {
    None = 0,
    EmptyPath,
    AbsolutePath,
    DevicePath,
    DriveRelativePath,
    ParentTraversal,
    EmptySegment,
    InvalidSegment,
    InvalidExtension,
    RootUnavailable,
    TargetUnavailable,
    ReparsePoint,
    NotDirectory,
    NotRegularFile,
    OutsidePackageRoot,
    IoFailure,
};

struct PluginPackagePathResult {
    PluginPackageError error{PluginPackageError::None};
    std::filesystem::path path;
    std::string message;

    [[nodiscard]] bool Ok() const noexcept { return error == PluginPackageError::None; }
};

// Applies Windows path semantics without touching the filesystem.
[[nodiscard]] PluginPackagePathResult ValidatePluginPackageRelativePath(
    std::string_view utf8_path, bool require_dll = false);

// Opens every component without following reparse points and verifies that the final
// target is an ordinary file below the package root. When opened_file is supplied,
// ownership of the verified final file handle transfers to the caller, which must
// CloseHandle it after use.
[[nodiscard]] PluginPackagePathResult OpenConfinedPluginPackageFile(
    const std::filesystem::path& package_root,
    std::string_view utf8_relative_path,
    bool require_dll = false,
    HANDLE* opened_file = nullptr);

}  // namespace anomaly
