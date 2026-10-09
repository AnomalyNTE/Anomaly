#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

enum class PluginNativeDependencyDiagnosticCode {
    InvalidPe,
    InvalidImportName,
    UnsupportedDelayImport,
    PrivateCrtModule,
    PrivateSystemModule,
    CrtRuntimeUnavailable,
    MissingPrivateImport,
    ModuleNameConflict,
    // The runtime maps plugin images without the Windows loader, so images
    // must not depend on loader services the mapper cannot provide.
    StaticTls,
    WritableExecutableSection,
    MissingCxxThrowBridge,
    StaticCrt,
    InternalFailure,
};

struct PluginNativeDependencyDiagnostic {
    PluginNativeDependencyDiagnosticCode code{
        PluginNativeDependencyDiagnosticCode::InternalFailure};
    std::filesystem::path requester;
    std::wstring module_name;
    std::filesystem::path expected_path;
    std::filesystem::path loaded_path;
    std::string message;
};

struct PluginNativeDependencyPreflightResult {
    std::vector<PluginNativeDependencyDiagnostic> diagnostics;

    [[nodiscard]] bool Ok() const noexcept { return diagnostics.empty(); }
};

[[nodiscard]] std::string_view PluginNativeDependencyDiagnosticCodeName(
    PluginNativeDependencyDiagnosticCode code) noexcept;

// Inspects the entry image plus package-private static and delay-load imports
// without mapping them. Private DLLs must live alongside the requesting DLL.
[[nodiscard]] PluginNativeDependencyPreflightResult PreflightPluginNativeDependencies(
    const std::filesystem::path& entry_file) noexcept;

}  // namespace anomaly
