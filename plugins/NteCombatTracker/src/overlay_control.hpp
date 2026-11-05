#pragma once

namespace nte::combat_stats::overlay {

inline constexpr wchar_t kManualEndEventName[] =
    L"Local\\NteCombatStats.ManualEnd.v1";
inline constexpr wchar_t kPluginStoppingEventName[] =
    L"Local\\NteCombatStats.PluginStopping.v1";
inline constexpr wchar_t kWindowClassName[] = L"NteCombatTracker.Window.v1";
inline constexpr wchar_t kWindowTitle[] = L"NTE Combat Tracker";
inline constexpr wchar_t kReloadSettingsMessageName[] =
    L"NteCombatStats.ReloadOverlaySettings.v1";
inline constexpr wchar_t kCloseOverlayMessageName[] =
    L"NteCombatStats.CloseOverlay.v1";

}  // namespace nte::combat_stats::overlay
