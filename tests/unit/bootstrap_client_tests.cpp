#include "anomaly/bootstrap_client.hpp"

#include <Windows.h>

#include <filesystem>
#include <iostream>

namespace {

using FixtureTypeFn = AnomalyBootstrapType(WINAPI*)();

bool CheckBootstrapType(
    const std::filesystem::path& fixture_path,
    AnomalyBootstrapType bootstrap_type) {
    const std::filesystem::path root = fixture_path.parent_path();
    const std::filesystem::path logs = root / L"logs";
    const AnomalyStartInfo start_info{
        .struct_size = ANOMALY_START_INFO_V1_SIZE,
        .bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION,
        .bootstrap_type = bootstrap_type,
        .flags = 0,
        .bootstrap_module = GetModuleHandleW(nullptr),
        .game_module = GetModuleHandleW(nullptr),
        .runtime_root = root.c_str(),
        .log_directory = logs.c_str(),
        .external_stop_event = nullptr,
    };

    const anomaly::BootstrapRuntimeResult result =
        anomaly::StartRuntimeCore(fixture_path, start_info);
    if (!result) {
        std::cerr << "bootstrap fixture start failed: " << result.error << '\n';
        return false;
    }
    const auto fixture_type = reinterpret_cast<FixtureTypeFn>(
        GetProcAddress(result.module, "AnomalyBootstrapFixtureType"));
    const bool matched = fixture_type != nullptr && fixture_type() == bootstrap_type;
    FreeLibrary(result.module);
    if (!matched) std::cerr << "bootstrap type did not cross the shared contract\n";
    return matched;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path fixture_path =
        std::filesystem::absolute(std::filesystem::path(argv[1]));
    if (!CheckBootstrapType(
            fixture_path, ANOMALY_BOOTSTRAP_TYPE_DWMAPI_PROXY)) {
        return 3;
    }
    if (!CheckBootstrapType(fixture_path, ANOMALY_BOOTSTRAP_TYPE_EXTERNAL)) {
        return 4;
    }

    const std::filesystem::path root = fixture_path.parent_path();
    const std::filesystem::path logs = root / L"logs";
    AnomalyStartInfo mismatched{
        .struct_size = ANOMALY_START_INFO_V1_SIZE,
        .bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION + 1,
        .bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL,
        .runtime_root = root.c_str(),
        .log_directory = logs.c_str(),
    };
    const auto rejected = anomaly::StartRuntimeCore(fixture_path, mismatched);
    if (rejected.error != ERROR_REVISION_MISMATCH || rejected.module != nullptr) {
        std::cerr << "bootstrap ABI mismatch did not release the fixture module\n";
        return 5;
    }

    const AnomalyStartInfo unused{};
    const auto empty_path = anomaly::StartRuntimeCore({}, unused);
    if (empty_path.error != ERROR_INVALID_PARAMETER || empty_path.module != nullptr) {
        std::cerr << "empty Core path was not rejected\n";
        return 6;
    }
    return 0;
}
