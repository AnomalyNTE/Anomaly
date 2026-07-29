#pragma once

namespace anomaly {

[[nodiscard]] constexpr bool ShouldTogglePlatformMenus(
    const bool recording_menu_key, const int asynchronous_key_state) noexcept {
    return !recording_menu_key && (asynchronous_key_state & 1) != 0;
}

}  // namespace anomaly
