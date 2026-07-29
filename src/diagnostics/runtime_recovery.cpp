#include "anomaly/runtime_recovery.hpp"

#include "anomaly/reliable_storage.hpp"
#include "anomaly/semver.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace anomaly {
namespace {

using Json = nlohmann::json;

constexpr std::wstring_view kRecoveryFile = L"runtime-recovery.json";
constexpr std::size_t kMaximumRecoveryBytes = 128U * 1024U;
constexpr std::size_t kMaximumIdentifierBytes = 128;
constexpr std::size_t kMaximumReasonBytes = 256;

RuntimeRecoveryResult Failure(RuntimeRecoveryError error, std::string message) {
    return {error, std::move(message), std::nullopt};
}

bool IsReparsePoint(const std::filesystem::path& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool ValidIdentifier(std::string_view value) noexcept {
    if (value.size() > kMaximumIdentifierBytes) return false;
    return std::ranges::all_of(value, [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '_' ||
            character == '-';
    });
}

bool ValidRuntimeVersion(std::string_view value) {
    if (value.empty()) return true;
    const auto parsed = ParseSemanticVersion(value);
    return parsed && parsed->ToString() == value;
}

std::span<const std::byte> Bytes(const std::string& value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::string Text(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::optional<RuntimeFailureSource> ParseSource(std::string_view value) noexcept {
    if (value == "unknown") return RuntimeFailureSource::Unknown;
    if (value == "runtime-startup") return RuntimeFailureSource::RuntimeStartup;
    if (value == "profile-override") return RuntimeFailureSource::ProfileOverride;
    if (value == "plugin-generation") return RuntimeFailureSource::PluginGeneration;
    if (value == "render-initialization") return RuntimeFailureSource::RenderInitialization;
    return std::nullopt;
}

bool KnownSource(RuntimeFailureSource source) noexcept {
    switch (source) {
    case RuntimeFailureSource::Unknown:
    case RuntimeFailureSource::RuntimeStartup:
    case RuntimeFailureSource::ProfileOverride:
    case RuntimeFailureSource::PluginGeneration:
    case RuntimeFailureSource::RenderInitialization:
        return true;
    }
    return false;
}

bool ValidObservation(const RuntimeFailureObservation& observation) {
    if (!KnownSource(observation.source) || observation.observed_at_unix_seconds == 0 ||
        observation.incident_id.empty() || !ValidIdentifier(observation.incident_id) ||
        !ValidRuntimeVersion(observation.runtime_version) ||
        !ValidIdentifier(observation.profile_id) ||
        !ValidIdentifier(observation.plugin_id)) {
        return false;
    }
    if (observation.source == RuntimeFailureSource::PluginGeneration) {
        return !observation.plugin_id.empty() && observation.plugin_generation != 0;
    }
    return observation.plugin_id.empty() && observation.plugin_generation == 0;
}

Json ObservationJson(const RuntimeFailureObservation& observation) {
    return {
        {"observedAtUnixSeconds", observation.observed_at_unix_seconds},
        {"incidentId", observation.incident_id},
        {"source", RuntimeFailureSourceName(observation.source)},
        {"runtimeVersion", observation.runtime_version},
        {"profileId", observation.profile_id},
        {"pluginId", observation.plugin_id},
        {"pluginGeneration", observation.plugin_generation},
    };
}

Json StateJson(const RuntimeRecoveryState& state) {
    Json failures = Json::array();
    for (const auto& failure : state.recent_failures) {
        failures.push_back(ObservationJson(failure));
    }
    return {
        {"schemaVersion", state.schema_version},
        {"revision", state.revision},
        {"recentFailures", std::move(failures)},
        {"safeMode", {
            {"minimalCore", state.safe_mode.minimal_core},
            {"thirdPartyPluginsSuspended", state.safe_mode.third_party_plugins_suspended},
            {"profileOverridesSuspended", state.safe_mode.profile_overrides_suspended},
            {"reason", state.safe_mode.reason},
        }},
    };
}

bool ExactObject(const Json& value, std::span<const std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size()) return false;
    return std::ranges::all_of(keys, [&value](std::string_view key) {
        return value.contains(key);
    });
}

std::optional<RuntimeFailureObservation> ParseObservation(const Json& value) {
    constexpr std::array<std::string_view, 7> keys{
        "observedAtUnixSeconds", "incidentId", "source", "runtimeVersion", "profileId",
        "pluginId", "pluginGeneration"};
    if (!ExactObject(value, keys)) return std::nullopt;
    try {
        const auto source = ParseSource(value.at("source").get<std::string>());
        if (!source) return std::nullopt;
        RuntimeFailureObservation observation{
            value.at("observedAtUnixSeconds").get<std::uint64_t>(),
            value.at("incidentId").get<std::string>(),
            *source,
            value.at("runtimeVersion").get<std::string>(),
            value.at("profileId").get<std::string>(),
            value.at("pluginId").get<std::string>(),
            value.at("pluginGeneration").get<std::uint64_t>(),
        };
        return ValidObservation(observation)
            ? std::optional<RuntimeFailureObservation>(std::move(observation))
            : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<RuntimeRecoveryState> ParseState(
    std::string_view text, const RuntimeRecoveryPolicy& policy) {
    try {
        const Json document = Json::parse(text.begin(), text.end());
        constexpr std::array<std::string_view, 4> root_keys{
            "schemaVersion", "revision", "recentFailures", "safeMode"};
        constexpr std::array<std::string_view, 4> safe_mode_keys{
            "minimalCore", "thirdPartyPluginsSuspended", "profileOverridesSuspended", "reason"};
        if (!ExactObject(document, root_keys) ||
            !ExactObject(document.at("safeMode"), safe_mode_keys) ||
            !document.at("recentFailures").is_array() ||
            document.at("recentFailures").size() > policy.maximum_recent_failures) {
            return std::nullopt;
        }
        RuntimeRecoveryState state;
        state.schema_version = document.at("schemaVersion").get<std::uint32_t>();
        state.revision = document.at("revision").get<std::uint64_t>();
        const Json& safe_mode = document.at("safeMode");
        state.safe_mode = {
            safe_mode.at("minimalCore").get<bool>(),
            safe_mode.at("thirdPartyPluginsSuspended").get<bool>(),
            safe_mode.at("profileOverridesSuspended").get<bool>(),
            safe_mode.at("reason").get<std::string>(),
        };
        if (state.schema_version != kRuntimeRecoverySchemaVersion || state.revision == 0 ||
            state.safe_mode.reason.size() > kMaximumReasonBytes ||
            (!state.safe_mode.Active() && !state.safe_mode.reason.empty())) {
            return std::nullopt;
        }
        for (const Json& value : document.at("recentFailures")) {
            auto observation = ParseObservation(value);
            if (!observation) return std::nullopt;
            state.recent_failures.push_back(std::move(*observation));
        }
        return state;
    } catch (...) {
        return std::nullopt;
    }
}

bool SameFingerprint(
    const RuntimeFailureObservation& left,
    const RuntimeFailureObservation& right) noexcept {
    return left.source == right.source && left.runtime_version == right.runtime_version &&
        left.profile_id == right.profile_id && left.plugin_id == right.plugin_id &&
        left.plugin_generation == right.plugin_generation;
}

void ApplySafeMode(
    RuntimeRecoveryState& state, const RuntimeFailureObservation& observation) {
    switch (observation.source) {
    case RuntimeFailureSource::PluginGeneration:
        state.safe_mode.third_party_plugins_suspended = true;
        break;
    case RuntimeFailureSource::ProfileOverride:
        state.safe_mode.profile_overrides_suspended = true;
        break;
    case RuntimeFailureSource::RuntimeStartup:
    case RuntimeFailureSource::RenderInitialization:
    case RuntimeFailureSource::Unknown:
        state.safe_mode.minimal_core = true;
        break;
    }
    const std::size_t active_axes =
        static_cast<std::size_t>(state.safe_mode.minimal_core) +
        static_cast<std::size_t>(state.safe_mode.third_party_plugins_suspended) +
        static_cast<std::size_t>(state.safe_mode.profile_overrides_suspended);
    state.safe_mode.reason = active_axes > 1
        ? "multiple crash loops"
        : std::string(RuntimeFailureSourceName(observation.source)) + " crash loop";
}

class RecoveryLock final {
public:
    explicit RecoveryLock(const std::filesystem::path& state_directory) noexcept {
        if (state_directory.empty()) {
            last_error_ = ERROR_INVALID_PARAMETER;
            return;
        }
        const auto path = state_directory / L"runtime-recovery.lock";
        for (int attempt = 0; attempt < 100; ++attempt) {
            handle_ = CreateFileW(
                path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) break;
            const DWORD error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) {
                last_error_ = error;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (handle_ == INVALID_HANDLE_VALUE) {
            last_error_ = ERROR_TIMEOUT;
            return;
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                handle_, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes &
                (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
            last_error_ = GetLastError();
            if (last_error_ == ERROR_SUCCESS) last_error_ = ERROR_ACCESS_DENIED;
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    ~RecoveryLock() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    [[nodiscard]] bool Acquired() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] DWORD LastError() const noexcept { return last_error_; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    DWORD last_error_{ERROR_SUCCESS};
};

}  // namespace

class RuntimeRecoveryStore::Impl final {
public:
    Impl(std::filesystem::path runtime_root, RuntimeRecoveryPolicy policy) noexcept
        : policy_(policy) {
        if (policy_.crash_loop_threshold < 2 ||
            policy_.crash_loop_window_seconds == 0 ||
            policy_.maximum_recent_failures < policy_.crash_loop_threshold ||
            policy_.maximum_recent_failures > 256) {
            initialization_error_ = RuntimeRecoveryError::InvalidPolicy;
            return;
        }
        try {
            root_ = std::filesystem::absolute(std::move(runtime_root));
            state_directory_ = root_ / L"state";
            std::error_code error;
            if (!std::filesystem::is_directory(root_, error) || error ||
                IsReparsePoint(root_)) {
                initialization_error_ = RuntimeRecoveryError::InvalidRoot;
                return;
            }
            if (std::filesystem::exists(state_directory_, error)) {
                if (error || !std::filesystem::is_directory(state_directory_, error) ||
                    IsReparsePoint(state_directory_)) {
                    initialization_error_ = RuntimeRecoveryError::InvalidRoot;
                    return;
                }
            } else {
                std::filesystem::create_directory(state_directory_, error);
                if (error) {
                    initialization_error_ = RuntimeRecoveryError::IoFailure;
                    return;
                }
            }
            storage_ = std::make_unique<ReliableStorage>(state_directory_.native());
            if (!storage_->InitializationResult()) {
                storage_.reset();
                initialization_error_ = RuntimeRecoveryError::InvalidRoot;
                return;
            }
            initialization_error_ = RuntimeRecoveryError::None;
        } catch (...) {
            initialization_error_ = RuntimeRecoveryError::InvalidRoot;
        }
    }

    RuntimeRecoveryResult Load() {
        RecoveryLock lock(state_directory_);
        if (auto failure = Ready(lock)) return *failure;
        return LoadLocked();
    }

    RuntimeRecoveryResult RecordFailure(const RuntimeFailureObservation& observation) {
        if (!ValidObservation(observation)) {
            return Failure(
                RuntimeRecoveryError::ObservationInvalid,
                "runtime failure observation is invalid");
        }
        RecoveryLock lock(state_directory_);
        if (auto failure = Ready(lock)) return *failure;
        RuntimeRecoveryResult loaded = LoadLocked();
        RuntimeRecoveryState state;
        if (loaded.error != RuntimeRecoveryError::StateUnavailable) {
            if (!loaded.Ok()) return loaded;
            state = std::move(*loaded.state);
        }

        if (std::ranges::any_of(state.recent_failures, [&](const auto& existing) {
                return existing.incident_id == observation.incident_id;
            })) {
            return {RuntimeRecoveryError::None, {}, std::move(state)};
        }

        const auto oldest = observation.observed_at_unix_seconds >
                policy_.crash_loop_window_seconds
            ? observation.observed_at_unix_seconds - policy_.crash_loop_window_seconds
            : 0;
        std::erase_if(state.recent_failures, [&](const auto& existing) {
            return existing.observed_at_unix_seconds < oldest ||
                existing.observed_at_unix_seconds > observation.observed_at_unix_seconds;
        });
        state.recent_failures.push_back(observation);
        if (state.recent_failures.size() > policy_.maximum_recent_failures) {
            state.recent_failures.erase(
                state.recent_failures.begin(),
                state.recent_failures.begin() + static_cast<std::ptrdiff_t>(
                    state.recent_failures.size() - policy_.maximum_recent_failures));
        }
        const std::size_t matching = static_cast<std::size_t>(std::ranges::count_if(
            state.recent_failures,
            [&](const auto& existing) { return SameFingerprint(existing, observation); }));
        if (matching >= policy_.crash_loop_threshold) ApplySafeMode(state, observation);
        ++state.revision;
        return SaveLocked(std::move(state));
    }

    RuntimeRecoveryResult MarkHealthy() {
        RecoveryLock lock(state_directory_);
        if (auto failure = Ready(lock)) return *failure;
        RuntimeRecoveryResult loaded = LoadLocked();
        if (!loaded.Ok()) return loaded;
        RuntimeRecoveryState state = std::move(*loaded.state);
        if (state.recent_failures.empty()) return loaded;
        state.recent_failures.clear();
        ++state.revision;
        return SaveLocked(std::move(state));
    }

    RuntimeRecoveryResult Restore(RuntimeRecoveryAxis axis) {
        RecoveryLock lock(state_directory_);
        if (auto failure = Ready(lock)) return *failure;
        RuntimeRecoveryResult loaded = LoadLocked();
        if (!loaded.Ok()) return loaded;
        RuntimeRecoveryState state = std::move(*loaded.state);
        bool* selected{};
        switch (axis) {
        case RuntimeRecoveryAxis::MinimalCore:
            selected = &state.safe_mode.minimal_core;
            break;
        case RuntimeRecoveryAxis::ThirdPartyPlugins:
            selected = &state.safe_mode.third_party_plugins_suspended;
            break;
        case RuntimeRecoveryAxis::ProfileOverrides:
            selected = &state.safe_mode.profile_overrides_suspended;
            break;
        }
        if (selected == nullptr || !*selected) return loaded;
        *selected = false;
        state.safe_mode.reason = state.safe_mode.Active()
            ? "safe mode safeguards remain" : std::string{};
        ++state.revision;
        return SaveLocked(std::move(state));
    }

private:
    std::optional<RuntimeRecoveryResult> Ready(const RecoveryLock& lock) const {
        if (initialization_error_ != RuntimeRecoveryError::None || !storage_) {
            return Failure(initialization_error_, "runtime recovery root is unavailable");
        }
        if (!lock.Acquired()) {
            return Failure(
                RuntimeRecoveryError::LockFailure,
                "runtime recovery lock failed with error " +
                    std::to_string(lock.LastError()));
        }
        return std::nullopt;
    }

    RuntimeRecoveryResult LoadLocked() {
        const StorageReadResult read = storage_->Read(kRecoveryFile, kMaximumRecoveryBytes);
        if (!read) {
            if (read.result.win32_error == ERROR_FILE_NOT_FOUND ||
                read.result.win32_error == ERROR_PATH_NOT_FOUND) {
                return Failure(
                    RuntimeRecoveryError::StateUnavailable,
                    "runtime recovery state does not exist");
            }
            return Failure(
                RuntimeRecoveryError::IoFailure,
                "runtime recovery state could not be read");
        }
        auto state = ParseState(Text(read.bytes), policy_);
        return state
            ? RuntimeRecoveryResult{RuntimeRecoveryError::None, {}, std::move(state)}
            : Failure(RuntimeRecoveryError::StateInvalid, "runtime recovery state is invalid");
    }

    RuntimeRecoveryResult SaveLocked(RuntimeRecoveryState state) {
        const std::string json = StateJson(state).dump(2) + '\n';
        if (!storage_->WriteAtomic(kRecoveryFile, Bytes(json))) {
            return Failure(
                RuntimeRecoveryError::IoFailure,
                "runtime recovery state could not be committed");
        }
        return {RuntimeRecoveryError::None, {}, std::move(state)};
    }

    RuntimeRecoveryPolicy policy_;
    std::filesystem::path root_;
    std::filesystem::path state_directory_;
    std::unique_ptr<ReliableStorage> storage_;
    RuntimeRecoveryError initialization_error_{RuntimeRecoveryError::InvalidRoot};
};

RuntimeRecoveryStore::RuntimeRecoveryStore(
    std::filesystem::path runtime_root, RuntimeRecoveryPolicy policy) noexcept {
    try {
        impl_ = std::make_unique<Impl>(std::move(runtime_root), policy);
    } catch (...) {
        impl_.reset();
    }
}

RuntimeRecoveryStore::~RuntimeRecoveryStore() = default;

RuntimeRecoveryResult RuntimeRecoveryStore::Load() {
    return impl_ ? impl_->Load()
                 : Failure(RuntimeRecoveryError::InvalidRoot, "runtime recovery is unavailable");
}

RuntimeRecoveryResult RuntimeRecoveryStore::RecordFailure(
    const RuntimeFailureObservation& observation) {
    return impl_ ? impl_->RecordFailure(observation)
                 : Failure(RuntimeRecoveryError::InvalidRoot, "runtime recovery is unavailable");
}

RuntimeRecoveryResult RuntimeRecoveryStore::MarkHealthy() {
    return impl_ ? impl_->MarkHealthy()
                 : Failure(RuntimeRecoveryError::InvalidRoot, "runtime recovery is unavailable");
}

RuntimeRecoveryResult RuntimeRecoveryStore::Restore(RuntimeRecoveryAxis axis) {
    return impl_ ? impl_->Restore(axis)
                 : Failure(RuntimeRecoveryError::InvalidRoot, "runtime recovery is unavailable");
}

const char* RuntimeFailureSourceName(RuntimeFailureSource source) noexcept {
    switch (source) {
    case RuntimeFailureSource::Unknown: return "unknown";
    case RuntimeFailureSource::RuntimeStartup: return "runtime-startup";
    case RuntimeFailureSource::ProfileOverride: return "profile-override";
    case RuntimeFailureSource::PluginGeneration: return "plugin-generation";
    case RuntimeFailureSource::RenderInitialization: return "render-initialization";
    }
    return "unknown";
}

}  // namespace anomaly
