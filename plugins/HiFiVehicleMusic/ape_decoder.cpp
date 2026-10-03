#include "plugins/HiFiVehicleMusic/ape_decoder.hpp"

#include "All.h"
#include "MACLib.h"

#include <new>

namespace hifi_vehicle_music {
namespace {

// ma_data_source_base must come first: miniaudio treats the pointer as both.
struct ApeSource final {
    ma_data_source_base base;
    APE::IAPEDecompress* decompress;
    ma_format format;
    ma_uint32 channels;
    ma_uint32 rate;
    ma_uint64 length;
};

ApeSource* Self(ma_data_source* source) noexcept { return static_cast<ApeSource*>(source); }

ma_result Read(ma_data_source* source, void* out, const ma_uint64 frames, ma_uint64* read) {
    ApeSource* const self = Self(source);
    APE::int64 retrieved{};
    int result = ERROR_UNDEFINED;
    try {
        result = self->decompress->GetData(static_cast<unsigned char*>(out),
            static_cast<APE::int64>(frames), &retrieved);
    } catch (...) {
        retrieved = 0;
    }
    if (read != nullptr) *read = static_cast<ma_uint64>(retrieved);
    if (result != ERROR_SUCCESS && retrieved == 0) return MA_ERROR;
    return retrieved == 0 && frames > 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result Seek(ma_data_source* source, const ma_uint64 frame) {
    try {
        return Self(source)->decompress->Seek(static_cast<APE::int64>(frame)) == ERROR_SUCCESS
            ? MA_SUCCESS : MA_ERROR;
    } catch (...) {
        return MA_ERROR;
    }
}

ma_result Format(ma_data_source* source, ma_format* format, ma_uint32* channels, ma_uint32* rate,
    ma_channel* map, const size_t map_cap) {
    const ApeSource* const self = Self(source);
    if (format != nullptr) *format = self->format;
    if (channels != nullptr) *channels = self->channels;
    if (rate != nullptr) *rate = self->rate;
    if (map != nullptr) ma_channel_map_init_standard(ma_standard_channel_map_microsoft, map, map_cap, self->channels);
    return MA_SUCCESS;
}

ma_result Cursor(ma_data_source* source, ma_uint64* cursor) {
    *cursor = static_cast<ma_uint64>(
        Self(source)->decompress->GetInfo(APE::IAPEDecompress::APE_DECOMPRESS_CURRENT_BLOCK));
    return MA_SUCCESS;
}

ma_result Length(ma_data_source* source, ma_uint64* length) {
    *length = Self(source)->length;
    return MA_SUCCESS;
}

ma_data_source_vtable g_source_vtable{&Read, &Seek, &Format, &Cursor, &Length, nullptr, 0};

ma_result InitFileW(void*, const wchar_t* path, const ma_decoding_backend_config*,
    const ma_allocation_callbacks*, ma_data_source** backend) {
    int error = ERROR_UNDEFINED;
    APE::IAPEDecompress* decompress{};
    try {
        // CreateIAPEDecompress only accepts .ape/.mac/.apl names, so other
        // files fail fast here and miniaudio moves on to its own decoders.
        decompress = CreateIAPEDecompress(path, &error, true, false, false);
    } catch (...) {
        decompress = nullptr;
    }
    if (decompress == nullptr || error != ERROR_SUCCESS) {
        delete decompress;
        return MA_INVALID_FILE;
    }
    using Info = APE::IAPEDecompress;
    const auto bits = decompress->GetInfo(Info::APE_INFO_BITS_PER_SAMPLE);
    const auto flags = decompress->GetInfo(Info::APE_INFO_FORMAT_FLAGS);
    ma_format format = ma_format_unknown;
    switch (bits) {
    case 8: format = ma_format_u8; break;
    case 16: format = ma_format_s16; break;
    case 24: format = ma_format_s24; break;
    case 32: format = (flags & APE_FORMAT_FLAG_FLOATING_POINT) != 0 ? ma_format_f32 : ma_format_s32; break;
    default: break;
    }
    // Signed 8-bit output would need the SDK's 8-bit processing; such files
    // are rare enough to leave unsupported.
    if (format == ma_format_unknown || (bits == 8 && (flags & APE_FORMAT_FLAG_SIGNED_8_BIT) != 0)) {
        delete decompress;
        return MA_INVALID_FILE;
    }
    auto* const self = new (std::nothrow) ApeSource{};
    if (self == nullptr) {
        delete decompress;
        return MA_OUT_OF_MEMORY;
    }
    self->decompress = decompress;
    self->format = format;
    self->channels = static_cast<ma_uint32>(decompress->GetInfo(Info::APE_INFO_CHANNELS));
    self->rate = static_cast<ma_uint32>(decompress->GetInfo(Info::APE_INFO_SAMPLE_RATE));
    self->length = static_cast<ma_uint64>(decompress->GetInfo(Info::APE_DECOMPRESS_TOTAL_BLOCKS));
    ma_data_source_config config = ma_data_source_config_init();
    config.vtable = &g_source_vtable;
    if (ma_data_source_init(&config, &self->base) != MA_SUCCESS) {
        delete decompress;
        delete self;
        return MA_ERROR;
    }
    *backend = &self->base;
    return MA_SUCCESS;
}

void Uninit(void*, ma_data_source* backend, const ma_allocation_callbacks*) {
    ApeSource* const self = Self(backend);
    ma_data_source_uninit(&self->base);
    delete self->decompress;
    delete self;
}

ma_decoding_backend_vtable g_backend_vtable{nullptr, nullptr, &InitFileW, nullptr, &Uninit};

} // namespace

ma_decoding_backend_vtable* ApeBackend() noexcept { return &g_backend_vtable; }

} // namespace hifi_vehicle_music
