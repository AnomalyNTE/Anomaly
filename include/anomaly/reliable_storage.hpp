#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace anomaly {

enum class StorageError : std::uint8_t {
    None,
    InvalidRoot,
    InvalidPath,
    PathOutsideRoot,
    ReparsePoint,
    SizeLimitExceeded,
    IoFailure,
    OutOfMemory,
    InternalFailure,
};

struct StorageResult {
    StorageError error{StorageError::None};
    std::uint32_t win32_error{};
    std::size_t bytes_processed{};

    [[nodiscard]] bool ok() const noexcept { return error == StorageError::None; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
};

struct StorageReadResult {
    StorageResult result;
    std::vector<std::byte> bytes;

    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
};

class ReliableStorage final {
public:
    explicit ReliableStorage(std::wstring_view root_directory) noexcept;
    ~ReliableStorage();

    ReliableStorage(const ReliableStorage&) = delete;
    ReliableStorage& operator=(const ReliableStorage&) = delete;
    ReliableStorage(ReliableStorage&&) = delete;
    ReliableStorage& operator=(ReliableStorage&&) = delete;

    [[nodiscard]] StorageResult InitializationResult() const noexcept;
    [[nodiscard]] StorageReadResult Read(
        std::wstring_view relative_path, std::size_t max_bytes) noexcept;
    [[nodiscard]] StorageResult WriteAtomic(
        std::wstring_view relative_path, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] StorageResult Delete(std::wstring_view relative_path) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
