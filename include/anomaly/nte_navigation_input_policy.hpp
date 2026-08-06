#pragma once

#include "anomaly/hook_manager.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace anomaly {

// Host-owned input policy used by the NTE navigation bridge. It is active only
// while MoveToPointByTransform is being dispatched on the Game thread.
class NteNavigationInputPolicy final {
public:
    NteNavigationInputPolicy(
        std::unique_ptr<HookBackend> backend,
        std::uint32_t controller_get_player_character_vtable_offset,
        std::uint32_t character_set_custom_ignore_move_input_vtable_offset,
        std::uint32_t character_set_custom_limit_input_vtable_offset);
    ~NteNavigationInputPolicy();

    NteNavigationInputPolicy(const NteNavigationInputPolicy&) = delete;
    NteNavigationInputPolicy& operator=(const NteNavigationInputPolicy&) = delete;

    [[nodiscard]] bool Start(void* target);
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    [[nodiscard]] bool Started() const noexcept;

    // Returns the prior thread-local scope and marks this policy active.
    [[nodiscard]] void* Enter() noexcept;
    void Leave(void* previous) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
