#include "anomaly/ue5_network_deserializer.hpp"

#include "anomaly/ue5_bit_reader.hpp"

#include <algorithm>
#include <bit>
#include <deque>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace anomaly {
namespace {

constexpr std::size_t kHardMaximumPacketBits = 1024U * 1024U * 8U;
constexpr std::size_t kHardMaximumConnections = 4096;
constexpr std::size_t kHardMaximumPacketHistory = 65536;
constexpr std::size_t kHardMaximumBunches = 65536;
constexpr std::size_t kHardMaximumChannels = 65536;
constexpr std::size_t kHardMaximumOpenPartials = 4096;
constexpr std::size_t kHardMaximumPartialFragments = 4096;
constexpr std::size_t kHardMaximumPartialPacketAge = 65536;
constexpr std::size_t kHardMaximumPartialStateBits = 32U * 1024U * 1024U * 8U;
constexpr std::size_t kHardMaximumFields = 65536;
constexpr std::size_t kHardMaximumFieldBytes = 1024U * 1024U;
constexpr std::size_t kHardMaximumSchemas = 4096;
constexpr std::size_t kHardMaximumTotalChannels = 65536;
constexpr std::size_t kHardMaximumTotalPacketHistory = 1024U * 1024U;
constexpr std::size_t kHardMaximumTotalOpenPartials = 8192;
constexpr std::size_t kHardMaximumTotalPartialFragments = 512U * 1024U;
constexpr std::size_t kHardMaximumTotalPartialStateBits = 128U * 1024U * 1024U * 8U;

[[nodiscard]] Ue5PacketDeserializeResult Failure(
    const Ue5PacketDeserializeStatus status,
    std::string diagnostic) {
    Ue5PacketDeserializeResult result;
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    return result;
}

[[nodiscard]] Ue5PacketDeserializeStatus ReaderStatus(const Ue5BitReader& reader) noexcept {
    return reader.Error() == Ue5BitReaderError::EndOfData
        ? Ue5PacketDeserializeStatus::Truncated
        : Ue5PacketDeserializeStatus::Malformed;
}

[[nodiscard]] const char* ReaderErrorName(const Ue5BitReader& reader) noexcept {
    switch (reader.Error()) {
    case Ue5BitReaderError::None: return "none";
    case Ue5BitReaderError::EndOfData: return "end-of-data";
    case Ue5BitReaderError::InvalidBitCount: return "invalid-bit-count";
    case Ue5BitReaderError::Misaligned: return "misaligned";
    case Ue5BitReaderError::InvalidMaximum: return "invalid-maximum";
    case Ue5BitReaderError::ValueOutOfRange: return "value-out-of-range";
    }
    return "unknown";
}

[[nodiscard]] bool ValidBitWidth(const std::uint8_t bit_width) noexcept {
    return bit_width != 0U && bit_width <= 64U;
}

[[nodiscard]] bool IsExactByteSpan(
    const std::span<const std::uint8_t> bytes,
    const std::size_t bit_count) noexcept {
    const std::size_t full_bytes = bit_count / 8U;
    const bool partial_byte = (bit_count % 8U) != 0U;
    if (full_bytes > bytes.size()) return false;
    if (!partial_byte) return full_bytes == bytes.size();
    return full_bytes < bytes.size() && full_bytes + 1U == bytes.size();
}

[[nodiscard]] bool IsValidUtf8(const std::string_view value) noexcept {
    for (std::size_t index{}; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t continuation_count{};
        std::uint32_t code_point{};
        std::uint32_t minimum{};
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuation_count > value.size() - index - 1U) return false;
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto byte = static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U) return false;
            code_point = (code_point << 6U) | (byte & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

enum class SequenceOrder : std::uint8_t {
    Equal,
    Newer,
    Older,
    Ambiguous,
    Invalid,
};

[[nodiscard]] SequenceOrder CompareSequence(
    const std::uint32_t candidate,
    const std::uint32_t previous,
    const std::uint32_t maximum) noexcept {
    if (maximum <= 1U || candidate >= maximum || previous >= maximum) {
        return SequenceOrder::Invalid;
    }
    const std::uint32_t forward = candidate >= previous
        ? candidate - previous
        : maximum - previous + candidate;
    if (forward == 0U) return SequenceOrder::Equal;
    if ((maximum % 2U) == 0U && forward == maximum / 2U) {
        return SequenceOrder::Ambiguous;
    }
    return forward < (maximum + 1U) / 2U
        ? SequenceOrder::Newer
        : SequenceOrder::Older;
}

[[nodiscard]] bool ProductAtMost(
    const std::size_t maximum,
    const std::initializer_list<std::size_t> factors) noexcept {
    std::size_t product{1U};
    for (const std::size_t factor : factors) {
        if (factor == 0U) return true;
        if (product > maximum / factor) return false;
        product *= factor;
    }
    return true;
}

[[nodiscard]] bool AppendBits(
    std::vector<std::uint8_t>& destination,
    std::size_t& destination_bits,
    const std::vector<std::uint8_t>& source,
    const std::size_t source_bits,
    const std::size_t maximum_bits) {
    if (source_bits > maximum_bits || destination_bits > maximum_bits - source_bits) {
        return false;
    }
    const std::size_t combined_bits = destination_bits + source_bits;
    const std::size_t combined_bytes = combined_bits / 8U +
        ((combined_bits % 8U) == 0U ? 0U : 1U);
    destination.resize(combined_bytes, 0U);
    for (std::size_t index{}; index < source_bits; ++index) {
        const bool set = (source[index / 8U] &
                          static_cast<std::uint8_t>(1U << (index % 8U))) != 0U;
        if (set) {
            const std::size_t target_bit = destination_bits + index;
            destination[target_bit / 8U] |=
                static_cast<std::uint8_t>(1U << (target_bit % 8U));
        }
    }
    destination_bits = combined_bits;
    return true;
}

[[nodiscard]] std::optional<std::string> ValidateProfile(
    const Ue5PacketProtocolProfile& profile) {
    if (!profile.post_handler_boundary_validated) {
        return "the packet profile has not passed boundary validation";
    }
    if (profile.profile_hash.empty()) {
        return "the packet profile has no source identity";
    }
    if (!ValidBitWidth(profile.header.sequence_bits) ||
        profile.header.bunch_count_bits > 32U) {
        return "the packet header has an invalid bit width";
    }
    switch (profile.header.ack_mode) {
    case Ue5AckMode::None:
        if (profile.header.ack_sequence_bits != 0U || profile.header.ack_history_bits != 0U) {
            return "an ACK-disabled packet header declares ACK fields";
        }
        break;
    case Ue5AckMode::Always:
    case Ue5AckMode::PresenceBit:
        if (!ValidBitWidth(profile.header.ack_sequence_bits) ||
            profile.header.ack_history_bits > 64U) {
            return "the packet header has invalid ACK field widths";
        }
        break;
    default:
        return "the packet header has an unknown ACK mode";
    }

    const auto& bunch = profile.bunch;
    if (bunch.maximum_channels == 0U || bunch.maximum_channels > kHardMaximumChannels ||
        bunch.maximum_channel_types == 0U || bunch.maximum_reliable_sequence <= 1U ||
        bunch.maximum_payload_bits == 0U ||
        bunch.maximum_payload_bits == (std::numeric_limits<std::uint32_t>::max)()) {
        return "the bunch layout has an invalid exclusive maximum";
    }
    switch (bunch.partial_id_mode) {
    case Ue5PartialIdMode::Explicit:
        if (bunch.maximum_partial_id == 0U) {
            return "an explicit partial identifier has no maximum";
        }
        break;
    case Ue5PartialIdMode::None:
        if (bunch.maximum_partial_id != 0U) {
            return "a non-explicit partial identifier declares a maximum";
        }
        break;
    case Ue5PartialIdMode::ReliableSequence:
        return "a reliable sequence alone is not a verified partial-bunch grouping codec";
    default:
        return "the bunch layout has an unknown partial identifier mode";
    }

    const auto& limits = profile.limits;
    if (limits.maximum_packet_bits == 0U || limits.maximum_packet_bits > kHardMaximumPacketBits ||
        limits.maximum_connections == 0U ||
        limits.maximum_connections > kHardMaximumConnections ||
        limits.maximum_packet_history == 0U ||
        limits.maximum_packet_history > kHardMaximumPacketHistory ||
        limits.maximum_bunches_per_packet == 0U ||
        limits.maximum_bunches_per_packet > kHardMaximumBunches ||
        limits.maximum_open_partial_bunches == 0U ||
        limits.maximum_open_partial_bunches > kHardMaximumOpenPartials ||
        limits.maximum_partial_fragments == 0U ||
        limits.maximum_partial_fragments > kHardMaximumPartialFragments ||
        limits.maximum_partial_packet_age == 0U ||
        limits.maximum_partial_packet_age > kHardMaximumPartialPacketAge ||
        limits.maximum_partial_bunch_bits == 0U ||
        limits.maximum_partial_bunch_bits > kHardMaximumPacketBits ||
        limits.maximum_partial_state_bits == 0U ||
        limits.maximum_partial_state_bits > kHardMaximumPartialStateBits ||
        limits.maximum_partial_state_bits < limits.maximum_partial_bunch_bits ||
        limits.maximum_fields_per_bunch == 0U ||
        limits.maximum_fields_per_bunch > kHardMaximumFields || limits.maximum_field_bytes == 0U ||
        limits.maximum_field_bytes > kHardMaximumFieldBytes ||
        bunch.maximum_payload_bits > limits.maximum_packet_bits ||
        limits.maximum_partial_bunch_bits < bunch.maximum_payload_bits) {
        return "the packet limits are inconsistent with the bunch layout";
    }
    if (profile.header.sequence_bits < (std::numeric_limits<std::size_t>::digits) &&
        limits.maximum_packet_history >=
            (std::size_t{1} << profile.header.sequence_bits)) {
        return "the packet replay history reaches the packet sequence space";
    }
    if (!ProductAtMost(
            kHardMaximumTotalChannels,
            {limits.maximum_connections, 2U, bunch.maximum_channels}) ||
        !ProductAtMost(
            kHardMaximumTotalPacketHistory,
            {limits.maximum_connections, 2U, limits.maximum_packet_history}) ||
        !ProductAtMost(
            kHardMaximumTotalOpenPartials,
            {limits.maximum_connections, 2U, limits.maximum_open_partial_bunches}) ||
        !ProductAtMost(
            kHardMaximumTotalPartialFragments,
            {limits.maximum_connections, 2U, limits.maximum_open_partial_bunches,
             limits.maximum_partial_fragments}) ||
        !ProductAtMost(
            kHardMaximumTotalPartialStateBits,
            {limits.maximum_connections, 2U, limits.maximum_partial_state_bits})) {
        return "the packet profile exceeds the decoder's aggregate state budget";
    }

    if (profile.schemas.size() > kHardMaximumSchemas) {
        return "the profile contains too many channel schemas";
    }
    std::unordered_set<std::uint32_t> channel_types;
    for (const auto& schema : profile.schemas) {
        if (schema.id.empty() || schema.channel_type >= bunch.maximum_channel_types ||
            !channel_types.insert(schema.channel_type).second) {
            return "the profile contains an invalid or duplicate channel schema";
        }
        if (schema.fields.size() > limits.maximum_fields_per_bunch ||
            schema.fields.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
            return "the channel schema has too many fields";
        }
        switch (schema.family) {
        case Ue5SchemaFamily::LegacyRepLayout:
        case Ue5SchemaFamily::Iris:
            break;
        default:
            return "the channel schema has an unknown UE schema family";
        }
        if (schema.encoding != Ue5FieldStreamEncoding::VerifiedTaggedFieldsV1) {
            return "the channel schema has an unsupported field stream encoding";
        }
        std::unordered_set<std::uint32_t> handles;
        for (const auto& field : schema.fields) {
            if (field.name.empty() || field.handle == (std::numeric_limits<std::uint32_t>::max)() ||
                !handles.insert(field.handle).second) {
                return "the channel schema contains an invalid or duplicate field";
            }
            switch (field.encoding) {
            case Ue5FieldEncoding::Unsigned:
            case Ue5FieldEncoding::Signed:
                if (!ValidBitWidth(field.bit_width)) {
                    return "an integer field has an invalid bit width";
                }
                break;
            case Ue5FieldEncoding::Utf8:
            case Ue5FieldEncoding::Bytes:
                if (field.maximum_length == (std::numeric_limits<std::uint32_t>::max)() ||
                    field.maximum_length > limits.maximum_field_bytes) {
                    return "a byte field has an invalid maximum length";
                }
                break;
            case Ue5FieldEncoding::Boolean:
            case Ue5FieldEncoding::Float32:
            case Ue5FieldEncoding::Vector3Float32:
                break;
            default:
                return "the channel schema has an unknown field encoding";
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> ValidateUe5PacketProtocolProfile(
    const Ue5PacketProtocolProfile& profile) {
    return ValidateProfile(profile);
}

class Ue5NetworkDeserializer::Impl final {
public:
    explicit Impl(Ue5PacketProtocolProfile profile)
        : profile_(std::move(profile)),
          validation_error_(ValidateUe5PacketProtocolProfile(profile_)) {}

    [[nodiscard]] Ue5PacketDeserializeResult Deserialize(const Ue5PacketInput& input) {
        try {
            if (validation_error_) {
                return Failure(Ue5PacketDeserializeStatus::UnsupportedProfile, *validation_error_);
            }
            if (input.profile_hash != profile_.profile_hash) {
                return Failure(
                    Ue5PacketDeserializeStatus::UnsupportedProfile,
                    "the packet identity does not match the active profile");
            }
            if (input.direction != Ue5PacketDirection::Inbound &&
                input.direction != Ue5PacketDirection::Outbound) {
                return Failure(
                    Ue5PacketDeserializeStatus::UnsupportedProfile,
                    "the packet direction is required to isolate connection state");
            }
            if (input.stage != Ue5PacketCaptureStage::PostPacketHandler) {
                return Failure(
                    Ue5PacketDeserializeStatus::UnsupportedProfile,
                    "the packet was not captured after the validated packet-handler boundary");
            }
            switch (input.protection) {
            case Ue5PacketProtection::Clear: break;
            case Ue5PacketProtection::Encrypted:
                return Failure(
                    Ue5PacketDeserializeStatus::Encrypted,
                    "encrypted packet bytes require a validated post-decryption boundary");
            case Ue5PacketProtection::Compressed:
                return Failure(
                    Ue5PacketDeserializeStatus::Compressed,
                    "compressed packet bytes require a validated post-decompression boundary");
            case Ue5PacketProtection::Unknown:
                return Failure(
                    Ue5PacketDeserializeStatus::UnsupportedProfile,
                    "the packet protection state is unknown");
            default:
                return Failure(
                    Ue5PacketDeserializeStatus::UnsupportedProfile,
                    "the packet protection state is invalid");
            }
            if (!IsExactByteSpan(input.bytes, input.bit_count)) {
                return Failure(
                    Ue5PacketDeserializeStatus::Malformed,
                    "packet byte length does not match its exact valid bit count");
            }
            if (input.bit_count > profile_.limits.maximum_packet_bits) {
                return Failure(
                    Ue5PacketDeserializeStatus::LimitExceeded,
                    "packet bit count exceeds the profile-defined limit");
            }
            if (input.capture_sequence == 0U) {
                return Failure(
                    Ue5PacketDeserializeStatus::Malformed,
                    "packet capture sequence must be nonzero");
            }
            const auto persisted_entry = connections_.find(input.connection_id);
            if (persisted_entry == connections_.end() &&
                connections_.size() >= profile_.limits.maximum_connections) {
                return Failure(
                    Ue5PacketDeserializeStatus::LimitExceeded,
                    "connection table exceeds the profile-defined limit");
            }
            const ConnectionState empty_connection;
            const ConnectionState& persisted_connection = persisted_entry == connections_.end()
                ? empty_connection
                : DirectionState(persisted_entry->second, input.direction);
            if (persisted_connection.has_capture_sequence &&
                input.capture_sequence <= persisted_connection.last_capture_sequence) {
                return Failure(
                    Ue5PacketDeserializeStatus::Malformed,
                    "packet capture sequence is not strictly increasing");
            }
            // Decode against a copy and publish state only when the entire
            // packet reaches a non-error terminal state. A malformed packet
            // must not suppress a later valid retransmission or reserve a
            // connection table entry.
            ConnectionState connection = persisted_connection;
            if (input.bit_count == 0U) {
                Ue5PacketDeserializeResult empty;
                empty.status = Ue5PacketDeserializeStatus::Empty;
                connection.has_capture_sequence = true;
                connection.last_capture_sequence = input.capture_sequence;
                PublishConnection(input, std::move(connection));
                return empty;
            }

            Ue5BitReader reader(input.bytes, input.bit_count);
            if (reader.HasError()) {
                return Failure(
                    Ue5PacketDeserializeStatus::Malformed,
                    std::string{"packet bit reader rejected the input: "} + ReaderErrorName(reader));
            }
            Ue5PacketDeserializeResult result;
            result.status = Ue5PacketDeserializeStatus::Decoded;
            if (!ReadHeader(reader, result.header, result.diagnostic)) {
                result.status = ReaderStatus(reader);
                result.bunches.clear();
                result.fields.clear();
                return result;
            }

            std::size_t bunch_count{};
            if (profile_.header.bunch_count_bits != 0U) {
                std::uint64_t encoded_count{};
                if (!reader.ReadUnsigned(profile_.header.bunch_count_bits, encoded_count)) {
                    result.status = ReaderStatus(reader);
                    result.diagnostic = std::string{"packet bunch count: "} + ReaderErrorName(reader);
                    return result;
                }
                if (encoded_count > profile_.limits.maximum_bunches_per_packet) {
                    result.status = Ue5PacketDeserializeStatus::LimitExceeded;
                    result.diagnostic = "packet bunch count exceeds the profile-defined limit";
                    return result;
                }
                bunch_count = static_cast<std::size_t>(encoded_count);
            }

            PruneExpiredPartials(connection, input.capture_sequence);
            if (HasPacketSequence(connection, result.header.sequence)) {
                result.status = Ue5PacketDeserializeStatus::Duplicate;
                result.diagnostic = "packet sequence was already delivered in this direction";
                connection.has_capture_sequence = true;
                connection.last_capture_sequence = input.capture_sequence;
                PublishConnection(input, std::move(connection));
                return result;
            }
            bool decoded_any{};
            bool duplicate_only{};
            bool pending_only{};
            const auto consume_bunch = [&](Ue5Bunch& bunch) -> bool {
                const auto status = ProcessBunch(
                    connection, bunch, input.capture_sequence, result, result.diagnostic);
                switch (status) {
                case Ue5PacketDeserializeStatus::Decoded:
                    decoded_any = true;
                    return true;
                case Ue5PacketDeserializeStatus::Duplicate:
                    duplicate_only = true;
                    return true;
                case Ue5PacketDeserializeStatus::ReassemblyPending:
                    pending_only = true;
                    return true;
                default:
                    result.status = status;
                    result.bunches.clear();
                    result.fields.clear();
                    return false;
                }
            };

            if (profile_.header.bunch_count_bits != 0U) {
                for (std::size_t index{}; index < bunch_count; ++index) {
                    Ue5Bunch bunch;
                    if (!ReadBunch(reader, bunch, result.diagnostic)) {
                        result.status = ReaderStatus(reader);
                        result.bunches.clear();
                        result.fields.clear();
                        return result;
                    }
                    if (!consume_bunch(bunch)) return result;
                }
                if (reader.BitsRemaining() != 0U) {
                    result.status = Ue5PacketDeserializeStatus::Malformed;
                    result.bunches.clear();
                    result.fields.clear();
                    result.diagnostic = "packet has trailing bits after its declared bunch count";
                    return result;
                }
            } else {
                while (reader.BitsRemaining() != 0U) {
                    if (bunch_count == profile_.limits.maximum_bunches_per_packet) {
                        result.status = Ue5PacketDeserializeStatus::LimitExceeded;
                        result.bunches.clear();
                        result.fields.clear();
                        result.diagnostic = "packet has more bunches than the profile-defined limit";
                        return result;
                    }
                    ++bunch_count;
                    Ue5Bunch bunch;
                    if (!ReadBunch(reader, bunch, result.diagnostic)) {
                        result.status = ReaderStatus(reader);
                        result.bunches.clear();
                        result.fields.clear();
                        return result;
                    }
                    if (!consume_bunch(bunch)) return result;
                }
            }

            if (decoded_any || bunch_count == 0U) {
                result.status = Ue5PacketDeserializeStatus::Decoded;
            } else if (pending_only) {
                result.status = Ue5PacketDeserializeStatus::ReassemblyPending;
            } else if (duplicate_only) {
                result.status = Ue5PacketDeserializeStatus::Duplicate;
            }
            RememberPacketSequence(connection, result.header.sequence);
            connection.has_capture_sequence = true;
            connection.last_capture_sequence = input.capture_sequence;
            PublishConnection(input, std::move(connection));
            return result;
        } catch (const std::bad_alloc&) {
            return Failure(
                Ue5PacketDeserializeStatus::ResourceExhausted,
                "the packet decoder could not allocate bounded state");
        } catch (...) {
            return Failure(
                Ue5PacketDeserializeStatus::Malformed,
                "the packet decoder rejected an unexpected internal failure");
        }
    }

    void Reset() noexcept { connections_.clear(); }

    void ResetConnection(const std::uint64_t connection_id) noexcept {
        connections_.erase(connection_id);
    }

    [[nodiscard]] const Ue5PacketProtocolProfile& Profile() const noexcept { return profile_; }

private:
    struct ChannelState final {
        bool open{};
        std::optional<std::uint32_t> channel_type;
        bool has_reliable_sequence{};
        std::uint32_t reliable_sequence{};
    };

    struct PartialBunch final {
        std::unordered_map<std::uint32_t, Ue5Bunch> fragments;
        bool has_initial{};
        bool has_final{};
        std::uint32_t initial_sequence{};
        std::uint32_t final_sequence{};
        std::uint64_t last_capture_sequence{};
        std::size_t stored_payload_bits{};
    };

    struct ConnectionState final {
        std::unordered_map<std::uint32_t, ChannelState> channels;
        std::unordered_map<std::uint64_t, PartialBunch> partials;
        std::size_t partial_state_bits{};
        std::deque<std::uint64_t> packet_history;
        std::unordered_set<std::uint64_t> packet_history_set;
        bool has_capture_sequence{};
        std::uint64_t last_capture_sequence{};
    };

    struct DirectionalConnectionState final {
        ConnectionState inbound;
        ConnectionState outbound;
    };

    [[nodiscard]] static ConnectionState& DirectionState(
        DirectionalConnectionState& directional,
        const Ue5PacketDirection direction) noexcept {
        return direction == Ue5PacketDirection::Inbound
            ? directional.inbound
            : directional.outbound;
    }

    [[nodiscard]] static const ConnectionState& DirectionState(
        const DirectionalConnectionState& directional,
        const Ue5PacketDirection direction) noexcept {
        return direction == Ue5PacketDirection::Inbound
            ? directional.inbound
            : directional.outbound;
    }

    void PublishConnection(const Ue5PacketInput& input, ConnectionState connection) {
        const auto existing = connections_.find(input.connection_id);
        if (existing != connections_.end()) {
            DirectionState(existing->second, input.direction) = std::move(connection);
            return;
        }

        DirectionalConnectionState directional;
        DirectionState(directional, input.direction) = std::move(connection);
        connections_.emplace(input.connection_id, std::move(directional));
    }

    [[nodiscard]] static bool HasPacketSequence(
        const ConnectionState& connection,
        const std::uint64_t sequence) noexcept {
        return connection.packet_history_set.contains(sequence);
    }

    void RememberPacketSequence(ConnectionState& connection, const std::uint64_t sequence) {
        if (!connection.packet_history_set.insert(sequence).second) return;
        connection.packet_history.push_back(sequence);
        while (connection.packet_history.size() > profile_.limits.maximum_packet_history) {
            connection.packet_history_set.erase(connection.packet_history.front());
            connection.packet_history.pop_front();
        }
    }

    static void ErasePartial(
        ConnectionState& connection,
        std::unordered_map<std::uint64_t, PartialBunch>::iterator iterator) noexcept {
        connection.partial_state_bits =
            connection.partial_state_bits >= iterator->second.stored_payload_bits
            ? connection.partial_state_bits - iterator->second.stored_payload_bits
            : 0U;
        connection.partials.erase(iterator);
    }

    void PruneExpiredPartials(
        ConnectionState& connection,
        const std::uint64_t capture_sequence) const noexcept {
        for (auto iterator = connection.partials.begin(); iterator != connection.partials.end();) {
            const auto& partial = iterator->second;
            if (capture_sequence > partial.last_capture_sequence &&
                capture_sequence - partial.last_capture_sequence >
                    profile_.limits.maximum_partial_packet_age) {
                auto expired = iterator++;
                ErasePartial(connection, expired);
            } else {
                ++iterator;
            }
        }
    }

    [[nodiscard]] bool ReadHeader(
        Ue5BitReader& reader,
        Ue5PacketHeader& header,
        std::string& diagnostic) const {
        if (!reader.ReadUnsigned(profile_.header.sequence_bits, header.sequence)) {
            diagnostic = std::string{"packet sequence: "} + ReaderErrorName(reader);
            return false;
        }
        switch (profile_.header.ack_mode) {
        case Ue5AckMode::None:
            header.has_ack = false;
            return true;
        case Ue5AckMode::Always:
            header.has_ack = true;
            break;
        case Ue5AckMode::PresenceBit:
            if (!reader.ReadBool(header.has_ack)) {
                diagnostic = std::string{"packet ACK presence: "} + ReaderErrorName(reader);
                return false;
            }
            break;
        default:
            diagnostic = "packet ACK mode is invalid";
            return false;
        }
        if (!header.has_ack) return true;
        if (!reader.ReadUnsigned(profile_.header.ack_sequence_bits, header.ack_sequence) ||
            !reader.ReadUnsigned(profile_.header.ack_history_bits, header.ack_history)) {
            diagnostic = std::string{"packet ACK fields: "} + ReaderErrorName(reader);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ReadBunch(
        Ue5BitReader& reader,
        Ue5Bunch& bunch,
        std::string& diagnostic) const {
        if (!reader.ReadSerializeInt(profile_.bunch.maximum_channels, bunch.channel_index) ||
            !reader.ReadBool(bunch.open) || !reader.ReadBool(bunch.close)) {
            diagnostic = std::string{"bunch channel flags: "} + ReaderErrorName(reader);
            return false;
        }
        if (bunch.close && profile_.bunch.close_has_dormancy_bit &&
            !reader.ReadBool(bunch.dormant)) {
            diagnostic = std::string{"bunch dormancy flag: "} + ReaderErrorName(reader);
            return false;
        }
        if (!reader.ReadBool(bunch.reliable)) {
            diagnostic = std::string{"bunch reliable flag: "} + ReaderErrorName(reader);
            return false;
        }
        if (bunch.reliable &&
            !reader.ReadSerializeInt(
                profile_.bunch.maximum_reliable_sequence, bunch.reliable_sequence)) {
            diagnostic = std::string{"bunch reliable sequence: "} + ReaderErrorName(reader);
            return false;
        }
        if ((bunch.open || bunch.close) &&
            !reader.ReadSerializeInt(profile_.bunch.maximum_channel_types, bunch.channel_type)) {
            diagnostic = std::string{"bunch channel type: "} + ReaderErrorName(reader);
            return false;
        }
        if (!reader.ReadBool(bunch.partial)) {
            diagnostic = std::string{"bunch partial flag: "} + ReaderErrorName(reader);
            return false;
        }
        if (bunch.partial) {
            if (!reader.ReadBool(bunch.partial_initial) || !reader.ReadBool(bunch.partial_final)) {
                diagnostic = std::string{"bunch partial flags: "} + ReaderErrorName(reader);
                return false;
            }
            switch (profile_.bunch.partial_id_mode) {
            case Ue5PartialIdMode::None:
                bunch.partial_id = 0U;
                break;
            case Ue5PartialIdMode::Explicit:
                if (!reader.ReadSerializeInt(
                        profile_.bunch.maximum_partial_id, bunch.partial_id)) {
                    diagnostic = std::string{"bunch partial identifier: "} + ReaderErrorName(reader);
                    return false;
                }
                break;
            case Ue5PartialIdMode::ReliableSequence:
                if (!bunch.reliable) {
                    diagnostic = "a reliable-sequence partial identifier was used by an unreliable bunch";
                    return false;
                }
                bunch.partial_id = bunch.reliable_sequence;
                break;
            default:
                diagnostic = "bunch partial identifier mode is invalid";
                return false;
            }
        }
        if (profile_.bunch.has_package_map_export_flags &&
            (!reader.ReadBool(bunch.has_package_map_exports) ||
             !reader.ReadBool(bunch.has_must_be_mapped_guids))) {
            diagnostic = std::string{"bunch package-map flags: "} + ReaderErrorName(reader);
            return false;
        }

        std::uint32_t payload_bits{};
        if (!reader.ReadSerializeInt(profile_.bunch.maximum_payload_bits + 1U, payload_bits)) {
            diagnostic = std::string{"bunch payload bit count: "} + ReaderErrorName(reader);
            return false;
        }
        bunch.payload_bit_count = payload_bits;
        if (bunch.payload_bit_count > profile_.bunch.maximum_payload_bits ||
            bunch.payload_bit_count > profile_.limits.maximum_packet_bits) {
            diagnostic = "bunch payload bit count exceeds the profile-defined limit";
            return false;
        }
        bunch.payload.assign(
            bunch.payload_bit_count / 8U +
                ((bunch.payload_bit_count % 8U) == 0U ? 0U : 1U),
            0U);
        for (std::size_t index{}; index < bunch.payload_bit_count; ++index) {
            bool bit{};
            if (!reader.ReadBit(bit)) {
                diagnostic = std::string{"bunch payload: "} + ReaderErrorName(reader);
                return false;
            }
            if (bit) {
                bunch.payload[index / 8U] |=
                    static_cast<std::uint8_t>(1U << (index % 8U));
            }
        }
        return true;
    }

    [[nodiscard]] Ue5PacketDeserializeStatus ReassemblePartialBunch(
        ConnectionState& connection,
        Ue5Bunch& bunch,
        const std::uint64_t capture_sequence,
        std::string& diagnostic) {
        if (profile_.bunch.partial_id_mode != Ue5PartialIdMode::Explicit) {
            diagnostic = "partial bunches require a profile-defined grouping identifier";
            return Ue5PacketDeserializeStatus::UnknownSchema;
        }
        if (!bunch.reliable) {
            diagnostic = "partial bunches require reliable sequence evidence";
            return Ue5PacketDeserializeStatus::Malformed;
        }
        const auto channel = connection.channels.find(bunch.channel_index);
        if (channel != connection.channels.end() && channel->second.has_reliable_sequence) {
            switch (CompareSequence(
                bunch.reliable_sequence, channel->second.reliable_sequence,
                profile_.bunch.maximum_reliable_sequence)) {
            case SequenceOrder::Newer:
                break;
            case SequenceOrder::Equal:
            case SequenceOrder::Older:
                diagnostic = "partial bunch sequence was already delivered or is outside the sequence window";
                return Ue5PacketDeserializeStatus::Duplicate;
            case SequenceOrder::Ambiguous:
            case SequenceOrder::Invalid:
                diagnostic = "partial bunch sequence is ambiguous in the profile-defined sequence space";
                return Ue5PacketDeserializeStatus::Malformed;
            }
        }

        const std::uint64_t key = (static_cast<std::uint64_t>(bunch.channel_index) << 32U) |
            static_cast<std::uint64_t>(bunch.partial_id);
        auto found = connection.partials.find(key);
        if (found == connection.partials.end()) {
            if (connection.partials.size() >= profile_.limits.maximum_open_partial_bunches) {
                diagnostic = "partial bunch table exceeds the profile-defined limit";
                return Ue5PacketDeserializeStatus::LimitExceeded;
            }
            found = connection.partials.emplace(key, PartialBunch{}).first;
        }
        auto& partial = found->second;
        if (partial.fragments.contains(bunch.reliable_sequence)) {
            diagnostic = "partial bunch fragment was already received";
            return Ue5PacketDeserializeStatus::Duplicate;
        }
        if (partial.fragments.size() >= profile_.limits.maximum_partial_fragments) {
            diagnostic = "partial bunch has too many fragments";
            return Ue5PacketDeserializeStatus::LimitExceeded;
        }
        if (partial.stored_payload_bits > profile_.limits.maximum_partial_bunch_bits ||
            connection.partial_state_bits > profile_.limits.maximum_partial_state_bits ||
            bunch.payload_bit_count > profile_.limits.maximum_partial_bunch_bits -
                partial.stored_payload_bits ||
            bunch.payload_bit_count > profile_.limits.maximum_partial_state_bits -
                connection.partial_state_bits) {
            diagnostic = "partial bunch storage exceeds the profile-defined limit";
            return Ue5PacketDeserializeStatus::LimitExceeded;
        }
        partial.fragments.emplace(bunch.reliable_sequence, bunch);
        partial.stored_payload_bits += bunch.payload_bit_count;
        connection.partial_state_bits += bunch.payload_bit_count;
        partial.last_capture_sequence = capture_sequence;
        if (bunch.partial_initial) {
            if (partial.has_initial && partial.initial_sequence != bunch.reliable_sequence) {
                diagnostic = "partial bunch has conflicting initial fragments";
                return Ue5PacketDeserializeStatus::Malformed;
            }
            partial.has_initial = true;
            partial.initial_sequence = bunch.reliable_sequence;
        }
        if (bunch.partial_final) {
            if (partial.has_final && partial.final_sequence != bunch.reliable_sequence) {
                diagnostic = "partial bunch has conflicting final fragments";
                return Ue5PacketDeserializeStatus::Malformed;
            }
            partial.has_final = true;
            partial.final_sequence = bunch.reliable_sequence;
        }
        if (!partial.has_initial || !partial.has_final) {
            diagnostic = "partial bunch is waiting for a contiguous fragment range";
            return Ue5PacketDeserializeStatus::ReassemblyPending;
        }

        const auto initial = partial.fragments.find(partial.initial_sequence);
        if (initial == partial.fragments.end()) {
            diagnostic = "partial bunch lost its initial fragment";
            return Ue5PacketDeserializeStatus::Malformed;
        }
        Ue5Bunch complete = initial->second;
        complete.partial = false;
        complete.partial_initial = false;
        complete.partial_final = false;
        complete.payload.clear();
        complete.payload_bit_count = 0U;
        std::uint32_t expected_sequence = partial.initial_sequence;
        bool reached_final{};
        for (std::size_t fragment_count{};
             fragment_count < profile_.limits.maximum_partial_fragments;
             ++fragment_count) {
            const auto fragment = partial.fragments.find(expected_sequence);
            if (fragment == partial.fragments.end()) {
                diagnostic = "partial bunch is waiting for a missing reliable sequence";
                return Ue5PacketDeserializeStatus::ReassemblyPending;
            }
            const auto& current = fragment->second;
            if (expected_sequence != partial.initial_sequence &&
                (current.open || current.close || current.channel_type != 0U)) {
                diagnostic = "partial bunch continuation changes channel lifecycle metadata";
                return Ue5PacketDeserializeStatus::Malformed;
            }
            if (!AppendBits(
                    complete.payload, complete.payload_bit_count, current.payload,
                    current.payload_bit_count, profile_.limits.maximum_partial_bunch_bits)) {
                diagnostic = "partial bunch exceeds the profile-defined reassembly limit";
                return Ue5PacketDeserializeStatus::LimitExceeded;
            }
            complete.has_package_map_exports =
                complete.has_package_map_exports || current.has_package_map_exports;
            complete.has_must_be_mapped_guids =
                complete.has_must_be_mapped_guids || current.has_must_be_mapped_guids;
            if (expected_sequence == partial.final_sequence) {
                reached_final = true;
                break;
            }
            expected_sequence = (expected_sequence + 1U) % profile_.bunch.maximum_reliable_sequence;
        }
        if (!reached_final) {
            diagnostic = "partial bunch exceeds the profile-defined fragment limit";
            return Ue5PacketDeserializeStatus::LimitExceeded;
        }
        complete.reliable_sequence = partial.final_sequence;
        bunch = std::move(complete);
        ErasePartial(connection, found);
        return Ue5PacketDeserializeStatus::Decoded;
    }

    [[nodiscard]] Ue5PacketDeserializeStatus ProcessBunch(
        ConnectionState& connection,
        Ue5Bunch& bunch,
        const std::uint64_t capture_sequence,
        Ue5PacketDeserializeResult& result,
        std::string& diagnostic) {
        if (bunch.partial) {
            const auto partial_status =
                ReassemblePartialBunch(connection, bunch, capture_sequence, diagnostic);
            if (partial_status != Ue5PacketDeserializeStatus::Decoded) return partial_status;
        }

        auto channel_iterator = connection.channels.find(bunch.channel_index);
        if (channel_iterator == connection.channels.end()) {
            if (!bunch.open) {
                diagnostic = "a bunch without an open flag has no established channel state";
                return Ue5PacketDeserializeStatus::UnknownSchema;
            }
            if (connection.channels.size() >= profile_.bunch.maximum_channels) {
                diagnostic = "channel table exceeds the profile-defined limit";
                return Ue5PacketDeserializeStatus::LimitExceeded;
            }
            channel_iterator = connection.channels.emplace(bunch.channel_index, ChannelState{}).first;
        }
        ChannelState& channel = channel_iterator->second;
        if (bunch.open && channel.open && channel.channel_type &&
            *channel.channel_type != bunch.channel_type) {
            diagnostic = "an open bunch changes an established channel type";
            return Ue5PacketDeserializeStatus::Malformed;
        }
        if (bunch.reliable && channel.has_reliable_sequence) {
            switch (CompareSequence(
                bunch.reliable_sequence, channel.reliable_sequence,
                profile_.bunch.maximum_reliable_sequence)) {
            case SequenceOrder::Newer:
                break;
            case SequenceOrder::Equal:
            case SequenceOrder::Older:
                diagnostic = "reliable bunch was already delivered or is outside the sequence window";
                return Ue5PacketDeserializeStatus::Duplicate;
            case SequenceOrder::Ambiguous:
            case SequenceOrder::Invalid:
                diagnostic = "reliable bunch sequence is ambiguous in the profile-defined sequence space";
                return Ue5PacketDeserializeStatus::Malformed;
            }
        }
        if (!(bunch.open || bunch.close) && channel.channel_type) {
            bunch.channel_type = *channel.channel_type;
        }

        const auto status = DecodeCompleteBunch(connection, bunch, result, diagnostic);
        if (status != Ue5PacketDeserializeStatus::Decoded) return status;

        if (bunch.open || bunch.close) channel.channel_type = bunch.channel_type;
        if (bunch.open) channel.open = true;
        if (bunch.reliable) {
            channel.has_reliable_sequence = true;
            channel.reliable_sequence = bunch.reliable_sequence;
        }
        if (bunch.close) {
            connection.channels.erase(channel_iterator);
            const std::uint64_t channel_prefix = static_cast<std::uint64_t>(bunch.channel_index) << 32U;
            for (auto iterator = connection.partials.begin(); iterator != connection.partials.end();) {
                if ((iterator->first & 0xffffffff00000000ULL) == channel_prefix) {
                    auto erased = iterator++;
                    ErasePartial(connection, erased);
                } else {
                    ++iterator;
                }
            }
        }
        return Ue5PacketDeserializeStatus::Decoded;
    }

    [[nodiscard]] Ue5PacketDeserializeStatus DecodeCompleteBunch(
        const ConnectionState& connection,
        const Ue5Bunch& bunch,
        Ue5PacketDeserializeResult& result,
        std::string& diagnostic) const {
        if (bunch.has_package_map_exports || bunch.has_must_be_mapped_guids) {
            diagnostic = "package-map export data requires a separately validated schema decoder";
            return Ue5PacketDeserializeStatus::UnknownSchema;
        }
        result.bunches.push_back(bunch);
        if (bunch.payload_bit_count == 0U) return Ue5PacketDeserializeStatus::Decoded;

        std::optional<std::uint32_t> channel_type;
        if (bunch.open || bunch.close) {
            channel_type = bunch.channel_type;
        } else if (const auto channel = connection.channels.find(bunch.channel_index);
                   channel != connection.channels.end()) {
            channel_type = channel->second.channel_type;
        }
        if (!channel_type) {
            diagnostic = "a payload bunch has no validated channel type";
            return Ue5PacketDeserializeStatus::UnknownSchema;
        }
        const auto schema = std::find_if(
            profile_.schemas.begin(), profile_.schemas.end(), [&](const Ue5ChannelSchema& candidate) {
                return candidate.channel_type == *channel_type;
            });
        if (schema == profile_.schemas.end()) {
            diagnostic = "the channel has no profile-defined field schema";
            return Ue5PacketDeserializeStatus::UnknownSchema;
        }
        return DecodeFields(*schema, bunch, result, diagnostic);
    }

    [[nodiscard]] Ue5PacketDeserializeStatus DecodeFields(
        const Ue5ChannelSchema& schema,
        const Ue5Bunch& bunch,
        Ue5PacketDeserializeResult& result,
        std::string& diagnostic) const {
        switch (schema.family) {
        case Ue5SchemaFamily::LegacyRepLayout:
        case Ue5SchemaFamily::Iris:
            break;
        default:
            diagnostic = "field stream has an invalid UE schema family";
            return Ue5PacketDeserializeStatus::UnknownSchema;
        }
        if (schema.encoding != Ue5FieldStreamEncoding::VerifiedTaggedFieldsV1) {
            diagnostic = "field stream has no verified decoder";
            return Ue5PacketDeserializeStatus::UnknownSchema;
        }
        Ue5BitReader reader(bunch.payload, bunch.payload_bit_count);
        if (reader.HasError()) {
            diagnostic = std::string{"field stream: "} + ReaderErrorName(reader);
            return Ue5PacketDeserializeStatus::Malformed;
        }
        std::uint32_t field_count{};
        const auto field_maximum = static_cast<std::uint32_t>(schema.fields.size() + 1U);
        if (!reader.ReadSerializeInt(field_maximum, field_count)) {
            diagnostic = std::string{"field count: "} + ReaderErrorName(reader);
            return ReaderStatus(reader);
        }
        if (field_count > schema.fields.size() ||
            field_count > profile_.limits.maximum_fields_per_bunch) {
            diagnostic = "field count exceeds the exact schema limit";
            return Ue5PacketDeserializeStatus::LimitExceeded;
        }

        std::uint32_t field_handle_maximum{};
        for (const auto& field : schema.fields) {
            field_handle_maximum = (std::max)(field_handle_maximum, field.handle);
        }
        ++field_handle_maximum;
        std::unordered_set<std::uint32_t> seen_handles;
        for (std::uint32_t index{}; index < field_count; ++index) {
            std::uint32_t handle{};
            if (!reader.ReadSerializeInt(field_handle_maximum, handle)) {
                diagnostic = std::string{"field handle: "} + ReaderErrorName(reader);
                return ReaderStatus(reader);
            }
            const auto definition = std::find_if(
                schema.fields.begin(), schema.fields.end(), [&](const Ue5FieldDefinition& candidate) {
                    return candidate.handle == handle;
                });
            if (definition == schema.fields.end() || !seen_handles.insert(handle).second) {
                diagnostic = "field stream references an unknown or repeated handle";
                return Ue5PacketDeserializeStatus::UnknownSchema;
            }

            Ue5DecodedField decoded;
            decoded.channel_index = bunch.channel_index;
            decoded.channel_type = bunch.channel_type;
            decoded.schema_id = schema.id;
            decoded.handle = definition->handle;
            decoded.name = definition->name;
            const auto status = DecodeFieldValue(reader, *definition, decoded.value, diagnostic);
            if (status != Ue5PacketDeserializeStatus::Decoded) return status;
            result.fields.push_back(std::move(decoded));
        }
        if (reader.BitsRemaining() != 0U) {
            diagnostic = "field stream has trailing bits outside its validated schema";
            return Ue5PacketDeserializeStatus::Malformed;
        }
        return Ue5PacketDeserializeStatus::Decoded;
    }

    [[nodiscard]] Ue5PacketDeserializeStatus DecodeFieldValue(
        Ue5BitReader& reader,
        const Ue5FieldDefinition& definition,
        Ue5FieldValue& value,
        std::string& diagnostic) const {
        const auto reader_failure = [&]() {
            diagnostic = std::string{"field "} + definition.name + ": " + ReaderErrorName(reader);
            return ReaderStatus(reader);
        };
        switch (definition.encoding) {
        case Ue5FieldEncoding::Boolean: {
            bool decoded{};
            if (!reader.ReadBool(decoded)) return reader_failure();
            value = decoded;
            return Ue5PacketDeserializeStatus::Decoded;
        }
        case Ue5FieldEncoding::Unsigned: {
            std::uint64_t decoded{};
            if (!reader.ReadUnsigned(definition.bit_width, decoded)) return reader_failure();
            value = decoded;
            return Ue5PacketDeserializeStatus::Decoded;
        }
        case Ue5FieldEncoding::Signed: {
            std::int64_t decoded{};
            if (!reader.ReadSigned(definition.bit_width, decoded)) return reader_failure();
            value = decoded;
            return Ue5PacketDeserializeStatus::Decoded;
        }
        case Ue5FieldEncoding::Float32: {
            std::uint64_t encoded{};
            if (!reader.ReadUnsigned(32U, encoded)) return reader_failure();
            value = std::bit_cast<float>(static_cast<std::uint32_t>(encoded));
            return Ue5PacketDeserializeStatus::Decoded;
        }
        case Ue5FieldEncoding::Utf8:
        case Ue5FieldEncoding::Bytes: {
            std::uint32_t length{};
            if (!reader.ReadSerializeInt(definition.maximum_length + 1U, length)) {
                return reader_failure();
            }
            if (length > profile_.limits.maximum_field_bytes) {
                diagnostic = std::string{"field "} + definition.name + " exceeds the field byte limit";
                return Ue5PacketDeserializeStatus::LimitExceeded;
            }
            std::vector<std::uint8_t> bytes(length);
            for (std::uint32_t index{}; index < length; ++index) {
                std::uint64_t byte{};
                if (!reader.ReadUnsigned(8U, byte)) return reader_failure();
                bytes[index] = static_cast<std::uint8_t>(byte);
            }
            if (definition.encoding == Ue5FieldEncoding::Bytes) {
                value = std::move(bytes);
                return Ue5PacketDeserializeStatus::Decoded;
            }
            std::string text(bytes.begin(), bytes.end());
            if (!IsValidUtf8(text)) {
                diagnostic = std::string{"field "} + definition.name + " is not valid UTF-8";
                return Ue5PacketDeserializeStatus::Malformed;
            }
            value = std::move(text);
            return Ue5PacketDeserializeStatus::Decoded;
        }
        case Ue5FieldEncoding::Vector3Float32: {
            Ue5Vector3f vector;
            std::uint64_t encoded{};
            if (!reader.ReadUnsigned(32U, encoded)) return reader_failure();
            vector.x = std::bit_cast<float>(static_cast<std::uint32_t>(encoded));
            if (!reader.ReadUnsigned(32U, encoded)) return reader_failure();
            vector.y = std::bit_cast<float>(static_cast<std::uint32_t>(encoded));
            if (!reader.ReadUnsigned(32U, encoded)) return reader_failure();
            vector.z = std::bit_cast<float>(static_cast<std::uint32_t>(encoded));
            value = vector;
            return Ue5PacketDeserializeStatus::Decoded;
        }
        }
        diagnostic = "field encoding is not supported by the exact schema";
        return Ue5PacketDeserializeStatus::UnknownSchema;
    }

    Ue5PacketProtocolProfile profile_;
    std::optional<std::string> validation_error_;
    std::unordered_map<std::uint64_t, DirectionalConnectionState> connections_;
};

Ue5NetworkDeserializer::Ue5NetworkDeserializer(Ue5PacketProtocolProfile profile)
    : impl_(std::make_unique<Impl>(std::move(profile))) {}

Ue5NetworkDeserializer::~Ue5NetworkDeserializer() = default;

Ue5PacketDeserializeResult Ue5NetworkDeserializer::Deserialize(const Ue5PacketInput& input) {
    return impl_->Deserialize(input);
}

void Ue5NetworkDeserializer::Reset() noexcept {
    if (impl_ != nullptr) impl_->Reset();
}

void Ue5NetworkDeserializer::ResetConnection(const std::uint64_t connection_id) noexcept {
    if (impl_ != nullptr) impl_->ResetConnection(connection_id);
}

const Ue5PacketProtocolProfile& Ue5NetworkDeserializer::Profile() const noexcept {
    return impl_->Profile();
}

}  // namespace anomaly
