#include "anomaly/plugin_package.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

int failures{};

void Expect(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

struct TempDirectory final {
    std::filesystem::path path;
    TempDirectory() {
        path = std::filesystem::temp_directory_path() /
            (L"anomaly-package-tests-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path / L"bin", error);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

}  // namespace

int main() {
    using anomaly::PluginPackageError;
    Expect(anomaly::ValidatePluginPackageRelativePath("plugin.dll", true).Ok(), "simple DLL");
    Expect(anomaly::ValidatePluginPackageRelativePath("bin/plugin.dll", true).Ok(), "nested DLL");
    Expect(anomaly::ValidatePluginPackageRelativePath("", true).error ==
        PluginPackageError::EmptyPath, "empty path");
    Expect(anomaly::ValidatePluginPackageRelativePath("C:\\plugin.dll", true).error ==
        PluginPackageError::DriveRelativePath, "absolute drive path");
    Expect(anomaly::ValidatePluginPackageRelativePath("C:plugin.dll", true).error ==
        PluginPackageError::DriveRelativePath, "drive-relative path");
    Expect(anomaly::ValidatePluginPackageRelativePath("\\\\server\\share\\plugin.dll", true).error ==
        PluginPackageError::AbsolutePath, "UNC path");
    Expect(anomaly::ValidatePluginPackageRelativePath("\\\\?\\C:\\plugin.dll", true).error ==
        PluginPackageError::DevicePath, "device path");
    Expect(anomaly::ValidatePluginPackageRelativePath("bin\\..\\plugin.dll", true).error ==
        PluginPackageError::ParentTraversal, "parent traversal");
    Expect(anomaly::ValidatePluginPackageRelativePath("bin//plugin.dll", true).error ==
        PluginPackageError::EmptySegment, "empty segment");
    Expect(anomaly::ValidatePluginPackageRelativePath("plugin.exe", true).error ==
        PluginPackageError::InvalidExtension, "DLL extension");
    Expect(anomaly::ValidatePluginPackageRelativePath("plugin.dll:stream", true).error ==
        PluginPackageError::InvalidSegment, "alternate data stream");

    TempDirectory temp;
    std::ofstream(temp.path / L"bin" / L"plugin.dll", std::ios::binary) << "fixture";
    const auto confined = anomaly::OpenConfinedPluginPackageFile(temp.path, "bin/plugin.dll", true);
    Expect(confined.Ok(), "confined ordinary file");

    std::error_code error;
    HANDLE held_file = INVALID_HANDLE_VALUE;
    const auto held = anomaly::OpenConfinedPluginPackageFile(
        temp.path, "bin/plugin.dll", true, &held_file);
    Expect(held.Ok() && held_file != INVALID_HANDLE_VALUE, "confined file handle");
    std::filesystem::rename(
        temp.path / L"bin" / L"plugin.dll", temp.path / L"bin" / L"plugin-original.dll", error);
    Expect(!error, "held package file rename");
    error.clear();
    std::ofstream(temp.path / L"bin" / L"plugin.dll", std::ios::binary) << "replacement";
    char held_bytes[8]{};
    DWORD held_read{};
    const BOOL held_read_succeeded = held_file != INVALID_HANDLE_VALUE &&
        ReadFile(held_file, held_bytes, 7, &held_read, nullptr) != FALSE;
    Expect(held_read_succeeded && held_read == 7 && std::string_view(held_bytes, 7) == "fixture",
        "held package file was replaced through its pathname");
    if (held_file != INVALID_HANDLE_VALUE) CloseHandle(held_file);

    Expect(anomaly::OpenConfinedPluginPackageFile(temp.path, "missing.dll", true).error ==
        PluginPackageError::TargetUnavailable, "missing target");
    Expect(anomaly::OpenConfinedPluginPackageFile(temp.path, "bin", false).error ==
        PluginPackageError::NotRegularFile, "directory target");

    const std::filesystem::path sibling = temp.path.parent_path() / (temp.path.filename().wstring() + L"-sibling");
    std::filesystem::create_directories(sibling, error);
    std::ofstream(sibling / L"plugin.dll", std::ios::binary) << "fixture";
    const std::filesystem::path link = temp.path / L"linked";
    const BOOLEAN linked = CreateSymbolicLinkW(
        link.c_str(), sibling.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (linked != FALSE) {
        Expect(anomaly::OpenConfinedPluginPackageFile(temp.path, "linked/plugin.dll", true).error ==
            PluginPackageError::ReparsePoint, "reparse point rejected");
    }
    std::filesystem::remove_all(sibling, error);

    if (failures != 0) return 1;
    std::cout << "plugin package confinement passed\n";
    return 0;
}
