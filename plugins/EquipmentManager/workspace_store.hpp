#pragma once

#include <anomaly/sdk/plugin.h>
#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace equipment_manager {

enum class WorkspaceFailure {
    invalid_request, stale_account, missing_account, last_account, missing_role,
    missing_slot, slot_limit, last_slot, slot_locked, conflict, invalid_plan,
    duplicate_uid, stale_snapshot, identity_unavailable, account_mismatch,
    invalid_archive, io_error
};
struct InvalidWorkspace { WorkspaceFailure reason; };
std::string_view WorkspaceErrorCode(WorkspaceFailure reason) noexcept;

// Worker/Lifecycle owned. User preferences go through anomaly.config; frozen
// plans, their versions, observed snapshots and jobs live in plugin-state.
class WorkspaceStore final {
public:
    WorkspaceStore();
    ~WorkspaceStore();
    WorkspaceStore(const WorkspaceStore&) = delete;
    WorkspaceStore& operator=(const WorkspaceStore&) = delete;
    void Load(const AnomalyHostApiV1* host, std::uint64_t plugin_generation);
    void Close() noexcept;
    bool Handles(std::string_view path) const;
    nlohmann::json Request(std::string_view method, std::string_view path,
        const nlohmann::json& input, const nlohmann::json& live, std::uint32_t& status);
    nlohmann::json View(const nlohmann::json& live);
    nlohmann::json Preferences();
    nlohmann::json ReadSnapshot(std::string_view id, const nlohmann::json& owner) const;
    void SavePreferences(const nlohmann::json& preferences);
    void ValidateAccount(const nlohmann::json& input) const;
    nlohmann::json ReservedEquipment(std::string_view except_role = {}) const;
    void Capture(const nlohmann::json& inventory);
    void RecordOperation(const nlohmann::json& operation);
    static std::string IdentityKey(const nlohmann::json& inventory);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace equipment_manager
