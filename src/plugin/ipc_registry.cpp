#include "anomaly/ipc_registry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace anomaly {
namespace {

constexpr std::uint32_t kMaximumPayloadBytes = 4U * 1024U * 1024U;
constexpr std::uint32_t kMaximumQueueDepth = 4096U;
constexpr std::uint32_t kMaximumTimeoutMilliseconds = 30'000U;
constexpr std::size_t kMaximumMetricSamples = 1024U;

AnomalyStatusV1 Status(
    const std::uint32_t code,
    const AnomalyIpcErrorV1 detail = ANOMALY_IPC_ERROR_V1_NONE,
    const char* message = nullptr) noexcept {
    return {code, static_cast<std::uint32_t>(detail),
            {message, message == nullptr ? 0U : std::strlen(message)}};
}

bool IsReverseDomainId(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 192 || value.front() == '.' || value.back() == '.' ||
        value.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t dots{};
    bool segment_start = true;
    for (const char character : value) {
        if (character == '.') {
            if (segment_start) return false;
            ++dots;
            segment_start = true;
            continue;
        }
        const bool valid = (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '-';
        if (!valid || (segment_start && character == '-')) return false;
        segment_start = false;
    }
    return dots >= 2 && !segment_start;
}

bool HashEqual(
    const AnomalyIpcSchemaHashV1& left, const AnomalyIpcSchemaHashV1& right) noexcept {
    return std::memcmp(left.bytes, right.bytes, ANOMALY_IPC_SCHEMA_HASH_V1_SIZE) == 0;
}

bool HashPresent(const AnomalyIpcSchemaHashV1& hash) noexcept {
    return std::ranges::any_of(hash.bytes, [](const std::uint8_t value) { return value != 0; });
}

std::string HashHex(const AnomalyIpcSchemaHashV1& hash) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(ANOMALY_IPC_SCHEMA_HASH_V1_SIZE * 2U, '0');
    for (std::size_t index = 0; index < ANOMALY_IPC_SCHEMA_HASH_V1_SIZE; ++index) {
        result[index * 2U] = digits[hash.bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[hash.bytes[index] & 0x0fU];
    }
    return result;
}

bool ValidSpan(const AnomalyByteSpanV1 span, const std::size_t maximum) noexcept {
    return (span.data != nullptr || span.size == 0) && span.size <= maximum;
}

std::vector<std::uint8_t> CopySpan(const AnomalyByteSpanV1 span) {
    return span.size == 0
        ? std::vector<std::uint8_t>{}
        : std::vector<std::uint8_t>(span.data, span.data + span.size);
}

bool SameOwner(const IpcPluginOwner& left, const IpcPluginOwner& right) noexcept {
    return left.scope != nullptr && right.scope != nullptr &&
        left.scope->Owner() == right.scope->Owner() &&
        left.scope->Generation() == right.scope->Generation();
}

bool Owns(
    const IpcPluginOwner& owner,
    const std::shared_ptr<PluginScope>& scope,
    const AnomalyGenerationHandleV1 handle,
    const std::uint64_t token) noexcept {
    return owner.scope != nullptr && scope != nullptr && handle.id == token &&
        handle.generation == owner.scope->Generation() &&
        owner.scope->Owner() == scope->Owner() &&
        owner.scope->Generation() == scope->Generation();
}

bool HasDependency(const IpcPluginOwner& consumer, const IpcPluginOwner& provider) noexcept {
    if (SameOwner(consumer, provider)) return true;
    return consumer.scope != nullptr && provider.scope != nullptr &&
        std::ranges::find(consumer.dependencies, provider.scope->Owner()) !=
            consumer.dependencies.end();
}

IpcCallingDomain DomainForAffinity(const std::uint32_t affinity) noexcept {
    switch (affinity) {
    case ANOMALY_IPC_AFFINITY_V1_LIFECYCLE: return IpcCallingDomain::Lifecycle;
    case ANOMALY_IPC_AFFINITY_V1_WORKER: return IpcCallingDomain::Worker;
    case ANOMALY_IPC_AFFINITY_V1_GAME: return IpcCallingDomain::Game;
    case ANOMALY_IPC_AFFINITY_V1_RENDER: return IpcCallingDomain::Render;
    default: return IpcCallingDomain::Unknown;
    }
}

}  // namespace

class IpcRegistry::Impl final : public std::enable_shared_from_this<IpcRegistry::Impl> {
public:
    struct Endpoint final {
        IpcPluginOwner owner;
        std::uint64_t token{};
        std::string id;
        std::uint32_t major{};
        std::uint32_t minor{};
        AnomalyIpcSchemaHashV1 request_schema{};
        AnomalyIpcSchemaHashV1 response_schema{};
        AnomalyIpcSchemaHashV1 event_schema{};
        std::uint32_t modes{};
        std::uint32_t affinity{};
        std::uint32_t timeout_milliseconds{};
        std::uint32_t reentrancy{};
        std::uint32_t maximum_request_bytes{};
        std::uint32_t maximum_response_bytes{};
        std::uint32_t maximum_event_bytes{};
        std::uint32_t maximum_queue_depth{};
        AnomalyIpcRequestHandlerV1 handler{};
        void* callback_user{};
        std::atomic_bool active{true};
        std::atomic_size_t queued{};
        std::uint64_t calls{};
        std::uint64_t failures{};
        std::uint64_t timeouts{};
        std::uint64_t events{};
        std::deque<double> durations;
        std::unordered_set<std::string> consumers;
    };

    struct QueueReservation final {
        explicit QueueReservation(std::shared_ptr<Endpoint> value)
            : endpoint(std::move(value)) {}
        ~QueueReservation() {
            if (endpoint) endpoint->queued.fetch_sub(1, std::memory_order_acq_rel);
        }

        std::shared_ptr<Endpoint> endpoint;
    };

    struct Pending final {
        IpcPluginOwner owner;
        std::shared_ptr<Endpoint> endpoint;
        std::uint64_t token{};
        std::uint64_t request_id{};
        AnomalyIpcCompletionCallbackV1 completion{};
        void* completion_user{};
        std::atomic_bool active{true};
        std::atomic_bool completion_claimed{false};
    };

    struct TimeoutEntry final {
        std::chrono::steady_clock::time_point deadline;
        std::weak_ptr<Pending> pending;
        std::uint64_t request_id{};
    };

    struct EarlierTimeout final {
        bool operator()(const TimeoutEntry& left, const TimeoutEntry& right) const noexcept {
            return left.deadline > right.deadline;
        }
    };

    struct Subscription final {
        IpcPluginOwner owner;
        std::shared_ptr<Endpoint> endpoint;
        std::uint64_t token{};
        AnomalyIpcEventCallbackV1 callback{};
        void* callback_user{};
        std::atomic_bool active{true};
    };

    explicit Impl(IpcPost external_post)
        : external_post_(std::move(external_post)),
          worker_([this](const std::stop_token stop) { RunWorker(stop); }),
          timeout_worker_([this](const std::stop_token stop) { RunTimeouts(stop); }) {}

    ~Impl() {
        worker_.request_stop();
        timeout_worker_.request_stop();
        queue_condition_.notify_all();
        timeout_condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (timeout_worker_.joinable()) timeout_worker_.join();
    }

    bool Post(
        const std::uint32_t affinity,
        const std::string& owner,
        const std::uint64_t generation,
        std::function<void()> callback) noexcept {
        try {
            if (affinity != ANOMALY_IPC_AFFINITY_V1_CALLER &&
                affinity != ANOMALY_IPC_AFFINITY_V1_WORKER && external_post_) {
                return external_post_(affinity, owner, generation, std::move(callback));
            }
            {
                std::scoped_lock lock(queue_mutex_);
                if (worker_.get_stop_token().stop_requested()) return false;
                queue_.push(std::move(callback));
            }
            queue_condition_.notify_one();
            return true;
        } catch (...) {
            return false;
        }
    }

    void RemoveEndpoint(const std::shared_ptr<Endpoint>& endpoint) noexcept {
        if (!endpoint || !endpoint->active.exchange(false, std::memory_order_acq_rel)) return;
        std::vector<std::pair<std::shared_ptr<PluginScope>, std::uint64_t>> dependants;
        {
            std::scoped_lock lock(mutex_);
            const auto found = endpoints_.find(endpoint->id);
            if (found != endpoints_.end() && found->second == endpoint) endpoints_.erase(found);
            for (const auto& [token, subscription] : subscriptions_) {
                if (subscription && subscription->endpoint == endpoint && subscription->owner.scope) {
                    dependants.emplace_back(subscription->owner.scope, token);
                }
            }
            for (const auto& [token, pending] : pending_) {
                if (pending && pending->endpoint == endpoint && pending->owner.scope) {
                    dependants.emplace_back(pending->owner.scope, token);
                }
            }
        }
        for (const auto& [scope, token] : dependants) static_cast<void>(scope->Release(token));
    }

    void RemovePending(const std::shared_ptr<Pending>& pending) noexcept {
        if (!pending || !pending->active.exchange(false, std::memory_order_acq_rel)) return;
        std::scoped_lock lock(mutex_);
        pending_.erase(pending->token);
    }

    void RemoveSubscription(const std::shared_ptr<Subscription>& subscription) noexcept {
        if (!subscription ||
            !subscription->active.exchange(false, std::memory_order_acq_rel)) return;
        std::scoped_lock lock(mutex_);
        subscriptions_.erase(subscription->token);
    }

    std::shared_ptr<QueueReservation> ReserveQueue(
        const std::shared_ptr<Endpoint>& endpoint) noexcept {
        if (!endpoint || endpoint->queued.fetch_add(1, std::memory_order_acq_rel) >=
                endpoint->maximum_queue_depth) {
            if (endpoint) endpoint->queued.fetch_sub(1, std::memory_order_acq_rel);
            return {};
        }
        try {
            return std::make_shared<QueueReservation>(endpoint);
        } catch (...) {
            endpoint->queued.fetch_sub(1, std::memory_order_acq_rel);
            return {};
        }
    }

    void ScheduleTimeout(
        const std::shared_ptr<Pending>& pending,
        const std::uint32_t timeout_milliseconds) noexcept {
        try {
            {
                std::scoped_lock lock(timeout_mutex_);
                timeouts_.push({
                    std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_milliseconds),
                    pending,
                    pending->request_id});
            }
            timeout_condition_.notify_one();
        } catch (...) {
        }
    }

    std::shared_ptr<Endpoint> Resolve(
        const IpcPluginOwner& consumer,
        const AnomalyIpcEndpointSelectorV1* selector,
        const std::uint32_t mode,
        AnomalyStatusV1& status) const {
        if (consumer.scope == nullptr || selector == nullptr ||
            selector->struct_size < sizeof(*selector) || selector->endpoint_id.data == nullptr) {
            status = Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
            return {};
        }
        const std::string_view id(selector->endpoint_id.data, selector->endpoint_id.size);
        std::shared_ptr<Endpoint> endpoint;
        {
            std::scoped_lock lock(mutex_);
            const auto found = endpoints_.find(std::string(id));
            if (found != endpoints_.end()) endpoint = found->second;
        }
        if (!endpoint || !endpoint->active.load(std::memory_order_acquire)) {
            status = Status(ANOMALY_STATUS_V1_NOT_FOUND,
                ANOMALY_IPC_ERROR_V1_PROVIDER_MISSING, "IPC provider is missing");
            return {};
        }
        if (!HasDependency(consumer, endpoint->owner)) {
            status = Status(ANOMALY_STATUS_V1_PERMISSION_DENIED,
                ANOMALY_IPC_ERROR_V1_DEPENDENCY_REQUIRED,
                "IPC provider is not a declared dependency");
            return {};
        }
        if (selector->major_version != endpoint->major ||
            selector->minimum_minor_version > endpoint->minor) {
            status = Status(ANOMALY_STATUS_V1_CONFLICT,
                ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH, "IPC endpoint version mismatch");
            return {};
        }
        const bool schema_matches = mode == ANOMALY_IPC_MODE_V1_EVENT
            ? HashEqual(selector->event_schema, endpoint->event_schema)
            : HashEqual(selector->request_schema, endpoint->request_schema) &&
                HashEqual(selector->response_schema, endpoint->response_schema);
        if (!schema_matches) {
            status = Status(ANOMALY_STATUS_V1_CONFLICT,
                ANOMALY_IPC_ERROR_V1_SCHEMA_MISMATCH, "IPC endpoint schema mismatch");
            return {};
        }
        if ((endpoint->modes & mode) == 0) {
            status = Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE, "IPC call mode is unavailable");
            return {};
        }
        {
            std::scoped_lock lock(mutex_);
            endpoint->consumers.insert(consumer.scope->Owner());
        }
        status = Status(ANOMALY_STATUS_V1_OK);
        return endpoint;
    }

    AnomalyStatusV1 RunRequest(
        const IpcPluginOwner& caller,
        const std::shared_ptr<Endpoint>& endpoint,
        const std::uint64_t request_id,
        const AnomalyByteSpanV1 request,
        const AnomalyMutableByteSpanV1 response,
        std::size_t* response_size) noexcept {
        if (!endpoint || !endpoint->handler) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE, "IPC request handler is unavailable");
        }
        auto lease = endpoint->owner.scope->AcquireCallback(endpoint->owner.scope->Generation());
        if (!lease || !endpoint->active.load(std::memory_order_acquire)) {
            return Status(ANOMALY_STATUS_V1_CANCELLED,
                ANOMALY_IPC_ERROR_V1_STALE_GENERATION, "IPC provider generation is stale");
        }
        const std::string& caller_id = caller.scope->Owner();
        const AnomalyIpcRequestContextV1 context{
            sizeof(AnomalyIpcRequestContextV1), 0, request_id,
            {caller_id.data(), caller_id.size()}};
        const auto started = std::chrono::steady_clock::now();
        AnomalyStatusV1 result = Status(ANOMALY_STATUS_V1_FAILED);
        try {
            result = endpoint->handler(
                endpoint->callback_user, &context, request, response, response_size);
        } catch (...) {
            result = Status(ANOMALY_STATUS_V1_FAILED, ANOMALY_IPC_ERROR_V1_NONE,
                "IPC provider callback failed");
        }
        const double milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        {
            std::scoped_lock lock(mutex_);
            ++endpoint->calls;
            if (result.code != ANOMALY_STATUS_V1_OK) ++endpoint->failures;
            endpoint->durations.push_back(milliseconds);
            if (endpoint->durations.size() > kMaximumMetricSamples) {
                endpoint->durations.pop_front();
            }
        }
        return result;
    }

    bool AddWaitEdge(const std::string& caller, const std::string& provider) {
        std::scoped_lock lock(wait_mutex_);
        std::vector<std::string> pending{provider};
        std::unordered_set<std::string> visited;
        while (!pending.empty()) {
            std::string cursor = std::move(pending.back());
            pending.pop_back();
            if (cursor == caller) return false;
            if (!visited.insert(cursor).second) continue;
            const auto found = wait_edges_.find(cursor);
            if (found == wait_edges_.end()) continue;
            for (const auto& [next, references] : found->second) {
                if (references != 0) pending.push_back(next);
            }
        }
        ++wait_edges_[caller][provider];
        return true;
    }

    void RemoveWaitEdge(const std::string& caller, const std::string& provider) noexcept {
        std::scoped_lock lock(wait_mutex_);
        const auto caller_edges = wait_edges_.find(caller);
        if (caller_edges == wait_edges_.end()) return;
        const auto edge = caller_edges->second.find(provider);
        if (edge == caller_edges->second.end()) return;
        if (--edge->second == 0) caller_edges->second.erase(edge);
        if (caller_edges->second.empty()) wait_edges_.erase(caller_edges);
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Endpoint>> endpoints_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Subscription>> subscriptions_;
    std::atomic_uint64_t next_request_id_{1};

private:
    void RunWorker(const std::stop_token stop) {
        std::unique_lock lock(queue_mutex_);
        while (!stop.stop_requested()) {
            queue_condition_.wait(lock, [&] { return stop.stop_requested() || !queue_.empty(); });
            if (stop.stop_requested()) break;
            std::function<void()> callback = std::move(queue_.front());
            queue_.pop();
            lock.unlock();
            try { if (callback) callback(); } catch (...) {}
            lock.lock();
        }
        while (!queue_.empty()) queue_.pop();
    }

    void RunTimeouts(const std::stop_token stop) {
        std::unique_lock lock(timeout_mutex_);
        while (!stop.stop_requested()) {
            if (timeouts_.empty()) {
                timeout_condition_.wait(lock, [&] {
                    return stop.stop_requested() || !timeouts_.empty();
                });
                continue;
            }
            const auto deadline = timeouts_.top().deadline;
            if (timeout_condition_.wait_until(lock, deadline) != std::cv_status::timeout) {
                continue;
            }
            TimeoutEntry entry = timeouts_.top();
            timeouts_.pop();
            lock.unlock();
            const auto pending = entry.pending.lock();
            if (pending && pending->request_id == entry.request_id &&
                pending->active.load(std::memory_order_acquire) &&
                !pending->completion_claimed.exchange(true, std::memory_order_acq_rel)) {
                {
                    std::scoped_lock metrics_lock(mutex_);
                    ++pending->endpoint->timeouts;
                }
                auto lease = pending->owner.scope->AcquireCallback(
                    pending->owner.scope->Generation());
                if (lease && pending->active.load(std::memory_order_acquire)) {
                    try {
                        pending->completion(
                            pending->completion_user,
                            {pending->token, pending->owner.scope->Generation()},
                            Status(ANOMALY_STATUS_V1_TIMEOUT, ANOMALY_IPC_ERROR_V1_TIMEOUT,
                                "IPC request timed out"),
                            {});
                    } catch (...) {
                    }
                }
                static_cast<void>(pending->owner.scope->Release(pending->token));
            }
            lock.lock();
        }
        while (!timeouts_.empty()) timeouts_.pop();
    }

    IpcPost external_post_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::queue<std::function<void()>> queue_;
    std::jthread worker_;
    std::mutex timeout_mutex_;
    std::condition_variable timeout_condition_;
    std::priority_queue<TimeoutEntry, std::vector<TimeoutEntry>, EarlierTimeout> timeouts_;
    std::jthread timeout_worker_;
    std::mutex wait_mutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> wait_edges_;
};

IpcRegistry::IpcRegistry(IpcPost post) : impl_(std::make_shared<Impl>(std::move(post))) {}
IpcRegistry::~IpcRegistry() = default;

AnomalyStatusV1 IpcRegistry::RegisterEndpoint(
    const IpcPluginOwner& owner,
    const AnomalyIpcEndpointDescriptorV1* descriptor,
    const AnomalyIpcRequestHandlerV1 request_handler,
    void* callback_user,
    AnomalyGenerationHandleV1* endpoint) noexcept {
    if (owner.scope == nullptr || descriptor == nullptr || endpoint == nullptr ||
        descriptor->struct_size < sizeof(*descriptor) || descriptor->endpoint_id.data == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const std::string_view id(descriptor->endpoint_id.data, descriptor->endpoint_id.size);
    const std::uint32_t request_modes = ANOMALY_IPC_MODE_V1_SYNC_REQUEST |
        ANOMALY_IPC_MODE_V1_ASYNC_REQUEST;
    const bool valid_modes = descriptor->modes != 0 &&
        (descriptor->modes & ~(request_modes | ANOMALY_IPC_MODE_V1_EVENT)) == 0;
    const bool valid_affinity = descriptor->affinity <= ANOMALY_IPC_AFFINITY_V1_RENDER;
    const bool valid_reentrancy = descriptor->reentrancy <= ANOMALY_IPC_REENTRANCY_V1_ALLOW;
    if (!IsReverseDomainId(id) || descriptor->major_version == 0 || !valid_modes ||
        !valid_affinity || !valid_reentrancy || descriptor->timeout_milliseconds == 0 ||
        descriptor->timeout_milliseconds > kMaximumTimeoutMilliseconds ||
        descriptor->maximum_request_bytes > kMaximumPayloadBytes ||
        descriptor->maximum_response_bytes > kMaximumPayloadBytes ||
        descriptor->maximum_event_bytes > kMaximumPayloadBytes ||
        descriptor->maximum_queue_depth == 0 ||
        descriptor->maximum_queue_depth > kMaximumQueueDepth ||
        ((descriptor->modes & request_modes) != 0 &&
            (!HashPresent(descriptor->request_schema) ||
             !HashPresent(descriptor->response_schema) || request_handler == nullptr)) ||
        ((descriptor->modes & ANOMALY_IPC_MODE_V1_EVENT) != 0 &&
            !HashPresent(descriptor->event_schema))) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    try {
        auto state = std::make_shared<Impl::Endpoint>();
        state->owner = owner;
        state->id = std::string(id);
        state->major = descriptor->major_version;
        state->minor = descriptor->minor_version;
        state->request_schema = descriptor->request_schema;
        state->response_schema = descriptor->response_schema;
        state->event_schema = descriptor->event_schema;
        state->modes = descriptor->modes;
        state->affinity = descriptor->affinity;
        state->timeout_milliseconds = descriptor->timeout_milliseconds;
        state->reentrancy = descriptor->reentrancy;
        state->maximum_request_bytes = descriptor->maximum_request_bytes;
        state->maximum_response_bytes = descriptor->maximum_response_bytes;
        state->maximum_event_bytes = descriptor->maximum_event_bytes;
        state->maximum_queue_depth = descriptor->maximum_queue_depth;
        state->handler = request_handler;
        state->callback_user = callback_user;
        {
            std::scoped_lock lock(impl_->mutex_);
            if (impl_->endpoints_.contains(state->id)) {
                return Status(ANOMALY_STATUS_V1_CONFLICT,
                    ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH,
                    "IPC endpoint ID is already registered");
            }
        }
        const std::weak_ptr<Impl> weak_impl = impl_;
        state->token = owner.scope->Register(
            PluginResourceKind::Ipc, "ipc.endpoint:" + state->id,
            [weak_impl, state] {
                if (const auto impl = weak_impl.lock()) impl->RemoveEndpoint(state);
            });
        if (state->token == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                ANOMALY_IPC_ERROR_V1_STALE_GENERATION, "plugin scope is stopping");
        }
        {
            std::scoped_lock lock(impl_->mutex_);
            if (!impl_->endpoints_.emplace(state->id, state).second) {
                static_cast<void>(owner.scope->Release(state->token));
                return Status(ANOMALY_STATUS_V1_CONFLICT,
                    ANOMALY_IPC_ERROR_V1_VERSION_MISMATCH,
                    "IPC endpoint ID is already registered");
            }
        }
        *endpoint = {state->token, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED);
    }
}

AnomalyStatusV1 IpcRegistry::UnregisterEndpoint(
    const IpcPluginOwner& owner, const AnomalyGenerationHandleV1 endpoint) noexcept {
    std::shared_ptr<Impl::Endpoint> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        for (const auto& [id, candidate] : impl_->endpoints_) {
            if (candidate && candidate->token == endpoint.id) { state = candidate; break; }
        }
    }
    if (!state || !Owns(owner, state->owner.scope, endpoint, state->token)) {
        return Status(ANOMALY_STATUS_V1_NOT_FOUND,
            ANOMALY_IPC_ERROR_V1_STALE_GENERATION, "IPC endpoint handle is stale");
    }
    return owner.scope->Release(endpoint.id)
        ? Status(ANOMALY_STATUS_V1_OK)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, ANOMALY_IPC_ERROR_V1_STALE_GENERATION);
}

AnomalyStatusV1 IpcRegistry::Invoke(
    const IpcPluginOwner& owner,
    const IpcCallingDomain calling_domain,
    const AnomalyIpcEndpointSelectorV1* selector,
    const AnomalyByteSpanV1 request,
    const AnomalyMutableByteSpanV1 response,
    std::size_t* response_size) noexcept {
    AnomalyStatusV1 resolved{};
    const auto endpoint = impl_->Resolve(owner, selector, ANOMALY_IPC_MODE_V1_SYNC_REQUEST, resolved);
    if (!endpoint) return resolved;
    if (response_size == nullptr || (response.data == nullptr && response.size != 0) ||
        !ValidSpan(request, endpoint->maximum_request_bytes) ||
        response.size > endpoint->maximum_response_bytes) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    if (calling_domain == IpcCallingDomain::Game || calling_domain == IpcCallingDomain::Render) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE,
            "blocking IPC is forbidden on Game and Render domains");
    }
    const std::string caller = owner.scope->Owner();
    const std::string provider = endpoint->owner.scope->Owner();
    if (endpoint->reentrancy == ANOMALY_IPC_REENTRANCY_V1_REJECT &&
        !impl_->AddWaitEdge(caller, provider)) {
        return Status(ANOMALY_STATUS_V1_CONFLICT,
            ANOMALY_IPC_ERROR_V1_REENTRANT_CYCLE, "IPC reentrant cycle rejected");
    }
    struct EdgeGuard final {
        Impl* impl{};
        std::string caller;
        std::string provider;
        ~EdgeGuard() { if (impl) impl->RemoveWaitEdge(caller, provider); }
    } edge{endpoint->reentrancy == ANOMALY_IPC_REENTRANCY_V1_REJECT ? impl_.get() : nullptr,
           caller, provider};
    const std::uint64_t request_id = impl_->next_request_id_.fetch_add(1);
    if (endpoint->affinity == ANOMALY_IPC_AFFINITY_V1_CALLER ||
        DomainForAffinity(endpoint->affinity) == calling_domain) {
        return impl_->RunRequest(owner, endpoint, request_id, request, response, response_size);
    }
    const std::vector<std::uint8_t> bytes = CopySpan(request);
    auto response_bytes = std::make_shared<std::vector<std::uint8_t>>(response.size);
    auto produced_size = std::make_shared<std::size_t>(response.size);
    auto promise = std::make_shared<std::promise<AnomalyStatusV1>>();
    std::future<AnomalyStatusV1> future = promise->get_future();
    const auto reservation = impl_->ReserveQueue(endpoint);
    if (!reservation) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            ANOMALY_IPC_ERROR_V1_QUEUE_FULL, "IPC endpoint queue is full");
    }
    const bool posted = impl_->Post(
        endpoint->affinity, endpoint->owner.scope->Owner(),
        endpoint->owner.scope->Generation(),
        [impl = impl_, owner, endpoint, request_id, bytes, response_bytes, produced_size, promise,
         reservation] {
            promise->set_value(impl->RunRequest(
                owner, endpoint, request_id, {bytes.data(), bytes.size()},
                {response_bytes->data(), response_bytes->size()}, produced_size.get()));
        });
    if (!posted) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE, "IPC affinity dispatcher is unavailable");
    }
    if (future.wait_for(std::chrono::milliseconds(endpoint->timeout_milliseconds)) !=
        std::future_status::ready) {
        {
            std::scoped_lock lock(impl_->mutex_);
            ++endpoint->timeouts;
        }
        return Status(ANOMALY_STATUS_V1_TIMEOUT,
            ANOMALY_IPC_ERROR_V1_TIMEOUT, "IPC request timed out");
    }
    const AnomalyStatusV1 result = future.get();
    *response_size = *produced_size;
    if (*produced_size <= response.size && *produced_size != 0) {
        std::memcpy(response.data, response_bytes->data(), *produced_size);
    }
    return result;
}

AnomalyStatusV1 IpcRegistry::InvokeAsync(
    const IpcPluginOwner& owner,
    const AnomalyIpcEndpointSelectorV1* selector,
    const AnomalyByteSpanV1 request,
    const AnomalyIpcCompletionCallbackV1 completion,
    void* completion_user,
    AnomalyGenerationHandleV1* pending_call) noexcept {
    AnomalyStatusV1 resolved{};
    const auto endpoint = impl_->Resolve(owner, selector, ANOMALY_IPC_MODE_V1_ASYNC_REQUEST, resolved);
    if (!endpoint) return resolved;
    if (completion == nullptr || pending_call == nullptr ||
        !ValidSpan(request, endpoint->maximum_request_bytes)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const auto reservation = impl_->ReserveQueue(endpoint);
    if (!reservation) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            ANOMALY_IPC_ERROR_V1_QUEUE_FULL, "IPC endpoint queue is full");
    }
    std::shared_ptr<Impl::Pending> pending;
    try {
        const std::vector<std::uint8_t> bytes = CopySpan(request);
        pending = std::make_shared<Impl::Pending>();
        pending->owner = owner;
        pending->endpoint = endpoint;
        pending->request_id = impl_->next_request_id_.fetch_add(1);
        pending->completion = completion;
        pending->completion_user = completion_user;
        const std::weak_ptr<Impl> weak_impl = impl_;
        pending->token = owner.scope->Register(
            PluginResourceKind::Ipc, "ipc.pending:" + endpoint->id,
            [weak_impl, pending] {
                if (const auto impl = weak_impl.lock()) impl->RemovePending(pending);
            });
        if (pending->token == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                ANOMALY_IPC_ERROR_V1_STALE_GENERATION, "plugin scope is stopping");
        }
        {
            std::scoped_lock lock(impl_->mutex_);
            impl_->pending_.emplace(pending->token, pending);
        }
        const bool posted = impl_->Post(
            endpoint->affinity, endpoint->owner.scope->Owner(),
            endpoint->owner.scope->Generation(),
            [impl = impl_, pending, bytes, reservation] {
                if (!pending->active.load(std::memory_order_acquire)) return;
                std::vector<std::uint8_t> response;
                std::size_t response_size{};
                AnomalyStatusV1 status = Status(ANOMALY_STATUS_V1_FAILED);
                try {
                    response.resize(pending->endpoint->maximum_response_bytes);
                    response_size = response.size();
                    status = impl->RunRequest(
                        pending->owner, pending->endpoint, pending->request_id,
                        {bytes.data(), bytes.size()}, {response.data(), response.size()},
                        &response_size);
                    if (response_size > response.size()) response_size = 0;
                } catch (...) {
                    status = Status(
                        ANOMALY_STATUS_V1_FAILED, ANOMALY_IPC_ERROR_V1_NONE,
                        "IPC request dispatch failed");
                }
                auto consumer_lease = pending->owner.scope->AcquireCallback(
                    pending->owner.scope->Generation());
                if (consumer_lease && pending->active.load(std::memory_order_acquire) &&
                    !pending->completion_claimed.exchange(true, std::memory_order_acq_rel)) {
                    try {
                        pending->completion(
                            pending->completion_user,
                            {pending->token, pending->owner.scope->Generation()}, status,
                            {response.data(), response_size});
                    } catch (...) {}
                }
                if (pending->owner.scope) static_cast<void>(pending->owner.scope->Release(pending->token));
            });
        if (!posted) {
            static_cast<void>(owner.scope->Release(pending->token));
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE, "IPC affinity dispatcher is unavailable");
        }
        impl_->ScheduleTimeout(pending, endpoint->timeout_milliseconds);
        *pending_call = {pending->token, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        if (pending && pending->token != 0 && owner.scope) {
            static_cast<void>(owner.scope->Release(pending->token));
        }
        return Status(ANOMALY_STATUS_V1_FAILED);
    }
}

AnomalyStatusV1 IpcRegistry::Cancel(
    const IpcPluginOwner& owner, const AnomalyGenerationHandleV1 pending_call) noexcept {
    std::shared_ptr<Impl::Pending> pending;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->pending_.find(pending_call.id);
        if (found != impl_->pending_.end()) pending = found->second;
    }
    if (!pending || !Owns(owner, pending->owner.scope, pending_call, pending->token)) {
        return Status(ANOMALY_STATUS_V1_NOT_FOUND,
            ANOMALY_IPC_ERROR_V1_STALE_GENERATION, "IPC pending handle is stale");
    }
    return owner.scope->Release(pending_call.id)
        ? Status(ANOMALY_STATUS_V1_CANCELLED)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, ANOMALY_IPC_ERROR_V1_STALE_GENERATION);
}

AnomalyStatusV1 IpcRegistry::Subscribe(
    const IpcPluginOwner& owner,
    const AnomalyIpcEndpointSelectorV1* selector,
    const AnomalyIpcEventCallbackV1 callback,
    void* callback_user,
    AnomalyGenerationHandleV1* subscription) noexcept {
    AnomalyStatusV1 resolved{};
    const auto endpoint = impl_->Resolve(owner, selector, ANOMALY_IPC_MODE_V1_EVENT, resolved);
    if (!endpoint) return resolved;
    if (callback == nullptr || subscription == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    try {
        auto state = std::make_shared<Impl::Subscription>();
        state->owner = owner;
        state->endpoint = endpoint;
        state->callback = callback;
        state->callback_user = callback_user;
        const std::weak_ptr<Impl> weak_impl = impl_;
        state->token = owner.scope->Register(
            PluginResourceKind::Ipc, "ipc.subscription:" + endpoint->id,
            [weak_impl, state] {
                if (const auto impl = weak_impl.lock()) impl->RemoveSubscription(state);
            });
        if (state->token == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                ANOMALY_IPC_ERROR_V1_STALE_GENERATION, "plugin scope is stopping");
        }
        {
            std::scoped_lock lock(impl_->mutex_);
            impl_->subscriptions_.emplace(state->token, state);
        }
        *subscription = {state->token, owner.scope->Generation()};
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED);
    }
}

AnomalyStatusV1 IpcRegistry::Unsubscribe(
    const IpcPluginOwner& owner, const AnomalyGenerationHandleV1 subscription) noexcept {
    std::shared_ptr<Impl::Subscription> state;
    {
        std::scoped_lock lock(impl_->mutex_);
        const auto found = impl_->subscriptions_.find(subscription.id);
        if (found != impl_->subscriptions_.end()) state = found->second;
    }
    if (!state || !Owns(owner, state->owner.scope, subscription, state->token)) {
        return Status(ANOMALY_STATUS_V1_NOT_FOUND,
            ANOMALY_IPC_ERROR_V1_STALE_GENERATION, "IPC subscription handle is stale");
    }
    return owner.scope->Release(subscription.id)
        ? Status(ANOMALY_STATUS_V1_OK)
        : Status(ANOMALY_STATUS_V1_NOT_FOUND, ANOMALY_IPC_ERROR_V1_STALE_GENERATION);
}

AnomalyStatusV1 IpcRegistry::Publish(
    const IpcPluginOwner& owner,
    const AnomalyGenerationHandleV1 endpoint_handle,
    const AnomalyByteSpanV1 event) noexcept {
    std::shared_ptr<Impl::Endpoint> endpoint;
    std::vector<std::shared_ptr<Impl::Subscription>> subscribers;
    {
        std::scoped_lock lock(impl_->mutex_);
        for (const auto& [id, candidate] : impl_->endpoints_) {
            if (candidate && candidate->token == endpoint_handle.id) {
                endpoint = candidate;
                break;
            }
        }
        if (endpoint) {
            for (const auto& [token, candidate] : impl_->subscriptions_) {
                if (candidate && candidate->endpoint == endpoint &&
                    candidate->active.load(std::memory_order_acquire)) {
                    subscribers.push_back(candidate);
                }
            }
        }
    }
    if (!endpoint || !Owns(owner, endpoint->owner.scope, endpoint_handle, endpoint->token)) {
        return Status(ANOMALY_STATUS_V1_NOT_FOUND,
            ANOMALY_IPC_ERROR_V1_STALE_GENERATION, "IPC endpoint handle is stale");
    }
    if ((endpoint->modes & ANOMALY_IPC_MODE_V1_EVENT) == 0 ||
        !ValidSpan(event, endpoint->maximum_event_bytes)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const std::vector<std::uint8_t> bytes = CopySpan(event);
    for (const auto& subscriber : subscribers) {
        const auto reservation = impl_->ReserveQueue(endpoint);
        if (!reservation) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                ANOMALY_IPC_ERROR_V1_QUEUE_FULL, "IPC endpoint queue is full");
        }
        const bool posted = impl_->Post(
            endpoint->affinity, subscriber->owner.scope->Owner(),
            subscriber->owner.scope->Generation(),
            [impl = impl_, endpoint, subscriber, bytes, reservation] {
                auto lease = subscriber->owner.scope->AcquireCallback(
                    subscriber->owner.scope->Generation());
                if (!lease || !endpoint->active.load(std::memory_order_acquire) ||
                    !subscriber->active.load(std::memory_order_acquire)) return;
                try {
                    subscriber->callback(
                        subscriber->callback_user,
                        {endpoint->id.data(), endpoint->id.size()},
                        {bytes.data(), bytes.size()});
                } catch (...) {}
            });
        if (!posted) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                ANOMALY_IPC_ERROR_V1_MODE_UNAVAILABLE, "IPC affinity dispatcher is unavailable");
        }
    }
    {
        std::scoped_lock lock(impl_->mutex_);
        ++endpoint->events;
    }
    return Status(ANOMALY_STATUS_V1_OK);
}

IpcDiagnostics IpcRegistry::Snapshot() const noexcept {
    IpcDiagnostics result;
    try {
        std::scoped_lock lock(impl_->mutex_);
        result.endpoints.reserve(impl_->endpoints_.size());
        for (const auto& [id, endpoint] : impl_->endpoints_) {
            if (!endpoint || !endpoint->active.load(std::memory_order_acquire)) continue;
            IpcEndpointDiagnostics view;
            view.id = id;
            view.provider = endpoint->owner.scope->Owner();
            view.consumers.assign(endpoint->consumers.begin(), endpoint->consumers.end());
            std::sort(view.consumers.begin(), view.consumers.end());
            view.request_schema_hash = HashHex(endpoint->request_schema);
            view.response_schema_hash = HashHex(endpoint->response_schema);
            view.event_schema_hash = HashHex(endpoint->event_schema);
            view.generation = endpoint->owner.scope->Generation();
            view.major_version = endpoint->major;
            view.minor_version = endpoint->minor;
            view.modes = endpoint->modes;
            view.affinity = endpoint->affinity;
            view.calls = endpoint->calls;
            view.failures = endpoint->failures;
            view.timeouts = endpoint->timeouts;
            view.events = endpoint->events;
            view.pending_calls = static_cast<std::size_t>(std::count_if(
                impl_->pending_.begin(), impl_->pending_.end(), [&](const auto& entry) {
                    return entry.second && entry.second->endpoint == endpoint;
                }));
            view.subscriptions = static_cast<std::size_t>(std::count_if(
                impl_->subscriptions_.begin(), impl_->subscriptions_.end(), [&](const auto& entry) {
                    return entry.second && entry.second->endpoint == endpoint;
                }));
            if (!endpoint->durations.empty()) {
                std::vector<double> samples(endpoint->durations.begin(), endpoint->durations.end());
                std::sort(samples.begin(), samples.end());
                const std::size_t index = static_cast<std::size_t>(
                    std::ceil(0.95 * static_cast<double>(samples.size()))) - 1U;
                view.p95_milliseconds = samples[(std::min)(index, samples.size() - 1U)];
            }
            result.endpoints.push_back(std::move(view));
        }
        std::sort(result.endpoints.begin(), result.endpoints.end(),
            [](const IpcEndpointDiagnostics& left, const IpcEndpointDiagnostics& right) {
                return left.id < right.id;
            });
    } catch (...) {}
    return result;
}

}  // namespace anomaly
