#include "anomaly/plugin_native_dependency.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

constexpr std::size_t kMaximumImageBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumImportDescriptors = 4096U;
constexpr std::size_t kMaximumImportNameBytes = 512U;
constexpr std::size_t kMaximumSections = 96U;

struct PeImage {
    std::vector<std::uint8_t> bytes;
    DWORD size_of_headers{};
    std::array<IMAGE_DATA_DIRECTORY, IMAGE_NUMBEROF_DIRECTORY_ENTRIES> directories{};
    std::vector<IMAGE_SECTION_HEADER> sections;

    template <typename T>
    [[nodiscard]] bool Read(std::size_t offset, T& value) const noexcept {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return true;
    }

    [[nodiscard]] std::optional<std::size_t> OffsetFromRva(DWORD rva) const noexcept {
        if (rva < size_of_headers && rva < bytes.size()) {
            return static_cast<std::size_t>(rva);
        }
        for (const IMAGE_SECTION_HEADER& section : sections) {
            const std::uint64_t start = section.VirtualAddress;
            const std::uint64_t size = (std::max)(
                static_cast<std::uint64_t>(section.Misc.VirtualSize),
                static_cast<std::uint64_t>(section.SizeOfRawData));
            const std::uint64_t value = rva;
            if (size == 0 || value < start || value - start >= size) continue;
            const std::uint64_t offset =
                static_cast<std::uint64_t>(section.PointerToRawData) + value - start;
            if (offset >= bytes.size()) return std::nullopt;
            return static_cast<std::size_t>(offset);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> AsciiAtRva(DWORD rva) const {
        const auto offset = OffsetFromRva(rva);
        if (!offset) return std::nullopt;
        const std::size_t available = bytes.size() - *offset;
        const std::size_t length = (std::min)(available, kMaximumImportNameBytes);
        const auto* first = reinterpret_cast<const char*>(bytes.data() + *offset);
        const auto* terminator = static_cast<const char*>(
            std::memchr(first, '\0', length));
        if (terminator == nullptr || terminator == first) return std::nullopt;
        return std::string(first, terminator);
    }
};

struct LoadedModule {
    std::wstring name;
    std::filesystem::path path;
};

[[nodiscard]] bool EqualInsensitive(
    std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](wchar_t a, wchar_t b) {
            return std::towlower(a) == std::towlower(b);
        });
}

[[nodiscard]] std::wstring Lowercase(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

[[nodiscard]] bool HasPrefix(std::wstring_view value, std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size() &&
        EqualInsensitive(value.substr(0, prefix.size()), prefix);
}

[[nodiscard]] std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::absolute(path, error);
    if (error) return path.lexically_normal();
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(normalized, error);
    return error ? normalized.lexically_normal() : canonical;
}

[[nodiscard]] bool SamePath(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    return EqualInsensitive(NormalizedPath(left).wstring(), NormalizedPath(right).wstring());
}

[[nodiscard]] bool IsRegularFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] std::optional<PeImage> ReadPeImage(
    const std::filesystem::path& path,
    std::string& failure) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        failure = "image could not be opened";
        return std::nullopt;
    }
    const std::streamoff length = input.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > kMaximumImageBytes) {
        failure = "image size is invalid or exceeds the preflight limit";
        return std::nullopt;
    }

    PeImage image;
    image.bytes.resize(static_cast<std::size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!input.read(
            reinterpret_cast<char*>(image.bytes.data()),
            static_cast<std::streamsize>(image.bytes.size()))) {
        failure = "image could not be read";
        return std::nullopt;
    }

    IMAGE_DOS_HEADER dos{};
    if (!image.Read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        failure = "image is missing a valid DOS header";
        return std::nullopt;
    }
    const std::size_t nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    DWORD signature{};
    IMAGE_FILE_HEADER file_header{};
    if (!image.Read(nt_offset, signature) || signature != IMAGE_NT_SIGNATURE ||
        !image.Read(nt_offset + sizeof(signature), file_header)) {
        failure = "image is missing a valid NT header";
        return std::nullopt;
    }
    if (file_header.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        file_header.NumberOfSections == 0 || file_header.NumberOfSections > kMaximumSections ||
        file_header.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) {
        failure = "image is not a supported x64 PE image";
        return std::nullopt;
    }
    const std::size_t optional_offset = nt_offset + sizeof(signature) + sizeof(file_header);
    IMAGE_OPTIONAL_HEADER64 optional_header{};
    if (!image.Read(optional_offset, optional_header) ||
        optional_header.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        failure = "image is not a PE32+ image";
        return std::nullopt;
    }
    image.size_of_headers = optional_header.SizeOfHeaders;
    const DWORD directory_count = (std::min)(
        optional_header.NumberOfRvaAndSizes,
        static_cast<DWORD>(image.directories.size()));
    for (DWORD index = 0; index < directory_count; ++index) {
        image.directories[index] = optional_header.DataDirectory[index];
    }

    const std::size_t sections_offset = optional_offset + file_header.SizeOfOptionalHeader;
    image.sections.resize(file_header.NumberOfSections);
    for (std::size_t index = 0; index < image.sections.size(); ++index) {
        if (!image.Read(
                sections_offset + index * sizeof(IMAGE_SECTION_HEADER), image.sections[index])) {
            failure = "image section table is truncated";
            return std::nullopt;
        }
    }
    return image;
}

[[nodiscard]] bool IsEmpty(const IMAGE_IMPORT_DESCRIPTOR& descriptor) noexcept {
    return descriptor.OriginalFirstThunk == 0 && descriptor.TimeDateStamp == 0 &&
        descriptor.ForwarderChain == 0 && descriptor.Name == 0 && descriptor.FirstThunk == 0;
}

template <typename Descriptor, typename IsTerminator, typename NameRva>
[[nodiscard]] bool CollectImports(
    const PeImage& image,
    DWORD directory_index,
    IsTerminator&& is_terminator,
    NameRva&& name_rva,
    std::vector<std::string>& imports,
    std::string& failure) {
    if (directory_index >= image.directories.size()) return true;
    const IMAGE_DATA_DIRECTORY& directory = image.directories[directory_index];
    if (directory.VirtualAddress == 0 && directory.Size == 0) return true;
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(Descriptor)) {
        failure = "PE import directory is malformed";
        return false;
    }
    const std::size_t descriptor_count = (std::min)(
        static_cast<std::size_t>(directory.Size / sizeof(Descriptor)),
        kMaximumImportDescriptors);
    if (descriptor_count == 0) {
        failure = "PE import directory is empty";
        return false;
    }
    for (std::size_t index = 0; index < descriptor_count; ++index) {
        const std::uint64_t rva = static_cast<std::uint64_t>(directory.VirtualAddress) +
            index * sizeof(Descriptor);
        if (rva > (std::numeric_limits<DWORD>::max)()) {
            failure = "PE import descriptor address overflows";
            return false;
        }
        const auto offset = image.OffsetFromRva(static_cast<DWORD>(rva));
        Descriptor descriptor{};
        if (!offset || !image.Read(*offset, descriptor)) {
            failure = "PE import descriptor is truncated";
            return false;
        }
        if (is_terminator(descriptor)) return true;
        const auto name_rva_value = name_rva(descriptor);
        if (!name_rva_value) return false;
        const auto import_name = image.AsciiAtRva(*name_rva_value);
        if (!import_name) {
            failure = "PE import module name is invalid";
            return false;
        }
        imports.push_back(*import_name);
    }
    failure = "PE import directory has no terminator";
    return false;
}

[[nodiscard]] bool ReadImports(
    const PeImage& image,
    std::vector<std::string>& imports,
    PluginNativeDependencyDiagnosticCode& code,
    std::string& failure) {
    if (!CollectImports<IMAGE_IMPORT_DESCRIPTOR>(
            image,
            IMAGE_DIRECTORY_ENTRY_IMPORT,
            [](const IMAGE_IMPORT_DESCRIPTOR& descriptor) { return IsEmpty(descriptor); },
            [](const IMAGE_IMPORT_DESCRIPTOR& descriptor) {
                return std::optional<DWORD>(descriptor.Name);
            },
            imports,
            failure)) {
        code = PluginNativeDependencyDiagnosticCode::InvalidPe;
        return false;
    }
    if (!CollectImports<IMAGE_DELAYLOAD_DESCRIPTOR>(
            image,
            IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
            [&](const IMAGE_DELAYLOAD_DESCRIPTOR& descriptor) {
                return descriptor.DllNameRVA == 0;
            },
            [&](const IMAGE_DELAYLOAD_DESCRIPTOR& descriptor) {
                if ((descriptor.Attributes.AllAttributes & 1U) == 0U) {
                    failure = "delay-load import does not use RVAs";
                    return std::optional<DWORD>{};
                }
                return std::optional<DWORD>(descriptor.DllNameRVA);
            },
            imports,
            failure)) {
        code = failure == "delay-load import does not use RVAs"
            ? PluginNativeDependencyDiagnosticCode::UnsupportedDelayImport
            : PluginNativeDependencyDiagnosticCode::InvalidPe;
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<std::wstring> NormalizeImportName(std::string_view value) {
    if (value.empty() || value.size() > MAX_PATH) return std::nullopt;
    for (const unsigned char character : value) {
        if (character < 0x21U || character > 0x7eU || character == '/' ||
            character == '\\' || character == ':') {
            return std::nullopt;
        }
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length) != length ||
        result == L"." || result == L"..") {
        return std::nullopt;
    }
    return Lowercase(std::move(result));
}

[[nodiscard]] bool IsApiSetModule(std::wstring_view module_name) noexcept {
    return HasPrefix(module_name, L"api-ms-win-") ||
        HasPrefix(module_name, L"ext-ms-win-");
}

[[nodiscard]] bool IsCrtModule(std::wstring_view module_name) noexcept {
    return HasPrefix(module_name, L"vcruntime") || HasPrefix(module_name, L"msvcp") ||
        HasPrefix(module_name, L"ucrtbase") || HasPrefix(module_name, L"concrt") ||
        HasPrefix(module_name, L"vcomp") || HasPrefix(module_name, L"mfc") ||
        HasPrefix(module_name, L"mfcm");
}

[[nodiscard]] std::filesystem::path SystemDirectory() {
    std::array<wchar_t, 32768> buffer{};
    const UINT length = GetSystemDirectoryW(
        buffer.data(), static_cast<UINT>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

[[nodiscard]] bool IsSystemModule(
    const std::filesystem::path& system_directory,
    std::wstring_view module_name) {
    if (system_directory.empty()) return false;
    const std::filesystem::path candidate =
        system_directory / std::wstring(module_name);
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] std::optional<std::vector<LoadedModule>> LoadedModules() {
    std::vector<HMODULE> handles(256);
    DWORD required{};
    for (;;) {
        if (K32EnumProcessModules(
                GetCurrentProcess(), handles.data(),
                static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &required) == FALSE) {
            return std::nullopt;
        }
        if (required <= handles.size() * sizeof(HMODULE)) {
            handles.resize(required / sizeof(HMODULE));
            break;
        }
        handles.resize(required / sizeof(HMODULE) + 16U);
    }

    std::vector<LoadedModule> result;
    result.reserve(handles.size());
    for (const HMODULE handle : handles) {
        std::array<wchar_t, 32768> path{};
        const DWORD length = K32GetModuleFileNameExW(
            GetCurrentProcess(), handle, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) continue;
        const std::filesystem::path module_path(std::wstring(path.data(), length));
        const std::wstring name = module_path.filename().wstring();
        if (!name.empty()) result.push_back({Lowercase(name), module_path});
    }
    return result;
}

class DependencyPreflight final {
public:
    explicit DependencyPreflight(const std::filesystem::path& entry)
        : system_directory_(SystemDirectory()) {
        if (system_directory_.empty()) {
            Add(
                PluginNativeDependencyDiagnosticCode::InternalFailure,
                entry,
                entry.filename().wstring(),
                {},
                {},
                "Windows system directory is unavailable for dependency preflight");
            return;
        }
        const auto modules = LoadedModules();
        if (!modules) {
            Add(
                PluginNativeDependencyDiagnosticCode::InternalFailure,
                entry,
                entry.filename().wstring(),
                {},
                {},
                "loaded module inventory is unavailable for dependency preflight");
            return;
        }
        loaded_modules_ = *modules;
        Visit(entry);
    }

    [[nodiscard]] PluginNativeDependencyPreflightResult TakeResult() && {
        return std::move(result_);
    }

private:
    [[nodiscard]] const LoadedModule* FindLoaded(std::wstring_view module_name) const noexcept {
        const auto found = std::ranges::find_if(loaded_modules_, [&](const LoadedModule& module) {
            return EqualInsensitive(module.name, module_name);
        });
        return found == loaded_modules_.end() ? nullptr : &*found;
    }

    void Add(
        PluginNativeDependencyDiagnosticCode code,
        const std::filesystem::path& requester,
        std::wstring module_name,
        std::filesystem::path expected_path,
        std::filesystem::path loaded_path,
        std::string message) {
        if (!result_.diagnostics.empty()) return;
        result_.diagnostics.push_back({
            code,
            requester,
            std::move(module_name),
            std::move(expected_path),
            std::move(loaded_path),
            std::move(message)});
    }

    void Visit(const std::filesystem::path& image_path) {
        if (!result_.Ok()) return;
        const std::wstring image_name = Lowercase(image_path.filename().wstring());
        if (image_name.empty() || !visited_modules_.insert(image_name).second) return;

        std::string failure;
        const auto image = ReadPeImage(image_path, failure);
        if (!image) {
            Add(
                PluginNativeDependencyDiagnosticCode::InvalidPe,
                image_path,
                image_name,
                {},
                {},
                std::move(failure));
            return;
        }

        // The runtime maps plugin images without the Windows loader, so an
        // image must not depend on loader services the mapper cannot provide:
        // loader-managed static TLS (thread_local / __declspec(thread)) has no
        // reserved slot in a mapped image.
        const IMAGE_DATA_DIRECTORY& tls_directory =
            image->directories[IMAGE_DIRECTORY_ENTRY_TLS];
        if (tls_directory.VirtualAddress != 0) {
            IMAGE_TLS_DIRECTORY64 tls{};
            const auto tls_offset = image->OffsetFromRva(tls_directory.VirtualAddress);
            if (!tls_offset || !image->Read(*tls_offset, tls)) {
                Add(
                    PluginNativeDependencyDiagnosticCode::InvalidPe,
                    image_path,
                    image_name,
                    {},
                    {},
                    "PE TLS directory is truncated");
                return;
            }
            if (tls.StartAddressOfRawData != tls.EndAddressOfRawData ||
                tls.SizeOfZeroFill != 0 || tls.AddressOfCallBacks != 0) {
                Add(
                    PluginNativeDependencyDiagnosticCode::StaticTls,
                    image_path,
                    image_name,
                    {},
                    {},
                    "the image uses loader-managed static TLS (thread_local or "
                    "__declspec(thread)); rebuild with the Anomaly SDK");
                return;
            }
        }
        for (const IMAGE_SECTION_HEADER& section : image->sections) {
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 &&
                (section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0) {
                Add(
                    PluginNativeDependencyDiagnosticCode::WritableExecutableSection,
                    image_path,
                    image_name,
                    {},
                    {},
                    "the image contains an executable and writable section");
                return;
            }
        }
        // A C++ plugin throws through the SDK exception bridge; without the
        // bridge export the mapper cannot rebind its _CxxThrowException import.
        // A direct RtlPcToFileHeader import marks statically linked vcruntime:
        // its embedded _CxxThrowException resolves the image through the PEB
        // module list, which has no entry for a mapped image, so its throws
        // escape every handler and crash the process.
        const IMAGE_DATA_DIRECTORY& import_directory =
            image->directories[IMAGE_DIRECTORY_ENTRY_IMPORT];
        bool imports_cxx_throw = false;
        bool imports_pc_to_file_header = false;
        if (import_directory.VirtualAddress != 0) {
            const std::size_t descriptor_count = (std::min)(
                static_cast<std::size_t>(import_directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR)),
                kMaximumImportDescriptors);
            for (std::size_t index = 0; index < descriptor_count; ++index) {
                const auto descriptor_offset = image->OffsetFromRva(
                    import_directory.VirtualAddress +
                    static_cast<DWORD>(index * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
                IMAGE_IMPORT_DESCRIPTOR descriptor{};
                if (!descriptor_offset ||
                    !image->Read(*descriptor_offset, descriptor) ||
                    IsEmpty(descriptor)) {
                    break;
                }
                const DWORD thunk_rva = descriptor.OriginalFirstThunk != 0
                    ? descriptor.OriginalFirstThunk
                    : descriptor.FirstThunk;
                for (DWORD thunk = 0;; ++thunk) {
                    const auto thunk_offset = image->OffsetFromRva(
                        thunk_rva + thunk * static_cast<DWORD>(sizeof(std::uint64_t)));
                    std::uint64_t lookup{};
                    if (!thunk_offset || !image->Read(*thunk_offset, lookup) ||
                        lookup == 0) {
                        break;
                    }
                    if (IMAGE_SNAP_BY_ORDINAL64(lookup)) continue;
                    // The import-by-name entry is Hint (WORD) followed by the
                    // name; read it as a bounded string instead of the 4-byte
                    // struct, whose fixed-size Name[1] would truncate the
                    // comparison to stack garbage.
                    const auto function_name = image->AsciiAtRva(
                        static_cast<DWORD>(lookup) + sizeof(WORD));
                    if (!function_name) continue;
                    if (*function_name == "_CxxThrowException") {
                        imports_cxx_throw = true;
                    } else if (*function_name == "RtlPcToFileHeader") {
                        imports_pc_to_file_header = true;
                    }
                    if (imports_cxx_throw && imports_pc_to_file_header) break;
                }
                if (imports_cxx_throw && imports_pc_to_file_header) break;
            }
        }
        if (imports_pc_to_file_header) {
            Add(
                PluginNativeDependencyDiagnosticCode::StaticCrt,
                image_path,
                image_name,
                {},
                {},
                "the image links the CRT statically (RtlPcToFileHeader import); "
                "rebuild with the Anomaly SDK");
            return;
        }
        if (imports_cxx_throw) {
            const IMAGE_DATA_DIRECTORY& export_directory =
                image->directories[IMAGE_DIRECTORY_ENTRY_EXPORT];
            bool exports_bridge = false;
            if (export_directory.VirtualAddress != 0) {
                const auto exports_offset =
                    image->OffsetFromRva(export_directory.VirtualAddress);
                IMAGE_EXPORT_DIRECTORY exports{};
                if (exports_offset && image->Read(*exports_offset, exports) &&
                    exports.NumberOfNames != 0) {
                    const auto names_offset =
                        image->OffsetFromRva(exports.AddressOfNames);
                    if (names_offset) {
                        const std::size_t name_count = (std::min)(
                            static_cast<std::size_t>(exports.NumberOfNames),
                            kMaximumImportDescriptors);
                        for (std::size_t index = 0; index < name_count; ++index) {
                            DWORD name_rva{};
                            if (!image->Read(
                                    *names_offset + index * sizeof(DWORD), name_rva)) {
                                break;
                            }
                            const auto bridge_name = image->AsciiAtRva(name_rva);
                            if (bridge_name && *bridge_name == "AnomalyPluginCxxThrowV1") {
                                exports_bridge = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (!exports_bridge) {
                Add(
                    PluginNativeDependencyDiagnosticCode::MissingCxxThrowBridge,
                    image_path,
                    image_name,
                    {},
                    {},
                    "C++ plugin image does not export the exception bridge; "
                    "rebuild with the Anomaly SDK");
                return;
            }
        }

        std::vector<std::string> imports;
        PluginNativeDependencyDiagnosticCode import_code{};
        if (!ReadImports(*image, imports, import_code, failure)) {
            Add(import_code, image_path, image_name, {}, {}, std::move(failure));
            return;
        }

        for (const std::string& import : imports) {
            const auto module_name = NormalizeImportName(import);
            if (!module_name) {
                Add(
                    PluginNativeDependencyDiagnosticCode::InvalidImportName,
                    image_path,
                    {},
                    {},
                    {},
                    "PE import module name is not a plain DLL file name");
                return;
            }
            if (IsApiSetModule(*module_name)) continue;

            const std::filesystem::path private_candidate =
                image_path.parent_path() / *module_name;
            const bool has_private_candidate = IsRegularFile(private_candidate);
            const bool is_system_module = IsSystemModule(system_directory_, *module_name);
            if (has_private_candidate && is_system_module) {
                Add(
                    PluginNativeDependencyDiagnosticCode::PrivateSystemModule,
                    image_path,
                    *module_name,
                    private_candidate,
                    {},
                    "plugin packages must not override a Windows system DLL");
                return;
            }
            if (IsCrtModule(*module_name)) {
                if (has_private_candidate) {
                    Add(
                        PluginNativeDependencyDiagnosticCode::PrivateCrtModule,
                        image_path,
                        *module_name,
                        private_candidate,
                        {},
                        "plugin packages must not provide a private CRT DLL");
                    return;
                }
                if (!is_system_module) {
                    Add(
                        PluginNativeDependencyDiagnosticCode::CrtRuntimeUnavailable,
                        image_path,
                        *module_name,
                        {},
                        {},
                        "the required CRT DLL is not available from the Windows system directory");
                    return;
                }
                continue;
            }

            if (has_private_candidate) {
                if (const LoadedModule* loaded = FindLoaded(*module_name);
                    loaded != nullptr && !SamePath(loaded->path, private_candidate)) {
                    Add(
                        PluginNativeDependencyDiagnosticCode::ModuleNameConflict,
                        image_path,
                        *module_name,
                        private_candidate,
                        loaded->path,
                        "a different module with this private DLL name is already loaded");
                    return;
                }
                Visit(private_candidate);
                if (!result_.Ok()) return;
                continue;
            }

            if (is_system_module) continue;
            if (const LoadedModule* loaded = FindLoaded(*module_name); loaded != nullptr) {
                Add(
                    PluginNativeDependencyDiagnosticCode::ModuleNameConflict,
                    image_path,
                    *module_name,
                    {},
                    loaded->path,
                    "the import has no package-private copy and would bind an existing module");
                return;
            }
            Add(
                PluginNativeDependencyDiagnosticCode::MissingPrivateImport,
                image_path,
                *module_name,
                private_candidate,
                {},
                "the import is neither a Windows system module nor a sibling private DLL");
            return;
        }
    }

    std::filesystem::path system_directory_;
    std::vector<LoadedModule> loaded_modules_;
    std::unordered_set<std::wstring> visited_modules_;
    PluginNativeDependencyPreflightResult result_;
};

}  // namespace

std::string_view PluginNativeDependencyDiagnosticCodeName(
    PluginNativeDependencyDiagnosticCode code) noexcept {
    switch (code) {
        case PluginNativeDependencyDiagnosticCode::InvalidPe: return "invalid-pe";
        case PluginNativeDependencyDiagnosticCode::InvalidImportName:
            return "invalid-import-name";
        case PluginNativeDependencyDiagnosticCode::UnsupportedDelayImport:
            return "unsupported-delay-import";
        case PluginNativeDependencyDiagnosticCode::PrivateCrtModule:
            return "private-crt-module";
        case PluginNativeDependencyDiagnosticCode::PrivateSystemModule:
            return "private-system-module";
        case PluginNativeDependencyDiagnosticCode::CrtRuntimeUnavailable:
            return "crt-runtime-unavailable";
        case PluginNativeDependencyDiagnosticCode::MissingPrivateImport:
            return "missing-private-import";
        case PluginNativeDependencyDiagnosticCode::ModuleNameConflict:
            return "module-name-conflict";
        case PluginNativeDependencyDiagnosticCode::StaticTls:
            return "static-tls";
        case PluginNativeDependencyDiagnosticCode::WritableExecutableSection:
            return "writable-executable-section";
        case PluginNativeDependencyDiagnosticCode::MissingCxxThrowBridge:
            return "missing-cxx-throw-bridge";
        case PluginNativeDependencyDiagnosticCode::StaticCrt:
            return "static-crt";
        case PluginNativeDependencyDiagnosticCode::InternalFailure: return "internal-failure";
    }
    return "unknown";
}

PluginNativeDependencyPreflightResult PreflightPluginNativeDependencies(
    const std::filesystem::path& entry_file) noexcept {
    try {
        return DependencyPreflight(entry_file).TakeResult();
    } catch (...) {
        PluginNativeDependencyPreflightResult result;
        result.diagnostics.push_back({
            PluginNativeDependencyDiagnosticCode::InternalFailure,
            entry_file,
            entry_file.filename().wstring(),
            {},
            {},
            "native dependency preflight failed unexpectedly"});
        return result;
    }
}

}  // namespace anomaly
