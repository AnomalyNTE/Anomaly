#include "anomaly/udp_packet_parser.hpp"

#include <algorithm>
#include <limits>

namespace anomaly {
namespace {

constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
constexpr std::uint16_t kEtherTypeIpv6 = 0x86DD;
constexpr std::uint16_t kEtherTypeVlan = 0x8100;
constexpr std::uint16_t kEtherTypeProviderVlan = 0x88A8;
constexpr std::uint8_t kIpProtocolUdp = 17;
constexpr std::uint8_t kIpv6HopByHop = 0;
constexpr std::uint8_t kIpv6Routing = 43;
constexpr std::uint8_t kIpv6Fragment = 44;
constexpr std::uint8_t kIpv6Esp = 50;
constexpr std::uint8_t kIpv6Authentication = 51;
constexpr std::uint8_t kIpv6NoNextHeader = 59;
constexpr std::uint8_t kIpv6DestinationOptions = 60;

[[nodiscard]] bool HasBytes(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset,
    const std::size_t length) noexcept {
    return offset <= bytes.size() && length <= bytes.size() - offset;
}

[[nodiscard]] bool HasLogicalBytes(
    const std::size_t offset,
    const std::size_t length,
    const std::size_t end) noexcept {
    return offset <= end && length <= end - offset;
}

[[nodiscard]] bool AddSize(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result) noexcept {
    if (right > (std::numeric_limits<std::size_t>::max)() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] std::uint16_t ReadBe16(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] std::uint32_t ReadBe32(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] bool IsVlanEtherType(const std::uint16_t ether_type) noexcept {
    return ether_type == kEtherTypeVlan || ether_type == kEtherTypeProviderVlan;
}

void MarkMalformed(UdpPacketParseResult& result) noexcept {
    result.metadata.malformed = true;
}

void MarkTruncated(UdpPacketParseResult& result) noexcept {
    result.metadata.truncated = true;
}

void MarkUnsupported(UdpPacketParseResult& result) noexcept {
    result.metadata.unsupported = true;
}

[[nodiscard]] UdpPacketParseResult Finish(UdpPacketParseResult result) noexcept {
    const auto& metadata = result.metadata;
    if (metadata.malformed) {
        result.status = UdpPacketParseStatus::Malformed;
    } else if (metadata.truncated) {
        result.status = UdpPacketParseStatus::Truncated;
    } else if (metadata.unsupported) {
        result.status = UdpPacketParseStatus::Unsupported;
    } else if (metadata.fragmented) {
        result.status = UdpPacketParseStatus::Fragmented;
    } else if (metadata.udp_present) {
        result.status = UdpPacketParseStatus::Parsed;
    } else {
        result.status = UdpPacketParseStatus::NotUdp;
    }
    return result;
}

[[nodiscard]] bool ParseUdp(
    const std::span<const std::uint8_t> frame,
    const std::size_t udp_offset,
    const std::size_t ip_packet_end,
    UdpPacketParseResult& result) noexcept {
    auto& metadata = result.metadata;
    metadata.udp_header_offset = udp_offset;

    if (!HasLogicalBytes(udp_offset, 8, ip_packet_end)) {
        MarkMalformed(result);
        return false;
    }
    if (!HasBytes(frame, udp_offset, 8)) {
        MarkTruncated(result);
        return false;
    }

    metadata.udp_present = true;
    metadata.source_port = ReadBe16(frame, udp_offset);
    metadata.destination_port = ReadBe16(frame, udp_offset + 2);
    metadata.udp_length = ReadBe16(frame, udp_offset + 4);
    if (metadata.udp_length < 8) {
        MarkMalformed(result);
        return false;
    }

    metadata.payload_offset = udp_offset + 8;
    metadata.declared_payload_length = metadata.udp_length - 8;
    const std::size_t ip_payload_available = ip_packet_end - metadata.payload_offset;
    const std::size_t captured_payload_available = metadata.payload_offset < frame.size()
        ? frame.size() - metadata.payload_offset
        : 0;

    if (metadata.fragmented && metadata.more_fragments) {
        metadata.payload_length = (std::min)(
            metadata.declared_payload_length,
            (std::min)(ip_payload_available, captured_payload_available));
        return true;
    }

    if (metadata.udp_length > ip_packet_end - udp_offset) {
        MarkMalformed(result);
        return false;
    }

    std::size_t udp_end{};
    if (!AddSize(udp_offset, metadata.udp_length, udp_end)) {
        MarkMalformed(result);
        return false;
    }
    if (frame.size() < udp_end) MarkTruncated(result);
    const std::size_t captured_end = (std::min)(frame.size(), udp_end);
    metadata.payload_length = captured_end > metadata.payload_offset
        ? captured_end - metadata.payload_offset
        : 0;
    return true;
}

void CopyAddress(
    const std::span<const std::uint8_t> frame,
    const std::size_t source_offset,
    const std::size_t destination_offset,
    const std::size_t length,
    UdpPacketMetadata& metadata) noexcept {
    std::copy_n(frame.begin() + source_offset, length, metadata.source_address.begin());
    std::copy_n(frame.begin() + destination_offset, length, metadata.destination_address.begin());
    metadata.address_length = static_cast<std::uint8_t>(length);
}

void ParseIpv4(
    const std::span<const std::uint8_t> frame,
    const std::size_t ip_offset,
    UdpPacketParseResult& result) noexcept {
    auto& metadata = result.metadata;
    metadata.ip_version = UdpPacketIpVersion::Ipv4;
    metadata.ip_header_offset = ip_offset;
    if (!HasBytes(frame, ip_offset, 20)) {
        MarkTruncated(result);
        return;
    }

    const std::uint8_t first = frame[ip_offset];
    const std::size_t header_length = static_cast<std::size_t>(first & 0x0fU) * 4;
    if ((first >> 4U) != 4 || header_length < 20) {
        MarkMalformed(result);
        return;
    }
    if (!HasBytes(frame, ip_offset, header_length)) {
        MarkTruncated(result);
        return;
    }

    metadata.ip_header_length = header_length;
    metadata.ipv4_options_length = header_length - 20;
    metadata.traffic_class = frame[ip_offset + 1];
    metadata.ip_packet_length = ReadBe16(frame, ip_offset + 2);
    metadata.ipv4_identification = ReadBe16(frame, ip_offset + 4);
    metadata.hop_limit = frame[ip_offset + 8];
    metadata.transport_protocol = frame[ip_offset + 9];
    CopyAddress(frame, ip_offset + 12, ip_offset + 16, 4, metadata);

    if (metadata.ip_packet_length < header_length) {
        MarkMalformed(result);
        return;
    }
    std::size_t ip_packet_end{};
    if (!AddSize(ip_offset, metadata.ip_packet_length, ip_packet_end)) {
        MarkMalformed(result);
        return;
    }
    if (frame.size() < ip_packet_end) MarkTruncated(result);

    const std::uint16_t fragment_field = ReadBe16(frame, ip_offset + 6);
    if ((fragment_field & 0x8000U) != 0) {
        MarkMalformed(result);
        return;
    }
    metadata.dont_fragment = (fragment_field & 0x4000U) != 0;
    metadata.more_fragments = (fragment_field & 0x2000U) != 0;
    metadata.fragment_offset = static_cast<std::uint32_t>(fragment_field & 0x1fffU) * 8U;
    metadata.fragmented = metadata.more_fragments || metadata.fragment_offset != 0;

    const std::size_t transport_offset = ip_offset + header_length;
    if (metadata.fragment_offset != 0 || metadata.transport_protocol != kIpProtocolUdp) return;
    static_cast<void>(ParseUdp(frame, transport_offset, ip_packet_end, result));
}

[[nodiscard]] bool IsLengthEncodedIpv6Extension(
    const std::uint8_t next_header) noexcept {
    return next_header == kIpv6HopByHop || next_header == kIpv6Routing ||
        next_header == kIpv6DestinationOptions;
}

void ParseIpv6(
    const std::span<const std::uint8_t> frame,
    const std::size_t ip_offset,
    UdpPacketParseResult& result) noexcept {
    auto& metadata = result.metadata;
    metadata.ip_version = UdpPacketIpVersion::Ipv6;
    metadata.ip_header_offset = ip_offset;
    if (!HasBytes(frame, ip_offset, 40)) {
        MarkTruncated(result);
        return;
    }

    const std::uint8_t first = frame[ip_offset];
    if ((first >> 4U) != 6) {
        MarkMalformed(result);
        return;
    }

    metadata.traffic_class = static_cast<std::uint8_t>(
        ((first & 0x0fU) << 4U) | (frame[ip_offset + 1] >> 4U));
    const std::size_t payload_length = ReadBe16(frame, ip_offset + 4);
    if (!AddSize(40, payload_length, metadata.ip_packet_length)) {
        MarkMalformed(result);
        return;
    }
    metadata.hop_limit = frame[ip_offset + 7];
    CopyAddress(frame, ip_offset + 8, ip_offset + 24, 16, metadata);

    std::size_t ip_packet_end{};
    if (!AddSize(ip_offset, metadata.ip_packet_length, ip_packet_end)) {
        MarkMalformed(result);
        return;
    }
    if (frame.size() < ip_packet_end) MarkTruncated(result);

    std::uint8_t next_header = frame[ip_offset + 6];
    std::size_t cursor = ip_offset + 40;
    if (payload_length == 0) {
        metadata.ip_header_length = 40;
        metadata.transport_protocol = next_header;
        if (next_header != kIpv6NoNextHeader) {
            if (frame.size() > cursor && next_header == kIpv6HopByHop) {
                MarkUnsupported(result);
            } else {
                MarkMalformed(result);
            }
        }
        return;
    }

    while (true) {
        if (next_header == kIpProtocolUdp) {
            metadata.transport_protocol = next_header;
            metadata.ip_header_length = cursor - ip_offset;
            if (metadata.fragment_offset != 0) return;
            static_cast<void>(ParseUdp(frame, cursor, ip_packet_end, result));
            return;
        }
        if (next_header == kIpv6NoNextHeader) {
            metadata.transport_protocol = next_header;
            metadata.ip_header_length = cursor - ip_offset;
            return;
        }
        if (next_header == kIpv6Esp) {
            metadata.transport_protocol = next_header;
            metadata.ip_header_length = cursor - ip_offset;
            MarkUnsupported(result);
            return;
        }
        if (!IsLengthEncodedIpv6Extension(next_header) &&
            next_header != kIpv6Fragment && next_header != kIpv6Authentication) {
            metadata.transport_protocol = next_header;
            metadata.ip_header_length = cursor - ip_offset;
            return;
        }
        if (metadata.ipv6_extension_header_count >= kUdpPacketMaximumIpv6ExtensionHeaders) {
            metadata.transport_protocol = next_header;
            metadata.ip_header_length = cursor - ip_offset;
            MarkUnsupported(result);
            return;
        }
        if (!HasLogicalBytes(cursor, 2, ip_packet_end)) {
            MarkMalformed(result);
            return;
        }
        if (!HasBytes(frame, cursor, 2)) {
            MarkTruncated(result);
            return;
        }

        std::size_t extension_length{};
        if (next_header == kIpv6Fragment) {
            extension_length = 8;
        } else if (next_header == kIpv6Authentication) {
            extension_length = (static_cast<std::size_t>(frame[cursor + 1]) + 2) * 4;
            if (extension_length < 12) {
                MarkMalformed(result);
                return;
            }
        } else {
            extension_length = (static_cast<std::size_t>(frame[cursor + 1]) + 1) * 8;
        }
        if (!HasLogicalBytes(cursor, extension_length, ip_packet_end)) {
            MarkMalformed(result);
            return;
        }
        if (!HasBytes(frame, cursor, extension_length)) {
            MarkTruncated(result);
            return;
        }

        const std::uint8_t following_header = frame[cursor];
        ++metadata.ipv6_extension_header_count;
        if (next_header == kIpv6Fragment) {
            const std::uint16_t fragment_field = ReadBe16(frame, cursor + 2);
            if ((fragment_field & 0x0006U) != 0) {
                MarkMalformed(result);
                return;
            }
            metadata.fragment_offset =
                static_cast<std::uint32_t>((fragment_field & 0xfff8U) >> 3U) * 8U;
            metadata.more_fragments = (fragment_field & 0x0001U) != 0;
            metadata.fragmented = metadata.more_fragments || metadata.fragment_offset != 0;
            metadata.ipv6_fragment_identification = ReadBe32(frame, cursor + 4);
        }

        cursor += extension_length;
        next_header = following_header;
    }
}

}  // namespace

UdpPacketParseResult ParseEthernetUdpPacket(
    const std::span<const std::uint8_t> frame) noexcept {
    UdpPacketParseResult result;
    auto& metadata = result.metadata;
    metadata.captured_length = frame.size();
    if (!HasBytes(frame, 0, 14)) {
        MarkTruncated(result);
        return Finish(result);
    }

    std::copy_n(frame.begin(), metadata.destination_mac.size(), metadata.destination_mac.begin());
    std::copy_n(
        frame.begin() + metadata.destination_mac.size(), metadata.source_mac.size(),
        metadata.source_mac.begin());
    metadata.ether_type = ReadBe16(frame, 12);
    std::size_t network_offset = 14;

    while (IsVlanEtherType(metadata.ether_type)) {
        if (metadata.vlan_tag_count == metadata.vlan_tags.size()) {
            MarkUnsupported(result);
            return Finish(result);
        }
        if (!HasBytes(frame, network_offset, 4)) {
            MarkTruncated(result);
            return Finish(result);
        }

        auto& tag = metadata.vlan_tags[metadata.vlan_tag_count++];
        tag.type = metadata.ether_type;
        tag.control_information = ReadBe16(frame, network_offset);
        tag.priority = static_cast<std::uint8_t>((tag.control_information >> 13U) & 0x07U);
        tag.drop_eligible = (tag.control_information & 0x1000U) != 0;
        tag.identifier = static_cast<std::uint16_t>(tag.control_information & 0x0fffU);
        metadata.ether_type = ReadBe16(frame, network_offset + 2);
        network_offset += 4;
    }

    if (metadata.ether_type == kEtherTypeIpv4) {
        ParseIpv4(frame, network_offset, result);
    } else if (metadata.ether_type == kEtherTypeIpv6) {
        ParseIpv6(frame, network_offset, result);
    }
    return Finish(result);
}

}  // namespace anomaly
