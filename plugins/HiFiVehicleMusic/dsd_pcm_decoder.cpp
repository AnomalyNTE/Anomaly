#include "plugins/HiFiVehicleMusic/dsd_pcm_decoder.hpp"

#include "plugins/HiFiVehicleMusic/dsd_reader.hpp"

#include "dsd2pcm.h"

#include <algorithm>
#include <memory>
#include <new>
#include <vector>

namespace hifi_vehicle_music {
namespace {

// One PCM frame per DSD byte per channel (8:1 decimation), so PCM frame
// indices and DsdReader frames are the same.
constexpr std::size_t kChunkFrames = 4096;

// ma_data_source_base must come first: miniaudio treats the pointer as both.
struct DsdPcmSource final {
    ma_data_source_base base;
    std::unique_ptr<DsdReader> reader;
    std::vector<dsd2pcm_ctx*> filters;  // one per channel
    std::vector<std::uint8_t> bytes;    // kChunkFrames interleaved frames
    ma_uint64 cursor;

    ~DsdPcmSource() {
        for (dsd2pcm_ctx* filter : filters) dsd2pcm_destroy(filter);
    }
};

DsdPcmSource* Self(ma_data_source* source) noexcept { return static_cast<DsdPcmSource*>(source); }

ma_result Read(ma_data_source* source, void* out, const ma_uint64 frames, ma_uint64* read) {
    DsdPcmSource* const self = Self(source);
    const unsigned channels = self->reader->Channels();
    auto* pcm = static_cast<float*>(out);
    ma_uint64 done = 0;
    try {
        while (done < frames) {
            const std::size_t want = static_cast<std::size_t>((std::min<ma_uint64>)(frames - done, kChunkFrames));
            const std::size_t got = self->reader->Read(self->bytes.data(), want);
            if (got == 0) break;
            for (unsigned c = 0; c < channels; ++c) {
                // The reader hands out MSB-first bytes.
                dsd2pcm_translate(self->filters[c], got, self->bytes.data() + c, channels, 0,
                    pcm + c, channels);
            }
            pcm += got * channels;
            done += got;
            if (got < want) break;
        }
    } catch (...) {
    }
    self->cursor += done;
    if (read != nullptr) *read = done;
    return done == 0 && frames > 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result Seek(ma_data_source* source, const ma_uint64 frame) {
    DsdPcmSource* const self = Self(source);
    self->reader->Seek(frame);
    for (dsd2pcm_ctx* filter : self->filters) dsd2pcm_reset(filter);
    self->cursor = (std::min<ma_uint64>)(frame, self->reader->Frames());
    return MA_SUCCESS;
}

ma_result Format(ma_data_source* source, ma_format* format, ma_uint32* channels, ma_uint32* rate,
    ma_channel* map, const size_t map_cap) {
    const DsdReader& reader = *Self(source)->reader;
    if (format != nullptr) *format = ma_format_f32;
    if (channels != nullptr) *channels = reader.Channels();
    if (rate != nullptr) *rate = reader.Rate() / 8;
    if (map != nullptr) ma_channel_map_init_standard(ma_standard_channel_map_microsoft, map, map_cap, reader.Channels());
    return MA_SUCCESS;
}

ma_result Cursor(ma_data_source* source, ma_uint64* cursor) {
    *cursor = Self(source)->cursor;
    return MA_SUCCESS;
}

ma_result Length(ma_data_source* source, ma_uint64* length) {
    *length = Self(source)->reader->Frames();
    return MA_SUCCESS;
}

ma_data_source_vtable g_source_vtable{&Read, &Seek, &Format, &Cursor, &Length, nullptr, 0};

ma_result InitFileW(void*, const wchar_t* path, const ma_decoding_backend_config*,
    const ma_allocation_callbacks*, ma_data_source** backend) {
    if (!IsDsdPath(path)) return MA_INVALID_FILE;
    std::unique_ptr<DsdPcmSource> self;
    try {
        auto reader = DsdReader::Open(path);
        if (!reader) return MA_INVALID_FILE;
        self.reset(new DsdPcmSource{});
        self->bytes.resize(kChunkFrames * reader->Channels());
        for (unsigned c = 0; c < reader->Channels(); ++c) {
            dsd2pcm_ctx* const filter = dsd2pcm_init();
            if (filter == nullptr) return MA_OUT_OF_MEMORY;
            self->filters.push_back(filter);
        }
        self->reader = std::move(reader);
    } catch (...) {
        return MA_OUT_OF_MEMORY;
    }
    ma_data_source_config config = ma_data_source_config_init();
    config.vtable = &g_source_vtable;
    if (ma_data_source_init(&config, &self->base) != MA_SUCCESS) return MA_ERROR;
    *backend = &self.release()->base;
    return MA_SUCCESS;
}

void Uninit(void*, ma_data_source* backend, const ma_allocation_callbacks*) {
    DsdPcmSource* const self = Self(backend);
    ma_data_source_uninit(&self->base);
    delete self;
}

ma_decoding_backend_vtable g_backend_vtable{nullptr, nullptr, &InitFileW, nullptr, &Uninit};

} // namespace

ma_decoding_backend_vtable* DsdPcmBackend() noexcept { return &g_backend_vtable; }

} // namespace hifi_vehicle_music
