#include "anomaly/ue5_nte_adapter.hpp"
#include "anomaly/thread_local_value.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

ThreadLocalScalar<const void*> g_active_tick_callback_state;
ThreadLocalScalar<const void*> g_active_ahud_callback_state;
ThreadLocalScalar<const void*> g_active_ahud_subscription_state;

class ActiveTickCallbackScope final {
public:
    explicit ActiveTickCallbackScope(const void* state) noexcept
        : previous_(g_active_tick_callback_state.Get()) {
        g_active_tick_callback_state.Set(state);
    }

    ActiveTickCallbackScope(const ActiveTickCallbackScope&) = delete;
    ActiveTickCallbackScope& operator=(const ActiveTickCallbackScope&) = delete;

    ~ActiveTickCallbackScope() {
        g_active_tick_callback_state.Set(previous_);
    }

private:
    const void* previous_{};
};

class ActiveAhudCallbackScope final {
public:
    ActiveAhudCallbackScope(const void* state, const void* subscription) noexcept
        : previous_state_(g_active_ahud_callback_state.Get()),
          previous_subscription_(g_active_ahud_subscription_state.Get()) {
        g_active_ahud_callback_state.Set(state);
        g_active_ahud_subscription_state.Set(subscription);
    }

    ActiveAhudCallbackScope(const ActiveAhudCallbackScope&) = delete;
    ActiveAhudCallbackScope& operator=(const ActiveAhudCallbackScope&) = delete;

    ~ActiveAhudCallbackScope() {
        g_active_ahud_subscription_state.Set(previous_subscription_);
        g_active_ahud_callback_state.Set(previous_state_);
    }

private:
    const void* previous_state_{};
    const void* previous_subscription_{};
};

class AddressWaitApi final {
public:
    using WaitOnAddressFunction = BOOL(WINAPI*)(
        volatile VOID*, PVOID, SIZE_T, DWORD);
    using WakeByAddressAllFunction = VOID(WINAPI*)(PVOID);

    [[nodiscard]] static const AddressWaitApi& Instance() noexcept {
        static const AddressWaitApi api;
        return api;
    }

    [[nodiscard]] WaitOnAddressFunction Wait() const noexcept {
        return wait_;
    }

    [[nodiscard]] WakeByAddressAllFunction WakeAll() const noexcept {
        return wake_all_;
    }

private:
    AddressWaitApi() noexcept {
        HMODULE module = GetModuleHandleW(L"kernelbase.dll");
        if (module == nullptr) module = GetModuleHandleW(L"kernel32.dll");
        if (module == nullptr) return;
        wait_ = reinterpret_cast<WaitOnAddressFunction>(
            GetProcAddress(module, "WaitOnAddress"));
        wake_all_ = reinterpret_cast<WakeByAddressAllFunction>(
            GetProcAddress(module, "WakeByAddressAll"));
    }

    WaitOnAddressFunction wait_{};
    WakeByAddressAllFunction wake_all_{};
};

class AdmissionGate final {
public:
    [[nodiscard]] bool TryEnter() noexcept {
        std::uint64_t observed = Load();
        for (;;) {
            if ((observed & kClosedBit) != 0 ||
                (observed & kActiveMask) == kActiveMask) {
                return false;
            }
            const std::uint64_t desired = observed + 1U;
            const auto previous = static_cast<std::uint64_t>(InterlockedCompareExchange64(
                &value_, static_cast<LONG64>(desired), static_cast<LONG64>(observed)));
            if (previous == observed) return true;
            observed = previous;
        }
    }

    void Close() noexcept {
        static_cast<void>(InterlockedOr64(&value_, static_cast<LONG64>(kClosedBit)));
        WakeAll(&value_);
    }

    void Leave() noexcept {
        const auto remaining =
            static_cast<std::uint64_t>(InterlockedDecrement64(&value_)) & kActiveMask;
        if (remaining == 0) {
            WakeAll(&value_);
        }
    }

    [[nodiscard]] bool IsDrained() const noexcept {
        return (Load() & kActiveMask) == 0;
    }

    [[nodiscard]] bool DrainUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        for (;;) {
            const std::uint64_t observed = Load();
            if ((observed & kActiveMask) == 0) return true;
            if (deadline != std::chrono::steady_clock::time_point::max() &&
                std::chrono::steady_clock::now() >= deadline) {
                return false;
            }

            const DWORD timeout = RemainingMilliseconds(deadline);
            LONG64 expected = static_cast<LONG64>(observed);
            Wait(&value_, expected, timeout);
        }
    }

private:
    static void Wait(volatile LONG64* address, LONG64 expected, DWORD timeout) noexcept {
        if (const auto wait = AddressWaitApi::Instance().Wait()) {
            static_cast<void>(wait(address, &expected, sizeof(expected), timeout));
            return;
        }
        if (timeout != 0) {
            Sleep(timeout == INFINITE
                ? static_cast<DWORD>(1)
                : (std::min)(timeout, static_cast<DWORD>(1)));
        }
    }

    static void WakeAll(volatile LONG64* address) noexcept {
        if (const auto wake = AddressWaitApi::Instance().WakeAll()) {
            wake(const_cast<void*>(static_cast<const volatile void*>(address)));
        }
    }

    [[nodiscard]] std::uint64_t Load() const noexcept {
        return static_cast<std::uint64_t>(InterlockedCompareExchange64(
            const_cast<volatile LONG64*>(&value_), 0, 0));
    }

    [[nodiscard]] static DWORD RemainingMilliseconds(
        std::chrono::steady_clock::time_point deadline) noexcept {
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            return INFINITE;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (std::chrono::duration_cast<std::chrono::steady_clock::duration>(milliseconds) <
            remaining) {
            ++milliseconds;
        }
        if (milliseconds <= std::chrono::milliseconds::zero()) return 0;
        return static_cast<DWORD>((std::min)(
            milliseconds.count(), static_cast<std::int64_t>(INFINITE - 1U)));
    }

    static constexpr std::uint64_t kClosedBit = std::uint64_t{1} << 63U;
    static constexpr std::uint64_t kActiveMask = ~kClosedBit;
    volatile LONG64 value_{};
};

class RetiredTickCallbackQueue final {
public:
    RetiredTickCallbackQueue() {
        std::thread([this] { Run(); }).detach();
    }

    void Retire(const Ue5NteAdapter::TickCallback* callback) noexcept {
        if (callback == nullptr) return;
        try {
            {
                std::scoped_lock lock(mutex_);
                callbacks_.push_back(callback);
            }
            ready_.notify_one();
        } catch (...) {
            // A callback target can own arbitrary code. Preserve it rather
            // than running that destructor on a bounded lifecycle path.
        }
    }

private:
    void Run() noexcept {
        for (;;) {
            const Ue5NteAdapter::TickCallback* callback{};
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return !callbacks_.empty(); });
                callback = callbacks_.front();
                callbacks_.pop_front();
            }
            delete callback;
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<const Ue5NteAdapter::TickCallback*> callbacks_;
};

RetiredTickCallbackQueue* ProcessRetiredTickCallbacks() noexcept {
    // This queue intentionally outlives Runtime teardown: a user callback
    // destructor may block or reenter after the bounded Adapter stop path.
    static auto* queue = []() noexcept -> RetiredTickCallbackQueue* {
        try {
            return new RetiredTickCallbackQueue();
        } catch (...) {
            return nullptr;
        }
    }();
    return queue;
}

void RetireTickCallback(const Ue5NteAdapter::TickCallback* callback) noexcept {
    if (auto* queue = ProcessRetiredTickCallbacks()) {
        queue->Retire(callback);
    }
    // If the process queue cannot be initialized, intentionally retain the
    // callback object rather than destroying arbitrary code during Stop.
}

std::shared_ptr<const Ue5NteAdapter::TickCallback> MakeTickCallback(
    Ue5NteAdapter::TickCallback callback) {
    if (!callback) return {};
    return std::shared_ptr<const Ue5NteAdapter::TickCallback>(
        new Ue5NteAdapter::TickCallback(std::move(callback)), RetireTickCallback);
}

template <typename Mutex>
[[nodiscard]] bool LockUntil(
    std::unique_lock<Mutex>& lock,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        lock.lock();
        return true;
    }
    return lock.try_lock_until(deadline);
}

AnomalyStatusV1 Status(std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

std::uint32_t SnapshotFlags(
    bool partial,
    std::uint64_t sample_sequence,
    std::uint64_t current_sequence) noexcept {
    std::uint32_t flags = ANOMALY_NTE_SNAPSHOT_V1_VALID;
    if (sample_sequence < current_sequence) flags |= ANOMALY_NTE_SNAPSHOT_V1_STALE;
    if (partial) flags |= ANOMALY_NTE_SNAPSHOT_V1_PARTIAL;
    return flags;
}

AnomalyStatusV1 CopyString(
    std::string_view value,
    char* destination,
    std::size_t* inout_size) noexcept {
    if (inout_size == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const std::size_t required = value.size() + 1;
    if (destination == nullptr || *inout_size < required) {
        *inout_size = required;
        return destination == nullptr
            ? Status(ANOMALY_STATUS_V1_OK)
            : Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
    *inout_size = required;
    return Status(ANOMALY_STATUS_V1_OK);
}

std::int64_t Layout(
    const BuildProfile& profile,
    std::string_view key,
    std::int64_t fallback = -1) noexcept {
    const auto found = profile.layout.find(key);
    return found == profile.layout.end() ? fallback : found->second;
}

template <typename T>
bool ReadValue(const SymbolMemory& memory, std::uintptr_t address, T& value) noexcept {
    return address != 0 && memory.Read(address, &value, sizeof(value));
}

bool AddAddress(std::uintptr_t base, std::int64_t offset, std::uintptr_t& result) noexcept {
    if (base == 0 || offset < 0 ||
        static_cast<std::uint64_t>(offset) >
            (std::numeric_limits<std::uintptr_t>::max)() - base) return false;
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

bool AddLayoutOffset(
    std::int64_t offset,
    std::int64_t additional,
    std::int64_t& result) noexcept {
    if (offset < 0 || additional < 0 ||
        offset > (std::numeric_limits<std::int64_t>::max)() - additional) {
        return false;
    }
    result = offset + additional;
    return true;
}

bool ReadPointerAt(
    const SymbolMemory& memory,
    std::uintptr_t base,
    std::int64_t offset,
    std::uintptr_t& result) noexcept {
    std::uintptr_t address{};
    return AddAddress(base, offset, address) && ReadValue(memory, address, result) && result != 0;
}

bool ReadNullablePointerAt(
    const SymbolMemory& memory,
    std::uintptr_t base,
    std::int64_t offset,
    std::uintptr_t& result) noexcept {
    std::uintptr_t address{};
    return AddAddress(base, offset, address) && ReadValue(memory, address, result);
}

bool AddUnsignedAddress(
    std::uintptr_t base,
    std::uint64_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

struct ObjectRegistryState {
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
    std::uint32_t chunk_size{};
    std::uint32_t item_stride{};
    std::uint32_t object_offset{};
    std::uint32_t serial_offset{};
    std::uint64_t chunk_signature{};
};

bool ReadObjectChunk(
    const SymbolMemory& memory,
    const ObjectRegistryState& registry,
    std::uint32_t page,
    std::uintptr_t& chunk) noexcept {
    if (registry.items == 0 || page >= registry.num_chunks ||
        page > (std::numeric_limits<std::uint64_t>::max)() / sizeof(std::uintptr_t)) {
        return false;
    }
    std::uintptr_t entry{};
    return AddUnsignedAddress(
               registry.items,
               static_cast<std::uint64_t>(page) * sizeof(std::uintptr_t), entry) &&
        ReadValue(memory, entry, chunk) && chunk != 0 &&
        (chunk & (alignof(std::uintptr_t) - 1U)) == 0;
}

bool ReadObjectSlot(
    const SymbolMemory& memory,
    const ObjectRegistryState& registry,
    std::uint32_t index,
    std::uintptr_t& object,
    std::uint32_t& serial) noexcept {
    if (registry.items == 0 || registry.chunk_size == 0 || registry.item_stride == 0 ||
        index >= registry.count || registry.count > registry.max_count ||
        registry.num_chunks > registry.max_chunks) {
        return false;
    }
    const std::uint32_t page = index / registry.chunk_size;
    const std::uint32_t slot = index % registry.chunk_size;
    std::uintptr_t chunk{};
    if (!ReadObjectChunk(memory, registry, page, chunk) ||
        slot > (std::numeric_limits<std::uint64_t>::max)() / registry.item_stride) {
        return false;
    }
    std::uintptr_t item{};
    std::uintptr_t object_address{};
    std::uintptr_t serial_address{};
    return AddUnsignedAddress(
               chunk, static_cast<std::uint64_t>(slot) * registry.item_stride, item) &&
        AddUnsignedAddress(item, registry.object_offset, object_address) &&
        AddUnsignedAddress(item, registry.serial_offset, serial_address) &&
        ReadValue(memory, object_address, object) && ReadValue(memory, serial_address, serial);
}

bool LoadObjectRegistry(
    const BuildProfile& profile,
    const SymbolMemory& memory,
    std::uintptr_t address,
    ObjectRegistryState& registry) noexcept {
    const auto items_offset = Layout(profile, "objects.itemsOffset");
    const auto count_offset = Layout(profile, "objects.countOffset");
    const auto max_count_offset = Layout(profile, "objects.maxCountOffset");
    const auto max_chunks_offset = Layout(profile, "objects.maxChunksOffset");
    const auto num_chunks_offset = Layout(profile, "objects.numChunksOffset");
    const auto chunk_count_size = Layout(
        profile, "objects.chunkCountSize", sizeof(std::uint32_t));
    const auto chunk_size = Layout(profile, "objects.chunkSize");
    const auto item_stride = Layout(profile, "objects.itemStride");
    const auto object_offset = Layout(profile, "objects.objectOffset");
    const auto serial_offset = Layout(profile, "objects.serialOffset");
    constexpr std::int64_t kMaximumHeaderOffset = 4096;
    constexpr std::int64_t kMaximumObjects = 16LL * 1024LL * 1024LL;
    constexpr std::int64_t kMaximumChunks = 4096;
    if (items_offset < 0 || count_offset < 0 || max_count_offset < 0 ||
        max_chunks_offset < 0 || num_chunks_offset < 0 ||
        items_offset > kMaximumHeaderOffset || count_offset > kMaximumHeaderOffset ||
        max_count_offset > kMaximumHeaderOffset ||
        max_chunks_offset > kMaximumHeaderOffset ||
        num_chunks_offset > kMaximumHeaderOffset ||
        (chunk_count_size != static_cast<std::int64_t>(sizeof(std::uint16_t)) &&
         chunk_count_size != static_cast<std::int64_t>(sizeof(std::uint32_t))) ||
        chunk_size <= 0 ||
        chunk_size > kMaximumObjects ||
        (chunk_size & (chunk_size - 1)) != 0 || item_stride <
            static_cast<std::int64_t>(sizeof(std::uintptr_t)) || item_stride > 4096 ||
        object_offset < 0 || object_offset > item_stride -
            static_cast<std::int64_t>(sizeof(std::uintptr_t)) || serial_offset < 0 ||
        serial_offset > item_stride - static_cast<std::int64_t>(sizeof(std::uint32_t))) {
        return false;
    }

    ObjectRegistryState next;
    next.chunk_size = static_cast<std::uint32_t>(chunk_size);
    next.item_stride = static_cast<std::uint32_t>(item_stride);
    next.object_offset = static_cast<std::uint32_t>(object_offset);
    next.serial_offset = static_cast<std::uint32_t>(serial_offset);
    std::uintptr_t count_address{};
    std::uintptr_t max_count_address{};
    std::uintptr_t max_chunks_address{};
    std::uintptr_t num_chunks_address{};
    const auto read_chunk_count = [&](std::uintptr_t field, std::uint32_t& value) {
        if (chunk_count_size == static_cast<std::int64_t>(sizeof(std::uint16_t))) {
            std::uint16_t packed{};
            if (!ReadValue(memory, field, packed)) return false;
            value = packed;
            return true;
        }
        return ReadValue(memory, field, value);
    };
    if (!ReadPointerAt(memory, address, items_offset, next.items) ||
        !AddAddress(address, count_offset, count_address) ||
        !AddAddress(address, max_count_offset, max_count_address) ||
        !AddAddress(address, max_chunks_offset, max_chunks_address) ||
        !AddAddress(address, num_chunks_offset, num_chunks_address) ||
        !ReadValue(memory, count_address, next.count) ||
        !ReadValue(memory, max_count_address, next.max_count) ||
        !read_chunk_count(max_chunks_address, next.max_chunks) ||
        !read_chunk_count(num_chunks_address, next.num_chunks) || next.max_count == 0 ||
        next.max_count > kMaximumObjects || next.count > next.max_count ||
        next.max_chunks == 0 || next.max_chunks > kMaximumChunks ||
        next.num_chunks > next.max_chunks ||
        static_cast<std::uint64_t>(next.max_count) >
            static_cast<std::uint64_t>(next.max_chunks) * next.chunk_size) {
        return false;
    }
    const std::uint64_t required_chunks = next.count == 0 ? 0 :
        (static_cast<std::uint64_t>(next.count) + next.chunk_size - 1U) / next.chunk_size;
    if (required_chunks > next.num_chunks) return false;

    constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    next.chunk_signature = kFnvOffset;
    for (std::uint32_t page = 0; page < required_chunks; ++page) {
        std::uintptr_t chunk{};
        if (!ReadObjectChunk(memory, next, page, chunk)) return false;
        next.chunk_signature ^= static_cast<std::uint64_t>(chunk);
        next.chunk_signature *= kFnvPrime;
    }
    if (next.count != 0) {
        std::uintptr_t ignored_object{};
        std::uint32_t ignored_serial{};
        if (!ReadObjectSlot(memory, next, 0, ignored_object, ignored_serial) ||
            !ReadObjectSlot(
                memory, next, next.count - 1U, ignored_object, ignored_serial)) {
            return false;
        }
    }
    registry = next;
    return true;
}

std::uint64_t EncodeObjectHandle(std::uint32_t index, std::uint32_t serial) noexcept {
    return (static_cast<std::uint64_t>(serial) << 32U) |
        (static_cast<std::uint64_t>(index) + 1U);
}

bool DecodeExactObjectPath(
    const AnomalyStringViewV1 path,
    std::wstring& decoded) {
    constexpr std::size_t kMaximumPathBytes = 16U * 1024U;
    decoded.clear();
    if (path.data == nullptr || path.size == 0 || path.size > kMaximumPathBytes ||
        path.size > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        std::memchr(path.data, '\0', path.size) != nullptr) {
        return false;
    }
    const int source_size = static_cast<int>(path.size);
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.data, source_size, nullptr, 0);
    if (count <= 0) return false;
    decoded.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(
               CP_UTF8,
               MB_ERR_INVALID_CHARS,
               path.data,
               source_size,
               decoded.data(),
               count) == count;
}

}  // namespace

struct Ue5NteAdapter::State {
    struct SemanticServiceEndpoint;
    struct CallbackEndpoint;
    struct AhudServiceEndpoint;

    BuildFingerprint fingerprint;
    BuildProfile profile;
    ProfileResolutionSnapshot resolution;
    std::shared_ptr<const SymbolMemory> memory;
    FeatureLayoutValidatorRegistry feature_layout_validators;
    AdapterServiceRegistry* services{};
    Ue5NteAdapter::ProcessEventInvoker process_event_invoker;
    Ue5NteAdapter::ObjectLookup object_lookup;
    mutable std::timed_mutex mutex;
    std::timed_mutex lifecycle_mutex;
    mutable std::recursive_timed_mutex publication_mutex;
    std::atomic<std::shared_ptr<const TickCallback>> configured_tick_callback;
    std::atomic_bool started{};
    std::atomic_bool stopping{};
    std::atomic<std::uint64_t> lifecycle_epoch{};
    std::atomic<DWORD> game_thread_id{};
    std::atomic<std::uint64_t> tick_sequence{};
    std::atomic<std::uint64_t> rejected_thread_ticks{};
    NteSnapshotSamplingOptions sampling;
    std::atomic_bool player_demand{};
    std::atomic_bool entity_demand{};
    std::atomic_bool navigation_demand{};
    static constexpr std::size_t kSessionEventCapacity = 64;
    static constexpr std::uint32_t kEntityPageCapacity = 256;
    struct SessionEvent {
        std::uint32_t kind{};
        std::uint64_t sequence{};
        std::uint64_t tick_sequence{};
        AnomalyGenerationHandleV1 previous_world{};
        AnomalyGenerationHandleV1 world{};
    };
    std::array<SessionEvent, kSessionEventCapacity> session_events{};
    std::size_t session_event_start{};
    std::size_t session_event_count{};
    std::uint64_t session_event_sequence{};
    std::uintptr_t world_pointer{};
    std::uint64_t world_generation{};
    std::uint64_t world_change_sequence{};
    std::uint32_t world_name_id{};
    bool world_name_layout_available{};
    bool world_name_readable{};
    ObjectRegistryState object_registry{};
    std::uint64_t object_generation{};
    struct ReflectedBoolParameter {
        std::uint16_t byte_offset{};
        std::uint8_t field_mask{};
        std::uint8_t byte_mask{};
    };
    struct TeleportBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::uint16_t new_location_offset{};
        std::uint16_t sweep_hit_result_offset{};
        std::uint16_t sweep_hit_result_size{};
        ReflectedBoolParameter b_sweep{};
        ReflectedBoolParameter b_teleport{};
        ReflectedBoolParameter return_value{};
        std::uint64_t object_generation{};
        std::uint32_t next_object_index{};
        bool available{};
        bool discovery_complete{};
    } teleport;
    struct NavigationBinding {
        std::uintptr_t move_to_point_by_transform{};
        std::uintptr_t util_class{};
        std::uint32_t move_object_index{};
        std::uint32_t move_object_serial{};
        std::uint16_t move_parms_size{};
        std::uint16_t world_context_object_offset{};
        std::uint16_t move_location_offset{};
        std::uint16_t move_rotator_offset{};
        ReflectedBoolParameter force_walk{};
        ReflectedBoolParameter auto_control{};
        ReflectedBoolParameter hide_ui{};
        std::uint16_t protect_time_offset{};
        ReflectedBoolParameter use_pathfinding{};
        std::uintptr_t stop_movement{};
        std::uint32_t stop_object_index{};
        std::uint32_t stop_object_serial{};
        std::uint16_t stop_parms_size{};
        std::uintptr_t registry_items{};
        std::uint64_t object_generation{};
        std::uint32_t next_object_index{};
        bool available{};
        bool discovery_complete{};
    } navigation;
    enum class AhudFunctionKind : std::size_t {
        ReceiveDrawHud,
        Project,
        DrawText,
        DrawLine,
        DrawRect,
        GetTextSize,
        Count,
    };
    static constexpr std::size_t kAhudFunctionCount =
        static_cast<std::size_t>(AhudFunctionKind::Count);
    static constexpr std::size_t kMaximumAhudParameters = 7;
    struct AhudFunctionBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::array<std::uint16_t, kMaximumAhudParameters> offsets{};
        std::array<ReflectedBoolParameter, kMaximumAhudParameters> bool_parameters{};
    };
    struct AhudBinding {
        std::array<AhudFunctionBinding, kAhudFunctionCount> functions{};
        std::uint64_t object_generation{};
    };
    struct AhudDiscovery {
        std::array<std::optional<AhudFunctionBinding>, kAhudFunctionCount> functions{};
        std::uint64_t object_generation{};
        std::uint32_t next_object_index{};
        bool discovery_complete{};
    } ahud_discovery;
    struct AhudParameterSpec {
        std::string_view name;
        std::string_view type;
        std::string_view structure;
        std::int32_t element_size{};
        bool return_value{};
    };
    struct AhudFunctionSpec {
        AhudFunctionKind kind{};
        std::string_view name;
        std::uint16_t parms_size{};
        std::span<const AhudParameterSpec> parameters;
    };
    struct AhudFrameCallContext {
        std::uintptr_t hud{};
        const AhudBinding* binding{};
        const ProcessEventInvoker* invoker{};
        std::atomic_uint64_t* process_event_call_count{};
    };
    struct NativeUtf16StringHeader {
        wchar_t* data{};
        std::int32_t count{};
        std::int32_t capacity{};
    };
    static_assert(sizeof(NativeUtf16StringHeader) == 16);
    std::atomic<std::shared_ptr<const AhudBinding>> ahud_binding;
    std::atomic_bool ahud_demand{};
    std::atomic_uint64_t ahud_frame_count{};
    std::atomic_uint64_t ahud_process_event_call_count{};
    std::uintptr_t player_pawn{};
    std::uintptr_t player_controller{};
    std::uintptr_t player_root{};
    std::uint64_t player_generation{};
    std::uint64_t player_attempt_sequence{};
    std::uint64_t player_sample_sequence{};
    std::array<double, 3> player_position{};
    std::array<double, 3> player_bounds_center{};
    std::array<double, 3> player_bounds_extent{};
    std::array<double, 3> camera_position{};
    std::array<double, 3> camera_rotation{};
    float camera_horizontal_fov{};
    bool player_available{};
    bool player_esp_available{};
    bool player_partial{};
    struct EntityRecord {
        std::uintptr_t actor{};
        std::uintptr_t class_object{};
        std::uint32_t object_index{};
        std::uint32_t object_serial{};
        std::uint32_t flags{};
        bool object_identity_available{};
        std::uint64_t entity_id{};
        std::uint64_t class_id{};
        std::uint32_t entity_name_id{};
        std::uint32_t class_name_id{};
        std::array<double, 3> bounds_center{};
        std::array<double, 3> bounds_extent{};
    };
    struct EntityFrameCache {
        std::vector<EntityRecord> entities;
        std::unordered_map<std::uint64_t, std::string> class_names;
        std::unordered_map<std::uint64_t, std::string> entity_names;
        std::uint64_t generation{};
        std::uint64_t sequence{};
        std::array<double, 3> camera_position{};
        std::array<double, 3> camera_rotation{};
        float camera_horizontal_fov{};
        bool partial{};
    };
    std::shared_ptr<const EntityFrameCache> entity_frame_cache;
    std::shared_ptr<const EntityFrameCache> previous_entity_frame_cache;
    std::uint64_t entity_generation{};
    std::uint64_t entity_attempt_sequence{};
    std::shared_ptr<const EntityFrameCache> actor_frame_cache;
    std::uint64_t actor_generation{};
    std::uint64_t actor_world_generation{};
    std::uint64_t snapshot_tick_count{};
    std::uint64_t latest_snapshot_cost_micros{};
    std::uint64_t total_snapshot_cost_micros{};
    std::uint64_t max_snapshot_cost_micros{};
    std::uint64_t player_refresh_count{};
    std::uint64_t player_cache_hit_count{};
    std::uint64_t entity_refresh_count{};
    std::uint64_t entity_cache_hit_count{};
    std::uint64_t entity_page_request_count{};
    std::uint64_t entity_page_cache_hit_count{};
    bool framework_hook_ready{};
    bool ahud_hook_ready{};
    std::shared_ptr<NteNavigationInputPolicy> navigation_input_policy;
    std::uint64_t deferred_resolution_retry_sequence{1};
    std::vector<std::pair<std::string, const void*>> published;
    std::vector<std::pair<std::string, const void*>> pending_revocations;
    std::optional<std::pair<std::string, const void*>> revocation_in_flight;
    std::atomic_bool revocation_call_active{};
    std::atomic<std::shared_ptr<SemanticServiceEndpoint>> semantic_endpoint;
    std::shared_ptr<SemanticServiceEndpoint> draining_semantic_endpoint;
    std::atomic<std::shared_ptr<CallbackEndpoint>> callback_endpoint;
    std::shared_ptr<CallbackEndpoint> draining_callback_endpoint;
    std::atomic<std::shared_ptr<AhudServiceEndpoint>> ahud_endpoint;
    std::shared_ptr<AhudServiceEndpoint> draining_ahud_endpoint;

    const ResolvedSymbol* Symbol(std::string_view id) const noexcept {
        return resolution.FindSymbol(id);
    }

    [[nodiscard]] static constexpr std::size_t AhudIndex(
        const AhudFunctionKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    [[nodiscard]] static AhudFunctionSpec AhudSpec(
        const AhudFunctionKind kind) noexcept {
        static constexpr std::array receive{
            AhudParameterSpec{"SizeX", "IntProperty", {}, 4, false},
            AhudParameterSpec{"SizeY", "IntProperty", {}, 4, false},
        };
        static constexpr std::array project{
            AhudParameterSpec{"Location", "StructProperty", "Vector", 24, false},
            AhudParameterSpec{"bClampToZeroPlane", "BoolProperty", {}, 1, false},
            AhudParameterSpec{"ReturnValue", "StructProperty", "Vector", 24, true},
        };
        static constexpr std::array draw_text{
            AhudParameterSpec{"Text", "StrProperty", {}, 16, false},
            AhudParameterSpec{"TextColor", "StructProperty", "LinearColor", 16, false},
            AhudParameterSpec{"ScreenX", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"ScreenY", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"Font", "ObjectProperty", {}, 8, false},
            AhudParameterSpec{"Scale", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"bScalePosition", "BoolProperty", {}, 1, false},
        };
        static constexpr std::array draw_line{
            AhudParameterSpec{"StartScreenX", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"StartScreenY", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"EndScreenX", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"EndScreenY", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"LineColor", "StructProperty", "LinearColor", 16, false},
            AhudParameterSpec{"LineThickness", "FloatProperty", {}, 4, false},
        };
        static constexpr std::array draw_rect{
            AhudParameterSpec{"RectColor", "StructProperty", "LinearColor", 16, false},
            AhudParameterSpec{"ScreenX", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"ScreenY", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"ScreenW", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"ScreenH", "FloatProperty", {}, 4, false},
        };
        static constexpr std::array get_text_size{
            AhudParameterSpec{"Text", "StrProperty", {}, 16, false},
            AhudParameterSpec{"OutWidth", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"OutHeight", "FloatProperty", {}, 4, false},
            AhudParameterSpec{"Font", "ObjectProperty", {}, 8, false},
            AhudParameterSpec{"Scale", "FloatProperty", {}, 4, false},
        };
        switch (kind) {
        case AhudFunctionKind::ReceiveDrawHud:
            return {kind, "ReceiveDrawHUD", 8, receive};
        case AhudFunctionKind::Project:
            return {kind, "Project", 56, project};
        case AhudFunctionKind::DrawText:
            return {kind, "DrawText", 53, draw_text};
        case AhudFunctionKind::DrawLine:
            return {kind, "DrawLine", 36, draw_line};
        case AhudFunctionKind::DrawRect:
            return {kind, "DrawRect", 32, draw_rect};
        case AhudFunctionKind::GetTextSize:
            return {kind, "GetTextSize", 36, get_text_size};
        case AhudFunctionKind::Count: break;
        }
        return {};
    }

    bool Publish(
        std::string id,
        std::uint32_t version,
        const void* table,
        AdapterServiceRegistry::QueryObserver query_observer = {},
        std::shared_ptr<const void> lifetime = {}) {
        std::scoped_lock publication_lock(publication_mutex);
        if (stopping.load(std::memory_order_acquire)) return false;
        if (!services->Publish(
                id, version, table, std::move(query_observer), std::move(lifetime))) {
            return false;
        }
        published.emplace_back(std::move(id), table);
        return true;
    }

    bool IsPublished(std::string_view id) const noexcept {
        std::scoped_lock publication_lock(publication_mutex);
        return std::ranges::any_of(published, [&](const auto& entry) {
            return entry.first == id;
        });
    }

    bool PublishIfMissing(
        std::string_view id,
        std::uint32_t version,
        const void* table,
        AdapterServiceRegistry::QueryObserver query_observer = {},
        std::shared_ptr<const void> lifetime = {}) {
        return IsPublished(id) || Publish(
            std::string(id), version, table, std::move(query_observer), std::move(lifetime));
    }

    [[nodiscard]] std::size_t PublishedCount() const noexcept {
        std::scoped_lock publication_lock(publication_mutex);
        return published.size();
    }

    [[nodiscard]] bool SemanticServicesAvailable() const noexcept {
        return resolution.state != ProfileResolutionState::NoProfile &&
            resolution.profile_hash == profile.source_hash;
    }

    [[nodiscard]] bool SemanticServicesRunning() const noexcept {
        return started.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool SemanticFeatureRunning(std::string_view feature) const noexcept {
        return SemanticServicesRunning() && SemanticFeatureAvailable(feature);
    }

    [[nodiscard]] static bool LayoutKeysAvailable(
        const BuildProfile& profile,
        std::initializer_list<std::string_view> keys) noexcept {
        constexpr std::int64_t kMaximumFieldOffset = 64LL * 1024LL * 1024LL;
        return std::ranges::all_of(keys, [&profile](const std::string_view key) {
            const auto value = Layout(profile, key);
            return value >= 0 && value <= kMaximumFieldOffset;
        });
    }

    [[nodiscard]] static bool FeatureDeclaresLayoutValidator(
        const BuildProfile& profile,
        std::string_view feature,
        std::string_view validator) noexcept {
        const auto validators = profile.feature_layout_validators.find(feature);
        return validators != profile.feature_layout_validators.end() && std::ranges::any_of(
            validators->second, [validator](const std::string& candidate) {
                return candidate == validator;
            });
    }

    [[nodiscard]] static bool FeatureDeclaresSymbol(
        const BuildProfile& profile,
        std::string_view feature,
        std::string_view symbol) noexcept {
        const auto symbols = profile.features.find(feature);
        return symbols != profile.features.end() && std::ranges::any_of(
            symbols->second, [symbol](const std::string& candidate) {
                return candidate == symbol;
            });
    }

    [[nodiscard]] static bool FeatureDeclaresDependency(
        const BuildProfile& profile,
        std::string_view feature,
        std::string_view dependency) noexcept {
        const auto dependencies = profile.feature_dependencies.find(feature);
        return dependencies != profile.feature_dependencies.end() && std::ranges::any_of(
            dependencies->second, [dependency](const std::string& candidate) {
                return candidate == dependency;
            });
    }

    [[nodiscard]] bool NtePlayerLayoutAvailable() const noexcept {
        return LayoutKeysAvailable(profile, {
            "world.gameInstance",
            "gameInstance.localPlayers",
            "localPlayer.controller",
            "controller.pawn",
            "actor.rootComponent",
            "sceneComponent.location"});
    }

    [[nodiscard]] bool NtePlayerEspLayoutAvailable() const noexcept {
        return NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
            "controller.cameraManager",
            "sceneComponent.boundsOrigin",
            "sceneComponent.boundsExtent",
            "cameraManager.location",
            "cameraManager.rotation",
            "cameraManager.fov"});
    }

    [[nodiscard]] bool NtePlayerTeleportAvailable() const noexcept {
        const auto* const process_event = resolution.FindSymbol("ue5.ProcessEvent");
        return static_cast<bool>(process_event_invoker) &&
            process_event != nullptr && process_event->Available() &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            resolution.FeatureAvailable("nte.player-teleport") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
            "object.class",
            "object.nameOffset",
            "object.outer",
            "ustruct.propertyLink",
            "ufunction.numParms",
            "ufunction.parmsSize",
            "ufunction.returnValueOffset",
            "ffield.name",
            "fproperty.arrayDim",
            "fproperty.elementSize",
            "fproperty.offsetInternal",
            "fproperty.propertyLinkNext",
            "fstructProperty.struct",
            "fboolProperty.fieldSize",
            "fboolProperty.byteOffset",
            "fboolProperty.byteMask",
            "fboolProperty.fieldMask"}) &&
            FeatureDeclaresSymbol(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventSymbol) &&
            FeatureDeclaresLayoutValidator(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventAbiValidator) &&
            FeatureDeclaresDependency(
                profile, "nte.player-teleport", "nte.player") &&
            FeatureDeclaresDependency(
                profile, "nte.player-teleport", "ue5.names") &&
            FeatureDeclaresDependency(
                profile, "nte.player-teleport", "ue5.objects") &&
            FeatureDeclaresDependency(
                profile, "nte.player-teleport", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile,
                "nte.player-teleport",
                "nte-player-teleport-layout-v1");
    }

    [[nodiscard]] bool NteNavigationReflectionAvailable() const noexcept {
        const auto* const process_event = resolution.FindSymbol("ue5.ProcessEvent");
        const auto* const input_policy = resolution.FindSymbol(
            "nte.ClientIgnoreGameAndUiInput");
        return static_cast<bool>(process_event_invoker) &&
            process_event != nullptr && process_event->Available() &&
            input_policy != nullptr && input_policy->Available() &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            resolution.FeatureAvailable("nte.navigation") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
                "object.class",
                "object.nameOffset",
                "object.outer",
                "uclass.classDefaultObject",
                "ustruct.superStruct",
                "ustruct.propertyLink",
                "ufunction.numParms",
                "ufunction.parmsSize",
                "ufunction.returnValueOffset",
                "ffield.class",
                "ffield.name",
                "ffieldClass.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "fstructProperty.struct",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset",
                "fboolProperty.byteMask",
                "fboolProperty.fieldMask",
                "controller.controlRotation",
                "controller.getPlayerCharacterVtableOffset",
                "character.setCustomIgnoreMoveInputVtableOffset",
                "character.setCustomLimitInputVtableOffset"}) &&
            FeatureDeclaresSymbol(
                profile, "nte.navigation", "nte.ClientIgnoreGameAndUiInput") &&
            FeatureDeclaresDependency(profile, "nte.navigation", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.navigation", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.navigation", "ue5.objects") &&
            FeatureDeclaresDependency(
                profile, "nte.navigation", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.navigation", "nte-navigation-layout-v1") &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.navigation", "nte-navigation-input-abi-v1");
    }

    [[nodiscard]] bool NteNavigationAvailable() const noexcept {
        return NteNavigationReflectionAvailable() && navigation_input_policy != nullptr &&
            navigation_input_policy->Started();
    }

    [[nodiscard]] bool EnsureNavigationInputPolicyLocked() noexcept {
        if (navigation_input_policy == nullptr) return false;
        if (navigation_input_policy->Started()) return true;
        if (!NteNavigationReflectionAvailable()) return false;
        const auto* const target = resolution.FindSymbol("nte.ClientIgnoreGameAndUiInput");
        if (target == nullptr || !target->Available()) return false;
        return navigation_input_policy->Start(reinterpret_cast<void*>(target->address));
    }

    [[nodiscard]] bool ObjectFindAvailable() const noexcept {
        return static_cast<bool>(object_lookup) &&
            resolution.FeatureAvailable(kUe5ObjectFindFeature) &&
            resolution.FeatureAvailable("ue5.objects") &&
            FeatureDeclaresSymbol(
                profile, kUe5ObjectFindFeature, kUe5StaticFindObjectSymbol) &&
            FeatureDeclaresDependency(
                profile, kUe5ObjectFindFeature, "ue5.objects") &&
            FeatureDeclaresLayoutValidator(
                profile,
                kUe5ObjectFindFeature,
                kUe5StaticFindObjectAbiValidator);
    }

    [[nodiscard]] bool AhudFeatureAvailable() const noexcept {
        return static_cast<bool>(process_event_invoker) &&
            framework_hook_ready && ahud_hook_ready &&
            resolution.FeatureAvailable("ue5.ahud") &&
            resolution.FeatureAvailable("ue5.functions") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            resolution.FeatureAvailable(kUe5ActorProcessEventFeature) &&
            LayoutKeysAvailable(profile, {
                "object.class",
                "object.nameOffset",
                "object.outer",
                "ustruct.propertyLink",
                "ufunction.numParms",
                "ufunction.parmsSize",
                "ufunction.returnValueOffset",
                "ffield.class",
                "ffield.name",
                "ffieldClass.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "fstructProperty.struct",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset",
                "fboolProperty.byteMask",
                "fboolProperty.fieldMask"}) &&
            FeatureDeclaresDependency(profile, "ue5.ahud", "ue5.functions") &&
            FeatureDeclaresDependency(
                profile, "ue5.ahud", kUe5ActorProcessEventFeature) &&
            FeatureDeclaresDependency(
                profile,
                kUe5ActorProcessEventFeature,
                kUe5ProcessEventFeature) &&
            FeatureDeclaresSymbol(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventSymbol) &&
            FeatureDeclaresLayoutValidator(
                profile,
                kUe5ProcessEventFeature,
                kUe5ProcessEventAbiValidator) &&
            FeatureDeclaresSymbol(
                profile,
                kUe5ActorProcessEventFeature,
                kUe5ActorProcessEventSymbol) &&
            FeatureDeclaresLayoutValidator(
                profile,
                kUe5ActorProcessEventFeature,
                kUe5ActorProcessEventAbiValidator) &&
            FeatureDeclaresLayoutValidator(
                profile, "ue5.ahud", "ue5-ahud-reflection-v1");
    }

    [[nodiscard]] bool NteEntitiesLayoutAvailable() const noexcept {
        return LayoutKeysAvailable(profile, {
            "world.persistentLevel",
            "level.actors",
            "actor.rootComponent",
            "sceneComponent.boundsOrigin",
            "sceneComponent.boundsExtent"});
    }

    [[nodiscard]] bool NteEntityReflectionLayoutAvailable() const noexcept {
        return NteEntitiesLayoutAvailable() &&
            resolution.FeatureAvailable("ue5.names") && LayoutKeysAvailable(profile, {
                "object.class",
                "ustruct.propertyLink",
                "ffield.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset",
                "fboolProperty.byteMask",
                "fboolProperty.fieldMask"});
    }

    [[nodiscard]] bool NteActorsLayoutAvailable() const noexcept {
        return NteEntityReflectionLayoutAvailable() &&
            SemanticFeatureAvailable("nte.entities") && LayoutKeysAvailable(profile, {
                "world.levels",
                "entities.maxLevels"});
    }

    [[nodiscard]] bool SemanticFeatureAvailable(std::string_view feature) const noexcept {
        if (!SemanticServicesAvailable() || !resolution.FeatureAvailable(feature)) return false;
        if (feature == "nte.player") return NtePlayerLayoutAvailable();
        if (feature == "nte.player-esp") {
            return resolution.FeatureAvailable("nte.player") && NtePlayerEspLayoutAvailable();
        }
        if (feature == "nte.player-teleport") {
            return resolution.FeatureAvailable("nte.player") &&
                NtePlayerTeleportAvailable();
        }
        if (feature == "nte.navigation") return NteNavigationAvailable();
        if (feature == "nte.entities") {
            return NteEntitiesLayoutAvailable();
        }
        return feature == "nte.session";
    }

    [[nodiscard]] bool MetricsFeatureAvailable() const noexcept {
        return SemanticFeatureAvailable("nte.session") ||
            SemanticFeatureAvailable("nte.player") ||
            SemanticFeatureAvailable("nte.entities");
    }

    bool PublishAvailableServices(const std::weak_ptr<State>& self);

    void RevokePublishedFrom(std::size_t first) noexcept {
        static_cast<void>(RevokePublishedFromUntil(
            first, std::chrono::steady_clock::time_point::max()));
    }

    [[nodiscard]] bool RevokePublishedFromUntil(
        std::size_t first,
        std::chrono::steady_clock::time_point deadline) noexcept {
        {
            std::unique_lock publication_lock(publication_mutex, std::defer_lock);
            if (!LockUntil(publication_lock, deadline)) return false;
            const std::size_t begin = (std::min)(first, published.size());
            try {
                pending_revocations.reserve(
                    pending_revocations.size() + published.size() - begin);
            } catch (...) {
                return false;
            }
            for (std::size_t index = begin; index < published.size(); ++index) {
                pending_revocations.emplace_back(std::move(published[index]));
            }
            published.resize(begin);
        }
        return RevokePendingUntil(deadline);
    }

    [[nodiscard]] bool RevokePendingUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        for (;;) {
            std::string_view id;
            const void* table{};
            {
                std::unique_lock publication_lock(publication_mutex, std::defer_lock);
                if (!LockUntil(publication_lock, deadline)) return false;
                // The in-flight entry owns the string behind id. Exactly one
                // revoker may borrow it until that owner has finalized the
                // registry call, otherwise another Stop could move it away.
                if (revocation_call_active.load(std::memory_order_acquire)) return false;
                if (!revocation_in_flight) {
                    if (pending_revocations.empty()) return true;
                    revocation_in_flight.emplace(std::move(pending_revocations.back()));
                    pending_revocations.pop_back();
                }
                revocation_call_active.store(true, std::memory_order_release);
                id = revocation_in_flight->first;
                table = revocation_in_flight->second;
            }

            const auto result = services->RevokeUntil(id, table, deadline);
            std::optional<std::pair<std::string, const void*>> completed;
            {
                std::unique_lock publication_lock(publication_mutex, std::defer_lock);
                if (!LockUntil(publication_lock, deadline)) {
                    revocation_call_active.store(false, std::memory_order_release);
                    return false;
                }
                if (result == AdapterServiceRegistry::RevokeResult::TimedOut) {
                    revocation_call_active.store(false, std::memory_order_release);
                    return false;
                }
                if (revocation_in_flight &&
                    revocation_in_flight->first == id &&
                    revocation_in_flight->second == table) {
                    completed.emplace(std::move(*revocation_in_flight));
                    revocation_in_flight.reset();
                }
                revocation_call_active.store(false, std::memory_order_release);
            }
            // The retired identifier is released after the publication lock.
        }
    }

    [[nodiscard]] bool ServiceAvailableForPublication(std::string_view id) const noexcept {
        if (id == ANOMALY_UE5_BUILD_SERVICE_V1_ID || id == ANOMALY_NTE_BUILD_SERVICE_V1_ID) {
            return true;
        }
        if (id == ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID) {
            return framework_hook_ready && resolution.FeatureAvailable("ue5.framework");
        }
        if (id == ANOMALY_UE5_AHUD_SERVICE_V1_ID) {
            return AhudFeatureAvailable();
        }
        if (id == ANOMALY_UE5_NAMES_SERVICE_V1_ID) {
            return resolution.FeatureAvailable("ue5.names");
        }
        if (id == ANOMALY_UE5_OBJECTS_SERVICE_V1_ID) {
            return framework_hook_ready && resolution.FeatureAvailable("ue5.objects");
        }
        if (id == ANOMALY_UE5_WORLD_SERVICE_V1_ID) {
            return framework_hook_ready && resolution.FeatureAvailable("ue5.world");
        }
        if (id == ANOMALY_NTE_SESSION_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.session");
        }
        if (id == ANOMALY_NTE_METRICS_SERVICE_V1_ID) {
            return framework_hook_ready && MetricsFeatureAvailable();
        }
        if (id == ANOMALY_NTE_PLAYER_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.player");
        }
        if (id == ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID) {
            return framework_hook_ready &&
                SemanticFeatureAvailable("nte.player-teleport");
        }
        if (id == ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.navigation");
        }
        if (id == ANOMALY_NTE_ENTITIES_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.entities");
        }
        if (id == ANOMALY_NTE_ACTORS_SERVICE_V1_ID) {
            return framework_hook_ready && NteActorsLayoutAvailable();
        }
        return false;
    }

    void RevokePublishedUnavailable() {
        {
            std::scoped_lock publication_lock(publication_mutex);
            const auto unavailable = std::count_if(
                published.begin(), published.end(), [this](const auto& service) {
                    return !ServiceAvailableForPublication(service.first);
                });
            pending_revocations.reserve(
                pending_revocations.size() + static_cast<std::size_t>(unavailable));
            auto service = published.begin();
            while (service != published.end()) {
                if (ServiceAvailableForPublication(service->first)) {
                    ++service;
                    continue;
                }
                pending_revocations.emplace_back(std::move(*service));
                service = published.erase(service);
            }
        }
        static_cast<void>(RevokePendingUntil(std::chrono::steady_clock::time_point::max()));
    }

    void RefreshDeferredResolution(
        std::uint64_t sequence,
        const std::shared_ptr<State>& self) noexcept {
        constexpr std::uint64_t kRetryIntervalTicks = 60;
        if (!started.load(std::memory_order_acquire) || !framework_hook_ready ||
            sequence < deferred_resolution_retry_sequence) {
            return;
        }
        deferred_resolution_retry_sequence = sequence + kRetryIntervalTicks;
        try {
            ProfileResolutionSnapshot refreshed = resolution;
            SymbolResolver resolver(
                memory, {}, {}, feature_layout_validators);
            if (!resolver.RevalidateDeferredCandidates(profile, refreshed)) return;

            if (!started.load(std::memory_order_acquire)) return;
            const auto first_new_service = PublishedCount();
            const ProfileResolutionSnapshot previous = std::move(resolution);
            resolution = std::move(refreshed);
            static_cast<void>(EnsureNavigationInputPolicyLocked());
            if (!PublishAvailableServices(self)) {
                RevokePublishedFrom(first_new_service);
                resolution = previous;
            } else {
                RevokePublishedUnavailable();
            }
        } catch (...) {
        }
    }

    std::uint32_t FeatureState(AnomalyStringViewV1 id) const noexcept {
        if (id.data == nullptr) return ANOMALY_FEATURE_V1_UNAVAILABLE;
        std::scoped_lock lock(mutex);
        const std::string_view feature(id.data, id.size);
        if (feature == "nte.metrics") {
            return MetricsFeatureAvailable()
                ? ANOMALY_FEATURE_V1_AVAILABLE
                : ANOMALY_FEATURE_V1_UNAVAILABLE;
        }
        if (feature == "ue5.ahud") {
            return AhudFeatureAvailable()
                ? ANOMALY_FEATURE_V1_AVAILABLE
                : ANOMALY_FEATURE_V1_UNAVAILABLE;
        }
        if (feature == "nte.session" || feature == "nte.player" ||
            feature == "nte.player-esp" ||
            feature == "nte.player-teleport" || feature == "nte.navigation" ||
            feature == "nte.entities") {
            return SemanticFeatureAvailable(feature)
                ? ANOMALY_FEATURE_V1_AVAILABLE
                : ANOMALY_FEATURE_V1_UNAVAILABLE;
        }
        return resolution.FeatureAvailable(feature)
            ? ANOMALY_FEATURE_V1_AVAILABLE
            : ANOMALY_FEATURE_V1_UNAVAILABLE;
    }

    static bool SamplingDue(
        std::uint64_t sequence,
        std::uint64_t previous_attempt,
        std::uint32_t interval) noexcept {
        return previous_attempt == 0 || sequence - previous_attempt >= interval;
    }

    void InvalidateEntities() noexcept {
        if (entity_frame_cache || previous_entity_frame_cache) ++entity_generation;
        entity_frame_cache.reset();
        previous_entity_frame_cache.reset();
    }

    void InvalidateActors() noexcept {
        if (actor_frame_cache) ++actor_generation;
        actor_frame_cache.reset();
        actor_world_generation = 0;
    }

    void InvalidatePlayer() noexcept {
        const bool had_identity = player_pawn != 0 || player_controller != 0 || player_root != 0 ||
            player_available || player_esp_available;
        if (had_identity) ++player_generation;
        player_pawn = 0;
        player_controller = 0;
        player_root = 0;
        player_sample_sequence = 0;
        player_position = {};
        player_bounds_center = {};
        player_bounds_extent = {};
        camera_position = {};
        camera_rotation = {};
        camera_horizontal_fov = 0.0F;
        player_available = false;
        player_esp_available = false;
        player_partial = false;
    }

    void InvalidateAhudBindingLocked() noexcept {
        ahud_binding.store({}, std::memory_order_release);
        ahud_discovery = {};
        ahud_discovery.object_generation = object_generation;
    }

    void ResetForStartLocked() noexcept {
        session_event_start = 0;
        session_event_count = 0;
        // Keep session cursors monotonic across Host lifecycles. Reserving one
        // value before a restarted stream makes every prior non-zero cursor
        // older than the new retained range, even after a new event is added.
        if (session_event_sequence != 0) ++session_event_sequence;
        session_events = {};
        if (world_pointer != 0) ++world_generation;
        world_pointer = 0;
        world_name_id = 0;
        world_name_layout_available = false;
        world_name_readable = false;
        if (object_registry.items != 0) ++object_generation;
        object_registry = {};
        teleport = {};
        navigation = {};
        InvalidateAhudBindingLocked();
        InvalidatePlayer();
        InvalidateEntities();
        InvalidateActors();
        entity_attempt_sequence = 0;
        player_attempt_sequence = 0;
        player_demand.store(false, std::memory_order_release);
        entity_demand.store(false, std::memory_order_release);
        navigation_demand.store(false, std::memory_order_release);
        ahud_demand.store(false, std::memory_order_release);
        ahud_frame_count.store(0, std::memory_order_release);
        ahud_process_event_call_count.store(0, std::memory_order_release);
        game_thread_id.store(0, std::memory_order_release);
        tick_sequence.store(0, std::memory_order_release);
        rejected_thread_ticks.store(0, std::memory_order_release);
        snapshot_tick_count = 0;
        latest_snapshot_cost_micros = 0;
        total_snapshot_cost_micros = 0;
        max_snapshot_cost_micros = 0;
        player_refresh_count = 0;
        player_cache_hit_count = 0;
        entity_refresh_count = 0;
        entity_cache_hit_count = 0;
        entity_page_request_count = 0;
        entity_page_cache_hit_count = 0;
        deferred_resolution_retry_sequence = 1;
    }

    void ClearSemanticStateForStopLocked() noexcept {
        player_demand.store(false, std::memory_order_release);
        entity_demand.store(false, std::memory_order_release);
        navigation_demand.store(false, std::memory_order_release);
        InvalidatePlayer();
        InvalidateEntities();
        InvalidateActors();
        if (world_pointer != 0) {
            world_pointer = 0;
            ++world_generation;
            ++world_change_sequence;
        }
        world_name_id = 0;
        world_name_layout_available = false;
        world_name_readable = false;
        teleport = {};
        navigation = {};
        framework_hook_ready = false;
        ahud_hook_ready = false;
        InvalidateAhudBindingLocked();
    }

    void RecordSessionEvent(
        const std::uint32_t kind,
        const std::uint64_t tick,
        const AnomalyGenerationHandleV1 previous_world,
        const AnomalyGenerationHandleV1 world) noexcept {
        const SessionEvent event{kind, ++session_event_sequence, tick, previous_world, world};
        if (session_event_count == kSessionEventCapacity) {
            session_events[session_event_start] = event;
            session_event_start = (session_event_start + 1U) % kSessionEventCapacity;
            return;
        }
        const std::size_t slot =
            (session_event_start + session_event_count) % kSessionEventCapacity;
        session_events[slot] = event;
        ++session_event_count;
    }

    void RefreshWorld(const std::uint64_t tick) noexcept {
        const auto* world_symbol = Symbol("ue5.GWorld");
        std::uintptr_t next{};
        if (world_symbol != nullptr && world_symbol->Available()) {
            static_cast<void>(ReadValue(*memory, world_symbol->address, next));
        }
        if (next != world_pointer) {
            const AnomalyGenerationHandleV1 previous_world = world_pointer == 0
                ? AnomalyGenerationHandleV1{}
                : AnomalyGenerationHandleV1{1, world_generation};
            InvalidatePlayer();
            InvalidateEntities();
            InvalidateActors();
            world_pointer = next;
            ++world_generation;
            ++world_change_sequence;
            const AnomalyGenerationHandleV1 world = world_pointer == 0
                ? AnomalyGenerationHandleV1{}
                : AnomalyGenerationHandleV1{1, world_generation};
            const std::uint32_t event_kind = previous_world.id == 0
                ? ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY
                : world.id == 0
                    ? ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE
                    : ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED;
            RecordSessionEvent(event_kind, tick, previous_world, world);
        }
        world_name_id = 0;
        auto name_offset = Layout(profile, "world.nameOffset");
        if (name_offset < 0) name_offset = Layout(profile, "object.nameOffset");
        world_name_layout_available = name_offset >= 0;
        world_name_readable = false;
        std::uintptr_t address{};
        if (world_pointer != 0 && world_name_layout_available &&
            AddAddress(world_pointer, name_offset, address)) {
            world_name_readable = ReadValue(*memory, address, world_name_id);
        }
    }

    void RefreshObjects() noexcept {
        const std::uint64_t previous_generation = object_generation;
        const auto* objects = Symbol("ue5.GObjects");
        ObjectRegistryState next;
        const bool available = objects != nullptr && objects->Available() &&
            LoadObjectRegistry(profile, *memory, objects->address, next);
        if (!available) {
            if (object_registry.items != 0) ++object_generation;
            object_registry = {};
            teleport = {};
            navigation = {};
            if (object_generation != previous_generation) {
                InvalidateAhudBindingLocked();
            }
            return;
        }
        if (object_registry.items == 0 || next.items != object_registry.items ||
            next.count < object_registry.count || next.max_count != object_registry.max_count ||
            next.max_chunks != object_registry.max_chunks ||
            next.num_chunks < object_registry.num_chunks ||
            next.chunk_signature != object_registry.chunk_signature) {
            ++object_generation;
            teleport = {};
            navigation = {};
        }
        object_registry = next;
        if (object_generation != previous_generation) {
            InvalidateAhudBindingLocked();
        }
    }

    struct PlayerLocationSample {
        std::uintptr_t controller{};
        std::uintptr_t pawn{};
        std::uintptr_t root{};
        std::uintptr_t location{};
        std::array<double, 3> position{};
    };

    [[nodiscard]] bool ReadCurrentPlayerLocation(PlayerLocationSample& sample) const noexcept {
        if (!SemanticFeatureAvailable("nte.player") || world_pointer == 0) return false;
        std::uintptr_t game_instance{};
        std::uintptr_t players_array{};
        std::int32_t players_count{};
        std::uintptr_t local_player{};
        if (!ReadPointerAt(*memory, world_pointer, Layout(profile, "world.gameInstance"), game_instance) ||
            !ReadPointerAt(*memory, game_instance, Layout(profile, "gameInstance.localPlayers"), players_array)) {
            return false;
        }
        std::uintptr_t count_address{};
        std::int64_t local_players_count_offset{};
        if (!AddLayoutOffset(
                Layout(profile, "gameInstance.localPlayers"),
                static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                local_players_count_offset) ||
            !AddAddress(game_instance, local_players_count_offset, count_address) ||
            !ReadValue(*memory, count_address, players_count) || players_count < 1 ||
            !ReadValue(*memory, players_array, local_player) || local_player == 0 ||
            !ReadPointerAt(
                *memory, local_player, Layout(profile, "localPlayer.controller"), sample.controller) ||
            !ReadPointerAt(*memory, sample.controller, Layout(profile, "controller.pawn"), sample.pawn) ||
            !ReadPointerAt(*memory, sample.pawn, Layout(profile, "actor.rootComponent"), sample.root)) {
            return false;
        }
        if (!AddAddress(sample.root, Layout(profile, "sceneComponent.location"), sample.location) ||
            !memory->Read(sample.location, sample.position.data(), sizeof(sample.position)) ||
            !std::ranges::all_of(
                sample.position, [](double value) { return std::isfinite(value); })) {
            return false;
        }
        return true;
    }

    void RefreshPlayer(std::uint64_t sequence) noexcept {
        player_attempt_sequence = sequence;
        PlayerLocationSample sample;
        if (!ReadCurrentPlayerLocation(sample)) {
            InvalidatePlayer();
            return;
        }
        if (sample.pawn != player_pawn || sample.controller != player_controller ||
            sample.root != player_root) {
            player_pawn = sample.pawn;
            player_controller = sample.controller;
            player_root = sample.root;
            ++player_generation;
        }
        player_position = sample.position;
        player_available = true;
        player_esp_available = false;
        player_partial = false;
        player_sample_sequence = sequence;
        player_bounds_center = {};
        player_bounds_extent = {};
        camera_position = {};
        camera_rotation = {};
        camera_horizontal_fov = 0.0F;

        std::uintptr_t camera_manager{};
        std::uintptr_t bounds_center_address{};
        std::uintptr_t bounds_extent_address{};
        std::uintptr_t camera_position_address{};
        std::uintptr_t camera_rotation_address{};
        std::uintptr_t camera_fov_address{};
        std::array<double, 3> bounds_center{};
        std::array<double, 3> bounds_extent{};
        std::array<double, 3> next_camera_position{};
        std::array<double, 3> next_camera_rotation{};
        float horizontal_fov{};
        if (!SemanticFeatureAvailable("nte.player-esp") ||
            !ReadPointerAt(
                *memory, sample.controller, Layout(profile, "controller.cameraManager"), camera_manager) ||
            !AddAddress(sample.root, Layout(profile, "sceneComponent.boundsOrigin"), bounds_center_address) ||
            !AddAddress(sample.root, Layout(profile, "sceneComponent.boundsExtent"), bounds_extent_address) ||
            !AddAddress(camera_manager, Layout(profile, "cameraManager.location"), camera_position_address) ||
            !AddAddress(camera_manager, Layout(profile, "cameraManager.rotation"), camera_rotation_address) ||
            !AddAddress(camera_manager, Layout(profile, "cameraManager.fov"), camera_fov_address) ||
            !memory->Read(bounds_center_address, bounds_center.data(), sizeof(bounds_center)) ||
            !memory->Read(bounds_extent_address, bounds_extent.data(), sizeof(bounds_extent)) ||
            !memory->Read(
                camera_position_address, next_camera_position.data(), sizeof(next_camera_position)) ||
            !memory->Read(
                camera_rotation_address, next_camera_rotation.data(), sizeof(next_camera_rotation)) ||
            !ReadValue(*memory, camera_fov_address, horizontal_fov)) {
            player_partial = SemanticFeatureAvailable("nte.player-esp");
            return;
        }
        const auto finite = [](const std::array<double, 3>& values) {
            return std::ranges::all_of(values, [](double value) { return std::isfinite(value); });
        };
        if (!finite(bounds_center) || !finite(bounds_extent) || !finite(next_camera_position) ||
            !finite(next_camera_rotation) || !std::isfinite(horizontal_fov) ||
            std::ranges::any_of(bounds_extent, [](double value) { return value <= 0.0; }) ||
            horizontal_fov <= 5.0F || horizontal_fov >= 175.0F) {
            player_partial = true;
            return;
        }
        player_bounds_center = bounds_center;
        player_bounds_extent = bounds_extent;
        camera_position = next_camera_position;
        camera_rotation = next_camera_rotation;
        camera_horizontal_fov = horizontal_fov;
        player_esp_available = true;
    }

    void RefreshEntityCache(
        std::uint64_t sequence,
        bool all_levels,
        std::shared_ptr<const EntityFrameCache>& destination,
        std::uint64_t& generation) noexcept {
        const auto invalidate = [&]() noexcept {
            if (destination) ++generation;
            destination.reset();
        };
        try {
            std::vector<EntityRecord> next;
            std::unordered_map<std::uint64_t, std::string> class_names;
            std::unordered_map<std::uint64_t, std::string> entity_names;
            const auto actors_offset = Layout(profile, "level.actors");
            const auto maximum = Layout(profile, "entities.maxCount", 16384);
            const auto maximum_levels = Layout(profile, "entities.maxLevels", 4096);
            std::int64_t actor_count_offset{};
            if ((!all_levels && !SemanticFeatureAvailable("nte.entities")) ||
                (all_levels && !NteActorsLayoutAvailable()) || maximum <= 0 ||
                maximum > (std::numeric_limits<std::int32_t>::max)() ||
                (all_levels && (maximum_levels <= 0 || maximum_levels > 4096)) ||
                !AddLayoutOffset(
                    actors_offset, static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                    actor_count_offset)) {
                invalidate();
                return;
            }

            bool partial{!player_esp_available};
            std::vector<std::uintptr_t> levels;
            const auto levels_offset = Layout(profile, "world.levels");
            if (all_levels) {
                std::uintptr_t levels_array{};
                std::uintptr_t levels_count_address{};
                std::int32_t level_count{};
                if (!ReadPointerAt(*memory, world_pointer, levels_offset, levels_array) ||
                    !AddAddress(
                        world_pointer,
                        levels_offset + static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                        levels_count_address) ||
                    !ReadValue(*memory, levels_count_address, level_count) || level_count < 0 ||
                    level_count > maximum_levels || (level_count != 0 && levels_array == 0)) {
                    invalidate();
                    return;
                }
                levels.reserve(static_cast<std::size_t>(level_count));
                for (std::int32_t index = 0; index < level_count; ++index) {
                    std::uintptr_t level_slot{};
                    std::uintptr_t level{};
                    if (!AddAddress(
                            levels_array,
                            static_cast<std::int64_t>(index) *
                                static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                            level_slot) ||
                        !ReadValue(*memory, level_slot, level)) {
                        partial = true;
                        continue;
                    }
                    if (level != 0) levels.push_back(level);
                }
            } else {
                std::uintptr_t level{};
                if (!ReadPointerAt(
                        *memory, world_pointer, Layout(profile, "world.persistentLevel"), level)) {
                    invalidate();
                    return;
                }
                levels.push_back(level);
            }

            std::unordered_set<std::uintptr_t> seen_actors;
            if (all_levels) {
                next.reserve(static_cast<std::size_t>(maximum));
                class_names.reserve(256);
                entity_names.reserve(static_cast<std::size_t>(maximum));
                seen_actors.reserve(static_cast<std::size_t>(maximum));
            }
            const auto finite = [](const std::array<double, 3>& values) {
                return std::ranges::all_of(values, [](double value) { return std::isfinite(value); });
            };
            std::uint64_t fallback_index{};
            std::int64_t total_actor_slots{};
            for (const std::uintptr_t level : levels) {
                std::uintptr_t actor_array{};
                std::uintptr_t count_address{};
                std::int32_t actor_count{};
                if (!ReadPointerAt(*memory, level, actors_offset, actor_array) ||
                    !AddAddress(level, actor_count_offset, count_address) ||
                    !ReadValue(*memory, count_address, actor_count) || actor_count < 0 ||
                    actor_count > maximum - total_actor_slots ||
                    (actor_count != 0 && actor_array == 0)) {
                    invalidate();
                    return;
                }
                total_actor_slots += actor_count;
                if (!all_levels) {
                    next.reserve(static_cast<std::size_t>(actor_count));
                    class_names.reserve(static_cast<std::size_t>(actor_count));
                    entity_names.reserve(static_cast<std::size_t>(actor_count));
                }
                for (std::int32_t index = 0; index < actor_count; ++index, ++fallback_index) {
                    std::uintptr_t actor_slot{};
                    std::uintptr_t actor{};
                    if (!AddAddress(
                            actor_array,
                            static_cast<std::int64_t>(index) *
                                static_cast<std::int64_t>(sizeof(std::uintptr_t)),
                            actor_slot) ||
                        !ReadValue(*memory, actor_slot, actor)) {
                        partial = true;
                        continue;
                    }
                    if (actor == 0 || (all_levels && !seen_actors.insert(actor).second)) continue;
                    std::uintptr_t root{};
                    std::uintptr_t pointer_address{};
                    std::uintptr_t bounds_center_address{};
                    std::uintptr_t bounds_extent_address{};
                    if (!AddAddress(
                            actor, Layout(profile, "actor.rootComponent"), pointer_address) ||
                        !ReadValue(*memory, pointer_address, root)) {
                        partial = true;
                        continue;
                    }
                    // PersistentLevel commonly contains actors without a scene root. They
                    // are not renderable entity candidates, but their presence does not
                    // make the rest of the actor-array sample incomplete.
                    if (root == 0) continue;
                    if (!AddAddress(
                            root, Layout(profile, "sceneComponent.boundsOrigin"),
                            bounds_center_address) ||
                        !AddAddress(
                            root, Layout(profile, "sceneComponent.boundsExtent"),
                            bounds_extent_address)) {
                        partial = true;
                        continue;
                    }

                    EntityRecord entity;
                    entity.actor = actor;
                    if (!memory->Read(
                            bounds_center_address, entity.bounds_center.data(),
                            sizeof(entity.bounds_center)) ||
                        !memory->Read(
                            bounds_extent_address, entity.bounds_extent.data(),
                            sizeof(entity.bounds_extent))) {
                        partial = true;
                        continue;
                    }
                    // Successfully-read zero, non-finite, or otherwise unusable bounds
                    // describe a non-renderable actor rather than a truncated frame.
                    if (!finite(entity.bounds_center) || !finite(entity.bounds_extent) ||
                        std::ranges::any_of(entity.bounds_extent, [](double value) {
                            return value <= 0.0 || value > 1000000000.0;
                        })) {
                        continue;
                    }

                    std::int32_t entity_index{-1};
                    std::int32_t class_index{-1};
                    std::uintptr_t address{};
                    if (AddAddress(actor, Layout(profile, "object.internalIndex"), address)) {
                        static_cast<void>(ReadValue(*memory, address, entity_index));
                    }
                    if (entity_index >= 0) {
                        std::uintptr_t registered_object{};
                        std::uint32_t registered_serial{};
                        entity.object_index = static_cast<std::uint32_t>(entity_index);
                        if (ReadObjectSlot(
                                *memory, object_registry, entity.object_index,
                                registered_object, registered_serial) &&
                            registered_object == actor && registered_serial != 0) {
                            entity.object_serial = registered_serial;
                            entity.object_identity_available = true;
                        }
                    }
                    if (AddAddress(actor, Layout(profile, "object.nameOffset"), address)) {
                        static_cast<void>(ReadValue(*memory, address, entity.entity_name_id));
                    }

                    // Class/name/index metadata is optional. Stable slot/name fallbacks
                    // keep otherwise valid geometry usable when metadata is absent.
                    std::uintptr_t class_object{};
                    if (AddAddress(actor, Layout(profile, "object.class"), address)) {
                        static_cast<void>(ReadValue(*memory, address, class_object));
                    }
                    if (class_object != 0) {
                        entity.class_object = class_object;
                        if (AddAddress(
                                class_object, Layout(profile, "object.internalIndex"), address)) {
                            static_cast<void>(ReadValue(*memory, address, class_index));
                        }
                        if (AddAddress(
                                class_object, Layout(profile, "object.nameOffset"), address)) {
                            static_cast<void>(ReadValue(*memory, address, entity.class_name_id));
                        }
                    }
                    entity.entity_id = entity_index >= 0
                        ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(entity_index)) + 1
                        : fallback_index + 1;
                    entity.class_id = class_index >= 0
                        ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(class_index)) + 1
                        : static_cast<std::uint64_t>(entity.class_name_id) + 1;

                    if (entity.class_name_id != 0 && !class_names.contains(entity.class_id)) {
                        if (std::string name = ResolveNameSnapshotLocked(entity.class_name_id);
                            !name.empty()) {
                            class_names.emplace(entity.class_id, std::move(name));
                        }
                    }
                    if (entity.entity_name_id != 0 && !entity_names.contains(entity.entity_id)) {
                        if (std::string name = ResolveNameSnapshotLocked(entity.entity_name_id);
                            !name.empty()) {
                            entity_names.emplace(entity.entity_id, std::move(name));
                        }
                    }

                    std::uint8_t mobility{};
                    if (AddAddress(root, Layout(profile, "sceneComponent.mobility"), address) &&
                        ReadValue(*memory, address, mobility)) {
                        if (mobility == 0) entity.flags |= ANOMALY_NTE_ENTITY_V1_STATIC;
                        else if (mobility == 1) entity.flags |= ANOMALY_NTE_ENTITY_V1_STATIONARY;
                        else if (mobility == 2) entity.flags |= ANOMALY_NTE_ENTITY_V1_MOVABLE;
                    } else {
                        partial = true;
                    }
                    if (actor == player_pawn) entity.flags |= ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER;
                    next.push_back(entity);
                }
            }
            auto cache = std::make_shared<EntityFrameCache>();
            cache->entities = std::move(next);
            cache->class_names = std::move(class_names);
            cache->entity_names = std::move(entity_names);
            cache->generation = ++generation;
            cache->sequence = sequence;
            cache->camera_position = camera_position;
            cache->camera_rotation = camera_rotation;
            cache->camera_horizontal_fov = camera_horizontal_fov;
            cache->partial = partial;
            destination = std::move(cache);
        } catch (...) {
            invalidate();
        }
    }

    void RefreshEntities(std::uint64_t sequence) noexcept {
        entity_attempt_sequence = sequence;
        const auto current = entity_frame_cache;
        const auto previous = previous_entity_frame_cache;
        RefreshEntityCache(
            sequence, false, entity_frame_cache, entity_generation);
        if (entity_frame_cache != current) {
            previous_entity_frame_cache = current ? current : previous;
        }
    }

    void RefreshActors(std::uint64_t sequence) noexcept {
        RefreshEntityCache(sequence, true, actor_frame_cache, actor_generation);
        if (actor_frame_cache) actor_world_generation = world_generation;
    }

    static AnomalyStatusV1 ANOMALY_CALL BuildId(
        void* user, char* destination, std::size_t* size) noexcept {
        return CopyString(static_cast<State*>(user)->fingerprint.id, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL ProfileHash(
        void* user, char* destination, std::size_t* size) noexcept {
        return CopyString(static_cast<State*>(user)->profile.source_hash, destination, size);
    }

    static std::uint32_t ANOMALY_CALL FeatureStateThunk(
        void* user, AnomalyStringViewV1 id) noexcept {
        return static_cast<State*>(user)->FeatureState(id);
    }

    static std::uint32_t ANOMALY_CALL GameThreadIdThunk(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return state.ServiceAvailableForPublication(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID)
            ? static_cast<std::uint32_t>(state.game_thread_id.load(std::memory_order_acquire))
            : 0;
    }

    static std::uint64_t ANOMALY_CALL TickSequenceThunk(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return state.ServiceAvailableForPublication(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID)
            ? state.tick_sequence.load(std::memory_order_acquire)
            : 0;
    }

    static int ANOMALY_CALL IsGameThreadThunk(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.ServiceAvailableForPublication(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID)) {
            return 0;
        }
        const DWORD expected = state.game_thread_id.load(std::memory_order_acquire);
        return expected != 0 && expected == GetCurrentThreadId() ? 1 : 0;
    }

    AnomalyStatusV1 ResolveNameIdLocked(
        std::uint32_t name_id, char* destination, std::size_t* size) const noexcept {
        const auto* names = Symbol("ue5.FNamePool");
        if (names == nullptr || !names->Available()) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
        const auto blocks_offset = Layout(profile, "names.blocksOffset");
        const auto block_bits = Layout(profile, "names.blockBits", 16);
        const auto entry_stride = Layout(profile, "names.entryStride", 2);
        const auto length_shift = Layout(profile, "names.headerLengthShift", 6);
        if (blocks_offset < 0 || block_bits <= 0 || block_bits >= 31 || entry_stride <= 0 ||
            length_shift <= 0 || length_shift >= 16) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "name layout is unavailable");
        }
        const std::uint32_t block_index = name_id >> block_bits;
        const std::uint32_t entry_offset = name_id & ((1U << block_bits) - 1U);
        std::uintptr_t block_slot{};
        std::uintptr_t block{};
        std::int64_t indexed_blocks_offset{};
        const auto block_stride = static_cast<std::uint64_t>(block_index) *
            sizeof(std::uintptr_t);
        if (block_stride > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) ||
            !AddLayoutOffset(
                blocks_offset, static_cast<std::int64_t>(block_stride), indexed_blocks_offset)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "name block layout is invalid");
        }
        if (!AddAddress(
                names->address,
                indexed_blocks_offset,
                block_slot) || !ReadValue(*memory, block_slot, block) || block == 0) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "name block is unavailable");
        }
        std::uintptr_t entry{};
        if (entry_offset != 0 && entry_stride >
                (std::numeric_limits<std::int64_t>::max)() /
                    static_cast<std::int64_t>(entry_offset)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "name entry layout is invalid");
        }
        const auto entry_distance = static_cast<std::int64_t>(entry_offset) * entry_stride;
        if (!AddAddress(block, entry_distance, entry)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        }
        std::uint16_t header{};
        if (!ReadValue(*memory, entry, header)) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        const std::size_t length = header >> length_shift;
        const bool wide = (header & 1U) != 0;
        if (length == 0 || length > 1024 || wide) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, wide ? "wide name entry" : "invalid name length");
        }
        std::string value(length, '\0');
        if (!memory->Read(entry + sizeof(header), value.data(), value.size())) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        }
        return CopyString(value, destination, size);
    }

    std::string ResolveNameSnapshotLocked(std::uint32_t name_id) const {
        if (name_id == 0) return {};
        std::size_t size{};
        if (ResolveNameIdLocked(name_id, nullptr, &size).code != ANOMALY_STATUS_V1_OK ||
            size <= 1 || size > 1024) {
            return {};
        }
        std::string value(size, '\0');
        if (ResolveNameIdLocked(name_id, value.data(), &size).code != ANOMALY_STATUS_V1_OK) {
            return {};
        }
        value.resize(size - 1);
        return value;
    }

    [[nodiscard]] bool ReadReflectedObjectNameLocked(
        const std::uintptr_t object,
        std::string& name) const {
        std::uintptr_t name_address{};
        std::uint32_t name_id{};
        if (!AddAddress(object, Layout(profile, "object.nameOffset"), name_address) ||
            !ReadValue(*memory, name_address, name_id)) {
            return false;
        }
        name = ResolveNameSnapshotLocked(name_id);
        return !name.empty();
    }

    [[nodiscard]] bool ReadReflectedFieldClassNameLocked(
        const std::uintptr_t field,
        std::string& name) const {
        std::uintptr_t field_class{};
        std::uintptr_t name_address{};
        std::uint32_t name_id{};
        return ReadPointerAt(
                   *memory, field, Layout(profile, "ffield.class"), field_class) &&
            AddAddress(
                field_class, Layout(profile, "ffieldClass.name"), name_address) &&
            ReadValue(*memory, name_address, name_id) &&
            !(name = ResolveNameSnapshotLocked(name_id)).empty();
    }

    [[nodiscard]] bool BuildAhudFunctionBindingLocked(
        const std::uintptr_t function,
        const AhudFunctionKind kind,
        AhudFunctionBinding& binding) const {
        try {
            const AhudFunctionSpec spec = AhudSpec(kind);
            if (spec.name.empty() || spec.parameters.empty() ||
                spec.parameters.size() > kMaximumAhudParameters) {
                return false;
            }

            std::string function_name;
            if (!ReadReflectedObjectNameLocked(function, function_name) ||
                function_name != spec.name) {
                return false;
            }
            std::uintptr_t class_object{};
            std::uintptr_t outer_object{};
            if (!ReadPointerAt(
                    *memory, function, Layout(profile, "object.class"), class_object) ||
                !ReadPointerAt(
                    *memory, function, Layout(profile, "object.outer"), outer_object)) {
                return false;
            }
            std::string class_name;
            std::string outer_name;
            if (!ReadReflectedObjectNameLocked(class_object, class_name) ||
                !ReadReflectedObjectNameLocked(outer_object, outer_name) ||
                class_name != "Function" || outer_name != "HUD") {
                return false;
            }

            std::uintptr_t property{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_value_offset{};
            if (!ReadPointerAt(
                    *memory, function, Layout(profile, "ustruct.propertyLink"), property) ||
                !ReadValue(
                    *memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(
                    *memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(
                    *memory,
                    function + Layout(profile, "ufunction.returnValueOffset"),
                    return_value_offset) ||
                num_parms != spec.parameters.size() || parms_size != spec.parms_size) {
                return false;
            }
            const bool has_return_value = std::ranges::any_of(
                spec.parameters,
                [](const AhudParameterSpec& parameter) { return parameter.return_value; });
            if (!has_return_value &&
                return_value_offset != (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }

            AhudFunctionBinding candidate;
            candidate.function = function;
            candidate.parms_size = parms_size;
            std::array<bool, kMaximumAhudParameters> found{};
            std::size_t property_count{};
            while (property != 0 && property_count < kMaximumAhudParameters) {
                std::uintptr_t next{};
                std::uintptr_t property_name_address{};
                std::uintptr_t next_address{};
                std::uint32_t property_name_id{};
                std::int32_t array_dim{};
                std::int32_t element_size{};
                std::int32_t offset{};
                if (!AddAddress(
                        property, Layout(profile, "ffield.name"), property_name_address) ||
                    !AddAddress(
                        property,
                        Layout(profile, "fproperty.propertyLinkNext"),
                        next_address) ||
                    !ReadValue(*memory, property_name_address, property_name_id) ||
                    !ReadValue(
                        *memory,
                        property + Layout(profile, "fproperty.arrayDim"),
                        array_dim) ||
                    !ReadValue(
                        *memory,
                        property + Layout(profile, "fproperty.elementSize"),
                        element_size) ||
                    !ReadValue(
                        *memory,
                        property + Layout(profile, "fproperty.offsetInternal"),
                        offset) ||
                    !ReadValue(*memory, next_address, next)) {
                    return false;
                }

                const std::string property_name =
                    ResolveNameSnapshotLocked(property_name_id);
                std::string property_type;
                if (property_name.empty() ||
                    !ReadReflectedFieldClassNameLocked(property, property_type) ||
                    array_dim != 1 || element_size <= 0 || offset < 0 ||
                    static_cast<std::uint64_t>(offset) +
                            static_cast<std::uint64_t>(element_size) >
                        parms_size) {
                    return false;
                }

                std::size_t parameter_index = spec.parameters.size();
                for (std::size_t index{}; index < spec.parameters.size(); ++index) {
                    if (spec.parameters[index].name == property_name) {
                        parameter_index = index;
                        break;
                    }
                }
                if (parameter_index == spec.parameters.size() || found[parameter_index]) {
                    return false;
                }
                const AhudParameterSpec& parameter = spec.parameters[parameter_index];
                if (parameter.type != property_type ||
                    parameter.element_size != element_size ||
                    static_cast<std::uint64_t>(offset) >
                        (std::numeric_limits<std::uint16_t>::max)()) {
                    return false;
                }

                if (!parameter.structure.empty()) {
                    std::uintptr_t structure{};
                    std::string structure_name;
                    if (!ReadPointerAt(
                            *memory,
                            property,
                            Layout(profile, "fstructProperty.struct"),
                            structure) ||
                        !ReadReflectedObjectNameLocked(structure, structure_name) ||
                        structure_name != parameter.structure) {
                        return false;
                    }
                }

                candidate.offsets[parameter_index] =
                    static_cast<std::uint16_t>(offset);
                if (parameter.type == "BoolProperty") {
                    std::uint8_t field_size{};
                    std::uint8_t byte_offset{};
                    std::uint8_t byte_mask{};
                    std::uint8_t field_mask{};
                    if (!ReadValue(
                            *memory,
                            property + Layout(profile, "fboolProperty.fieldSize"),
                            field_size) ||
                        !ReadValue(
                            *memory,
                            property + Layout(profile, "fboolProperty.byteOffset"),
                            byte_offset) ||
                        !ReadValue(
                            *memory,
                            property + Layout(profile, "fboolProperty.byteMask"),
                            byte_mask) ||
                        !ReadValue(
                            *memory,
                            property + Layout(profile, "fboolProperty.fieldMask"),
                            field_mask) ||
                        field_size == 0 || byte_offset >= field_size ||
                        byte_mask == 0 || field_mask == 0 ||
                        (byte_mask & field_mask) != byte_mask ||
                        static_cast<std::uint64_t>(offset) + byte_offset >= parms_size ||
                        static_cast<std::uint64_t>(offset) + byte_offset >
                            (std::numeric_limits<std::uint16_t>::max)()) {
                        return false;
                    }
                    candidate.bool_parameters[parameter_index] = {
                        static_cast<std::uint16_t>(
                            static_cast<std::uint32_t>(offset) + byte_offset),
                        field_mask,
                        byte_mask};
                }
                if (parameter.return_value &&
                    return_value_offset != static_cast<std::uint16_t>(offset)) {
                    return false;
                }

                found[parameter_index] = true;
                ++property_count;
                property = next;
            }
            if (property != 0 || property_count != spec.parameters.size() ||
                !std::ranges::all_of(
                    std::span(found).first(spec.parameters.size()),
                    [](const bool value) { return value; })) {
                return false;
            }
            binding = candidate;
            return true;
        } catch (...) {
            return false;
        }
    }

    void RefreshAhudBindingLocked() noexcept {
        try {
            if (!AhudFeatureAvailable() || object_registry.items == 0 ||
                object_registry.count == 0) {
                ahud_binding.store({}, std::memory_order_release);
                return;
            }
            if (!ahud_demand.load(std::memory_order_acquire)) return;
            const auto current = ahud_binding.load(std::memory_order_acquire);
            if (current && current->object_generation == object_generation) return;
            if (ahud_discovery.object_generation != object_generation) {
                InvalidateAhudBindingLocked();
            }
            if (ahud_discovery.discovery_complete) return;

            constexpr std::uint32_t kDiscoveryBatch = 4096;
            const std::uint32_t end = (std::min)(
                object_registry.count,
                ahud_discovery.next_object_index + kDiscoveryBatch);
            for (std::uint32_t index = ahud_discovery.next_object_index;
                 index < end;
                 ++index) {
                std::uintptr_t object{};
                std::uint32_t serial{};
                if (!ReadObjectSlot(
                        *memory, object_registry, index, object, serial) ||
                    object == 0) {
                    continue;
                }
                std::string name;
                if (!ReadReflectedObjectNameLocked(object, name)) continue;
                for (std::size_t function_index{};
                     function_index < kAhudFunctionCount;
                     ++function_index) {
                    if (ahud_discovery.functions[function_index]) continue;
                    const auto kind = static_cast<AhudFunctionKind>(function_index);
                    if (AhudSpec(kind).name != name) continue;
                    AhudFunctionBinding candidate;
                    if (BuildAhudFunctionBindingLocked(object, kind, candidate)) {
                        ahud_discovery.functions[function_index] = candidate;
                    }
                    break;
                }
            }
            ahud_discovery.next_object_index = end;
            const bool complete = std::ranges::all_of(
                ahud_discovery.functions,
                [](const auto& function) { return function.has_value(); });
            if (complete) {
                auto binding = std::make_shared<AhudBinding>();
                binding->object_generation = object_generation;
                for (std::size_t index{}; index < kAhudFunctionCount; ++index) {
                    binding->functions[index] = *ahud_discovery.functions[index];
                }
                ahud_binding.store(std::move(binding), std::memory_order_release);
                ahud_discovery.discovery_complete = true;
            } else if (end == object_registry.count) {
                ahud_discovery.discovery_complete = true;
            }
        } catch (...) {
            ahud_discovery.discovery_complete = true;
        }
    }

    [[nodiscard]] static const AhudFunctionBinding* AhudFunction(
        const AhudFrameCallContext* context,
        const AhudFunctionKind kind) noexcept {
        if (context == nullptr || context->hud == 0 || context->binding == nullptr ||
            context->invoker == nullptr || !*context->invoker) {
            return nullptr;
        }
        const auto index = AhudIndex(kind);
        if (index >= context->binding->functions.size()) return nullptr;
        const auto& function = context->binding->functions[index];
        return function.function != 0 && function.parms_size != 0
            ? &function
            : nullptr;
    }

    static bool WriteAhudBytes(
        const AhudFunctionBinding& function,
        const std::size_t parameter_index,
        const void* const source,
        const std::size_t size,
        std::span<std::uint8_t> destination) noexcept {
        if (source == nullptr || parameter_index >= function.offsets.size()) return false;
        const std::size_t offset = function.offsets[parameter_index];
        if (offset > destination.size() || size > destination.size() - offset ||
            destination.size() < function.parms_size) {
            return false;
        }
        std::memcpy(destination.data() + offset, source, size);
        return true;
    }

    static bool ReadAhudBytes(
        const AhudFunctionBinding& function,
        const std::size_t parameter_index,
        void* const destination,
        const std::size_t size,
        std::span<const std::uint8_t> source) noexcept {
        if (destination == nullptr || parameter_index >= function.offsets.size()) return false;
        const std::size_t offset = function.offsets[parameter_index];
        if (offset > source.size() || size > source.size() - offset ||
            source.size() < function.parms_size) {
            return false;
        }
        std::memcpy(destination, source.data() + offset, size);
        return true;
    }

    template <typename Value>
    static bool WriteAhudValue(
        const AhudFunctionBinding& function,
        const std::size_t parameter_index,
        const Value& value,
        std::span<std::uint8_t> destination) noexcept {
        return WriteAhudBytes(
            function, parameter_index, &value, sizeof(value), destination);
    }

    template <typename Value>
    static bool ReadAhudValue(
        const AhudFunctionBinding& function,
        const std::size_t parameter_index,
        Value& value,
        std::span<const std::uint8_t> source) noexcept {
        return ReadAhudBytes(
            function, parameter_index, &value, sizeof(value), source);
    }

    static bool InvokeAhud(
        const AhudFrameCallContext& context,
        const AhudFunctionBinding& function,
        std::span<std::uint8_t> parameters) noexcept {
        if (parameters.size() < function.parms_size || context.invoker == nullptr) {
            return false;
        }
        try {
            const bool invoked = (*context.invoker)(
                context.hud,
                function.function,
                parameters.data(),
                function.parms_size);
            if (invoked && context.process_event_call_count != nullptr) {
                context.process_event_call_count->fetch_add(
                    1, std::memory_order_relaxed);
            }
            return invoked;
        } catch (...) {
            return false;
        }
    }

    static bool EncodeAhudText(
        const AnomalyStringViewV1 text,
        std::vector<wchar_t>& storage,
        NativeUtf16StringHeader& header) {
        constexpr std::size_t kMaximumTextBytes = 16U * 1024U;
        header = {};
        storage.clear();
        if ((text.data == nullptr && text.size != 0) ||
            text.size > kMaximumTextBytes ||
            text.size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return false;
        }
        if (text.size == 0) return true;
        const int source_size = static_cast<int>(text.size);
        const int count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.data, source_size, nullptr, 0);
        if (count <= 0 || count >= (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        storage.resize(static_cast<std::size_t>(count) + 1U);
        if (MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text.data,
                source_size,
                storage.data(),
                count) != count) {
            storage.clear();
            return false;
        }
        storage[static_cast<std::size_t>(count)] = L'\0';
        header.data = storage.data();
        header.count = count + 1;
        header.capacity = count + 1;
        return true;
    }

    [[nodiscard]] static std::array<float, 4> LinearColor(
        const std::uint32_t color) noexcept {
        constexpr float scale = 1.0F / 255.0F;
        return {
            static_cast<float>(color & 0xFFU) * scale,
            static_cast<float>((color >> 8U) & 0xFFU) * scale,
            static_cast<float>((color >> 16U) & 0xFFU) * scale,
            static_cast<float>((color >> 24U) & 0xFFU) * scale};
    }

    static int ANOMALY_CALL AhudProject(
        void* user,
        const double world[3],
        float screen[2],
        double* depth) noexcept {
        const auto* context = static_cast<const AhudFrameCallContext*>(user);
        const auto* function = AhudFunction(context, AhudFunctionKind::Project);
        if (function == nullptr || world == nullptr || screen == nullptr ||
            !std::ranges::all_of(
                std::span(world, 3), [](const double value) { return std::isfinite(value); })) {
            return 0;
        }
        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        if (!WriteAhudBytes(
                *function, 0, world, sizeof(double) * 3U, parameters) ||
            !InvokeAhud(*context, *function, parameters)) {
            return 0;
        }
        std::array<double, 3> projected{};
        if (!ReadAhudValue(*function, 2, projected, parameters) ||
            !std::ranges::all_of(
                projected, [](const double value) { return std::isfinite(value); }) ||
            std::abs(projected[0]) > (std::numeric_limits<float>::max)() ||
            std::abs(projected[1]) > (std::numeric_limits<float>::max)()) {
            return 0;
        }
        screen[0] = static_cast<float>(projected[0]);
        screen[1] = static_cast<float>(projected[1]);
        if (depth != nullptr) *depth = projected[2];
        return projected[2] > 0.0 ? 1 : 0;
    }

    static int ANOMALY_CALL AhudMeasureText(
        void* user,
        const AnomalyStringViewV1 text,
        const float scale,
        float* width,
        float* height) noexcept {
        const auto* context = static_cast<const AhudFrameCallContext*>(user);
        const auto* function = AhudFunction(context, AhudFunctionKind::GetTextSize);
        if (function == nullptr || width == nullptr || height == nullptr ||
            !std::isfinite(scale) || scale <= 0.0F) {
            return 0;
        }
        try {
            std::vector<wchar_t> storage;
            NativeUtf16StringHeader native_text;
            alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
            if (!EncodeAhudText(text, storage, native_text) ||
                !WriteAhudValue(*function, 0, native_text, parameters) ||
                !WriteAhudValue(*function, 4, scale, parameters) ||
                !InvokeAhud(*context, *function, parameters)) {
                return 0;
            }
            float measured_width{};
            float measured_height{};
            if (!ReadAhudValue(*function, 1, measured_width, parameters) ||
                !ReadAhudValue(*function, 2, measured_height, parameters) ||
                !std::isfinite(measured_width) || !std::isfinite(measured_height) ||
                measured_width < 0.0F || measured_height < 0.0F) {
                return 0;
            }
            *width = measured_width;
            *height = measured_height;
            return 1;
        } catch (...) {
            return 0;
        }
    }

    static int ANOMALY_CALL AhudDrawText(
        void* user,
        const AnomalyStringViewV1 text,
        const float x,
        const float y,
        const std::uint32_t color_rgba,
        const float scale) noexcept {
        const auto* context = static_cast<const AhudFrameCallContext*>(user);
        const auto* function = AhudFunction(context, AhudFunctionKind::DrawText);
        if (function == nullptr || !std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(scale) || scale <= 0.0F) {
            return 0;
        }
        try {
            std::vector<wchar_t> storage;
            NativeUtf16StringHeader native_text;
            const auto color = LinearColor(color_rgba);
            alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
            if (!EncodeAhudText(text, storage, native_text) ||
                !WriteAhudValue(*function, 0, native_text, parameters) ||
                !WriteAhudValue(*function, 1, color, parameters) ||
                !WriteAhudValue(*function, 2, x, parameters) ||
                !WriteAhudValue(*function, 3, y, parameters) ||
                !WriteAhudValue(*function, 5, scale, parameters)) {
                return 0;
            }
            return InvokeAhud(*context, *function, parameters) ? 1 : 0;
        } catch (...) {
            return 0;
        }
    }

    static int ANOMALY_CALL AhudDrawLine(
        void* user,
        const float start_x,
        const float start_y,
        const float end_x,
        const float end_y,
        const std::uint32_t color_rgba,
        const float thickness) noexcept {
        const auto* context = static_cast<const AhudFrameCallContext*>(user);
        const auto* function = AhudFunction(context, AhudFunctionKind::DrawLine);
        const std::array coordinates{start_x, start_y, end_x, end_y, thickness};
        if (function == nullptr || thickness <= 0.0F ||
            !std::ranges::all_of(
                coordinates, [](const float value) { return std::isfinite(value); })) {
            return 0;
        }
        const auto color = LinearColor(color_rgba);
        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        if (!WriteAhudValue(*function, 0, start_x, parameters) ||
            !WriteAhudValue(*function, 1, start_y, parameters) ||
            !WriteAhudValue(*function, 2, end_x, parameters) ||
            !WriteAhudValue(*function, 3, end_y, parameters) ||
            !WriteAhudValue(*function, 4, color, parameters) ||
            !WriteAhudValue(*function, 5, thickness, parameters)) {
            return 0;
        }
        return InvokeAhud(*context, *function, parameters) ? 1 : 0;
    }

    static int ANOMALY_CALL AhudDrawRect(
        void* user,
        const float x,
        const float y,
        const float width,
        const float height,
        const std::uint32_t color_rgba) noexcept {
        const auto* context = static_cast<const AhudFrameCallContext*>(user);
        const auto* function = AhudFunction(context, AhudFunctionKind::DrawRect);
        const std::array values{x, y, width, height};
        if (function == nullptr || width < 0.0F || height < 0.0F ||
            !std::ranges::all_of(
                values, [](const float value) { return std::isfinite(value); })) {
            return 0;
        }
        const auto color = LinearColor(color_rgba);
        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        if (!WriteAhudValue(*function, 0, color, parameters) ||
            !WriteAhudValue(*function, 1, x, parameters) ||
            !WriteAhudValue(*function, 2, y, parameters) ||
            !WriteAhudValue(*function, 3, width, parameters) ||
            !WriteAhudValue(*function, 4, height, parameters)) {
            return 0;
        }
        return InvokeAhud(*context, *function, parameters) ? 1 : 0;
    }

    void DispatchAhudFrame(
        std::uintptr_t object,
        std::uintptr_t function,
        void* parameters,
        const ProcessEventInvoker& actor_process_event) noexcept;

    [[nodiscard]] bool BuildTeleportBindingLocked(
        const std::uintptr_t function,
        TeleportBinding& binding) const {
        try {
            std::string function_name;
            if (!ReadReflectedObjectNameLocked(function, function_name) ||
                function_name != "K2_SetActorLocation") {
                return false;
            }

            std::uintptr_t class_object{};
            std::uintptr_t outer_object{};
            if (!ReadPointerAt(*memory, function, Layout(profile, "object.class"), class_object) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), outer_object)) {
                return false;
            }
            std::string class_name;
            std::string outer_name;
            if (!ReadReflectedObjectNameLocked(class_object, class_name) ||
                !ReadReflectedObjectNameLocked(outer_object, outer_name) ||
                class_name != "Function" || outer_name != "Actor") {
                return false;
            }

            std::uintptr_t property{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_value_offset{};
            if (!ReadPointerAt(
                    *memory, function, Layout(profile, "ustruct.propertyLink"), property) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(
                    *memory,
                    function + Layout(profile, "ufunction.returnValueOffset"),
                    return_value_offset) ||
                num_parms != 5 || parms_size == 0 || parms_size > 4096) {
                return false;
            }

            TeleportBinding candidate;
            candidate.function = function;
            candidate.parms_size = parms_size;
            candidate.object_generation = object_generation;
            bool found_new_location{};
            bool found_sweep_hit_result{};
            bool found_b_sweep{};
            bool found_b_teleport{};
            bool found_return_value{};
            std::uint32_t property_count{};

            const auto read_bool = [&](const std::int32_t offset, const std::int32_t element_size,
                                       const std::int32_t array_dim,
                                       ReflectedBoolParameter& result) {
                std::uint8_t field_size{};
                std::uint8_t byte_offset{};
                std::uint8_t byte_mask{};
                std::uint8_t field_mask{};
                return array_dim == 1 && element_size == 1 && offset >= 0 &&
                    ReadValue(
                        *memory,
                        property + Layout(profile, "fboolProperty.fieldSize"), field_size) &&
                    ReadValue(
                        *memory,
                        property + Layout(profile, "fboolProperty.byteOffset"), byte_offset) &&
                    ReadValue(
                        *memory,
                        property + Layout(profile, "fboolProperty.byteMask"), byte_mask) &&
                    ReadValue(
                        *memory,
                        property + Layout(profile, "fboolProperty.fieldMask"), field_mask) &&
                    field_size != 0 && byte_offset < field_size && byte_mask != 0 &&
                    field_mask != 0 && (byte_mask & field_mask) == byte_mask &&
                    static_cast<std::uint64_t>(offset) + byte_offset < parms_size &&
                    ((result = {static_cast<std::uint16_t>(
                                      static_cast<std::uint32_t>(offset) + byte_offset),
                                  field_mask,
                                  byte_mask}),
                     true);
            };

            while (property != 0 && property_count < 16) {
                std::uintptr_t next{};
                std::uintptr_t property_name_address{};
                std::uint32_t property_name_id{};
                std::int32_t array_dim{};
                std::int32_t element_size{};
                std::int32_t offset{};
                std::uintptr_t next_address{};
                if (!AddAddress(
                        property, Layout(profile, "ffield.name"), property_name_address) ||
                    !AddAddress(
                        property, Layout(profile, "fproperty.propertyLinkNext"), next_address) ||
                    !ReadValue(*memory, property_name_address, property_name_id) ||
                    !ReadValue(
                        *memory, property + Layout(profile, "fproperty.arrayDim"), array_dim) ||
                    !ReadValue(
                        *memory, property + Layout(profile, "fproperty.elementSize"), element_size) ||
                    !ReadValue(
                        *memory, property + Layout(profile, "fproperty.offsetInternal"), offset) ||
                    !ReadValue(*memory, next_address, next)) {
                    return false;
                }
                const std::string property_name = ResolveNameSnapshotLocked(property_name_id);
                if (property_name.empty() || array_dim != 1 || element_size <= 0 || offset < 0 ||
                    static_cast<std::uint64_t>(offset) +
                            static_cast<std::uint64_t>(element_size) >
                        parms_size) {
                    return false;
                }

                if (property_name == "NewLocation" && !found_new_location) {
                    std::uintptr_t structure{};
                    std::string structure_name;
                    if (element_size != static_cast<std::int32_t>(sizeof(double) * 3U) ||
                        !ReadPointerAt(
                            *memory,
                            property,
                            Layout(profile, "fstructProperty.struct"),
                            structure) ||
                        !ReadReflectedObjectNameLocked(structure, structure_name) ||
                        structure_name != "Vector") {
                        return false;
                    }
                    candidate.new_location_offset = static_cast<std::uint16_t>(offset);
                    found_new_location = true;
                } else if (property_name == "SweepHitResult" && !found_sweep_hit_result) {
                    std::uintptr_t structure{};
                    std::string structure_name;
                    if (!ReadPointerAt(
                            *memory,
                            property,
                            Layout(profile, "fstructProperty.struct"),
                            structure) ||
                        !ReadReflectedObjectNameLocked(structure, structure_name) ||
                        structure_name != "HitResult") {
                        return false;
                    }
                    candidate.sweep_hit_result_offset = static_cast<std::uint16_t>(offset);
                    candidate.sweep_hit_result_size = static_cast<std::uint16_t>(element_size);
                    found_sweep_hit_result = true;
                } else if (property_name == "bSweep" && !found_b_sweep) {
                    if (!read_bool(offset, element_size, array_dim, candidate.b_sweep)) return false;
                    found_b_sweep = true;
                } else if (property_name == "bTeleport" && !found_b_teleport) {
                    if (!read_bool(offset, element_size, array_dim, candidate.b_teleport)) return false;
                    found_b_teleport = true;
                } else if (property_name == "ReturnValue" && !found_return_value) {
                    if (static_cast<std::uint16_t>(offset) != return_value_offset ||
                        !read_bool(offset, element_size, array_dim, candidate.return_value)) {
                        return false;
                    }
                    found_return_value = true;
                } else {
                    return false;
                }

                property = next;
                ++property_count;
            }

            if (property != 0 || property_count != 5 || !found_new_location ||
                !found_sweep_hit_result || !found_b_sweep || !found_b_teleport ||
                !found_return_value) {
                return false;
            }
            candidate.available = true;
            candidate.discovery_complete = true;
            binding = candidate;
            return true;
        } catch (...) {
            return false;
        }
    }

    void RefreshTeleportBindingLocked() noexcept {
        try {
            if (!NtePlayerTeleportAvailable() || object_registry.items == 0 ||
                object_registry.count == 0) {
                teleport = {};
                return;
            }
            if (teleport.object_generation != object_generation) {
                teleport = {};
                teleport.object_generation = object_generation;
            }
            if (teleport.available || teleport.discovery_complete) return;

            constexpr std::uint32_t kDiscoveryBatch = 2048;
            const std::uint32_t end = (std::min)(
                object_registry.count,
                teleport.next_object_index + kDiscoveryBatch);
            for (std::uint32_t index = teleport.next_object_index;
                 index < end;
                 ++index) {
                std::uintptr_t object{};
                std::uint32_t serial{};
                if (!ReadObjectSlot(*memory, object_registry, index, object, serial) || object == 0) {
                    continue;
                }
                std::string name;
                if (!ReadReflectedObjectNameLocked(object, name) ||
                    name != "K2_SetActorLocation") {
                    continue;
                }
                TeleportBinding candidate;
                if (BuildTeleportBindingLocked(object, candidate)) {
                    candidate.next_object_index = index + 1U;
                    teleport = candidate;
                    return;
                }
                // A name collision is not evidence that a later UFunction with the
                // same short name cannot have the required Actor ABI.
                teleport.next_object_index = index + 1U;
            }
            teleport.next_object_index = end;
            if (end == object_registry.count) teleport.discovery_complete = true;
        } catch (...) {
            teleport.discovery_complete = true;
        }
    }

    [[nodiscard]] bool BuildNavigationMoveBindingLocked(
        const std::uintptr_t function,
        NavigationBinding& result) const noexcept {
        try {
            std::string function_name;
            std::uintptr_t class_object{};
            std::uintptr_t outer_object{};
            if (!ReadReflectedObjectNameLocked(function, function_name) ||
                function_name != "MoveToPointByTransform" ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.class"), class_object) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), outer_object)) {
                return false;
            }
            std::string class_name;
            std::string outer_name;
            if (!ReadReflectedObjectNameLocked(class_object, class_name) ||
                !ReadReflectedObjectNameLocked(outer_object, outer_name) ||
                class_name != "Function" || outer_name != "HTUtil") {
                return false;
            }

            std::uintptr_t property{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_value_offset{};
            if (!ReadPointerAt(
                    *memory, function, Layout(profile, "ustruct.propertyLink"), property) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(
                    *memory, function + Layout(profile, "ufunction.returnValueOffset"),
                    return_value_offset) ||
                num_parms != 8 || parms_size != 0x41 ||
                return_value_offset != (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }

            NavigationBinding candidate = result;
            candidate.move_to_point_by_transform = function;
            candidate.util_class = outer_object;
            candidate.move_parms_size = parms_size;
            std::array<bool, 8> found{};
            std::size_t property_count{};
            const auto read_bool = [&](const std::int32_t offset,
                                       const std::int32_t element_size,
                                       const std::int32_t array_dim,
                                       ReflectedBoolParameter& output) {
                std::uint8_t field_size{};
                std::uint8_t byte_offset{};
                std::uint8_t byte_mask{};
                std::uint8_t field_mask{};
                return array_dim == 1 && element_size == 1 && offset >= 0 &&
                    ReadValue(*memory, property + Layout(profile, "fboolProperty.fieldSize"), field_size) &&
                    ReadValue(*memory, property + Layout(profile, "fboolProperty.byteOffset"), byte_offset) &&
                    ReadValue(*memory, property + Layout(profile, "fboolProperty.byteMask"), byte_mask) &&
                    ReadValue(*memory, property + Layout(profile, "fboolProperty.fieldMask"), field_mask) &&
                    field_size != 0 && byte_offset < field_size && byte_mask != 0 &&
                    field_mask != 0 && (byte_mask & field_mask) == byte_mask &&
                    static_cast<std::uint64_t>(offset) + byte_offset < parms_size &&
                    ((output = {static_cast<std::uint16_t>(
                                   static_cast<std::uint32_t>(offset) + byte_offset),
                               field_mask, byte_mask}), true);
            };

            while (property != 0 && property_count < found.size()) {
                std::uintptr_t next{};
                std::uint32_t name_id{};
                std::int32_t array_dim{};
                std::int32_t element_size{};
                std::int32_t offset{};
                std::uintptr_t name_address{};
                std::uintptr_t next_address{};
                if (!AddAddress(property, Layout(profile, "ffield.name"), name_address) ||
                    !AddAddress(property, Layout(profile, "fproperty.propertyLinkNext"), next_address) ||
                    !ReadValue(*memory, name_address, name_id) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.arrayDim"), array_dim) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.elementSize"), element_size) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.offsetInternal"), offset) ||
                    !ReadValue(*memory, next_address, next) || array_dim != 1 ||
                    element_size <= 0 || offset < 0 ||
                    static_cast<std::uint64_t>(offset) + element_size > parms_size) {
                    return false;
                }
                const std::string name = ResolveNameSnapshotLocked(name_id);
                std::size_t index = 8;
                static constexpr std::array<std::string_view, 8> names{
                    "WorldContextObject", "MoveLocation", "MoveRotator", "ForceWalk",
                    "AutoControl", "bHideUI", "ProtectTime", "bUsingPathFinding"};
                for (std::size_t candidate_index{}; candidate_index < names.size(); ++candidate_index) {
                    if (name == names[candidate_index]) {
                        index = candidate_index;
                        break;
                    }
                }
                if (index >= names.size() || found[index]) return false;
                const auto field_class = [&]() {
                    std::string value;
                    return ReadReflectedFieldClassNameLocked(property, value) ? value : std::string{};
                }();
                if ((index == 0 && (element_size != 8 || field_class != "ObjectProperty")) ||
                    (index == 1 && (element_size != 24 || field_class != "StructProperty")) ||
                    (index == 2 && (element_size != 24 || field_class != "StructProperty")) ||
                    ((index == 3 || index == 4 || index == 5 || index == 7) &&
                     (element_size != 1 || field_class != "BoolProperty")) ||
                    (index == 6 && (element_size != 4 || field_class != "FloatProperty"))) {
                    return false;
                }
                if (index == 1 || index == 2) {
                    std::uintptr_t structure{};
                    std::string structure_name;
                    if (!ReadPointerAt(
                            *memory, property, Layout(profile, "fstructProperty.struct"), structure) ||
                        !ReadReflectedObjectNameLocked(structure, structure_name) ||
                        structure_name != (index == 1 ? "Vector" : "Rotator")) {
                        return false;
                    }
                }
                const auto field_offset = static_cast<std::uint16_t>(offset);
                switch (index) {
                case 0: candidate.world_context_object_offset = field_offset; break;
                case 1: candidate.move_location_offset = field_offset; break;
                case 2: candidate.move_rotator_offset = field_offset; break;
                case 3:
                    if (!read_bool(offset, element_size, array_dim, candidate.force_walk)) return false;
                    break;
                case 4:
                    if (!read_bool(offset, element_size, array_dim, candidate.auto_control)) return false;
                    break;
                case 5:
                    if (!read_bool(offset, element_size, array_dim, candidate.hide_ui)) return false;
                    break;
                case 6: candidate.protect_time_offset = field_offset; break;
                case 7:
                    if (!read_bool(offset, element_size, array_dim, candidate.use_pathfinding)) return false;
                    break;
                default: return false;
                }
                found[index] = true;
                ++property_count;
                property = next;
            }
            if (property != 0 || property_count != found.size() ||
                !std::ranges::all_of(found, [](bool value) { return value; })) {
                return false;
            }
            result = candidate;
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool BuildNavigationStopBindingLocked(
        const std::uintptr_t function,
        NavigationBinding& result) const noexcept {
        try {
            std::string function_name;
            std::uintptr_t class_object{};
            std::uintptr_t outer_object{};
            std::uintptr_t property{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_value_offset{};
            if (!ReadReflectedObjectNameLocked(function, function_name) ||
                function_name != "StopMovement" ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.class"), class_object) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), outer_object) ||
                !ReadReflectedObjectNameLocked(class_object, function_name) || function_name != "Function" ||
                !ReadReflectedObjectNameLocked(outer_object, function_name) || function_name != "Controller" ||
                !ReadNullablePointerAt(
                    *memory, function, Layout(profile, "ustruct.propertyLink"), property) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.returnValueOffset"), return_value_offset) ||
                property != 0 || num_parms != 0 || parms_size != 0 ||
                return_value_offset != (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            result.stop_movement = function;
            result.stop_parms_size = parms_size;
            return true;
        } catch (...) {
            return false;
        }
    }

    void RefreshNavigationBindingLocked() noexcept {
        try {
            if (!NteNavigationAvailable() || object_registry.items == 0 || object_registry.count == 0) {
                navigation = {};
                return;
            }
            if (navigation.object_generation != object_generation) {
                navigation = {};
                navigation.object_generation = object_generation;
            }
            if (navigation.available || navigation.discovery_complete) return;
            constexpr std::uint32_t kDiscoveryBatch = 2048;
            const std::uint32_t end = (std::min)(
                object_registry.count, navigation.next_object_index + kDiscoveryBatch);
            for (std::uint32_t index = navigation.next_object_index; index < end; ++index) {
                std::uintptr_t object{};
                std::uint32_t serial{};
                if (!ReadObjectSlot(*memory, object_registry, index, object, serial) || object == 0) continue;
                std::string name;
                if (!ReadReflectedObjectNameLocked(object, name)) continue;
                NavigationBinding candidate = navigation;
                if (!candidate.move_to_point_by_transform && name == "MoveToPointByTransform") {
                    static_cast<void>(BuildNavigationMoveBindingLocked(object, candidate));
                }
                if (!candidate.stop_movement && name == "StopMovement") {
                    static_cast<void>(BuildNavigationStopBindingLocked(object, candidate));
                }
                candidate.next_object_index = index + 1U;
                candidate.registry_items = object_registry.items;
                candidate.object_generation = object_generation;
                if (candidate.move_to_point_by_transform == object) {
                    candidate.move_object_index = index;
                    candidate.move_object_serial = serial;
                }
                if (candidate.stop_movement == object) {
                    candidate.stop_object_index = index;
                    candidate.stop_object_serial = serial;
                }
                navigation = candidate;
                if (navigation.move_to_point_by_transform != 0 && navigation.stop_movement != 0) {
                    navigation.available = true;
                    return;
                }
            }
            navigation.next_object_index = end;
            if (end == object_registry.count) navigation.discovery_complete = true;
        } catch (...) {
            navigation.discovery_complete = true;
        }
    }

    [[nodiscard]] bool NavigationBindingSlotMatchesLocked(
        const NavigationBinding& binding) const noexcept {
        if (!binding.available || binding.object_generation != object_generation ||
            binding.registry_items != object_registry.items || object_registry.items == 0 ||
            binding.move_to_point_by_transform == 0 || binding.stop_movement == 0) {
            return false;
        }
        std::uintptr_t move_object{};
        std::uint32_t move_serial{};
        std::uintptr_t stop_object{};
        std::uint32_t stop_serial{};
        return ReadObjectSlot(
                   *memory, object_registry, binding.move_object_index,
                   move_object, move_serial) &&
            move_object == binding.move_to_point_by_transform &&
            move_serial == binding.move_object_serial &&
            ReadObjectSlot(
                *memory, object_registry, binding.stop_object_index,
                stop_object, stop_serial) &&
            stop_object == binding.stop_movement &&
            stop_serial == binding.stop_object_serial;
    }

    [[nodiscard]] bool ResolveNavigationReceiverLocked(
        const NavigationBinding& binding,
        std::uintptr_t& receiver) const noexcept {
        receiver = 0;
        std::uintptr_t receiver_class{};
        std::string receiver_name;
        return binding.util_class != 0 &&
            ReadPointerAt(
                *memory, binding.util_class,
                Layout(profile, "uclass.classDefaultObject"), receiver) &&
            ReadReflectedObjectNameLocked(receiver, receiver_name) &&
            receiver_name == "Default__HTUtil" &&
            ReadPointerAt(*memory, receiver, Layout(profile, "object.class"), receiver_class) &&
            receiver_class == binding.util_class;
    }

    [[nodiscard]] bool ObjectClassChainContainsLocked(
        const std::uintptr_t object,
        const std::string_view expected) const noexcept {
        std::uintptr_t current{};
        if (!ReadPointerAt(*memory, object, Layout(profile, "object.class"), current)) {
            return false;
        }
        for (std::size_t depth{}; current != 0 && depth < 32; ++depth) {
            std::string name;
            if (!ReadReflectedObjectNameLocked(current, name)) return false;
            if (name == expected) return true;
            std::uintptr_t next{};
            if (!ReadNullablePointerAt(
                    *memory, current, Layout(profile, "ustruct.superStruct"), next) ||
                next == current) {
                return false;
            }
            current = next;
        }
        return false;
    }

    AnomalyStatusV1 ResolveNameId(
        std::uint32_t name_id, char* destination, std::size_t* size) noexcept {
        std::scoped_lock lock(mutex);
        if (!ServiceAvailableForPublication(ANOMALY_UE5_NAMES_SERVICE_V1_ID)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UE5 names service is unavailable");
        }
        return ResolveNameIdLocked(name_id, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL ResolveName(
        void* user, std::uint32_t name_id, char* destination, std::size_t* size) noexcept {
        return static_cast<State*>(user)->ResolveNameId(name_id, destination, size);
    }

    static std::uint64_t ANOMALY_CALL ObjectGeneration(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return state.ServiceAvailableForPublication(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID)
            ? state.object_generation
            : 0;
    }

    static std::uint32_t ANOMALY_CALL ObjectCount(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return state.ServiceAvailableForPublication(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID)
            ? state.object_registry.count
            : 0;
    }

    static AnomalyStatusV1 ObjectSnapshotLocked(
        State& state, std::uint32_t index, const std::uint32_t* expected_serial,
        AnomalyUe5ObjectSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        if (!state.ServiceAvailableForPublication(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UE5 objects service is unavailable");
        }
        if (state.object_registry.items == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "object registry is unavailable");
        }
        if (index >= state.object_registry.count) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "object index is not found");
        }
        std::uintptr_t object{};
        std::uint32_t serial{};
        if (!ReadObjectSlot(
                *state.memory, state.object_registry, index, object, serial)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "object slot is unreadable");
        }
        if (expected_serial != nullptr && serial != *expected_serial) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale object serial");
        }
        if (object == 0) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "object slot is empty");
        }
        std::uint32_t name_id{};
        std::uintptr_t name_address{};
        if (AddAddress(object, Layout(state.profile, "object.nameOffset"), name_address)) {
            static_cast<void>(ReadValue(*state.memory, name_address, name_id));
        }
        snapshot->reserved = 0;
        snapshot->handle = {EncodeObjectHandle(index, serial), state.object_generation};
        snapshot->name_id = name_id;
        snapshot->flags = 0;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ObjectSnapshot(
        void* user, std::uint32_t index, AnomalyUe5ObjectSnapshotV1* snapshot) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return ObjectSnapshotLocked(state, index, nullptr, snapshot);
    }

    static AnomalyStatusV1 ANOMALY_CALL ObjectSnapshotByHandle(
        void* user, AnomalyGenerationHandleV1 handle,
        AnomalyUe5ObjectSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.ServiceAvailableForPublication(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UE5 objects service is unavailable");
        }
        if (handle.generation != state.object_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale object registry generation");
        }
        const auto encoded_index = static_cast<std::uint32_t>(handle.id);
        if (encoded_index == 0) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "object handle is invalid");
        }
        const std::uint32_t index = encoded_index - 1U;
        const std::uint32_t serial = static_cast<std::uint32_t>(handle.id >> 32U);
        return ObjectSnapshotLocked(state, index, &serial, snapshot);
    }

    static AnomalyStatusV1 ANOMALY_CALL FindExactObject(
        void* user,
        const AnomalyStringViewV1 path,
        AnomalyGenerationHandleV1* handle) noexcept {
        if (handle == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        *handle = {};
        try {
            std::wstring decoded;
            if (!DecodeExactObjectPath(path, decoded)) {
                return Status(
                    ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                    "exact object path must be non-empty UTF-8 without NUL bytes");
            }

            auto& state = *static_cast<State*>(user);
            const DWORD expected = state.game_thread_id.load(std::memory_order_acquire);
            if (expected == 0 || expected != GetCurrentThreadId()) {
                return Status(
                    ANOMALY_STATUS_V1_CONFLICT,
                    "exact object lookup requires the Game thread");
            }

            Ue5NteAdapter::ObjectLookup lookup;
            {
                std::scoped_lock lock(state.mutex);
                if (!state.ServiceAvailableForPublication(
                        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID) ||
                    !state.ObjectFindAvailable()) {
                    return Status(
                        ANOMALY_STATUS_V1_UNAVAILABLE,
                        "exact object lookup is unavailable for the active Profile");
                }
                lookup = state.object_lookup;
            }

            const std::uintptr_t object = lookup(decoded.c_str());
            if (object == 0) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "exact object is not loaded");
            }

            std::scoped_lock lock(state.mutex);
            if (!state.ServiceAvailableForPublication(
                    ANOMALY_UE5_OBJECTS_SERVICE_V1_ID) ||
                !state.ObjectFindAvailable() || state.object_registry.items == 0) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "object registry changed during exact lookup");
            }
            std::uintptr_t index_address{};
            std::int32_t internal_index{-1};
            if (!AddAddress(
                    object,
                    Layout(state.profile, "object.internalIndex"),
                    index_address) ||
                !ReadValue(*state.memory, index_address, internal_index) ||
                internal_index < 0 ||
                static_cast<std::uint64_t>(internal_index) >=
                    state.object_registry.count) {
                return Status(
                    ANOMALY_STATUS_V1_NOT_FOUND,
                    "exact object has no valid registry index");
            }
            const auto index = static_cast<std::uint32_t>(internal_index);
            std::uintptr_t slot_object{};
            std::uint32_t serial{};
            if (!ReadObjectSlot(
                    *state.memory,
                    state.object_registry,
                    index,
                    slot_object,
                    serial) ||
                slot_object != object) {
                return Status(
                    ANOMALY_STATUS_V1_NOT_FOUND,
                    "exact object registry identity changed");
            }
            *handle = {EncodeObjectHandle(index, serial), state.object_generation};
            return Status(ANOMALY_STATUS_V1_OK);
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "exact object lookup failed");
        }
    }

    static AnomalyStatusV1 ANOMALY_CALL CurrentWorld(
        void* user, AnomalyGenerationHandleV1* handle) noexcept {
        if (handle == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.ServiceAvailableForPublication(ANOMALY_UE5_WORLD_SERVICE_V1_ID)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UE5 world service is unavailable");
        }
        if (state.world_pointer == 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
        *handle = {1, state.world_generation};
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL WorldSnapshot(
        void* user, AnomalyGenerationHandleV1 handle,
        AnomalyUe5WorldSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.ServiceAvailableForPublication(ANOMALY_UE5_WORLD_SERVICE_V1_ID)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UE5 world service is unavailable");
        }
        if (state.world_pointer == 0 || handle.id != 1 ||
            handle.generation != state.world_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale world handle");
        }
        snapshot->reserved = 0;
        snapshot->handle = handle;
        snapshot->change_sequence = state.world_change_sequence;
        snapshot->name_id = state.world_name_id;
        snapshot->flags = 0;
        if (!state.world_name_layout_available) {
            return Status(
                ANOMALY_STATUS_V1_OK,
                "world.nameOffset and object.nameOffset are unavailable");
        }
        return state.world_name_readable
            ? Status(ANOMALY_STATUS_V1_OK)
            : Status(ANOMALY_STATUS_V1_OK, "world name is unreadable");
    }

    static AnomalyStatusV1 ANOMALY_CALL SessionSnapshot(
        void* user, AnomalyNteSessionSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.session")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE session service is unavailable");
        }
        snapshot->state = state.world_pointer == 0
            ? ANOMALY_NTE_SESSION_V1_LOADING
            : ANOMALY_NTE_SESSION_V1_WORLD_READY;
        snapshot->sequence = state.world_change_sequence;
        snapshot->world = state.world_pointer == 0
            ? AnomalyGenerationHandleV1{}
            : AnomalyGenerationHandleV1{1, state.world_generation};
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL SessionNextEvent(
        void* user,
        const std::uint64_t after_sequence,
        AnomalyNteSessionEventV1* event) noexcept {
        if (event == nullptr || event->struct_size < sizeof(*event)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.session")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE session service is unavailable");
        }
        if (state.session_event_count == 0 ||
            after_sequence >= state.session_event_sequence) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "no newer session event");
        }
        const SessionEvent& first = state.session_events[state.session_event_start];
        if (after_sequence != 0 && after_sequence < first.sequence - 1U) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "session event cursor has expired");
        }
        for (std::size_t index = 0; index < state.session_event_count; ++index) {
            const SessionEvent& candidate = state.session_events[
                (state.session_event_start + index) % kSessionEventCapacity];
            if (candidate.sequence <= after_sequence) continue;
            event->kind = candidate.kind;
            event->sequence = candidate.sequence;
            event->tick_sequence = candidate.tick_sequence;
            event->previous_world = candidate.previous_world;
            event->world = candidate.world;
            return Status(ANOMALY_STATUS_V1_OK);
        }
        return Status(ANOMALY_STATUS_V1_NOT_FOUND, "no newer session event");
    }

    static std::uint64_t ANOMALY_CALL SessionLatestEventSequence(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return state.SemanticFeatureRunning("nte.session") ? state.session_event_sequence : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL Teleport(
        void* user,
        const AnomalyNtePlayerTeleportRequestV1* request) noexcept {
        if (request == nullptr || request->struct_size < sizeof(*request) || request->flags != 0 ||
            !std::ranges::all_of(
                request->position, [](double value) { return std::isfinite(value); })) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        const DWORD bound_game_thread = state.game_thread_id.load(std::memory_order_acquire);
        if (bound_game_thread == 0 || bound_game_thread != GetCurrentThreadId()) {
            return Status(ANOMALY_STATUS_V1_CONFLICT, "teleport requires the Game thread");
        }

        const std::array<double, 3> target{
            request->position[0], request->position[1], request->position[2]};
        const AnomalyGenerationHandleV1 world = request->world;
        const AnomalyGenerationHandleV1 player = request->player;
        TeleportBinding binding;
        Ue5NteAdapter::ProcessEventInvoker invoker;
        std::uintptr_t pawn{};
        {
            std::scoped_lock lock(state.mutex);
            if (!state.SemanticFeatureRunning("nte.player-teleport")) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "teleport is unavailable for the active Profile");
            }
            if (!state.teleport.available ||
                state.teleport.object_generation != state.object_generation) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "teleport reflection is not ready");
            }
            if (!state.process_event_invoker) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "teleport requires a trusted invocation bridge");
            }
            if (state.world_pointer == 0 || world.id != 1 ||
                world.generation != state.world_generation) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale world handle");
            }
            if (!state.player_available || player.id != 1 ||
                player.generation != state.player_generation) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale player handle");
            }

            PlayerLocationSample live;
            if (!state.ReadCurrentPlayerLocation(live)) {
                return Status(ANOMALY_STATUS_V1_FAILED, "local player chain is unreadable");
            }
            if (live.controller != state.player_controller || live.pawn != state.player_pawn ||
                live.root != state.player_root) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "local player identity changed");
            }
            binding = state.teleport;
            pawn = live.pawn;
            invoker = state.process_event_invoker;
        }

        try {
            std::vector<std::uint64_t> parameter_words(
                (static_cast<std::size_t>(binding.parms_size) + sizeof(std::uint64_t) - 1U) /
                    sizeof(std::uint64_t),
                0);
            auto* parameters = reinterpret_cast<std::uint8_t*>(parameter_words.data());
            std::memcpy(parameters + binding.new_location_offset, target.data(), sizeof(target));
            const auto set_bool = [parameters](
                                      const ReflectedBoolParameter& parameter,
                                      const bool value) {
                auto& byte = parameters[parameter.byte_offset];
                byte = static_cast<std::uint8_t>((byte & ~parameter.field_mask) |
                    (value ? parameter.byte_mask : 0U));
            };
            set_bool(binding.b_sweep, false);
            set_bool(binding.b_teleport, true);

            bool invoked{};
            invoked = invoker(pawn, binding.function, parameters, binding.parms_size);
            if (!invoked ||
                (parameters[binding.return_value.byte_offset] & binding.return_value.field_mask) == 0) {
                return Status(ANOMALY_STATUS_V1_FAILED, "teleport returned false");
            }
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "teleport invocation failed");
        }

        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.player-teleport")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "teleport service stopped");
        }
        if (state.world_pointer == 0 || world.id != 1 ||
            world.generation != state.world_generation || !state.player_available ||
            player.id != 1 || player.generation != state.player_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "player identity changed during teleport");
        }
        PlayerLocationSample observed;
        if (!state.ReadCurrentPlayerLocation(observed)) {
            return Status(ANOMALY_STATUS_V1_FAILED, "post-teleport player chain is unreadable");
        }
        if (observed.pawn != pawn || observed.pawn != state.player_pawn ||
            observed.controller != state.player_controller || observed.root != state.player_root) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "local player identity changed during teleport");
        }
        constexpr double kPositionTolerance = 0.01;
        for (std::size_t axis{}; axis != target.size(); ++axis) {
            if (std::fabs(observed.position[axis] - target[axis]) > kPositionTolerance) {
                return Status(ANOMALY_STATUS_V1_FAILED, "teleport postcondition failed");
            }
        }
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL MoveToLocation(
        void* user,
        const double destination[3]) noexcept {
        if (destination == nullptr ||
            !std::ranges::all_of(
                std::span(destination, 3),
                [](const double value) { return std::isfinite(value); })) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        const DWORD bound_game_thread = state.game_thread_id.load(std::memory_order_acquire);
        if (bound_game_thread == 0 || bound_game_thread != GetCurrentThreadId()) {
            return Status(ANOMALY_STATUS_V1_CONFLICT, "navigation requires the Game thread");
        }

        NavigationBinding binding;
        Ue5NteAdapter::ProcessEventInvoker invoker;
        std::shared_ptr<NteNavigationInputPolicy> input_policy;
        std::uintptr_t receiver{};
        std::uintptr_t world{};
        std::array<double, 3> rotation{};
        {
            std::scoped_lock lock(state.mutex);
            if (!state.SemanticFeatureRunning("nte.navigation")) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "navigation is unavailable for the active Profile");
            }
            if (!state.navigation.available ||
                !state.NavigationBindingSlotMatchesLocked(state.navigation)) {
                state.navigation = {};
                state.navigation.object_generation = state.object_generation;
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "navigation reflection is not ready");
            }
            if (!state.process_event_invoker || state.navigation_input_policy == nullptr ||
                !state.navigation_input_policy->Started()) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "navigation invocation policy is unavailable");
            }
            PlayerLocationSample live;
            if (!state.ReadCurrentPlayerLocation(live) ||
                !state.ObjectClassChainContainsLocked(live.controller, "HTPlayerController") ||
                !state.ResolveNavigationReceiverLocked(state.navigation, receiver)) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "local navigation objects are unavailable");
            }
            std::uintptr_t rotation_address{};
            if (!AddAddress(
                    live.controller, Layout(state.profile, "controller.controlRotation"),
                    rotation_address) ||
                !ReadValue(*state.memory, rotation_address, rotation) ||
                !std::ranges::all_of(
                    rotation, [](const double value) { return std::isfinite(value); })) {
                return Status(
                    ANOMALY_STATUS_V1_FAILED,
                    "controller rotation is unreadable");
            }
            binding = state.navigation;
            invoker = state.process_event_invoker;
            input_policy = state.navigation_input_policy;
            world = state.world_pointer;
        }

        constexpr std::size_t kMoveParametersSize = 0x41;
        if (binding.move_parms_size != kMoveParametersSize || world == 0 || receiver == 0 ||
            binding.world_context_object_offset + sizeof(world) > kMoveParametersSize ||
            binding.move_location_offset + sizeof(double) * 3U > kMoveParametersSize ||
            binding.move_rotator_offset + sizeof(rotation) > kMoveParametersSize ||
            binding.protect_time_offset + sizeof(float) > kMoveParametersSize) {
            return Status(ANOMALY_STATUS_V1_FAILED, "navigation parameter layout changed");
        }

        alignas(std::uint64_t) std::array<std::uint8_t, kMoveParametersSize> parameters{};
        std::memcpy(
            parameters.data() + binding.world_context_object_offset,
            &world, sizeof(world));
        std::memcpy(
            parameters.data() + binding.move_location_offset,
            destination, sizeof(double) * 3U);
        std::memcpy(
            parameters.data() + binding.move_rotator_offset,
            rotation.data(), sizeof(rotation));
        constexpr float kProtectTime = 0.0F;
        std::memcpy(
            parameters.data() + binding.protect_time_offset,
            &kProtectTime, sizeof(kProtectTime));
        const auto set_bool = [&parameters](
                                  const ReflectedBoolParameter& parameter,
                                  const bool value) noexcept {
            if (parameter.byte_offset >= parameters.size() ||
                parameter.field_mask == 0 || parameter.byte_mask == 0 ||
                (parameter.byte_mask & parameter.field_mask) != parameter.byte_mask) {
                return false;
            }
            auto& byte = parameters[parameter.byte_offset];
            byte = static_cast<std::uint8_t>(
                (byte & ~parameter.field_mask) |
                (value ? parameter.byte_mask : 0U));
            return true;
        };
        if (!set_bool(binding.force_walk, false) ||
            !set_bool(binding.auto_control, true) ||
            !set_bool(binding.hide_ui, false) ||
            !set_bool(binding.use_pathfinding, true)) {
            return Status(ANOMALY_STATUS_V1_FAILED, "navigation bool layout changed");
        }

        class InputPolicyScope final {
        public:
            explicit InputPolicyScope(std::shared_ptr<NteNavigationInputPolicy> policy) noexcept
                : policy_(std::move(policy)), previous_(policy_->Enter()) {}
            ~InputPolicyScope() { policy_->Leave(previous_); }
            InputPolicyScope(const InputPolicyScope&) = delete;
            InputPolicyScope& operator=(const InputPolicyScope&) = delete;
        private:
            std::shared_ptr<NteNavigationInputPolicy> policy_;
            void* previous_{};
        } input_scope(std::move(input_policy));

        try {
            return invoker(
                       receiver,
                       binding.move_to_point_by_transform,
                       parameters.data(),
                       binding.move_parms_size)
                ? Status(ANOMALY_STATUS_V1_OK)
                : Status(ANOMALY_STATUS_V1_FAILED, "navigation dispatch failed");
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "navigation invocation failed");
        }
    }

    static AnomalyStatusV1 ANOMALY_CALL StopMovement(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        const DWORD bound_game_thread = state.game_thread_id.load(std::memory_order_acquire);
        if (bound_game_thread == 0 || bound_game_thread != GetCurrentThreadId()) {
            return Status(ANOMALY_STATUS_V1_CONFLICT, "navigation requires the Game thread");
        }

        NavigationBinding binding;
        Ue5NteAdapter::ProcessEventInvoker invoker;
        std::uintptr_t controller{};
        {
            std::scoped_lock lock(state.mutex);
            if (!state.SemanticFeatureRunning("nte.navigation")) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "navigation is unavailable for the active Profile");
            }
            if (!state.navigation.available ||
                !state.NavigationBindingSlotMatchesLocked(state.navigation)) {
                state.navigation = {};
                state.navigation.object_generation = state.object_generation;
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "navigation reflection is not ready");
            }
            PlayerLocationSample live;
            if (!state.ReadCurrentPlayerLocation(live) ||
                !state.ObjectClassChainContainsLocked(live.controller, "HTPlayerController")) {
                return Status(
                    ANOMALY_STATUS_V1_UNAVAILABLE,
                    "local player controller is unavailable");
            }
            binding = state.navigation;
            invoker = state.process_event_invoker;
            controller = live.controller;
        }

        try {
            return invoker && invoker(
                       controller, binding.stop_movement, nullptr,
                       binding.stop_parms_size)
                ? Status(ANOMALY_STATUS_V1_OK)
                : Status(ANOMALY_STATUS_V1_FAILED, "navigation stop failed");
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "navigation stop invocation failed");
        }
    }

    static AnomalyStatusV1 ANOMALY_CALL PlayerSnapshot(
        void* user, AnomalyNtePlayerSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.player")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE player service is unavailable");
        }
        if (!state.player_available) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
        const auto current_sequence = state.tick_sequence.load(std::memory_order_acquire);
        snapshot->flags = SnapshotFlags(
            state.player_partial, state.player_sample_sequence, current_sequence);
        snapshot->handle = {1, state.player_generation};
        snapshot->sequence = state.player_sample_sequence;
        std::ranges::copy(state.player_position, snapshot->position);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL PlayerEspSnapshot(
        void* user, AnomalyNtePlayerEspSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.player-esp")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE player ESP service is unavailable");
        }
        if (!state.player_esp_available) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "player ESP snapshot is unavailable");
        }
        const auto current_sequence = state.tick_sequence.load(std::memory_order_acquire);
        snapshot->flags = SnapshotFlags(
            state.player_partial, state.player_sample_sequence, current_sequence);
        snapshot->handle = {1, state.player_generation};
        snapshot->sequence = state.player_sample_sequence;
        std::ranges::copy(state.player_bounds_center, snapshot->bounds_center);
        std::ranges::copy(state.player_bounds_extent, snapshot->bounds_extent);
        std::ranges::copy(state.camera_position, snapshot->camera_position);
        std::ranges::copy(state.camera_rotation, snapshot->camera_rotation);
        snapshot->horizontal_fov_degrees = state.camera_horizontal_fov;
        snapshot->reserved = 0;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL CameraSnapshot(
        void* user, AnomalyNteCameraSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.player-esp")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE player ESP service is unavailable");
        }
        if (!state.player_esp_available || state.world_pointer == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "camera snapshot is unavailable");
        }
        const auto current_sequence = state.tick_sequence.load(std::memory_order_acquire);
        snapshot->flags = SnapshotFlags(
            state.player_partial, state.player_sample_sequence, current_sequence);
        snapshot->world = {1, state.world_generation};
        snapshot->player = {1, state.player_generation};
        snapshot->sequence = state.player_sample_sequence;
        std::ranges::copy(state.camera_position, snapshot->position);
        std::ranges::copy(state.camera_rotation, snapshot->rotation);
        snapshot->horizontal_fov_degrees = state.camera_horizontal_fov;
        snapshot->reserved = 0;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static std::shared_ptr<const EntityFrameCache> CaptureEntityFrame(
        State& state) noexcept {
        std::scoped_lock lock(state.mutex);
        return state.SemanticFeatureRunning("nte.entities") ? state.entity_frame_cache : nullptr;
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityFrame(
        void* user, AnomalyNteEntityFrameV1* frame) noexcept {
        if (frame == nullptr || frame->struct_size < sizeof(*frame)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.entity_demand.store(true, std::memory_order_release);
        const std::shared_ptr<const EntityFrameCache> cache = CaptureEntityFrame(state);
        if (!cache) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "entity frame is unavailable");
        }
        const auto current_sequence = state.tick_sequence.load(std::memory_order_acquire);
        frame->flags = SnapshotFlags(cache->partial, cache->sequence, current_sequence);
        frame->generation = cache->generation;
        frame->sequence = cache->sequence;
        frame->entity_count = static_cast<std::uint32_t>(cache->entities.size());
        frame->reserved = 0;
        std::ranges::copy(cache->camera_position, frame->camera_position);
        std::ranges::copy(cache->camera_rotation, frame->camera_rotation);
        frame->horizontal_fov_degrees = cache->camera_horizontal_fov;
        frame->reserved2 = 0;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static void PopulateEntitySnapshot(
        const EntityFrameCache& cache,
        const std::uint64_t current_sequence,
        const EntityRecord& entity,
        AnomalyNteEntitySnapshotV1* snapshot) noexcept {
        snapshot->flags = entity.flags |
            SnapshotFlags(cache.partial, cache.sequence, current_sequence);
        snapshot->handle = {entity.entity_id, cache.generation};
        snapshot->entity_id = entity.entity_id;
        snapshot->class_id = entity.class_id;
        snapshot->entity_name_id = entity.entity_name_id;
        snapshot->class_name_id = entity.class_name_id;
        std::ranges::copy(entity.bounds_center, snapshot->bounds_center);
        std::ranges::copy(entity.bounds_extent, snapshot->bounds_extent);
    }

    static bool EntityMatches(
        const EntityRecord& entity,
        const AnomalyNteEntityPageRequestV1& request) noexcept {
        return (request.class_id == 0 || entity.class_id == request.class_id) &&
            (request.class_name_id == 0 || entity.class_name_id == request.class_name_id) &&
            (request.entity_name_id == 0 || entity.entity_name_id == request.entity_name_id) &&
            (entity.flags & request.required_flags) == request.required_flags &&
            (entity.flags & request.excluded_flags) == 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL EntitySnapshotAt(
        void* user, std::uint64_t generation, std::uint32_t index,
        AnomalyNteEntitySnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        const std::shared_ptr<const EntityFrameCache> cache = CaptureEntityFrame(state);
        if (!cache) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "entity frame is unavailable");
        }
        if (generation != cache->generation || index >= cache->entities.size()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale entity frame or index");
        }
        PopulateEntitySnapshot(
            *cache, state.tick_sequence.load(std::memory_order_acquire), cache->entities[index], snapshot);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityPage(
        void* user,
        const AnomalyNteEntityPageRequestV1* request,
        AnomalyNteEntitySnapshotV1* destination,
        AnomalyNteEntityPageResultV1* result) noexcept {
        if (request == nullptr || result == nullptr ||
            request->struct_size < sizeof(*request) ||
            result->struct_size < sizeof(*result) ||
            request->flags != 0 ||
            request->capacity > ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY ||
            (request->capacity != 0 && destination == nullptr)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        for (std::uint32_t index = 0; index < request->capacity; ++index) {
            if (destination[index].struct_size < sizeof(AnomalyNteEntitySnapshotV1)) {
                return Status(
                    ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                    "entity page destination has an invalid struct_size");
            }
        }

        auto& state = *static_cast<State*>(user);
        std::shared_ptr<const EntityFrameCache> cache;
        {
            std::scoped_lock lock(state.mutex);
            if (!state.SemanticFeatureRunning("nte.entities")) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE entities service is unavailable");
            }
            cache = state.entity_frame_cache;
            if (!cache) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "entity frame is unavailable");
            }
            if (request->generation != 0 && request->generation != cache->generation) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale entity frame generation");
            }
            ++state.entity_page_request_count;
            ++state.entity_page_cache_hit_count;
        }

        const auto current_sequence = state.tick_sequence.load(std::memory_order_acquire);
        result->flags = SnapshotFlags(cache->partial, cache->sequence, current_sequence);
        result->generation = cache->generation;
        result->sequence = cache->sequence;
        result->total_matches = 0;
        result->returned = 0;
        result->next_offset = 0;
        result->reserved = 0;

        const bool unfiltered = request->class_id == 0 && request->class_name_id == 0 &&
            request->entity_name_id == 0 && request->required_flags == 0 &&
            request->excluded_flags == 0;
        if (unfiltered) {
            result->total_matches = static_cast<std::uint32_t>(cache->entities.size());
            if (request->offset < result->total_matches) {
                const std::uint32_t available = result->total_matches - request->offset;
                result->returned = (std::min)(request->capacity, available);
                for (std::uint32_t index = 0; index < result->returned; ++index) {
                    PopulateEntitySnapshot(
                        *cache, current_sequence,
                        cache->entities[static_cast<std::size_t>(request->offset) + index],
                        &destination[index]);
                }
            }
            const std::uint64_t consumed =
                static_cast<std::uint64_t>(request->offset) + result->returned;
            result->next_offset = consumed < result->total_matches
                ? static_cast<std::uint32_t>(consumed)
                : result->total_matches;
            return Status(ANOMALY_STATUS_V1_OK);
        }

        for (const EntityRecord& entity : cache->entities) {
            if (!EntityMatches(entity, *request)) continue;
            if (result->total_matches >= request->offset &&
                result->returned < request->capacity) {
                PopulateEntitySnapshot(
                    *cache, current_sequence, entity, &destination[result->returned]);
                ++result->returned;
            }
            ++result->total_matches;
        }
        const std::uint64_t consumed =
            static_cast<std::uint64_t>(request->offset) + result->returned;
        result->next_offset = consumed < result->total_matches
            ? static_cast<std::uint32_t>(consumed)
            : result->total_matches;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityClassName(
        void* user, std::uint64_t class_id, char* destination,
        std::size_t* size) noexcept {
        auto& state = *static_cast<State*>(user);
        const std::shared_ptr<const EntityFrameCache> cache = CaptureEntityFrame(state);
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "entity frame is unavailable");
        const auto found = std::ranges::find_if(cache->entities, [class_id](const auto& entity) {
            return entity.class_id == class_id;
        });
        if (found == cache->entities.end()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        const auto name = cache->class_names.find(class_id);
        if (name == cache->class_names.end()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.entities")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE entities service is unavailable");
        }
        if (state.entity_frame_cache != cache) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "entity frame changed");
        }
        return CopyString(name->second, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityName(
        void* user, std::uint64_t entity_id, char* destination,
        std::size_t* size) noexcept {
        auto& state = *static_cast<State*>(user);
        const std::shared_ptr<const EntityFrameCache> cache = CaptureEntityFrame(state);
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "entity frame is unavailable");
        const auto found = std::ranges::find_if(cache->entities, [entity_id](const auto& entity) {
            return entity.entity_id == entity_id;
        });
        if (found == cache->entities.end()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        const auto name = cache->entity_names.find(entity_id);
        if (name == cache->entity_names.end()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.entities")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE entities service is unavailable");
        }
        if (state.entity_frame_cache != cache) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "entity frame changed");
        }
        return CopyString(name->second, destination, size);
    }

    static std::shared_ptr<const EntityFrameCache> CaptureActorFrame(State& state) noexcept {
        std::scoped_lock lock(state.mutex);
        return state.NteActorsLayoutAvailable() ? state.actor_frame_cache : nullptr;
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorFrame(
        void* user, AnomalyNteEntityFrameV1* frame) noexcept {
        if (frame == nullptr || frame->struct_size < sizeof(*frame)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::shared_ptr<const EntityFrameCache> cache;
        {
            std::scoped_lock lock(state.mutex);
            if (!state.NteActorsLayoutAvailable()) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE actors service is unavailable");
            }
            if (!state.actor_frame_cache ||
                state.actor_world_generation != state.world_generation) {
                const DWORD expected = state.game_thread_id.load(std::memory_order_acquire);
                if (expected == 0 || expected != GetCurrentThreadId() ||
                    g_active_tick_callback_state.Get() != &state) {
                    return Status(
                        ANOMALY_STATUS_V1_UNAVAILABLE,
                        "actor discovery requires the active Game callback domain");
                }
                state.RefreshActors(state.tick_sequence.load(std::memory_order_acquire));
            }
            cache = state.actor_frame_cache;
        }
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "actor frame is unavailable");
        frame->flags = SnapshotFlags(cache->partial, cache->sequence, cache->sequence);
        frame->generation = cache->generation;
        frame->sequence = cache->sequence;
        frame->entity_count = static_cast<std::uint32_t>(cache->entities.size());
        frame->reserved = 0;
        std::ranges::copy(cache->camera_position, frame->camera_position);
        std::ranges::copy(cache->camera_rotation, frame->camera_rotation);
        frame->horizontal_fov_degrees = cache->camera_horizontal_fov;
        frame->reserved2 = 0;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorSnapshotAt(
        void* user, std::uint64_t generation, std::uint32_t index,
        AnomalyNteEntitySnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        const auto cache = CaptureActorFrame(state);
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "actor frame is unavailable");
        if (generation != cache->generation || index >= cache->entities.size()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale actor frame or index");
        }
        PopulateEntitySnapshot(*cache, cache->sequence, cache->entities[index], snapshot);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorPage(
        void* user, const AnomalyNteEntityPageRequestV1* request,
        AnomalyNteEntitySnapshotV1* destination,
        AnomalyNteEntityPageResultV1* result) noexcept {
        if (request == nullptr || result == nullptr ||
            request->struct_size < sizeof(*request) || result->struct_size < sizeof(*result) ||
            request->flags != 0 ||
            request->capacity > ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY ||
            (request->capacity != 0 && destination == nullptr)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        for (std::uint32_t index = 0; index < request->capacity; ++index) {
            if (destination[index].struct_size < sizeof(AnomalyNteEntitySnapshotV1)) {
                return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
            }
        }
        auto& state = *static_cast<State*>(user);
        const auto cache = CaptureActorFrame(state);
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "actor frame is unavailable");
        if (request->generation != 0 && request->generation != cache->generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale actor frame generation");
        }
        result->flags = SnapshotFlags(cache->partial, cache->sequence, cache->sequence);
        result->generation = cache->generation;
        result->sequence = cache->sequence;
        result->total_matches = 0;
        result->returned = 0;
        result->next_offset = 0;
        result->reserved = 0;
        for (const EntityRecord& actor : cache->entities) {
            if (!EntityMatches(actor, *request)) continue;
            if (result->total_matches >= request->offset &&
                result->returned < request->capacity) {
                PopulateEntitySnapshot(
                    *cache, cache->sequence, actor, &destination[result->returned]);
                ++result->returned;
            }
            ++result->total_matches;
        }
        const std::uint64_t consumed =
            static_cast<std::uint64_t>(request->offset) + result->returned;
        result->next_offset = consumed < result->total_matches
            ? static_cast<std::uint32_t>(consumed)
            : result->total_matches;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorClassName(
        void* user, std::uint64_t class_id, char* destination,
        std::size_t* size) noexcept {
        auto& state = *static_cast<State*>(user);
        const auto cache = CaptureActorFrame(state);
        if (!cache) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "actor frame is unavailable");
        }
        const auto name = cache->class_names.find(class_id);
        return name == cache->class_names.end()
            ? Status(ANOMALY_STATUS_V1_NOT_FOUND)
            : CopyString(name->second, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorName(
        void* user, std::uint64_t actor_id, char* destination,
        std::size_t* size) noexcept {
        auto& state = *static_cast<State*>(user);
        const auto cache = CaptureActorFrame(state);
        if (!cache) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "actor frame is unavailable");
        }
        const auto name = cache->entity_names.find(actor_id);
        return name == cache->entity_names.end()
            ? Status(ANOMALY_STATUS_V1_NOT_FOUND)
            : CopyString(name->second, destination, size);
    }

    struct ReflectedEntityProperty {
        std::uintptr_t field{};
        std::int32_t offset{};
        std::int32_t element_size{};
    };

    [[nodiscard]] bool FindReflectedEntityPropertyLocked(
        const EntityRecord& entity,
        const std::string_view requested_name,
        ReflectedEntityProperty& result) const {
        if (entity.class_object == 0 || requested_name.empty() || requested_name.size() > 128) {
            return false;
        }
        std::uintptr_t property{};
        if (!ReadPointerAt(
                *memory, entity.class_object, Layout(profile, "ustruct.propertyLink"), property)) {
            return false;
        }
        for (std::uint32_t count = 0; property != 0 && count < 2048; ++count) {
            std::uintptr_t name_address{};
            std::uintptr_t next_address{};
            std::uintptr_t next{};
            std::uint32_t name_id{};
            std::int32_t array_dim{};
            std::int32_t element_size{};
            std::int32_t offset{};
            if (!AddAddress(property, Layout(profile, "ffield.name"), name_address) ||
                !AddAddress(
                    property, Layout(profile, "fproperty.propertyLinkNext"), next_address) ||
                !ReadValue(*memory, name_address, name_id) ||
                !ReadValue(*memory, property + Layout(profile, "fproperty.arrayDim"), array_dim) ||
                !ReadValue(
                    *memory, property + Layout(profile, "fproperty.elementSize"), element_size) ||
                !ReadValue(
                    *memory, property + Layout(profile, "fproperty.offsetInternal"), offset) ||
                !ReadValue(*memory, next_address, next) || next == property ||
                array_dim <= 0 || array_dim > 1024 || element_size <= 0 ||
                element_size > 1024 * 1024 || offset < 0 || offset > 64 * 1024 * 1024) {
                return false;
            }
            if (ResolveNameSnapshotLocked(name_id) == requested_name) {
                result = {property, offset, element_size};
                return true;
            }
            property = next;
        }
        return false;
    }

    [[nodiscard]] static const EntityRecord* FindEntityByHandleLocked(
        const EntityFrameCache& cache,
        const AnomalyGenerationHandleV1 handle) noexcept {
        if (handle.id == 0 || handle.generation != cache.generation) return nullptr;
        const auto found = std::ranges::find_if(cache.entities, [handle](const EntityRecord& entity) {
            return entity.entity_id == handle.id;
        });
        return found == cache.entities.end() ? nullptr : &*found;
    }

    [[nodiscard]] bool EntityReflectionCallAvailableLocked() const noexcept {
        const DWORD expected = game_thread_id.load(std::memory_order_acquire);
        return SemanticFeatureRunning("nte.entities") &&
            NteEntityReflectionLayoutAvailable() && expected != 0 &&
            expected == GetCurrentThreadId() &&
            g_active_tick_callback_state.Get() == this;
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityComponentBounds(
        void* user, const AnomalyGenerationHandleV1 handle,
        const AnomalyStringViewV1 property_name,
        AnomalyNteEntityComponentBoundsV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot) ||
            property_name.data == nullptr || property_name.size == 0 || property_name.size > 128) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.EntityReflectionCallAvailableLocked()) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "entity reflection reads require the active Game callback domain");
        }
        const auto cache = state.entity_frame_cache;
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "entity frame is unavailable");
        const EntityRecord* entity = FindEntityByHandleLocked(*cache, handle);
        if (entity == nullptr) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale entity handle");

        ReflectedEntityProperty property;
        if (!state.FindReflectedEntityPropertyLocked(
                *entity, {property_name.data, property_name.size}, property) ||
            property.element_size != static_cast<std::int32_t>(sizeof(std::uintptr_t))) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "scene component property was not found");
        }
        std::uintptr_t component{};
        std::uintptr_t property_address{};
        std::uintptr_t center_address{};
        std::uintptr_t extent_address{};
        std::array<double, 3> center{};
        std::array<double, 3> extent{};
        if (!AddAddress(entity->actor, property.offset, property_address) ||
            !ReadValue(*state.memory, property_address, component) || component == 0 ||
            !AddAddress(
                component, Layout(state.profile, "sceneComponent.boundsOrigin"), center_address) ||
            !AddAddress(
                component, Layout(state.profile, "sceneComponent.boundsExtent"), extent_address) ||
            !state.memory->Read(center_address, center.data(), sizeof(center)) ||
            !state.memory->Read(extent_address, extent.data(), sizeof(extent)) ||
            !std::ranges::all_of(center, [](double value) { return std::isfinite(value); }) ||
            !std::ranges::all_of(extent, [](double value) {
                return std::isfinite(value) && value >= 0.0 && value <= 1000000000.0;
            })) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "scene component bounds are unavailable");
        }
        snapshot->flags = SnapshotFlags(
            cache->partial, cache->sequence,
            state.tick_sequence.load(std::memory_order_acquire));
        snapshot->entity = handle;
        snapshot->sequence = cache->sequence;
        std::ranges::copy(center, snapshot->bounds_center);
        std::ranges::copy(extent, snapshot->bounds_extent);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityBoolProperty(
        void* user, const AnomalyGenerationHandleV1 handle,
        const AnomalyStringViewV1 property_name,
        AnomalyNteEntityBoolPropertyV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot) ||
            property_name.data == nullptr || property_name.size == 0 || property_name.size > 128) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.EntityReflectionCallAvailableLocked()) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "entity reflection reads require the active Game callback domain");
        }
        const auto cache = state.entity_frame_cache;
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "entity frame is unavailable");
        const EntityRecord* entity = FindEntityByHandleLocked(*cache, handle);
        if (entity == nullptr) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale entity handle");

        ReflectedEntityProperty property;
        std::uint8_t field_size{};
        std::uint8_t byte_offset{};
        std::uint8_t byte_mask{};
        std::uint8_t field_mask{};
        if (!state.FindReflectedEntityPropertyLocked(
                *entity, {property_name.data, property_name.size}, property) ||
            property.element_size != 1 ||
            !ReadValue(
                *state.memory,
                property.field + Layout(state.profile, "fboolProperty.fieldSize"), field_size) ||
            !ReadValue(
                *state.memory,
                property.field + Layout(state.profile, "fboolProperty.byteOffset"), byte_offset) ||
            !ReadValue(
                *state.memory,
                property.field + Layout(state.profile, "fboolProperty.byteMask"), byte_mask) ||
            !ReadValue(
                *state.memory,
                property.field + Layout(state.profile, "fboolProperty.fieldMask"), field_mask) ||
            field_size == 0 || byte_offset >= field_size || byte_mask == 0 || field_mask == 0 ||
            (byte_mask & field_mask) != byte_mask) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "bool property was not found");
        }
        std::uintptr_t value_address{};
        std::uint8_t value{};
        if (!AddAddress(
                entity->actor,
                static_cast<std::int64_t>(property.offset) + byte_offset,
                value_address) ||
            !ReadValue(*state.memory, value_address, value)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "bool property is unreadable");
        }
        snapshot->flags = SnapshotFlags(
            cache->partial, cache->sequence,
            state.tick_sequence.load(std::memory_order_acquire));
        snapshot->entity = handle;
        snapshot->sequence = cache->sequence;
        snapshot->value = (value & byte_mask) != 0 ? 1U : 0U;
        snapshot->reserved = 0;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityFNameProperty(
        void* user, const AnomalyGenerationHandleV1 handle,
        const AnomalyStringViewV1 property_name,
        char* destination, std::size_t* size) noexcept {
        if (size == nullptr || property_name.data == nullptr || property_name.size == 0 ||
            property_name.size > 128) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.EntityReflectionCallAvailableLocked()) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "entity reflection reads require the active Game callback domain");
        }
        const auto cache = state.entity_frame_cache;
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "entity frame is unavailable");
        const EntityRecord* entity = FindEntityByHandleLocked(*cache, handle);
        if (entity == nullptr) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale entity handle");

        ReflectedEntityProperty property;
        struct FNameValue {
            std::uint32_t comparison_index{};
            std::uint32_t number{};
        } value;
        std::uintptr_t value_address{};
        if (!state.FindReflectedEntityPropertyLocked(
                *entity, {property_name.data, property_name.size}, property) ||
            property.element_size != static_cast<std::int32_t>(sizeof(value)) ||
            !AddAddress(entity->actor, property.offset, value_address) ||
            !ReadValue(*state.memory, value_address, value)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "FName property was not found");
        }
        std::string name = state.ResolveNameSnapshotLocked(value.comparison_index);
        if (name.empty()) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "FName value is unavailable");
        if (value.number != 0) {
            name += "_" + std::to_string(value.number - 1U);
        }
        return CopyString(name, destination, size);
    }

    [[nodiscard]] bool ActorReflectionCallAvailableLocked() const noexcept {
        const DWORD expected = game_thread_id.load(std::memory_order_acquire);
        return NteActorsLayoutAvailable() && expected != 0 &&
            expected == GetCurrentThreadId() &&
            g_active_tick_callback_state.Get() == this;
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorComponentBounds(
        void* user, const AnomalyGenerationHandleV1 handle,
        const AnomalyStringViewV1 property_name,
        AnomalyNteEntityComponentBoundsV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot) ||
            property_name.data == nullptr || property_name.size == 0 || property_name.size > 128) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.ActorReflectionCallAvailableLocked()) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "actor reflection reads require the active Game callback domain");
        }
        const auto cache = state.actor_frame_cache;
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "actor frame is unavailable");
        const EntityRecord* entity = FindEntityByHandleLocked(*cache, handle);
        if (entity == nullptr) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale entity handle");

        ReflectedEntityProperty property;
        if (!state.FindReflectedEntityPropertyLocked(
                *entity, {property_name.data, property_name.size}, property) ||
            property.element_size != static_cast<std::int32_t>(sizeof(std::uintptr_t))) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "scene component property was not found");
        }
        std::uintptr_t component{};
        std::uintptr_t property_address{};
        std::uintptr_t center_address{};
        std::uintptr_t extent_address{};
        std::array<double, 3> center{};
        std::array<double, 3> extent{};
        if (!AddAddress(entity->actor, property.offset, property_address) ||
            !ReadValue(*state.memory, property_address, component) || component == 0 ||
            !AddAddress(
                component, Layout(state.profile, "sceneComponent.boundsOrigin"), center_address) ||
            !AddAddress(
                component, Layout(state.profile, "sceneComponent.boundsExtent"), extent_address) ||
            !state.memory->Read(center_address, center.data(), sizeof(center)) ||
            !state.memory->Read(extent_address, extent.data(), sizeof(extent)) ||
            !std::ranges::all_of(center, [](double value) { return std::isfinite(value); }) ||
            !std::ranges::all_of(extent, [](double value) {
                return std::isfinite(value) && value >= 0.0 && value <= 1000000000.0;
            })) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "scene component bounds are unavailable");
        }
        const auto current_sequence = state.tick_sequence.load(std::memory_order_acquire);
        snapshot->flags = SnapshotFlags(cache->partial, current_sequence, current_sequence);
        snapshot->entity = handle;
        snapshot->sequence = current_sequence;
        std::ranges::copy(center, snapshot->bounds_center);
        std::ranges::copy(extent, snapshot->bounds_extent);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorBoolProperty(
        void* user, const AnomalyGenerationHandleV1 handle,
        const AnomalyStringViewV1 property_name,
        AnomalyNteEntityBoolPropertyV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot) ||
            property_name.data == nullptr || property_name.size == 0 || property_name.size > 128) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.ActorReflectionCallAvailableLocked()) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "actor reflection reads require the active Game callback domain");
        }
        const auto cache = state.actor_frame_cache;
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "actor frame is unavailable");
        const EntityRecord* entity = FindEntityByHandleLocked(*cache, handle);
        if (entity == nullptr) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale entity handle");

        ReflectedEntityProperty property;
        std::uint8_t field_size{};
        std::uint8_t byte_offset{};
        std::uint8_t byte_mask{};
        std::uint8_t field_mask{};
        if (!state.FindReflectedEntityPropertyLocked(
                *entity, {property_name.data, property_name.size}, property) ||
            property.element_size != 1 ||
            !ReadValue(
                *state.memory,
                property.field + Layout(state.profile, "fboolProperty.fieldSize"), field_size) ||
            !ReadValue(
                *state.memory,
                property.field + Layout(state.profile, "fboolProperty.byteOffset"), byte_offset) ||
            !ReadValue(
                *state.memory,
                property.field + Layout(state.profile, "fboolProperty.byteMask"), byte_mask) ||
            !ReadValue(
                *state.memory,
                property.field + Layout(state.profile, "fboolProperty.fieldMask"), field_mask) ||
            field_size == 0 || byte_offset >= field_size || byte_mask == 0 || field_mask == 0 ||
            (byte_mask & field_mask) != byte_mask) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "bool property was not found");
        }
        std::uintptr_t value_address{};
        std::uint8_t value{};
        if (!AddAddress(
                entity->actor,
                static_cast<std::int64_t>(property.offset) + byte_offset,
                value_address) ||
            !ReadValue(*state.memory, value_address, value)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "bool property is unreadable");
        }
        const auto current_sequence = state.tick_sequence.load(std::memory_order_acquire);
        snapshot->flags = SnapshotFlags(cache->partial, current_sequence, current_sequence);
        snapshot->entity = handle;
        snapshot->sequence = current_sequence;
        snapshot->value = (value & byte_mask) != 0 ? 1U : 0U;
        snapshot->reserved = 0;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorFNameProperty(
        void* user, const AnomalyGenerationHandleV1 handle,
        const AnomalyStringViewV1 property_name,
        char* destination, std::size_t* size) noexcept {
        if (size == nullptr || property_name.data == nullptr || property_name.size == 0 ||
            property_name.size > 128) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.ActorReflectionCallAvailableLocked()) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "actor reflection reads require the active Game callback domain");
        }
        const auto cache = state.actor_frame_cache;
        if (!cache) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "actor frame is unavailable");
        const EntityRecord* entity = FindEntityByHandleLocked(*cache, handle);
        if (entity == nullptr) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale entity handle");

        ReflectedEntityProperty property;
        struct FNameValue {
            std::uint32_t comparison_index{};
            std::uint32_t number{};
        } value;
        std::uintptr_t value_address{};
        if (!state.FindReflectedEntityPropertyLocked(
                *entity, {property_name.data, property_name.size}, property) ||
            property.element_size != static_cast<std::int32_t>(sizeof(value)) ||
            !AddAddress(entity->actor, property.offset, value_address) ||
            !ReadValue(*state.memory, value_address, value)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "FName property was not found");
        }
        std::string name = state.ResolveNameSnapshotLocked(value.comparison_index);
        if (name.empty()) return Status(ANOMALY_STATUS_V1_NOT_FOUND, "FName value is unavailable");
        if (value.number != 0) {
            name += "_" + std::to_string(value.number - 1U);
        }
        return CopyString(name, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL MetricsSnapshot(
        void* user, AnomalyNteSnapshotMetricsV1* metrics) noexcept {
        if (metrics == nullptr || metrics->struct_size < sizeof(*metrics)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticServicesRunning() || !state.MetricsFeatureAvailable()) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE metrics service is unavailable");
        }
        metrics->flags = ANOMALY_NTE_METRICS_V1_VALID;
        metrics->tick_sequence = state.tick_sequence.load(std::memory_order_acquire);
        metrics->session_event_sequence = state.session_event_sequence;
        metrics->snapshot_tick_count = state.snapshot_tick_count;
        metrics->latest_snapshot_cost_micros = state.latest_snapshot_cost_micros;
        metrics->total_snapshot_cost_micros = state.total_snapshot_cost_micros;
        metrics->max_snapshot_cost_micros = state.max_snapshot_cost_micros;
        metrics->player_refresh_count = state.player_refresh_count;
        metrics->player_cache_hit_count = state.player_cache_hit_count;
        metrics->entity_refresh_count = state.entity_refresh_count;
        metrics->entity_cache_hit_count = state.entity_cache_hit_count;
        metrics->entity_page_request_count = state.entity_page_request_count;
        metrics->entity_page_cache_hit_count = state.entity_page_cache_hit_count;
        return Status(ANOMALY_STATUS_V1_OK);
    }
};

struct Ue5NteAdapter::State::SemanticServiceEndpoint final {
    class CallLease final {
    public:
        CallLease() = default;
        CallLease(SemanticServiceEndpoint* endpoint, std::shared_ptr<State> state) noexcept
            : endpoint_(endpoint), state_(std::move(state)) {}
        CallLease(const CallLease&) = delete;
        CallLease& operator=(const CallLease&) = delete;
        CallLease(CallLease&& other) noexcept
            : endpoint_(std::exchange(other.endpoint_, nullptr)), state_(std::move(other.state_)) {}
        CallLease& operator=(CallLease&& other) noexcept {
            if (this == &other) return *this;
            Release();
            endpoint_ = std::exchange(other.endpoint_, nullptr);
            state_ = std::move(other.state_);
            return *this;
        }
        ~CallLease() { Release(); }

        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
        [[nodiscard]] void* User() const noexcept { return state_.get(); }

    private:
        void Release() noexcept {
            if (endpoint_ == nullptr) return;
            endpoint_->ReleaseCall();
            endpoint_ = nullptr;
            state_.reset();
        }

        SemanticServiceEndpoint* endpoint_{};
        std::shared_ptr<State> state_;
    };

    explicit SemanticServiceEndpoint(std::weak_ptr<State> state) : state_(std::move(state)) {
        build_service = {
            sizeof(AnomalyUe5BuildServiceV1), ANOMALY_UE5_BUILD_SERVICE_V1_VERSION,
            this, Ue5BuildIdThunk, Ue5ProfileHashThunk, Ue5FeatureStateThunk};
        framework_service = {
            sizeof(AnomalyUe5FrameworkServiceV1), ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION,
            this, GameThreadIdThunk, TickSequenceThunk, IsGameThreadThunk};
        names_service = {
            sizeof(AnomalyUe5NamesServiceV1), ANOMALY_UE5_NAMES_SERVICE_V1_VERSION,
            this, ResolveNameThunk};
        objects_service = {
            sizeof(AnomalyUe5ObjectsServiceV1), ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION,
            this, ObjectGenerationThunk, ObjectCountThunk, ObjectSnapshotThunk,
            ObjectSnapshotByHandleThunk, FindExactObjectThunk};
        world_service = {
            sizeof(AnomalyUe5WorldServiceV1), ANOMALY_UE5_WORLD_SERVICE_V1_VERSION,
            this, CurrentWorldThunk, WorldSnapshotThunk};
        nte_build_service = {
            sizeof(AnomalyNteBuildServiceV1), ANOMALY_NTE_BUILD_SERVICE_V1_VERSION,
            this, BuildIdThunk, FeatureStateThunk};
        session_service = {
            sizeof(AnomalyNteSessionServiceV1), ANOMALY_NTE_SESSION_SERVICE_V1_VERSION,
            this, SessionSnapshotThunk, SessionNextEventThunk, SessionLatestEventSequenceThunk};
        player_service = {
            sizeof(AnomalyNtePlayerServiceV1), ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION,
            this, PlayerSnapshotThunk, PlayerEspSnapshotThunk, CameraSnapshotThunk};
        player_teleport_service = {
            sizeof(AnomalyNtePlayerTeleportServiceV1),
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION,
            this, TeleportThunk};
        navigation_service = {
            sizeof(AnomalyNteNavigationServiceV1),
            ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION,
            this, MoveToLocationThunk, StopMovementThunk};
        entities_service = {
            sizeof(AnomalyNteEntitiesServiceV1), ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION,
            this, EntityFrameThunk, EntitySnapshotAtThunk, EntityClassNameThunk,
            EntityNameThunk, EntityPageThunk, EntityComponentBoundsThunk,
            EntityBoolPropertyThunk, EntityFNamePropertyThunk};
        actors_service = {
            sizeof(AnomalyNteActorsServiceV1), ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION,
            this, ActorFrameThunk, ActorSnapshotAtThunk, ActorClassNameThunk,
            ActorNameThunk, ActorPageThunk, ActorComponentBoundsThunk,
            ActorBoolPropertyThunk, ActorFNamePropertyThunk};
        metrics_service = {
            sizeof(AnomalyNteMetricsServiceV1), ANOMALY_NTE_METRICS_SERVICE_V1_VERSION,
            this, MetricsSnapshotThunk};
    }

    [[nodiscard]] CallLease Acquire() noexcept {
        if (!gate_.TryEnter()) return {};
        auto state = state_.lock();
        if (!state) {
            gate_.Leave();
            return {};
        }
        return CallLease(this, std::move(state));
    }

    void Close() noexcept {
        gate_.Close();
    }

    [[nodiscard]] bool IsDrained() const noexcept {
        return gate_.IsDrained();
    }

    [[nodiscard]] bool DrainUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        return gate_.DrainUntil(deadline);
    }

    AnomalyNteBuildServiceV1 nte_build_service{};
    AnomalyNteSessionServiceV1 session_service{};
    AnomalyNtePlayerServiceV1 player_service{};
    AnomalyNtePlayerTeleportServiceV1 player_teleport_service{};
    AnomalyNteNavigationServiceV1 navigation_service{};
    AnomalyNteEntitiesServiceV1 entities_service{};
    AnomalyNteActorsServiceV1 actors_service{};
    AnomalyNteMetricsServiceV1 metrics_service{};

private:
    void ReleaseCall() noexcept {
        gate_.Leave();
    }

    static AnomalyStatusV1 StoppedStatus() noexcept {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE semantic services are stopped");
    }

    static AnomalyStatusV1 ANOMALY_CALL Ue5BuildIdThunk(
        void* user, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::BuildId(lease.User(), destination, size) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL Ue5ProfileHashThunk(
        void* user, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ProfileHash(lease.User(), destination, size) : StoppedStatus();
    }

    static std::uint32_t ANOMALY_CALL Ue5FeatureStateThunk(
        void* user, AnomalyStringViewV1 id) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::FeatureStateThunk(lease.User(), id) : ANOMALY_FEATURE_V1_UNAVAILABLE;
    }

    static std::uint32_t ANOMALY_CALL GameThreadIdThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::GameThreadIdThunk(lease.User()) : 0;
    }

    static std::uint64_t ANOMALY_CALL TickSequenceThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::TickSequenceThunk(lease.User()) : 0;
    }

    static int ANOMALY_CALL IsGameThreadThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::IsGameThreadThunk(lease.User()) : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL ResolveNameThunk(
        void* user, std::uint32_t name_id, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ResolveName(lease.User(), name_id, destination, size) : StoppedStatus();
    }

    static std::uint64_t ANOMALY_CALL ObjectGenerationThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ObjectGeneration(lease.User()) : 0;
    }

    static std::uint32_t ANOMALY_CALL ObjectCountThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ObjectCount(lease.User()) : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL ObjectSnapshotThunk(
        void* user, std::uint32_t index, AnomalyUe5ObjectSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ObjectSnapshot(lease.User(), index, snapshot) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ObjectSnapshotByHandleThunk(
        void* user, AnomalyGenerationHandleV1 handle, AnomalyUe5ObjectSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ObjectSnapshotByHandle(lease.User(), handle, snapshot) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL FindExactObjectThunk(
        void* user,
        AnomalyStringViewV1 path,
        AnomalyGenerationHandleV1* handle) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::FindExactObject(lease.User(), path, handle)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL CurrentWorldThunk(
        void* user, AnomalyGenerationHandleV1* handle) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::CurrentWorld(lease.User(), handle) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL WorldSnapshotThunk(
        void* user, AnomalyGenerationHandleV1 handle, AnomalyUe5WorldSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::WorldSnapshot(lease.User(), handle, snapshot) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL BuildIdThunk(
        void* user, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::BuildId(lease.User(), destination, size) : StoppedStatus();
    }

    static std::uint32_t ANOMALY_CALL FeatureStateThunk(
        void* user, AnomalyStringViewV1 id) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::FeatureStateThunk(lease.User(), id) : ANOMALY_FEATURE_V1_UNAVAILABLE;
    }

    static AnomalyStatusV1 ANOMALY_CALL SessionSnapshotThunk(
        void* user, AnomalyNteSessionSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::SessionSnapshot(lease.User(), snapshot) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL SessionNextEventThunk(
        void* user, std::uint64_t after_sequence, AnomalyNteSessionEventV1* event) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::SessionNextEvent(lease.User(), after_sequence, event) : StoppedStatus();
    }

    static std::uint64_t ANOMALY_CALL SessionLatestEventSequenceThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::SessionLatestEventSequence(lease.User()) : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL PlayerSnapshotThunk(
        void* user, AnomalyNtePlayerSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::PlayerSnapshot(lease.User(), snapshot) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL PlayerEspSnapshotThunk(
        void* user, AnomalyNtePlayerEspSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::PlayerEspSnapshot(lease.User(), snapshot) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL CameraSnapshotThunk(
        void* user, AnomalyNteCameraSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::CameraSnapshot(lease.User(), snapshot) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL TeleportThunk(
        void* user,
        const AnomalyNtePlayerTeleportRequestV1* request) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::Teleport(lease.User(), request) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL MoveToLocationThunk(
        void* user,
        const double destination[3]) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::MoveToLocation(lease.User(), destination) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL StopMovementThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::StopMovement(lease.User()) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityFrameThunk(
        void* user, AnomalyNteEntityFrameV1* frame) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::EntityFrame(lease.User(), frame) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL EntitySnapshotAtThunk(
        void* user, std::uint64_t generation, std::uint32_t index,
        AnomalyNteEntitySnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::EntitySnapshotAt(lease.User(), generation, index, snapshot) :
            StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityClassNameThunk(
        void* user, std::uint64_t class_id, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::EntityClassName(lease.User(), class_id, destination, size) :
            StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityNameThunk(
        void* user, std::uint64_t entity_id, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::EntityName(lease.User(), entity_id, destination, size) :
            StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityPageThunk(
        void* user, const AnomalyNteEntityPageRequestV1* request,
        AnomalyNteEntitySnapshotV1* destination, AnomalyNteEntityPageResultV1* result) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::EntityPage(lease.User(), request, destination, result) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityComponentBoundsThunk(
        void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        AnomalyNteEntityComponentBoundsV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::EntityComponentBounds(lease.User(), entity, property_name, snapshot)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityBoolPropertyThunk(
        void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        AnomalyNteEntityBoolPropertyV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::EntityBoolProperty(lease.User(), entity, property_name, snapshot)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL EntityFNamePropertyThunk(
        void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::EntityFNameProperty(
                  lease.User(), entity, property_name, destination, size)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorFrameThunk(
        void* user, AnomalyNteEntityFrameV1* frame) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ActorFrame(lease.User(), frame) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorSnapshotAtThunk(
        void* user, std::uint64_t generation, std::uint32_t index,
        AnomalyNteEntitySnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ActorSnapshotAt(lease.User(), generation, index, snapshot) :
            StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorClassNameThunk(
        void* user, std::uint64_t class_id, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ActorClassName(lease.User(), class_id, destination, size) :
            StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorNameThunk(
        void* user, std::uint64_t actor_id, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ActorName(lease.User(), actor_id, destination, size) :
            StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorPageThunk(
        void* user, const AnomalyNteEntityPageRequestV1* request,
        AnomalyNteEntitySnapshotV1* destination, AnomalyNteEntityPageResultV1* result) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ActorPage(lease.User(), request, destination, result) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorComponentBoundsThunk(
        void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        AnomalyNteEntityComponentBoundsV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::ActorComponentBounds(lease.User(), entity, property_name, snapshot)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorBoolPropertyThunk(
        void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        AnomalyNteEntityBoolPropertyV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::ActorBoolProperty(lease.User(), entity, property_name, snapshot)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActorFNamePropertyThunk(
        void* user, AnomalyGenerationHandleV1 entity, AnomalyStringViewV1 property_name,
        char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::ActorFNameProperty(
                  lease.User(), entity, property_name, destination, size)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL MetricsSnapshotThunk(
        void* user, AnomalyNteSnapshotMetricsV1* metrics) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::MetricsSnapshot(lease.User(), metrics) : StoppedStatus();
    }

    std::weak_ptr<State> state_;
    AdmissionGate gate_;

public:
    AnomalyUe5BuildServiceV1 build_service{};
    AnomalyUe5FrameworkServiceV1 framework_service{};
    AnomalyUe5NamesServiceV1 names_service{};
    AnomalyUe5ObjectsServiceV1 objects_service{};
    AnomalyUe5WorldServiceV1 world_service{};
};

struct Ue5NteAdapter::State::CallbackEndpoint final {
    class CallLease final {
    public:
        CallLease() = default;
        CallLease(
            CallbackEndpoint* endpoint,
            std::shared_ptr<const TickCallback> callback) noexcept
            : endpoint_(endpoint), callback_(std::move(callback)) {}
        CallLease(const CallLease&) = delete;
        CallLease& operator=(const CallLease&) = delete;
        CallLease(CallLease&& other) noexcept
            : endpoint_(std::exchange(other.endpoint_, nullptr)),
              callback_(std::move(other.callback_)) {}
        CallLease& operator=(CallLease&& other) noexcept {
            if (this == &other) return *this;
            Release();
            endpoint_ = std::exchange(other.endpoint_, nullptr);
            callback_ = std::move(other.callback_);
            return *this;
        }
        ~CallLease() { Release(); }

        [[nodiscard]] explicit operator bool() const noexcept {
            return callback_ != nullptr;
        }

        void Invoke(double delta_seconds) const {
            (*callback_)(delta_seconds);
        }

    private:
        void Release() noexcept {
            if (endpoint_ == nullptr) return;
            callback_.reset();
            endpoint_->ReleaseCall();
            endpoint_ = nullptr;
        }

        CallbackEndpoint* endpoint_{};
        std::shared_ptr<const TickCallback> callback_;
    };

    explicit CallbackEndpoint(std::shared_ptr<const TickCallback> callback) noexcept
        : callback_(std::move(callback)) {}

    [[nodiscard]] CallLease Acquire() noexcept {
        if (!gate_.TryEnter()) return {};
        auto callback = callback_.load(std::memory_order_acquire);
        if (!callback) {
            gate_.Leave();
            return {};
        }
        return CallLease(this, std::move(callback));
    }

    void Close() noexcept {
        gate_.Close();
    }

    [[nodiscard]] std::shared_ptr<const TickCallback> Clear() noexcept {
        return callback_.exchange({}, std::memory_order_acq_rel);
    }

    [[nodiscard]] std::shared_ptr<const TickCallback> Set(
        std::shared_ptr<const TickCallback> callback) noexcept {
        return callback_.exchange(std::move(callback), std::memory_order_acq_rel);
    }

    [[nodiscard]] bool IsDrained() const noexcept {
        return gate_.IsDrained();
    }

    [[nodiscard]] bool DrainUntil(
        std::chrono::steady_clock::time_point deadline) noexcept {
        return gate_.DrainUntil(deadline);
    }

private:
    void ReleaseCall() noexcept {
        gate_.Leave();
    }

    AdmissionGate gate_;
    std::atomic<std::shared_ptr<const TickCallback>> callback_;
};

struct Ue5NteAdapter::State::AhudServiceEndpoint final {
    struct Subscription final {
        class CallLease final {
        public:
            CallLease() = default;
            explicit CallLease(std::shared_ptr<Subscription> subscription) noexcept
                : subscription_(std::move(subscription)) {}
            CallLease(const CallLease&) = delete;
            CallLease& operator=(const CallLease&) = delete;
            CallLease(CallLease&&) noexcept = default;
            CallLease& operator=(CallLease&&) noexcept = delete;
            ~CallLease() {
                if (subscription_) subscription_->gate.Leave();
            }

            [[nodiscard]] explicit operator bool() const noexcept {
                return subscription_ != nullptr;
            }

            void Invoke(const AnomalyUe5AhudFrameV1* frame) const {
                subscription_->callback(subscription_->callback_user, frame);
            }

        private:
            std::shared_ptr<Subscription> subscription_;
        };

        Subscription(
            const std::uint64_t id_value,
            const std::uint64_t generation_value,
            const AnomalyUe5AhudDrawCallbackV1 callback_value,
            void* const callback_user_value) noexcept
            : id(id_value),
              generation(generation_value),
              callback(callback_value),
              callback_user(callback_user_value) {}

        [[nodiscard]] CallLease Acquire(
            const std::shared_ptr<Subscription>& self) noexcept {
            return gate.TryEnter() ? CallLease(self) : CallLease{};
        }

        void Close() noexcept {
            gate.Close();
        }

        [[nodiscard]] bool DrainUntil(
            const std::chrono::steady_clock::time_point deadline) noexcept {
            return gate.DrainUntil(deadline);
        }

        [[nodiscard]] bool IsDrained() const noexcept {
            return gate.IsDrained();
        }

        const std::uint64_t id{};
        const std::uint64_t generation{};
        const AnomalyUe5AhudDrawCallbackV1 callback{};
        void* const callback_user{};
        AdmissionGate gate;
    };

    class ServiceCallLease final {
    public:
        ServiceCallLease() = default;
        ServiceCallLease(AhudServiceEndpoint* endpoint, std::shared_ptr<State> state) noexcept
            : endpoint_(endpoint), state_(std::move(state)) {}
        ServiceCallLease(const ServiceCallLease&) = delete;
        ServiceCallLease& operator=(const ServiceCallLease&) = delete;
        ServiceCallLease(ServiceCallLease&& other) noexcept
            : endpoint_(std::exchange(other.endpoint_, nullptr)),
              state_(std::move(other.state_)) {}
        ServiceCallLease& operator=(ServiceCallLease&& other) noexcept {
            if (this == &other) return *this;
            Release();
            endpoint_ = std::exchange(other.endpoint_, nullptr);
            state_ = std::move(other.state_);
            return *this;
        }
        ~ServiceCallLease() { Release(); }

        [[nodiscard]] explicit operator bool() const noexcept {
            return state_ != nullptr;
        }

        [[nodiscard]] const std::shared_ptr<State>& StateOwner() const noexcept {
            return state_;
        }

    private:
        void Release() noexcept {
            if (endpoint_ == nullptr) return;
            state_.reset();
            endpoint_->gate_.Leave();
            endpoint_ = nullptr;
        }

        AhudServiceEndpoint* endpoint_{};
        std::shared_ptr<State> state_;
    };

    AhudServiceEndpoint(std::weak_ptr<State> state, const std::uint64_t generation) noexcept
        : state_(std::move(state)), generation_(generation == 0 ? 1 : generation) {
        service = {
            sizeof(AnomalyUe5AhudServiceV1), ANOMALY_UE5_AHUD_SERVICE_V1_VERSION,
            this, SubscribeThunk, UnsubscribeThunk};
    }

    [[nodiscard]] ServiceCallLease Acquire() noexcept {
        if (!gate_.TryEnter()) return {};
        auto state = state_.lock();
        if (!state) {
            gate_.Leave();
            return {};
        }
        return ServiceCallLease(this, std::move(state));
    }

    void Close() noexcept {
        gate_.Close();
        callback_gate_.Close();
        const auto state = state_.lock();
        {
            std::scoped_lock lock(mutex_);
            closed_ = true;
            for (const auto& [id, subscription] : subscriptions_) {
                static_cast<void>(id);
                subscription->Close();
            }
            if (state) {
                state->ahud_demand.store(false, std::memory_order_release);
            }
        }
    }

    [[nodiscard]] bool IsDrained() const noexcept {
        if (!gate_.IsDrained() || !callback_gate_.IsDrained()) return false;
        std::scoped_lock lock(mutex_);
        return std::ranges::all_of(subscriptions_, [](const auto& entry) {
            return entry.second->IsDrained();
        });
    }

    [[nodiscard]] bool DrainUntil(
        const std::chrono::steady_clock::time_point deadline) noexcept {
        if (!gate_.DrainUntil(deadline) || !callback_gate_.DrainUntil(deadline)) {
            return false;
        }
        std::vector<std::shared_ptr<Subscription>> subscriptions;
        try {
            std::scoped_lock lock(mutex_);
            subscriptions.reserve(subscriptions_.size());
            for (const auto& [id, subscription] : subscriptions_) {
                static_cast<void>(id);
                subscriptions.push_back(subscription);
            }
        } catch (...) {
            return false;
        }
        return std::ranges::all_of(subscriptions, [deadline](const auto& subscription) {
            return subscription->DrainUntil(deadline);
        });
    }

    void Dispatch(const void* state, const AnomalyUe5AhudFrameV1* frame) noexcept {
        std::vector<std::shared_ptr<Subscription>> subscriptions;
        try {
            {
                std::scoped_lock lock(mutex_);
                if (closed_) return;
                subscriptions.reserve(subscriptions_.size());
                for (const auto& [id, subscription] : subscriptions_) {
                    static_cast<void>(id);
                    subscriptions.push_back(subscription);
                }
            }
            for (const auto& subscription : subscriptions) {
                if (!callback_gate_.TryEnter()) return;
                const auto leave_callback_gate = std::unique_ptr<AdmissionGate, void(*)(AdmissionGate*)>(
                    &callback_gate_, [](AdmissionGate* gate) { gate->Leave(); });
                auto lease = subscription->Acquire(subscription);
                if (!lease) continue;
                const ActiveAhudCallbackScope callback_scope(state, subscription.get());
                try {
                    lease.Invoke(frame);
                } catch (...) {
                }
            }
        } catch (...) {
        }
    }

    AnomalyUe5AhudServiceV1 service{};

private:
    [[nodiscard]] AnomalyStatusV1 Subscribe(
        const std::shared_ptr<State>& state,
        const AnomalyUe5AhudDrawCallbackV1 callback,
        void* const callback_user,
        AnomalyGenerationHandleV1* const handle) noexcept {
        if (handle == nullptr || callback == nullptr) {
            return Status(
                ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                "AHUD subscription callback and handle are required");
        }
        *handle = {};
        {
            std::scoped_lock state_lock(state->mutex);
            if (!state->started.load(std::memory_order_acquire) ||
                !state->AhudFeatureAvailable()) {
                return StoppedStatus();
            }
        }

        try {
            std::shared_ptr<Subscription> subscription;
            {
                std::scoped_lock lock(mutex_);
                if (closed_) return StoppedStatus();
                if (next_id_ == (std::numeric_limits<std::uint64_t>::max)()) {
                    return Status(
                        ANOMALY_STATUS_V1_UNAVAILABLE,
                        "AHUD subscription handle space is exhausted");
                }
                const std::uint64_t id = ++next_id_;
                subscription = std::make_shared<Subscription>(
                    id, generation_, callback, callback_user);
                subscriptions_.emplace(id, subscription);
                *handle = {id, generation_};
                state->ahud_demand.store(true, std::memory_order_release);
            }
            return Status(ANOMALY_STATUS_V1_OK);
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "AHUD subscription allocation failed");
        }
    }

    [[nodiscard]] AnomalyStatusV1 Unsubscribe(
        const std::shared_ptr<State>& state,
        const AnomalyGenerationHandleV1 handle) noexcept {
        std::shared_ptr<Subscription> subscription;
        bool has_subscribers{};
        {
            std::scoped_lock lock(mutex_);
            const auto found = subscriptions_.find(handle.id);
            if (handle.id == 0 || handle.generation != generation_ ||
                found == subscriptions_.end()) {
                return Status(
                    ANOMALY_STATUS_V1_NOT_FOUND,
                    "AHUD subscription handle is not found");
            }
            subscription = std::move(found->second);
            subscriptions_.erase(found);
            has_subscribers = !closed_ && !subscriptions_.empty();
            state->ahud_demand.store(has_subscribers, std::memory_order_release);
        }
        subscription->Close();
        if (g_active_ahud_subscription_state.Get() == subscription.get()) {
            return Status(ANOMALY_STATUS_V1_OK);
        }
        return subscription->DrainUntil(
                   std::chrono::steady_clock::time_point::max())
            ? Status(ANOMALY_STATUS_V1_OK)
            : Status(ANOMALY_STATUS_V1_TIMEOUT, "AHUD subscription drain timed out");
    }

    static AnomalyStatusV1 StoppedStatus() noexcept {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "AHUD service is stopped");
    }

    static AnomalyStatusV1 ANOMALY_CALL SubscribeThunk(
        void* const user,
        const AnomalyUe5AhudDrawCallbackV1 callback,
        void* const callback_user,
        AnomalyGenerationHandleV1* const handle) noexcept {
        auto* const endpoint = static_cast<AhudServiceEndpoint*>(user);
        auto lease = endpoint->Acquire();
        return lease
            ? endpoint->Subscribe(lease.StateOwner(), callback, callback_user, handle)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL UnsubscribeThunk(
        void* const user,
        const AnomalyGenerationHandleV1 handle) noexcept {
        auto* const endpoint = static_cast<AhudServiceEndpoint*>(user);
        auto lease = endpoint->Acquire();
        return lease
            ? endpoint->Unsubscribe(lease.StateOwner(), handle)
            : StoppedStatus();
    }

    std::weak_ptr<State> state_;
    const std::uint64_t generation_{};
    AdmissionGate gate_;
    AdmissionGate callback_gate_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Subscription>> subscriptions_;
    std::uint64_t next_id_{};
    bool closed_{};
};

void Ue5NteAdapter::State::DispatchAhudFrame(
    const std::uintptr_t object,
    const std::uintptr_t function,
    void* const parameters,
    const ProcessEventInvoker& actor_process_event) noexcept {
    if (object == 0 || function == 0 || parameters == nullptr ||
        !actor_process_event ||
        !started.load(std::memory_order_acquire) ||
        GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire)) {
        return;
    }
    const auto binding = ahud_binding.load(std::memory_order_acquire);
    if (!binding) return;
    const auto& receive = binding->functions[AhudIndex(AhudFunctionKind::ReceiveDrawHud)];
    if (receive.function != function || receive.parms_size == 0) return;

    const auto parameter_bytes = std::span(
        static_cast<const std::uint8_t*>(parameters), receive.parms_size);
    std::int32_t viewport_width{};
    std::int32_t viewport_height{};
    constexpr std::int32_t kMaximumViewportDimension = 1 << 20;
    if (!ReadAhudValue(receive, 0, viewport_width, parameter_bytes) ||
        !ReadAhudValue(receive, 1, viewport_height, parameter_bytes) ||
        viewport_width <= 0 || viewport_height <= 0 ||
        viewport_width > kMaximumViewportDimension ||
        viewport_height > kMaximumViewportDimension) {
        return;
    }

    const auto endpoint = ahud_endpoint.load(std::memory_order_acquire);
    if (!endpoint) return;
    ahud_frame_count.fetch_add(1, std::memory_order_relaxed);
    AhudFrameCallContext context{
        object,
        binding.get(),
        &actor_process_event,
        &ahud_process_event_call_count};
    const AnomalyUe5AhudFrameV1 frame{
        sizeof(AnomalyUe5AhudFrameV1),
        ANOMALY_UE5_AHUD_FRAME_V1_NONE,
        &context,
        static_cast<std::uint32_t>(viewport_width),
        static_cast<std::uint32_t>(viewport_height),
        AhudProject,
        AhudMeasureText,
        AhudDrawText,
        AhudDrawLine,
        AhudDrawRect};
    endpoint->Dispatch(this, &frame);
}

bool Ue5NteAdapter::State::PublishAvailableServices(const std::weak_ptr<State>& self) {
    const auto endpoint = semantic_endpoint.load(std::memory_order_acquire);
    if (!endpoint) return false;
    const auto semantic_lifetime = std::static_pointer_cast<const void>(endpoint);
    const std::weak_ptr<SemanticServiceEndpoint> observer_endpoint = endpoint;
    if (!PublishIfMissing(
            ANOMALY_UE5_BUILD_SERVICE_V1_ID,
            ANOMALY_UE5_BUILD_SERVICE_V1_VERSION,
            &endpoint->build_service,
            {}, semantic_lifetime) ||
        !PublishIfMissing(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID,
            ANOMALY_NTE_BUILD_SERVICE_V1_VERSION,
            &endpoint->nte_build_service,
            {}, semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && resolution.FeatureAvailable("ue5.framework") &&
        !PublishIfMissing(
            ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID,
            ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION,
            &endpoint->framework_service,
            {}, semantic_lifetime)) {
        return false;
    }
    const auto current_ahud_endpoint = ahud_endpoint.load(std::memory_order_acquire);
    if (AhudFeatureAvailable() &&
        (!current_ahud_endpoint ||
         !PublishIfMissing(
             ANOMALY_UE5_AHUD_SERVICE_V1_ID,
             ANOMALY_UE5_AHUD_SERVICE_V1_VERSION,
             &current_ahud_endpoint->service,
             {},
             std::static_pointer_cast<const void>(current_ahud_endpoint)))) {
        return false;
    }
    if (resolution.FeatureAvailable("ue5.names") &&
        !PublishIfMissing(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID,
            ANOMALY_UE5_NAMES_SERVICE_V1_VERSION,
            &endpoint->names_service,
            {}, semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && resolution.FeatureAvailable("ue5.objects") &&
        !PublishIfMissing(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
            ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION,
            &endpoint->objects_service,
            {}, semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && resolution.FeatureAvailable("ue5.world") &&
        !PublishIfMissing(
            ANOMALY_UE5_WORLD_SERVICE_V1_ID,
            ANOMALY_UE5_WORLD_SERVICE_V1_VERSION,
            &endpoint->world_service,
            {}, semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && SemanticFeatureAvailable("nte.session") &&
        !PublishIfMissing(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID,
            ANOMALY_NTE_SESSION_SERVICE_V1_VERSION,
            &endpoint->session_service,
            {}, semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && MetricsFeatureAvailable() &&
        !PublishIfMissing(
            ANOMALY_NTE_METRICS_SERVICE_V1_ID,
            ANOMALY_NTE_METRICS_SERVICE_V1_VERSION,
            &endpoint->metrics_service,
            {}, semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && SemanticFeatureAvailable("nte.player") &&
        !PublishIfMissing(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION,
            &endpoint->player_service,
            [self, observer_endpoint] {
                const auto locked = self.lock();
                const auto observed = observer_endpoint.lock();
                if (!locked || !observed ||
                    locked->semantic_endpoint.load(std::memory_order_acquire) != observed) {
                    return;
                }
                locked->player_demand.store(true, std::memory_order_release);
            },
            semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && SemanticFeatureAvailable("nte.player-teleport") &&
        !PublishIfMissing(
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION,
            &endpoint->player_teleport_service,
            [self, observer_endpoint] {
                const auto locked = self.lock();
                const auto observed = observer_endpoint.lock();
                if (!locked || !observed ||
                    locked->semantic_endpoint.load(std::memory_order_acquire) != observed) {
                    return;
                }
                locked->player_demand.store(true, std::memory_order_release);
            },
            semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && SemanticFeatureAvailable("nte.navigation") &&
        !PublishIfMissing(
            ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID,
            ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION,
            &endpoint->navigation_service,
            [self, observer_endpoint] {
                const auto locked = self.lock();
                const auto observed = observer_endpoint.lock();
                if (!locked || !observed ||
                    locked->semantic_endpoint.load(std::memory_order_acquire) != observed) {
                    return;
                }
                locked->player_demand.store(true, std::memory_order_release);
                locked->navigation_demand.store(true, std::memory_order_release);
            },
            semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && SemanticFeatureAvailable("nte.entities") &&
        !PublishIfMissing(
            ANOMALY_NTE_ENTITIES_SERVICE_V1_ID,
            ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION,
            &endpoint->entities_service,
            [self, observer_endpoint] {
                const auto locked = self.lock();
                const auto observed = observer_endpoint.lock();
                if (!locked || !observed ||
                    locked->semantic_endpoint.load(std::memory_order_acquire) != observed) {
                    return;
                }
                locked->player_demand.store(true, std::memory_order_release);
                locked->entity_demand.store(true, std::memory_order_release);
            },
            semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && NteActorsLayoutAvailable() &&
        !PublishIfMissing(
            ANOMALY_NTE_ACTORS_SERVICE_V1_ID,
            ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION,
            &endpoint->actors_service,
            {}, semantic_lifetime)) {
        return false;
    }
    return true;
}

Ue5NteAdapter::Ue5NteAdapter(
    BuildFingerprint fingerprint,
    BuildProfile profile,
    ProfileResolutionSnapshot resolution,
    std::shared_ptr<const SymbolMemory> memory,
    AdapterServiceRegistry& services,
    NteSnapshotSamplingOptions sampling,
    FeatureLayoutValidatorRegistry feature_layout_validators,
    ProcessEventInvoker process_event_invoker,
    ObjectLookup object_lookup,
    std::shared_ptr<NteNavigationInputPolicy> navigation_input_policy)
    : state_(std::make_shared<State>()) {
    state_->fingerprint = std::move(fingerprint);
    state_->profile = std::move(profile);
    state_->resolution = std::move(resolution);
    state_->memory = std::move(memory);
    state_->feature_layout_validators = std::move(feature_layout_validators);
    state_->services = &services;
    state_->process_event_invoker = std::move(process_event_invoker);
    state_->object_lookup = std::move(object_lookup);
    state_->navigation_input_policy = std::move(navigation_input_policy);
    state_->sampling.player_tick_interval = (std::max)(1U, sampling.player_tick_interval);
    state_->sampling.entity_tick_interval = (std::max)(1U, sampling.entity_tick_interval);
}

Ue5NteAdapter::~Ue5NteAdapter() {
    static_cast<void>(Stop(std::chrono::milliseconds::zero()));
}

bool Ue5NteAdapter::Start(bool framework_hook_ready, bool ahud_hook_ready) {
    const auto state = state_;
    std::shared_ptr<State::SemanticServiceEndpoint> retired_semantic_endpoint;
    std::shared_ptr<State::CallbackEndpoint> retired_callback_endpoint;
    std::shared_ptr<State::AhudServiceEndpoint> retired_ahud_endpoint;
    std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex);
    if (state->stopping.load(std::memory_order_acquire) ||
        state->semantic_endpoint.load(std::memory_order_acquire) ||
        state->callback_endpoint.load(std::memory_order_acquire) ||
        state->ahud_endpoint.load(std::memory_order_acquire) ||
        (state->draining_semantic_endpoint &&
            !state->draining_semantic_endpoint->IsDrained()) ||
        (state->draining_callback_endpoint &&
            !state->draining_callback_endpoint->IsDrained()) ||
        (state->draining_ahud_endpoint &&
            !state->draining_ahud_endpoint->IsDrained())) {
        return false;
    }
    std::unique_lock<std::timed_mutex> lock(state->mutex);
    if (state->started.load(std::memory_order_acquire)) {
        return false;
    }
    retired_semantic_endpoint = std::move(state->draining_semantic_endpoint);
    retired_callback_endpoint = std::move(state->draining_callback_endpoint);
    retired_ahud_endpoint = std::move(state->draining_ahud_endpoint);
    const auto lifecycle_generation =
        state->lifecycle_epoch.fetch_add(1, std::memory_order_acq_rel) + 1U;
    state->ResetForStartLocked();
    state->framework_hook_ready = framework_hook_ready;
    state->ahud_hook_ready = ahud_hook_ready;
    if (framework_hook_ready) {
        static_cast<void>(state->EnsureNavigationInputPolicyLocked());
    }
    state->semantic_endpoint.store(
        std::make_shared<State::SemanticServiceEndpoint>(state),
        std::memory_order_release);
    state->callback_endpoint.store(
        std::make_shared<State::CallbackEndpoint>(
            state->configured_tick_callback.load(std::memory_order_acquire)),
        std::memory_order_release);
    state->ahud_endpoint.store(
        std::make_shared<State::AhudServiceEndpoint>(state, lifecycle_generation),
        std::memory_order_release);
    const auto first_service = state->PublishedCount();
    if (state->PublishAvailableServices(state)) {
        state->started.store(true, std::memory_order_release);
        return true;
    }
    const auto failed_endpoint = state->semantic_endpoint.exchange(
        std::shared_ptr<State::SemanticServiceEndpoint>{}, std::memory_order_acq_rel);
    const auto failed_callback_endpoint = state->callback_endpoint.exchange(
        std::shared_ptr<State::CallbackEndpoint>{}, std::memory_order_acq_rel);
    const auto failed_ahud_endpoint = state->ahud_endpoint.exchange(
        std::shared_ptr<State::AhudServiceEndpoint>{}, std::memory_order_acq_rel);
    if (failed_endpoint) failed_endpoint->Close();
    if (failed_callback_endpoint) failed_callback_endpoint->Close();
    if (failed_ahud_endpoint) failed_ahud_endpoint->Close();
    state->RevokePublishedFrom(first_service);
    if (state->navigation_input_policy != nullptr) {
        static_cast<void>(state->navigation_input_policy->Stop());
    }
    return false;
}

bool Ue5NteAdapter::Stop(std::chrono::milliseconds timeout) noexcept {
    const auto state = state_;
    const auto bounded_timeout =
        (std::max)(timeout, std::chrono::milliseconds::zero());
    const auto deadline = bounded_timeout == std::chrono::milliseconds::max()
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + bounded_timeout;
    const bool called_by_active_tick_callback =
        g_active_tick_callback_state.Get() == state.get();
    const bool called_by_active_ahud_callback =
        g_active_ahud_callback_state.Get() == state.get();
    const bool called_by_active_callback =
        called_by_active_tick_callback || called_by_active_ahud_callback;
    std::shared_ptr<State::SemanticServiceEndpoint> semantic_endpoint;
    std::shared_ptr<State::CallbackEndpoint> callback_endpoint;
    std::shared_ptr<State::AhudServiceEndpoint> ahud_endpoint;
    std::shared_ptr<const TickCallback> detached_endpoint_callback;
    std::shared_ptr<const TickCallback> detached_configured_callback;

    // A callback can safely initiate its own transition, but it must not wait
    // behind another stopper that is already draining that callback.
    if (called_by_active_callback && state->stopping.load(std::memory_order_acquire)) {
        return false;
    }
    std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex, std::defer_lock);
    const bool lifecycle_locked = called_by_active_callback
        ? lifecycle_lock.try_lock()
        : LockUntil(lifecycle_lock, deadline);
    if (!lifecycle_locked) return false;

    const bool was_started = state->started.exchange(false, std::memory_order_acq_rel);
    if (was_started) state->lifecycle_epoch.fetch_add(1, std::memory_order_acq_rel);
    state->stopping.store(true, std::memory_order_release);
    semantic_endpoint = state->semantic_endpoint.exchange(
        std::shared_ptr<State::SemanticServiceEndpoint>{}, std::memory_order_acq_rel);
    if (semantic_endpoint) {
        semantic_endpoint->Close();
        if (!state->draining_semantic_endpoint) {
            state->draining_semantic_endpoint = semantic_endpoint;
        }
    } else {
        semantic_endpoint = state->draining_semantic_endpoint;
        if (semantic_endpoint) semantic_endpoint->Close();
    }
    callback_endpoint = state->callback_endpoint.exchange(
        std::shared_ptr<State::CallbackEndpoint>{}, std::memory_order_acq_rel);
    if (callback_endpoint) {
        callback_endpoint->Close();
        detached_endpoint_callback = callback_endpoint->Clear();
        if (!state->draining_callback_endpoint) {
            state->draining_callback_endpoint = callback_endpoint;
        }
    } else {
        callback_endpoint = state->draining_callback_endpoint;
        if (callback_endpoint) {
            callback_endpoint->Close();
            detached_endpoint_callback = callback_endpoint->Clear();
        }
    }
    ahud_endpoint = state->ahud_endpoint.exchange(
        std::shared_ptr<State::AhudServiceEndpoint>{}, std::memory_order_acq_rel);
    if (ahud_endpoint) {
        ahud_endpoint->Close();
        if (!state->draining_ahud_endpoint) {
            state->draining_ahud_endpoint = ahud_endpoint;
        }
    } else {
        ahud_endpoint = state->draining_ahud_endpoint;
        if (ahud_endpoint) ahud_endpoint->Close();
    }
    detached_configured_callback = state->configured_tick_callback.exchange(
        {}, std::memory_order_acq_rel);
    lifecycle_lock.unlock();

    // Callback-owned state can execute arbitrary destruction. Release it only
    // after the lifecycle lock no longer protects the stopping generation.
    detached_endpoint_callback.reset();
    detached_configured_callback.reset();

    if (!state->RevokePublishedFromUntil(0, deadline)) return false;

    std::unique_lock<std::timed_mutex> state_lock(state->mutex, std::defer_lock);
    if (!LockUntil(state_lock, deadline)) return false;

    state->ClearSemanticStateForStopLocked();
    state_lock.unlock();

    if (called_by_active_callback) return false;
    if (ahud_endpoint && !ahud_endpoint->DrainUntil(deadline)) return false;
    if (callback_endpoint && !callback_endpoint->DrainUntil(deadline)) return false;
    if (semantic_endpoint && !semantic_endpoint->DrainUntil(deadline)) return false;

    const auto navigation_policy_timeout = [&]() noexcept {
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            return std::chrono::milliseconds::max();
        }
        const auto now = std::chrono::steady_clock::now();
        return now >= deadline
            ? std::chrono::milliseconds::zero()
            : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    }();
    if (state->navigation_input_policy != nullptr &&
        !state->navigation_input_policy->Stop(navigation_policy_timeout)) {
        return false;
    }

    std::unique_lock<std::timed_mutex> final_lifecycle_lock(
        state->lifecycle_mutex, std::defer_lock);
    if (!LockUntil(final_lifecycle_lock, deadline)) return false;
    std::shared_ptr<State::SemanticServiceEndpoint> retired_semantic_endpoint;
    std::shared_ptr<State::CallbackEndpoint> retired_callback_endpoint;
    std::shared_ptr<State::AhudServiceEndpoint> retired_ahud_endpoint;
    if (state->draining_semantic_endpoint == semantic_endpoint) {
        retired_semantic_endpoint = std::move(state->draining_semantic_endpoint);
    }
    if (state->draining_callback_endpoint == callback_endpoint) {
        retired_callback_endpoint = std::move(state->draining_callback_endpoint);
    }
    if (state->draining_ahud_endpoint == ahud_endpoint) {
        retired_ahud_endpoint = std::move(state->draining_ahud_endpoint);
    }
    if (!state->semantic_endpoint.load(std::memory_order_acquire) &&
        !state->callback_endpoint.load(std::memory_order_acquire) &&
        !state->ahud_endpoint.load(std::memory_order_acquire)) {
        state->stopping.store(false, std::memory_order_release);
    }
    final_lifecycle_lock.unlock();
    return true;
}

void Ue5NteAdapter::SetTickCallback(TickCallback callback) {
    const auto state = state_;
    std::shared_ptr<const TickCallback> replacement = MakeTickCallback(std::move(callback));
    std::shared_ptr<const TickCallback> retired_configured;
    std::shared_ptr<const TickCallback> retired_endpoint;
    {
        // Serialize publication with Start/Stop/Clear so a setter that overlaps
        // Stop cannot repopulate the configured callback after Stop detached it.
        std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex);
        if (state->stopping.load(std::memory_order_acquire)) return;
        retired_configured = state->configured_tick_callback.exchange(
            replacement, std::memory_order_acq_rel);
        const auto endpoint = state->callback_endpoint.load(std::memory_order_acquire);
        if (endpoint) {
            retired_endpoint = endpoint->Set(std::move(replacement));
        }
    }
}

bool Ue5NteAdapter::ClearTickCallback(std::chrono::milliseconds timeout) noexcept {
    const auto state = state_;
    const bool called_by_active_tick_callback =
        g_active_tick_callback_state.Get() == state.get();
    const auto bounded_timeout =
        (std::max)(timeout, std::chrono::milliseconds::zero());
    const auto deadline = bounded_timeout == std::chrono::milliseconds::max()
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + bounded_timeout;
    std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex, std::defer_lock);
    const bool lifecycle_locked = called_by_active_tick_callback
        ? lifecycle_lock.try_lock()
        : LockUntil(lifecycle_lock, deadline);
    if (!lifecycle_locked) return false;
    const auto endpoint = state->callback_endpoint.load(std::memory_order_acquire);
    auto retired_endpoint_callback = endpoint ? endpoint->Clear() : nullptr;
    auto retired_configured_callback = state->configured_tick_callback.exchange(
        {}, std::memory_order_acq_rel);
    lifecycle_lock.unlock();
    if (called_by_active_tick_callback) return false;
    return !endpoint || endpoint->DrainUntil(deadline);
}

void Ue5NteAdapter::OnGameTick(double delta_seconds) noexcept {
    const auto state = state_;
    const auto entry_epoch = state->lifecycle_epoch.load(std::memory_order_acquire);
    if (!state->started.load(std::memory_order_acquire)) return;
    const DWORD current = GetCurrentThreadId();
    {
        std::scoped_lock lock(state->mutex);
        if (!state->started.load(std::memory_order_acquire) ||
            state->lifecycle_epoch.load(std::memory_order_acquire) != entry_epoch) {
            return;
        }
        DWORD expected{};
        if (!state->game_thread_id.compare_exchange_strong(
                expected, current, std::memory_order_acq_rel, std::memory_order_acquire) &&
            expected != current) {
            state->rejected_thread_ticks.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto sampling_started = std::chrono::steady_clock::now();
        const auto sequence =
            state->tick_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        state->RefreshDeferredResolution(sequence, state);
        state->RefreshWorld(sequence);
        state->RefreshObjects();
        state->RefreshTeleportBindingLocked();
        if (state->navigation_demand.load(std::memory_order_acquire)) {
            static_cast<void>(state->EnsureNavigationInputPolicyLocked());
            state->RefreshNavigationBindingLocked();
        }
        state->RefreshAhudBindingLocked();
        const bool entity_requested = state->entity_demand.load(std::memory_order_acquire);
        const bool player_requested = state->player_demand.load(std::memory_order_acquire);
        const bool entity_due = entity_requested && State::SamplingDue(
            sequence, state->entity_attempt_sequence, state->sampling.entity_tick_interval);
        const bool player_due = player_requested && State::SamplingDue(
            sequence, state->player_attempt_sequence, state->sampling.player_tick_interval);
        if (entity_due || player_due) {
            state->RefreshPlayer(sequence);
            ++state->player_refresh_count;
        } else if (player_requested) {
            ++state->player_cache_hit_count;
        }
        if (entity_due) {
            // One observed frame request is satisfied by one refresh. A later
            // frame() call requests another sample.
            static_cast<void>(state->entity_demand.exchange(false, std::memory_order_acq_rel));
            state->RefreshEntities(sequence);
            ++state->entity_refresh_count;
        } else if (entity_requested) {
            ++state->entity_cache_hit_count;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - sampling_started).count();
        const auto elapsed_micros = static_cast<std::uint64_t>(
            (std::max)(std::int64_t{0}, static_cast<std::int64_t>(elapsed)));
        ++state->snapshot_tick_count;
        state->latest_snapshot_cost_micros = elapsed_micros;
        state->total_snapshot_cost_micros += elapsed_micros;
        state->max_snapshot_cost_micros = (std::max)(
            state->max_snapshot_cost_micros, elapsed_micros);
    }
    const auto endpoint = state->callback_endpoint.load(std::memory_order_acquire);
    if (!endpoint) return;
    auto callback = endpoint->Acquire();
    if (!callback || !state->started.load(std::memory_order_acquire) ||
        state->lifecycle_epoch.load(std::memory_order_acquire) != entry_epoch) {
        return;
    }
    const ActiveTickCallbackScope callback_scope(state.get());
    try {
        callback.Invoke(delta_seconds);
    } catch (...) {
    }
}

void Ue5NteAdapter::OnProcessEvent(
    const std::uintptr_t object,
    const std::uintptr_t function,
    void* const parameters,
    const ProcessEventInvoker& actor_process_event) noexcept {
    const auto state = state_;
    state->DispatchAhudFrame(object, function, parameters, actor_process_event);
}

bool Ue5NteAdapter::Started() const noexcept {
    const auto state = state_;
    return state->started.load(std::memory_order_acquire);
}

DWORD Ue5NteAdapter::GameThreadId() const noexcept {
    const auto state = state_;
    return state->game_thread_id.load(std::memory_order_acquire);
}

std::uint64_t Ue5NteAdapter::TickSequence() const noexcept {
    const auto state = state_;
    return state->tick_sequence.load(std::memory_order_acquire);
}

std::uint64_t Ue5NteAdapter::RejectedThreadTicks() const noexcept {
    const auto state = state_;
    return state->rejected_thread_ticks.load(std::memory_order_acquire);
}

bool Ue5NteAdapter::AhudBindingReady() const noexcept {
    const auto state = state_;
    return state->ahud_binding.load(std::memory_order_acquire) != nullptr;
}

std::uint64_t Ue5NteAdapter::AhudFrameCount() const noexcept {
    const auto state = state_;
    return state->ahud_frame_count.load(std::memory_order_acquire);
}

std::uint64_t Ue5NteAdapter::AhudProcessEventCallCount() const noexcept {
    const auto state = state_;
    return state->ahud_process_event_call_count.load(std::memory_order_acquire);
}

ProfileResolutionSnapshot Ue5NteAdapter::Resolution() const {
    const auto state = state_;
    std::scoped_lock lock(state->mutex);
    return state->resolution;
}

}  // namespace anomaly
