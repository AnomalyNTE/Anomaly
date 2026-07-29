#include "anomaly/repository_coordinator.hpp"

#include "anomaly/artifact_crypto.hpp"
#include "anomaly/plugin_manifest.hpp"
#include "anomaly/plugin_package.hpp"
#include "anomaly/plugin_repository_config.hpp"
#include "anomaly/repository_network.hpp"
#include "anomaly/safe_zip.hpp"
#include "anomaly/semver.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace anomaly {
namespace {

constexpr std::uint64_t kMaximumPluginListBytes = 8ULL * 1024ULL * 1024ULL;

struct RepositorySource {
    std::string url;
    std::vector<PluginListEntry> entries;
    bool from_cache{};
};

std::string ReadText(const std::filesystem::path& path, std::uint64_t maximum) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > maximum) return {};
    std::string value(static_cast<std::size_t>(size), '\0');
    input.seekg(0, std::ios::beg);
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    return input || input.eof() ? value : std::string{};
}

bool WriteTextAtomic(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    const std::filesystem::path temporary =
        path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    output.close();
    if (!output || MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

// A stable, collision-resistant cache key so cached lists survive channel edits
// without allowing one channel URL to alias another channel's last-good data.
std::wstring UrlCacheKey(std::string_view url) {
    const auto bytes = std::as_bytes(std::span(url.data(), url.size()));
    const auto hash = Sha256Hex(bytes);
    return std::wstring(hash.begin(), hash.end());
}

std::string EscapeJson(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character < 0x20 ? '?' : static_cast<char>(character)); break;
        }
    }
    return result;
}

bool IsCompatible(const PluginListEntry& entry, const RepositoryCoordinatorOptions& options,
                  const PluginRepositoryConfig& config, std::string& reason) {
    if (entry.api_major != 0 && options.api_major != 0 && entry.api_major != options.api_major) {
        reason = "requires plugin API " + std::to_string(entry.api_major);
        return false;
    }
    if (!entry.games.empty() && !options.game.empty() &&
        std::ranges::find(entry.games, options.game) == entry.games.end()) {
        reason = "not published for this game";
        return false;
    }
    if (!IsPluginRepositoryUriAllowed(
            entry.download_link_install, config.allow_insecure_sources)) {
        reason = "plugin package URL must use HTTPS";
        return false;
    }
    return true;
}

// Keeps the highest-version entry per internal_name across all sources.
std::vector<RepositoryPluginView> BuildCatalog(
    const std::vector<RepositorySource>& sources, const RepositoryCoordinatorOptions& options,
    const PluginRepositoryConfig& config) {
    std::map<std::string, RepositoryPluginView, std::less<>> selected;
    for (const auto& source : sources) {
        for (const auto& entry : source.entries) {
            const auto existing = selected.find(entry.internal_name);
            if (existing != selected.end()) {
                const auto current = ParseSemanticVersion(existing->second.entry.version);
                const auto candidate = ParseSemanticVersion(entry.version);
                if (!(current && candidate &&
                      CompareSemanticVersionPrecedence(*current, *candidate) < 0)) {
                    continue;
                }
            }
            RepositoryPluginView view;
            view.entry = entry;
            view.source_id = source.url;
            view.compatible = IsCompatible(entry, options, config, view.compatibility_reason);
            selected.insert_or_assign(entry.internal_name, std::move(view));
        }
    }
    std::vector<RepositoryPluginView> result;
    result.reserve(selected.size());
    for (auto& [id, plugin] : selected) {
        static_cast<void>(id);
        result.push_back(std::move(plugin));
    }
    return result;
}

// Confirms the extracted package carries a manifest whose id matches the entry.
bool ValidateStaging(const std::filesystem::path& staging, const PluginListEntry& expected,
                     const RepositoryCoordinatorOptions& options, std::string& error) {
    const auto manifest = staging / L"manifest.json";
    std::error_code ec;
    if (!std::filesystem::exists(manifest, ec)) {
        error = "package has no manifest.json at its root";
        return false;
    }
    const auto text = ReadText(manifest, 256ULL * 1024ULL);
    const auto parsed = ParsePluginManifest(text);
    if (!parsed.Ok()) {
        error = parsed.diagnostics.empty() ? "manifest.json is invalid"
                                          : parsed.diagnostics.front().message;
        return false;
    }
    if (parsed.manifest->id != expected.internal_name) {
        error = "manifest id does not match the repository entry";
        return false;
    }
    const auto expected_version = ParseSemanticVersion(expected.version);
    if (!expected_version || parsed.manifest->version.ToString() != expected_version->ToString()) {
        error = "manifest version does not match the repository entry";
        return false;
    }
    if ((expected.api_major != 0 && parsed.manifest->api.major != expected.api_major) ||
        (options.api_major != 0 && parsed.manifest->api.major != options.api_major)) {
        error = "manifest plugin API does not match the repository entry or host";
        return false;
    }
    if (!options.game.empty() &&
        std::ranges::find(parsed.manifest->games, options.game) == parsed.manifest->games.end()) {
        error = "manifest is not published for this game";
        return false;
    }
    const auto entry = OpenConfinedPluginPackageFile(staging, parsed.manifest->entry, true);
    if (!entry.Ok()) {
        error = entry.message.empty() ? "plugin entry is not a confined DLL" : entry.message;
        return false;
    }
    return true;
}

bool ValidateInstalledPlugin(
    const std::filesystem::path& package, std::string_view expected_id, std::string& error) {
    const auto manifest = OpenConfinedPluginPackageFile(package, "manifest.json");
    if (!manifest.Ok()) {
        error = manifest.message.empty() ? "installed plugin manifest is not confined" : manifest.message;
        return false;
    }
    const auto parsed = ParsePluginManifest(ReadText(manifest.path, 256ULL * 1024ULL));
    if (!parsed.Ok()) {
        error = parsed.diagnostics.empty() ? "installed plugin manifest is invalid"
                                           : parsed.diagnostics.front().message;
        return false;
    }
    if (parsed.manifest->id != expected_id) {
        error = "installed plugin identity does not match the repository entry";
        return false;
    }
    return true;
}

bool ValidateConfiguration(const PluginRepositoryConfig& config, std::string& error) {
    for (const auto& repository : config.repositories) {
        if (!repository.enabled || repository.url.empty()) continue;
        if (!IsPluginRepositoryUriAllowed(repository.url, config.allow_insecure_sources)) {
            error = "plugin repository URLs must use HTTPS";
            return false;
        }
    }
    return true;
}

}  // namespace

class RepositoryCoordinator::Impl final {
public:
    explicit Impl(RepositoryCoordinatorOptions options) : options_(std::move(options)) {}
    ~Impl() { Stop(); }

    bool Start() {
        std::scoped_lock start_lock(start_mutex_);
        if (started_.load(std::memory_order_acquire)) return true;
        {
            std::scoped_lock lock(mutex_);
            stopping_ = false;
            refresh_queued_ = false;
            queue_.clear();
            sources_.clear();
            snapshot_ = {};
            config_ = {};
            ++config_generation_;
            next_operation_id_ = 1;
        }

        RecoverInterruptedInstalls();

        const auto configuration_path = ConfigurationPath();
        std::error_code exists_error;
        if (!std::filesystem::exists(configuration_path, exists_error) || exists_error) {
            std::scoped_lock lock(mutex_);
            snapshot_.state = exists_error ? RepositoryCoordinatorState::Unavailable
                                           : RepositoryCoordinatorState::Disabled;
            snapshot_.reason = exists_error ? "plugin repository config cannot be inspected"
                                            : "plugin repository config is missing";
            started_.store(true, std::memory_order_release);
            return true;
        }

        auto loaded = LoadPluginRepositoryConfig(configuration_path);
        if (!loaded.ok) {
            std::scoped_lock lock(mutex_);
            snapshot_.state = RepositoryCoordinatorState::Unavailable;
            snapshot_.reason = loaded.message;
            started_.store(true, std::memory_order_release);
            return true;
        }
        std::string validation_error;
        if (!ValidateConfiguration(loaded.config, validation_error)) {
            std::scoped_lock lock(mutex_);
            snapshot_.state = RepositoryCoordinatorState::Unavailable;
            snapshot_.reason = std::move(validation_error);
            started_.store(true, std::memory_order_release);
            return true;
        }
        config_ = std::move(loaded.config);
        const auto active = ActiveRepositoryUrls();
        {
            std::scoped_lock lock(mutex_);
            snapshot_.configured_sources = active.size();
        }
        if (!config_.enabled || active.empty()) {
            // Started with channels off or empty. The network/worker stay idle
            // until the user enables a channel from Settings (see Configure).
            std::scoped_lock lock(mutex_);
            snapshot_.state = RepositoryCoordinatorState::Disabled;
            snapshot_.reason = config_.enabled ? "no plugin repositories are configured"
                                               : "plugin repositories are disabled by configuration";
            started_.store(true, std::memory_order_release);
            return true;
        }

        LoadCachedSources(options_.automatic_refresh);
        EnsureWorkerStarted();
        started_.store(true, std::memory_order_release);
        if (options_.automatic_refresh) static_cast<void>(QueueRefresh());
        return true;
    }

    void Stop() noexcept {
        std::scoped_lock start_lock(start_mutex_);
        if (!started_.load(std::memory_order_acquire)) return;
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
            for (auto& operation : snapshot_.operations) {
                if (operation.state == RepositoryOperationState::Queued) {
                    operation.state = RepositoryOperationState::Cancelled;
                    operation.message = "repository stopped";
                }
            }
        }
        worker_.request_stop();
        ready_.notify_all();
        if (network_) network_->Stop();
        if (worker_.joinable()) worker_.join();
        network_.reset();
        {
            std::scoped_lock lock(mutex_);
            snapshot_.state = RepositoryCoordinatorState::Stopped;
            snapshot_.reason = "repository stopped";
            queue_.clear();
        }
        started_.store(false, std::memory_order_release);
    }

    RepositoryCoordinatorSnapshot Snapshot() const {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    RepositoryOperationSubmission QueueRefresh() {
        std::scoped_lock lock(mutex_);
        if (!started_.load(std::memory_order_acquire) || stopping_ || !config_.enabled ||
            ActiveRepositoryUrls().empty()) {
            return {false, 0, "plugin repositories are not enabled"};
        }
        if (refresh_queued_) return {false, 0, "repository refresh is already queued"};
        refresh_queued_ = true;
        queue_.push_back({WorkKind::Refresh, 0, {}});
        if (snapshot_.plugins.empty()) {
            snapshot_.state = RepositoryCoordinatorState::Refreshing;
            snapshot_.reason = "refreshing plugin repositories";
        }
        ready_.notify_one();
        return {true, 0, "repository refresh queued"};
    }

    RepositoryOperationSubmission QueueInstall(std::string_view plugin_id, std::string_view version) {
        std::scoped_lock lock(mutex_);
        if (!started_.load(std::memory_order_acquire) || stopping_ || !config_.enabled) {
            return {false, 0, "plugin repositories are not enabled"};
        }
        const auto found = std::ranges::find_if(snapshot_.plugins, [&](const auto& plugin) {
            return plugin.entry.internal_name == plugin_id &&
                (version.empty() || plugin.entry.version == version);
        });
        if (found == snapshot_.plugins.end()) return {false, 0, "plugin is not in the catalog"};
        if (!found->compatible) return {false, 0, found->compatibility_reason};
        const bool pending = std::ranges::any_of(snapshot_.operations, [&](const auto& operation) {
            return operation.plugin_id == plugin_id &&
                operation.state != RepositoryOperationState::Succeeded &&
                operation.state != RepositoryOperationState::Failed &&
                operation.state != RepositoryOperationState::Cancelled;
        });
        if (pending) return {false, 0, "plugin operation is already pending"};

        RepositoryOperationView operation;
        operation.id = next_operation_id_++;
        operation.kind = RepositoryOperationKind::Install;
        operation.plugin_id = found->entry.internal_name;
        operation.version = found->entry.version;
        operation.message = "queued";
        snapshot_.operations.push_back(operation);
        queue_.push_back({WorkKind::Install, operation.id, *found, config_generation_});
        ready_.notify_one();
        return {true, operation.id, "plugin install queued"};
    }

    RepositoryOperationSubmission QueueUninstall(std::string_view plugin_id) {
        std::scoped_lock lock(mutex_);
        if (!started_.load(std::memory_order_acquire) || stopping_ || !config_.enabled) {
            return {false, 0, "plugin repositories are not enabled"};
        }
        const auto found = std::ranges::find_if(snapshot_.plugins, [&](const auto& plugin) {
            return plugin.entry.internal_name == plugin_id;
        });
        if (found == snapshot_.plugins.end()) {
            return {false, 0, "plugin is not in the third-party catalog"};
        }
        const bool pending = std::ranges::any_of(snapshot_.operations, [&](const auto& operation) {
            return operation.plugin_id == plugin_id &&
                operation.state != RepositoryOperationState::Succeeded &&
                operation.state != RepositoryOperationState::Failed &&
                operation.state != RepositoryOperationState::Cancelled;
        });
        if (pending) return {false, 0, "plugin operation is already pending"};

        RepositoryOperationView operation;
        operation.id = next_operation_id_++;
        operation.kind = RepositoryOperationKind::Uninstall;
        operation.plugin_id = found->entry.internal_name;
        operation.version = found->entry.version;
        operation.message = "queued";
        snapshot_.operations.push_back(operation);
        queue_.push_back({WorkKind::Uninstall, operation.id, *found, config_generation_});
        ready_.notify_one();
        return {true, operation.id, "plugin uninstall queued"};
    }

    PluginRepositoryConfig Configuration() const {
        std::scoped_lock lock(mutex_);
        return config_;
    }

    RepositoryOperationSubmission Configure(PluginRepositoryConfig new_config) {
        std::scoped_lock start_lock(start_mutex_);
        if (!started_.load(std::memory_order_acquire)) {
            return {false, 0, "repository coordinator has not started"};
        }
        // Persist first, so the on-disk configuration and in-memory state agree.
        std::string validation_error;
        if (!ValidateConfiguration(new_config, validation_error)) {
            return {false, 0, std::move(validation_error)};
        }
        {
            std::scoped_lock lock(mutex_);
            if (stopping_) return {false, 0, "plugin repositories are stopping"};
        }
        if (!WriteTextAtomic(ConfigurationPath(), SerializePluginRepositoryConfig(new_config))) {
            return {false, 0, "could not write the plugin channel configuration"};
        }

        bool activate = false;
        {
            std::scoped_lock commit_lock(install_commit_mutex_);
            std::scoped_lock lock(mutex_);
            config_ = std::move(new_config);
            ++config_generation_;  // supersedes in-flight refresh and install work
            for (auto& operation : snapshot_.operations) {
                if (operation.state == RepositoryOperationState::Queued ||
                    operation.state == RepositoryOperationState::Downloading ||
                    operation.state == RepositoryOperationState::Installing ||
                    operation.state == RepositoryOperationState::Uninstalling) {
                    operation.state = RepositoryOperationState::Cancelled;
                    operation.message = "plugin channel configuration changed";
                }
            }
            std::erase_if(queue_, [](const Work& work) { return work.kind != WorkKind::Refresh; });
            const auto active = ActiveRepositoryUrls();
            snapshot_.configured_sources = active.size();
            sources_.clear();
            snapshot_.plugins.clear();
            snapshot_.online_sources = 0;
            snapshot_.cached_sources = 0;
            if (!config_.enabled || active.empty()) {
                snapshot_.state = RepositoryCoordinatorState::Disabled;
                snapshot_.reason = config_.enabled
                    ? "no plugin repositories are configured"
                    : "plugin repositories are disabled by configuration";
                return {true, 0, "plugin channels updated"};
            }
            activate = true;
        }

        // Enabled with at least one channel: ensure the worker exists (it may not,
        // if the coordinator first came up disabled) and rebuild the catalog.
        if (activate) {
            LoadCachedSources(true);
            EnsureWorkerStarted();
        }
        static_cast<void>(QueueRefresh());
        return {true, 0, "plugin channels updated"};
    }

private:
    enum class WorkKind : std::uint8_t { Refresh, Install, Uninstall };
    struct Work {
        WorkKind kind{WorkKind::Refresh};
        std::uint64_t operation_id{};
        RepositoryPluginView plugin;
        std::uint64_t config_generation{};
    };

    [[nodiscard]] std::filesystem::path ConfigurationPath() const {
        return options_.configuration_file.is_absolute()
            ? options_.configuration_file
            : options_.runtime_root / options_.configuration_file;
    }
    [[nodiscard]] std::filesystem::path StateDirectory() const {
        return options_.runtime_root / L"state" / L"repository";
    }
    [[nodiscard]] std::filesystem::path CachePath(std::string_view url) const {
        return StateDirectory() / L"plugin-lists" / (UrlCacheKey(url) + L".json");
    }
    [[nodiscard]] std::filesystem::path PluginDirectory() const {
        return options_.plugin_directory.is_absolute()
            ? options_.plugin_directory
            : options_.runtime_root / options_.plugin_directory;
    }
    [[nodiscard]] std::filesystem::path TransactionDirectory() const {
        return PluginDirectory().parent_path() / L".anomaly-plugin-transactions";
    }

    // Enabled, non-empty channel URLs. Callers must either hold mutex_ or run
    // before the worker starts, so config_ cannot change under them.
    [[nodiscard]] std::vector<std::string> ActiveRepositoryUrls() const {
        std::vector<std::string> urls;
        std::set<std::string, std::less<>> seen;
        urls.reserve(config_.repositories.size());
        for (const auto& entry : config_.repositories) {
            if (entry.enabled && !entry.url.empty() &&
                IsPluginRepositoryUriAllowed(entry.url, config_.allow_insecure_sources) &&
                seen.insert(entry.url).second) {
                urls.push_back(entry.url);
            }
        }
        return urls;
    }

    void RecoverInterruptedInstalls() noexcept {
        const auto transactions = TransactionDirectory();
        const auto plugins = PluginDirectory();
        std::error_code ec;
        std::filesystem::directory_iterator iterator(transactions, ec);
        const std::filesystem::directory_iterator end;
        while (!ec && iterator != end) {
            const auto transaction = iterator->path();
            std::error_code operation_error;
            const auto status = iterator->symlink_status(operation_error);
            iterator.increment(ec);
            if (operation_error || !std::filesystem::is_directory(status) ||
                std::filesystem::is_symlink(status)) {
                continue;
            }
            const auto id = transaction.filename();
            const auto target = plugins / id;
            const auto backup = transaction / L"backup";
            const bool target_exists = std::filesystem::exists(target, operation_error);
            if (operation_error) continue;
            const bool backup_exists = std::filesystem::exists(backup, operation_error);
            if (operation_error) continue;
            if (ReadText(transaction / L"operation", 64) == "uninstall") {
                if (!target_exists && backup_exists) {
                    std::filesystem::remove_all(backup, operation_error);
                }
                operation_error.clear();
                const bool backup_remaining = std::filesystem::exists(backup, operation_error);
                if (!operation_error && (target_exists || !backup_remaining)) {
                    std::filesystem::remove_all(transaction, operation_error);
                }
                continue;
            }
            if (!target_exists && backup_exists) {
                std::filesystem::create_directories(plugins, operation_error);
                if (!operation_error) std::filesystem::rename(backup, target, operation_error);
            }
            operation_error.clear();
            const bool recovered_target = std::filesystem::exists(target, operation_error);
            if (!operation_error && (recovered_target || !backup_exists)) {
                operation_error.clear();
                std::filesystem::remove_all(transaction, operation_error);
            }
        }
        ec.clear();
        if (std::filesystem::is_empty(transactions, ec) && !ec) {
            std::filesystem::remove(transactions, ec);
        }
    }

    // Creates the network service and worker on first use. Held under
    // start_mutex_ (never mutex_), so the worker can immediately take mutex_.
    void EnsureWorkerStarted() {
        if (network_ == nullptr) network_ = std::make_unique<RepositoryNetworkService>();
        if (!worker_.joinable()) {
            worker_ = std::jthread([this](std::stop_token token) { Run(token); });
        }
    }

    void LoadCachedSources(bool refresh_pending) {
        std::vector<RepositorySource> cached;
        for (const auto& url : ActiveRepositoryUrls()) {
            const auto text = ReadText(CachePath(url), kMaximumPluginListBytes);
            if (text.empty()) continue;
            auto parsed = ParsePluginList(text);
            if (parsed.Ok() && !parsed.entries.empty()) {
                cached.push_back({url, std::move(parsed.entries), true});
            }
        }
        std::scoped_lock lock(mutex_);
        sources_ = std::move(cached);
        snapshot_.cached_sources = sources_.size();
        snapshot_.plugins = BuildCatalog(sources_, options_, config_);
        if (!snapshot_.plugins.empty()) {
            snapshot_.state = RepositoryCoordinatorState::Degraded;
            snapshot_.reason = refresh_pending ? "using cached plugin lists while refreshing"
                                               : "using cached plugin lists";
        } else if (!refresh_pending) {
            snapshot_.state = RepositoryCoordinatorState::Unavailable;
            snapshot_.reason = "plugin repositories have not been refreshed";
        } else {
            snapshot_.state = RepositoryCoordinatorState::Refreshing;
            snapshot_.reason = "refreshing plugin repositories";
        }
    }

    void Run(std::stop_token token) noexcept {
        while (!token.stop_requested()) {
            Work work;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, token, [&] { return stopping_ || !queue_.empty(); });
                if (token.stop_requested() || stopping_ || queue_.empty()) break;
                work = std::move(queue_.front());
                queue_.pop_front();
                if (work.kind == WorkKind::Refresh) refresh_queued_ = false;
            }
            try {
                if (work.kind == WorkKind::Refresh) RefreshOnline(token);
                else if (work.kind == WorkKind::Install) Install(work, token);
                else Uninstall(work, token);
            } catch (const std::exception& error) {
                if (work.kind != WorkKind::Refresh) {
                    SetOperation(work.operation_id, RepositoryOperationState::Failed, error.what());
                } else {
                    SetRefreshFailure(error.what());
                }
            } catch (...) {
                if (work.kind != WorkKind::Refresh) {
                    SetOperation(work.operation_id, RepositoryOperationState::Failed,
                                 "repository operation failed");
                } else {
                    SetRefreshFailure("repository refresh failed");
                }
            }
        }
    }

    void RefreshOnline(std::stop_token token) {
        // Snapshot the channel list the moment this refresh begins. A concurrent
        // Configure bumps config_generation_; if it changes while we work, our
        // results are stale and must not clobber the newer configuration.
        std::vector<std::string> active;
        std::uint64_t generation{};
        {
            std::scoped_lock lock(mutex_);
            active = ActiveRepositoryUrls();
            generation = config_generation_;
        }

        std::vector<RepositorySource> next;
        std::size_t online{};
        std::size_t cached{};
        std::string last_error;
        const auto downloads = StateDirectory() / L"downloads";

        for (std::size_t index = 0; index < active.size() && !token.stop_requested(); ++index) {
            const auto& url = active[index];
            const auto destination = downloads / (L"list-" + std::to_wstring(index) + L".json");
            const auto downloaded =
                network_->FetchToFile({url, destination, kMaximumPluginListBytes}).get();
            if (downloaded.Ok()) {
                const auto json = ReadText(destination, kMaximumPluginListBytes);
                std::error_code ignored;
                std::filesystem::remove(destination, ignored);
                auto parsed = ParsePluginList(json);
                if (parsed.Ok()) {
                    static_cast<void>(WriteTextAtomic(CachePath(url), json));
                    next.push_back({url, std::move(parsed.entries), false});
                    ++online;
                    continue;
                }
                last_error = parsed.message;
            } else {
                std::error_code ignored;
                std::filesystem::remove(destination, ignored);
                last_error = downloaded.message;
            }
            // Fall back to the last-good cached list for this channel.
            const auto cache_text = ReadText(CachePath(url), kMaximumPluginListBytes);
            if (!cache_text.empty()) {
                auto parsed = ParsePluginList(cache_text);
                if (parsed.Ok() && !parsed.entries.empty()) {
                    next.push_back({url, std::move(parsed.entries), true});
                    ++cached;
                }
            }
        }
        if (token.stop_requested()) return;

        std::scoped_lock lock(mutex_);
        if (generation != config_generation_) return;  // superseded by a newer Configure
        sources_ = std::move(next);
        snapshot_.online_sources = online;
        snapshot_.cached_sources = cached;
        snapshot_.plugins = BuildCatalog(sources_, options_, config_);
        if (online == active.size()) {
            snapshot_.state = RepositoryCoordinatorState::Ready;
            snapshot_.reason = "plugin lists are current";
        } else if (!sources_.empty()) {
            snapshot_.state = RepositoryCoordinatorState::Degraded;
            snapshot_.reason = last_error.empty() ? "one or more repositories are using cache"
                                                  : last_error;
        } else {
            snapshot_.state = RepositoryCoordinatorState::Unavailable;
            snapshot_.reason = last_error.empty() ? "no plugin repository is available" : last_error;
        }
    }

    void Install(const Work& work, std::stop_token token) {
        const auto& entry = work.plugin.entry;
        if (!GenerationCurrent(work.config_generation)) {
            SetOperation(work.operation_id, RepositoryOperationState::Cancelled,
                         "plugin channel configuration changed");
            return;
        }
        const auto transaction = TransactionDirectory() /
            std::filesystem::path(entry.internal_name);
        {
            std::scoped_lock commit_lock(install_commit_mutex_);
            RecoverInterruptedInstalls();
            std::error_code ec;
            if (std::filesystem::exists(transaction, ec) || ec) {
                SetOperation(work.operation_id, RepositoryOperationState::Failed,
                             "a previous plugin install could not be recovered");
                return;
            }
        }
        SetOperation(work.operation_id, RepositoryOperationState::Downloading, "downloading");

        const auto downloads = StateDirectory() / L"downloads";
        const auto archive =
            downloads / (L"plugin-" + std::to_wstring(work.operation_id) + L".zip");
        const std::string& url =
            entry.download_link_install.empty() ? entry.download_link_update
                                                : entry.download_link_install;
        const auto downloaded =
            network_->FetchToFile({url, archive, kMaximumZipArchiveBytes}).get();
        if (!downloaded.Ok() || token.stop_requested() ||
            !GenerationCurrent(work.config_generation)) {
            std::error_code ignored;
            std::filesystem::remove(archive, ignored);
            const bool superseded = !GenerationCurrent(work.config_generation);
            SetOperation(work.operation_id,
                         token.stop_requested() || superseded
                             ? RepositoryOperationState::Cancelled
                             : RepositoryOperationState::Failed,
                         token.stop_requested() ? "repository stopped"
                             : superseded ? "plugin channel configuration changed"
                                          : downloaded.message);
            return;
        }

        SetOperation(work.operation_id, RepositoryOperationState::Installing, "installing",
                     downloaded.bytes);

        std::error_code ec;
        const auto staging = transaction / L"staging";
        std::filesystem::remove_all(transaction, ec);

        std::string message = "installed";
        bool ok = false;
        const auto extracted = ExtractZip(archive, staging);
        if (!extracted.Ok()) {
            message = extracted.message.empty() ? "could not extract package" : extracted.message;
        } else if (!ValidateStaging(staging, entry, options_, message)) {
            // message set by ValidateStaging
        } else {
            std::scoped_lock commit_lock(install_commit_mutex_);
            if (GenerationCurrent(work.config_generation)) {
                ok = InstallFromStaging(staging, entry.internal_name, transaction, message);
                if (ok) {
                    SetOperation(work.operation_id, RepositoryOperationState::Succeeded,
                                 message, downloaded.bytes);
                }
            } else {
                message = "plugin channel configuration changed";
            }
        }

        std::filesystem::remove(archive, ec);
        CleanupTransaction(transaction, entry.internal_name);
        if (ok) return;
        const bool superseded = !GenerationCurrent(work.config_generation);
        SetOperation(work.operation_id,
                     superseded ? RepositoryOperationState::Cancelled
                                : RepositoryOperationState::Failed,
                     std::move(message), downloaded.bytes);
    }

    void Uninstall(const Work& work, std::stop_token token) {
        const auto& entry = work.plugin.entry;
        if (token.stop_requested() || !GenerationCurrent(work.config_generation)) {
            SetOperation(work.operation_id, RepositoryOperationState::Cancelled,
                token.stop_requested() ? "repository stopped"
                                       : "plugin channel configuration changed");
            return;
        }
        SetOperation(work.operation_id, RepositoryOperationState::Uninstalling, "uninstalling");

        std::scoped_lock commit_lock(install_commit_mutex_);
        if (token.stop_requested() || !GenerationCurrent(work.config_generation)) {
            SetOperation(work.operation_id, RepositoryOperationState::Cancelled,
                token.stop_requested() ? "repository stopped"
                                       : "plugin channel configuration changed");
            return;
        }
        RecoverInterruptedInstalls();
        const auto transaction =
            TransactionDirectory() / std::filesystem::path(entry.internal_name);
        std::error_code ec;
        if (std::filesystem::exists(transaction, ec) || ec) {
            SetOperation(work.operation_id, RepositoryOperationState::Failed,
                "a previous plugin operation could not be recovered");
            return;
        }
        const auto target = PluginDirectory() / std::filesystem::path(entry.internal_name);
        if (!std::filesystem::exists(target, ec) || ec) {
            SetOperation(work.operation_id, RepositoryOperationState::Failed,
                ec ? "installed plugin cannot be inspected" : "plugin is not installed");
            return;
        }
        std::string validation_error;
        if (!ValidateInstalledPlugin(target, entry.internal_name, validation_error)) {
            SetOperation(work.operation_id, RepositoryOperationState::Failed,
                std::move(validation_error));
            return;
        }
        if (!WriteTextAtomic(transaction / L"operation", "uninstall")) {
            SetOperation(work.operation_id, RepositoryOperationState::Failed,
                "plugin uninstall transaction could not be prepared");
            return;
        }
        const auto backup = transaction / L"backup";
        std::filesystem::rename(target, backup, ec);
        if (ec) {
            std::error_code ignored;
            std::filesystem::remove_all(transaction, ignored);
            SetOperation(work.operation_id, RepositoryOperationState::Failed,
                "plugin directory could not be removed");
            return;
        }

        std::filesystem::remove_all(backup, ec);
        const std::string message = ec ? "uninstalled; cleanup deferred" : "uninstalled";
        if (!ec) {
            std::filesystem::remove_all(transaction, ec);
        }
        SetOperation(work.operation_id, RepositoryOperationState::Succeeded, message);
    }

    // Atomically replaces plugins/<id> with the staged package, keeping a backup
    // to restore on failure.
    bool InstallFromStaging(const std::filesystem::path& staging, const std::string& id,
                            const std::filesystem::path& transaction, std::string& error) {
        std::error_code ec;
        const auto plugins = PluginDirectory();
        std::filesystem::create_directories(plugins, ec);
        const auto target = plugins / std::filesystem::path(id);
        const auto backup = transaction / L"backup";

        std::filesystem::remove_all(backup, ec);
        const bool had_existing = std::filesystem::exists(target, ec);
        if (had_existing) {
            std::filesystem::rename(target, backup, ec);
            if (ec) {
                error = "cannot replace the installed plugin";
                return false;
            }
        }
        std::filesystem::rename(staging, target, ec);
        if (ec) {
            if (had_existing) {
                std::error_code restore;
                std::filesystem::rename(backup, target, restore);
            }
            error = "cannot place the plugin into the plugins directory";
            return false;
        }
        if (had_existing) {
            std::error_code ignored;
            std::filesystem::remove_all(backup, ignored);
        }
        return true;
    }

    void CleanupTransaction(
        const std::filesystem::path& transaction, const std::string& id) noexcept {
        std::error_code ec;
        const bool target_exists =
            std::filesystem::exists(PluginDirectory() / std::filesystem::path(id), ec);
        if (ec) return;
        const bool backup_exists = std::filesystem::exists(transaction / L"backup", ec);
        if (ec) return;
        if (target_exists || !backup_exists) {
            std::filesystem::remove_all(transaction, ec);
        } else {
            std::filesystem::remove_all(transaction / L"staging", ec);
        }
    }

    [[nodiscard]] bool GenerationCurrent(std::uint64_t generation) const {
        std::scoped_lock lock(mutex_);
        return !stopping_ && generation == config_generation_;
    }

    void SetOperation(std::uint64_t id, RepositoryOperationState state, std::string message,
                      std::uint64_t received = 0) {
        std::scoped_lock lock(mutex_);
        const auto found = std::ranges::find(snapshot_.operations, id, &RepositoryOperationView::id);
        if (found == snapshot_.operations.end()) return;
        if (found->state == RepositoryOperationState::Cancelled &&
            state != RepositoryOperationState::Cancelled) {
            return;
        }
        found->state = state;
        found->message = std::move(message);
        if (received != 0) found->received_bytes = received;
    }

    void SetRefreshFailure(std::string message) {
        std::scoped_lock lock(mutex_);
        snapshot_.state = snapshot_.plugins.empty() ? RepositoryCoordinatorState::Unavailable
                                                     : RepositoryCoordinatorState::Degraded;
        snapshot_.reason = std::move(message);
    }

    RepositoryCoordinatorOptions options_;
    PluginRepositoryConfig config_;
    std::uint64_t config_generation_{};
    std::unique_ptr<RepositoryNetworkService> network_;
    std::vector<RepositorySource> sources_;
    mutable std::mutex start_mutex_;
    mutable std::mutex install_commit_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Work> queue_;
    std::jthread worker_;
    RepositoryCoordinatorSnapshot snapshot_;
    std::uint64_t next_operation_id_{1};
    std::atomic_bool started_{};
    bool stopping_{};
    bool refresh_queued_{};
};

RepositoryCoordinator::RepositoryCoordinator(RepositoryCoordinatorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
RepositoryCoordinator::~RepositoryCoordinator() = default;
bool RepositoryCoordinator::Start() { return impl_->Start(); }
void RepositoryCoordinator::Stop() noexcept { impl_->Stop(); }
RepositoryCoordinatorSnapshot RepositoryCoordinator::Snapshot() const { return impl_->Snapshot(); }
PluginRepositoryConfig RepositoryCoordinator::Configuration() const { return impl_->Configuration(); }
RepositoryOperationSubmission RepositoryCoordinator::Refresh() { return impl_->QueueRefresh(); }
RepositoryOperationSubmission RepositoryCoordinator::InstallPlugin(
    std::string_view plugin_id, std::string_view version) {
    return impl_->QueueInstall(plugin_id, version);
}
RepositoryOperationSubmission RepositoryCoordinator::UninstallPlugin(std::string_view plugin_id) {
    return impl_->QueueUninstall(plugin_id);
}
RepositoryOperationSubmission RepositoryCoordinator::Configure(PluginRepositoryConfig config) {
    return impl_->Configure(std::move(config));
}

std::string_view RepositoryCoordinatorStateName(RepositoryCoordinatorState state) noexcept {
    switch (state) {
    case RepositoryCoordinatorState::Disabled: return "disabled";
    case RepositoryCoordinatorState::Refreshing: return "refreshing";
    case RepositoryCoordinatorState::Ready: return "ready";
    case RepositoryCoordinatorState::Degraded: return "degraded";
    case RepositoryCoordinatorState::Unavailable: return "unavailable";
    case RepositoryCoordinatorState::Stopped: return "stopped";
    }
    return "unavailable";
}

std::string_view RepositoryOperationStateName(RepositoryOperationState state) noexcept {
    switch (state) {
    case RepositoryOperationState::Queued: return "queued";
    case RepositoryOperationState::Downloading: return "downloading";
    case RepositoryOperationState::Installing: return "installing";
    case RepositoryOperationState::Uninstalling: return "uninstalling";
    case RepositoryOperationState::Succeeded: return "succeeded";
    case RepositoryOperationState::Failed: return "failed";
    case RepositoryOperationState::Cancelled: return "cancelled";
    }
    return "failed";
}

std::string SerializeRepositoryCoordinatorSnapshotJson(
    const RepositoryCoordinatorSnapshot& snapshot) {
    const std::string_view state = snapshot.state == RepositoryCoordinatorState::Disabled
        ? std::string_view{"disabled"}
        : snapshot.state == RepositoryCoordinatorState::Unavailable ||
                snapshot.state == RepositoryCoordinatorState::Stopped
            ? std::string_view{"unavailable"}
            : std::string_view{"configured"};
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"state\":\"" << state << "\",\"coordinatorState\":\""
         << RepositoryCoordinatorStateName(snapshot.state) << "\",\"reason\":\""
         << EscapeJson(snapshot.reason) << "\",\"sources\":" << snapshot.configured_sources
         << ",\"online\":" << snapshot.online_sources << ",\"cached\":" << snapshot.cached_sources
         << ",\"plugins\":" << snapshot.plugins.size()
         << ",\"operations\":" << snapshot.operations.size() << '}';
    return json.str();
}

}  // namespace anomaly
