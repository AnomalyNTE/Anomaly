#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace hifi_vehicle_music {

// 1-bit DSD byte with its bit order reversed (LSB-first <-> MSB-first).
[[nodiscard]] constexpr std::uint8_t ReverseBits(std::uint8_t value) noexcept {
    value = static_cast<std::uint8_t>((value & 0xF0U) >> 4 | (value & 0x0FU) << 4);
    value = static_cast<std::uint8_t>((value & 0xCCU) >> 2 | (value & 0x33U) << 2);
    return static_cast<std::uint8_t>((value & 0xAAU) >> 1 | (value & 0x55U) << 1);
}

// .dsf / .dff by extension.
[[nodiscard]] bool IsDsdPath(const std::wstring& path);

// Reads the raw DSD bitstream of an uncompressed DSF or DSDIFF (DFF) file.
// A frame is one byte per channel (8 DSD samples); Read returns frames
// interleaved by channel, first sample in the most significant bit. The data
// is passed through unchanged apart from that bit order.
class DsdReader final {
public:
    [[nodiscard]] static std::unique_ptr<DsdReader> Open(const std::wstring& path);

    [[nodiscard]] unsigned Channels() const noexcept { return channels_; }
    [[nodiscard]] unsigned Rate() const noexcept { return rate_; }  // DSD sample rate, e.g. 2822400
    [[nodiscard]] std::uint64_t Frames() const noexcept { return frames_; }

    void Seek(std::uint64_t frame) noexcept;
    std::size_t Read(std::uint8_t* out, std::size_t frames);

private:
    bool OpenDsf();
    bool OpenDff();

    std::ifstream file_;
    unsigned channels_{};
    unsigned rate_{};
    std::uint64_t frames_{};
    std::uint64_t data_offset_{};  // first data byte in the file
    std::uint64_t position_{};
    // DSF stores each channel in blocks of block_size_ bytes, LSB first when
    // its bits-per-sample field is 1. DFF interleaves bytes, MSB first.
    bool dsf_{};
    bool lsb_first_{};
    std::uint32_t block_size_{};
    std::uint64_t cached_group_{UINT64_MAX};
    std::vector<std::uint8_t> group_;
};

} // namespace hifi_vehicle_music
