#include "anomaly/dispatcher.hpp"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace anomaly {
namespace {

struct Task {
    TaskHandle handle;
    std::string owner;
    std::uint64_t generation{};
    TaskState state{TaskState::Queued};
    Dispatcher::Callback callback;
    std::exception_ptr failure;
    std::string failure_message;
    bool cancellation_cleanup_pending{};
};

using TaskPtr = std::shared_ptr<Task>;
using GenerationCounts = std::unordered_map<std::uint64_t, std::size_t>;
using OwnerCounts = std::unordered_map<std::string, GenerationCounts>;

std::size_t CountFor(
    const OwnerCounts& counts, std::string_view owner, std::uint64_t generation) noexcept {
    for (const auto& [stored_owner, generations] : counts) {
        if (stored_owner != owner) continue;
        const auto found = generations.find(generation);
        return found == generations.end() ? 0 : found->second;
    }
    return 0;
}

void Decrement(OwnerCounts& counts, const Task& task) noexcept {
    const auto owner = counts.find(task.owner);
    assert(owner != counts.end());
    if (owner == counts.end()) return;
    const auto generation = owner->second.find(task.generation);
    assert(generation != owner->second.end() && generation->second != 0);
    if (generation == owner->second.end() || generation->second == 0) return;
    --generation->second;
}

void Increment(OwnerCounts& counts, const Task& task) noexcept {
    const auto owner = counts.find(task.owner);
    assert(owner != counts.end());
    if (owner == counts.end()) return;
    const auto generation = owner->second.find(task.generation);
    assert(generation != owner->second.end());
    if (generation == owner->second.end()) return;
    ++generation->second;
}

std::string CurrentExceptionMessage() noexcept {
    try {
        throw;
    } catch (const std::exception& error) {
        try {
            return error.what();
        } catch (...) {
            return {};
        }
    } catch (...) {
        try {
            return "non-standard exception";
        } catch (...) {
            return {};
        }
    }
}

struct CancelledCallback {
    TaskPtr task;
    Dispatcher::Callback callback;
};

}  // namespace

class Dispatcher::Impl final {
public:
    explicit Impl(DispatcherOptions options)
        : terminal_history_capacity_(options.terminal_history_capacity) {}

    TaskHandle Post(std::string owner, std::uint64_t generation, Callback callback) {
        auto task = std::make_shared<Task>();
        task->owner = std::move(owner);
        task->generation = generation;
        task->callback = std::move(callback);

        bool stale{};
        {
            std::scoped_lock lock(mutex_);
            task->handle = NextHandleLocked();

            const auto current = current_generations_.find(task->owner);
            stale = current != current_generations_.end() && current->second != generation;

            auto& queued = queued_counts_[task->owner][generation];
            static_cast<void>(in_flight_counts_[task->owner][generation]);

            task->state = stale ? TaskState::Cancelled : TaskState::Queued;
            tasks_.emplace(task->handle.value, task);
            try {
                if (!stale) queue_.push_back(task);
            } catch (...) {
                tasks_.erase(task->handle.value);
                throw;
            }
            ++queued;
        }

        condition_.notify_one();

        if (stale) {
            Callback discarded = std::move(task->callback);
            discarded = {};
            {
                std::scoped_lock lock(mutex_);
                Decrement(queued_counts_, *task);
                RecordTerminalLocked(task);
            }
            condition_.notify_all();
        }
        return task->handle;
    }

    bool Cancel(TaskHandle handle) {
        Callback discarded;
        TaskPtr task;
        {
            std::scoped_lock lock(mutex_);
            const auto found = tasks_.find(handle.value);
            if (found == tasks_.end() || found->second->state != TaskState::Queued) return false;
            task = found->second;
            task->state = TaskState::Cancelled;
            discarded = std::move(task->callback);
        }

        discarded = {};
        {
            std::scoped_lock lock(mutex_);
            Decrement(queued_counts_, *task);
            RecordTerminalLocked(task);
        }
        condition_.notify_all();
        return true;
    }

    std::size_t CancelPending() noexcept {
        std::deque<TaskPtr> detached;
        std::size_t cancelled{};
        {
            std::scoped_lock lock(mutex_);
            detached.swap(queue_);
            for (const auto& task : detached) {
                if (task->state != TaskState::Queued) continue;
                task->state = TaskState::Cancelled;
                task->cancellation_cleanup_pending = true;
                ++cancelled;
            }
        }
        for (const auto& task : detached) {
            if (!task->cancellation_cleanup_pending) continue;
            Callback discarded = std::move(task->callback);
            discarded = {};
            {
                std::scoped_lock lock(mutex_);
                task->cancellation_cleanup_pending = false;
                Decrement(queued_counts_, *task);
                RecordTerminalLocked(task);
            }
        }
        if (cancelled != 0) condition_.notify_all();
        return cancelled;
    }

    bool DrainAll(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const auto idle = [this] {
            const auto count = [](const OwnerCounts& values) {
                std::size_t total{};
                for (const auto& [owner, generations] : values) {
                    static_cast<void>(owner);
                    for (const auto& [generation, value] : generations) {
                        static_cast<void>(generation);
                        total += value;
                    }
                }
                return total;
            };
            return count(queued_counts_) == 0 && count(in_flight_counts_) == 0;
        };
        if (timeout == std::chrono::milliseconds::max()) {
            condition_.wait(lock, idle);
            return true;
        }
        return condition_.wait_for(
            lock, (std::max)(timeout, std::chrono::milliseconds::zero()), idle);
    }

    std::size_t CancelOwnerGeneration(std::string_view owner, std::uint64_t generation) {
        std::vector<CancelledCallback> cancelled;
        {
            std::scoped_lock lock(mutex_);
            cancelled.reserve(queue_.size());
            for (const auto& task : queue_) {
                if (task->state != TaskState::Queued || task->owner != owner ||
                    task->generation != generation) {
                    continue;
                }
                task->state = TaskState::Cancelled;
                cancelled.push_back({task, std::move(task->callback)});
            }
        }
        FinishCancellation(cancelled);
        return cancelled.size();
    }

    void SetGeneration(std::string owner, std::uint64_t generation) {
        std::vector<CancelledCallback> cancelled;
        {
            std::scoped_lock lock(mutex_);
            cancelled.reserve(queue_.size());
            current_generations_[owner] = generation;
            for (const auto& task : queue_) {
                if (task->state != TaskState::Queued || task->owner != owner ||
                    task->generation == generation) {
                    continue;
                }
                task->state = TaskState::Cancelled;
                cancelled.push_back({task, std::move(task->callback)});
            }
        }
        FinishCancellation(cancelled);
    }

    bool Drain(
        std::string_view owner,
        std::uint64_t generation,
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const auto drained = [&] {
            return CountFor(queued_counts_, owner, generation) == 0 &&
                CountFor(in_flight_counts_, owner, generation) == 0;
        };
        if (timeout == std::chrono::milliseconds::max()) {
            condition_.wait(lock, drained);
            return true;
        }
        return condition_.wait_for(
            lock, (std::max)(timeout, std::chrono::milliseconds::zero()), drained);
    }

    void BindToCurrentThread() noexcept {
        const auto current = std::this_thread::get_id();
        std::scoped_lock lock(mutex_);
        if (bound_thread_ == std::thread::id{}) bound_thread_ = current;
        assert(bound_thread_ == current && "Dispatcher used from the wrong thread");
    }

    bool IsCurrentThread() const noexcept {
        std::scoped_lock lock(mutex_);
        return bound_thread_ != std::thread::id{} &&
            bound_thread_ == std::this_thread::get_id();
    }

    std::thread::id BoundThread() const noexcept {
        std::scoped_lock lock(mutex_);
        return bound_thread_;
    }

    std::size_t Pump(std::size_t max_callbacks) noexcept {
        if (max_callbacks == 0) return 0;
        BindToCurrentThread();
        {
            std::scoped_lock lock(mutex_);
            if (bound_thread_ != std::this_thread::get_id()) return 0;
        }

        std::size_t invoked{};
        while (invoked < max_callbacks) {
            TaskPtr task;
            Callback callback;
            bool stale{};
            {
                std::scoped_lock lock(mutex_);
                while (!queue_.empty() && queue_.front()->state != TaskState::Queued) {
                    queue_.pop_front();
                }
                if (queue_.empty()) break;

                task = queue_.front();
                queue_.pop_front();
                const auto current = current_generations_.find(task->owner);
                stale = current != current_generations_.end() &&
                    current->second != task->generation;
                if (stale) {
                    task->state = TaskState::Cancelled;
                } else {
                    task->state = TaskState::Running;
                    Decrement(queued_counts_, *task);
                    Increment(in_flight_counts_, *task);
                }
                callback = std::move(task->callback);
            }

            if (stale) {
                callback = {};
                {
                    std::scoped_lock lock(mutex_);
                    Decrement(queued_counts_, *task);
                    RecordTerminalLocked(task);
                }
                condition_.notify_all();
                continue;
            }

            std::exception_ptr failure;
            std::string failure_message;
            try {
                callback();
            } catch (...) {
                failure = std::current_exception();
                failure_message = CurrentExceptionMessage();
            }
            callback = {};

            {
                std::scoped_lock lock(mutex_);
                task->failure = failure;
                task->failure_message = std::move(failure_message);
                task->state = failure ? TaskState::Failed : TaskState::Completed;
                Decrement(in_flight_counts_, *task);
                RecordTerminalLocked(task);
            }
            condition_.notify_all();
            ++invoked;
        }
        return invoked;
    }

    void Run(std::stop_token stop_token) noexcept {
        BindToCurrentThread();
        while (!stop_token.stop_requested()) {
            static_cast<void>(Pump((std::numeric_limits<std::size_t>::max)()));
            std::unique_lock lock(mutex_);
            static_cast<void>(condition_.wait(lock, stop_token, [this] {
                return std::ranges::any_of(queue_, [](const TaskPtr& task) {
                    return task->state == TaskState::Queued;
                });
            }));
        }
    }

    std::optional<TaskSnapshot> GetTask(TaskHandle handle) const {
        std::scoped_lock lock(mutex_);
        const auto found = tasks_.find(handle.value);
        if (found == tasks_.end()) return std::nullopt;
        const Task& task = *found->second;
        return TaskSnapshot{
            task.handle, task.owner, task.generation, task.state, task.failure_message};
    }

    std::optional<TaskState> GetState(TaskHandle handle) const {
        std::scoped_lock lock(mutex_);
        const auto found = tasks_.find(handle.value);
        if (found == tasks_.end()) return std::nullopt;
        return found->second->state;
    }

    std::vector<TaskFailure> Failures() const {
        std::vector<TaskFailure> failures;
        {
            std::scoped_lock lock(mutex_);
            for (const auto& [id, task] : tasks_) {
                static_cast<void>(id);
                if (task->state != TaskState::Failed) continue;
                failures.push_back(TaskFailure{
                    task->handle,
                    task->owner,
                    task->generation,
                    task->failure_message,
                    task->failure});
            }
        }
        std::ranges::sort(failures, {}, [](const TaskFailure& failure) {
            return failure.handle.value;
        });
        return failures;
    }

    std::size_t TrackedTaskCount() const noexcept {
        std::scoped_lock lock(mutex_);
        return tasks_.size();
    }

private:
    TaskHandle NextHandleLocked() noexcept {
        TaskHandle result;
        do {
            result.value = next_handle_++;
        } while (result.value == 0 || tasks_.contains(result.value));
        return result;
    }

    void FinishCancellation(std::vector<CancelledCallback>& cancelled) noexcept {
        for (auto& entry : cancelled) entry.callback = {};
        {
            std::scoped_lock lock(mutex_);
            for (const auto& entry : cancelled) {
                Decrement(queued_counts_, *entry.task);
                RecordTerminalLocked(entry.task);
            }
        }
        if (!cancelled.empty()) condition_.notify_all();
    }

    void RecordTerminalLocked(const TaskPtr& task) noexcept {
        try {
            terminal_history_.push_back(task->handle.value);
        } catch (...) {
            tasks_.erase(task->handle.value);
            return;
        }
        while (terminal_history_.size() > terminal_history_capacity_) {
            const std::uint64_t expired = terminal_history_.front();
            terminal_history_.pop_front();
            const auto found = tasks_.find(expired);
            if (found != tasks_.end() &&
                found->second->state != TaskState::Queued &&
                found->second->state != TaskState::Running) {
                tasks_.erase(found);
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<TaskPtr> queue_;
    std::deque<std::uint64_t> terminal_history_;
    std::unordered_map<std::uint64_t, TaskPtr> tasks_;
    std::unordered_map<std::string, std::uint64_t> current_generations_;
    OwnerCounts queued_counts_;
    OwnerCounts in_flight_counts_;
    std::thread::id bound_thread_;
    std::uint64_t next_handle_{1};
    const std::size_t terminal_history_capacity_;
};

Dispatcher::Dispatcher(DispatcherOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

Dispatcher::~Dispatcher() = default;

TaskHandle Dispatcher::Post(std::string owner, std::uint64_t generation, Callback callback) {
    return impl_->Post(std::move(owner), generation, std::move(callback));
}

bool Dispatcher::Cancel(TaskHandle handle) {
    return impl_->Cancel(handle);
}

std::size_t Dispatcher::CancelPending() noexcept {
    return impl_->CancelPending();
}

bool Dispatcher::DrainAll(std::chrono::milliseconds timeout) {
    return impl_->DrainAll(timeout);
}

std::size_t Dispatcher::CancelOwnerGeneration(
    std::string_view owner, std::uint64_t generation) {
    return impl_->CancelOwnerGeneration(owner, generation);
}

void Dispatcher::SetGeneration(std::string owner, std::uint64_t generation) {
    impl_->SetGeneration(std::move(owner), generation);
}

bool Dispatcher::Drain(
    std::string_view owner,
    std::uint64_t generation,
    std::chrono::milliseconds timeout) {
    return impl_->Drain(owner, generation, timeout);
}

void Dispatcher::BindToCurrentThread() noexcept {
    impl_->BindToCurrentThread();
}

bool Dispatcher::IsCurrentThread() const noexcept {
    return impl_->IsCurrentThread();
}

std::thread::id Dispatcher::BoundThread() const noexcept {
    return impl_->BoundThread();
}

std::size_t Dispatcher::Pump(std::size_t max_callbacks) noexcept {
    return impl_->Pump(max_callbacks);
}

void Dispatcher::Run(std::stop_token stop_token) noexcept {
    impl_->Run(stop_token);
}

std::optional<TaskSnapshot> Dispatcher::GetTask(TaskHandle handle) const {
    return impl_->GetTask(handle);
}

std::optional<TaskState> Dispatcher::GetState(TaskHandle handle) const {
    return impl_->GetState(handle);
}

std::vector<TaskFailure> Dispatcher::Failures() const {
    return impl_->Failures();
}

std::size_t Dispatcher::TrackedTaskCount() const noexcept {
    return impl_->TrackedTaskCount();
}

}  // namespace anomaly
