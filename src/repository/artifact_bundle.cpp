#include "anomaly/artifact_bundle.hpp"

#include "anomaly/artifact_crypto.hpp"
#include "anomaly/plugin_package.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <span>
#include <type_traits>
#include <vector>

namespace anomaly {
namespace {

constexpr std::array<char, 8> kMagic{'A', 'N', 'O', 'M', 'B', 'N', 'D', '1'};

ArtifactBundleResult Failure(ArtifactBundleError error, std::string message) {
    return {error, std::move(message), 0, 0};
}

template <typename Integer>
bool WriteInteger(std::ofstream& output, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    std::array<unsigned char, sizeof(Integer)> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<unsigned char>(value >> (index * 8));
    }
    output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    return static_cast<bool>(output);
}

template <typename Integer>
bool ReadInteger(std::ifstream& input, Integer& value) {
    static_assert(std::is_unsigned_v<Integer>);
    std::array<unsigned char, sizeof(Integer)> encoded{};
    input.read(reinterpret_cast<char*>(encoded.data()), encoded.size());
    if (!input) return false;
    value = 0;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        value |= static_cast<Integer>(encoded[index]) << (index * 8);
    }
    return true;
}

std::string Utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool IsReparsePoint(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool CopyBytes(std::ifstream& input, std::ofstream& output, std::uint64_t bytes) {
    std::array<char, 64U * 1024U> buffer{};
    while (bytes != 0) {
        const auto chunk = static_cast<std::streamsize>((std::min)(
            bytes, static_cast<std::uint64_t>(buffer.size())));
        input.read(buffer.data(), chunk);
        if (input.gcount() != chunk) return false;
        output.write(buffer.data(), chunk);
        if (!output) return false;
        bytes -= static_cast<std::uint64_t>(chunk);
    }
    return true;
}

struct InputEntry {
    std::filesystem::path source;
    std::string relative;
    std::uint64_t size{};
    std::array<std::uint8_t, 32> digest{};
};

}  // namespace

ArtifactBundleResult CreateArtifactBundle(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& output_file) {
    try {
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(source_directory, error);
        if (error || !std::filesystem::is_directory(root, error) || IsReparsePoint(root)) {
            return Failure(ArtifactBundleError::SourceUnavailable, "bundle source is unavailable");
        }
        std::vector<InputEntry> entries;
        std::uint64_t total{};
        for (std::filesystem::recursive_directory_iterator iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (IsReparsePoint(iterator->path())) {
                return Failure(ArtifactBundleError::ReparsePoint, "bundle source contains a reparse point");
            }
            if (iterator->is_directory(error)) continue;
            if (!iterator->is_regular_file(error)) {
                return Failure(ArtifactBundleError::InvalidFormat, "bundle source contains a non-file entry");
            }
            const auto relative_path = std::filesystem::relative(iterator->path(), root, error);
            const std::string relative = Utf8(relative_path);
            const auto validated = ValidatePluginPackageRelativePath(relative, false);
            if (error || !validated.Ok()) {
                return Failure(ArtifactBundleError::InvalidPath, "bundle source path is invalid");
            }
            const std::uint64_t size = iterator->file_size(error);
            if (error || size > kMaximumArtifactBundleBytes || total > kMaximumArtifactBundleBytes - size) {
                return Failure(ArtifactBundleError::LimitExceeded, "bundle size limit exceeded");
            }
            const std::string digest_hex = Sha256FileHex(iterator->path(), size);
            std::vector<std::uint8_t> digest;
            if (digest_hex.empty() || !DecodeHex(digest_hex, digest) || digest.size() != 32) {
                return Failure(ArtifactBundleError::IoFailure, "bundle source hash failed");
            }
            InputEntry entry{iterator->path(), relative, size};
            std::copy(digest.begin(), digest.end(), entry.digest.begin());
            entries.push_back(std::move(entry));
            total += size;
            if (entries.size() > kMaximumArtifactBundleEntries) {
                return Failure(ArtifactBundleError::LimitExceeded, "bundle entry limit exceeded");
            }
        }
        if (error || entries.empty()) {
            return Failure(ArtifactBundleError::SourceUnavailable, "bundle source enumeration failed");
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.relative < right.relative;
        });
        std::filesystem::create_directories(output_file.parent_path(), error);
        if (error) return Failure(ArtifactBundleError::IoFailure, "bundle output directory failed");
        const auto temporary = output_file.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(kMagic.data(), kMagic.size());
        if (!WriteInteger(output, static_cast<std::uint32_t>(entries.size()))) {
            return Failure(ArtifactBundleError::IoFailure, "bundle header write failed");
        }
        for (const auto& entry : entries) {
            if (entry.relative.empty() || entry.relative.size() > 4096 ||
                !WriteInteger(output, static_cast<std::uint32_t>(entry.relative.size())) ||
                !WriteInteger(output, entry.size)) {
                return Failure(ArtifactBundleError::IoFailure, "bundle entry header write failed");
            }
            output.write(reinterpret_cast<const char*>(entry.digest.data()), entry.digest.size());
            output.write(entry.relative.data(), static_cast<std::streamsize>(entry.relative.size()));
            std::ifstream input(entry.source, std::ios::binary);
            if (!input || !CopyBytes(input, output, entry.size)) {
                return Failure(ArtifactBundleError::IoFailure, "bundle entry write failed");
            }
        }
        output.flush();
        output.close();
        if (!output) return Failure(ArtifactBundleError::IoFailure, "bundle flush failed");
        std::filesystem::remove(output_file, error);
        error.clear();
        std::filesystem::rename(temporary, output_file, error);
        if (error) return Failure(ArtifactBundleError::IoFailure, "bundle publish failed");
        return {ArtifactBundleError::None, {}, static_cast<std::uint32_t>(entries.size()), total};
    } catch (const std::exception& error) {
        return Failure(ArtifactBundleError::IoFailure, error.what());
    }
}

ArtifactBundleResult ExtractArtifactBundle(
    const std::filesystem::path& bundle_file,
    const std::filesystem::path& empty_destination_directory) {
    try {
        std::error_code error;
        if (std::filesystem::exists(empty_destination_directory, error)) {
            if (!std::filesystem::is_empty(empty_destination_directory, error)) {
                return Failure(ArtifactBundleError::InvalidFormat, "bundle destination is not empty");
            }
        } else {
            std::filesystem::create_directories(empty_destination_directory, error);
        }
        if (error || IsReparsePoint(empty_destination_directory)) {
            return Failure(ArtifactBundleError::ReparsePoint, "bundle destination is unavailable");
        }
        std::ifstream input(bundle_file, std::ios::binary);
        std::array<char, kMagic.size()> magic{};
        input.read(magic.data(), magic.size());
        std::uint32_t count{};
        if (!input || magic != kMagic || !ReadInteger(input, count) || count == 0 ||
            count > kMaximumArtifactBundleEntries) {
            return Failure(ArtifactBundleError::InvalidFormat, "bundle header is invalid");
        }
        std::set<std::string> paths;
        std::uint64_t total{};
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t path_size{};
            std::uint64_t file_size{};
            std::array<std::uint8_t, 32> expected{};
            if (!ReadInteger(input, path_size) || !ReadInteger(input, file_size) ||
                path_size == 0 || path_size > 4096 || file_size > kMaximumArtifactBundleBytes) {
                return Failure(ArtifactBundleError::InvalidFormat, "bundle entry header is invalid");
            }
            input.read(reinterpret_cast<char*>(expected.data()), expected.size());
            std::string relative(path_size, '\0');
            input.read(relative.data(), static_cast<std::streamsize>(relative.size()));
            const auto validated = ValidatePluginPackageRelativePath(relative, false);
            if (!input || !validated.Ok()) {
                return Failure(ArtifactBundleError::InvalidPath, "bundle entry path is invalid");
            }
            std::string folded = relative;
            std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (!paths.insert(folded).second) {
                return Failure(ArtifactBundleError::DuplicateEntry, "bundle contains a duplicate path");
            }
            if (total > kMaximumArtifactBundleBytes - file_size) {
                return Failure(ArtifactBundleError::LimitExceeded, "bundle extraction limit exceeded");
            }
            const auto destination = empty_destination_directory / validated.path;
            std::filesystem::create_directories(destination.parent_path(), error);
            if (error || IsReparsePoint(destination.parent_path())) {
                return Failure(ArtifactBundleError::ReparsePoint, "bundle destination path is unsafe");
            }
            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            if (!output || !CopyBytes(input, output, file_size)) {
                return Failure(ArtifactBundleError::IoFailure, "bundle entry extraction failed");
            }
            output.close();
            const std::string actual_hex = Sha256FileHex(destination, file_size);
            std::vector<std::uint8_t> actual;
            if (!DecodeHex(actual_hex, actual) || actual.size() != expected.size() ||
                !std::equal(actual.begin(), actual.end(), expected.begin())) {
                return Failure(ArtifactBundleError::HashMismatch, "bundle entry hash mismatch");
            }
            total += file_size;
        }
        char trailing{};
        if (input.read(&trailing, 1)) {
            return Failure(ArtifactBundleError::InvalidFormat, "bundle contains trailing data");
        }
        if (!input.eof()) return Failure(ArtifactBundleError::IoFailure, "bundle read failed");
        return {ArtifactBundleError::None, {}, count, total};
    } catch (const std::exception& error) {
        return Failure(ArtifactBundleError::IoFailure, error.what());
    }
}

}  // namespace anomaly
