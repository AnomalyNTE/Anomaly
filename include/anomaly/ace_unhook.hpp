#pragma once

#include <cstdint>
#include <filesystem>

namespace anomaly {

// What the restore did.
struct AceUnhookReport final {
    // False when the anti-cheat is absent, or its threads and images could not be inspected.
    bool available{};
    // Passes run (now exactly one), prologue writes, and hooks found re-applied after a restore.
    unsigned long passes{};
    unsigned long restored{};
    unsigned long rehooks{};
};

// Restores what the anti-cheat hooked. The threads that write those hooks are frozen, the named loader entries
// and every hooked export prologue of the system modules are put back, and the call returns: nothing stays
// resident, and every write is appended to `ace-unhook-trace.txt` in `trace_directory`.
[[nodiscard]] AceUnhookReport StartAntiCheatHookRestore(
    const std::filesystem::path& trace_directory) noexcept;

}  // namespace anomaly