#pragma once

#include "anomaly/symbol_resolver.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace anomaly {

// Bounds a diagnostic reflection request so a pipe client cannot turn object
// discovery into an unbounded game-thread or worker-thread operation.
struct Ue5ReflectionQueryOptions final {
    std::size_t maximum_results{256};
    std::size_t maximum_objects_per_request{262144};
};

// Reflection queries are deliberately read-only and operate only on a
// Runtime-selected profile whose current symbols and feature
// validators have passed. They enumerate metadata; they never invoke
// ProcessEvent or expose a mutable UObject handle.
struct Ue5ReflectionQueryContext final {
    const BuildProfile& profile;
    const ProfileResolutionSnapshot& resolution;
    const SymbolMemory& memory;
    Ue5ReflectionQueryOptions options{};
};

// Supported requests:
//   actors <filter|*> [limit] [cursor]
//   functions <filter|*> [limit] [cursor]
// Filters are ASCII case-insensitive substrings. A cursor is the next actor
// or GObjects slot to inspect and makes large catalogs resumable.
[[nodiscard]] std::string ExecuteUe5ReflectionQuery(
    const Ue5ReflectionQueryContext& context,
    std::string_view request) noexcept;

}  // namespace anomaly
