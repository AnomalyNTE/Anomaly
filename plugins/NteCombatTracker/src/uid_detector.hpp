#pragma once

#include "account_identity.hpp"
#include "native_layout.hpp"

#include <anomaly/sdk/anomaly_sdk.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace nte::account {

enum class UidDetectorPhase : std::uint8_t {
    WaitingForServices,
    WaitingForBindings,
    Scanning,
    WidgetPending,
    Detected
};

struct UidDetectorDiagnostics {
    UidDetectorPhase phase{UidDetectorPhase::WaitingForServices};
    std::string detected_uid;
    std::string active_account_uid;
    std::string missing_services;
    std::string missing_bindings;
    UidDetectorPhase last_failure_phase{UidDetectorPhase::WaitingForServices};
    std::string last_failure;
    bool current_observation_valid{};
    std::uint64_t update_tick{};
    std::uint64_t object_generation{};
    std::uint32_t object_count{};
    std::uint32_t scan_cursor{};
    std::uint32_t scan_start{};
    std::size_t tracked_widgets{};
    std::uint64_t successful_reads{};
    std::uint64_t failed_reads{};
    std::string retry_status;
    std::uint64_t next_retry_tick{};
};

// Read-only extraction of the original numeric TextBlock_RoleID value. The
// discovery path is derived from Anomaly v1.7.0's FakeUID 1.1.4 object
// discovery and validation path; it intentionally contains none of FakeUID's
// SetText behavior. Its signatures, offsets and vtable slot come from
// profile/compatibility.profile.json through `native::Snapshot`.
class UidDetector {
public:
    // The profile-derived snapshot is owned by the caller for the whole
    // session; the detector only reads it.
    UidDetector(const AnomalyHostApiV1* host, const native::Snapshot& native);
    ~UidDetector();
    UidDetector(UidDetector&&) noexcept;
    UidDetector& operator=(UidDetector&&) noexcept;
    UidDetector(const UidDetector&) = delete;
    UidDetector& operator=(const UidDetector&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::optional<AccountIdentity> Update() noexcept;
    [[nodiscard]] UidDetectorDiagnostics diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nte::account
