#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace anomaly {

enum class Ue5BitReaderError : std::uint8_t {
    None,
    EndOfData,
    InvalidBitCount,
    Misaligned,
    InvalidMaximum,
    ValueOutOfRange,
};

// An offline, bounded reader for UE-style least-significant-bit-first bit
// streams. Failed reads leave the output value and cursor unchanged, then set
// a sticky error that makes later reads fail without consuming data.
class Ue5BitReader final {
public:
    explicit Ue5BitReader(std::span<const std::uint8_t> bytes) noexcept;
    Ue5BitReader(std::span<const std::uint8_t> bytes, std::size_t bit_count) noexcept;

    [[nodiscard]] std::size_t BitCount() const noexcept;
    [[nodiscard]] std::size_t BitPosition() const noexcept;
    [[nodiscard]] std::size_t BitsRemaining() const noexcept;
    [[nodiscard]] bool HasError() const noexcept;
    [[nodiscard]] Ue5BitReaderError Error() const noexcept;

    [[nodiscard]] bool ReadBool(bool& value) noexcept;
    [[nodiscard]] bool ReadBit(bool& value) noexcept;
    [[nodiscard]] bool ReadUnsigned(std::uint32_t bit_count, std::uint64_t& value) noexcept;
    [[nodiscard]] bool ReadSigned(std::uint32_t bit_count, std::int64_t& value) noexcept;

    // Reads whole bytes only when the current position is byte-aligned.
    [[nodiscard]] bool ReadBytes(std::span<std::uint8_t> bytes) noexcept;

    // Decodes UE FArchive SerializeInt's fixed-width [0, max) representation:
    // ceil(log2(max)) low-bit-first bits. Encoded values outside that range fail.
    [[nodiscard]] bool ReadSerializeInt(std::uint32_t max, std::uint32_t& value) noexcept;

private:
    [[nodiscard]] bool EnsureBits(std::size_t bit_count) noexcept;
    void Fail(Ue5BitReaderError error) noexcept;

    std::span<const std::uint8_t> bytes_;
    std::size_t bit_count_{};
    std::size_t bit_position_{};
    Ue5BitReaderError error_{Ue5BitReaderError::None};
};

}  // namespace anomaly
