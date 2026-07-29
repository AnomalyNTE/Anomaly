#include "anomaly/ue5_bit_reader.hpp"

#include <cstring>
#include <limits>

namespace anomaly {
namespace {

[[nodiscard]] std::uint32_t BitsForExclusiveMaximum(const std::uint32_t maximum) noexcept {
    std::uint32_t bits{};
    for (std::uint32_t value = maximum - 1U; value != 0; value >>= 1U) {
        ++bits;
    }
    return bits;
}

}  // namespace

Ue5BitReader::Ue5BitReader(const std::span<const std::uint8_t> bytes) noexcept
    : Ue5BitReader(
          bytes,
          bytes.size() <= (std::numeric_limits<std::size_t>::max)() / 8U
              ? bytes.size() * 8U
              : 0U) {
    if (bytes.size() > (std::numeric_limits<std::size_t>::max)() / 8U) {
        Fail(Ue5BitReaderError::InvalidBitCount);
    }
}

Ue5BitReader::Ue5BitReader(
    const std::span<const std::uint8_t> bytes,
    const std::size_t bit_count) noexcept
    : bytes_(bytes), bit_count_(bit_count) {
    const std::size_t required_bytes = bit_count / 8U;
    const bool requires_partial_byte = (bit_count % 8U) != 0;
    if (required_bytes > bytes.size() ||
        (required_bytes == bytes.size() && requires_partial_byte)) {
        bit_count_ = 0;
        Fail(Ue5BitReaderError::InvalidBitCount);
    }
}

std::size_t Ue5BitReader::BitCount() const noexcept {
    return bit_count_;
}

std::size_t Ue5BitReader::BitPosition() const noexcept {
    return bit_position_;
}

std::size_t Ue5BitReader::BitsRemaining() const noexcept {
    return bit_count_ - bit_position_;
}

bool Ue5BitReader::HasError() const noexcept {
    return error_ != Ue5BitReaderError::None;
}

Ue5BitReaderError Ue5BitReader::Error() const noexcept {
    return error_;
}

bool Ue5BitReader::ReadBool(bool& value) noexcept {
    std::uint64_t bit{};
    if (!ReadUnsigned(1, bit)) return false;
    value = bit != 0;
    return true;
}

bool Ue5BitReader::ReadBit(bool& value) noexcept {
    return ReadBool(value);
}

bool Ue5BitReader::ReadUnsigned(
    const std::uint32_t bit_count,
    std::uint64_t& value) noexcept {
    if (bit_count > 64U) {
        Fail(Ue5BitReaderError::InvalidBitCount);
        return false;
    }
    if (!EnsureBits(bit_count)) return false;

    std::uint64_t decoded{};
    for (std::uint32_t index = 0; index < bit_count; ++index) {
        const std::size_t source_bit = bit_position_ + index;
        const std::uint8_t bit = static_cast<std::uint8_t>(
            (bytes_[source_bit / 8U] >> (source_bit % 8U)) & 1U);
        decoded |= static_cast<std::uint64_t>(bit) << index;
    }

    bit_position_ += bit_count;
    value = decoded;
    return true;
}

bool Ue5BitReader::ReadSigned(
    const std::uint32_t bit_count,
    std::int64_t& value) noexcept {
    std::uint64_t decoded{};
    if (!ReadUnsigned(bit_count, decoded)) return false;

    std::int64_t signed_value{};
    if (bit_count == 0U) {
        signed_value = 0;
    } else if (bit_count == 64U) {
        if ((decoded & (std::uint64_t{1} << 63U)) == 0U) {
            signed_value = static_cast<std::int64_t>(decoded);
        } else {
            signed_value = static_cast<std::int64_t>(
                               decoded & static_cast<std::uint64_t>(
                                             (std::numeric_limits<std::int64_t>::max)())) -
                (std::numeric_limits<std::int64_t>::max)() - 1;
        }
    } else {
        const std::uint64_t sign_bit = std::uint64_t{1} << (bit_count - 1U);
        if ((decoded & sign_bit) == 0U) {
            signed_value = static_cast<std::int64_t>(decoded);
        } else {
            const std::uint64_t magnitude = (std::uint64_t{1} << bit_count) - decoded;
            signed_value = -static_cast<std::int64_t>(magnitude);
        }
    }

    value = signed_value;
    return true;
}

bool Ue5BitReader::ReadBytes(const std::span<std::uint8_t> bytes) noexcept {
    if (HasError()) return false;
    if ((bit_position_ % 8U) != 0U) {
        Fail(Ue5BitReaderError::Misaligned);
        return false;
    }
    if (bytes.size() > BitsRemaining() / 8U) {
        Fail(Ue5BitReaderError::EndOfData);
        return false;
    }
    if (bytes.empty()) return true;

    std::memmove(bytes.data(), bytes_.data() + bit_position_ / 8U, bytes.size());
    bit_position_ += bytes.size() * 8U;
    return true;
}

bool Ue5BitReader::ReadSerializeInt(
    const std::uint32_t maximum,
    std::uint32_t& value) noexcept {
    if (HasError()) return false;
    if (maximum == 0U) {
        Fail(Ue5BitReaderError::InvalidMaximum);
        return false;
    }

    const std::size_t original_position = bit_position_;
    std::uint64_t decoded{};
    if (!ReadUnsigned(BitsForExclusiveMaximum(maximum), decoded)) return false;
    if (decoded >= maximum) {
        bit_position_ = original_position;
        Fail(Ue5BitReaderError::ValueOutOfRange);
        return false;
    }

    value = static_cast<std::uint32_t>(decoded);
    return true;
}

bool Ue5BitReader::EnsureBits(const std::size_t bit_count) noexcept {
    if (HasError()) return false;
    if (bit_count > BitsRemaining()) {
        Fail(Ue5BitReaderError::EndOfData);
        return false;
    }
    return true;
}

void Ue5BitReader::Fail(const Ue5BitReaderError error) noexcept {
    if (error_ == Ue5BitReaderError::None) error_ = error;
}

}  // namespace anomaly
