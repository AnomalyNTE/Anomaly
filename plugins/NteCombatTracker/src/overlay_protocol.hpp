#pragma once

#include "account_identity.hpp"
#include "combat_stats_engine.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nte::combat_stats {

inline constexpr std::string_view kOverlayChannel =
    "shikari.nte-combat-tracker.overlay";
inline constexpr std::uint32_t kOverlaySchemaVersion = 2;
inline constexpr std::uint32_t kMetricVersion = 1;
inline constexpr std::size_t kMaximumOverlayMessageBytes = 64U * 1024U;

struct NteOverlaySnapshot {
    struct Data {
        std::string producer_instance_id;
        std::uint64_t snapshot_sequence{};
        std::uint64_t latest_event_sequence{};
        std::int64_t sent_utc_ms{};
        bool producer_stopping{};
        bool capture_available{};
        std::string capture_status;
        std::string healing_capture_status{"unavailable"};
        std::string control_status;
        account::AccountIdentity account;
        std::string report_id;
        std::string encounter_id;
        std::string primary_target;
        std::uint16_t websocket_port{};
        std::uint32_t connected_clients{};
        std::uint64_t published_messages{};
        std::uint64_t transport_dropped_messages{};
        // Active Host locale, so the overlay process can load the same catalog
        // the plugin resolved. The overlay is a separate executable and has no
        // Host services of its own.
        std::string locale{"zh-CN"};
        Snapshot combat;
    };

    explicit NteOverlaySnapshot(Data data = {});

    const std::string producer_instance_id;
    const std::uint64_t snapshot_sequence{};
    const std::uint64_t latest_event_sequence{};
    const std::int64_t sent_utc_ms{};
    const bool producer_stopping{};
    const bool capture_available{};
    const std::string capture_status;
    const std::string healing_capture_status;
    const std::string control_status;
    const account::AccountIdentity account;
    const std::string report_id;
    const std::string encounter_id;
    const std::string primary_target;
    const std::uint16_t websocket_port{};
    const std::uint32_t connected_clients{};
    const std::uint64_t published_messages{};
    const std::uint64_t transport_dropped_messages{};
    const std::string locale;
    const Snapshot combat;
};

[[nodiscard]] std::string SerializeOverlaySnapshot(const NteOverlaySnapshot& snapshot);
[[nodiscard]] std::optional<NteOverlaySnapshot> ParseOverlaySnapshot(
    std::string_view json,
    std::string& error);

}  // namespace nte::combat_stats
