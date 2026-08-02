#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

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

#pragma pack(push, 2)
struct GroupIconDirectory final {
    WORD reserved{};
    WORD type{};
    WORD count{};
};

struct GroupIconEntry final {
    BYTE width{};
    BYTE height{};
    BYTE color_count{};
    BYTE reserved{};
    WORD planes{};
    WORD bit_count{};
    DWORD bytes_in_resource{};
    WORD resource_id{};
};
#pragma pack(pop)

class PeImage final {
public:
    explicit PeImage(const wchar_t* path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto end = input.tellg();
        if (!input || end <= 0 ||
            static_cast<unsigned long long>(end) >
                (std::numeric_limits<std::size_t>::max)()) {
            return;
        }
        bytes_.resize(static_cast<std::size_t>(end));
        input.seekg(0);
        input.read(
            reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(end));
        if (!input || bytes_.size() < sizeof(IMAGE_DOS_HEADER)) return;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes_.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            static_cast<std::size_t>(dos->e_lfanew) >
                bytes_.size() - sizeof(IMAGE_NT_HEADERS64)) {
            return;
        }
        headers_ = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            bytes_.data() + dos->e_lfanew);
        if (headers_->Signature != IMAGE_NT_SIGNATURE ||
            headers_->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            headers_ = nullptr;
        }
    }

    [[nodiscard]] bool Valid() const noexcept { return headers_ != nullptr; }

    template <typename T>
    [[nodiscard]] const T* Rva(DWORD rva, std::size_t count = 1) const noexcept {
        if (!Valid() || count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) {
            return nullptr;
        }
        const std::size_t size = count * sizeof(T);
        if (rva < headers_->OptionalHeader.SizeOfHeaders) return At<T>(rva, size);

        const auto* sections = IMAGE_FIRST_SECTION(headers_);
        for (WORD index = 0; index < headers_->FileHeader.NumberOfSections; ++index) {
            const auto& section = sections[index];
            const DWORD extent = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva < section.VirtualAddress) continue;
            const DWORD relative = rva - section.VirtualAddress;
            if (relative > extent || size > extent - relative) continue;
            const std::size_t offset = section.PointerToRawData + relative;
            return At<T>(offset, size);
        }
        return nullptr;
    }

    [[nodiscard]] const IMAGE_DATA_DIRECTORY& Directory(std::size_t index) const noexcept {
        static constexpr IMAGE_DATA_DIRECTORY empty{};
        return Valid() && index < headers_->OptionalHeader.NumberOfRvaAndSizes
            ? headers_->OptionalHeader.DataDirectory[index] : empty;
    }

    [[nodiscard]] bool NameEquals(DWORD rva, std::string_view expected) const noexcept {
        const char* name = Rva<char>(rva);
        if (name == nullptr) return false;
        const std::size_t offset = static_cast<std::size_t>(
            reinterpret_cast<const std::byte*>(name) - bytes_.data());
        if (std::memchr(name, '\0', bytes_.size() - offset) == nullptr) return false;
        const std::string expected_name(expected);
        return _stricmp(name, expected_name.c_str()) == 0;
    }

private:
    template <typename T>
    [[nodiscard]] const T* At(std::size_t offset, std::size_t size) const noexcept {
        if (offset > bytes_.size() || size > bytes_.size() - offset) return nullptr;
        return reinterpret_cast<const T*>(bytes_.data() + offset);
    }

    std::vector<std::byte> bytes_;
    const IMAGE_NT_HEADERS64* headers_{};
};

bool ImportsNormally(const PeImage& image, std::string_view module) {
    const auto& directory = image.Directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    const std::size_t maximum = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t index = 0; directory.VirtualAddress != 0 && index < maximum; ++index) {
        const auto* descriptor = image.Rva<IMAGE_IMPORT_DESCRIPTOR>(
            directory.VirtualAddress +
                static_cast<DWORD>(index * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
        if (descriptor == nullptr || descriptor->Name == 0) return false;
        if (image.NameEquals(descriptor->Name, module)) return true;
    }
    return false;
}

bool ImportsWithDelay(const PeImage& image, std::string_view module) {
    const auto& directory = image.Directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    const std::size_t maximum = directory.Size / sizeof(DelayImportDescriptor);
    for (std::size_t index = 0; directory.VirtualAddress != 0 && index < maximum; ++index) {
        const auto* descriptor = image.Rva<DelayImportDescriptor>(
            directory.VirtualAddress +
                static_cast<DWORD>(index * sizeof(DelayImportDescriptor)));
        if (descriptor == nullptr || descriptor->name == 0) return false;
        if (image.NameEquals(descriptor->name, module)) return true;
    }
    return false;
}

bool HasHighResolutionIcon(const HMODULE executable, const HRSRC resource) {
    const HGLOBAL loaded = resource == nullptr ? nullptr : LoadResource(executable, resource);
    const auto* directory = loaded == nullptr
        ? nullptr : static_cast<const GroupIconDirectory*>(LockResource(loaded));
    const DWORD size = resource == nullptr ? 0 : SizeofResource(executable, resource);
    if (directory == nullptr || directory->reserved != 0 || directory->type != 1 ||
        size < sizeof(*directory) || directory->count == 0 ||
        directory->count > (size - sizeof(*directory)) / sizeof(GroupIconEntry)) {
        return false;
    }
    const auto* entries = reinterpret_cast<const GroupIconEntry*>(directory + 1);
    return std::any_of(entries, entries + directory->count, [](const GroupIconEntry& entry) {
        return entry.width == 0 && entry.height == 0 && entry.resource_id != 0;
    });
}

#if !defined(ANOMALY_ENABLE_ASAN)
bool RequiresLoaderManagedTls(const PeImage& image) {
    const auto& directory = image.Directory(IMAGE_DIRECTORY_ENTRY_TLS);
    if (directory.VirtualAddress == 0) return false;
    const auto* tls = image.Rva<IMAGE_TLS_DIRECTORY64>(directory.VirtualAddress);
    return tls == nullptr || directory.Size < sizeof(*tls) ||
        tls->StartAddressOfRawData != tls->EndAddressOfRawData ||
        tls->SizeOfZeroFill != 0;
}
#endif

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const PeImage image(argv[1]);
#if !defined(ANOMALY_ENABLE_ASAN)
    const PeImage core(argv[2]);
#endif
    const HMODULE executable = LoadLibraryExW(
        argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!Check(executable != nullptr, "launcher executable could not be loaded as data")) {
        return 1;
    }
    const HRSRC resource = FindResourceW(executable, MAKEINTRESOURCEW(1), RT_MANIFEST);
    const HRSRC icon = FindResourceW(
        executable, MAKEINTRESOURCEW(201), RT_GROUP_ICON);
    const HGLOBAL loaded = resource == nullptr ? nullptr : LoadResource(executable, resource);
    const void* data = loaded == nullptr ? nullptr : LockResource(loaded);
    const DWORD size = resource == nullptr ? 0 : SizeofResource(executable, resource);
    const std::string_view manifest(
        data == nullptr ? "" : static_cast<const char*>(data), size);
    const bool result =
        Check(image.Valid(), "launcher executable is not a valid x64 PE image") &&
        Check(!ImportsNormally(image, "dwmapi.dll"),
              "launcher resolves the proxy payload during process startup") &&
        Check(ImportsWithDelay(image, "dwmapi.dll"),
              "launcher system dwmapi dependency is not delay-loaded") &&
#if !defined(ANOMALY_ENABLE_ASAN)
        Check(core.Valid(), "Anomaly core is not a valid x64 PE image") &&
        Check(!RequiresLoaderManagedTls(core),
              "Anomaly core requires loader-managed static TLS") &&
#endif
        Check(resource != nullptr && data != nullptr && size != 0,
              "launcher manifest resource is missing") &&
        Check(HasHighResolutionIcon(executable, icon),
              "launcher 256px icon resource is missing") &&
        Check(manifest.find("requestedExecutionLevel") != std::string_view::npos,
              "launcher manifest does not declare an execution level") &&
        Check(manifest.find("level=\"requireAdministrator\"") != std::string_view::npos,
              "launcher manifest does not require administrator privileges") &&
        Check(manifest.find("<dpiAware") != std::string_view::npos &&
                  manifest.find("true/pm") != std::string_view::npos,
              "launcher manifest does not declare per-monitor DPI awareness") &&
        Check(manifest.find("<dpiAwareness") != std::string_view::npos &&
                  manifest.find("PerMonitorV2") != std::string_view::npos,
              "launcher manifest does not declare Per-Monitor V2 DPI awareness");
    FreeLibrary(executable);
    return result ? 0 : 1;
}
