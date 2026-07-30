#pragma once

#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/platform_settings.hpp"

namespace anomaly {

// Process-lifetime function table; calls are valid only while the current thread owns
// an active ImGui frame and the UI service is published by the renderer.
[[nodiscard]] const AnomalyUiServiceV1* HostUiServiceTable() noexcept;

// Host-managed windows consume a pending collapse request together at the start
// of a frame. ESP foreground drawing remains independent from this menu state.
void SetHostUiMenusCollapsed(bool collapsed) noexcept;
[[nodiscard]] bool HostUiMenusCollapsed() noexcept;
void RequestHostUiManagementExpansion() noexcept;
[[nodiscard]] bool ConsumeHostUiManagementExpansionRequest() noexcept;
[[nodiscard]] bool HostUiMenusCaptureMouse() noexcept;
void SetHostUiInputCapturePolicy(PlatformInputCapturePolicy policy) noexcept;
void SetHostUiDeveloperMode(bool enabled) noexcept;
[[nodiscard]] bool HostUiDeveloperModeEnabled() noexcept;
// These resolve the top-level host window even when called from a child.
// They are valid only while an active ImGui frame owns a current window.
[[nodiscard]] bool HostUiCurrentWindowLocked() noexcept;
void SetHostUiCurrentWindowLocked(bool locked) noexcept;
void PrepareHostUiFrame() noexcept;
[[nodiscard]] bool UpdateHostUiWindowState() noexcept;

}  // namespace anomaly
