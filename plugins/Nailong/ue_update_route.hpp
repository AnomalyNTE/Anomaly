#pragma once

namespace nailong {
enum class BoundUpdateRoute { Unbound, Animate, Restore, RestoreThenDiscover, Retry };

inline BoundUpdateRoute RouteBoundUpdate(bool enabled, bool bound, bool revision_matches, int actor_alive) {
    if (!enabled) return BoundUpdateRoute::Restore;
    if (!bound) return BoundUpdateRoute::Unbound;
    if (!revision_matches || actor_alive == 0) return BoundUpdateRoute::RestoreThenDiscover;
    if (actor_alive == 1) return BoundUpdateRoute::Animate;
    return BoundUpdateRoute::Retry;
}
} // namespace nailong
