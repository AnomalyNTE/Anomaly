#include "anomaly/ui_resource_decoder.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace anomaly {
namespace {

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}

    ~UniqueHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return value_; }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

class ComApartment final {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }

    [[nodiscard]] bool Usable() const noexcept {
        // RPC_E_CHANGED_MODE means this worker is already initialized in a
        // different apartment. COM remains usable on that thread.
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_{};
};

[[nodiscard]] std::uint32_t NativeStatus(const HRESULT value) noexcept {
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] UiImageDecodeResult ImageFailure(
    const UiResourceDecodeError error, const std::uint32_t native_error = 0) noexcept {
    UiImageDecodeResult result;
    result.error = error;
    result.native_error = native_error;
    return result;
}

[[nodiscard]] UiResourceDecodeError ErrorForWicFailure(const HRESULT result) noexcept {
    if (result == WINCODEC_ERR_UNKNOWNIMAGEFORMAT ||
        result == WINCODEC_ERR_COMPONENTNOTFOUND ||
        result == WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT) {
        return UiResourceDecodeError::UnsupportedImage;
    }
    if (result == WINCODEC_ERR_BADIMAGE) return UiResourceDecodeError::InvalidImage;
    return UiResourceDecodeError::DecodeFailure;
}

[[nodiscard]] bool ProductFits(
    const std::uint64_t left, const std::uint64_t right, std::uint64_t& product) noexcept {
    if (left != 0 && right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        return false;
    }
    product = left * right;
    return true;
}

UiResourceReadResult ReadOpenFile(
    const HANDLE file, const std::size_t maximum_bytes,
    const UiResourceAllocationAdmission admission) noexcept {
    UiResourceReadResult result;
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        result.error = UiResourceDecodeError::InvalidArgument;
        return result;
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            file, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
        result.error = UiResourceDecodeError::IoFailure;
        result.native_error = GetLastError();
        return result;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.error = UiResourceDecodeError::ReparsePoint;
        return result;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        GetFileType(file) != FILE_TYPE_DISK) {
        result.error = UiResourceDecodeError::NotRegularFile;
        return result;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size)) {
        result.error = UiResourceDecodeError::IoFailure;
        result.native_error = GetLastError();
        return result;
    }
    if (file_size.QuadPart < 0 ||
        static_cast<std::uint64_t>(file_size.QuadPart) >
            static_cast<std::uint64_t>(maximum_bytes) ||
        static_cast<std::uint64_t>(file_size.QuadPart) >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        result.error = UiResourceDecodeError::SizeLimitExceeded;
        return result;
    }

    const std::size_t byte_count = static_cast<std::size_t>(file_size.QuadPart);
    if (!admission.Reserve(byte_count)) {
        result.error = UiResourceDecodeError::OutOfMemory;
        result.native_error = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    }
    try {
        result.bytes.resize(byte_count);
    } catch (...) {
        result.error = UiResourceDecodeError::OutOfMemory;
        result.native_error = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    }

    std::size_t offset{};
    while (offset < result.bytes.size()) {
        const std::size_t remaining = result.bytes.size() - offset;
        const DWORD requested = static_cast<DWORD>((std::min)(
            remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read{};
        const BOOL read_succeeded =
            ReadFile(file, result.bytes.data() + offset, requested, &read, nullptr);
        if (!read_succeeded || read != requested) {
            result.bytes.clear();
            result.error = UiResourceDecodeError::IoFailure;
            result.native_error = read_succeeded ? ERROR_HANDLE_EOF : GetLastError();
            return result;
        }
        offset += read;
    }
    return result;
}

}  // namespace

UiResourceReadResult ReadUiResourceBytes(
    const std::filesystem::path& path, const std::size_t maximum_bytes,
    const UiResourceAllocationAdmission admission) noexcept {
    UiResourceReadResult result;
    if (path.empty()) {
        result.error = UiResourceDecodeError::InvalidArgument;
        return result;
    }

    const HANDLE raw_handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (raw_handle == INVALID_HANDLE_VALUE) {
        result.error = UiResourceDecodeError::PathUnavailable;
        result.native_error = GetLastError();
        return result;
    }
    const UniqueHandle file(raw_handle);
    return ReadOpenFile(file.Get(), maximum_bytes, admission);
}

UiResourceReadResult ReadUiResourceBytesFromFileHandle(
    void* const native_file_handle, const std::size_t maximum_bytes,
    const UiResourceAllocationAdmission admission) noexcept {
    return ReadOpenFile(static_cast<HANDLE>(native_file_handle), maximum_bytes, admission);
}

UiImageDecodeResult DecodeUiImageRgba8(
    const std::span<const std::uint8_t> encoded_bytes,
    const UiImageDecodeLimits& limits,
    const UiResourceAllocationAdmission admission) noexcept {
    if (limits.maximum_encoded_bytes == 0 || limits.maximum_decoded_bytes == 0 ||
        limits.maximum_width == 0 || limits.maximum_height == 0 || encoded_bytes.empty()) {
        return ImageFailure(UiResourceDecodeError::InvalidArgument);
    }
    if (encoded_bytes.size() > limits.maximum_encoded_bytes ||
        encoded_bytes.size() > static_cast<std::size_t>((std::numeric_limits<UINT>::max)())) {
        return ImageFailure(UiResourceDecodeError::SizeLimitExceeded);
    }

    const ComApartment apartment;
    if (!apartment.Usable()) {
        return ImageFailure(
            UiResourceDecodeError::ComInitializationFailed, NativeStatus(apartment.Result()));
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT status = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));

    Microsoft::WRL::ComPtr<IWICStream> stream;
    status = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));

    // IWICStream only borrows this buffer for the synchronous decode below.
    auto* input = const_cast<BYTE*>(
        reinterpret_cast<const BYTE*>(encoded_bytes.data()));
    status = stream->InitializeFromMemory(input, static_cast<DWORD>(encoded_bytes.size()));
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    status = factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    status = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));

    UINT width{};
    UINT height{};
    status = frame->GetSize(&width, &height);
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));
    if (width == 0 || height == 0) return ImageFailure(UiResourceDecodeError::InvalidImage);
    if (width > limits.maximum_width || height > limits.maximum_height) {
        return ImageFailure(UiResourceDecodeError::ImageLimitExceeded);
    }

    std::uint64_t pixel_count{};
    std::uint64_t byte_count{};
    if (!ProductFits(width, height, pixel_count) || !ProductFits(pixel_count, 4U, byte_count) ||
        byte_count > limits.maximum_decoded_bytes ||
        byte_count > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) ||
        byte_count > static_cast<std::uint64_t>((std::numeric_limits<UINT>::max)()) ||
        static_cast<std::uint64_t>(width) * 4U >
            static_cast<std::uint64_t>((std::numeric_limits<UINT>::max)())) {
        return ImageFailure(UiResourceDecodeError::ImageLimitExceeded);
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    status = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));
    status = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));

    if (!admission.Reserve(static_cast<std::size_t>(byte_count))) {
        return ImageFailure(UiResourceDecodeError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY);
    }

    UiRgba8Image image;
    try {
        image.pixels.resize(static_cast<std::size_t>(byte_count));
    } catch (...) {
        return ImageFailure(UiResourceDecodeError::OutOfMemory, ERROR_NOT_ENOUGH_MEMORY);
    }

    status = converter->CopyPixels(
        nullptr, width * 4U, static_cast<UINT>(byte_count), image.pixels.data());
    if (FAILED(status)) return ImageFailure(ErrorForWicFailure(status), NativeStatus(status));

    image.width = width;
    image.height = height;
    UiImageDecodeResult result;
    result.image = std::move(image);
    return result;
}

}  // namespace anomaly
