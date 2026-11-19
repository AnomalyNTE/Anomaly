#pragma once

#include <anomaly/sdk/services/ipc.h>

#include <cstddef>
#include <cstdint>

namespace summon_lab::combat_roster {

// V2 reply: header followed by zero or more summon handles. All fields use
// little-endian uint64. The sequence bounds the applied damage events this roster covers.
inline constexpr char kEndpoint[] = "anomaly.summon-lab.combat-roster";
inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::size_t kMaximumReplyBytes = 64 * 1024;
static_assert(sizeof(AnomalyGenerationHandleV1) == 16);
struct ReplyHeader {
    AnomalyGenerationHandleV1 world;
    std::uint64_t latest_applied_sequence;
};
static_assert(sizeof(ReplyHeader) == 24);

// SHA-256 of "empty" and "world:handle(u64le id,u64le generation);latest_applied:u64le;actors:handle[]".
inline constexpr AnomalyIpcSchemaHashV1 kRequest{{0x2e, 0x1c, 0xfa, 0x82,
    0xb0, 0x35, 0xc2, 0x6c, 0xbb, 0xbd, 0xae, 0x63, 0x2c, 0xea, 0x07, 0x05,
    0x14, 0xeb, 0x8b, 0x77, 0x3f, 0x61, 0x6a, 0xae, 0xf6, 0x68, 0xe2, 0xf0,
    0xbe, 0x8f, 0x10, 0xd0}};
inline constexpr AnomalyIpcSchemaHashV1 kResponse{{0x2a, 0x54, 0x21, 0xcc,
    0xe3, 0xb2, 0x6e, 0xab, 0xa9, 0x02, 0xca, 0xc5, 0xf3, 0x42, 0xf5, 0xd2,
    0x73, 0xb1, 0x95, 0xd7, 0xca, 0x22, 0xd0, 0x45, 0x5f, 0xa3, 0x4e, 0xcc,
    0x0a, 0x34, 0xf4, 0x02}};

}  // namespace summon_lab::combat_roster
