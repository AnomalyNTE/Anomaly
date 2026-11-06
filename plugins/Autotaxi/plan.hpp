#pragma once
#include <array>
#include <cstdint>

namespace taxi {
// ETaxiGameplayState, as exposed by the native gameplay API.
enum class Stage : uint8_t { Idle, Waiting, Pickup, Destination, SpecialEvent, Settle };
enum class Action { None, Pickup, Deliver, Continue };
using Identity = std::array<uint64_t, 8>; // world, player, controller, vehicle: id + generation
using Order = std::array<uint64_t, 3>;    // order template, pickup point, destination point
struct Observation {
    Identity identity{};
    Order order{};
    Stage stage{Stage::Idle};
};

struct Run {
    Identity identity{};
    Order order{};
    Stage stage{Stage::Idle};
    bool bound{}, submitted{}, failed{};
    uint64_t submitted_at{};

    void Reset() { *this = {}; }
    Action Next(const Observation& value, uint64_t now) {
        if (!bound || identity != value.identity) {
            Reset(); bound = true; identity = value.identity;
        }
        // A template may recur. Waiting/Idle is the native boundary between orders.
        if (value.stage == Stage::Idle || value.stage == Stage::Waiting) {
            order = {}; stage = value.stage; submitted = failed = false;
            return Action::None;
        }
        if (!value.order[0] || !value.order[1] || !value.order[2] ||
            static_cast<uint8_t>(value.stage) > static_cast<uint8_t>(Stage::Settle))
            return Action::None;
        if (order != value.order) {
            order = value.order; stage = value.stage; submitted = failed = false;
        } else if (stage != value.stage) {
            // A completed TalkSelf event can return to the delivery leg. Other
            // regressions without an order boundary are not a new request.
            const bool event_finished=stage==Stage::SpecialEvent && value.stage==Stage::Destination;
            if (static_cast<uint8_t>(value.stage) < static_cast<uint8_t>(stage) && !event_finished) failed = true;
            stage = value.stage; submitted = false;
        }
        if (submitted && now >= submitted_at && now - submitted_at >= 15000) failed = true;
        if (failed || submitted) return Action::None;
        switch (stage) {
        case Stage::Pickup: return Action::Pickup;
        case Stage::Destination: return Action::Deliver;
        case Stage::Settle: return Action::Continue;
        default: return Action::None; // Special events/dialogue remain owned by the game.
        }
    }
    void Submitted(bool success, uint64_t now) {
        // Dispatch success only means the native call ran, not that the order was accepted.
        submitted = true; submitted_at = now; failed = !success;
    }
    void Suspend(uint64_t now) { if(submitted) submitted_at=now; }
};

struct ConfirmedOrders {
    std::array<uint64_t, 2> player{};
    int32_t high_water{};
    uint64_t added{};
    bool initialized{};
    void Reset() { *this = {}; }
    void Observe(std::array<uint64_t, 2> identity, int32_t finished) {
        if (finished < 0) return;
        if (!initialized || player != identity) {
            player = identity; high_water = finished; initialized = true;
            return;
        }
        // Only a native completed-order count increase is displayed as confirmation.
        if (finished > high_water) {
            added += static_cast<uint32_t>(finished - high_water); high_water = finished;
        }
    }
};
}
