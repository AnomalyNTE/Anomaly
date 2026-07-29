#include "anomaly/runtime_session.hpp"

#include "anomaly/service_graph.hpp"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

namespace anomaly {
namespace {

std::atomic_uint64_t g_next_generation{1};

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

}  // namespace

class RuntimeSession::Impl final {
public:
    Impl(RuntimeStartContext start_context, RuntimeSessionOptions options)
        : start_context_(std::move(start_context)),
          options_(std::move(options)),
          dispatchers_(options_.dispatcher_options),
          generation_(g_next_generation.fetch_add(1)) {
        if (options_.services != nullptr) {
            options_.services->SetAffinityExecutors({
                [this](
                    ServiceAffinity affinity,
                    std::function<DWORD(std::stop_token)> callback,
                    std::stop_token stop_token) -> DWORD {
                    if (!callback) return ERROR_INVALID_FUNCTION;
                    if (affinity == ServiceAffinity::Lifecycle ||
                        affinity == ServiceAffinity::Any) {
                        return callback(stop_token);
                    }
                    const ExecutionDomain domain = affinity == ServiceAffinity::Game
                        ? ExecutionDomain::Game
                        : affinity == ServiceAffinity::Render
                            ? ExecutionDomain::Render
                            : ExecutionDomain::Worker;
                    DWORD result = ERROR_SUCCESS;
                    const DWORD invoke_result = dispatchers_.Invoke(
                        domain,
                        [&] { result = callback(stop_token); },
                        std::chrono::seconds(30));
                    return invoke_result == ERROR_SUCCESS ? result : invoke_result;
                },
                [this](ServiceAffinity affinity, std::function<void()> callback) {
                    if (!callback) return;
                    if (affinity == ServiceAffinity::Lifecycle ||
                        affinity == ServiceAffinity::Any) {
                        callback();
                        return;
                    }
                    const ExecutionDomain domain = affinity == ServiceAffinity::Game
                        ? ExecutionDomain::Game
                        : affinity == ServiceAffinity::Render
                            ? ExecutionDomain::Render
                            : ExecutionDomain::Worker;
                    static_cast<void>(dispatchers_.Invoke(
                        domain, std::move(callback), std::chrono::seconds(30)));
                }});
        }
        if (start_context_.external_stop_event == nullptr) return;
        if (!DuplicateHandle(
                GetCurrentProcess(), start_context_.external_stop_event,
                GetCurrentProcess(), &external_stop_event_, SYNCHRONIZE, FALSE, 0)) {
            initialization_error_ = GetLastError();
            start_context_.external_stop_event = nullptr;
        } else {
            start_context_.external_stop_event = external_stop_event_;
        }
    }

    ~Impl() {
        RequestStop();
        Join();
        if (external_stop_event_ != nullptr) CloseHandle(external_stop_event_);
    }

    DWORD Start() noexcept {
        std::unique_lock start_lock(start_mutex_);
        bool cancelled_before_start{};
        {
            std::scoped_lock state_lock(state_mutex_);
            if (stop_requested_.load() &&
                (state_.load() == ANOMALY_RUNTIME_STATE_DORMANT ||
                 state_.load() == ANOMALY_RUNTIME_STATE_STOP_REQUESTED)) {
                last_error_.store(ERROR_CANCELLED);
                cancelled_before_start = true;
            } else if (state_.load() != ANOMALY_RUNTIME_STATE_DORMANT) {
                return ERROR_ALREADY_INITIALIZED;
            } else if (initialization_error_ == ERROR_SUCCESS) {
                state_.store(ANOMALY_RUNTIME_STATE_BOOTSTRAPPING);
            }
        }
        if (cancelled_before_start) {
            start_lock.unlock();
            SetState(ANOMALY_RUNTIME_STATE_STOPPED);
            return ERROR_CANCELLED;
        }
        if (initialization_error_ != ERROR_SUCCESS) {
            stop_requested_.store(true);
            dispatchers_.RequestStop();
            last_error_.store(initialization_error_);
            start_lock.unlock();
            SetState(ANOMALY_RUNTIME_STATE_STOPPED);
            return initialization_error_;
        }

        state_condition_.notify_all();
        try {
            if (external_stop_event_ != nullptr) {
                external_stop_thread_ = std::jthread(
                    [this](std::stop_token stop_token) { MonitorExternalStop(stop_token); });
            }
            lifecycle_thread_ = std::jthread([this](std::stop_token) {
                Lifecycle(lifecycle_stop_source_.get_token());
            });
        } catch (...) {
            const DWORD error = ExceptionError();
            stop_requested_.store(true);
            lifecycle_stop_source_.request_stop();
            dispatchers_.RequestStop();
            StopExternalMonitor();
            last_error_.store(error);
            start_lock.unlock();
            SetState(ANOMALY_RUNTIME_STATE_STOPPED);
            return error;
        }
        return ERROR_SUCCESS;
    }

    void RequestStop() noexcept {
        std::shared_ptr<ServiceGraph> active_services;
        {
            std::scoped_lock lock(state_mutex_);
            stop_requested_.store(true);
            const AnomalyRuntimeState state = state_.load();
            if (state != ANOMALY_RUNTIME_STATE_STOPPING_SERVICES &&
                state != ANOMALY_RUNTIME_STATE_STOPPED) {
                active_services = options_.services;
            }
            if (state != ANOMALY_RUNTIME_STATE_FAILED &&
                state != ANOMALY_RUNTIME_STATE_STOPPING_PLUGINS &&
                state != ANOMALY_RUNTIME_STATE_STOPPING_SERVICES &&
                state != ANOMALY_RUNTIME_STATE_STOPPED) {
                state_.store(ANOMALY_RUNTIME_STATE_STOP_REQUESTED);
            }
        }
        if (active_services) active_services->CancelStartup();
        lifecycle_stop_source_.request_stop();
        // Close caller-owned work immediately, but keep domain workers alive
        // until ServiceGraph stop callbacks have completed.
        dispatchers_.CloseExternalPosts();
        state_condition_.notify_all();
    }

    bool WaitForStop(std::chrono::milliseconds timeout) const noexcept {
        if (state_.load() == ANOMALY_RUNTIME_STATE_STOPPED) return true;
        std::unique_lock lock(state_mutex_);
        const auto stopped = [this] {
            return state_.load() == ANOMALY_RUNTIME_STATE_STOPPED;
        };
        if (timeout == std::chrono::milliseconds::max()) {
            state_condition_.wait(lock, stopped);
            return true;
        }
        return state_condition_.wait_for(lock, timeout, stopped);
    }

    void Join() noexcept {
        std::scoped_lock lock(join_mutex_);
        JoinThread(lifecycle_thread_);
    }

    RuntimeSessionSnapshot Snapshot() const noexcept {
        return RuntimeSessionSnapshot{state_.load(), last_error_.load(), generation_};
    }

    const RuntimeStartContext& StartContext() const noexcept {
        return start_context_;
    }

    RuntimeDispatchers& Dispatchers() noexcept {
        return dispatchers_;
    }

    const RuntimeDispatchers& Dispatchers() const noexcept {
        return dispatchers_;
    }

private:
    void JoinThread(std::jthread& thread) noexcept {
        if (!thread.joinable() || thread.get_id() == std::this_thread::get_id()) return;
        try {
            thread.join();
        } catch (...) {
            DWORD expected = ERROR_SUCCESS;
            const DWORD error = ExceptionError();
            static_cast<void>(last_error_.compare_exchange_strong(expected, error));
            try {
                thread.detach();
            } catch (...) {
            }
        }
    }

    void SetState(AnomalyRuntimeState state) noexcept {
        {
            std::scoped_lock lock(state_mutex_);
            state_.store(state);
        }
        state_condition_.notify_all();
        if (state == ANOMALY_RUNTIME_STATE_STOPPED) NotifyStopped();
    }

    void NotifyStopped() noexcept {
        if (on_stopped_notified_.exchange(true, std::memory_order_acq_rel)) return;
        auto callback = std::move(options_.on_stopped);
        if (!callback) return;
        try {
            callback(Snapshot());
        } catch (...) {
        }
    }

    bool SetStartingState(AnomalyRuntimeState state) noexcept {
        {
            std::scoped_lock lock(state_mutex_);
            if (stop_requested_.load()) return false;
            state_.store(state);
        }
        state_condition_.notify_all();
        return true;
    }

    void ReportFailure(DWORD error) noexcept {
        if (error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
        DWORD expected = ERROR_SUCCESS;
        static_cast<void>(last_error_.compare_exchange_strong(expected, error));
        std::shared_ptr<ServiceGraph> active_services;
        {
            std::scoped_lock lock(state_mutex_);
            stop_requested_.store(true);
            const AnomalyRuntimeState state = state_.load();
            if (state != ANOMALY_RUNTIME_STATE_STOPPING_SERVICES &&
                state != ANOMALY_RUNTIME_STATE_STOPPED) {
                active_services = options_.services;
            }
            if (state != ANOMALY_RUNTIME_STATE_STOPPING_PLUGINS &&
                state != ANOMALY_RUNTIME_STATE_STOPPING_SERVICES &&
                state != ANOMALY_RUNTIME_STATE_STOPPED) {
                state_.store(ANOMALY_RUNTIME_STATE_FAILED);
            }
        }
        if (active_services) active_services->CancelStartup();
        lifecycle_stop_source_.request_stop();
        dispatchers_.CloseExternalPosts();
        state_condition_.notify_all();
    }

    void LaunchWorkers() noexcept {
        try {
            active_workers_.reserve(options_.workers.size());
        } catch (...) {
            ReportFailure(ExceptionError());
            return;
        }
        for (std::size_t index = 0; index < options_.workers.size(); ++index) {
            if (stop_requested_.load()) break;
            try {
                active_workers_.emplace_back([this, index](std::stop_token stop_token) {
                    DWORD result = ERROR_SUCCESS;
                    try {
                        const auto& worker = options_.workers[index];
                        if (!worker.run) {
                            result = ERROR_INVALID_FUNCTION;
                        } else {
                            result = worker.run(stop_token);
                        }
                    } catch (...) {
                        result = ExceptionError();
                    }
                    if (result != ERROR_SUCCESS && !stop_token.stop_requested() &&
                        !stop_requested_.load()) {
                        ReportFailure(result);
                    }
                });
            } catch (...) {
                ReportFailure(ExceptionError());
                break;
            }
        }
    }

    void MonitorExternalStop(std::stop_token stop_token) noexcept {
        while (!stop_token.stop_requested() && !stop_requested_.load()) {
            const DWORD result = WaitForSingleObject(external_stop_event_, 25);
            if (result == WAIT_OBJECT_0) {
                RequestStop();
                return;
            }
            if (result == WAIT_FAILED) {
                ReportFailure(GetLastError());
                return;
            }
        }
    }

    void StopExternalMonitor() noexcept {
        external_stop_thread_.request_stop();
        JoinThread(external_stop_thread_);
    }

    void StartServiceAsyncMonitor() noexcept {
        if (!options_.services) return;
        const std::shared_ptr<ServiceGraph> services = options_.services;
        try {
            service_async_monitor_thread_ = std::jthread(
                [this, services](std::stop_token thread_stop_token) {
                    const DWORD result = services->WaitForAsync(thread_stop_token);
                    if (result == ERROR_SUCCESS ||
                        thread_stop_token.stop_requested() ||
                        stop_requested_.load()) {
                        return;
                    }
                    if (result == ERROR_CANCELLED) {
                        RequestStop();
                    } else {
                        ReportFailure(result);
                    }
                });
        } catch (...) {
            if (!stop_requested_.load()) ReportFailure(ExceptionError());
        }
    }

    void StopServiceAsyncMonitor() noexcept {
        service_async_monitor_thread_.request_stop();
        JoinThread(service_async_monitor_thread_);
    }

    void StopWorkers() noexcept {
        for (auto worker = active_workers_.rbegin(); worker != active_workers_.rend(); ++worker) {
            worker->request_stop();
        }
        for (auto worker = active_workers_.rbegin(); worker != active_workers_.rend(); ++worker) {
            JoinThread(*worker);
        }
        active_workers_.clear();
    }

    void Lifecycle(std::stop_token lifecycle_stop_token) noexcept {
        dispatchers_.BindLifecycleToCurrentThread();
        bool initialization_started{};
        if (!dispatchers_.StartWorkers() && !stop_requested_.load()) {
            ReportFailure(ERROR_GEN_FAILURE);
        }
        if (SetStartingState(ANOMALY_RUNTIME_STATE_STARTING_BLOCKING_SERVICES)) {
            DWORD result = ERROR_SUCCESS;
            initialization_started = true;
            try {
                if (options_.initialize) result = options_.initialize(lifecycle_stop_token);
                if (result == ERROR_SUCCESS && options_.services) {
                    result = options_.services->Build();
                }
                if (result == ERROR_SUCCESS && options_.services) {
                    result = options_.services->StartAll();
                }
            } catch (...) {
                result = ExceptionError();
            }
            if (result != ERROR_SUCCESS && !stop_requested_.load()) {
                ReportFailure(result);
            }
        }

        if (SetStartingState(ANOMALY_RUNTIME_STATE_STARTING_ASYNC_SERVICES)) {
            LaunchWorkers();
        }
        if (SetStartingState(ANOMALY_RUNTIME_STATE_AWAITING_GAME_READINESS)) {
            if (SetStartingState(ANOMALY_RUNTIME_STATE_RUNNING)) {
                StartServiceAsyncMonitor();
                if (!stop_requested_.load()) {
                    dispatchers_.RunLifecycle(lifecycle_stop_token);
                }
                if (!stop_requested_.load()) RequestStop();
            }
        }

        SetState(ANOMALY_RUNTIME_STATE_STOPPING_PLUGINS);
        StopWorkers();
        StopServiceAsyncMonitor();
        // Lifecycle affinity work queued before the stop request cannot be
        // pumped after RunLifecycle returns. Cancel those stale invocations
        // while keeping admission open for service stop callbacks below.
        dispatchers_.CancelQueuedInvocations();
        static_cast<void>(dispatchers_.DrainExternalWork(options_.plugin_stop_timeout));
        // A bounded Invoke may have returned after its callback entered
        // Running. Establish the callback lifetime boundary before plugin
        // generations and their captured state begin to tear down.
        if (!dispatchers_.DrainInvocations(options_.plugin_stop_timeout)) {
            DWORD expected = ERROR_SUCCESS;
            static_cast<void>(last_error_.compare_exchange_strong(expected, ERROR_TIMEOUT));
        }
        if (options_.stop_plugins) {
            try {
                const DWORD result = options_.stop_plugins(options_.plugin_stop_timeout);
                if (result != ERROR_SUCCESS) {
                    DWORD expected = ERROR_SUCCESS;
                    static_cast<void>(last_error_.compare_exchange_strong(expected, result));
                }
            } catch (...) {
                DWORD expected = ERROR_SUCCESS;
                const DWORD error = ExceptionError();
                static_cast<void>(last_error_.compare_exchange_strong(expected, error));
            }
        }
        SetState(ANOMALY_RUNTIME_STATE_STOPPING_SERVICES);
        if (options_.services) options_.services->StopAll();
        dispatchers_.RequestStop();
        dispatchers_.JoinWorkers();
        if (initialization_started && options_.shutdown) {
            try {
                options_.shutdown();
            } catch (...) {
                DWORD expected = ERROR_SUCCESS;
                const DWORD error = ExceptionError();
                static_cast<void>(last_error_.compare_exchange_strong(expected, error));
            }
        }
        options_.workers.clear();
        options_.services.reset();
        options_.initialize = {};
        options_.stop_plugins = {};
        options_.shutdown = {};
        StopExternalMonitor();
        SetState(ANOMALY_RUNTIME_STATE_STOPPED);
    }

    RuntimeStartContext start_context_;
    RuntimeSessionOptions options_;
    RuntimeDispatchers dispatchers_;
    const std::uint64_t generation_;
    HANDLE external_stop_event_{};
    DWORD initialization_error_{ERROR_SUCCESS};

    std::atomic<AnomalyRuntimeState> state_{ANOMALY_RUNTIME_STATE_DORMANT};
    std::atomic<DWORD> last_error_{ERROR_SUCCESS};
    std::atomic_bool stop_requested_{};
    std::atomic_bool on_stopped_notified_{};
    std::stop_source lifecycle_stop_source_;
    std::jthread lifecycle_thread_;
    std::jthread external_stop_thread_;
    std::jthread service_async_monitor_thread_;
    std::vector<std::jthread> active_workers_;

    std::mutex start_mutex_;
    std::mutex join_mutex_;
    mutable std::mutex state_mutex_;
    mutable std::condition_variable state_condition_;
};

RuntimeSession::RuntimeSession(RuntimeStartContext start_context, RuntimeSessionOptions options)
    : impl_(std::make_unique<Impl>(std::move(start_context), std::move(options))) {}

RuntimeSession::~RuntimeSession() = default;

DWORD RuntimeSession::Start() noexcept {
    return impl_->Start();
}

void RuntimeSession::RequestStop() noexcept {
    impl_->RequestStop();
}

bool RuntimeSession::WaitForStop(std::chrono::milliseconds timeout) const noexcept {
    return impl_->WaitForStop(timeout);
}

void RuntimeSession::Join() noexcept {
    impl_->Join();
}

RuntimeSessionSnapshot RuntimeSession::Snapshot() const noexcept {
    return impl_->Snapshot();
}

const RuntimeStartContext& RuntimeSession::StartContext() const noexcept {
    return impl_->StartContext();
}

RuntimeDispatchers& RuntimeSession::Dispatchers() noexcept {
    return impl_->Dispatchers();
}

const RuntimeDispatchers& RuntimeSession::Dispatchers() const noexcept {
    return impl_->Dispatchers();
}

}  // namespace anomaly
