#include "anomaly/runtime_recovery.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

void Write(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

anomaly::RuntimeFailureObservation PluginFailure(
    std::uint64_t timestamp, std::uint64_t generation = 7) {
    return {
        timestamp,
        "plugin-" + std::to_string(timestamp) + "-" + std::to_string(generation),
        anomaly::RuntimeFailureSource::PluginGeneration,
        "1.2.3",
        "nte-current",
        "example.plugin",
        generation,
    };
}

}  // namespace

int main() {
    bool result = true;
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-runtime-recovery-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);

    anomaly::RuntimeRecoveryPolicy policy;
    policy.crash_loop_threshold = 3;
    policy.crash_loop_window_seconds = 60;
    policy.maximum_recent_failures = 8;
    anomaly::RuntimeRecoveryStore store(root, policy);

    result = Check(
        store.Load().error == anomaly::RuntimeRecoveryError::StateUnavailable,
        "missing recovery state did not remain unavailable") && result;
    result = Check(
        store.RecordFailure({}).error == anomaly::RuntimeRecoveryError::ObservationInvalid,
        "invalid recovery observation was accepted") && result;
    auto invalid_source = PluginFailure(99);
    invalid_source.source = static_cast<anomaly::RuntimeFailureSource>(255);
    invalid_source.plugin_id.clear();
    invalid_source.plugin_generation = 0;
    result = Check(
        store.RecordFailure(invalid_source).error ==
            anomaly::RuntimeRecoveryError::ObservationInvalid,
        "unknown recovery source enum was accepted") && result;

    auto recorded = store.RecordFailure(PluginFailure(100));
    const auto duplicate = store.RecordFailure(PluginFailure(100));
    result = Check(duplicate.Ok() && duplicate.state->revision == recorded.state->revision &&
        duplicate.state->recent_failures.size() == 1,
        "duplicate incident was counted more than once") && result;
    recorded = store.RecordFailure(PluginFailure(120));
    result = Check(recorded.Ok() && !recorded.state->safe_mode.Active(),
        "safe mode activated before the crash-loop threshold") && result;

    recorded = store.RecordFailure(PluginFailure(130, 8));
    result = Check(recorded.Ok() && !recorded.state->safe_mode.Active(),
        "different plugin generation shared a crash-loop fingerprint") && result;
    recorded = store.RecordFailure(PluginFailure(140));
    result = Check(recorded.Ok() &&
        recorded.state->safe_mode.third_party_plugins_suspended &&
        !recorded.state->safe_mode.minimal_core &&
        recorded.state->safe_mode.reason == "plugin-generation crash loop",
        "plugin crash loop did not suspend the plugin axis") && result;

    anomaly::RuntimeFailureObservation profile{
        145, "profile-145", anomaly::RuntimeFailureSource::ProfileOverride,
        "1.2.3", "nte-local", {}, 0};
    static_cast<void>(store.RecordFailure(profile));
    profile.observed_at_unix_seconds = 150;
    profile.incident_id = "profile-150";
    static_cast<void>(store.RecordFailure(profile));
    profile.observed_at_unix_seconds = 155;
    profile.incident_id = "profile-155";
    recorded = store.RecordFailure(profile);
    result = Check(recorded.Ok() &&
        recorded.state->safe_mode.third_party_plugins_suspended &&
        recorded.state->safe_mode.profile_overrides_suspended,
        "profile crash loop did not preserve independent recovery axes") && result;

    anomaly::RuntimeFailureObservation startup{
        160, "runtime-160", anomaly::RuntimeFailureSource::RuntimeStartup,
        "2.0.0", {}, {}, 0};
    static_cast<void>(store.RecordFailure(startup));
    startup.observed_at_unix_seconds = 165;
    startup.incident_id = "runtime-165";
    static_cast<void>(store.RecordFailure(startup));
    startup.observed_at_unix_seconds = 170;
    startup.incident_id = "runtime-170";
    recorded = store.RecordFailure(startup);
    result = Check(recorded.Ok() && recorded.state->safe_mode.minimal_core,
        "runtime startup crash loop did not request minimal Core mode") && result;

    const auto restored_plugins =
        store.Restore(anomaly::RuntimeRecoveryAxis::ThirdPartyPlugins);
    result = Check(restored_plugins.Ok() &&
        !restored_plugins.state->safe_mode.third_party_plugins_suspended &&
        restored_plugins.state->safe_mode.profile_overrides_suspended &&
        restored_plugins.state->safe_mode.minimal_core,
        "restoring one recovery axis changed another axis") && result;

    const auto healthy = store.MarkHealthy();
    result = Check(healthy.Ok() && healthy.state->recent_failures.empty() &&
        healthy.state->safe_mode.Active(),
        "healthy confirmation cleared explicit safe-mode decisions") && result;

    anomaly::RuntimeRecoveryStore second(root, policy);
    const auto second_load = second.Load();
    result = Check(second_load.Ok() &&
        second_load.state->revision == healthy.state->revision,
        "second recovery store did not observe the committed revision") && result;

    static_cast<void>(second.Restore(anomaly::RuntimeRecoveryAxis::ProfileOverrides));
    const auto final_restore =
        second.Restore(anomaly::RuntimeRecoveryAxis::MinimalCore);
    result = Check(final_restore.Ok() && !final_restore.state->safe_mode.Active() &&
        final_restore.state->safe_mode.reason.empty(),
        "final recovery restore did not clear safe-mode reason") && result;

    anomaly::RuntimeFailureObservation unknown{
        300, "unknown-300", anomaly::RuntimeFailureSource::Unknown,
        "1.2.3", {}, {}, 0};
    static_cast<void>(store.RecordFailure(unknown));
    unknown.observed_at_unix_seconds = 370;
    unknown.incident_id = "unknown-370";
    static_cast<void>(store.RecordFailure(unknown));
    unknown.observed_at_unix_seconds = 380;
    unknown.incident_id = "unknown-380";
    recorded = store.RecordFailure(unknown);
    result = Check(recorded.Ok() && !recorded.state->safe_mode.Active(),
        "expired failures contributed to a crash loop") && result;
    unknown.observed_at_unix_seconds = 390;
    unknown.incident_id = "unknown-390";
    recorded = store.RecordFailure(unknown);
    result = Check(recorded.Ok() && recorded.state->safe_mode.minimal_core,
        "unknown crash loop did not request minimal Core mode") && result;

    Write(root / L"state" / L"runtime-recovery.json", "{not-json");
    result = Check(
        store.Load().error == anomaly::RuntimeRecoveryError::StateInvalid &&
        store.RecordFailure(PluginFailure(200)).error ==
            anomaly::RuntimeRecoveryError::StateInvalid,
        "corrupt recovery state was silently overwritten") && result;

    anomaly::RuntimeRecoveryStore bad_policy(
        root, anomaly::RuntimeRecoveryPolicy{1, 60, 8});
    result = Check(
        bad_policy.Load().error == anomaly::RuntimeRecoveryError::InvalidPolicy,
        "invalid crash-loop policy was accepted") && result;

    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    return result ? 0 : 1;
}
