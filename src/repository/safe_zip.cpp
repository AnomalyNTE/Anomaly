#include "anomaly/safe_zip.hpp"

#include "anomaly/plugin_package.hpp"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace anomaly {
namespace {

SafeZipResult Fail(SafeZipError error, std::string message) {
    return {error, std::move(message), 0, 0};
}

struct ExtractionWriter {
    std::ofstream* output{};
    std::uint64_t offset{};
};

std::size_t WriteExtracted(
    void* opaque, mz_uint64 file_offset, const void* data, std::size_t size) {
    auto& writer = *static_cast<ExtractionWriter*>(opaque);
    if (writer.output == nullptr || file_offset != writer.offset) return 0;
    writer.output->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!*writer.output) return 0;
    writer.offset += size;
    return size;
}

// Reads the whole archive into memory so miniz never touches the filesystem
// (its fopen is ANSI on Windows and would choke on Unicode paths).
bool ReadFile(const std::filesystem::path& path, std::vector<unsigned char>& out) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaximumZipArchiveBytes) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    out.resize(static_cast<std::size_t>(size));
    if (size != 0 &&
        !stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
        return false;
    }
    return true;
}

// Validates one zip entry name and resolves it to a path confined under root.
// Returns false when the entry escapes the destination in any way.
bool ResolveConfined(const std::filesystem::path& root, std::string name,
                     std::filesystem::path& out, bool& is_directory) {
    if (name.empty()) return false;
    is_directory = name.back() == '/' || name.back() == '\\';
    while (!name.empty() && (name.back() == '/' || name.back() == '\\')) name.pop_back();
    const auto relative = ValidatePluginPackageRelativePath(name);
    if (!relative.Ok()) return false;

    const std::filesystem::path target = (root / relative.path).lexically_normal();
    const std::filesystem::path relative_check = target.lexically_relative(root.lexically_normal());
    if (relative_check.empty() || *relative_check.begin() == "..") return false;

    out = target;
    return true;
}

}  // namespace

SafeZipResult ExtractZip(const std::filesystem::path& zip_file,
                         const std::filesystem::path& destination_directory) {
    std::error_code size_error;
    const auto archive_size = std::filesystem::file_size(zip_file, size_error);
    if (size_error) {
        return Fail(SafeZipError::SourceUnavailable, "cannot inspect archive: " + zip_file.string());
    }
    if (archive_size > kMaximumZipArchiveBytes) {
        return Fail(SafeZipError::LimitExceeded, "archive exceeds compressed size limit");
    }
    std::vector<unsigned char> archive;
    if (!ReadFile(zip_file, archive)) {
        return Fail(SafeZipError::SourceUnavailable, "cannot read archive: " + zip_file.string());
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0)) {
        return Fail(SafeZipError::InvalidArchive, "not a valid zip archive");
    }

    struct ZipGuard {
        mz_zip_archive* zip;
        ~ZipGuard() { mz_zip_reader_end(zip); }
    } guard{&zip};

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > kMaximumZipEntries) {
        return Fail(SafeZipError::LimitExceeded, "archive has too many entries");
    }

    std::error_code ec;
    std::filesystem::create_directories(destination_directory, ec);
    if (ec) {
        return Fail(SafeZipError::IoFailure, "cannot create destination directory");
    }

    SafeZipResult result;
    std::unordered_set<std::wstring> destinations;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            return Fail(SafeZipError::InvalidArchive, "corrupt archive entry");
        }

        std::filesystem::path target;
        bool is_directory = false;
        if (!ResolveConfined(destination_directory, stat.m_filename, target, is_directory)) {
            return Fail(SafeZipError::UnsafePath,
                        std::string("unsafe entry name: ") + stat.m_filename);
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) is_directory = true;

        auto destination_key = target.lexically_relative(destination_directory).wstring();
        std::ranges::transform(destination_key, destination_key.begin(),
                               [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (!destinations.insert(std::move(destination_key)).second) {
            return Fail(SafeZipError::UnsafePath, "archive contains duplicate destination paths");
        }

        if (is_directory) {
            std::filesystem::create_directories(target, ec);
            if (ec) return Fail(SafeZipError::IoFailure, "cannot create directory");
            continue;
        }

        if (stat.m_uncomp_size > kMaximumZipBytes - result.bytes) {
            return Fail(SafeZipError::LimitExceeded, "archive expands beyond size limit");
        }

        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) return Fail(SafeZipError::IoFailure, "cannot create parent directory");

        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        ExtractionWriter writer{&file};
        const bool written = file &&
            mz_zip_reader_extract_to_callback(&zip, i, WriteExtracted, &writer, 0) != MZ_FALSE &&
            writer.offset == stat.m_uncomp_size;
        file.flush();
        file.close();
        const bool completed = written && static_cast<bool>(file);
        if (!completed) {
            return Fail(SafeZipError::IoFailure, "cannot write extracted file");
        }

        result.bytes += stat.m_uncomp_size;
        ++result.entries;
    }

    return result;
}

}  // namespace anomaly
