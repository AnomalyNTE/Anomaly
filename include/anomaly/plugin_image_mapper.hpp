#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace anomaly {

enum class PluginImageMapError : std::uint8_t {
    None = 0,
    ImageUnavailable,
    ImageInvalid,
    UnsupportedImage,
    AllocationFailure,
    RelocationFailure,
    ImportFailure,
    BridgeMissing,
    EntryRejected,
    InternalFailure,
};

struct PluginImageMapResult final {
    PluginImageMapError error{PluginImageMapError::None};
    std::string message;
    std::shared_ptr<class MappedPluginImage> image;

    [[nodiscard]] bool Ok() const noexcept { return image != nullptr; }
};

// A plugin package image mapped into this process without the Windows loader.
//
// The mapped image is private memory: it never enters the PEB module list and
// the loader never sees the plugin file, so module enumeration and loader
// notifications report nothing about it. Windows system imports bind to
// already-loaded modules whenever possible; package-private sibling DLLs are
// mapped the same way and one mapping is shared between the importers of a
// single load tree, mirroring the loader's per-name deduplication.
//
// Destroying the last reference runs the image's DLL_PROCESS_DETACH (including
// its atexit table), unregisters the dynamic function table and releases the
// allocation. A quarantine holder that must keep a generation alive simply
// never destroys its last reference.
class MappedPluginImage final {
public:
    // PE data directory location; rva 0 marks an absent directory.
    struct Directory final {
        std::uint32_t rva{};
        std::uint32_t size{};
    };

    // Internal bookkeeping of one mapped image; defined with std types only so
    // the header stays independent of the PE definitions.
    struct State final {
        ~State();

        void* base{};
        std::size_t size{};
        void* entry{};  // DllMain, cast to the entry signature at the call site
        bool function_table_registered{};
        bool attach_attempted{};
        bool attach_succeeded{};
        bool mapping_complete{};
        bool detached{};
        std::uint32_t headers_size{};
        Directory export_directory{};
        Directory exception_directory{};
        Directory debug_directory{};
        std::filesystem::path source;
        std::vector<std::shared_ptr<State>> dependencies;
    };

    MappedPluginImage() = default;
    ~MappedPluginImage();

    MappedPluginImage(const MappedPluginImage&) = delete;
    MappedPluginImage& operator=(const MappedPluginImage&) = delete;

    [[nodiscard]] void* Base() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;

private:
    friend PluginImageMapResult MapPluginImage(
        const std::filesystem::path&) noexcept;
    friend void* FindPluginImageExport(
        const MappedPluginImage& image, const char* name) noexcept;
    friend void EraseMappedImageHeaders(const MappedPluginImage& image) noexcept;

    std::shared_ptr<State> state_;

    explicit MappedPluginImage(std::shared_ptr<State> state) noexcept;
};

// Maps one plugin entry image (and, recursively, its package-private sibling
// dependencies) into this process and runs DLL_PROCESS_ATTACH. Returns a
// failed result with nothing left behind when any step rejects.
[[nodiscard]] PluginImageMapResult MapPluginImage(
    const std::filesystem::path& entry_file) noexcept;

// Resolves an export of a mapped image by name. Must be called before
// EraseMappedImageHeaders removes the export directory.
[[nodiscard]] void* FindPluginImageExport(
    const MappedPluginImage& image, const char* name) noexcept;

// Removes the on-disk fingerprints of a mapped image after every export and
// header consumer is done: PE headers with the section table, the relocation
// directory, the export directory and the debug records including their PDB
// path strings. Unwind data stays, so exceptions keep working.
void EraseMappedImageHeaders(const MappedPluginImage& image) noexcept;

}  // namespace anomaly
