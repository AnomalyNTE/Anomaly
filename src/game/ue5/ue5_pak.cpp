#include "anomaly/ue5_pak.hpp"

#include "anomaly/hook_manager.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

// FPlatformFileManager::Get() -> the manager singleton whose first member is TopmostPlatformFile.
using ManagerGetFn = std::uintptr_t(__fastcall*)();
// FPakPlatformFile::Mount(const FPakMountArgs&, FIoStatus*, FPakListEntry*)
using MountWithStatusFn = bool(__fastcall*)(std::uintptr_t, const void*, void*, void*);
// FPakPlatformFile::Unmount(const TCHAR*)
using UnmountFn = bool(__fastcall*)(std::uintptr_t, const wchar_t*);
// FlushAsyncLoading(TConstArrayView<int32>) — an empty view drains all async package requests.
struct FlushAsyncLoadingRequestIds final {
    const std::int32_t* data;
    std::int32_t size;
};
static_assert(offsetof(FlushAsyncLoadingRequestIds, size) == 0x08);
static_assert(sizeof(FlushAsyncLoadingRequestIds) == 0x10);
using FlushAsyncLoadingFn = void(__fastcall*)(FlushAsyncLoadingRequestIds);
// The two IPlatformFile virtuals the chain walk needs.
using GetLowerLevelFn = std::uintptr_t(__fastcall*)(std::uintptr_t);
using GetNameFn = const wchar_t*(__fastcall*)(std::uintptr_t);

constexpr std::uint32_t kMaximumChainDepth = 32;
constexpr std::uint32_t kMaximumVtableOffset = 4096;
// FIoStatus is { EIoErrorCode ErrorCode; TCHAR ErrorMessage[128]; }; zeroing it is exactly the
// engine's own "Ok" value.
constexpr std::size_t kIoStatusBytes = sizeof(std::int32_t) + 128 * sizeof(wchar_t);

constexpr std::string_view kSignatureOwner = "anomaly.ue5.pak-signature";
constexpr std::uint64_t kSignatureGeneration = 1;
constexpr std::string_view kSignatureLabel = "ue5-pak-container-signature";

// Deliberately leaked: the detour must stay valid for the life of the process, and the hook record
// it points at cannot be destroyed while the engine may still call through it.
struct SignatureBypass final {
    HookManager hooks{CreateMinHookBackend()};
    void* original{};
    std::uintptr_t target{};
    bool installed{};
};

SignatureBypass* g_signature_bypass{};
std::mutex g_signature_bypass_mutex;

// Mirrors FPakMountArgs of the live build: the offsets were read out of
// FPakPlatformFile::Mount, which is why each one is asserted below.
struct PakMountArgs final {
    const wchar_t* pak_file;
    std::uint32_t order;
    std::uint32_t reserved;
    const wchar_t* mount_point;
    std::uint32_t mount_flags;
    std::uint8_t load_index;
    std::uint8_t padding[3];
};
static_assert(sizeof(PakMountArgs) == 0x20);
static_assert(offsetof(PakMountArgs, order) == 0x08);
static_assert(offsetof(PakMountArgs, mount_point) == 0x10);
static_assert(offsetof(PakMountArgs, mount_flags) == 0x18);
static_assert(offsetof(PakMountArgs, load_index) == 0x1C);

// FIoStatus: { EIoErrorCode ErrorCode; TCHAR ErrorMessage[128]; }. Plain storage the engine assigns
// into, so it needs no constructor of its own here.
struct PakIoStatus final {
    std::int32_t code;
    wchar_t message[128];
};
static_assert(sizeof(PakIoStatus) == kIoStatusBytes);

[[nodiscard]] bool ExecutableAddress(std::uintptr_t address, const SymbolMemory& memory) noexcept {
    const auto region = memory.Query(address);
    return region && region->state == MEM_COMMIT &&
        (region->protection & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                  PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Get() is a loader thunk: lea rax, [manager] / ret. Checking the shape rather than the bytes at
// one build's displacements keeps a stale pattern from being used as a call target.
[[nodiscard]] bool ManagerGetterShape(std::uintptr_t address, const SymbolMemory& memory) noexcept {
    std::array<std::uint8_t, 8> bytes{};
    if (!memory.Read(address, bytes.data(), bytes.size())) return false;
    return bytes[0] == 0x48 && bytes[1] == 0x8D && bytes[2] == 0x05 && bytes[7] == 0xC3;
}

bool ReadLayoutField(const BuildProfile& profile, std::string_view key, std::uint32_t& value) {
    const auto found = profile.layout.find(key);
    if (found == profile.layout.end()) return false;
    if (found->second < 0 || found->second > static_cast<std::int64_t>(kMaximumVtableOffset)) {
        return false;
    }
    value = static_cast<std::uint32_t>(found->second);
    return true;
}

FeatureValidationResult ValidatePakMount(const BuildProfile& profile, std::string_view feature,
    const ProfileResolutionSnapshot& resolution, const SymbolMemory& memory) {
    if (feature != kUe5PakFeature) return {false, "pak mount validator used by another feature"};
    Ue5PakBinding binding{};
    std::string error;
    if (!BindUe5Pak(profile, resolution, binding, error, &memory)) return {false, std::move(error)};
    // The chain walk cannot be validated without a live platform file; the getter shape and the
    // three entry points are what a stale Profile would break first.
    const std::pair<std::string_view, std::uintptr_t> entries[]{
        {kUe5PakPlatformFileManagerSymbol, binding.platform_file_manager},
        {kUe5PakMountSymbol, binding.mount},
        {kUe5PakUnmountSymbol, binding.unmount},
        {kUe5PakFlushAsyncLoadingSymbol, binding.flush_async_loading},
        {kUe5PakReleaseSymbol, binding.release}};
    for (const auto& [id, address] : entries) {
        if (!ExecutableAddress(address, memory)) {
            return {false, "pak entry is not executable: " + std::string(id)};
        }
    }
    if (binding.mount_with_status != 0 && !ExecutableAddress(binding.mount_with_status, memory)) {
        return {false, "pak status mount entry is not executable"};
    }
    if (binding.signature_verify != 0 &&
        !ExecutableAddress(binding.signature_verify, memory)) {
        return {false, "pak signature verifier is not executable"};
    }
    if (binding.list_entry_pak_file_offset != sizeof(void*))
        return {false, "pak output entry layout is unavailable"};
    if (!ManagerGetterShape(binding.platform_file_manager, memory)) {
        return {false, "platform file manager getter no longer has its loader shape"};
    }
    if (!binding.cache_verify_layout.configured ||
        !ExecutableAddress(binding.cache_verify, memory) ||
        !ExecutableAddress(binding.cache_verify_complete, memory)) {
        return {false, "pak cache verifier layout or entry points are unavailable"};
    }
    return {true, {}};
}

const ResolvedSymbol* RequireSymbol(const ProfileResolutionSnapshot& resolution,
    std::string_view id, std::string& error) {
    const auto* symbol = resolution.FindSymbol(id);
    if (symbol == nullptr || !symbol->Available() || symbol->address == 0) {
        error = "pak symbol is unavailable: " + std::string(id);
        return nullptr;
    }
    return symbol;
}

// The chain walk calls engine virtuals, so each step is fault-guarded and object-less.
const wchar_t* CallGetName(std::uintptr_t entry, GetNameFn function) noexcept {
    __try {
        return function(entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::uintptr_t CallGetLowerLevel(std::uintptr_t entry, GetLowerLevelFn function) noexcept {
    __try {
        return function(entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Mount transfers one strong FPakFile reference through this output entry.
struct PakListEntryScratch final {
    std::uint32_t read_order;
    void* pak_file;
};
// FPakListEntry is { uint32 ReadOrder; TRefCountPtr<FPakFile> PakFile; }; the Profile value of
// pak.listEntryPakFileOffset is checked against this member instead of trusting a literal.
static_assert(offsetof(PakListEntryScratch, pak_file) == 0x08);

// Separate from FIoStatus: its 128-character engine ABI cannot hold a fatal assertion.
struct PakMountException final {
    DWORD code{};
    void* address{};
    void* program_counter{};
    wchar_t message[2048]{};
};

int CaptureMountException(EXCEPTION_POINTERS* exception, PakMountException& fault) noexcept {
    fault.code = exception->ExceptionRecord->ExceptionCode;
    fault.address = exception->ExceptionRecord->ExceptionAddress;
    // UE's ReportAssert passes one FAssertInfo pointer to RaiseException. Keep the assertion
    // itself: ExceptionAddress otherwise points only to KERNELBASE!RaiseException.
    if (exception->ExceptionRecord->ExceptionCode == 0x4000 &&
        exception->ExceptionRecord->NumberParameters == 1) {
        struct AssertInfo { const wchar_t* message; void* program_counter; };
        __try {
            const auto* info = reinterpret_cast<const AssertInfo*>(exception->ExceptionRecord->ExceptionInformation[0]);
            if (info->message) {
                wcsncpy_s(fault.message, info->message, _TRUNCATE);
                fault.program_counter = info->program_counter;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Returns the raw engine verdict; a guarded fault must not look like EIoErrorCode::Ok.
bool CallMountWithStatus(std::uintptr_t pak_platform_file, MountWithStatusFn function,
    const PakMountArgs& args, PakIoStatus& status, PakListEntryScratch& entry,
    PakMountException& fault) noexcept {
    __try {
        return function(pak_platform_file, &args, &status, &entry);
    } __except (CaptureMountException(GetExceptionInformation(), fault)) {
        status.code = -1;
        return false;
    }
}

bool CallUnmount(std::uintptr_t pak_platform_file, UnmountFn function,
    const wchar_t* pak_file) noexcept {
    __try {
        return function(pak_platform_file, pak_file);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallFlushAsyncLoading(FlushAsyncLoadingFn function) noexcept {
    const FlushAsyncLoadingRequestIds request_ids{nullptr, 0};
    __try {
        function(request_ids);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Optional diagnostic read. Signature tables come from .sig; probing data does not create them.
// IPlatformFile::FDirectoryVisitor is abstract with the virtual order destructor,
// ShouldVisitLeafPathname, Visit: the engine asks ShouldVisitLeafPathname at vtable+8 before
// visiting and calls Visit at vtable+16. Mirrored here so the engine's directory iteration can call
// into a local object.
//
// IPlatformFile::IterateDirectory only lists ONE directory level (the recursive overload is a
// different virtual slot in this build, so the walk below drives this one per directory instead).
// The names it yields already include the container's mount point
// (FPakFile::FindFilesAtPathInPakDirectoryInternal prepends MountPoint), which is the form
// FPakPlatformFile::OpenRead expects, and it reports the engine's own order: entries stored in a pak
// come before anything the loose file system contributes.
class PakDirectoryScan final {
public:
    static constexpr std::size_t kMaximumEntries = 32;

    virtual ~PakDirectoryScan() = default;
    // Refusing a leaf here would skip every path the container holds.
    virtual bool ShouldVisitLeafPathname(const void*) { return true; }
    // The name points into the engine's iteration state and is gone once Visit returns, so it is
    // copied here. A name that does not fit is skipped rather than truncated: a truncated path must
    // never be opened as if it were the entry the engine listed.
    virtual bool Visit(const wchar_t* filename, bool is_directory) {
        if (filename == nullptr || count_ == kMaximumEntries) return true;
        std::size_t length{};
        while (length + 1 < kMaximumPathLength && filename[length] != L'\0') ++length;
        if (filename[length] != L'\0') return true;
        std::wmemcpy(entries_[count_].path, filename, length);
        entries_[count_].path[length] = L'\0';
        entries_[count_].is_directory = is_directory;
        ++count_;
        return count_ < kMaximumEntries;
    }
    void Reset() noexcept { count_ = 0; }
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] const wchar_t* path(std::size_t index) const noexcept {
        return entries_[index].path;
    }
    [[nodiscard]] bool directory(std::size_t index) const noexcept {
        return entries_[index].is_directory;
    }

private:
    static constexpr std::size_t kMaximumPathLength = 512;
    struct Entry final {
        wchar_t path[kMaximumPathLength];
        bool is_directory;
    };
    Entry entries_[kMaximumEntries]{};
    std::size_t count_{};
};

using IterateDirectoryFn = bool(__fastcall*)(std::uintptr_t, const wchar_t*, void*);
using PakOpenReadFn = void*(__fastcall*)(std::uintptr_t, const wchar_t*, bool);
using HandleReadFn = bool(__fastcall*)(void*, std::uint8_t*, std::int64_t);
using HandleSizeFn = std::int64_t(__fastcall*)(void*);
using HandleDestroyFn = void(__fastcall*)(void*, int);
// FPakFile::Find(const FString& FullPath, FPakEntry* OutEntry) -> FPakFile::EFindResult
// { NotFound = 0, Found = 1, FoundDeleted = 2 }. The entry out-parameter stays null: the answer
// wanted here is only "does this container own the path".
using PakFindFn = std::uint8_t(__fastcall*)(std::uintptr_t, const void*, void*);

// FString is a TArray<TCHAR>. The container only reads it during the lookup, so a view over a local
// buffer is enough and nothing is allocated or freed through the engine's allocator.
struct PakStringView final {
    wchar_t* data;
    std::int32_t num;
    std::int32_t max;
};
static_assert(sizeof(PakStringView) == 0x10);
static_assert(offsetof(PakStringView, num) == 0x08);
static_assert(offsetof(PakStringView, max) == 0x0C);

[[nodiscard]] PakStringView MakeStringView(const wchar_t* text) noexcept {
    const std::size_t length = text == nullptr ? 0 : std::wcslen(text);
    return PakStringView{const_cast<wchar_t*>(text), static_cast<std::int32_t>(length + 1),
        static_cast<std::int32_t>(length + 1)};
}

// The priming path has no SymbolMemory to query: it runs against the live instance the engine just
// handed back, so the executable check goes to the process itself.
[[nodiscard]] bool ExecutableCode(std::uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    if (address == 0 ||
        VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information)) ==
            0) {
        return false;
    }
    return information.State == MEM_COMMIT &&
        (information.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                   PAGE_EXECUTE_WRITECOPY)) != 0;
}

[[nodiscard]] bool TryReadPointer(std::uintptr_t address, std::uintptr_t& value) noexcept {
    __try {
        value = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A vtable slot of a live engine object, checked twice: the slot is readable and its target is
// executable code. A Profile that names the wrong offset therefore warns instead of calling data.
[[nodiscard]] std::uintptr_t VtableEntry(std::uintptr_t object, std::uint32_t offset) noexcept {
    if (object == 0) return 0;
    std::uintptr_t vtable{};
    if (!TryReadPointer(object, vtable) || vtable == 0) return 0;
    std::uintptr_t entry{};
    if (!TryReadPointer(vtable + offset, entry)) return 0;
    return ExecutableCode(entry) ? entry : 0;
}

bool CallIterateDirectory(std::uintptr_t function, std::uintptr_t pak_platform_file,
    const wchar_t* directory, PakDirectoryScan& scan) noexcept {
    __try {
        return reinterpret_cast<IterateDirectoryFn>(function)(pak_platform_file, directory, &scan);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Returns the handle, or nullptr; `faulted` distinguishes a fault from an ordinary refusal.
void* CallPakOpenRead(std::uintptr_t function, std::uintptr_t pak_platform_file,
    const wchar_t* filename, bool& faulted) noexcept {
    faulted = false;
    __try {
        return reinterpret_cast<PakOpenReadFn>(function)(pak_platform_file, filename, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return nullptr;
    }
}

// `bytes` reports the engine's answer: -1 when the handle has no usable read entry.
std::int64_t CallHandleRead(std::uintptr_t function, void* handle, std::uint8_t* buffer,
    std::int64_t bytes) noexcept {
    if (function == 0) return -1;
    __try {
        return reinterpret_cast<HandleReadFn>(function)(handle, buffer, bytes) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

std::int64_t CallHandleSize(std::uintptr_t function, void* handle) noexcept {
    if (function == 0) return -1;
    __try {
        return reinterpret_cast<HandleSizeFn>(function)(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// Returns false only when the Profile declared no release entry at all; a fault is reported
// separately so the report can say the handle was leaked on purpose.
bool CallHandleDestroy(std::uintptr_t function, void* handle, bool& faulted) noexcept {
    faulted = false;
    if (function == 0) return false;
    __try {
        reinterpret_cast<HandleDestroyFn>(function)(handle, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        return false;
    }
}

std::uint8_t CallPakFind(std::uintptr_t function, std::uintptr_t container,
    const wchar_t* path) noexcept {
    if (function == 0 || container == 0 || path == nullptr) return 0xFF;
    __try {
        const PakStringView view = MakeStringView(path);
        return reinterpret_cast<PakFindFn>(function)(container, &view, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFE;
    }
}

// Path text is diagnostic only, so it is bounded on both sides: the wide buffer it comes from is
// capped, and the UTF-8 form is cut at a character boundary rather than mid-sequence.
constexpr std::size_t kMaximumLoggedPathBytes = 200;

[[nodiscard]] std::string NarrowForLog(const wchar_t* text) {
    if (text == nullptr) return {};
    const int wide_length = static_cast<int>(std::wcslen(text));
    if (wide_length <= 0) return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, text, wide_length, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, wide_length, utf8.data(), length, nullptr, nullptr);
    if (utf8.size() <= kMaximumLoggedPathBytes) return utf8;
    std::size_t cut = kMaximumLoggedPathBytes;
    // Do not split a multi-byte sequence: back off to the start of the character.
    while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0U) == 0x80U) --cut;
    utf8.resize(cut);
    utf8 += "...";
    return utf8;
}

[[nodiscard]] bool StartsWith(const wchar_t* text, const wchar_t* prefix) noexcept {
    if (text == nullptr || prefix == nullptr) return false;
    const std::size_t length = std::wcslen(prefix);
    return std::wcsncmp(text, prefix, length) == 0;
}

constexpr std::size_t kMaximumMountPointLength = 256;

// Copies a NUL-terminated engine string into a capped local buffer. A stale Profile can turn this
// pointer into anything, so every character is read under the fault guard and an unreadable string
// comes back empty instead of taking the process down inside a diagnostic.
std::size_t ReadBoundedText(const wchar_t* text, wchar_t* buffer, std::size_t capacity) noexcept {
    if (text == nullptr || buffer == nullptr || capacity < 2) return 0;
    buffer[0] = L'\0';
    __try {
        for (std::size_t index = 0; index + 1 < capacity; ++index) {
            buffer[index] = text[index];
            if (buffer[index] == L'\0') return index;
        }
        buffer[capacity - 1] = L'\0';
        return capacity - 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        buffer[0] = L'\0';
        return 0;
    }
}

// One line of evidence for a stage: the path form that was tried, the path itself and the number the
// engine answered with. The stage id is kept beside it instead of inside it, so the caller can prefix
// its own `stage=` field once.
[[nodiscard]] std::string PrimeDetail(std::string_view form, const wchar_t* path,
    std::int64_t number) {
    std::string detail;
    if (!form.empty()) {
        detail.append("form=");
        detail.append(form);
    }
    if (path != nullptr) {
        if (!detail.empty()) detail.push_back(' ');
        detail.append("path=\"");
        detail.append(NarrowForLog(path));
        detail.push_back('"');
    }
    if (!detail.empty()) detail.push_back(' ');
    detail.append("engine=");
    detail.append(std::to_string(number));
    return detail;
}

// One deliberate read through an already mounted container: enumerate a few paths inside it, try
// each path form, and read one block through the platform file. Every step reports its own stage, so
// a single launch says exactly where the chain stops.
[[nodiscard]] Ue5PakPrimeReport PrimeContainerInternal(const Ue5PakBinding& binding,
    std::uintptr_t pak_platform_file, std::uintptr_t container) {
    Ue5PakPrimeReport report{};
    const Ue5PakPrimeLayout& prime = binding.prime;
    if (!prime.configured) {
        report.stage = "no-priming-layout";
        report.detail = "the profile declares no block-cache priming layout";
        return report;
    }
    if (pak_platform_file == 0) {
        report.stage = "no-platform-file";
        report.detail = "the pak platform file is unavailable";
        return report;
    }
    if (container == 0) {
        report.stage = "no-container";
        report.detail = "the mount reported no container object";
        return report;
    }
    std::uintptr_t mount_point{};
    if (!TryReadPointer(container + prime.pak_file_mount_point_offset, mount_point) ||
        mount_point == 0) {
        report.stage = "mount-point-unreadable";
        report.detail = "pak.pakFileMountPointOffset does not lead to a readable mount point";
        return report;
    }
    // Copied out once: every later use compares and logs this buffer, never engine memory.
    wchar_t mount_point_text[kMaximumMountPointLength]{};
    if (ReadBoundedText(reinterpret_cast<const wchar_t*>(mount_point), mount_point_text,
            ARRAYSIZE(mount_point_text)) == 0) {
        report.stage = "mount-point-unreadable";
        report.detail = "the container's mount point is not a readable string";
        return report;
    }
    const std::uintptr_t iterate =
        VtableEntry(pak_platform_file, prime.iterate_directory_vtable_offset);
    if (iterate == 0) {
        report.stage = "no-directory-iterator";
        report.detail = PrimeDetail({}, mount_point_text, 0);
        return report;
    }
    PakDirectoryScan scan;
    // Breadth-first walk of the container, bounded on every dimension. Two properties matter:
    //  - breadth-first, because a mount point that is the project root holds `Engine/...` next to
    //    `HT/Content/...`; depth-first would spend the directory budget inside the deep, unrelated
    //    subtree before ever reaching the shallow one the container cares about;
    //  - ownership first, because the platform file's listing merges every mounted container with
    //    the loose file system: a name is only allowed to occupy a candidate slot (and later an open
    //    attempt) once the container's own index claims it, so foreign names cannot starve the pass.
    constexpr std::size_t kMaximumDirectories = 256;
    const std::wstring mount_point_string(mount_point_text);
    std::vector<std::wstring> pending{mount_point_string};
    std::vector<std::wstring> files;
    std::vector<std::wstring> first_directories;
    Ue5PakPrimeTally tally;
    std::size_t directories_visited{};
    std::size_t files_seen{};
    std::size_t outside_mount_point{};
    bool iterate_faulted{};
    for (std::size_t next = 0; next < pending.size() && directories_visited < kMaximumDirectories &&
                              !tally.FilesExhausted();
         ++next) {
        const std::wstring directory = pending[next];
        ++directories_visited;
        if (first_directories.size() < 3) first_directories.push_back(directory);
        scan.Reset();
        // A false return is expected whenever the scan stops the iteration itself.
        const bool visited =
            CallIterateDirectory(iterate, pak_platform_file, directory.c_str(), scan);
        if (!visited && scan.count() == 0) {
            iterate_faulted = true;
            continue;
        }
        for (std::size_t index = 0; index < scan.count(); ++index) {
            const wchar_t* entry = scan.path(index);
            // A path outside the container's own mount point belongs to another container mounted
            // higher up or to the loose file system; it cannot be ours and is not descended into.
            if (!StartsWith(entry, mount_point_text) || directory == entry) {
                ++outside_mount_point;
                continue;
            }
            if (scan.directory(index)) {
                if (pending.size() < kMaximumDirectories) pending.emplace_back(entry);
                continue;
            }
            ++files_seen;
            // Ownership first, budget second: only a name the container claims returns true here, and
            // only then does it occupy one of the candidate slots.
            if (tally.AdmitFile(CallPakFind(prime.find, container, entry))) {
                files.emplace_back(entry);
            }
        }
    }
    const std::string counters = " dirs=" + std::to_string(directories_visited) +
        " files=" + std::to_string(files_seen) + " outside=" + std::to_string(outside_mount_point) +
        " owned=" + std::to_string(tally.owned) + " foreign=" + std::to_string(tally.foreign) +
        (tally.lookup_failed == 0
                ? std::string{}
                : " lookupFailed=" + std::to_string(tally.lookup_failed));
    if (files.empty()) {
        // Nothing the container owns was reached. When the walk did see names, the interesting fact
        // is that its root and the container's own index do not agree, so the mount point and the
        // first directories that were walked go into the line.
        if (files_seen == 0 && outside_mount_point == 0) {
            report.stage = iterate_faulted ? "iterate-faulted" : "no-filename";
            report.detail = PrimeDetail({}, mount_point_text, 0) + counters;
            return report;
        }
        if (tally.lookup_failed != 0) {
            report.stage = "container-lookup-faulted";
            report.detail = PrimeDetail({}, mount_point_text, tally.lookup_failed) + counters;
            return report;
        }
        report.stage = "container-index-mismatch";
        std::string walked{"mountPoint=\""};
        walked.append(NarrowForLog(mount_point_text));
        walked.append("\" walk=\"");
        for (std::size_t index = 0; index < first_directories.size(); ++index) {
            if (index != 0) walked.push_back('|');
            walked.append(NarrowForLog(first_directories[index].c_str()));
        }
        walked.push_back('"');
        report.detail = walked + counters;
        return report;
    }
    // A name directly under the mount point is what a loose marker file looks like too, so entries
    // below the root are asked about first while everything else stays as fallback.
    const auto below_root = [&mount_point_string](const std::wstring& path) {
        const std::size_t offset =
            mount_point_string.size() <= path.size() ? mount_point_string.size() : path.size();
        return path.find(L'/', offset) != std::wstring::npos;
    };
    std::stable_partition(files.begin(), files.end(), below_root);
    const std::uintptr_t open_read =
        VtableEntry(pak_platform_file, prime.open_read_vtable_offset);
    if (open_read == 0) {
        report.stage = "no-open-read-entry";
        report.detail = PrimeDetail({}, mount_point_text, 0) + counters;
        return report;
    }
    // The evidence of the last attempt that got furthest: that is the one worth reporting.
    std::string last_stage{"no-readable-file"};
    std::string last_detail = PrimeDetail("as-is", files.front().c_str(), 0);
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (!tally.TakeAttempt()) break;
        const std::wstring& candidate = files[index];
        bool open_faulted{};
        void* handle = CallPakOpenRead(open_read, pak_platform_file, candidate.c_str(),
            open_faulted);
        if (handle == nullptr) {
            last_stage = open_faulted ? "open-read-faulted" : "open-read-null";
            last_detail = PrimeDetail("as-is", candidate.c_str(), open_faulted ? 1 : 0);
            continue;
        }
        const auto handle_address = reinterpret_cast<std::uintptr_t>(handle);
        const auto release = [&]() {
            bool release_faulted{};
            const bool released = CallHandleDestroy(
                VtableEntry(handle_address, prime.file_handle_destroy_vtable_offset), handle,
                release_faulted);
            return released ? 0 : (release_faulted ? 2 : 1);
        };
        // The open resolves to the highest read order that holds the path, which is normally this
        // container but does not have to be: a same-name mod with a higher order wins.
        std::uintptr_t owner{};
        if (!TryReadPointer(handle_address + prime.file_handle_pak_file_offset, owner) ||
            owner != container) {
            static_cast<void>(release());
            last_stage = "not-our-container";
            last_detail = PrimeDetail("as-is", candidate.c_str(), 0);
            continue;
        }
        const std::int64_t size = CallHandleSize(
            VtableEntry(handle_address, prime.file_handle_size_vtable_offset), handle);
        // A container can index a path it holds no data for; such an entry reports no bytes and the
        // engine's read path rejects it before touching any block.
        if (size <= 0) {
            static_cast<void>(release());
            last_stage = size < 0 ? "size-unavailable" : "entry-size-zero";
            last_detail = PrimeDetail("as-is", candidate.c_str(), size);
            continue;
        }
        std::array<std::uint8_t, 256> block{};
        const auto wanted = (std::min)(size, static_cast<std::int64_t>(block.size()));
        ++tally.readable;
        const std::int64_t read = CallHandleRead(
            VtableEntry(handle_address, prime.file_handle_read_vtable_offset), handle,
            block.data(), wanted);
        const int released = release();
        if (read != 1) {
            last_stage = read == -1    ? "no-read-entry"
                : read == -2           ? "read-faulted"
                                       : "read-rejected";
            last_detail = PrimeDetail("as-is", candidate.c_str(), size);
            continue;
        }
        report.primed = true;
        report.stage = "ok";
        report.detail = PrimeDetail("as-is", candidate.c_str(), size) + counters;
        if (released != 0) report.detail += released == 2 ? " release=faulted" : " release=skipped";
        return report;
    }
    // Nothing was readable. Each way of getting there answers a different question, so they are
    // reported apart: a container whose entries carry no data, or a read the engine refused.
    report.stage = tally.readable == 0
        ? (directories_visited <= 1 ? "root-marker-only" : "entry-size-zero")
        : last_stage;
    report.detail = last_detail + counters + " candidates=" + std::to_string(tally.readable) +
        " attempts=" + std::to_string(tally.attempts);
    return report;
}

// The verifier's own signature: (cache object, request byte, request object, slice index).
using CacheVerifyFn = std::int64_t(__fastcall*)(void*, std::uint8_t, void*, std::uint32_t);
constexpr std::uint32_t kCacheVerifyDiagnosticSamples = 8;

// Deliberately leaked for the same reason as the signature bypass: the detour has to stay valid while
// the engine may still call through it.
struct CacheVerifyDetour final {
    HookManager hooks{CreateMinHookBackend()};
    void* original{};
    std::uintptr_t target{};
    // The completion half of the verifier. A request whose element has no table skips only the
    // verification loop and still goes through this, or its caller waits forever.
    std::uintptr_t complete{};
    Ue5PakCacheVerifyLayout layout{};
    Ue5PakCacheVerifySink sink{};
    void* context{};
    bool installed{};
    std::mutex sink_mutex;
    std::atomic<std::uint32_t> diagnostic_samples_remaining{};
};

CacheVerifyDetour* g_cache_verify{};
std::mutex g_cache_verify_mutex;
std::mutex g_cache_verify_lifecycle_mutex;
CacheVerifyFn g_cache_verify_passthrough{};

constexpr std::string_view kCacheVerifyOwner = "anomaly.ue5.pak-cache-verify";
constexpr std::uint64_t kCacheVerifyGeneration = 1;
constexpr std::string_view kCacheVerifyLabel = "ue5-pak-cache-verify";

// Fills the sample from the cache object using the offsets the verifier itself uses. Every read is
// guarded: a stale or half-built request must produce a log line rather than a fault, which is the
// whole reason this is a detour instead of a blind patch.
bool ReadCacheVerifySample(const Ue5PakCacheVerifyLayout& layout, std::uintptr_t cache,
    std::uint32_t request, Ue5PakCacheVerifySample& sample) noexcept {
    sample.cache = cache;
    sample.request = request;
    sample.element = 2;
    __try {
        std::uint32_t mask{};
        std::memcpy(&mask, reinterpret_cast<const void*>(cache + layout.cache_owner_mask_offset),
            sizeof(mask));
        std::uint32_t position{};
        std::memcpy(&position,
            reinterpret_cast<const void*>(cache + static_cast<std::uintptr_t>(layout.cache_slice_stride) * request +
                layout.cache_record_index_offset),
            sizeof(position));
        const std::uint32_t index = position & ~mask;
        std::uintptr_t records{};
        std::memcpy(&records, reinterpret_cast<const void*>(cache + layout.cache_records_offset),
            sizeof(records));
        std::uint64_t record{};
        std::memcpy(&record,
            reinterpret_cast<const void*>(records + static_cast<std::uintptr_t>(layout.record_stride) * index), sizeof(record));
        std::uintptr_t elements{};
        std::memcpy(&elements, reinterpret_cast<const void*>(cache + layout.cache_elements_offset),
            sizeof(elements));
        const std::uint32_t element_index = static_cast<std::uint32_t>(record >> 48);
        const std::uintptr_t element =
            elements + static_cast<std::uintptr_t>(layout.element_stride) * element_index;
        std::uintptr_t table{};
        std::memcpy(&table,
            reinterpret_cast<const void*>(element + layout.element_signature_table_offset),
            sizeof(table));
        std::uintptr_t container{};
        std::memcpy(&container,
            reinterpret_cast<const void*>(element + layout.element_container_offset),
            sizeof(container));
        std::uintptr_t handle{};
        std::memcpy(&handle, reinterpret_cast<const void*>(element + layout.element_handle_offset),
            sizeof(handle));
        sample.record = record;
        sample.index = element_index;
        sample.tag = static_cast<std::uint32_t>(record >> 16);
        sample.element = table == 0 ? 0 : 1;
        sample.element_pointer = element;
        sample.container = container;
        sample.handle = handle;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sample.element = 2;
        return false;
    }
}

// FPakPrecacher protects both backing arrays with CachedFilesScopeLock. SEH alone cannot
// make an unlocked read safe when another request grows or trims those arrays.
void ReadLockedCacheVerifySample(const Ue5PakCacheVerifyLayout& layout, std::uintptr_t cache,
    std::uint32_t slice, Ue5PakCacheVerifySample& sample) noexcept {
    sample.element = 2;
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(cache + layout.cache_lock_offset);
    __try {
        EnterCriticalSection(lock);
        __try {
            static_cast<void>(ReadCacheVerifySample(layout, cache, slice, sample));
        } __finally {
            LeaveCriticalSection(lock);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sample.element = 2;
    }
}

// Reports the attribution and routes the request: a container with a table keeps the engine's whole
// body, one without a table gets only the completion half so the caller is never left waiting. The
// engine sees the routed half's own result.
std::int64_t __fastcall CacheVerifyDetourFn(void* cache, std::uint8_t request_byte, void* request,
    std::uint32_t slice) noexcept {
    CacheVerifyDetour* detour{};
    CacheVerifyFn original{};
    PluginScope::CallbackLease lease;
    {
        std::scoped_lock lock(g_cache_verify_mutex);
        detour = g_cache_verify;
        if (detour != nullptr && detour->installed) {
            lease = detour->hooks.AcquireCallback(kCacheVerifyOwner, kCacheVerifyGeneration);
            if (lease) original = reinterpret_cast<CacheVerifyFn>(detour->original);
        }
        if (!lease) original = g_cache_verify_passthrough;
    }
    if (original == nullptr) return 1;
    if (!lease) return original(cache, request_byte, request, slice);
    Ue5PakCacheVerifySample sample{};
    ReadLockedCacheVerifySample(detour->layout, reinterpret_cast<std::uintptr_t>(cache), slice, sample);
    if (sample.element != 1 && detour->diagnostic_samples_remaining.load(std::memory_order_relaxed) != 0) {
        // Removal detaches the sink under this lock even if a trampoline cannot be removed.
        std::scoped_lock lock(detour->sink_mutex);
        const auto remaining = detour->diagnostic_samples_remaining.load(std::memory_order_relaxed);
        if (detour->sink != nullptr && remaining != 0) {
            // Bound attempts, not unique containers: even one unsigned mod can stream indefinitely.
            detour->diagnostic_samples_remaining.store(remaining - 1, std::memory_order_relaxed);
            detour->sink(detour->context, sample);
        }
    }
    // Only a successfully read null signature table is eligible. An unreadable record must
    // never be treated as an unsigned container. Completion owns GetReadResults and its lock;
    // do not duplicate that transfer or mutate the request from this hook.
    if (sample.element == 0 && detour->complete != 0) {
        return reinterpret_cast<CacheVerifyFn>(detour->complete)(cache, request_byte, request, slice);
    }
    return original(cache, request_byte, request, slice);
}

}  // namespace

Ue5PakCacheVerifyStatus InstallUe5PakBlockCacheVerifyDetour(
    const Ue5PakCacheVerifyLayout& layout, std::uintptr_t verify, std::uintptr_t complete,
    Ue5PakCacheVerifySink sink, void* context, std::uintptr_t* target) noexcept {
    if (target != nullptr) *target = verify;
    // Without the completion half there is no way to answer a request whose element has no table
    // without hanging its caller, so the detour is not installed at all and the mount-time line says
    // `unavailable`. A half-installed detour would trade a crash for a stall.
    if (verify == 0 || complete == 0 || !layout.configured || sink == nullptr) {
        return Ue5PakCacheVerifyStatus::Unavailable;
    }
    if (!ExecutableCode(verify) || !ExecutableCode(complete)) {
        return Ue5PakCacheVerifyStatus::Unreadable;
    }
    std::scoped_lock lifecycle_lock(g_cache_verify_lifecycle_mutex);
    std::scoped_lock lock(g_cache_verify_mutex);
    if (g_cache_verify == nullptr) {
        g_cache_verify = new (std::nothrow) CacheVerifyDetour();
        if (g_cache_verify == nullptr) return Ue5PakCacheVerifyStatus::Unavailable;
    }
    if (g_cache_verify->installed) {
        // A second container must not install a second detour; the first one keeps reporting for every
        // container in the process, so the caller only gets "already".
        if (g_cache_verify->target != verify) return Ue5PakCacheVerifyStatus::Unavailable;
        std::scoped_lock sink_lock(g_cache_verify->sink_mutex);
        g_cache_verify->sink = sink;
        g_cache_verify->context = context;
        g_cache_verify->diagnostic_samples_remaining.store(kCacheVerifyDiagnosticSamples, std::memory_order_relaxed);
        return Ue5PakCacheVerifyStatus::AlreadyInstalled;
    }
    if (g_cache_verify->original != nullptr) return Ue5PakCacheVerifyStatus::Unavailable;
    g_cache_verify->layout = layout;
    g_cache_verify->complete = complete;
    g_cache_verify->sink = sink;
    g_cache_verify->context = context;
    g_cache_verify->diagnostic_samples_remaining.store(kCacheVerifyDiagnosticSamples, std::memory_order_relaxed);
    if (!g_cache_verify->hooks.Create(std::string(kCacheVerifyOwner), kCacheVerifyGeneration,
            std::string(kCacheVerifyLabel), reinterpret_cast<void*>(verify),
            reinterpret_cast<void*>(&CacheVerifyDetourFn), &g_cache_verify->original)) {
        return Ue5PakCacheVerifyStatus::Unreadable;
    }
    g_cache_verify_passthrough = reinterpret_cast<CacheVerifyFn>(verify);
    if (!g_cache_verify->hooks.EnableOwner(kCacheVerifyOwner, kCacheVerifyGeneration)) {
        static_cast<void>(g_cache_verify->hooks.RemoveOwner(kCacheVerifyOwner, kCacheVerifyGeneration));
        g_cache_verify->original = nullptr;
        return Ue5PakCacheVerifyStatus::Unreadable;
    }
    g_cache_verify->target = verify;
    g_cache_verify->installed = true;
    return Ue5PakCacheVerifyStatus::Detoured;
}

void RemoveUe5PakBlockCacheVerifyDetour() noexcept {
    std::scoped_lock lifecycle_lock(g_cache_verify_lifecycle_mutex);
    CacheVerifyDetour* detour{};
    {
        std::scoped_lock lock(g_cache_verify_mutex);
        detour = g_cache_verify;
    }
    if (detour == nullptr) return;
    {
        std::scoped_lock sink_lock(detour->sink_mutex);
        detour->sink = nullptr;
        detour->context = nullptr;
        detour->diagnostic_samples_remaining.store(0, std::memory_order_relaxed);
    }
    if (!detour->installed || !detour->hooks.DisableOwner(kCacheVerifyOwner, kCacheVerifyGeneration)) return;
    {
        std::scoped_lock lock(g_cache_verify_mutex);
        detour->installed = false;
    }
    // The lease covers the original/completion call as well as the reporting callback.
    if (!detour->hooks.RemoveOwner(kCacheVerifyOwner, kCacheVerifyGeneration)) return;
    std::scoped_lock lock(g_cache_verify_mutex);
    detour->target = 0;
    detour->original = nullptr;
}

void DetachUe5PakBlockCacheVerifySink() noexcept {
    std::scoped_lock lifecycle_lock(g_cache_verify_lifecycle_mutex);
    CacheVerifyDetour* detour{};
    {
        std::scoped_lock lock(g_cache_verify_mutex);
        detour = g_cache_verify;
    }
    if (detour == nullptr) return;
    std::scoped_lock sink_lock(detour->sink_mutex);
    detour->sink = nullptr;
    detour->context = nullptr;
    detour->diagnostic_samples_remaining.store(0, std::memory_order_relaxed);
}

namespace {
// The verifier's only product is the status it writes; replacing the call with the engine's own
// success value leaves container parsing untouched.
void* __fastcall SignatureVerifyDetour(void* status, void*, void*, void*, void*, void*) noexcept {
    if (status != nullptr) std::memset(status, 0, kIoStatusBytes);
    return status;
}

}  // namespace

void RegisterUe5PakValidator(FeatureLayoutValidatorRegistry& validators) {
    validators.Register(std::string(kUe5PakValidator), ValidatePakMount);
}

bool BindUe5Pak(const BuildProfile& profile, const ProfileResolutionSnapshot& resolution,
    Ue5PakBinding& binding, std::string& error, const SymbolMemory* memory) {
    binding = Ue5PakBinding{};
    const auto* manager = RequireSymbol(resolution, kUe5PakPlatformFileManagerSymbol, error);
    if (manager == nullptr) return false;
    const auto* mount = RequireSymbol(resolution, kUe5PakMountSymbol, error);
    if (mount == nullptr) return false;
    const auto* unmount = RequireSymbol(resolution, kUe5PakUnmountSymbol, error);
    if (unmount == nullptr) return false;
    const auto* flush_async_loading =
        RequireSymbol(resolution, kUe5PakFlushAsyncLoadingSymbol, error);
    if (flush_async_loading == nullptr) return false;
    Ue5PakBinding candidate{};
    candidate.platform_file_manager = manager->address;
    candidate.mount = mount->address;
    candidate.unmount = unmount->address;
    candidate.flush_async_loading = flush_async_loading->address;
    const auto codec_symbol = [&](std::string_view name) -> std::uintptr_t {
        const auto* symbol = resolution.FindSymbol(name);
        return symbol && symbol->Available() && (!memory || ExecutableAddress(symbol->address, *memory)) ? symbol->address : 0;
    };
    candidate.encryption_key_manager = codec_symbol("ue5.FEncryptionKeyManager.Get");
    candidate.try_encryption_key = codec_symbol("ue5.FEncryptionKeyManager.TryGetKey");
    candidate.uncompress_memory = codec_symbol("ue5.FCompression.UncompressMemory");
    candidate.name_from_enum = codec_symbol("ue5.FName.FromEName");
    ReadLayoutField(profile, "pak.compressionZlibName", candidate.compression_zlib);
    ReadLayoutField(profile, "pak.compressionGzipName", candidate.compression_gzip);
    ReadLayoutField(profile, "pak.compressionLz4Name", candidate.compression_lz4);
    ReadLayoutField(profile, "pak.compressionOodleName", candidate.compression_oodle);
    const auto* status_mount = RequireSymbol(resolution, kUe5PakMountWithStatusSymbol, error);
    const auto* release = RequireSymbol(resolution, kUe5PakReleaseSymbol, error);
    if (status_mount == nullptr || release == nullptr) return false;
    candidate.mount_with_status = status_mount->address;
    candidate.release = release->address;
    if (memory != nullptr && !ManagerGetterShape(candidate.platform_file_manager, *memory)) {
        error = "platform file manager getter is not a lea rax / ret thunk";
        return false;
    }
    // The byte itself is checked where it is patched, so a refusal there can say what it saw.
    if (const auto* gate = resolution.FindSymbol(kUe5PakSignatureGateSymbol);
        gate != nullptr && gate->Available() && gate->address != 0) {
        candidate.signature_gate = gate->address;
    }
    if (const auto* verify = resolution.FindSymbol(kUe5PakSignatureVerifySymbol);
        verify != nullptr && verify->Available()) {
        candidate.signature_verify = verify->address;
    }
    // The feature validator requires both entries and the cache layout before any mount.
    if (const auto* cache_verify = resolution.FindSymbol(kUe5PakCacheVerifySymbol);
        cache_verify != nullptr && cache_verify->Available()) {
        candidate.cache_verify = cache_verify->address;
    }
    if (const auto* complete = resolution.FindSymbol(kUe5PakCacheVerifyCompleteSymbol);
        complete != nullptr && complete->Available()) {
        candidate.cache_verify_complete = complete->address;
    }
    Ue5PakCacheVerifyLayout cache_layout{};
    cache_layout.configured =
        ReadLayoutField(profile, kUe5PakCacheLockKey, cache_layout.cache_lock_offset) &&
        cache_layout.cache_lock_offset % alignof(CRITICAL_SECTION) == 0 &&
        ReadLayoutField(profile, kUe5PakCacheSliceStrideKey, cache_layout.cache_slice_stride) &&
        ReadLayoutField(profile, kUe5PakCacheRecordIndexKey, cache_layout.cache_record_index_offset) &&
        ReadLayoutField(profile, kUe5PakCacheOwnerMaskKey, cache_layout.cache_owner_mask_offset) &&
        ReadLayoutField(profile, kUe5PakCacheRecordsKey, cache_layout.cache_records_offset) &&
        ReadLayoutField(profile, kUe5PakRecordStrideKey, cache_layout.record_stride) &&
        ReadLayoutField(profile, kUe5PakCacheElementsKey, cache_layout.cache_elements_offset) &&
        ReadLayoutField(profile, kUe5PakElementStrideKey, cache_layout.element_stride) &&
        ReadLayoutField(
            profile, kUe5PakElementTableKey, cache_layout.element_signature_table_offset) &&
        ReadLayoutField(profile, kUe5PakElementHandleKey, cache_layout.element_handle_offset) &&
        ReadLayoutField(profile, kUe5PakElementContainerKey, cache_layout.element_container_offset) &&
        cache_layout.record_stride >= sizeof(std::uint64_t) &&
        cache_layout.record_stride % alignof(std::uint64_t) == 0 &&
        cache_layout.cache_slice_stride >= sizeof(std::uint32_t) &&
        cache_layout.cache_slice_stride % alignof(std::uint32_t) == 0 &&
        cache_layout.cache_record_index_offset % alignof(std::uint32_t) == 0 &&
        cache_layout.cache_owner_mask_offset % alignof(std::uint32_t) == 0 &&
        cache_layout.cache_records_offset % alignof(void*) == 0 &&
        cache_layout.cache_elements_offset % alignof(void*) == 0 &&
        cache_layout.element_stride % alignof(void*) == 0;
    const std::uint32_t element_fields[]{cache_layout.element_signature_table_offset,
        cache_layout.element_handle_offset, cache_layout.element_container_offset};
    for (const auto offset : element_fields) {
        if (offset % alignof(void*) != 0 || offset + sizeof(void*) > cache_layout.element_stride)
            cache_layout.configured = false;
    }
    if (element_fields[0] == element_fields[1] || element_fields[0] == element_fields[2] ||
        element_fields[1] == element_fields[2]) cache_layout.configured = false;
    candidate.cache_verify_layout = cache_layout;
    if (!ReadLayoutField(profile, kUe5PakListEntryKey, candidate.list_entry_pak_file_offset) ||
        !ReadLayoutField(profile, kUe5PakMountedListKey, candidate.mounted_list_offset) ||
        !ReadLayoutField(profile, kUe5PakMountedLockKey, candidate.mounted_list_lock_offset) ||
        !ReadLayoutField(profile, kUe5PakFilenameKey, candidate.pak_file_filename_offset) ||
        !ReadLayoutField(profile, kUe5PakManagerTopmostKey, candidate.manager_topmost_offset) ||
        !ReadLayoutField(profile, kUe5PakLowerLevelKey, candidate.lower_level_vtable_offset) ||
        !ReadLayoutField(profile, kUe5PakNameKey, candidate.name_vtable_offset) ||
        !ReadLayoutField(profile, kUe5PakSignedFlagKey, candidate.signed_flag_offset)) {
        error = "pak platform file chain layout is unavailable";
        return false;
    }
    if (candidate.mounted_list_offset % alignof(void*) != 0 ||
        candidate.mounted_list_offset < sizeof(void*) ||
        candidate.mounted_list_lock_offset % alignof(CRITICAL_SECTION) != 0 ||
        candidate.mounted_list_lock_offset < candidate.mounted_list_offset + sizeof(PakStringView) ||
        candidate.pak_file_filename_offset < sizeof(void*) ||
        candidate.pak_file_filename_offset % alignof(void*) != 0) {
        error = "pak mounted list layout is invalid";
        return false;
    }
    // The priming layout is read separately: it is only exercised after a mount, so a Profile that
    // predates it still mounts containers and reports the missing layout instead of refusing.
    Ue5PakPrimeLayout prime{};
    prime.configured =
        ReadLayoutField(profile, kUe5PakMountPointKey, prime.pak_file_mount_point_offset) &&
        ReadLayoutField(profile, kUe5PakOpenReadKey, prime.open_read_vtable_offset) &&
        ReadLayoutField(profile, kUe5PakIterateDirectoryKey, prime.iterate_directory_vtable_offset) &&
        ReadLayoutField(profile, kUe5PakHandleReadKey, prime.file_handle_read_vtable_offset) &&
        ReadLayoutField(profile, kUe5PakHandleSizeKey, prime.file_handle_size_vtable_offset) &&
        ReadLayoutField(profile, kUe5PakHandlePakFileKey, prime.file_handle_pak_file_offset) &&
        ReadLayoutField(profile, kUe5PakHandleDestroyKey, prime.file_handle_destroy_vtable_offset) &&
        // Slot zero is the handle's deleting destructor, so a zero open/read/size offset would call
        // the wrong entry; such a Profile is treated as undeclared.
        prime.open_read_vtable_offset != 0 && prime.iterate_directory_vtable_offset != 0 &&
        prime.file_handle_read_vtable_offset != 0 && prime.file_handle_size_vtable_offset != 0;
    for (const auto offset : {prime.open_read_vtable_offset, prime.iterate_directory_vtable_offset,
             prime.file_handle_read_vtable_offset, prime.file_handle_size_vtable_offset,
             prime.file_handle_destroy_vtable_offset, prime.file_handle_pak_file_offset,
             prime.pak_file_mount_point_offset}) {
        if (offset % alignof(void*) != 0) prime.configured = false;
    }
    // Without the container's own index there is no way to tell our files from another container's,
    // so a Profile that cannot resolve it declares no priming layout at all.
    if (const auto* find = resolution.FindSymbol(kUe5PakFindSymbol);
        find != nullptr && find->Available()) {
        prime.find = find->address;
    }
    if (prime.find == 0 || (memory != nullptr && !ExecutableAddress(prime.find, *memory)))
        prime.configured = false;
    candidate.prime = prime;
    if (!candidate.Complete()) {
        error = "pak platform file chain layout is misaligned";
        return false;
    }
    binding = candidate;
    return true;
}

namespace {
bool ContainerKeyCall(const Ue5PakBinding& binding, const std::uint8_t* guid, std::uint8_t* key) noexcept {
    if (!binding.encryption_key_manager || !binding.try_encryption_key) return false;
    __try {
        const auto manager = reinterpret_cast<std::uintptr_t(__fastcall*)()>(binding.encryption_key_manager)();
        return manager && reinterpret_cast<bool(__fastcall*)(std::uintptr_t, const void*, void*)>(
            binding.try_encryption_key)(manager, guid, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ContainerDecompressCall(const Ue5PakBinding& binding, std::uint32_t format,
    const void* input, std::size_t input_size, void* output, std::size_t output_size) noexcept {
    if (!format || !binding.uncompress_memory || !binding.name_from_enum) return false;
    __try {
        std::uint64_t name{};
        reinterpret_cast<void*(__fastcall*)(void*, std::uint32_t)>(binding.name_from_enum)(&name, format);
        return reinterpret_cast<bool(__fastcall*)(std::uint64_t, void*, std::int64_t,
            const void*, std::int64_t, std::uint32_t, std::uintptr_t)>(binding.uncompress_memory)(
                name, output, static_cast<std::int64_t>(output_size), input, static_cast<std::int64_t>(input_size), 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace
Ue5ContainerCodecs Ue5PakContainerCodecs(const Ue5PakBinding& binding) {
    return {
        [binding](const auto& guid, auto& key) { return ContainerKeyCall(binding, guid.data(), key.data()); },
        [binding](std::string_view method, std::span<const std::uint8_t> input, std::span<std::uint8_t> output) {
            const auto format = method == "Zlib" ? binding.compression_zlib : method == "Gzip" ? binding.compression_gzip :
                method == "LZ4" ? binding.compression_lz4 : method == "Oodle" ? binding.compression_oodle : 0;
            return ContainerDecompressCall(binding, format, input.data(), input.size(), output.data(), output.size());
        }
    };
}

std::uintptr_t FindUe5PakPlatformFile(
    const Ue5PakBinding& binding, const SymbolMemory& memory) noexcept {
    if (!binding.Complete()) return 0;
    std::uintptr_t manager{};
    __try {
        manager = reinterpret_cast<ManagerGetFn>(binding.platform_file_manager)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (manager == 0) return 0;
    std::uintptr_t entry{};
    if (!memory.Read(manager + binding.manager_topmost_offset, &entry, sizeof(entry))) return 0;

    for (std::uint32_t depth = 0; depth < kMaximumChainDepth && entry != 0; ++depth) {
        const auto region = memory.Query(entry);
        if (!region || region->state != MEM_COMMIT) return 0;
        std::uintptr_t vtable{};
        if (!memory.Read(entry, &vtable, sizeof(vtable)) || vtable == 0) return 0;
        std::uintptr_t name_function{};
        std::uintptr_t lower_function{};
        if (!memory.Read(vtable + binding.name_vtable_offset, &name_function,
                sizeof(name_function)) ||
            !memory.Read(vtable + binding.lower_level_vtable_offset, &lower_function,
                sizeof(lower_function))) {
            return 0;
        }
        if (!ExecutableAddress(name_function, memory) ||
            !ExecutableAddress(lower_function, memory)) {
            return 0;
        }
        const wchar_t* name =
            CallGetName(entry, reinterpret_cast<GetNameFn>(name_function));
        if (name == nullptr) return 0;
        wchar_t name_copy[8]{};
        if (!memory.Read(reinterpret_cast<std::uintptr_t>(name), name_copy, sizeof(name_copy))) return 0;
        if (std::memcmp(name_copy, L"PakFile", sizeof(name_copy)) == 0) return entry;
        entry = CallGetLowerLevel(entry, reinterpret_cast<GetLowerLevelFn>(lower_function));
    }
    return 0;
}

Ue5PakPrimeReport PrimeUe5PakContainer(const Ue5PakBinding& binding,
    std::uintptr_t pak_platform_file, std::uintptr_t container) noexcept {
    try {
        return PrimeContainerInternal(binding, pak_platform_file, container);
    } catch (...) {
        Ue5PakPrimeReport report{};
        report.stage = "prime-exception";
        report.detail = "the priming read raised an exception";
        return report;
    }
}

Ue5PakMountResult MountUe5Pak(const Ue5PakBinding& binding, std::uintptr_t pak_platform_file,
    const Ue5PakMountRequest& request) noexcept {
    Ue5PakMountResult result{};
    if (!binding.Complete() || pak_platform_file == 0 || request.pak_file.empty() ||
        binding.list_entry_pak_file_offset != offsetof(PakListEntryScratch, pak_file)) return result;
    const wchar_t* mount_point =
        request.mount_point.empty() ? nullptr : request.mount_point.c_str();
    // Keep the output reference for explicitly requested diagnostic reads.
    PakListEntryScratch entry{};
    PakMountArgs args{};
    args.pak_file = request.pak_file.c_str();
    args.order = request.order;
    args.mount_point = mount_point;
    // UE 5.6 FPakMountOptions::SkipContainerFile; pure paks have no IoStore sidecar.
    args.mount_flags = request.pak_only ? 4U : 0U;
    args.load_index = 1;
    PakIoStatus status{-1, {}};
    PakMountException fault;
    result.mounted = CallMountWithStatus(pak_platform_file,
        reinterpret_cast<MountWithStatusFn>(binding.mount_with_status), args, status, entry, fault);
    result.error_code = status.code;
    result.native_fault = fault.code != 0;
    if (result.native_fault) {
        wchar_t text[128]{};
        swprintf_s(text, L"mount exception 0x%08lX at %p (assert pc %p): ",
            fault.code, fault.address, fault.program_counter);
        result.error_message = text;
        result.error_message += fault.message;
    } else if (status.code != 0) {
        std::size_t length{};
        while (length < ARRAYSIZE(status.message) && status.message[length] != L'\0') ++length;
        result.error_message.assign(status.message, length);
    }
    if (!result.mounted) {
        ReleaseUe5PakContainer(binding, reinterpret_cast<std::uintptr_t>(entry.pak_file));
        return result;
    }
    result.container = reinterpret_cast<std::uintptr_t>(entry.pak_file);
    return result;
}

namespace {
bool EnterPakList(CRITICAL_SECTION* lock) noexcept {
    __try { EnterCriticalSection(lock); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct PakListLock {
    CRITICAL_SECTION* lock;
    ~PakListLock() { LeaveCriticalSection(lock); }
};
}  // namespace

std::optional<std::vector<Ue5MountedPakFile>> MountedUe5PakFiles(
    const Ue5PakBinding& binding, std::uintptr_t pak_platform_file, const SymbolMemory& memory) {
    if (!binding.Complete() || !pak_platform_file || !binding.mounted_list_lock_offset)
        return std::nullopt;
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(pak_platform_file + binding.mounted_list_lock_offset);
    if (!EnterPakList(lock)) return std::nullopt;
    const PakListLock scope{lock};
    struct Array { std::uintptr_t data; std::int32_t num, max; } list{};
    if (!memory.Read(pak_platform_file + binding.mounted_list_offset, &list, sizeof(list)) ||
        list.num < 0 || list.num > 65536 || list.max < list.num || (list.num && !list.data))
        return std::nullopt;
    std::vector<Ue5MountedPakFile> result;
    result.reserve(list.num);
    for (std::int32_t index{}; index < list.num; ++index) {
        PakListEntryScratch entry{};
        if (!memory.Read(list.data + sizeof(entry) * index, &entry, sizeof(entry)) || !entry.pak_file)
            return std::nullopt;
        const auto container = reinterpret_cast<std::uintptr_t>(entry.pak_file);
        PakStringView name{};
        if (!memory.Read(container + binding.pak_file_filename_offset, &name, sizeof(name)) ||
            !name.data || name.num <= 1 || name.num > 32768 || name.max < name.num)
            return std::nullopt;
        std::wstring filename(name.num, L'\0');
        if (!memory.Read(reinterpret_cast<std::uintptr_t>(name.data), filename.data(), filename.size() * sizeof(wchar_t)) ||
            filename.back() != L'\0') return std::nullopt;
        filename.pop_back();
        if (filename.find(L'\0') != std::wstring::npos) return std::nullopt;
        result.push_back({std::move(filename), container});
    }
    return result;
}

bool UnmountUe5Pak(const Ue5PakBinding& binding, std::uintptr_t pak_platform_file,
    const std::wstring& pak_file) noexcept {
    if (!binding.Complete() || pak_platform_file == 0 || pak_file.empty()) return false;
    return CallUnmount(pak_platform_file, reinterpret_cast<UnmountFn>(binding.unmount),
        pak_file.c_str());
}

bool FlushUe5AsyncLoading(const Ue5PakBinding& binding) noexcept {
    if (!binding.Complete()) return false;
    return CallFlushAsyncLoading(
        reinterpret_cast<FlushAsyncLoadingFn>(binding.flush_async_loading));
}

void ReleaseUe5PakContainer(const Ue5PakBinding& binding, std::uintptr_t container) noexcept {
    if (container == 0 || binding.release == 0) return;
    __try {
        reinterpret_cast<void(__fastcall*)(std::uintptr_t)>(binding.release)(container);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ClearUe5PakSignatureRequirement(const Ue5PakBinding& binding,
    std::uintptr_t pak_platform_file, const SymbolMemory& memory) noexcept {
    if (!binding.Complete() || pak_platform_file == 0) return false;
    const std::uintptr_t address = pak_platform_file + binding.signed_flag_offset;
    std::uint8_t value{};
    if (!memory.Read(address, &value, sizeof(value))) return false;
    // Only a bool lives here; anything else means the offset is wrong for this build.
    if (value > 1) return false;
    if (value == 0) return true;
    const std::uint8_t cleared{};
    if (!memory.Write(address, &cleared, sizeof(cleared))) return false;
    std::uint8_t confirmed{1};
    return memory.Read(address, &confirmed, sizeof(confirmed)) && confirmed == 0;
}

Ue5PakGateStatus DisableUe5PakSignatureGate(
    const Ue5PakBinding& binding, std::uint8_t* observed) noexcept {
    if (observed != nullptr) *observed = 0;
    if (binding.signature_gate == 0) return Ue5PakGateStatus::NoSymbol;
    // Read and write through the process directly: this layer already lives in the target process,
    // and a single byte in .text needs no region bookkeeping.
    std::uint8_t byte{};
    SIZE_T transferred{};
    if (ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(binding.signature_gate), &byte, sizeof(byte),
            &transferred) == FALSE ||
        transferred != sizeof(byte)) {
        return Ue5PakGateStatus::Unreadable;
    }
    if (observed != nullptr) *observed = byte;
    if (byte == 0xEB) return Ue5PakGateStatus::AlreadyPatched;
    // Force the IsSigningEnabled()==false path. Its destination still tests the container's
    // Signed flag before consuming a signature section. Forcing the later JNZ instead reads
    // directory/hash bytes as HashSize in unsigned containers and can fatal in TArray::Reserve.
    // Requiring JZ also rejects Profiles that still resolve that old JNZ patch site.
    if (byte != 0x74) return Ue5PakGateStatus::UnexpectedByte;
    const std::uint8_t jump{0xEB};
    DWORD previous{};
    if (VirtualProtect(reinterpret_cast<void*>(binding.signature_gate), sizeof(jump),
            PAGE_EXECUTE_READWRITE, &previous) == FALSE) {
        return Ue5PakGateStatus::Unreadable;
    }
    const BOOL written = WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(binding.signature_gate), &jump, sizeof(jump), &transferred);
    static_cast<void>(VirtualProtect(reinterpret_cast<void*>(binding.signature_gate),
        sizeof(jump), previous, &previous));
    if (written == FALSE || transferred != sizeof(jump)) return Ue5PakGateStatus::Unreadable;
    FlushInstructionCache(GetCurrentProcess(),
        reinterpret_cast<const void*>(binding.signature_gate), sizeof(jump));
    return Ue5PakGateStatus::Patched;
}

bool InstallUe5PakSignatureVerificationBypass(std::uintptr_t verifier) noexcept {
    if (verifier == 0) return false;
    std::scoped_lock lock(g_signature_bypass_mutex);
    if (g_signature_bypass == nullptr) {
        g_signature_bypass = new (std::nothrow) SignatureBypass();
        if (g_signature_bypass == nullptr) return false;
    }
    if (g_signature_bypass->installed) return g_signature_bypass->target == verifier;
    if (!g_signature_bypass->hooks.Create(std::string(kSignatureOwner), kSignatureGeneration,
            std::string(kSignatureLabel), reinterpret_cast<void*>(verifier),
            reinterpret_cast<void*>(&SignatureVerifyDetour), &g_signature_bypass->original)) {
        return false;
    }
    if (!g_signature_bypass->hooks.EnableOwner(kSignatureOwner, kSignatureGeneration)) {
        static_cast<void>(g_signature_bypass->hooks.RemoveOwner(kSignatureOwner, kSignatureGeneration));
        return false;
    }
    g_signature_bypass->target = verifier;
    g_signature_bypass->installed = true;
    return true;
}

void RemoveUe5PakSignatureVerificationBypass() noexcept {
    std::scoped_lock lock(g_signature_bypass_mutex);
    if (g_signature_bypass == nullptr || !g_signature_bypass->installed) return;
    if (g_signature_bypass->hooks.DisableOwner(kSignatureOwner, kSignatureGeneration)) {
        static_cast<void>(g_signature_bypass->hooks.RemoveOwner(kSignatureOwner, kSignatureGeneration));
        g_signature_bypass->installed = false;
        g_signature_bypass->target = 0;
        g_signature_bypass->original = nullptr;
    }
}

}  // namespace anomaly
