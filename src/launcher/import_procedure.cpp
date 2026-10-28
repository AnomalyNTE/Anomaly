#include "import_procedure.hpp"

#include <charconv>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace anomaly::launcher {
namespace {

template <typename T>
const T* ImageRva(std::span<const std::byte> image, DWORD rva, DWORD count = 1) noexcept {
    if (rva > image.size() || count > (image.size() - rva) / sizeof(T)) return nullptr;
    return reinterpret_cast<const T*>(image.data() + rva);
}

std::optional<std::string_view> ImageString(
    std::span<const std::byte> image, DWORD rva) noexcept {
    const auto* start = ImageRva<char>(image, rva);
    if (start == nullptr) return std::nullopt;
    const auto* end = static_cast<const char*>(std::memchr(start, '\0', image.size() - rva));
    if (end == nullptr) return std::nullopt;
    return std::string_view(start, static_cast<std::size_t>(end - start));
}

}  // namespace

ImportProcedureResolver::~ImportProcedureResolver() {
    for (const auto module : forwarded_modules_) FreeLibrary(module);
}

FARPROC ImportProcedureResolver::Resolve(HMODULE module, const char* name) {
    // Only loader-owned HMODULEs enter here. Bound forwarder traversal so an
    // export cycle fails resolution instead of looping forever.
    for (unsigned depth = 0; module != nullptr && depth < 32; ++depth) {
        const auto* base = reinterpret_cast<const std::byte*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const std::span<const std::byte> image(base, headers->OptionalHeader.SizeOfImage);
        const auto& directory =
            headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory.VirtualAddress == 0) return nullptr;
        const auto* exports = ImageRva<IMAGE_EXPORT_DIRECTORY>(image, directory.VirtualAddress);
        if (exports == nullptr) return nullptr;
        const auto* functions =
            ImageRva<DWORD>(image, exports->AddressOfFunctions, exports->NumberOfFunctions);
        if (functions == nullptr) return nullptr;

        DWORD index = exports->NumberOfFunctions;
        if (IS_INTRESOURCE(name)) {
            const auto ordinal = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(name));
            if (ordinal < exports->Base) return nullptr;
            index = ordinal - exports->Base;
        } else {
            const auto* names =
                ImageRva<DWORD>(image, exports->AddressOfNames, exports->NumberOfNames);
            const auto* ordinals =
                ImageRva<WORD>(image, exports->AddressOfNameOrdinals, exports->NumberOfNames);
            if (names == nullptr || ordinals == nullptr) return nullptr;
            for (DWORD slot = 0; slot < exports->NumberOfNames; ++slot) {
                const auto candidate = ImageString(image, names[slot]);
                if (!candidate) return nullptr;
                if (*candidate == name) {
                    index = ordinals[slot];
                    break;
                }
            }
        }
        if (index >= exports->NumberOfFunctions) return nullptr;
        const DWORD rva = functions[index];
        if (rva == 0 || rva >= image.size()) return nullptr;
        if (rva < directory.VirtualAddress || rva - directory.VirtualAddress >= directory.Size) {
            return reinterpret_cast<FARPROC>(const_cast<std::byte*>(base + rva));
        }

        const auto forwarder = ImageString(image, rva);
        if (!forwarder) return nullptr;
        const auto separator = forwarder->rfind('.');
        if (separator == std::string_view::npos || separator == 0 ||
            separator + 1 == forwarder->size()) {
            return nullptr;
        }
        std::wstring library(forwarder->begin(), forwarder->begin() + separator);
        if (library.find(L'.') == std::wstring::npos) library += L".dll";
        const HMODULE forwarded = LoadLibraryExW(
            library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (forwarded == nullptr) return nullptr;
        try {
            forwarded_modules_.push_back(forwarded);
        } catch (...) {
            FreeLibrary(forwarded);
            throw;
        }
        module = forwarded;
        name = forwarder->data() + separator + 1;
        if (*name == '#') {
            unsigned ordinal{};
            const auto end = forwarder->data() + forwarder->size();
            const auto parsed = std::from_chars(name + 1, end, ordinal);
            if (parsed.ec != std::errc{} || parsed.ptr != end ||
                ordinal > (std::numeric_limits<WORD>::max)()) {
                return nullptr;
            }
            name = MAKEINTRESOURCEA(ordinal);
        }
    }
    return nullptr;
}

}  // namespace anomaly::launcher
