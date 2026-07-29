#pragma once

#include <cstdint>

namespace ue5mem::embedded {

inline constexpr std::uint32_t kEmbeddedWmSetCursor = 0x0020u;
inline constexpr std::uint16_t kEmbeddedHtClient = 1u;

[[nodiscard]] constexpr bool ShouldForwardCollapsedClientCursorToGame(
    bool menus_capture_mouse,
    std::uint32_t message,
    std::uint16_t hit_test) noexcept {
    return !menus_capture_mouse && message == kEmbeddedWmSetCursor &&
        hit_test == kEmbeddedHtClient;
}

}  // namespace ue5mem::embedded
