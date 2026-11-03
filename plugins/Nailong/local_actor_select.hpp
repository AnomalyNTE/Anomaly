#pragma once
#include <anomaly/sdk/services/nte.h>
#include <cstdint>

namespace nailong {
inline bool ValidHandle(const AnomalyGenerationHandleV1& handle) {
    return handle.id != 0 && handle.generation != 0;
}
inline bool SameHandle(const AnomalyGenerationHandleV1& left, const AnomalyGenerationHandleV1& right) {
    return ValidHandle(left) && ValidHandle(right) &&
           left.id == right.id && left.generation == right.generation;
}
inline bool UsableActorSnapshot(const AnomalyNteEntitySnapshotV1& snapshot) {
    // Actor frames are deliberately cached by the Host; STALE means the cache
    // predates this tick, not that its LOCAL_PLAYER identity is invalid. The
    // separate player-generation check gates pawn switches.
    return (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0;
}
inline std::uint32_t SelectLocalActor(
    const AnomalyNteEntitySnapshotV1* snapshots,
    std::uint32_t count) {
    if (!snapshots) return UINT32_MAX;
    std::uint32_t selected=UINT32_MAX;
    for (std::uint32_t i=0;i<count;++i) {
        if (!UsableActorSnapshot(snapshots[i])) continue;
        if ((snapshots[i].flags & ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER)==0) continue;
        if (selected!=UINT32_MAX) return UINT32_MAX; // ambiguous: fail closed
        selected=i;
    }
    return selected;
}
} // namespace nailong
