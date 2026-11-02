#pragma once

#include <anomaly/sdk/services/nte.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace nailong {

// player_position uses UE centimetres, like AnomalyNteEntitySnapshotV1::bounds_center.
// The radius is inclusive and is measured in three dimensions.
[[nodiscard]] inline bool IsNpcInRadius(
    const AnomalyNteEntitySnapshotV1& snapshot,
    std::uint64_t player_entity_id,
    std::span<const double, 3> player_position,
    double npc_radius_m) noexcept {
    if ((snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) return false;
    if (snapshot.entity_id == 0 ||
        snapshot.entity_id > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    if ((snapshot.flags & ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER) != 0 ||
        snapshot.entity_id == player_entity_id) {
        return false;
    }
    if (!(npc_radius_m > 0.0) || !std::isfinite(npc_radius_m)) return false;

    double delta_m[3]{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double center_cm = snapshot.bounds_center[axis];
        const double player_cm = player_position[axis];
        if (!std::isfinite(center_cm) || !std::isfinite(player_cm)) return false;
        delta_m[axis] = (center_cm - player_cm) * 0.01;
    }

    // std::hypot avoids the overflow/underflow caused by manually squaring deltas.
    const double distance_m = std::hypot(delta_m[0], delta_m[1], delta_m[2]);
    return std::isfinite(distance_m) && distance_m <= npc_radius_m;
}

// The actor page lists every loaded actor in range: props, triggers, effects and
// spawners included. None of those can carry a replaceable character mesh, and
// probing them for one burned the whole NPC attempt (runtime log:
// "actors=256 range=2 slots=2 meshes=0 skeletal=0 candidates=0"). A candidate is
// therefore admitted by its class before any component work happens.
//
// Class names below are taken from the Dumper-7 SDK dump
// (C:\Dumper-7\5.6.1-0+UE5-HT1.4b, build 5.6.1-0+UE5-HT1.4b), not guessed. The real
// hierarchy is:
//     ACharacter -> AModularCharacter -> AHottaCharacter -> AHTCharacter
//                -> AHTAbilityCharacter -> AHTAICharacter
//                -> { AHTMonsterCharacter, AHTNPCCharacter, AHTPlayerNPCCharacter }
// AHTCharacterActor is intentionally NOT part of it: it derives straight from
// AActor and owns its SkeletalMeshComponent at 0x02C8, so component enumeration
// on it finds nothing.
[[nodiscard]] inline bool IsCharacterClassName(std::string_view name) noexcept {
    if (name.empty()) return false;
    constexpr std::string_view kNeedles[] = {
        "character", "pawn", "npc", "human", "avatar", "monster", "enemy", "mannequin",
        // HT-specific bases from the SDK dump
        "hottacharacter", "htai", "htnpc", "htmonster", "htplayernpc", "modularcharacter"};
    for (const auto needle : kNeedles) {
        if (needle.size() > name.size()) continue;
        for (std::size_t offset = 0; offset + needle.size() <= name.size(); ++offset) {
            bool match = true;
            for (std::size_t i = 0; i < needle.size(); ++i) {
                auto c = name[offset + i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != needle[i]) { match = false; break; }
            }
            if (match) return true;
        }
    }
    return false;
}

// The AActor-derived branch (AHTCharacterActor) passes any text gate, including the
// name fallback, yet only carries a mesh when its own component field is live. A
// text-only admission of that branch produces exactly the "character admitted, no
// mesh to hide" failures seen in the runtime log, so the fallback must exclude it.
[[nodiscard]] inline bool IsActorOwnedMeshBranch(std::string_view name) noexcept {
    constexpr std::string_view kNeedles[] = {
        "characteractor", "charactor", "floatactor", "ftcharacter"};
    for (const auto needle : kNeedles) {
        if (needle.size() > name.size()) continue;
        for (std::size_t offset = 0; offset + needle.size() <= name.size(); ++offset) {
            bool match = true;
            for (std::size_t i = 0; i < needle.size(); ++i) {
                auto c = name[offset + i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != needle[i]) { match = false; break; }
            }
            if (match) return true;
        }
    }
    return false;
}

// A pending candidate is dropped once its grace window closes. NaN or an
// already-past deadline counts as expired, so an unresolved clock never leaks
// entries forever.
[[nodiscard]] inline bool NpcPendingExpired(double now, double deadline) noexcept {
    return !(deadline > now);
}

} // namespace nailong
