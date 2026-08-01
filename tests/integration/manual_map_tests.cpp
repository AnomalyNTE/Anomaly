#include "anomaly/launcher/manual_map.hpp"

#include <Windows.h>
#include <Aclapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::wstring Quote(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

bool StartTarget(
    const std::filesystem::path& target, std::wstring_view ready_name,
    std::wstring_view stop_name, DWORD flags, PROCESS_INFORMATION& process,
    SECURITY_ATTRIBUTES* process_security = nullptr) {
    std::wstring command = Quote(target) + L" \"" + std::wstring(ready_name) +
        L"\" \"" + std::wstring(stop_name) + L"\"";
    STARTUPINFOW startup{.cb = sizeof(startup)};
    return CreateProcessW(
            nullptr, command.data(), process_security, nullptr, FALSE,
            flags, nullptr, nullptr, &startup, &process) != FALSE;
}

int WaitForTarget(PROCESS_INFORMATION& process) {
    if (process.hThread != nullptr) {
        CloseHandle(process.hThread);
        process.hThread = nullptr;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code{24};
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    process.hProcess = nullptr;
    return wait == WAIT_OBJECT_0 ? static_cast<int>(exit_code) : 24;
}

void StopTarget(PROCESS_INFORMATION& process) {
    if (process.hProcess == nullptr) return;
    static_cast<void>(TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED));
    static_cast<void>(WaitForSingleObject(process.hProcess, 5000));
    if (process.hThread != nullptr) CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    process.hThread = nullptr;
    process.hProcess = nullptr;
}

int RunSuspendedLauncher(int argc, wchar_t** argv) {
    if (argc != 6 && argc != 7) return 20;
    wchar_t* delay_end{};
    const unsigned long delay = std::wcstoul(argv[5], &delay_end, 10);
    if (delay_end == argv[5] || *delay_end != L'\0') return 21;

    std::wstring command = Quote(argv[2]) + L" \"" + argv[3] + L"\" \"" +
        argv[4] + L"\"";
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &process) == FALSE) {
        return 22;
    }
    Sleep(static_cast<DWORD>(delay));
    if (argc == 7) {
        if (std::wstring_view(argv[6]) != L"--restrict-access") {
            TerminateProcess(process.hProcess, 25);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return 25;
        }
        ACL empty_acl{};
        if (InitializeAcl(&empty_acl, sizeof(empty_acl), ACL_REVISION) == FALSE ||
            SetSecurityInfo(
                process.hProcess, SE_KERNEL_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, &empty_acl, nullptr) != ERROR_SUCCESS) {
            TerminateProcess(process.hProcess, 26);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return 26;
        }
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.hProcess, 23);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 23;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code{24};
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 ? static_cast<int>(exit_code) : 24;
}

int RunAccessDelayedLauncher(int argc, wchar_t** argv) {
    if (argc != 6) return 50;
    wchar_t* delay_end{};
    const unsigned long delay = std::wcstoul(argv[5], &delay_end, 10);
    if (delay_end == argv[5] || *delay_end != L'\0') return 51;

    ACL empty_acl{};
    SECURITY_DESCRIPTOR descriptor{};
    if (InitializeAcl(&empty_acl, sizeof(empty_acl), ACL_REVISION) == FALSE ||
        InitializeSecurityDescriptor(
            &descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
        SetSecurityDescriptorDacl(
            &descriptor, TRUE, &empty_acl, FALSE) == FALSE) {
        return 52;
    }
    SECURITY_ATTRIBUTES security{
        .nLength = sizeof(security),
        .lpSecurityDescriptor = &descriptor,
        .bInheritHandle = FALSE,
    };
    PROCESS_INFORMATION target{};
    if (!StartTarget(
            argv[2], argv[3], argv[4], CREATE_SUSPENDED | CREATE_NO_WINDOW,
            target, &security)) {
        return 53;
    }

    Sleep(static_cast<DWORD>(delay));
    const DWORD restore_error = SetSecurityInfo(
        target.hProcess, SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr);
    if (restore_error != ERROR_SUCCESS ||
        ResumeThread(target.hThread) == static_cast<DWORD>(-1)) {
        StopTarget(target);
        return 54;
    }
    return WaitForTarget(target);
}

int RunCandidateLauncher(int argc, wchar_t** argv) {
    if (argc != 6) return 30;
    wchar_t* delay_end{};
    const unsigned long delay = std::wcstoul(argv[5], &delay_end, 10);
    if (delay_end == argv[5] || *delay_end != L'\0') return 31;

    PROCESS_INFORMATION stalled{};
    if (!StartTarget(
            argv[2], argv[3], argv[4], CREATE_SUSPENDED | CREATE_NO_WINDOW,
            stalled)) {
        return 32;
    }
    Sleep(static_cast<DWORD>(delay));

    PROCESS_INFORMATION ready{};
    if (!StartTarget(argv[2], argv[3], argv[4], CREATE_NO_WINDOW, ready)) {
        StopTarget(stalled);
        return 33;
    }
    const int result = WaitForTarget(ready);
    StopTarget(stalled);
    return result;
}

int RunStalledOnceLauncher(int argc, wchar_t** argv) {
    if (argc != 6) return 40;

    HANDLE marker = CreateFileW(
        argv[5], GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool first_attempt = marker != INVALID_HANDLE_VALUE;
    const DWORD marker_error = first_attempt ? ERROR_SUCCESS : GetLastError();
    if (!first_attempt && marker_error != ERROR_FILE_EXISTS &&
        marker_error != ERROR_ALREADY_EXISTS) {
        return 41;
    }

    PROCESS_INFORMATION target{};
    const DWORD flags = first_attempt
        ? CREATE_SUSPENDED | CREATE_NO_WINDOW : CREATE_NO_WINDOW;
    if (!StartTarget(argv[2], argv[3], argv[4], flags, target)) {
        if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
        return 42;
    }
    if (first_attempt) {
        const std::string pid = std::to_string(target.dwProcessId);
        DWORD written{};
        static_cast<void>(WriteFile(
            marker, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr));
        CloseHandle(marker);
        return WaitForTarget(target);
    }
    return WaitForTarget(target);
}

std::filesystem::path CurrentExecutable() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) return {};
    path.resize(size);
    return path;
}

std::optional<bool> RemoteModuleVisible(
    DWORD process_id, std::wstring_view module_name) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    MODULEENTRY32W entry{.dwSize = sizeof(entry)};
    if (Module32FirstW(snapshot, &entry) == FALSE) {
        CloseHandle(snapshot);
        return std::nullopt;
    }
    const std::wstring requested(module_name);
    bool found{};
    do {
        if (_wcsicmp(entry.szModule, requested.c_str()) == 0) {
            found = true;
            break;
        }
        entry.dwSize = sizeof(entry);
    } while (Module32NextW(snapshot, &entry) != FALSE);
    CloseHandle(snapshot);
    return found;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--launch-suspended") {
        return RunSuspendedLauncher(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--launch-access-delayed") {
        return RunAccessDelayedLauncher(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--launch-candidates") {
        return RunCandidateLauncher(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--launch-stalled-once") {
        return RunStalledOnceLauncher(argc, argv);
    }
    if (argc != 5) return 2;
    bool result = true;
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const std::wstring ready_name = L"Local\\AnomalyManualMapReady-" + suffix;
    const std::wstring stop_name = L"Local\\AnomalyManualMapStop-" + suffix;
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    const HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
    if (ready == nullptr || stop == nullptr) return 3;

    std::wstring command = Quote(argv[2]) + L" \"" + ready_name + L"\" \"" + stop_name + L"\"";
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) == FALSE) {
        CloseHandle(stop);
        CloseHandle(ready);
        return 4;
    }
    CloseHandle(process.hThread);
    if (WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0) {
        SetEvent(stop);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        CloseHandle(stop);
        CloseHandle(ready);
        return 5;
    }

    const auto inspected = anomaly::launcher::InspectAttachableProcess(process.dwProcessId);
    result = Check(
        inspected.Compatible() && inspected.process_id == process.dwProcessId,
        "self-owned x64 fixture was not attachable") && result;

    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-manual-map-" + suffix);
    std::filesystem::create_directories(root);
    const auto image_root = root / L"image";
    std::filesystem::create_directories(image_root);
    const auto isolated_core = image_root / L"manual-map-core-fixture.dll";
    std::error_code copy_error;
    std::filesystem::copy_file(
        argv[1], isolated_core, std::filesystem::copy_options::overwrite_existing, copy_error);
    result = Check(!copy_error, "manual-map fixture core could not be isolated") && result;
    const HMODULE collision = LoadLibraryExW(
        argv[3], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    result = Check(
        collision != nullptr,
        "private dwmapi collision fixture could not be loaded") && result;
    if (!result) {
        if (collision != nullptr) FreeLibrary(collision);
        SetEvent(stop);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        CloseHandle(stop);
        CloseHandle(ready);
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        return 1;
    }
    anomaly::launcher::ManualMapOptions options;
    options.process_id = process.dwProcessId;
    options.core_path = isolated_core;
    options.runtime_root = root;
    options.log_directory = root / L"logs";
    options.timeout = std::chrono::seconds(15);
    const auto invalid_core = root / L"invalid-core.dll";
    {
        std::ofstream output(invalid_core, std::ios::binary | std::ios::trunc);
        output << "not-a-pe";
    }
    auto invalid_options = options;
    invalid_options.core_path = invalid_core;
    const auto invalid = anomaly::launcher::ManualMapRuntimeCore(invalid_options);
    result = Check(
        invalid.error == anomaly::launcher::ManualMapError::ImageInvalid,
        "invalid core image was accepted") && result;

    auto static_tls_options = options;
    static_tls_options.core_path = argv[4];
    const auto static_tls =
        anomaly::launcher::ManualMapRuntimeCore(static_tls_options);
    result = Check(
        static_tls.error == anomaly::launcher::ManualMapError::ImageInvalid &&
            static_tls.win32_error == ERROR_NOT_SUPPORTED,
        "loader-managed static TLS image was accepted for manual mapping") && result;

    const auto attached = anomaly::launcher::ManualMapRuntimeCore(options);
    result = Check(attached.Ok() && attached.remote_image != 0,
        attached.message.empty() ? "manual-map attach failed" : attached.message.c_str()) && result;
    const auto core_visible =
        RemoteModuleVisible(process.dwProcessId, isolated_core.filename().wstring());
    result = Check(core_visible.has_value(),
                 "target loader module list could not be inspected") && result;
    result = Check(!core_visible.value_or(true),
                 "manual-map core was published in the Windows loader module list") && result;
    result = Check(
        std::filesystem::is_regular_file(root / L"manual-map-marker.txt"),
        "manual-map fixture did not observe FLS, DllMain and AnomalyStart") && result;

    const auto duplicate = anomaly::launcher::ManualMapRuntimeCore(options);
    result = Check(
        duplicate.error == anomaly::launcher::ManualMapError::AlreadyAttached,
        "duplicate manual-map core was not rejected") && result;

    SetEvent(stop);
    result = Check(
        WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0,
        "manual-map fixture process did not stop") && result;
    DWORD exit_code{};
    GetExitCodeProcess(process.hProcess, &exit_code);
    result = Check(exit_code == 0, "manual-map fixture process failed") && result;
    CloseHandle(process.hProcess);

    ResetEvent(ready);
    ResetEvent(stop);
    anomaly::launcher::ManualMapLaunchOptions launch_options;
    std::wstring system_root(32768, L'\0');
    const DWORD system_root_size = GetEnvironmentVariableW(
        L"SystemRoot", system_root.data(), static_cast<DWORD>(system_root.size()));
    if (system_root_size == 0 || system_root_size >= system_root.size()) return 6;
    system_root.resize(system_root_size);
    const auto target_executable = std::filesystem::absolute(argv[2]);
    launch_options.launcher_path =
        std::filesystem::path(system_root) / L"System32" / L"cmd.exe";
    launch_options.launcher_arguments = L"/d /s /c \"\"" +
        target_executable.wstring() + L"\" \"" + ready_name + L"\" \"" +
        stop_name + L"\"\"";
    launch_options.working_directory = std::filesystem::path(argv[2]).parent_path();
    launch_options.target_executable_name = target_executable.filename().wstring();
    launch_options.creation_flags = CREATE_NO_WINDOW;
    launch_options.target_timeout = std::chrono::seconds(15);
    launch_options.manual_map = options;

    auto invalid_launch_options = launch_options;
    invalid_launch_options.manual_map.core_path = invalid_core;
    const auto invalid_launch =
        anomaly::launcher::LaunchAndManualMapRuntimeCore(invalid_launch_options);
    result = Check(
        invalid_launch.mapping.error == anomaly::launcher::ManualMapError::ImageInvalid &&
            invalid_launch.process_id != 0,
        "invalid core did not fail discovered target mapping") && result;
    HANDLE failed_process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, invalid_launch.process_id);
    if (failed_process != nullptr) {
        result = Check(
            WaitForSingleObject(failed_process, 0) == WAIT_OBJECT_0,
            "failed discovered target mapping left the target running") && result;
        CloseHandle(failed_process);
    } else {
        result = Check(
            GetLastError() == ERROR_INVALID_PARAMETER,
            "failed discovered target cleanup could not be verified") && result;
    }

    ResetEvent(ready);
    ResetEvent(stop);
    auto access_delayed_options = launch_options;
    access_delayed_options.launcher_path = CurrentExecutable();
    access_delayed_options.launcher_arguments = L"--launch-access-delayed " +
        Quote(target_executable) + L" \"" + ready_name + L"\" \"" +
        stop_name + L"\" 250";
    access_delayed_options.target_timeout = std::chrono::seconds(2);
    access_delayed_options.loader_timeout = std::chrono::seconds(5);
    const auto access_delayed = anomaly::launcher::LaunchAndManualMapRuntimeCore(
        access_delayed_options);
    result = Check(
        access_delayed.Ok() && access_delayed.mapping.remote_image != 0,
        access_delayed.mapping.message.empty()
            ? "temporarily inaccessible target was not recaptured"
            : access_delayed.mapping.message.c_str()) && result;
    result = Check(
        access_delayed.mapping.message.find("after one loader-ready retry") ==
            std::string::npos,
        "temporarily inaccessible target required a launcher retry") && result;
    HANDLE access_delayed_process = access_delayed.process_id != 0
        ? OpenProcess(SYNCHRONIZE, FALSE, access_delayed.process_id) : nullptr;
    result = Check(
        access_delayed_process != nullptr,
        "recaptured target process could not be opened") && result;
    result = Check(
        WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0,
        "recaptured target did not run") && result;
    SetEvent(stop);
    if (access_delayed_process != nullptr) {
        result = Check(
            WaitForSingleObject(access_delayed_process, 5000) == WAIT_OBJECT_0,
            "recaptured target did not stop") && result;
        CloseHandle(access_delayed_process);
    }

    ResetEvent(ready);
    ResetEvent(stop);
    auto candidate_launch_options = launch_options;
    candidate_launch_options.launcher_path = CurrentExecutable();
    candidate_launch_options.launcher_arguments = L"--launch-candidates " +
        Quote(target_executable) + L" \"" + ready_name + L"\" \"" +
        stop_name + L"\" 100";
    candidate_launch_options.loader_timeout = std::chrono::seconds(5);
    const auto candidate_launch = anomaly::launcher::LaunchAndManualMapRuntimeCore(
        candidate_launch_options);
    result = Check(
        candidate_launch.Ok() && candidate_launch.mapping.remote_image != 0,
        candidate_launch.mapping.message.empty()
            ? "candidate-pool launch failed" : candidate_launch.mapping.message.c_str()) && result;
    result = Check(
        candidate_launch.mapping.message.find("after one loader-ready retry") == std::string::npos,
        "candidate-pool launch required an unnecessary retry") && result;
    HANDLE candidate_process = candidate_launch.process_id != 0
        ? OpenProcess(SYNCHRONIZE, FALSE, candidate_launch.process_id) : nullptr;
    result = Check(
        candidate_process != nullptr,
        "candidate-pool selected process could not be opened") && result;
    result = Check(
        WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0,
        "loader-ready candidate did not run") && result;
    SetEvent(stop);
    if (candidate_process != nullptr) {
        result = Check(
            WaitForSingleObject(candidate_process, 5000) == WAIT_OBJECT_0,
            "candidate-pool selected process did not stop") && result;
        CloseHandle(candidate_process);
    }

    ResetEvent(ready);
    ResetEvent(stop);
    const auto retry_marker = root / L"loader-retry.marker";
    std::filesystem::remove(retry_marker);
    auto retry_launch_options = launch_options;
    retry_launch_options.launcher_path = CurrentExecutable();
    retry_launch_options.launcher_arguments = L"--launch-stalled-once " +
        Quote(target_executable) + L" \"" + ready_name + L"\" \"" +
        stop_name + L"\" \"" + retry_marker.wstring() + L"\"";
    retry_launch_options.loader_timeout = std::chrono::seconds(1);
    const auto retried_launch = anomaly::launcher::LaunchAndManualMapRuntimeCore(
        retry_launch_options);
    result = Check(
        retried_launch.Ok() && retried_launch.mapping.remote_image != 0,
        retried_launch.mapping.message.empty()
            ? "loader-ready retry launch failed" : retried_launch.mapping.message.c_str()) && result;
    result = Check(
        retried_launch.mapping.message.find("after one loader-ready retry") != std::string::npos,
        "loader-stalled first launch did not report one retry") && result;
    DWORD stalled_process_id{};
    {
        std::ifstream marker(retry_marker);
        marker >> stalled_process_id;
    }
    result = Check(
        stalled_process_id != 0 && stalled_process_id != retried_launch.process_id,
        "loader retry marker did not identify the first target") && result;
    HANDLE stalled_process = stalled_process_id != 0
        ? OpenProcess(SYNCHRONIZE, FALSE, stalled_process_id) : nullptr;
    if (stalled_process != nullptr) {
        result = Check(
            WaitForSingleObject(stalled_process, 0) == WAIT_OBJECT_0,
            "loader-stalled first target was left running") && result;
        CloseHandle(stalled_process);
    } else {
        result = Check(
            GetLastError() == ERROR_INVALID_PARAMETER,
            "loader-stalled first target could not be inspected") && result;
    }
    HANDLE retried_process = retried_launch.process_id != 0
        ? OpenProcess(SYNCHRONIZE, FALSE, retried_launch.process_id) : nullptr;
    result = Check(
        retried_process != nullptr,
        "loader-ready retry target could not be opened") && result;
    result = Check(
        WaitForSingleObject(ready, 2000) == WAIT_OBJECT_0,
        "loader-ready retry target did not run") && result;
    SetEvent(stop);
    if (retried_process != nullptr) {
        result = Check(
            WaitForSingleObject(retried_process, 5000) == WAIT_OBJECT_0,
            "loader-ready retry target did not stop") && result;
        CloseHandle(retried_process);
    }

    ResetEvent(ready);
    ResetEvent(stop);
    auto delayed_launch_options = launch_options;
    delayed_launch_options.launcher_path = CurrentExecutable();
    delayed_launch_options.launcher_arguments = L"--launch-suspended " +
        Quote(target_executable) + L" \"" + ready_name + L"\" \"" +
        stop_name + L"\" 350";
    delayed_launch_options.launcher_arguments.append(L" --restrict-access");
    delayed_launch_options.loader_timeout = std::chrono::seconds(5);
    const auto launched = anomaly::launcher::LaunchAndManualMapRuntimeCore(
        delayed_launch_options);
    result = Check(
        launched.Ok() && launched.mapping.remote_image != 0,
        launched.mapping.message.empty()
            ? "discovered target manual-map failed" : launched.mapping.message.c_str()) && result;
    HANDLE denied_process = launched.process_id != 0
        ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, launched.process_id)
        : nullptr;
    const DWORD denied_error = denied_process == nullptr ? GetLastError() : ERROR_SUCCESS;
    if (denied_process != nullptr) CloseHandle(denied_process);
    result = Check(
        denied_process == nullptr && denied_error == ERROR_ACCESS_DENIED,
        "fixture did not deny process handles after early capture") && result;
    if (launched.Ok()) {
        result = Check(
            WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0,
            "discovered target did not run without a debugger") && result;
        result = Check(
            std::filesystem::is_regular_file(root / L"manual-map-marker.txt"),
            "discovered launch did not initialize the mapped core") && result;
    }
    SetEvent(stop);
    const auto exit_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool target_exited{};
    do {
        const auto processes = anomaly::launcher::EnumerateAttachableProcesses(
            target_executable.filename().wstring());
        target_exited = std::none_of(
            processes.begin(), processes.end(), [&launched](const auto& candidate) {
                return candidate.process_id == launched.process_id;
            });
        if (!target_exited) Sleep(10);
    } while (!target_exited && std::chrono::steady_clock::now() < exit_deadline);
    result = Check(
        target_exited, "restricted discovered fixture process did not stop") && result;
    CloseHandle(stop);
    CloseHandle(ready);
    FreeLibrary(collision);
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    return result ? 0 : 1;
}
