#include "anomaly/scoped_platform_services.hpp"

#include "anomaly/hook_manager.hpp"
#include "anomaly/reliable_storage.hpp"
#include "anomaly/sdk/version.h"

#include <Windows.h>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumStorageBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumConfigBytes = 1024U * 1024U;
// Config data is wrapped with schema metadata before it reaches durable storage.
// Keep the file limit large enough for a maximum-sized JSON document plus its
// compact envelope, while still bounding plugin-owned configuration I/O.
constexpr std::size_t kMaximumConfigEnvelopeBytes = kMaximumConfigBytes + 256U;
constexpr std::size_t kMaximumSchemaBytes = 256U * 1024U;
constexpr std::size_t kMaximumCallbackOutputBytes = 1024U * 1024U;

AnomalyStatusV1 Status(const std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

AnomalyStatusV1 InvalidArgument(const char* message = "invalid argument") noexcept {
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, message);
}

bool IsText(const std::string_view value) noexcept {
    return !value.empty() && value.find('\0') == std::string_view::npos;
}

bool IsSafeToken(const std::string_view value) noexcept {
    if (!IsText(value) || value.size() > 128 || value == "." || value == "..") return false;
    for (const char character : value) {
        const bool alpha_numeric = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9');
        if (!alpha_numeric && character != '.' && character != '_' && character != '-') return false;
    }
    return value.find("..") == std::string_view::npos;
}

bool IsSafeConfigSchemaId(const std::string_view value) noexcept {
    return IsSafeToken(value);
}

bool IsSafeRelativePath(const std::string_view value) noexcept {
    if (!IsText(value) || value.size() > 512 || value.front() == '/' || value.front() == '\\' ||
        value.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t begin{};
    while (begin < value.size()) {
        const std::size_t end = value.find_first_of("/\\", begin);
        const std::string_view component = value.substr(
            begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (!IsSafeToken(component)) return false;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return true;
}

std::wstring WideUtf8(const std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required) != required) {
        return {};
    }
    return result;
}

std::span<const std::byte> AsBytes(const AnomalyByteSpanV1 value) noexcept {
    return std::as_bytes(std::span(value.data, value.size));
}

AnomalyStatusV1 CopyBytes(
    const std::span<const std::uint8_t> source,
    const AnomalyMutableByteSpanV1 destination,
    std::size_t* inout_size) noexcept {
    if (inout_size == nullptr) return InvalidArgument("result size is required");
    const std::size_t required = source.size();
    if (destination.data == nullptr) {
        *inout_size = required;
        return Status(ANOMALY_STATUS_V1_OK);
    }
    if (*inout_size < required || destination.size < required) {
        *inout_size = required;
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    if (required != 0) std::memcpy(destination.data, source.data(), required);
    *inout_size = required;
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 CopyString(
    const std::string_view source, char* destination, std::size_t* inout_size) noexcept {
    if (inout_size == nullptr) return InvalidArgument("result size is required");
    const std::size_t required = source.size() + 1U;
    if (destination == nullptr) {
        *inout_size = required;
        return Status(ANOMALY_STATUS_V1_OK);
    }
    if (*inout_size < required) {
        *inout_size = required;
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    std::memcpy(destination, source.data(), source.size());
    destination[source.size()] = '\0';
    *inout_size = required;
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 StorageStatus(const StorageResult& result) noexcept {
    if (result.ok()) return Status(ANOMALY_STATUS_V1_OK);
    if (result.win32_error == ERROR_FILE_NOT_FOUND || result.win32_error == ERROR_PATH_NOT_FOUND) {
        return Status(ANOMALY_STATUS_V1_NOT_FOUND, "storage entry was not found");
    }
    if (result.error == StorageError::InvalidPath ||
        result.error == StorageError::PathOutsideRoot ||
        result.error == StorageError::ReparsePoint) {
        return InvalidArgument("storage path is outside the service root");
    }
    if (result.error == StorageError::SizeLimitExceeded) {
        return Status(ANOMALY_STATUS_V1_FAILED, "storage entry exceeds the service limit");
    }
    return Status(ANOMALY_STATUS_V1_FAILED, "storage operation failed");
}

bool SameOwner(
    const ScopedPluginServiceOwner& left,
    const ScopedPluginServiceOwner& right) noexcept {
    return left.scope != nullptr && right.scope != nullptr &&
        left.scope->Owner() == right.scope->Owner() &&
        left.scope->Generation() == right.scope->Generation();
}

std::string OwnerKey(const ScopedPluginServiceOwner& owner) {
    return owner.scope->Owner() + '#' + std::to_string(owner.scope->Generation());
}

bool IsExecutableAddress(const std::uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION information{};
    if (address == 0 || VirtualQuery(reinterpret_cast<const void*>(address), &information,
                        sizeof(information)) != sizeof(information) ||
        information.State != MEM_COMMIT || (information.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = information.Protect & 0xffU;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool IsPatchableRange(const std::uintptr_t address, const std::size_t size) noexcept {
    if (address == 0 || size == 0 || size > kMaximumConfigBytes ||
        address > (std::numeric_limits<std::uintptr_t>::max)() - size) {
        return false;
    }
    std::uintptr_t current = address;
    const std::uintptr_t end = address + size;
    while (current < end) {
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &information,
                sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT || (information.Protect & PAGE_GUARD) != 0 ||
            (information.Protect & 0xffU) == PAGE_NOACCESS) {
            return false;
        }
        const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(information.BaseAddress) +
            information.RegionSize;
        if (region_end <= current) return false;
        current = (std::min)(region_end, end);
    }
    return true;
}

bool RangesOverlap(
    const std::uintptr_t left_address, const std::size_t left_size,
    const std::uintptr_t right_address, const std::size_t right_size) noexcept {
    const std::uintptr_t left_end = left_address + left_size;
    const std::uintptr_t right_end = right_address + right_size;
    return left_address < right_end && right_address < left_end;
}

std::chrono::milliseconds RemainingUntil(
    const std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::milliseconds::max();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds::zero();
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

std::chrono::steady_clock::time_point DefaultHookRemovalDeadline() noexcept {
    return std::chrono::steady_clock::now() + std::chrono::seconds(1);
}

}  // namespace

class ScopedPlatformServices::Impl final {
public:
    struct ResourceState {
        ScopedPluginServiceOwner owner;
        std::uint64_t token{};
        std::atomic_bool active{true};
    };

    struct ConfigState final : ResourceState {
        std::string id;
        std::uint32_t version{};
        std::unique_ptr<nlohmann::json_schema::json_validator> validator;
    };

    struct SelfTestState final : ResourceState {
        std::string id;
        AnomalyDiagnosticSelfTestV1 callback{};
        void* callback_user{};
    };

    struct TaskState final : ResourceState {
        AnomalyTaskCallbackV1 callback{};
        void* callback_user{};
    };

    struct CommandState final : ResourceState {
        std::string name;
        std::string description;
        AnomalyCommandCallbackV1 callback{};
        void* callback_user{};
    };

    struct NotificationState final : ResourceState {
        AnomalyNotificationSeverityV1 severity{};
        std::string title;
        std::string body;
        std::uint32_t timeout_milliseconds{};
    };

    struct HookState final : ResourceState {
        std::uintptr_t target{};
        std::mutex removal_mutex;
        bool installed{};
    };

    struct PatchState final : ResourceState {
        std::uintptr_t address{};
        std::vector<std::uint8_t> original;
        bool applied{};
    };

    struct HookLeaseState final {
        ScopedPluginServiceOwner owner;
        PluginScope::CallbackLease hook_lease;
        PluginScope::CallbackLease plugin_lease;
    };

    struct CallbackStats final {
        std::uint64_t calls{};
        std::uint64_t faults{};
        std::uint64_t slow_calls{};
    };

    struct WorkItem final {
        std::chrono::steady_clock::time_point due;
        std::uint64_t sequence{};
        std::string owner_key;
        std::uint64_t resource_token{};
        std::function<void()> callback;
    };

    Impl(CoreMemoryServices memory_services, std::shared_ptr<ResourceLedger> ledger,
         std::unique_ptr<HookBackend> hook_backend)
        : memory_services_(NormalizeCoreMemoryServices(std::move(memory_services))),
          ledger_(std::move(ledger)),
          hooks_(std::make_unique<HookManager>(std::move(hook_backend), ledger_)),
          started_(std::chrono::steady_clock::now()),
          worker_([this](std::stop_token stop_token) { RunWorker(stop_token); }) {}

    ~Impl() {
        worker_.request_stop();
        work_condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        RevokeAllResources();
    }

    void Enqueue(
        std::chrono::milliseconds delay,
        std::string owner_key,
        const std::uint64_t resource_token,
        std::function<void()> callback) {
        std::scoped_lock lock(work_mutex_);
        work_.push_back({
            std::chrono::steady_clock::now() + delay,
            ++next_work_sequence_,
            std::move(owner_key),
            resource_token,
            std::move(callback)});
        work_condition_.notify_all();
    }

    void CancelQueuedWork(
        const ScopedPluginServiceOwner& owner, const std::uint64_t resource_token) noexcept {
        if (owner.scope == nullptr || resource_token == 0) return;
        std::scoped_lock lock(work_mutex_);
        const std::string key = OwnerKey(owner);
        std::erase_if(work_, [&](const WorkItem& item) {
            return item.owner_key == key && item.resource_token == resource_token;
        });
        work_condition_.notify_all();
    }

    void RecordCallback(const ScopedPluginServiceOwner& owner, bool fault,
                        std::chrono::steady_clock::duration elapsed) {
        std::scoped_lock lock(stats_mutex_);
        CallbackStats& stats = callback_stats_[OwnerKey(owner)];
        ++stats.calls;
        if (fault) ++stats.faults;
        if (elapsed >= std::chrono::milliseconds(2)) ++stats.slow_calls;
    }

    CallbackStats StatsFor(const ScopedPluginServiceOwner& owner) const {
        std::scoped_lock lock(stats_mutex_);
        const auto found = callback_stats_.find(OwnerKey(owner));
        return found == callback_stats_.end() ? CallbackStats{} : found->second;
    }

    [[nodiscard]] ScopedPlatformDiagnosticsView Snapshot(
        const ScopedPluginServiceOwner& owner) const noexcept {
        ScopedPlatformDiagnosticsView result;
        if (owner.scope == nullptr) return result;
        try {
            result.ledger_resources = owner.scope->Resources().size();
            const auto count = [&](const auto& states) {
                return static_cast<std::size_t>(std::count_if(
                    states.begin(), states.end(), [&](const auto& entry) {
                        return entry.second &&
                            entry.second->active.load(std::memory_order_acquire) &&
                            SameOwner(entry.second->owner, owner);
                    }));
            };
            {
                std::scoped_lock lock(mutex_);
                result.resources = {
                    count(configs_), count(self_tests_), count(tasks_),
                    count(commands_), count(notifications_), count(hooks_by_token_), count(patches_)};
            }
            {
                std::scoped_lock lock(work_mutex_);
                const std::string key = OwnerKey(owner);
                result.queued_tasks = static_cast<std::size_t>(std::count_if(
                    work_.begin(), work_.end(), [&](const WorkItem& item) {
                        return item.owner_key == key;
                    }));
            }
            const CallbackStats statistics = StatsFor(owner);
            result.callback_calls = statistics.calls;
            result.callback_faults = statistics.faults;
            result.slow_callbacks = statistics.slow_calls;
        } catch (...) {
        }
        return result;
    }

    CoreMemoryServices memory_services_;
    std::shared_ptr<ResourceLedger> ledger_;
    std::unique_ptr<HookManager> hooks_;
    std::chrono::steady_clock::time_point started_;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<ConfigState>> configs_;
    std::unordered_map<std::uint64_t, std::shared_ptr<SelfTestState>> self_tests_;
    std::unordered_map<std::uint64_t, std::shared_ptr<TaskState>> tasks_;
    std::unordered_map<std::uint64_t, std::shared_ptr<CommandState>> commands_;
    std::unordered_map<std::uint64_t, std::shared_ptr<NotificationState>> notifications_;
    std::unordered_map<std::uint64_t, std::shared_ptr<HookState>> hooks_by_token_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PatchState>> patches_;
    std::unordered_map<std::uint64_t, HookLeaseState> hook_leases_;
    std::uint64_t next_hook_lease_{1};

    mutable std::mutex stats_mutex_;
    std::unordered_map<std::string, CallbackStats> callback_stats_;

private:
    void RunWorker(const std::stop_token stop_token) {
        std::unique_lock lock(work_mutex_);
        while (!stop_token.stop_requested()) {
            if (work_.empty()) {
                work_condition_.wait(lock, [&] {
                    return stop_token.stop_requested() || !work_.empty();
                });
                continue;
            }
            const auto next = std::min_element(
                work_.begin(), work_.end(), [](const WorkItem& left, const WorkItem& right) {
                    return left.due == right.due ? left.sequence < right.sequence : left.due < right.due;
                });
            const auto now = std::chrono::steady_clock::now();
            if (next->due > now) {
                // New work can have an earlier deadline than the item selected above.
                // Re-evaluate the queue after every notification instead of sleeping until
                // the stale deadline.
                work_condition_.wait_until(lock, next->due);
                continue;
            }
            std::function<void()> callback = std::move(next->callback);
            work_.erase(next);
            lock.unlock();
            try {
                if (callback) callback();
            } catch (...) {
            }
            lock.lock();
        }
        work_.clear();
    }

    void RevokeAllResources() noexcept {
        std::vector<std::pair<std::shared_ptr<PluginScope>, std::uint64_t>> resources;
        {
            std::scoped_lock lock(mutex_);
            const auto collect = [&resources](const auto& map) {
                for (const auto& [token, state] : map) {
                    if (state && state->owner.scope) resources.emplace_back(state->owner.scope, token);
                }
            };
            collect(configs_);
            collect(self_tests_);
            collect(tasks_);
            collect(commands_);
            collect(notifications_);
            collect(hooks_by_token_);
            collect(patches_);
        }
        for (const auto& [scope, token] : resources) {
            if (scope) static_cast<void>(scope->Release(token));
        }
    }

    mutable std::mutex work_mutex_;
    std::condition_variable work_condition_;
    std::vector<WorkItem> work_;
    std::uint64_t next_work_sequence_{};
    std::jthread worker_;
};

namespace {

template <typename State>
bool Owns(const std::shared_ptr<State>& state, const ScopedPluginServiceOwner& owner,
          const AnomalyGenerationHandleV1 handle) noexcept {
    return state && state->active.load(std::memory_order_acquire) &&
        handle.id == state->token && owner.scope != nullptr &&
        state->owner.scope != nullptr && SameOwner(state->owner, owner) &&
        handle.generation == owner.scope->Generation();
}

template <typename State>
void RemoveState(
    std::unordered_map<std::uint64_t, std::shared_ptr<State>>& states,
    const std::shared_ptr<State>& state) noexcept {
    if (!state) return;
    state->active.store(false, std::memory_order_release);
    states.erase(state->token);
}

std::wstring StoragePath(const std::string_view relative_path) {
    std::wstring result = WideUtf8(relative_path);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
}

std::wstring ConfigPath(const std::string_view id) {
    return L"config-" + WideUtf8(id) + L".json";
}

AnomalyStatusV1 EnsureStateDirectory(const ScopedPluginServiceOwner& owner) noexcept {
    if (owner.state_directory.empty()) return InvalidArgument("plugin state root is unavailable");
    try {
        std::error_code error;
        std::filesystem::create_directories(owner.state_directory, error);
        if (error || !std::filesystem::is_directory(owner.state_directory, error) || error) {
            return Status(ANOMALY_STATUS_V1_FAILED, "plugin state root is unavailable");
        }
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "plugin state root is unavailable");
    }
}

AnomalyStatusV1 EnsureConfigurationDirectory(const ScopedPluginServiceOwner& owner) noexcept {
    if (owner.configuration_directory.empty()) {
        return InvalidArgument("plugin configuration root is unavailable");
    }
    try {
        std::error_code error;
        std::filesystem::create_directories(owner.configuration_directory, error);
        if (error || !std::filesystem::is_directory(owner.configuration_directory, error) || error) {
            return Status(ANOMALY_STATUS_V1_FAILED, "plugin configuration root is unavailable");
        }
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "plugin configuration root is unavailable");
    }
}

std::shared_ptr<ScopedPlatformServices::Impl::ConfigState> FindConfig(
    ScopedPlatformServices::Impl& impl,
    const ScopedPluginServiceOwner& owner,
    const std::string_view id) {
    std::scoped_lock lock(impl.mutex_);
    const auto found = std::find_if(
        impl.configs_.begin(), impl.configs_.end(), [&](const auto& entry) {
            return entry.second && entry.second->active.load(std::memory_order_acquire) &&
                SameOwner(entry.second->owner, owner) && entry.second->id == id;
        });
    return found == impl.configs_.end() ? nullptr : found->second;
}

AnomalyStatusV1 ValidateConfigDocument(
    const ScopedPlatformServices::Impl::ConfigState& config,
    const AnomalyByteSpanV1 document) noexcept {
    if (document.data == nullptr || document.size == 0 || document.size > kMaximumConfigBytes) {
        return InvalidArgument("configuration document is invalid");
    }
    try {
        if (!config.validator) return Status(ANOMALY_STATUS_V1_FAILED, "configuration schema is unavailable");
        const Json value = Json::parse(document.data, document.data + document.size);
        config.validator->validate(value);
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "configuration does not satisfy its schema");
    }
}

AnomalyStatusV1 ReadConfigEnvelope(
    const ScopedPluginServiceOwner& owner,
    const ScopedPlatformServices::Impl::ConfigState& config,
    std::uint32_t* version,
    std::vector<std::uint8_t>& document) noexcept {
    if (version == nullptr) return InvalidArgument();
    const AnomalyStatusV1 configuration_root = EnsureConfigurationDirectory(owner);
    if (configuration_root.code != ANOMALY_STATUS_V1_OK) return configuration_root;
    ReliableStorage storage(owner.configuration_directory.wstring());
    if (!storage.InitializationResult()) {
        return Status(ANOMALY_STATUS_V1_FAILED, "configuration storage unavailable");
    }
    const StorageReadResult read = storage.Read(ConfigPath(config.id), kMaximumConfigEnvelopeBytes);
    const AnomalyStatusV1 storage_status = StorageStatus(read.result);
    if (storage_status.code != ANOMALY_STATUS_V1_OK) return storage_status;
    try {
        const std::string envelope(
            reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
        const Json parsed = Json::parse(envelope);
        if (!parsed.is_object() || !parsed.contains("schemaVersion") || !parsed.contains("data") ||
            !parsed["schemaVersion"].is_number_unsigned()) {
            return Status(ANOMALY_STATUS_V1_FAILED, "stored configuration envelope is invalid");
        }
        const std::uint64_t stored_version = parsed["schemaVersion"].get<std::uint64_t>();
        if (stored_version == 0 || stored_version > UINT32_MAX) {
            return Status(ANOMALY_STATUS_V1_FAILED, "stored configuration version is invalid");
        }
        const std::string data = parsed["data"].dump();
        if (data.size() > kMaximumConfigBytes) {
            return Status(ANOMALY_STATUS_V1_FAILED, "stored configuration is too large");
        }
        *version = static_cast<std::uint32_t>(stored_version);
        document.assign(data.begin(), data.end());
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "stored configuration is not valid JSON");
    }
}

void RemoveConfig(ScopedPlatformServices::Impl& impl,
                  const std::shared_ptr<ScopedPlatformServices::Impl::ConfigState>& state) noexcept {
    std::scoped_lock lock(impl.mutex_);
    RemoveState(impl.configs_, state);
}

void RemoveSelfTest(ScopedPlatformServices::Impl& impl,
                    const std::shared_ptr<ScopedPlatformServices::Impl::SelfTestState>& state) noexcept {
    std::scoped_lock lock(impl.mutex_);
    RemoveState(impl.self_tests_, state);
}

void RemoveTask(ScopedPlatformServices::Impl& impl,
                const std::shared_ptr<ScopedPlatformServices::Impl::TaskState>& state) noexcept {
    if (state) impl.CancelQueuedWork(state->owner, state->token);
    std::scoped_lock lock(impl.mutex_);
    RemoveState(impl.tasks_, state);
}

void RemoveCommand(ScopedPlatformServices::Impl& impl,
                   const std::shared_ptr<ScopedPlatformServices::Impl::CommandState>& state) noexcept {
    std::scoped_lock lock(impl.mutex_);
    RemoveState(impl.commands_, state);
}

void RemoveNotification(
    ScopedPlatformServices::Impl& impl,
    const std::shared_ptr<ScopedPlatformServices::Impl::NotificationState>& state) noexcept {
    std::scoped_lock lock(impl.mutex_);
    RemoveState(impl.notifications_, state);
}

void RemovePatch(ScopedPlatformServices::Impl& impl,
                 const std::shared_ptr<ScopedPlatformServices::Impl::PatchState>& state) noexcept {
    if (!state) return;
    state->active.store(false, std::memory_order_release);
    if (state->applied && impl.memory_services_.memory) {
        static_cast<void>(impl.memory_services_.memory->PatchMemory(
            state->address, state->original.data(), state->original.size()));
        state->applied = false;
    }
    std::scoped_lock lock(impl.mutex_);
    impl.patches_.erase(state->token);
}

bool RemoveHook(
    ScopedPlatformServices::Impl& impl,
    const std::shared_ptr<ScopedPlatformServices::Impl::HookState>& state,
    const std::chrono::steady_clock::time_point deadline) noexcept {
    if (!state) return true;
    std::scoped_lock removal_lock(state->removal_mutex);
    if (!state->active.load(std::memory_order_acquire)) return true;
    if (state->installed) {
        if (!state->owner.scope || !impl.hooks_) return false;
        if (!impl.hooks_->Remove(
                state->owner.scope->Owner(), state->owner.scope->Generation(),
                reinterpret_cast<void*>(state->target), RemainingUntil(deadline))) {
            return false;
        }
        state->installed = false;
    }
    state->active.store(false, std::memory_order_release);
    std::scoped_lock lock(impl.mutex_);
    impl.hooks_by_token_.erase(state->token);
    return true;
}

}  // namespace

ScopedPlatformServices::ScopedPlatformServices(
    CoreMemoryServices memory_services, std::shared_ptr<ResourceLedger> ledger)
    : ScopedPlatformServices(
          std::move(memory_services), std::move(ledger), CreateMinHookBackend()) {}

ScopedPlatformServices::ScopedPlatformServices(
    CoreMemoryServices memory_services, std::shared_ptr<ResourceLedger> ledger,
    std::unique_ptr<HookBackend> hook_backend)
    : impl_(std::make_unique<Impl>(
          std::move(memory_services), std::move(ledger), std::move(hook_backend))) {}

ScopedPlatformServices::~ScopedPlatformServices() = default;

AnomalyStatusV1 ScopedPlatformServices::RegisterConfigSchema(
    const ScopedPluginServiceOwner& owner,
    const std::string_view schema_id,
    const std::uint32_t schema_version,
    const AnomalyByteSpanV1 schema_json,
    AnomalyGenerationHandleV1* handle) noexcept {
    if (owner.scope == nullptr || handle == nullptr || !IsSafeConfigSchemaId(schema_id) ||
        schema_version == 0 || schema_json.data == nullptr || schema_json.size == 0 ||
        schema_json.size > kMaximumSchemaBytes) {
        return InvalidArgument("configuration schema is invalid");
    }
    std::shared_ptr<Impl::ConfigState> state;
    try {
        const std::string schema(
            reinterpret_cast<const char*>(schema_json.data), schema_json.size);
        const Json parsed = Json::parse(schema);
        if (!parsed.is_object()) return InvalidArgument("configuration schema must be an object");
        state = std::make_shared<Impl::ConfigState>();
        state->owner = owner;
        state->id = std::string(schema_id);
        state->version = schema_version;
        state->validator = std::make_unique<nlohmann::json_schema::json_validator>(parsed);
        bool inserted{};
        {
            std::scoped_lock lock(impl_->mutex_);
            const bool duplicate = std::any_of(
                impl_->configs_.begin(), impl_->configs_.end(), [&](const auto& entry) {
                    return entry.second && entry.second->active.load(std::memory_order_acquire) &&
                        SameOwner(entry.second->owner, owner) && entry.second->id == schema_id;
                });
            if (duplicate) {
                return Status(ANOMALY_STATUS_V1_CONFLICT, "configuration schema is already registered");
            }
            state->token = owner.scope->Register(
                PluginResourceKind::Config, "config.schema:" + state->id,
                [this, state] { RemoveConfig(*impl_, state); });
            if (state->token == 0) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
            }
            inserted = impl_->configs_.emplace(state->token, state).second;
        }
        if (!inserted) {
            static_cast<void>(owner.scope->Release(state->token));
            return Status(ANOMALY_STATUS_V1_FAILED, "configuration schema registration failed");
        }
        *handle = {state->token, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        if (state && state->token != 0) static_cast<void>(owner.scope->Release(state->token));
        return Status(ANOMALY_STATUS_V1_FAILED, "configuration schema is not valid");
    }
}

AnomalyStatusV1 ScopedPlatformServices::UnregisterConfigSchema(
    const ScopedPluginServiceOwner& owner, const AnomalyGenerationHandleV1 handle) noexcept {
    std::shared_ptr<Impl::ConfigState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->configs_.find(handle.id);
        if (found != impl_->configs_.end()) state = found->second;
    }
    if (!Owns(state, owner, handle)) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "configuration schema handle is stale");
    return owner.scope->Release(handle.id)
        ? Status(ANOMALY_STATUS_V1_OK)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, "configuration schema handle is stale");
}

AnomalyStatusV1 ScopedPlatformServices::ReadConfig(
    const ScopedPluginServiceOwner& owner,
    const std::string_view schema_id,
    std::uint32_t* schema_version,
    const AnomalyMutableByteSpanV1 destination,
    std::size_t* inout_size) noexcept {
    if (owner.scope == nullptr || !IsSafeConfigSchemaId(schema_id)) return InvalidArgument();
    const auto config = FindConfig(*impl_, owner, schema_id);
    if (!config) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "configuration schema is not registered");
    std::vector<std::uint8_t> document;
    const AnomalyStatusV1 status = ReadConfigEnvelope(owner, *config, schema_version, document);
    if (status.code != ANOMALY_STATUS_V1_OK) return status;
    return CopyBytes(document, destination, inout_size);
}

AnomalyStatusV1 ScopedPlatformServices::WriteConfig(
    const ScopedPluginServiceOwner& owner,
    const std::string_view schema_id,
    const std::uint32_t schema_version,
    const AnomalyByteSpanV1 document) noexcept {
    if (owner.scope == nullptr || !IsSafeConfigSchemaId(schema_id) ||
        owner.configuration_directory.empty()) {
        return InvalidArgument();
    }
    const auto config = FindConfig(*impl_, owner, schema_id);
    if (!config) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "configuration schema is not registered");
    const AnomalyStatusV1 configuration_root = EnsureConfigurationDirectory(owner);
    if (configuration_root.code != ANOMALY_STATUS_V1_OK) return configuration_root;
    if (config->version != schema_version) {
        return Status(ANOMALY_STATUS_V1_CONFLICT, "configuration schema version is not registered");
    }
    const AnomalyStatusV1 validation = ValidateConfigDocument(*config, document);
    if (validation.code != ANOMALY_STATUS_V1_OK) return validation;
    try {
        const Json value = Json::parse(document.data, document.data + document.size);
        if (value.dump().size() > kMaximumConfigBytes) {
            return Status(ANOMALY_STATUS_V1_FAILED, "canonical configuration is too large");
        }
        Json envelope;
        envelope["schemaVersion"] = schema_version;
        envelope["data"] = value;
        const std::string serialized = envelope.dump();
        if (serialized.size() > kMaximumConfigEnvelopeBytes) {
            return Status(ANOMALY_STATUS_V1_FAILED, "configuration envelope is too large");
        }
        ReliableStorage storage(owner.configuration_directory.wstring());
        if (!storage.InitializationResult()) {
            return Status(ANOMALY_STATUS_V1_FAILED, "configuration storage unavailable");
        }
        const StorageResult written = storage.WriteAtomic(
            ConfigPath(config->id), std::as_bytes(std::span(serialized.data(), serialized.size())));
        return StorageStatus(written);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "configuration write failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::MigrateConfig(
    const ScopedPluginServiceOwner& owner,
    const std::string_view schema_id,
    const AnomalyConfigMigrationV1 migration,
    void* migration_user) noexcept {
    if (owner.scope == nullptr || !IsSafeConfigSchemaId(schema_id) || migration == nullptr) {
        return InvalidArgument();
    }
    const auto config = FindConfig(*impl_, owner, schema_id);
    if (!config) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "configuration schema is not registered");
    std::uint32_t stored_version{};
    std::vector<std::uint8_t> source;
    const AnomalyStatusV1 read = ReadConfigEnvelope(owner, *config, &stored_version, source);
    if (read.code != ANOMALY_STATUS_V1_OK) return read;
    if (stored_version == config->version) return Status(ANOMALY_STATUS_V1_OK);
    std::size_t required{};
    AnomalyStatusV1 result{};
    try {
        result = migration(migration_user, stored_version, {source.data(), source.size()}, {nullptr, 0}, &required);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "configuration migration raised an exception");
    }
    if ((result.code != ANOMALY_STATUS_V1_OK && result.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) ||
        required == 0 || required > kMaximumCallbackOutputBytes) {
        return Status(ANOMALY_STATUS_V1_FAILED, "configuration migration did not provide an output size");
    }
    std::vector<std::uint8_t> destination(required);
    try {
        result = migration(
            migration_user, stored_version, {source.data(), source.size()},
            {destination.data(), destination.size()}, &required);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "configuration migration raised an exception");
    }
    if (result.code != ANOMALY_STATUS_V1_OK || required > destination.size()) {
        return Status(ANOMALY_STATUS_V1_FAILED, "configuration migration failed");
    }
    destination.resize(required);
    return WriteConfig(owner, schema_id, config->version, {destination.data(), destination.size()});
}

AnomalyStatusV1 ScopedPlatformServices::ReadStorage(
    const ScopedPluginServiceOwner& owner,
    const std::string_view relative_path,
    const AnomalyMutableByteSpanV1 destination,
    std::size_t* inout_size) noexcept {
    if (owner.state_directory.empty() || !IsSafeRelativePath(relative_path)) return InvalidArgument();
    try {
        const AnomalyStatusV1 state_root = EnsureStateDirectory(owner);
        if (state_root.code != ANOMALY_STATUS_V1_OK) return state_root;
        ReliableStorage storage(owner.state_directory.wstring());
        if (!storage.InitializationResult()) return Status(ANOMALY_STATUS_V1_FAILED, "state storage unavailable");
        const StorageReadResult read = storage.Read(StoragePath(relative_path), kMaximumStorageBytes);
        const AnomalyStatusV1 status = StorageStatus(read.result);
        if (status.code != ANOMALY_STATUS_V1_OK) return status;
        return CopyBytes(
            std::span(reinterpret_cast<const std::uint8_t*>(read.bytes.data()), read.bytes.size()),
            destination, inout_size);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "storage read failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::WriteStorage(
    const ScopedPluginServiceOwner& owner,
    const std::string_view relative_path,
    const AnomalyByteSpanV1 source) noexcept {
    if (owner.state_directory.empty() || !IsSafeRelativePath(relative_path) ||
        (source.data == nullptr && source.size != 0) || source.size > kMaximumStorageBytes) {
        return InvalidArgument();
    }
    try {
        const AnomalyStatusV1 state_root = EnsureStateDirectory(owner);
        if (state_root.code != ANOMALY_STATUS_V1_OK) return state_root;
        ReliableStorage storage(owner.state_directory.wstring());
        if (!storage.InitializationResult()) return Status(ANOMALY_STATUS_V1_FAILED, "state storage unavailable");
        return StorageStatus(storage.WriteAtomic(StoragePath(relative_path), AsBytes(source)));
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "storage write failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::RemoveStorage(
    const ScopedPluginServiceOwner& owner, const std::string_view relative_path) noexcept {
    if (owner.state_directory.empty() || !IsSafeRelativePath(relative_path)) return InvalidArgument();
    try {
        const AnomalyStatusV1 state_root = EnsureStateDirectory(owner);
        if (state_root.code != ANOMALY_STATUS_V1_OK) return state_root;
        ReliableStorage storage(owner.state_directory.wstring());
        if (!storage.InitializationResult()) return Status(ANOMALY_STATUS_V1_FAILED, "state storage unavailable");
        return StorageStatus(storage.Delete(StoragePath(relative_path)));
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "storage delete failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::RuntimeInfo(
    const ScopedPluginServiceOwner& owner, AnomalyRuntimeInfoV1* snapshot) noexcept {
    if (owner.scope == nullptr || snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
        return InvalidArgument();
    }
    snapshot->runtime_version_major = ANOMALY_SDK_VERSION_MAJOR;
    snapshot->runtime_version_minor = ANOMALY_SDK_VERSION_MINOR;
    snapshot->runtime_version_patch = ANOMALY_SDK_VERSION_PATCH;
    snapshot->process_id = GetCurrentProcessId();
    snapshot->thread_id = GetCurrentThreadId();
    snapshot->uptime_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - impl_->started_).count());
    snapshot->plugin_generation = owner.scope->Generation();
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ScopedPlatformServices::RuntimeVersion(
    char* destination, std::size_t* inout_size) noexcept {
    return CopyString(ANOMALY_SDK_VERSION_STRING, destination, inout_size);
}

AnomalyStatusV1 ScopedPlatformServices::RegisterSelfTest(
    const ScopedPluginServiceOwner& owner,
    const std::string_view id,
    const AnomalyDiagnosticSelfTestV1 callback,
    void* callback_user,
    AnomalyGenerationHandleV1* handle) noexcept {
    if (owner.scope == nullptr || handle == nullptr || callback == nullptr || !IsSafeToken(id)) {
        return InvalidArgument();
    }
    try {
        auto state = std::make_shared<Impl::SelfTestState>();
        state->owner = owner;
        state->id = std::string(id);
        state->callback = callback;
        state->callback_user = callback_user;
        state->token = owner.scope->Register(
            PluginResourceKind::Diagnostics, "diagnostics.self-test:" + state->id,
            [this, state] { RemoveSelfTest(*impl_, state); });
        if (state->token == 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
        bool duplicate{};
        {
            std::scoped_lock lock(impl_->mutex_);
            duplicate = std::any_of(
                impl_->self_tests_.begin(), impl_->self_tests_.end(), [&](const auto& entry) {
                    return entry.second && entry.second->active.load(std::memory_order_acquire) &&
                        SameOwner(entry.second->owner, owner) && entry.second->id == id;
                });
            if (!duplicate) impl_->self_tests_.emplace(state->token, state);
        }
        if (duplicate) {
            static_cast<void>(owner.scope->Release(state->token));
            return Status(ANOMALY_STATUS_V1_CONFLICT, "self-test id is already registered");
        }
        *handle = {state->token, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "self-test registration failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::UnregisterSelfTest(
    const ScopedPluginServiceOwner& owner, const AnomalyGenerationHandleV1 handle) noexcept {
    std::shared_ptr<Impl::SelfTestState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->self_tests_.find(handle.id);
        if (found != impl_->self_tests_.end()) state = found->second;
    }
    if (!Owns(state, owner, handle)) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "self-test handle is stale");
    return owner.scope->Release(handle.id)
        ? Status(ANOMALY_STATUS_V1_OK)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, "self-test handle is stale");
}

AnomalyStatusV1 ScopedPlatformServices::RunSelfTest(
    const ScopedPluginServiceOwner& owner,
    const std::string_view id,
    const AnomalyMutableByteSpanV1 destination,
    std::size_t* inout_size) noexcept {
    if (owner.scope == nullptr || !IsSafeToken(id) || inout_size == nullptr) return InvalidArgument();
    std::shared_ptr<Impl::SelfTestState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = std::find_if(
            impl_->self_tests_.begin(), impl_->self_tests_.end(), [&](const auto& entry) {
                return entry.second && entry.second->active.load(std::memory_order_acquire) &&
                    entry.second->id == id && SameOwner(entry.second->owner, owner);
            });
        if (found != impl_->self_tests_.end()) state = found->second;
    }
    if (!state) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "self-test is not registered");
    auto lease = state->owner.scope->AcquireCallback(state->owner.scope->Generation());
    if (!lease) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try {
        return state->callback(state->callback_user, destination, inout_size);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "self-test raised an exception");
    }
}

AnomalyStatusV1 ScopedPlatformServices::DiagnosticsSnapshot(
    const ScopedPluginServiceOwner& owner,
    const AnomalyMutableByteSpanV1 destination,
    std::size_t* inout_size) noexcept {
    if (owner.scope == nullptr) return InvalidArgument();
    try {
        const ScopedPlatformDiagnosticsView diagnostics = Snapshot(owner);
        Json snapshot;
        snapshot["schemaVersion"] = 1;
        snapshot["plugin"] = {
            {"id", owner.scope->Owner()}, {"generation", owner.scope->Generation()}};
        snapshot["ledgerResources"] = diagnostics.ledger_resources;
        snapshot["resources"] = {
            {"schemas", diagnostics.resources.configs},
            {"selfTests", diagnostics.resources.self_tests},
            {"tasks", diagnostics.resources.tasks},
            {"commands", diagnostics.resources.commands},
            {"notifications", diagnostics.resources.notifications},
            {"hooks", diagnostics.resources.hooks},
            {"patches", diagnostics.resources.patches}};
        snapshot["queuedTasks"] = diagnostics.queued_tasks;
        snapshot["callbacks"] = {
            {"calls", diagnostics.callback_calls},
            {"faults", diagnostics.callback_faults},
            {"slowCalls", diagnostics.slow_callbacks}};
        const std::string encoded = snapshot.dump();
        return CopyBytes(
            std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()),
            destination, inout_size);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "diagnostics snapshot failed");
    }
}

ScopedPlatformDiagnosticsView ScopedPlatformServices::Snapshot(
    const ScopedPluginServiceOwner& owner) const noexcept {
    return impl_ == nullptr ? ScopedPlatformDiagnosticsView{} : impl_->Snapshot(owner);
}

AnomalyStatusV1 ScopedPlatformServices::Schedule(
    const ScopedPluginServiceOwner& owner,
    const std::uint32_t delay_milliseconds,
    const AnomalyTaskCallbackV1 callback,
    void* callback_user,
    AnomalyGenerationHandleV1* handle) noexcept {
    if (owner.scope == nullptr || callback == nullptr || handle == nullptr) return InvalidArgument();
    try {
        auto state = std::make_shared<Impl::TaskState>();
        state->owner = owner;
        state->callback = callback;
        state->callback_user = callback_user;
        state->token = owner.scope->Register(
            PluginResourceKind::Task, "scheduler.task",
            [this, state] { RemoveTask(*impl_, state); });
        if (state->token == 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
        {
            std::scoped_lock lock(impl_->mutex_);
            impl_->tasks_.emplace(state->token, state);
        }
        const AnomalyGenerationHandleV1 task{state->token, owner.scope->Generation()};
        impl_->Enqueue(
            std::chrono::milliseconds(delay_milliseconds), OwnerKey(owner), task.id,
            [this, state, task] {
            if (!state->active.exchange(false, std::memory_order_acq_rel)) return;
            const auto started = std::chrono::steady_clock::now();
            bool fault{};
            auto lease = state->owner.scope->AcquireCallback(task.generation);
            if (lease) {
                try { state->callback(state->callback_user, task); } catch (...) { fault = true; }
            }
            impl_->RecordCallback(state->owner, fault, std::chrono::steady_clock::now() - started);
            static_cast<void>(state->owner.scope->Release(task.id));
        });
        *handle = task;
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "task scheduling failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::CancelTask(
    const ScopedPluginServiceOwner& owner, const AnomalyGenerationHandleV1 handle) noexcept {
    std::shared_ptr<Impl::TaskState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->tasks_.find(handle.id);
        if (found != impl_->tasks_.end()) state = found->second;
    }
    if (!Owns(state, owner, handle)) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "task handle is stale");
    return owner.scope->Release(handle.id)
        ? Status(ANOMALY_STATUS_V1_CANCELLED)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, "task handle is stale");
}

AnomalyStatusV1 ScopedPlatformServices::RegisterCommand(
    const ScopedPluginServiceOwner& owner,
    const std::string_view name,
    const std::string_view description,
    const AnomalyCommandCallbackV1 callback,
    void* callback_user,
    AnomalyGenerationHandleV1* handle) noexcept {
    if (owner.scope == nullptr || handle == nullptr || callback == nullptr || !IsSafeToken(name) ||
        description.size() > 512 || description.find('\0') != std::string_view::npos) {
        return InvalidArgument();
    }
    try {
        {
            std::scoped_lock lock(impl_->mutex_);
            const bool duplicate = std::any_of(
                impl_->commands_.begin(), impl_->commands_.end(), [&](const auto& entry) {
                    return entry.second && entry.second->active.load(std::memory_order_acquire) &&
                        entry.second->name == name;
                });
            if (duplicate) return Status(ANOMALY_STATUS_V1_CONFLICT, "command name is already registered");
        }
        auto state = std::make_shared<Impl::CommandState>();
        state->owner = owner;
        state->name = std::string(name);
        state->description = std::string(description);
        state->callback = callback;
        state->callback_user = callback_user;
        state->token = owner.scope->Register(
            PluginResourceKind::Command, "command:" + state->name,
            [this, state] { RemoveCommand(*impl_, state); });
        if (state->token == 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
        {
            std::scoped_lock lock(impl_->mutex_);
            impl_->commands_.emplace(state->token, state);
        }
        *handle = {state->token, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "command registration failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::UnregisterCommand(
    const ScopedPluginServiceOwner& owner, const AnomalyGenerationHandleV1 handle) noexcept {
    std::shared_ptr<Impl::CommandState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->commands_.find(handle.id);
        if (found != impl_->commands_.end()) state = found->second;
    }
    if (!Owns(state, owner, handle)) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "command handle is stale");
    return owner.scope->Release(handle.id)
        ? Status(ANOMALY_STATUS_V1_OK)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, "command handle is stale");
}

AnomalyStatusV1 ScopedPlatformServices::InvokeCommand(
    const ScopedPluginServiceOwner& owner,
    const std::string_view name,
    const std::string_view arguments,
    const AnomalyMutableByteSpanV1 destination,
    std::size_t* inout_size) noexcept {
    if (owner.scope == nullptr || !IsSafeToken(name) || arguments.find('\0') != std::string_view::npos ||
        inout_size == nullptr) {
        return InvalidArgument();
    }
    std::shared_ptr<Impl::CommandState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = std::find_if(
            impl_->commands_.begin(), impl_->commands_.end(), [&](const auto& entry) {
                return entry.second && entry.second->active.load(std::memory_order_acquire) &&
                    entry.second->name == name;
            });
        if (found != impl_->commands_.end()) state = found->second;
    }
    if (!state) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "command is not registered");
    auto lease = state->owner.scope->AcquireCallback(state->owner.scope->Generation());
    if (!lease) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "command owner is stopping");
    const auto started = std::chrono::steady_clock::now();
    bool fault{};
    AnomalyStatusV1 result{};
    try {
        result = state->callback(state->callback_user, {arguments.data(), arguments.size()}, destination, inout_size);
    } catch (...) {
        fault = true;
        result = Status(ANOMALY_STATUS_V1_FAILED, "command callback raised an exception");
    }
    impl_->RecordCallback(state->owner, fault, std::chrono::steady_clock::now() - started);
    return result;
}

AnomalyStatusV1 ScopedPlatformServices::PostNotification(
    const ScopedPluginServiceOwner& owner,
    const AnomalyNotificationSeverityV1 severity,
    const std::string_view title,
    const std::string_view body,
    const std::uint32_t timeout_milliseconds,
    AnomalyGenerationHandleV1* handle) noexcept {
    if (owner.scope == nullptr || handle == nullptr || !IsText(title) || body.find('\0') != std::string_view::npos ||
        title.size() > 256 || body.size() > 4096 || severity > ANOMALY_NOTIFICATION_V1_ERROR) {
        return InvalidArgument();
    }
    try {
        auto state = std::make_shared<Impl::NotificationState>();
        state->owner = owner;
        state->severity = severity;
        state->title = std::string(title);
        state->body = std::string(body);
        state->timeout_milliseconds = timeout_milliseconds;
        state->token = owner.scope->Register(
            PluginResourceKind::Notification, "notification:" + state->title,
            [this, state] { RemoveNotification(*impl_, state); });
        if (state->token == 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
        {
            std::scoped_lock lock(impl_->mutex_);
            impl_->notifications_.emplace(state->token, state);
        }
        *handle = {state->token, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "notification post failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::DismissNotification(
    const ScopedPluginServiceOwner& owner, const AnomalyGenerationHandleV1 handle) noexcept {
    std::shared_ptr<Impl::NotificationState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->notifications_.find(handle.id);
        if (found != impl_->notifications_.end()) state = found->second;
    }
    if (!Owns(state, owner, handle)) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "notification handle is stale");
    return owner.scope->Release(handle.id)
        ? Status(ANOMALY_STATUS_V1_OK)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, "notification handle is stale");
}

AnomalyStatusV1 ScopedPlatformServices::ResolveSignature(
    const ScopedPluginServiceOwner& owner,
    const std::string_view module_name,
    const std::string_view section_name,
    const std::string_view pattern,
    std::uintptr_t* address) noexcept {
    if (owner.scope == nullptr || address == nullptr || !IsSafeToken(module_name) ||
        !IsSafeToken(section_name) || !IsText(pattern) || pattern.size() > 1024 ||
        !impl_->memory_services_.memory || !impl_->memory_services_.patterns) {
        return InvalidArgument();
    }
    try {
        const std::wstring module = WideUtf8(module_name);
        if (module.empty()) return InvalidArgument("module name is not UTF-8");
        const auto module_info = impl_->memory_services_.memory->FindModule(module);
        if (!module_info) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "module is not loaded");
        const ue5mem::Pattern parsed = impl_->memory_services_.patterns->Parse(pattern);
        const auto matches = impl_->memory_services_.patterns->ScanSection(
            *module_info, section_name, parsed, 2);
        if (matches.empty()) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "signature did not match");
        if (matches.size() != 1) return Status(ANOMALY_STATUS_V1_CONFLICT, "signature matched multiple targets");
        *address = matches.front();
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "signature preflight failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::CreateHook(
    const ScopedPluginServiceOwner& owner,
    const AnomalyHookRequestV1* request,
    std::uintptr_t* original,
    AnomalyGenerationHandleV1* handle) noexcept {
    constexpr std::size_t request_size = offsetof(AnomalyHookRequestV1, label) +
        sizeof(AnomalyHookRequestV1::label);
    if (owner.scope == nullptr || request == nullptr || original == nullptr || handle == nullptr ||
        request->struct_size < request_size || request->target == 0 || request->detour == nullptr ||
        request->label.data == nullptr || !IsSafeToken({request->label.data, request->label.size})) {
        return InvalidArgument("hook request is invalid");
    }
    if (request->kind != ANOMALY_HOOK_V1_FUNCTION) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "hook kind requires a dedicated adapter");
    }
    if (!IsExecutableAddress(request->target)) {
        return Status(ANOMALY_STATUS_V1_FAILED, "hook target failed executable preflight");
    }
    std::shared_ptr<Impl::HookState> state;
    try {
        state = std::make_shared<Impl::HookState>();
        state->owner = owner;
        state->target = request->target;
        state->token = owner.scope->Register(
            PluginResourceKind::Hook, "hook:" + std::string(request->label.data, request->label.size),
            [this, state] { RemoveHook(*impl_, state, DefaultHookRemovalDeadline()); });
        if (state->token == 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");

        void* trampoline{};
        bool created{};
        {
            std::scoped_lock removal_lock(state->removal_mutex);
            {
                std::scoped_lock lock(impl_->mutex_);
                impl_->hooks_by_token_.emplace(state->token, state);
            }
            if (impl_->hooks_ != nullptr && impl_->hooks_->Create(
                    owner.scope->Owner(), owner.scope->Generation(),
                    std::string(request->label.data, request->label.size),
                    reinterpret_cast<void*>(request->target), request->detour, &trampoline)) {
                state->installed = true;
                *original = reinterpret_cast<std::uintptr_t>(trampoline);
                *handle = {state->token, owner.scope->Generation()};
                created = impl_->hooks_->Enable(
                    owner.scope->Owner(), owner.scope->Generation(),
                    reinterpret_cast<void*>(request->target));
            }
        }
        if (!created) {
            const bool removed = RemoveHook(*impl_, state, DefaultHookRemovalDeadline());
            if (removed) static_cast<void>(owner.scope->Release(state->token));
            return removed
                ? Status(ANOMALY_STATUS_V1_CONFLICT, "hook target conflicts with an existing hook")
                : Status(ANOMALY_STATUS_V1_FAILED, "hook setup cleanup failed");
        }
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        if (state && state->token != 0) {
            if (RemoveHook(*impl_, state, DefaultHookRemovalDeadline())) {
                static_cast<void>(owner.scope->Release(state->token));
            }
        }
        return Status(ANOMALY_STATUS_V1_FAILED, "hook creation failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::ReleaseHook(
    const ScopedPluginServiceOwner& owner, const AnomalyGenerationHandleV1 handle) noexcept {
    std::shared_ptr<Impl::HookState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->hooks_by_token_.find(handle.id);
        if (found != impl_->hooks_by_token_.end()) state = found->second;
    }
    if (!Owns(state, owner, handle)) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "hook handle is stale");
    if (!RemoveHook(*impl_, state, DefaultHookRemovalDeadline())) {
        return Status(ANOMALY_STATUS_V1_TIMEOUT, "hook callback did not drain");
    }
    return owner.scope->Release(handle.id)
        ? Status(ANOMALY_STATUS_V1_OK)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, "hook handle is stale");
}

AnomalyStatusV1 ScopedPlatformServices::BeginHookCallback(
    const ScopedPluginServiceOwner& owner,
    const AnomalyGenerationHandleV1 hook,
    AnomalyGenerationHandleV1* callback_lease) noexcept {
    if (callback_lease == nullptr) return InvalidArgument();
    std::shared_ptr<Impl::HookState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->hooks_by_token_.find(hook.id);
        if (found != impl_->hooks_by_token_.end()) state = found->second;
    }
    if (!Owns(state, owner, hook)) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "hook handle is stale");
    if (!impl_->hooks_) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "hook service is unavailable");
    auto hook_lease = impl_->hooks_->AcquireCallback(
        owner.scope->Owner(), owner.scope->Generation(), reinterpret_cast<void*>(state->target));
    if (!hook_lease) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "hook callback source is stopping");
    auto plugin_lease = owner.scope->AcquireCallback(owner.scope->Generation());
    if (!plugin_lease) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try {
        std::scoped_lock lock(impl_->mutex_);
        const std::uint64_t id = ++impl_->next_hook_lease_;
        impl_->hook_leases_.emplace(
            id, Impl::HookLeaseState{owner, std::move(hook_lease), std::move(plugin_lease)});
        *callback_lease = {id, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "hook callback lease registration failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::EndHookCallback(
    const ScopedPluginServiceOwner& owner,
    const AnomalyGenerationHandleV1 callback_lease) noexcept {
    if (owner.scope == nullptr) return InvalidArgument();
    std::scoped_lock lock(impl_->mutex_);
    const auto found = impl_->hook_leases_.find(callback_lease.id);
    if (found == impl_->hook_leases_.end() || callback_lease.generation != owner.scope->Generation() ||
        !SameOwner(found->second.owner, owner)) {
        return Status(ANOMALY_STATUS_V1_NOT_FOUND, "hook callback lease is stale");
    }
    impl_->hook_leases_.erase(found);
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ScopedPlatformServices::ApplyPatch(
    const ScopedPluginServiceOwner& owner,
    const std::uintptr_t address,
    const AnomalyByteSpanV1 replacement,
    const std::string_view label,
    AnomalyGenerationHandleV1* handle) noexcept {
    if (owner.scope == nullptr || handle == nullptr || replacement.data == nullptr || replacement.size == 0 ||
        replacement.size > kMaximumConfigBytes || !IsPatchableRange(address, replacement.size) ||
        (!label.empty() && !IsSafeToken(label)) || !impl_->memory_services_.memory) {
        return InvalidArgument("patch request failed preflight");
    }
    try {
        {
            std::scoped_lock lock(impl_->mutex_);
            const bool conflict = std::any_of(impl_->patches_.begin(), impl_->patches_.end(), [&](const auto& entry) {
                const auto& state = entry.second;
                return state && state->active.load(std::memory_order_acquire) &&
                    RangesOverlap(address, replacement.size, state->address, state->original.size());
            });
            if (conflict) return Status(ANOMALY_STATUS_V1_CONFLICT, "patch range conflicts with a tracked patch");
        }
        const auto original = impl_->memory_services_.memory->ReadMemory(address, replacement.size);
        if (!original || original->size() != replacement.size) {
            return Status(ANOMALY_STATUS_V1_FAILED, "patch target could not be read");
        }
        auto state = std::make_shared<Impl::PatchState>();
        state->owner = owner;
        state->address = address;
        state->original = *original;
        state->token = owner.scope->Register(
            PluginResourceKind::Patch,
            "patch:" + (label.empty() ? std::to_string(address) : std::string(label)),
            [this, state] { RemovePatch(*impl_, state); });
        if (state->token == 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
        if (!impl_->memory_services_.memory->PatchMemory(address, replacement.data, replacement.size)) {
            static_cast<void>(owner.scope->Release(state->token));
            return Status(ANOMALY_STATUS_V1_FAILED, "patch target could not be written");
        }
        state->applied = true;
        {
            std::scoped_lock lock(impl_->mutex_);
            impl_->patches_.emplace(state->token, state);
        }
        *handle = {state->token, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "patch application failed");
    }
}

AnomalyStatusV1 ScopedPlatformServices::ReleasePatch(
    const ScopedPluginServiceOwner& owner, const AnomalyGenerationHandleV1 handle) noexcept {
    std::shared_ptr<Impl::PatchState> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->patches_.find(handle.id);
        if (found != impl_->patches_.end()) state = found->second;
    }
    if (!Owns(state, owner, handle)) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "patch handle is stale");
    return owner.scope->Release(handle.id)
        ? Status(ANOMALY_STATUS_V1_OK)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, "patch handle is stale");
}

bool ScopedPlatformServices::RevokeScope(
    const ScopedPluginServiceOwner& owner,
    const std::chrono::steady_clock::time_point deadline,
    const ScopedPlatformRevokePhase phase) noexcept {
    if (owner.scope == nullptr) return true;
    std::vector<std::shared_ptr<Impl::HookState>> hooks;
    std::vector<std::uint64_t> other_tokens;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto collect = [&](const auto& map) {
            for (const auto& [token, state] : map) {
                if (state && SameOwner(state->owner, owner)) other_tokens.push_back(token);
            }
        };
        if (phase == ScopedPlatformRevokePhase::Final) collect(impl_->configs_);
        collect(impl_->self_tests_);
        collect(impl_->tasks_);
        collect(impl_->commands_);
        collect(impl_->notifications_);
        collect(impl_->patches_);
        for (const auto& [token, state] : impl_->hooks_by_token_) {
            if (state && SameOwner(state->owner, owner)) hooks.push_back(state);
        }
    }
    std::sort(hooks.begin(), hooks.end(), [](const auto& left, const auto& right) {
        return left->token > right->token;
    });
    for (const auto& state : hooks) {
        if (!RemoveHook(*impl_, state, deadline)) return false;
        static_cast<void>(owner.scope->Release(state->token));
    }
    std::sort(other_tokens.begin(), other_tokens.end(), std::greater<>());
    other_tokens.erase(std::unique(other_tokens.begin(), other_tokens.end()), other_tokens.end());
    for (const std::uint64_t token : other_tokens) {
        static_cast<void>(owner.scope->Release(token));
    }
    return true;
}

}  // namespace anomaly
