#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anomaly {

inline constexpr std::uint32_t kRuntimeRecoverySchemaVersion = 1;

enum class RuntimeFailureSource : std::uint8_t {
    Unknown,
    RuntimeStartup,
    ProfileOverride,
    PluginGeneration,
    RenderInitialization,
};

enum class RuntimeRecoveryAxis : std::uint8_t {
    MinimalCore,
    ThirdPartyPlugins,
    ProfileOverrides,
};

struct RuntimeFailureObservation {
    std::uint64_t observed_at_unix_seconds{};
    std::string incident_id;
    RuntimeFailureSource source{RuntimeFailureSource::Unknown};
    std::string runtime_version;
    std::string profile_id;
    std::string plugin_id;
    std::uint64_t plugin_generation{};
};

struct RuntimeSafeModeState {
    bool minimal_core{};
    bool third_party_plugins_suspended{};
    bool profile_overrides_suspended{};
    std::string reason;

    [[nodiscard]] bool Active() const noexcept {
        return minimal_core || third_party_plugins_suspended ||
            profile_overrides_suspended;
    }
};

struct RuntimeRecoveryState {
    std::uint32_t schema_version{kRuntimeRecoverySchemaVersion};
    std::uint64_t revision{};
    std::vector<RuntimeFailureObservation> recent_failures;
    RuntimeSafeModeState safe_mode;
};

struct RuntimeRecoveryPolicy {
    std::uint32_t crash_loop_threshold{3};
    std::uint64_t crash_loop_window_seconds{600};
    std::size_t maximum_recent_failures{32};
};

enum class RuntimeRecoveryError : std::uint8_t {
    None,
    InvalidRoot,
    InvalidPolicy,
    LockFailure,
    StateUnavailable,
    StateInvalid,
    ObservationInvalid,
    IoFailure,
};

struct RuntimeRecoveryResult {
    RuntimeRecoveryError error{RuntimeRecoveryError::None};
    std::string message;
    std::optional<RuntimeRecoveryState> state;

    [[nodiscard]] bool Ok() const noexcept {
        return error == RuntimeRecoveryError::None && state.has_value();
    }
};

class RuntimeRecoveryStore final {
public:
    explicit RuntimeRecoveryStore(
        std::filesystem::path runtime_root,
        RuntimeRecoveryPolicy policy = {}) noexcept;
    ~RuntimeRecoveryStore();

    RuntimeRecoveryStore(const RuntimeRecoveryStore&) = delete;
    RuntimeRecoveryStore& operator=(const RuntimeRecoveryStore&) = delete;

    [[nodiscard]] RuntimeRecoveryResult Load();
    [[nodiscard]] RuntimeRecoveryResult RecordFailure(
        const RuntimeFailureObservation& observation);
    [[nodiscard]] RuntimeRecoveryResult MarkHealthy();
    [[nodiscard]] RuntimeRecoveryResult Restore(RuntimeRecoveryAxis axis);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] const char* RuntimeFailureSourceName(RuntimeFailureSource source) noexcept;

}  // namespace anomaly
