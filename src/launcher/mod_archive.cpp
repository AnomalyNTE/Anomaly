#include "anomaly/launcher/mod_archive.hpp"
#include "anomaly/plugin_package.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <set>
#include <utility>

namespace anomaly::launcher {
namespace {

constexpr std::uint64_t kMaximumBytes = 16ULL * 1024 * 1024 * 1024;
constexpr std::size_t kMaximumEntries = 20000;

struct File final {
    HANDLE handle{INVALID_HANDLE_VALUE};
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

bool DeviceName(const std::filesystem::path& path) {
    for (const auto& part : path) {
        auto name = part.wstring();
        name.resize(name.find('.') == std::wstring::npos ? name.size() : name.find('.'));
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" ||
            (name.size() == 4 && (name.starts_with(L"COM") || name.starts_with(L"LPT")) &&
                ((name[3] >= L'1' && name[3] <= L'9') || name[3] == L'\u00b9' || name[3] == L'\u00b2' || name[3] == L'\u00b3')))
            return true;
    }
    return false;
}

}  // namespace

ExtractedModArchive::~ExtractedModArchive() {
    if (!directory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
}

ExtractedModArchive::ExtractedModArchive(ExtractedModArchive&& other) noexcept
    : error(other.error), detail(std::move(other.detail)), directory(std::exchange(other.directory, {})) {}

ExtractedModArchive ExtractModArchive(const std::filesystem::path& source, const std::atomic<bool>* cancelled) {
    ExtractedModArchive result;
    const auto fail = [&](ModArchiveError error, std::string detail) {
        result.error = error;
        result.detail = std::move(detail);
    };
    try {
        const auto is_cancelled = [&] { return cancelled && cancelled->load(std::memory_order_relaxed); };
        if (is_cancelled()) { fail(ModArchiveError::Cancelled, "extraction cancelled"); return result; }
        std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
        if (!reader) { fail(ModArchiveError::Io, "cannot create archive reader"); return result; }
        archive_read_support_filter_none(reader.get());
        archive_read_support_format_zip(reader.get());
        archive_read_support_format_7zip(reader.get());
        archive_read_support_format_rar(reader.get());
        archive_read_support_format_rar5(reader.get());
        const auto read_failure = [&](ModArchiveError fallback) {
            const char* detail = archive_error_string(reader.get());
            fail(archive_read_has_encrypted_entries(reader.get()) == 1 ? ModArchiveError::Encrypted : fallback,
                detail ? detail : "cannot decode archive");
        };
        if (archive_read_open_filename_w(reader.get(), source.c_str(), 128 * 1024) != ARCHIVE_OK) {
            read_failure(std::filesystem::is_regular_file(source) ? ModArchiveError::Unsupported : ModArchiveError::Io);
            return result;
        }

        GUID id{};
        wchar_t temporary_name[40]{};
        if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, temporary_name, static_cast<int>(std::size(temporary_name))) == 0) {
            fail(ModArchiveError::Io, "cannot allocate extraction directory"); return result;
        }
        const auto directory = std::filesystem::temp_directory_path() / (std::wstring(L"anomaly-mod-") + temporary_name);
        if (!std::filesystem::create_directory(directory)) {
            fail(ModArchiveError::Io, "extraction directory already exists"); return result;
        }
        result.directory = directory;
        std::set<std::wstring> paths;
        std::uint64_t total{};
        std::size_t entries{};
        std::array<char, 128 * 1024> buffer{};
        struct archive_entry* entry{};
        for (;;) {
            if (is_cancelled()) { fail(ModArchiveError::Cancelled, "extraction cancelled"); break; }
            const int status = archive_read_next_header(reader.get(), &entry);
            if (status == ARCHIVE_EOF) break;
            if (status < ARCHIVE_OK) { read_failure(ModArchiveError::Corrupt); break; }
            if (++entries > kMaximumEntries) { fail(ModArchiveError::LimitExceeded, "too many archive entries"); break; }
            if (archive_entry_is_encrypted(entry)) { fail(ModArchiveError::Encrypted, "encrypted archive"); break; }
            const auto type = archive_entry_filetype(entry);
            if (archive_entry_symlink(entry) || archive_entry_hardlink(entry) || (type != AE_IFREG && type != AE_IFDIR)) {
                fail(ModArchiveError::UnsafePath, "archive links and special files are not supported"); break;
            }
            const wchar_t* pathname = archive_entry_pathname_w(entry);
            if (!pathname) { fail(ModArchiveError::Corrupt, "cannot decode archive filename"); break; }
            std::wstring name = pathname;
            std::replace(name.begin(), name.end(), L'\\', L'/');
            while (name.starts_with(L"./")) name.erase(0, 2);
            while (!name.empty() && name.back() == L'/') name.pop_back();
            if (type == AE_IFDIR && (name.empty() || name == L".")) continue;
            const auto utf8 = std::filesystem::path(name).u8string();
            const auto relative = ValidatePluginPackageRelativePath(
                std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
            if (!relative.Ok() || DeviceName(relative.path)) {
                fail(ModArchiveError::UnsafePath, "unsafe archive filename"); break;
            }
            auto key = relative.path.wstring();
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (!paths.insert(key).second) { fail(ModArchiveError::UnsafePath, "duplicate archive destination"); break; }
            const auto target = directory / relative.path;
            if (type == AE_IFDIR) { std::filesystem::create_directories(target); continue; }
            const auto declared_size = archive_entry_size(entry);
            if (declared_size < 0 || static_cast<std::uint64_t>(declared_size) > kMaximumBytes - total) {
                fail(ModArchiveError::LimitExceeded, "archive exceeds extraction size limit"); break;
            }
            std::filesystem::create_directories(target.parent_path());
            File output{CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            if (output.handle == INVALID_HANDLE_VALUE) { fail(ModArchiveError::Io, "cannot create extracted file"); break; }
            std::uint64_t written{};
            for (;;) {
                if (is_cancelled()) { fail(ModArchiveError::Cancelled, "extraction cancelled"); break; }
                const auto count = archive_read_data(reader.get(), buffer.data(), buffer.size());
                if (count == 0) break;
                if (count < 0) { read_failure(ModArchiveError::Corrupt); break; }
                if (static_cast<std::uint64_t>(count) > kMaximumBytes - total) {
                    fail(ModArchiveError::LimitExceeded, "archive exceeds extraction size limit"); break;
                }
                DWORD bytes{};
                if (!WriteFile(output.handle, buffer.data(), static_cast<DWORD>(count), &bytes, nullptr) || bytes != count) {
                    fail(ModArchiveError::Io, "cannot write extracted file"); break;
                }
                total += bytes;
                written += bytes;
            }
            if (!result.Ok()) break;
            if (archive_entry_size_is_set(entry) && written != static_cast<std::uint64_t>(declared_size)) {
                fail(ModArchiveError::Corrupt, "truncated archive entry"); break;
            }
        }
        if (result.Ok() && archive_read_close(reader.get()) != ARCHIVE_OK) read_failure(ModArchiveError::Corrupt);
    } catch (const std::exception& error) { fail(ModArchiveError::Io, error.what()); }
    return result;
}

}  // namespace anomaly::launcher
