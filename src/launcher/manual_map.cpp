#include "anomaly/launcher/manual_map.hpp"

#include "anomaly/core_api.h"

#include <TlHelp32.h>
#include <Psapi.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>

extern "C" void AnomalyRemoteBootstrapBegin();
extern "C" void AnomalyRemoteBootstrapEnd();

namespace anomaly::launcher {
namespace {

constexpr DWORD kAttachProcessAccess =
    PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ |
    PROCESS_VM_WRITE | PROCESS_CREATE_THREAD;
std::mutex g_debug_privilege_mutex;

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

Handle OpenProcessForCapture(DWORD process_id, DWORD& error) noexcept {
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
    DWORD snapshot_error{};
    modules = SnapshotModules(process, snapshot_error);
    if (snapshot_error != ERROR_SUCCESS) {
        failure = "remote modules could not be refreshed";
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
    failure = "remote dependency was not visible after loading";
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

bool RemoteImageExportsAnomalyStart(
    HANDLE process, std::uintptr_t base, std::size_t region_size) {
    IMAGE_DOS_HEADER dos{};
    if (region_size < sizeof(IMAGE_DOS_HEADER) ||
        !ReadRemote(process, base, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        region_size < sizeof(IMAGE_NT_HEADERS64) ||
        static_cast<std::size_t>(dos.e_lfanew) > region_size - sizeof(IMAGE_NT_HEADERS64)) {
        return false;
    }
    IMAGE_NT_HEADERS64 headers{};
    if (!ReadRemote(process, base + dos.e_lfanew, &headers, sizeof(headers)) ||
        headers.Signature != IMAGE_NT_SIGNATURE ||
        headers.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        (headers.FileHeader.Characteristics & IMAGE_FILE_DLL) == 0 ||
        headers.OptionalHeader.SizeOfImage < headers.OptionalHeader.SizeOfHeaders) {
        return false;
    }
    const auto& data = headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (data.VirtualAddress == 0 || data.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        data.VirtualAddress > headers.OptionalHeader.SizeOfImage - sizeof(IMAGE_EXPORT_DIRECTORY)) {
        return false;
    }
    IMAGE_EXPORT_DIRECTORY exports{};
    if (!ReadRemote(process, base + data.VirtualAddress, &exports, sizeof(exports)) ||
        exports.NumberOfNames == 0 || exports.NumberOfNames > 65536) {
        return false;
    }
    std::vector<DWORD> names(exports.NumberOfNames);
    if (!ReadRemote(
            process, base + exports.AddressOfNames, names.data(),
            names.size() * sizeof(DWORD))) {
        return false;
    }
    for (DWORD rva : names) {
        std::array<char, 64> name{};
        SIZE_T read{};
        if (ReadProcessMemory(
                process, reinterpret_cast<const void*>(base + rva), name.data(),
                name.size() - 1, &read) != FALSE && read != 0 &&
            std::string_view(name.data()) == ANOMALY_CORE_START_ENTRY) {
            return true;
        }
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
            base != 0 && base != inspected_allocation &&
            RemoteImageExportsAnomalyStart(process, base, region.RegionSize)) {
            return true;
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

static ManualMapResult ManualMapRuntimeCoreWithHandle(
    const ManualMapOptions& options, const AttachableProcess& inspected,
    const Handle& process) noexcept {
    try {
        if (HasPrivateMappedCore(process.Get())) {
            return Failure(
                ManualMapError::AlreadyAttached, ERROR_ALREADY_EXISTS,
                "an externally mapped Anomaly core is already present");
        }

        std::string image_failure;
        auto parsed = ParseImage(options.core_path, image_failure);
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

        void* remote_base = VirtualAllocEx(
            process.Get(), reinterpret_cast<void*>(headers->OptionalHeader.ImageBase),
            parsed->mapped.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote_base == nullptr) {
            remote_base = VirtualAllocEx(
                process.Get(), nullptr, parsed->mapped.size(),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        RemoteAllocation remote_image(process.Get(), remote_base, parsed->mapped.size());
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
        auto modules = SnapshotModules(process.Get(), module_error);
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
                process.Get(), *parsed, modules, image_failure,
                &cxx_throw_override)) {
            return Failure(
                ManualMapError::DependencyFailure, ERROR_MOD_NOT_FOUND,
                std::move(image_failure));
        }
        if (!cxx_throw_override.applied) {
            return Failure(
                ManualMapError::ImageInvalid, ERROR_PROC_NOT_FOUND,
                "core image does not import _CxxThrowException");
        }
        headers = Headers(parsed->mapped);
        if (headers == nullptr || WriteProcessMemory(
                process.Get(), remote_image.address, parsed->mapped.data(),
                parsed->mapped.size(), nullptr) == FALSE) {
            return Failure(
                ManualMapError::WriteFailure, GetLastError(),
                "remote core image could not be written");
        }
        if (!ProtectImage(process.Get(), remote_image.address, parsed->mapped)) {
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
        modules = SnapshotModules(process.Get(), snapshot_error);
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
        RemoteAllocation remote_start_info(
            process.Get(), VirtualAllocEx(
                process.Get(), nullptr, sizeof(start_info),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), sizeof(start_info));
        if (remote_start_info.address == nullptr || WriteProcessMemory(
                process.Get(), remote_start_info.address, &start_info,
                sizeof(start_info), nullptr) == FALSE) {
            return Failure(
                ManualMapError::WriteFailure, GetLastError(),
                "remote AnomalyStartInfo could not be written");
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
            process.Get(), VirtualAllocEx(
                process.Get(), nullptr, sizeof(context),
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), sizeof(context));
        if (remote_context.address == nullptr || WriteProcessMemory(
                process.Get(), remote_context.address, &context, sizeof(context), nullptr) == FALSE) {
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
            process.Get(), VirtualAllocEx(
                process.Get(), nullptr, bootstrap_size,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE), bootstrap_size);
        if (remote_bootstrap.address == nullptr || WriteProcessMemory(
                process.Get(), remote_bootstrap.address, bootstrap_begin,
                bootstrap_size, nullptr) == FALSE) {
            return Failure(
                ManualMapError::WriteFailure, GetLastError(),
                "remote bootstrap code could not be written");
        }
        DWORD previous{};
        if (VirtualProtectEx(
                process.Get(), remote_bootstrap.address, bootstrap_size,
                PAGE_EXECUTE_READ, &previous) == FALSE ||
            FlushInstructionCache(
                process.Get(), remote_bootstrap.address, bootstrap_size) == FALSE) {
            return Failure(
                ManualMapError::ProtectionFailure, GetLastError(),
                "remote bootstrap code could not be made executable");
        }

        Handle thread(CreateRemoteThread(
            process.Get(), nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_bootstrap.address),
            remote_context.address, 0, nullptr));
        if (!thread) {
            return Failure(
                ManualMapError::BootstrapFailure, GetLastError(),
                "remote bootstrap thread could not be created");
        }
        const auto bounded_timeout = std::clamp<std::int64_t>(
            options.timeout.count(), 1, static_cast<std::int64_t>(INFINITE - 1));
        const DWORD wait = WaitForSingleObject(
            thread.Get(), static_cast<DWORD>(bounded_timeout));
        if (wait != WAIT_OBJECT_0) {
            remote_image.retained = true;
            remote_context.retained = true;
            remote_bootstrap.retained = true;
            remote_start_info.retained = true;
            remote_root.retained = true;
            remote_log.retained = true;
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
            process.Get(), remote_context.address, &context, sizeof(context), nullptr) == FALSE) {
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

ManualMapResult ManualMapRuntimeCore(const ManualMapOptions& options) noexcept {
    try {
        if (options.process_id == 0 || options.core_path.empty()) {
            return Failure(
                ManualMapError::ProcessUnavailable, ERROR_INVALID_PARAMETER,
                "manual-map options are incomplete");
        }
        const AttachableProcess inspected = InspectAttachableProcess(options.process_id);
        if (inspected.inspection_error != ERROR_SUCCESS) {
            return Failure(
                inspected.inspection_error == ERROR_ACCESS_DENIED
                    ? ManualMapError::AccessDenied : ManualMapError::ProcessUnavailable,
                inspected.inspection_error,
                inspected.inspection_error == ERROR_ACCESS_DENIED
                    ? "target process denied the access required for attach"
                    : "target process could not be inspected");
        }
        if (!inspected.owned_by_current_user) {
            return Failure(
                ManualMapError::DifferentUser, ERROR_ACCESS_DENIED,
                "target process is owned by a different user");
        }
        if (!inspected.x64) {
            return Failure(
                ManualMapError::IncompatibleArchitecture, ERROR_BAD_EXE_FORMAT,
                "target process is not x64");
        }

        DWORD open_error{};
        Handle process = OpenProcessForAttach(options.process_id, open_error);
        if (!process) {
            return Failure(
                open_error == ERROR_ACCESS_DENIED ? ManualMapError::AccessDenied
                                                  : ManualMapError::ProcessUnavailable,
                open_error, "target process could not be opened for attach");
        }
        return ManualMapRuntimeCoreWithHandle(options, inspected, process);
    } catch (...) {
        return Failure(
            ManualMapError::ProcessUnavailable, ERROR_UNHANDLED_EXCEPTION,
            "manual-map target inspection raised an unexpected exception");
    }
}

static ManualMapLaunchResult LaunchAndManualMapRuntimeCoreOnce(
    const ManualMapLaunchOptions& options) noexcept {
    ManualMapLaunchResult result;
    try {
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

        STARTUPINFOW startup{.cb = sizeof(startup)};
        PROCESS_INFORMATION created{};
        constexpr DWORD incompatible_flags =
            DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | CREATE_SUSPENDED;
        if (CreateProcessW(
                launcher.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                options.creation_flags & ~incompatible_flags,
                nullptr, working_directory.c_str(), &startup, &created) == FALSE) {
            result.mapping = Failure(
                ManualMapError::ProcessLaunchFailure, GetLastError(),
                "official launcher could not be started for target capture");
            return result;
        }

        Handle launcher_process(created.hProcess);
        Handle launcher_thread(created.hThread);

        const std::wstring requested_target = Fold(options.target_executable_name);
        const auto discovery_timeout = std::clamp<std::int64_t>(
            options.target_timeout.count(), 1, std::chrono::minutes(10).count() * 60'000LL);
        const auto discovery_deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(discovery_timeout);
        Handle target_process;
        Handle target_cleanup;
        AttachableProcess target;
        struct PendingTarget final {
            Handle process;
            Handle cleanup;
            AttachableProcess info;
        };
        std::map<DWORD, PendingTarget> pending_targets;
        std::set<DWORD> rejected_targets;
        std::optional<std::chrono::steady_clock::time_point> loader_deadline;
        const auto loader_timeout = std::clamp<std::int64_t>(
            options.loader_timeout.count(), 1,
            std::chrono::minutes(10).count() * 60'000LL);
        ManualMapResult deferred_failure;
        DWORD deferred_failure_process{};
        DWORD last_module_error{ERROR_NOT_READY};
        bool observed_compatible_target{};
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if ((!loader_deadline && now >= discovery_deadline) ||
                (loader_deadline && now >= *loader_deadline)) {
                break;
            }

            DWORD discovery_error{};
            const auto observed_targets = SnapshotProcessIds(
                options.target_executable_name, discovery_error);
            if (discovery_error != ERROR_SUCCESS) {
                result.mapping = Failure(
                    ManualMapError::ProcessControlFailure, discovery_error,
                    "new target processes could not be inspected after launch");
                break;
            }

            for (const DWORD process_id : observed_targets) {
                if (existing_targets.contains(process_id) ||
                    rejected_targets.contains(process_id) ||
                    pending_targets.contains(process_id)) {
                    continue;
                }

                DWORD open_error{};
                Handle candidate = OpenProcessForCapture(process_id, open_error);
                if (!candidate) {
                    if (open_error == ERROR_ACCESS_DENIED) {
                        deferred_failure_process = process_id;
                        deferred_failure = Failure(
                            ManualMapError::AccessDenied, open_error,
                            "new target process could not retain mapping access; use Proxy mode "
                            "if the game protects process handles at creation");
                    }
                    rejected_targets.insert(process_id);
                    continue;
                }

                DWORD inspection_error{};
                const auto path = ProcessPath(candidate.Get(), inspection_error);
                if (inspection_error != ERROR_SUCCESS ||
                    Fold(path.filename().wstring()) != requested_target) {
                    rejected_targets.insert(process_id);
                    continue;
                }

                DWORD cleanup_error{};
                Handle cleanup = OpenProcessWithAccess(
                    process_id, PROCESS_TERMINATE | SYNCHRONIZE, cleanup_error);
                AttachableProcess info;
                info.process_id = process_id;
                info.executable_path = path;
                info.executable_name = path.filename().wstring();
                info.owned_by_current_user = SameUser(candidate.Get(), inspection_error);
                info.x64 = inspection_error == ERROR_SUCCESS &&
                    IsX64Process(candidate.Get(), inspection_error);
                info.inspection_error = inspection_error;
                if (inspection_error != ERROR_SUCCESS ||
                    !info.owned_by_current_user || !info.x64) {
                    deferred_failure_process = process_id;
                    deferred_failure = Failure(
                        !info.owned_by_current_user
                            ? ManualMapError::DifferentUser
                            : ManualMapError::IncompatibleArchitecture,
                        inspection_error != ERROR_SUCCESS ? inspection_error
                            : !info.owned_by_current_user ? ERROR_ACCESS_DENIED
                                                        : ERROR_BAD_EXE_FORMAT,
                        "captured target process is not a compatible x64 process");
                    rejected_targets.insert(process_id);
                    continue;
                }

                observed_compatible_target = true;
                if (!loader_deadline) {
                    loader_deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(loader_timeout);
                }
                pending_targets.emplace(
                    process_id,
                    PendingTarget{
                        std::move(candidate), std::move(cleanup), std::move(info)});
            }

            for (auto candidate = pending_targets.begin();
                 candidate != pending_targets.end();) {
                DWORD exit_code{STILL_ACTIVE};
                if (GetExitCodeProcess(
                        candidate->second.process.Get(), &exit_code) == FALSE ||
                    exit_code != STILL_ACTIVE) {
                    rejected_targets.insert(candidate->first);
                    candidate = pending_targets.erase(candidate);
                    continue;
                }

                DWORD module_error{};
                const auto modules = SnapshotModules(
                    candidate->second.process.Get(), module_error);
                last_module_error = module_error;
                const bool loader_ready = module_error == ERROR_SUCCESS &&
                    modules.contains(Fold(candidate->second.info.executable_name)) &&
                    modules.contains(L"ntdll.dll") &&
                    modules.contains(L"kernel32.dll");
                if (loader_ready) {
                    result.process_id = candidate->first;
                    target_process = std::move(candidate->second.process);
                    target_cleanup = std::move(candidate->second.cleanup);
                    target = std::move(candidate->second.info);
                    pending_targets.erase(candidate);
                    break;
                }

                const bool transient_error =
                    module_error == ERROR_SUCCESS ||
                    module_error == ERROR_NOT_READY ||
                    module_error == ERROR_PARTIAL_COPY ||
                    module_error == ERROR_BAD_LENGTH;
                if (!transient_error) {
                    deferred_failure_process = candidate->first;
                    deferred_failure = Failure(
                        ManualMapError::DependencyFailure, module_error,
                        "captured target module state could not be inspected");
                    rejected_targets.insert(candidate->first);
                    candidate = pending_targets.erase(candidate);
                    continue;
                }
                ++candidate;
            }
            if (target_process) break;
            Sleep(1);
        }

        if (!target_process) {
            if (result.mapping.error != ManualMapError::None) return result;
            if (observed_compatible_target) {
                result.process_id = !pending_targets.empty()
                    ? pending_targets.begin()->first : deferred_failure_process;
                for (auto& [process_id, pending] : pending_targets) {
                    static_cast<void>(process_id);
                    if (pending.cleanup) {
                        static_cast<void>(TerminateProcess(
                            pending.cleanup.Get(), ERROR_PROCESS_ABORTED));
                    }
                }
                for (auto& [process_id, pending] : pending_targets) {
                    static_cast<void>(process_id);
                    if (pending.cleanup) {
                        static_cast<void>(WaitForSingleObject(
                            pending.cleanup.Get(), 5000));
                    }
                }
                result.mapping = Failure(
                    ManualMapError::DependencyFailure,
                    last_module_error == ERROR_SUCCESS
                        ? ERROR_TIMEOUT : last_module_error,
                    "no newly captured target reached loader-ready state before the timeout; "
                    "mapping was not started");
            } else if (deferred_failure.error != ManualMapError::None) {
                result.process_id = deferred_failure_process;
                result.mapping = std::move(deferred_failure);
            } else {
                result.mapping = Failure(
                    ManualMapError::ProcessLaunchFailure, ERROR_TIMEOUT,
                    "official launcher did not create a new target process before the timeout");
            }
            return result;
        }

        ManualMapOptions mapping_options = options.manual_map;
        mapping_options.process_id = target.process_id;
        result.mapping = ManualMapRuntimeCoreWithHandle(
            mapping_options, target, target_process);

        if (!result.mapping.Ok()) {
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

static bool ShouldRetryLoaderCapture(
    const ManualMapLaunchResult& result) noexcept {
    if (result.process_id == 0 ||
        result.mapping.error != ManualMapError::DependencyFailure ||
        result.mapping.remote_image != 0) {
        return false;
    }
    return result.mapping.win32_error == ERROR_TIMEOUT ||
        result.mapping.win32_error == ERROR_NOT_READY ||
        result.mapping.win32_error == ERROR_PARTIAL_COPY ||
        result.mapping.win32_error == ERROR_BAD_LENGTH;
}

ManualMapLaunchResult LaunchAndManualMapRuntimeCore(
    const ManualMapLaunchOptions& options) noexcept {
    auto result = LaunchAndManualMapRuntimeCoreOnce(options);
    if (!ShouldRetryLoaderCapture(result)) return result;
    Sleep(250);
    auto retried = LaunchAndManualMapRuntimeCoreOnce(options);
    if (retried.Ok()) {
        retried.mapping.message.append(" after one loader-ready retry");
    }
    return retried;
}

}  // namespace anomaly::launcher
