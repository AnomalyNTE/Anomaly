// kmap_client.exe -- user-mode brain, ring-0 hands.
//
// Drives AnomalyDriver.sys to map a core DLL into a target process where every
// target-side operation (allocation, write, protect, read, user-APC queue) is
// performed by the driver with temporary kernel handles. This process never
// opens the target: the spawn handles are closed immediately, capture comes
// from the driver's create-process callback, and the final assertion walks this
// process's own handle table to prove no Process handles exist at all.
//
// usage: kmap_client.exe <core.dll> [target.exe]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "anomaly/kernel-mapper/akr_ioctl.h"
#include "anomaly/kernel-mapper/kmap_context.hpp"

#include "anomaly/core_api.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <vector>

extern "C" void AkrApcBootstrapBegin();
extern "C" void AkrApcBootstrapEnd();

namespace {

#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// driver channel

constexpr wchar_t kDriverServiceName[] = L"AnomalyDriver";

// The service registration outlives every client run: whichever client
// created it last left its own driver path behind, and a development tree
// and a launcher package both host a copy of the image. Starting a stale
// registration loads the OTHER driver -- observed live: an old-contract
// image answered every APC-queue IOCTL with STATUS_BUFFER_TOO_SMALL while
// the freshly signed driver sat in the package. Returns the registered
// binary path (with its \??\ prefix) on success.
static bool QueryServiceImagePath(SC_HANDLE service, std::wstring* path) {
    DWORD needed = 0;
    QueryServiceConfigW(service, nullptr, 0, &needed);
    if (needed == 0) return false;
    std::vector<BYTE> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, config, needed, &needed) ||
        config->lpBinaryPathName == nullptr) {
        return false;
    }
    *path = config->lpBinaryPathName;
    return true;
}

static bool ServiceImageMatches(
    const std::wstring& registered, const std::filesystem::path& driver_file) {
    const wchar_t* image = registered.c_str();
    if (wcsncmp(image, L"\\??\\", 4) == 0) image += 4;
    return lstrcmpiW(image, driver_file.c_str()) == 0;
}

struct Driver {
    HANDLE device = INVALID_HANDLE_VALUE;
    // True when this process created or started the service: only then is a
    // stop owed on the way out. A service some other session left running is
    // left exactly as it was found.
    bool owns_service = false;

    bool Open() {
        device = CreateFileW(
            AKR_USER_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        return device != INVALID_HANDLE_VALUE;
    }

    // The signed driver lives in `driver\` next to the launcher, placed there
    // by hand. This client itself sits in the payload's `Anomaly\` subdirectory
    // (AnomalyLauncher.exe lives two levels up), so the search is: launcher
    // directory first, then this client's own directory for layouts where the
    // client and the driver sit together. The service is created on demand and
    // stopped again once the attach finishes (see ShutdownService) -- it never
    // stays running.
    bool EnsureService() {
        std::filesystem::path driver_file;
        const std::filesystem::path module_directory =
            std::filesystem::path(_wpgmptr).parent_path();
        for (const std::filesystem::path& root :
             {module_directory.parent_path(), module_directory}) {
            const std::filesystem::path candidate =
                root / L"driver" / L"AnomalyDriver.sys";
            if (std::filesystem::is_regular_file(candidate)) {
                driver_file = candidate;
                break;
            }
        }
        if (driver_file.empty()) {
            wprintf(L"driver image is missing: %ls (looked next to the "
                    L"launcher and next to this client)\n",
                (module_directory.parent_path() / L"driver" /
                    L"AnomalyDriver.sys")
                    .c_str());
            return false;
        }

        const SC_HANDLE manager = OpenSCManagerW(
            nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (manager == nullptr) {
            wprintf(L"service manager could not be opened (GLE=%lu)\n",
                GetLastError());
            return false;
        }

        SC_HANDLE service = OpenServiceW(
            manager, kDriverServiceName, SERVICE_ALL_ACCESS);
        if (service == nullptr) {
            const std::wstring binary =
                L"\\??\\" + driver_file.wstring();
            service = CreateServiceW(
                manager, kDriverServiceName, kDriverServiceName,
                SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                SERVICE_ERROR_NORMAL, binary.c_str(), nullptr, nullptr, nullptr,
                nullptr, nullptr);
            if (service == nullptr && GetLastError() != ERROR_SERVICE_EXISTS) {
                wprintf(L"driver service could not be created (GLE=%lu)\n",
                    GetLastError());
                CloseServiceHandle(manager);
                return false;
            }
            if (service == nullptr) {
                service = OpenServiceW(
                    manager, kDriverServiceName, SERVICE_ALL_ACCESS);
            }
            owns_service = true;
        } else {
            owns_service = true;
        }

        bool started = false;
        if (service != nullptr) {
            SERVICE_STATUS status{};
            if (QueryServiceStatus(service, &status) &&
                status.dwCurrentState == SERVICE_RUNNING) {
                // Already running: keep it running after the attach.
                owns_service = false;
                started = true;
                std::wstring registered;
                if (QueryServiceImagePath(service, &registered) &&
                    !ServiceImageMatches(registered, driver_file)) {
                    wprintf(
                        L"warning: running service image is %ls, not the "
                        L"driver next to this client\n",
                        registered.c_str());
                }
            } else {
                // Stopped: realign a stale registration with the image this
                // client located before starting it, so the load always comes
                // from this package's driver\ directory.
                std::wstring registered;
                if (QueryServiceImagePath(service, &registered) &&
                    !ServiceImageMatches(registered, driver_file)) {
                    const std::wstring binary =
                        L"\\??\\" + driver_file.wstring();
                    if (ChangeServiceConfigW(
                            service, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                            SERVICE_NO_CHANGE, binary.c_str(), nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr)) {
                        wprintf(
                            L"service image realigned: %ls\n",
                            driver_file.c_str());
                    } else {
                        wprintf(
                            L"warning: service image could not be realigned "
                            L"(GLE=%lu)\n",
                            GetLastError());
                    }
                }
                if (StartServiceW(service, 0, nullptr) ||
                    GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                    // StartServiceW returns immediately; poll until the state
                    // settles so the device exists before Open() retries.
                    for (int attempt = 0; attempt < 100; ++attempt) {
                        if (QueryServiceStatus(service, &status) &&
                            status.dwCurrentState == SERVICE_RUNNING) {
                            started = true;
                            break;
                        }
                        if (status.dwCurrentState == SERVICE_STOPPED) break;
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(50));
                    }
                }
            }
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
        if (!started) {
            wprintf(L"driver service did not start\n");
            return false;
        }
        return true;
    }

    // The stop half of the lifecycle: the driver stays loaded only for the
    // length of one attach. Stopping the service unloads the image, and a
    // queued user APC holds KernelRoutine/RundownRoutine pointers into that
    // image -- stopping with APCs still pending is bugcheck 0xCE (seen live:
    // 165 queued, one claimed, the service stopped immediately). The wait is
    // potentially long (a thread that never goes alertable holds its APC
    // until the 30s bound), so the launcher must not see it: report the
    // result first, then drain on a detached thread while the main process
    // exits. The driver's own unload spin is the safety net if even that
    // thread dies.
    void DrainAndStopService() {
        if (device != INVALID_HANDLE_VALUE) {
            // Force first, then poll: threads that never enter an alertable
            // wait hold their queued APCs indefinitely (a 30s poll alone
            // timed out with 100+ still pending, and the eventual thread-exit
            // rundown crashed the box -- bugcheck 0xCE, twice). One force
            // sweep converts them into immediate delivery/rundown; the poll
            // below then only waits for that propagation.
            static_cast<void>(Control(
                IOCTL_AKR_APC_FORCE_DRAIN, nullptr, 0, nullptr, 0));
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                AkrApcDrainOut drain{};
                DWORD bytes = 0;
                if (DeviceIoControl(device, IOCTL_AKR_APC_DRAIN, nullptr, 0,
                        &drain, sizeof(drain), &bytes, nullptr) &&
                    drain.pending == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
        }
        if (!owns_service) return;
        owns_service = false;

        const SC_HANDLE manager =
            OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (manager == nullptr) return;
        const SC_HANDLE service =
            OpenServiceW(manager, kDriverServiceName, SERVICE_STOP);
        if (service != nullptr) {
            SERVICE_STATUS status{};
            ControlService(service, SERVICE_CONTROL_STOP, &status);
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
    }

    // Synchronous stop for failure paths: a failed attach leaves the caller
    // waiting, and the drain bound is already paid for there. Success paths
    // use BeginServiceShutdown instead so the result surfaces immediately.
    void ShutdownService() { DrainAndStopService(); }

    // Detached drain: the device handle and the owns_service flag move to a
    // thread of the process's own making, so the main thread can report the
    // attach result and exit without waiting out the drain bound. The
    // driver's unload-time spin covers the case where this thread is killed.
    void BeginServiceShutdown() {
        if (device == INVALID_HANDLE_VALUE && !owns_service) return;
        const uintptr_t raw = reinterpret_cast<uintptr_t>(this);
        const HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(
            nullptr, 0,
            [](void* context) -> unsigned {
                reinterpret_cast<Driver*>(context)->DrainAndStopService();
                return 0;
            },
            reinterpret_cast<void*>(raw), 0, nullptr));
        if (thread != nullptr) {
            CloseHandle(thread);
            // Ownership moved to the detached thread.
            device = INVALID_HANDLE_VALUE;
            owns_service = false;
            return;
        }
        // No thread could be spawned: fall back to the synchronous stop.
        DrainAndStopService();
    }

    bool Control(DWORD code, void* in, DWORD in_size, void* out, DWORD out_size) {
        DWORD bytes = 0;
        return DeviceIoControl(
            device, code, in, in_size, out, out_size, &bytes, nullptr) != FALSE;
    }

    // Same call, but returns the driver's NTSTATUS when the IOCTL itself fails;
    // DeviceIoControl only surfaces ERROR_MR_MID_NOT_FOUND-shaped win32 codes.
    bool ControlStatus(
        DWORD code, void* in, DWORD in_size, void* out, DWORD out_size,
        int32_t* status) {
        DWORD bytes = 0;
        if (DeviceIoControl(
                device, code, in, in_size, out, out_size, &bytes, nullptr)) {
            if (status != nullptr) *status = 0;
            return true;
        }
        if (status != nullptr) {
            *status = out != nullptr && bytes >= sizeof(int32_t)
                ? *static_cast<int32_t*>(out) : 0;
        }
        return false;
    }
};

Driver g_driver;
uint32_t g_target_pid = 0;
// Hot attach: the game was already running when the client started. The
// loader there is fully armed (an anti-cheat hooks it), so no loader call is
// ever issued -- not even a probe. A hung LoadLibraryW thread holds the
// loader lock forever and poisons every later loader user in the process.
bool g_hot_attach = false;

// Whole-session deadline. The launcher budgets the client at its attach
// timeout plus slack, but that timeout used to gate ONLY the final bootstrap
// poll: a wedged target (anti-cheat suspending our threads) accumulated a
// 15s timeout per loader preload, DllMain, and TLS registration -- minutes
// past the budget, terminated by the launcher with no result INI. Every
// stage poll now shares this deadline, and the stage messages land in the
// INI instead of a budget kill.
std::chrono::steady_clock::time_point g_deadline{
    std::chrono::steady_clock::time_point::max()};
bool SessionExpired() {
    return std::chrono::steady_clock::now() >= g_deadline;
}
// A stage's own timeout, clamped to whatever session budget remains.
std::chrono::steady_clock::time_point StageDeadline(
    std::chrono::milliseconds stage) {
    const auto local = std::chrono::steady_clock::now() + stage;
    return local < g_deadline ? local : g_deadline;
}

// Every thread id of the target, through the snapshot walk. The user
// APC only delivers when a thread enters an alertable wait, so the queue
// request fans out to all of them and the stub claims the context
// atomically: any one alertable thread is enough, whichever it is.
std::vector<uint32_t> FindThreadIds(uint32_t pid) {
    std::vector<uint32_t> tids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return tids;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == pid) {
                tids.push_back(entry.th32ThreadID);
            }
        } while (Thread32Next(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return tids;
}

// ---------------------------------------------------------------------------
// driver primitive wrappers

bool RemoteAlloc(uint64_t size, uint32_t protect, uint64_t hint, uint64_t* out) {
    AkrAllocIn in{};
    in.pid = g_target_pid;
    in.size = size;
    in.protect = protect;
    in.hint = hint;
    AkrAllocOut result{};
    if (!g_driver.Control(
            IOCTL_AKR_ALLOC, &in, sizeof(in), &result, sizeof(result))) {
        return false;
    }
    *out = result.address;
    return true;
}

bool RemoteWrite(uint64_t address, const void* data, uint32_t size) {
    // The driver copies METHOD_BUFFERED input as one block; chunk large writes
    // so no single IOCTL buffer gets excessive.
    const uint32_t kChunk = 0x10000;
    for (uint32_t offset = 0; offset < size;) {
        const uint32_t now = std::min(kChunk, size - offset);
        std::vector<uint8_t> buffer(sizeof(AkrReadWriteIn) + now);
        AkrReadWriteIn* head = reinterpret_cast<AkrReadWriteIn*>(buffer.data());
        head->pid = g_target_pid;
        head->size = now;
        head->address = address + offset;
        std::memcpy(buffer.data() + sizeof(AkrReadWriteIn),
            static_cast<const uint8_t*>(data) + offset, now);
        if (!g_driver.Control(
                IOCTL_AKR_WRITE, buffer.data(),
                static_cast<DWORD>(buffer.size()), nullptr, 0)) {
            return false;
        }
        offset += now;
    }
    return true;
}

bool RemoteRead(uint64_t address, void* data, uint32_t size) {
    AkrReadWriteIn in{};
    in.pid = g_target_pid;
    in.size = size;
    in.address = address;
    return g_driver.Control(IOCTL_AKR_READ, &in, sizeof(in), data, size);
}

bool RemoteProtect(uint64_t address, uint64_t size, uint32_t protect) {
    AkrProtectIn in{};
    in.pid = g_target_pid;
    in.address = address;
    in.size = size;
    in.protect = protect;
    AkrProtectOut out{};
    return g_driver.Control(
        IOCTL_AKR_PROTECT, &in, sizeof(in), &out, sizeof(out));
}

bool RemoteModules(std::vector<AkrModuleInfo>* out) {
    // The driver fills as many entries as the output buffer holds, so this
    // allocates well past AKR_MAX_MODULES: a real game (engine + anti-cheat)
    // easily exceeds 256 modules, and a truncated list makes modules that
    // ARE loaded look absent -- which sends the mapper down a remote-load
    // path that then cannot find the module it just loaded, either.
    constexpr ULONG kModuleCapacity = 1024;
    std::vector<uint8_t> buffer(
        sizeof(AkrModulesOutHeader) + kModuleCapacity * sizeof(AkrModuleInfo));
    AkrModulesIn in{};
    in.pid = g_target_pid;
    if (!g_driver.Control(
            IOCTL_AKR_MODULES, &in, sizeof(in), buffer.data(),
            static_cast<DWORD>(buffer.size()))) {
        return false;
    }
    const auto* header =
        reinterpret_cast<const AkrModulesOutHeader*>(buffer.data());
    const auto* first = reinterpret_cast<const AkrModuleInfo*>(
        buffer.data() + sizeof(AkrModulesOutHeader));
    out->assign(first, first + header->count);
    if (header->truncated != 0) {
        wprintf(
            L"module list truncated at %lu entries -- raise the capacity\n",
            header->count);
    }
    return true;
}

bool RemoteTerminate() {
    AkrTerminateIn in{};
    in.pid = g_target_pid;
    return g_driver.Control(IOCTL_AKR_TERMINATE, &in, sizeof(in), nullptr, 0);
}

// True while the target is process-frozen: every Fail() exit must thaw the
// game first, or the session leaves it dead-suspended.
bool g_frozen = false;

bool RemoteFreeze() {
    AkrFreezeIn in{};
    in.pid = g_target_pid;
    if (!g_driver.Control(IOCTL_AKR_FREEZE, &in, sizeof(in), nullptr, 0)) {
        return false;
    }
    g_frozen = true;
    return true;
}

bool RemoteResume() {
    AkrFreezeIn in{};
    in.pid = g_target_pid;
    if (!g_driver.Control(IOCTL_AKR_RESUME, &in, sizeof(in), nullptr, 0)) {
        return false;
    }
    g_frozen = false;
    return true;
}

// Queue the user APC on EVERY target thread: it delivers when a thread
// next enters an alertable wait, so the fan-out means any alertable thread
// is enough, and the stub's atomic claim makes exactly one delivery run
// the entry. A thread that exits between the snapshot and the queue is
// normal churn and just drops out of the count.
bool QueueUserApc(uint64_t rip, uint64_t context_address) {
    const std::vector<uint32_t> tids = FindThreadIds(g_target_pid);
    uint32_t queued = 0;
    uint32_t refused = 0;
    for (uint32_t tid : tids) {
        AkrHijackIn in{};
        in.thread_id = tid;
        in.rip = rip;
        in.rcx = context_address;
        AkrHijackOut out{};
        int32_t status = 0;
        if (g_driver.ControlStatus(
                IOCTL_AKR_HIJACK, &in, sizeof(in), &out, sizeof(out),
                &status) &&
            out.status == 0) {
            ++queued;
        } else {
            ++refused;
        }
    }
    wprintf(
        L"apc: queued=%lu refused=%lu of %zu threads\n", queued, refused,
        tids.size());
    return queued != 0;
}

std::wstring Fold(std::wstring_view text) {
    std::wstring result(text);
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

const AkrModuleInfo* FindModule(
    const std::vector<AkrModuleInfo>& modules, std::wstring_view name) {
    const std::wstring folded = Fold(name);
    for (const AkrModuleInfo& module : modules) {
        if (Fold(module.name) == folded) return &module;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// remote export resolution

struct ExportCache {
    uint64_t module_base = 0;
    std::vector<DWORD> names;
    std::vector<WORD> ordinals;
    std::vector<DWORD> functions;
    std::vector<std::string> resolved;
    DWORD export_rva = 0;   // export directory bounds, for forwarder detection
    DWORD export_size = 0;
    DWORD timestamp = 0;    // FileHeader.TimeDateStamp of the remote module
    DWORD ordinal_base = 0; // export ordinal base: index = ordinal - base
};

bool ResolveRemoteExportTable(uint64_t module_base, ExportCache* cache) {
    IMAGE_DOS_HEADER dos{};
    if (!RemoteRead(module_base, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    IMAGE_NT_HEADERS64 headers{};
    if (!RemoteRead(module_base + dos.e_lfanew, &headers, sizeof(headers)) ||
        headers.Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const auto& directory =
        headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0) return false;
    IMAGE_EXPORT_DIRECTORY exports{};
    if (!RemoteRead(
            module_base + directory.VirtualAddress, &exports, sizeof(exports)) ||
        exports.NumberOfNames == 0) {
        return false;
    }
    cache->module_base = module_base;
    cache->export_rva = directory.VirtualAddress;
    cache->export_size = directory.Size;
    cache->timestamp = headers.FileHeader.TimeDateStamp;
    cache->ordinal_base = exports.Base;
    cache->names.resize(exports.NumberOfNames);
    cache->ordinals.resize(exports.NumberOfNames);
    cache->functions.resize(exports.NumberOfFunctions);
    cache->resolved.assign(exports.NumberOfNames, std::string{});
    return RemoteRead(
               module_base + exports.AddressOfNames, cache->names.data(),
               static_cast<uint32_t>(cache->names.size() * sizeof(DWORD))) &&
        RemoteRead(
            module_base + exports.AddressOfNameOrdinals, cache->ordinals.data(),
            static_cast<uint32_t>(cache->ordinals.size() * sizeof(WORD))) &&
        RemoteRead(
            module_base + exports.AddressOfFunctions, cache->functions.data(),
            static_cast<uint32_t>(cache->functions.size() * sizeof(DWORD)));
}

uint64_t RemoteExport(uint64_t module_base, const char* name, ExportCache* cache) {
    if (cache->module_base != module_base &&
        !ResolveRemoteExportTable(module_base, cache)) {
        return 0;
    }
    for (size_t index = 0; index < cache->names.size(); ++index) {
        if (cache->resolved[index].empty()) {
            char buffer[128]{};
            if (!RemoteRead(
                    cache->module_base + cache->names[index], buffer,
                    sizeof(buffer) - 1)) {
                continue;
            }
            cache->resolved[index] = buffer;
        }
        if (cache->resolved[index] == name) {
            const WORD ordinal = cache->ordinals[index];
            if (ordinal < cache->functions.size()) {
                return module_base + cache->functions[ordinal];
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// dependency loading: local resolution, remote load. Mirrors the product
// mapper -- each import is resolved through the local loader (which also
// settles API-set names), the owning module is looked up, and when the target
// does not have that module yet it is loaded there through an APC-queued
// LoadLibraryW call, so no user-mode process ever holds a target handle.

HMODULE LoadLocalDependency(
    const std::filesystem::path& core_directory, const std::string& name) {
    const std::wstring wide(name.begin(), name.end());
    // A copy shipped next to the core wins, exactly like the product mapper.
    std::error_code error;
    const auto adjacent = core_directory / wide;
    if (std::filesystem::is_regular_file(adjacent, error) && !error) {
        if (const HMODULE local = LoadLibraryExW(
                adjacent.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            return local;
        }
    }
    return LoadLibraryExW(wide.c_str(), nullptr, 0);
}

std::filesystem::path LocalModulePath(HMODULE module) {
    wchar_t buffer[1024]{};
    const DWORD length = GetModuleFileNameW(
        module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    return std::filesystem::path(buffer, buffer + length);
}

HMODULE OwningModule(FARPROC procedure) {
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(procedure), &owner) == FALSE) {
        return nullptr;
    }
    return owner;
}

// ---------------------------------------------------------------------------
// local image preparation

std::mt19937_64& Engine() {
    static std::mt19937_64 engine{[] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) | device();
    }()};
    return engine;
}

uint64_t RandomHint() {
    std::uniform_int_distribution<std::uint64_t> distribution(
        0x0001000000000000ULL, 0x0007000000000000ULL);
    return distribution(Engine()) & ~0xFFFFULL;
}

template <typename T>
T* Rva(std::vector<uint8_t>& image, uint64_t rva, size_t count = sizeof(T)) {
    if (rva + count > image.size()) return nullptr;
    return reinterpret_cast<T*>(image.data() + rva);
}

template <typename T>
const T* Rva(
    const std::vector<uint8_t>& image, uint64_t rva, size_t count = sizeof(T)) {
    if (rva + count > image.size()) return nullptr;
    return reinterpret_cast<const T*>(image.data() + rva);
}

bool StretchImage(const std::vector<uint8_t>& file, std::vector<uint8_t>* image) {
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        file.size() < dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64)) {
        return false;
    }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        file.data() + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        (headers->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0) {
        return false;
    }
    image->assign(headers->OptionalHeader.SizeOfImage, 0);
    std::memcpy(image->data(), file.data(), headers->OptionalHeader.SizeOfHeaders);
    const auto* sections = IMAGE_FIRST_SECTION(headers);
    for (WORD index = 0; index < headers->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if (section.SizeOfRawData == 0) continue;
        if (section.VirtualAddress + section.SizeOfRawData > image->size() ||
            section.PointerToRawData + section.SizeOfRawData > file.size()) {
            return false;
        }
        std::memcpy(
            image->data() + section.VirtualAddress,
            file.data() + section.PointerToRawData, section.SizeOfRawData);
    }
    return true;
}

bool RelocateImage(std::vector<uint8_t>& image, uint64_t image_base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image.data() + dos->e_lfanew);
    const int64_t delta = static_cast<int64_t>(image_base) -
        static_cast<int64_t>(headers->OptionalHeader.ImageBase);
    if (delta == 0) return true;
    const auto& directory =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    uint64_t offset = 0;
    while (offset < directory.Size) {
        const auto* block =
            Rva<IMAGE_BASE_RELOCATION>(image, directory.VirtualAddress + offset);
        if (block == nullptr ||
            block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
            return false;
        }
        const WORD count =
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        const WORD* entries = reinterpret_cast<const WORD*>(block + 1);
        for (WORD index = 0; index < count; ++index) {
            const WORD type = entries[index] >> 12;
            const WORD rva = entries[index] & 0xFFF;
            if (type == IMAGE_REL_BASED_DIR64) {
                auto* slot = Rva<uint64_t>(image, block->VirtualAddress + rva);
                if (slot == nullptr) return false;
                *slot = static_cast<uint64_t>(static_cast<int64_t>(*slot) + delta);
            }
        }
        offset += block->SizeOfBlock;
    }
    return true;
}

DWORD SectionProtection(DWORD characteristics) {
    const bool executable = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool writable = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    if (executable) return writable ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (writable) return PAGE_READWRITE;
    return PAGE_READONLY;
}

// Header-derived facts captured before the header scramble: after it the image
// bytes no longer parse, so protection works from this copy.
struct ImageLayout {
    DWORD header_size = 0;
    DWORD exception_rva = 0;
    DWORD exception_size = 0;
    std::vector<IMAGE_SECTION_HEADER> sections;
};

bool ProtectImage(
    uint64_t remote_base, uint64_t image_size, const ImageLayout& layout) {
    if (!RemoteProtect(remote_base, image_size, PAGE_NOACCESS)) return false;
    if (!RemoteProtect(remote_base, layout.header_size, PAGE_READONLY)) {
        return false;
    }
    for (const auto& section : layout.sections) {
        const uint64_t size =
            std::max<uint64_t>(section.Misc.VirtualSize, section.SizeOfRawData);
        if (size == 0) continue;
        if (!RemoteProtect(
                remote_base + section.VirtualAddress, size,
                SectionProtection(section.Characteristics))) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// dependency mapping session

// Manual dependency mapping state. Modules the target already had are read
// from the PEB snapshot; every module the mapper itself placed into the
// target is recorded here by folded name -- manually mapped images are never
// linked into the PEB module list, so this registry IS their remote identity.
// The target's own loader is bypassed entirely: no LoadLibraryW call is ever
// made in the game, which also means no LdrLoadDll hook can observe or block
// a dependency being placed.
//
// DllMain calls are DEFERRED: mapping is pure kernel I/O (alloc/write/
// protect/read -- no target code, safe while the process is frozen), but a
// DllMain runs target code and must never execute against a frozen process,
// where an arbitrary thread may hold any lock mid-acquisition. The queue
// records creation order, which is bottom-up by construction (a dependency
// is always mapped before the module whose import needed it), so replaying
// it front-to-back initializes leaves first.
struct MapSession {
    struct PendingDllMain {
        std::wstring name;
        uint64_t base;
        DWORD entry_rva;
        ImageLayout layout;
        std::vector<std::wstring> dependencies;  // folded names of manual deps
        bool delay_slots_unresolved = false;     // DllMain path touches a null slot
        DWORD tls_rva = 0;  // TLS directory: registration precedes DllMain
        uint32_t size_of_image = 0;
    };

    std::vector<AkrModuleInfo> modules;       // PEB snapshot
    std::map<std::wstring, uint64_t> manual;  // folded name -> remote base
    std::vector<PendingDllMain> pending_dllmains;  // creation order
    uint64_t bootstrap = 0;                   // remote bootstrap stub
    std::filesystem::path core_directory;     // adjacent-copy search dir
    // Target ntdll RaiseException, resolved once: per-module C++ throw
    // thunks call it with the throwing module's base as param[3].
    uint64_t raise_exception = 0;

    // Remote base of a module by folded name: manually mapped first, then the
    // PEB snapshot. 0 = absent.
    uint64_t BaseOf(const std::wstring& folded_name) const {
        const auto found = manual.find(folded_name);
        if (found != manual.end()) return found->second;
        const AkrModuleInfo* module = FindModule(modules, folded_name);
        return module != nullptr ? module->base : 0;
    }

    // Reorders pending DllMains so every manual dependency initializes before
    // its users: a plain creation-order replay runs a module's DllMain while
    // its own dependencies (mapped later via another import chain) are still
    // uninitialized -- wintrust before crypt32 is the observed crash. Edges
    // only connect manual modules; PEB modules are loader-initialized already.
    std::vector<PendingDllMain> DllMainOrder() const {
        std::vector<PendingDllMain> ordered;
        ordered.reserve(pending_dllmains.size());
        std::set<std::wstring> placed;
        std::function<bool(const PendingDllMain&)> place =
            [&](const PendingDllMain& entry) -> bool {
            if (placed.count(entry.name) != 0) return false;  // cycle guard
            placed.insert(entry.name);
            for (const auto& dep : entry.dependencies) {
                for (const auto& candidate : pending_dllmains) {
                    if (candidate.name == dep) place(candidate);
                }
            }
            ordered.push_back(entry);
            return true;
        };
        for (const auto& entry : pending_dllmains) place(entry);
        return ordered;
    }
};

bool ResolveImports(
    std::vector<uint8_t>& image, MapSession& session,
    uint64_t cxx_throw_override, uint64_t image_remote_base,
    std::string* detail);
uint64_t LocalExportRva(const std::vector<uint8_t>& image, const char* name);
bool MapDependencyManually(
    MapSession& session, const std::filesystem::path& path,
    std::string* failure);

// Pre-resolves the delay-load import directory of a mapped image. The file
// leaves every delay IAT slot zeroed; a loader-loaded image patches each slot
// to its __delayLoadHelper2 thunk on first call, which re-enters the loader
// (LoadLibrary inside the target). For a manual map that machinery is absent,
// so a DllMain-time call through an unresolved slot jumps to null. Slots are
// resolved eagerly when their owner is already present or manually mappable;
// a slot whose owner cannot be established stays zero -- delay semantics are
// "on first call", so an uncalled slot is never triggered.
// Returns true when every slot resolved; `unresolved` is set otherwise.
bool ResolveDelayImports(
    std::vector<uint8_t>& image, MapSession& session, bool* unresolved);

// Ensures the module owning `path` exists in the target, manually mapping it
// when missing. API-set names never appear in a module list under their own
// name, so they count as present either way.
bool EnsureRemoteModule(
    MapSession& session, const std::filesystem::path& path,
    std::string* failure) {
    const std::wstring name = Fold(path.filename().wstring());
    if (session.BaseOf(name) != 0) return true;
    if (name.starts_with(L"api-ms-") || name.starts_with(L"ext-ms-")) {
        return true;
    }
    return MapDependencyManually(session, path, failure);
}

// ---------------------------------------------------------------------------
// target-side import resolution

// FileHeader.TimeDateStamp of a locally mapped module; 0 when unreadable.
// The value is a build identity, not a date: equal stamps mean the same
// file bytes, so an RVA measured locally transfers to the remote copy.
DWORD LocalTimestamp(uint64_t module_base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(
        static_cast<uintptr_t>(module_base));
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        static_cast<uintptr_t>(module_base) + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) return 0;
    return headers->FileHeader.TimeDateStamp;
}

// FileHeader.TimeDateStamp of a module inside the target.
DWORD RemoteTimestamp(uint64_t module_base) {
    IMAGE_DOS_HEADER dos{};
    if (!RemoteRead(module_base, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    IMAGE_NT_HEADERS64 headers{};
    if (!RemoteRead(module_base + dos.e_lfanew, &headers, sizeof(headers)) ||
        headers.Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return headers.FileHeader.TimeDateStamp;
}

// Resolves one import against the target's OWN export table at
// `owner_base`. The local loader stays in the loop only to settle which
// module owns the export (API-set schemas, forwarder chains); the final
// address must come from the target's bytes, because the target can carry a
// different file version than the local copy -- a game ships its own
// msvcp140.dll -- and an RVA measured in one version lands mid-instruction
// in the other (observed on the real game: 0xC000001D at a fixed msvcp140
// offset, twice). Forwarded exports ("OtherDll.EntryName" strings inside
// the owner's export directory) are followed through the target's tables.
// Returns 0 when the target does not export it.
uint64_t TargetProcAddress(
    MapSession& session, uint64_t owner_base, const char* import_name,
    WORD import_ordinal, ExportCache* cache, int depth) {
    if (owner_base == 0) return 0;
    if (cache->module_base != owner_base &&
        !ResolveRemoteExportTable(owner_base, cache)) {
        return 0;
    }
    uint64_t address = 0;
    if (import_name != nullptr) {
        address = RemoteExport(owner_base, import_name, cache);
    } else {
        // Binding ordinals are absolute: the function array is indexed by
        // ordinal minus the table's base (shell32's base is 2), and a hole
        // in the ordinal space has a zero RVA.
        if (import_ordinal >= cache->ordinal_base) {
            const DWORD index = import_ordinal - cache->ordinal_base;
            if (index < cache->functions.size() && cache->functions[index] != 0) {
                address = owner_base + cache->functions[index];
            }
        }
    }
    if (address == 0) return 0;
    // A forwarded export points into the owner's own export directory.
    const DWORD rva = static_cast<DWORD>(address - owner_base);
    if (depth < 4 && rva >= cache->export_rva &&
        rva < cache->export_rva + cache->export_size) {
        char forwarder[160]{};
        if (!RemoteRead(address, forwarder, sizeof(forwarder) - 1)) return 0;
        char* dot = std::strchr(forwarder, '.');
        if (dot == nullptr) return 0;
        *dot = '\0';
        const char* entry = dot + 1;
        WORD entry_ordinal = 0;
        const char* entry_name = entry;
        if (entry[0] == '#') {
            entry_ordinal = static_cast<WORD>(
                std::strtoul(entry + 1, nullptr, 10));
            entry_name = nullptr;
        }
        // The forwarder's owner joins the target when missing; an owner
        // that cannot be established leaves the import unresolved.
        const std::string forward_module(forwarder);
        std::wstring folded = Fold(std::wstring(
            forward_module.begin(), forward_module.end()));
        if (session.BaseOf(folded) == 0 &&
            folded.find(L".") == std::wstring::npos) {
            // Forwarder strings name modules without any extension
            // ("NTDLL.RtlSetLastWin32Error", "api-ms-win-core-com-l1-1-0"
            // ...) while PEB and manual registries always carry it.
            folded += L".dll";
        }
        uint64_t forward_base = session.BaseOf(folded);
        if (forward_base == 0) {
            const HMODULE local = LoadLocalDependency(
                session.core_directory, forward_module);
            if (local == nullptr) return 0;
            const std::filesystem::path owner_path = LocalModulePath(local);
            std::string load_failure;
            if (!EnsureRemoteModule(session, owner_path, &load_failure)) {
                return 0;
            }
            // An API-set forwarder names a virtual module
            // ("api-ms-win-core-com-l1-1-0") that no PEB ever lists under
            // its own name: the local load above resolved it to the
            // physical host (combase.dll, kernelbase.dll, ...), and
            // EnsureRemoteModule registered THAT name in the target.
            // Follow the host -- the virtual name never resolves.
            forward_base = session.BaseOf(
                Fold(owner_path.filename().wstring()));
        }
        if (forward_base == 0) return 0;
        return TargetProcAddress(
            session, forward_base, entry_name, entry_ordinal,
            cache, depth + 1);
    }
    return address;
}

// ---------------------------------------------------------------------------
// TLS-bearing dependencies: loader territory

// Loads one module into the target through the target's own loader. This is
// the path for modules with a static TLS directory: their TLS template is
// registered per-thread by the loader (LdrpHandleTlsData) -- a loader-only
// job no manual mapper can replicate. Mapping such a module manually leaves
// _tls_index pointing at another module's slot, and the first TLS access
// reads foreign TLS data as a pointer and crashes. Runs only in the
// pre-freeze phase: it executes target code.
bool RemoteLoadLibrary(
    MapSession& session, const std::filesystem::path& path,
    std::string* failure) {
    wprintf(L"dep: target loader %ls\n", path.c_str());
    const AkrModuleInfo* kernel32 = FindModule(session.modules, L"kernel32.dll");
    if (kernel32 == nullptr) {
        *failure = "target kernel32 was not visible for dependency loading";
        return false;
    }
    ExportCache cache;
    const uint64_t load_library =
        RemoteExport(kernel32->base, "LoadLibraryW", &cache);
    if (load_library == 0) {
        *failure = "remote LoadLibraryW could not be resolved";
        return false;
    }

    const std::wstring wide = path.wstring();
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    uint64_t remote_path = 0;
    if (!RemoteAlloc(bytes, PAGE_READWRITE, 0, &remote_path) ||
        !RemoteWrite(remote_path, wide.c_str(), static_cast<uint32_t>(bytes))) {
        *failure = "remote dependency path could not be written";
        return false;
    }

    KmapContext context{};
    context.entry_point = load_library;
    context.start_info = remote_path;
    uint64_t remote_context = 0;
    if (!RemoteAlloc(sizeof(context), PAGE_READWRITE, 0, &remote_context) ||
        !RemoteWrite(remote_context, &context, sizeof(context))) {
        *failure = "remote dependency load context could not be prepared";
        return false;
    }

    if (!QueueUserApc(session.bootstrap, remote_context)) {
        *failure = "remote dependency load APC could not be queued";
        return false;
    }
    const auto deadline = StageDeadline(std::chrono::seconds(3));
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        KmapContext remote{};
        if (RemoteRead(remote_context, &remote, sizeof(remote)) &&
            remote.done == 2) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!finished) {
        *failure = "remote dependency loader did not complete";
        return false;
    }
    if (!RemoteModules(&session.modules)) {
        *failure = "remote modules could not be refreshed after loading";
        return false;
    }
    if (FindModule(session.modules, path.filename().wstring()) == nullptr) {
        *failure = "remote dependency was not visible after loading: " +
            path.filename().string();
        return false;
    }
    return true;
}

// True when the module carries a static TLS directory: its TLS block is
// registered per-thread by the loader, so it must go through RemoteLoadLibrary.
bool HasStaticTls(HMODULE module) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) return false;
    return headers->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress != 0;
}

// Import-descriptor module names of an already-loaded module, read from its
// in-memory image.
std::vector<std::string> ModuleImportNames(HMODULE module) {
    std::vector<std::string> names;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return names;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) return names;
    const auto& directory =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return names;
    const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress);
    const std::size_t maximum = directory.Size / sizeof(*descriptor);
    for (std::size_t index = 0; index < maximum; ++index, ++descriptor) {
        if (descriptor->Name == 0) break;
        if (descriptor->Name >= headers->OptionalHeader.SizeOfImage) continue;
        names.emplace_back(
            reinterpret_cast<const char*>(base + descriptor->Name));
    }
    return names;
}

// True when the module's own export table contains this entry. A local
// GetProcAddress address is only transplantable into the target (the
// same-file RVA fallback in ResolveImports) when it is one of the owner's
// real exports: an apphelp/EAT hook in THIS process resolves some imports to
// a stub inside a foreign module (observed: a launcher-spawned client
// resolved ole32!CoCreateInstance to apphelp!InsHook_CoCreateInstance), and
// that stub dereferences a shim global that only exists in a process the
// shim engine actually engaged -- the target crashes on the first call.
bool LocalExportExists(HMODULE owner, const char* name, WORD ordinal) {
    const auto* base = reinterpret_cast<const uint8_t*>(owner);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0) return false;
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        base + directory.VirtualAddress);
    if (name != nullptr) {
        if (exports->AddressOfNames == 0) return false;
        const auto* names = reinterpret_cast<const DWORD*>(
            base + exports->AddressOfNames);
        for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
            if (std::strcmp(
                    reinterpret_cast<const char*>(base + names[index]),
                    name) == 0) {
                return true;
            }
        }
        return false;
    }
    if (ordinal < exports->Base) return false;
    const DWORD slot = static_cast<DWORD>(ordinal) - exports->Base;
    return slot < exports->NumberOfFunctions;
}

// Pre-freeze pass over the core's whole local dependency closure: every
// TLS-bearing module is loaded into the target through its own loader now,
// while the process is still running normally. The frozen mapping phase then
// only ever manually maps TLS-free modules, whose identity the session
// registry owns.
bool PreloadTlsDependencies(
    const std::vector<uint8_t>& core_image, MapSession& session,
    std::string* failure) {
    if (g_hot_attach) {
        // Never touch a hot target's loader by default: the game is fully
        // up, an anti-cheat that hooks LdrLoadDll hangs the probe thread
        // inside LoadLibraryW WITH the loader lock held, and every later
        // loader user in the process hangs behind it. TLS modules go down
        // the manual path (LdrpHandleTlsData) instead. Fixture hosts (bare
        // test processes without the OS module set) can opt back in.
        static const bool allow_hot_preload = [] {
            char flag[8]{};
            size_t read = 0;
            getenv_s(&read, flag, sizeof(flag), "AKR_HOT_PRELOAD");
            return read > 0;
        }();
        if (!allow_hot_preload) {
            wprintf(L"tls: loader preload skipped (hot attach)\n");
            return true;
        }
    }
    std::vector<std::string> pending;
    {
        const auto* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(core_image.data());
        const auto* headers =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                core_image.data() + dos->e_lfanew);
        const auto& directory = headers->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress != 0) {
            const auto* descriptor =
                reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                    core_image.data() + directory.VirtualAddress);
            const std::size_t maximum =
                directory.Size / sizeof(*descriptor);
            for (std::size_t index = 0; index < maximum; ++index, ++descriptor) {
                if (descriptor->Name == 0) break;
                pending.emplace_back(
                    reinterpret_cast<const char*>(
                        core_image.data() + descriptor->Name));
            }
        }
    }

    std::set<std::wstring> visited;
    while (!pending.empty()) {
        const std::string name = pending.back();
        pending.pop_back();
        const std::wstring folded = Fold(
            std::wstring(name.begin(), name.end()));
        if (!visited.insert(folded).second) continue;

        const HMODULE local = LoadLocalDependency(session.core_directory, name);
        if (local == nullptr) continue;  // the resolver owns that failure
        for (const auto& dep : ModuleImportNames(local)) {
            pending.push_back(dep);
        }
        if (!HasStaticTls(local)) continue;
        if (FindModule(session.modules, folded) != nullptr) continue;
        // Test escape hatch: forces every TLS module down the manual-map +
        // LdrpHandleTlsData path, simulating an anti-cheat that blocks
        // LdrLoadDll in the target (the real-game condition).
        static const bool tls_forbid_loader = [] {
            char flag[8]{};
            size_t read = 0;
            getenv_s(&read, flag, sizeof(flag), "AKR_TLS_NO_LOADER");
            return read > 0;
        }();
        if (tls_forbid_loader) {
            wprintf(L"tls: loader preload suppressed for %ls (test mode)\n",
                folded.c_str());
            continue;
        }

        std::string load_failure;
        if (!RemoteLoadLibrary(session, LocalModulePath(local), &load_failure)) {
            // A loader crash inside the target leaves the loader lock held
            // by a dead thread: every further loader call in this process
            // then blocks forever. Stop issuing loader calls immediately;
            // remaining TLS modules simply skip preloading.
            wprintf(L"tls: loader preload failed for %ls: %hs\n",
                folded.c_str(), load_failure.c_str());
            *failure = load_failure + " (loader path abandoned: " +
                std::to_string(visited.size()) + " modules visited)";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// manual dependency mapping

// Resolves ntdll!LdrpHandleTlsData's RVA in the LOCAL ntdll (same build as
// the target's -- one machine, one binary). Public symbols are needed: the
// routine is not exported. Returns 0 when unresolvable.
DWORD LdrpHandleTlsDataRva() {
    struct Resolver {
        DWORD rva = 0;
        Resolver() {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            const HANDLE process = GetCurrentProcess();
            if (!SymInitialize(process, nullptr, FALSE)) return;
            // The srv* element requires symsrv.dll beside this exe; plain
            // cache directories are read natively by dbghelp. The debugger
            // cache covers development boxes; exe-adjacent "symbols" covers
            // deployments.
            std::wstring search;
            wchar_t module_path[MAX_PATH]{};
            if (GetModuleFileNameW(
                    nullptr, module_path, MAX_PATH) > 0) {
                std::filesystem::path base(module_path);
                search = (base.parent_path() / L"symbols").wstring();
                search += L";C:\\Program Files (x86)\\Windows Kits\\10\\"
                          L"Debuggers\\x64\\sym";
            }
            wchar_t cache[MAX_PATH]{};
            if (GetTempPathW(MAX_PATH, cache) > 0) {
                if (!search.empty()) search += L";";
                search += std::wstring(cache) + L"akr_symbols;srv*" +
                    std::wstring(cache) + L"akr_symbols*" +
                    L"https://msdl.microsoft.com/download/symbols";
            }
            if (search.empty() || !SymSetSearchPathW(process, search.c_str())) {
                SymCleanup(process);
                return;
            }
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll == nullptr) {
                SymCleanup(process);
                return;
            }
            const DWORD64 base = SymLoadModuleExW(
                process, nullptr, L"ntdll.dll", nullptr,
                reinterpret_cast<DWORD64>(ntdll), 0, nullptr, 0);
            if (base == 0) {
                SymCleanup(process);
                return;
            }
            char buffer[sizeof(SYMBOL_INFO) + 128]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 127;
            if (SymFromName(process, "ntdll!LdrpHandleTlsData", symbol)) {
                rva = static_cast<DWORD>(
                    symbol->Address - reinterpret_cast<DWORD64>(ntdll));
            }
            SymCleanup(process);
        }
    };
    static const Resolver resolver;
    return resolver.rva;
}

// Registers a manually mapped TLS-bearing image with the loader's TLS
// machinery: ntdll!LdrpHandleTlsData(fake_entry) assigns a fresh TLS index,
// writes it into the image's _tls_index, extends every loader-known
// thread's TLS array, and bumps the global count so future threads are
// sized correctly. The fake LDR entry needs DllBase (+0x30), the state
// probe at +0x10C (zero -- must differ from 9), and a name; verified
// locally in test_mapdep with test_tls.dll.
bool RegisterStaticTls(
    MapSession& session, const MapSession::PendingDllMain& pending,
    std::string* failure) {
    static const DWORD tls_handler_rva = LdrpHandleTlsDataRva();
    if (tls_handler_rva == 0) {
        *failure = "LdrpHandleTlsData symbol unresolved (ntdll public "
                   "symbols required)";
        return false;
    }
    const AkrModuleInfo* ntdll = FindModule(session.modules, L"ntdll.dll");
    if (ntdll == nullptr) {
        *failure = "ntdll was not visible";
        return false;
    }
    const uint64_t handler = ntdll->base + tls_handler_rva;

    // Fake LDR_DATA_TABLE_ENTRY: 0x200 bytes, mostly zero.
    uint8_t entry[0x200]{};
    const auto write_qword = [&](size_t offset, uint64_t value) {
        std::memcpy(entry + offset, &value, sizeof(value));
    };
    const auto write_dword = [&](size_t offset, uint32_t value) {
        std::memcpy(entry + offset, &value, sizeof(value));
    };
    // The name lives in its own remote allocation; UNICODE_STRINGs at
    // +0x48 (FullDllName) and +0x58 (BaseDllName).
    wchar_t name_buffer[64]{};
    const std::wstring wide(pending.name.begin(), pending.name.end());
    std::memcpy(name_buffer, wide.c_str(),
        std::min(wide.size(), size_t{60}) * sizeof(wchar_t));
    const USHORT name_length = static_cast<USHORT>(
        wcslen(name_buffer) * sizeof(wchar_t));

    uint64_t remote_name = 0;
    if (!RemoteAlloc(sizeof(name_buffer), PAGE_READWRITE, 0, &remote_name) ||
        !RemoteWrite(remote_name, name_buffer, sizeof(name_buffer))) {
        *failure = "TLS entry name could not be placed";
        return false;
    }
    uint64_t remote_entry = 0;
    if (!RemoteAlloc(sizeof(entry), PAGE_READWRITE, 0, &remote_entry)) {
        *failure = "TLS LDR entry could not be placed";
        return false;
    }
    write_qword(0x30, pending.base);                    // DllBase
    write_qword(0x38, pending.base + pending.entry_rva);  // EntryPoint
    write_dword(0x40, pending.size_of_image);           // SizeOfImage
    write_qword(0x48 + 0x00, remote_name);              // FullDllName.Buffer
    write_dword(0x48 + 0x08, name_length);              // .Length
    write_dword(0x48 + 0x0C, sizeof(name_buffer));      // .MaximumLength
    write_qword(0x58 + 0x00, remote_name);              // BaseDllName.Buffer
    write_dword(0x58 + 0x08, name_length);
    write_dword(0x58 + 0x0C, sizeof(name_buffer));
    // +0x10C state probe stays zero (0 != 9 passes).
    if (!RemoteWrite(remote_entry, entry, sizeof(entry))) {
        *failure = "TLS LDR entry could not be written";
        return false;
    }

    // Trampoline: LdrpHandleTlsData(entry) -> NTSTATUS in eax, recorded by
    // the bootstrap stub as the thread result.
    std::vector<uint8_t> code;
    const auto imm64 = [&](uint64_t value) {
        for (int index = 0; index < 8; ++index) {
            code.push_back(static_cast<uint8_t>(value >> (index * 8)));
        }
    };
    code.insert(code.end(), {0x48, 0x83, 0xEC, 0x28});  // sub rsp, 0x28
    code.insert(code.end(), {0x48, 0xB9});              // mov rcx, entry
    imm64(remote_entry);
    code.insert(code.end(), {0x48, 0xB8});              // mov rax, handler
    imm64(handler);
    code.insert(code.end(), {0xFF, 0xD0});              // call rax
    code.insert(code.end(), {0x48, 0x83, 0xC4, 0x28});  // add rsp, 0x28
    code.insert(code.end(), {0xC3});                    // ret
    uint64_t remote_code = 0;
    if (!RemoteAlloc(code.size(), PAGE_READWRITE, 0, &remote_code) ||
        !RemoteWrite(
            remote_code, code.data(), static_cast<uint32_t>(code.size())) ||
        !RemoteProtect(remote_code, code.size(), PAGE_EXECUTE_READ)) {
        *failure = "TLS registration trampoline could not be placed";
        return false;
    }

    KmapContext context{};
    context.entry_point = remote_code;
    context.start_info = 0;
    uint64_t remote_context = 0;
    if (!RemoteAlloc(sizeof(context), PAGE_READWRITE, 0, &remote_context) ||
        !RemoteWrite(remote_context, &context, sizeof(context))) {
        *failure = "TLS registration context could not be prepared";
        return false;
    }
    if (!QueueUserApc(session.bootstrap, remote_context)) {
        *failure = "TLS registration APC could not be queued";
        return false;
    }
    const auto deadline = StageDeadline(std::chrono::seconds(5));
    while (std::chrono::steady_clock::now() < deadline) {
        KmapContext remote{};
        if (RemoteRead(remote_context, &remote, sizeof(remote)) &&
            remote.done == 2) {
            if (remote.result != 0) {
                char status[32]{};
                std::snprintf(status, sizeof(status), " nt=0x%x",
                    static_cast<unsigned>(remote.result));
                *failure = "LdrpHandleTlsData failed:" + std::string(status);
                return false;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    *failure = "TLS registration did not complete";
    return false;
}

// Runs DllMain(base, DLL_PROCESS_ATTACH, nullptr) in the target through a
// kernel-created thread. The bootstrap stub passes a single RCX argument, so
// a hand-assembled position-independent trampoline carries the other two;
// the trampoline also registers the image's unwind table permanently --
// exception unwinding in a manually mapped DLL needs it, and the bootstrap
// stub's own add/delete pair would unregister it right after the call.
bool RunDllMain(
    MapSession& session, uint64_t remote_base,
    DWORD entry_rva, const ImageLayout& layout, std::string* failure) {
    const AkrModuleInfo* ntdll = FindModule(session.modules, L"ntdll.dll");
    if (ntdll == nullptr) {
        *failure = "ntdll was not visible";
        return false;
    }
    ExportCache cache;
    const uint64_t rtl_add = RemoteExport(
        ntdll->base, "RtlAddFunctionTable", &cache);
    if (rtl_add == 0) {
        *failure = "target ntdll exports were not resolvable";
        return false;
    }
    const uint64_t exception_table =
        layout.exception_rva == 0 ? 0 : remote_base + layout.exception_rva;
    const uint64_t exception_count =
        layout.exception_size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);

    std::vector<uint8_t> code;
    const auto imm64 = [&](uint64_t value) {
        for (int index = 0; index < 8; ++index) {
            code.push_back(static_cast<uint8_t>(value >> (index * 8)));
        }
    };
    const auto mov_imm = [&](std::initializer_list<uint8_t> opcode, uint64_t value) {
        code.insert(code.end(), opcode);
        imm64(value);
    };
    // sub rsp, 0x28 -- entry lands at rsp%16==8 (call pushed a return
    // address); after the sub every nested call sees the ABI alignment.
    code.insert(code.end(), {0x48, 0x83, 0xEC, 0x28});
    // RtlAddFunctionTable(ExceptionTable, Count, Base); harmless FALSE at 0.
    mov_imm({0x48, 0xB9}, exception_table);
    mov_imm({0x48, 0xBA}, exception_count);
    mov_imm({0x49, 0xB8}, remote_base);
    mov_imm({0x48, 0xB8}, rtl_add);
    code.insert(code.end(), {0xFF, 0xD0});
    // DllMain(hinst, DLL_PROCESS_ATTACH, nullptr)
    mov_imm({0x48, 0xB9}, remote_base);
    code.insert(code.end(), {0x48, 0xC7, 0xC2, 0x01, 0x00, 0x00, 0x00});
    code.insert(code.end(), {0x4D, 0x31, 0xC0});
    mov_imm({0x48, 0xB8}, remote_base + entry_rva);
    code.insert(code.end(), {0xFF, 0xD0});
    code.insert(code.end(), {0x48, 0x83, 0xC4, 0x28});
    code.insert(code.end(), {0xC3});

    uint64_t remote_code = 0;
    if (!RemoteAlloc(code.size(), PAGE_READWRITE, 0, &remote_code) ||
        !RemoteWrite(
            remote_code, code.data(), static_cast<uint32_t>(code.size())) ||
        !RemoteProtect(remote_code, code.size(), PAGE_EXECUTE_READ)) {
        *failure = "DllMain trampoline could not be placed";
        return false;
    }

    KmapContext context{};
    context.entry_point = remote_code;
    context.start_info = 0;
    uint64_t remote_context = 0;
    if (!RemoteAlloc(sizeof(context), PAGE_READWRITE, 0, &remote_context) ||
        !RemoteWrite(remote_context, &context, sizeof(context))) {
        *failure = "DllMain context could not be prepared";
        return false;
    }
    if (!QueueUserApc(session.bootstrap, remote_context)) {
        *failure = "DllMain APC could not be queued";
        return false;
    }
    const auto deadline = StageDeadline(std::chrono::seconds(5));
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        KmapContext remote{};
        if (RemoteRead(remote_context, &remote, sizeof(remote)) &&
            remote.done == 2) {
            finished = true;
            if (remote.result == 0) {
                *failure = "DllMain returned FALSE";
                return false;
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!finished) {
        // Distinguish a stuck DllMain from a dead target: an unhandled
        // exception in DllMain tears the process down, and every later probe
        // fails with garbage. Naming the state here turns a confusing
        // cascade into one actionable line.
        HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
            g_target_pid);
        if (probe == nullptr) {
            *failure = "DllMain did not complete (target died during DllMain)";
        } else {
            DWORD code = STILL_ACTIVE;
            GetExitCodeProcess(probe, &code);
            CloseHandle(probe);
            *failure = code == STILL_ACTIVE
                ? "DllMain did not complete (target alive, DllMain stuck)"
                : "DllMain did not complete (target died during DllMain)";
        }
        return false;
    }
    return true;
}

// Emits a per-module _CxxThrowException thunk in the target: it raises the
// MSVC C++ exception with the THROWING module's base as parameter[3], which
// the EH walk needs to decode ThrowInfo/catchable-type RVAs. The stock CRT
// resolves that base with RtlPcToFileHeader, blind to manual maps. This is
// the dependency-side twin of the core's AnomalyManualMapCxxThrow bridge.
// Returns 0 when it cannot be placed (imports then resolve normally).
uint64_t MakeThrowThunk(MapSession& session, uint64_t module_base) {
    if (session.raise_exception == 0) {
        const AkrModuleInfo* ntdll = FindModule(session.modules, L"ntdll.dll");
        if (ntdll == nullptr) return 0;
        ExportCache cache;
        session.raise_exception =
            RemoteExport(ntdll->base, "RaiseException", &cache);
        if (session.raise_exception == 0) return 0;
    }
    std::vector<uint8_t> code;
    const auto imm64 = [&](uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            code.push_back(static_cast<uint8_t>(value >> (byte * 8)));
        }
    };
    const auto imm32 = [&](uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            code.push_back(static_cast<uint8_t>(value >> (byte * 8)));
        }
    };
    // void thunk(void* object /*rcx*/, const void* throw_info /*rdx*/)
    code.insert(code.end(), {0x48, 0x83, 0xEC, 0x48});  // sub rsp, 0x48
    // ULONG_PTR parameters[4] at [rsp+0x20]
    code.insert(code.end(), {0x48, 0x89, 0x4C, 0x24, 0x28});  // [0x28]=object
    code.insert(code.end(), {0x48, 0x89, 0x54, 0x24, 0x30});  // [0x30]=info
    code.insert(code.end(), {0x48, 0xB8});
    imm64(0x19930520);  // MSVC EH magic
    code.insert(code.end(), {0x48, 0x89, 0x44, 0x24, 0x20});  // [0x20]=magic
    code.insert(code.end(), {0x48, 0xB8});
    imm64(module_base);
    code.insert(code.end(), {0x48, 0x89, 0x44, 0x24, 0x38});  // [0x38]=base
    code.insert(code.end(), {0xB9});
    imm32(0xE06D7363);  // mov ecx, CXX_EXCEPTION
    code.insert(code.end(), {0xBA, 0x01, 0x00, 0x00, 0x00});  // mov edx, 1
    code.insert(code.end(), {0x41, 0xB8, 0x04, 0x00, 0x00, 0x00});  // r8d, 4
    code.insert(code.end(), {0x4C, 0x8D, 0x4C, 0x24, 0x20});  // lea r9, params
    code.insert(code.end(), {0x48, 0xB8});
    imm64(session.raise_exception);
    code.insert(code.end(), {0xFF, 0xD0});  // call rax
    code.insert(code.end(), {0xCC});        // RaiseException never returns
    uint64_t remote = 0;
    if (!RemoteAlloc(code.size(), PAGE_READWRITE, 0, &remote) ||
        !RemoteWrite(
            remote, code.data(), static_cast<uint32_t>(code.size())) ||
        !RemoteProtect(remote, code.size(), PAGE_EXECUTE_READ)) {
        return 0;
    }
    return remote;
}

// Maps one dependency into the target with kernel primitives only: disk
// bytes -> sections -> random hint allocation -> relocation -> own imports
// (recursive) -> write -> protect -> DllMain. Nothing runs through the
// target's loader, so loader hooks never see this module, and the image is
// never linked into the PEB list -- its remote identity lives in the
// session's manual registry instead.
bool MapDependencyManually(
    MapSession& session, const std::filesystem::path& path,
    std::string* failure) {
    const std::wstring name = Fold(path.filename().wstring());
    wprintf(L"dep: manual map %ls\n", path.c_str());

    std::ifstream stream(path, std::ios::binary);
    const std::vector<uint8_t> file(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    if (file.empty()) {
        *failure =
            "dependency file could not be read: " + path.filename().string();
        return false;
    }
    std::vector<uint8_t> image;
    if (!StretchImage(file, &image)) {
        *failure =
            "dependency image invalid: " + path.filename().string();
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image.data() + dos->e_lfanew);
    // A static-TLS module is mappable: after the image lands, ntdll's own
    // LdrpHandleTlsData (the loader's TLS registrar) is invoked on a fake
    // LDR entry to assign a fresh index, patch the image's _tls_index, and
    // extend every existing thread's TLS array -- verified locally with
    // test_mapdep + test_tls.dll (DLL_THREAD_ATTACH foreign thread reads
    // the template correctly).
    ImageLayout layout;
    layout.header_size = headers->OptionalHeader.SizeOfHeaders;
    const auto& exception = headers->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    layout.exception_rva = exception.VirtualAddress;
    layout.exception_size = exception.Size;
    const auto* first_section = IMAGE_FIRST_SECTION(headers);
    layout.sections.assign(
        first_section, first_section + headers->FileHeader.NumberOfSections);
    const DWORD entry_rva = headers->OptionalHeader.AddressOfEntryPoint;

    uint64_t remote_base = 0;
    if (!RemoteAlloc(
            image.size(), PAGE_READWRITE, RandomHint(), &remote_base) ||
        remote_base == 0) {
        if (!RemoteAlloc(image.size(), PAGE_READWRITE, 0, &remote_base) ||
            remote_base == 0) {
            *failure = "dependency allocation failed: " +
                path.filename().string();
            return false;
        }
    }
    if (!RelocateImage(image, remote_base)) {
        *failure = "dependency relocations failed: " +
            path.filename().string();
        return false;
    }
    // Registered before its own imports resolve: a circular DLL pair
    // resolves against the base recorded here, and export data is already
    // stretched at this point.
    session.manual[name] = remote_base;
    // The registry diff across import resolution is this module's manual
    // dependency set -- the DllMain topological order edges.
    std::set<std::wstring> before;
    for (const auto& entry : session.manual) before.insert(entry.first);

    std::string detail;
    const uint64_t cxx_thunk = MakeThrowThunk(session, remote_base);
    if (!ResolveImports(
            image, session, cxx_thunk, remote_base, &detail)) {
        session.manual.erase(name);
        *failure = "dependency imports failed: " + detail;
        return false;
    }
    bool delay_unresolved = false;
    ResolveDelayImports(image, session, &delay_unresolved);
    std::vector<std::wstring> dependencies;
    for (const auto& entry : session.manual) {
        if (before.count(entry.first) == 0 && entry.first != name) {
            dependencies.push_back(entry.first);
        }
    }
    // No header scramble here: unlike the core, a dependency's export table
    // must stay parseable in the target -- later import resolution reads it
    // back through RemoteExport.
    if (!RemoteWrite(
            remote_base, image.data(), static_cast<uint32_t>(image.size()))) {
        *failure = "dependency write failed: " + path.filename().string();
        return false;
    }
    if (!ProtectImage(remote_base, image.size(), layout)) {
        *failure = "dependency protect failed: " + path.filename().string();
        return false;
    }
    // DllMain is deferred to the post-thaw replay: it runs target code, and
    // target code must never run against a frozen process.
    const auto& tls_dir = headers->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    session.pending_dllmains.push_back(
        {name, remote_base, entry_rva, std::move(layout),
         std::move(dependencies), delay_unresolved,
         tls_dir.VirtualAddress,
         headers->OptionalHeader.SizeOfImage});
    return true;
}

bool ResolveImports(
    std::vector<uint8_t>& image, MapSession& session,
    uint64_t cxx_throw_override, uint64_t image_remote_base,
    std::string* detail) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image.data() + dos->e_lfanew);
    const auto& directory =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return true;

    // CRT throwers (modules that export or import _CxxThrowException) decode
    // every exception's ThrowInfo RVAs against RtlPcToFileHeader's result --
    // and RtlPcToFileHeader resolves through LdrpModuleBaseAddressIndex,
    // which cannot see a manual map, so an internal CRT throw during DllMain
    // walks catchable types against base 0 (observed: msvcrt dies reading
    // 0x94638). Their RtlPcToFileHeader import is redirected to a stub
    // returning the image's own base, which is exactly what the EH machinery
    // asks it for. Non-throwers keep the real function (stack-walking
    // libraries pass foreign PCs through it).
    const bool crt_thrower = image_remote_base != 0 &&
        (LocalExportRva(image, "_CxxThrowException") != 0 ||
         [&] {
             if (directory.VirtualAddress == 0) return false;
             const std::size_t count =
                 directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
             for (std::size_t i = 0; i < count; ++i) {
                 const auto* descriptor = Rva<IMAGE_IMPORT_DESCRIPTOR>(
                     image, directory.VirtualAddress +
                         static_cast<DWORD>(
                             i * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
                 if (descriptor == nullptr || descriptor->Name == 0) break;
                 const DWORD lookup_rva = descriptor->OriginalFirstThunk != 0
                     ? descriptor->OriginalFirstThunk
                     : descriptor->FirstThunk;
                 for (std::size_t slot = 0;; ++slot) {
                     const auto* lookup = Rva<uint64_t>(
                         image, lookup_rva + slot * 8);
                     if (lookup == nullptr || *lookup == 0) break;
                     if ((*lookup & (1ULL << 63)) != 0) continue;
                     const auto* hint = Rva<IMAGE_IMPORT_BY_NAME>(
                         image, *lookup, 24);
                     if (hint == nullptr) continue;
                     if (std::strncmp(
                             hint->Name, "_CxxThrowException", 18) == 0) {
                         return true;
                     }
                 }
             }
             return false;
         }());

    const std::size_t maximum = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t entry_index = 0; entry_index < maximum; ++entry_index) {
        auto* descriptor = Rva<IMAGE_IMPORT_DESCRIPTOR>(
            image,
            directory.VirtualAddress +
                static_cast<DWORD>(entry_index * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
        if (descriptor == nullptr) return false;
        if (descriptor->Name == 0) return true;  // terminator reached
        char module_name[128]{};
        const char* name_bytes =
            Rva<char>(image, descriptor->Name, sizeof(module_name) - 1);
        if (name_bytes == nullptr) return false;
        std::memcpy(module_name, name_bytes, sizeof(module_name) - 1);

        const HMODULE local = LoadLocalDependency(session.core_directory, module_name);
        if (local == nullptr) {
            const DWORD gle = GetLastError();
            if (detail != nullptr) {
                *detail = std::string("local load of ") + module_name +
                    " failed (GLE=" + std::to_string(gle) + ")";
            }
            wprintf(L"local dependency could not be loaded: %hs (GLE=%lu)\n",
                module_name, gle);
            return false;
        }
        std::string failure;
        if (!EnsureRemoteModule(session, LocalModulePath(local), &failure)) {
            if (detail != nullptr) {
                *detail = std::string("remote load of ") + module_name +
                    " failed: " + failure;
            }
            wprintf(L"dependency could not be loaded into the target: %hs: %hs\n",
                module_name, failure.c_str());
            return false;
        }

        const DWORD lookup_rva = descriptor->OriginalFirstThunk != 0
            ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        ExportCache remote_cache;
        // The descriptor module's own base in the target, when present. Its
        // imports resolve through the target's export tables -- forwarders
        // included -- before this process's loader is ever asked for an
        // address: an apphelp shim propagated from the launching process
        // hooks some EAT entries here (CoCreateInstance -> InsHook_*), and a
        // binding transplanted from a hooked resolution crashes the target
        // on first call. The target's own tables are the only trustworthy
        // source for a target-side binding.
        ExportCache descriptor_cache;
        uint64_t descriptor_remote_base = session.BaseOf(
            Fold(std::wstring(
                module_name, module_name + std::strlen(module_name))));
        if (descriptor_remote_base == 0 &&
            (std::strncmp(module_name, "api-ms-", 7) == 0 ||
             std::strncmp(module_name, "ext-ms-", 7) == 0)) {
            // An API-set descriptor names a virtual module no PEB lists:
            // the local handle's owning file is the physical host that
            // also owns these exports in the target.
            descriptor_remote_base = session.BaseOf(
                Fold(LocalModulePath(local).filename().wstring()));
        }
        for (std::size_t index = 0;; ++index) {
            auto* lookup = Rva<uint64_t>(image, lookup_rva + index * 8);
            auto* iat = Rva<uint64_t>(image, descriptor->FirstThunk + index * 8);
            if (lookup == nullptr || iat == nullptr || *lookup == 0) break;
            uint64_t address = 0;
            const char* import_name = nullptr;
            WORD import_ordinal = 0;
            char import_name_buffer[128]{};
            if ((*lookup & (1ULL << 63)) != 0) {
                import_ordinal = static_cast<WORD>(*lookup & 0xFFFF);
            } else {
                const auto* hint = Rva<IMAGE_IMPORT_BY_NAME>(
                    image, *lookup, sizeof(import_name_buffer));
                if (hint == nullptr) return false;
                std::memcpy(
                    import_name_buffer, hint->Name,
                    sizeof(import_name_buffer) - 1);
                import_name = import_name_buffer;
                // The dynamic CRT's _CxxThrowException is redirected to the
                // image's own exception bridge, exactly like the product mapper.
                if (std::strcmp(import_name, "_CxxThrowException") == 0) {
                    *iat = cxx_throw_override;
                    continue;
                }
                // DisableThreadLibraryCalls always rejects a manual map's
                // base (not a loader module handle), and at least one CRT
                // DLL gates its whole DllMain on it -- msvcp140_atomic_wait's
                // user DllMain is literally `return DisableThreadLibraryCalls(
                // hinst);`. A FALSE there makes _DllMainCRTStartup roll the
                // already-run CRT init back (onexit teardown, static
                // destructors) while the exports stay bound in the core's
                // IAT -- the first contended std::atomic wait then runs on
                // destructed globals. Thread notifications never fire for a
                // manual map anyway, so "already disabled" is the honest
                // answer. Stub: mov eax, 1; ret.
                if (std::strcmp(import_name, "DisableThreadLibraryCalls") == 0) {
                    const std::vector<uint8_t> stub{
                        0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
                    uint64_t remote_stub = 0;
                    if (RemoteAlloc(stub.size(), PAGE_READWRITE, 0,
                                    &remote_stub) &&
                        RemoteWrite(remote_stub, stub.data(),
                                    static_cast<uint32_t>(stub.size())) &&
                        RemoteProtect(
                            remote_stub, stub.size(), PAGE_EXECUTE_READ)) {
                        *iat = remote_stub;
                        continue;
                    }
                    // fall through to the real function on placement failure
                }
                if (crt_thrower &&
                    std::strcmp(import_name, "RtlPcToFileHeader") == 0) {
                    // Stub: mov rax, <image base>; ret
                    std::vector<uint8_t> stub{0x48, 0xB8};
                    for (int byte = 0; byte < 8; ++byte) {
                        stub.push_back(static_cast<uint8_t>(
                            image_remote_base >> (byte * 8)));
                    }
                    stub.push_back(0xC3);
                    uint64_t remote_stub = 0;
                    if (RemoteAlloc(stub.size(), PAGE_READWRITE, 0,
                                    &remote_stub) &&
                        RemoteWrite(remote_stub, stub.data(),
                                    static_cast<uint32_t>(stub.size())) &&
                        RemoteProtect(
                            remote_stub, stub.size(), PAGE_EXECUTE_READ)) {
                        *iat = remote_stub;
                        continue;
                    }
                    // fall through to the real function on placement failure
                }
            }
            // Target-first: the descriptor module is already in the target,
            // so its own export table -- and the forwarder chain it names --
            // settles this import without consulting this process's loader
            // (see descriptor_remote_base above for why that matters).
            if (descriptor_remote_base != 0) {
                const uint64_t direct = TargetProcAddress(
                    session, descriptor_remote_base, import_name,
                    import_ordinal, &descriptor_cache, 0);
                if (direct != 0) {
                    *iat = direct;
                    continue;
                }
            }
            // Local fallback: modules absent from the target (API-set hosts
            // on a bare fixture) still need this process's loader to settle
            // the owning module.
            address = reinterpret_cast<uint64_t>(
                import_name != nullptr
                    ? GetProcAddress(local, import_name)
                    : GetProcAddress(
                          local, reinterpret_cast<const char*>(
                                     static_cast<uintptr_t>(import_ordinal))));
            if (address == 0) {
                if (detail != nullptr) {
                    *detail = std::string("unresolved import in ") + module_name;
                }
                wprintf(L"unresolved import in %hs\n", module_name);
                return false;
            }
            // The procedure may live in a module the descriptor name only
            // forwards to (API sets do this constantly), so the owner is found
            // per procedure and loaded on demand.
            const HMODULE owner = OwningModule(
                reinterpret_cast<FARPROC>(static_cast<uintptr_t>(address)));
            if (owner == nullptr) return false;
            const std::filesystem::path owner_path = LocalModulePath(owner);
            if (!EnsureRemoteModule(session, owner_path, &failure)) {
                if (detail != nullptr) {
                    *detail = std::string("owner module ") +
                        owner_path.filename().string() +
                        " could not be loaded into the target: " + failure;
                }
                wprintf(L"owner module could not be loaded into the target: %hs: %hs\n",
                    owner_path.filename().string().c_str(), failure.c_str());
                return false;
            }
            const uint64_t remote_owner_base =
                session.BaseOf(Fold(owner_path.filename().wstring()));
            if (remote_owner_base == 0) {
                if (detail != nullptr) {
                    *detail = std::string("owner module ") +
                        owner_path.filename().string() +
                        " was not visible in the target after mapping";
                }
                return false;
            }
            // The address comes from the target's own export table: the
            // local copy may be a different file version than the target's
            // (the game ships its own CRT), and an RVA measured in one
            // version lands mid-instruction in the other.
            const uint64_t resolved = TargetProcAddress(
                session, remote_owner_base, import_name, import_ordinal,
                &remote_cache, 0);
            if (resolved != 0) {
                *iat = resolved;
                continue;
            }
            // The target does not export it (renamed forwarder, version
            // skew). An RVA measured locally only transfers when both sides
            // run the same binary -- and when the local address is one of
            // the owner's real exports: a hooked local resolution (see
            // LocalExportExists) must never be transplanted.
            const DWORD remote_stamp = RemoteTimestamp(remote_owner_base);
            if (remote_stamp != 0 &&
                remote_stamp == LocalTimestamp(reinterpret_cast<uint64_t>(owner)) &&
                LocalExportExists(owner, import_name, import_ordinal)) {
                *iat = remote_owner_base +
                    (address - reinterpret_cast<uint64_t>(owner));
                continue;
            }
            if (detail != nullptr) {
                char entry[64]{};
                if (import_name != nullptr) {
                    std::snprintf(entry, sizeof(entry), "%s", import_name);
                } else {
                    std::snprintf(
                        entry, sizeof(entry), "#%u",
                        static_cast<unsigned>(import_ordinal));
                }
                *detail = std::string("export missing from target's ") +
                    owner_path.filename().string() + ": " + entry;
            }
            wprintf(L"export missing from target's %ls: %hs\n",
                owner_path.filename().c_str(),
                import_name != nullptr ? import_name : "#ordinal");
            return false;
        }
    }
    return true;
}

// Pre-resolves every delay-load import slot (see the forward declaration for
// the full rationale).
bool ResolveDelayImports(
    std::vector<uint8_t>& image, MapSession& session, bool* unresolved) {
    *unresolved = false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image.data() + dos->e_lfanew);
    const auto& directory = headers->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (directory.VirtualAddress == 0) return true;

    const std::size_t maximum =
        directory.Size / sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
    for (std::size_t index = 0; index < maximum; ++index) {
        auto* descriptor = Rva<IMAGE_DELAYLOAD_DESCRIPTOR>(
            image, directory.VirtualAddress +
                static_cast<DWORD>(index * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)));
        if (descriptor == nullptr || descriptor->DllNameRVA == 0) break;
        char module_name[128]{};
        const char* name_bytes =
            Rva<char>(image, descriptor->DllNameRVA, sizeof(module_name) - 1);
        if (name_bytes == nullptr) continue;
        std::memcpy(module_name, name_bytes, sizeof(module_name) - 1);

        const HMODULE local =
            LoadLocalDependency(session.core_directory, module_name);
        if (local == nullptr) {
            *unresolved = true;  // stays zero: resolve-on-demand
            continue;
        }

        const uint64_t* name_table = Rva<uint64_t>(
            image, descriptor->ImportNameTableRVA);
        uint64_t* iat = Rva<uint64_t>(
            image, descriptor->ImportAddressTableRVA);
        if (name_table == nullptr || iat == nullptr) continue;
        ExportCache remote_cache;
        for (std::size_t slot = 0;; ++slot) {
            if (name_table[slot] == 0) break;
            uint64_t address = 0;
            const char* import_name = nullptr;
            WORD import_ordinal = 0;
            char import_name_buffer[128]{};
            if ((name_table[slot] & (1ULL << 63)) != 0) {
                import_ordinal =
                    static_cast<WORD>(name_table[slot] & 0xFFFF);
                address = reinterpret_cast<uint64_t>(GetProcAddress(
                    local, reinterpret_cast<const char*>(
                               static_cast<uintptr_t>(import_ordinal))));
            } else {
                const auto* hint = Rva<IMAGE_IMPORT_BY_NAME>(
                    image, name_table[slot], 8);
                if (hint == nullptr) {
                    *unresolved = true;
                    continue;
                }
                std::memcpy(
                    import_name_buffer, hint->Name,
                    sizeof(import_name_buffer) - 1);
                import_name = import_name_buffer;
                address = reinterpret_cast<uint64_t>(
                    GetProcAddress(local, import_name));
            }
            if (address == 0) {
                *unresolved = true;  // unresolvable slot stays zero
                continue;
            }
            const HMODULE owner = OwningModule(
                reinterpret_cast<FARPROC>(static_cast<uintptr_t>(address)));
            if (owner == nullptr) {
                *unresolved = true;
                continue;
            }
            // Delay imports carry "on first call" semantics: a slot whose
            // owner is not already in the target stays zero rather than
            // pulling the owner (and its own closure) in -- eagerly mapping
            // delay owners dragged ~100 OS DllMains into the replay and one
            // of them (wintrust) takes the whole process down. The core's
            // real dependencies are normal imports and are mapped normally.
            const std::filesystem::path owner_path = LocalModulePath(owner);
            const uint64_t remote_owner_base =
                session.BaseOf(Fold(owner_path.filename().wstring()));
            if (remote_owner_base == 0) {
                *unresolved = true;
                continue;
            }
            // Same rule as normal imports: the address comes from the
            // target's own export table, because the target may run a
            // different file version of the owner than the local copy.
            const uint64_t resolved = TargetProcAddress(
                session, remote_owner_base, import_name, import_ordinal,
                &remote_cache, 0);
            if (resolved != 0) {
                iat[slot] = resolved;
                continue;
            }
            // A local RVA only transfers on the same binary AND when it is
            // one of the owner's real exports -- never a hooked resolution
            // (see LocalExportExists in ResolveImports).
            const DWORD remote_stamp = RemoteTimestamp(remote_owner_base);
            if (remote_stamp != 0 &&
                remote_stamp == LocalTimestamp(reinterpret_cast<uint64_t>(owner)) &&
                LocalExportExists(owner, import_name, import_ordinal)) {
                iat[slot] = remote_owner_base +
                    (address - reinterpret_cast<uint64_t>(owner));
                continue;
            }
            *unresolved = true;  // stays zero: resolve-on-demand
        }
    }
    return true;
}

uint64_t LocalExportRva(const std::vector<uint8_t>& image, const char* name) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image.data() + dos->e_lfanew);
    const auto& directory =
        headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto* exports =
        Rva<IMAGE_EXPORT_DIRECTORY>(image, directory.VirtualAddress);
    if (exports == nullptr) return 0;
    const DWORD* names = Rva<DWORD>(
        image, exports->AddressOfNames,
        exports->NumberOfNames * sizeof(DWORD));
    const WORD* ordinals = Rva<WORD>(
        image, exports->AddressOfNameOrdinals,
        exports->NumberOfNames * sizeof(WORD));
    const DWORD* functions = Rva<DWORD>(
        image, exports->AddressOfFunctions,
        exports->NumberOfFunctions * sizeof(DWORD));
    if (names == nullptr || ordinals == nullptr || functions == nullptr) return 0;
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        char buffer[128]{};
        const char* bytes = Rva<char>(image, names[index], sizeof(buffer) - 1);
        if (bytes == nullptr) continue;
        std::memcpy(buffer, bytes, sizeof(buffer) - 1);
        if (std::strcmp(buffer, name) == 0) {
            return functions[ordinals[index]];
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// attribution proof: no Process handles in this process's table

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* NtQuerySystemInformationFn)(ULONG, void*, ULONG, ULONG*);

bool AssertNoProcessHandles() {
    const auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (query == nullptr) return false;
    struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
        PVOID Object;
        ULONG_PTR ProcessId;
        ULONG_PTR Handle;
        ULONG GrantedAccess;
        USHORT CreatorBackTraceIndex;
        USHORT ObjectTypeIndex;
        ULONG HandleAttributes;
        ULONG Reserved;
    };
    struct HANDLE_LIST {
        ULONG_PTR Count;
        ULONG_PTR Reserved;  // kernel layout: entries begin after Count+Reserved
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
    };
    ULONG size = 1 << 20;
    std::vector<uint8_t> buffer(size);
    // SystemExtendedHandleInformation (0x40): matches the EX entry layout
    // above. The old class (0x10) returned 24-byte entries, so the 40-byte
    // stride walked off the buffer and crashed the client on handle-heavy
    // systems (the real game with its anti-cheat).
    while (query(0x40 /* SystemExtendedHandleInformation */, buffer.data(), size,
                  nullptr) == 0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */) {
        size *= 2;
        buffer.assign(size, 0);
    }
    const auto* list = reinterpret_cast<const HANDLE_LIST*>(buffer.data());
    const ULONG_PTR self = GetCurrentProcessId();
    const ULONG_PTR target = g_target_pid;
    int process_handles = 0;
    int target_handles = 0;
    for (ULONG_PTR index = 0; index < list->Count; ++index) {
        const auto& entry = list->Handles[index];
        if (entry.ProcessId != self) continue;
        HANDLE duplicated = nullptr;
        if (DuplicateHandle(
                GetCurrentProcess(),
                reinterpret_cast<HANDLE>(entry.Handle), GetCurrentProcess(),
                &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE) {
            continue;
        }
        using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
        static const auto object_query = reinterpret_cast<NtQueryObjectFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
        struct {
            ULONG_PTR reserved[2];
            UNICODE_STRING type_name;
        } object_type{};
        if (object_query != nullptr &&
            object_query(
                duplicated, 2 /* ObjectTypeInformation */, &object_type,
                sizeof(object_type), nullptr) == 0 &&
            object_type.type_name.Buffer != nullptr) {
            const std::wstring type(
                object_type.type_name.Buffer,
                object_type.type_name.Length / sizeof(wchar_t));
            if (type == L"Process") {
                ++process_handles;
                PROCESS_BASIC_INFORMATION basic{};
                using NtQueryInformationProcessFn =
                    LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
                const auto process_query =
                    reinterpret_cast<NtQueryInformationProcessFn>(GetProcAddress(
                        GetModuleHandleW(L"ntdll.dll"),
                        "NtQueryInformationProcess"));
                if (process_query != nullptr &&
                    process_query(
                        duplicated, 0, &basic, sizeof(basic), nullptr) == 0 &&
                    static_cast<ULONG_PTR>(basic.UniqueProcessId) == target) {
                    ++target_handles;
                }
            }
        }
        CloseHandle(duplicated);
    }
    wprintf(
        L"attribution: this process holds %d process handles, %d of them "
        L"pointing at the target\n",
        process_handles, target_handles);
    return process_handles == 0 && target_handles == 0;
}

// ---------------------------------------------------------------------------
// launcher contract

// Set in contract mode: the launcher supervises this process and reads its
// outcome from this INI instead of stdout.
std::filesystem::path g_result_path;
bool g_contract_mode = false;

// Mirrors the product ManualMapError ordering (manual_map.hpp).
enum class MapError : uint32_t {
    None = 0,
    ProcessLaunchFailure = 1,
    ProcessControlFailure = 2,
    ProcessUnavailable = 3,
    AccessDenied = 4,
    DifferentUser = 5,
    IncompatibleArchitecture = 6,
    ImageUnavailable = 7,
    ImageInvalid = 8,
    AlreadyAttached = 9,
    DependencyFailure = 10,
    AllocationFailure = 11,
    WriteFailure = 12,
    ProtectionFailure = 13,
    BootstrapFailure = 14,
    Timeout = 15,
};

void WriteResultIni(
    uint32_t error, uint32_t win32_code, uint32_t pid, uint64_t image,
    uint32_t runtime_start, const wchar_t* message) {
    if (g_result_path.empty()) return;
    const std::wstring ini = g_result_path.wstring();
    const std::wstring section = L"result";
    auto put = [&](const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(
            section.c_str(), key, value.c_str(), ini.c_str());
    };
    put(L"error", std::to_wstring(error));
    put(L"win32", std::to_wstring(win32_code));
    put(L"pid", std::to_wstring(pid));
    put(L"image", std::to_wstring(image));
    put(L"runtime_start", std::to_wstring(runtime_start));
    put(L"message", message != nullptr ? message : L"");
}

void Fail(
    const wchar_t* message, MapError map_error = MapError::BootstrapFailure,
    uint32_t win32_code = 0) {
    const DWORD gle = win32_code != 0 ? win32_code : GetLastError();
    wprintf(L"FAIL: %ls (GLE=%lu)\n", message, gle);
    // A frozen target must never be left behind: this is the only exit path
    // after the freeze window opens.
    if (g_frozen && !RemoteResume()) {
        wprintf(L"FAIL: target could not be resumed (still frozen!)\n");
    }
    if (g_contract_mode) {
        WriteResultIni(
            static_cast<uint32_t>(map_error), gle, g_target_pid, 0, 0, message);
    }
    // The driver service runs only for the length of one attach. A failed
    // attach has the launcher waiting, so the drain (force sweep + short
    // poll) happens synchronously here.
    g_driver.ShutdownService();
    ExitProcess(1);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Three shapes:
    //   fixture:   kmap_client.exe <core.dll> [target.exe]
    //              spawns the target itself and checks the marker file;
    //   contract:  kmap_client.exe --core <p> --target <name> --runtime-root <p>
    //              --log-directory <p> --capture-timeout <ms> --timeout <ms>
    //              [--armed-timeout <ms>] --result <ini>
    //              the launcher already started the game; capture only, and
    //              report the outcome through the result INI;
    //   hot:       contract shape plus --pid <n>: attach to a process that is
    //              already running -- no launcher, no create callback, the
    //              driver adopts the pid directly.
    std::filesystem::path core_path;
    std::wstring target_name = L"FakeGame.exe";
    std::filesystem::path runtime_root;
    std::filesystem::path log_directory;
    std::chrono::milliseconds capture_timeout{std::chrono::seconds(10)};
    std::chrono::milliseconds attach_timeout{std::chrono::seconds(30)};
    bool spawn_target = true;
    uint32_t hot_pid = 0;

    if (argc >= 2 && wcscmp(argv[1], L"--core") == 0) {
        g_contract_mode = true;
        spawn_target = false;
        for (int index = 1; index < argc; ++index) {
            const std::wstring flag = argv[index];
            const auto value = [&]() -> std::wstring {
                return index + 1 < argc ? argv[index + 1] : L"";
            };
            if (flag == L"--core") {
                core_path = std::filesystem::absolute(value());
                ++index;
            } else if (flag == L"--target") {
                target_name = value();
                ++index;
            } else if (flag == L"--pid") {
                hot_pid = static_cast<uint32_t>(
                    std::wcstoul(value().c_str(), nullptr, 10));
                ++index;
            } else if (flag == L"--runtime-root") {
                runtime_root = std::filesystem::absolute(value());
                ++index;
            } else if (flag == L"--log-directory") {
                // GUI launches have no console: mirror every progress line
                // to a log file there, or a real-game session leaves no
                // trail at all.
                log_directory = std::filesystem::absolute(value());
                ++index;
            } else if (flag == L"--capture-timeout") {
                capture_timeout = std::chrono::milliseconds(std::wcstol(value().c_str(), nullptr, 10));
                ++index;
            } else if (flag == L"--timeout") {
                attach_timeout = std::chrono::milliseconds(std::wcstol(value().c_str(), nullptr, 10));
                ++index;
            } else if (flag == L"--armed-timeout") {
                ++index;  // hook arming wait; folded into the capture budget
            } else if (flag == L"--result") {
                g_result_path = value();
                ++index;
            }
        }
        if (core_path.empty() || g_result_path.empty()) {
            wprintf(L"usage: kmap_client.exe --core <p> --target <name> "
                    L"--runtime-root <p> --log-directory <p> "
                    L"--capture-timeout <ms> --timeout <ms> --result <ini>\n");
            return 2;
        }
    } else {
        if (argc < 2) {
            wprintf(L"usage: kmap_client.exe <core.dll> [target.exe]\n");
            return 2;
        }
        core_path = std::filesystem::absolute(argv[1]);
        target_name = argc >= 3 ? argv[2] : L"FakeGame.exe";
    }
    if (runtime_root.empty()) runtime_root = std::filesystem::current_path();

    // The whole client must fit inside the attach budget: the launcher
    // terminates it at attach_ms + slack, and every stage poll below shares
    // this deadline so the result INI names the stuck stage instead of the
    // process dying to a budget kill.
    g_deadline = std::chrono::steady_clock::now() + attach_timeout;

    if (!log_directory.empty()) {
        std::error_code made{};
        std::filesystem::create_directories(log_directory, made);
        const std::filesystem::path log_file = log_directory /
            (L"Anomaly.Kmap.Client." +
             std::to_wstring(GetCurrentProcessId()) + L".log");
        if (_wfreopen(log_file.c_str(), L"w,ccs=UTF-8", stdout) != nullptr) {
            wprintf(L"kmap client log: %ls\n", log_file.c_str());
        }
    }

    // The service lifecycle belongs to this one attach: start (or adopt) the
    // driver service, open the device, and stop what we started on the way
    // out -- a service some other session left running is not ours to stop.
    if (!g_driver.Open() && !g_driver.EnsureService()) {
        Fail(L"driver service could not be started", MapError::AccessDenied);
    }
    if (g_driver.device == INVALID_HANDLE_VALUE && !g_driver.Open()) {
        g_driver.ShutdownService();
        Fail(L"driver device could not be opened", MapError::AccessDenied);
    }

    if (hot_pid != 0) {
        // Hot attach: the target is already running. The driver adopts the
        // pid directly; no create callback, no launcher, no arming race.
        AkrSetTargetIn in{};
        in.pid = hot_pid;
        if (!g_driver.Control(IOCTL_AKR_SET_TARGET, &in, sizeof(in), nullptr, 0)) {
            Fail(L"target pid could not be adopted", MapError::ProcessUnavailable);
        }
        g_target_pid = hot_pid;
        g_hot_attach = true;
        wprintf(L"hot attach: pid=%lu (ring-0 direct)\n", g_target_pid);
    } else {
        // Arm capture before the target exists anywhere.
        AkrArmCaptureIn arm{};
        arm.name_length = static_cast<uint32_t>(target_name.size());
        std::memcpy(
            arm.name, target_name.c_str(), arm.name_length * sizeof(wchar_t));
        if (!g_driver.Control(
                IOCTL_AKR_ARM_CAPTURE, &arm, sizeof(arm), nullptr, 0)) {
            Fail(
                L"capture could not be armed",
                MapError::ProcessControlFailure);
        }

        // Fixture mode spawns the target itself; drop the spawn handles
        // immediately -- this process must never hold a handle to it. Contract
        // mode never spawns: the launcher already started the game.
        if (spawn_target) {
            STARTUPINFOW startup{.cb = sizeof(startup)};
            PROCESS_INFORMATION created{};
            const std::wstring command =
                L"\"" + (std::filesystem::current_path() / target_name).wstring() +
                L"\"";
            std::vector<wchar_t> mutable_command(command.begin(), command.end());
            mutable_command.push_back(L'\0');
            if (CreateProcessW(
                    nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0,
                    nullptr, nullptr, &startup, &created) == FALSE) {
                Fail(
                    L"target could not be started",
                    MapError::ProcessLaunchFailure);
            }
            CloseHandle(created.hProcess);
            CloseHandle(created.hThread);
            wprintf(L"target spawned\n");
        }

        // Capture from the driver's create-process callback.
        {
            const auto deadline = StageDeadline(capture_timeout);
            while (std::chrono::steady_clock::now() < deadline) {
                AkrProcessEvent event{};
                if (g_driver.Control(
                        IOCTL_AKR_GET_EVENT, nullptr, 0, &event, sizeof(event)) &&
                    event.pid != 0 && event.is_new) {
                    g_target_pid = event.pid;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (g_target_pid == 0) {
                Fail(
                    L"driver never reported the target creation",
                    MapError::Timeout, WAIT_TIMEOUT);
            }
            wprintf(L"capture: pid=%lu (ring-0 callback)\n", g_target_pid);
        }
    }

    // Load and stretch the core image locally.
    std::vector<uint8_t> file_data;
    {
        std::ifstream stream(core_path, std::ios::binary | std::ios::ate);
        if (!stream) {
            Fail(L"core file could not be read", MapError::ImageUnavailable);
        }
        const std::streamsize size = stream.tellg();
        stream.seekg(0);
        file_data.resize(static_cast<size_t>(size));
        stream.read(reinterpret_cast<char*>(file_data.data()), size);
    }
    std::vector<uint8_t> image;
    if (!StretchImage(file_data, &image)) {
        Fail(L"core image failed to parse", MapError::ImageInvalid);
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image.data() + dos->e_lfanew);

    const uint64_t start_rva = LocalExportRva(image, ANOMALY_CORE_START_ENTRY);
    const uint64_t cxx_throw_rva =
        LocalExportRva(image, ANOMALY_CORE_MANUAL_MAP_CXX_THROW_ENTRY);
    if (start_rva == 0 || cxx_throw_rva == 0) {
        Fail(
            L"core does not export the manual-map contract",
            MapError::ImageInvalid);
    }

    // Wait for the loader to settle far enough that kernel32 is visible: the
    // core's remaining dependencies are mapped on demand during import
    // resolution, each with kernel primitives only -- the target's own
    // loader is never called again from here on.
    MapSession session;
    session.core_directory = core_path.parent_path();
    {
        const auto deadline = StageDeadline(std::chrono::seconds(30));
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (RemoteModules(&session.modules) &&
                FindModule(session.modules, L"kernel32.dll") != nullptr) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ready) {
            Fail(
                L"target loader did not settle before the timeout",
                MapError::Timeout, WAIT_TIMEOUT);
        }
        wprintf(L"loader ready: %zu modules\n", session.modules.size());
    }

    // Bootstrap code: position-independent range copied into the target. The
    // special-user-APC shape is the only one used now: the stub is queued onto
    // an EXISTING thread (ZwQueueApcThreadEx, special flavor); the kernel
    // dispatches it with the thread's full context saved, and the stub's ret
    // returns through KiUserApcDispatcher -> NtContinue, restoring that
    // context exactly. No new thread is ever created -- every fresh user
    // thread must first run ntdll!LdrInitializeThunk, whose entry ACE
    // inline-hooks (observed: every kernel-created thread hung at birth) --
    // and no thread context is captured or patched by us at all.
    uint64_t remote_bootstrap = 0;
    {
        const auto* bootstrap_begin =
            reinterpret_cast<const uint8_t*>(&AkrApcBootstrapBegin);
        const auto* bootstrap_end =
            reinterpret_cast<const uint8_t*>(&AkrApcBootstrapEnd);
        const size_t bootstrap_size = bootstrap_end - bootstrap_begin;
        if (bootstrap_size == 0 || bootstrap_size > 4096) {
            Fail(L"bootstrap code range is invalid", MapError::ImageInvalid);
        }
        if (!RemoteAlloc(bootstrap_size, PAGE_READWRITE, 0, &remote_bootstrap) ||
            !RemoteWrite(
                remote_bootstrap, bootstrap_begin,
                static_cast<uint32_t>(bootstrap_size)) ||
            !RemoteProtect(remote_bootstrap, bootstrap_size, PAGE_EXECUTE_READ)) {
            Fail(
                L"remote bootstrap failed", MapError::AllocationFailure);
        }
        session.bootstrap = remote_bootstrap;
    }

    // TLS-bearing dependencies go through the target's own loader NOW, while
    // the process runs normally: the loader registers their per-thread TLS
    // templates, which no manual mapper can replicate. The frozen phase below
    // then only manually maps TLS-free modules. A preload failure is a
    // warning, not fatal: the target keeps running, and a module whose TLS
    // preload failed simply cannot be manual-mapped later (refused by the
    // safety net) -- its importer's slots against it resolve only if the
    // module is present some other way. Real games pre-load these OS
    // modules; a bare test host may not, and one loader hiccup must not
    // take the whole attach down.
    {
        std::string preload_failure;
        if (!PreloadTlsDependencies(image, session, &preload_failure)) {
            wprintf(L"TLS dependency preload warning: %hs\n",
                preload_failure.c_str());
        }
    }

    // Freeze the target: PsSuspendProcess parks every existing thread --
    // anti-cheat watchdogs included -- for the whole mapping window. Threads
    // the driver creates afterwards still run, and nothing below touches the
    // target's loader, so no parked thread is ever needed. Every Fail() exit
    // resumes the target first.
    if (!RemoteFreeze()) {
        Fail(L"target could not be frozen", MapError::ProcessControlFailure);
    }
    wprintf(L"target frozen: pid=%lu\n", g_target_pid);

    // Allocate at a randomized hint -- the same per-session fingerprint
    // randomization as the user-mode mapper. The hint range mirrors the product
    // mapper, and an unusable hint falls back to a system-chosen address there;
    // the kernel path mirrors the fallback too.
    uint64_t remote_base = 0;
    if (!RemoteAlloc(image.size(), PAGE_READWRITE, RandomHint(), &remote_base) ||
        remote_base == 0) {
        if (!RemoteAlloc(image.size(), PAGE_READWRITE, 0, &remote_base) ||
            remote_base == 0) {
            Fail(L"remote image allocation failed", MapError::AllocationFailure);
        }
    }
    wprintf(L"image: 0x%llx (random hint)\n", remote_base);

    if (!RelocateImage(image, remote_base)) {
        Fail(L"relocations failed", MapError::ImageInvalid);
    }
    {
        std::string import_detail;
        if (!ResolveImports(
                image, session, remote_base + cxx_throw_rva, remote_base,
                &import_detail)) {
            const std::wstring detail(import_detail.begin(), import_detail.end());
            Fail(
                (L"import resolution failed: " + detail).c_str(),
                MapError::DependencyFailure);
        }
        bool delay_unresolved = false;
        ResolveDelayImports(image, session, &delay_unresolved);
        if (delay_unresolved) {
            wprintf(L"core: some delay slots left zero (owner absent)\n");
        }
    }

    // Capture every header-derived fact BEFORE the scramble: afterwards the
    // image bytes no longer parse (that is the point), and re-reading them
    // yields wild pointers. The product mapper parses once up front for the
    // same reason.
    ImageLayout layout;
    layout.header_size = headers->OptionalHeader.SizeOfHeaders;
    const auto& exception = headers->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    layout.exception_rva = exception.VirtualAddress;
    layout.exception_size = exception.Size;
    const auto* first_section = IMAGE_FIRST_SECTION(headers);
    layout.sections.assign(
        first_section, first_section + headers->FileHeader.NumberOfSections);
    // The core's DllMain must replay in the target before AnomalyStart: the
    // CRT's dynamic initializers (g_runtime_control and every other global
    // object) run inside _CRT_INIT there. Without it AnomalyStart sees a null
    // runtime control block and fails with ERROR_INVALID_STATE (5023) while
    // the client kept reporting success -- the runtime_start result was never
    // checked against the contract.
    const uint64_t core_dllmain_rva =
        headers->OptionalHeader.AddressOfEntryPoint;
    // Product parity (manual_map.cpp): the core carries no loader-managed
    // static TLS; only the callback array may exist, replayed ATTACH before
    // DllMain. Relocation ran above, so AddressOfCallBacks already holds the
    // remote VA.
    uint64_t tls_callbacks = 0;
    const auto& tls = headers->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls.VirtualAddress != 0) {
        const auto* tls_directory =
            Rva<IMAGE_TLS_DIRECTORY64>(image, tls.VirtualAddress);
        if (tls.Size < sizeof(IMAGE_TLS_DIRECTORY64) || tls_directory == nullptr) {
            Fail(
                L"core TLS directory is outside the image",
                MapError::ImageInvalid);
        }
        if (tls_directory->StartAddressOfRawData !=
                tls_directory->EndAddressOfRawData ||
            tls_directory->SizeOfZeroFill != 0) {
            Fail(
                L"core image requires loader-managed static TLS",
                MapError::ImageInvalid, ERROR_NOT_SUPPORTED);
        }
        tls_callbacks = tls_directory->AddressOfCallBacks;
        if (tls_callbacks != 0) {
            if (tls_callbacks < remote_base ||
                tls_callbacks >= remote_base + image.size()) {
                Fail(
                    L"core TLS callback table is outside the image",
                    MapError::ImageInvalid);
            }
            const auto callback_rva = static_cast<DWORD>(
                tls_callbacks - remote_base);
            const size_t maximum_callbacks = (std::min)(
                (image.size() - callback_rva) / sizeof(uint64_t),
                static_cast<size_t>(4096));
            bool terminated = false;
            for (size_t index = 0; index < maximum_callbacks; ++index) {
                const auto* callback = Rva<uint64_t>(
                    image,
                    callback_rva + static_cast<DWORD>(index * sizeof(uint64_t)));
                if (callback == nullptr) break;
                if (*callback == 0) {
                    terminated = true;
                    break;
                }
                if (*callback < remote_base ||
                    *callback >= remote_base + image.size()) {
                    Fail(
                        L"core TLS callback target is outside the image",
                        MapError::ImageInvalid);
                }
            }
            if (!terminated) {
                Fail(
                    L"core TLS callback table is not terminated",
                    MapError::ImageInvalid);
            }
        }
    }

    // The mapped headers must stay parseable: the core resolves its ESC-menu
    // logo through FindResourceW(__ImageBase, ...) about a second after
    // AnomalyStart returns, and a scrambled header made that resource lookup
    // fail -- plugin_host=start_failed, runtime stopped, no UI. The product
    // mapper never scrambles; the randomized base is the hiding measure.

    if (!RemoteWrite(
            remote_base, image.data(), static_cast<uint32_t>(image.size()))) {
        Fail(L"remote image write failed", MapError::WriteFailure);
    }
    if (!ProtectImage(remote_base, image.size(), layout)) {
        Fail(L"remote image protect failed", MapError::ProtectionFailure);
    }

    // start_info + remote strings. modules[0] is the main module (the PEB list
    // head's first entry is the executable).
    uint64_t remote_root = 0;
    {
        const std::wstring root = runtime_root.wstring();
        const size_t bytes = (root.size() + 1) * sizeof(wchar_t);
        if (!RemoteAlloc(bytes, PAGE_READWRITE, 0, &remote_root) ||
            !RemoteWrite(
                remote_root, root.c_str(), static_cast<uint32_t>(bytes))) {
            Fail(L"remote root string failed", MapError::WriteFailure);
        }
    }
    AnomalyStartInfo start_info{};
    start_info.struct_size = ANOMALY_START_INFO_V1_SIZE;
    start_info.bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION;
    start_info.bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_EXTERNAL;
    start_info.game_module = reinterpret_cast<HMODULE>(session.modules[0].base);
    start_info.runtime_root = reinterpret_cast<const WCHAR*>(remote_root);
    uint64_t remote_start_info = 0;
    if (!RemoteAlloc(sizeof(start_info), PAGE_READWRITE, 0, &remote_start_info) ||
        !RemoteWrite(remote_start_info, &start_info, sizeof(start_info))) {
        Fail(L"remote start info failed", MapError::WriteFailure);
    }

    ExportCache ntdll_cache;
    const uint64_t rtl_add = RemoteExport(
        FindModule(session.modules, L"ntdll.dll")->base, "RtlAddFunctionTable",
        &ntdll_cache);
    const uint64_t rtl_delete = RemoteExport(
        FindModule(session.modules, L"ntdll.dll")->base, "RtlDeleteFunctionTable",
        &ntdll_cache);
    if (rtl_add == 0 || rtl_delete == 0) {
        Fail(
            L"Rtl*FunctionTable could not be resolved in the target",
            MapError::DependencyFailure);
    }

    KmapContext context{};
    context.image_base = remote_base;
    context.exception_table =
        layout.exception_rva == 0 ? 0 : remote_base + layout.exception_rva;
    context.exception_count =
        layout.exception_size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    context.rtl_add = rtl_add;
    context.rtl_delete = rtl_delete;
    context.entry_point = remote_base + start_rva;
    context.start_info = remote_start_info;
    context.core_dllmain =
        core_dllmain_rva == 0 ? 0 : remote_base + core_dllmain_rva;
    context.tls_callbacks = tls_callbacks;
    context.bootstrap_error = ERROR_IO_PENDING;
    uint64_t remote_context = 0;
    if (!RemoteAlloc(sizeof(context), PAGE_READWRITE, 0, &remote_context) ||
        !RemoteWrite(remote_context, &context, sizeof(context))) {
        Fail(L"remote bootstrap context failed", MapError::WriteFailure);
    }

    // Thaw: everything above was pure kernel I/O against a frozen process;
    // everything below runs code in the target. The replay stages queue
    // special user APCs onto existing threads -- the process must be running
    // for the APCs to deliver, and no byte of any loaded module was touched,
    // so there is nothing for the anti-cheat writers to notice on wake-up.
    if (!RemoteResume()) {
        Fail(L"target could not be resumed", MapError::ProcessControlFailure);
    }
    wprintf(L"target resumed: pid=%lu\n", g_target_pid);

    // Deferred DllMain replay in topological order: every dependency the
    // mapper placed initializes before its users (creation order alone ran
    // wintrust before crypt32). A FALSE return is tolerated with a warning:
    // for a manual map the IAT already points at real functions, and OSDll
    // DllMains commonly FALSE out on environment probes (e.g. bcrypt's ETW
    // registration) without affecting the exports the core consumes. A module
    // with unresolved delay slots skips its DllMain entirely: its init path
    // commonly calls through those slots (dxgi calls IsRectEmpty during CRT
    // initterm), and a null-slot call kills the thread mid-DllMain.
    for (const auto& pending : session.DllMainOrder()) {
        if (SessionExpired()) {
            Fail(
                L"session budget exhausted before DllMain replay finished",
                MapError::Timeout, WAIT_TIMEOUT);
        }
        // Loader order: TLS registration precedes the module's DllMain --
        // msvcrt's CRT init reads its TLS slot immediately. The registration
        // thread also extends the loader-known threads' arrays, so game
        // threads entering core hooks later resolve TLS correctly.
        if (pending.tls_rva != 0) {
            std::string tls_failure;
            if (!RegisterStaticTls(session, pending, &tls_failure)) {
                wprintf(L"dep: TLS registration failed %ls: %hs\n",
                    pending.name.c_str(), tls_failure.c_str());
            } else {
                wprintf(L"dep: TLS registered %ls @ 0x%llx\n",
                    pending.name.c_str(), pending.base);
            }
        }
        if (pending.delay_slots_unresolved) {
            wprintf(L"dep: DllMain skipped %ls @ 0x%llx (unresolved delay slots)\n",
                pending.name.c_str(), pending.base);
            continue;
        }
        std::string failure;
        if (!RunDllMain(
                session, pending.base, pending.entry_rva, pending.layout,
                &failure)) {
            wprintf(L"dep: DllMain warning %ls @ 0x%llx: %hs\n",
                pending.name.c_str(), pending.base, failure.c_str());
        } else {
            wprintf(L"dep: DllMain ok %ls @ 0x%llx\n", pending.name.c_str(),
                pending.base);
        }
    }

    // Injection: user APCs are queued onto every thread; whichever enters
    // an alertable wait first runs the core entry and returns through
    // NtContinue, leaving its thread exactly where the APC found it.
    if (!QueueUserApc(remote_bootstrap, remote_context)) {
        Fail(
            L"bootstrap APC could not be queued",
            MapError::ProcessControlFailure);
    }

    // Poll for completion. Capped at 10s regardless of the session budget:
    // a healthy core reports in milliseconds, and the delivering thread --
    // one of the game's own -- must not be left running our stub forever.
    uint32_t runtime_start = 0;
    uint32_t bootstrap_error = 0;
    {
        const auto deadline = StageDeadline(std::chrono::seconds(10));
        bool finished = false;
        while (std::chrono::steady_clock::now() < deadline) {
            KmapContext remote{};
            if (RemoteRead(remote_context, &remote, sizeof(remote)) &&
                remote.done == 2) {
                finished = true;
                runtime_start = remote.result;
                bootstrap_error = remote.bootstrap_error;
                wprintf(
                    L"bootstrap: done result=%lu error=%lu\n", remote.result,
                    remote.bootstrap_error);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!finished) {
            Fail(
                L"bootstrap never reported completion", MapError::Timeout,
                WAIT_TIMEOUT);
        }
    }
    // The bootstrap contract: a non-zero code from either stage is a failed
    // attach, full stop. 5023 (ERROR_INVALID_STATE) here previously surfaced
    // as a "successful" attach with no UI in the game.
    if (bootstrap_error != ERROR_SUCCESS || runtime_start != ERROR_SUCCESS) {
        const uint32_t code = bootstrap_error != ERROR_SUCCESS
            ? bootstrap_error : runtime_start;
        Fail(
            (L"Anomaly core failed to start in the target (code=" +
             std::to_wstring(code) + L")")
                .c_str(),
            MapError::BootstrapFailure, code);
    }

    // Attribution proof.
    if (!AssertNoProcessHandles()) {
        Fail(
            L"attribution check failed: process handles are held",
            MapError::ProcessControlFailure);
    }

    // Fixture mode verifies through the marker the mapped core writes;
    // contract mode reports the bootstrap result through the INI and leaves
    // the game running -- the launcher owns its lifecycle.
    if (g_contract_mode) {
        WriteResultIni(
            static_cast<uint32_t>(MapError::None), ERROR_SUCCESS,
            g_target_pid, remote_base, runtime_start,
            L"Anomaly core attached through the kernel mapper");
        wprintf(L"kernel-mapper: attached pid=%lu image=0x%llx\n",
            g_target_pid, remote_base);
        // Result reported: drain (force sweep + poll) continues on a
        // detached thread so the launcher sees the outcome immediately
        // instead of "attaching" for the drain window.
        g_driver.BeginServiceShutdown();
        return 0;
    }

    bool marker_ok = false;
    {
        const auto deadline = StageDeadline(std::chrono::seconds(10));
        while (std::chrono::steady_clock::now() < deadline) {
            std::ifstream marker("core-marker.txt");
            if (marker) {
                std::string line;
                std::getline(marker, line);
                printf("marker: %s\n", line.c_str());
                marker_ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // Stop the target through the driver as well; still never a local handle.
    if (g_target_pid != 0) static_cast<void>(RemoteTerminate());

    wprintf(L"kernel-mapper e2e: %ls\n", marker_ok ? L"PASS" : L"FAIL");
    // Fixture mode owns its target's lifetime (it was terminated above), so
    // the drain happens in the foreground: the E2E harness wants the service
    // fully stopped before it inspects anything.
    g_driver.ShutdownService();
    return marker_ok ? 0 : 1;
}
