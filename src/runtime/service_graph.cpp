#include "anomaly/service_graph.hpp"
#include "anomaly/thread_local_value.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <queue>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace anomaly {
namespace {

DWORD ExceptionError() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::system_error& error) {
        if (error.code().category() == std::system_category() && error.code().value() > 0) {
            return static_cast<DWORD>(error.code().value());
        }
        return ERROR_GEN_FAILURE;
    } catch (...) {
        return ERROR_UNHANDLED_EXCEPTION;
    }
}

bool IsReady(ServiceState state) noexcept {
    return state == ServiceState::Ready || state == ServiceState::Degraded;
}

ThreadLocalScalar<const void*> g_service_start_callback_owner;

class ServiceStartCallbackScope final {
public:
    explicit ServiceStartCallbackScope(const void* owner) noexcept
        : previous_(g_service_start_callback_owner.Get()) {
        g_service_start_callback_owner.Set(owner);
    }

    ~ServiceStartCallbackScope() { g_service_start_callback_owner.Set(previous_); }

private:
    const void* previous_{};
};

}  // namespace

class ServiceGraph::Impl final {
public:
    ~Impl() {
        StopAll();
    }

    DWORD Register(ServiceDescriptor descriptor) noexcept {
        try {
            std::scoped_lock lock(mutex_);
            if (registration_closed_ || operation_active_ || stop_source_.stop_requested()) {
                return ERROR_INVALID_STATE;
            }
            nodes_.push_back(Node{std::move(descriptor)});
            return ERROR_SUCCESS;
        } catch (...) {
            return ExceptionError();
        }
    }

    void SetAffinityExecutors(ServiceAffinityExecutors executors) noexcept {
        try {
            std::scoped_lock lock(mutex_);
            if (registration_closed_ || operation_active_) return;
            affinity_executors_ = std::move(executors);
        } catch (...) {
            // Configuration is best-effort and must not make graph teardown
            // throw from a noexcept boundary.
        }
    }

    DWORD Build() noexcept {
        const DWORD begin_error = BeginOperation();
        if (begin_error != ERROR_SUCCESS) return begin_error;
        OperationEnd operation_end(*this);

        try {
            std::scoped_lock lock(mutex_);
            if (registration_closed_) {
                return built_ ? ERROR_SUCCESS : last_error_;
            }
            registration_closed_ = true;
            ResetBuildState();

            std::unordered_map<std::string, std::size_t> ids;
            ids.reserve(nodes_.size());
            bool invalid{};
            for (std::size_t index = 0; index < nodes_.size(); ++index) {
                Node& node = nodes_[index];
                const ServiceDescriptor& descriptor = node.descriptor;
                DWORD error = ERROR_SUCCESS;
                if (descriptor.id.empty() || descriptor.version == 0) {
                    error = ERROR_INVALID_PARAMETER;
                } else if (descriptor.lifetime != ServiceLifetime::Provided &&
                           (!descriptor.start || !descriptor.stop)) {
                    error = ERROR_INVALID_FUNCTION;
                } else {
                    const auto [existing, inserted] = ids.emplace(descriptor.id, index);
                    if (!inserted) {
                        error = ERROR_DUP_NAME;
                        MarkBuildFailure(existing->second, ERROR_DUP_NAME, descriptor.id);
                    }
                }
                if (error != ERROR_SUCCESS) {
                    MarkBuildFailure(index, error, {});
                    invalid = true;
                }
            }
            if (invalid) return FinishBuildFailure();

            adjacency_.assign(nodes_.size(), {});
            required_dependents_.assign(nodes_.size(), {});
            std::vector<std::size_t> indegrees(nodes_.size());
            for (std::size_t index = 0; index < nodes_.size(); ++index) {
                Node& node = nodes_[index];
                std::unordered_set<std::string> declared_dependencies;
                declared_dependencies.reserve(
                    node.descriptor.required_dependencies.size() +
                    node.descriptor.optional_dependencies.size());
                for (const ServiceDependency& dependency :
                     node.descriptor.required_dependencies) {
                    if (!declared_dependencies.emplace(dependency.id).second) {
                        MarkBuildFailure(index, ERROR_INVALID_DATA, dependency.id);
                        invalid = true;
                        continue;
                    }
                    const auto resolved = ids.find(dependency.id);
                    if (dependency.id.empty() || dependency.minimum_version == 0 ||
                        resolved == ids.end()) {
                        MarkBuildFailure(index, ERROR_NOT_FOUND, dependency.id);
                        invalid = true;
                        continue;
                    }
                    const std::size_t dependency_index = resolved->second;
                    if (nodes_[dependency_index].descriptor.version < dependency.minimum_version) {
                        MarkBuildFailure(index, ERROR_REVISION_MISMATCH, dependency.id);
                        invalid = true;
                        continue;
                    }
                    node.required_indices.push_back(dependency_index);
                    adjacency_[dependency_index].push_back(index);
                    required_dependents_[dependency_index].push_back(index);
                    ++indegrees[index];
                }
                for (const ServiceDependency& dependency :
                     node.descriptor.optional_dependencies) {
                    OptionalDependency optional;
                    optional.dependency = dependency;
                    if (!declared_dependencies.emplace(dependency.id).second ||
                        dependency.id.empty() || dependency.minimum_version == 0) {
                        MarkBuildFailure(index, ERROR_INVALID_DATA, dependency.id);
                        invalid = true;
                    } else {
                        const auto resolved = ids.find(dependency.id);
                        if (resolved != ids.end() &&
                            nodes_[resolved->second].descriptor.version >=
                                dependency.minimum_version) {
                            optional.index = resolved->second;
                            adjacency_[*optional.index].push_back(index);
                            ++indegrees[index];
                        }
                    }
                    node.optional_dependencies.push_back(std::move(optional));
                }
            }
            if (invalid) return FinishBuildFailure();

            std::priority_queue<
                std::size_t, std::vector<std::size_t>, std::greater<std::size_t>> ready;
            for (std::size_t index = 0; index < indegrees.size(); ++index) {
                if (indegrees[index] == 0) ready.push(index);
            }
            while (!ready.empty()) {
                const std::size_t index = ready.top();
                ready.pop();
                topological_order_.push_back(index);
                for (const std::size_t dependent : adjacency_[index]) {
                    if (--indegrees[dependent] == 0) ready.push(dependent);
                }
            }
            if (topological_order_.size() != nodes_.size()) {
                const std::vector<std::size_t> cycle = FindCycle();
                if (cycle.empty()) {
                    for (std::size_t index = 0; index < indegrees.size(); ++index) {
                        if (indegrees[index] != 0) {
                            MarkBuildFailure(index, ERROR_CIRCULAR_DEPENDENCY, {});
                        }
                    }
                } else {
                    for (std::size_t position = 0; position < cycle.size(); ++position) {
                        const std::size_t index = cycle[position];
                        const std::size_t caused_by = cycle[(position + 1) % cycle.size()];
                        MarkBuildFailure(
                            index, ERROR_CIRCULAR_DEPENDENCY,
                            nodes_[caused_by].descriptor.id);
                    }
                }
                return FinishBuildFailure();
            }

            built_ = true;
            for (Node& node : nodes_) {
                if (node.descriptor.lifetime == ServiceLifetime::Provided) {
                    node.state = ServiceState::Ready;
                }
            }
            last_error_ = ERROR_SUCCESS;
            return ERROR_SUCCESS;
        } catch (...) {
            const DWORD error = ExceptionError();
            std::scoped_lock lock(mutex_);
            built_ = false;
            last_error_ = error;
            failures_.push_back({{}, error, {}});
            return error;
        }
    }

    DWORD StartAll(std::stop_token external_stop_token) noexcept {
        return StartSelected(std::nullopt, external_stop_token);
    }

    DWORD StartService(
        std::string_view service_id, std::stop_token external_stop_token) noexcept {
        try {
            std::optional<std::size_t> requested;
            {
                std::scoped_lock lock(mutex_);
                for (std::size_t index = 0; index < nodes_.size(); ++index) {
                    if (nodes_[index].descriptor.id == service_id) {
                        requested = index;
                        break;
                    }
                }
            }
            if (!requested) return ERROR_NOT_FOUND;
            return StartSelected(requested, external_stop_token);
        } catch (...) {
            return ExceptionError();
        }
    }

    DWORD WaitForAsync(std::stop_token external_stop_token) noexcept {
        try {
            if (g_service_start_callback_owner.Get() == this) return ERROR_BUSY;
            std::stop_callback external_stop_callback(
                external_stop_token, [this] { stop_source_.request_stop(); });
            std::unique_lock lock(mutex_);
            if (operation_active_ && operation_thread_ == std::this_thread::get_id()) {
                return ERROR_BUSY;
            }
            startup_condition_.wait(lock, [this] { return async_startup_complete_; });
            const DWORD result = async_startup_error_;
            lock.unlock();
            JoinStartupThread();
            return result;
        } catch (...) {
            return ExceptionError();
        }
    }

    void CancelStartup() noexcept {
        defer_cancel_rollback_.store(true, std::memory_order_release);
        stop_source_.request_stop();
    }

    void StopAll() noexcept {
        stop_source_.request_stop();
        if (g_service_start_callback_owner.Get() == this) return;
        const DWORD begin_error = BeginOperation();
        if (begin_error != ERROR_SUCCESS) return;
        OperationEnd operation_end(*this);

        JoinStartupThread();
        StopStartedServices();
        try {
            std::scoped_lock lock(mutex_);
            for (Node& node : nodes_) {
                if (node.state == ServiceState::Registered ||
                    node.state == ServiceState::Starting) {
                    node.state = ServiceState::Stopped;
                }
            }
        } catch (...) {
        }
    }

    ServiceGraphSnapshot Snapshot() const {
        std::scoped_lock lock(mutex_);
        ServiceGraphSnapshot snapshot;
        snapshot.built = built_;
        snapshot.stop_requested = stop_source_.stop_requested();
        snapshot.startup_active = startup_active_;
        snapshot.blocking_startup_complete = blocking_startup_complete_;
        snapshot.async_startup_complete = async_startup_complete_;
        snapshot.error = last_error_;
        snapshot.async_startup_error = async_startup_error_;
        snapshot.failures = failures_;
        snapshot.services.reserve(nodes_.size());
        for (const Node& node : nodes_) {
            ServiceSnapshot service;
            service.id = node.descriptor.id;
            service.version = node.descriptor.version;
            service.lifetime = node.descriptor.lifetime;
            service.startup = node.descriptor.startup;
            service.affinity = node.descriptor.affinity;
            service.state = node.state;
            service.error = node.error;
            service.startup_duration = node.startup_duration;
            service.start_thread_id = node.start_thread_id;
            service.stop_thread_id = node.stop_thread_id;
            service.start_queue_delay = node.start_queue_delay;
            service.stop_queue_delay = node.stop_queue_delay;
            service.affinity_bypassed = node.affinity_bypassed;
            service.failure_chain = node.failure_chain;
            service.dependencies.reserve(
                node.descriptor.required_dependencies.size() +
                node.descriptor.optional_dependencies.size());
            for (const ServiceDependency& dependency :
                 node.descriptor.required_dependencies) {
                ServiceDependencySnapshot dependency_snapshot;
                dependency_snapshot.id = dependency.id;
                dependency_snapshot.minimum_version = dependency.minimum_version;
                const std::optional<std::size_t> resolved = FindResolvedDependency(dependency);
                if (resolved) {
                    FillResolvedDependency(dependency_snapshot, *resolved);
                }
                service.dependencies.push_back(std::move(dependency_snapshot));
            }
            for (const OptionalDependency& dependency : node.optional_dependencies) {
                ServiceDependencySnapshot dependency_snapshot;
                dependency_snapshot.id = dependency.dependency.id;
                dependency_snapshot.minimum_version = dependency.dependency.minimum_version;
                dependency_snapshot.optional = true;
                if (dependency.index) {
                    FillResolvedDependency(dependency_snapshot, *dependency.index);
                }
                service.dependencies.push_back(std::move(dependency_snapshot));
            }
            snapshot.services.push_back(std::move(service));
        }
        return snapshot;
    }

private:
    struct OptionalDependency {
        ServiceDependency dependency;
        std::optional<std::size_t> index;
    };

    struct Node {
        explicit Node(ServiceDescriptor value) : descriptor(std::move(value)) {}

        ServiceDescriptor descriptor;
        ServiceState state{ServiceState::Registered};
        DWORD error{ERROR_SUCCESS};
        std::chrono::microseconds startup_duration{};
        std::uint64_t start_thread_id{};
        std::uint64_t stop_thread_id{};
        std::chrono::microseconds start_queue_delay{};
        std::chrono::microseconds stop_queue_delay{};
        bool affinity_bypassed{};
        std::vector<std::size_t> required_indices;
        std::vector<OptionalDependency> optional_dependencies;
        std::vector<std::string> failure_chain;
        bool started{};
    };

    struct StartResult {
        std::size_t index{};
        DWORD error{ERROR_SUCCESS};
        std::chrono::microseconds duration{};
    };

    struct ActiveStart {
        std::size_t index{};
        std::future<StartResult> result;
    };

    class CancelOnUnwind final {
    public:
        explicit CancelOnUnwind(std::stop_source& source) noexcept
            : source_(source), exception_count_(std::uncaught_exceptions()) {}

        ~CancelOnUnwind() {
            if (std::uncaught_exceptions() > exception_count_) source_.request_stop();
        }

    private:
        std::stop_source& source_;
        int exception_count_{};
    };

    class OperationEnd final {
    public:
        explicit OperationEnd(Impl& owner) noexcept : owner_(owner) {}
        ~OperationEnd() { owner_.EndOperation(); }

        OperationEnd(const OperationEnd&) = delete;
        OperationEnd& operator=(const OperationEnd&) = delete;

    private:
        Impl& owner_;
    };

    DWORD BeginOperation() noexcept {
        try {
            if (g_service_start_callback_owner.Get() == this) return ERROR_BUSY;
            std::unique_lock lock(mutex_);
            const std::thread::id current_thread = std::this_thread::get_id();
            if (operation_active_ && operation_thread_ == current_thread) return ERROR_BUSY;
            operation_condition_.wait(lock, [this] { return !operation_active_; });
            operation_active_ = true;
            operation_thread_ = current_thread;
            return ERROR_SUCCESS;
        } catch (...) {
            return ExceptionError();
        }
    }

    void EndOperation() noexcept {
        {
            std::scoped_lock lock(mutex_);
            operation_active_ = false;
            operation_thread_ = {};
        }
        operation_condition_.notify_all();
    }

    void ResetBuildState() {
        defer_cancel_rollback_.store(false, std::memory_order_relaxed);
        built_ = false;
        last_error_ = ERROR_SUCCESS;
        startup_active_ = false;
        blocking_startup_complete_ = false;
        async_startup_complete_ = true;
        blocking_startup_error_ = ERROR_SUCCESS;
        async_startup_error_ = ERROR_SUCCESS;
        failures_.clear();
        topological_order_.clear();
        adjacency_.clear();
        required_dependents_.clear();
        for (Node& node : nodes_) {
            node.state = ServiceState::Registered;
            node.error = ERROR_SUCCESS;
            node.startup_duration = {};
            node.required_indices.clear();
            node.optional_dependencies.clear();
            node.failure_chain.clear();
            node.started = false;
        }
    }

    void MarkBuildFailure(std::size_t index, DWORD error, std::string caused_by) {
        Node& node = nodes_[index];
        node.state = ServiceState::Failed;
        node.error = error;
        node.failure_chain = {node.descriptor.id};
        if (!caused_by.empty()) node.failure_chain.push_back(caused_by);
        failures_.push_back({node.descriptor.id, error, std::move(caused_by)});
        if (last_error_ == ERROR_SUCCESS) last_error_ = error;
    }

    DWORD FinishBuildFailure() noexcept {
        built_ = false;
        if (last_error_ == ERROR_SUCCESS) last_error_ = ERROR_INVALID_DATA;
        return last_error_;
    }

    std::vector<std::size_t> FindCycle() const {
        std::vector<unsigned char> colors(nodes_.size());
        std::vector<std::size_t> stack;
        std::vector<std::size_t> cycle;
        const auto visit = [&](const auto& self, std::size_t index) -> bool {
            colors[index] = 1;
            stack.push_back(index);
            for (const std::size_t dependency : DependenciesOf(index)) {
                if (colors[dependency] == 0) {
                    if (self(self, dependency)) return true;
                } else if (colors[dependency] == 1) {
                    const auto begin = std::find(stack.begin(), stack.end(), dependency);
                    cycle.assign(begin, stack.end());
                    return true;
                }
            }
            stack.pop_back();
            colors[index] = 2;
            return false;
        };
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            if (colors[index] == 0 && visit(visit, index)) break;
        }
        return cycle;
    }

    std::vector<std::size_t> DependenciesOf(std::size_t index) const {
        std::vector<std::size_t> dependencies = nodes_[index].required_indices;
        for (const OptionalDependency& optional : nodes_[index].optional_dependencies) {
            if (optional.index) dependencies.push_back(*optional.index);
        }
        return dependencies;
    }

    void FillResolvedDependency(
        ServiceDependencySnapshot& snapshot, std::size_t dependency_index) const {
        const Node& dependency = nodes_[dependency_index];
        snapshot.resolved = true;
        snapshot.resolved_version = dependency.descriptor.version;
        snapshot.state = dependency.state;
    }

    std::optional<std::size_t> FindResolvedDependency(
        const ServiceDependency& dependency) const noexcept {
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            const ServiceDescriptor& candidate = nodes_[index].descriptor;
            if (candidate.id == dependency.id &&
                candidate.version >= dependency.minimum_version) {
                return index;
            }
        }
        return std::nullopt;
    }

    DWORD StartSelected(
        std::optional<std::size_t> requested, std::stop_token external_stop_token) noexcept {
        const DWORD begin_error = BeginOperation();
        if (begin_error != ERROR_SUCCESS) return begin_error;
        bool operation_transferred{};
        try {
            JoinStartupThread();
            std::vector<bool> selected;
            DWORD selection_error = ERROR_SUCCESS;
            {
                std::scoped_lock lock(mutex_);
                if (!built_) {
                    selection_error =
                        last_error_ == ERROR_SUCCESS ? ERROR_INVALID_STATE : last_error_;
                } else if (stop_source_.stop_requested()) {
                    selection_error = ERROR_CANCELLED;
                } else {
                    selected.assign(nodes_.size(), false);
                    if (requested) {
                        SelectRequiredClosure(*requested, selected, true);
                    } else {
                        for (std::size_t index = 0; index < nodes_.size(); ++index) {
                            const ServiceDescriptor& descriptor = nodes_[index].descriptor;
                            if (descriptor.startup != ServiceStartup::Lazy &&
                                descriptor.lifetime != ServiceLifetime::PluginScoped) {
                                SelectRequiredClosure(index, selected, false);
                            }
                        }
                    }
                    startup_active_ = true;
                    blocking_startup_complete_ = false;
                    async_startup_complete_ = false;
                    blocking_startup_error_ = ERROR_IO_PENDING;
                    async_startup_error_ = ERROR_IO_PENDING;
                }
            }
            if (selection_error != ERROR_SUCCESS) {
                EndOperation();
                return selection_error;
            }

            try {
                std::scoped_lock thread_lock(startup_thread_mutex_);
                startup_thread_ = std::thread(
                    [this, selected = std::move(selected), external_stop_token]() mutable {
                        {
                            std::scoped_lock lock(mutex_);
                            operation_thread_ = std::this_thread::get_id();
                        }
                        RunStartup(std::move(selected), external_stop_token);
                        EndOperation();
                    });
                operation_transferred = true;
            } catch (...) {
                const DWORD error = ExceptionError();
                FinishStartup(error);
                EndOperation();
                return error;
            }

            std::unique_lock lock(mutex_);
            startup_condition_.wait(lock, [this] {
                return blocking_startup_complete_ || async_startup_complete_;
            });
            return blocking_startup_complete_
                ? blocking_startup_error_
                : async_startup_error_;
        } catch (...) {
            const DWORD error = ExceptionError();
            SetLastError(error);
            stop_source_.request_stop();
            if (!operation_transferred) {
                FinishStartup(error);
                EndOperation();
            }
            return error;
        }
    }

    void RunStartup(
        std::vector<bool> selected, std::stop_token external_stop_token) noexcept {
        DWORD result = ERROR_SUCCESS;
        try {
            std::stop_callback external_stop_callback(
                external_stop_token, [this] { stop_source_.request_stop(); });
            result = RunStartupLoop(selected);
        } catch (...) {
            result = ExceptionError();
            SetLastError(result);
            stop_source_.request_stop();
        }
        const bool defer_cancel_rollback =
            result == ERROR_CANCELLED &&
            defer_cancel_rollback_.load(std::memory_order_acquire);
        if (result != ERROR_SUCCESS && !defer_cancel_rollback) StopStartedServices();
        FinishStartup(result);
    }

    DWORD RunStartupLoop(const std::vector<bool>& selected) {
        std::vector<ActiveStart> active;
        CancelOnUnwind cancel_on_unwind(stop_source_);
        DWORD result = ERROR_SUCCESS;
        if (stop_source_.stop_requested()) {
            RecordCancellation(selected);
            return ERROR_CANCELLED;
        }
        SignalBlockingStartupIfComplete(selected);

        for (;;) {
            if (stop_source_.stop_requested() && result == ERROR_SUCCESS) {
                result = ERROR_CANCELLED;
            }

            if (result != ERROR_SUCCESS) {
                stop_source_.request_stop();
                DrainActiveStarts(active, selected, result);
                if (result == ERROR_CANCELLED) RecordCancellation(selected);
                SetLastError(result);
                return result;
            }

            std::vector<std::size_t> ready_blocking;
            std::vector<std::size_t> ready_async;
            CollectReadyStarts(selected, ready_blocking, ready_async);
            bool progressed = !ready_blocking.empty() || !ready_async.empty();

            active.reserve(active.size() + ready_async.size());

            for (const std::size_t index : ready_async) {
                BeginStart(index);
                std::future<StartResult> start_future;
                try {
                    start_future = std::async(std::launch::async, [this, index] {
                        StartResult result = InvokeStart(index);
                        if (result.error != ERROR_SUCCESS) {
                            stop_source_.request_stop();
                        }
                        return result;
                    });
                    active.push_back({index, std::move(start_future)});
                } catch (...) {
                    stop_source_.request_stop();
                    result = ExceptionError();
                    MarkStartFailure(index, result, {});
                    PropagateRequiredFailure(index, selected);
                    break;
                }
            }

            for (const std::size_t index : ready_blocking) {
                if (result != ERROR_SUCCESS) break;
                BeginStart(index);
                const StartResult start_result = InvokeStart(index);
                MergeStartupError(result, CompleteStart(start_result, selected));
                SignalBlockingStartupIfComplete(selected);
            }

            for (auto position = active.begin(); position != active.end();) {
                if (position->result.wait_for(std::chrono::milliseconds(0)) !=
                    std::future_status::ready) {
                    ++position;
                    continue;
                }
                const StartResult start_result = position->result.get();
                const DWORD completion_error = CompleteStart(start_result, selected);
                MergeStartupError(result, completion_error);
                position = active.erase(position);
                progressed = true;
                SignalBlockingStartupIfComplete(selected);
            }

            if (result != ERROR_SUCCESS) continue;
            if (StartupSelectionComplete(selected) && active.empty()) {
                if (stop_source_.stop_requested()) {
                    result = ERROR_CANCELLED;
                    RecordCancellation(selected);
                    continue;
                }
                return ERROR_SUCCESS;
            }

            if (!progressed) {
                if (!active.empty()) {
                    active.front().result.wait_for(std::chrono::milliseconds(1));
                    continue;
                }
                const std::optional<std::size_t> blocked = FirstPendingService(selected);
                if (!blocked) return ERROR_SUCCESS;
                result = ERROR_DEPENDENCY_NOT_FOUND;
                MarkStartFailure(*blocked, result, {});
                PropagateRequiredFailure(*blocked, selected);
            }
        }
    }

    void CollectReadyStarts(
        const std::vector<bool>& selected,
        std::vector<std::size_t>& ready_blocking,
        std::vector<std::size_t>& ready_async) {
        std::scoped_lock lock(mutex_);
        for (const std::size_t index : topological_order_) {
            const Node& node = nodes_[index];
            if (!selected[index] || node.state != ServiceState::Registered ||
                !DependenciesReady(index, selected)) {
                continue;
            }
            if (node.descriptor.startup == ServiceStartup::Async) {
                ready_async.push_back(index);
            } else {
                ready_blocking.push_back(index);
            }
        }
    }

    void BeginStart(std::size_t index) noexcept {
        std::scoped_lock lock(mutex_);
        Node& node = nodes_[index];
        node.state = ServiceState::Starting;
        node.error = ERROR_SUCCESS;
        node.failure_chain.clear();
    }

    bool DependenciesReady(
        std::size_t index, const std::vector<bool>& selected) const noexcept {
        const Node& node = nodes_[index];
        for (const std::size_t dependency : node.required_indices) {
            if (!IsReady(nodes_[dependency].state)) return false;
        }
        for (const OptionalDependency& dependency : node.optional_dependencies) {
            if (dependency.index && selected[*dependency.index]) {
                const ServiceState state = nodes_[*dependency.index].state;
                if (state == ServiceState::Registered || state == ServiceState::Starting) {
                    return false;
                }
            }
        }
        return true;
    }

    StartResult InvokeStart(std::size_t index) noexcept {
        ServiceStartCallbackScope callback_scope(this);
        const auto start_time = std::chrono::steady_clock::now();
        DWORD error = ERROR_SUCCESS;
        const ServiceDescriptor descriptor = [&] {
            std::scoped_lock lock(mutex_);
            return nodes_[index].descriptor;
        }();
        const auto callback = [this, index](std::stop_token token) -> DWORD {
            const auto thread_id = static_cast<std::uint64_t>(GetCurrentThreadId());
            {
                std::scoped_lock lock(mutex_);
                nodes_[index].start_thread_id = thread_id;
            }
            try {
                return nodes_[index].descriptor.start(token);
            } catch (...) {
                return ExceptionError();
            }
        };
        try {
            const auto dispatch_started = std::chrono::steady_clock::now();
            if (affinity_executors_.start && descriptor.affinity != ServiceAffinity::Any) {
                error = affinity_executors_.start(
                    descriptor.affinity, callback, stop_source_.get_token());
                std::scoped_lock lock(mutex_);
                nodes_[index].start_queue_delay =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        dispatch_started - start_time);
            } else {
                error = callback(stop_source_.get_token());
                std::scoped_lock lock(mutex_);
                nodes_[index].affinity_bypassed =
                    nodes_[index].affinity_bypassed ||
                    (descriptor.affinity != ServiceAffinity::Any &&
                     !affinity_executors_.start);
            }
        } catch (...) {
            error = ExceptionError();
        }
        return {
            index,
            error,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_time)};
    }

    DWORD CompleteStart(
        const StartResult& result, const std::vector<bool>& selected) {
        if (result.error != ERROR_SUCCESS) {
            {
                std::scoped_lock lock(mutex_);
                nodes_[result.index].startup_duration = result.duration;
            }
            MarkStartFailure(result.index, result.error, {});
            PropagateRequiredFailure(result.index, selected);
            return result.error;
        }

        std::scoped_lock lock(mutex_);
        Node& node = nodes_[result.index];
        node.startup_duration = result.duration;
        node.state = HasUnavailableOptionalDependency(result.index)
            ? ServiceState::Degraded
            : ServiceState::Ready;
        node.started = true;
        return ERROR_SUCCESS;
    }

    void DrainActiveStarts(
        std::vector<ActiveStart>& active,
        const std::vector<bool>& selected,
        DWORD& primary_error) {
        for (ActiveStart& start : active) {
            const StartResult result = start.result.get();
            const DWORD completion_error = CompleteStart(result, selected);
            MergeStartupError(primary_error, completion_error);
        }
        active.clear();
    }

    static void MergeStartupError(DWORD& primary_error, DWORD candidate) noexcept {
        if (candidate == ERROR_SUCCESS) return;
        if (primary_error == ERROR_SUCCESS ||
            (primary_error == ERROR_CANCELLED && candidate != ERROR_CANCELLED)) {
            primary_error = candidate;
        }
    }

    bool StartupSelectionComplete(const std::vector<bool>& selected) const noexcept {
        std::scoped_lock lock(mutex_);
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            if (!selected[index]) continue;
            const ServiceState state = nodes_[index].state;
            if (state == ServiceState::Registered || state == ServiceState::Starting) {
                return false;
            }
        }
        return true;
    }

    std::optional<std::size_t> FirstPendingService(
        const std::vector<bool>& selected) const noexcept {
        std::scoped_lock lock(mutex_);
        for (const std::size_t index : topological_order_) {
            if (selected[index] && nodes_[index].state == ServiceState::Registered) {
                return index;
            }
        }
        return std::nullopt;
    }

    void SignalBlockingStartupIfComplete(const std::vector<bool>& selected) noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (blocking_startup_complete_ || stop_source_.stop_requested()) return;
            for (std::size_t index = 0; index < nodes_.size(); ++index) {
                if (!selected[index] ||
                    nodes_[index].descriptor.startup == ServiceStartup::Async) {
                    continue;
                }
                if (!IsReady(nodes_[index].state)) return;
            }
            blocking_startup_complete_ = true;
            blocking_startup_error_ = ERROR_SUCCESS;
        }
        startup_condition_.notify_all();
    }

    void FinishStartup(DWORD error) noexcept {
        {
            std::scoped_lock lock(mutex_);
            startup_active_ = false;
            if (!blocking_startup_complete_) {
                blocking_startup_complete_ = true;
                blocking_startup_error_ = error;
            }
            async_startup_complete_ = true;
            async_startup_error_ = error;
            if (error != ERROR_SUCCESS &&
                (last_error_ == ERROR_SUCCESS ||
                 (last_error_ == ERROR_CANCELLED && error != ERROR_CANCELLED))) {
                last_error_ = error;
            }
        }
        startup_condition_.notify_all();
    }

    void JoinStartupThread() noexcept {
        std::thread thread;
        {
            std::scoped_lock lock(startup_thread_mutex_);
            if (!startup_thread_.joinable() ||
                startup_thread_.get_id() == std::this_thread::get_id()) {
                return;
            }
            thread = std::move(startup_thread_);
        }
        try {
            thread.join();
        } catch (...) {
            try {
                thread.detach();
            } catch (...) {
            }
        }
    }

    void SelectRequiredClosure(
        std::size_t index,
        std::vector<bool>& selected,
        bool include_plugin_scoped) const {
        if (!include_plugin_scoped &&
            nodes_[index].descriptor.lifetime == ServiceLifetime::PluginScoped) {
            return;
        }
        if (selected[index]) return;
        selected[index] = true;
        for (const std::size_t dependency : nodes_[index].required_indices) {
            SelectRequiredClosure(dependency, selected, include_plugin_scoped);
        }
    }

    bool HasUnavailableOptionalDependency(std::size_t index) const noexcept {
        const Node& node = nodes_[index];
        for (const OptionalDependency& dependency : node.optional_dependencies) {
            if (!dependency.index || !IsReady(nodes_[*dependency.index].state)) return true;
        }
        return false;
    }

    void SetLastError(DWORD error) noexcept {
        if (error == ERROR_SUCCESS) return;
        std::scoped_lock lock(mutex_);
        if (last_error_ == ERROR_SUCCESS ||
            (last_error_ == ERROR_CANCELLED && error != ERROR_CANCELLED)) {
            last_error_ = error;
        }
    }

    void MarkStartFailure(std::size_t index, DWORD error, std::string caused_by) {
        std::scoped_lock lock(mutex_);
        Node& node = nodes_[index];
        node.state = ServiceState::Failed;
        node.error = error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
        node.failure_chain = {node.descriptor.id};
        if (!caused_by.empty()) node.failure_chain.push_back(caused_by);
        failures_.push_back({node.descriptor.id, node.error, std::move(caused_by)});
        if (last_error_ == ERROR_SUCCESS ||
            (last_error_ == ERROR_CANCELLED && node.error != ERROR_CANCELLED)) {
            last_error_ = node.error;
        }
    }

    void PropagateRequiredFailure(std::size_t root, const std::vector<bool>& selected) {
        std::scoped_lock lock(mutex_);
        std::deque<std::size_t> pending;
        pending.push_back(root);
        while (!pending.empty()) {
            const std::size_t failed = pending.front();
            pending.pop_front();
            for (const std::size_t dependent : required_dependents_[failed]) {
                Node& node = nodes_[dependent];
                if (!selected[dependent] || node.state != ServiceState::Registered) continue;
                node.state = ServiceState::Failed;
                node.error = ERROR_DEPENDENCY_NOT_FOUND;
                node.failure_chain = nodes_[failed].failure_chain;
                node.failure_chain.push_back(node.descriptor.id);
                failures_.push_back(
                    {node.descriptor.id, node.error, nodes_[failed].descriptor.id});
                pending.push_back(dependent);
            }
        }
    }

    void RecordCancellation(
        const std::vector<bool>& selected,
        std::optional<std::size_t> preferred = std::nullopt) {
        std::scoped_lock lock(mutex_);
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            if (selected[index] && nodes_[index].error == ERROR_CANCELLED) {
                if (last_error_ == ERROR_SUCCESS) last_error_ = ERROR_CANCELLED;
                return;
            }
        }
        std::optional<std::size_t> target = preferred;
        if (!target) {
            for (const std::size_t index : topological_order_) {
                if (selected[index] && nodes_[index].state == ServiceState::Registered) {
                    target = index;
                    break;
                }
            }
        }
        if (target) {
            Node& node = nodes_[*target];
            node.state = ServiceState::Failed;
            node.error = ERROR_CANCELLED;
            node.failure_chain = {node.descriptor.id};
            failures_.push_back({node.descriptor.id, ERROR_CANCELLED, {}});
        } else {
            failures_.push_back({{}, ERROR_CANCELLED, {}});
        }
        if (last_error_ == ERROR_SUCCESS) last_error_ = ERROR_CANCELLED;
    }

    void StopStartedServices() noexcept {
        for (auto position = topological_order_.rbegin();
             position != topological_order_.rend(); ++position) {
            const std::size_t index = *position;
            {
                std::scoped_lock lock(mutex_);
                Node& node = nodes_[index];
                if (!node.started || !IsReady(node.state)) continue;
                node.started = false;
                node.state = ServiceState::Stopping;
            }

            DWORD error = ERROR_SUCCESS;
            const auto stop_started = std::chrono::steady_clock::now();
            const ServiceDescriptor descriptor = [&] {
                std::scoped_lock lock(mutex_);
                return nodes_[index].descriptor;
            }();
            std::atomic<DWORD> callback_error{ERROR_SUCCESS};
            const auto callback = [this, index, &callback_error] {
                {
                    std::scoped_lock lock(mutex_);
                    nodes_[index].stop_thread_id =
                        static_cast<std::uint64_t>(GetCurrentThreadId());
                }
                try {
                    nodes_[index].descriptor.stop();
                } catch (...) {
                    callback_error.store(ExceptionError(), std::memory_order_release);
                }
            };
            try {
                if (affinity_executors_.stop && descriptor.affinity != ServiceAffinity::Any) {
                    affinity_executors_.stop(descriptor.affinity, callback);
                    std::scoped_lock lock(mutex_);
                    nodes_[index].stop_queue_delay =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - stop_started);
                    error = callback_error.load(std::memory_order_acquire);
                } else {
                    descriptor.stop();
                    std::scoped_lock lock(mutex_);
                    nodes_[index].affinity_bypassed =
                        nodes_[index].affinity_bypassed ||
                        (descriptor.affinity != ServiceAffinity::Any &&
                         !affinity_executors_.stop);
                    nodes_[index].stop_thread_id =
                        static_cast<std::uint64_t>(GetCurrentThreadId());
                }
            } catch (...) {
                error = ExceptionError();
            }
            {
                std::scoped_lock lock(mutex_);
                Node& node = nodes_[index];
                if (error == ERROR_SUCCESS) {
                    node.state = ServiceState::Stopped;
                } else {
                    node.state = ServiceState::Failed;
                    if (node.error == ERROR_SUCCESS) node.error = error;
                    node.failure_chain = {node.descriptor.id};
                    failures_.push_back({node.descriptor.id, error, {}});
                    if (last_error_ == ERROR_SUCCESS) last_error_ = error;
                }
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable operation_condition_;
    std::condition_variable startup_condition_;
    bool operation_active_{};
    std::thread::id operation_thread_{};
    bool registration_closed_{};
    bool built_{};
    bool startup_active_{};
    bool blocking_startup_complete_{};
    bool async_startup_complete_{true};
    DWORD last_error_{ERROR_SUCCESS};
    DWORD blocking_startup_error_{ERROR_SUCCESS};
    DWORD async_startup_error_{ERROR_SUCCESS};
    std::atomic_bool defer_cancel_rollback_{};
    std::stop_source stop_source_;
    std::mutex startup_thread_mutex_;
    std::thread startup_thread_;
    std::vector<Node> nodes_;
    std::vector<std::vector<std::size_t>> adjacency_;
    std::vector<std::vector<std::size_t>> required_dependents_;
    std::vector<std::size_t> topological_order_;
    std::vector<ServiceFailureSnapshot> failures_;
    ServiceAffinityExecutors affinity_executors_;
};

ServiceGraph::ServiceGraph() : impl_(std::make_unique<Impl>()) {}

ServiceGraph::~ServiceGraph() = default;

DWORD ServiceGraph::Register(ServiceDescriptor descriptor) noexcept {
    return impl_->Register(std::move(descriptor));
}

void ServiceGraph::SetAffinityExecutors(ServiceAffinityExecutors executors) noexcept {
    impl_->SetAffinityExecutors(std::move(executors));
}

DWORD ServiceGraph::Build() noexcept {
    return impl_->Build();
}

DWORD ServiceGraph::StartAll(std::stop_token stop_token) noexcept {
    return impl_->StartAll(stop_token);
}

DWORD ServiceGraph::StartService(
    std::string_view service_id, std::stop_token stop_token) noexcept {
    return impl_->StartService(service_id, stop_token);
}

DWORD ServiceGraph::WaitForAsync(std::stop_token stop_token) noexcept {
    return impl_->WaitForAsync(stop_token);
}

void ServiceGraph::CancelStartup() noexcept {
    impl_->CancelStartup();
}

void ServiceGraph::StopAll() noexcept {
    impl_->StopAll();
}

ServiceGraphSnapshot ServiceGraph::Snapshot() const {
    return impl_->Snapshot();
}

}  // namespace anomaly
