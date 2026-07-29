#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace anomaly {

// These defaults keep a malformed image from turning a worker request into an
// unbounded allocation. Callers can lower them for a particular resource class.
inline constexpr std::size_t kDefaultUiResourceEncodedByteLimit = 32U * 1024U * 1024U;
inline constexpr std::size_t kDefaultUiResourceDecodedByteLimit = 256U * 1024U * 1024U;
inline constexpr std::uint32_t kDefaultUiResourceImageDimensionLimit = 16384U;

enum class UiResourceDecodeError : std::uint8_t {
    None,
    InvalidArgument,
    PathUnavailable,
    NotRegularFile,
    ReparsePoint,
    SizeLimitExceeded,
    IoFailure,
    OutOfMemory,
    ComInitializationFailed,
    UnsupportedImage,
    InvalidImage,
    ImageLimitExceeded,
    DecodeFailure,
};

// native_error is a Win32 error for file operations and an HRESULT bit pattern
// for WIC/COM operations. It is zero when no native status is available.
struct UiResourceReadResult final {
    UiResourceDecodeError error{UiResourceDecodeError::None};
    std::uint32_t native_error{};
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] bool Ok() const noexcept { return error == UiResourceDecodeError::None; }
    [[nodiscard]] explicit operator bool() const noexcept { return Ok(); }
};

struct UiRgba8Image final {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels;
};

struct UiImageDecodeLimits final {
    std::size_t maximum_encoded_bytes{kDefaultUiResourceEncodedByteLimit};
    std::size_t maximum_decoded_bytes{kDefaultUiResourceDecodedByteLimit};
    std::uint32_t maximum_width{kDefaultUiResourceImageDimensionLimit};
    std::uint32_t maximum_height{kDefaultUiResourceImageDimensionLimit};
};

struct UiImageDecodeResult final {
    UiResourceDecodeError error{UiResourceDecodeError::None};
    std::uint32_t native_error{};
    UiRgba8Image image;

    [[nodiscard]] bool Ok() const noexcept { return error == UiResourceDecodeError::None; }
    [[nodiscard]] explicit operator bool() const noexcept { return Ok(); }
};

// A Worker can use this hook to reserve host-owned staging capacity after a
// file/image size has been validated but before the corresponding heap buffer
// is allocated. It is intentionally a noexcept C-style callback so decoder
// failure paths remain allocation-free.
using UiResourceAllocationAdmissionCallback = bool (*)(
    void* user, std::size_t byte_count) noexcept;

struct UiResourceAllocationAdmission final {
    void* user{};
    UiResourceAllocationAdmissionCallback reserve{};

    [[nodiscard]] bool Reserve(const std::size_t byte_count) const noexcept {
        return reserve == nullptr || reserve(user, byte_count);
    }
};

// Reads one ordinary, non-reparse-point file into owned memory. The caller must
// resolve and confine the path to its package before calling this helper.
[[nodiscard]] UiResourceReadResult ReadUiResourceBytes(
    const std::filesystem::path& path, std::size_t maximum_bytes,
    UiResourceAllocationAdmission admission = {}) noexcept;

// Reads from an already-opened regular file handle. The handle remains owned by
// the caller and must stay live until this synchronous Worker operation returns.
[[nodiscard]] UiResourceReadResult ReadUiResourceBytesFromFileHandle(
    void* native_file_handle, std::size_t maximum_bytes,
    UiResourceAllocationAdmission admission = {}) noexcept;

// Decodes the first frame exposed by Windows WIC into straight RGBA8 pixels.
// This is synchronous CPU work for a Worker domain; it neither uploads nor
// caches a GPU resource.
[[nodiscard]] UiImageDecodeResult DecodeUiImageRgba8(
    std::span<const std::uint8_t> encoded_bytes,
    const UiImageDecodeLimits& limits = {},
    UiResourceAllocationAdmission admission = {}) noexcept;

}  // namespace anomaly
