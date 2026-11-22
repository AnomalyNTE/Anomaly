#pragma once
#include <anomaly/sdk/base.h>
#include <array>
#include <cstdint>
#include <string>

namespace nte_vehicle {
inline constexpr std::uint32_t kSnapshotValid = 1u << 0u;
inline constexpr std::uint32_t kHasSpeed = 1u << 1u;
inline constexpr std::uint32_t kHasTopSpeedRatio = 1u << 2u;
inline constexpr std::uint32_t kHasWheelFriction = 1u << 3u;
inline constexpr std::uint32_t kHasSummon = 1u << 4u;
inline constexpr std::uint32_t kHasEngineTorqueRatio = 1u << 5u;
inline constexpr std::uint32_t kCatalogValid = 1u << 0u;
inline constexpr std::uint32_t kVehicleIdMaxBytes = 128u;
inline constexpr std::uint32_t kSummonHasPosition = 1u << 0u;
inline constexpr std::uint32_t kSummonSetOwnerToPlayer = 1u << 1u;
inline constexpr std::uint32_t kSummonCustomConfiguration = 1u << 3u;
inline constexpr std::uint32_t kCustomizationModule = 1u;
inline constexpr std::uint32_t kCustomizationSkin = 2u;
inline constexpr std::uint32_t kOptionValid = 1u << 0u;
inline constexpr std::uint32_t kOptionDefault = 1u << 1u;

enum class SpeedScope { Current, OtherLoaded };

struct StateSnapshot {
    std::uint32_t flags{};
    double speed_kmh{};
    float top_speed_ratio{1.0F};
    bool wheel_friction_enabled{true};
    float engine_torque_ratio{1.0F};
    std::uint64_t vehicle_entity_id{};
    std::string vehicle_class_name;
};

struct CatalogSnapshot {
    std::uint32_t flags{};
    std::uint64_t sequence{};
    std::uint32_t entry_count{};
};

struct ModuleSelection {
    std::uint32_t module_type{};
    AnomalyStringViewV1 module_id{};
};

struct SummonRequest {
    std::uint32_t flags{};
    std::array<double, 3> world_position{};
    AnomalyStringViewV1 skin_id{};
    const ModuleSelection* module_selections{};
    std::uint32_t module_selection_count{};
};

struct SpeedTarget {
    SpeedScope scope{SpeedScope::Current};
    float top_speed_kmh{};
    float minimum_speed_kmh{};
};

struct CustomizationEntry {
    std::uint32_t flags{};
    std::uint32_t kind{};
    std::uint32_t module_type{};
    std::string option_id;
    std::string label;
};
} // namespace nte_vehicle
