#pragma once

#include "anomaly/i18n.hpp"
#include "anomaly/platform_ui_theme.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

inline constexpr std::uint32_t kPlatformInterfaceScaleMinimumPercent = 100;
inline constexpr std::uint32_t kPlatformInterfaceScaleMaximumPercent = 200;
inline constexpr std::uint32_t kPlatformInterfaceScaleStepPercent = 5;
inline constexpr std::uint32_t kPlatformInterfaceScaleDefaultPercent = 125;

enum class PlatformInputCapturePolicy : std::uint8_t {
    Automatic,
    MenuOpen,
};

enum class PlatformUpdateChannel : std::uint8_t {
    Stable,
    Preview,
    Nightly,
};

enum class PlatformMinimumLogLevel : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
};

struct PlatformSettingsValues final {
    LanguagePreference interface_language{LanguagePreference::Auto};
    PlatformUiPalette interface_palette{PlatformUiPalette::AnomalyHub};
    PlatformUiCustomColors interface_custom_colors;
    std::uint32_t interface_scale_percent{kPlatformInterfaceScaleDefaultPercent};
    std::uint32_t interface_opacity_percent{100};
    bool interface_reduced_motion{};
    bool interface_remember_last_route{true};
    std::uint32_t input_menu_toggle{0x2d};
    PlatformInputCapturePolicy input_capture_policy{PlatformInputCapturePolicy::Automatic};
    bool input_gamepad_navigation{true};
    PlatformUpdateChannel updates_channel{PlatformUpdateChannel::Stable};
    bool updates_automatic_check{true};
    bool updates_include_disabled{true};
    PlatformMinimumLogLevel diagnostics_log_level{PlatformMinimumLogLevel::Info};
    std::uint32_t diagnostics_ring_capacity{10000};
    bool advanced_developer_mode{};
    bool advanced_detailed_performance_diagnostics{};

    friend bool operator==(const PlatformSettingsValues&, const PlatformSettingsValues&) = default;
};

struct PlatformSettingsValidationError final {
    std::string setting_id;
    std::string message;
};

struct PlatformSettingsSnapshot final {
    bool ready{};
    std::uint64_t revision{};
    PlatformSettingsValues values;
    std::string last_route{"plugins"};
    std::string reason{"settings store has not started"};
};

enum class PlatformSettingsApplyCode : std::uint8_t {
    Applied,
    RevisionConflict,
    ValidationFailed,
    ProviderUnavailable,
    IoFailure,
};

struct PlatformSettingsApplyRequest final {
    std::uint64_t expected_revision{};
    PlatformSettingsValues values;
};

struct PlatformSettingsApplyResult final {
    PlatformSettingsApplyCode code{PlatformSettingsApplyCode::ProviderUnavailable};
    PlatformSettingsSnapshot snapshot;
    std::vector<PlatformSettingsValidationError> validation_errors;
    std::string message;

    [[nodiscard]] bool Applied() const noexcept {
        return code == PlatformSettingsApplyCode::Applied;
    }
};

[[nodiscard]] std::vector<PlatformSettingsValidationError> ValidatePlatformSettings(
    const PlatformSettingsValues& values);
[[nodiscard]] std::string_view PlatformInputCapturePolicyName(
    PlatformInputCapturePolicy value) noexcept;
[[nodiscard]] std::string_view PlatformUpdateChannelName(
    PlatformUpdateChannel value) noexcept;
[[nodiscard]] std::string_view PlatformMinimumLogLevelName(
    PlatformMinimumLogLevel value) noexcept;

class PlatformSettingsStore final {
public:
    explicit PlatformSettingsStore(std::filesystem::path runtime_root);
    ~PlatformSettingsStore();

    PlatformSettingsStore(const PlatformSettingsStore&) = delete;
    PlatformSettingsStore& operator=(const PlatformSettingsStore&) = delete;

    [[nodiscard]] bool Start();
    [[nodiscard]] PlatformSettingsSnapshot Snapshot() const;
    [[nodiscard]] PlatformSettingsApplyResult Apply(const PlatformSettingsApplyRequest& request);
    [[nodiscard]] bool RecordLastRoute(std::string_view route);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
