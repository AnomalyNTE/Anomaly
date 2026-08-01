#pragma once

#include "anomaly/game_tick_hook.hpp"
#include "anomaly/ue5_nte_adapter.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

struct NteProfileRuntimeOptions {
    std::filesystem::path runtime_root;
    std::filesystem::path profile_directory{L"profiles"};
    std::filesystem::path local_profile_directory{L"profiles-local"};
    std::filesystem::path managed_profile_directory{L"state/profiles/managed"};
    bool profile_overrides_enabled{true};
    std::string game_id{"nte"};
    HMODULE game_module{};
    CoreMemoryServices memory_services;
    std::chrono::milliseconds section_readiness_timeout{};
    std::chrono::milliseconds section_readiness_poll_interval{std::chrono::milliseconds(50)};
    NteSnapshotSamplingOptions snapshot_sampling;
    std::function<void(std::uint32_t thread_id, double duration_micros)>
        tick_evidence_observer;
};

struct NteProfileEvidenceSnapshot {
    std::optional<BuildFingerprint> fingerprint;
    std::optional<BuildProfile> profile;
    std::optional<ProfileResolutionSnapshot> resolution;
    bool tick_hook_ready{};
    bool ahud_hook_ready{};
    std::uint32_t game_thread_id{};
    std::uint64_t tick_sequence{};
    std::uint64_t rejected_thread_ticks{};
};

class NteProfileRuntime final {
public:
    explicit NteProfileRuntime(NteProfileRuntimeOptions options);
    ~NteProfileRuntime();

    NteProfileRuntime(const NteProfileRuntime&) = delete;
    NteProfileRuntime& operator=(const NteProfileRuntime&) = delete;

    // Unknown builds and partial symbol failures are successful degraded starts.
    [[nodiscard]] bool Start(std::stop_token stop_token = {}) noexcept;
    // Uses one deadline for both hooks and the adapter. A timed-out generation
    // is retained in process quarantine so Runtime shutdown can continue.
    bool Stop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;

    [[nodiscard]] bool Started() const noexcept;
    [[nodiscard]] std::optional<BuildFingerprint> Fingerprint() const;
    [[nodiscard]] std::shared_ptr<const ProfileResolutionSnapshot> Resolution() const;
    [[nodiscard]] std::shared_ptr<Ue5NteAdapter> Adapter() const;
    [[nodiscard]] NteProfileEvidenceSnapshot Evidence() const;
    [[nodiscard]] std::string DiagnosticsJson() const;
    [[nodiscard]] std::vector<HookRecordView> Hooks() const;
    [[nodiscard]] std::string ExecuteReflectionQuery(std::string_view request) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
