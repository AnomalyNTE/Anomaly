#include "anomaly/runtime_crash_coordinator.hpp"

#include <Windows.h>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

template <typename Integer>
bool ParseInteger(std::wstring_view value, Integer& result) {
    std::string narrow;
    narrow.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') return false;
        narrow.push_back(static_cast<char>(character));
    }
    const auto parsed = std::from_chars(
        narrow.data(), narrow.data() + narrow.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == narrow.data() + narrow.size();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 11 || std::wstring_view(argv[1]) != L"--runtime-root" ||
        std::wstring_view(argv[3]) != L"--process-id" ||
        std::wstring_view(argv[5]) != L"--creation-time" ||
        std::wstring_view(argv[7]) != L"--generation" ||
        std::wstring_view(argv[9]) != L"--incident") {
        return ERROR_INVALID_PARAMETER;
    }
    anomaly::RuntimeCrashMonitorOptions options;
    options.runtime_root = argv[2];
    const std::wstring_view incident(argv[10]);
    options.incident_id.reserve(incident.size());
    for (const wchar_t character : incident) {
        if (character > 0x7f) return ERROR_INVALID_DATA;
        options.incident_id.push_back(static_cast<char>(character));
    }
    if (!ParseInteger<DWORD>(argv[4], options.process_id) ||
        !ParseInteger<std::uint64_t>(argv[6], options.process_creation_time) ||
        !ParseInteger<std::uint64_t>(argv[8], options.session_generation)) {
        return ERROR_INVALID_DATA;
    }
    const auto result = anomaly::RunRuntimeCrashMonitor(options);
    if (!result.Ok()) {
        std::cerr << anomaly::RuntimeCrashCoordinatorErrorName(result.error)
                  << ": " << result.message << '\n';
        return result.win32_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE
                                                   : static_cast<int>(result.win32_error);
    }
    return 0;
}
