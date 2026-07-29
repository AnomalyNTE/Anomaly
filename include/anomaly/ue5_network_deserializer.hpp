#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace anomaly {

// This describes a recorder boundary, not a packet inferred from a UDP length.
// Only a verified post-handler boundary can supply the exact valid bit count.
enum class Ue5PacketCaptureStage : std::uint8_t {
    Unknown,
    PrePacketHandler,
    PostPacketHandler,
};

enum class Ue5PacketDirection : std::uint8_t {
    Unknown,
    Inbound,
    Outbound,
};

enum class Ue5PacketProtection : std::uint8_t {
    Clear,
    Compressed,
    Encrypted,
    Unknown,
};

enum class Ue5AckMode : std::uint8_t {
    None,
    Always,
    PresenceBit,
};

enum class Ue5PartialIdMode : std::uint8_t {
    // The wire layout contains no grouping identifier. A partial bunch is
    // therefore rejected until a validated profile supplies a grouping codec.
    None,
    Explicit,
    // Reserved for a separately verified codec. A reliable sequence by itself
    // cannot distinguish all fragments in a multi-fragment bunch.
    ReliableSequence,
};

enum class Ue5SchemaFamily : std::uint8_t {
    LegacyRepLayout,
    Iris,
};

// The format of the field stream at the verified capture boundary. This is
// deliberately separate from the UE schema family: raw RepLayout and Iris
// streams require distinct, evidence-backed codecs and are never inferred.
enum class Ue5FieldStreamEncoding : std::uint8_t {
    Unknown,
    VerifiedTaggedFieldsV1,
};

enum class Ue5FieldEncoding : std::uint8_t {
    Boolean,
    Unsigned,
    Signed,
    Float32,
    Utf8,
    Bytes,
    Vector3Float32,
};

enum class Ue5PacketDeserializeStatus : std::uint8_t {
    Decoded,
    Empty,
    Duplicate,
    ReassemblyPending,
    UnsupportedProfile,
    Encrypted,
    Compressed,
    Truncated,
    Malformed,
    UnknownSchema,
    LimitExceeded,
    ResourceExhausted,
};

struct Ue5PacketLimits final {
    std::size_t maximum_packet_bits{65535U * 8U};
    std::size_t maximum_connections{64};
    std::size_t maximum_packet_history{128};
    std::size_t maximum_bunches_per_packet{1024};
    std::size_t maximum_open_partial_bunches{64};
    std::size_t maximum_partial_fragments{64};
    std::size_t maximum_partial_packet_age{256};
    std::size_t maximum_partial_bunch_bits{65535U * 8U};
    std::size_t maximum_partial_state_bits{65535U * 8U * 16U};
    std::size_t maximum_fields_per_bunch{256};
    std::size_t maximum_field_bytes{4096};
};

struct Ue5PacketHeaderLayout final {
    std::uint8_t sequence_bits{};
    Ue5AckMode ack_mode{Ue5AckMode::None};
    std::uint8_t ack_sequence_bits{};
    std::uint8_t ack_history_bits{};
    // Zero consumes bunches until the exact packet bit count. A nonzero value
    // serializes the number of bunches as an unsigned fixed-width integer.
    std::uint8_t bunch_count_bits{};
};

struct Ue5BunchLayout final {
    std::uint32_t maximum_channels{};
    std::uint32_t maximum_channel_types{};
    std::uint32_t maximum_reliable_sequence{};
    std::uint32_t maximum_partial_id{};
    std::uint32_t maximum_payload_bits{};
    bool close_has_dormancy_bit{};
    bool has_package_map_export_flags{};
    Ue5PartialIdMode partial_id_mode{Ue5PartialIdMode::None};
};

struct Ue5FieldDefinition final {
    std::uint32_t handle{};
    std::string name;
    Ue5FieldEncoding encoding{Ue5FieldEncoding::Boolean};
    // Used by Unsigned and Signed encodings. Float and vector fields always
    // consume their IEEE-754 width.
    std::uint8_t bit_width{};
    // Used by Utf8 and Bytes. The limit is inclusive.
    std::uint32_t maximum_length{};
};

struct Ue5ChannelSchema final {
    std::uint32_t channel_type{};
    std::string id;
    Ue5SchemaFamily family{Ue5SchemaFamily::LegacyRepLayout};
    Ue5FieldStreamEncoding encoding{Ue5FieldStreamEncoding::Unknown};
    // VerifiedTaggedFieldsV1 consists of a SerializeInt field count followed
    // by handle/value pairs. It may only be selected when the exact capture
    // boundary is proven to produce this normalized format.
    std::vector<Ue5FieldDefinition> fields;
};

// A profile is supplied only after the recorder ABI, packet handler stage,
// packet layout, and schema have all been independently verified. No bundled
// profile enables this API today.
struct Ue5PacketProtocolProfile final {
    std::string profile_hash;
    bool post_handler_boundary_validated{};
    Ue5PacketHeaderLayout header;
    Ue5BunchLayout bunch;
    Ue5PacketLimits limits;
    std::vector<Ue5ChannelSchema> schemas;
};

struct Ue5PacketInput final {
    std::uint64_t connection_id{};
    Ue5PacketDirection direction{Ue5PacketDirection::Unknown};
    // Strictly increasing per connection and direction. The recorder assigns
    // this before a packet enters the Worker-domain deserializer.
    std::uint64_t capture_sequence{};
    std::string_view profile_hash;
    Ue5PacketCaptureStage stage{Ue5PacketCaptureStage::Unknown};
    Ue5PacketProtection protection{Ue5PacketProtection::Unknown};
    std::span<const std::uint8_t> bytes;
    std::size_t bit_count{};
};

struct Ue5PacketHeader final {
    std::uint64_t sequence{};
    bool has_ack{};
    std::uint64_t ack_sequence{};
    std::uint64_t ack_history{};
};

struct Ue5Bunch final {
    std::uint32_t channel_index{};
    std::uint32_t channel_type{};
    bool open{};
    bool close{};
    bool dormant{};
    bool reliable{};
    std::uint32_t reliable_sequence{};
    bool partial{};
    bool partial_initial{};
    bool partial_final{};
    std::uint32_t partial_id{};
    bool has_package_map_exports{};
    bool has_must_be_mapped_guids{};
    std::size_t payload_bit_count{};
    std::vector<std::uint8_t> payload;
};

struct Ue5Vector3f final {
    float x{};
    float y{};
    float z{};
};

using Ue5FieldValue = std::variant<
    bool,
    std::uint64_t,
    std::int64_t,
    float,
    std::string,
    std::vector<std::uint8_t>,
    Ue5Vector3f>;

struct Ue5DecodedField final {
    std::uint32_t channel_index{};
    std::uint32_t channel_type{};
    std::string schema_id;
    std::uint32_t handle{};
    std::string name;
    Ue5FieldValue value{false};
};

struct Ue5PacketDeserializeResult final {
    Ue5PacketDeserializeStatus status{Ue5PacketDeserializeStatus::UnsupportedProfile};
    Ue5PacketHeader header;
    std::vector<Ue5Bunch> bunches;
    std::vector<Ue5DecodedField> fields;
    std::string diagnostic;

    [[nodiscard]] bool Decoded() const noexcept {
        return status == Ue5PacketDeserializeStatus::Decoded;
    }
};

// Validates profile-defined limits and codec declarations without consuming a
// packet. Callers use this before exposing a profile to a Worker decoder.
[[nodiscard]] std::optional<std::string> ValidateUe5PacketProtocolProfile(
    const Ue5PacketProtocolProfile& profile);

class Ue5NetworkDeserializer final {
public:
    explicit Ue5NetworkDeserializer(Ue5PacketProtocolProfile profile);
    ~Ue5NetworkDeserializer();

    Ue5NetworkDeserializer(const Ue5NetworkDeserializer&) = delete;
    Ue5NetworkDeserializer& operator=(const Ue5NetworkDeserializer&) = delete;
    Ue5NetworkDeserializer(Ue5NetworkDeserializer&&) = delete;
    Ue5NetworkDeserializer& operator=(Ue5NetworkDeserializer&&) = delete;

    // Instances have one Worker-domain owner. Callers must serialize access so
    // reliable and partial-bunch state is observed in capture order.
    [[nodiscard]] Ue5PacketDeserializeResult Deserialize(const Ue5PacketInput& input);
    void Reset() noexcept;
    void ResetConnection(std::uint64_t connection_id) noexcept;

    [[nodiscard]] const Ue5PacketProtocolProfile& Profile() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
