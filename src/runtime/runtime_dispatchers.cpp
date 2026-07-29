#include "anomaly/runtime_dispatchers.hpp"
#include "anomaly/thread_local_value.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <future>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace anomaly {
namespace {

ThreadLocalScalar<const void*> g_runtime_dispatchers_worker_owner;

class WorkerThreadScope final {
public:
    explicit WorkerThreadScope(const void* owner) noexcept
        : previous_(g_runtime_dispatchers_worker_owner.Get()) {
        g_runtime_dispatchers_worker_owner.Set(owner);
    }

    ~WorkerThreadScope() { g_runtime_dispatchers_worker_owner.Set(previous_); }

private:
    const void* previous_{};
};

std::size_t NormalizeWorkerCount(std::size_t count) noexcept {
    return (std::max)(count, std::size_t{1});
}

std::chrono::milliseconds Remaining(
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::milliseconds::max();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds::zero();
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

std::chrono::steady_clock::time_point DeadlineAfter(
    std::chrono::milliseconds timeout) noexcept {
    const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
    const auto now = std::chrono::steady_clock::now();
    if (bounded == std::chrono::milliseconds::max()) {
        return std::chrono::steady_clock::time_point::max();
    }
    const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now);
    return bounded >= available
        ? std::chrono::steady_clock::time_point::max()
        : now + bounded;
}

constexpr std::string_view kInvocationOwner = "anomaly.service-affinity";

class InvocationTracker final {
public:
    struct Ticket final {
        std::promise<DWORD> completion;
        std::atomic_bool settled{};
        std::atomic_bool admission_finished{};
    };

    [[nodiscard]] std::shared_ptr<Ticket> Begin() {
        auto ticket = std::make_shared<Ticket>();
        {
            std::scoped_lock lock(mutex_);
            if (closed_) return {};
            ++active_;
            ++admissions_;
        }
        return ticket;
    }

    void FinishAdmission(const std::shared_ptr<Ticket>& ticket) noexcept {
        if (ticket == nullptr ||
            ticket->admission_finished.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        {
            std::scoped_lock lock(mutex_);
            if (admissions_ != 0) --admissions_;
        }
        condition_.notify_all();
    }

    void Complete(const std::shared_ptr<Ticket>& ticket, DWORD result) noexcept {
        if (ticket == nullptr ||
            ticket->settled.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        FinishAdmission(ticket);
        try {
            ticket->completion.set_value(result);
        } catch (...) {
        }
        {
            std::scoped_lock lock(mutex_);
            if (active_ != 0) --active_;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool Drain(std::chrono::milliseconds timeout) const {
        const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
        std::unique_lock lock(mutex_);
        const auto idle = [this] { return active_ == 0; };
        if (bounded == std::chrono::milliseconds::max()) {
            condition_.wait(lock, idle);
            return true;
        }
        return condition_.wait_for(lock, bounded, idle);
    }

    void Close() noexcept {
        std::unique_lock lock(mutex_);
        closed_ = true;
        condition_.wait(lock, [this] { return admissions_ == 0; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    std::size_t active_{};
    std::size_t admissions_{};
    bool closed_{};
};

// Dispatcher cancellation destroys a queued callback without invoking it.
// Keep completion on a shared state so that both normal execution and that
// destruction path release the tracker ticket exactly once.
struct InvocationCallbackState final {
    std::shared_ptr<InvocationTracker> tracker;
    std::shared_ptr<InvocationTracker::Ticket> ticket;
    RuntimeDispatchers::Callback callback;

    ~InvocationCallbackState() {
        // Release user captures before publishing completion. A drain is the
        // lifetime boundary for those captures, including queued cancellation.
        callback = {};
        if (tracker != nullptr) tracker->Complete(ticket, ERROR_CANCELLED);
    }
};

}  // namespace

class RuntimeDispatchers::Impl final {
public:
    explicit Impl(RuntimeDispatchersOptions options)
        : dispatcher_options_{options.terminal_history_capacity},
          lifecycle_(dispatcher_options_),
          game_(dispatcher_options_),
          render_(dispatcher_options_) {
        invocation_tracker_ = std::make_shared<InvocationTracker>();
        const std::size_t count = NormalizeWorkerCount(options.worker_threads);
        workers_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            workers_.push_back(std::make_unique<Dispatcher>(dispatcher_options_));
        }
    }

    ~Impl() {
        invocation_tracker_->Close();
        {
            std::scoped_lock lock(external_activity_mutex_);
            external_calls_closing_ = true;
        }
        RequestStop();
        {
            std::unique_lock lock(external_activity_mutex_);
            external_activity_condition_.wait(
                lock, [this] { return active_external_calls_ == 0; });
        }
        JoinWorkers();
    }

    bool StartWorkers() noexcept {
        std::scoped_lock lock(lifecycle_mutex_);
        if (workers_started_ || stop_requested_.load(std::memory_order_acquire)) return false;
        try {
            worker_threads_.reserve(workers_.size());
            for (std::size_t lane = 0; lane < workers_.size(); ++lane) {
                worker_threads_.emplace_back([this, lane](std::stop_token stop_token) {
                    WorkerThreadScope worker_scope(this);
                    workers_[lane]->Run(stop_token);
                });
            }
            workers_started_ = true;
            return true;
        } catch (...) {
            for (auto& thread : worker_threads_) thread.request_stop();
            for (auto& thread : worker_threads_) {
                if (thread.joinable()) thread.join();
            }
            worker_threads_.clear();
            stop_requested_.store(true, std::memory_order_release);
            return false;
        }
    }

    void CloseExternalPosts() noexcept {
        accepting_external_posts_.store(false, std::memory_order_release);
        static_cast<void>(lifecycle_.CancelPending());
        static_cast<void>(game_.CancelPending());
        static_cast<void>(render_.CancelPending());
        for (const auto& worker : workers_) {
            static_cast<void>(worker->CancelPending());
        }
    }

    bool DrainExternalWork(std::chrono::milliseconds timeout) noexcept {
        const auto deadline = DeadlineAfter(timeout);
        const auto remaining = [&] { return Remaining(deadline); };
        if (!lifecycle_.DrainAll(remaining())) return false;
        for (const auto& worker : workers_) {
            if (!worker->DrainAll(remaining())) return false;
        }
        if (!game_.DrainAll(remaining())) return false;
        return render_.DrainAll(remaining());
    }

    void RequestStop() noexcept {
        CloseInvocations();
        accepting_external_posts_.store(false, std::memory_order_release);
        stop_requested_.store(true, std::memory_order_release);
        {
            std::scoped_lock lock(post_mutex_);
            for (auto& [owner, generations] : post_states_) {
                static_cast<void>(owner);
                for (auto& [generation, state] : generations) {
                    static_cast<void>(generation);
                    state.closed = true;
                }
            }
        }
        lifecycle_stop_source_.request_stop();
        static_cast<void>(lifecycle_.CancelPending());
        static_cast<void>(game_.CancelPending());
        static_cast<void>(render_.CancelPending());
        for (const auto& worker : workers_) {
            static_cast<void>(worker->CancelPending());
        }
        std::scoped_lock lock(lifecycle_mutex_);
        for (auto& thread : worker_threads_) thread.request_stop();
    }

    void JoinWorkers() noexcept {
        RequestStop();
        if (g_runtime_dispatchers_worker_owner.Get() == this) return;
        std::vector<std::jthread> threads;
        {
            std::unique_lock lock(lifecycle_mutex_);
            if (joining_workers_) {
                workers_joined_condition_.wait(lock, [this] { return !joining_workers_; });
                return;
            }
            if (worker_threads_.empty()) {
                workers_started_ = false;
                return;
            }
            joining_workers_ = true;
            threads.swap(worker_threads_);
        }
        for (auto thread = threads.rbegin(); thread != threads.rend(); ++thread) {
            if (!thread->joinable()) continue;
            thread->join();
        }
        {
            std::scoped_lock lock(lifecycle_mutex_);
            joining_workers_ = false;
            workers_started_ = false;
        }
        workers_joined_condition_.notify_all();
    }

    void BindLifecycleToCurrentThread() noexcept {
        lifecycle_.BindToCurrentThread();
    }

    DomainTaskHandle Post(
        ExecutionDomain domain,
        std::string owner,
        std::uint64_t generation,
        Callback callback) {
        if (stop_requested_.load(std::memory_order_acquire) ||
            !accepting_external_posts_.load(std::memory_order_acquire)) return {};
        if (domain == ExecutionDomain::Worker && !WorkersStarted()) return {};

        PostState* post_state = BeginPost(owner, generation, false);
        if (post_state == nullptr) return {};
        PostActivity post_activity(*this, *post_state);

        const std::size_t lane = domain == ExecutionDomain::Worker
            ? next_worker_.fetch_add(1, std::memory_order_relaxed) % workers_.size()
            : 0;
        Dispatcher* dispatcher = Select(domain, lane);
        if (dispatcher == nullptr) return {};
        DomainTaskHandle result{
            domain, lane,
            dispatcher->Post(std::move(owner), generation, std::move(callback))};
        if (stop_requested_.load(std::memory_order_acquire) ||
            !accepting_external_posts_.load(std::memory_order_acquire) ||
            IsClosed(*post_state)) {
            if (dispatcher->Cancel(result.task)) return {};
        }
        return result;
    }

    DWORD Invoke(
        ExecutionDomain domain, Callback callback,
        std::chrono::milliseconds timeout) {
        if (!callback) return ERROR_INVALID_FUNCTION;
        ExternalActivity activity(*this);
        if (!activity) return ERROR_CANCELLED;
        if (stop_requested_.load(std::memory_order_acquire)) return ERROR_CANCELLED;
        Dispatcher* dispatcher = Select(domain, 0);
        if (dispatcher == nullptr) return ERROR_INVALID_PARAMETER;
        const auto tracker = invocation_tracker_;
        const auto ticket = tracker->Begin();
        if (ticket == nullptr) return ERROR_CANCELLED;
        if (dispatcher->IsCurrentThread()) {
            tracker->FinishAdmission(ticket);
            DWORD error = ERROR_SUCCESS;
            try {
                callback();
            } catch (...) {
                error = ERROR_UNHANDLED_EXCEPTION;
            }
            tracker->Complete(ticket, error);
            return error;
        }

        std::future<DWORD> result;
        try {
            result = ticket->completion.get_future();
        } catch (...) {
            tracker->Complete(ticket, ERROR_UNHANDLED_EXCEPTION);
            return ERROR_UNHANDLED_EXCEPTION;
        }
        DomainTaskHandle handle;
        try {
            auto callback_state = std::make_shared<InvocationCallbackState>();
            callback_state->tracker = tracker;
            callback_state->ticket = ticket;
            callback_state->callback = std::move(callback);
            handle = PostInternal(
                domain, std::string(kInvocationOwner), 0,
                [callback_state = std::move(callback_state)]() mutable {
                    DWORD error = ERROR_SUCCESS;
                    try {
                        if (callback_state->callback) callback_state->callback();
                    } catch (...) {
                        error = ERROR_UNHANDLED_EXCEPTION;
                    }
                    callback_state->callback = {};
                    callback_state->tracker->Complete(callback_state->ticket, error);
                });
        } catch (...) {
            tracker->FinishAdmission(ticket);
            tracker->Complete(ticket, ERROR_UNHANDLED_EXCEPTION);
            return ERROR_UNHANDLED_EXCEPTION;
        }
        tracker->FinishAdmission(ticket);
        if (!handle) {
            tracker->Complete(ticket, ERROR_NOT_READY);
            return ERROR_NOT_READY;
        }
        const auto bounded = timeout < std::chrono::milliseconds::zero()
            ? std::chrono::milliseconds::zero() : timeout;
        if (bounded == std::chrono::milliseconds::max()) {
            result.wait();
        } else if (result.wait_for(bounded) != std::future_status::ready) {
            if (Cancel(handle)) tracker->Complete(ticket, ERROR_CANCELLED);
            return ERROR_TIMEOUT;
        }
        return result.get();
    }

    bool DrainInvocations(std::chrono::milliseconds timeout) {
        ExternalActivity activity(*this);
        if (!activity) return false;
        const auto deadline = DeadlineAfter(timeout);
        if (!Drain(kInvocationOwner, 0, Remaining(deadline))) return false;
        return invocation_tracker_->Drain(Remaining(deadline));
    }

    void CloseInvocations() noexcept {
        invocation_tracker_->Close();
        CancelQueuedInvocations();
    }

    void CancelQueuedInvocations() noexcept {
        try {
            static_cast<void>(lifecycle_.CancelOwnerGeneration(kInvocationOwner, 0));
            static_cast<void>(game_.CancelOwnerGeneration(kInvocationOwner, 0));
            static_cast<void>(render_.CancelOwnerGeneration(kInvocationOwner, 0));
            for (const auto& worker : workers_) {
                static_cast<void>(worker->CancelOwnerGeneration(kInvocationOwner, 0));
            }
        } catch (...) {
            // A later RequestStop or dispatcher destruction will perform the
            // broad cancellation pass if an individual lane rejects cleanup.
        }
    }

    bool Cancel(DomainTaskHandle handle) {
        Dispatcher* dispatcher = Select(handle.domain, handle.lane);
        return dispatcher != nullptr && dispatcher->Cancel(handle.task);
    }

    DomainTaskHandle PostInternal(
        ExecutionDomain domain, std::string owner, std::uint64_t generation,
        Callback callback) {
        if (stop_requested_.load(std::memory_order_acquire)) return {};
        if (domain == ExecutionDomain::Worker && !WorkersStarted()) return {};

        PostState* post_state = BeginPost(owner, generation, true);
        if (post_state == nullptr) return {};
        PostActivity post_activity(*this, *post_state);
        const std::size_t lane = domain == ExecutionDomain::Worker
            ? next_worker_.fetch_add(1, std::memory_order_relaxed) % workers_.size()
            : 0;
        Dispatcher* dispatcher = Select(domain, lane);
        if (dispatcher == nullptr) return {};
        return DomainTaskHandle{
            domain, lane,
            dispatcher->Post(std::move(owner), generation, std::move(callback))};
    }

    std::size_t CancelOwnerGeneration(
        std::string_view owner, std::uint64_t generation) {
        const std::string owner_key(owner);
        bool leader{};
        {
            std::scoped_lock transition_lock(generation_transition_mutex_);
            CloseGeneration(owner_key, generation);
            const auto existing_intent = generation_intents_.find(owner_key);
            if (existing_intent != generation_intents_.end()) {
                ++existing_intent->second.revision;
                if (existing_intent->second.generation == generation) {
                    existing_intent->second.open = false;
                }
                if (!existing_intent->second.leader_active) {
                    existing_intent->second.leader_active = true;
                    leader = true;
                }
            }
        }

        try {
            std::size_t cancelled = lifecycle_.CancelOwnerGeneration(owner, generation);
            cancelled += game_.CancelOwnerGeneration(owner, generation);
            cancelled += render_.CancelOwnerGeneration(owner, generation);
            for (const auto& worker : workers_) {
                cancelled += worker->CancelOwnerGeneration(owner, generation);
            }
            if (leader) ReconcileGeneration(owner_key);
            return cancelled;
        } catch (...) {
            if (leader) ReleaseGenerationLeader(owner_key);
            throw;
        }
    }

    void SetGeneration(std::string owner, std::uint64_t generation) {
        bool leader{};
        {
            std::scoped_lock transition_lock(generation_transition_mutex_);
            CloseAllGenerations(owner, generation);
            GenerationIntent& intent = generation_intents_[owner];
            intent.generation = generation;
            intent.open = true;
            ++intent.revision;
            if (!intent.leader_active) {
                intent.leader_active = true;
                leader = true;
            }
        }
        if (!leader) return;

        try {
            ReconcileGeneration(owner);
        } catch (...) {
            ReleaseGenerationLeader(owner);
            throw;
        }
    }

    void ApplyGeneration(const std::string& owner, std::uint64_t generation) {
        lifecycle_.SetGeneration(owner, generation);
        game_.SetGeneration(owner, generation);
        render_.SetGeneration(owner, generation);
        for (const auto& worker : workers_) worker->SetGeneration(owner, generation);
    }

    bool Drain(
        std::string_view owner,
        std::uint64_t generation,
        std::chrono::milliseconds timeout) {
        const auto deadline = DeadlineAfter(timeout);
        PostState* post_state = GetPostState(owner, generation);
        for (;;) {
            std::uint64_t epoch{};
            {
                std::unique_lock lock(post_mutex_);
                if (!WaitForInactivePost(lock, *post_state, deadline)) return false;
                epoch = post_state->epoch;
            }
            if (!lifecycle_.Drain(owner, generation, Remaining(deadline))) return false;
            for (const auto& worker : workers_) {
                if (!worker->Drain(owner, generation, Remaining(deadline))) return false;
            }
            if (!game_.Drain(owner, generation, Remaining(deadline))) return false;
            if (!render_.Drain(owner, generation, Remaining(deadline))) return false;
            {
                std::unique_lock lock(post_mutex_);
                if (!WaitForInactivePost(lock, *post_state, deadline)) return false;
                if (post_state->epoch == epoch) return true;
            }
        }
    }

    void RunLifecycle(std::stop_token stop_token) noexcept {
        ExternalActivity activity(*this);
        if (!activity) return;
        std::stop_callback propagate_stop(
            stop_token, [this] { lifecycle_stop_source_.request_stop(); });
        lifecycle_.Run(lifecycle_stop_source_.get_token());
    }

    std::size_t PumpGame(std::size_t max_callbacks) noexcept {
        ExternalActivity activity(*this);
        if (!activity) return 0;
        return game_.Pump(max_callbacks);
    }

    std::size_t PumpRender(std::size_t max_callbacks) noexcept {
        ExternalActivity activity(*this);
        if (!activity) return 0;
        return render_.Pump(max_callbacks);
    }

    std::optional<TaskSnapshot> GetTask(DomainTaskHandle handle) const {
        const Dispatcher* dispatcher = Select(handle.domain, handle.lane);
        return dispatcher == nullptr ? std::nullopt : dispatcher->GetTask(handle.task);
    }

    std::thread::id BoundThread(ExecutionDomain domain, std::size_t lane) const noexcept {
        const Dispatcher* dispatcher = Select(domain, lane);
        return dispatcher == nullptr ? std::thread::id{} : dispatcher->BoundThread();
    }

    std::size_t WorkerCount() const noexcept { return workers_.size(); }

    bool IsAccepting() const noexcept {
        return accepting_external_posts_.load(std::memory_order_acquire) &&
            !stop_requested_.load(std::memory_order_acquire);
    }

private:
    struct PostState {
        std::size_t active{};
        std::uint64_t epoch{};
        bool closed{};
    };

    struct GenerationIntent {
        std::uint64_t generation{};
        std::uint64_t revision{};
        bool open{};
        bool leader_active{};
    };

    class PostActivity final {
    public:
        PostActivity(Impl& owner, PostState& state) noexcept
            : owner_(owner), state_(state) {}

        ~PostActivity() {
            {
                std::scoped_lock lock(owner_.post_mutex_);
                --state_.active;
                ++state_.epoch;
            }
            owner_.post_condition_.notify_all();
        }

        PostActivity(const PostActivity&) = delete;
        PostActivity& operator=(const PostActivity&) = delete;

    private:
        Impl& owner_;
        PostState& state_;
    };

    class ExternalActivity final {
    public:
        explicit ExternalActivity(Impl& owner) noexcept : owner_(&owner) {
            std::scoped_lock lock(owner_->external_activity_mutex_);
            if (owner_->external_calls_closing_) {
                owner_ = nullptr;
            } else {
                ++owner_->active_external_calls_;
            }
        }

        ~ExternalActivity() {
            if (owner_ == nullptr) return;
            {
                std::scoped_lock lock(owner_->external_activity_mutex_);
                --owner_->active_external_calls_;
            }
            owner_->external_activity_condition_.notify_all();
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }

        ExternalActivity(const ExternalActivity&) = delete;
        ExternalActivity& operator=(const ExternalActivity&) = delete;

    private:
        Impl* owner_{};
    };

    bool WorkersStarted() const noexcept {
        std::scoped_lock lock(lifecycle_mutex_);
        return workers_started_;
    }

    PostState* BeginPost(
        std::string_view owner, std::uint64_t generation, bool internal) {
        std::scoped_lock lock(post_mutex_);
        if (stop_requested_.load(std::memory_order_acquire) ||
            (!internal && !accepting_external_posts_.load(std::memory_order_acquire))) {
            return nullptr;
        }
        PostState& state = post_states_[std::string(owner)][generation];
        if (state.closed) return nullptr;
        ++state.active;
        return &state;
    }

    PostState* GetPostState(std::string_view owner, std::uint64_t generation) {
        std::scoped_lock lock(post_mutex_);
        return &post_states_[std::string(owner)][generation];
    }

    bool IsClosed(const PostState& state) const noexcept {
        std::scoped_lock lock(post_mutex_);
        return state.closed;
    }

    void CloseGeneration(std::string_view owner, std::uint64_t generation) {
        std::scoped_lock lock(post_mutex_);
        post_states_[std::string(owner)][generation].closed = true;
    }

    void CloseAllGenerations(std::string_view owner, std::uint64_t generation) {
        std::scoped_lock lock(post_mutex_);
        auto& generations = post_states_[std::string(owner)];
        for (auto& [stored_generation, state] : generations) {
            static_cast<void>(stored_generation);
            state.closed = true;
        }
        generations[generation].closed = true;
    }

    void ReconcileGeneration(const std::string& owner) {
        for (;;) {
            GenerationIntent intended;
            {
                std::scoped_lock transition_lock(generation_transition_mutex_);
                intended = generation_intents_.at(owner);
            }
            ApplyGeneration(owner, intended.generation);

            std::scoped_lock transition_lock(generation_transition_mutex_);
            GenerationIntent& current = generation_intents_.at(owner);
            if (current.revision != intended.revision) continue;
            {
                std::scoped_lock lock(post_mutex_);
                auto& generations = post_states_.at(owner);
                for (auto& [stored_generation, state] : generations) {
                    static_cast<void>(stored_generation);
                    state.closed = true;
                }
                if (current.open && !stop_requested_.load(std::memory_order_acquire)) {
                    generations.at(current.generation).closed = false;
                }
            }
            current.leader_active = false;
            return;
        }
    }

    void ReleaseGenerationLeader(const std::string& owner) noexcept {
        std::scoped_lock transition_lock(generation_transition_mutex_);
        const auto intent = generation_intents_.find(owner);
        if (intent != generation_intents_.end()) intent->second.leader_active = false;
    }

    bool WaitForInactivePost(
        std::unique_lock<std::mutex>& lock,
        const PostState& state,
        std::chrono::steady_clock::time_point deadline) {
        const auto inactive = [&] { return state.active == 0; };
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            post_condition_.wait(lock, inactive);
            return true;
        }
        return post_condition_.wait_until(lock, deadline, inactive);
    }

    Dispatcher* Select(ExecutionDomain domain, std::size_t lane) noexcept {
        return const_cast<Dispatcher*>(std::as_const(*this).Select(domain, lane));
    }

    const Dispatcher* Select(ExecutionDomain domain, std::size_t lane) const noexcept {
        switch (domain) {
            case ExecutionDomain::Lifecycle: return lane == 0 ? &lifecycle_ : nullptr;
            case ExecutionDomain::Game: return lane == 0 ? &game_ : nullptr;
            case ExecutionDomain::Render: return lane == 0 ? &render_ : nullptr;
            case ExecutionDomain::Worker:
                return lane < workers_.size() ? workers_[lane].get() : nullptr;
        }
        return nullptr;
    }

    const DispatcherOptions dispatcher_options_;
    Dispatcher lifecycle_;
    Dispatcher game_;
    Dispatcher render_;
    std::vector<std::unique_ptr<Dispatcher>> workers_;
    std::vector<std::jthread> worker_threads_;
    std::atomic_size_t next_worker_{};
    std::atomic_bool stop_requested_{};
    std::atomic_bool accepting_external_posts_{true};
    std::stop_source lifecycle_stop_source_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable workers_joined_condition_;
    bool workers_started_{};
    bool joining_workers_{};
    std::mutex external_activity_mutex_;
    std::condition_variable external_activity_condition_;
    std::size_t active_external_calls_{};
    bool external_calls_closing_{};
    mutable std::mutex post_mutex_;
    std::mutex generation_transition_mutex_;
    std::condition_variable post_condition_;
    std::unordered_map<std::string, std::unordered_map<std::uint64_t, PostState>> post_states_;
    std::unordered_map<std::string, GenerationIntent> generation_intents_;
    std::shared_ptr<InvocationTracker> invocation_tracker_;
};

RuntimeDispatchers::RuntimeDispatchers(RuntimeDispatchersOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

RuntimeDispatchers::~RuntimeDispatchers() = default;

bool RuntimeDispatchers::StartWorkers() noexcept {
    return impl_->StartWorkers();
}

void RuntimeDispatchers::CloseExternalPosts() noexcept {
    impl_->CloseExternalPosts();
}

bool RuntimeDispatchers::DrainExternalWork(std::chrono::milliseconds timeout) noexcept {
    return impl_->DrainExternalWork(timeout);
}

void RuntimeDispatchers::RequestStop() noexcept {
    impl_->RequestStop();
}

void RuntimeDispatchers::JoinWorkers() noexcept {
    impl_->JoinWorkers();
}

void RuntimeDispatchers::BindLifecycleToCurrentThread() noexcept {
    impl_->BindLifecycleToCurrentThread();
}

DomainTaskHandle RuntimeDispatchers::Post(
    ExecutionDomain domain,
    std::string owner,
    std::uint64_t generation,
    Callback callback) {
    return impl_->Post(domain, std::move(owner), generation, std::move(callback));
}

DWORD RuntimeDispatchers::Invoke(
    ExecutionDomain domain, Callback callback, std::chrono::milliseconds timeout) {
    return impl_->Invoke(domain, std::move(callback), timeout);
}

bool RuntimeDispatchers::DrainInvocations(std::chrono::milliseconds timeout) {
    return impl_->DrainInvocations(timeout);
}

void RuntimeDispatchers::CancelQueuedInvocations() noexcept {
    impl_->CancelQueuedInvocations();
}

void RuntimeDispatchers::CloseInvocations() noexcept {
    impl_->CloseInvocations();
}

bool RuntimeDispatchers::Cancel(DomainTaskHandle handle) {
    return impl_->Cancel(handle);
}

std::size_t RuntimeDispatchers::CancelOwnerGeneration(
    std::string_view owner, std::uint64_t generation) {
    return impl_->CancelOwnerGeneration(owner, generation);
}

void RuntimeDispatchers::SetGeneration(std::string owner, std::uint64_t generation) {
    impl_->SetGeneration(std::move(owner), generation);
}

bool RuntimeDispatchers::Drain(
    std::string_view owner,
    std::uint64_t generation,
    std::chrono::milliseconds timeout) {
    return impl_->Drain(owner, generation, timeout);
}

void RuntimeDispatchers::RunLifecycle(std::stop_token stop_token) noexcept {
    impl_->RunLifecycle(stop_token);
}

std::size_t RuntimeDispatchers::PumpGame(std::size_t max_callbacks) noexcept {
    return impl_->PumpGame(max_callbacks);
}

std::size_t RuntimeDispatchers::PumpRender(std::size_t max_callbacks) noexcept {
    return impl_->PumpRender(max_callbacks);
}

std::optional<TaskSnapshot> RuntimeDispatchers::GetTask(DomainTaskHandle handle) const {
    return impl_->GetTask(handle);
}

std::thread::id RuntimeDispatchers::BoundThread(
    ExecutionDomain domain, std::size_t lane) const noexcept {
    return impl_->BoundThread(domain, lane);
}

std::size_t RuntimeDispatchers::WorkerCount() const noexcept {
    return impl_->WorkerCount();
}

bool RuntimeDispatchers::IsAccepting() const noexcept {
    return impl_->IsAccepting();
}

}  // namespace anomaly
