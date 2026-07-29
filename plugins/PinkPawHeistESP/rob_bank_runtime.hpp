#pragma once

#include "anomaly/sdk/anomaly_sdk.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace pink_paw_heist_esp {

inline constexpr std::string_view kWorldMarkerClassName = "Bank_Door_key_07_C";

enum class PinkPawWorldState {
    unavailable,
    outside,
    active,
};

class PinkPawWorldGate final {
public:
    PinkPawWorldGate();
    ~PinkPawWorldGate();

    PinkPawWorldGate(const PinkPawWorldGate&) = delete;
    PinkPawWorldGate& operator=(const PinkPawWorldGate&) = delete;

    // Uses one exact class FName as the per-World marker. A completed negative
    // probe is retained until the World changes or Invalidate is requested.
    [[nodiscard]] PinkPawWorldState Refresh(
        const AnomalyHostApiV1* host) noexcept;
    void Invalidate() noexcept;
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct RobBankEntity final {
    std::uint32_t object_index{};
    std::uint32_t object_serial{};

    [[nodiscard]] bool Valid() const noexcept {
        return object_serial != 0;
    }
};

enum class RobBankPickability {
    unavailable,
    blocked,
    candidate,
};

struct RobBankInspection final {
    RobBankEntity entity;
    RobBankPickability pickability{RobBankPickability::unavailable};
};

class RobBankRuntime final {
public:
    RobBankRuntime();
    ~RobBankRuntime();

    RobBankRuntime(const RobBankRuntime&) = delete;
    RobBankRuntime& operator=(const RobBankRuntime&) = delete;

    // Resolves plugin-owned signatures. Failure only disables RobBank-specific
    // actions; the generic ESP, actor, player, and teleport services stay usable.
    [[nodiscard]] bool Start(const AnomalyHostApiV1* host) noexcept;
    void Stop() noexcept;

    // Game-thread refresh of the object registry, local controller, and
    // RobBank point-table state used by Inspect and Pickup.
    [[nodiscard]] bool Refresh() noexcept;

    [[nodiscard]] RobBankInspection Inspect(
        std::uint64_t entity_id,
        std::string_view expected_class_name) noexcept;

    [[nodiscard]] AnomalyStatusV1 Pickup(RobBankEntity entity) noexcept;
    [[nodiscard]] bool Available() const noexcept;
    [[nodiscard]] bool CanInspect() const noexcept;
    [[nodiscard]] bool DiscoveryPending() const noexcept;
    [[nodiscard]] bool PickabilityReady() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pink_paw_heist_esp
