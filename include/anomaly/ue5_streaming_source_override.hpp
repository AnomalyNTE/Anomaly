#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace anomaly {

// Owns the single process-wide hook on the local player controller's
// streaming-source update and the override window that redirects it.
//
// The controller re-evaluates its streaming source every frame, so rewriting the
// location it asks the world to stream keeps the destination loaded while an
// override is active. Every consumer shares this one hook, which is why it lives
// in the framework instead of in a plugin: the hook manager admits a single owner
// per target address, and plugins cannot coordinate that between themselves.
class Ue5StreamingSourceOverride final {
public:
    using Position = std::array<double, 3>;

    Ue5StreamingSourceOverride();
    ~Ue5StreamingSourceOverride();
    Ue5StreamingSourceOverride(const Ue5StreamingSourceOverride&) = delete;
    Ue5StreamingSourceOverride& operator=(const Ue5StreamingSourceOverride&) = delete;

    // Installs the hook on the controller's streaming-source update. The controller
    // instance changes across level transitions while the function address does not,
    // so a repeated Install with the same target only refreshes the controller.
    [[nodiscard]] bool Install(std::uintptr_t controller, std::uintptr_t streaming_source);
    void Remove() noexcept;
    [[nodiscard]] bool Installed() const noexcept;
    [[nodiscard]] std::uintptr_t Controller() const noexcept;

    // An override with a zero duration stays until ClearOverride or Remove. A rotation
    // given here is applied only when redirect_rotation is set, which is what a free
    // camera following the view needs; a preload only cares about the position.
    void SetOverride(
        const Position& position, const Position& rotation, bool redirect_rotation,
        std::chrono::milliseconds duration);
    void ClearOverride() noexcept;
    [[nodiscard]] bool Active() const noexcept;
    [[nodiscard]] std::chrono::milliseconds Remaining(
        std::chrono::steady_clock::time_point now) const noexcept;
    // Drops an expired override. Driven from the game tick.
    void Expire(std::chrono::steady_clock::time_point now) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly