#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace anomaly {

inline constexpr std::size_t kUdpPacketMaximumVlanTags = 4;
inline constexpr std::size_t kUdpPacketMaximumIpv6ExtensionHeaders = 8;

enum class UdpPacketParseStatus : std::uint8_t {
    Parsed,
    NotUdp,
    Fragmented,
    Unsupported,
    Malformed,
    Truncated,
};

enum class UdpPacketIpVersion : std::uint8_t {
    None,
    Ipv4,
    Ipv6,
};

struct UdpPacketVlanTag {
    std::uint16_t type{};
    std::uint16_t control_information{};
    std::uint16_t identifier{};
    std::uint8_t priority{};
    bool drop_eligible{};
};

// All offsets and lengths are relative to the supplied Ethernet frame. A
// payload_length describes captured bytes; declared_payload_length is the UDP
// datagram payload length advertised on the wire.
struct UdpPacketMetadata {
    std::size_t captured_length{};

    std::array<std::uint8_t, 6> destination_mac{};
    std::array<std::uint8_t, 6> source_mac{};
    std::uint16_t ether_type{};
    std::array<UdpPacketVlanTag, kUdpPacketMaximumVlanTags> vlan_tags{};
    std::size_t vlan_tag_count{};

    UdpPacketIpVersion ip_version{UdpPacketIpVersion::None};
    std::array<std::uint8_t, 16> source_address{};
    std::array<std::uint8_t, 16> destination_address{};
    std::uint8_t address_length{};
    std::uint8_t traffic_class{};
    std::uint8_t hop_limit{};
    std::uint8_t transport_protocol{};
    std::size_t ip_header_offset{};
    std::size_t ip_header_length{};
    std::size_t ip_packet_length{};
    std::size_t ipv4_options_length{};
    std::size_t ipv6_extension_header_count{};

    std::uint16_t ipv4_identification{};
    std::uint32_t ipv6_fragment_identification{};
    std::uint32_t fragment_offset{};
    bool dont_fragment{};
    bool more_fragments{};
    bool fragmented{};

    bool udp_present{};
    std::uint16_t source_port{};
    std::uint16_t destination_port{};
    std::uint16_t udp_length{};
    std::size_t udp_header_offset{};
    std::size_t payload_offset{};
    std::size_t payload_length{};
    std::size_t declared_payload_length{};

    bool malformed{};
    bool truncated{};
    bool unsupported{};
};

struct UdpPacketParseResult {
    UdpPacketParseStatus status{UdpPacketParseStatus::NotUdp};
    UdpPacketMetadata metadata;

    [[nodiscard]] bool Parsed() const noexcept {
        return status == UdpPacketParseStatus::Parsed;
    }
};

// Parses an Ethernet II frame containing zero or more 802.1Q/802.1ad VLAN
// headers and, when present, an IPv4 or IPv6 UDP datagram. This function is
// offline-only and performs no packet capture, allocation, or I/O.
[[nodiscard]] UdpPacketParseResult ParseEthernetUdpPacket(
    std::span<const std::uint8_t> frame) noexcept;

}  // namespace anomaly
