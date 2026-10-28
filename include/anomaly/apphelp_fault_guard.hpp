#pragma once

namespace anomaly {

// The anti-cheat asks the compatibility database about modules through a code path that dereferences the result
// of a lookup without checking it. For an image this process mapped itself that lookup has nothing to return, so
// the query faults inside apphelp -- and the game dies with it, several seconds after the core starts running in
// the game's own call paths.
//
// This guard answers that query the way a missing entry would: it catches the fault inside apphelp, unwinds the
// one frame that faulted, leaves zero in the return register, and lets the caller continue. Nothing about the
// image, the loader or the mapping changes, which is what keeps the core invisible.
struct ApphelpFaultGuardReport final {
    bool installed{};
    // Faults swallowed so far, read at the point of interest.
    unsigned long long caught{};
};

[[nodiscard]] ApphelpFaultGuardReport InstallApphelpFaultGuard() noexcept;

}  // namespace anomaly