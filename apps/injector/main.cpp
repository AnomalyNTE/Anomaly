// anomaly-inject -- attach the runtime core to a game that is already running.
//
// The launcher's live-attach mode does the same thing from inside its own process. This tool exists for
// the two cases the launcher cannot cover: a game that was started from its own shortcut, and scripted or
// verification runs, where the target is a process this repository owns.
//
// It manual-maps Anomaly.Core.dll into the target: the core is never loaded by the target's loader, so
// nothing appears in its module list and nothing in its imports changes.

#include "anomaly/launcher/manual_map.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options final {
    DWORD process_id{};
    std::filesystem::path core_path;
    std::filesystem::path runtime_root;
    std::filesystem::path log_directory;
    std::chrono::seconds timeout{30};
    anomaly::launcher::ManualMapDelivery delivery{anomaly::launcher::ManualMapDelivery::Automatic};
    bool list{};
    bool valid{true};
};

void Usage() {
    std::printf(
        "usage: anomaly-inject --pid <pid> [--core <path>] [--runtime-root <path>] [--logs <path>]\n"
        "                      [--timeout <seconds>] [--delivery remote-thread|hijack|auto]\n"
        "       anomaly-inject --list\n"
        "example: anomaly-inject --pid 1234 --delivery auto\n");
}

std::string Narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required,
        nullptr, nullptr);
    return result;
}

const char* DeliveryKey(anomaly::launcher::ManualMapDelivery delivery) {
    using anomaly::launcher::ManualMapDelivery;
    switch (delivery) {
        case ManualMapDelivery::RemoteThread: return "remote-thread";
        case ManualMapDelivery::ThreadHijack: return "hijack";
        case ManualMapDelivery::Automatic:
        default: return "auto";
    }
}

bool ParseDelivery(std::wstring_view value, anomaly::launcher::ManualMapDelivery& output) {
    using anomaly::launcher::ManualMapDelivery;
    if (value == L"remote-thread" || value == L"thread") {
        output = ManualMapDelivery::RemoteThread;
        return true;
    }
    if (value == L"hijack") {
        output = ManualMapDelivery::ThreadHijack;
        return true;
    }
    if (value == L"auto" || value == L"automatic") {
        output = ManualMapDelivery::Automatic;
        return true;
    }
    return false;
}

const char* ErrorName(anomaly::launcher::ManualMapError error) {
    using anomaly::launcher::ManualMapError;
    switch (error) {
        case ManualMapError::None: return "none";
        case ManualMapError::ProcessLaunchFailure: return "process-launch-failure";
        case ManualMapError::ProcessControlFailure: return "process-control-failure";
        case ManualMapError::ProcessUnavailable: return "process-unavailable";
        case ManualMapError::AccessDenied: return "access-denied";
        case ManualMapError::DifferentUser: return "different-user";
        case ManualMapError::IncompatibleArchitecture: return "incompatible-architecture";
        case ManualMapError::ImageUnavailable: return "image-unavailable";
        case ManualMapError::ImageInvalid: return "image-invalid";
        case ManualMapError::AlreadyAttached: return "already-attached";
        case ManualMapError::DependencyFailure: return "dependency-failure";
        case ManualMapError::AllocationFailure: return "allocation-failure";
        case ManualMapError::WriteFailure: return "write-failure";
        case ManualMapError::ProtectionFailure: return "protection-failure";
        case ManualMapError::BootstrapFailure: return "bootstrap-failure";
        case ManualMapError::Timeout: return "timeout";
        default: return "unknown";
    }
}

// The tool ships next to the runtime it injects, so the default core is the one beside it.
std::filesystem::path DefaultCorePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    const auto directory = std::filesystem::path(buffer).parent_path();
    const auto nested = directory / L"Anomaly" / L"Anomaly.Core.dll";
    std::error_code error;
    if (std::filesystem::is_regular_file(nested, error) && !error) return nested;
    return directory / L"Anomaly.Core.dll";
}

Options Parse(const std::vector<std::wstring>& arguments) {
    Options options;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        const auto next = [&arguments, &index]() -> std::wstring {
            return index + 1 < arguments.size() ? arguments[++index] : std::wstring{};
        };
        if (argument == L"--pid") {
            options.process_id = static_cast<DWORD>(std::wcstoul(next().c_str(), nullptr, 10));
        } else if (argument == L"--core") {
            options.core_path = next();
        } else if (argument == L"--runtime-root") {
            options.runtime_root = next();
        } else if (argument == L"--logs") {
            options.log_directory = next();
        } else if (argument == L"--timeout") {
            options.timeout = std::chrono::seconds(std::wcstol(next().c_str(), nullptr, 10));
        } else if (argument == L"--delivery") {
            if (!ParseDelivery(next(), options.delivery)) {
                std::printf("[x] unknown delivery method\n");
                options.valid = false;
            }
        } else if (argument == L"--list") {
            options.list = true;
        } else if (argument == L"--help" || argument == L"-h") {
            options.valid = false;
        } else {
            std::printf("[x] unknown argument: %s\n", Narrow(argument).c_str());
            options.valid = false;
        }
    }
    if (options.list) return options;
    if (options.process_id == 0) {
        std::printf("[x] --pid is required\n");
        options.valid = false;
    }
    if (options.core_path.empty()) options.core_path = DefaultCorePath();
    if (options.core_path.empty()) {
        std::printf("[x] the runtime core path could not be resolved; pass --core\n");
        options.valid = false;
    }
    return options;
}

void ListAttachable() {
    const auto processes = anomaly::launcher::EnumerateAttachableProcesses(L"HTGame.exe");
    if (processes.empty()) {
        std::printf("[i] no HTGame.exe process is running\n");
        return;
    }
    for (const auto& process : processes) {
        std::printf(
            "%6lu  %-16s  %s  %s\n", process.process_id,
            Narrow(process.executable_name).c_str(),
            process.Compatible() ? "attachable" : "not-attachable",
            Narrow(process.executable_path.wstring()).c_str());
        if (!process.Compatible()) {
            std::printf(
                "        owner=%s x64=%s error=%lu\n",
                process.owned_by_current_user ? "this-user" : "other-user",
                process.x64 ? "yes" : "no", process.inspection_error);
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    static_cast<void>(argc);
    std::vector<std::wstring> arguments(argv, argv + argc);
    const Options options = Parse(arguments);
    if (!options.valid) {
        Usage();
        return 1;
    }
    if (options.list) {
        ListAttachable();
        return 0;
    }

    anomaly::launcher::ManualMapOptions mapping;
    mapping.process_id = options.process_id;
    mapping.core_path = options.core_path;
    mapping.runtime_root = options.runtime_root;
    mapping.log_directory = options.log_directory;
    mapping.timeout = std::chrono::duration_cast<std::chrono::milliseconds>(options.timeout);
    mapping.delivery = options.delivery;

    std::printf(
        "injecting %s into pid %lu (delivery %s)\n", Narrow(mapping.core_path.wstring()).c_str(),
        options.process_id, DeliveryKey(options.delivery));
    const auto result = anomaly::launcher::AttachToRunningProcess(mapping);
    std::printf(
        "error=%s win32=%lu runtime=%lu remote_image=0x%llX\n", ErrorName(result.error),
        result.win32_error, result.runtime_start_error,
        static_cast<unsigned long long>(result.remote_image));
    std::printf("message=%s\n", result.message.c_str());
    return result.Ok() ? 0 : 2;
}
