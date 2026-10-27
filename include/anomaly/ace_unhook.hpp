#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace anomaly {

// What the restore did before the watch took over.
struct AceUnhookReport final {
    // False when the anti-cheat is absent, or its threads and images could not be inspected.
    bool available{};
    // Passes run so far, prologue writes, and hooks found re-applied after a restore.
    unsigned long passes{};
    unsigned long restored{};
    unsigned long rehooks{};
};

// Starts the ACE inline-hook restore as a resident watch: the anti-cheat's own threads are frozen for the
// watch window, the on-disk prologues of the loader and anti-debug entries it hooks are put back every pass,
// and every flip is appended to `ace-unhook-trace.txt` in `trace_directory`.
//
// This is the implementation the ACE unhook probe plugin runs, moved into the core so it starts before any
// service or plugin loads anything. A watch window of zero means the whole session.
[[nodiscard]] AceUnhookReport StartAntiCheatHookRestore(
    const std::filesystem::path& trace_directory,
    std::chrono::milliseconds watch_window) noexcept;

}  // namespace anomaly