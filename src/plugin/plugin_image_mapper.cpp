#include "anomaly/plugin_image_mapper.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

constexpr std::size_t kMaximumImageBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumSections = 96U;
constexpr std::size_t kMaximumImportDescriptors = 4096U;
constexpr std::size_t kMaximumImportNameBytes = 512U;
constexpr std::size_t kMaximumSiblingDepth = 8U;
constexpr std::size_t kMaximumExportNameBytes = 512U;

// The stock vcruntime helper derives the throwing module's base through the
// loader, which cannot resolve a manually mapped image. Every C++ plugin
// therefore exports this bridge (added by the SDK's anomaly_add_plugin) and
// the mapper rebinds the _CxxThrowException import to it.
constexpr char kCxxThrowImportName[] = "_CxxThrowException";
constexpr char kCxxThrowBridgeExportName[] = "AnomalyPluginCxxThrowV1";

using EntryFn = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
using AddFunctionTableFn = BOOLEAN(WINAPI*)(PVOID, DWORD, ULONG64);
using DeleteFunctionTableFn = BOOLEAN(WINAPI*)(PVOID);

AddFunctionTableFn AddFunctionTable() noexcept {
    static const auto fn = reinterpret_cast<AddFunctionTableFn>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "RtlAddFunctionTable"));
    return fn;
}

DeleteFunctionTableFn DeleteFunctionTable() noexcept {
    static const auto fn = reinterpret_cast<DeleteFunctionTableFn>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "RtlDeleteFunctionTable"));
    return fn;
}

[[nodiscard]] std::wstring Lowercase(std::wstring value) noexcept {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

[[nodiscard]] std::optional<std::wstring> CanonicalImageKey(
    const std::filesystem::path& path) noexcept {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::canonical(path, error);
    if (error) return std::nullopt;
    return Lowercase(canonical.wstring());
}

// std::strnlen is not in MSVC's <cstring>; bound it by hand.
[[nodiscard]] std::size_t BoundedLength(const char* value, std::size_t limit) noexcept {
    for (std::size_t index = 0; index < limit; ++index) {
        if (value[index] == '\0') return index;
    }
    return limit;
}

// ---------------------------------------------------------------------------
// File image parsing and validation
// ---------------------------------------------------------------------------

struct ParsedImage final {
    std::vector<std::uint8_t> bytes;
    IMAGE_NT_HEADERS64 headers{};
    std::vector<IMAGE_SECTION_HEADER> sections;

    // Translates an RVA into a file offset; the image is not mapped yet, so
    // the section layout decides where the bytes live.
    [[nodiscard]] const std::uint8_t* FileBytes(
        std::uint32_t rva, std::size_t count) const noexcept {
        if (rva < headers.OptionalHeader.SizeOfHeaders) {
            return count <= bytes.size() - rva ? bytes.data() + rva : nullptr;
        }
        for (const IMAGE_SECTION_HEADER& section : sections) {
            const std::uint64_t start = section.VirtualAddress;
            const std::uint64_t size = (std::max)(
                static_cast<std::uint64_t>(section.Misc.VirtualSize),
                static_cast<std::uint64_t>(section.SizeOfRawData));
            if (size == 0 || rva < start || rva - start >= size) continue;
            const std::uint64_t offset =
                static_cast<std::uint64_t>(section.PointerToRawData) + (rva - start);
            return offset <= bytes.size() && count <= bytes.size() - offset
                ? bytes.data() + offset
                : nullptr;
        }
        return nullptr;
    }

    template <typename T>
    [[nodiscard]] const T* FileAt(std::uint32_t rva, std::size_t count = 1) const noexcept {
        return reinterpret_cast<const T*>(FileBytes(rva, count * sizeof(T)));
    }
};

struct ImagePolicy final {
    PluginImageMapError error{PluginImageMapError::ImageInvalid};
    std::string failure;
};

[[nodiscard]] bool ParseImage(
    const std::filesystem::path& file, ParsedImage& parsed, ImagePolicy& policy) {
    std::error_code error;
    const std::uintmax_t byte_count = std::filesystem::file_size(file, error);
    if (error || byte_count == 0 || byte_count > kMaximumImageBytes) {
        policy.error = PluginImageMapError::ImageUnavailable;
        policy.failure = "image file is unavailable or too large";
        return false;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        policy.error = PluginImageMapError::ImageUnavailable;
        policy.failure = "image file cannot be opened";
        return false;
    }
    parsed.bytes.resize(static_cast<std::size_t>(byte_count));
    input.read(reinterpret_cast<char*>(parsed.bytes.data()),
        static_cast<std::streamsize>(parsed.bytes.size()));
    if (!input || static_cast<std::size_t>(input.gcount()) != parsed.bytes.size()) {
        policy.error = PluginImageMapError::ImageUnavailable;
        policy.failure = "image file could not be read";
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(parsed.bytes.data());
    if (parsed.bytes.size() < sizeof(IMAGE_DOS_HEADER) ||
        dos->e_magic != IMAGE_DOS_SIGNATURE ||
        static_cast<std::uint64_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) >
            parsed.bytes.size()) {
        policy.failure = "image is not a PE executable";
        return false;
    }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        parsed.bytes.data() + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        policy.failure = "image is not a 64-bit PE executable";
        return false;
    }
    if ((headers->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0) {
        policy.failure = "image is not a dynamic-link library";
        return false;
    }
    const WORD section_count = headers->FileHeader.NumberOfSections;
    if (section_count == 0 || section_count > kMaximumSections) {
        policy.failure = "image section table is empty or oversized";
        return false;
    }
    const DWORD section_table_offset =
        dos->e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        headers->FileHeader.SizeOfOptionalHeader;
    if (static_cast<std::uint64_t>(section_table_offset) +
            static_cast<std::uint64_t>(section_count) * sizeof(IMAGE_SECTION_HEADER) >
        parsed.bytes.size()) {
        policy.failure = "image section table is outside the file";
        return false;
    }
    parsed.headers = *headers;
    const auto* section_table = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        parsed.bytes.data() + section_table_offset);
    parsed.sections.assign(section_table, section_table + section_count);

    const IMAGE_OPTIONAL_HEADER64& optional = parsed.headers.OptionalHeader;
    if (optional.SizeOfHeaders > parsed.bytes.size() ||
        optional.SizeOfHeaders > optional.SizeOfImage) {
        policy.failure = "image headers are outside the image";
        return false;
    }
    if (optional.AddressOfEntryPoint >= optional.SizeOfImage) {
        policy.failure = "image entry point is outside the image";
        return false;
    }
    for (const IMAGE_SECTION_HEADER& section : parsed.sections) {
        const std::uint64_t mapped_size = (std::max)(
            static_cast<std::uint64_t>(section.Misc.VirtualSize),
            static_cast<std::uint64_t>(section.SizeOfRawData));
        if (section.VirtualAddress + mapped_size > optional.SizeOfImage ||
            section.PointerToRawData + section.SizeOfRawData > parsed.bytes.size()) {
            policy.failure = "image section is outside the image";
            return false;
        }
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 &&
            (section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0) {
            policy.error = PluginImageMapError::UnsupportedImage;
            policy.failure = "image contains an executable and writable section";
            return false;
        }
    }

    const auto& directories = optional.DataDirectory;
    if (directories[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress != 0) {
        policy.error = PluginImageMapError::UnsupportedImage;
        policy.failure = "image carries a CLR header";
        return false;
    }
    if (directories[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress != 0) {
        policy.error = PluginImageMapError::UnsupportedImage;
        policy.failure = "image uses delay-load imports; rebuild without /DELAYLOAD";
        return false;
    }
    if (directories[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress != 0) {
        const auto* tls = parsed.FileAt<IMAGE_TLS_DIRECTORY64>(
            directories[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        if (tls == nullptr) {
            policy.failure = "image TLS directory is outside the image";
            return false;
        }
        if (tls->StartAddressOfRawData != tls->EndAddressOfRawData ||
            tls->SizeOfZeroFill != 0 || tls->AddressOfCallBacks != 0) {
            policy.error = PluginImageMapError::UnsupportedImage;
            policy.failure =
                "image uses loader-managed static TLS (thread_local or "
                "__declspec(thread)); rebuild with the Anomaly SDK";
            return false;
        }
    }
    return true;
}

[[nodiscard]] MappedPluginImage::Directory ToDirectory(
    const IMAGE_DATA_DIRECTORY& directory) noexcept {
    return {directory.VirtualAddress, directory.Size};
}

// ---------------------------------------------------------------------------
// Live image helpers (operate on the mapped copy)
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] T* Live(void* base, std::size_t size, std::uint32_t rva,
    std::size_t count = 1) noexcept {
    const std::uint64_t offset = rva;
    if (offset > size || count > (size - offset) / sizeof(T)) return nullptr;
    return reinterpret_cast<T*>(static_cast<std::uint8_t*>(base) + offset);
}

[[nodiscard]] void* LiveAddress(void* base, std::size_t size, std::uint32_t rva) noexcept {
    if (rva >= size) return nullptr;
    return static_cast<std::uint8_t*>(base) + rva;
}

[[nodiscard]] void* LiveExport(
    const MappedPluginImage::State& state, const char* name) noexcept {
    const MappedPluginImage::Directory& directory = state.export_directory;
    if (directory.rva == 0 || name == nullptr) return nullptr;
    const auto* exports =
        Live<IMAGE_EXPORT_DIRECTORY>(state.base, state.size, directory.rva);
    if (exports == nullptr) return nullptr;
    const auto* names = Live<DWORD>(
        state.base, state.size, exports->AddressOfNames, exports->NumberOfNames);
    const auto* ordinals = Live<WORD>(
        state.base, state.size, exports->AddressOfNameOrdinals, exports->NumberOfNames);
    const auto* functions = Live<DWORD>(
        state.base, state.size, exports->AddressOfFunctions, exports->NumberOfFunctions);
    if (names == nullptr || ordinals == nullptr || functions == nullptr) return nullptr;
    const std::size_t name_length = std::strlen(name);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const auto* candidate = Live<char>(
            state.base, state.size, names[index], kMaximumExportNameBytes);
        if (candidate == nullptr ||
            BoundedLength(candidate, kMaximumExportNameBytes) != name_length ||
            std::memcmp(candidate, name, name_length) != 0) {
            continue;
        }
        const WORD ordinal = ordinals[index];
        if (ordinal >= exports->NumberOfFunctions) return nullptr;
        const DWORD rva = functions[ordinal];
        if (rva >= directory.rva && rva < directory.rva + directory.size) {
            return nullptr;  // forwarded export; plugin entries never are
        }
        return LiveAddress(state.base, state.size, rva);
    }
    return nullptr;
}

[[nodiscard]] void* LiveExportOrdinal(
    const MappedPluginImage::State& state, WORD ordinal) noexcept {
    const MappedPluginImage::Directory& directory = state.export_directory;
    if (directory.rva == 0) return nullptr;
    const auto* exports =
        Live<IMAGE_EXPORT_DIRECTORY>(state.base, state.size, directory.rva);
    if (exports == nullptr || ordinal < exports->Base) return nullptr;
    const DWORD slot = static_cast<DWORD>(ordinal) - exports->Base;
    if (slot >= exports->NumberOfFunctions) return nullptr;
    const auto* functions = Live<DWORD>(
        state.base, state.size, exports->AddressOfFunctions, exports->NumberOfFunctions);
    if (functions == nullptr) return nullptr;
    const DWORD rva = functions[slot];
    if (rva >= directory.rva && rva < directory.rva + directory.size) {
        return nullptr;
    }
    return LiveAddress(state.base, state.size, rva);
}

void DetachState(MappedPluginImage::State& state, LPVOID reserved) noexcept {
    if (!state.attach_attempted || state.detached) return;
    state.detached = true;
    if (state.entry == nullptr) return;
    try {
        reinterpret_cast<EntryFn>(state.entry)(
            static_cast<HINSTANCE>(state.base), DLL_PROCESS_DETACH, reserved);
    } catch (...) {
        // An escaping destructor behaves as it would through FreeLibrary: the
        // teardown continues.
    }
}

void ReleaseState(MappedPluginImage::State& state) noexcept {
    if (state.base == nullptr) return;
    if (state.function_table_registered) {
        if (const auto delete_table = DeleteFunctionTable()) {
            static_cast<void>(delete_table(state.base));
        }
        state.function_table_registered = false;
    }
    static_cast<void>(VirtualFree(state.base, 0, MEM_RELEASE));
    state.base = nullptr;
}

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------

struct LoadTree final {
    // Canonical lowercase path -> mapped state. A completed entry is shared by
    // every importer of the same file; an entry still being mapped marks a
    // cyclic dependency.
    std::map<std::wstring, std::shared_ptr<MappedPluginImage::State>> images;
};

struct MapOutcome final {
    std::shared_ptr<MappedPluginImage::State> state;
    std::string failure;
    PluginImageMapError error{PluginImageMapError::InternalFailure};
};

[[nodiscard]] MapOutcome MapImageTree(
    const std::filesystem::path& file, std::uint32_t depth, LoadTree& tree);

[[nodiscard]] DWORD SectionProtection(DWORD characteristics) noexcept {
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

[[nodiscard]] bool ProtectImage(
    MappedPluginImage::State& state, const ParsedImage& parsed, std::string& failure) {
    DWORD previous{};
    if (VirtualProtect(state.base, state.size, PAGE_NOACCESS, &previous) == FALSE ||
        VirtualProtect(state.base, parsed.headers.OptionalHeader.SizeOfHeaders,
            PAGE_READONLY, &previous) == FALSE) {
        failure = "image protection could not be applied";
        return false;
    }
    for (const IMAGE_SECTION_HEADER& section : parsed.sections) {
        const std::uint64_t mapped_size = (std::max)(
            static_cast<std::uint64_t>(section.Misc.VirtualSize),
            static_cast<std::uint64_t>(section.SizeOfRawData));
        if (mapped_size == 0) continue;
        if (VirtualProtect(static_cast<std::uint8_t*>(state.base) + section.VirtualAddress,
                static_cast<SIZE_T>(mapped_size),
                SectionProtection(section.Characteristics), &previous) == FALSE) {
            failure = "image protection could not be applied";
            return false;
        }
    }
    static_cast<void>(FlushInstructionCache(GetCurrentProcess(), state.base, state.size));
    return true;
}

[[nodiscard]] bool RelocateImage(
    MappedPluginImage::State& state, const ParsedImage& parsed, std::string& failure) {
    const std::uint64_t delta = reinterpret_cast<std::uintptr_t>(state.base) -
        parsed.headers.OptionalHeader.ImageBase;
    if (delta == 0) return true;
    const IMAGE_DATA_DIRECTORY& directory =
        parsed.headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress == 0) {
        failure = "image cannot be relocated at the allocated address";
        return false;
    }
    std::uint64_t remaining = directory.Size;
    std::uint32_t offset = directory.VirtualAddress;
    while (remaining >= sizeof(IMAGE_BASE_RELOCATION)) {
        const auto* block = Live<IMAGE_BASE_RELOCATION>(state.base, state.size, offset);
        if (block == nullptr || block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block->SizeOfBlock > remaining) {
            failure = "image relocation directory is malformed";
            return false;
        }
        const std::size_t entry_count =
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        const auto* entries = reinterpret_cast<const WORD*>(block + 1);
        for (std::size_t index = 0; index < entry_count; ++index) {
            const WORD entry = entries[index];
            const WORD type = entry >> 12;
            const WORD fixup = entry & 0x0FFF;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_DIR64) {
                failure = "image uses an unsupported relocation type";
                return false;
            }
            auto* target = Live<std::uint64_t>(
                state.base, state.size, block->VirtualAddress + fixup);
            if (target == nullptr) {
                failure = "image relocation target is outside the image";
                return false;
            }
            *target += delta;
        }
        remaining -= block->SizeOfBlock;
        offset += block->SizeOfBlock;
    }
    return true;
}

[[nodiscard]] HMODULE ResolveSystemModule(const std::wstring& name) noexcept {
    HMODULE handle{};
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, name.c_str(), &handle) != FALSE &&
        handle != nullptr) {
        return handle;
    }
    // Pre-validated import names are plain Windows module names, so the search
    // can never leave the system directory. The reference is held for the
    // process lifetime: mapped images cannot participate in refcounting.
    return LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

[[nodiscard]] bool BindImports(
    MappedPluginImage::State& state, const ParsedImage& parsed, std::uint32_t depth,
    LoadTree& tree, std::string& failure, PluginImageMapError& error) {
    const IMAGE_DATA_DIRECTORY& directory =
        parsed.headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) return true;

    void* const bridge = LiveExport(state, kCxxThrowBridgeExportName);
    std::error_code filesystem_error;
    for (DWORD descriptor_index = 0; descriptor_index < kMaximumImportDescriptors;
         ++descriptor_index) {
        const auto* descriptor = Live<IMAGE_IMPORT_DESCRIPTOR>(
            state.base, state.size, directory.VirtualAddress +
                descriptor_index * static_cast<DWORD>(sizeof(IMAGE_IMPORT_DESCRIPTOR)));
        if (descriptor == nullptr) {
            failure = "image import directory is malformed";
            return false;
        }
        if (descriptor->Name == 0) break;
        const auto* name_bytes = Live<char>(
            state.base, state.size, descriptor->Name, kMaximumImportNameBytes);
        if (name_bytes == nullptr ||
            std::memchr(name_bytes, '\0', kMaximumImportNameBytes) == nullptr) {
            failure = "image import module name is invalid";
            return false;
        }
        const std::string module_name(name_bytes);
        const std::wstring wide_name(module_name.begin(), module_name.end());
        const std::filesystem::path sibling = state.source.parent_path() / wide_name;

        std::shared_ptr<MappedPluginImage::State> dependency;
        HMODULE system_module{};
        if (std::filesystem::is_regular_file(sibling, filesystem_error) && !filesystem_error) {
            MapOutcome sibling_outcome = MapImageTree(sibling, depth + 1, tree);
            if (sibling_outcome.state == nullptr) {
                error = sibling_outcome.error;
                failure = "package dependency '" + module_name + "' could not be mapped";
                if (!sibling_outcome.failure.empty()) {
                    failure += ": " + sibling_outcome.failure;
                }
                return false;
            }
            dependency = std::move(sibling_outcome.state);
        } else {
            system_module = ResolveSystemModule(wide_name);
            if (system_module == nullptr) {
                failure = "import module '" + module_name + "' could not be resolved";
                return false;
            }
        }

        const DWORD thunk_rva = descriptor->OriginalFirstThunk != 0
            ? descriptor->OriginalFirstThunk
            : descriptor->FirstThunk;
        std::uint32_t thunk_index = 0;
        for (;; ++thunk_index) {
            if (thunk_index > state.size / 8) {
                failure = "image import thunk table is not terminated";
                return false;
            }
            const auto* lookup = Live<std::uint64_t>(
                state.base, state.size, thunk_rva + thunk_index * 8);
            auto* slot = Live<std::uint64_t>(
                state.base, state.size, descriptor->FirstThunk + thunk_index * 8);
            if (lookup == nullptr || slot == nullptr) {
                failure = "image import thunk table is malformed";
                return false;
            }
            if (*lookup == 0) break;
            void* resolved{};
            if (IMAGE_SNAP_BY_ORDINAL64(*lookup)) {
                const WORD ordinal = static_cast<WORD>(IMAGE_ORDINAL64(*lookup));
                resolved = dependency != nullptr
                    ? LiveExportOrdinal(*dependency, ordinal)
                    : reinterpret_cast<void*>(
                          GetProcAddress(system_module, MAKEINTRESOURCEA(ordinal)));
            } else {
                const auto* function = Live<IMAGE_IMPORT_BY_NAME>(
                    state.base, state.size, static_cast<std::uint32_t>(*lookup));
                if (function == nullptr ||
                    std::memchr(function->Name, '\0', kMaximumImportNameBytes) == nullptr) {
                    failure = "image import name is invalid";
                    return false;
                }
                if (std::strcmp(function->Name, kCxxThrowImportName) == 0) {
                    if (bridge == nullptr) {
                        error = PluginImageMapError::BridgeMissing;
                        failure =
                            "C++ plugin image does not export the exception bridge; "
                            "rebuild with the Anomaly SDK";
                        return false;
                    }
                    resolved = bridge;
                } else {
                    resolved = dependency != nullptr
                        ? LiveExport(*dependency, function->Name)
                        : reinterpret_cast<void*>(
                              GetProcAddress(system_module, function->Name));
                }
            }
            if (resolved == nullptr) {
                failure = "import '" + module_name + "' could not be resolved";
                return false;
            }
            *slot = reinterpret_cast<std::uint64_t>(resolved);
        }
        if (dependency != nullptr) {
            state.dependencies.push_back(std::move(dependency));
        }
    }
    return true;
}

[[nodiscard]] MapOutcome MapImageTree(
    const std::filesystem::path& file, std::uint32_t depth, LoadTree& tree) {
    MapOutcome outcome;
    if (depth > kMaximumSiblingDepth) {
        outcome.error = PluginImageMapError::ImportFailure;
        outcome.failure = "package dependency nesting is too deep";
        return outcome;
    }
    const auto key = CanonicalImageKey(file);
    if (!key) {
        outcome.error = PluginImageMapError::ImageUnavailable;
        outcome.failure = "image path could not be resolved";
        return outcome;
    }
    const auto existing = tree.images.find(*key);
    if (existing != tree.images.end()) {
        if (existing->second == nullptr || !existing->second->mapping_complete) {
            outcome.error = PluginImageMapError::ImportFailure;
            outcome.failure =
                "package dependency cycle at '" + file.filename().string() + "'";
            return outcome;
        }
        outcome.error = PluginImageMapError::None;
        outcome.state = existing->second;
        return outcome;
    }

    ParsedImage parsed;
    ImagePolicy policy;
    if (!ParseImage(file, parsed, policy)) {
        outcome.error = policy.error;
        outcome.failure = policy.failure;
        return outcome;
    }

    auto state = std::make_shared<MappedPluginImage::State>();
    state->size = parsed.headers.OptionalHeader.SizeOfImage;
    state->headers_size = parsed.headers.OptionalHeader.SizeOfHeaders;
    state->export_directory =
        ToDirectory(parsed.headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]);
    state->exception_directory = ToDirectory(
        parsed.headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]);
    state->debug_directory =
        ToDirectory(parsed.headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]);
    state->source = file;
    // Register before mapping so a cyclic import sees an in-progress entry.
    tree.images.emplace(*key, state);

    const auto fail = [&](PluginImageMapError error_code, std::string message) {
        tree.images.erase(*key);
        outcome.error = error_code;
        outcome.failure = std::move(message);
        return outcome;
    };

    state->base = VirtualAlloc(nullptr, state->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (state->base == nullptr) {
        return fail(PluginImageMapError::AllocationFailure, "image allocation failed");
    }
    std::memcpy(state->base, parsed.bytes.data(),
        static_cast<std::size_t>(parsed.headers.OptionalHeader.SizeOfHeaders));
    for (const IMAGE_SECTION_HEADER& section : parsed.sections) {
        if (section.SizeOfRawData == 0) continue;
        std::memcpy(static_cast<std::uint8_t*>(state->base) + section.VirtualAddress,
            parsed.bytes.data() + section.PointerToRawData, section.SizeOfRawData);
    }

    std::string failure;
    PluginImageMapError error = PluginImageMapError::InternalFailure;
    if (!RelocateImage(*state, parsed, failure)) {
        return fail(PluginImageMapError::RelocationFailure, std::move(failure));
    }
    if (!BindImports(*state, parsed, depth, tree, failure, error)) {
        return fail(error, std::move(failure));
    }
    if (!ProtectImage(*state, parsed, failure)) {
        return fail(PluginImageMapError::InternalFailure, std::move(failure));
    }

    if (state->exception_directory.rva != 0 && state->exception_directory.size != 0) {
        const auto add_table = AddFunctionTable();
        const DWORD entry_count = state->exception_directory.size / sizeof(RUNTIME_FUNCTION);
        if (add_table == nullptr ||
            add_table(LiveAddress(state->base, state->size, state->exception_directory.rva),
                entry_count, reinterpret_cast<ULONG64>(state->base)) == FALSE) {
            return fail(PluginImageMapError::InternalFailure,
                "exception function table could not be registered");
        }
        state->function_table_registered = true;
    }

    if (parsed.headers.OptionalHeader.AddressOfEntryPoint != 0) {
        state->entry = LiveAddress(
            state->base, state->size, parsed.headers.OptionalHeader.AddressOfEntryPoint);
        if (state->entry == nullptr) {
            return fail(PluginImageMapError::ImageInvalid, "image entry point is invalid");
        }
        state->attach_attempted = true;
        BOOL accepted = FALSE;
        try {
            accepted = reinterpret_cast<EntryFn>(state->entry)(
                static_cast<HINSTANCE>(state->base), DLL_PROCESS_ATTACH, nullptr);
        } catch (...) {
            return fail(PluginImageMapError::EntryRejected,
                "image entry point raised an exception during attach");
        }
        if (accepted == FALSE) {
            return fail(PluginImageMapError::EntryRejected,
                "image entry point rejected process attach");
        }
        state->attach_succeeded = true;
    }

    state->mapping_complete = true;
    outcome.error = PluginImageMapError::None;
    outcome.state = std::move(state);
    return outcome;
}

}  // namespace

MappedPluginImage::State::~State() {
    // A failed attach detaches with the termination convention, so the CRT
    // skips its atexit table exactly like a rejected LoadLibrary would.
    DetachState(*this, attach_succeeded ? nullptr : reinterpret_cast<LPVOID>(1));
    // The own detach runs first: atexit callbacks may still call into the
    // dependencies. Members release afterwards, so a shared dependency stays
    // mapped until its last importer is detached.
    dependencies.clear();
    ReleaseState(*this);
}

MappedPluginImage::MappedPluginImage(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

MappedPluginImage::~MappedPluginImage() = default;

void* MappedPluginImage::Base() const noexcept {
    return state_ ? state_->base : nullptr;
}

std::size_t MappedPluginImage::Size() const noexcept {
    return state_ ? state_->size : 0;
}

PluginImageMapResult MapPluginImage(const std::filesystem::path& entry_file) noexcept {
    PluginImageMapResult result;
    try {
        LoadTree tree;
        MapOutcome outcome = MapImageTree(entry_file, 0, tree);
        if (outcome.state == nullptr) {
            result.error = outcome.error;
            result.message = std::move(outcome.failure);
            return result;
        }
        result.error = PluginImageMapError::None;
        result.image = std::shared_ptr<MappedPluginImage>(
            new MappedPluginImage(std::move(outcome.state)));
        return result;
    } catch (const std::exception& exception) {
        result.error = PluginImageMapError::InternalFailure;
        result.message = std::string("image mapping failed: ") + exception.what();
        return result;
    } catch (...) {
        result.error = PluginImageMapError::InternalFailure;
        result.message = "image mapping failed unexpectedly";
        return result;
    }
}

void* FindPluginImageExport(const MappedPluginImage& image, const char* name) noexcept {
    if (image.state_ == nullptr) return nullptr;
    return LiveExport(*image.state_, name);
}

void EraseMappedImageHeaders(const MappedPluginImage& image) noexcept {
    if (image.state_ == nullptr) return;
    // Shared dependencies of one tree may be reached twice; erase each once.
    std::vector<MappedPluginImage::State*> pending{image.state_.get()};
    std::vector<MappedPluginImage::State*> visited;
    while (!pending.empty()) {
        MappedPluginImage::State* state = pending.back();
        pending.pop_back();
        if (std::find(visited.begin(), visited.end(), state) != visited.end()) continue;
        visited.push_back(state);
        for (const auto& dependency : state->dependencies) {
            if (dependency != nullptr) pending.push_back(dependency.get());
        }

        struct Range final {
            std::uint32_t rva;
            std::size_t bytes;
        };
        std::vector<Range> ranges;
        ranges.push_back({0, state->headers_size});
        if (state->export_directory.rva != 0 && state->export_directory.size != 0) {
            ranges.push_back({state->export_directory.rva, state->export_directory.size});
        }
        if (state->debug_directory.rva != 0 && state->debug_directory.size != 0) {
            const DWORD entry_count =
                state->debug_directory.size / sizeof(IMAGE_DEBUG_DIRECTORY);
            if (const auto* debug = Live<IMAGE_DEBUG_DIRECTORY>(
                    state->base, state->size, state->debug_directory.rva, entry_count);
                debug != nullptr) {
                for (DWORD index = 0; index < entry_count; ++index) {
                    if (debug[index].AddressOfRawData != 0 && debug[index].SizeOfData != 0) {
                        ranges.push_back(
                            {debug[index].AddressOfRawData, debug[index].SizeOfData});
                    }
                }
                ranges.push_back({state->debug_directory.rva, state->debug_directory.size});
            }
        }
        // The relocation range is read from the still-intact headers; the
        // section table itself is zeroed as part of the header range.
        if (const auto* headers = Live<IMAGE_NT_HEADERS64>(state->base, state->size, 0);
            headers != nullptr) {
            const auto& directory =
                headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (directory.VirtualAddress != 0 && directory.Size != 0) {
                ranges.push_back({directory.VirtualAddress, directory.Size});
            }
        }

        for (const Range& range : ranges) {
            if (range.rva >= state->size || range.bytes > state->size - range.rva) continue;
            void* target = static_cast<std::uint8_t*>(state->base) + range.rva;
            DWORD previous{};
            if (VirtualProtect(target, range.bytes, PAGE_READWRITE, &previous) == FALSE) {
                continue;
            }
            std::memset(target, 0, range.bytes);
            static_cast<void>(VirtualProtect(target, range.bytes, previous, &previous));
        }
        static_cast<void>(
            FlushInstructionCache(GetCurrentProcess(), state->base, state->size));
    }
}

}  // namespace anomaly
