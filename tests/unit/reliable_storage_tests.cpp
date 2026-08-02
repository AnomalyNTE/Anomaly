#include "anomaly/reliable_storage.hpp"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TempDirectory final {
public:
    explicit TempDirectory(std::wstring_view label) {
        std::array<wchar_t, MAX_PATH + 1> temp{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (length == 0 || length >= temp.size()) {
            throw std::runtime_error("GetTempPathW failed");
        }

        for (std::uint32_t attempt = 0; attempt < 128; ++attempt) {
            path_ = std::filesystem::path(temp.data()) /
                (std::wstring(L"anomaly-storage-") + std::wstring(label) + L"-" +
                 std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(sequence_.fetch_add(1)));
            if (CreateDirectoryW(path_.c_str(), nullptr)) return;
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                throw std::runtime_error("CreateDirectoryW failed");
            }
        }
        throw std::runtime_error("unique temporary directory was not created");
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    inline static std::atomic<std::uint64_t> sequence_{};
    std::filesystem::path path_;
};

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result(text.size());
    if (!text.empty()) std::memcpy(result.data(), text.data(), text.size());
    return result;
}

std::string Text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool HasStorageTemp(const std::filesystem::path& directory) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::wstring name = entry.path().filename().native();
        if (name.starts_with(L".anomaly-tmp-")) return true;
    }
    return false;
}

bool TestFirstWriteOverwriteAndRead() {
    TempDirectory root(L"basic");
    anomaly::ReliableStorage storage(root.path().native());
    bool result = Check(storage.InitializationResult().ok(), "storage root was not initialized");

    const auto first = Bytes("first-value");
    const auto second = Bytes("replacement-value");
    const auto first_write = storage.WriteAtomic(L"settings.json", first);
    result = Check(first_write.ok(), "first atomic write failed") &&
        Check(first_write.bytes_processed == first.size(), "first write byte count was wrong") &&
        result;

    auto read = storage.Read(L"settings.json", 1024);
    result = Check(read.ok() && Text(read.bytes) == "first-value", "first read was wrong") && result;

    const auto overwrite = storage.WriteAtomic(L"settings.json", second);
    result = Check(overwrite.ok(), "atomic overwrite failed") && result;
    read = storage.Read(L"settings.json", 1024);
    result = Check(read.ok() && Text(read.bytes) == "replacement-value",
                   "overwrite was not visible as a complete value") &&
        Check(!HasStorageTemp(root.path()), "successful write left a temporary file") && result;

    const auto deleted = storage.Delete(L"settings.json");
    result = Check(deleted.ok(), "delete failed") && result;
    read = storage.Read(L"settings.json", 1024);
    return Check(!read.ok() && read.result.win32_error == ERROR_FILE_NOT_FOUND,
                 "deleted file was still readable") && result;
}

bool TestFailedReplacePreservesOldDataAndCleansTemp() {
    TempDirectory root(L"failure");
    anomaly::ReliableStorage storage(root.path().native());
    const auto old_bytes = Bytes("durable-old-value");
    const auto new_bytes = Bytes("new-value-that-must-not-appear");
    if (!Check(storage.WriteAtomic(L"state.bin", old_bytes).ok(),
               "failure fixture initial write failed")) {
        return false;
    }

    const std::filesystem::path target = root.path() / L"state.bin";
    const HANDLE lock = CreateFileW(
        target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!Check(lock != INVALID_HANDLE_VALUE, "failed to lock old target against replacement")) {
        return false;
    }

    const anomaly::StorageResult failed = storage.WriteAtomic(L"state.bin", new_bytes);
    CloseHandle(lock);
    const auto read = storage.Read(L"state.bin", 1024);
    return Check(!failed.ok() && failed.error == anomaly::StorageError::IoFailure &&
                     failed.win32_error != ERROR_SUCCESS,
                 "locked replacement unexpectedly succeeded") &&
        Check(read.ok() && Text(read.bytes) == "durable-old-value",
              "failed replacement changed the old data") &&
        Check(!HasStorageTemp(root.path()), "failed replacement left a temporary file");
}

struct JunctionData {
    DWORD tag{};
    WORD data_length{};
    WORD reserved{};
    WORD substitute_offset{};
    WORD substitute_length{};
    WORD print_offset{};
    WORD print_length{};
    std::array<wchar_t, 1024> path_buffer{};
};

bool CreateJunction(
    const std::filesystem::path& link, const std::filesystem::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;

    const std::wstring print_name = std::filesystem::absolute(target).native();
    const std::wstring substitute_name = L"\\??\\" + print_name;
    const std::size_t substitute_bytes = substitute_name.size() * sizeof(wchar_t);
    const std::size_t print_bytes = print_name.size() * sizeof(wchar_t);
    const std::size_t names_bytes = substitute_bytes + sizeof(wchar_t) +
        print_bytes + sizeof(wchar_t);
    if (names_bytes > sizeof(JunctionData::path_buffer) ||
        names_bytes + 8 > (std::numeric_limits<WORD>::max)()) {
        RemoveDirectoryW(link.c_str());
        return false;
    }

    JunctionData data;
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute_bytes);
    data.print_offset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print_bytes);
    data.data_length = static_cast<WORD>(8 + names_bytes);
    std::memcpy(data.path_buffer.data(), substitute_name.c_str(),
                substitute_bytes + sizeof(wchar_t));
    std::memcpy(
        reinterpret_cast<std::byte*>(data.path_buffer.data()) + data.print_offset,
        print_name.c_str(), print_bytes + sizeof(wchar_t));

    const HANDLE directory = CreateFileW(
        link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }

    DWORD returned{};
    const DWORD input_size = static_cast<DWORD>(8 + data.data_length);
    const BOOL created = DeviceIoControl(
        directory, FSCTL_SET_REPARSE_POINT, &data, input_size, nullptr, 0, &returned, nullptr);
    CloseHandle(directory);
    if (!created) RemoveDirectoryW(link.c_str());
    return created != FALSE;
}

class DirectoryLinkGuard final {
public:
    explicit DirectoryLinkGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~DirectoryLinkGuard() { static_cast<void>(RemoveDirectoryW(path_.c_str())); }

    DirectoryLinkGuard(const DirectoryLinkGuard&) = delete;
    DirectoryLinkGuard& operator=(const DirectoryLinkGuard&) = delete;

private:
    std::filesystem::path path_;
};

bool TestBoundRootSurvivesPathReplacement() {
    TempDirectory sandbox(L"root-swap");
    const std::filesystem::path root = sandbox.path() / L"root";
    const std::filesystem::path bound_root = sandbox.path() / L"bound-root";
    const std::filesystem::path outside = sandbox.path() / L"outside";
    if (!Check(CreateDirectoryW(root.c_str(), nullptr) != FALSE,
               "root-swap fixture root was not created") ||
        !Check(CreateDirectoryW(outside.c_str(), nullptr) != FALSE,
               "root-swap fixture outside was not created") ||
        !Check(CreateDirectoryW((root / L"nested").c_str(), nullptr) != FALSE,
               "root-swap fixture bound parent was not created") ||
        !Check(CreateDirectoryW((outside / L"nested").c_str(), nullptr) != FALSE,
               "root-swap fixture outside parent was not created")) {
        return false;
    }

    anomaly::ReliableStorage storage(root.native());
    anomaly::ReliableStorage outside_storage(outside.native());
    const auto bound_read = Bytes("bound-read");
    const auto bound_delete = Bytes("bound-delete");
    const auto outside_read = Bytes("outside-read");
    const auto outside_delete = Bytes("outside-delete");
    if (!Check(storage.InitializationResult().ok(), "bound root was not initialized") ||
        !Check(outside_storage.InitializationResult().ok(), "outside root was not initialized") ||
        !Check(storage.WriteAtomic(L"read.bin", bound_read).ok(),
               "bound read fixture was not written") ||
        !Check(storage.WriteAtomic(L"delete.bin", bound_delete).ok(),
               "bound delete fixture was not written") ||
        !Check(outside_storage.WriteAtomic(L"read.bin", outside_read).ok(),
               "outside read fixture was not written") ||
        !Check(outside_storage.WriteAtomic(L"delete.bin", outside_delete).ok(),
               "outside delete fixture was not written")) {
        return false;
    }

    if (!Check(MoveFileExW(root.c_str(), bound_root.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
               "bound root could not be renamed") ||
        !Check(CreateJunction(root, outside),
               "replacement root junction could not be created")) {
        return false;
    }
    DirectoryLinkGuard link_guard(root);

    const auto write = storage.WriteAtomic(L"write.bin", Bytes("bound-write"));
    const auto nested_write =
        storage.WriteAtomic(L"nested\\write.bin", Bytes("bound-nested-write"));
    const auto read = storage.Read(L"read.bin", 1024);
    const auto deleted = storage.Delete(L"delete.bin");
    const auto outside_read_after = outside_storage.Read(L"read.bin", 1024);
    const auto outside_delete_after = outside_storage.Read(L"delete.bin", 1024);
    const auto outside_write_after = outside_storage.Read(L"write.bin", 1024);
    const auto outside_nested_write_after =
        outside_storage.Read(L"nested\\write.bin", 1024);

    anomaly::ReliableStorage rebound(bound_root.native());
    const auto bound_write_after = rebound.Read(L"write.bin", 1024);
    const auto bound_nested_write_after = rebound.Read(L"nested\\write.bin", 1024);
    const auto bound_delete_after = rebound.Read(L"delete.bin", 1024);
    return Check(write.ok(), "write through the bound root handle failed") &&
        Check(nested_write.ok(), "write through the bound parent handle failed") &&
        Check(read.ok() && Text(read.bytes) == "bound-read",
              "read followed the replacement root path") &&
        Check(deleted.ok(), "delete through the bound root handle failed") &&
        Check(bound_write_after.ok() && Text(bound_write_after.bytes) == "bound-write",
              "write did not land in the bound root") &&
        Check(bound_nested_write_after.ok() &&
                  Text(bound_nested_write_after.bytes) == "bound-nested-write",
              "nested write did not land in the bound parent") &&
        Check(!bound_delete_after.ok() &&
                  bound_delete_after.result.win32_error == ERROR_FILE_NOT_FOUND,
              "delete did not remove the file from the bound root") &&
        Check(outside_read_after.ok() && Text(outside_read_after.bytes) == "outside-read",
              "read fixture in outside was changed") &&
        Check(outside_delete_after.ok() && Text(outside_delete_after.bytes) == "outside-delete",
              "delete crossed into outside") &&
        Check(!outside_write_after.ok() &&
                  outside_write_after.result.win32_error == ERROR_FILE_NOT_FOUND,
              "write crossed into outside") &&
        Check(!outside_nested_write_after.ok() &&
                  outside_nested_write_after.result.win32_error == ERROR_FILE_NOT_FOUND,
              "nested write crossed into outside");
}

bool TestReplacedParentIsRejected() {
    TempDirectory sandbox(L"parent-swap");
    const std::filesystem::path root = sandbox.path() / L"root";
    const std::filesystem::path parent = root / L"parent";
    const std::filesystem::path bound_parent = root / L"bound-parent";
    const std::filesystem::path outside = sandbox.path() / L"outside";
    if (!Check(CreateDirectoryW(root.c_str(), nullptr) != FALSE,
               "parent-swap fixture root was not created") ||
        !Check(CreateDirectoryW(parent.c_str(), nullptr) != FALSE,
               "parent-swap fixture parent was not created") ||
        !Check(CreateDirectoryW(outside.c_str(), nullptr) != FALSE,
               "parent-swap fixture outside was not created")) {
        return false;
    }

    anomaly::ReliableStorage storage(root.native());
    anomaly::ReliableStorage outside_storage(outside.native());
    if (!Check(outside_storage.WriteAtomic(L"read.bin", Bytes("outside-read")).ok(),
               "parent-swap outside read fixture was not written") ||
        !Check(outside_storage.WriteAtomic(L"delete.bin", Bytes("outside-delete")).ok(),
               "parent-swap outside delete fixture was not written") ||
        !Check(MoveFileExW(
                   parent.c_str(), bound_parent.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
               "parent directory could not be renamed") ||
        !Check(CreateJunction(parent, outside),
               "replacement parent junction could not be created")) {
        return false;
    }
    DirectoryLinkGuard link_guard(parent);

    const auto write = storage.WriteAtomic(L"parent\\write.bin", Bytes("blocked"));
    const auto read = storage.Read(L"parent\\read.bin", 1024);
    const auto deleted = storage.Delete(L"parent\\delete.bin");
    const auto outside_read_after = outside_storage.Read(L"read.bin", 1024);
    const auto outside_delete_after = outside_storage.Read(L"delete.bin", 1024);
    const auto outside_write_after = outside_storage.Read(L"write.bin", 1024);
    return Check(!write.ok() && write.error == anomaly::StorageError::ReparsePoint,
                 "write accepted a replaced parent") &&
        Check(!read.ok() && read.result.error == anomaly::StorageError::ReparsePoint,
              "read accepted a replaced parent") &&
        Check(!deleted.ok() && deleted.error == anomaly::StorageError::ReparsePoint,
              "delete accepted a replaced parent") &&
        Check(outside_read_after.ok() && Text(outside_read_after.bytes) == "outside-read",
              "read through a replaced parent changed outside") &&
        Check(outside_delete_after.ok() && Text(outside_delete_after.bytes) == "outside-delete",
              "delete through a replaced parent reached outside") &&
        Check(!outside_write_after.ok() &&
                  outside_write_after.result.win32_error == ERROR_FILE_NOT_FOUND,
              "write through a replaced parent reached outside");
}

bool TestPathEscapeAndReparseEscapeAreRejected() {
    TempDirectory root(L"paths");
    TempDirectory outside(L"outside");
    anomaly::ReliableStorage storage(root.path().native());
    const auto value = Bytes("blocked");

    const auto traversal = storage.WriteAtomic(L"..\\escaped.bin", value);
    const std::wstring absolute_path = (outside.path() / L"absolute.bin").native();
    const auto absolute = storage.WriteAtomic(absolute_path, value);
    bool result = Check(
        !traversal.ok() && traversal.error == anomaly::StorageError::PathOutsideRoot,
        "parent traversal was accepted");
    result = Check(!absolute.ok(), "absolute path was accepted") && result;
    for (const std::wstring_view reserved : {
             L"CONIN$", L"CONOUT$.txt", L"COM\u00B9.bin", L"LPT\u00B2"}) {
        const auto device = storage.WriteAtomic(reserved, value);
        result = Check(!device.ok() && device.error == anomaly::StorageError::InvalidPath,
                       "reserved Windows device name was accepted") && result;
    }

    const std::filesystem::path link = root.path() / L"outside-link";
    if (!Check(CreateJunction(link, outside.path()), "junction fixture could not be created")) {
        return false;
    }
    DirectoryLinkGuard link_guard(link);
    const auto through_link = storage.WriteAtomic(L"outside-link\\escaped.bin", value);
    result = Check(
        !through_link.ok() && through_link.error == anomaly::StorageError::ReparsePoint,
        "reparse-point escape was accepted") &&
        Check(!std::filesystem::exists(outside.path() / L"escaped.bin"),
              "write through reparse point reached the outside directory") &&
        result;
    return result;
}

bool TestConcurrentWritesAreComplete() {
    TempDirectory root(L"concurrent");
    anomaly::ReliableStorage storage(root.path().native());
    constexpr std::size_t writer_count = 8;
    constexpr std::size_t payload_size = 64 * 1024;
    constexpr std::size_t writes_per_thread = 12;

    std::vector<std::vector<std::byte>> payloads;
    payloads.reserve(writer_count);
    for (std::size_t index = 0; index < writer_count; ++index) {
        payloads.emplace_back(payload_size, static_cast<std::byte>(index + 1));
        const std::uint64_t marker = 0xA110000000000000ULL + index;
        std::memcpy(payloads.back().data(), &marker, sizeof(marker));
    }

    std::atomic_bool writes_ok{true};
    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    for (std::size_t index = 0; index < writer_count; ++index) {
        writers.emplace_back([&, index] {
            for (std::size_t write = 0; write < writes_per_thread; ++write) {
                if (!storage.WriteAtomic(L"shared.bin", payloads[index]).ok()) {
                    writes_ok.store(false);
                    return;
                }
            }
        });
    }
    for (auto& writer : writers) writer.join();

    const auto read = storage.Read(L"shared.bin", payload_size);
    const bool matches_one = read.ok() && std::ranges::any_of(payloads, [&](const auto& payload) {
        return payload == read.bytes;
    });
    return Check(writes_ok.load(), "a concurrent atomic write failed") &&
        Check(matches_one, "concurrent writes produced a torn value") &&
        Check(!HasStorageTemp(root.path()), "concurrent writes left a temporary file");
}

bool TestConcurrentInstancesReplaceAtomically() {
    TempDirectory root(L"multi-instance");
    constexpr std::size_t writer_count = 8;
    constexpr std::size_t payload_size = 32 * 1024;
    std::vector<std::unique_ptr<anomaly::ReliableStorage>> stores;
    std::vector<std::vector<std::byte>> payloads;
    stores.reserve(writer_count);
    payloads.reserve(writer_count);
    for (std::size_t index = 0; index < writer_count; ++index) {
        stores.push_back(std::make_unique<anomaly::ReliableStorage>(root.path().native()));
        payloads.emplace_back(payload_size, static_cast<std::byte>(index + 1));
    }

    std::barrier ready(static_cast<std::ptrdiff_t>(writer_count));
    std::atomic_bool writes_ok{true};
    std::array<anomaly::StorageResult, writer_count> write_results{};
    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    for (std::size_t index = 0; index < writer_count; ++index) {
        writers.emplace_back([&, index] {
            ready.arrive_and_wait();
            write_results[index] = stores[index]->WriteAtomic(L"shared.bin", payloads[index]);
            if (!write_results[index].ok()) {
                writes_ok.store(false);
            }
        });
    }
    for (auto& writer : writers) writer.join();

    const auto read = stores.front()->Read(L"shared.bin", payload_size);
    const bool matches_one = read.ok() && std::ranges::any_of(payloads, [&](const auto& payload) {
        return payload == read.bytes;
    });
    if (!writes_ok.load()) {
        for (std::size_t index = 0; index < writer_count; ++index) {
            if (!write_results[index].ok()) {
                std::cerr << "cross-instance writer " << index << " error="
                          << static_cast<unsigned>(write_results[index].error)
                          << " win32=" << write_results[index].win32_error << '\n';
            }
        }
    }
    return Check(writes_ok.load(), "a cross-instance atomic replace failed") &&
        Check(matches_one, "cross-instance writes produced a torn value") &&
        Check(!HasStorageTemp(root.path()), "cross-instance writes left a temporary file");
}

bool TestReadSizeLimit() {
    TempDirectory root(L"limit");
    anomaly::ReliableStorage storage(root.path().native());
    const std::vector<std::byte> value(4096, std::byte{0x5A});
    if (!Check(storage.WriteAtomic(L"large.bin", value).ok(), "size fixture write failed")) {
        return false;
    }

    const auto limited = storage.Read(L"large.bin", value.size() - 1);
    const auto exact = storage.Read(L"large.bin", value.size());
    return Check(!limited.ok() &&
                     limited.result.error == anomaly::StorageError::SizeLimitExceeded &&
                     limited.result.win32_error == ERROR_FILE_TOO_LARGE && limited.bytes.empty(),
                 "oversized read did not fail before returning data") &&
        Check(exact.ok() && exact.bytes == value, "exact-size read failed");
}

}  // namespace

int main() {
    try {
        if (!TestFirstWriteOverwriteAndRead()) return 1;
        if (!TestFailedReplacePreservesOldDataAndCleansTemp()) return 2;
        if (!TestBoundRootSurvivesPathReplacement()) return 3;
        if (!TestReplacedParentIsRejected()) return 4;
        if (!TestPathEscapeAndReparseEscapeAreRejected()) return 5;
        if (!TestConcurrentWritesAreComplete()) return 6;
        if (!TestConcurrentInstancesReplaceAtomically()) return 7;
        if (!TestReadSizeLimit()) return 8;
    } catch (const std::exception& error) {
        std::cerr << "unexpected test exception: " << error.what() << '\n';
        return 100;
    } catch (...) {
        std::cerr << "unexpected non-standard test exception\n";
        return 101;
    }
    return 0;
}
