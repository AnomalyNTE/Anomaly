#include "anomaly/ace_unhook.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace anomaly {
namespace {

// The loader and anti-debug entries this game's anti-cheat inline-hooks. The loader ones are what matter here:
// while ntdll!LdrLoadDll runs the anti-cheat's code instead, every module load passes through a path that asks
// the compatibility database about a module identity this process cannot provide for a core it mapped itself,
// and that answer faults inside apphelp.
struct Target final {
    const char* module;
    const char* function;
};

constexpr Target kTargets[] = {
    {"ntdll.dll", "LdrInitializeThunk"},
    {"ntdll.dll", "LdrLoadDll"},
    {"ntdll.dll", "NtCreateThread"},
    {"ntdll.dll", "NtCreateThreadEx"},
    {"ntdll.dll", "RtlAddVectoredExceptionHandler"},
    {"kernel32.dll", "IsProcessorFeaturePresent"},
    {"kernel32.dll", "SetUnhandledExceptionFilter"},
    {"kernelbase.dll", "RegQueryValueExW"},
};

constexpr wchar_t kAntiCheatModule[] = L"ACE-Base64.dll";

// Threads that re-apply the hooks while their start address sits in an unrelated module: the rewriter runs
// disguised inside the game own libraries, so an image-scoped freeze never matches it.
//
// Login, account, patcher, input and network modules are deliberately absent: freezing their threads stops the
// login flow itself (the SDK threads are indistinguishable from a rewriter by address alone), so only the
// modules that carry the rewriter are listed here.
struct FreezeRule final {
    const char* module;
    std::uintptr_t offset;
};

constexpr FreezeRule kFreezeRules[] = {
    {"HTGameBase.dll", 0x250D80},
    {"HTGameBase.dll", 0x253820},
    {"HTGameBase.dll", 0x4AFB40},
    {"combase.dll", 0x135530},
    {"ucrtbase.dll", 0x95050},
};


// ThreadQuerySetWin32StartAddress: the address a thread was created with.
constexpr int kThreadQuerySetWin32StartAddress = 9;
// The prologue a hook overwrites: eight bytes covers an E9 rel32, sixteen also covers
// `mov rax, imm64; jmp rax`.
constexpr std::size_t kRestoreBytes = 16;
// The cadence a re-applied hook is noticed at.
constexpr DWORD kPassIntervalMs = 250;
// Freeze threads that start in generated code as well as the ones inside the anti-cheat image. The probe this
// is modelled on leaves that off by default: it also matches any other just-in-time code in the process, and
// freezing a thread the game needs stalls it. Turn it on when a watchdog survives the image-scoped freeze and
// the game still reacts to the restore.
constexpr bool kFreezeGeneratedCode = true;
constexpr std::size_t kMaximumDiskImageBytes = 16u * 1024u * 1024u;

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, HANDLE*);

// ---------------------------------------------------------------------------
// trace
// ---------------------------------------------------------------------------

std::wstring g_trace_path;
// The trace exists to make decisions inspectable, not to record every pass: once it reaches this size it stops,
// because a session that keeps fighting a re-hook would otherwise write from now until the process exits.
constexpr std::uint64_t kMaximumTraceBytes = 256u * 1024u;
std::uint64_t g_trace_bytes;

void Trace(const char* const format, ...) {
    if (g_trace_path.empty()) return;
    if (g_trace_bytes >= kMaximumTraceBytes) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_trace_path.c_str(), L"ab") != 0 || file == nullptr) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fprintf(
        file, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", now.wYear, now.wMonth, now.wDay, now.wHour,
        now.wMinute, now.wSecond, now.wMilliseconds);
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(file, format, arguments);
    va_end(arguments);
    std::fputc('\n', file);
    const long position = std::ftell(file);
    std::fclose(file);
    if (position > 0) g_trace_bytes = static_cast<std::uint64_t>(position);
}

// ---------------------------------------------------------------------------
// system images
// ---------------------------------------------------------------------------

// A module as it exists on disk: the byte source the restore writes from, because these prologues differ per
// Windows build and a baked-in pattern would write garbage after any update.
struct DiskImage final {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> relocations;
    std::array<IMAGE_SECTION_HEADER, 96> sections{};
    std::size_t section_count{};
    const IMAGE_NT_HEADERS64* headers{};
    std::uint32_t time_date_stamp{};
    std::uint32_t size_of_image{};

    [[nodiscard]] bool Valid() const noexcept { return headers != nullptr; }

    [[nodiscard]] std::size_t FileOffset(const std::uint32_t rva) const noexcept {
        for (std::size_t index = 0; index < section_count; ++index) {
            const auto& section = sections[index];
            const std::size_t span = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva >= section.VirtualAddress && rva < section.VirtualAddress + span) {
                const std::size_t offset = section.PointerToRawData + (rva - section.VirtualAddress);
                return (std::min)(offset, bytes.size());
            }
        }
        return (std::min)(static_cast<std::size_t>(rva), bytes.size());
    }
};

[[nodiscard]] DiskImage LoadDiskImage(const wchar_t* const path) {
    DiskImage image;
    const HANDLE file = CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return image;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaximumDiskImageBytes) {
        CloseHandle(file);
        return image;
    }
    image.bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read{};
    const bool ok =
        ReadFile(file, image.bytes.data(), static_cast<DWORD>(image.bytes.size()), &read, nullptr) != FALSE &&
        read == image.bytes.size();
    CloseHandle(file);
    if (!ok || image.bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        image.bytes.clear();
        return image;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.bytes.size()) {
        return image;
    }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.bytes.data() + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) return image;
    image.headers = headers;
    image.time_date_stamp = headers->FileHeader.TimeDateStamp;
    image.size_of_image = headers->OptionalHeader.SizeOfImage;
    image.section_count = (std::min)(
        static_cast<std::size_t>(headers->FileHeader.NumberOfSections), image.sections.size());
    const auto* sections = IMAGE_FIRST_SECTION(headers);
    for (std::size_t index = 0; index < image.section_count; ++index) {
        image.sections[index] = sections[index];
    }
    const auto& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress != 0 && directory.Size != 0) {
        const std::size_t offset = image.FileOffset(directory.VirtualAddress);
        if (offset < image.bytes.size()) {
            const std::size_t span =
                (std::min)(static_cast<std::size_t>(directory.Size), image.bytes.size() - offset);
            image.relocations.assign(
                image.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                image.bytes.begin() + static_cast<std::ptrdiff_t>(offset + span));
        }
    }
    return image;
}

// The bytes the prologue has once the loader mapped the image at `module_base`: the file bytes with the
// absolute addresses inside the window rebased. Copying file bytes over relocated code would corrupt the very
// entry being repaired.
[[nodiscard]] bool ExpectedBytes(
    const DiskImage& image, const std::uintptr_t module_base, const std::uint32_t rva,
    const std::size_t window, std::uint8_t* const out) noexcept {
    const std::size_t offset = image.FileOffset(rva);
    if (offset + window > image.bytes.size()) return false;
    std::memcpy(out, image.bytes.data() + offset, window);

    const std::uintptr_t delta =
        module_base - static_cast<std::uintptr_t>(image.headers->OptionalHeader.ImageBase);
    if (delta == 0 || image.relocations.empty()) return true;
    std::size_t cursor = 0;
    while (cursor + sizeof(IMAGE_BASE_RELOCATION) <= image.relocations.size()) {
        const auto* block =
            reinterpret_cast<const IMAGE_BASE_RELOCATION*>(image.relocations.data() + cursor);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            cursor + block->SizeOfBlock > image.relocations.size()) {
            break;
        }
        const std::size_t count =
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(std::uint16_t);
        const auto* entries = reinterpret_cast<const std::uint16_t*>(
            image.relocations.data() + cursor + sizeof(IMAGE_BASE_RELOCATION));
        for (std::size_t index = 0; index < count; ++index) {
            const std::uint16_t entry = entries[index];
            if (static_cast<std::uint16_t>(entry >> 12) != IMAGE_REL_BASED_DIR64) continue;
            const std::uint32_t target = block->VirtualAddress + (entry & 0x0FFFu);
            if (target < rva || target + sizeof(std::uint64_t) > rva + window) continue;
            std::uint64_t value{};
            std::memcpy(&value, out + (target - rva), sizeof(value));
            value += delta;
            std::memcpy(out + (target - rva), &value, sizeof(value));
        }
        cursor += block->SizeOfBlock;
    }
    return true;
}

// Every page in the window must be committed and readable before it is read: a target near a section end could
// otherwise read into an unmapped page.
[[nodiscard]] bool Readable(const std::uintptr_t address, const std::size_t window) noexcept {
    std::uintptr_t cursor = address;
    const std::uintptr_t end = address + window;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) == 0) {
            return false;
        }
        if (region.State != MEM_COMMIT || region.Protect == 0 ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        cursor = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

void WritePrologue(
    const std::uintptr_t address, const std::uint8_t* const bytes, const std::size_t window) noexcept {
    DWORD previous{};
    if (VirtualProtect(reinterpret_cast<void*>(address), window, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        Trace("0x%llX: VirtualProtect failed (%lu)", static_cast<unsigned long long>(address), GetLastError());
        return;
    }
    std::memcpy(reinterpret_cast<void*>(address), bytes, window);
    DWORD ignored{};
    VirtualProtect(reinterpret_cast<void*>(address), window, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), window);
}

// The jump target of the hook currently in place, for the trace only.
[[nodiscard]] std::uintptr_t HookTarget(const std::uint8_t* const memory) noexcept {
    if (memory[0] == 0xE9) {
        std::int32_t relative{};
        std::memcpy(&relative, memory + 1, sizeof(relative));
        return reinterpret_cast<std::uintptr_t>(memory) + 5 +
            static_cast<std::uintptr_t>(static_cast<std::intptr_t>(relative));
    }
    if (memory[0] == 0x48 && memory[1] == 0xB8 && memory[11] == 0xFF && memory[12] == 0xE0) {
        std::uintptr_t target{};
        std::memcpy(&target, memory + 2, sizeof(target));
        return target;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// threads
// ---------------------------------------------------------------------------

struct ImageRange final {
    std::uintptr_t begin{};
    std::uintptr_t end{};
    [[nodiscard]] bool Valid() const noexcept { return begin != 0 && end > begin; }
};

struct FrozenThread final {
    DWORD id{};
    HANDLE handle{};
};

// Threads this code suspended (and may therefore release), and threads it deliberately left running or found
// already suspended by somebody else -- neither of which may ever be resumed here.
struct ThreadLedger final {
    std::vector<FrozenThread> frozen;
    std::vector<DWORD> kept;
    std::vector<DWORD> borrowed;
};

[[nodiscard]] ImageRange AntiCheatRange() noexcept {
    ImageRange range;
    const HMODULE module = GetModuleHandleW(kAntiCheatModule);
    if (module == nullptr) return range;
    MODULEINFO information{};
    if (GetModuleInformation(GetCurrentProcess(), module, &information, sizeof(information)) == FALSE) {
        return range;
    }
    range.begin = reinterpret_cast<std::uintptr_t>(information.lpBaseOfDll);
    range.end = range.begin + information.SizeOfImage;
    return range;
}

// The module leaf name and offset a start address belongs to, for the freeze rules. False when the address
// is not inside a loader-mapped module.
[[nodiscard]] bool SolveStart(
    const std::uintptr_t address, char* const module, const std::size_t size,
    std::uintptr_t& offset) noexcept {
    HMODULE owner = nullptr;
    if (address == 0 ||
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &owner) == 0 ||
        owner == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(owner, path, MAX_PATH) == 0) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* leaf = slash != nullptr ? slash + 1 : path;
    char narrow[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, leaf, -1, narrow, sizeof(narrow), nullptr, nullptr);
    std::snprintf(module, size, "%s", narrow);
    offset = address - reinterpret_cast<std::uintptr_t>(owner);
    return module[0] != '\0';
}

[[nodiscard]] bool MatchesFreezeRule(
    const char* const module, const std::uintptr_t offset) noexcept {
    if (module[0] == '\0') return false;
    for (const auto& rule : kFreezeRules) {
        if (rule.offset == offset && _stricmp(rule.module, module) == 0) return true;
    }
    return false;
}

// Generated code: committed, private, executable memory. Every loader-mapped module is MEM_IMAGE, so this only
// matches code the anti-cheat built for itself at runtime -- the watchdog payloads whose threads do not start
// inside its image, which is how an image-scoped freeze gets bypassed.
[[nodiscard]] bool IsGeneratedCode(const std::uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) == 0) return false;
    if (region.State != MEM_COMMIT || region.Type != MEM_PRIVATE) return false;
    switch (region.Protect & 0xFFu) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

// The image this code runs from: its own threads must never be frozen, and the image it runs from has no
// headers to ask about its extent.
[[nodiscard]] bool InOwnImage(const std::uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION own{};
    if (VirtualQuery(reinterpret_cast<const void*>(&IsGeneratedCode), &own, sizeof(own)) == 0) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) == 0) return false;
    return region.AllocationBase == own.AllocationBase;
}

// Walks only this process's threads: the Toolhelp snapshot walks every process under a global lock, which at
// this cadence is a visible periodic stutter.
void ForEachThread(const NtGetNextThreadFn next, const std::function<void(HANDLE, DWORD)>& visit) {
    HANDLE current = nullptr;
    HANDLE following = nullptr;
    while (next(
               GetCurrentProcess(), current, THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, 0, 0,
               &following) >= 0) {
        if (current != nullptr) CloseHandle(current);
        current = following;
        following = nullptr;
        if (current == nullptr) break;
        const DWORD id = GetThreadId(current);
        if (id != 0) visit(current, id);
    }
    if (current != nullptr) CloseHandle(current);
}

// One freeze pass. A thread stays frozen once suspended: the point is that the anti-cheat's writers and
// watchdogs are not running while prologues are put back and verified, and a watchdog released between passes
// sees the restored state and treats it as tampering.
void FreezeWriters(
    ThreadLedger& ledger, const ImageRange& anti_cheat, const NtQueryInformationThreadFn query,
    const NtGetNextThreadFn next) {
    if (query == nullptr || next == nullptr) return;
    const DWORD self = GetCurrentThreadId();
    ForEachThread(next, [&](const HANDLE thread, const DWORD id) {
        if (id == self) return;
        const auto recorded = [&ledger, id](const std::vector<FrozenThread>& list) {
            return std::any_of(list.begin(), list.end(), [id](const FrozenThread& entry) {
                return entry.id == id;
            });
        };
        const auto listed = [id](const std::vector<DWORD>& list) {
            return std::find(list.begin(), list.end(), id) != list.end();
        };
        if (recorded(ledger.frozen) || listed(ledger.kept) || listed(ledger.borrowed)) return;

        void* start_raw = nullptr;
        if (query(thread, kThreadQuerySetWin32StartAddress, &start_raw, sizeof(start_raw), nullptr) < 0 ||
            start_raw == nullptr) {
            return;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(start_raw);
        if (InOwnImage(start)) {
            ledger.kept.push_back(id);
            return;
        }
        const bool in_anti_cheat =
            anti_cheat.Valid() && start >= anti_cheat.begin && start < anti_cheat.end;
        char module[MAX_PATH]{};
        std::uintptr_t module_offset{};
        const bool ruled = SolveStart(start, module, sizeof(module), module_offset) &&
            MatchesFreezeRule(module, module_offset);
        const bool generated = kFreezeGeneratedCode && IsGeneratedCode(start);
        if (!in_anti_cheat && !ruled && !generated) return;

        // A count above zero means somebody else already stopped it: undo this suspension and remember it, so a
        // suspension this code did not take is never released here.
        if (SuspendThread(thread) > 0) {
            ResumeThread(thread);
            ledger.borrowed.push_back(id);
            Trace("thread %lu (start 0x%llX) already suspended elsewhere; left alone", id,
                static_cast<unsigned long long>(start));
            return;
        }
        HANDLE owned = nullptr;
        if (DuplicateHandle(
                GetCurrentProcess(), thread, GetCurrentProcess(), &owned, THREAD_SUSPEND_RESUME, FALSE, 0) ==
            FALSE) {
            ResumeThread(thread);
            return;
        }
        ledger.frozen.push_back(FrozenThread{id, owned});
        if (ruled) {
            Trace("frozen: thread %lu (start %s+0x%llX) [rule]", id, module,
                static_cast<unsigned long long>(module_offset));
        } else {
            Trace("frozen: thread %lu (start 0x%llX) %s", id, static_cast<unsigned long long>(start),
                in_anti_cheat ? "[image]" : "[generated code]");
        }
    });
}

void ReleaseWriters(ThreadLedger& ledger) {
    for (auto& entry : ledger.frozen) {
        ResumeThread(entry.handle);
        CloseHandle(entry.handle);
    }
    ledger.frozen.clear();
}

// ---------------------------------------------------------------------------
// restore
// ---------------------------------------------------------------------------

struct TargetState final {
    bool hooked{};
    std::uintptr_t address{};
};

struct Watch final {
    ThreadLedger threads;
    std::vector<std::pair<std::string, DiskImage>> images;
    std::vector<TargetState> states;
    std::uint64_t passes{};
    std::uint64_t restored{};
    std::uint64_t rehooks{};
};

[[nodiscard]] DiskImage& CachedImage(Watch& watch, const std::string& module, const HMODULE handle) {
    for (auto& cached : watch.images) {
        if (cached.first == module) return cached.second;
    }
    wchar_t path[MAX_PATH]{};
    DiskImage image;
    if (GetModuleFileNameW(handle, path, MAX_PATH) != 0) {
        image = LoadDiskImage(path);
    } else {
        Trace("target module %s: GetModuleFileNameW failed", module.c_str());
    }
    watch.images.emplace_back(module, std::move(image));
    return watch.images.back().second;
}

// One target. `first_pass` logs every target as the baseline; later passes log only state changes, so a site
// that stays hooked does not repeat itself four times a second.
void RestoreTarget(Watch& watch, const std::size_t index, const bool first_pass) {
    const Target& target = kTargets[index];
    TargetState& state = watch.states[index];
    const HMODULE module = GetModuleHandleA(target.module);
    if (module == nullptr) return;
    const auto module_base = reinterpret_cast<std::uintptr_t>(module);
    DiskImage& image = CachedImage(watch, target.module, module);
    if (!image.Valid()) return;

    // The file must be the image that is actually loaded: a TimeDateStamp or SizeOfImage mismatch means those
    // bytes belong to another build and must not be copied anywhere.
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->FileHeader.TimeDateStamp != image.time_date_stamp ||
        headers->OptionalHeader.SizeOfImage != image.size_of_image) {
        if (first_pass) {
            Trace("target %s!%s: on-disk file does not match the loaded module; skipped", target.module,
                target.function);
        }
        return;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(GetProcAddress(module, target.function));
    if (address == 0) return;
    state.address = address;
    // A forwarded export resolves inside another module, so the RVA computed against this one is nonsense.
    if (address < module_base || address >= module_base + headers->OptionalHeader.SizeOfImage) {
        if (first_pass) {
            Trace("target %s!%s: export is forwarded outside the module; skipped", target.module,
                target.function);
        }
        return;
    }

    const std::uint32_t rva = static_cast<std::uint32_t>(address - module_base);
    std::uint8_t expected[kRestoreBytes]{};
    if (!ExpectedBytes(image, module_base, rva, kRestoreBytes, expected)) {
        if (first_pass) {
            Trace("target %s!%s rva=0x%X: prologue is not fully file-backed; skipped", target.module,
                target.function, rva);
        }
        return;
    }
    if (!Readable(address, kRestoreBytes)) {
        if (first_pass) {
            Trace("target %s!%s rva=0x%X: entry window is not fully mapped; skipped", target.module,
                target.function, rva);
        }
        return;
    }

    std::uint8_t current[kRestoreBytes]{};
    std::memcpy(current, reinterpret_cast<const void*>(address), kRestoreBytes);
    const bool hooked = std::memcmp(current, expected, kRestoreBytes) != 0;
    const bool report = first_pass || hooked != state.hooked;
    if (!hooked) {
        if (report) Trace("target %s!%s rva=0x%X: clean", target.module, target.function, rva);
        state.hooked = false;
        return;
    }

    // Hooked again after this code restored it is the one event that must never pass silently.
    const bool rehooked = !first_pass && !state.hooked;
    if (report) {
        const std::uintptr_t jump = HookTarget(current);
        Trace("target %s!%s rva=0x%X addr=0x%llX: HOOKED first byte %02X jump=0x%llX%s", target.module,
            target.function, rva, static_cast<unsigned long long>(address), current[0],
            static_cast<unsigned long long>(jump), rehooked ? " [re-hooked after restore]" : "");
        if (rehooked) ++watch.rehooks;
    }

    WritePrologue(address, expected, kRestoreBytes);
    ++watch.restored;

    std::uint8_t verify[kRestoreBytes]{};
    std::memcpy(verify, reinterpret_cast<const void*>(address), kRestoreBytes);
    const bool verified = std::memcmp(verify, expected, kRestoreBytes) == 0;
    state.hooked = !verified;
    // Written only when the outcome differed from the pass before: a re-hook every 250 ms would otherwise fill
    // the file with one line per site per pass, and the count is what matters, not the repetition.
    if (report || !verified) {
        Trace("  %s %zu bytes at 0x%llX", verified ? "restored" : "restore FAILED verification of",
            kRestoreBytes, static_cast<unsigned long long>(address));
    }
}

void Pass(Watch& watch, const bool first_pass) {
    const ImageRange anti_cheat = AntiCheatRange();
    if (!anti_cheat.Valid()) return;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query =
        reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    const auto next = reinterpret_cast<NtGetNextThreadFn>(GetProcAddress(ntdll, "NtGetNextThread"));
    // Freeze first: a restore done with the writers running is reverted, or noticed, in seconds.
    FreezeWriters(watch.threads, anti_cheat, query, next);
    for (std::size_t index = 0; index < std::size(kTargets); ++index) {
        RestoreTarget(watch, index, first_pass);
    }
    ++watch.passes;
}

}  // namespace

AceUnhookReport StartAntiCheatHookRestore(
    const std::filesystem::path& trace_directory, const std::chrono::milliseconds watch_window) noexcept {
    AceUnhookReport report;
    if (!AntiCheatRange().Valid()) return report;
    if (!trace_directory.empty()) {
        g_trace_path = (trace_directory / L"ace-unhook-trace.txt").wstring();
    }
    auto watch = std::make_shared<Watch>();
    watch->states.resize(std::size(kTargets));
    Trace("restore started (pid %lu, window %s)", GetCurrentProcessId(),
        watch_window.count() > 0 ? "bounded" : "whole session");

    // The first pass runs here, so the loader entry is the real one before the first service loads anything.
    Pass(*watch, true);
    report.available = true;
    report.passes = static_cast<unsigned long>(watch->passes);
    report.restored = static_cast<unsigned long>(watch->restored);
    report.rehooks = static_cast<unsigned long>(watch->rehooks);

    // The watch runs off the calling thread, and the threads it froze stay frozen for its whole life: that is
    // what keeps a watchdog from re-checking the prologues between passes.
    const ULONGLONG started = GetTickCount64();
    const auto window = watch_window.count();
    std::thread([watch, started, window] {
        for (;;) {
            Sleep(kPassIntervalMs);
            Pass(*watch, false);
            if (window > 0 && GetTickCount64() - started >= static_cast<ULONGLONG>(window)) break;
        }
        Trace("watch finished: passes=%llu restored=%llu rehooks=%llu",
            static_cast<unsigned long long>(watch->passes),
            static_cast<unsigned long long>(watch->restored),
            static_cast<unsigned long long>(watch->rehooks));
    }).detach();
    return report;
}

}  // namespace anomaly
