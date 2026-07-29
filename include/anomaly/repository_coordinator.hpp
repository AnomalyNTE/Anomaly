#pragma once

#include "anomaly/plugin_list.hpp"
#include "anomaly/plugin_repository_config.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

enum class RepositoryCoordinatorState : std::uint8_t {
    Disabled,
    Refreshing,
    Ready,
    Degraded,
    Unavailable,
    Stopped,
};

enum class RepositoryOperationKind : std::uint8_t {
    Install,
    Uninstall,
};

enum class RepositoryOperationState : std::uint8_t {
    Queued,
    Downloading,
    Installing,
    Uninstalling,
    Succeeded,
    Failed,
    Cancelled,
};

struct RepositoryPluginView {
    PluginListEntry entry;
    std::string source_id;  // the repository URL this entry came from
    bool compatible{true};
    std::string compatibility_reason;
};

struct RepositoryOperationView {
    std::uint64_t id{};
    RepositoryOperationKind kind{RepositoryOperationKind::Install};
    std::string plugin_id;
    std::string version;
    RepositoryOperationState state{RepositoryOperationState::Queued};
    std::string message;
    std::uint64_t received_bytes{};
    std::uint64_t total_bytes{};
};

struct RepositoryCoordinatorSnapshot {
    RepositoryCoordinatorState state{RepositoryCoordinatorState::Unavailable};
    std::string reason{"not started"};
    std::size_t configured_sources{};
    std::size_t online_sources{};
    std::size_t cached_sources{};
    std::vector<RepositoryPluginView> plugins;
    std::vector<RepositoryOperationView> operations;

    [[nodiscard]] bool BrowseAvailable() const noexcept {
        return state == RepositoryCoordinatorState::Ready ||
            state == RepositoryCoordinatorState::Degraded ||
            state == RepositoryCoordinatorState::Refreshing;
    }
};

struct RepositoryOperationSubmission {
    bool accepted{};
    std::uint64_t operation_id{};
    std::string message;
};

struct RepositoryCoordinatorOptions {
    std::filesystem::path runtime_root;
    std::filesystem::path configuration_file{L"plugin-repositories.json"};
    std::filesystem::path plugin_directory{L"plugins"};
    std::string game;             // host game id (e.g. "nte") for compatibility filtering
    std::uint32_t api_major{};    // host plugin ABI major, for compatibility filtering
    bool automatic_refresh{true};
};

class RepositoryCoordinator final {
public:
    explicit RepositoryCoordinator(RepositoryCoordinatorOptions options);
    ~RepositoryCoordinator();
    RepositoryCoordinator(const RepositoryCoordinator&) = delete;
    RepositoryCoordinator& operator=(const RepositoryCoordinator&) = delete;

    // Start performs only local validation (config load). Online refresh and
    // downloads run on the coordinator/network workers.
    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] RepositoryCoordinatorSnapshot Snapshot() const;
    // Returns the channel configuration currently in effect, for the settings
    // editor to seed its draft.
    [[nodiscard]] PluginRepositoryConfig Configuration() const;
    [[nodiscard]] RepositoryOperationSubmission Refresh();
    [[nodiscard]] RepositoryOperationSubmission InstallPlugin(
        std::string_view plugin_id, std::string_view version = {});
    [[nodiscard]] RepositoryOperationSubmission UninstallPlugin(std::string_view plugin_id);
    // Replaces the channel configuration: persists it to the configuration file
    // and re-refreshes online. Applies without a runtime restart, lazily starting
    // the network/worker if the coordinator first came up with channels disabled.
    [[nodiscard]] RepositoryOperationSubmission Configure(PluginRepositoryConfig config);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view RepositoryCoordinatorStateName(
    RepositoryCoordinatorState state) noexcept;
[[nodiscard]] std::string_view RepositoryOperationStateName(
    RepositoryOperationState state) noexcept;
[[nodiscard]] std::string SerializeRepositoryCoordinatorSnapshotJson(
    const RepositoryCoordinatorSnapshot& snapshot);

}  // namespace anomaly
