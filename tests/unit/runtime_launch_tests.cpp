#include "anomaly/runtime_launch.hpp"
#include "anomaly/sdk/version.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

void Write(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

}  // namespace

int main() {
    bool result = true;
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-runtime-launch-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));

    auto launched = anomaly::ResolveRuntimeLaunch({root});
    result = Check(
        launched.error == anomaly::RuntimeLaunchError::InvalidRoot,
        "missing Runtime root was accepted") && result;

    std::filesystem::create_directories(root);
    launched = anomaly::ResolveRuntimeLaunch({root});
    result = Check(
        launched.error == anomaly::RuntimeLaunchError::CoreUnavailable,
        "Runtime root without Core was accepted") && result;

    Write(root / L"Anomaly.Core.dll", "current-core");
    launched = anomaly::ResolveRuntimeLaunch({root});
    result = Check(
        launched.Ok() && launched.runtime_root == std::filesystem::absolute(root) &&
            launched.core_path == std::filesystem::absolute(root) / L"Anomaly.Core.dll" &&
            launched.version == ANOMALY_SDK_VERSION_STRING,
        "current flat Runtime was not selected") && result;

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result ? 0 : 1;
}
