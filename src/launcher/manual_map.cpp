#include "anomaly/launcher/manual_map.hpp"
#include "anomaly/launcher/relay_map.hpp"

#include "anomaly/core_api.h"

#include <TlHelp32.h>
#include <Psapi.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" void AnomalyRemoteBootstrapBegin();
extern "C" void AnomalyRemoteBootstrapEnd();

namespace anomaly::launcher {
namespace {

constexpr DWORD kAttachProcessAccess =
    PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ |
    PROCESS_VM_WRITE | PROCESS_CREATE_THREAD;

// The hook injected into the launcher signals this immediately before asking it to start the game.
// The concrete name is generated per session (see GenerateArmedEventName): a fixed, product-named
// event would be enumerable by any process in the session and would correlate runs across machines.
// The fallback literal is duplicated in src/launcher/hook/nte_game_hook.cpp rather than shared
// through a header, because that DLL runs inside a process this repository does not own and is
// deliberately free-standing.
constexpr wchar_t kLauncherArmedEventFallbackName[] = L"Local\\Anomaly.NteGameHook.Armed";
// The environment variable carrying the session's armed-event name to the hook.
constexpr wchar_t kLauncherArmedEventVariable[] = L"ANOMALY_ARMED_EVENT";

// How long capture waits for that signal before falling back to the session's own timing. It only has
// to cover the hook's startup delay plus injection, so it is far shorter than the discovery budget.
constexpr DWORD kHookArmedTimeoutMilliseconds = 90000;

// How long a freshly created process is given to publish its loader list. This is a one-time startup
// cost rather than a user-paced wait: the image, ntdll and kernel32 are in the list within
// milliseconds of creation, so a process still without them is one that never started.
constexpr DWORD kLoaderReadyTimeoutMilliseconds = 15000;

std::mutex g_debug_privilege_mutex;

// One PRNG per process, seeded from the system CSPRNG, for per-session randomization:
// mapping hints, host selection, and event names. Each use needs unpredictability
// between runs, not cryptographic strength.
std::mt19937_64& Engine() {
    static std::mt19937_64 engine{[] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) | device();
    }()};
    return engine;
}

// Per-session armed-event name: "Local\\<16 hex chars>". The name carries no product
// marker and never repeats, so the event cannot be enumerated as a fingerprint of this
// launcher, and hook side learns it through the environment variable below.
std::wstring GenerateArmedEventName() {
    constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::uniform_int_distribution<std::size_t> pick(0, 15);
    std::wstring name = L"Local\\";
    for (int index = 0; index < 16; ++index) {
        name.push_back(kHex[pick(Engine())]);
    }
    return name;
}

// Builds a null-terminated, double-null-terminated environment block that is the
// current process's environment plus one variable. Returns an empty vector on failure,
// and an empty block makes CreateProcessW inherit the current environment, which keeps
// the launch working (the hook then falls back to the fallback event name).
std::vector<wchar_t> BuildEnvironmentWith(const wchar_t* name, const wchar_t* value) {
    const LPWCH current = GetEnvironmentStringsW();
    if (current == nullptr) return {};
    std::vector<wchar_t> block;
    const auto append = [&block](const wchar_t* text) {
        for (const wchar_t* cursor = text; *cursor != L'\0'; ++cursor) {
            block.push_back(*cursor);
        }
        block.push_back(L'\0');
    };
    for (const wchar_t* entry = current; *entry != L'\0';) {
        if (std::wcsncmp(entry, name, std::wcslen(name)) != 0) {
            append(entry);
        }
        entry += std::wcslen(entry) + 1;
    }
    append((std::wstring(name) + L"=" + value).c_str());
    block.push_back(L'\0');
    FreeEnvironmentStringsW(current);
    return block;
}

class Handle final {
public:
    Handle() = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void Reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_{};
};

class ScopedPrivilege final {
public:
    ScopedPrivilege() = default;
    ~ScopedPrivilege() {
        if (enabled_) {
            AdjustTokenPrivileges(
                token_.Get(), FALSE, &previous_, 0, nullptr, nullptr);
        }
    }
    ScopedPrivilege(const ScopedPrivilege&) = delete;
    ScopedPrivilege& operator=(const ScopedPrivilege&) = delete;

    [[nodiscard]] bool Enable(const wchar_t* name, DWORD& error) noexcept {
        HANDLE token{};
        if (OpenProcessToken(
                GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE) {
            error = GetLastError();
            return false;
        }
        token_.Reset(token);

        LUID identifier{};
        if (LookupPrivilegeValueW(nullptr, name, &identifier) == FALSE) {
            error = GetLastError();
            return false;
        }
        TOKEN_PRIVILEGES requested{};
        requested.PrivilegeCount = 1;
        requested.Privileges[0].Luid = identifier;
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD previous_size = sizeof(previous_);
        SetLastError(ERROR_SUCCESS);
        if (AdjustTokenPrivileges(
                token_.Get(), FALSE, &requested, sizeof(previous_),
                &previous_, &previous_size) == FALSE) {
            error = GetLastError();
            return false;
        }
        error = GetLastError();
        if (error != ERROR_SUCCESS) return false;
        enabled_ = true;
        return true;
    }

private:
    Handle token_;
    TOKEN_PRIVILEGES previous_{};
    bool enabled_{};
};

Handle OpenProcessWithAccess(
    DWORD process_id, DWORD access, DWORD& error) noexcept {
    Handle process(OpenProcess(access, FALSE, process_id));
    if (process) {
        error = ERROR_SUCCESS;
        return process;
    }
    const DWORD initial_error = GetLastError();
    if (initial_error != ERROR_ACCESS_DENIED) {
        error = initial_error;
        return {};
    }

    std::scoped_lock privilege_lock(g_debug_privilege_mutex);
    ScopedPrivilege debug_privilege;
    DWORD privilege_error{};
    if (!debug_privilege.Enable(SE_DEBUG_NAME, privilege_error)) {
        error = initial_error;
        return {};
    }
    process.Reset(OpenProcess(access, FALSE, process_id));
    error = process ? ERROR_SUCCESS : GetLastError();
    return process;
}

Handle OpenProcessForAttach(DWORD process_id, DWORD& error) noexcept {
    return OpenProcessWithAccess(process_id, kAttachProcessAccess, error);
}

std::set<DWORD> SnapshotProcessIds(
    std::wstring_view executable_name, DWORD& error) noexcept {
    std::set<DWORD> result;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        error = GetLastError();
        return result;
    }
    PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
    if (Process32FirstW(snapshot.Get(), &entry) == FALSE) {
        error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) error = ERROR_SUCCESS;
        return result;
    }
    const std::wstring requested(executable_name);
    do {
        if (_wcsicmp(entry.szExeFile, requested.c_str()) == 0) {
            result.insert(entry.th32ProcessID);
        }
        entry.dwSize = sizeof(entry);
    } while (Process32NextW(snapshot.Get(), &entry) != FALSE);
    error = GetLastError();
    if (error == ERROR_NO_MORE_FILES) error = ERROR_SUCCESS;
    return result;
}

class LocalModule final {
public:
    explicit LocalModule(HMODULE value = nullptr) noexcept : value_(value) {}
    ~LocalModule() { if (value_ != nullptr) FreeLibrary(value_); }
    LocalModule(const LocalModule&) = delete;
    LocalModule& operator=(const LocalModule&) = delete;
    LocalModule(LocalModule&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    LocalModule& operator=(LocalModule&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) FreeLibrary(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HMODULE Get() const noexcept { return value_; }
private:
    HMODULE value_{};
};

struct RemoteAllocation final {
    HANDLE process{};
    void* address{};
    std::size_t size{};
    bool retained{};

    RemoteAllocation() = default;
    RemoteAllocation(HANDLE owner, void* value, std::size_t bytes) noexcept
        : process(owner), address(value), size(bytes) {}
    ~RemoteAllocation() {
        if (!retained && process != nullptr && address != nullptr) {
            VirtualFreeEx(process, address, 0, MEM_RELEASE);
        }
    }
    RemoteAllocation(const RemoteAllocation&) = delete;
    RemoteAllocation& operator=(const RemoteAllocation&) = delete;
    RemoteAllocation(RemoteAllocation&& other) noexcept
        : process(std::exchange(other.process, nullptr)),
          address(std::exchange(other.address, nullptr)),
          size(std::exchange(other.size, 0)), retained(std::exchange(other.retained, false)) {}
    RemoteAllocation& operator=(RemoteAllocation&& other) noexcept {
        if (this != &other) {
            if (!retained && process != nullptr && address != nullptr) {
                VirtualFreeEx(process, address, 0, MEM_RELEASE);
            }
            process = std::exchange(other.process, nullptr);
            address = std::exchange(other.address, nullptr);
            size = std::exchange(other.size, 0);
            retained = std::exchange(other.retained, false);
        }
        return *this;
    }
};

struct RemoteModule final {
    std::uintptr_t base{};
    std::filesystem::path path;
};

using RemoteModuleMap = std::multimap<std::wstring, RemoteModule, std::less<>>;

struct RemoteBootstrapContext final {
    std::uint64_t image_base{};
    std::uint64_t exception_table{};
    std::uint32_t exception_count{};
    std::uint32_t reserved{};
    std::uint64_t rtl_add_function_table{};
    std::uint64_t rtl_delete_function_table{};
    std::uint64_t tls_callbacks{};
    std::uint64_t entry_point{};
    std::uint64_t anomaly_start{};
    std::uint64_t start_info{};
    std::uint32_t bootstrap_error{};
    std::uint32_t runtime_start_error{};
    std::uint32_t unwind_registered{};
    std::uint32_t reserved2{};
};

static_assert(offsetof(RemoteBootstrapContext, image_base) == 0);
static_assert(offsetof(RemoteBootstrapContext, exception_table) == 8);
static_assert(offsetof(RemoteBootstrapContext, exception_count) == 16);
static_assert(offsetof(RemoteBootstrapContext, rtl_add_function_table) == 24);
static_assert(offsetof(RemoteBootstrapContext, rtl_delete_function_table) == 32);
static_assert(offsetof(RemoteBootstrapContext, tls_callbacks) == 40);
static_assert(offsetof(RemoteBootstrapContext, entry_point) == 48);
static_assert(offsetof(RemoteBootstrapContext, anomaly_start) == 56);
static_assert(offsetof(RemoteBootstrapContext, start_info) == 64);
static_assert(offsetof(RemoteBootstrapContext, bootstrap_error) == 72);
static_assert(offsetof(RemoteBootstrapContext, runtime_start_error) == 76);
static_assert(offsetof(RemoteBootstrapContext, unwind_registered) == 80);

struct DelayImportDescriptor final {
    DWORD attributes{};
    DWORD name{};
    DWORD module_handle{};
    DWORD import_address_table{};
    DWORD import_name_table{};
    DWORD bound_import_address_table{};
    DWORD unload_import_address_table{};
    DWORD timestamp{};
};

struct ImportOverride final {
    std::string_view name;
    std::uint64_t address{};
    bool applied{};
};

struct ParsedImage final {
    std::vector<std::byte> mapped;
    std::filesystem::path source_path;
};

ManualMapResult Failure(
    ManualMapError error, DWORD win32_error, std::string message,
    DWORD runtime_error = ERROR_SUCCESS, std::uintptr_t remote_image = 0) {
    return {error, win32_error, runtime_error, remote_image, std::move(message)};
}

std::wstring Fold(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool SameUser(HANDLE process, DWORD& error) noexcept {
    Handle current_token;
    Handle target_token;
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        error = GetLastError();
        return false;
    }
    current_token.Reset(token);
    token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token) == FALSE) {
        error = GetLastError();
        return false;
    }
    target_token.Reset(token);

    auto token_user = [](HANDLE value, DWORD& failure) -> std::vector<std::byte> {
        DWORD size{};
        GetTokenInformation(value, TokenUser, nullptr, 0, &size);
        if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            failure = GetLastError();
            return {};
        }
        std::vector<std::byte> buffer(size);
        if (GetTokenInformation(value, TokenUser, buffer.data(), size, &size) == FALSE) {
            failure = GetLastError();
            return {};
        }
        return buffer;
    };
    auto current = token_user(current_token.Get(), error);
    if (current.empty()) return false;
    auto target = token_user(target_token.Get(), error);
    if (target.empty()) return false;
    error = ERROR_SUCCESS;
    return EqualSid(
        reinterpret_cast<TOKEN_USER*>(current.data())->User.Sid,
        reinterpret_cast<TOKEN_USER*>(target.data())->User.Sid) != FALSE;
}

bool IsX64Process(HANDLE process, DWORD& error) noexcept {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto function = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (function != nullptr) {
        USHORT process_machine{};
        USHORT native_machine{};
        if (function(process, &process_machine, &native_machine) == FALSE) {
            error = GetLastError();
            return false;
        }
        error = ERROR_SUCCESS;
        return process_machine == IMAGE_FILE_MACHINE_UNKNOWN &&
            native_machine == IMAGE_FILE_MACHINE_AMD64;
    }
    BOOL wow64{};
    if (IsWow64Process(process, &wow64) == FALSE) {
        error = GetLastError();
        return false;
    }
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    error = ERROR_SUCCESS;
    return wow64 == FALSE && system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

std::filesystem::path ProcessPath(HANDLE process, DWORD& error) {
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &size) == FALSE) {
        error = GetLastError();
        return {};
    }
    path.resize(size);
    error = ERROR_SUCCESS;
    return path;
}

bool PathsEqual(const std::filesystem::path& left, const std::filesystem::path& right) noexcept {
    try {
        return Fold(std::filesystem::absolute(left).lexically_normal().wstring()) ==
            Fold(std::filesystem::absolute(right).lexically_normal().wstring());
    } catch (...) {
        return false;
    }
}

bool PathIsWithin(
    const std::filesystem::path& candidate, const std::filesystem::path& root) noexcept {
    try {
        std::wstring candidate_value = Fold(
            std::filesystem::absolute(candidate).lexically_normal().wstring());
        std::wstring root_value = Fold(
            std::filesystem::absolute(root).lexically_normal().wstring());
        if (candidate_value.empty() || root_value.empty()) return false;
        if (candidate_value == root_value) return true;
        if (root_value.back() != L'\\') root_value.push_back(L'\\');
        return candidate_value.starts_with(root_value);
    } catch (...) {
        return false;
    }
}

std::set<DWORD> SnapshotLauncherProcessIds(
    const std::filesystem::path& launcher_directory, DWORD& error) noexcept {
    constexpr std::array<std::wstring_view, 12> launcher_names{
        L"NTELauncher.exe", L"NTEGame.exe", L"NTEBrowser.exe", L"NTEWebBooster.exe",
        L"NTEUpdate.exe", L"NTEErrRep.exe",
        L"NTEGlobalLauncher.exe", L"NTEGlobalGame.exe", L"NTEGlobalBrowser.exe",
        L"NTEGlobalWebBooster.exe", L"NTEGlobalUpdate.exe", L"NTEGlobalErrRep.exe"};
    std::set<DWORD> result;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        error = GetLastError();
        return result;
    }
    PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
    if (Process32FirstW(snapshot.Get(), &entry) == FALSE) {
        error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) error = ERROR_SUCCESS;
        return result;
    }
    do {
        bool known_launcher{};
        for (const auto name : launcher_names) {
            if (_wcsicmp(entry.szExeFile, std::wstring(name).c_str()) == 0) {
                known_launcher = true;
                break;
            }
        }
        if (known_launcher && entry.th32ProcessID != GetCurrentProcessId()) {
            DWORD open_error{};
            Handle process = OpenProcessWithAccess(
                entry.th32ProcessID, PROCESS_QUERY_LIMITED_INFORMATION, open_error);
            if (process) {
                DWORD path_error{};
                const auto path = ProcessPath(process.Get(), path_error);
                if (path_error == ERROR_SUCCESS &&
                    PathIsWithin(path.parent_path(), launcher_directory)) {
                    result.insert(entry.th32ProcessID);
                }
            }
        }
        entry.dwSize = sizeof(entry);
    } while (Process32NextW(snapshot.Get(), &entry) != FALSE);
    error = GetLastError();
    if (error == ERROR_NO_MORE_FILES) error = ERROR_SUCCESS;
    return result;
}

bool TerminateExistingLauncherProcesses(
    const std::filesystem::path& launcher_directory, DWORD& error) noexcept {
    DWORD snapshot_error{};
    const auto process_ids = SnapshotLauncherProcessIds(launcher_directory, snapshot_error);
    if (snapshot_error != ERROR_SUCCESS) {
        error = snapshot_error;
        return false;
    }
    for (const DWORD process_id : process_ids) {
        DWORD open_error{};
        Handle process = OpenProcessWithAccess(
            process_id, PROCESS_TERMINATE | SYNCHRONIZE, open_error);
        if (!process) {
            error = open_error;
            return false;
        }
        if (TerminateProcess(process.Get(), ERROR_PROCESS_ABORTED) == FALSE &&
            WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0) {
            error = GetLastError();
            return false;
        }
        const DWORD wait = WaitForSingleObject(process.Get(), 5000);
        if (wait != WAIT_OBJECT_0) {
            error = wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
            return false;
        }
    }
    error = ERROR_SUCCESS;
    return true;
}

DWORD NtStatusToWin32(NTSTATUS status) noexcept {
    using RtlNtStatusToDosErrorFn = ULONG(WINAPI*)(NTSTATUS);
    const auto convert = reinterpret_cast<RtlNtStatusToDosErrorFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    return convert != nullptr ? convert(status) : ERROR_GEN_FAILURE;
}

class ScopedCaptureThreadPriority final {
public:
    ScopedCaptureThreadPriority() noexcept
        : previous_(GetThreadPriority(GetCurrentThread())),
          applied_(SetThreadPriority(
              GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE) {}

    ~ScopedCaptureThreadPriority() {
        if (applied_ && previous_ != THREAD_PRIORITY_ERROR_RETURN) {
            static_cast<void>(SetThreadPriority(GetCurrentThread(), previous_));
        }
    }

private:
    int previous_{THREAD_PRIORITY_ERROR_RETURN};
    bool applied_{};
};

struct FastTargetCaptureResult final {
    Handle process;
    std::filesystem::path path;
    DWORD process_id{};
    DWORD error{ERROR_NOT_READY};
};

bool QueryMatchingProcessIds(
    std::wstring_view requested_name,
    std::vector<std::byte>& buffer,
    std::vector<DWORD>& process_ids,
    DWORD& error) {
    using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(
        SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (query == nullptr) {
        error = ERROR_PROC_NOT_FOUND;
        return false;
    }

    constexpr NTSTATUS kStatusInfoLengthMismatch =
        static_cast<NTSTATUS>(0xC0000004UL);
    ULONG returned{};
    NTSTATUS status{};
    for (;;) {
        status = query(
            SystemProcessInformation, buffer.data(),
            static_cast<ULONG>(buffer.size()), &returned);
        if (status != kStatusInfoLengthMismatch) break;
        const std::size_t next_size = returned > buffer.size()
            ? static_cast<std::size_t>(returned) + 64 * 1024
            : buffer.size() * 2;
        if (next_size > 32 * 1024 * 1024) {
            error = ERROR_NOT_ENOUGH_MEMORY;
            return false;
        }
        buffer.resize(next_size);
    }
    if (status < 0) {
        error = NtStatusToWin32(status);
        return false;
    }

    process_ids.clear();
    const std::size_t available = returned == 0
        ? buffer.size() : static_cast<std::size_t>(returned);
    std::size_t offset{};
    while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= available) {
        const auto* process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(
            buffer.data() + offset);
        const std::size_t name_length =
            static_cast<std::size_t>(process->ImageName.Length) / sizeof(wchar_t);
        if (process->ImageName.Buffer != nullptr &&
            name_length == requested_name.size() &&
            _wcsnicmp(
                process->ImageName.Buffer, requested_name.data(), name_length) == 0) {
            const auto process_id = static_cast<DWORD>(
                reinterpret_cast<std::uintptr_t>(process->UniqueProcessId));
            if (process_id != 0) process_ids.push_back(process_id);
        }
        if (process->NextEntryOffset == 0) break;
        if (process->NextEntryOffset < sizeof(*process) ||
            process->NextEntryOffset > available - offset) {
            error = ERROR_BAD_LENGTH;
            return false;
        }
        offset += process->NextEntryOffset;
    }
    error = ERROR_SUCCESS;
    return true;
}

bool QueryGrantedAccess(HANDLE handle, ACCESS_MASK& access, DWORD& error) noexcept {
    using NtQueryObjectFn = NTSTATUS(NTAPI*)(
        HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQueryObjectFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    if (query == nullptr) {
        error = ERROR_PROC_NOT_FOUND;
        return false;
    }

    PUBLIC_OBJECT_BASIC_INFORMATION basic{};
    const NTSTATUS status = query(
        handle, ObjectBasicInformation, &basic, sizeof(basic), nullptr);
    if (status < 0) {
        error = NtStatusToWin32(status);
        return false;
    }
    access = basic.GrantedAccess;
    error = ERROR_SUCCESS;
    return true;
}

FastTargetCaptureResult CaptureTargetByFastPolling(
    std::wstring_view requested_target,
    const std::set<DWORD>& existing_targets,
    std::chrono::steady_clock::time_point deadline,
    std::atomic_bool& ready,
    std::stop_token stop) {
    FastTargetCaptureResult result;
    result.error = ERROR_TIMEOUT;
    ScopedCaptureThreadPriority priority;
    std::vector<std::byte> buffer(1024 * 1024);
    std::vector<DWORD> process_ids;
    process_ids.reserve(4);
    std::set<DWORD> rejected;
    ready.store(true, std::memory_order_release);

    while (!stop.stop_requested() &&
        std::chrono::steady_clock::now() < deadline) {
        DWORD query_error{};
        if (!QueryMatchingProcessIds(
                requested_target, buffer, process_ids, query_error)) {
            result.error = query_error;
            return result;
        }

        for (const DWORD process_id : process_ids) {
            if (existing_targets.contains(process_id) || rejected.contains(process_id)) {
                continue;
            }

            DWORD open_error{};
            Handle candidate = OpenProcessWithAccess(
                process_id, kAttachProcessAccess, open_error);
            if (!candidate) {
                result.process_id = process_id;
                result.error = open_error;
                return result;
            }

            ACCESS_MASK granted_access{};
            DWORD access_error{};
            if (!QueryGrantedAccess(candidate.Get(), granted_access, access_error) ||
                (granted_access & kAttachProcessAccess) != kAttachProcessAccess) {
                result.process_id = process_id;
                result.error = access_error == ERROR_SUCCESS
                    ? ERROR_ACCESS_DENIED : access_error;
                return result;
            }

            DWORD inspection_error{};
            const auto path = ProcessPath(candidate.Get(), inspection_error);
            if (inspection_error != ERROR_SUCCESS ||
                Fold(path.filename().wstring()) != requested_target) {
                rejected.insert(process_id);
                continue;
            }

            result.process = std::move(candidate);
            result.path = path;
            result.process_id = process_id;
            result.error = ERROR_SUCCESS;
            return result;
        }
        static_cast<void>(SwitchToThread());
    }
    return result;
}

RemoteModuleMap SnapshotModules(HANDLE process, DWORD& error) {
    RemoteModuleMap result;
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(
        HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (query == nullptr) {
        error = ERROR_PROC_NOT_FOUND;
        return result;
    }

    PROCESS_BASIC_INFORMATION basic{};
    const NTSTATUS query_status = query(
        process, ProcessBasicInformation, &basic, sizeof(basic), nullptr);
    if (query_status < 0) {
        using RtlNtStatusToDosErrorFn = ULONG(WINAPI*)(NTSTATUS);
        const auto convert = reinterpret_cast<RtlNtStatusToDosErrorFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
        error = convert != nullptr ? convert(query_status) : ERROR_GEN_FAILURE;
        return result;
    }

    auto read = [process](const void* remote, void* local, std::size_t size) {
        SIZE_T bytes_read{};
        return remote != nullptr &&
            ReadProcessMemory(process, remote, local, size, &bytes_read) != FALSE &&
            bytes_read == size;
    };
    PEB peb{};
    if (!read(basic.PebBaseAddress, &peb, sizeof(peb))) {
        error = GetLastError();
        return result;
    }
    if (peb.Ldr == nullptr) {
        error = ERROR_NOT_READY;
        return result;
    }

    PEB_LDR_DATA loader{};
    if (!read(peb.Ldr, &loader, sizeof(loader))) {
        error = GetLastError();
        return result;
    }
    const auto list_head = reinterpret_cast<std::uintptr_t>(peb.Ldr) +
        offsetof(PEB_LDR_DATA, InMemoryOrderModuleList);
    auto link = reinterpret_cast<std::uintptr_t>(
        loader.InMemoryOrderModuleList.Flink);
    std::set<std::uintptr_t> visited;
    constexpr std::size_t kMaximumLoaderModules = 1024;
    while (link != list_head) {
        if (link == 0 || visited.size() >= kMaximumLoaderModules ||
            !visited.insert(link).second ||
            link < offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks)) {
            error = ERROR_BAD_LENGTH;
            return {};
        }

        LDR_DATA_TABLE_ENTRY entry{};
        const auto entry_address = link -
            offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (!read(
                reinterpret_cast<const void*>(entry_address),
                &entry, sizeof(entry))) {
            error = GetLastError();
            return {};
        }
        if (entry.DllBase != nullptr && entry.FullDllName.Buffer != nullptr &&
            entry.FullDllName.Length != 0 &&
            entry.FullDllName.Length % sizeof(wchar_t) == 0 &&
            entry.FullDllName.Length <= 32766 * sizeof(wchar_t)) {
            std::wstring path(
                entry.FullDllName.Length / sizeof(wchar_t), L'\0');
            if (!read(
                    entry.FullDllName.Buffer, path.data(),
                    entry.FullDllName.Length)) {
                error = GetLastError();
                return {};
            }
            const std::filesystem::path module_path(path);
            result.emplace(
                Fold(module_path.filename().wstring()),
                RemoteModule{
                    reinterpret_cast<std::uintptr_t>(entry.DllBase),
                    module_path});
        }
        link = reinterpret_cast<std::uintptr_t>(entry.InMemoryOrderLinks.Flink);
    }
    if (result.empty()) {
        error = ERROR_NOT_READY;
        return result;
    }
    error = ERROR_SUCCESS;
    return result;
}

// A process CreateProcessW has only just returned from may not have built its loader list yet: the
// PEB's Ldr is still null, so a module snapshot reports ERROR_NOT_READY and anything that needs a
// remote module address -- an injection, an import resolution -- has nothing to anchor against.
// Every Windows process maps its own image, ntdll and kernel32 before anything else runs, so those
// three are the readiness test.
bool WaitForLoaderReady(
    HANDLE process, std::wstring_view executable_name,
    std::chrono::steady_clock::time_point deadline, RemoteModuleMap& modules, DWORD& error) {
    do {
        modules = SnapshotModules(process, error);
        if (error == ERROR_SUCCESS &&
            modules.contains(Fold(std::wstring(executable_name))) &&
            modules.contains(L"ntdll.dll") && modules.contains(L"kernel32.dll")) {
            return true;
        }
        if (error != ERROR_SUCCESS && error != ERROR_NOT_READY &&
            error != ERROR_PARTIAL_COPY && error != ERROR_BAD_LENGTH) {
            return false;
        }
        Sleep(1);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

template <typename T>
T* Rva(std::vector<std::byte>& image, std::uint32_t rva, std::size_t count = 1) noexcept {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) return nullptr;
    const std::size_t bytes = count * sizeof(T);
    if (rva > image.size() || bytes > image.size() - rva) return nullptr;
    return reinterpret_cast<T*>(image.data() + rva);
}

template <typename T>
const T* Rva(const std::vector<std::byte>& image, std::uint32_t rva, std::size_t count = 1) noexcept {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) return nullptr;
    const std::size_t bytes = count * sizeof(T);
    if (rva > image.size() || bytes > image.size() - rva) return nullptr;
    return reinterpret_cast<const T*>(image.data() + rva);
}

IMAGE_NT_HEADERS64* Headers(std::vector<std::byte>& image) noexcept {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::size_t>(dos->e_lfanew) > image.size() - sizeof(IMAGE_NT_HEADERS64)) {
        return nullptr;
    }
    auto* headers = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    return headers->Signature == IMAGE_NT_SIGNATURE ? headers : nullptr;
}

const IMAGE_NT_HEADERS64* Headers(const std::vector<std::byte>& image) noexcept {
    return Headers(const_cast<std::vector<std::byte>&>(image));
}

std::optional<ParsedImage> ParseImage(
    const std::filesystem::path& path, std::string& failure) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < sizeof(IMAGE_DOS_HEADER) || size > 1024ULL * 1024ULL * 1024ULL) {
        failure = "core image is unavailable or has an invalid size";
        return std::nullopt;
    }
    std::vector<std::byte> file(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (!input) {
        failure = "core image could not be read";
        return std::nullopt;
    }
    auto* source_headers = Headers(file);
    if (source_headers == nullptr ||
        source_headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        (source_headers->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0 ||
        source_headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        source_headers->FileHeader.NumberOfSections == 0 ||
        source_headers->FileHeader.NumberOfSections > 96 ||
        source_headers->OptionalHeader.SizeOfImage < source_headers->OptionalHeader.SizeOfHeaders ||
        source_headers->OptionalHeader.SizeOfImage > 1024ULL * 1024ULL * 1024ULL ||
        source_headers->OptionalHeader.SizeOfHeaders > file.size()) {
        failure = "core image is not a supported x64 DLL";
        return std::nullopt;
    }
    const auto* sections = IMAGE_FIRST_SECTION(source_headers);
    const auto section_table_end = reinterpret_cast<const std::byte*>(
        sections + source_headers->FileHeader.NumberOfSections);
    if (section_table_end > file.data() + file.size()) {
        failure = "core image section table is truncated";
        return std::nullopt;
    }

    ParsedImage result;
    result.source_path = std::filesystem::absolute(path);
    result.mapped.resize(source_headers->OptionalHeader.SizeOfImage);
    std::memcpy(
        result.mapped.data(), file.data(), source_headers->OptionalHeader.SizeOfHeaders);
    for (WORD index = 0; index < source_headers->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        const std::size_t virtual_size = (std::max)(
            static_cast<std::size_t>(section.Misc.VirtualSize),
            static_cast<std::size_t>(section.SizeOfRawData));
        if (section.VirtualAddress > result.mapped.size() ||
            virtual_size > result.mapped.size() - section.VirtualAddress ||
            section.PointerToRawData > file.size() ||
            section.SizeOfRawData > file.size() - section.PointerToRawData) {
            failure = "core image section is outside the file or virtual image";
            return std::nullopt;
        }
        if (section.SizeOfRawData != 0) {
            std::memcpy(
                result.mapped.data() + section.VirtualAddress,
                file.data() + section.PointerToRawData,
                section.SizeOfRawData);
        }
    }
    return result;
}

bool RelocateImage(
    std::vector<std::byte>& image, std::uintptr_t remote_base, std::string& failure) {
    auto* headers = Headers(image);
    if (headers == nullptr) return false;
    const std::int64_t delta = static_cast<std::int64_t>(
        remote_base - static_cast<std::uintptr_t>(headers->OptionalHeader.ImageBase));
    if (delta == 0) return true;
    const auto& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_BASE_RELOCATION)) {
        failure = "core image cannot be relocated";
        return false;
    }
    std::size_t consumed{};
    while (consumed < directory.Size) {
        auto* block = Rva<IMAGE_BASE_RELOCATION>(
            image, directory.VirtualAddress + static_cast<std::uint32_t>(consumed));
        if (block == nullptr || block->SizeOfBlock < sizeof(*block) ||
            block->SizeOfBlock > directory.Size - consumed) {
            failure = "core relocation directory is invalid";
            return false;
        }
        const std::size_t count =
            (block->SizeOfBlock - sizeof(*block)) / sizeof(WORD);
        auto* entries = reinterpret_cast<WORD*>(block + 1);
        for (std::size_t index = 0; index < count; ++index) {
            const WORD type = entries[index] >> 12;
            const WORD offset = entries[index] & 0x0fff;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_DIR64) {
                failure = "core image uses an unsupported relocation";
                return false;
            }
            auto* address = Rva<std::uint64_t>(image, block->VirtualAddress + offset);
            if (address == nullptr) {
                failure = "core relocation target is outside the image";
                return false;
            }
            *address = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(*address) + delta);
        }
        consumed += block->SizeOfBlock;
    }
    headers->OptionalHeader.ImageBase = remote_base;
    return true;
}

LocalModule LoadLocalDependency(
    const std::filesystem::path& image_directory, std::wstring_view name) {
    const auto adjacent = image_directory / std::filesystem::path(name);
    std::error_code error;
    HMODULE module{};
    if (std::filesystem::is_regular_file(adjacent, error) && !error) {
        module = LoadLibraryExW(
            adjacent.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    } else {
        std::wstring system_directory(32768, L'\0');
        const UINT length = GetSystemDirectoryW(
            system_directory.data(), static_cast<UINT>(system_directory.size()));
        if (length != 0 && length < system_directory.size()) {
            system_directory.resize(length);
            const auto system_path =
                std::filesystem::path(system_directory) / std::filesystem::path(name);
            if (std::filesystem::is_regular_file(system_path, error) && !error) {
                module = LoadLibraryExW(
                    system_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
        if (module == nullptr) {
            module = LoadLibraryExW(
                std::wstring(name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
    }
    return LocalModule(module);
}

std::string NarrowPath(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            result.data(), required, nullptr, nullptr) == 0) {
        return {};
    }
    return result;
}

// A remote loader thread exits with the module handle it produced, so its exit code is reported in
// hex where the surrounding diagnostics are read.
std::string Hex32(std::uint32_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) text.push_back(digits[(value >> shift) & 0xF]);
    return text;
}

std::filesystem::path LocalModulePath(HMODULE module) {
    if (module == nullptr) return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::wstring PathKey(const std::filesystem::path& path) {
    std::wstring value = path.lexically_normal().wstring();
    constexpr std::wstring_view extended_prefix{L"\\\\?\\"};
    constexpr std::wstring_view extended_unc_prefix{L"\\\\?\\UNC\\"};
    if (value.starts_with(extended_unc_prefix)) {
        value = L"\\\\" + value.substr(extended_unc_prefix.size());
    } else if (value.starts_with(extended_prefix)) {
        value.erase(0, extended_prefix.size());
    }
    return Fold(std::move(value));
}

RemoteModuleMap::const_iterator FindRemoteModule(
    const RemoteModuleMap& modules, const std::filesystem::path& requested) {
    const auto range = modules.equal_range(Fold(requested.filename().wstring()));
    if (range.first == range.second) return modules.end();
    if (!requested.has_parent_path()) return range.first;
    const std::wstring requested_key = PathKey(requested);
    const auto found = std::find_if(range.first, range.second, [&requested_key](const auto& entry) {
        return PathKey(entry.second.path) == requested_key;
    });
    return found == range.second ? modules.end() : found;
}

std::optional<std::pair<HMODULE, std::filesystem::path>> OwningModule(FARPROC procedure) {
    HMODULE owner{};
    if (procedure == nullptr || GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(procedure), &owner) == FALSE) {
        return std::nullopt;
    }
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return std::nullopt;
    path.resize(length);
    return std::pair(owner, std::filesystem::path(path));
}

std::optional<std::uintptr_t> RemoteAddressForLocalProcedure(
    FARPROC procedure, const RemoteModuleMap& modules) {
    const auto owner = OwningModule(procedure);
    if (!owner) return std::nullopt;
    const auto found = FindRemoteModule(modules, owner->second);
    if (found == modules.end()) return std::nullopt;
    return found->second.base +
        (reinterpret_cast<std::uintptr_t>(procedure) -
         reinterpret_cast<std::uintptr_t>(owner->first));
}

bool RemoteLoadLibrary(
    HANDLE process, const std::filesystem::path& path,
    RemoteModuleMap& modules, std::string& failure) {
    const FARPROC local_load_library = GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    const auto remote_load_library =
        RemoteAddressForLocalProcedure(local_load_library, modules);
    if (!remote_load_library) {
        failure = "remote LoadLibraryW address could not be resolved";
        return false;
    }
    const std::wstring value = path.wstring();
    const std::size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    RemoteAllocation argument(
        process, VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE),
        bytes);
    if (argument.address == nullptr || WriteProcessMemory(
            process, argument.address, value.c_str(), bytes, nullptr) == FALSE) {
        failure = "remote dependency path could not be written";
        return false;
    }
    Handle thread(CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(*remote_load_library),
        argument.address, 0, nullptr));
    if (!thread) {
        failure = "remote dependency loader thread could not be created";
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread.Get(), 15000);
    if (wait != WAIT_OBJECT_0) {
        argument.retained = true;
        failure = "remote dependency loader did not complete";
        return false;
    }
    DWORD loaded{};
    static_cast<void>(GetExitCodeThread(thread.Get(), &loaded));
    DWORD snapshot_error{};
    modules = SnapshotModules(process, snapshot_error);
    if (snapshot_error != ERROR_SUCCESS) {
        failure = "remote modules could not be refreshed";
        return false;
    }
    // The loader thread exits with the module handle it produced, so a null result means nothing was
    // loaded. Reporting success there would hide the failure behind whatever symptom it causes next.
    if (FindRemoteModule(modules, path) == modules.end()) {
        failure = "remote loader returned 0x" + Hex32(loaded) + " for " + NarrowPath(path);
        return false;
    }
    return true;
}

bool EnsureRemoteModule(
    HANDLE process, const std::filesystem::path& requested,
    RemoteModuleMap& modules, std::string& failure) {
    const std::wstring name = Fold(requested.filename().wstring());
    if (FindRemoteModule(modules, requested) != modules.end()) return true;
    if (!RemoteLoadLibrary(process, requested, modules, failure)) return false;
    if (FindRemoteModule(modules, requested) != modules.end()) return true;
    const bool api_set = name.starts_with(L"api-ms-") || name.starts_with(L"ext-ms-");
    if (api_set) return true;
    // Naming the module matters: a manual map loads a whole import table, and "a dependency is
    // missing" without saying which one leaves nothing to act on.
    failure = "remote dependency was not visible after loading: " + NarrowPath(requested);
    return false;
}

bool ResolveOneImport(
    HANDLE process, HMODULE local_module,
    const IMAGE_THUNK_DATA64& source, IMAGE_THUNK_DATA64& destination,
    RemoteModuleMap& modules, std::string& failure,
    const std::vector<std::byte>& image, ImportOverride* import_override) {
    FARPROC procedure{};
    if (IMAGE_SNAP_BY_ORDINAL64(source.u1.Ordinal)) {
        procedure = GetProcAddress(
            local_module,
            reinterpret_cast<const char*>(IMAGE_ORDINAL64(source.u1.Ordinal)));
    } else {
        const auto* import = Rva<IMAGE_IMPORT_BY_NAME>(
            image, static_cast<std::uint32_t>(source.u1.AddressOfData));
        if (import == nullptr) {
            failure = "import name is outside the core image";
            return false;
        }
        const std::size_t name_offset = static_cast<std::size_t>(
            reinterpret_cast<const std::byte*>(import->Name) - image.data());
        if (name_offset >= image.size() ||
            std::memchr(import->Name, '\0', image.size() - name_offset) == nullptr) {
            failure = "import name is not terminated";
            return false;
        }
        if (import_override != nullptr && import_override->name == import->Name) {
            destination.u1.Function = import_override->address;
            import_override->applied = true;
            return true;
        }
        procedure = GetProcAddress(local_module, reinterpret_cast<const char*>(import->Name));
    }
    const auto owner = OwningModule(procedure);
    if (!owner) {
        failure = "imported procedure could not be resolved";
        return false;
    }
    if (!EnsureRemoteModule(process, owner->second, modules, failure)) {
        return false;
    }
    const auto remote = RemoteAddressForLocalProcedure(procedure, modules);
    if (!remote) {
        failure = "remote imported procedure address could not be resolved";
        return false;
    }
    destination.u1.Function = *remote;
    return true;
}

bool ResolveThunkTable(
    HANDLE process, std::vector<std::byte>& image, HMODULE local_module,
    DWORD source_rva, DWORD destination_rva,
    RemoteModuleMap& modules, std::string& failure,
    ImportOverride* import_override) {
    const std::size_t maximum = image.size() / sizeof(IMAGE_THUNK_DATA64);
    for (std::size_t index = 0; index < maximum; ++index) {
        auto* source = Rva<IMAGE_THUNK_DATA64>(
            image, source_rva + static_cast<DWORD>(index * sizeof(IMAGE_THUNK_DATA64)));
        auto* destination = Rva<IMAGE_THUNK_DATA64>(
            image, destination_rva + static_cast<DWORD>(index * sizeof(IMAGE_THUNK_DATA64)));
        if (source == nullptr || destination == nullptr) {
            failure = "import thunk table is outside the core image";
            return false;
        }
        if (source->u1.AddressOfData == 0) return true;
        const IMAGE_THUNK_DATA64 source_value = *source;
        if (!ResolveOneImport(
                process, local_module, source_value, *destination,
                modules, failure, image, import_override)) {
            return false;
        }
    }
    failure = "import thunk table is not terminated";
    return false;
}

bool ResolveImports(
    HANDLE process, ParsedImage& parsed,
    RemoteModuleMap& modules, std::string& failure,
    ImportOverride* import_override = nullptr) {
    auto* headers = Headers(parsed.mapped);
    if (headers == nullptr) return false;
    const auto image_directory = parsed.source_path.parent_path();
    const auto& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress != 0) {
        const std::size_t maximum = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
        bool terminated{};
        for (std::size_t index = 0; index < maximum; ++index) {
            auto* descriptor = Rva<IMAGE_IMPORT_DESCRIPTOR>(
                parsed.mapped,
                directory.VirtualAddress + static_cast<DWORD>(
                    index * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            if (descriptor == nullptr) {
                failure = "import directory is outside the core image";
                return false;
            }
            if (descriptor->Name == 0) {
                terminated = true;
                break;
            }
            const char* name = Rva<char>(parsed.mapped, descriptor->Name);
            if (name == nullptr || std::memchr(
                    name, '\0', parsed.mapped.size() - descriptor->Name) == nullptr) {
                failure = "import module name is invalid";
                return false;
            }
            const std::wstring wide_name(name, name + std::strlen(name));
            const auto adjacent = image_directory / wide_name;
            std::error_code error;
            LocalModule local = LoadLocalDependency(image_directory, wide_name);
            if (local.Get() == nullptr) {
                failure = "local dependency could not be loaded for import resolution";
                return false;
            }
            const auto remote_request = std::filesystem::is_regular_file(adjacent, error) && !error
                ? std::filesystem::absolute(adjacent) : LocalModulePath(local.Get());
            if (remote_request.empty() || !EnsureRemoteModule(
                    process, remote_request, modules, failure)) {
                return false;
            }
            const DWORD source = descriptor->OriginalFirstThunk != 0
                ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
            if (!ResolveThunkTable(
                    process, parsed.mapped, local.Get(),
                    source, descriptor->FirstThunk, modules, failure,
                    import_override)) {
                return false;
            }
        }
        if (!terminated) {
            failure = "import directory is not terminated";
            return false;
        }
    }

    const auto& delay =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delay.VirtualAddress == 0) return true;
    const std::size_t maximum = delay.Size / sizeof(DelayImportDescriptor);
    bool terminated{};
    for (std::size_t index = 0; index < maximum; ++index) {
        auto* descriptor = Rva<DelayImportDescriptor>(
            parsed.mapped,
            delay.VirtualAddress + static_cast<DWORD>(
                index * sizeof(DelayImportDescriptor)));
        if (descriptor == nullptr) {
            failure = "delay import directory is outside the core image";
            return false;
        }
        if (descriptor->name == 0) {
            terminated = true;
            break;
        }
        if ((descriptor->attributes & 1U) == 0) {
            failure = "VA-based delay imports are not supported for x64 images";
            return false;
        }
        const char* name = Rva<char>(parsed.mapped, descriptor->name);
        if (name == nullptr || std::memchr(
                name, '\0', parsed.mapped.size() - descriptor->name) == nullptr) {
            failure = "delay import module name is invalid";
            return false;
        }
        const std::wstring wide_name(name, name + std::strlen(name));
        const auto adjacent = image_directory / wide_name;
        std::error_code error;
        LocalModule local = LoadLocalDependency(image_directory, wide_name);
        if (local.Get() == nullptr) {
            failure = "delay import dependency could not be loaded";
            return false;
        }
        const auto remote_request = std::filesystem::is_regular_file(adjacent, error) && !error
            ? std::filesystem::absolute(adjacent) : LocalModulePath(local.Get());
        if (remote_request.empty() || !EnsureRemoteModule(
                process, remote_request, modules, failure) ||
            !ResolveThunkTable(
                process, parsed.mapped, local.Get(),
                descriptor->import_name_table, descriptor->import_address_table,
                modules, failure, import_override)) {
            if (failure.empty()) failure = "delay import dependency could not be resolved";
            return false;
        }
    }
    if (!terminated) {
        failure = "delay import directory is not terminated";
        return false;
    }
    return true;
}

std::optional<DWORD> ExportRva(
    const std::vector<std::byte>& image, std::string_view requested) {
    const auto* headers = Headers(image);
    if (headers == nullptr) return std::nullopt;
    const auto& data = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto* exports = Rva<IMAGE_EXPORT_DIRECTORY>(image, data.VirtualAddress);
    if (data.VirtualAddress == 0 || exports == nullptr) return std::nullopt;
    const auto* names = Rva<DWORD>(image, exports->AddressOfNames, exports->NumberOfNames);
    const auto* ordinals = Rva<WORD>(image, exports->AddressOfNameOrdinals, exports->NumberOfNames);
    const auto* functions = Rva<DWORD>(image, exports->AddressOfFunctions, exports->NumberOfFunctions);
    if (names == nullptr || ordinals == nullptr || functions == nullptr) return std::nullopt;
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const char* name = Rva<char>(image, names[index]);
        if (name == nullptr || std::memchr(name, '\0', image.size() - names[index]) == nullptr) {
            return std::nullopt;
        }
        if (requested == name) {
            const WORD ordinal = ordinals[index];
            if (ordinal >= exports->NumberOfFunctions) return std::nullopt;
            const DWORD result = functions[ordinal];
            if (result >= data.VirtualAddress && result < data.VirtualAddress + data.Size) {
                return std::nullopt;
            }
            return result;
        }
    }
    return std::nullopt;
}

DWORD SectionProtection(DWORD characteristics) noexcept {
    const bool execute = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool read = (characteristics & IMAGE_SCN_MEM_READ) != 0;
    const bool write = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    if (execute) {
        if (write) return PAGE_EXECUTE_READWRITE;
        return read ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    }
    if (write) return PAGE_READWRITE;
    return read ? PAGE_READONLY : PAGE_NOACCESS;
}

bool ProtectImage(HANDLE process, void* remote, std::vector<std::byte>& image) {
    auto* headers = Headers(image);
    if (headers == nullptr) return false;
    DWORD previous{};
    if (VirtualProtectEx(
            process, remote, image.size(), PAGE_NOACCESS, &previous) == FALSE) {
        return false;
    }
    if (VirtualProtectEx(
            process, remote, headers->OptionalHeader.SizeOfHeaders,
            PAGE_READONLY, &previous) == FALSE) {
        return false;
    }
    const auto* sections = IMAGE_FIRST_SECTION(headers);
    for (WORD index = 0; index < headers->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        const SIZE_T size = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (size == 0) continue;
        if (VirtualProtectEx(
                process,
                static_cast<std::byte*>(remote) + section.VirtualAddress,
                size, SectionProtection(section.Characteristics), &previous) == FALSE) {
            return false;
        }
    }
    return FlushInstructionCache(process, remote, image.size()) != FALSE;
}

bool ReadRemote(HANDLE process, std::uintptr_t address, void* output, std::size_t size) {
    SIZE_T read{};
    return ReadProcessMemory(
        process, reinterpret_cast<const void*>(address), output, size, &read) != FALSE &&
        read == size;
}

// A mapped Anomaly image is recognized by the "AnomalyStart\0" export-name string that
// its export directory keeps in .rdata. Header-based detection (MZ/PE signature, export
// directory walk) stopped working when the mapper started overwriting mapped headers
// with random bytes: the string survives that, the headers do not.
bool RemoteImageExportsAnomalyStart(
    HANDLE process, std::uintptr_t base, std::size_t region_size) {
    constexpr std::string_view needle{ANOMALY_CORE_START_ENTRY};
    constexpr std::size_t kChunkSize = 0x10000;
    std::vector<std::byte> chunk;
    std::size_t overlap = 0;
    for (std::size_t offset = 0; offset < region_size; offset += kChunkSize) {
        const std::size_t bytes = (std::min)(kChunkSize + overlap, region_size - offset);
        chunk.assign(bytes, std::byte{});
        // Section gaps keep PAGE_NOACCESS from ProtectImage; a chunk that fails to
        // read is skipped rather than treated as a verdict, because the readable
        // .rdata holding the needle sits in a different chunk of the same region.
        if (!ReadRemote(process, base + offset, chunk.data(), bytes)) {
            overlap = 0;
            continue;
        }
        for (std::size_t index = 0; index + needle.size() < bytes; ++index) {
            if (std::memcmp(
                    chunk.data() + index, needle.data(), needle.size()) == 0) {
                return true;
            }
        }
        overlap = needle.size() - 1;
    }
    return false;
}

bool HasPrivateMappedCore(HANDLE process) {
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    std::uintptr_t inspected_allocation{};
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQueryEx(
                process, reinterpret_cast<const void*>(address), &region, sizeof(region)) == 0) {
            break;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
        if (region.State == MEM_COMMIT && region.Type == MEM_PRIVATE &&
            base != 0 && base != inspected_allocation) {
            // A manually mapped image is one allocation whose regions carry different
            // protections (read-only headers, execute text, readable rdata, ...), and
            // VirtualQueryEx reports each protection run separately -- the export
            // string sits a few regions into the allocation, so the scan must cover the
            // whole allocation, not the first region. Walk forward while the
            // AllocationBase stays the same to get that extent.
            std::uintptr_t extent_end =
                reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            std::uintptr_t walk = extent_end;
            while (walk < maximum) {
                MEMORY_BASIC_INFORMATION next_region{};
                if (VirtualQueryEx(
                        process, reinterpret_cast<const void*>(walk), &next_region,
                        sizeof(next_region)) == 0) {
                    break;
                }
                if (reinterpret_cast<std::uintptr_t>(next_region.AllocationBase) != base) {
                    break;
                }
                extent_end = reinterpret_cast<std::uintptr_t>(next_region.BaseAddress) +
                    next_region.RegionSize;
                walk = extent_end;
            }
            if (RemoteImageExportsAnomalyStart(process, base, extent_end - base)) {
                return true;
            }
            inspected_allocation = base;
            address = extent_end;
            continue;
        }
        inspected_allocation = base;
        const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;
        address = next;
    }
    return false;
}

RemoteAllocation WriteRemoteWideString(HANDLE process, const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    const std::size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    RemoteAllocation result(
        process, VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE),
        bytes);
    if (result.address == nullptr || WriteProcessMemory(
            process, result.address, value.c_str(), bytes, nullptr) == FALSE) {
        result = {};
    }
    return result;
}

}  // namespace

AttachableProcess InspectAttachableProcess(DWORD process_id) noexcept {
    AttachableProcess result;
    result.process_id = process_id;
    if (process_id == 0 || process_id == GetCurrentProcessId()) {
        result.inspection_error = ERROR_INVALID_PARAMETER;
        return result;
    }
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    if (!process) {
        result.inspection_error = GetLastError();
        return result;
    }
    DWORD error{};
    result.executable_path = ProcessPath(process.Get(), error);
    if (error != ERROR_SUCCESS) {
        result.inspection_error = error;
        return result;
    }
    result.executable_name = result.executable_path.filename().wstring();
    result.owned_by_current_user = SameUser(process.Get(), error);
    if (error != ERROR_SUCCESS || !result.owned_by_current_user) {
        result.inspection_error = error;
        return result;
    }
    result.x64 = IsX64Process(process.Get(), error);
    if (error != ERROR_SUCCESS || !result.x64) {
        result.inspection_error = error;
        return result;
    }
    Handle attach_process = OpenProcessForAttach(process_id, error);
    result.inspection_error = attach_process ? ERROR_SUCCESS : error;
    return result;
}

std::vector<AttachableProcess> EnumerateAttachableProcesses(
    std::wstring_view executable_name) noexcept {
    std::vector<AttachableProcess> result;
    try {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) return result;
        PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
        if (Process32FirstW(snapshot.Get(), &entry) == FALSE) return result;
        const std::wstring folded_requested = Fold(std::wstring(executable_name));
        do {
            if (folded_requested.empty() || Fold(entry.szExeFile) == folded_requested) {
                result.push_back(InspectAttachableProcess(entry.th32ProcessID));
            }
            entry.dwSize = sizeof(entry);
        } while (Process32NextW(snapshot.Get(), &entry) != FALSE);
    } catch (...) {
        result.clear();
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.process_id < right.process_id;
    });
    return result;
}

// Maps a loader-independent PE image that exports AnomalyStart plus the manual-map C++
// exception bridge into a remote process and runs it through the remote bootstrap. The
// start-info block is marshaled verbatim: the core flow passes an AnomalyStartInfo, the
// relay flow passes a RelayStartInfo, and each image reinterprets the pointer its own way.
struct RemoteImageMapRequest final {
    std::filesystem::path image_path;
    std::span<const std::byte> start_info;
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
    // The dynamic-CRT core imports _CxxThrowException and must have it redirected to its
    // own exception bridge; a static-CRT image (the relay) throws through the statically
    // linked runtime against its own image base and carries no such import.
    bool require_cxx_throw_import{true};
};

static ManualMapResult MapImageWithRemoteBootstrap(
    HANDLE process, const RemoteImageMapRequest& request) noexcept {
    try {
        if (HasPrivateMappedCore(process)) {
            return Failure(
                ManualMapError::AlreadyAttached, ERROR_ALREADY_EXISTS,
                "an externally mapped Anomaly core is already present");
        }

        std::string image_failure;
        auto parsed = ParseImage(request.image_path, image_failure);
        if (!parsed) {
            return Failure(
                ManualMapError::ImageInvalid, ERROR_BAD_EXE_FORMAT,
                std::move(image_failure));
        }
        auto* headers = Headers(parsed->mapped);
        const auto start_rva = ExportRva(parsed->mapped, ANOMALY_CORE_START_ENTRY);
        if (headers == nullptr || !start_rva) {
            return Failure(
                ManualMapError::ImageInvalid, ERROR_PROC_NOT_FOUND,
                "core image does not export AnomalyStart");
        }

        const auto& source_tls =
            headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (source_tls.VirtualAddress != 0) {
            const auto* source_tls_directory = Rva<IMAGE_TLS_DIRECTORY64>(
                parsed->mapped, source_tls.VirtualAddress);
            if (source_tls.Size < sizeof(IMAGE_TLS_DIRECTORY64) ||
                source_tls_directory == nullptr) {
                return Failure(
                    ManualMapError::ImageInvalid, ERROR_BAD_EXE_FORMAT,
                    "core TLS directory is outside the image");
            }
            if (source_tls_directory->StartAddressOfRawData !=
                    source_tls_directory->EndAddressOfRawData ||
                source_tls_directory->SizeOfZeroFill != 0) {
                return Failure(
                    ManualMapError::ImageInvalid, ERROR_NOT_SUPPORTED,
                    "core image requires loader-managed static TLS");
            }
        }

        const auto cxx_throw_rva = ExportRva(
            parsed->mapped, ANOMALY_CORE_MANUAL_MAP_CXX_THROW_ENTRY);
        if (!cxx_throw_rva) {
            return Failure(
                ManualMapError::ImageInvalid, ERROR_PROC_NOT_FOUND,
                "core image does not export the manual-map C++ exception bridge");
        }

        // A preferred-base hit leaves the image at the same address in every run and in
        // every process; a hint chosen per mapping keeps the relocations (already run
        // below) moving the image somewhere unpredictable instead. The hint is aligned
        // to the allocation granularity and bounded far below the usual module range so
        // the allocation stays valid for any image size.
        std::uniform_int_distribution<std::uint64_t> hint_distribution(
            0x0001000000000000ULL, 0x0007000000000000ULL);
        const std::uint64_t hint = hint_distribution(Engine()) &
            ~static_cast<std::uint64_t>(0xFFFF);
        void* remote_base = VirtualAllocEx(
            process, reinterpret_cast<void*>(hint),
            parsed->mapped.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote_base == nullptr) {
            remote_base = VirtualAllocEx(
                process, nullptr, parsed->mapped.size(),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        RemoteAllocation remote_image(process, remote_base, parsed->mapped.size());
        if (remote_image.address == nullptr) {
            return Failure(
                ManualMapError::AllocationFailure, GetLastError(),
                "remote core image could not be allocated");
        }
        const auto remote_image_address =
            reinterpret_cast<std::uintptr_t>(remote_image.address);
        if (!RelocateImage(parsed->mapped, remote_image_address, image_failure)) {
            return Failure(
                ManualMapError::ImageInvalid, ERROR_BAD_EXE_FORMAT,
                std::move(image_failure));
        }

        DWORD module_error{};
        auto modules = SnapshotModules(process, module_error);
        if (module_error != ERROR_SUCCESS) {
            return Failure(
                ManualMapError::DependencyFailure, module_error,
                "target modules could not be enumerated");
        }
        ImportOverride cxx_throw_override{
            .name = "_CxxThrowException",
            .address = remote_image_address + *cxx_throw_rva,
        };
        if (!ResolveImports(
                process, *parsed, modules, image_failure,
                &cxx_throw_override)) {
            return Failure(
                ManualMapError::DependencyFailure, ERROR_MOD_NOT_FOUND,
                std::move(image_failure));
        }
        if (!cxx_throw_override.applied && request.require_cxx_throw_import) {
            return Failure(
                ManualMapError::ImageInvalid, ERROR_PROC_NOT_FOUND,
                "core image does not import _CxxThrowException");
        }
        headers = Headers(parsed->mapped);
        if (headers == nullptr || WriteProcessMemory(
                process, remote_image.address, parsed->mapped.data(),
                parsed->mapped.size(), nullptr) == FALSE) {
            return Failure(
                ManualMapError::WriteFailure, GetLastError(),
                "remote core image could not be written");
        }
        // Overwrite the mapped headers (DOS+NT headers plus the section table) with
        // random bytes while the whole region is still writable. Everything the
        // bootstrap thread, the runtime and any export lookup needs has been read from
        // the headers above, or lives past the header range, so the image keeps working;
        // a memory scan of the region no longer finds an MZ/PE signature at its start,
        // which is the most trivially searchable artifact of a manually mapped image.
        const DWORD header_bytes = headers->OptionalHeader.SizeOfHeaders;
        if (header_bytes != 0 && header_bytes <= parsed->mapped.size()) {
            std::vector<std::byte> noise(header_bytes);
            std::uniform_int_distribution<int> byte_distribution(0, 255);
            for (std::byte& value : noise) {
                value = static_cast<std::byte>(byte_distribution(Engine()));
            }
            if (WriteProcessMemory(
                    process, remote_image.address, noise.data(),
                    noise.size(), nullptr) == FALSE) {
                return Failure(
                    ManualMapError::WriteFailure, GetLastError(),
                    "remote image headers could not be overwritten");
            }
        }
        if (!ProtectImage(process, remote_image.address, parsed->mapped)) {
            return Failure(
                ManualMapError::ProtectionFailure, GetLastError(),
                "remote core section protections could not be applied");
        }

        const auto rtl_add = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAddFunctionTable");
        const auto rtl_delete = GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlDeleteFunctionTable");
        const auto remote_rtl_add = RemoteAddressForLocalProcedure(rtl_add, modules);
        const auto remote_rtl_delete = RemoteAddressForLocalProcedure(rtl_delete, modules);
        if (!remote_rtl_add || !remote_rtl_delete) {
            return Failure(
                ManualMapError::BootstrapFailure, ERROR_PROC_NOT_FOUND,
                "remote unwind registration functions could not be resolved");
        }

        RemoteAllocation remote_start_info(
            process, VirtualAllocEx(
                process, nullptr, request.start_info.size(),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), request.start_info.size());
        if (remote_start_info.address == nullptr || WriteProcessMemory(
                process, remote_start_info.address, request.start_info.data(),
                request.start_info.size(), nullptr) == FALSE) {
            return Failure(
                ManualMapError::WriteFailure, GetLastError(),
                "remote start information could not be written");
        }

        const auto& exception =
            headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        const auto& tls = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (headers->OptionalHeader.AddressOfEntryPoint >= parsed->mapped.size() ||
            (exception.VirtualAddress != 0 &&
                (exception.Size == 0 ||
                 exception.Size % sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) != 0 ||
                 Rva<std::byte>(
                     parsed->mapped, exception.VirtualAddress, exception.Size) == nullptr))) {
            return Failure(
                ManualMapError::ImageInvalid, ERROR_BAD_EXE_FORMAT,
                "core entry point or exception directory is invalid");
        }
        std::uint64_t tls_callbacks{};
        if (tls.VirtualAddress != 0) {
            const auto* tls_directory = Rva<IMAGE_TLS_DIRECTORY64>(
                parsed->mapped, tls.VirtualAddress);
            if (tls.Size < sizeof(IMAGE_TLS_DIRECTORY64) || tls_directory == nullptr) {
                return Failure(
                    ManualMapError::ImageInvalid, ERROR_BAD_EXE_FORMAT,
                    "core TLS directory is outside the image");
            }
            tls_callbacks = tls_directory->AddressOfCallBacks;
            if (tls_callbacks != 0) {
                if (tls_callbacks < remote_image_address ||
                    tls_callbacks >= remote_image_address + parsed->mapped.size()) {
                    return Failure(
                        ManualMapError::ImageInvalid, ERROR_BAD_EXE_FORMAT,
                        "core TLS callback table is outside the image");
                }
                const auto callback_rva = static_cast<DWORD>(
                    tls_callbacks - remote_image_address);
                const std::size_t maximum_callbacks = (std::min)(
                    (parsed->mapped.size() - callback_rva) / sizeof(std::uint64_t),
                    static_cast<std::size_t>(4096));
                bool terminated{};
                for (std::size_t index = 0; index < maximum_callbacks; ++index) {
                    const auto* callback = Rva<std::uint64_t>(
                        parsed->mapped,
                        callback_rva + static_cast<DWORD>(index * sizeof(std::uint64_t)));
                    if (callback == nullptr) break;
                    if (*callback == 0) {
                        terminated = true;
                        break;
                    }
                    if (*callback < remote_image_address ||
                        *callback >= remote_image_address + parsed->mapped.size()) {
                        return Failure(
                            ManualMapError::ImageInvalid, ERROR_BAD_EXE_FORMAT,
                            "core TLS callback target is outside the image");
                    }
                }
                if (!terminated) {
                    return Failure(
                        ManualMapError::ImageInvalid, ERROR_BAD_EXE_FORMAT,
                        "core TLS callback table is not terminated");
                }
            }
        }
        RemoteBootstrapContext context{
            .image_base = remote_image_address,
            .exception_table = exception.VirtualAddress == 0
                ? 0 : remote_image_address + exception.VirtualAddress,
            .exception_count = exception.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY),
            .rtl_add_function_table = *remote_rtl_add,
            .rtl_delete_function_table = *remote_rtl_delete,
            .tls_callbacks = tls_callbacks,
            .entry_point = headers->OptionalHeader.AddressOfEntryPoint == 0
                ? 0 : remote_image_address + headers->OptionalHeader.AddressOfEntryPoint,
            .anomaly_start = remote_image_address + *start_rva,
            .start_info = reinterpret_cast<std::uintptr_t>(remote_start_info.address),
        };
        RemoteAllocation remote_context(
            process, VirtualAllocEx(
                process, nullptr, sizeof(context),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), sizeof(context));
        if (remote_context.address == nullptr || WriteProcessMemory(
                process, remote_context.address, &context, sizeof(context), nullptr) == FALSE) {
            return Failure(
                ManualMapError::WriteFailure, GetLastError(),
                "remote bootstrap context could not be written");
        }

        const auto* bootstrap_begin = reinterpret_cast<const std::byte*>(
            reinterpret_cast<const void*>(&AnomalyRemoteBootstrapBegin));
        const auto* bootstrap_end = reinterpret_cast<const std::byte*>(
            reinterpret_cast<const void*>(&AnomalyRemoteBootstrapEnd));
        if (bootstrap_end <= bootstrap_begin ||
            static_cast<std::size_t>(bootstrap_end - bootstrap_begin) > 4096) {
            return Failure(
                ManualMapError::BootstrapFailure, ERROR_INVALID_ADDRESS,
                "remote bootstrap code range is invalid");
        }
        const std::size_t bootstrap_size =
            static_cast<std::size_t>(bootstrap_end - bootstrap_begin);
        RemoteAllocation remote_bootstrap(
            process, VirtualAllocEx(
                process, nullptr, bootstrap_size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), bootstrap_size);
        if (remote_bootstrap.address == nullptr || WriteProcessMemory(
                process, remote_bootstrap.address, bootstrap_begin,
                bootstrap_size, nullptr) == FALSE) {
            return Failure(
                ManualMapError::WriteFailure, GetLastError(),
                "remote bootstrap code could not be written");
        }
        DWORD previous{};
        if (VirtualProtectEx(
                process, remote_bootstrap.address, bootstrap_size,
                PAGE_EXECUTE_READ, &previous) == FALSE ||
            FlushInstructionCache(
                process, remote_bootstrap.address, bootstrap_size) == FALSE) {
            return Failure(
                ManualMapError::ProtectionFailure, GetLastError(),
                "remote bootstrap code could not be made executable");
        }

        Handle thread(CreateRemoteThread(
            process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_bootstrap.address),
            remote_context.address, 0, nullptr));
        if (!thread) {
            return Failure(
                ManualMapError::BootstrapFailure, GetLastError(),
                "remote bootstrap thread could not be created");
        }
        const auto bounded_timeout = std::clamp<std::int64_t>(
            request.timeout.count(), 1, static_cast<std::int64_t>(INFINITE - 1));
        const DWORD wait = WaitForSingleObject(
            thread.Get(), static_cast<DWORD>(bounded_timeout));
        if (wait != WAIT_OBJECT_0) {
            remote_image.retained = true;
            remote_context.retained = true;
            remote_bootstrap.retained = true;
            remote_start_info.retained = true;
            return Failure(
                ManualMapError::Timeout,
                wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT,
                "remote bootstrap did not complete before the timeout",
                ERROR_TIMEOUT, remote_image_address);
        }
        DWORD thread_exit_code{STILL_ACTIVE};
        const bool thread_exit_available =
            GetExitCodeThread(thread.Get(), &thread_exit_code) != FALSE;
        const DWORD thread_exit_error =
            thread_exit_available ? ERROR_SUCCESS : GetLastError();
        if (ReadProcessMemory(
            process, remote_context.address, &context, sizeof(context), nullptr) == FALSE) {
            if (thread_exit_available && thread_exit_code != ERROR_SUCCESS) {
                return Failure(
                    ManualMapError::BootstrapFailure, thread_exit_code,
                    "remote bootstrap thread terminated unexpectedly (exit=" +
                        std::to_string(thread_exit_code) + ")",
                    thread_exit_code, remote_image_address);
            }
            return Failure(
                ManualMapError::BootstrapFailure, GetLastError(),
                "remote bootstrap result could not be read");
        }
        if (context.bootstrap_error != ERROR_SUCCESS ||
            context.runtime_start_error != ERROR_SUCCESS) {
            const DWORD error = context.bootstrap_error != ERROR_SUCCESS
                ? context.bootstrap_error : context.runtime_start_error;
            return Failure(
                ManualMapError::BootstrapFailure, error,
                "Anomaly core initialization failed in the target process",
                context.runtime_start_error, remote_image_address);
        }
        if (!thread_exit_available || thread_exit_code != ERROR_SUCCESS) {
            return Failure(
                ManualMapError::BootstrapFailure,
                thread_exit_available ? thread_exit_code : thread_exit_error,
                "remote bootstrap thread terminated unexpectedly (exit=" +
                    std::to_string(thread_exit_code) + ")",
                thread_exit_available ? thread_exit_code : ERROR_UNHANDLED_EXCEPTION,
                remote_image_address);
        }
        remote_image.retained = true;
        return {ManualMapError::None, ERROR_SUCCESS, ERROR_SUCCESS,
                remote_image_address, "Anomaly core attached"};
    } catch (...) {
        return Failure(
            ManualMapError::ImageInvalid, ERROR_UNHANDLED_EXCEPTION,
            "manual-map attach raised an unexpected exception");
    }
}

static ManualMapResult ManualMapRuntimeCoreWithHandle(
    const ManualMapOptions& options, const AttachableProcess& inspected,
    const Handle& process) noexcept {
    try {
        const auto runtime_root = options.runtime_root.empty()
            ? std::filesystem::absolute(options.core_path).parent_path()
            : std::filesystem::absolute(options.runtime_root);
        const auto log_directory = options.log_directory.empty()
            ? runtime_root / L"logs" : std::filesystem::absolute(options.log_directory);
        RemoteAllocation remote_root = WriteRemoteWideString(process.Get(), runtime_root);
        RemoteAllocation remote_log = WriteRemoteWideString(process.Get(), log_directory);
        if (remote_root.address == nullptr || remote_log.address == nullptr) {
            return Failure(
                ManualMapError::AllocationFailure, GetLastError(),
                "remote runtime paths could not be allocated");
        }
        DWORD snapshot_error{};
        const auto modules = SnapshotModules(process.Get(), snapshot_error);
        const auto main_module = modules.find(Fold(inspected.executable_name));
        if (snapshot_error != ERROR_SUCCESS || main_module == modules.end()) {
            return Failure(
                ManualMapError::BootstrapFailure,
                snapshot_error == ERROR_SUCCESS ? ERROR_MOD_NOT_FOUND : snapshot_error,
                "target main module could not be resolved");
        }

        const AnomalyStartInfo start_info{
            .struct_size = ANOMALY_START_INFO_V1_SIZE,
            .bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION,
            .bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL,
            .flags = 0,
            .bootstrap_module = nullptr,
            .game_module = reinterpret_cast<HMODULE>(main_module->second.base),
            .runtime_root = reinterpret_cast<const WCHAR*>(remote_root.address),
            .log_directory = reinterpret_cast<const WCHAR*>(remote_log.address),
            .external_stop_event = nullptr,
        };
        const auto* start_info_bytes = reinterpret_cast<const std::byte*>(&start_info);
        RemoteImageMapRequest request{
            .image_path = options.core_path,
            .start_info = std::span(start_info_bytes, sizeof(start_info)),
            .timeout = options.timeout,
        };
        ManualMapResult result = MapImageWithRemoteBootstrap(process.Get(), request);
        // A timeout leaves the bootstrap thread running in the target; the strings it may
        // still be reading must outlive this call, exactly like the image allocation the
        // mapping core retains in the same situation.
        if (result.error == ManualMapError::Timeout) {
            remote_root.retained = true;
            remote_log.retained = true;
        }
        return result;
    } catch (...) {
        return Failure(
            ManualMapError::ImageInvalid, ERROR_UNHANDLED_EXCEPTION,
            "manual-map attach raised an unexpected exception");
    }
}

TargetCaptureOutcome WaitForNewTargetProcess(
    const TargetCaptureRequest& request) noexcept {
    TargetCaptureOutcome outcome;
    try {
        if (request.executable_name.empty()) {
            outcome.error = ManualMapError::ProcessLaunchFailure;
            outcome.win32_error = ERROR_INVALID_PARAMETER;
            outcome.message = "target executable name is empty";
            return outcome;
        }
        const std::wstring requested_target = Fold(request.executable_name);

        // Either the caller pinned the exclusion set at the moment that matters (the
        // relay's launcher did, before the game could exist), or this call is the
        // earliest point and the set is taken here.
        std::set<DWORD> existing_targets(
            request.excluded_pids.begin(), request.excluded_pids.end());
        if (request.excluded_pids.empty()) {
            DWORD snapshot_error{};
            existing_targets =
                SnapshotProcessIds(request.executable_name, snapshot_error);
            if (snapshot_error != ERROR_SUCCESS) {
                outcome.error = ManualMapError::ProcessControlFailure;
                outcome.win32_error = snapshot_error;
                outcome.message = "existing target processes could not be inspected";
                return outcome;
            }
        }

        // The launcher hook signals this manual-reset event right before it asks the
        // launcher to start the game, so polling does not spend the whole budget watching
        // a login screen. A missing event simply means nobody gates the capture.
        if (!request.armed_event_name.empty()) {
            Handle armed(OpenEventW(
                SYNCHRONIZE, FALSE, request.armed_event_name.c_str()));
            if (armed) {
                const auto bounded_armed = std::clamp<std::int64_t>(
                    request.armed_timeout.count(), 0,
                    static_cast<std::int64_t>(INFINITE - 1));
                static_cast<void>(WaitForSingleObject(
                    armed.Get(), static_cast<DWORD>(bounded_armed)));
            }
        }

        const auto bounded_timeout = std::clamp<std::int64_t>(
            request.timeout.count(), 1,
            std::chrono::minutes(10).count() * 60'000LL);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(bounded_timeout);
        std::atomic_bool ready{};
        std::stop_source stop;
        const FastTargetCaptureResult captured = CaptureTargetByFastPolling(
            requested_target, existing_targets, deadline, ready, stop.get_token());

        outcome.process_id = captured.process_id;
        outcome.win32_error = captured.error;
        if (captured.error == ERROR_SUCCESS && captured.process) {
            outcome.error = ManualMapError::None;
            return outcome;
        }
        outcome.error = captured.error == ERROR_ACCESS_DENIED
            ? ManualMapError::AccessDenied
            : captured.error == ERROR_TIMEOUT
                ? ManualMapError::ProcessLaunchFailure
                : ManualMapError::ProcessUnavailable;
        outcome.message = captured.error == ERROR_ACCESS_DENIED
            ? "new target process was detected after mapping access was filtered"
            : captured.error == ERROR_TIMEOUT
                ? "no new target process appeared before the timeout"
                : "low-latency target capture failed";
        return outcome;
    } catch (...) {
        outcome.error = ManualMapError::ProcessControlFailure;
        outcome.win32_error = ERROR_UNHANDLED_EXCEPTION;
        outcome.message = "target capture raised an unexpected exception";
        return outcome;
    }
}

ManualMapResult ManualMapAttachCoreToProcessId(
    DWORD process_id, const ManualMapOptions& options) noexcept {
    try {
        if (process_id == 0 || options.core_path.empty()) {
            return Failure(
                ManualMapError::ProcessLaunchFailure, ERROR_INVALID_PARAMETER,
                "attach options are incomplete");
        }
        DWORD open_error{};
        Handle process = OpenProcessForAttach(process_id, open_error);
        if (!process) {
            return Failure(
                ManualMapError::ProcessUnavailable, open_error,
                "target process could not be opened for mapping");
        }
        const AttachableProcess inspected = InspectAttachableProcess(process_id);
        if (!inspected.Compatible()) {
            return Failure(
                !inspected.owned_by_current_user
                    ? ManualMapError::DifferentUser
                    : ManualMapError::IncompatibleArchitecture,
                inspected.inspection_error != ERROR_SUCCESS ? inspected.inspection_error
                    : !inspected.owned_by_current_user ? ERROR_ACCESS_DENIED
                                                       : ERROR_BAD_EXE_FORMAT,
                "target process is not a compatible x64 process owned by this user");
        }
        const auto bounded_loader = std::clamp<std::int64_t>(
            options.loader_timeout.count(), 1,
            std::chrono::minutes(10).count() * 60'000LL);
        const auto loader_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(bounded_loader);
        DWORD module_error = ERROR_NOT_READY;
        RemoteModuleMap loader_modules;
        if (!WaitForLoaderReady(process.Get(), inspected.executable_name,
                loader_deadline, loader_modules, module_error)) {
            return Failure(
                ManualMapError::DependencyFailure,
                module_error == ERROR_SUCCESS ? ERROR_TIMEOUT : module_error,
                "target process loader did not initialize before the timeout");
        }
        return ManualMapRuntimeCoreWithHandle(options, inspected, process);
    } catch (...) {
        return Failure(
            ManualMapError::ImageInvalid, ERROR_UNHANDLED_EXCEPTION,
            "manual-map attach raised an unexpected exception");
    }
}

// ---- Relay flow: the launcher maps a relay image into an innocent host process, and the
// relay captures the game process and maps the core into it, so the injection chain is
// attributed to the host rather than to the launcher. ----

bool CopyRelayField(
    wchar_t* destination, std::size_t capacity, const std::wstring& value) noexcept {
    if (value.size() + 1 > capacity) return false;
    std::copy(value.begin(), value.end(), destination);
    destination[value.size()] = L'\0';
    return true;
}

std::string NarrowRelayMessage(const wchar_t* message) noexcept {
    std::string result;
    for (const wchar_t* cursor = message; *cursor != L'\0'; ++cursor) {
        result.push_back(static_cast<char>(*cursor));
    }
    return result;
}

ManualMapLaunchResult RunRelayAttachSession(
    const ManualMapLaunchOptions& options,
    const std::set<DWORD>& pre_launch_targets,
    const std::wstring& armed_event_name) noexcept {
    ManualMapLaunchResult result;
    try {
        const std::filesystem::path relay_image =
            std::filesystem::absolute(options.relay.relay_image);
        const std::filesystem::path host =
            std::filesystem::absolute(options.relay.host_executable);
        std::error_code relay_probe{};
        if (!std::filesystem::exists(relay_image, relay_probe)) {
            result.mapping = Failure(
                ManualMapError::ImageUnavailable, ERROR_FILE_NOT_FOUND,
                "relay image could not be found");
            return result;
        }
        if (!std::filesystem::exists(host, relay_probe)) {
            result.mapping = Failure(
                ManualMapError::ImageUnavailable, ERROR_FILE_NOT_FOUND,
                "relay host executable could not be found");
            return result;
        }

        // The host is created normally and stays intact: nothing is unmapped, its image
        // and VAD tree look exactly like an ordinary instance, and the relay is added as
        // a private mapping the way the core is added to the game. The window is hidden
        // because the host has no user-facing role.
        STARTUPINFOW startup{
            .cb = sizeof(startup),
            .dwFlags = STARTF_USESHOWWINDOW,
            .wShowWindow = SW_HIDE,
        };
        PROCESS_INFORMATION created{};
        std::wstring command_line = L"\"" + host.wstring() + L"\"";
        if (CreateProcessW(
                host.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0,
                nullptr, host.parent_path().c_str(), &startup, &created) == FALSE) {
            result.mapping = Failure(
                ManualMapError::ProcessLaunchFailure, GetLastError(),
                "relay host process could not be started");
            return result;
        }
        Handle host_process(created.hProcess);
        Handle host_thread(created.hThread);

        const auto host_loader_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(kLoaderReadyTimeoutMilliseconds);
        DWORD host_loader_error{};
        RemoteModuleMap host_modules;
        if (!WaitForLoaderReady(host_process.Get(), host.filename().wstring(),
                host_loader_deadline, host_modules, host_loader_error)) {
            static_cast<void>(TerminateProcess(host_process.Get(), ERROR_PROCESS_ABORTED));
            static_cast<void>(WaitForSingleObject(host_process.Get(), 5000));
            result.mapping = Failure(
                ManualMapError::ProcessControlFailure,
                host_loader_error == ERROR_SUCCESS ? ERROR_TIMEOUT : host_loader_error,
                "relay host loader did not initialize before the timeout");
            return result;
        }

        const std::wstring unique = std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64());
        const std::wstring mapping_name = L"Local\\Anomaly.Relay.Result." + unique;
        const std::wstring event_name = L"Local\\Anomaly.Relay.Done." + unique;
        Handle result_mapping(CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(RelayResult), mapping_name.c_str()));
        if (!result_mapping) {
            static_cast<void>(TerminateProcess(host_process.Get(), ERROR_PROCESS_ABORTED));
            static_cast<void>(WaitForSingleObject(host_process.Get(), 5000));
            result.mapping = Failure(
                ManualMapError::AllocationFailure, GetLastError(),
                "relay result mapping could not be created");
            return result;
        }
        void* result_view = MapViewOfFile(
            result_mapping.Get(), FILE_MAP_WRITE, 0, 0, sizeof(RelayResult));
        Handle result_event(
            CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
        if (result_view == nullptr || !result_event) {
            if (result_view != nullptr) UnmapViewOfFile(result_view);
            static_cast<void>(TerminateProcess(host_process.Get(), ERROR_PROCESS_ABORTED));
            static_cast<void>(WaitForSingleObject(host_process.Get(), 5000));
            result.mapping = Failure(
                ManualMapError::AllocationFailure, GetLastError(),
                "relay result channel could not be created");
            return result;
        }
        std::memset(result_view, 0, sizeof(RelayResult));

        const auto runtime_root = options.manual_map.runtime_root.empty()
            ? std::filesystem::absolute(options.manual_map.core_path).parent_path()
            : std::filesystem::absolute(options.manual_map.runtime_root);
        const auto log_directory = options.manual_map.log_directory.empty()
            ? runtime_root / L"logs"
            : std::filesystem::absolute(options.manual_map.log_directory);

        RelayStartInfo info{};
        info.struct_size = ANOMALY_RELAY_START_INFO_V1_SIZE;
        info.version = ANOMALY_RELAY_START_INFO_VERSION;
        info.armed_timeout_ms = options.hook_path.empty()
            ? 0u : static_cast<std::uint32_t>(kHookArmedTimeoutMilliseconds);
        info.capture_timeout_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            options.target_timeout.count(), 1,
            std::chrono::minutes(10).count() * 60'000LL));
        info.loader_timeout_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            options.loader_timeout.count(), 1,
            std::chrono::minutes(10).count() * 60'000LL));
        info.attach_timeout_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            options.manual_map.timeout.count(), 1,
            static_cast<std::int64_t>(INFINITE - 1)));
        // The pre-launch snapshot is the exclusion set the relay captures against. It was
        // taken before the official launcher existed, so it cannot contain this session's
        // game no matter how early the hook spawned it.
        constexpr std::size_t kMaxExcludedPids =
            std::size(RelayStartInfo{}.excluded_pids);
        info.excluded_pid_count = static_cast<std::uint32_t>(
            std::min(pre_launch_targets.size(), kMaxExcludedPids));
        std::size_t copied{};
        for (const DWORD pid : pre_launch_targets) {
            if (copied == kMaxExcludedPids) break;
            info.excluded_pids[copied++] = pid;
        }
        const bool fields_copied =
            CopyRelayField(info.armed_event_name, std::size(info.armed_event_name),
                armed_event_name) &&
            CopyRelayField(info.game_executable_name, std::size(info.game_executable_name),
                Fold(options.target_executable_name)) &&
            CopyRelayField(info.core_path, std::size(info.core_path),
                options.manual_map.core_path.wstring()) &&
            CopyRelayField(info.runtime_root, std::size(info.runtime_root),
                runtime_root.wstring()) &&
            CopyRelayField(info.log_directory, std::size(info.log_directory),
                log_directory.wstring()) &&
            CopyRelayField(info.result_mapping_name, std::size(info.result_mapping_name),
                mapping_name) &&
            CopyRelayField(info.result_event_name, std::size(info.result_event_name),
                event_name);
        if (!fields_copied) {
            UnmapViewOfFile(result_view);
            static_cast<void>(TerminateProcess(host_process.Get(), ERROR_PROCESS_ABORTED));
            static_cast<void>(WaitForSingleObject(host_process.Get(), 5000));
            result.mapping = Failure(
                ManualMapError::ProcessLaunchFailure, ERROR_INVALID_PARAMETER,
                "relay parameters exceed the relay contract field widths");
            return result;
        }

        // The relay runs the whole capture-then-attach sequence inside AnomalyStart, so
        // hop one is budgeted for all of it: the armed wait, target discovery, the game
        // loader, the core bootstrap, and slack for the IPC itself.
        const std::int64_t hop1_budget = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(info.armed_timeout_ms) +
                info.capture_timeout_ms + info.loader_timeout_ms +
                info.attach_timeout_ms + 30'000LL,
            1, static_cast<std::int64_t>(INFINITE - 1));
        const auto* info_bytes = reinterpret_cast<const std::byte*>(&info);
        RemoteImageMapRequest hop1{
            .image_path = relay_image,
            .start_info = std::span(info_bytes, sizeof(info)),
            .timeout = std::chrono::milliseconds(hop1_budget),
            .require_cxx_throw_import = false,
        };
        const ManualMapResult hop1_result =
            MapImageWithRemoteBootstrap(host_process.Get(), hop1);

        // The relay publishes its result and signals the event before AnomalyStart
        // returns, so by the time hop one settles the event is either signalled or the
        // relay never got far enough to report.
        RelayResult relay_result{};
        bool have_result = false;
        if (WaitForSingleObject(result_event.Get(), 5000) == WAIT_OBJECT_0) {
            std::memcpy(&relay_result, result_view, sizeof(RelayResult));
            have_result = relay_result.struct_size == ANOMALY_RELAY_RESULT_V1_SIZE &&
                relay_result.version == ANOMALY_RELAY_RESULT_VERSION;
        }
        UnmapViewOfFile(result_view);
        // The host is a throwaway: whatever the relay reported, the process has no
        // further role once its AnomalyStart has returned.
        static_cast<void>(TerminateProcess(host_process.Get(), ERROR_PROCESS_ABORTED));
        static_cast<void>(WaitForSingleObject(host_process.Get(), 5000));

        if (hop1_result.Ok() && have_result &&
            relay_result.error ==
                static_cast<std::uint32_t>(ManualMapError::None)) {
            result.mapping = ManualMapResult{
                ManualMapError::None,
                ERROR_SUCCESS,
                relay_result.runtime_start_error,
                static_cast<std::uintptr_t>(relay_result.remote_image),
                "Anomaly core attached through the relay host"};
            result.process_id = relay_result.game_process_id;
            return result;
        }
        if (have_result) {
            result.mapping = Failure(
                static_cast<ManualMapError>(relay_result.error),
                relay_result.win32_error,
                NarrowRelayMessage(relay_result.message),
                relay_result.runtime_start_error,
                static_cast<std::uintptr_t>(relay_result.remote_image));
            result.process_id = relay_result.game_process_id;
            return result;
        }
        if (hop1_result.Ok()) {
            result.mapping = Failure(
                ManualMapError::BootstrapFailure, ERROR_UNHANDLED_EXCEPTION,
                "relay image completed without reporting a result");
            return result;
        }
        result.mapping = Failure(
            hop1_result.error, hop1_result.win32_error,
            "relay bootstrap: " + hop1_result.message,
            hop1_result.runtime_start_error, hop1_result.remote_image);
        return result;
    } catch (...) {
        result.mapping = Failure(
            ManualMapError::ProcessLaunchFailure, ERROR_UNHANDLED_EXCEPTION,
            "relay attach session raised an unexpected exception");
        return result;
    }
}

// The kernel session hands the capture-and-map tail to the mapper client. The client
// owns the driver conversation: it arms the create-process callback, waits for the game
// the official launcher produces, and maps the core through the driver's one-shot
// operations. The result contract is an INI file:
//   [result] error=<ManualMapError> win32=<code> pid=<id> image=<base>
//   runtime_start=<code> message=<text>
// written before the client exits, so the launcher never talks to the driver itself.
ManualMapLaunchResult RunKernelAttachSession(
    const ManualMapLaunchOptions& options) noexcept {
    ManualMapLaunchResult result;
    try {
        const std::filesystem::path client =
            std::filesystem::absolute(options.kernel.client_executable);
        std::error_code client_probe{};
        if (!std::filesystem::is_regular_file(client, client_probe) || client_probe) {
            result.mapping = Failure(
                ManualMapError::ImageUnavailable, ERROR_FILE_NOT_FOUND,
                "kernel mapper client could not be found");
            return result;
        }
        const auto core_path = std::filesystem::absolute(options.manual_map.core_path);
        const auto runtime_root = options.manual_map.runtime_root.empty()
            ? core_path.parent_path()
            : std::filesystem::absolute(options.manual_map.runtime_root);
        const auto log_directory = options.manual_map.log_directory.empty()
            ? runtime_root / L"logs"
            : std::filesystem::absolute(options.manual_map.log_directory);

        wchar_t temp_directory[MAX_PATH]{};
        const UINT temp_length = GetTempPathW(
            MAX_PATH, temp_directory);
        if (temp_length == 0 || temp_length >= MAX_PATH) {
            result.mapping = Failure(
                ManualMapError::ProcessLaunchFailure, GetLastError(),
                "kernel mapper result path could not be resolved");
            return result;
        }
        const std::filesystem::path result_file =
            std::filesystem::path(temp_directory) /
            (L"Anomaly.Kmap.Result." +
             std::to_wstring(GetCurrentProcessId()) + L"." +
             std::to_wstring(GetTickCount64()) + L".ini");
        std::error_code remove_probe{};
        static_cast<void>(std::filesystem::remove(result_file, remove_probe));

        const auto capture_ms = std::clamp<std::int64_t>(
            options.kernel.hot_pid != 0
                ? 1  // hot attach: no creation to wait for
                : options.target_timeout.count(),
            1, std::chrono::minutes(10).count() * 60'000LL);
        // The kernel session's stages are serial and each can stall for its
        // full 15s window under a hooked loader (loader probe, per-module
        // DllMain, TLS registration, bootstrap poll), so the shared 30s
        // default dies mid-replay with no result INI. Floor the kernel path
        // at two minutes; an explicit larger timeout still wins.
        const auto attach_ms = std::clamp<std::int64_t>(
            std::max<std::int64_t>(
                options.manual_map.timeout.count(),
                std::chrono::seconds(120).count()),
            1, INFINITE - 1);
        // The armed wait runs inside the client, so its budget belongs here too, exactly
        // the way the relay session folds it into hop one.
        const std::int64_t armed_ms = options.hook_path.empty() || options.kernel.hot_pid != 0
            ? 0 : static_cast<std::int64_t>(kHookArmedTimeoutMilliseconds);
        const std::int64_t budget = std::clamp<std::int64_t>(
            armed_ms + capture_ms + attach_ms + 30'000LL, 1,
            static_cast<std::int64_t>(INFINITE - 1));

        std::wstring arguments = L"--core \"" + core_path.wstring() + L"\"";
        arguments += L" --target " + options.target_executable_name;
        if (options.kernel.hot_pid != 0) {
            // Hot attach: the game is already running, so the client adopts the
            // pid directly instead of waiting for a creation callback.
            arguments += L" --pid " + std::to_wstring(options.kernel.hot_pid);
        }
        arguments += L" --runtime-root \"" + runtime_root.wstring() + L"\"";
        arguments += L" --log-directory \"" + log_directory.wstring() + L"\"";
        arguments += L" --capture-timeout " + std::to_wstring(capture_ms);
        arguments += L" --timeout " + std::to_wstring(attach_ms);
        if (!options.hook_path.empty()) {
            arguments += L" --armed-timeout " + std::to_wstring(armed_ms);
        }
        arguments += L" --result \"" + result_file.wstring() + L"\"";
        const std::wstring command_line =
            L"\"" + client.wstring() + L"\" " + arguments;

        STARTUPINFOW startup{
            .cb = sizeof(startup),
            .dwFlags = STARTF_USESHOWWINDOW,
            .wShowWindow = SW_HIDE,
        };
        PROCESS_INFORMATION created{};
        if (CreateProcessW(
                client.c_str(), const_cast<LPWSTR>(command_line.c_str()), nullptr,
                nullptr, FALSE, 0, nullptr, client.parent_path().c_str(), &startup,
                &created) == FALSE) {
            result.mapping = Failure(
                ManualMapError::ProcessLaunchFailure, GetLastError(),
                "kernel mapper client could not be started");
            return result;
        }
        Handle client_process(created.hProcess);
        Handle client_thread(created.hThread);

        const DWORD wait = WaitForSingleObject(
            client_process.Get(), static_cast<DWORD>(budget));
        if (wait != WAIT_OBJECT_0) {
            static_cast<void>(
                TerminateProcess(client_process.Get(), ERROR_PROCESS_ABORTED));
            static_cast<void>(WaitForSingleObject(client_process.Get(), 5000));
            result.mapping = Failure(
                ManualMapError::Timeout, WAIT_TIMEOUT,
                "kernel mapper client did not finish before the session budget");
            return result;
        }

        std::error_code result_probe{};
        if (!std::filesystem::is_regular_file(result_file, result_probe) ||
            result_probe) {
            result.mapping = Failure(
                ManualMapError::BootstrapFailure, ERROR_UNHANDLED_EXCEPTION,
                "kernel mapper client exited without reporting a result");
            return result;
        }
        const std::wstring ini = result_file.wstring();
        const auto read_int = [&ini](const wchar_t* key) noexcept -> std::uint32_t {
            const UINT value = GetPrivateProfileIntW(L"result", key, 0, ini.c_str());
            return value;
        };
        // Every key is read before the file is removed: once the file is gone
        // the profile APIs answer every key with its default, and those
        // defaults -- error 0, pid 0 -- are exactly the shape of "succeeded
        // with no pid", which the caller reports as a failure with error
        // code 0. Deleting after reading keeps the two from blurring.
        const auto reported_error = static_cast<ManualMapError>(read_int(L"error"));
        const auto reported_pid = read_int(L"pid");
        const auto reported_win32 = read_int(L"win32");
        const auto reported_runtime_start = read_int(L"runtime_start");
        // The mapped base is a 64-bit pointer; GetPrivateProfileIntW would
        // truncate it to 32 bits, so it travels as a string.
        wchar_t image_text[32]{};
        GetPrivateProfileStringW(
            L"result", L"image", L"0", image_text,
            static_cast<DWORD>(std::size(image_text)), ini.c_str());
        const std::uintptr_t reported_image =
            static_cast<std::uintptr_t>(std::wcstoull(image_text, nullptr, 10));
        wchar_t message[512]{};
        GetPrivateProfileStringW(
            L"result", L"message", L"", message, static_cast<DWORD>(std::size(message)),
            ini.c_str());
        std::string narrow;
        for (const wchar_t* cursor = message; *cursor != L'\0'; ++cursor) {
            narrow.push_back(static_cast<char>(*cursor));
        }
        static_cast<void>(std::filesystem::remove(result_file, remove_probe));

        if (reported_error == ManualMapError::None) {
            result.mapping = ManualMapResult{
                ManualMapError::None,
                ERROR_SUCCESS,
                reported_runtime_start,
                reported_image,
                "Anomaly core attached through the kernel mapper",
            };
            result.process_id = reported_pid;
            return result;
        }
        result.mapping = Failure(
            reported_error, reported_win32,
            narrow.empty() ? "kernel mapper client reported a failure" : narrow,
            reported_runtime_start, reported_image);
        result.process_id = reported_pid;
        return result;
    } catch (...) {
        result.mapping = Failure(
            ManualMapError::ProcessLaunchFailure, ERROR_UNHANDLED_EXCEPTION,
            "kernel attach session raised an unexpected exception");
        return result;
    }
}

ManualMapLaunchResult LaunchAndManualMapRuntimeCore(
    const ManualMapLaunchOptions& options) noexcept {
    ManualMapLaunchResult result;
    try {
        // Hot attach: the game is already running and the kernel client adopts
        // its pid directly. Starting the official launcher, hooking it, and
        // snapshotting for capture are all the cold path -- none of them apply,
        // so none of them run. It also needs none of the cold path's inputs:
        // the launcher path is meaningless when no launcher is started.
        if (options.kernel.Enabled() && options.kernel.hot_pid != 0) {
            if (options.manual_map.core_path.empty()) {
                result.mapping = Failure(
                    ManualMapError::ProcessLaunchFailure, ERROR_INVALID_PARAMETER,
                    "hot attach options are incomplete");
                return result;
            }
            return RunKernelAttachSession(options);
        }

        if (options.launcher_path.empty() || options.target_executable_name.empty() ||
            options.manual_map.core_path.empty()) {
            result.mapping = Failure(
                ManualMapError::ProcessLaunchFailure, ERROR_INVALID_PARAMETER,
                "launcher capture options are incomplete");
            return result;
        }

        const std::filesystem::path launcher =
            std::filesystem::absolute(options.launcher_path);
        const std::filesystem::path working_directory = options.working_directory.empty()
            ? launcher.parent_path() : std::filesystem::absolute(options.working_directory);
        std::wstring command_line = L"\"" + launcher.wstring() + L"\"";
        if (!options.launcher_arguments.empty()) {
            command_line.push_back(L' ');
            command_line.append(options.launcher_arguments);
        }

        DWORD launcher_cleanup_error{};
        if (!TerminateExistingLauncherProcesses(
                launcher.parent_path(), launcher_cleanup_error)) {
            result.mapping = Failure(
                ManualMapError::ProcessControlFailure, launcher_cleanup_error,
                "existing official launcher processes could not be stopped");
            return result;
        }

        // Debugger and Job membership both alter launcher-visible process state.
        DWORD snapshot_error{};
        const auto existing_targets = SnapshotProcessIds(
            options.target_executable_name, snapshot_error);
        if (snapshot_error != ERROR_SUCCESS) {
            result.mapping = Failure(
                ManualMapError::ProcessControlFailure, snapshot_error,
                "existing target processes could not be inspected before launch");
            return result;
        }

        const std::wstring requested_target = Fold(options.target_executable_name);
        const auto discovery_timeout = std::clamp<std::int64_t>(
            options.target_timeout.count(), 1, std::chrono::minutes(10).count() * 60'000LL);

        // The hook signals this immediately before asking it to start the game, so capture
        // can be held until the seconds that matter. Polling is unavoidable -- the game process is
        // created by ACE-BASE.sys, which offers nothing to subscribe to -- but a loop that starts with
        // the launcher would spend most of its life watching a process that is still logging in.
        // Nothing injects a hook while the launcher's auto-start is off, so nothing has to be waited
        // for: capture polls from the start for the HTGame.exe the player starts from the launcher.
        Handle armed;
        std::wstring armed_event_name;
        if (!options.hook_path.empty()) {
            armed_event_name = GenerateArmedEventName();
            armed.Reset(CreateEventW(nullptr, TRUE, FALSE, armed_event_name.c_str()));
            if (!armed) {
                result.mapping = Failure(ManualMapError::ProcessControlFailure, GetLastError(),
                    "the launcher hook handshake could not be created");
                return result;
            }
        }

        STARTUPINFOW startup{.cb = sizeof(startup)};
        PROCESS_INFORMATION created{};
        constexpr DWORD incompatible_flags =
            DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED;
        // The armed-event name travels to the hook through the environment: the hook runs
        // inside the launched process and reads the variable at startup. On a block-build
        // failure the environment is inherited whole, and the hook falls back to the
        // fallback event name -- the handshake then uses the old fixed name, which keeps
        // the session alive at the cost of the randomized name.
        const std::vector<wchar_t> environment = armed_event_name.empty()
            ? std::vector<wchar_t>{} : BuildEnvironmentWith(
                kLauncherArmedEventVariable, armed_event_name.c_str());
        if (CreateProcessW(
                launcher.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                options.creation_flags & ~incompatible_flags,
                environment.empty()
                    ? nullptr
                    : const_cast<wchar_t*>(environment.data()),
                const_cast<LPWSTR>(working_directory.c_str()), &startup, &created) == FALSE) {
            const DWORD launch_error = GetLastError();
            result.mapping = Failure(
                ManualMapError::ProcessLaunchFailure, launch_error,
                "official launcher could not be started for target capture");
            return result;
        }

        Handle launcher_process(created.hProcess);
        Handle launcher_thread(created.hThread);

        // The hook is injected before capture begins, because it is what puts the launcher into a
        // state where the game can start at all and what reports that the moment has arrived.
        if (!options.hook_path.empty()) {
            // Injection resolves LoadLibraryW through the launcher's own module list, which does not
            // exist the instant CreateProcessW returns. Asking too early reports ERROR_NOT_READY and
            // leaves the launcher running with nothing in it.
            const auto loader_deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kLoaderReadyTimeoutMilliseconds);
            DWORD hook_error{};
            RemoteModuleMap launcher_modules;
            std::string hook_failure;
            if (!WaitForLoaderReady(launcher_process.Get(), launcher.filename().wstring(),
                    loader_deadline, launcher_modules, hook_error)) {
                result.mapping = Failure(ManualMapError::ProcessControlFailure,
                    hook_error == ERROR_SUCCESS ? ERROR_TIMEOUT : hook_error,
                    "the launcher's loader did not initialize before the hook timeout");
                return result;
            }
            if (!RemoteLoadLibrary(launcher_process.Get(), options.hook_path, launcher_modules,
                    hook_failure)) {
                result.mapping = Failure(ManualMapError::ProcessControlFailure,
                    ERROR_DLL_INIT_FAILED, "the game hook could not be injected");
                return result;
            }
        }

        // The kernel path replaces this process's capture-and-map tail the way the relay
        // path does, but with a stronger property: the mapper client performing the tail
        // holds no game handles either, because every target operation happens inside
        // the driver. Kernel takes precedence when both are configured.
        if (options.kernel.Enabled()) {
            return RunKernelAttachSession(options);
        }
        if (options.relay.Enabled()) {
            return RunRelayAttachSession(options, existing_targets, armed_event_name);
        }

        FastTargetCaptureResult captured;
        std::atomic_bool capture_ready{};
        std::jthread capture_thread([&](std::stop_token stop) noexcept {
            try {
                // Bounded, so a launcher whose hook never reports still falls back to capturing on
                // the session's own timing instead of stalling the launch.
                if (armed) {
                    static_cast<void>(
                        WaitForSingleObject(armed.Get(), kHookArmedTimeoutMilliseconds));
                }
                const auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(discovery_timeout);
                captured = CaptureTargetByFastPolling(
                    requested_target, existing_targets, deadline, capture_ready, stop);
            } catch (...) {
                captured.error = ERROR_UNHANDLED_EXCEPTION;
                capture_ready.store(true, std::memory_order_release);
            }
        });
        capture_thread.join();
        Handle target_process = std::move(captured.process);
        Handle target_cleanup;
        AttachableProcess target;
        result.process_id = captured.process_id;

        if (!target_process) {
            result.mapping = Failure(
                captured.error == ERROR_ACCESS_DENIED
                    ? ManualMapError::AccessDenied
                    : captured.error == ERROR_TIMEOUT
                        ? ManualMapError::ProcessLaunchFailure
                        : ManualMapError::ProcessUnavailable,
                captured.error,
                captured.error == ERROR_ACCESS_DENIED
                    ? "new target process was detected after mapping access was filtered"
                    : captured.error == ERROR_TIMEOUT
                        ? "official launcher did not create a new target process before the timeout"
                        : "low-latency target capture failed");
            return result;
        }

        DWORD cleanup_error{};
        target_cleanup = OpenProcessWithAccess(
            captured.process_id, PROCESS_TERMINATE | SYNCHRONIZE, cleanup_error);
        target.process_id = captured.process_id;
        target.executable_path = captured.path;
        target.executable_name = captured.path.filename().wstring();
        DWORD inspection_error{};
        target.owned_by_current_user = SameUser(target_process.Get(), inspection_error);
        target.x64 = inspection_error == ERROR_SUCCESS &&
            IsX64Process(target_process.Get(), inspection_error);
        target.inspection_error = inspection_error;
        if (inspection_error != ERROR_SUCCESS ||
            !target.owned_by_current_user || !target.x64) {
            result.mapping = Failure(
                !target.owned_by_current_user
                    ? ManualMapError::DifferentUser
                    : ManualMapError::IncompatibleArchitecture,
                inspection_error != ERROR_SUCCESS ? inspection_error
                    : !target.owned_by_current_user ? ERROR_ACCESS_DENIED
                                                    : ERROR_BAD_EXE_FORMAT,
                "captured target process is not a compatible x64 process");
        }

        bool mapping_started{};
        if (result.mapping.error == ManualMapError::None) {
            const auto loader_timeout = std::clamp<std::int64_t>(
                options.loader_timeout.count(), 1,
                std::chrono::minutes(10).count() * 60'000LL);
            const auto loader_deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(loader_timeout);
            DWORD module_error = ERROR_NOT_READY;
            RemoteModuleMap loader_modules;
            if (!WaitForLoaderReady(target_process.Get(), target.executable_name,
                    loader_deadline, loader_modules, module_error)) {
                result.mapping = Failure(
                    ManualMapError::DependencyFailure,
                    module_error == ERROR_SUCCESS ? ERROR_TIMEOUT : module_error,
                    "target process loader did not initialize before the timeout; the target "
                    "was left running because mapping had not started");
            } else {
                ManualMapOptions mapping_options = options.manual_map;
                mapping_options.process_id = target.process_id;
                mapping_started = true;
                result.mapping = ManualMapRuntimeCoreWithHandle(
                    mapping_options, target, target_process);
            }
        }

        if (!result.mapping.Ok() && mapping_started) {
            if (target_cleanup) {
                static_cast<void>(TerminateProcess(
                    target_cleanup.Get(), ERROR_PROCESS_ABORTED));
                static_cast<void>(WaitForSingleObject(target_cleanup.Get(), 5000));
            }
        }
        return result;
    } catch (...) {
        result.mapping = Failure(
            ManualMapError::ProcessLaunchFailure, ERROR_UNHANDLED_EXCEPTION,
            "official launcher target capture raised an unexpected exception");
        return result;
    }
}

}  // namespace anomaly::launcher
