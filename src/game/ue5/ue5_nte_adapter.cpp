#include "anomaly/ue5_nte_adapter.hpp"
#include "anomaly/nte_damage_capture.hpp"
#include "anomaly/nte_monster_names.hpp"
#include "anomaly/ue5_ftext.hpp"
#include "anomaly/thread_local_value.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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

[[nodiscard]] bool ReadableRange(
    const SymbolMemory& memory,
    const std::uintptr_t address,
    const std::size_t size) noexcept {
    if (address < 0x10000U || size == 0 ||
        size > (std::numeric_limits<std::uintptr_t>::max)() - address) {
        return false;
    }
    const auto region = memory.Query(address);
    if (!region || region->state != MEM_COMMIT ||
        (region->protection & PAGE_GUARD) != 0 ||
        (region->protection & 0xFFU) == PAGE_NOACCESS) {
        return false;
    }
    const auto region_end = region->base > (std::numeric_limits<std::uintptr_t>::max)() -
            region->size
        ? (std::numeric_limits<std::uintptr_t>::max)()
        : region->base + region->size;
    return address >= region->base && size <= region_end - address;
}

[[nodiscard]] bool InvokeProcessEventGuarded(
    const Ue5NteAdapter::ProcessEventInvoker& invoker,
    const std::uintptr_t receiver,
    const std::uintptr_t function,
    void* const parameters,
    const std::size_t parameter_size,
    std::uint32_t* const fault_code = nullptr) noexcept {
    // UE5 x64 objects and code in the active process are above the 4 GiB
    // boundary. Rejecting low values also blocks stale/truncated FText data
    // before it can become a virtual call target.
    if (!invoker || receiver < 0x100000000ULL || function < 0x100000000ULL) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        return invoker(receiver, function, parameters, parameter_size);
    } __except ((fault_code != nullptr ? *fault_code = GetExceptionCode() : 0),
                EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    try {
        return invoker(receiver, function, parameters, parameter_size);
    } catch (...) {
        return false;
    }
#endif
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
    struct ProcessEventServiceEndpoint;

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
    std::atomic_bool pickup_demand{};
    static constexpr std::size_t kSessionEventCapacity = 64;
    static constexpr std::uint32_t kEntityPageCapacity = 256;
    static constexpr std::size_t kMaximumPickupParameterSize = 4096;
    static constexpr std::size_t kMaximumPickupChoices = 128;
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
    enum class NteFunctionKind : std::size_t {
        GetAbilitySystemComponent,
        GetMainCharacterId,
        GetNpcMainCharacterId,
        GetHp,
        GetHpMax,
        GetIsDead,
        GetAttackTarget,
        GetShieldHealth,
        GetActiveEffectTimeRemainingAndDuration,
        ActivateAbilityByClass,
        ShowDamageFloaties,
        MulticastShowMonsterDamageInfo,
        ClientShowPlayerDamageInfo,
        SetDamageInfo,
        OnActiveGameplayEffectAdded,
        OnAnyGameplayEffectRemoved,
        AddBuffControl,
        RemoveFromBuffControl,
        AddBufferManagerBuffControl,
        AddHeadUpBattleMarkBuffControl,
        RemoveHeadUpBattleMarkBuffControl,
        AddMonsterBufferControl,
        GetMonsterStaticData,
        CurrentDamageIsCrit,
        Count,
    };
    static constexpr std::size_t kNteFunctionCount =
        static_cast<std::size_t>(NteFunctionKind::Count);
    static constexpr std::size_t kMaximumNteFunctionParameters = 6;
    struct NteFunctionParameterSpec {
        std::string_view name;
        std::string_view type;
        std::int32_t element_size{};
        bool return_value{};
    };
    struct NteFunctionSpec {
        NteFunctionKind kind{};
        std::string_view name;
        std::string_view outer;
        std::uint16_t parms_size{};
        std::span<const NteFunctionParameterSpec> parameters;
    };
    struct NteFunctionBinding {
        std::uintptr_t function{};
        std::uintptr_t outer_class{};
        std::uintptr_t meta_class{};
        std::uint16_t parms_size{};
        std::array<std::uint16_t, kMaximumNteFunctionParameters> offsets{};
        std::array<ReflectedBoolParameter, kMaximumNteFunctionParameters> bool_parameters{};
    };
    struct CombatSkillDiscovery {
        std::array<std::optional<NteFunctionBinding>, kNteFunctionCount> functions{};
        std::uintptr_t ability_system_class{};
        std::uintptr_t gameplay_ability_class{};
        std::uintptr_t ability_spawn_actor_class{};
        std::uint64_t object_generation{};
        bool direct_lookup_succeeded{};
        std::uint64_t direct_lookup_next_sequence{};
        std::uint32_t direct_lookup_retry_interval{300};
        bool damage_event_layout_valid{};
        bool skill_layout_valid{};
        bool cooldown_layout_valid{};
        bool combat_event_bindings_attempted{};
    } combat_skill_discovery;
    static constexpr std::size_t kDamageEventCapacity = 512;
    static constexpr std::uint64_t kDamageSourceBase = 0x8000000000000000ULL;
    struct DamageRecord {
        AnomalyNteDamageEventV1 event{};
    };
    std::array<DamageRecord, kDamageEventCapacity> damage_events{};
    std::size_t damage_event_start{};
    std::size_t damage_event_count{};
    std::uint64_t damage_event_sequence{};
    std::uint64_t damage_world_sequence_base{};
    std::uint64_t damage_dropped_count{};
    std::atomic<std::uint64_t> damage_native_call_count{};
    mutable std::atomic<std::uint64_t> reflection_fault_count{};
    mutable std::atomic<std::uintptr_t> last_reflection_fault_function{};
    mutable std::atomic<std::uint32_t> last_reflection_fault_code{};
    std::uint64_t damage_captured_event_count{};
    std::atomic<std::uint64_t> damage_capture_drop_count{};
    std::uint64_t damage_attacker_resolution_failure_count{};
    std::uint64_t damage_victim_resolution_failure_count{};
    std::uint64_t damage_source_resolution_failure_count{};
    std::uint64_t damage_next_source_id{kDamageSourceBase};
    std::unordered_map<std::uint64_t, std::uint64_t> damage_source_object_ids;
    std::unordered_map<std::uint64_t, std::uintptr_t> damage_source_objects;
    std::unordered_map<std::uint64_t, std::string> damage_source_names;
    struct DamageSourceAbilityMapping {
        std::uintptr_t class_pointer{};
        AnomalyGenerationHandleV1 class_handle{};
        bool name_pending{};
        std::uint8_t name_attempts{};
        std::uint64_t next_name_sequence{};
    };
    std::unordered_map<std::uint64_t, DamageSourceAbilityMapping>
        damage_source_ability_classes;
    struct PendingDamageSourceMapping {
        std::uint64_t source_id{};
        std::uintptr_t saved_skill_cdo{};
        std::int32_t active_spec_handle{};
        bool attacker_is_player{};
        std::uint8_t attempts{};
    };
    std::deque<PendingDamageSourceMapping> pending_damage_source_mappings;
    std::unordered_set<std::uint64_t> observed_damage_source_mappings;
    std::uint64_t saved_trigger_skill_mapping_count{};
    std::uint64_t trigger_ability_handle_mapping_count{};
    std::uint64_t damage_source_mapping_failure_count{};
    std::uint64_t delayed_damage_name_completion_count{};
    std::unordered_map<std::uint64_t, bool> damage_critical_tag_cache;
    std::unordered_map<std::uint64_t, std::string> damage_participant_paths;
    static constexpr std::size_t kCombatEventCapacity = 512;
    struct CombatEventRecord {
        AnomalyNteCombatEventV1 event{};
    };
    std::array<CombatEventRecord, kCombatEventCapacity> combat_events{};
    std::size_t combat_event_start{};
    std::size_t combat_event_count{};
    std::uint64_t combat_event_sequence{};
    std::unordered_map<std::uint64_t, std::string> combat_event_names;
    std::unordered_map<std::uint64_t, std::uintptr_t> combat_event_objects;
    std::deque<std::uint64_t> pending_combat_event_names;
    std::unordered_set<std::uint64_t> queued_combat_event_names;
    std::unordered_set<std::uint64_t> failed_combat_event_names;
    std::unordered_map<std::uint64_t, std::string> combat_participant_names;
    struct PendingCombatParticipantName {
        AnomalyGenerationHandleV1 participant{};
        std::uintptr_t object{};
        std::array<std::uint64_t, 3> keys{};
        std::array<std::string, 3> key_texts{};
        std::uint8_t key_count{};
        std::uint8_t attempts{};
        std::uint8_t scene_retry_attempts{};
        bool player_participant{};
        std::uint64_t config_id_key{};
        bool has_config_id{};
        std::uint64_t next_retry_sequence{};
    };
    std::deque<PendingCombatParticipantName> pending_combat_participant_names;
    std::unordered_set<std::uint64_t> queued_combat_participant_names;
    std::unordered_set<std::uint64_t> failed_combat_participant_names;
    std::uint64_t monster_static_data_resolution_calls{};
    std::uint64_t monster_static_data_resolution_successes{};
    mutable std::uint64_t string_table_binding_attempts{};
    mutable std::uint64_t string_table_binding_failures{};
    mutable std::uint64_t string_table_call_count{};
    mutable std::uint64_t string_table_success_count{};
    mutable std::uint64_t string_table_thread_rejections{};
    mutable std::uint8_t string_table_binding_failure_code{};
    mutable std::string string_table_last_key;
    mutable std::string string_table_last_value;
    std::uint64_t participant_last_handle{};
    bool participant_last_player{};
    std::string participant_last_class_text;
    std::string participant_last_config_text;
    std::unordered_map<std::uint64_t, std::string> ability_display_names;
    std::unordered_map<std::uint64_t, std::uint8_t> ability_display_name_attempts;
    // FName -> localized FText values decoded only for keys requested by a
    // combat event, participant, or skill.
    std::unordered_map<std::uint64_t, std::string> localized_names_by_fname;
    std::unordered_map<std::string, std::string> localized_names_by_key;
    std::uint64_t display_table_generation{};
    bool display_table_scan_complete{};
    std::uint8_t display_table_loaded_mask{};
    std::uintptr_t display_game_data{};
    std::uint64_t display_game_data_generation{};
    struct SparseMapView {
        std::uintptr_t map{};
        std::uintptr_t data{};
        std::uintptr_t flags_data{};
        std::int32_t num{};
        std::int32_t num_free{};
        std::int32_t max{};
        std::int32_t flags_num{};
        std::int32_t flags_max{};
        std::int64_t stride{};
        std::int64_t row_offset{};
        std::vector<std::uint32_t> flags;
    };
    struct DisplayTableIndex {
        std::uintptr_t table{};
        std::int64_t text_offset{-1};
        std::unordered_map<std::uint64_t, std::uintptr_t> rows_by_fname;
        std::unordered_map<std::string, std::uintptr_t> rows_by_key;
    };
    struct DamageSkillIndex {
        std::uintptr_t table{};
        std::unordered_map<std::uint64_t, std::uint64_t> ability_by_effect_fname;
        std::unordered_map<std::string, std::uint64_t> ability_by_effect_key;
    } damage_skill_index;

    struct StringTableEntryBinding {
        std::uintptr_t function{};
        std::uintptr_t receiver{};
        std::uint64_t table_id{};
        std::uint16_t parms_size{};
        std::uint16_t table_offset{};
        std::uint16_t key_offset{};
        std::uint16_t return_offset{};
        std::uint64_t object_generation{};
        bool attempted{};
        std::array<std::uint8_t, 64> parameters{};
    };
    mutable StringTableEntryBinding string_table_entry;
    mutable StringTableEntryBinding abyss_string_table_entry;
    struct RegisteredStringTablesBinding {
        std::uintptr_t function{};
        std::uintptr_t receiver{};
        std::uint16_t parms_size{};
        std::uint16_t return_offset{};
        std::uint64_t object_generation{};
        bool attempted{};
        std::array<std::uint8_t, 64> parameters{};
    };
    mutable RegisteredStringTablesBinding registered_string_tables;
    std::array<DisplayTableIndex, 4> display_table_indexes{};
    static constexpr std::size_t kSceneMonsterTableCapacity = 5;
    std::array<DisplayTableIndex, kSceneMonsterTableCapacity>
        scene_monster_table_indexes{};
    std::size_t scene_monster_table_count{};
    std::uint64_t scene_monster_table_generation{};
    bool scene_monster_table_scan_complete{};
    static constexpr std::size_t kCombatCaptureQueueCapacity = 128;
    // The ProcessEvent tap only needs the reflected fields below. Keeping the
    // queue payload bounded to the largest useful value avoids copying the
    // 0x298-byte FGameplayEffectSpec for every buff notification.
    static constexpr std::size_t kCombatCapturePayloadBytes =
        (std::max)(std::size_t{96}, sizeof(NteCharacterDamageCapture));
    enum class CombatCaptureKind : std::uint8_t {
        CharacterDamage,
        Damage,
        BuffAdd,
        BuffRemove,
    };
    struct PendingCombatCapture {
        CombatCaptureKind kind{};
        std::uint16_t payload_size{};
        std::uint16_t info_offset{};
        std::uint64_t tick_sequence{};
        std::array<std::uint8_t, kCombatCapturePayloadBytes> payload{};
    };
    std::array<PendingCombatCapture, kCombatCaptureQueueCapacity> combat_capture_queue{};
    std::atomic<std::uint32_t> combat_capture_write{};
    std::atomic<std::uint32_t> combat_capture_read{};
    std::atomic<std::uint64_t> combat_capture_drop_count{};
    struct CombatCaptureBindings {
        std::uintptr_t damage{};
        std::uintptr_t monster_damage{};
        std::uintptr_t player_damage_queue{};
        std::uint16_t player_damage_queue_offset{};
        std::uintptr_t damage_widget{};
        std::uint16_t damage_widget_info_offset{};
        struct Buff {
            std::uintptr_t function{};
            std::uint16_t parms_size{};
            std::uint16_t object_offset{0xFFFFU};
            std::uint16_t definition_offset{0xFFFFU};
            std::uint16_t duration_offset{0xFFFFU};
            std::uint16_t stack_offset{0xFFFFU};
            std::uint16_t is_add_offset{0xFFFFU};
        };
        std::array<Buff, 8> buffs{};
        std::uint16_t damage_info_offset{};
        std::uint16_t damage_value_offset{0xFFFFU};
        std::uint16_t damage_source_offset{0xFFFFU};
        std::uint16_t damage_tags_offset{0xFFFFU};
    };
    // Slots are generation-local and never freed. Publish a fully populated
    // slot with one acquire/release pointer load in the ProcessEvent hook.
    std::array<CombatCaptureBindings, 2> combat_capture_binding_slots{};
    std::atomic<const CombatCaptureBindings*> combat_capture_bindings{};
    std::uint8_t combat_capture_binding_slot{};
    std::atomic<std::uint64_t> damage_floaties_call_count{};
    std::atomic<std::uint64_t> monster_damage_call_count{};
    std::atomic<std::uint64_t> player_damage_queue_call_count{};
    std::atomic<std::uint64_t> damage_widget_call_count{};
    std::atomic<std::uint64_t> buff_call_count{};
    std::atomic<std::uint64_t> crit_query_call_count{};
    std::atomic<std::uint64_t> crit_query_success_count{};
    std::atomic<std::uint64_t> crit_true_count{};
    std::atomic_bool combat_demand{};
    std::atomic_bool display_name_demand{};
    std::uint64_t combat_attempt_sequence{};
    std::uint64_t combat_sample_sequence{};
    AnomalyGenerationHandleV1 combat_character{};
    AnomalyGenerationHandleV1 combat_target{};
    double combat_hp{};
    double combat_max_hp{};
    double combat_shield{};
    bool combat_dead{};
    bool combat_available{};
    bool combat_partial{};
    std::uint32_t combat_refresh_failure{};
    struct ActiveEffectRecord {
        std::int32_t replication_id{};
        std::int32_t replication_key{};
        std::uintptr_t definition{};
        std::uint64_t name_id{};
        float duration_seconds{};
        std::int32_t stack_count{};
    };
    std::vector<ActiveEffectRecord> active_effects;
    std::uintptr_t active_effect_ability_system{};
    std::int32_t active_effect_array_key{};
    bool active_effects_initialized{};
    struct SkillRecord {
        AnomalyGenerationHandleV1 handle{};
        AnomalyGenerationHandleV1 character{};
        AnomalyGenerationHandleV1 ability_class{};
        std::uint64_t sequence{};
        std::uint32_t flags{};
        std::int32_t level{};
        std::int32_t input_id{};
        std::int32_t spec_handle{};
        float cooldown_remaining_seconds{};
        float cooldown_duration_seconds{};
        std::uintptr_t ability{};
        std::uintptr_t ability_class_pointer{};
        std::string ability_path;
    };
    std::vector<SkillRecord> skills;
    std::atomic_bool skill_demand{};
    std::uint64_t skill_generation{};
    std::uint64_t skill_next_id{1};
    std::uint64_t skill_attempt_sequence{};
    std::uint64_t skill_sample_sequence{};
    std::uintptr_t skill_ability_system{};
    AnomalyGenerationHandleV1 skill_character{};
    bool skills_available{};
    bool skills_partial{};
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
    struct MapLandmarkBinding {
        std::uintptr_t function{};
        std::uint16_t parms_size{};
        std::uint16_t teleport_id_offset{};
        std::uint16_t transfer_mode_offset{};
        std::uint64_t object_generation{};
        bool available{};
    } map_landmark_binding;
    struct MapLandmarkRecord {
        std::string teleport_id;
        std::string world;
        std::array<double, 3> world_position{};
        std::array<double, 3> destination{};
        std::uint32_t point_type{};
        std::int32_t floor{};
        bool destination_overridden{};
    };
    struct MapLandmarkCatalog {
        std::vector<MapLandmarkRecord> entries;
        std::uint64_t sequence{};
        std::uint64_t object_generation{};
    };
    std::shared_ptr<const MapLandmarkCatalog> map_landmark_catalog;
    std::uint64_t map_landmark_catalog_sequence{};
    std::uint64_t map_landmark_next_refresh_sequence{};
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
    struct PendingPickupRequest {
        double radius{};
        std::uint32_t maximum_items{};
        std::uint32_t attempts{};
        bool queued{};
    } pickup_request;
    struct PickupConfirmationCandidate {
        std::uintptr_t actor{};
        std::uintptr_t controller{};
        std::uintptr_t can_try_function{};
        std::uint32_t object_index{};
        std::uint32_t object_serial{};
        std::uint64_t entity_sequence{};
        std::int32_t interact_index{};
        std::uint8_t baseline{};
    };
    struct PickupConfirmation {
        std::vector<PickupConfirmationCandidate> candidates;
        std::chrono::steady_clock::time_point next_check{};
        std::chrono::steady_clock::time_point deadline{};
    } pickup_confirmation;
    AnomalyNtePickupSnapshotV1 pickup_snapshot{sizeof(AnomalyNtePickupSnapshotV1)};
    std::uint64_t pickup_sequence{};
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
    // FName 的 comparison index 在进程内稳定，同一 name_id 永远对应同一个字符串。
    // 全关卡 actor 扫描会为每个 actor 解析一次实体名（数千次），逐次解码宽字符名
    // 的代价要一秒以上；记忆化后只有首次扫描需要真正解码。失败结果不缓存，
    // 因为那通常意味着布局尚未就绪，之后的扫描应当重试。
    mutable std::unordered_map<std::uint32_t, std::string> name_snapshot_cache;
    std::shared_ptr<const EntityFrameCache> entity_frame_cache;
    std::shared_ptr<const EntityFrameCache> previous_entity_frame_cache;
    std::uint64_t entity_generation{};
    std::uint64_t entity_attempt_sequence{};
    std::shared_ptr<const EntityFrameCache> actor_frame_cache;
    std::uint64_t actor_generation{};
    std::uint64_t actor_world_generation{};
    // 全部关卡 actor 快照按 tick 节流重扫：只在 World 变化时刷新会让快照永久冻结，
    // 死掉的 actor 留在列表里、新生成的 actor 永远不可见（大世界全程是同一个 World）。
    std::uint64_t actor_attempt_sequence{};
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
    bool process_event_hook_ready{};
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
    std::atomic<std::shared_ptr<ProcessEventServiceEndpoint>> process_event_endpoint;
    std::shared_ptr<ProcessEventServiceEndpoint> draining_process_event_endpoint;

    const ResolvedSymbol* Symbol(std::string_view id) const noexcept {
        return resolution.FindSymbol(id);
    }

    [[nodiscard]] static constexpr std::size_t AhudIndex(
        const AhudFunctionKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    [[nodiscard]] static constexpr std::size_t NteIndex(
        const NteFunctionKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    [[nodiscard]] static NteFunctionSpec NteSpec(
        const NteFunctionKind kind) noexcept {
        static constexpr std::array get_ability_system{
            NteFunctionParameterSpec{"ReturnValue", "ObjectProperty", 8, true}};
        static constexpr std::array get_name{
            NteFunctionParameterSpec{"ReturnValue", "NameProperty", 8, true}};
        static constexpr std::array get_float{
            NteFunctionParameterSpec{"ReturnValue", "FloatProperty", 4, true}};
        static constexpr std::array get_hp_max{
            NteFunctionParameterSpec{"bIsFixHPMax", "BoolProperty", 1, false},
            NteFunctionParameterSpec{"ReturnValue", "FloatProperty", 4, true}};
        static constexpr std::array get_bool{
            NteFunctionParameterSpec{"ReturnValue", "BoolProperty", 1, true}};
        static constexpr std::array get_object{
            NteFunctionParameterSpec{"ReturnValue", "ObjectProperty", 8, true}};
        static constexpr std::array activate{
            NteFunctionParameterSpec{
                "InAbilityToActivate", "ClassProperty", 8, false},
            NteFunctionParameterSpec{"ReturnValue", "BoolProperty", 1, true}};
        static constexpr std::array cooldown{
            NteFunctionParameterSpec{
                "GameplayEffect", "ClassProperty", 8, false},
            NteFunctionParameterSpec{"TimeRemaining", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"CooldownDuration", "FloatProperty", 4, false}};
        static constexpr std::array damage_text{
            NteFunctionParameterSpec{"InDamageInfo", "StructProperty", 72, false}};
        static constexpr std::array monster_damage_text{
            NteFunctionParameterSpec{"InDamageTextInfo", "StructProperty", 72, false}};
        static constexpr std::array damage_text_queue{
            NteFunctionParameterSpec{
                "InDamageTextInfoQueue", "StructProperty", 16, false}};
        static constexpr std::array damage_widget{
            NteFunctionParameterSpec{"InDamageFloatiesForm", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"InDamageInfo", "StructProperty", 72, false},
            NteFunctionParameterSpec{"bNeedSetTransform", "BoolProperty", 1, false}};
        static constexpr std::array active_effect_added{
            NteFunctionParameterSpec{"Source", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"SpecApplied", "StructProperty", 664, false},
            NteFunctionParameterSpec{"ActiveHandle", "StructProperty", 8, false}};
        static constexpr std::array active_effect_removed{
            NteFunctionParameterSpec{"ActiveEffect", "StructProperty", 864, false}};
        static constexpr std::array buff_info_add{
            NteFunctionParameterSpec{"SpecApplied", "StructProperty", 664, false},
            NteFunctionParameterSpec{"fCurDuration", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"nCurStackCount", "IntProperty", 4, false}};
        static constexpr std::array buff_info_remove{
            NteFunctionParameterSpec{"SpecApplied", "StructProperty", 664, false}};
        static constexpr std::array buffer_manager_add{
            NteFunctionParameterSpec{"ActiveHandle", "StructProperty", 8, false},
            NteFunctionParameterSpec{"SpecApplied", "StructProperty", 664, false},
            NteFunctionParameterSpec{"bIsAdd", "BoolProperty", 1, false},
            NteFunctionParameterSpec{"fCurDuration", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"nCurStackCount", "IntProperty", 4, false},
            NteFunctionParameterSpec{"bInPlayOpenAnim", "BoolProperty", 1, false}};
        static constexpr std::array head_up_add{
            NteFunctionParameterSpec{"BuffClass", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"fCurDuration", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"nCurStackCount", "IntProperty", 4, false}};
        static constexpr std::array head_up_remove{
            NteFunctionParameterSpec{"BuffClass", "ObjectProperty", 8, false}};
        static constexpr std::array monster_buffer_add{
            NteFunctionParameterSpec{"ActiveHandle", "StructProperty", 8, false},
            NteFunctionParameterSpec{"BuffClass", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"bIsAdd", "BoolProperty", 1, false},
            NteFunctionParameterSpec{"fCurDuration", "FloatProperty", 4, false},
            NteFunctionParameterSpec{"nCurStackCount", "IntProperty", 4, false},
        };
        static constexpr std::array get_monster_static_data{
            NteFunctionParameterSpec{"WorldContextObject", "ObjectProperty", 8, false},
            NteFunctionParameterSpec{"ConfigID", "NameProperty", 8, false},
            NteFunctionParameterSpec{"InOutMonsterStaticData", "StructProperty", 280, false},
            NteFunctionParameterSpec{"ReturnValue", "BoolProperty", 1, true}};
        switch (kind) {
        case NteFunctionKind::GetAbilitySystemComponent:
            return {kind, "K2_GetAbilitySystemComponent", "HTAbilityCharacter", 8,
                get_ability_system};
        case NteFunctionKind::GetMainCharacterId:
            return {kind, "GetMainCharacterID", "HTPlayerCharacter", 8, get_name};
        case NteFunctionKind::GetNpcMainCharacterId:
            return {kind, "GetMainCharacterID", "HTPlayerNPCCharacter", 8, get_name};
        case NteFunctionKind::GetHp:
            return {kind, "GetHP", "HTAbilityCharacter", 4, get_float};
        case NteFunctionKind::GetHpMax:
            return {kind, "GetHPMax", "HTAbilityCharacter", 8, get_hp_max};
        case NteFunctionKind::GetIsDead:
            return {kind, "GetIsDead", "HTAbilityCharacter", 1, get_bool};
        case NteFunctionKind::GetAttackTarget:
            return {kind, "GetAttackTarget", "HTAbilityCharacter", 8, get_object};
        case NteFunctionKind::GetShieldHealth:
            return {kind, "GetShieldHealth", "HTAttributeComponent", 4, get_float};
        case NteFunctionKind::GetActiveEffectTimeRemainingAndDuration:
            return {kind, "GetActiveEffectTimeRemainingAndDuration",
                "HTAbilitySystemComponent", 16, cooldown};
        case NteFunctionKind::ActivateAbilityByClass:
            return {kind, "HTTryActivateAbilityByClass", "HTAbilitySystemComponent", 9,
                activate};
        case NteFunctionKind::ShowDamageFloaties:
            return {kind, "ShowDamageFloaties", "HTUI_DamageFloatiesForm", 72, damage_text};
        case NteFunctionKind::MulticastShowMonsterDamageInfo:
            return {kind, "MulticastShowMonsterDamageInfo", "HTMonsterCharacter", 72,
                monster_damage_text};
        case NteFunctionKind::ClientShowPlayerDamageInfo:
            return {kind, "ClientShowPlayerDamageInfo", "HTPlayerCharacter", 16,
                damage_text_queue};
        case NteFunctionKind::SetDamageInfo:
            return {kind, "SetDamageInfo", "HTUI_DamageFloatiesWidget", 81,
                damage_widget};
        case NteFunctionKind::OnActiveGameplayEffectAdded:
            return {kind, "BP_OnActiveGameplayEffectAdded", "HTUI_AbilityCustomBase", 680,
                active_effect_added};
        case NteFunctionKind::OnAnyGameplayEffectRemoved:
            return {kind, "BP_OnAnyGameplayEffectRemoved", "HTUI_AbilityCustomBase", 864,
                active_effect_removed};
        case NteFunctionKind::AddBuffControl:
            return {kind, "AddBuffControl", "HTUI_BuffInfoManager", 672, buff_info_add};
        case NteFunctionKind::RemoveFromBuffControl:
            return {kind, "RemoveFromBuffControl", "HTUI_BuffInfoManager", 664, buff_info_remove};
        case NteFunctionKind::AddBufferManagerBuffControl:
            return {kind, "AddBuffControl", "HTUI_BufferManager", 685,
                buffer_manager_add};
        case NteFunctionKind::AddHeadUpBattleMarkBuffControl:
            return {kind, "AddBuffControl", "HTUI_HeadUpBattleMark", 16, head_up_add};
        case NteFunctionKind::RemoveHeadUpBattleMarkBuffControl:
            return {kind, "RemoveFromBuffControl", "HTUI_HeadUpBattleMark", 8, head_up_remove};
        case NteFunctionKind::AddMonsterBufferControl:
            return {kind, "AddBuffControl", "HTUI_MonsterBufferManager", 28,
                monster_buffer_add};
        case NteFunctionKind::GetMonsterStaticData:
            return {kind, "K2_GetMonsterStaticData", "HTSceneSolelyDataAsset", 297,
                get_monster_static_data};
        case NteFunctionKind::CurrentDamageIsCrit:
            return {kind, "CurrentDamageIsCrit", "HTAttributeComponent", 1, get_bool};
        case NteFunctionKind::Count: break;
        }
        return {};
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

    [[nodiscard]] bool NteCombatProfileAvailable() const noexcept {
        return static_cast<bool>(process_event_invoker) && framework_hook_ready &&
            resolution.FeatureAvailable("nte.combat") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
                "object.internalIndex", "object.class", "object.outer",
                "uclass.classDefaultObject",
                "ustruct.superStruct", "ustruct.propertyLink", "ufunction.numParms",
                "ufunction.parmsSize", "ufunction.returnValueOffset",
                "ffield.class",
                "ffield.name", "ffieldClass.name", "fproperty.arrayDim",
                "fproperty.elementSize", "fproperty.offsetInternal",
                "fproperty.propertyLinkNext", "fstructProperty.struct",
                "fobjectProperty.propertyClass", "farrayProperty.inner",
                "fboolProperty.fieldSize",
                "fboolProperty.byteOffset", "fboolProperty.byteMask",
                "fboolProperty.fieldMask", "damageEvent.size", "damageEvent.damage",
                "damageEvent.damageGEDef", "damageEvent.damageTags",
                "controller.playerState", "playerState.roleName",
                "abilityCharacter.characterConfigId",
                "weakObject.index", "weakObject.serial",
                "abilitySpawnActor.triggerAbilityHandle",
                 "abilitySpawnActor.savedTriggerSkillCDO",
                 "abilitySystem.tryActivateAbilityStack", "gameplayEffectSpec.size",
                 "gameplayEffectSpec.def", "gameplayEffectSpec.duration",
                 "gameplayEffectSpec.stackCount", "activeGameplayEffect.size",
                 "activeGameplayEffect.spec", "activeGameplayEffect.replicationId",
                 "activeGameplayEffect.replicationKey", "abilitySystem.activeGameplayEffects",
                 "activeGameplayEffects.size", "activeGameplayEffects.arrayReplicationKey",
                 "activeGameplayEffects.items", "buffs.maxCount",
                "damageTextInfo.displayDamage", "damageTextInfo.damageType",
                "damageTextInfo.critical", "damageTextInfo.headHit",
                "damageTextInfo.weakUnbalance", "damageTextInfo.attacker",
                "damageTextInfo.victim", "damageTextInfo.combatStatistics",
                "damageTextInfo.basicDamage", "damageTextInfo.finalDamage",
                 "damageTextInfo.displayType", "damageTextInfo.reactionType",
                 "damageTextInfo.reactionDisplayType",
                 "gameplayEffect.uiData", "gameplayEffectUIData.description",
                 "buff.specDef",
                 "buff.duration", "buff.stackCount", "ftext.textData",
                 "ftextData.textSource", "fstring.data", "fstring.count",
                 "fstring.capacity", "abilityCharacter.abilitySystemComponent",
                 "gameData.abilityDataAsset",
                 "abilityData.skillDamageDataTable", "skillDamage.gaName",
                 "gameData.characterDataTable", "gameData.gameplayAbilityTipsDataTable",
                 "gameData.gameplayEffectTipsDataTable", "gameplayAbilityTips.name",
                 "gameplayAbilityTips.gameplayAbility", "gameplayEffectTips.name",
                 "gameplayEffectTips.geParamName",
                 "monsterData.textName", "dataTable.rowMap", "dataTable.rowMapData",
                 "dataTable.rowMapNum", "dataTable.rowMapNumFree", "dataTable.rowMapMax",
                 "dataTable.rowMapElementStride", "dataTable.rowMapRowOffset",
                 "dataTable.rowMapInlineFlags", "dataTable.rowMapFlagsData",
                 "dataTable.rowMapFlagsNum", "dataTable.rowMapFlagsMax"}) &&
            FeatureDeclaresDependency(profile, "nte.combat", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.combat", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.combat", "ue5.objects") &&
            FeatureDeclaresDependency(
                profile, "nte.combat", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.combat", "nte-combat-reflection-v1");
    }

    [[nodiscard]] bool NteSkillsProfileAvailable() const noexcept {
        return static_cast<bool>(process_event_invoker) && framework_hook_ready &&
            resolution.FeatureAvailable("nte.skills") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            NtePlayerLayoutAvailable() && LayoutKeysAvailable(profile, {
                "object.internalIndex", "object.class", "object.outer",
                "ustruct.superStruct", "ustruct.propertyLink", "ufunction.numParms",
                "ufunction.parmsSize", "ufunction.returnValueOffset", "ffield.class",
                "ffield.name", "ffieldClass.name", "fproperty.arrayDim",
                "fproperty.elementSize", "fproperty.offsetInternal",
                "fproperty.propertyLinkNext", "fstructProperty.struct",
                "fobjectProperty.propertyClass", "farrayProperty.inner",
                "fclassProperty.metaClass",
                "fboolProperty.fieldSize", "fboolProperty.byteOffset",
                "fboolProperty.byteMask", "fboolProperty.fieldMask", "tarray.data",
                "tarray.num", "tarray.max", "abilitySystem.activatableAbilities",
                "abilitySpecContainer.items", "abilitySpec.stride",
                "abilitySpec.handle", "abilitySpec.ability", "abilitySpec.level",
                "abilitySpec.inputId", "abilitySpec.activeCount",
                "abilitySpec.stateBits", "ability.cooldownGameplayEffectClass",
                "skills.maxCount"}) &&
            FeatureDeclaresDependency(profile, "nte.skills", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.skills", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.skills", "ue5.objects") &&
            FeatureDeclaresDependency(profile, "nte.skills", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.skills", "nte-skills-layout-v1");
    }

    [[nodiscard]] bool NteSkillInvocationProfileAvailable() const noexcept {
        return NteSkillsProfileAvailable() &&
            resolution.FeatureAvailable("nte.skill-invocation") &&
            FeatureDeclaresDependency(
                profile, "nte.skill-invocation", "nte.skills") &&
            FeatureDeclaresDependency(
                profile, "nte.skill-invocation", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.skill-invocation", "nte-skill-invocation-v1");
    }

    [[nodiscard]] bool NteFunctionReady(const NteFunctionKind kind) const noexcept {
        return combat_skill_discovery.functions[NteIndex(kind)].has_value();
    }

    [[nodiscard]] bool NteCombatReflectionReady() const noexcept {
        return combat_skill_discovery.damage_event_layout_valid &&
            NteFunctionReady(NteFunctionKind::GetAbilitySystemComponent) &&
            NteFunctionReady(NteFunctionKind::GetHp) &&
            NteFunctionReady(NteFunctionKind::GetHpMax) &&
            NteFunctionReady(NteFunctionKind::GetIsDead) &&
            NteFunctionReady(NteFunctionKind::GetAttackTarget) &&
            NteFunctionReady(NteFunctionKind::GetShieldHealth);
    }

    [[nodiscard]] bool NteSkillsReflectionReady() const noexcept {
        return combat_skill_discovery.skill_layout_valid &&
            NteFunctionReady(NteFunctionKind::GetAbilitySystemComponent) &&
            combat_skill_discovery.gameplay_ability_class != 0;
    }

    [[nodiscard]] bool NteSkillInvocationReflectionReady() const noexcept {
        return NteSkillsReflectionReady() &&
            NteFunctionReady(NteFunctionKind::ActivateAbilityByClass) &&
            combat_skill_discovery.gameplay_ability_class != 0;
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

    [[nodiscard]] bool NteMapLandmarksAvailable() const noexcept {
        const auto* const process_event = resolution.FindSymbol("ue5.ProcessEvent");
        return static_cast<bool>(process_event_invoker) && ObjectFindAvailable() &&
            process_event != nullptr && process_event->Available() &&
            resolution.FeatureAvailable("nte.map-landmarks") &&
            SemanticFeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) && LayoutKeysAvailable(profile, {
                "object.internalIndex",
                "object.class",
                "object.nameOffset",
                "object.outer",
                "controller.playerState",
                "uclass.classDefaultObject",
                "ustruct.propertyLink",
                "ufunction.numParms",
                "ufunction.parmsSize",
                "ufunction.returnValueOffset",
                "ffield.name",
                "fproperty.arrayDim",
                "fproperty.elementSize",
                "fproperty.offsetInternal",
                "fproperty.propertyLinkNext",
                "gameData.teleportPointDataTable",
                "dataTable.rowStruct",
                "dataTable.rowMap",
                "dataTable.rowMapData",
                "dataTable.rowMapNum",
                "dataTable.rowMapNumFree",
                "dataTable.rowMapMax",
                "dataTable.rowMapElementStride",
                "dataTable.rowMapRowOffset",
                "dataTable.rowMapInlineFlags",
                "dataTable.rowMapFlagsData",
                "dataTable.rowMapFlagsNum",
                "dataTable.rowMapFlagsMax",
                "dataTable.maxRows",
                "teleportPoint.belongsLevel",
                "teleportPoint.floor",
                "teleportPoint.transformTranslation",
                "teleportPoint.type",
                "teleportPoint.canTeleport",
                "teleportPoint.overrideTransform",
                "teleportPoint.overrideTranslation"}) &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", "ue5.objects") &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", "ue5.object-find") &&
            FeatureDeclaresDependency(profile, "nte.map-landmarks", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.map-landmarks", "nte-map-landmarks-layout-v1");
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

    [[nodiscard]] bool NtePickupAvailable() const noexcept {
        const auto* const process_event = resolution.FindSymbol(kUe5ProcessEventSymbol);
        return static_cast<bool>(process_event_invoker) && process_event != nullptr &&
            process_event->Available() && resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
            resolution.FeatureAvailable("nte.pickup") &&
            resolution.FeatureAvailable("nte.player") &&
            resolution.FeatureAvailable("nte.entities") &&
            resolution.FeatureAvailable("ue5.names") &&
            resolution.FeatureAvailable("ue5.objects") &&
            NtePlayerLayoutAvailable() && NteEntityReflectionLayoutAvailable() &&
            LayoutKeysAvailable(profile, {
                "object.internalIndex", "object.class", "object.nameOffset", "object.outer",
                "ustruct.superStruct", "ustruct.children", "ufield.next",
                "ufunction.flags", "ufunction.nativeFlag", "ufunction.numParms",
                "ufunction.parmsSize", "pickup.actor.interactFinish",
                "pickup.trigger.numParms", "pickup.trigger.parmsSize",
                "pickup.trigger.actor", "pickup.trigger.index",
                "pickup.trigger.onlyClientSide", "pickup.canTry.controller",
                "pickup.canTry.numParms", "pickup.canTry.parmsSize",
                "pickup.canTry.index", "pickup.canTry.returnValue",
                "pickup.entries.numParms", "pickup.entries.parmsSize",
                "pickup.entries.controller", "pickup.entries.array",
                "pickup.entries.maximumChoices",
                "pickup.interactEntryStride", "pickup.interactEntryIndex"}) &&
            FeatureDeclaresSymbol(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventSymbol) &&
            FeatureDeclaresLayoutValidator(
                profile, kUe5ProcessEventFeature, kUe5ProcessEventAbiValidator) &&
            FeatureDeclaresDependency(profile, "nte.pickup", "nte.player") &&
            FeatureDeclaresDependency(profile, "nte.pickup", "nte.entities") &&
            FeatureDeclaresDependency(profile, "nte.pickup", "ue5.names") &&
            FeatureDeclaresDependency(profile, "nte.pickup", "ue5.objects") &&
            FeatureDeclaresDependency(
                profile, "nte.pickup", kUe5ProcessEventFeature) &&
            FeatureDeclaresLayoutValidator(
                profile, "nte.pickup", "nte-pickup-layout-v1");
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
        return framework_hook_ready && ahud_hook_ready &&
            resolution.FeatureAvailable("ue5.ahud") &&
            resolution.FeatureAvailable("ue5.functions") &&
            resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
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
                profile, "ue5.ahud", kUe5ProcessEventFeature) &&
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
        if (feature == "nte.map-landmarks") return NteMapLandmarksAvailable();
        if (feature == "nte.navigation") return NteNavigationAvailable();
        if (feature == "nte.pickup") return NtePickupAvailable();
        if (feature == "nte.entities") {
            return NteEntitiesLayoutAvailable();
        }
        if (feature == "nte.combat") {
            return NteCombatProfileAvailable() && NteCombatReflectionReady();
        }
        if (feature == "nte.skills") {
            return NteSkillsProfileAvailable() && NteSkillsReflectionReady();
        }
        if (feature == "nte.skill-invocation") {
            return NteSkillInvocationProfileAvailable() &&
                NteSkillInvocationReflectionReady();
        }
        return feature == "nte.session";
    }

    [[nodiscard]] bool MetricsFeatureAvailable() const noexcept {
        return SemanticFeatureAvailable("nte.session") ||
            SemanticFeatureAvailable("nte.player") ||
            SemanticFeatureAvailable("nte.entities") ||
            SemanticFeatureAvailable("nte.combat") ||
            SemanticFeatureAvailable("nte.skills");
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
        if (id == ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID) {
            return framework_hook_ready && process_event_hook_ready &&
                resolution.FeatureAvailable(kUe5ProcessEventFeature) &&
                process_event_endpoint.load(std::memory_order_acquire) != nullptr;
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
        if (id == ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.map-landmarks");
        }
        if (id == ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.navigation");
        }
        if (id == ANOMALY_NTE_PICKUP_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.pickup");
        }
        if (id == ANOMALY_NTE_ENTITIES_SERVICE_V1_ID) {
            return framework_hook_ready && SemanticFeatureAvailable("nte.entities");
        }
        if (id == ANOMALY_NTE_ACTORS_SERVICE_V1_ID) {
            return framework_hook_ready && NteActorsLayoutAvailable();
        }
        if (id == ANOMALY_NTE_COMBAT_SERVICE_V1_ID) {
            return framework_hook_ready && NteCombatProfileAvailable();
        }
        if (id == ANOMALY_NTE_SKILLS_SERVICE_V1_ID) {
            return framework_hook_ready && NteSkillsProfileAvailable();
        }
        if (id == ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID) {
            return framework_hook_ready &&
                SemanticFeatureAvailable("nte.skill-invocation");
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
            feature == "nte.pickup" ||
            feature == "nte.entities" ||
            feature == "nte.combat" || feature == "nte.skills" ||
            feature == "nte.skill-invocation") {
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
        actor_attempt_sequence = 0;
    }

    void InvalidateCombatSnapshot() noexcept {
        combat_sample_sequence = 0;
        combat_character = {};
        combat_target = {};
        combat_hp = 0.0;
        combat_max_hp = 0.0;
        combat_shield = 0.0;
        combat_dead = false;
        combat_available = false;
        combat_partial = false;
    }

    void ResetDamageEvents() noexcept {
        if (damage_event_sequence != 0) ++damage_event_sequence;
        damage_world_sequence_base = damage_event_sequence;
        damage_event_start = 0;
        damage_event_count = 0;
        damage_events = {};
        damage_dropped_count = 0;
        damage_next_source_id = kDamageSourceBase;
        damage_source_object_ids.clear();
        damage_source_objects.clear();
        damage_source_names.clear();
        damage_source_ability_classes.clear();
        pending_damage_source_mappings.clear();
        observed_damage_source_mappings.clear();
        damage_critical_tag_cache.clear();
        damage_participant_paths.clear();
        if (combat_event_sequence != 0) ++combat_event_sequence;
        combat_event_start = 0;
        combat_event_count = 0;
        combat_events = {};
        combat_event_names.clear();
        combat_event_objects.clear();
        pending_combat_event_names.clear();
        queued_combat_event_names.clear();
        failed_combat_event_names.clear();
        combat_participant_names.clear();
        pending_combat_participant_names.clear();
        queued_combat_participant_names.clear();
        failed_combat_participant_names.clear();
        localized_names_by_fname.clear();
        localized_names_by_key.clear();
        display_table_generation = 0;
        display_table_scan_complete = false;
        display_table_loaded_mask = 0;
        display_game_data = 0;
        display_game_data_generation = 0;
        display_table_indexes = {};
        damage_skill_index = {};
        active_effects.clear();
        active_effect_ability_system = 0;
        active_effect_array_key = 0;
        active_effects_initialized = false;
        combat_capture_read.store(0, std::memory_order_release);
        combat_capture_write.store(0, std::memory_order_release);
        display_name_demand.store(false, std::memory_order_release);
    }

    void InvalidateSkills() noexcept {
        if (skills_available || !skills.empty()) ++skill_generation;
        skills.clear();
        ability_display_names.clear();
        ability_display_name_attempts.clear();
        skill_sample_sequence = 0;
        skill_ability_system = 0;
        skill_character = {};
        skills_available = false;
        skills_partial = false;
    }

    void InvalidateCombatSkillDiscoveryLocked() noexcept {
        combat_skill_discovery = {};
        combat_skill_discovery.object_generation = object_generation;
        combat_capture_bindings.store(nullptr, std::memory_order_release);
        InvalidateCombatSnapshot();
        InvalidateSkills();
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

    void InvalidatePickupLocked(const std::uint32_t status) noexcept {
        pickup_demand.store(false, std::memory_order_release);
        pickup_request = {};
        pickup_confirmation = {};
        pickup_snapshot = {sizeof(pickup_snapshot)};
        pickup_snapshot.sequence = pickup_sequence;
        pickup_snapshot.state = pickup_sequence == 0
            ? ANOMALY_NTE_PICKUP_V1_IDLE
            : ANOMALY_NTE_PICKUP_V1_COMPLETE;
        pickup_snapshot.status = status;
        if (pickup_sequence != 0) pickup_snapshot.flags = ANOMALY_NTE_PICKUP_V1_VALID;
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
        map_landmark_binding = {};
        map_landmark_catalog.reset();
        map_landmark_next_refresh_sequence = 0;
        navigation = {};
        pickup_sequence = 0;
        InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
        InvalidateAhudBindingLocked();
        InvalidateCombatSkillDiscoveryLocked();
        ResetDamageEvents();
        damage_native_call_count = 0;
        reflection_fault_count = 0;
        last_reflection_fault_function = 0;
        last_reflection_fault_code = 0;
        damage_captured_event_count = 0;
        damage_capture_drop_count = 0;
        damage_attacker_resolution_failure_count = 0;
        damage_victim_resolution_failure_count = 0;
        damage_source_resolution_failure_count = 0;
        saved_trigger_skill_mapping_count = 0;
        trigger_ability_handle_mapping_count = 0;
        damage_source_mapping_failure_count = 0;
        delayed_damage_name_completion_count = 0;
        InvalidatePlayer();
        InvalidateEntities();
        InvalidateActors();
        entity_attempt_sequence = 0;
        player_attempt_sequence = 0;
        combat_attempt_sequence = 0;
        skill_attempt_sequence = 0;
        player_demand.store(false, std::memory_order_release);
        entity_demand.store(false, std::memory_order_release);
        navigation_demand.store(false, std::memory_order_release);
        pickup_demand.store(false, std::memory_order_release);
        ahud_demand.store(false, std::memory_order_release);
        combat_demand.store(false, std::memory_order_release);
        skill_demand.store(false, std::memory_order_release);
        combat_capture_read.store(0, std::memory_order_release);
        combat_capture_write.store(0, std::memory_order_release);
        combat_capture_drop_count.store(0, std::memory_order_release);
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
        InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
        InvalidatePlayer();
        InvalidateEntities();
        InvalidateActors();
        InvalidateCombatSnapshot();
        InvalidateSkills();
        ResetDamageEvents();
        if (world_pointer != 0) {
            world_pointer = 0;
            ++world_generation;
            ++world_change_sequence;
        }
        world_name_id = 0;
        world_name_layout_available = false;
        world_name_readable = false;
        teleport = {};
        map_landmark_binding = {};
        map_landmark_catalog.reset();
        map_landmark_next_refresh_sequence = 0;
        navigation = {};
        framework_hook_ready = false;
        ahud_hook_ready = false;
        process_event_hook_ready = false;
        InvalidateAhudBindingLocked();
        InvalidateCombatSkillDiscoveryLocked();
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
            InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            InvalidateCombatSnapshot();
            InvalidateSkills();
            ResetDamageEvents();
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
            map_landmark_binding = {};
            map_landmark_catalog.reset();
            map_landmark_next_refresh_sequence = 0;
            navigation = {};
            InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            if (object_generation != previous_generation) {
                InvalidateAhudBindingLocked();
                InvalidateCombatSkillDiscoveryLocked();
                ResetDamageEvents();
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
            map_landmark_binding = {};
            map_landmark_catalog.reset();
            map_landmark_next_refresh_sequence = 0;
            navigation = {};
            InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
        }
        object_registry = next;
        if (object_generation != previous_generation) {
            InvalidateAhudBindingLocked();
            InvalidatePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            InvalidateCombatSkillDiscoveryLocked();
            ResetDamageEvents();
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

    [[nodiscard]] bool InvokeNteFunctionLocked(
        const NteFunctionKind kind,
        const std::uintptr_t object,
        std::span<std::uint8_t> parameters) const noexcept {
        const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
        return binding && object != 0 && process_event_invoker &&
            ReadableRange(*memory, object, 0x20U) &&
            ReadableRange(*memory, binding->function, 0x20U) &&
            parameters.size() >= binding->parms_size &&
            InvokeNativeProcessEventLocked(
                object, binding->function, parameters.data(), binding->parms_size);
    }

    [[nodiscard]] bool InvokeNativeProcessEventLocked(
        const std::uintptr_t receiver,
        const std::uintptr_t function,
        void* const parameters,
        const std::size_t parameter_size) const noexcept {
        std::uintptr_t flags_address{};
        std::uint32_t original_flags{};
        const auto native_flag = Layout(profile, "ufunction.nativeFlag", -1);
        if (native_flag <= 0 || !process_event_invoker ||
            !AddAddress(function, Layout(profile, "ufunction.flags"), flags_address) ||
            !ReadValue(*memory, flags_address, original_flags)) {
            return false;
        }
        const auto invocation_flags = original_flags |
            static_cast<std::uint32_t>(native_flag);
        if (!memory->Write(
                flags_address, &invocation_flags, sizeof(invocation_flags))) {
            return false;
        }
        std::uint32_t exception_code{};
        const bool invoked = InvokeProcessEventGuarded(
            process_event_invoker, receiver, function, parameters, parameter_size,
            &exception_code);
        if (exception_code != 0) {
            ++reflection_fault_count;
            last_reflection_fault_function = function;
            last_reflection_fault_code = exception_code;
        }
        const bool restored = memory->Write(
            flags_address, &original_flags, sizeof(original_flags));
        return invoked && restored;
    }

    template <typename Value>
    [[nodiscard]] bool InvokeNteReturnLocked(
        const NteFunctionKind kind,
        const std::uintptr_t object,
        Value& value) const noexcept {
        const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
        if (!binding || binding->parms_size > 64U) return false;
        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        if (!InvokeNteFunctionLocked(kind, object, parameters)) return false;
        const std::size_t return_index = NteSpec(kind).parameters.size() - 1U;
        const std::size_t offset = binding->offsets[return_index];
        if (offset > binding->parms_size || sizeof(Value) > binding->parms_size - offset) {
            return false;
        }
        std::memcpy(&value, parameters.data() + offset, sizeof(Value));
        return true;
    }

    [[nodiscard]] bool InvokeNteBoolReturnLocked(
        const NteFunctionKind kind,
        const std::uintptr_t object,
        bool& value) const noexcept {
        const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
        if (!binding || binding->parms_size > 64U) return false;
        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        if (!InvokeNteFunctionLocked(kind, object, parameters)) return false;
        const std::size_t index = NteSpec(kind).parameters.size() - 1U;
        const ReflectedBoolParameter& reflected = binding->bool_parameters[index];
        if (reflected.byte_offset >= binding->parms_size) return false;
        value = (parameters[reflected.byte_offset] & reflected.field_mask) != 0;
        return true;
    }

    [[nodiscard]] bool EnsureSceneMonsterTableIndexLocked() noexcept {
        if (scene_monster_table_generation == object_generation &&
            scene_monster_table_scan_complete) {
            return scene_monster_table_count != 0;
        }
        if (scene_monster_table_generation != object_generation) {
            scene_monster_table_generation = object_generation;
            scene_monster_table_count = 0;
            scene_monster_table_scan_complete = false;
            for (auto& index : scene_monster_table_indexes) {
                index = DisplayTableIndex{};
            }
        }
        if (world_pointer == 0) return false;
        const auto persistent_level_offset =
            Layout(profile, "world.persistentLevel", -1);
        const auto level_settings_offset =
            Layout(profile, "level.worldSettings", -1);
        const auto scene_asset_offset =
            Layout(profile, "worldSettings.htSceneSolelyDataAsset", -1);
        const auto array_offset =
            Layout(profile, "sceneSolelyDataAsset.monsterArrayDataTable", -1);
        const auto count_offset = Layout(
            profile, "sceneSolelyDataAsset.monsterArrayDataTableCount", -1);
        const auto text_offset = Layout(profile, "monsterData.textName", -1);
        if (persistent_level_offset < 0 || level_settings_offset < 0 ||
            scene_asset_offset < 0 || array_offset < 0 || count_offset < 0 ||
            text_offset < 0) {
            return false;
        }
        std::uintptr_t persistent_level{};
        std::uintptr_t world_settings{};
        std::uintptr_t scene_asset{};
        if (!ReadPointerAt(
                *memory, world_pointer, persistent_level_offset, persistent_level) ||
            !ReadPointerAt(
                *memory, persistent_level, level_settings_offset, world_settings) ||
            !ReadPointerAt(
                *memory, world_settings, scene_asset_offset, scene_asset)) {
            return false;
        }
        std::uintptr_t array_data{};
        std::int32_t count{};
        if (!ReadPointerAt(*memory, scene_asset, array_offset, array_data) ||
            !ReadValue(*memory,
                scene_asset + static_cast<std::uintptr_t>(count_offset), count) ||
            count <= 0 ||
            static_cast<std::size_t>(count) > scene_monster_table_indexes.size()) {
            return false;
        }
        std::size_t built{};
        for (std::int32_t index = 0; index < count; ++index) {
            std::uintptr_t table{};
            if (!ReadValue(*memory,
                    array_data +
                        static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t),
                    table) || table == 0) {
                continue;
            }
            if (BuildDisplayTableIndexLocked(
                    table, "TextName", text_offset, -1, true,
                    scene_monster_table_indexes[built])) {
                ++built;
            }
        }
        scene_monster_table_count = built;
        scene_monster_table_scan_complete = true;
        return built != 0;
    }

    [[nodiscard]] bool ResolveMonsterNameFromSceneTablesLocked(
        const std::uint64_t key,
        const std::string_view supplied_key_text,
        std::string& value,
        const bool base_only = false) noexcept {
        value.clear();
        if (!EnsureSceneMonsterTableIndexLocked()) return false;
        std::string normalized(supplied_key_text);
        if (normalized.starts_with("Default__")) {
            normalized.erase(0, std::string_view{"Default__"}.size());
        }
        if (normalized.ends_with("_C")) {
            normalized.resize(normalized.size() - 2U);
        }
        const auto identity = MonsterDisplayIdentity(normalized);
        const auto try_row = [this, &value](
                                 const DisplayTableIndex& index,
                                 const std::uintptr_t row) {
            std::uintptr_t ftext_address{};
            return row != 0 && index.text_offset >= 0 &&
                AddAddress(row, index.text_offset, ftext_address) &&
                ResolveFTextLocked(ftext_address, value) && !value.empty();
        };
        for (std::size_t index{}; !base_only && index < scene_monster_table_count; ++index) {
            const auto& table_index = scene_monster_table_indexes[index];
            if (table_index.rows_by_fname.empty()) continue;
            std::uintptr_t row{};
            if (key != 0) {
                const auto found = table_index.rows_by_fname.find(key);
                if (found != table_index.rows_by_fname.end()) row = found->second;
            }
            if (row == 0 && !normalized.empty()) {
                const auto found = table_index.rows_by_key.find(normalized);
                if (found != table_index.rows_by_key.end()) row = found->second;
            }
            if (try_row(table_index, row)) return true;
            if (!identity.empty()) {
                const auto found = table_index.rows_by_key.find(identity);
                if (found != table_index.rows_by_key.end() &&
                    try_row(table_index, found->second)) {
                    return true;
                }
                for (const auto& [candidate, candidate_row] :
                         table_index.rows_by_key) {
                    if (!MonsterIdentityMatches(candidate, identity)) continue;
                    if (try_row(table_index, candidate_row)) return true;
                }
            }
        }
        if (!base_only) return false;
        const auto base_identity = NteMonsterBaseIdentity(identity);
        if (!base_identity.empty() && base_identity != identity) {
            std::string fallback;
            std::unordered_set<std::uintptr_t> visited;
            for (std::size_t index{}; index < scene_monster_table_count; ++index) {
                const auto& table_index = scene_monster_table_indexes[index];
                for (const auto& [candidate, row] : table_index.rows_by_key) {
                    if (MonsterDisplayIdentity(candidate) != base_identity ||
                        !visited.insert(row).second || !try_row(table_index, row)) continue;
                    if (!MergeNteMonsterFallbackName(fallback, value)) {
                        value.clear();
                        return false;
                    }
                }
            }
            value = std::move(fallback);
            return !value.empty();
        }
        value.clear();
        return false;
    }

    [[nodiscard]] bool ResolveMonsterNameWithFallbackLocked(
        const std::uint64_t key, const std::string_view key_text, std::string& value) {
        if (ResolveMonsterNameFromSceneTablesLocked(key, key_text, value) ||
            ResolveActorMonsterNameLocked(key_text, value)) return true;
        const auto abyss_key = AbyssStringTableMonsterKey(key_text);
        if (!abyss_key.empty() && ResolveAbyssStringTableEntryLocked(abyss_key, value)) return true;
        // A specific text-table entry takes precedence over a broad scene alias.
        return ResolveMonsterNameFromSceneTablesLocked(key, key_text, value, true) ||
            ResolveActorMonsterNameLocked(key_text, value, true);
    }

    [[nodiscard]] bool ResolveMonsterStaticDataNameLocked(
        const std::uint64_t config_id_key, std::string& value) noexcept {
        value.clear();
        ++monster_static_data_resolution_calls;
        if (config_id_key != 0) {
            // GetDataTableRowFromName is a custom thunk with a wildcard inline
            // OutRow, not an output pointer. Use the validated table index.
            std::string config_text;
            static_cast<void>(ResolveFNameLocked(
                static_cast<std::uint32_t>(config_id_key & 0xFFFFFFFFU),
                static_cast<std::uint32_t>(config_id_key >> 32U), config_text));
            if (ResolveMonsterNameFromSceneTablesLocked(
                    config_id_key, config_text, value) &&
                !value.empty()) {
                ++monster_static_data_resolution_successes;
                return true;
            }
        }
        const auto& binding = combat_skill_discovery.functions[
            NteIndex(NteFunctionKind::GetMonsterStaticData)];
        constexpr std::uint16_t kMonsterStaticDataSize = 280;
        constexpr std::size_t kMonsterStaticDataParameterCapacity = 304;
        const auto text_offset = Layout(profile, "monsterData.textName", -1);
        if (!binding || binding->parms_size == 0 ||
            binding->parms_size > kMonsterStaticDataParameterCapacity ||
            config_id_key == 0 || world_pointer == 0 || !process_event_invoker ||
            text_offset < 0 ||
            text_offset + 16 > kMonsterStaticDataSize) {
            return false;
        }
        const auto world_offset = binding->offsets[0];
        const auto config_offset = binding->offsets[1];
        const auto output_offset = binding->offsets[2];
        if (world_offset > binding->parms_size ||
            sizeof(std::uintptr_t) > binding->parms_size - world_offset ||
            config_offset > binding->parms_size ||
            sizeof(std::uint64_t) > binding->parms_size - config_offset ||
            output_offset > binding->parms_size ||
            kMonsterStaticDataSize > binding->parms_size - output_offset) {
            return false;
        }
        const auto persistent_level_offset =
            Layout(profile, "world.persistentLevel", -1);
        const auto level_settings_offset =
            Layout(profile, "level.worldSettings", -1);
        const auto scene_asset_offset =
            Layout(profile, "worldSettings.htSceneSolelyDataAsset", -1);
        std::uintptr_t persistent_level{};
        std::uintptr_t world_settings{};
        std::uintptr_t receiver{};
        std::uintptr_t receiver_class{};
        if (persistent_level_offset < 0 || level_settings_offset < 0 ||
            scene_asset_offset < 0 ||
            !ReadPointerAt(
                *memory, world_pointer, persistent_level_offset, persistent_level) ||
            persistent_level == 0 ||
            !ReadPointerAt(
                *memory, persistent_level, level_settings_offset, world_settings) ||
            world_settings == 0 ||
            !ReadPointerAt(
                *memory, world_settings, scene_asset_offset, receiver) ||
            receiver == 0 ||
            !ReadPointerAt(
                *memory, receiver, Layout(profile, "object.class"), receiver_class) ||
            !IsClassDerivedFromLocked(receiver_class, binding->outer_class)) {
            return false;
        }
        alignas(std::uint64_t)
            std::array<std::uint8_t, kMonsterStaticDataParameterCapacity> parameters{};
        std::memcpy(parameters.data() + world_offset,
            &world_pointer, sizeof(world_pointer));
        const auto comparison_index = static_cast<std::uint32_t>(
            config_id_key & 0xFFFFFFFFU);
        const auto number = static_cast<std::uint32_t>(config_id_key >> 32U);
        std::memcpy(parameters.data() + config_offset,
            &comparison_index, sizeof(comparison_index));
        std::memcpy(parameters.data() + config_offset + sizeof(comparison_index),
            &number, sizeof(number));
        if (!InvokeNteFunctionLocked(
                NteFunctionKind::GetMonsterStaticData, receiver, parameters)) {
            return false;
        }
        const auto return_index =
            NteSpec(NteFunctionKind::GetMonsterStaticData).parameters.size() - 1U;
        const ReflectedBoolParameter& reflected = binding->bool_parameters[return_index];
        if (reflected.byte_offset >= binding->parms_size ||
            (parameters[reflected.byte_offset] & reflected.field_mask) == 0) {
            return false;
        }
        const auto ftext_offset =
            static_cast<std::size_t>(output_offset) + static_cast<std::size_t>(text_offset);
        std::array<std::uint8_t, 16> ftext_bytes{};
        if (ftext_offset > binding->parms_size ||
            ftext_bytes.size() > binding->parms_size - ftext_offset) {
            return false;
        }
        std::memcpy(ftext_bytes.data(),
            parameters.data() + ftext_offset, ftext_bytes.size());
        if (!ResolveFTextBytesLocked(ftext_bytes, value)) {
            return false;
        }
        ++monster_static_data_resolution_successes;
        return true;
    }

    [[nodiscard]] bool CurrentAbilitySystemLocked(
        const std::uintptr_t pawn,
        std::uintptr_t& ability_system) const noexcept {
        ability_system = 0;
        if (!InvokeNteReturnLocked(
                NteFunctionKind::GetAbilitySystemComponent, pawn, ability_system) ||
            ability_system == 0 ||
            combat_skill_discovery.ability_system_class == 0) {
            return false;
        }
        std::uintptr_t ability_system_class{};
        return ReadPointerAt(
                   *memory, ability_system, Layout(profile, "object.class"),
                   ability_system_class) &&
            IsClassDerivedFromLocked(
                ability_system_class,
                combat_skill_discovery.ability_system_class);
    }

    void RefreshCombat(std::uint64_t sequence) noexcept {
        combat_attempt_sequence = sequence;
        combat_refresh_failure = 0;
        if (!SemanticFeatureAvailable("nte.combat") || !player_available ||
            player_pawn == 0 || world_pointer == 0) {
            combat_refresh_failure = 1;
            InvalidateCombatSnapshot();
            return;
        }
        AnomalyGenerationHandleV1 character{};
        if (!ObjectHandleLocked(player_pawn, character)) {
            combat_refresh_failure = 2;
            InvalidateCombatSnapshot();
            return;
        }
        std::uintptr_t pawn_class{};
        const auto& get_hp = combat_skill_discovery.functions[
            NteIndex(NteFunctionKind::GetHp)];
        if (!get_hp ||
            !ReadPointerAt(
                *memory, player_pawn, Layout(profile, "object.class"), pawn_class) ||
            !IsClassDerivedFromLocked(pawn_class, get_hp->outer_class)) {
            combat_refresh_failure = 3;
            InvalidateCombatSnapshot();
            return;
        }

        float hp{};
        float max_hp{};
        float shield{};
        bool dead{};
        std::uintptr_t target{};
        std::uintptr_t ability_system{};
        const auto& hp_max_binding = combat_skill_discovery.functions[
            NteIndex(NteFunctionKind::GetHpMax)];
        alignas(std::uint64_t) std::array<std::uint8_t, 8> hp_max_parameters{};
        if (!InvokeNteReturnLocked(NteFunctionKind::GetHp, player_pawn, hp) ||
            !hp_max_binding ||
            !InvokeNteFunctionLocked(
                NteFunctionKind::GetHpMax, player_pawn, hp_max_parameters) ||
            !InvokeNteBoolReturnLocked(
                NteFunctionKind::GetIsDead, player_pawn, dead)) {
            combat_refresh_failure = 4;
            InvalidateCombatSnapshot();
            return;
        }
        bool partial = false;
        if (!InvokeNteReturnLocked(
                NteFunctionKind::GetAttackTarget, player_pawn, target)) {
            target = 0;
            combat_refresh_failure = 5;
            partial = true;
        }
        if (!CurrentAbilitySystemLocked(player_pawn, ability_system)) {
            ability_system = 0;
            combat_refresh_failure = 6;
            partial = true;
        } else if (!InvokeNteReturnLocked(
                NteFunctionKind::GetShieldHealth, ability_system, shield)) {
            shield = 0.0F;
            combat_refresh_failure = 7;
            partial = true;
        }
        const std::size_t max_hp_return = hp_max_binding->offsets[1];
        if (max_hp_return > hp_max_parameters.size() ||
            sizeof(max_hp) > hp_max_parameters.size() - max_hp_return) {
            combat_refresh_failure = 8;
            InvalidateCombatSnapshot();
            return;
        }
        std::memcpy(&max_hp, hp_max_parameters.data() + max_hp_return, sizeof(max_hp));
        if (!std::isfinite(hp) || !std::isfinite(max_hp) ||
            !std::isfinite(shield) || max_hp < 0.0F) {
            combat_refresh_failure = 9;
            InvalidateCombatSnapshot();
            return;
        }
        AnomalyGenerationHandleV1 target_handle{};
        const bool target_handle_failed =
            target != 0 && !ObjectHandleLocked(target, target_handle);
        partial = partial || target_handle_failed;
        if (target_handle_failed) combat_refresh_failure = 10;
        combat_character = character;
        combat_target = target_handle;
        combat_hp = hp;
        combat_max_hp = max_hp;
        combat_shield = shield;
        combat_dead = dead;
        combat_partial = partial;
        combat_available = true;
        combat_sample_sequence = sequence;
        if (!partial) combat_refresh_failure = 0;
        try {
            if (ability_system != 0) {
                RefreshActiveGameplayEffectsLocked(
                    ability_system, sequence, character);
            }
        } catch (...) {
            combat_partial = true;
            combat_refresh_failure = 11;
        }
    }

    struct NativeArrayHeader {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    };

    [[nodiscard]] bool ReadNativeArrayHeaderLocked(
        const std::uintptr_t address,
        NativeArrayHeader& header,
        const std::int32_t maximum) const noexcept {
        NativeArrayHeader next;
        if (address == 0 || maximum <= 0 ||
            !ReadValue(
                *memory, address + Layout(profile, "tarray.data"), next.data) ||
            !ReadValue(
                *memory, address + Layout(profile, "tarray.num"), next.count) ||
            !ReadValue(
                *memory, address + Layout(profile, "tarray.max"), next.capacity) ||
            next.count < 0 || next.capacity < next.count || next.capacity > maximum ||
            (next.count != 0 && next.data == 0)) {
            return false;
        }
        header = next;
        return true;
    }

    [[nodiscard]] bool ReadSkillArrayLocked(
        const std::uintptr_t ability_system,
        NativeArrayHeader& header) const noexcept {
        std::uintptr_t container{};
        std::uintptr_t items{};
        return AddAddress(
                   ability_system,
                   Layout(profile, "abilitySystem.activatableAbilities"),
                   container) &&
            AddAddress(
                container, Layout(profile, "abilitySpecContainer.items"), items) &&
            ReadNativeArrayHeaderLocked(
                items, header,
                static_cast<std::int32_t>(Layout(profile, "skills.maxCount")));
    }

    [[nodiscard]] bool ReadCombatNameIdentityLocked(
        const std::uintptr_t object, std::uint64_t& name_id) noexcept {
        name_id = 0;
        const auto name_offset = Layout(profile, "object.nameOffset", -1);
        std::uint32_t comparison_index{};
        std::uint32_t number{};
        if (object == 0 || name_offset < 0 ||
            !ReadValue(*memory, object + static_cast<std::uintptr_t>(name_offset),
                comparison_index) ||
            !ReadValue(*memory,
                object + static_cast<std::uintptr_t>(name_offset) +
                    sizeof(comparison_index),
                number)) {
            return false;
        }
        name_id = static_cast<std::uint64_t>(comparison_index) |
            (static_cast<std::uint64_t>(number) << 32U);
        if (name_id == 0) return false;
        combat_event_objects.emplace(name_id, object);
        CacheCombatEventNameLocked(name_id, object);
        return true;
    }

    void PublishActiveEffectEventLocked(
        const ActiveEffectRecord& effect, const std::uint32_t kind,
        const std::uint64_t sequence,
        const AnomalyGenerationHandleV1 target) noexcept {
        AnomalyNteCombatEventV1 event{};
        event.kind = kind;
        event.tick_sequence = sequence;
        event.world = {1, world_generation};
        event.target = target;
        event.name_id = effect.name_id;
        event.duration_seconds = kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD
            ? effect.duration_seconds : 0.0F;
        event.stack_count = kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD
            ? effect.stack_count : 0;
        if (combat_event_names.contains(event.name_id)) {
            event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
        }
        RecordCombatEventLocked(event);
    }

    void RefreshActiveGameplayEffectsLocked(
        const std::uintptr_t ability_system, const std::uint64_t sequence,
        const AnomalyGenerationHandleV1 target) {
        std::uintptr_t container{};
        const auto container_offset = Layout(
            profile, "abilitySystem.activeGameplayEffects", -1);
        const auto key_offset = Layout(
            profile, "activeGameplayEffects.arrayReplicationKey", -1);
        const auto items_offset = Layout(profile, "activeGameplayEffects.items", -1);
        if (ability_system == 0 || container_offset < 0 || key_offset < 0 ||
            items_offset < 0 || !AddAddress(ability_system, container_offset, container)) {
            return;
        }
        std::int32_t key_before{};
        if (!ReadValue(*memory,
                container + static_cast<std::uintptr_t>(key_offset), key_before)) {
            return;
        }
        if (active_effects_initialized &&
            active_effect_ability_system == ability_system &&
            active_effect_array_key == key_before) {
            return;
        }

        NativeArrayHeader array;
        if (!ReadNativeArrayHeaderLocked(
                container + static_cast<std::uintptr_t>(items_offset), array,
                static_cast<std::int32_t>(Layout(profile, "buffs.maxCount")))) {
            return;
        }
        const auto stride = static_cast<std::size_t>(
            Layout(profile, "activeGameplayEffect.size"));
        const auto spec_offset = static_cast<std::size_t>(
            Layout(profile, "activeGameplayEffect.spec"));
        const auto replication_id_offset = static_cast<std::size_t>(
            Layout(profile, "activeGameplayEffect.replicationId"));
        const auto replication_key_offset = static_cast<std::size_t>(
            Layout(profile, "activeGameplayEffect.replicationKey"));
        const auto definition_offset = spec_offset + static_cast<std::size_t>(
            Layout(profile, "gameplayEffectSpec.def"));
        const auto duration_offset = spec_offset + static_cast<std::size_t>(
            Layout(profile, "gameplayEffectSpec.duration"));
        const auto stack_offset = spec_offset + static_cast<std::size_t>(
            Layout(profile, "gameplayEffectSpec.stackCount"));
        if (stride == 0 || static_cast<std::size_t>(array.count) >
                (std::numeric_limits<std::size_t>::max)() / stride) {
            return;
        }
        const std::size_t byte_count = static_cast<std::size_t>(array.count) * stride;
        std::vector<std::uint8_t> bytes(byte_count);
        if (byte_count != 0 && !memory->Read(array.data, bytes.data(), byte_count)) return;

        const bool same_owner = active_effects_initialized &&
            active_effect_ability_system == ability_system;
        std::unordered_map<std::int32_t, std::size_t> previous_by_id;
        std::vector<bool> previous_seen;
        if (same_owner) {
            previous_by_id.reserve(active_effects.size());
            previous_seen.resize(active_effects.size());
            for (std::size_t index{}; index < active_effects.size(); ++index) {
                previous_by_id.emplace(active_effects[index].replication_id, index);
            }
        }

        std::vector<ActiveEffectRecord> next;
        next.reserve(static_cast<std::size_t>(array.count));
        for (std::int32_t index{}; index < array.count; ++index) {
            const auto* item = bytes.data() + static_cast<std::size_t>(index) * stride;
            ActiveEffectRecord effect;
            std::memcpy(&effect.replication_id, item + replication_id_offset,
                sizeof(effect.replication_id));
            std::memcpy(&effect.replication_key, item + replication_key_offset,
                sizeof(effect.replication_key));
            std::memcpy(&effect.definition, item + definition_offset,
                sizeof(effect.definition));
            std::memcpy(&effect.duration_seconds, item + duration_offset,
                sizeof(effect.duration_seconds));
            std::memcpy(&effect.stack_count, item + stack_offset,
                sizeof(effect.stack_count));
            if (effect.replication_id <= 0 || effect.definition == 0 ||
                !std::isfinite(effect.duration_seconds) || effect.stack_count < 0) {
                continue;
            }
            const auto previous = previous_by_id.find(effect.replication_id);
            if (previous != previous_by_id.end() &&
                active_effects[previous->second].definition == effect.definition) {
                effect.name_id = active_effects[previous->second].name_id;
                previous_seen[previous->second] = true;
            } else if (!ReadCombatNameIdentityLocked(effect.definition, effect.name_id)) {
                continue;
            }
            next.push_back(effect);
        }
        std::int32_t key_after{};
        if (!ReadValue(*memory,
                container + static_cast<std::uintptr_t>(key_offset), key_after) ||
            key_after != key_before) {
            return;
        }

        if (same_owner) {
            for (std::size_t index{}; index < active_effects.size(); ++index) {
                if (!previous_seen[index]) {
                    PublishActiveEffectEventLocked(active_effects[index],
                        ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE, sequence, target);
                }
            }
        }
        for (const auto& current : next) {
            const auto previous = previous_by_id.find(current.replication_id);
            if (previous == previous_by_id.end() ||
                active_effects[previous->second].definition != current.definition ||
                active_effects[previous->second].replication_key != current.replication_key ||
                active_effects[previous->second].stack_count != current.stack_count) {
                PublishActiveEffectEventLocked(current,
                    ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD, sequence, target);
            }
        }
        active_effects = std::move(next);
        active_effect_ability_system = ability_system;
        active_effect_array_key = key_after;
        active_effects_initialized = true;
    }

    [[nodiscard]] bool ReadSkillIdentityLocked(
        const NativeArrayHeader& array,
        const std::int32_t index,
        SkillRecord& record) const {
        if (index < 0 || index >= array.count) return false;
        const auto stride = static_cast<std::uint64_t>(Layout(profile, "abilitySpec.stride"));
        if (static_cast<std::uint64_t>(index) >
            (std::numeric_limits<std::uint64_t>::max)() / stride) {
            return false;
        }
        std::uintptr_t spec{};
        if (!AddUnsignedAddress(
                array.data, static_cast<std::uint64_t>(index) * stride, spec)) {
            return false;
        }
        std::uint8_t active_count{};
        std::uint8_t state_bits{};
        if (!ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.handle"),
                record.spec_handle) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.ability"), record.ability) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.level"), record.level) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.inputId"), record.input_id) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.activeCount"), active_count) ||
            !ReadValue(
                *memory, spec + Layout(profile, "abilitySpec.stateBits"), state_bits) ||
            record.spec_handle == 0 || record.ability == 0 ||
            !ReadPointerAt(
                *memory, record.ability, Layout(profile, "object.class"),
                record.ability_class_pointer) ||
            combat_skill_discovery.gameplay_ability_class == 0 ||
            !IsClassDerivedFromLocked(
                record.ability_class_pointer,
                combat_skill_discovery.gameplay_ability_class) ||
            !ObjectHandleLocked(record.ability_class_pointer, record.ability_class)) {
            return false;
        }
        record.flags = ANOMALY_NTE_SKILL_V1_VALID |
            ANOMALY_NTE_SKILL_V1_PARTIAL;
        if (active_count != 0) record.flags |= ANOMALY_NTE_SKILL_V1_ACTIVE;
        if ((state_bits & 0x01U) != 0) record.flags |= ANOMALY_NTE_SKILL_V1_INPUT_PRESSED;
        if ((state_bits & 0x02U) != 0) {
            record.flags |= ANOMALY_NTE_SKILL_V1_REMOVE_AFTER_ACTIVATION;
        }
        if ((state_bits & 0x04U) != 0) record.flags |= ANOMALY_NTE_SKILL_V1_PENDING_REMOVE;
        return true;
    }

    [[nodiscard]] static bool SameSkillIdentity(
        const SkillRecord& left,
        const SkillRecord& right) noexcept {
        return left.spec_handle == right.spec_handle &&
            left.ability == right.ability &&
            left.ability_class_pointer == right.ability_class_pointer;
    }

    [[nodiscard]] bool RefreshSkillCooldownLocked(
        const std::uintptr_t ability_system,
        SkillRecord& record) const noexcept {
        const auto& binding = combat_skill_discovery.functions[NteIndex(
            NteFunctionKind::GetActiveEffectTimeRemainingAndDuration)];
        if (!combat_skill_discovery.cooldown_layout_valid || !binding ||
            binding->parms_size > 64U) {
            return false;
        }

        std::uintptr_t effect_address{};
        std::uintptr_t effect_class{};
        if (!AddAddress(
                record.ability,
                Layout(profile, "ability.cooldownGameplayEffectClass"),
                effect_address) ||
            !ReadValue(*memory, effect_address, effect_class)) {
            return false;
        }
        if (effect_class == 0) {
            record.flags &= ~ANOMALY_NTE_SKILL_V1_PARTIAL;
            record.flags |= ANOMALY_NTE_SKILL_V1_COOLDOWN_VALID;
            return true;
        }
        AnomalyGenerationHandleV1 effect_handle{};
        if (!ObjectHandleLocked(effect_class, effect_handle) ||
            binding->meta_class == 0 ||
            !IsClassDerivedFromLocked(effect_class, binding->meta_class)) {
            return false;
        }

        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        const std::size_t class_offset = binding->offsets[0];
        if (class_offset > binding->parms_size ||
            sizeof(effect_class) > binding->parms_size - class_offset) {
            return false;
        }
        std::memcpy(
            parameters.data() + class_offset, &effect_class, sizeof(effect_class));
        if (!InvokeNteFunctionLocked(
                NteFunctionKind::GetActiveEffectTimeRemainingAndDuration,
                ability_system,
                parameters)) {
            return false;
        }

        const std::size_t remaining_offset = binding->offsets[1];
        const std::size_t duration_offset = binding->offsets[2];
        float remaining{};
        float duration{};
        if (remaining_offset > binding->parms_size ||
            sizeof(remaining) > binding->parms_size - remaining_offset ||
            duration_offset > binding->parms_size ||
            sizeof(duration) > binding->parms_size - duration_offset) {
            return false;
        }
        std::memcpy(
            &remaining, parameters.data() + remaining_offset, sizeof(remaining));
        std::memcpy(
            &duration, parameters.data() + duration_offset, sizeof(duration));
        if (!std::isfinite(remaining) || !std::isfinite(duration)) return false;
        record.cooldown_remaining_seconds = (std::max)(0.0F, remaining);
        record.cooldown_duration_seconds = (std::max)(0.0F, duration);
        record.flags &= ~ANOMALY_NTE_SKILL_V1_PARTIAL;
        record.flags |= ANOMALY_NTE_SKILL_V1_COOLDOWN_VALID;
        return true;
    }

    void RefreshSkills(std::uint64_t sequence) noexcept {
        skill_attempt_sequence = sequence;
        if (!SemanticFeatureAvailable("nte.skills") || !player_available ||
            player_pawn == 0 || world_pointer == 0) {
            InvalidateSkills();
            return;
        }
        std::uintptr_t ability_system{};
        AnomalyGenerationHandleV1 character{};
        NativeArrayHeader array;
        if (!CurrentAbilitySystemLocked(player_pawn, ability_system) ||
            !ObjectHandleLocked(player_pawn, character) ||
            !ReadSkillArrayLocked(ability_system, array)) {
            InvalidateSkills();
            return;
        }
        try {
            std::vector<SkillRecord> next;
            next.reserve(static_cast<std::size_t>(array.count));
            bool partial{};
            for (std::int32_t index{}; index < array.count; ++index) {
                SkillRecord record;
                if (!ReadSkillIdentityLocked(array, index, record)) {
                    InvalidateSkills();
                    return;
                }
                const auto previous = std::ranges::find_if(
                    skills, [&](const SkillRecord& current) {
                        return SameSkillIdentity(current, record);
                    });
                if (previous != skills.end()) {
                    record.ability_path = previous->ability_path;
                } else {
                    record.ability_path = ObjectPathLocked(record.ability_class_pointer);
                }
                if (record.ability_path.empty()) {
                    InvalidateSkills();
                    return;
                }
                if (!RefreshSkillCooldownLocked(ability_system, record)) {
                    partial = true;
                }
                record.character = character;
                record.sequence = sequence;
                next.push_back(std::move(record));
            }

            bool same_identity = skills_available && skill_ability_system == ability_system &&
                skill_character.id == character.id &&
                skill_character.generation == character.generation &&
                skills.size() == next.size();
            if (same_identity) {
                std::vector<bool> matched(skills.size());
                for (SkillRecord& candidate : next) {
                    const auto found = std::find_if(
                        skills.begin(), skills.end(), [&](const SkillRecord& current) {
                            const std::size_t old_index = static_cast<std::size_t>(
                                &current - skills.data());
                            return !matched[old_index] &&
                                SameSkillIdentity(current, candidate);
                        });
                    if (found == skills.end()) {
                        same_identity = false;
                        break;
                    }
                    const std::size_t old_index = static_cast<std::size_t>(found - skills.begin());
                    matched[old_index] = true;
                    candidate.handle = found->handle;
                }
            }
            if (!same_identity) {
                ++skill_generation;
                for (SkillRecord& candidate : next) {
                    if (skill_next_id == 0) ++skill_next_id;
                    candidate.handle = {skill_next_id++, skill_generation};
                }
            }

            skills = std::move(next);
            skill_ability_system = ability_system;
            skill_character = character;
            skill_sample_sequence = sequence;
            skills_available = true;
            skills_partial = partial;
            if ((display_table_loaded_mask & 0x04U) == 0) {
                display_name_demand.store(true, std::memory_order_release);
            } else {
                for (const auto& record : skills) {
                    if (ability_display_names.contains(record.ability_class.id)) {
                        continue;
                    }
                    const auto attempts = ability_display_name_attempts.find(
                        record.ability_class.id);
                    if (attempts != ability_display_name_attempts.end() &&
                        attempts->second >= 2U) {
                        continue;
                    }
                    std::string display_name;
                    if (ResolveAbilityDisplayNameForClassLocked(
                            record.ability_class_pointer, display_name)) {
                        ability_display_name_attempts.erase(record.ability_class.id);
                        CompleteDamageSourceNamesForAbilityLocked(
                            record.ability_class.id, display_name);
                    } else {
                        auto& attempt_count = ability_display_name_attempts[record.ability_class.id];
                        if (attempt_count < 2U) ++attempt_count;
                    }
                    break;
                }
            }
        } catch (...) {
            InvalidateSkills();
        }
    }

    [[nodiscard]] bool WeakObjectHandleLocked(
        const std::int32_t index,
        const std::int32_t serial,
        AnomalyGenerationHandleV1& handle) const noexcept {
        handle = {};
        if (index < 0 || serial <= 0 ||
            static_cast<std::uint64_t>(index) >= object_registry.count) {
            return false;
        }
        std::uintptr_t object{};
        std::uint32_t observed_serial{};
        if (!ReadObjectSlot(
                *memory, object_registry, static_cast<std::uint32_t>(index),
                object, observed_serial) || object == 0 ||
            observed_serial != static_cast<std::uint32_t>(serial)) {
            return false;
        }
        handle = {
            EncodeObjectHandle(
                static_cast<std::uint32_t>(index), observed_serial),
            object_generation};
        return true;
    }

    [[nodiscard]] bool ReadWeakObjectPointerLocked(
        const std::uintptr_t address,
        std::uintptr_t& object) const noexcept {
        object = 0;
        if (address == 0) return false;
        std::int32_t index{};
        std::int32_t serial{};
        if (!ReadValue(*memory, address, index) ||
            !ReadValue(
                *memory,
                address + static_cast<std::uintptr_t>(sizeof(index)),
                serial)) {
            return false;
        }
        AnomalyGenerationHandleV1 handle{};
        if (!WeakObjectHandleLocked(index, serial, handle)) return false;
        return ResolveObjectHandleLocked(handle, object) && object != 0;
    }

    void RecordDamageEventLocked(AnomalyNteDamageEventV1 event) noexcept {
        event.struct_size = sizeof(event);
        event.sequence = ++damage_event_sequence;
        if (damage_event_count == kDamageEventCapacity) {
            damage_events[damage_event_start].event = event;
            damage_event_start = (damage_event_start + 1U) % kDamageEventCapacity;
            return;
        }
        const std::size_t slot =
            (damage_event_start + damage_event_count) % kDamageEventCapacity;
        damage_events[slot].event = event;
        ++damage_event_count;
    }

    void RecordCombatEventLocked(AnomalyNteCombatEventV1 event) noexcept {
        event.struct_size = sizeof(event);
        event.sequence = ++combat_event_sequence;
        QueueCombatParticipantNameLocked(event.source);
        QueueCombatParticipantNameLocked(event.target);
        if (combat_event_count == kCombatEventCapacity) {
            combat_events[combat_event_start].event = event;
            combat_event_start = (combat_event_start + 1U) % kCombatEventCapacity;
            return;
        }
        const std::size_t slot =
            (combat_event_start + combat_event_count) % kCombatEventCapacity;
        combat_events[slot].event = event;
        ++combat_event_count;
    }

    static void AppendParticipantNameCandidate(
        PendingCombatParticipantName& pending,
        const std::uint64_t key,
        std::string key_text) {
        if ((key == 0 && key_text.empty()) ||
            pending.key_count >= pending.keys.size()) {
            return;
        }
        for (std::size_t index{}; index < pending.key_count; ++index) {
            if ((key != 0 && pending.keys[index] == key) ||
                (!key_text.empty() && pending.key_texts[index] == key_text)) {
                return;
            }
        }
        const auto index = pending.key_count++;
        pending.keys[index] = key;
        pending.key_texts[index] = std::move(key_text);
    }

    [[nodiscard]] bool CaptureCombatParticipantIdentityLocked(
        const AnomalyGenerationHandleV1 participant,
        const std::uintptr_t object,
        PendingCombatParticipantName& pending) {
        pending = {};
        pending.participant = participant;
        pending.object = object;
        participant_last_handle = participant.id;
        participant_last_player = false;
        participant_last_class_text.clear();
        participant_last_config_text.clear();
        if (object == 0) return false;

        const auto append_fname = [this, &pending](
                                      const std::uint32_t comparison_index,
                                      const std::uint32_t number) {
            const auto key = static_cast<std::uint64_t>(comparison_index) |
                (static_cast<std::uint64_t>(number) << 32U);
            if (key == 0) return;
            std::string key_text;
            static_cast<void>(ResolveFNameLocked(
                comparison_index, number, key_text));
            AppendParticipantNameCandidate(pending, key, std::move(key_text));
        };

        std::uintptr_t object_class{};
        const auto name_offset = Layout(profile, "object.nameOffset", -1);
        std::uint32_t class_index{};
        std::uint32_t class_number{};
        if (name_offset >= 0 &&
            ReadPointerAt(*memory, object, Layout(profile, "object.class"), object_class) &&
            object_class != 0 &&
            ReadValue(*memory,
                object_class + static_cast<std::uintptr_t>(name_offset), class_index) &&
            ReadValue(*memory,
                object_class + static_cast<std::uintptr_t>(name_offset) +
                    sizeof(class_index),
                class_number)) {
            std::string class_name;
            static_cast<void>(ResolveFNameLocked(
                class_index, class_number, class_name));
            participant_last_class_text = class_name;
            const auto class_key = static_cast<std::uint64_t>(class_index) |
                (static_cast<std::uint64_t>(class_number) << 32U);
            AppendParticipantNameCandidate(pending, class_key, class_name);
        }
        const bool is_player_participant = std::ranges::any_of(
            std::array{NteFunctionKind::GetMainCharacterId,
                       NteFunctionKind::GetNpcMainCharacterId},
            [this, object_class](const NteFunctionKind kind) {
                const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
                return binding && object_class != 0 &&
                    IsClassDerivedFromLocked(object_class, binding->outer_class);
        });
        pending.player_participant = is_player_participant;
        participant_last_player = is_player_participant;
        if (is_player_participant) {
            const auto read_character_id = [this, object, &pending](
                                               const std::int64_t offset) {
                if (offset < 0) return;
                std::uint32_t index{};
                std::uint32_t number{};
                if (ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(offset),
                        index) &&
                    ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(offset) +
                            sizeof(index),
                        number)) {
                    const auto key = static_cast<std::uint64_t>(index) |
                        (static_cast<std::uint64_t>(number) << 32U);
                    AppendParticipantNameCandidate(pending, key, {});
                }
            };
            read_character_id(Layout(
                profile, "playerCharacter.defaultCharacterId", -1));
            read_character_id(Layout(
                profile, "playerCharacter.currentDisplayCharacterId", -1));
        }
        const auto config_id_offset = Layout(
            profile, "abilityCharacter.characterConfigId", -1);
        if (!is_player_participant && config_id_offset >= 0) {
            std::uint32_t config_index{};
            std::uint32_t config_number{};
            if (ReadValue(*memory,
                    object + static_cast<std::uintptr_t>(config_id_offset),
                    config_index) &&
                ReadValue(*memory,
                    object + static_cast<std::uintptr_t>(config_id_offset) +
                        sizeof(config_index),
                    config_number)) {
                pending.config_id_key = static_cast<std::uint64_t>(config_index) |
                    (static_cast<std::uint64_t>(config_number) << 32U);
                pending.has_config_id = pending.config_id_key != 0;
                static_cast<void>(ResolveFNameLocked(
                    config_index, config_number, participant_last_config_text));
                append_fname(config_index, config_number);
            }
        }
        // A player can expose a valid MainCharacterID even when its class FName
        // is temporarily unreadable. Keep the object for the deferred getter
        // instead of permanently failing the participant at capture time.
        return true;
    }

    void QueueCombatParticipantNameLocked(
        const AnomalyGenerationHandleV1 participant,
        const std::uintptr_t object) noexcept {
        if (participant.id == 0 || participant.generation != object_generation ||
            combat_participant_names.contains(participant.id) ||
            failed_combat_participant_names.contains(participant.id) ||
            queued_combat_participant_names.contains(participant.id)) {
            return;
        }
        try {
            PendingCombatParticipantName pending;
            if (!CaptureCombatParticipantIdentityLocked(
                    participant, object, pending)) {
                failed_combat_participant_names.insert(participant.id);
                return;
            }
            queued_combat_participant_names.insert(participant.id);
            pending_combat_participant_names.push_back(std::move(pending));
            if (!display_table_scan_complete) {
                display_name_demand.store(true, std::memory_order_release);
            }
        } catch (...) {
            queued_combat_participant_names.erase(participant.id);
        }
    }

    void QueueCombatParticipantNameLocked(
        const AnomalyGenerationHandleV1 participant) noexcept {
        if (participant.id == 0 || participant.generation != object_generation ||
            combat_participant_names.contains(participant.id) ||
            failed_combat_participant_names.contains(participant.id) ||
            queued_combat_participant_names.contains(participant.id)) {
            return;
        }
        std::uintptr_t object{};
        if (!ResolveObjectHandleLocked(participant, object)) {
            return;
        }
        QueueCombatParticipantNameLocked(participant, object);
    }

    void ResolveNextCombatParticipantNameLocked() noexcept {
        if ((display_table_loaded_mask & 0x01U) == 0 ||
            pending_combat_participant_names.empty()) {
            return;
        }
        auto pending = std::move(pending_combat_participant_names.front());
        pending_combat_participant_names.pop_front();
        queued_combat_participant_names.erase(pending.participant.id);
        const auto sequence = tick_sequence.load(std::memory_order_relaxed);
        if (pending.next_retry_sequence > sequence) {
            queued_combat_participant_names.insert(pending.participant.id);
            pending_combat_participant_names.push_back(std::move(pending));
            return;
        }
        const auto retry_or_fail = [&]() noexcept {
            constexpr std::uint8_t kMaximumAttempts = 32;
            constexpr std::uint8_t kMaximumSceneReadyRetries = 4;
            if (++pending.attempts < kMaximumAttempts) {
                pending.next_retry_sequence = sequence + 15U;
                queued_combat_participant_names.insert(pending.participant.id);
                pending_combat_participant_names.push_back(std::move(pending));
            } else if (scene_monster_table_scan_complete &&
                       pending.scene_retry_attempts < kMaximumSceneReadyRetries) {
                ++pending.scene_retry_attempts;
                pending.attempts = kMaximumAttempts - 1U;
                pending.next_retry_sequence = sequence + 120U;
                queued_combat_participant_names.insert(pending.participant.id);
                pending_combat_participant_names.push_back(std::move(pending));
            } else {
                failed_combat_participant_names.insert(pending.participant.id);
            }
        };
        try {
            if (!pending.player_participant) {
                for (std::size_t index{}; index < pending.key_count; ++index) {
                    std::string value;
                    if (ResolveMonsterNameWithFallbackLocked(
                            pending.keys[index], pending.key_texts[index], value) &&
                        !value.empty()) {
                        combat_participant_names.emplace(
                            pending.participant.id, std::move(value));
                        return;
                    }
                }
            }
            std::uintptr_t object{};
            if (!ResolveObjectHandleLocked(pending.participant, object)) {
                retry_or_fail();
                return;
            }
            std::uintptr_t object_class{};
            if (!ReadPointerAt(
                    *memory, object, Layout(profile, "object.class"), object_class)) {
                retry_or_fail();
                return;
            }
            bool player_participant{};
            const auto try_main_character_id = [&](const NteFunctionKind kind) {
                const auto& binding = combat_skill_discovery.functions[NteIndex(kind)];
                if (!binding || !IsClassDerivedFromLocked(object_class, binding->outer_class)) {
                    return std::string{};
                }
                player_participant = true;
                std::uint64_t character_id{};
                std::string value;
                if (InvokeNteReturnLocked(kind, object, character_id) &&
                    character_id != 0 &&
                    ResolveDisplayNameFromTableLocked(0, character_id, {}, value) &&
                    !value.empty()) {
                    return value;
                }
                return std::string{};
            };
            for (const auto kind : {NteFunctionKind::GetMainCharacterId,
                                    NteFunctionKind::GetNpcMainCharacterId}) {
                auto value = try_main_character_id(kind);
                if (!value.empty()) {
                    combat_participant_names.emplace(
                        pending.participant.id, std::move(value));
                    return;
                }
            }
            if (!player_participant) {
                const auto name_offset = Layout(profile, "object.nameOffset", -1);
                std::uint32_t class_index{};
                std::uint32_t class_number{};
                if (name_offset >= 0 &&
                    ReadValue(*memory,
                        object_class + static_cast<std::uintptr_t>(name_offset),
                        class_index) &&
                    ReadValue(*memory,
                        object_class + static_cast<std::uintptr_t>(name_offset) +
                            sizeof(class_index),
                        class_number)) {
                    const auto class_key = static_cast<std::uint64_t>(class_index) |
                        (static_cast<std::uint64_t>(class_number) << 32U);
                    std::string class_text;
                    if (ResolveFNameLocked(
                            class_index, class_number, class_text) &&
                        !class_text.empty()) {
                        AppendParticipantNameCandidate(
                            pending, class_key, class_text);
                        std::string value;
                        if (ResolveMonsterNameWithFallbackLocked(
                                class_key, class_text, value) &&
                            !value.empty()) {
                            combat_participant_names.emplace(
                                pending.participant.id, std::move(value));
                            return;
                        }
                    }
                }
                const auto config_id_offset = Layout(
                    profile, "abilityCharacter.characterConfigId", -1);
                std::uint32_t config_index{};
                std::uint32_t config_number{};
                if (config_id_offset >= 0 &&
                    ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(config_id_offset),
                        config_index) &&
                    ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(config_id_offset) +
                            sizeof(config_index),
                        config_number)) {
                    const auto config_key = static_cast<std::uint64_t>(config_index) |
                        (static_cast<std::uint64_t>(config_number) << 32U);
                    if (config_key != 0) {
                        pending.config_id_key = config_key;
                        pending.has_config_id = true;
                        std::string config_text;
                        static_cast<void>(ResolveFNameLocked(
                            config_index, config_number, config_text));
                        AppendParticipantNameCandidate(
                            pending, config_key, config_text);
                        std::string value;
                        if (!config_text.empty()) {
                            if (ResolveMonsterNameWithFallbackLocked(
                                    config_key, config_text, value) &&
                                !value.empty()) {
                                combat_participant_names.emplace(
                                    pending.participant.id,
                                    std::move(value));
                                return;
                            }
                        }
                    }
                }
                if (pending.has_config_id && pending.config_id_key != 0) {
                    std::string value;
                    if (ResolveMonsterStaticDataNameLocked(
                            pending.config_id_key, value) &&
                        !value.empty()) {
                        combat_participant_names.emplace(
                            pending.participant.id, std::move(value));
                        return;
                    }
                }
            }
            const std::size_t table_index = player_participant ? 0U : 1U;
            for (std::size_t index{}; index < pending.key_count; ++index) {
                std::string value;
                if (ResolveDisplayNameFromTableLocked(
                        table_index, pending.keys[index],
                        pending.key_texts[index], value) &&
                    !value.empty()) {
                    combat_participant_names.emplace(
                        pending.participant.id, std::move(value));
                    return;
                }
            }
        } catch (...) {
        }
        retry_or_fail();
    }

    [[nodiscard]] bool ReadWeakHandleAtLocked(
        const std::uintptr_t address, AnomalyGenerationHandleV1& handle) const noexcept {
        std::int32_t index{-1};
        std::int32_t serial{};
        if (!ReadValue(*memory, address + static_cast<std::uintptr_t>(Layout(profile, "weakObject.index")), index) ||
            !ReadValue(*memory, address + static_cast<std::uintptr_t>(Layout(profile, "weakObject.serial")), serial)) {
            return false;
        }
        return WeakObjectHandleLocked(index, serial, handle);
    }

    void CacheCombatEventNameLocked(
        const std::uint64_t name_id, const std::uintptr_t object = 0) noexcept {
        if (name_id == 0) return;
        if (object != 0) {
            combat_event_objects.emplace(name_id, object);
        }
        if ((name_id & kDamageSourceBase) != 0 ||
            combat_event_names.contains(name_id) ||
            failed_combat_event_names.contains(name_id) ||
            queued_combat_event_names.contains(name_id)) {
            return;
        }
        try {
            queued_combat_event_names.insert(name_id);
            pending_combat_event_names.push_back(name_id);
            if (!display_table_scan_complete) {
                display_name_demand.store(true, std::memory_order_release);
            }
        } catch (...) {
            queued_combat_event_names.erase(name_id);
        }
    }

    void MarkCombatEventNameResolvedLocked(const std::uint64_t name_id) noexcept {
        for (std::size_t index{}; index < combat_event_count; ++index) {
            auto& event = combat_events[
                (combat_event_start + index) % kCombatEventCapacity].event;
            if (event.name_id == name_id) {
                event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
            }
        }
    }

    void ResolveNextCombatEventNameLocked() noexcept {
        if (!display_table_scan_complete || pending_combat_event_names.empty()) return;
        const std::uint64_t name_id = pending_combat_event_names.front();
        pending_combat_event_names.pop_front();
        queued_combat_event_names.erase(name_id);
        try {
            std::string value;
            const auto object = combat_event_objects.find(name_id);
            if (object != combat_event_objects.end() && object->second != 0) {
                std::uint32_t comparison_index{};
                std::uint32_t number{};
                const auto name_offset = Layout(profile, "object.nameOffset", -1);
                if (name_offset >= 0 &&
                    ReadValue(*memory,
                        object->second + static_cast<std::uintptr_t>(name_offset),
                        comparison_index) &&
                    ReadValue(*memory,
                        object->second + static_cast<std::uintptr_t>(name_offset) +
                            sizeof(comparison_index),
                        number)) {
                    const auto object_key = static_cast<std::uint64_t>(comparison_index) |
                        (static_cast<std::uint64_t>(number) << 32U);
                    std::string object_name;
                    static_cast<void>(ResolveFNameLocked(
                        comparison_index, number, object_name));
                    static_cast<void>(ResolveIndexedDisplayNameLocked(
                        object_key, object_name, value));
                }
            } else {
                static_cast<void>(ResolveIndexedDisplayNameLocked(name_id, {}, value));
            }
            if (!value.empty()) {
                combat_event_names.emplace(name_id, std::move(value));
                MarkCombatEventNameResolvedLocked(name_id);
                return;
            }
        } catch (...) {
        }
        failed_combat_event_names.insert(name_id);
    }

    [[nodiscard]] bool ReadSparseMapViewLocked(
        const std::uintptr_t map,
        SparseMapView& view) const noexcept {
        const auto row_map_offset = Layout(profile, "dataTable.rowMap", -1);
        const auto data_offset = Layout(profile, "dataTable.rowMapData", 0);
        const auto num_offset = Layout(profile, "dataTable.rowMapNum", 8);
        const auto num_free_offset = Layout(profile, "dataTable.rowMapNumFree", 52);
        const auto max_offset = Layout(profile, "dataTable.rowMapMax", 12);
        const auto stride = Layout(profile, "dataTable.rowMapElementStride", 24);
        const auto row_offset = Layout(profile, "dataTable.rowMapRowOffset", 8);
        const auto flags_data_offset = Layout(profile, "dataTable.rowMapFlagsData", 32);
        const auto flags_num_offset = Layout(profile, "dataTable.rowMapFlagsNum", 40);
        const auto flags_max_offset = Layout(profile, "dataTable.rowMapFlagsMax", 44);
        const auto inline_flags_offset = Layout(profile, "dataTable.rowMapInlineFlags", 16);
        if (map == 0 || row_map_offset < 0 || data_offset < 0 || num_offset < 0 ||
            num_free_offset < 0 ||
            max_offset < 0 || flags_data_offset < 0 || flags_num_offset < 0 ||
            flags_max_offset < 0 || inline_flags_offset < 0 || stride < 16 ||
            stride > 128 || row_offset < 0 ||
            row_offset + static_cast<std::int64_t>(sizeof(std::uintptr_t)) > stride) {
            return false;
        }
        SparseMapView next;
        next.map = map;
        next.stride = stride;
        next.row_offset = row_offset;
        if (!ReadValue(*memory, map + static_cast<std::uintptr_t>(data_offset), next.data) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(num_offset), next.num) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(num_free_offset), next.num_free) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(max_offset), next.max) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_data_offset), next.flags_data) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_num_offset), next.flags_num) ||
            !ReadValue(*memory, map + static_cast<std::uintptr_t>(flags_max_offset), next.flags_max) ||
            next.num < 0 || next.num_free < 0 || next.num_free > next.num ||
            next.num > next.max || next.max > 4096 || next.flags_num < next.num ||
            next.flags_max < next.flags_num || next.flags_num > 4096 ||
            (next.num != 0 && next.data == 0)) {
            return false;
        }
        if (next.num == 0) {
            view = std::move(next);
            return true;
        }
        const auto word_count = static_cast<std::size_t>((next.flags_num + 31) / 32);
        if (word_count == 0 || word_count > 128) return false;
        next.flags.resize(word_count);
        if (next.flags_data != 0) {
            if (!memory->Read(next.flags_data, next.flags.data(),
                              next.flags.size() * sizeof(std::uint32_t))) {
                return false;
            }
        } else if (word_count > 4 || !memory->Read(
                       map + static_cast<std::uintptr_t>(inline_flags_offset),
                       next.flags.data(), next.flags.size() * sizeof(std::uint32_t))) {
            return false;
        }
        view = std::move(next);
        return true;
    }

    void CacheLocalizedNameLocked(
        const std::uint64_t key,
        const std::string_view localized) {
        if (key == 0 || localized.empty()) return;
        localized_names_by_fname[key] = std::string(localized);
        std::string name;
        if (ResolveFNameLocked(
                static_cast<std::uint32_t>(key & 0xFFFFFFFFU),
                static_cast<std::uint32_t>(key >> 32U), name) && !name.empty()) {
            const auto add = [this, &localized](std::string value) {
                if (!value.empty()) localized_names_by_key[std::move(value)] = std::string(localized);
            };
            add(name);
            if (name.starts_with("Default__")) {
                name.erase(0, std::string_view{"Default__"}.size());
                add(name);
            }
            if (name.ends_with("_C")) add(name.substr(0, name.size() - 2U));
            for (const std::string_view prefix : {std::string_view{"GA_"},
                                                   std::string_view{"BP_"},
                                                   std::string_view{"GE_"},
                                                   std::string_view{"Buff_"}}) {
                if (name.starts_with(prefix)) add(name.substr(prefix.size()));
            }
        }
    }

    [[nodiscard]] bool BuildDisplayTableIndexLocked(
        const std::uintptr_t table,
        const std::string_view text_property,
        const std::int64_t text_offset,
        const std::int64_t alias_offset,
        const bool add_monster_identity_alias,
        DisplayTableIndex& index) {
        if (table == 0 || text_offset < 0) return false;
        if (index.table == table && index.text_offset == text_offset &&
            !index.rows_by_fname.empty()) {
            return true;
        }
        std::uintptr_t row_struct{};
        if (text_offset > (std::numeric_limits<std::int32_t>::max)() ||
            !ReadPointerAt(*memory, table, Layout(profile, "dataTable.rowStruct", -1), row_struct) ||
            !ValidateStructFieldLocked(row_struct,
                {text_property, "TextProperty", {}, static_cast<std::int32_t>(text_offset), 16}, true)) {
            return false;
        }
        const auto row_map_offset = Layout(profile, "dataTable.rowMap", -1);
        SparseMapView map;
        if (row_map_offset < 0 ||
            !ReadSparseMapViewLocked(
                table + static_cast<std::uintptr_t>(row_map_offset), map) ||
            map.num <= 0) {
            return false;
        }
        const auto element_bytes = static_cast<std::size_t>(map.num) *
            static_cast<std::size_t>(map.stride);
        std::vector<std::uint8_t> elements(element_bytes);
        if (!memory->Read(map.data, elements.data(), elements.size())) return false;

        DisplayTableIndex next;
        next.table = table;
        next.text_offset = text_offset;
        next.rows_by_fname.reserve(static_cast<std::size_t>(map.num - map.num_free));
        next.rows_by_key.reserve(static_cast<std::size_t>(map.num - map.num_free));
        for (std::int32_t slot{}; slot < map.num; ++slot) {
            const auto unsigned_slot = static_cast<std::uint32_t>(slot);
            if ((map.flags[static_cast<std::size_t>(unsigned_slot) / 32U] &
                    (1U << (unsigned_slot & 31U))) == 0) {
                continue;
            }
            const auto* element = elements.data() +
                static_cast<std::size_t>(slot) * static_cast<std::size_t>(map.stride);
            std::uint32_t comparison_index{};
            std::uint32_t number{};
            std::uintptr_t row{};
            std::memcpy(&comparison_index, element, sizeof(comparison_index));
            std::memcpy(&number, element + sizeof(comparison_index), sizeof(number));
            std::memcpy(&row, element + map.row_offset, sizeof(row));
            if (row == 0) continue;
            const auto key = static_cast<std::uint64_t>(comparison_index) |
                (static_cast<std::uint64_t>(number) << 32U);
            next.rows_by_fname.emplace(key, row);
            std::string key_text;
            if (ResolveFNameLocked(comparison_index, number, key_text) && !key_text.empty()) {
                if (add_monster_identity_alias) {
                    const auto monster_identity = MonsterDisplayIdentity(key_text);
                    if (!monster_identity.empty()) {
                        next.rows_by_key.emplace(monster_identity, row);
                    }
                }
                AddDisplayRowAliases(next, std::move(key_text), row);
            }
            if (alias_offset >= 0) {
                std::uint32_t alias_index{};
                std::uint32_t alias_number{};
                if (ReadValue(*memory,
                        row + static_cast<std::uintptr_t>(alias_offset),
                        alias_index) &&
                    ReadValue(*memory,
                        row + static_cast<std::uintptr_t>(alias_offset) +
                            sizeof(alias_index),
                        alias_number)) {
                    const auto alias = static_cast<std::uint64_t>(alias_index) |
                        (static_cast<std::uint64_t>(alias_number) << 32U);
                    if (alias != 0) {
                        next.rows_by_fname.emplace(alias, row);
                        std::string alias_text;
                        if (ResolveFNameLocked(
                                alias_index, alias_number, alias_text) &&
                            !alias_text.empty()) {
                            AddDisplayRowAliases(next, std::move(alias_text), row);
                        }
                    }
                }
            }
        }
        if (next.rows_by_fname.empty()) return false;
        index = std::move(next);
        return true;
    }

    static void AddDamageSkillAliases(
        DamageSkillIndex& index,
        std::string name,
        const std::uint64_t ability_key) {
        const auto add = [&index, ability_key](const std::string_view key) {
            if (!key.empty()) {
                index.ability_by_effect_key.emplace(std::string(key), ability_key);
            }
        };
        add(name);
        if (name.starts_with("Default__")) {
            name.erase(0, std::string_view{"Default__"}.size());
            add(name);
        }
        if (name.ends_with("_C")) {
            name.resize(name.size() - 2U);
            add(name);
        }
        if (name.starts_with("GE_")) add(std::string_view(name).substr(3U));
    }

    [[nodiscard]] bool BuildDamageSkillIndexLocked(
        const std::uintptr_t table,
        const std::int64_t ability_name_offset) {
        if (table == 0 || ability_name_offset < 0) return false;
        if (damage_skill_index.table == table &&
            !damage_skill_index.ability_by_effect_fname.empty()) {
            return true;
        }
        const auto row_map_offset = Layout(profile, "dataTable.rowMap", -1);
        SparseMapView map;
        if (row_map_offset < 0 ||
            !ReadSparseMapViewLocked(
                table + static_cast<std::uintptr_t>(row_map_offset), map) ||
            map.num <= 0) {
            return false;
        }
        const auto element_bytes = static_cast<std::size_t>(map.num) *
            static_cast<std::size_t>(map.stride);
        std::vector<std::uint8_t> elements(element_bytes);
        if (!memory->Read(map.data, elements.data(), elements.size())) return false;

        DamageSkillIndex next;
        next.table = table;
        next.ability_by_effect_fname.reserve(
            static_cast<std::size_t>(map.num - map.num_free));
        next.ability_by_effect_key.reserve(
            static_cast<std::size_t>(map.num - map.num_free));
        for (std::int32_t slot{}; slot < map.num; ++slot) {
            const auto unsigned_slot = static_cast<std::uint32_t>(slot);
            if ((map.flags[static_cast<std::size_t>(unsigned_slot) / 32U] &
                    (1U << (unsigned_slot & 31U))) == 0) {
                continue;
            }
            const auto* element = elements.data() +
                static_cast<std::size_t>(slot) * static_cast<std::size_t>(map.stride);
            std::uint32_t effect_index{};
            std::uint32_t effect_number{};
            std::uintptr_t row{};
            std::memcpy(&effect_index, element, sizeof(effect_index));
            std::memcpy(&effect_number,
                element + sizeof(effect_index), sizeof(effect_number));
            std::memcpy(&row, element + map.row_offset, sizeof(row));
            if (row == 0) continue;
            std::uint32_t ability_index{};
            std::uint32_t ability_number{};
            if (!ReadValue(*memory,
                    row + static_cast<std::uintptr_t>(ability_name_offset),
                    ability_index) ||
                !ReadValue(*memory,
                    row + static_cast<std::uintptr_t>(ability_name_offset) +
                        sizeof(ability_index),
                    ability_number)) {
                continue;
            }
            const auto effect_key = static_cast<std::uint64_t>(effect_index) |
                (static_cast<std::uint64_t>(effect_number) << 32U);
            const auto ability_key = static_cast<std::uint64_t>(ability_index) |
                (static_cast<std::uint64_t>(ability_number) << 32U);
            if (effect_key == 0 || ability_key == 0) continue;
            next.ability_by_effect_fname.emplace(effect_key, ability_key);
            std::string effect_name;
            if (ResolveFNameLocked(effect_index, effect_number, effect_name) &&
                !effect_name.empty()) {
                AddDamageSkillAliases(next, std::move(effect_name), ability_key);
            }
        }
        if (next.ability_by_effect_fname.empty()) return false;
        damage_skill_index = std::move(next);
        return true;
    }

    [[nodiscard]] bool FindDamageSkillAbilityLocked(
        const std::uint64_t effect_key,
        const std::string_view effect_name,
        std::uint64_t& ability_key) const {
        ability_key = 0;
        if (effect_key != 0) {
            const auto found =
                damage_skill_index.ability_by_effect_fname.find(effect_key);
            if (found != damage_skill_index.ability_by_effect_fname.end()) {
                ability_key = found->second;
                return true;
            }
        }
        if (effect_name.empty()) return false;
        std::string normalized(effect_name);
        if (normalized.starts_with("Default__")) normalized.erase(0, 9U);
        if (normalized.ends_with("_C")) normalized.resize(normalized.size() - 2U);
        const auto find = [this, &ability_key](const std::string_view candidate) {
            const auto found = damage_skill_index.ability_by_effect_key.find(
                std::string(candidate));
            if (found == damage_skill_index.ability_by_effect_key.end()) return false;
            ability_key = found->second;
            return true;
        };
        if (find(normalized)) return true;
        return normalized.starts_with("GE_") &&
            find(std::string_view(normalized).substr(3U));
    }

    static void AddDisplayRowAliases(
        DisplayTableIndex& index,
        std::string name,
        const std::uintptr_t row) {
        const auto add = [&index, row](const std::string_view key) {
            if (!key.empty()) index.rows_by_key.emplace(std::string(key), row);
        };
        add(name);
        if (name.starts_with("Default__")) {
            name.erase(0, std::string_view{"Default__"}.size());
            add(name);
        }
        if (name.ends_with("_C")) {
            name.resize(name.size() - 2U);
            add(name);
        }
        for (const std::string_view prefix : {std::string_view{"GA_"},
                                               std::string_view{"GE_"},
                                               std::string_view{"Buff_"},
                                               std::string_view{"BP_"}}) {
            if (name.starts_with(prefix)) {
                add(std::string_view(name).substr(prefix.size()));
            }
        }
    }

    [[nodiscard]] static std::string MonsterDisplayIdentity(
        const std::string_view source) {
        return NteMonsterDisplayIdentity(source);
    }

    [[nodiscard]] static bool MonsterIdentityMatches(
        const std::string_view candidate,
        const std::string_view identity) {
        if (candidate.empty() || identity.empty()) return false;
        return MonsterDisplayIdentity(candidate) == identity;
    }

    [[nodiscard]] bool ResolveActorMonsterNameLocked(
        const std::string_view source, std::string& value, const bool base_only = false) const {
        value.clear();
        for (const auto& key : NteMonsterStringTableKeys(source, base_only)) {
            if (ResolveStringTableEntryLocked(key, value) && !value.empty()) return true;
        }
        return false;
    }

    [[nodiscard]] static std::string AbyssStringTableMonsterKey(
        const std::string_view source) {
        std::string key(source);
        if (key.starts_with("Default__")) key.erase(0, 9U);
        if (key.ends_with("_C")) key.resize(key.size() - 2U);
        const auto marker = key.find("_BP_");
        if (marker != std::string::npos) {
            key.resize(marker + 3U);
            return key;
        }
        if (key.ends_with("_BP")) return key;
        return {};
    }

    [[nodiscard]] bool FindDisplayRowLocked(
        const std::uint64_t key,
        const std::string_view key_text,
        std::uintptr_t& row,
        std::int64_t& text_offset) const {
        row = 0;
        text_offset = -1;
        if (key != 0) {
            for (const auto& index : display_table_indexes) {
                const auto found = index.rows_by_fname.find(key);
                if (found != index.rows_by_fname.end()) {
                    row = found->second;
                    text_offset = index.text_offset;
                    return true;
                }
            }
        }
        if (key_text.empty()) return false;
        std::string normalized(key_text);
        if (normalized.starts_with("Default__")) {
            normalized.erase(0, std::string_view{"Default__"}.size());
        }
        if (normalized.ends_with("_C")) normalized.resize(normalized.size() - 2U);
        const auto find_text = [this, &row, &text_offset](const std::string_view candidate) {
            for (const auto& index : display_table_indexes) {
                const auto found = index.rows_by_key.find(std::string(candidate));
                if (found == index.rows_by_key.end()) continue;
                row = found->second;
                text_offset = index.text_offset;
                return true;
            }
            return false;
        };
        if (find_text(normalized)) return true;
        if (const auto monster_identity = MonsterDisplayIdentity(normalized);
            !monster_identity.empty()) {
            for (std::size_t index{}; index < display_table_indexes.size(); ++index) {
                if (index != 1U) continue;
                for (const auto& [candidate, candidate_row] :
                         display_table_indexes[index].rows_by_key) {
                    if (!MonsterIdentityMatches(candidate, monster_identity)) continue;
                    row = candidate_row;
                    text_offset = display_table_indexes[index].text_offset;
                    return true;
                }
            }
        }
        for (const std::string_view prefix : {std::string_view{"GA_"},
                                               std::string_view{"GE_"},
                                               std::string_view{"Buff_"},
                                               std::string_view{"BP_"}}) {
            if (normalized.starts_with(prefix) &&
                find_text(std::string_view(normalized).substr(prefix.size()))) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool ResolveDisplayNameFromTableLocked(
        const std::size_t table_index,
        const std::uint64_t key,
        const std::string_view key_text,
        std::string& value) {
        value.clear();
        if (table_index >= display_table_indexes.size()) return false;
        const auto& index = display_table_indexes[table_index];
        std::uintptr_t row{};
        if (key != 0) {
            const auto found = index.rows_by_fname.find(key);
            if (found != index.rows_by_fname.end()) row = found->second;
        }
        const auto find_text = [&index, &row](const std::string_view candidate) {
            const auto found = index.rows_by_key.find(std::string(candidate));
            if (found == index.rows_by_key.end()) return false;
            row = found->second;
            return true;
        };
        if (row == 0 && !key_text.empty()) {
            std::string normalized(key_text);
            if (normalized.starts_with("Default__")) {
                normalized.erase(0, std::string_view{"Default__"}.size());
            }
            if (normalized.ends_with("_C")) normalized.resize(normalized.size() - 2U);
            if (!find_text(normalized)) {
                bool found_alias{};
                if (table_index == 1U) {
                    const auto monster_identity = MonsterDisplayIdentity(normalized);
                    found_alias = !monster_identity.empty() && find_text(monster_identity);
                    if (!found_alias && !monster_identity.empty()) {
                        for (const auto& [candidate, candidate_row] : index.rows_by_key) {
                            if (!MonsterIdentityMatches(candidate, monster_identity)) continue;
                            row = candidate_row;
                            found_alias = true;
                            break;
                        }
                    }
                }
                if (!found_alias) {
                    for (const std::string_view prefix : {std::string_view{"GA_"},
                                                           std::string_view{"GE_"},
                                                           std::string_view{"Buff_"},
                                                           std::string_view{"BP_"}}) {
                        if (normalized.starts_with(prefix) &&
                            find_text(std::string_view(normalized).substr(prefix.size()))) {
                            break;
                        }
                    }
                }
            }
        }
        const auto try_row = [this, &index, &value](const std::uintptr_t candidate) {
            std::uintptr_t ftext_address{};
            return candidate != 0 && index.text_offset >= 0 &&
                AddAddress(candidate, index.text_offset, ftext_address) &&
                ResolveFTextLocked(ftext_address, value) && !value.empty();
        };
        if (try_row(row)) return true;
        if (table_index == 1U && !key_text.empty()) {
            std::string normalized(key_text);
            if (normalized.starts_with("Default__")) {
                normalized.erase(0, std::string_view{"Default__"}.size());
            }
            if (normalized.ends_with("_C")) normalized.resize(normalized.size() - 2U);
            const auto identity = MonsterDisplayIdentity(normalized);
            if (!identity.empty()) {
                for (const auto& [candidate, candidate_row] : index.rows_by_key) {
                    if (candidate_row == row || !MonsterIdentityMatches(candidate, identity)) {
                        continue;
                    }
                    if (try_row(candidate_row)) return true;
                }
            }
        }
        value.clear();
        return false;
    }

    [[nodiscard]] bool ResolveIndexedDisplayNameLocked(
        const std::uint64_t key,
        const std::string_view supplied_key_text,
        std::string& value) {
        value.clear();
        if (key != 0) {
            const auto cached = localized_names_by_fname.find(key);
            if (cached != localized_names_by_fname.end() && !cached->second.empty()) {
                value = cached->second;
                return true;
            }
        }
        if (!supplied_key_text.empty()) {
            const auto cached = localized_names_by_key.find(
                std::string(supplied_key_text));
            if (cached != localized_names_by_key.end() && !cached->second.empty()) {
                value = cached->second;
                return true;
            }
        }
        std::string key_text(supplied_key_text);
        if (key_text.empty()) {
            static_cast<void>(ResolveFNameLocked(
                static_cast<std::uint32_t>(key & 0xFFFFFFFFU),
                static_cast<std::uint32_t>(key >> 32U), key_text));
        }
        std::uintptr_t row{};
        std::int64_t text_offset{};
        std::uintptr_t ftext_address{};
        if (!FindDisplayRowLocked(key, key_text, row, text_offset) ||
            text_offset < 0 || !AddAddress(row, text_offset, ftext_address) ||
            !ResolveFTextLocked(ftext_address, value) || value.empty()) {
            display_name_demand.store(true, std::memory_order_release);
            return false;
        }
        if (key != 0) {
            CacheLocalizedNameLocked(key, value);
        } else if (!key_text.empty()) {
            localized_names_by_key[std::move(key_text)] = value;
        }
        return true;
    }

    void RefreshDisplayTablesLocked(const std::uint64_t sequence) noexcept {
        if (display_table_generation != object_generation) {
            display_table_generation = object_generation;
            display_table_scan_complete = false;
            display_table_loaded_mask = 0;
            display_game_data = 0;
            display_game_data_generation = object_generation;
            display_table_indexes = {};
            damage_skill_index = {};
        }
        static_cast<void>(sequence);
        if (!display_name_demand.exchange(false, std::memory_order_acq_rel) ||
            display_table_scan_complete ||
            object_registry.items == 0 || object_registry.count == 0) {
            return;
        }
        try {
            if (display_game_data_generation != object_generation) {
                display_game_data = 0;
                display_game_data_generation = object_generation;
            }
            if (display_game_data == 0 && !ResolveGameDataLocked(display_game_data)) return;
            struct TableSpec {
                std::size_t slot;
                std::string_view table_key;
                std::string_view text_property;
                std::string_view text_key;
                std::string_view alias_key;
                std::int64_t alias_additional{};
                std::uint8_t bit;
            };
            static constexpr std::array specs{
                TableSpec{0, "gameData.characterDataTable", "ItemName", "staticItemData.itemName",
                    {}, 0, 0x01U},
                // Slot 1 was an icon-only StaticMonsterInfo table, not a name source.
                TableSpec{2, "gameData.gameplayAbilityTipsDataTable", "Name", "gameplayAbilityTips.name",
                    "gameplayAbilityTips.gameplayAbility", 24, 0x04U},
                TableSpec{3, "gameData.gameplayEffectTipsDataTable", "Name", "gameplayEffectTips.name",
                    "gameplayEffectTips.geParamName", 0, 0x08U},
            };
            for (const auto& spec : specs) {
                if ((display_table_loaded_mask & spec.bit) != 0) continue;
                std::uintptr_t table{};
                const auto table_offset = Layout(profile, spec.table_key, -1);
                const auto alias_base = spec.alias_key.empty()
                    ? -1 : Layout(profile, spec.alias_key, -1);
                const auto alias_offset = alias_base < 0
                    ? -1 : alias_base + spec.alias_additional;
                if (table_offset < 0 ||
                    !ReadPointerAt(*memory, display_game_data, table_offset, table) ||
                    table == 0 ||
                    !BuildDisplayTableIndexLocked(
                        table, spec.text_property, Layout(profile, spec.text_key, -1), alias_offset,
                        false, display_table_indexes[spec.slot])) {
                    continue;
                }
                display_table_loaded_mask |= spec.bit;
            }
            if ((display_table_loaded_mask & 0x10U) == 0) {
                std::uintptr_t ability_data{};
                std::uintptr_t skill_damage_table{};
                if (ReadPointerAt(*memory, display_game_data,
                        Layout(profile, "gameData.abilityDataAsset"), ability_data) &&
                    ability_data != 0 &&
                    ReadPointerAt(*memory, ability_data,
                        Layout(profile, "abilityData.skillDamageDataTable"),
                        skill_damage_table) &&
                    skill_damage_table != 0 &&
                    BuildDamageSkillIndexLocked(
                        skill_damage_table, Layout(profile, "skillDamage.gaName", -1))) {
                    display_table_loaded_mask |= 0x10U;
                }
            }
            display_table_scan_complete = display_table_loaded_mask == 0x1DU;
        } catch (...) {
        }
    }

    [[nodiscard]] static bool CombatHandleCompatible(
        const AnomalyGenerationHandleV1 left,
        const AnomalyGenerationHandleV1 right) noexcept {
        return left.id == 0 || right.id == 0 ||
            (left.id == right.id && left.generation == right.generation);
    }

    // Damage text already contains the complete weak-object pair. Validate it
    // only when a consumer resolves the participant; walking the object table
    // for both sides of every hit stalls the game-thread tick.
    [[nodiscard]] AnomalyGenerationHandleV1 WeakHandleFromBytesLocked(
        const std::int32_t index, const std::int32_t serial) const noexcept {
        if (index < 0 || serial <= 0) return {};
        return {EncodeObjectHandle(
                    static_cast<std::uint32_t>(index),
                    static_cast<std::uint32_t>(serial)),
                object_generation};
    }

    [[nodiscard]] bool MergeCombatDamageLocked(
        const AnomalyNteCombatEventV1& incoming) noexcept {
        const bool incoming_display =
            (incoming.flags & ANOMALY_NTE_COMBAT_EVENT_V1_DISPLAY_VALID) != 0;
        constexpr std::size_t kMaximumMergeLookback = 64;
        const std::size_t lookback = (std::min)(combat_event_count, kMaximumMergeLookback);
        for (std::size_t index = combat_event_count;
             index > combat_event_count - lookback; --index) {
            auto& candidate = combat_events[
                (combat_event_start + index - 1U) % kCombatEventCapacity].event;
            if (candidate.kind != ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE ||
                candidate.tick_sequence != incoming.tick_sequence) {
                continue;
            }
            const bool same_participants =
                CombatHandleCompatible(candidate.source, incoming.source) &&
                CombatHandleCompatible(candidate.target, incoming.target);
            const bool same_source_name = candidate.name_id != 0 &&
                incoming.name_id != 0 && candidate.name_id == incoming.name_id;
            if (!same_participants && !same_source_name) continue;
            const bool candidate_display =
                (candidate.flags & ANOMALY_NTE_COMBAT_EVENT_V1_DISPLAY_VALID) != 0;
            if (candidate.value == incoming.value &&
                candidate.basic_value == incoming.basic_value &&
                candidate.final_value == incoming.final_value &&
                candidate.damage_type == incoming.damage_type &&
                candidate.display_type == incoming.display_type &&
                candidate.reaction_type == incoming.reaction_type &&
                candidate.reaction_display_type == incoming.reaction_display_type &&
                candidate_display == incoming_display &&
                candidate.name_id == incoming.name_id) {
                // CharacterOnDamaged and the UI multicast can both emit the
                // same hit. Collapse exact duplicates before publishing the
                // event so the demo never shows damage twice.
                return true;
            }
            if (candidate_display == incoming_display) {
                // Both native damage delegates and both floaties delegates can
                // publish the same hit. Participant compatibility plus the
                // complete numeric payload distinguishes it from a real
                // multi-hit that happens in the same game tick.
                if (candidate.value == incoming.value &&
                    candidate.basic_value == incoming.basic_value &&
                    candidate.final_value == incoming.final_value &&
                    candidate.damage_type == incoming.damage_type &&
                    candidate.display_type == incoming.display_type &&
                    candidate.reaction_type == incoming.reaction_type &&
                    candidate.reaction_display_type == incoming.reaction_display_type) {
                    return true;
                }
                continue;
            }
            if (candidate.source.id == 0) candidate.source = incoming.source;
            if (candidate.target.id == 0) candidate.target = incoming.target;
            candidate.flags |= incoming.flags;
            // The native event uses a generation-local GameplayEffect source
            // id. Damage text carries the stable InjurySourceName FName used by
            // the indexed localized tables, so it is the authoritative display
            // key even when its FText is materialized later in this tick.
            if (incoming.name_id != 0) {
                const auto incoming_name = combat_event_names.find(incoming.name_id);
                const auto candidate_name = combat_event_names.find(candidate.name_id);
                const bool incoming_resolved = incoming_name != combat_event_names.end() &&
                    !incoming_name->second.empty();
                const bool candidate_resolved = candidate_name != combat_event_names.end() &&
                    !candidate_name->second.empty();
                const bool incoming_is_object_source =
                    (incoming.name_id & kDamageSourceBase) != 0;
                const bool candidate_is_object_source =
                    (candidate.name_id & kDamageSourceBase) != 0;
                if (incoming_is_object_source && !candidate_is_object_source &&
                    candidate.name_id != 0) {
                    const auto source = damage_source_objects.find(incoming.name_id);
                    if (source != damage_source_objects.end()) {
                        combat_event_objects[candidate.name_id] = source->second;
                    }
                } else if (candidate.name_id == 0 ||
                    (!incoming_is_object_source && candidate_is_object_source) ||
                    (incoming_resolved && !candidate_resolved)) {
                    candidate.name_id = incoming.name_id;
                }
                if (!incoming_is_object_source &&
                    incoming_name == combat_event_names.end()) {
                    CacheCombatEventNameLocked(incoming.name_id);
                }
            }
            if (incoming_display) {
                candidate.value = incoming.value;
                candidate.basic_value = incoming.basic_value;
                candidate.final_value = incoming.final_value;
                candidate.damage_type = incoming.damage_type;
                candidate.display_type = incoming.display_type;
                candidate.reaction_type = incoming.reaction_type;
                candidate.reaction_display_type = incoming.reaction_display_type;
            } else {
                candidate.final_value = incoming.final_value;
                if (candidate.basic_value == 0) candidate.basic_value = incoming.basic_value;
                if (candidate.value == 0) candidate.value = incoming.value;
            }
            return true;
        }
        return false;
    }

    template <typename Value>
    [[nodiscard]] static bool ReadCaptureBytes(
        const std::span<const std::uint8_t> bytes,
        const std::int64_t offset,
        Value& destination) noexcept {
        if (offset < 0 || static_cast<std::uint64_t>(offset) > bytes.size() ||
            sizeof(Value) > bytes.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(
            &destination, bytes.data() + static_cast<std::size_t>(offset), sizeof(Value));
        return true;
    }

    [[nodiscard]] bool DamageTagsContainCriticalLocked(
        const NteDamageTags& tags) noexcept {
        const auto is_critical = [](std::string_view tag) noexcept {
            std::string normalized(tag);
            for (char& character : normalized) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            return normalized == "critical" || normalized == "criticalhit" ||
                normalized.ends_with(".critical") ||
                normalized.ends_with(".criticalhit") || normalized.ends_with(".crit");
        };
        for (const auto key : tags.Values()) {
            const auto cached = damage_critical_tag_cache.find(key);
            if (cached != damage_critical_tag_cache.end()) {
                if (cached->second) return true;
                continue;
            }
            std::string tag;
            const bool critical = ResolveFNameLocked(
                static_cast<std::uint32_t>(key),
                static_cast<std::uint32_t>(key >> 32U), tag) && is_critical(tag);
            damage_critical_tag_cache.emplace(key, critical);
            if (critical) return true;
        }
        return false;
    }

    void EnrichDamageRecordLocked(
        const AnomalyNteCombatEventV1& display_event) noexcept {
        if (display_event.tick_sequence == 0 ||
            (display_event.source.id == 0 && display_event.target.id == 0)) {
            return;
        }
        const std::size_t lookback = (std::min)(damage_event_count, std::size_t{64});
        for (std::size_t index = damage_event_count;
             index > damage_event_count - lookback; --index) {
            auto& candidate = damage_events[
                (damage_event_start + index - 1U) % kDamageEventCapacity].event;
            if (candidate.tick_sequence != display_event.tick_sequence ||
                (display_event.source.id != 0 &&
                    !SameHandle(candidate.attacker, display_event.source)) ||
                (display_event.target.id != 0 &&
                    !SameHandle(candidate.victim, display_event.target))) {
                continue;
            }
            candidate.display_damage = display_event.value;
            candidate.basic_damage = display_event.basic_value;
            candidate.final_damage = display_event.final_value;
            candidate.damage_type = display_event.damage_type;
            candidate.display_type = display_event.display_type;
            candidate.reaction_type = display_event.reaction_type;
            candidate.reaction_display_type = display_event.reaction_display_type;
            if ((display_event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL) != 0) {
                candidate.flags |= ANOMALY_NTE_DAMAGE_V1_CRITICAL;
            }
            if ((display_event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID) != 0) {
                candidate.flags |= ANOMALY_NTE_DAMAGE_V1_CRITICAL_VALID;
            }
            if ((display_event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_HEAD_HIT) != 0) {
                candidate.flags |= ANOMALY_NTE_DAMAGE_V1_HEAD_HIT;
            }
            if ((display_event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_WEAK_UNBALANCE) != 0) {
                candidate.flags |= ANOMALY_NTE_DAMAGE_V1_WEAK_UNBALANCE;
            }
            if (candidate.source_id != 0 && display_event.name_id != 0) {
                const auto source = damage_source_objects.find(candidate.source_id);
                if (source != damage_source_objects.end()) {
                    combat_event_objects[display_event.name_id] = source->second;
                }
            }
            return;
        }
    }

    void CaptureDamageTextBytesLocked(
        const std::span<const std::uint8_t> parameters,
        const std::uint16_t info_offset,
        const std::uint64_t capture_tick_sequence) noexcept {
        if (info_offset > parameters.size() ||
            72U > parameters.size() - static_cast<std::size_t>(info_offset)) return;
        const auto bytes = parameters.subspan(info_offset, 72U);
        const auto read_at = [&bytes](const std::int64_t offset, auto& destination) noexcept {
            using Value = std::remove_reference_t<decltype(destination)>;
            if (offset < 0 || static_cast<std::uint64_t>(offset) > bytes.size() ||
                sizeof(Value) > bytes.size() - static_cast<std::size_t>(offset)) {
                return false;
            }
            std::memcpy(
                &destination, bytes.data() + static_cast<std::size_t>(offset), sizeof(Value));
            return true;
        };
        const auto read_weak = [&read_at, this](
                                   const std::int64_t offset,
                                   AnomalyGenerationHandleV1& handle) noexcept {
            std::int32_t index{-1};
            std::int32_t serial{};
            const auto index_offset = Layout(profile, "weakObject.index", -1);
            const auto serial_offset = Layout(profile, "weakObject.serial", -1);
            if (index_offset < 0 || serial_offset < 0 ||
                !read_at(offset + index_offset, index) ||
                !read_at(offset + serial_offset, serial)) {
                return false;
            }
            handle = WeakHandleFromBytesLocked(index, serial);
            return handle.id != 0;
        };
        AnomalyNteCombatEventV1 event{};
        event.kind = ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE;
        event.tick_sequence = capture_tick_sequence;
        event.world = {1, world_generation};
        std::int32_t display_damage{};
        std::uint8_t damage_type{};
        std::uint8_t critical{};
        std::uint8_t head_hit{};
        std::uint8_t weak_unbalance{};
        std::uint8_t display_type{};
        std::uint8_t reaction_type{};
        std::uint8_t reaction_display_type{};
        std::int32_t basic_damage{};
        std::int32_t final_damage{};
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.displayDamage"), display_damage));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.damageType"), damage_type));
        const bool critical_valid = read_at(Layout(profile, "damageTextInfo.critical"), critical);
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.headHit"), head_hit));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.weakUnbalance"), weak_unbalance));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.displayType"), display_type));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.reactionType"), reaction_type));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.reactionDisplayType"), reaction_display_type));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.basicDamage"), basic_damage));
        static_cast<void>(read_at(Layout(profile, "damageTextInfo.finalDamage"), final_damage));
        static_cast<void>(read_weak(Layout(profile, "damageTextInfo.attacker"), event.source));
        static_cast<void>(read_weak(Layout(profile, "damageTextInfo.victim"), event.target));
        event.value = display_damage;
        event.damage_type = damage_type;
        event.display_type = display_type;
        event.reaction_type = reaction_type;
        event.reaction_display_type = reaction_display_type;
        event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_DISPLAY_VALID;
        if (critical != 0) event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL;
        if (critical_valid) event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID;
        if (head_hit != 0) event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_HEAD_HIT;
        if (weak_unbalance != 0) event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_WEAK_UNBALANCE;
        std::uint32_t source_name_id{};
        std::uint32_t source_name_number{};
        // FCombatStatisticsData::InjurySourceName is the first FName in the
        // embedded statistics struct. Keep the explicit field name while
        // accepting older Profiles that only exposed the struct offset.
        const auto statistics = Layout(
            profile, "damageTextInfo.injurySourceName",
            Layout(profile, "damageTextInfo.combatStatistics"));
        static_cast<void>(read_at(statistics, source_name_id));
        static_cast<void>(read_at(statistics + static_cast<std::int64_t>(sizeof(source_name_id)), source_name_number));
        event.name_id = static_cast<std::uint64_t>(source_name_id) |
            (static_cast<std::uint64_t>(source_name_number) << 32U);
        if (event.name_id != 0 && !combat_event_names.contains(event.name_id)) {
            CacheCombatEventNameLocked(event.name_id);
        }
        event.basic_value = basic_damage;
        event.final_value = final_damage;
        EnrichDamageRecordLocked(event);
        if (combat_event_names.contains(event.name_id)) {
            event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
        }
        if (!MergeCombatDamageLocked(event)) RecordCombatEventLocked(event);
    }

    void CaptureBuffBytesLocked(
        const std::span<const std::uint8_t> parameters,
        const std::uint64_t capture_tick_sequence) noexcept {
        // EnqueueCombatProcessEvent compacts every Buff ABI variant to
        // [GameplayEffect*, duration, stack count, is-add]. This keeps the
        // game-thread hook independent of the 0x298-byte reflected spec.
        std::uintptr_t definition{};
        if (!ReadCaptureBytes(parameters, 0, definition) || definition == 0) return;
        float duration{};
        std::int32_t stack_count{};
        std::uint8_t is_add =
            (parameters.size() >= sizeof(definition) + sizeof(float) + sizeof(std::int32_t) + sizeof(is_add) &&
             ReadCaptureBytes(parameters,
                 sizeof(definition) + sizeof(float) + sizeof(std::int32_t), is_add))
            ? is_add : static_cast<std::uint8_t>(0);
        static_cast<void>(ReadCaptureBytes(parameters, sizeof(definition), duration));
        static_cast<void>(ReadCaptureBytes(
            parameters, sizeof(definition) + sizeof(duration), stack_count));
        const bool added = is_add != 0;
        AnomalyNteCombatEventV1 event{};
        event.kind = added ? ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD : ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE;
        event.tick_sequence = capture_tick_sequence;
        event.world = {1, world_generation};
        AnomalyGenerationHandleV1 definition_handle{};
        if (!ObjectHandleLocked(definition, definition_handle)) return;
        std::uint32_t comparison_index{};
        std::uint32_t number{};
        const auto name_offset = Layout(profile, "object.nameOffset", -1);
        if (name_offset >= 0 &&
            ReadValue(*memory, definition + static_cast<std::uintptr_t>(name_offset), comparison_index) &&
            ReadValue(*memory, definition + static_cast<std::uintptr_t>(name_offset) + sizeof(comparison_index), number)) {
            event.name_id = static_cast<std::uint64_t>(comparison_index) |
                (static_cast<std::uint64_t>(number) << 32U);
        }
        if (event.name_id == 0) event.name_id = definition_handle.id;
        CacheCombatEventNameLocked(event.name_id, definition);
        if (combat_event_names.contains(event.name_id)) {
            event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
        }
        if (added) {
            event.duration_seconds = duration;
            event.stack_count = stack_count;
        }
        RecordCombatEventLocked(event);
    }

    void CaptureCombatProcessEventLocked(
        const CombatCaptureKind kind,
        const std::span<const std::uint8_t> parameters,
        const std::uint16_t info_offset,
        const std::uint64_t capture_tick_sequence) noexcept {
        switch (kind) {
        case CombatCaptureKind::CharacterDamage:
            CaptureCharacterDamageBytesLocked(parameters, capture_tick_sequence);
            break;
        case CombatCaptureKind::Damage:
            CaptureDamageTextBytesLocked(parameters, info_offset, capture_tick_sequence);
            break;
        case CombatCaptureKind::BuffAdd:
        case CombatCaptureKind::BuffRemove:
            CaptureBuffBytesLocked(parameters, capture_tick_sequence);
            break;
        }
    }

    void DrainCombatCaptureQueueLocked() noexcept {
        auto read = combat_capture_read.load(std::memory_order_relaxed);
        const auto write = combat_capture_write.load(std::memory_order_acquire);
        constexpr std::size_t kMaximumCaptureDrainPerTick = 32;
        std::size_t drained{};
        while (read != write && drained < kMaximumCaptureDrainPerTick) {
            const auto& capture = combat_capture_queue[
                read % kCombatCaptureQueueCapacity];
            CaptureCombatProcessEventLocked(
                capture.kind,
                std::span<const std::uint8_t>(capture.payload).first(capture.payload_size),
                capture.info_offset, capture.tick_sequence);
            ++read;
            ++drained;
        }
        combat_capture_read.store(read, std::memory_order_release);
    }

    [[nodiscard]] bool EnqueueDamageTextPayload(
        const std::uint8_t* const source,
        const std::uint64_t capture_tick_sequence) noexcept {
        if (source == nullptr) return false;
        const auto write = combat_capture_write.load(std::memory_order_relaxed);
        const auto read = combat_capture_read.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(write - read) >= kCombatCaptureQueueCapacity) {
            combat_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto& capture = combat_capture_queue[write % kCombatCaptureQueueCapacity];
        capture.kind = CombatCaptureKind::Damage;
        capture.payload_size = 72;
        capture.info_offset = 0;
        capture.tick_sequence = capture_tick_sequence;
        std::memcpy(capture.payload.data(), source, capture.payload_size);
        combat_capture_write.store(write + 1U, std::memory_order_release);
        return true;
    }

    void EnqueueCombatProcessEvent(
        const CombatCaptureBindings& bindings,
        const std::uintptr_t function,
        const void* const parameters) noexcept {
        if (parameters == nullptr) return;
        if (function == bindings.damage) {
            damage_floaties_call_count.fetch_add(1, std::memory_order_relaxed);
        } else if (function == bindings.monster_damage) {
            monster_damage_call_count.fetch_add(1, std::memory_order_relaxed);
        } else if (function == bindings.player_damage_queue) {
            player_damage_queue_call_count.fetch_add(1, std::memory_order_relaxed);
        } else if (function == bindings.damage_widget) {
            damage_widget_call_count.fetch_add(1, std::memory_order_relaxed);
        } else if (std::ranges::any_of(
                       bindings.buffs, [function](const auto& buff) {
                           return buff.function != 0 && buff.function == function;
                       })) {
            buff_call_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            return;
        }
        if (function == bindings.player_damage_queue) {
            struct DamageTextArrayView {
                std::uintptr_t data{};
                std::int32_t num{};
                std::int32_t max{};
            } view;
            const auto* const bytes = static_cast<const std::uint8_t*>(parameters);
            std::memcpy(&view, bytes + bindings.player_damage_queue_offset, sizeof(view));
            if (view.data == 0 || view.num <= 0 || view.max < view.num) return;
            const auto capture_tick_sequence =
                tick_sequence.load(std::memory_order_relaxed);
            for (std::int32_t index{}; index < view.num; ++index) {
                const auto* const damage_text = reinterpret_cast<const std::uint8_t*>(
                    view.data + static_cast<std::uintptr_t>(index) * 72U);
                if (!EnqueueDamageTextPayload(damage_text, capture_tick_sequence)) {
                    if (index + 1 < view.num) {
                        combat_capture_drop_count.fetch_add(
                            static_cast<std::uint64_t>(view.num - index - 1),
                            std::memory_order_relaxed);
                    }
                    break;
                }
            }
            return;
        }
        if (function == bindings.damage_widget) {
            const auto* const damage_text =
                static_cast<const std::uint8_t*>(parameters) +
                bindings.damage_widget_info_offset;
            static_cast<void>(EnqueueDamageTextPayload(
                damage_text, tick_sequence.load(std::memory_order_relaxed)));
            return;
        }
        CombatCaptureKind kind{};
        std::uint16_t payload_size{};
        std::uint16_t info_offset{};
        if (function == bindings.damage || function == bindings.monster_damage) {
            kind = CombatCaptureKind::Damage;
            info_offset = bindings.damage_info_offset;
            payload_size = 72;
        } else {
            const CombatCaptureBindings::Buff* buff = nullptr;
            for (const auto& candidate : bindings.buffs) {
                if (candidate.function != 0 && candidate.function == function) {
                    buff = &candidate;
                    break;
                }
            }
            if (buff == nullptr || buff->object_offset == 0xFFFFU) return;
            kind = CombatCaptureKind::BuffAdd;
            payload_size = static_cast<std::uint16_t>(
                sizeof(std::uintptr_t) + sizeof(float) + sizeof(std::int32_t) + sizeof(std::uint8_t));
            const auto write = combat_capture_write.load(std::memory_order_relaxed);
            const auto read = combat_capture_read.load(std::memory_order_acquire);
            if (static_cast<std::uint32_t>(write - read) >= kCombatCaptureQueueCapacity) {
                combat_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            auto& capture = combat_capture_queue[write % kCombatCaptureQueueCapacity];
            capture.kind = kind;
            capture.payload_size = payload_size;
            capture.info_offset = 0;
            capture.tick_sequence = tick_sequence.load(std::memory_order_relaxed);
            std::memset(capture.payload.data(), 0, payload_size);
            const auto* source = static_cast<const std::uint8_t*>(parameters);
            std::uintptr_t object_value{};
            if (buff->parms_size == 0 ||
                static_cast<std::size_t>(buff->object_offset) + sizeof(object_value) > buff->parms_size ||
                (buff->duration_offset != 0xFFFFU &&
                    static_cast<std::size_t>(buff->duration_offset) + sizeof(float) > buff->parms_size) ||
                (buff->stack_offset != 0xFFFFU &&
                    static_cast<std::size_t>(buff->stack_offset) + sizeof(std::int32_t) > buff->parms_size) ||
                (buff->is_add_offset != 0xFFFFU &&
                    static_cast<std::size_t>(buff->is_add_offset) + sizeof(std::uint8_t) > buff->parms_size)) {
                return;
            }
            if (buff->definition_offset != 0xFFFFU) {
                if (static_cast<std::size_t>(buff->object_offset) + buff->definition_offset +
                    sizeof(object_value) > buff->parms_size) return;
                std::memcpy(&object_value, source + buff->object_offset + buff->definition_offset,
                    sizeof(object_value));
            } else {
                std::memcpy(&object_value, source + buff->object_offset, sizeof(object_value));
            }
            if (object_value == 0) return;
            std::memcpy(capture.payload.data(), &object_value, sizeof(object_value));
            if (buff->duration_offset != 0xFFFFU) {
                std::memcpy(capture.payload.data() + sizeof(object_value),
                    source + buff->duration_offset, sizeof(float));
            }
            if (buff->stack_offset != 0xFFFFU) {
                std::memcpy(capture.payload.data() + sizeof(object_value) + sizeof(float),
                    source + buff->stack_offset, sizeof(std::int32_t));
            }
            std::uint8_t added =
                (buff->duration_offset == 0xFFFFU &&
                    buff->stack_offset == 0xFFFFU &&
                    buff->is_add_offset == 0xFFFFU)
                ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(1);
            if (buff->is_add_offset != 0xFFFFU) {
                std::memcpy(&added, source + buff->is_add_offset, sizeof(added));
            }
            std::memcpy(capture.payload.data() + sizeof(object_value) + sizeof(float) + sizeof(std::int32_t),
                &added, sizeof(added));
            capture.kind = added != 0 ? CombatCaptureKind::BuffAdd : CombatCaptureKind::BuffRemove;
            combat_capture_write.store(write + 1U, std::memory_order_release);
            return;
        }
        if (payload_size == 0 || payload_size > kCombatCapturePayloadBytes) return;
        const auto write = combat_capture_write.load(std::memory_order_relaxed);
        const auto read = combat_capture_read.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(write - read) >=
            kCombatCaptureQueueCapacity) {
            combat_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& capture = combat_capture_queue[write % kCombatCaptureQueueCapacity];
        capture.kind = kind;
        capture.payload_size = payload_size;
        capture.info_offset = 0;
        capture.tick_sequence = tick_sequence.load(std::memory_order_relaxed);
        const auto* const source = static_cast<const std::uint8_t*>(parameters);
        std::memcpy(capture.payload.data(), source + info_offset, payload_size);
        combat_capture_write.store(write + 1U, std::memory_order_release);
    }

    void EnqueueCharacterDamage(
        const CombatCaptureBindings& bindings,
        const std::uintptr_t damage_event,
        const std::uintptr_t victim,
        const std::uintptr_t attacker,
        const std::uintptr_t damage_causer) noexcept {
        damage_native_call_count.fetch_add(1, std::memory_order_relaxed);
        const auto drop = [this]() noexcept {
            damage_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
        };
        const DWORD expected_thread = game_thread_id.load(std::memory_order_acquire);
        if (!started.load(std::memory_order_acquire) || damage_event == 0 || victim == 0 ||
            expected_thread == 0 || expected_thread != GetCurrentThreadId() ||
            bindings.damage_value_offset == 0xFFFFU ||
            bindings.damage_source_offset == 0xFFFFU ||
            bindings.damage_tags_offset == 0xFFFFU) {
            drop();
            return;
        }
        NteDamageCriticalState synchronous_critical;
        bool query_failed{};
        for (const std::uintptr_t candidate : {victim, attacker}) {
            if (candidate == 0 || synchronous_critical.critical) continue;
            std::uintptr_t ability_system{};
            if (!CurrentAbilitySystemLocked(candidate, ability_system)) {
                query_failed = true;
                continue;
            }
            bool candidate_critical{};
            crit_query_call_count.fetch_add(1, std::memory_order_relaxed);
            if (InvokeNteBoolReturnLocked(
                    NteFunctionKind::CurrentDamageIsCrit, ability_system, candidate_critical)) {
                synchronous_critical.valid = true;
                crit_query_success_count.fetch_add(1, std::memory_order_relaxed);
                if (candidate_critical) {
                    synchronous_critical.critical = true;
                    crit_true_count.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                query_failed = true;
            }
        }
        synchronous_critical.valid = synchronous_critical.critical ||
            (synchronous_critical.valid && !query_failed);
        // Query before reserving the queue slot: ProcessEvent can reenter capture.
        const auto write = combat_capture_write.load(std::memory_order_relaxed);
        const auto read = combat_capture_read.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(write - read) >= kCombatCaptureQueueCapacity) {
            drop();
            return;
        }
        auto& capture = combat_capture_queue[write % kCombatCaptureQueueCapacity];
        capture.kind = CombatCaptureKind::CharacterDamage;
        capture.payload_size = sizeof(NteCharacterDamageCapture);
        capture.info_offset = 0;
        capture.tick_sequence = tick_sequence.load(std::memory_order_relaxed);
        NteCharacterDamageCapture damage;
        const auto* const source = static_cast<const std::uint8_t*>(
            reinterpret_cast<const void*>(damage_event));
        std::memcpy(&damage.damage, source + bindings.damage_value_offset, sizeof(float));
        std::memcpy(&damage.source_index, source + bindings.damage_source_offset, 4);
        std::memcpy(&damage.source_serial, source + bindings.damage_source_offset + 4, 4);
        damage.victim = victim;
        damage.attacker = attacker;
        damage.critical = synchronous_critical;
        std::uintptr_t saved_skill_cdo{};
        std::int32_t active_spec_handle{};
        const auto trigger_handle_offset = Layout(
            profile, "abilitySpawnActor.triggerAbilityHandle", -1);
        const auto saved_skill_offset = Layout(
            profile, "abilitySpawnActor.savedTriggerSkillCDO", -1);
        if (damage_causer != 0 && trigger_handle_offset >= 0) {
            static_cast<void>(ReadValue(
                *memory,
                damage_causer + static_cast<std::uintptr_t>(trigger_handle_offset),
                active_spec_handle));
            if (active_spec_handle < 0) active_spec_handle = 0;
        }
        if (damage_causer != 0 && saved_skill_offset >= 0) {
            static_cast<void>(ReadPointerAt(
                *memory, damage_causer, saved_skill_offset, saved_skill_cdo));
        }
        damage.saved_skill_cdo = saved_skill_cdo;
        damage.active_spec_handle = active_spec_handle;
        damage.tags = CaptureNteDamageTags(damage_event + bindings.damage_tags_offset,
            [this](std::uintptr_t address, void* destination, std::size_t size) {
                return memory->Read(address, destination, size);
            });
        std::memcpy(capture.payload.data(), &damage, sizeof(damage));
        combat_capture_write.store(write + 1U, std::memory_order_release);
    }

    void CacheDamageParticipantPathLocked(
        const AnomalyGenerationHandleV1 participant) noexcept {
        if (participant.id == 0 || participant.generation != object_generation ||
            damage_participant_paths.contains(participant.id)) {
            return;
        }
        try {
            std::uintptr_t object{};
            if (!ResolveObjectHandleLocked(participant, object)) return;
            std::string path = ObjectPathLocked(object);
            if (!path.empty()) {
                damage_participant_paths.emplace(participant.id, std::move(path));
            }
        } catch (...) {
        }
    }

    [[nodiscard]] std::uint64_t ResolveDamageEventSourceLocked(
        const std::int32_t source_index,
        const std::int32_t source_serial) noexcept {
        if (source_index < 0 || source_serial <= 0) return 0;
        AnomalyGenerationHandleV1 source_handle{};
        if (!WeakObjectHandleLocked(source_index, source_serial, source_handle)) {
            return 0;
        }
        const auto existing = damage_source_object_ids.find(source_handle.id);
        if (existing != damage_source_object_ids.end()) return existing->second;

        std::uintptr_t source_object{};
        if (!ResolveObjectHandleLocked(source_handle, source_object)) return 0;
        const std::uint64_t source_id = damage_next_source_id++;
        damage_source_object_ids.emplace(source_handle.id, source_id);
        damage_source_objects.emplace(source_id, source_object);
        combat_event_objects.emplace(source_id, source_object);
        return source_id;
    }

    [[nodiscard]] const std::string& ResolveDamageSourceNameLocked(
        const std::uint64_t source_id) noexcept {
        const auto cached = damage_source_names.find(source_id);
        if (cached != damage_source_names.end()) return cached->second;
        static const std::string empty;
        return empty;
    }

    [[nodiscard]] bool ResolveDamageDefinitionDisplayNameLocked(
        const std::uint64_t source_id) noexcept {
        if (source_id == 0 || combat_event_names.contains(source_id) ||
            !display_table_scan_complete || damage_skill_index.table == 0) {
            return combat_event_names.contains(source_id);
        }
        const auto source = damage_source_objects.find(source_id);
        if (source == damage_source_objects.end() || source->second == 0) return false;
        try {
            const auto name_offset = Layout(profile, "object.nameOffset", -1);
            if (name_offset < 0) return false;
            const auto try_object = [this, name_offset](
                                        const std::uintptr_t object,
                                        std::string& value) {
                std::uint32_t comparison_index{};
                std::uint32_t number{};
                if (object == 0 ||
                    !ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(name_offset),
                        comparison_index) ||
                    !ReadValue(*memory,
                        object + static_cast<std::uintptr_t>(name_offset) +
                            sizeof(comparison_index),
                        number)) {
                    return false;
                }
                const auto effect_key = static_cast<std::uint64_t>(comparison_index) |
                    (static_cast<std::uint64_t>(number) << 32U);
                std::string effect_name;
                static_cast<void>(ResolveFNameLocked(
                    comparison_index, number, effect_name));
                std::uint64_t ability_key{};
                if (!FindDamageSkillAbilityLocked(
                        effect_key, effect_name, ability_key) || ability_key == 0) {
                    return false;
                }
                std::string ability_name;
                static_cast<void>(ResolveFNameLocked(
                    static_cast<std::uint32_t>(ability_key & 0xFFFFFFFFU),
                    static_cast<std::uint32_t>(ability_key >> 32U), ability_name));
                return ResolveIndexedDisplayNameLocked(
                    ability_key, ability_name, value) && !value.empty();
            };

            std::string value;
            std::uintptr_t source_class{};
            if ((ReadPointerAt(*memory, source->second,
                     Layout(profile, "object.class"), source_class) &&
                    try_object(source_class, value)) ||
                try_object(source->second, value)) {
                combat_event_names.emplace(source_id, std::move(value));
                MarkCombatEventNameResolvedLocked(source_id);
                return true;
            }
        } catch (...) {
        }
        return false;
    }

    [[nodiscard]] bool ResolveAbilityDisplayNameForClassLocked(
        const std::uintptr_t ability_class,
        std::string& value) noexcept {
        value.clear();
        if (ability_class == 0) return false;
        AnomalyGenerationHandleV1 handle{};
        if (!ObjectHandleLocked(ability_class, handle)) return false;
        const auto cached = ability_display_names.find(handle.id);
        if (cached != ability_display_names.end() && !cached->second.empty()) {
            value = cached->second;
            return true;
        }
        try {
            const auto name_offset = Layout(profile, "object.nameOffset", -1);
            if (name_offset < 0) return false;
            std::uint32_t comparison_index{};
            std::uint32_t number{};
            if (!ReadValue(*memory,
                    ability_class + static_cast<std::uintptr_t>(name_offset),
                    comparison_index) ||
                !ReadValue(*memory,
                    ability_class + static_cast<std::uintptr_t>(name_offset) +
                        sizeof(comparison_index), number)) {
                return false;
            }
            const auto key = static_cast<std::uint64_t>(comparison_index) |
                (static_cast<std::uint64_t>(number) << 32U);
            std::string key_text;
            static_cast<void>(ResolveFNameLocked(comparison_index, number, key_text));
            if (!ResolveIndexedDisplayNameLocked(key, key_text, value) || value.empty()) {
                value.clear();
                return false;
            }
            ability_display_names.emplace(handle.id, value);
            return true;
        } catch (...) {
            value.clear();
            return false;
        }
    }

    enum class DamageSourceMappingKind : std::uint8_t {
        None,
        SavedTriggerSkillCdo,
        TriggerAbilityHandle,
    };

    [[nodiscard]] bool ResolveDamageSourceAbilityClassLocked(
        const PendingDamageSourceMapping& pending,
        std::uintptr_t& ability_class,
        DamageSourceMappingKind& mapping_kind) noexcept {
        ability_class = 0;
        mapping_kind = DamageSourceMappingKind::None;
        if (pending.saved_skill_cdo != 0 &&
            ReadPointerAt(*memory, pending.saved_skill_cdo,
                Layout(profile, "object.class"), ability_class) &&
            ability_class != 0 && combat_skill_discovery.gameplay_ability_class != 0 &&
            IsClassDerivedFromLocked(
                ability_class, combat_skill_discovery.gameplay_ability_class)) {
            mapping_kind = DamageSourceMappingKind::SavedTriggerSkillCdo;
            return true;
        }
        if (pending.attacker_is_player && pending.active_spec_handle != 0 && skills_available) {
            const auto found = std::ranges::find_if(
                skills, [&](const SkillRecord& record) {
                    return record.spec_handle == pending.active_spec_handle;
                });
            if (found != skills.end()) {
                ability_class = found->ability_class_pointer;
                mapping_kind = DamageSourceMappingKind::TriggerAbilityHandle;
                return ability_class != 0;
            }
        }
        if (pending.attacker_is_player && pending.active_spec_handle == 0 && skills_available) {
            const SkillRecord* active{};
            for (const auto& record : skills) {
                if ((record.flags & ANOMALY_NTE_SKILL_V1_ACTIVE) == 0) continue;
                if (active != nullptr) {
                    active = nullptr;
                    break;
                }
                active = &record;
            }
            if (active != nullptr) {
                ability_class = active->ability_class_pointer;
                mapping_kind = DamageSourceMappingKind::TriggerAbilityHandle;
                return ability_class != 0;
            }
        }
        return false;
    }

    void CompleteDamageSourceNamesForAbilityLocked(
        const std::uint64_t ability_class_id,
        const std::string_view name) noexcept {
        if (name.empty()) return;
        for (auto& [source_id, mapping] : damage_source_ability_classes) {
            if (!mapping.name_pending || mapping.class_handle.id != ability_class_id ||
                combat_event_names.contains(source_id)) {
                continue;
            }
            combat_event_names.emplace(source_id, std::string(name));
            mapping.name_pending = false;
            ++delayed_damage_name_completion_count;
            MarkCombatEventNameResolvedLocked(source_id);
        }
    }

    void QueueDamageSourceAbilityNameLocked(
        const std::uint64_t source_id,
        const std::uintptr_t saved_skill_cdo,
        const std::int32_t active_spec_handle,
        const bool attacker_is_player) noexcept {
        if (source_id == 0 || combat_event_names.contains(source_id)) return;
        if (
            damage_source_ability_classes.contains(source_id) ||
            observed_damage_source_mappings.contains(source_id)) {
            return;
        }
        try {
            observed_damage_source_mappings.insert(source_id);
            pending_damage_source_mappings.push_back({
                source_id, saved_skill_cdo, active_spec_handle, attacker_is_player, 0});
        } catch (...) {
            observed_damage_source_mappings.erase(source_id);
        }
    }

    void ResolveNextDamageSourceMappingLocked() noexcept {
        constexpr std::size_t kMaximumMappingsPerTick = 1;
        constexpr std::uint8_t kMaximumMappingAttempts = 8;
        const auto mapping_count = (std::min)(
            pending_damage_source_mappings.size(), kMaximumMappingsPerTick);
        for (std::size_t mapping_index{}; mapping_index < mapping_count;
             ++mapping_index) {
            auto pending = pending_damage_source_mappings.front();
            if (ResolveDamageDefinitionDisplayNameLocked(pending.source_id)) {
                pending_damage_source_mappings.pop_front();
                continue;
            }
            std::uintptr_t ability_class{};
            DamageSourceMappingKind mapping_kind{};
            if (!ResolveDamageSourceAbilityClassLocked(
                    pending, ability_class, mapping_kind)) {
                // A player hit may arrive before the first skill frame. Keep the
                // source queued until that frame supplies the handle-to-class map;
                // NPC hits do not depend on the player's skill cache.
                if (pending.attacker_is_player && !skills_available) return;
                pending_damage_source_mappings.pop_front();
                if (pending.attempts + 1U < kMaximumMappingAttempts) {
                    ++pending.attempts;
                    pending_damage_source_mappings.push_back(pending);
                } else {
                    ++damage_source_mapping_failure_count;
                }
                continue;
            }
            pending_damage_source_mappings.pop_front();
            AnomalyGenerationHandleV1 class_handle{};
            if (!ObjectHandleLocked(ability_class, class_handle)) {
                if (pending.attempts + 1U < kMaximumMappingAttempts) {
                    ++pending.attempts;
                    pending_damage_source_mappings.push_back(pending);
                } else {
                    ++damage_source_mapping_failure_count;
                }
                continue;
            }
            if (mapping_kind == DamageSourceMappingKind::SavedTriggerSkillCdo) {
                ++saved_trigger_skill_mapping_count;
            } else if (mapping_kind == DamageSourceMappingKind::TriggerAbilityHandle) {
                ++trigger_ability_handle_mapping_count;
            }
            auto [mapping, inserted] = damage_source_ability_classes.emplace(
                pending.source_id,
                DamageSourceAbilityMapping{ability_class, class_handle, true, 0, 0});
            if (!inserted) continue;
            const auto name = ability_display_names.find(class_handle.id);
            if (name != ability_display_names.end() && !name->second.empty()) {
                combat_event_names.emplace(pending.source_id, name->second);
                mapping->second.name_pending = false;
                MarkCombatEventNameResolvedLocked(pending.source_id);
            } else {
                std::string resolved_name;
                if (ResolveAbilityDisplayNameForClassLocked(
                        ability_class, resolved_name)) {
                    CompleteDamageSourceNamesForAbilityLocked(
                        class_handle.id, resolved_name);
                } else {
                    mapping->second.name_attempts = 1;
                    mapping->second.next_name_sequence =
                        tick_sequence.load(std::memory_order_relaxed) + 15U;
                }
            }
        }
    }

    void ResolveNextPendingDamageSourceNameLocked(
        const std::uint64_t sequence) noexcept {
        if ((display_table_loaded_mask & 0x04U) == 0) {
            display_name_demand.store(true, std::memory_order_release);
            return;
        }
        constexpr std::uint8_t kMaximumNameAttempts = 8;
        constexpr std::uint64_t kRetryInterval = 15;
        for (auto& [source_id, mapping] : damage_source_ability_classes) {
            static_cast<void>(source_id);
            if (!mapping.name_pending ||
                mapping.name_attempts >= kMaximumNameAttempts ||
                sequence < mapping.next_name_sequence) {
                continue;
            }
            std::string resolved_name;
            if (ResolveAbilityDisplayNameForClassLocked(
                    mapping.class_pointer, resolved_name)) {
                CompleteDamageSourceNamesForAbilityLocked(
                    mapping.class_handle.id, resolved_name);
            } else {
                ++mapping.name_attempts;
                mapping.next_name_sequence = sequence + kRetryInterval;
            }
            return;
        }
    }

    void ProcessCharacterDamageLocked(
        const float damage,
        const std::int32_t source_index,
        const std::int32_t source_serial,
        const std::uintptr_t victim,
        const std::uintptr_t attacker,
        const std::uintptr_t saved_skill_cdo,
        const std::int32_t active_spec_handle,
        const bool critical,
        const bool critical_valid,
        const bool partial,
        const std::uint64_t capture_tick_sequence) noexcept {
        const auto drop = [this]() noexcept {
            ++damage_dropped_count;
            damage_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
        };
        if (!started.load(std::memory_order_acquire) || victim == 0 ||
            !SemanticFeatureAvailable("nte.combat") || world_pointer == 0 ||
            !std::isfinite(damage) || damage < 0.0F ||
            damage > static_cast<float>((std::numeric_limits<std::int64_t>::max)())) {
            drop();
            return;
        }
        try {
            bool is_critical = critical;
            AnomalyNteDamageEventV1 event{};
            event.flags = ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT;
            if (is_critical) event.flags |= ANOMALY_NTE_DAMAGE_V1_CRITICAL;
            if (critical_valid) event.flags |= ANOMALY_NTE_DAMAGE_V1_CRITICAL_VALID;
            event.tick_sequence = capture_tick_sequence;
            event.world = {1, world_generation};
            if (attacker != 0 && !ObjectHandleLocked(attacker, event.attacker)) {
                ++damage_attacker_resolution_failure_count;
            }
            if (!ObjectHandleLocked(victim, event.victim)) {
                ++damage_victim_resolution_failure_count;
                drop();
                return;
            }
            if (attacker != 0 && event.attacker.id != 0) {
                QueueCombatParticipantNameLocked(event.attacker, attacker);
            }
            QueueCombatParticipantNameLocked(event.victim, victim);
            event.source_id = ResolveDamageEventSourceLocked(source_index, source_serial);
            if (event.source_id == 0) ++damage_source_resolution_failure_count;
            if (event.source_id != 0) {
                QueueDamageSourceAbilityNameLocked(
                    event.source_id, saved_skill_cdo, active_spec_handle,
                    attacker == player_pawn);
            }
            event.final_damage = static_cast<std::int64_t>(std::llround(damage));
            RecordDamageEventLocked(event);
            AnomalyNteCombatEventV1 combat_event{};
            combat_event.kind = ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE;
            combat_event.tick_sequence = event.tick_sequence;
            combat_event.world = event.world;
            combat_event.source = event.attacker;
            combat_event.target = event.victim;
            combat_event.final_value = event.final_damage;
            combat_event.value = event.final_damage;
            combat_event.name_id = event.source_id;
            if (partial) combat_event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_PARTIAL;
            if (critical_valid) combat_event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL_VALID;
            if (is_critical) {
                combat_event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL;
            }
            if (event.source_id != 0) {
                if (combat_event_names.contains(event.source_id)) {
                    combat_event.flags |= ANOMALY_NTE_COMBAT_EVENT_V1_NAME_VALID;
                }
            }
            if (!MergeCombatDamageLocked(combat_event)) {
                RecordCombatEventLocked(combat_event);
            }
            ++damage_captured_event_count;
        } catch (...) {
            drop();
        }
    }

    void CaptureCharacterDamageBytesLocked(
        const std::span<const std::uint8_t> parameters,
        const std::uint64_t capture_tick_sequence) noexcept {
        NteCharacterDamageCapture damage;
        if (!ReadCaptureBytes(parameters, 0, damage)) {
            damage_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool critical = damage.critical.critical ||
            DamageTagsContainCriticalLocked(damage.tags);
        const bool critical_valid = damage.critical.valid || critical;
        ProcessCharacterDamageLocked(
            damage.damage, damage.source_index, damage.source_serial,
            damage.victim, damage.attacker, damage.saved_skill_cdo, damage.active_spec_handle,
            critical, critical_valid, damage.tags.partial || !critical_valid,
            capture_tick_sequence);
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
                        if (std::string name = ResolveNameForScanLocked(entity.class_name_id);
                            !name.empty()) {
                            class_names.emplace(entity.class_id, std::move(name));
                        }
                    }
                    if (entity.entity_name_id != 0 && !entity_names.contains(entity.entity_id)) {
                        if (std::string name = ResolveNameForScanLocked(entity.entity_name_id);
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
        if (length == 0 || length > 1024) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "invalid name length");
        }
        if (!wide) {
            std::string value(length, '\0');
            if (!memory->Read(entry + sizeof(header), value.data(), value.size())) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND);
            }
            return CopyString(value, destination, size);
        }
        std::vector<wchar_t> wide_value(length);
        if (!memory->Read(entry + sizeof(header), wide_value.data(), wide_value.size() * sizeof(wchar_t))) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        }
        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide_value.data(), static_cast<int>(wide_value.size()),
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "wide name decode failed");
        std::string value(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide_value.data(), static_cast<int>(wide_value.size()),
                value.data(), required, nullptr, nullptr) != required) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "wide name decode failed");
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

    // 仅供全量扫描使用：FName 的 comparison index 在进程内稳定，同一 name_id 永远对应
    // 同一个字符串，因此把解码结果记下来。全关卡扫描会为每个 actor 解析一次实体名
    // （数千个互不相同的 name_id），逐次解码宽字符名的总代价超过一秒；记忆化之后只有
    // 首次扫描需要真正解码。失败结果不缓存，那通常意味着布局尚未就绪，应当重试。
    std::string ResolveNameForScanLocked(std::uint32_t name_id) const {
        if (name_id == 0) return {};
        if (const auto cached = name_snapshot_cache.find(name_id);
            cached != name_snapshot_cache.end()) {
            return cached->second;
        }
        std::string value = ResolveNameSnapshotLocked(name_id);
        if (!value.empty()) name_snapshot_cache.emplace(name_id, value);
        return value;
    }

    [[nodiscard]] bool ResolveFTextBytesLocked(
        const std::array<std::uint8_t, 16>& ftext_bytes,
        std::string& value) const {
        value = ReadUe5FTextUtf8(profile, resolution, *memory, ftext_bytes);
        return !value.empty();
    }

    [[nodiscard]] bool EnsureRegisteredStringTablesBindingLocked() const noexcept {
        auto& binding = registered_string_tables;
        if (binding.object_generation != object_generation) {
            binding = {};
            binding.object_generation = object_generation;
        }
        if (binding.attempted) {
            return binding.function != 0 && binding.receiver != 0;
        }
        if (!ObjectFindAvailable() || !process_event_invoker) return false;
        try {
            std::uintptr_t function{};
            std::uintptr_t library_class{};
            if (!FindExactObjectLocked(
                    L"/Script/Engine.KismetStringTableLibrary.GetRegisteredStringTables",
                    function) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"),
                    library_class) ||
                !ReadPointerAt(*memory, library_class,
                    Layout(profile, "uclass.classDefaultObject"),
                    binding.receiver) ||
                binding.receiver == 0) {
                return false;
            }
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (!ReadValue(*memory,
                    function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory,
                    function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(*memory,
                    function + Layout(profile, "ufunction.returnValueOffset"),
                    return_offset) ||
                num_parms != 1 || parms_size != 16 || return_offset != 0 ||
                parms_size > binding.parameters.size()) {
                binding.receiver = 0;
                return false;
            }
            binding.parms_size = parms_size;
            binding.return_offset = return_offset;
            binding.function = function;
            binding.attempted = true;
            return true;
        } catch (...) {
            binding.function = 0;
            binding.receiver = 0;
            binding.attempted = false;
            return false;
        }
    }

    [[nodiscard]] bool ResolveRegisteredStringTableIdLocked(
        const std::string_view short_name,
        std::uint64_t& table_id) const noexcept {
        table_id = 0;
        if (short_name.empty() ||
            GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire) ||
            !EnsureRegisteredStringTablesBindingLocked()) {
            return false;
        }
        auto& binding = registered_string_tables;
        std::memset(binding.parameters.data(), 0, binding.parms_size);
        try {
            if (!ReadableRange(*memory, binding.receiver, 0x20U) ||
                !ReadableRange(*memory, binding.function, 0x20U) ||
                !InvokeNativeProcessEventLocked(
                    binding.receiver, binding.function,
                    binding.parameters.data(), binding.parms_size)) {
                return false;
            }
        } catch (...) {
            return false;
        }
        const auto data_offset = Layout(profile, "tarray.data");
        const auto num_offset = Layout(profile, "tarray.num");
        const auto max_offset = Layout(profile, "tarray.max");
        if (data_offset < 0 || num_offset < 0 || max_offset < 0 ||
            binding.return_offset > binding.parms_size ||
            static_cast<std::size_t>(data_offset) + sizeof(std::uintptr_t) >
                binding.parms_size - binding.return_offset ||
            static_cast<std::size_t>(num_offset) + sizeof(std::int32_t) >
                binding.parms_size - binding.return_offset ||
            static_cast<std::size_t>(max_offset) + sizeof(std::int32_t) >
                binding.parms_size - binding.return_offset) {
            return false;
        }
        std::uintptr_t array_data{};
        std::int32_t array_count{};
        std::int32_t array_capacity{};
        std::memcpy(&array_data,
            binding.parameters.data() + binding.return_offset + data_offset,
            sizeof(array_data));
        std::memcpy(&array_count,
            binding.parameters.data() + binding.return_offset + num_offset,
            sizeof(array_count));
        std::memcpy(&array_capacity,
            binding.parameters.data() + binding.return_offset + max_offset,
            sizeof(array_capacity));
        if (array_count < 0 || array_capacity < array_count ||
            array_capacity > 4096 ||
            (array_count != 0 && array_data == 0)) {
            return false;
        }
        const std::string short_owned(short_name);
        int best_score{};
        std::uint64_t best_id{};
        for (std::int32_t index{}; index < array_count; ++index) {
            std::uintptr_t entry{};
            if (!AddAddress(
                    array_data, static_cast<std::int64_t>(index) * 8, entry)) {
                continue;
            }
            std::uint32_t comparison_index{};
            std::uint32_t number{};
            if (!ReadValue(*memory, entry, comparison_index) ||
                !ReadValue(*memory, entry + sizeof(comparison_index), number)) {
                continue;
            }
            std::string name;
            if (!ResolveFNameLocked(comparison_index, number, name) || name.empty()) {
                continue;
            }
            int score{};
            if (name == short_owned) {
                score = 3;
            } else if (name.size() > short_owned.size() &&
                       name.compare(name.size() - short_owned.size() - 1U,
                           short_owned.size() + 1U,
                           "." + short_owned) == 0) {
                score = 2;
            } else if (name.size() > short_owned.size() &&
                       name.compare(name.size() - short_owned.size() - 1U,
                           short_owned.size() + 1U,
                           "/" + short_owned) == 0) {
                score = 1;
            }
            if (score > best_score) {
                best_score = score;
                best_id = static_cast<std::uint64_t>(comparison_index) |
                    (static_cast<std::uint64_t>(number) << 32U);
                if (score == 3) break;
            }
        }
        if (best_score == 0 || best_id == 0) return false;
        table_id = best_id;
        return true;
    }

    [[nodiscard]] bool EnsureStringTableEntryBindingLocked(
        const wchar_t* const table_object_path,
        StringTableEntryBinding& binding) const noexcept {
        if (binding.object_generation != object_generation) {
            binding = {};
            binding.object_generation = object_generation;
            string_table_binding_failure_code = 0;
        }
        if (binding.attempted) {
            return binding.function != 0 && binding.receiver != 0 &&
                binding.table_id != 0;
        }
        ++string_table_binding_attempts;
        if (!ObjectFindAvailable() || !process_event_invoker) {
            ++string_table_binding_failures;
            string_table_binding_failure_code = 1;
            return false;
        }
        try {
            std::uintptr_t function{};
            std::uintptr_t library_class{};
            std::uintptr_t table_object{};
            if (!FindExactObjectLocked(
                    L"/Script/Engine.KismetTextLibrary.TextFromStringTable",
                    function) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"),
                    library_class) ||
                !ReadPointerAt(*memory, library_class,
                    Layout(profile, "uclass.classDefaultObject"),
                    binding.receiver) ||
                binding.receiver == 0 ||
                !FindExactObjectLocked(
                    table_object_path, table_object) ||
                table_object == 0) {
                ++string_table_binding_failures;
                string_table_binding_failure_code = 2;
                return false;
            }
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (!ReadValue(*memory,
                    function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory,
                    function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(*memory,
                    function + Layout(profile, "ufunction.returnValueOffset"),
                    return_offset) ||
                num_parms != 3 || parms_size != 40 || return_offset != 24 ||
                parms_size > binding.parameters.size()) {
                binding.receiver = 0;
                ++string_table_binding_failures;
                string_table_binding_failure_code = 3;
                return false;
            }
            const auto name_offset = Layout(profile, "object.nameOffset", -1);
            std::uint32_t table_index{};
            std::uint32_t table_number{};
            if (name_offset < 0 ||
                !ReadValue(*memory,
                    table_object + static_cast<std::uintptr_t>(name_offset),
                    table_index) ||
                !ReadValue(*memory,
                    table_object + static_cast<std::uintptr_t>(name_offset) +
                        sizeof(table_index),
                    table_number) ||
                table_index == 0) {
                binding.receiver = 0;
                ++string_table_binding_failures;
                string_table_binding_failure_code = 4;
                return false;
            }
            std::string table_short_name;
            static_cast<void>(ResolveFNameLocked(
                table_index, table_number, table_short_name));
            std::uint64_t registered_table_id{};
            if (!table_short_name.empty() &&
                ResolveRegisteredStringTableIdLocked(
                    table_short_name, registered_table_id) &&
                registered_table_id != 0) {
                binding.table_id = registered_table_id;
            } else {
                binding.table_id = static_cast<std::uint64_t>(table_index) |
                    (static_cast<std::uint64_t>(table_number) << 32U);
            }
            binding.table_offset = 0;
            binding.key_offset = 8;
            binding.return_offset = return_offset;
            binding.function = function;
            binding.parms_size = parms_size;
            binding.attempted = true;
            string_table_binding_failure_code = 0;
            return true;
        } catch (...) {
            binding.function = 0;
            binding.receiver = 0;
            binding.table_id = 0;
            binding.attempted = false;
            ++string_table_binding_failures;
            string_table_binding_failure_code = 5;
            return false;
        }
    }

    [[nodiscard]] bool EnsureActorStringTableEntryBindingLocked() const noexcept {
        return EnsureStringTableEntryBindingLocked(
            L"/Game/Text/ST_ActorName.ST_ActorName", string_table_entry);
    }

    [[nodiscard]] bool EnsureAbyssStringTableEntryBindingLocked() const noexcept {
        return EnsureStringTableEntryBindingLocked(
            L"/Game/DataAssets/DataAssetSet/Abyss/ST_AbyssBattle.ST_AbyssBattle",
            abyss_string_table_entry);
    }

    [[nodiscard]] bool ResolveStringTableEntryLockedImpl(
        const std::string_view key,
        const wchar_t* const table_object_path,
        StringTableEntryBinding& binding,
        std::string& value) const noexcept {
        value.clear();
        string_table_last_key.assign(key.data(), key.size());
        string_table_last_value.clear();
        ++string_table_call_count;
        if (key.empty() || key.size() > 256 ||
            GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire) ||
            !EnsureStringTableEntryBindingLocked(table_object_path, binding)) {
            if (!key.empty() && key.size() <= 256 &&
                GetCurrentThreadId() != game_thread_id.load(std::memory_order_acquire)) {
                ++string_table_thread_rejections;
            }
            return false;
        }
        if (binding.table_offset > binding.parameters.size() ||
            sizeof(binding.table_id) >
                binding.parameters.size() - binding.table_offset ||
            binding.key_offset > binding.parameters.size() ||
            sizeof(NativeUtf16StringHeader) >
                binding.parameters.size() - binding.key_offset ||
            binding.parms_size > binding.parameters.size()) {
            ++string_table_binding_failures;
            string_table_binding_failure_code = 6;
            return false;
        }
        const int source_size = static_cast<int>(key.size());
        const int count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, key.data(), source_size, nullptr, 0);
        if (count <= 0 || count >= (std::numeric_limits<std::int32_t>::max)() - 1) {
            return false;
        }
        std::vector<wchar_t> key_storage(static_cast<std::size_t>(count) + 1U);
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, key.data(), source_size,
                key_storage.data(), count) != count) {
            return false;
        }
        key_storage[static_cast<std::size_t>(count)] = L'\0';
        NativeUtf16StringHeader key_header;
        key_header.data = key_storage.data();
        key_header.count = count + 1;
        key_header.capacity = count + 1;
        std::memset(binding.parameters.data(), 0, binding.parms_size);
        std::memcpy(
            binding.parameters.data() + binding.table_offset,
            &binding.table_id, sizeof(binding.table_id));
        std::memcpy(
            binding.parameters.data() + binding.key_offset,
            &key_header, sizeof(key_header));
        try {
            if (!ReadableRange(*memory, binding.receiver, 0x20U) ||
                !ReadableRange(*memory, binding.function, 0x20U) ||
                !InvokeNativeProcessEventLocked(
                    binding.receiver, binding.function,
                    binding.parameters.data(), binding.parms_size)) {
                ++string_table_binding_failures;
                string_table_binding_failure_code = 7;
                return false;
            }
        } catch (...) {
            ++string_table_binding_failures;
            string_table_binding_failure_code = 8;
            return false;
        }
        constexpr std::uint16_t kFTextSize = 16;
        if (binding.return_offset > binding.parms_size ||
            kFTextSize > binding.parms_size - binding.return_offset) {
            ++string_table_binding_failures;
            string_table_binding_failure_code = 9;
            return false;
        }
        std::array<std::uint8_t, kFTextSize> ftext_bytes{};
        std::memcpy(ftext_bytes.data(),
            binding.parameters.data() + binding.return_offset, ftext_bytes.size());
        const bool converted = ResolveFTextBytesLocked(ftext_bytes, value);
        const bool decoded = converted && !value.empty() &&
            value != "<MISSING STRING TABLE ENTRY>";
        if (converted && !decoded && value == "<MISSING STRING TABLE ENTRY>") {
            value.clear();
        }
        if (decoded) {
            ++string_table_success_count;
            string_table_last_value = value;
        } else {
            string_table_binding_failure_code = value.empty() ? 10 : 9;
        }
        return decoded;
    }

    [[nodiscard]] bool ResolveStringTableEntryLocked(
        const std::string_view key, std::string& value) const noexcept {
        return ResolveStringTableEntryLockedImpl(
            key, L"/Game/Text/ST_ActorName.ST_ActorName",
            string_table_entry, value);
    }

    [[nodiscard]] bool ResolveAbyssStringTableEntryLocked(
        const std::string_view key, std::string& value) const noexcept {
        return ResolveStringTableEntryLockedImpl(
            key,
            L"/Game/DataAssets/DataAssetSet/Abyss/ST_AbyssBattle.ST_AbyssBattle",
            abyss_string_table_entry, value);
    }

    [[nodiscard]] bool ResolveFTextLocked(
        const std::uintptr_t ftext_address, std::string& value) const {
        std::array<std::uint8_t, 16> bytes{};
        value.clear();
        return ftext_address != 0 &&
            memory->Read(ftext_address, bytes.data(), bytes.size()) &&
            ResolveFTextBytesLocked(bytes, value);
    }

    AnomalyStatusV1 ResolveFTextAddress(
        const std::uintptr_t address, char* destination, std::size_t* size) noexcept {
        if (size == nullptr || address == 0) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        std::scoped_lock lock(mutex);
        if (!ServiceAvailableForPublication(ANOMALY_UE5_NAMES_SERVICE_V1_ID)) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UE5 names service is unavailable");
        }
        std::string value;
        try {
            if (!ResolveFTextLocked(address, value)) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "FText is unreadable");
            }
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "FText decode failed");
        }
        return CopyString(value, destination, size);
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
        std::array<char, 1025> resolved{};
        std::size_t resolved_size = resolved.size();
        if (ResolveNameIdLocked(name_id, resolved.data(), &resolved_size).code !=
                ANOMALY_STATUS_V1_OK ||
            resolved_size <= 1 || resolved_size > resolved.size()) {
            return false;
        }
        name.assign(resolved.data(), resolved_size - 1U);
        return !name.empty();
    }

    [[nodiscard]] bool FindExactObjectLocked(
        const wchar_t* const path,
        std::uintptr_t& object) const noexcept {
        object = 0;
        if (path == nullptr || !ObjectFindAvailable() || object_registry.items == 0) return false;
        object = object_lookup(path);
        if (object == 0) return false;

        std::uintptr_t index_address{};
        std::int32_t internal_index{-1};
        if (!AddAddress(object, Layout(profile, "object.internalIndex"), index_address) ||
            !ReadValue(*memory, index_address, internal_index) || internal_index < 0 ||
            static_cast<std::uint64_t>(internal_index) >= object_registry.count) {
            object = 0;
            return false;
        }
        std::uintptr_t slot_object{};
        std::uint32_t serial{};
        if (!ReadObjectSlot(*memory, object_registry, static_cast<std::uint32_t>(internal_index),
                slot_object, serial) || slot_object != object) {
            object = 0;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool DecodeUtf16StringLocked(
        const NativeUtf16StringHeader& native,
        std::string& value) const {
        value.clear();
        if (native.count == 0) return true;
        if (native.data == nullptr || native.count < 1 || native.count > 512 ||
            native.capacity < native.count || native.capacity > 4096) {
            return false;
        }
        std::vector<wchar_t> storage(static_cast<std::size_t>(native.count));
        if (!memory->Read(
                reinterpret_cast<std::uintptr_t>(native.data),
                storage.data(), storage.size() * sizeof(wchar_t))) {
            return false;
        }
        // FString counts include the terminator; the public UTF-8 ABI cannot
        // represent embedded nulls without silently truncating the result.
        if (storage.back() != L'\0' ||
            std::find(storage.begin(), storage.end() - 1, L'\0') != storage.end() - 1) {
            return false;
        }
        const std::size_t length = storage.size() - 1;
        if (length == 0) return true;
        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, storage.data(), static_cast<int>(length),
            nullptr, 0, nullptr, nullptr);
        if (required <= 0 || required >
                static_cast<int>(ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES)) {
            return false;
        }
        value.resize(static_cast<std::size_t>(required));
        return WideCharToMultiByte(
                   CP_UTF8, WC_ERR_INVALID_CHARS, storage.data(), static_cast<int>(length),
                   value.data(), required, nullptr, nullptr) == required;
    }

    [[nodiscard]] bool ResolveFNameLocked(
        const std::uint32_t comparison_index,
        const std::uint32_t number,
        std::string& value) const {
        if (comparison_index == 0) {
            value = "None";
            return true;
        }
        value = ResolveNameSnapshotLocked(comparison_index);
        if (value.empty()) return false;
        if (number != 0) {
            value += '_';
            value += std::to_string(number - 1U);
        }
        return value.size() <= ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES;
    }

    [[nodiscard]] bool BuildMapLandmarkBindingLocked(
        const std::uintptr_t function,
        MapLandmarkBinding& binding) const {
        try {
            std::string name;
            std::uintptr_t class_object{};
            std::uintptr_t outer_object{};
            if (!ReadReflectedObjectNameLocked(function, name) || name != "MapIconTransfer" ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.class"), class_object) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), outer_object) ||
                !ReadReflectedObjectNameLocked(class_object, name) || name != "Function" ||
                !ReadReflectedObjectNameLocked(outer_object, name) || name != "HTPlayerState") {
                return false;
            }

            std::uintptr_t property{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_value_offset{};
            if (!ReadPointerAt(*memory, function, Layout(profile, "ustruct.propertyLink"), property) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(*memory, function + Layout(profile, "ufunction.returnValueOffset"),
                    return_value_offset) ||
                num_parms != 2 || parms_size < 17 || parms_size > 128 ||
                return_value_offset != (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }

            MapLandmarkBinding candidate;
            candidate.function = function;
            candidate.parms_size = parms_size;
            candidate.object_generation = object_generation;
            bool found_id{};
            bool found_mode{};
            for (std::uint32_t count{}; property != 0 && count < 4; ++count) {
                std::uint32_t property_name_id{};
                std::int32_t array_dim{};
                std::int32_t element_size{};
                std::int32_t offset{};
                std::uintptr_t next{};
                if (!ReadValue(*memory, property + Layout(profile, "ffield.name"), property_name_id) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.arrayDim"), array_dim) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.elementSize"), element_size) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.offsetInternal"), offset) ||
                    !ReadValue(*memory, property + Layout(profile, "fproperty.propertyLinkNext"), next) ||
                    array_dim != 1 || element_size <= 0 || offset < 0 ||
                    static_cast<std::uint64_t>(offset) +
                            static_cast<std::uint64_t>(element_size) >
                        parms_size) {
                    return false;
                }
                const std::string property_name = ResolveNameSnapshotLocked(property_name_id);
                if (property_name == "TeleportID" && element_size == 16 && !found_id) {
                    candidate.teleport_id_offset = static_cast<std::uint16_t>(offset);
                    found_id = true;
                } else if (property_name == "InIconTransferFunc" && element_size == 1 && !found_mode) {
                    candidate.transfer_mode_offset = static_cast<std::uint16_t>(offset);
                    found_mode = true;
                } else {
                    return false;
                }
                property = next;
            }
            if (property != 0 || !found_id || !found_mode) return false;
            candidate.available = true;
            binding = candidate;
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool ResolveGameDataLocked(std::uintptr_t& game_data) const {
        game_data = 0;
        std::uintptr_t function{};
        std::uintptr_t class_object{};
        std::uintptr_t class_default_object{};
        std::uint8_t num_parms{};
        std::uint16_t parms_size{};
        std::uint16_t return_value_offset{};
        if (!FindExactObjectLocked(L"/Script/HTGame.HTGameData.GetGameData", function) ||
            !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), class_object) ||
            !ReadPointerAt(*memory, class_object, Layout(profile, "uclass.classDefaultObject"),
                class_default_object) ||
            !ReadValue(*memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
            !ReadValue(*memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
            !ReadValue(*memory, function + Layout(profile, "ufunction.returnValueOffset"),
                return_value_offset) ||
            num_parms != 1 || parms_size < sizeof(std::uintptr_t) || parms_size > 64 ||
            return_value_offset > parms_size - sizeof(std::uintptr_t) || !process_event_invoker) {
            return false;
        }
        std::array<std::uint8_t, 64> parameters{};
        try {
            if (!ReadableRange(*memory, class_default_object, 0x20U) ||
                !ReadableRange(*memory, function, 0x20U) ||
                !InvokeProcessEventGuarded(
                    process_event_invoker, class_default_object, function,
                    parameters.data(), parms_size)) {
                return false;
            }
        } catch (...) {
            return false;
        }
        std::memcpy(&game_data, parameters.data() + return_value_offset, sizeof(game_data));
        return game_data != 0;
    }

    [[nodiscard]] bool ScanMapLandmarksLocked(MapLandmarkCatalog& catalog) {
        std::uintptr_t map_function{};
        if (!FindExactObjectLocked(
                L"/Script/HTGame.HTPlayerState.MapIconTransfer", map_function) ||
            !BuildMapLandmarkBindingLocked(map_function, map_landmark_binding)) {
            return false;
        }

        std::uintptr_t game_data{};
        std::uintptr_t table{};
        std::uintptr_t row_struct{};
        std::string row_struct_name;
        if (!ResolveGameDataLocked(game_data) ||
            !ReadPointerAt(*memory, game_data, Layout(profile, "gameData.teleportPointDataTable"), table) ||
            !ReadPointerAt(*memory, table, Layout(profile, "dataTable.rowStruct"), row_struct) ||
            !ReadReflectedObjectNameLocked(row_struct, row_struct_name) ||
            row_struct_name != "TeleportPoint") {
            return false;
        }

        const auto row_map = static_cast<std::uintptr_t>(Layout(profile, "dataTable.rowMap"));
        std::uintptr_t data{};
        std::int32_t num{};
        std::int32_t num_free{};
        std::int32_t max{};
        std::uintptr_t flags_data{};
        std::int32_t flags_num{};
        std::int32_t flags_max{};
        if (!ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapData"), data) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapNum"), num) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapNumFree"), num_free) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapMax"), max) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapFlagsData"), flags_data) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapFlagsNum"), flags_num) ||
            !ReadValue(*memory, table + row_map + Layout(profile, "dataTable.rowMapFlagsMax"), flags_max) ||
            data == 0 || num <= 0 || num_free < 0 || num_free > num || max < num ||
            flags_num < num || flags_max < flags_num ||
            max > Layout(profile, "dataTable.maxRows")) {
            return false;
        }

        const auto word_count = static_cast<std::size_t>((flags_num + 31) / 32);
        if (word_count == 0 || word_count > 128) return false;
        std::vector<std::uint32_t> flags(word_count);
        if (flags_data != 0) {
            if (!memory->Read(flags_data, flags.data(), flags.size() * sizeof(std::uint32_t))) return false;
        } else if (word_count > 4 || !memory->Read(
                       table + row_map + Layout(profile, "dataTable.rowMapInlineFlags"),
                       flags.data(), flags.size() * sizeof(std::uint32_t))) {
            return false;
        }

        const auto row_stride = static_cast<std::size_t>(Layout(profile, "dataTable.rowMapElementStride"));
        const auto row_offset = static_cast<std::size_t>(Layout(profile, "dataTable.rowMapRowOffset"));
        const auto belongs_level = static_cast<std::size_t>(Layout(profile, "teleportPoint.belongsLevel"));
        const auto floor_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.floor"));
        const auto position_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.transformTranslation"));
        const auto type_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.type"));
        const auto can_teleport_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.canTeleport"));
        const auto override_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.overrideTransform"));
        const auto destination_offset = static_cast<std::size_t>(Layout(profile, "teleportPoint.overrideTranslation"));
        const auto row_bytes = (std::max)({belongs_level + sizeof(NativeUtf16StringHeader),
            floor_offset + sizeof(std::int32_t), position_offset + sizeof(double) * 3U,
            type_offset + sizeof(std::uint32_t), can_teleport_offset + sizeof(std::uint8_t),
            override_offset + sizeof(std::uint8_t), destination_offset + sizeof(double) * 3U});
        if (row_stride < row_offset + sizeof(std::uintptr_t) || row_stride > 256 ||
            row_bytes > 4096) {
            return false;
        }

        const auto slot_count = static_cast<std::size_t>(num);
        std::vector<std::uint8_t> elements(slot_count * row_stride);
        if (!memory->Read(data, elements.data(), elements.size())) return false;

        catalog.entries.clear();
        catalog.entries.reserve(slot_count);
        std::vector<std::uint8_t> row(row_bytes);
        for (std::int32_t index{}; index < num; ++index) {
            if ((flags[static_cast<std::size_t>(index) / 32U] &
                    (1U << (static_cast<std::uint32_t>(index) & 31U))) == 0) {
                continue;
            }
            const auto* const element = elements.data() + static_cast<std::size_t>(index) * row_stride;
            std::uint32_t comparison_index{};
            std::uint32_t number{};
            std::uintptr_t row_address{};
            std::memcpy(&comparison_index, element, sizeof(comparison_index));
            std::memcpy(&number, element + sizeof(comparison_index), sizeof(number));
            std::memcpy(&row_address, element + row_offset, sizeof(row_address));
            if (row_address == 0 || !memory->Read(row_address, row.data(), row.size())) continue;

            std::uint8_t can_teleport{};
            std::uint8_t destination_overridden{};
            MapLandmarkRecord record;
            NativeUtf16StringHeader native_world{};
            std::memcpy(&can_teleport, row.data() + can_teleport_offset, sizeof(can_teleport));
            std::memcpy(&destination_overridden, row.data() + override_offset,
                sizeof(destination_overridden));
            if (can_teleport == 0) continue;
            std::memcpy(&native_world, row.data() + belongs_level, sizeof(native_world));
            std::memcpy(&record.floor, row.data() + floor_offset, sizeof(record.floor));
            std::memcpy(record.world_position.data(), row.data() + position_offset,
                sizeof(record.world_position));
            std::memcpy(&record.point_type, row.data() + type_offset, sizeof(record.point_type));
            if (!ResolveFNameLocked(comparison_index, number, record.teleport_id) ||
                !DecodeUtf16StringLocked(native_world, record.world) ||
                !std::ranges::all_of(record.world_position,
                    [](const double value) { return std::isfinite(value); })) {
                continue;
            }
            record.destination = record.world_position;
            record.destination_overridden = destination_overridden != 0;
            if (record.destination_overridden) {
                std::memcpy(record.destination.data(), row.data() + destination_offset,
                    sizeof(record.destination));
                if (!std::ranges::all_of(record.destination,
                        [](const double value) { return std::isfinite(value); })) {
                    continue;
                }
            }
            catalog.entries.push_back(std::move(record));
        }
        std::ranges::sort(catalog.entries, [](const MapLandmarkRecord& left,
                                              const MapLandmarkRecord& right) {
            return left.world == right.world ? left.teleport_id < right.teleport_id
                                             : left.world < right.world;
        });
        catalog.object_generation = object_generation;
        return true;
    }

    void RefreshMapLandmarksLocked(const std::uint64_t sequence) noexcept {
        try {
            if (!NteMapLandmarksAvailable()) {
                map_landmark_binding = {};
                map_landmark_catalog.reset();
                map_landmark_next_refresh_sequence = sequence + 120U;
                return;
            }
            if (map_landmark_catalog &&
                map_landmark_catalog->object_generation == object_generation) {
                return;
            }
            if (sequence < map_landmark_next_refresh_sequence) return;

            auto catalog = std::make_shared<MapLandmarkCatalog>();
            if (!ScanMapLandmarksLocked(*catalog)) {
                map_landmark_next_refresh_sequence = sequence + 120U;
                return;
            }
            ++map_landmark_catalog_sequence;
            if (map_landmark_catalog_sequence == 0) ++map_landmark_catalog_sequence;
            catalog->sequence = map_landmark_catalog_sequence;
            map_landmark_catalog = std::move(catalog);
            map_landmark_next_refresh_sequence = 0;
        } catch (...) {
            map_landmark_next_refresh_sequence = sequence + 120U;
        }
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

    struct ReflectedPropertyInfo {
        std::uintptr_t property{};
        std::uintptr_t next{};
        std::string name;
        std::string type;
        std::int32_t array_dim{};
        std::int32_t element_size{};
        std::int32_t offset{};
    };

    [[nodiscard]] bool ReadReflectedPropertyLocked(
        const std::uintptr_t property,
        ReflectedPropertyInfo& info) const {
        std::uintptr_t name_address{};
        std::uintptr_t next_address{};
        std::uint32_t name_id{};
        ReflectedPropertyInfo candidate;
        candidate.property = property;
        if (property == 0 ||
            !AddAddress(property, Layout(profile, "ffield.name"), name_address) ||
            !AddAddress(
                property, Layout(profile, "fproperty.propertyLinkNext"), next_address) ||
            !ReadValue(*memory, name_address, name_id) ||
            !ReadValue(
                *memory, property + Layout(profile, "fproperty.arrayDim"),
                candidate.array_dim) ||
            !ReadValue(
                *memory, property + Layout(profile, "fproperty.elementSize"),
                candidate.element_size) ||
            !ReadValue(
                *memory, property + Layout(profile, "fproperty.offsetInternal"),
                candidate.offset) ||
            !ReadValue(*memory, next_address, candidate.next) ||
            !(candidate.name = ResolveNameSnapshotLocked(name_id)).size() ||
            !ReadReflectedFieldClassNameLocked(property, candidate.type)) {
            return false;
        }
        info = std::move(candidate);
        return true;
    }

    [[nodiscard]] bool ObjectHandleLocked(
        const std::uintptr_t object,
        AnomalyGenerationHandleV1& handle) const noexcept {
        handle = {};
        std::uintptr_t index_address{};
        std::int32_t index{-1};
        if (object == 0 || object_registry.items == 0 ||
            !AddAddress(object, Layout(profile, "object.internalIndex"), index_address) ||
            !ReadValue(*memory, index_address, index) || index < 0 ||
            static_cast<std::uint64_t>(index) >= object_registry.count) {
            return false;
        }
        std::uintptr_t registered{};
        std::uint32_t serial{};
        if (!ReadObjectSlot(
                *memory, object_registry, static_cast<std::uint32_t>(index),
                registered, serial) || registered != object) {
            return false;
        }
        handle = {
            EncodeObjectHandle(static_cast<std::uint32_t>(index), serial),
            object_generation};
        return true;
    }

    [[nodiscard]] bool ResolveObjectHandleLocked(
        const AnomalyGenerationHandleV1 handle,
        std::uintptr_t& object) const noexcept {
        object = 0;
        if (handle.generation != object_generation || handle.id == 0) return false;
        const std::uint32_t encoded_index = static_cast<std::uint32_t>(handle.id);
        if (encoded_index == 0) return false;
        const std::uint32_t index = encoded_index - 1U;
        const std::uint32_t serial = static_cast<std::uint32_t>(handle.id >> 32U);
        std::uint32_t observed_serial{};
        return ReadObjectSlot(*memory, object_registry, index, object, observed_serial) &&
            object != 0 && observed_serial == serial;
    }

    [[nodiscard]] bool IsClassDerivedFromLocked(
        std::uintptr_t candidate,
        const std::uintptr_t expected_base) const noexcept {
        constexpr std::size_t kMaximumDepth = 128;
        for (std::size_t depth{};
             candidate != 0 && depth < kMaximumDepth;
             ++depth) {
            if (candidate == expected_base) return true;
            std::uintptr_t next{};
            if (!ReadValue(
                    *memory,
                    candidate + Layout(profile, "ustruct.superStruct"),
                    next) || next == candidate) {
                return false;
            }
            candidate = next;
        }
        return false;
    }

    [[nodiscard]] std::string ObjectPathLocked(
        std::uintptr_t object) const {
        constexpr std::size_t kMaximumDepth = 64;
        std::vector<std::string> names;
        names.reserve(8);
        for (std::size_t depth{}; object != 0 && depth < kMaximumDepth; ++depth) {
            std::string name;
            if (!ReadReflectedObjectNameLocked(object, name)) return {};
            names.push_back(std::move(name));
            std::uintptr_t outer{};
            if (!ReadValue(
                    *memory, object + Layout(profile, "object.outer"), outer) ||
                outer == object) {
                return {};
            }
            object = outer;
        }
        if (object != 0 || names.empty()) return {};
        std::string path;
        for (auto iterator = names.rbegin(); iterator != names.rend(); ++iterator) {
            if (!path.empty()) path.push_back('.');
            path.append(*iterator);
        }
        return path;
    }

    [[nodiscard]] bool ReadReflectedBoolParameterLocked(
        const ReflectedPropertyInfo& property,
        const std::uint16_t parms_size,
        ReflectedBoolParameter& parameter) const noexcept {
        std::uint8_t field_size{};
        std::uint8_t byte_offset{};
        std::uint8_t byte_mask{};
        std::uint8_t field_mask{};
        if (property.type != "BoolProperty" || property.array_dim != 1 ||
            property.element_size != 1 || property.offset < 0 ||
            !ReadValue(
                *memory,
                property.property + Layout(profile, "fboolProperty.fieldSize"),
                field_size) ||
            !ReadValue(
                *memory,
                property.property + Layout(profile, "fboolProperty.byteOffset"),
                byte_offset) ||
            !ReadValue(
                *memory,
                property.property + Layout(profile, "fboolProperty.byteMask"),
                byte_mask) ||
            !ReadValue(
                *memory,
                property.property + Layout(profile, "fboolProperty.fieldMask"),
                field_mask) ||
            field_size == 0 || byte_offset >= field_size || byte_mask == 0 ||
            field_mask == 0 || (byte_mask & field_mask) != byte_mask ||
            static_cast<std::uint64_t>(property.offset) + byte_offset >= parms_size ||
            static_cast<std::uint64_t>(property.offset) + byte_offset >
                (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        parameter = {
            static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(property.offset) + byte_offset),
            field_mask,
            byte_mask};
        return true;
    }

    [[nodiscard]] bool BuildNteFunctionBindingLocked(
        const std::uintptr_t function,
        const NteFunctionKind kind,
        NteFunctionBinding& binding) const {
        try {
            const NteFunctionSpec spec = NteSpec(kind);
            if (spec.name.empty() || spec.parameters.empty() ||
                spec.parameters.size() > kMaximumNteFunctionParameters) {
                return false;
            }
            std::string function_name;
            if (!ReadReflectedObjectNameLocked(function, function_name) ||
                function_name != spec.name) {
                return false;
            }
            std::uintptr_t function_class{};
            std::uintptr_t outer{};
            std::string function_class_name;
            std::string outer_name;
            if (!ReadPointerAt(
                    *memory, function, Layout(profile, "object.class"), function_class) ||
                !ReadPointerAt(*memory, function, Layout(profile, "object.outer"), outer) ||
                !ReadReflectedObjectNameLocked(function_class, function_class_name) ||
                !ReadReflectedObjectNameLocked(outer, outer_name) ||
                function_class_name != "Function" || outer_name != spec.outer) {
                return false;
            }

            std::uintptr_t property{};
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            std::uint16_t return_offset{};
            if (!ReadPointerAt(
                    *memory, function, Layout(profile, "ustruct.propertyLink"), property) ||
                !ReadValue(
                    *memory, function + Layout(profile, "ufunction.numParms"), num_parms) ||
                !ReadValue(
                    *memory, function + Layout(profile, "ufunction.parmsSize"), parms_size) ||
                !ReadValue(
                    *memory,
                    function + Layout(profile, "ufunction.returnValueOffset"),
                    return_offset) ||
                num_parms != spec.parameters.size() || parms_size != spec.parms_size) {
                return false;
            }
            const bool has_return = std::ranges::any_of(
                spec.parameters,
                [](const NteFunctionParameterSpec& parameter) {
                    return parameter.return_value;
                });
            if (!has_return &&
                return_offset != (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }

            NteFunctionBinding candidate;
            candidate.function = function;
            candidate.outer_class = outer;
            candidate.parms_size = parms_size;
            std::array<bool, kMaximumNteFunctionParameters> found{};
            std::size_t property_count{};
            while (property != 0 && property_count < kMaximumNteFunctionParameters) {
                ReflectedPropertyInfo info;
                if (!ReadReflectedPropertyLocked(property, info) || info.array_dim != 1 ||
                    info.element_size <= 0 || info.offset < 0 ||
                    static_cast<std::uint64_t>(info.offset) +
                            static_cast<std::uint64_t>(info.element_size) >
                        parms_size) {
                    return false;
                }
                std::size_t parameter_index = spec.parameters.size();
                for (std::size_t index{}; index < spec.parameters.size(); ++index) {
                    if (spec.parameters[index].name == info.name) {
                        parameter_index = index;
                        break;
                    }
                }
                if (parameter_index == spec.parameters.size() || found[parameter_index]) {
                    return false;
                }
                const NteFunctionParameterSpec& expected = spec.parameters[parameter_index];
                if (expected.type != info.type ||
                    expected.element_size != info.element_size ||
                    static_cast<std::uint64_t>(info.offset) >
                        (std::numeric_limits<std::uint16_t>::max)()) {
                    return false;
                }
                candidate.offsets[parameter_index] =
                    static_cast<std::uint16_t>(info.offset);
                if (info.type == "BoolProperty" &&
                    !ReadReflectedBoolParameterLocked(
                        info, parms_size, candidate.bool_parameters[parameter_index])) {
                    return false;
                }
                if (info.type == "ClassProperty") {
                    std::uintptr_t meta_class{};
                    std::string meta_class_name;
                    const std::string_view expected_meta_class =
                        kind == NteFunctionKind::ActivateAbilityByClass
                        ? std::string_view{"HTGameplayAbility"}
                        : kind == NteFunctionKind::GetActiveEffectTimeRemainingAndDuration
                            ? std::string_view{"GameplayEffect"}
                            : std::string_view{};
                    if (!ReadPointerAt(
                            *memory, info.property,
                            Layout(profile, "fclassProperty.metaClass"), meta_class) ||
                        !ReadReflectedObjectNameLocked(meta_class, meta_class_name) ||
                        expected_meta_class.empty() ||
                        meta_class_name != expected_meta_class) {
                        return false;
                    }
                    candidate.meta_class = meta_class;
                }
                if (info.type == "ObjectProperty" && expected.return_value) {
                    std::uintptr_t property_class{};
                    std::string property_class_name;
                    const std::string_view expected_class =
                        kind == NteFunctionKind::GetAbilitySystemComponent
                        ? std::string_view{"HTAbilitySystemComponent"}
                        : kind == NteFunctionKind::GetAttackTarget
                            ? std::string_view{"HTAbilityCharacter"}
                            : std::string_view{};
                    if (expected_class.empty() ||
                        !ReadPointerAt(
                            *memory, info.property,
                            Layout(profile, "fobjectProperty.propertyClass"),
                            property_class) ||
                        !ReadReflectedObjectNameLocked(
                            property_class, property_class_name) ||
                        property_class_name != expected_class) {
                        return false;
                    }
                    candidate.meta_class = property_class;
                }
                if (expected.return_value &&
                    return_offset != static_cast<std::uint16_t>(info.offset)) {
                    return false;
                }
                found[parameter_index] = true;
                ++property_count;
                property = info.next;
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

    struct NteStructFieldSpec {
        std::string_view name;
        std::string_view type;
        std::string_view structure;
        std::int32_t offset{};
        std::int32_t element_size{};
        std::uint8_t bool_mask{};
    };

    [[nodiscard]] bool FindReflectedPropertyLocked(
        std::uintptr_t structure,
        const std::string_view name,
        ReflectedPropertyInfo& result,
        const bool include_super) const {
        constexpr std::size_t kMaximumClassDepth = 128;
        constexpr std::size_t kMaximumProperties = 512;
        for (std::size_t depth{};
             structure != 0 && depth < kMaximumClassDepth;
             ++depth) {
            std::uintptr_t property{};
            if (!ReadValue(
                    *memory,
                    structure + Layout(profile, "ustruct.propertyLink"),
                    property)) {
                return false;
            }
            for (std::size_t count{}; property != 0 && count < kMaximumProperties; ++count) {
                ReflectedPropertyInfo info;
                if (!ReadReflectedPropertyLocked(property, info)) return false;
                if (info.name == name) {
                    result = std::move(info);
                    return true;
                }
                if (info.next == property) return false;
                property = info.next;
            }
            if (property != 0 || !include_super) return false;
            std::uintptr_t next{};
            if (!ReadValue(
                    *memory,
                    structure + Layout(profile, "ustruct.superStruct"),
                    next) || next == structure) {
                return false;
            }
            structure = next;
        }
        return false;
    }

    [[nodiscard]] bool ValidateStructFieldLocked(
        const std::uintptr_t structure,
        const NteStructFieldSpec& expected,
        const bool include_super = false) const {
        ReflectedPropertyInfo property;
        if (!FindReflectedPropertyLocked(
                structure, expected.name, property, include_super) ||
            property.array_dim != 1 || property.offset != expected.offset ||
            property.element_size != expected.element_size) {
            return false;
        }
        const bool byte_like = expected.type == "ByteLike" &&
            (property.type == "ByteProperty" || property.type == "EnumProperty" ||
                property.type == "UInt8Property");
        if (!byte_like && property.type != expected.type) return false;
        if (!expected.structure.empty()) {
            std::uintptr_t nested{};
            std::string name;
            if (!ReadPointerAt(
                    *memory, property.property,
                    Layout(profile, "fstructProperty.struct"), nested) ||
                !ReadReflectedObjectNameLocked(nested, name) ||
                name != expected.structure) {
                return false;
            }
        }
        if (expected.bool_mask != 0) {
            ReflectedBoolParameter reflected;
            const auto extent = static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(expected.offset) + 1U);
            if (!ReadReflectedBoolParameterLocked(property, extent, reflected) ||
                reflected.byte_offset != expected.offset ||
                reflected.field_mask != expected.bool_mask) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool ValidateDamageEventLayoutLocked(
        const std::uintptr_t damage_event_structure) const {
        try {
            return ValidateStructFieldLocked(
                       damage_event_structure,
                       NteStructFieldSpec{
                           "Damage", "FloatProperty", {},
                           static_cast<std::int32_t>(
                               Layout(profile, "damageEvent.damage")),
                           4},
                       true) &&
                ValidateStructFieldLocked(
                    damage_event_structure,
                    NteStructFieldSpec{
                        "DamageGEDef", "WeakObjectProperty", {},
                        static_cast<std::int32_t>(
                            Layout(profile, "damageEvent.damageGEDef")),
                        8}) &&
                ValidateStructFieldLocked(
                    damage_event_structure,
                    NteStructFieldSpec{
                        "DamageTags", "StructProperty", "GameplayTagContainer",
                        static_cast<std::int32_t>(
                            Layout(profile, "damageEvent.damageTags")),
                        0x20});
        } catch (...) {
            return false;
        }
    }


    [[nodiscard]] bool ValidateSkillLayoutLocked(
        const std::uintptr_t ability_system_class) const {
        try {
            ReflectedPropertyInfo activatable;
            std::uintptr_t container_structure{};
            std::string container_name;
            if (!FindReflectedPropertyLocked(
                    ability_system_class, "ActivatableAbilities", activatable, true) ||
                activatable.type != "StructProperty" || activatable.array_dim != 1 ||
                activatable.offset != Layout(profile, "abilitySystem.activatableAbilities") ||
                activatable.element_size <
                    Layout(profile, "abilitySpecContainer.items") + 16 ||
                !ReadPointerAt(
                    *memory, activatable.property,
                    Layout(profile, "fstructProperty.struct"), container_structure) ||
                !ReadReflectedObjectNameLocked(container_structure, container_name) ||
                container_name != "GameplayAbilitySpecContainer") {
                return false;
            }
            ReflectedPropertyInfo items;
            std::uintptr_t inner{};
            std::string inner_type;
            std::uintptr_t spec_structure{};
            std::string spec_name;
            std::int32_t spec_size{};
            if (!FindReflectedPropertyLocked(
                    container_structure, "Items", items, false) ||
                items.type != "ArrayProperty" || items.array_dim != 1 ||
                items.element_size != 16 ||
                items.offset != Layout(profile, "abilitySpecContainer.items") ||
                !ReadPointerAt(
                    *memory, items.property, Layout(profile, "farrayProperty.inner"), inner) ||
                !ReadReflectedFieldClassNameLocked(inner, inner_type) ||
                inner_type != "StructProperty" ||
                !ReadValue(
                    *memory, inner + Layout(profile, "fproperty.elementSize"), spec_size) ||
                spec_size != Layout(profile, "abilitySpec.stride") ||
                !ReadPointerAt(
                    *memory, inner, Layout(profile, "fstructProperty.struct"), spec_structure) ||
                !ReadReflectedObjectNameLocked(spec_structure, spec_name) ||
                spec_name != "GameplayAbilitySpec") {
                return false;
            }
            const std::array fields{
                NteStructFieldSpec{"Handle", "StructProperty", "GameplayAbilitySpecHandle",
                    static_cast<std::int32_t>(Layout(profile, "abilitySpec.handle")), 4},
                NteStructFieldSpec{"Ability", "ObjectProperty", {},
                    static_cast<std::int32_t>(Layout(profile, "abilitySpec.ability")), 8},
                NteStructFieldSpec{"Level", "IntProperty", {},
                    static_cast<std::int32_t>(Layout(profile, "abilitySpec.level")), 4},
                NteStructFieldSpec{"InputID", "IntProperty", {},
                    static_cast<std::int32_t>(Layout(profile, "abilitySpec.inputId")), 4},
                NteStructFieldSpec{"ActiveCount", "ByteLike", {},
                    static_cast<std::int32_t>(Layout(profile, "abilitySpec.activeCount")), 1},
                NteStructFieldSpec{"InputPressed", "BoolProperty", {},
                    static_cast<std::int32_t>(Layout(profile, "abilitySpec.stateBits")), 1, 0x01},
                NteStructFieldSpec{"RemoveAfterActivation", "BoolProperty", {},
                    static_cast<std::int32_t>(Layout(profile, "abilitySpec.stateBits")), 1, 0x02},
                NteStructFieldSpec{"PendingRemove", "BoolProperty", {},
                    static_cast<std::int32_t>(Layout(profile, "abilitySpec.stateBits")), 1, 0x04}};
            return std::ranges::all_of(fields, [&](const auto& expected) {
                return ValidateStructFieldLocked(spec_structure, expected, true);
            });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool ValidateSkillCooldownLayoutLocked(
        const std::uintptr_t gameplay_ability_class) const {
        try {
            ReflectedPropertyInfo cooldown_effect;
            std::uintptr_t meta_class{};
            std::string meta_class_name;
            return FindReflectedPropertyLocked(
                    gameplay_ability_class,
                    "CooldownGameplayEffectClass",
                    cooldown_effect,
                    true) &&
                cooldown_effect.type == "ClassProperty" &&
                cooldown_effect.array_dim == 1 &&
                cooldown_effect.element_size ==
                    static_cast<std::int32_t>(sizeof(std::uintptr_t)) &&
                cooldown_effect.offset ==
                    Layout(profile, "ability.cooldownGameplayEffectClass") &&
                ReadPointerAt(
                    *memory,
                    cooldown_effect.property,
                    Layout(profile, "fclassProperty.metaClass"),
                    meta_class) &&
                ReadReflectedObjectNameLocked(meta_class, meta_class_name) &&
                meta_class_name == "GameplayEffect";
        } catch (...) {
            return false;
        }
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

    void RefreshCombatSkillBindingsLocked() noexcept {
        try {
            // Reflection discovery is demand driven. Publishing the service does
            // not justify walking the game's object registry on every tick when
            // no consumer has requested a combat or skill sample.
            const bool combat_requested = combat_demand.load(std::memory_order_acquire);
            const bool skills_requested = skill_demand.load(std::memory_order_acquire);
            if (!combat_requested && !skills_requested) return;
            const bool combat_profile = NteCombatProfileAvailable();
            const bool skills_profile = NteSkillsProfileAvailable();
            if ((!combat_profile && !skills_profile) || object_registry.items == 0 ||
                object_registry.count == 0) {
                InvalidateCombatSkillDiscoveryLocked();
                return;
            }
            if (combat_skill_discovery.object_generation != object_generation) {
                InvalidateCombatSkillDiscoveryLocked();
            }
            // Name-table indexing is demand-driven and generation-local. Each
            // table's sparse element buffer is read once; FText row payloads
            // are decoded later only for requested keys.
            RefreshDisplayTablesLocked(tick_sequence.load(std::memory_order_relaxed));

            // The Dumper SDK supplies stable reflected paths for the native
            // functions used by combat and skills. Resolve those paths on a
            // generation-local retry interval, never by walking GObjects. The
            // binding builder still validates the live UObject metadata, so
            // this is a lookup optimization rather than a new ABI source.
            const auto current_sequence = tick_sequence.load(std::memory_order_relaxed);
            if (!combat_skill_discovery.direct_lookup_succeeded &&
                current_sequence >= combat_skill_discovery.direct_lookup_next_sequence &&
                ObjectFindAvailable()) {
                const auto retry_interval = combat_skill_discovery.direct_lookup_retry_interval;
                combat_skill_discovery.direct_lookup_next_sequence =
                    current_sequence + retry_interval;
                CombatCaptureBindings next_capture_bindings;
                bool direct_lookup_succeeded = true;
                const auto make_path = [](const NteFunctionSpec& spec) {
                    std::wstring path = L"/Script/HTGame.";
                    path.reserve(path.size() + spec.outer.size() + 1U + spec.name.size());
                    for (const char value : spec.outer) {
                        path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(value)));
                    }
                    path.push_back(L'.');
                    for (const char value : spec.name) {
                        path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(value)));
                    }
                    return path;
                };
                const auto lookup_function = [&](const NteFunctionKind kind) {
                    const auto spec = NteSpec(kind);
                    std::uintptr_t function{};
                    NteFunctionBinding binding;
                    const auto path = make_path(spec);
                    if (!FindExactObjectLocked(path.c_str(), function) ||
                        !BuildNteFunctionBindingLocked(function, kind, binding)) {
                        direct_lookup_succeeded = false;
                        return;
                    }
                    combat_skill_discovery.functions[NteIndex(kind)] = binding;
                    if (kind == NteFunctionKind::GetAbilitySystemComponent) {
                        combat_skill_discovery.ability_system_class = binding.meta_class;
                    } else if (kind == NteFunctionKind::ActivateAbilityByClass) {
                        combat_skill_discovery.ability_system_class = binding.outer_class;
                        combat_skill_discovery.gameplay_ability_class = binding.meta_class;
                    }
                };
                const auto lookup_optional = [&](const NteFunctionKind kind) {
                    const auto spec = NteSpec(kind);
                    std::uintptr_t function{};
                    NteFunctionBinding binding;
                    const auto path = make_path(spec);
                    if (FindExactObjectLocked(path.c_str(), function) &&
                        BuildNteFunctionBindingLocked(function, kind, binding)) {
                        combat_skill_discovery.functions[NteIndex(kind)] = binding;
                        const auto address = binding.function;
                        switch (kind) {
                        case NteFunctionKind::ShowDamageFloaties:
                            next_capture_bindings.damage = address;
                            next_capture_bindings.damage_info_offset = binding.offsets[0];
                            break;
                        case NteFunctionKind::MulticastShowMonsterDamageInfo:
                            next_capture_bindings.monster_damage = address;
                            if (next_capture_bindings.damage == 0) {
                                next_capture_bindings.damage_info_offset = binding.offsets[0];
                            }
                            break;
                        case NteFunctionKind::ClientShowPlayerDamageInfo:
                            next_capture_bindings.player_damage_queue = address;
                            next_capture_bindings.player_damage_queue_offset = binding.offsets[0];
                            break;
                        case NteFunctionKind::SetDamageInfo:
                            next_capture_bindings.damage_widget = address;
                            next_capture_bindings.damage_widget_info_offset = binding.offsets[1];
                            break;
                        case NteFunctionKind::OnActiveGameplayEffectAdded: {
                            const auto spec_offset = binding.offsets[1];
                            next_capture_bindings.buffs[0] = {
                                address, binding.parms_size, spec_offset,
                                static_cast<std::uint16_t>(Layout(
                                    profile, "gameplayEffectSpec.def")),
                                static_cast<std::uint16_t>(spec_offset + Layout(
                                    profile, "gameplayEffectSpec.duration")),
                                static_cast<std::uint16_t>(spec_offset + Layout(
                                    profile, "gameplayEffectSpec.stackCount")),
                                0xFFFFU};
                            break;
                        }
                        case NteFunctionKind::OnAnyGameplayEffectRemoved:
                            next_capture_bindings.buffs[1] = {
                                address, binding.parms_size, binding.offsets[0],
                                static_cast<std::uint16_t>(
                                    Layout(profile, "activeGameplayEffect.spec") +
                                    Layout(profile, "gameplayEffectSpec.def")),
                                0xFFFFU, 0xFFFFU, 0xFFFFU};
                            break;
                        case NteFunctionKind::AddBuffControl:
                            next_capture_bindings.buffs[2] = {
                                address, binding.parms_size, binding.offsets[0], 0,
                                binding.offsets[1], binding.offsets[2], 0xFFFFU};
                            break;
                        case NteFunctionKind::RemoveFromBuffControl:
                            next_capture_bindings.buffs[3] = {
                                address, binding.parms_size, binding.offsets[0], 0,
                                0xFFFFU, 0xFFFFU, 0xFFFFU};
                            break;
                        case NteFunctionKind::AddBufferManagerBuffControl:
                            next_capture_bindings.buffs[4] = {
                                address, binding.parms_size, binding.offsets[1], 0,
                                binding.offsets[3], binding.offsets[4], binding.offsets[2]};
                            break;
                        case NteFunctionKind::AddHeadUpBattleMarkBuffControl:
                            next_capture_bindings.buffs[5] = {
                                address, binding.parms_size, binding.offsets[0], 0xFFFFU,
                                binding.offsets[1], binding.offsets[2], 0xFFFFU};
                            break;
                        case NteFunctionKind::RemoveHeadUpBattleMarkBuffControl:
                            next_capture_bindings.buffs[6] = {
                                address, binding.parms_size, binding.offsets[0], 0xFFFFU,
                                0xFFFFU, 0xFFFFU, 0xFFFFU};
                            break;
                        case NteFunctionKind::AddMonsterBufferControl:
                            next_capture_bindings.buffs[7] = {
                                address, binding.parms_size, binding.offsets[1], 0xFFFFU,
                                binding.offsets[3], binding.offsets[4], binding.offsets[2]};
                            break;
                        default:
                            break;
                        }
                    }
                };
                if (combat_profile) {
                    std::uintptr_t damage_event{};
                    if (!FindExactObjectLocked(L"/Script/HTGame.HTDamageEvent", damage_event) ||
                        !ValidateDamageEventLayoutLocked(damage_event)) {
                        direct_lookup_succeeded = false;
                    } else {
                        combat_skill_discovery.damage_event_layout_valid = true;
                        next_capture_bindings.damage_value_offset = static_cast<std::uint16_t>(
                            Layout(profile, "damageEvent.damage"));
                        next_capture_bindings.damage_source_offset = static_cast<std::uint16_t>(
                            Layout(profile, "damageEvent.damageGEDef"));
                        next_capture_bindings.damage_tags_offset = static_cast<std::uint16_t>(
                            Layout(profile, "damageEvent.damageTags"));
                    }
                    if (combat_skill_discovery.ability_spawn_actor_class == 0 &&
                        !FindExactObjectLocked(L"/Script/HTGame.HTAbilitySpawnActor",
                            combat_skill_discovery.ability_spawn_actor_class)) {
                        direct_lookup_succeeded = false;
                    }
                    lookup_function(NteFunctionKind::GetAbilitySystemComponent);
                    lookup_optional(NteFunctionKind::GetMainCharacterId);
                    lookup_optional(NteFunctionKind::GetNpcMainCharacterId);
                    lookup_function(NteFunctionKind::GetHp);
                    lookup_function(NteFunctionKind::GetHpMax);
                    lookup_function(NteFunctionKind::GetIsDead);
                    lookup_function(NteFunctionKind::GetAttackTarget);
                    lookup_function(NteFunctionKind::GetShieldHealth);
                    // UI/achievement bindings are optional enrichers. Their
                    // absence must not disable the native damage service.
                    lookup_optional(NteFunctionKind::ShowDamageFloaties);
                    lookup_optional(NteFunctionKind::MulticastShowMonsterDamageInfo);
                    lookup_optional(NteFunctionKind::ClientShowPlayerDamageInfo);
                    lookup_optional(NteFunctionKind::SetDamageInfo);
                    lookup_optional(NteFunctionKind::CurrentDamageIsCrit);
                    lookup_optional(NteFunctionKind::OnActiveGameplayEffectAdded);
                    lookup_optional(NteFunctionKind::OnAnyGameplayEffectRemoved);
                    lookup_optional(NteFunctionKind::AddBuffControl);
                    lookup_optional(NteFunctionKind::RemoveFromBuffControl);
                    lookup_optional(NteFunctionKind::AddBufferManagerBuffControl);
                    lookup_optional(NteFunctionKind::AddHeadUpBattleMarkBuffControl);
                    lookup_optional(NteFunctionKind::RemoveHeadUpBattleMarkBuffControl);
                    lookup_optional(NteFunctionKind::AddMonsterBufferControl);
                    lookup_optional(NteFunctionKind::GetMonsterStaticData);
                    combat_skill_discovery.combat_event_bindings_attempted = true;
                }
                if (skills_profile) {
                    if (!combat_skill_discovery.functions[NteIndex(
                            NteFunctionKind::GetAbilitySystemComponent)]) {
                        lookup_function(NteFunctionKind::GetAbilitySystemComponent);
                    }
                    lookup_function(NteFunctionKind::GetActiveEffectTimeRemainingAndDuration);
                    lookup_function(NteFunctionKind::ActivateAbilityByClass);
                }
                const bool has_capture_binding =
                    (next_capture_bindings.damage_value_offset != 0xFFFFU &&
                        next_capture_bindings.damage_source_offset != 0xFFFFU) ||
                    next_capture_bindings.damage != 0 ||
                    next_capture_bindings.monster_damage != 0 ||
                    next_capture_bindings.player_damage_queue != 0 ||
                    next_capture_bindings.damage_widget != 0 ||
                    std::ranges::any_of(next_capture_bindings.buffs,
                        [](const auto& buff) { return buff.function != 0; });
                if (has_capture_binding) {
                    // Publish capture-only bindings even when a separate
                    // combat/skill snapshot function is still unavailable.
                    // This keeps damage/buff events flowing while the
                    // optional reflection set finishes resolving.
                    combat_capture_binding_slot ^= 1U;
                    auto& slot = combat_capture_binding_slots[combat_capture_binding_slot];
                    slot = next_capture_bindings;
                    combat_capture_bindings.store(&slot, std::memory_order_release);
                }
                if (direct_lookup_succeeded) {
                    combat_skill_discovery.direct_lookup_succeeded = true;
                    combat_skill_discovery.direct_lookup_retry_interval = 300U;
                } else {
                    combat_skill_discovery.direct_lookup_retry_interval =
                        (std::min)(retry_interval * 2U, 3600U);
                }
            }
            if (skills_profile && !combat_skill_discovery.skill_layout_valid &&
                combat_skill_discovery.ability_system_class != 0) {
                combat_skill_discovery.skill_layout_valid = ValidateSkillLayoutLocked(
                    combat_skill_discovery.ability_system_class);
            }
            if (skills_profile && !combat_skill_discovery.cooldown_layout_valid &&
                combat_skill_discovery.gameplay_ability_class != 0) {
                combat_skill_discovery.cooldown_layout_valid =
                    ValidateSkillCooldownLayoutLocked(
                        combat_skill_discovery.gameplay_ability_class);
            }
        } catch (...) {
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
        const ProcessEventInvoker& process_event) noexcept;

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

    static AnomalyStatusV1 ANOMALY_CALL ResolveFText(
        void* user, const std::uintptr_t address, char* destination,
        std::size_t* size) noexcept {
        return static_cast<State*>(user)->ResolveFTextAddress(address, destination, size);
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

    static std::uint64_t ANOMALY_CALL MapLandmarkSequence(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        return state.SemanticFeatureRunning("nte.map-landmarks") && state.map_landmark_catalog
            ? state.map_landmark_catalog->sequence
            : 0;
    }

    static std::uint32_t ANOMALY_CALL MapLandmarkCount(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.map-landmarks") || !state.map_landmark_catalog) {
            return 0;
        }
        return static_cast<std::uint32_t>(state.map_landmark_catalog->entries.size());
    }

    static AnomalyStatusV1 ANOMALY_CALL MapLandmarkSnapshotAt(
        void* user,
        const std::uint32_t index,
        AnomalyNteMapLandmarkSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.map-landmarks")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "map landmark service is unavailable");
        }
        const auto catalog = state.map_landmark_catalog;
        if (!catalog) return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "map landmark catalog is not ready");
        if (index >= catalog->entries.size()) return Status(ANOMALY_STATUS_V1_NOT_FOUND);

        const MapLandmarkRecord& record = catalog->entries[index];
        if (record.teleport_id.size() > ANOMALY_NTE_MAP_LANDMARK_V1_ID_MAX_BYTES ||
            record.world.size() > ANOMALY_NTE_MAP_LANDMARK_V1_WORLD_MAX_UTF8_BYTES) {
            return Status(ANOMALY_STATUS_V1_FAILED, "map landmark text exceeds the SDK limit");
        }
        *snapshot = {};
        snapshot->struct_size = sizeof(*snapshot);
        snapshot->flags = ANOMALY_NTE_MAP_LANDMARK_V1_VALID |
            (record.destination_overridden
                 ? ANOMALY_NTE_MAP_LANDMARK_V1_DESTINATION_OVERRIDDEN
                 : 0U);
        snapshot->sequence = catalog->sequence;
        snapshot->point_type = record.point_type;
        snapshot->floor = record.floor;
        std::ranges::copy(record.world_position, snapshot->world_position);
        std::ranges::copy(record.destination, snapshot->destination);
        std::memcpy(snapshot->teleport_id, record.teleport_id.data(), record.teleport_id.size());
        std::memcpy(snapshot->world, record.world.data(), record.world.size());
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL MapLandmarkTeleport(
        void* user,
        const AnomalyNteMapLandmarkTeleportRequestV1* request) noexcept {
        if (request == nullptr || request->struct_size < sizeof(*request) || request->flags != 0 ||
            request->mode > ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_SELLING_INDULGENCES) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        const DWORD bound_game_thread = state.game_thread_id.load(std::memory_order_acquire);
        if (bound_game_thread == 0 || bound_game_thread != GetCurrentThreadId()) {
            return Status(ANOMALY_STATUS_V1_CONFLICT, "map landmark teleport requires the Game thread");
        }

        MapLandmarkBinding binding;
        Ue5NteAdapter::ProcessEventInvoker invoker;
        std::string teleport_id;
        std::uintptr_t player_state{};
        {
            std::scoped_lock lock(state.mutex);
            if (!state.SemanticFeatureRunning("nte.map-landmarks")) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "map landmark service is unavailable");
            }
            const auto catalog = state.map_landmark_catalog;
            if (!catalog || request->sequence != catalog->sequence ||
                request->index >= catalog->entries.size()) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "map landmark catalog entry is stale");
            }
            if (!state.map_landmark_binding.available ||
                state.map_landmark_binding.object_generation != state.object_generation ||
                !state.process_event_invoker) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                    "map landmark transfer reflection is not ready");
            }
            PlayerLocationSample player;
            if (!state.ReadCurrentPlayerLocation(player) ||
                !ReadPointerAt(*state.memory, player.controller,
                    Layout(state.profile, "controller.playerState"), player_state)) {
                return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "local player state is unavailable");
            }
            binding = state.map_landmark_binding;
            invoker = state.process_event_invoker;
            teleport_id = catalog->entries[request->index].teleport_id;
        }

        try {
            std::vector<wchar_t> storage;
            NativeUtf16StringHeader native_id;
            const AnomalyStringViewV1 id_view{teleport_id.data(), teleport_id.size()};
            if (!EncodeAhudText(id_view, storage, native_id)) {
                return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "map landmark id is invalid UTF-8");
            }
            std::vector<std::uint8_t> parameters(binding.parms_size, 0);
            std::memcpy(parameters.data() + binding.teleport_id_offset, &native_id, sizeof(native_id));
            parameters[binding.transfer_mode_offset] = static_cast<std::uint8_t>(request->mode);
            return invoker(player_state, binding.function, parameters.data(), binding.parms_size)
                ? Status(ANOMALY_STATUS_V1_OK)
                : Status(ANOMALY_STATUS_V1_FAILED, "map landmark transfer failed");
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "map landmark transfer invocation failed");
        }
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
            const std::uint64_t tick = state.tick_sequence.load(std::memory_order_acquire);
            const bool world_changed =
                state.actor_world_generation != state.world_generation;
            if (!state.actor_frame_cache || world_changed ||
                State::SamplingDue(
                    tick, state.actor_attempt_sequence, state.sampling.actor_tick_interval)) {
                const DWORD expected = state.game_thread_id.load(std::memory_order_acquire);
                if (expected != 0 && expected == GetCurrentThreadId() &&
                    g_active_tick_callback_state.Get() == &state) {
                    state.actor_attempt_sequence = tick;
                    state.RefreshActors(tick);
                } else if (!state.actor_frame_cache || world_changed) {
                    return Status(
                        ANOMALY_STATUS_V1_UNAVAILABLE,
                        "actor discovery requires the active Game callback domain");
                }
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

    [[nodiscard]] bool FindPickupFunctionLocked(
        const std::uintptr_t object_class,
        const std::string_view requested_name,
        const std::uint8_t expected_num_parms,
        const std::uint16_t expected_parms_size,
        std::uintptr_t& result) const noexcept {
        result = 0;
        try {
            std::uintptr_t owner = object_class;
            for (std::uint32_t depth{}; owner != 0 && depth < 64; ++depth) {
                std::uintptr_t field{};
                if (!ReadNullablePointerAt(
                        *memory, owner, Layout(profile, "ustruct.children"), field)) {
                    return false;
                }
                std::uint32_t field_count{};
                for (; field != 0 && field_count < 4096; ++field_count) {
                    std::uintptr_t next{};
                    std::uintptr_t outer{};
                    std::uintptr_t field_class{};
                    std::uint8_t num_parms{};
                    std::uint16_t parms_size{};
                    std::string name;
                    std::string class_name;
                    if (!ReadNullablePointerAt(
                            *memory, field, Layout(profile, "ufield.next"), next) ||
                        !ReadPointerAt(*memory, field, Layout(profile, "object.outer"), outer) ||
                        !ReadPointerAt(
                            *memory, field, Layout(profile, "object.class"), field_class) ||
                        !ReadReflectedObjectNameLocked(field, name) ||
                        !ReadReflectedObjectNameLocked(field_class, class_name)) {
                        return false;
                    }
                    if (outer == owner && name == requested_name && class_name == "Function" &&
                        ReadValue(
                            *memory, field + Layout(profile, "ufunction.numParms"), num_parms) &&
                        ReadValue(
                            *memory, field + Layout(profile, "ufunction.parmsSize"), parms_size) &&
                        num_parms == expected_num_parms &&
                        parms_size == expected_parms_size) {
                        result = field;
                        return true;
                    }
                    if (next == field) return false;
                    field = next;
                }
                if (field != 0) return false;
                std::uintptr_t super{};
                if (!ReadNullablePointerAt(
                        *memory, owner, Layout(profile, "ustruct.superStruct"), super) ||
                    super == owner) {
                    return false;
                }
                owner = super;
            }
        } catch (...) {
        }
        return false;
    }

    [[nodiscard]] bool ReadPickupBoolLocked(
        const EntityRecord& entity,
        const std::string_view name,
        bool& value) const noexcept {
        value = false;
        try {
            ReflectedEntityProperty property;
            std::uint8_t field_size{};
            std::uint8_t byte_offset{};
            std::uint8_t byte_mask{};
            std::uint8_t field_mask{};
            std::uint8_t packed{};
            std::uintptr_t address{};
            if (!FindReflectedEntityPropertyLocked(entity, name, property) ||
                property.element_size != 1 ||
                !ReadValue(
                    *memory, property.field + Layout(profile, "fboolProperty.fieldSize"),
                    field_size) ||
                !ReadValue(
                    *memory, property.field + Layout(profile, "fboolProperty.byteOffset"),
                    byte_offset) ||
                !ReadValue(
                    *memory, property.field + Layout(profile, "fboolProperty.byteMask"),
                    byte_mask) ||
                !ReadValue(
                    *memory, property.field + Layout(profile, "fboolProperty.fieldMask"),
                    field_mask) ||
                field_size == 0 || byte_offset >= field_size || byte_mask == 0 ||
                field_mask == 0 || (byte_mask & field_mask) != byte_mask ||
                !AddAddress(
                    entity.actor,
                    static_cast<std::int64_t>(property.offset) + byte_offset,
                    address) ||
                !ReadValue(*memory, address, packed)) {
                return false;
            }
            value = (packed & byte_mask) != 0;
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool PickupEntityStillCurrentLocked(
        const EntityRecord& entity,
        std::uintptr_t& object_class) const noexcept {
        object_class = 0;
        if (!entity.object_identity_available || object_registry.items == 0) return false;
        std::uintptr_t actor{};
        std::uint32_t serial{};
        return ReadObjectSlot(
                   *memory, object_registry, entity.object_index, actor, serial) &&
            actor == entity.actor && serial == entity.object_serial &&
            ReadPointerAt(*memory, actor, Layout(profile, "object.class"), object_class) &&
            object_class == entity.class_object;
    }

    [[nodiscard]] bool InvokePickupRawLocked(
        const std::uintptr_t receiver,
        const std::uintptr_t function,
        void* const parameters,
        const std::size_t parameter_size) const noexcept {
        if (!process_event_invoker) return false;
        try {
            return InvokeProcessEventGuarded(
                process_event_invoker, receiver, function, parameters, parameter_size);
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool InvokePickupNativeLocked(
        const std::uintptr_t receiver,
        const std::uintptr_t function,
        void* const parameters,
        const std::size_t parameter_size) const noexcept {
        std::uintptr_t flags_address{};
        std::uint32_t original_flags{};
        const auto native_flag = Layout(profile, "ufunction.nativeFlag");
        if (native_flag <= 0 ||
            !AddAddress(function, Layout(profile, "ufunction.flags"), flags_address) ||
            !ReadValue(*memory, flags_address, original_flags)) {
            return false;
        }
        const auto invocation_flags = original_flags |
            static_cast<std::uint32_t>(native_flag);
        if (!memory->Write(
                flags_address, &invocation_flags, sizeof(invocation_flags))) {
            return false;
        }
        const bool invoked = InvokePickupRawLocked(
            receiver, function, parameters, parameter_size);
        const bool restored = memory->Write(
            flags_address, &original_flags, sizeof(original_flags));
        return invoked && restored;
    }

    [[nodiscard]] bool InvokeCanTryInteractLocked(
        const std::uintptr_t actor,
        const std::uintptr_t controller,
        const std::int32_t interact_index,
        const std::uintptr_t function,
        bool& can_try) const noexcept {
        const auto parameter_size = Layout(profile, "pickup.canTry.parmsSize");
        if (parameter_size <= 0 ||
            static_cast<std::size_t>(parameter_size) > kMaximumPickupParameterSize) {
            return false;
        }
        std::array<std::uint8_t, kMaximumPickupParameterSize> parameters{};
        const auto controller_offset = Layout(profile, "pickup.canTry.controller");
        const auto index_offset = Layout(profile, "pickup.canTry.index");
        const auto result_offset = Layout(profile, "pickup.canTry.returnValue");
        if (controller_offset < 0 || index_offset < 0 || result_offset < 0 ||
            static_cast<std::size_t>(controller_offset) + sizeof(controller) >
                static_cast<std::size_t>(parameter_size) ||
            static_cast<std::size_t>(index_offset) + sizeof(interact_index) >
                static_cast<std::size_t>(parameter_size) ||
            static_cast<std::size_t>(result_offset) >= static_cast<std::size_t>(parameter_size)) {
            return false;
        }
        std::memcpy(parameters.data() + controller_offset, &controller, sizeof(controller));
        std::memcpy(parameters.data() + index_offset, &interact_index, sizeof(interact_index));
        if (!InvokePickupRawLocked(
                actor, function, parameters.data(), static_cast<std::size_t>(parameter_size))) {
            return false;
        }
        can_try = parameters[static_cast<std::size_t>(result_offset)] != 0;
        return true;
    }

    [[nodiscard]] bool ReadInteractChoicesLocked(
        const std::uintptr_t actor,
        const std::uintptr_t controller,
        const std::uintptr_t function,
        std::array<std::int32_t, kMaximumPickupChoices>& choices,
        std::size_t& count) const noexcept {
        struct ArrayHeader {
            std::uintptr_t data{};
            std::int32_t count{};
            std::int32_t capacity{};
        };
        static_assert(sizeof(ArrayHeader) == 16);
        const auto parameter_size = Layout(profile, "pickup.entries.parmsSize");
        if (parameter_size <= 0 ||
            static_cast<std::size_t>(parameter_size) > kMaximumPickupParameterSize) {
            return false;
        }
        std::array<std::uint8_t, kMaximumPickupParameterSize> parameters{};
        const auto controller_offset = Layout(profile, "pickup.entries.controller");
        const auto array_offset = Layout(profile, "pickup.entries.array");
        if (controller_offset < 0 || array_offset < 0 ||
            static_cast<std::size_t>(controller_offset) + sizeof(controller) >
                static_cast<std::size_t>(parameter_size) ||
            static_cast<std::size_t>(array_offset) + sizeof(ArrayHeader) >
                static_cast<std::size_t>(parameter_size)) {
            return false;
        }
        std::memcpy(parameters.data() + controller_offset, &controller, sizeof(controller));
        if (!InvokePickupRawLocked(
                actor, function, parameters.data(), static_cast<std::size_t>(parameter_size))) {
            return false;
        }
        ArrayHeader entries;
        std::memcpy(&entries, parameters.data() + array_offset, sizeof(entries));
        choices = {};
        count = 0;
        const auto maximum_choices = Layout(profile, "pickup.entries.maximumChoices");
        if (maximum_choices <= 0 ||
            static_cast<std::size_t>(maximum_choices) > kMaximumPickupChoices) {
            return false;
        }
        if (entries.count <= 0 || entries.data == 0 ||
            entries.count > maximum_choices || entries.capacity < entries.count) {
            return true;
        }
        const auto stride = Layout(profile, "pickup.interactEntryStride");
        const auto index_offset = Layout(profile, "pickup.interactEntryIndex");
        if (stride < static_cast<std::int64_t>(sizeof(std::int32_t)) ||
            index_offset < 0 || index_offset + static_cast<std::int64_t>(sizeof(std::int32_t)) > stride) {
            return false;
        }
        for (std::int32_t index{}; index < entries.count; ++index) {
            std::uintptr_t address{};
            if (!AddAddress(
                    entries.data,
                    static_cast<std::int64_t>(index) * stride + index_offset,
                    address) ||
                !ReadValue(*memory, address, choices[count])) {
                choices = {};
                count = 0;
                return false;
            }
            ++count;
        }
        return true;
    }

    [[nodiscard]] bool InvokeTriggerInteractLocked(
        const std::uintptr_t controller,
        const std::uintptr_t actor,
        const std::int32_t interact_index,
        const std::uintptr_t function) const noexcept {
        const auto parameter_size = Layout(profile, "pickup.trigger.parmsSize");
        if (parameter_size <= 0 ||
            static_cast<std::size_t>(parameter_size) > kMaximumPickupParameterSize) {
            return false;
        }
        std::array<std::uint8_t, kMaximumPickupParameterSize> parameters{};
        const auto actor_offset = Layout(profile, "pickup.trigger.actor");
        const auto index_offset = Layout(profile, "pickup.trigger.index");
        const auto client_offset = Layout(profile, "pickup.trigger.onlyClientSide");
        if (actor_offset < 0 || index_offset < 0 || client_offset < 0 ||
            static_cast<std::size_t>(actor_offset) + sizeof(actor) >
                static_cast<std::size_t>(parameter_size) ||
            static_cast<std::size_t>(index_offset) + sizeof(interact_index) >
                static_cast<std::size_t>(parameter_size) ||
            static_cast<std::size_t>(client_offset) >= static_cast<std::size_t>(parameter_size)) {
            return false;
        }
        std::memcpy(parameters.data() + actor_offset, &actor, sizeof(actor));
        std::memcpy(parameters.data() + index_offset, &interact_index, sizeof(interact_index));
        parameters[static_cast<std::size_t>(client_offset)] = 0;
        return InvokePickupNativeLocked(
            controller, function, parameters.data(), static_cast<std::size_t>(parameter_size));
    }

    void CompletePickupLocked(const std::uint32_t status) noexcept {
        pickup_request = {};
        pickup_demand.store(false, std::memory_order_release);
        pickup_snapshot.flags |= ANOMALY_NTE_PICKUP_V1_VALID;
        pickup_snapshot.flags &= ~ANOMALY_NTE_PICKUP_V1_CHECKING_FLAG;
        if (pickup_snapshot.unconfirmed != 0) {
            pickup_snapshot.flags |= ANOMALY_NTE_PICKUP_V1_HAS_UNCONFIRMED;
        }
        pickup_snapshot.state = ANOMALY_NTE_PICKUP_V1_COMPLETE;
        pickup_snapshot.status = status;
        pickup_snapshot.checking = 0;
    }

    void RefreshPickupConfirmationLocked() noexcept {
        if (pickup_confirmation.candidates.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < pickup_confirmation.next_check) return;
        const bool expired = now >= pickup_confirmation.deadline;
        pickup_confirmation.next_check = now + std::chrono::milliseconds(100);
        std::vector<PickupConfirmationCandidate> remaining;
        remaining.reserve(pickup_confirmation.candidates.size());
        for (const PickupConfirmationCandidate& candidate : pickup_confirmation.candidates) {
            std::uintptr_t registered_actor{};
            std::uint32_t registered_serial{};
            const bool identity_read = object_registry.items != 0 && ReadObjectSlot(
                *memory, object_registry, candidate.object_index,
                registered_actor, registered_serial);
            const bool identity_current = identity_read &&
                registered_actor == candidate.actor &&
                registered_serial == candidate.object_serial;
            if (identity_read && !identity_current) {
                ++pickup_snapshot.confirmed;
                continue;
            }
            if (!identity_current) {
                if (expired) ++pickup_snapshot.unconfirmed;
                else remaining.push_back(candidate);
                continue;
            }
            const auto cache = entity_frame_cache;
            const bool cache_advanced = cache != nullptr &&
                cache->sequence > candidate.entity_sequence;
            const bool actor_still_present =
                cache == nullptr || cache->partial || !cache_advanced ||
                std::ranges::any_of(cache->entities, [&candidate](const EntityRecord& entity) {
                    return entity.actor == candidate.actor &&
                        entity.object_identity_available &&
                        entity.object_index == candidate.object_index &&
                        entity.object_serial == candidate.object_serial;
                });
            if (!actor_still_present) {
                ++pickup_snapshot.confirmed;
                continue;
            }
            std::uint8_t current{};
            std::uintptr_t address{};
            if (AddAddress(
                    candidate.actor, Layout(profile, "pickup.actor.interactFinish"), address) &&
                ReadValue(*memory, address, current) && current != candidate.baseline) {
                ++pickup_snapshot.confirmed;
                continue;
            }
            if (!expired) {
                remaining.push_back(candidate);
                continue;
            }
            bool can_try{};
            if (InvokeCanTryInteractLocked(
                    candidate.actor, candidate.controller, candidate.interact_index,
                    candidate.can_try_function, can_try) && !can_try) {
                ++pickup_snapshot.confirmed;
            } else {
                ++pickup_snapshot.unconfirmed;
            }
        }
        pickup_confirmation.candidates = std::move(remaining);
        pickup_snapshot.checking = static_cast<std::uint32_t>(
            pickup_confirmation.candidates.size());
        if (pickup_confirmation.candidates.empty()) {
            CompletePickupLocked(ANOMALY_STATUS_V1_OK);
        }
    }

    void PerformPickupLocked() noexcept {
        if (!pickup_request.queued) return;
        if (!SemanticFeatureRunning("nte.pickup")) {
            CompletePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            return;
        }
        const auto cache = entity_frame_cache;
        if (!cache || !player_available) {
            if (++pickup_request.attempts < 3) {
                pickup_demand.store(true, std::memory_order_release);
                return;
            }
            CompletePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            return;
        }
        PlayerLocationSample live;
        if (!ReadCurrentPlayerLocation(live) || live.controller != player_controller ||
            live.pawn != player_pawn || live.root != player_root ||
            !ObjectClassChainContainsLocked(live.controller, "HTPlayerController")) {
            CompletePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            return;
        }
        std::uintptr_t controller_class{};
        std::uintptr_t trigger_function{};
        const auto trigger_num_parms = Layout(profile, "pickup.trigger.numParms");
        const auto trigger_parms_size = Layout(profile, "pickup.trigger.parmsSize");
        if (!ReadPointerAt(
                *memory, live.controller, Layout(profile, "object.class"), controller_class) ||
            !FindPickupFunctionLocked(
                controller_class, "TriggerInteract",
                static_cast<std::uint8_t>(trigger_num_parms),
                static_cast<std::uint16_t>(trigger_parms_size), trigger_function)) {
            CompletePickupLocked(ANOMALY_STATUS_V1_UNAVAILABLE);
            return;
        }

        struct Candidate {
            const EntityRecord* entity{};
            double distance_squared{};
        };
        std::vector<Candidate> candidates;
        const double radius_squared = pickup_request.radius * pickup_request.radius;
        for (const EntityRecord& entity : cache->entities) {
            const auto class_name = cache->class_names.find(entity.class_id);
            if (class_name == cache->class_names.end() ||
                (class_name->second != "HTRandomItemActor" &&
                 !class_name->second.starts_with("PropBox_") &&
                 !class_name->second.starts_with("InteractBox_"))) {
                continue;
            }
            const double dx = entity.bounds_center[0] - live.position[0];
            const double dy = entity.bounds_center[1] - live.position[1];
            const double dz = entity.bounds_center[2] - live.position[2];
            const double distance_squared = dx * dx + dy * dy + dz * dz;
            if (!std::isfinite(distance_squared) || distance_squared > radius_squared) continue;
            ++pickup_snapshot.nearby;
            bool not_pickup{};
            if (!ReadPickupBoolLocked(entity, "IsNotPickUp", not_pickup) || not_pickup) {
                ++pickup_snapshot.skipped;
                continue;
            }
            candidates.push_back({&entity, distance_squared});
        }
        std::ranges::sort(candidates, {}, &Candidate::distance_squared);
        if (candidates.size() > pickup_request.maximum_items) {
            pickup_snapshot.skipped += static_cast<std::uint32_t>(
                candidates.size() - pickup_request.maximum_items);
            candidates.resize(pickup_request.maximum_items);
        }

        std::vector<PickupConfirmationCandidate> confirmation;
        confirmation.reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            std::uintptr_t actor_class{};
            if (!PickupEntityStillCurrentLocked(*candidate.entity, actor_class)) {
                ++pickup_snapshot.skipped;
                continue;
            }
            std::uintptr_t can_try_function{};
            std::uintptr_t entries_function{};
            const auto can_num_parms = Layout(profile, "pickup.canTry.numParms");
            const auto can_parms_size = Layout(profile, "pickup.canTry.parmsSize");
            const auto entries_num_parms = Layout(profile, "pickup.entries.numParms");
            const auto entries_parms_size = Layout(profile, "pickup.entries.parmsSize");
            if (!FindPickupFunctionLocked(
                    actor_class, "BPCanTryInteract",
                    static_cast<std::uint8_t>(can_num_parms),
                    static_cast<std::uint16_t>(can_parms_size), can_try_function) ||
                !FindPickupFunctionLocked(
                    actor_class, "BPGetInteractEntries",
                    static_cast<std::uint8_t>(entries_num_parms),
                    static_cast<std::uint16_t>(entries_parms_size), entries_function)) {
                ++pickup_snapshot.skipped;
                continue;
            }
            std::array<std::int32_t, kMaximumPickupChoices> choices{};
            std::size_t choice_count{};
            if (!ReadInteractChoicesLocked(
                    candidate.entity->actor, live.controller, entries_function,
                    choices, choice_count) || choice_count == 0) {
                ++pickup_snapshot.skipped;
                continue;
            }
            bool sent{};
            for (std::size_t choice_index{}; choice_index < choice_count; ++choice_index) {
                bool can_try{};
                if (!InvokeCanTryInteractLocked(
                        candidate.entity->actor, live.controller, choices[choice_index],
                        can_try_function, can_try) || !can_try) {
                    continue;
                }
                std::uint8_t baseline{};
                std::uintptr_t baseline_address{};
                const bool baseline_read = AddAddress(
                    candidate.entity->actor,
                    Layout(profile, "pickup.actor.interactFinish"), baseline_address) &&
                    ReadValue(*memory, baseline_address, baseline);
                if (!InvokeTriggerInteractLocked(
                        live.controller, candidate.entity->actor, choices[choice_index],
                        trigger_function)) {
                    continue;
                }
                ++pickup_snapshot.triggered;
                if (baseline_read) {
                    confirmation.push_back({
                        candidate.entity->actor, live.controller, can_try_function,
                        candidate.entity->object_index, candidate.entity->object_serial,
                        cache->sequence, choices[choice_index], baseline});
                } else {
                    ++pickup_snapshot.unconfirmed;
                }
                sent = true;
                break;
            }
            if (!sent) ++pickup_snapshot.skipped;
        }
        pickup_request = {};
        pickup_confirmation = {};
        pickup_confirmation.candidates = std::move(confirmation);
        if (pickup_snapshot.triggered == 0) {
            CompletePickupLocked(
                pickup_snapshot.nearby == 0
                    ? ANOMALY_STATUS_V1_OK
                    : ANOMALY_STATUS_V1_FAILED);
            return;
        }
        if (pickup_confirmation.candidates.empty()) {
            CompletePickupLocked(ANOMALY_STATUS_V1_OK);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        pickup_confirmation.next_check = now + std::chrono::milliseconds(100);
        pickup_confirmation.deadline = now + std::chrono::seconds(2);
        pickup_snapshot.flags = ANOMALY_NTE_PICKUP_V1_VALID |
            ANOMALY_NTE_PICKUP_V1_CHECKING_FLAG;
        pickup_snapshot.state = ANOMALY_NTE_PICKUP_V1_CHECKING;
        pickup_snapshot.status = ANOMALY_STATUS_V1_OK;
        pickup_snapshot.checking = static_cast<std::uint32_t>(
            pickup_confirmation.candidates.size());
    }

    static AnomalyStatusV1 ANOMALY_CALL PickupRequestNearby(
        void* user, const AnomalyNtePickupRequestV1* request) noexcept {
        if (request == nullptr || request->struct_size < sizeof(*request) ||
            request->flags != 0 || request->reserved != 0 ||
            !std::isfinite(request->radius) || request->radius < 50.0 ||
            request->radius > 5000.0 || request->maximum_items == 0 ||
            request->maximum_items > 128) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        const DWORD expected = state.game_thread_id.load(std::memory_order_acquire);
        if (expected == 0 || expected != GetCurrentThreadId() ||
            g_active_tick_callback_state.Get() != &state) {
            return Status(
                ANOMALY_STATUS_V1_CONFLICT,
                "pickup requests require the active Game callback domain");
        }
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.pickup")) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "pickup is unavailable for the active Profile");
        }
        if (state.pickup_request.queued ||
            !state.pickup_confirmation.candidates.empty()) {
            return Status(ANOMALY_STATUS_V1_CONFLICT, "pickup request is already active");
        }
        ++state.pickup_sequence;
        state.pickup_request = {
            request->radius, request->maximum_items, 0, true};
        state.pickup_confirmation = {};
        state.pickup_snapshot = {sizeof(state.pickup_snapshot)};
        state.pickup_snapshot.flags = ANOMALY_NTE_PICKUP_V1_VALID;
        state.pickup_snapshot.sequence = state.pickup_sequence;
        state.pickup_snapshot.state = ANOMALY_NTE_PICKUP_V1_QUEUED;
        state.pickup_snapshot.status = ANOMALY_STATUS_V1_OK;
        state.pickup_demand.store(true, std::memory_order_release);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL PickupSnapshot(
        void* user, AnomalyNtePickupSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.pickup")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "pickup service is unavailable");
        }
        const std::uint32_t struct_size = snapshot->struct_size;
        *snapshot = state.pickup_snapshot;
        snapshot->struct_size = struct_size;
        if (snapshot->sequence == 0) {
            snapshot->flags = ANOMALY_NTE_PICKUP_V1_VALID;
            snapshot->state = ANOMALY_NTE_PICKUP_V1_IDLE;
            snapshot->status = ANOMALY_STATUS_V1_OK;
        }
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL CombatantSnapshot(
        void* user, AnomalyNteCombatantSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.combat")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE combat service is unavailable");
        }
        if (!state.combat_available || state.world_pointer == 0) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "combatant snapshot is unavailable");
        }
        const auto current_sequence = state.tick_sequence.load(std::memory_order_acquire);
        snapshot->flags = SnapshotFlags(
            state.combat_partial, state.combat_sample_sequence, current_sequence);
        if (state.combat_dead) snapshot->flags |= ANOMALY_NTE_COMBATANT_V1_DEAD;
        snapshot->sequence = state.combat_sample_sequence;
        snapshot->world = {1, state.world_generation};
        snapshot->character = state.combat_character;
        snapshot->target = state.combat_target;
        snapshot->hp = state.combat_hp;
        snapshot->max_hp = state.combat_max_hp;
        snapshot->shield = state.combat_shield;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static std::uint64_t ANOMALY_CALL LatestDamageSequence(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        state.combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        return state.SemanticFeatureRunning("nte.combat")
            ? state.damage_event_sequence
            : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL NextDamageEvent(
        void* user,
        const std::uint64_t after_sequence,
        AnomalyNteDamageEventV1* event) noexcept {
        if (event == nullptr || event->struct_size < sizeof(*event)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.combat")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE combat service is unavailable");
        }
        if (state.damage_event_count == 0 || after_sequence >= state.damage_event_sequence) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "no newer damage event");
        }
        const auto& first = state.damage_events[state.damage_event_start].event;
        if (after_sequence != 0 && after_sequence < first.sequence - 1U) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "damage event cursor has expired");
        }
        for (std::size_t index{}; index < state.damage_event_count; ++index) {
            const auto& candidate = state.damage_events[
                (state.damage_event_start + index) % kDamageEventCapacity].event;
            if (candidate.sequence <= after_sequence) continue;
            *event = candidate;
            return Status(ANOMALY_STATUS_V1_OK);
        }
        return Status(ANOMALY_STATUS_V1_NOT_FOUND, "no newer damage event");
    }

    [[nodiscard]] static bool SameHandle(
        const AnomalyGenerationHandleV1 left,
        const AnomalyGenerationHandleV1 right) noexcept {
        return left.id == right.id && left.generation == right.generation;
    }

    template <typename Value>
    static void SaturatingAdd(
        Value& destination,
        const Value value,
        std::uint32_t& flags) noexcept {
        if constexpr (std::is_unsigned_v<Value>) {
            if (value > (std::numeric_limits<Value>::max)() - destination) {
                destination = (std::numeric_limits<Value>::max)();
                flags |= ANOMALY_NTE_COMBAT_STATISTICS_V1_OVERFLOW;
            } else {
                destination += value;
            }
        } else {
            if ((value > 0 && destination > (std::numeric_limits<Value>::max)() - value) ||
                (value < 0 && destination < (std::numeric_limits<Value>::min)() - value)) {
                destination = value > 0
                    ? (std::numeric_limits<Value>::max)()
                    : (std::numeric_limits<Value>::min)();
                flags |= ANOMALY_NTE_COMBAT_STATISTICS_V1_OVERFLOW;
            } else {
                destination += value;
            }
        }
    }

    static AnomalyStatusV1 ANOMALY_CALL CombatStatistics(
        void* user,
        const AnomalyNteCombatStatisticsRequestV1* request,
        AnomalyNteCombatStatisticsV1* statistics) noexcept {
        if (request == nullptr || request->struct_size < sizeof(*request) ||
            statistics == nullptr || statistics->struct_size < sizeof(*statistics) ||
            request->flags != 0 || request->reserved != 0 ||
            request->direction > ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_VICTIM) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.combat")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE combat service is unavailable");
        }
        if (state.world_pointer == 0 || request->world.id != 1 ||
            request->world.generation != state.world_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale combat world handle");
        }
        AnomalyNteCombatStatisticsV1 result{sizeof(result)};
        result.through_sequence = state.damage_event_sequence;
        if (state.damage_dropped_count != 0) {
            result.flags |= ANOMALY_NTE_COMBAT_STATISTICS_V1_PARTIAL;
        }
        if (state.damage_event_count != 0 &&
            state.damage_events[state.damage_event_start].event.sequence >
                state.damage_world_sequence_base + 1U) {
            result.flags |= ANOMALY_NTE_COMBAT_STATISTICS_V1_PARTIAL;
        }
        for (std::size_t index{}; index < state.damage_event_count; ++index) {
            const auto& event = state.damage_events[
                (state.damage_event_start + index) % kDamageEventCapacity].event;
            if (request->source_id != 0 && event.source_id != request->source_id) continue;
            if (request->character.id != 0) {
                const bool attacker = SameHandle(request->character, event.attacker);
                const bool victim = SameHandle(request->character, event.victim);
                if ((request->direction == ANOMALY_NTE_COMBAT_DIRECTION_V1_ANY &&
                        !attacker && !victim) ||
                    (request->direction == ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_ATTACKER &&
                        !attacker) ||
                    (request->direction == ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_VICTIM &&
                        !victim)) {
                    continue;
                }
            }
            SaturatingAdd(result.hit_count, std::uint64_t{1}, result.flags);
            if ((event.flags & ANOMALY_NTE_DAMAGE_V1_CRITICAL_VALID) == 0) {
                result.flags |= ANOMALY_NTE_COMBAT_STATISTICS_V1_PARTIAL;
            }
            if ((event.flags & ANOMALY_NTE_DAMAGE_V1_CRITICAL) != 0) {
                SaturatingAdd(result.critical_count, std::uint64_t{1}, result.flags);
            }
            if ((event.flags & ANOMALY_NTE_DAMAGE_V1_HEAD_HIT) != 0) {
                SaturatingAdd(result.head_hit_count, std::uint64_t{1}, result.flags);
            }
            SaturatingAdd(result.display_damage_total, event.display_damage, result.flags);
            SaturatingAdd(result.basic_damage_total, event.basic_damage, result.flags);
            SaturatingAdd(result.final_damage_total, event.final_damage, result.flags);
        }
        *statistics = result;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL DamageSourceName(
        void* user,
        const std::uint64_t source_id,
        char* destination,
        std::size_t* size) noexcept {
        if (size == nullptr || source_id == 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.combat")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE combat service is unavailable");
        }
        auto found = state.damage_source_names.find(source_id);
        if (found == state.damage_source_names.end()) {
            const auto object = state.damage_source_objects.find(source_id);
            if (object == state.damage_source_objects.end()) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "damage source is unavailable");
            }
            std::string path = state.ObjectPathLocked(object->second);
            if (path.empty()) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "damage source path is unavailable");
            }
            found = state.damage_source_names.emplace(source_id, std::move(path)).first;
        }
        if (found->second.empty()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "damage source name is unavailable");
        }
        return CopyString(found->second, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL DamageParticipantPath(
        void* user,
        const AnomalyGenerationHandleV1 participant,
        char* destination,
        std::size_t* size) noexcept {
        if (size == nullptr || participant.id == 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.combat")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE combat service is unavailable");
        }
        if (participant.generation != state.object_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale damage participant handle");
        }
        auto found = state.damage_participant_paths.find(participant.id);
        if (found == state.damage_participant_paths.end()) {
            std::uintptr_t object{};
            if (!state.ResolveObjectHandleLocked(participant, object)) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale damage participant handle");
            }
            std::string path = state.ObjectPathLocked(object);
            if (path.empty()) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND, "damage participant path is unavailable");
            }
            found = state.damage_participant_paths.emplace(participant.id, std::move(path)).first;
        }
        return CopyString(found->second, destination, size);
    }

    static std::uint64_t ANOMALY_CALL LatestCombatEventSequence(void* user) noexcept {
        auto& state = *static_cast<State*>(user);
        state.combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        return state.SemanticFeatureRunning("nte.combat") ? state.combat_event_sequence : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL NextCombatEvent(
        void* user, const std::uint64_t after_sequence,
        AnomalyNteCombatEventV1* event) noexcept {
        if (event == nullptr || event->struct_size < sizeof(*event)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.combat")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE combat service is unavailable");
        }
        if (state.combat_event_count == 0 || after_sequence >= state.combat_event_sequence) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "no newer combat event");
        }
        const auto& first = state.combat_events[state.combat_event_start].event;
        if (after_sequence != 0 && after_sequence < first.sequence - 1U) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "combat event cursor has expired");
        }
        for (std::size_t index{}; index < state.combat_event_count; ++index) {
            const auto& candidate = state.combat_events[
                (state.combat_event_start + index) % kCombatEventCapacity].event;
            if (candidate.sequence <= after_sequence) continue;
            *event = candidate;
            return Status(ANOMALY_STATUS_V1_OK);
        }
        return Status(ANOMALY_STATUS_V1_NOT_FOUND, "no newer combat event");
    }

    static AnomalyStatusV1 ANOMALY_CALL CombatEventName(
        void* user, const AnomalyNteCombatEventV1* event,
        char* destination, std::size_t* size) noexcept {
        if (event == nullptr || size == nullptr || event->name_id == 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        auto found = state.combat_event_names.find(event->name_id);
        if (found == state.combat_event_names.end() || found->second.empty()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "combat event name is unavailable");
        }
        return CopyString(found->second, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL ParticipantDisplayName(
        void* user, const AnomalyGenerationHandleV1 participant,
        char* destination, std::size_t* size) noexcept {
        if (size == nullptr || participant.id == 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.combat")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE combat service is unavailable");
        }
        const auto cached = state.combat_participant_names.find(participant.id);
        if (cached != state.combat_participant_names.end()) {
            return CopyString(cached->second, destination, size);
        }
        return Status(ANOMALY_STATUS_V1_NOT_FOUND, "participant display name is unavailable");
    }

    static void FillSkillSnapshot(
        const State& state,
        const SkillRecord& record,
        AnomalyNteSkillSnapshotV1& snapshot) noexcept {
        snapshot.flags = record.flags;
        if (record.sequence < state.tick_sequence.load(std::memory_order_acquire)) {
            snapshot.flags |= ANOMALY_NTE_SKILL_V1_STALE;
        }
        snapshot.handle = record.handle;
        snapshot.character = record.character;
        snapshot.ability_class = record.ability_class;
        snapshot.sequence = record.sequence;
        snapshot.level = record.level;
        snapshot.input_id = record.input_id;
        snapshot.cooldown_remaining_seconds = record.cooldown_remaining_seconds;
        snapshot.cooldown_duration_seconds = record.cooldown_duration_seconds;
    }

    static AnomalyStatusV1 ANOMALY_CALL SkillFrame(
        void* user, AnomalyNteSkillFrameV1* frame) noexcept {
        if (frame == nullptr || frame->struct_size < sizeof(*frame)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.skill_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.skills")) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "NTE skills service is unavailable");
        }
        if (!state.skills_available) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "skill frame is unavailable");
        }
        frame->flags = SnapshotFlags(
            state.skills_partial, state.skill_sample_sequence,
            state.tick_sequence.load(std::memory_order_acquire));
        frame->generation = state.skill_generation;
        frame->sequence = state.skill_sample_sequence;
        frame->character = state.skill_character;
        frame->skill_count = static_cast<std::uint32_t>(state.skills.size());
        frame->reserved = 0;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 SkillSnapshotAtLocked(
        State& state,
        const std::uint64_t generation,
        const std::uint32_t index,
        AnomalyNteSkillSnapshotV1* snapshot) noexcept {
        if (!state.SemanticFeatureRunning("nte.skills") || !state.skills_available) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "skill frame is unavailable");
        }
        if (generation != 0 && generation != state.skill_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale skill frame generation");
        }
        if (index >= state.skills.size()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "skill index is not found");
        }
        FillSkillSnapshot(state, state.skills[index], *snapshot);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL SkillSnapshotAt(
        void* user,
        const std::uint64_t generation,
        const std::uint32_t index,
        AnomalyNteSkillSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.skill_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        return SkillSnapshotAtLocked(state, generation, index, snapshot);
    }

    static AnomalyStatusV1 ANOMALY_CALL SkillPage(
        void* user,
        const AnomalyNteSkillPageRequestV1* request,
        AnomalyNteSkillSnapshotV1* destination,
        AnomalyNteSkillPageResultV1* result) noexcept {
        if (request == nullptr || request->struct_size < sizeof(*request) ||
            result == nullptr || result->struct_size < sizeof(*result) ||
            request->flags != 0 ||
            request->capacity > ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY ||
            (request->capacity != 0 && destination == nullptr)) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        for (std::uint32_t index{}; index < request->capacity; ++index) {
            if (destination[index].struct_size < sizeof(AnomalyNteSkillSnapshotV1)) {
                return Status(
                    ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                    "every skill destination must advertise its struct size");
            }
        }
        auto& state = *static_cast<State*>(user);
        state.skill_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.skills") || !state.skills_available) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "skill frame is unavailable");
        }
        const std::uint64_t generation = request->generation == 0
            ? state.skill_generation
            : request->generation;
        if (generation != state.skill_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale skill frame generation");
        }
        const std::uint32_t total = static_cast<std::uint32_t>(state.skills.size());
        const std::uint32_t offset = (std::min)(request->offset, total);
        const std::uint32_t returned = (std::min)(request->capacity, total - offset);
        for (std::uint32_t index{}; index < returned; ++index) {
            FillSkillSnapshot(state, state.skills[offset + index], destination[index]);
        }
        AnomalyNteSkillPageResultV1 page{sizeof(page)};
        page.flags = SnapshotFlags(
            state.skills_partial, state.skill_sample_sequence,
            state.tick_sequence.load(std::memory_order_acquire));
        page.generation = state.skill_generation;
        page.sequence = state.skill_sample_sequence;
        page.total_skills = total;
        page.returned = returned;
        page.next_offset = offset + returned;
        *result = page;
        return Status(ANOMALY_STATUS_V1_OK);
    }

    static AnomalyStatusV1 ANOMALY_CALL AbilityPath(
        void* user,
        const AnomalyGenerationHandleV1 ability_class,
        char* destination,
        std::size_t* size) noexcept {
        if (size == nullptr || ability_class.id == 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.skill_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.skills") || !state.skills_available) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "skill frame is unavailable");
        }
        const auto found = std::ranges::find_if(state.skills, [&](const SkillRecord& skill) {
            return SameHandle(skill.ability_class, ability_class);
        });
        return found == state.skills.end()
            ? Status(ANOMALY_STATUS_V1_NOT_FOUND, "ability class handle is stale")
            : CopyString(found->ability_path, destination, size);
    }

    static AnomalyStatusV1 ANOMALY_CALL AbilityDisplayName(
        void* user, const AnomalyGenerationHandleV1 ability_class,
        char* destination, std::size_t* size) noexcept {
        if (size == nullptr || ability_class.id == 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.skill_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.skills") || !state.skills_available) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "skill frame is unavailable");
        }
        const auto found = std::ranges::find_if(state.skills, [&](const SkillRecord& skill) {
            return SameHandle(skill.ability_class, ability_class);
        });
        if (found == state.skills.end()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "ability class handle is stale");
        }
        const auto cached = state.ability_display_names.find(ability_class.id);
        if (cached != state.ability_display_names.end() && !cached->second.empty()) {
            return CopyString(cached->second, destination, size);
        }
        return Status(ANOMALY_STATUS_V1_NOT_FOUND, "localized ability name is unavailable");
    }

    static AnomalyStatusV1 ANOMALY_CALL SkillSnapshotByHandle(
        void* user,
        const AnomalyGenerationHandleV1 skill,
        AnomalyNteSkillSnapshotV1* snapshot) noexcept {
        if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot) ||
            skill.id == 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.skill_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.skills") || !state.skills_available) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "skill frame is unavailable");
        }
        if (skill.generation != state.skill_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale skill handle generation");
        }
        const auto found = std::ranges::find_if(state.skills, [&](const SkillRecord& record) {
            return SameHandle(record.handle, skill);
        });
        if (found == state.skills.end()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "skill handle is not found");
        }
        FillSkillSnapshot(state, *found, *snapshot);
        return Status(ANOMALY_STATUS_V1_OK);
    }

    [[nodiscard]] bool LiveSkillIdentityLocked(
        const SkillRecord& expected,
        const std::uintptr_t ability_system) const noexcept {
        NativeArrayHeader array;
        if (!ReadSkillArrayLocked(ability_system, array)) return false;
        for (std::int32_t index{}; index < array.count; ++index) {
            SkillRecord observed;
            if (!ReadSkillIdentityLocked(array, index, observed)) return false;
            if (observed.spec_handle == expected.spec_handle) {
                return SameSkillIdentity(observed, expected);
            }
        }
        return false;
    }

    static AnomalyStatusV1 ANOMALY_CALL ActivateSkill(
        void* user,
        const AnomalyNteSkillInvocationRequestV1* request,
        AnomalyNteSkillInvocationResultV1* result) noexcept {
        if (request == nullptr || request->struct_size < sizeof(*request) ||
            result == nullptr || result->struct_size < sizeof(*result) ||
            request->flags != 0) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
        }
        auto& state = *static_cast<State*>(user);
        state.skill_demand.store(true, std::memory_order_release);
        const DWORD expected_thread = state.game_thread_id.load(std::memory_order_acquire);
        if (expected_thread == 0 || expected_thread != GetCurrentThreadId()) {
            return Status(
                ANOMALY_STATUS_V1_CONFLICT,
                "skill activation requires the Game thread");
        }
        std::unique_lock lock(state.mutex);
        if (!state.SemanticFeatureRunning("nte.skill-invocation") ||
            !state.skills_available) {
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "skill invocation is unavailable for the active Profile");
        }
        if (state.world_pointer == 0 || request->world.id != 1 ||
            request->world.generation != state.world_generation) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale skill world handle");
        }
        if (!SameHandle(request->character, state.skill_character)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale skill character handle");
        }
        if (request->skill.generation != state.skill_generation || request->skill.id == 0) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "stale skill handle generation");
        }
        const auto found = std::ranges::find_if(state.skills, [&](const SkillRecord& record) {
            return SameHandle(record.handle, request->skill);
        });
        if (found == state.skills.end()) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "skill handle is not found");
        }
        std::uintptr_t ability_system{};
        std::uintptr_t ability_class{};
        if (!state.CurrentAbilitySystemLocked(state.player_pawn, ability_system) ||
            ability_system != state.skill_ability_system ||
            !state.ResolveObjectHandleLocked(found->ability_class, ability_class) ||
            ability_class != found->ability_class_pointer ||
            !state.IsClassDerivedFromLocked(
                ability_class,
                state.combat_skill_discovery.gameplay_ability_class) ||
            !state.LiveSkillIdentityLocked(*found, ability_system)) {
            return Status(ANOMALY_STATUS_V1_NOT_FOUND, "live skill identity changed");
        }
        const auto& binding = state.combat_skill_discovery.functions[
            NteIndex(NteFunctionKind::ActivateAbilityByClass)];
        if (!binding || binding->parms_size > 64U) {
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "skill activation binding is unavailable");
        }
        const NteFunctionBinding activation = *binding;
        const ProcessEventInvoker invoker = state.process_event_invoker;
        alignas(std::uint64_t) std::array<std::uint8_t, 64> parameters{};
        const std::size_t class_offset = activation.offsets[0];
        if (class_offset > activation.parms_size ||
            sizeof(ability_class) > activation.parms_size - class_offset) {
            return Status(ANOMALY_STATUS_V1_FAILED, "skill activation parameters are invalid");
        }
        std::memcpy(parameters.data() + class_offset, &ability_class, sizeof(ability_class));

        // Ability activation can synchronously enter the Actor ProcessEvent hook.
        // Release the state lock so that reentrant event observation can proceed.
        lock.unlock();
        if (!invoker ||
            !invoker(
                ability_system,
                activation.function,
                parameters.data(),
                activation.parms_size)) {
            return Status(ANOMALY_STATUS_V1_FAILED, "skill activation invocation failed");
        }
        const ReflectedBoolParameter& returned = activation.bool_parameters[1];
        if (returned.byte_offset >= activation.parms_size) {
            return Status(ANOMALY_STATUS_V1_FAILED, "skill activation result is invalid");
        }
        AnomalyNteSkillInvocationResultV1 response{sizeof(response)};
        response.tick_sequence = state.tick_sequence.load(std::memory_order_acquire);
        response.accepted =
            (parameters[returned.byte_offset] & returned.field_mask) != 0 ? 1U : 0U;
        *result = response;
        return Status(ANOMALY_STATUS_V1_OK);
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
            this, ResolveNameThunk, ResolveFTextThunk};
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
        map_landmarks_service = {
            sizeof(AnomalyNteMapLandmarksServiceV1),
            ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION,
            this, MapLandmarkSequenceThunk, MapLandmarkCountThunk,
            MapLandmarkSnapshotAtThunk, MapLandmarkTeleportThunk};
        navigation_service = {
            sizeof(AnomalyNteNavigationServiceV1),
            ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION,
            this, MoveToLocationThunk, StopMovementThunk};
        pickup_service = {
            sizeof(AnomalyNtePickupServiceV1),
            ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION,
            this, PickupRequestNearbyThunk, PickupSnapshotThunk};
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
        combat_service = {
            sizeof(AnomalyNteCombatServiceV1), ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION,
            this, CombatantSnapshotThunk, LatestDamageSequenceThunk,
            NextDamageEventThunk, CombatStatisticsThunk, DamageSourceNameThunk,
            DamageParticipantPathThunk, LatestCombatEventSequenceThunk,
            NextCombatEventThunk, CombatEventNameThunk, ParticipantDisplayNameThunk};
        skills_service = {
            sizeof(AnomalyNteSkillsServiceV1), ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION,
            this, SkillFrameThunk, SkillSnapshotAtThunk, SkillPageThunk,
            AbilityPathThunk, SkillSnapshotByHandleThunk, AbilityDisplayNameThunk};
        skill_invocation_service = {
            sizeof(AnomalyNteSkillInvocationServiceV1),
            ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION,
            this, ActivateSkillThunk};
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
    AnomalyNteMapLandmarksServiceV1 map_landmarks_service{};
    AnomalyNteNavigationServiceV1 navigation_service{};
    AnomalyNtePickupServiceV1 pickup_service{};
    AnomalyNteEntitiesServiceV1 entities_service{};
    AnomalyNteActorsServiceV1 actors_service{};
    AnomalyNteCombatServiceV1 combat_service{};
    AnomalyNteSkillsServiceV1 skills_service{};
    AnomalyNteSkillInvocationServiceV1 skill_invocation_service{};
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

    static AnomalyStatusV1 ANOMALY_CALL ResolveFTextThunk(
        void* user, std::uintptr_t address, char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ResolveFText(lease.User(), address, destination, size) : StoppedStatus();
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

    static std::uint64_t ANOMALY_CALL MapLandmarkSequenceThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::MapLandmarkSequence(lease.User()) : 0;
    }

    static std::uint32_t ANOMALY_CALL MapLandmarkCountThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::MapLandmarkCount(lease.User()) : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL MapLandmarkSnapshotAtThunk(
        void* user,
        std::uint32_t index,
        AnomalyNteMapLandmarkSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::MapLandmarkSnapshotAt(lease.User(), index, snapshot) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL MapLandmarkTeleportThunk(
        void* user,
        const AnomalyNteMapLandmarkTeleportRequestV1* request) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::MapLandmarkTeleport(lease.User(), request) : StoppedStatus();
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

    static AnomalyStatusV1 ANOMALY_CALL PickupRequestNearbyThunk(
        void* user, const AnomalyNtePickupRequestV1* request) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::PickupRequestNearby(lease.User(), request) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL PickupSnapshotThunk(
        void* user, AnomalyNtePickupSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::PickupSnapshot(lease.User(), snapshot) : StoppedStatus();
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

    static AnomalyStatusV1 ANOMALY_CALL CombatantSnapshotThunk(
        void* user, AnomalyNteCombatantSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::CombatantSnapshot(lease.User(), snapshot) : StoppedStatus();
    }

    static std::uint64_t ANOMALY_CALL LatestDamageSequenceThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::LatestDamageSequence(lease.User()) : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL NextDamageEventThunk(
        void* user, std::uint64_t after_sequence,
        AnomalyNteDamageEventV1* event) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::NextDamageEvent(lease.User(), after_sequence, event)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL CombatStatisticsThunk(
        void* user,
        const AnomalyNteCombatStatisticsRequestV1* request,
        AnomalyNteCombatStatisticsV1* statistics) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::CombatStatistics(lease.User(), request, statistics)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL DamageSourceNameThunk(
        void* user, std::uint64_t source_id,
        char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::DamageSourceName(lease.User(), source_id, destination, size)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL DamageParticipantPathThunk(
        void* user, AnomalyGenerationHandleV1 participant,
        char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::DamageParticipantPath(
                  lease.User(), participant, destination, size)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL SkillFrameThunk(
        void* user, AnomalyNteSkillFrameV1* frame) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::SkillFrame(lease.User(), frame) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL SkillSnapshotAtThunk(
        void* user, std::uint64_t generation, std::uint32_t index,
        AnomalyNteSkillSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::SkillSnapshotAt(lease.User(), generation, index, snapshot)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL SkillPageThunk(
        void* user, const AnomalyNteSkillPageRequestV1* request,
        AnomalyNteSkillSnapshotV1* destination,
        AnomalyNteSkillPageResultV1* result) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::SkillPage(lease.User(), request, destination, result)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL AbilityPathThunk(
        void* user, AnomalyGenerationHandleV1 ability_class,
        char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::AbilityPath(lease.User(), ability_class, destination, size)
            : StoppedStatus();
    }

    static std::uint64_t ANOMALY_CALL LatestCombatEventSequenceThunk(void* user) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::LatestCombatEventSequence(lease.User()) : 0;
    }

    static AnomalyStatusV1 ANOMALY_CALL NextCombatEventThunk(
        void* user, std::uint64_t after_sequence,
        AnomalyNteCombatEventV1* event) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::NextCombatEvent(lease.User(), after_sequence, event) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL CombatEventNameThunk(
        void* user, const AnomalyNteCombatEventV1* event,
        char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::CombatEventName(lease.User(), event, destination, size) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ParticipantDisplayNameThunk(
        void* user, AnomalyGenerationHandleV1 participant,
        char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease ? State::ParticipantDisplayName(lease.User(), participant, destination, size) : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL AbilityDisplayNameThunk(
        void* user, AnomalyGenerationHandleV1 ability_class,
        char* destination, std::size_t* size) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::AbilityDisplayName(lease.User(), ability_class, destination, size)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL SkillSnapshotByHandleThunk(
        void* user, AnomalyGenerationHandleV1 skill,
        AnomalyNteSkillSnapshotV1* snapshot) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::SkillSnapshotByHandle(lease.User(), skill, snapshot)
            : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL ActivateSkillThunk(
        void* user,
        const AnomalyNteSkillInvocationRequestV1* request,
        AnomalyNteSkillInvocationResultV1* result) noexcept {
        auto lease = static_cast<SemanticServiceEndpoint*>(user)->Acquire();
        return lease
            ? State::ActivateSkill(lease.User(), request, result)
            : StoppedStatus();
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

struct Ue5NteAdapter::State::ProcessEventServiceEndpoint final {
    struct Subscription final {
        Subscription(
            const std::uint64_t id_value, const std::uint64_t generation_value,
            const AnomalyUe5ProcessEventCallbackV1 callback_value,
            void* const callback_user_value) noexcept
            : id(id_value), generation(generation_value), callback(callback_value),
              callback_user(callback_user_value) {}
        const std::uint64_t id{};
        const std::uint64_t generation{};
        const AnomalyUe5ProcessEventCallbackV1 callback{};
        void* const callback_user{};
        AdmissionGate gate;
    };

    using SubscriptionList = std::vector<std::shared_ptr<Subscription>>;

    class ServiceLease final {
    public:
        ServiceLease() = default;
        ServiceLease(ProcessEventServiceEndpoint* endpoint,
                     std::shared_ptr<State> state) noexcept
            : endpoint_(endpoint), state_(std::move(state)) {}
        ServiceLease(const ServiceLease&) = delete;
        ServiceLease(ServiceLease&& other) noexcept
            : endpoint_(std::exchange(other.endpoint_, nullptr)),
              state_(std::move(other.state_)) {}
        ~ServiceLease() { Release(); }
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
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
        ProcessEventServiceEndpoint* endpoint_{};
        std::shared_ptr<State> state_;
    };

    ProcessEventServiceEndpoint(std::weak_ptr<State> state, const std::uint64_t generation) noexcept
        : state_(std::move(state)), generation_(generation == 0 ? 1 : generation) {
        service = {sizeof(AnomalyUe5ProcessEventServiceV1),
                   ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION, this,
                   SubscribeThunk, UnsubscribeThunk};
        subscriptions_snapshot_.store(
            std::make_shared<const SubscriptionList>(), std::memory_order_release);
    }

    ServiceLease Acquire() noexcept {
        if (!gate_.TryEnter()) return {};
        auto state = state_.lock();
        if (!state) {
            gate_.Leave();
            return {};
        }
        return ServiceLease(this, std::move(state));
    }

    void Close() noexcept {
        gate_.Close();
        callback_gate_.Close();
        std::scoped_lock lock(mutex_);
        closed_.store(true, std::memory_order_release);
        for (const auto& [id, subscription] : subscriptions_) {
            static_cast<void>(id);
            subscription->gate.Close();
        }
    }

    bool DrainUntil(const std::chrono::steady_clock::time_point deadline) noexcept {
        if (!gate_.DrainUntil(deadline) || !callback_gate_.DrainUntil(deadline)) return false;
        const auto subscriptions = subscriptions_snapshot_.load(std::memory_order_acquire);
        return subscriptions != nullptr && std::ranges::all_of(*subscriptions, [deadline](const auto& subscription) {
            return subscription->gate.DrainUntil(deadline);
        });
    }

    void Dispatch(const std::uintptr_t object, const std::uintptr_t function,
                  void* const parameters) noexcept {
        if (object == 0 || function == 0) return;
        try {
            const auto subscriptions = subscriptions_snapshot_.load(std::memory_order_acquire);
            if (closed_.load(std::memory_order_acquire) || subscriptions == nullptr) return;
            for (const auto& subscription : *subscriptions) {
                if (!callback_gate_.TryEnter()) return;
                const auto leave = std::unique_ptr<AdmissionGate, void(*)(AdmissionGate*)>(
                    &callback_gate_, [](AdmissionGate* gate) { gate->Leave(); });
                if (!subscription->gate.TryEnter()) continue;
                try {
                    subscription->callback(subscription->callback_user, object, function,
                                           parameters);
                } catch (...) {
                }
                subscription->gate.Leave();
            }
        } catch (...) {
        }
    }

    AnomalyUe5ProcessEventServiceV1 service{};

private:
    static AnomalyStatusV1 StoppedStatus() noexcept {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "ProcessEvent service is stopped");
    }

    AnomalyStatusV1 Subscribe(const std::shared_ptr<State>& state,
                              const AnomalyUe5ProcessEventCallbackV1 callback,
                              void* const callback_user,
                              AnomalyGenerationHandleV1* const handle) noexcept {
        if (handle == nullptr || callback == nullptr) {
            return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                          "ProcessEvent subscription callback and handle are required");
        }
        *handle = {};
        if (!state->started.load(std::memory_order_acquire) ||
            !state->framework_hook_ready || !state->process_event_hook_ready) {
            return StoppedStatus();
        }
        try {
            std::scoped_lock lock(mutex_);
            if (closed_.load(std::memory_order_acquire) ||
                next_id_ == (std::numeric_limits<std::uint64_t>::max)()) {
                return StoppedStatus();
            }
            const auto id = ++next_id_;
            auto subscription = std::make_shared<Subscription>(
                id, generation_, callback, callback_user);
            subscriptions_.emplace(id, subscription);
            auto next = std::make_shared<SubscriptionList>();
            const auto current = subscriptions_snapshot_.load(std::memory_order_acquire);
            if (current != nullptr) *next = *current;
            next->push_back(std::move(subscription));
            subscriptions_snapshot_.store(std::move(next), std::memory_order_release);
            *handle = {id, generation_};
            return Status(ANOMALY_STATUS_V1_OK);
        } catch (...) {
            return Status(ANOMALY_STATUS_V1_FAILED, "ProcessEvent subscription allocation failed");
        }
    }

    AnomalyStatusV1 Unsubscribe(const AnomalyGenerationHandleV1 handle) noexcept {
        std::shared_ptr<Subscription> subscription;
        {
            std::scoped_lock lock(mutex_);
            const auto found = subscriptions_.find(handle.id);
            if (handle.id == 0 || handle.generation != generation_ ||
                found == subscriptions_.end()) {
                return Status(ANOMALY_STATUS_V1_NOT_FOUND,
                              "ProcessEvent subscription handle is not found");
            }
            subscription = std::move(found->second);
            subscriptions_.erase(found);
            auto next = std::make_shared<SubscriptionList>();
            next->reserve(subscriptions_.size());
            for (const auto& [id, candidate] : subscriptions_) {
                static_cast<void>(id);
                next->push_back(candidate);
            }
            subscriptions_snapshot_.store(std::move(next), std::memory_order_release);
        }
        subscription->gate.Close();
        return subscription->gate.DrainUntil(std::chrono::steady_clock::time_point::max())
            ? Status(ANOMALY_STATUS_V1_OK)
            : Status(ANOMALY_STATUS_V1_TIMEOUT, "ProcessEvent subscription drain timed out");
    }

    static AnomalyStatusV1 ANOMALY_CALL SubscribeThunk(
        void* user, const AnomalyUe5ProcessEventCallbackV1 callback,
        void* callback_user, AnomalyGenerationHandleV1* handle) noexcept {
        auto* endpoint = static_cast<ProcessEventServiceEndpoint*>(user);
        auto lease = endpoint->Acquire();
        return lease ? endpoint->Subscribe(lease.StateOwner(), callback, callback_user, handle)
                     : StoppedStatus();
    }

    static AnomalyStatusV1 ANOMALY_CALL UnsubscribeThunk(
        void* user, const AnomalyGenerationHandleV1 handle) noexcept {
        auto* endpoint = static_cast<ProcessEventServiceEndpoint*>(user);
        auto lease = endpoint->Acquire();
        return lease ? endpoint->Unsubscribe(handle) : StoppedStatus();
    }

    std::weak_ptr<State> state_;
    const std::uint64_t generation_{};
    AdmissionGate gate_;
    AdmissionGate callback_gate_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Subscription>> subscriptions_;
    std::atomic<std::shared_ptr<const SubscriptionList>> subscriptions_snapshot_;
    std::uint64_t next_id_{};
    std::atomic_bool closed_{};
};

void Ue5NteAdapter::State::DispatchAhudFrame(
    const std::uintptr_t object,
    const std::uintptr_t function,
    void* const parameters,
    const ProcessEventInvoker& process_event) noexcept {
    if (object == 0 || function == 0 || parameters == nullptr ||
        !process_event ||
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
        &process_event,
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
    const auto current_process_event_endpoint =
        process_event_endpoint.load(std::memory_order_acquire);
    // ProcessEvent is the shared ingress used by plugins and does not depend
    // on the optional AHUD reflection binding.  Publishing it only after the
    // AHUD gate leaves subscribers stuck in waiting-for-service even though
    // the core ProcessEvent hook is already active.
    if (framework_hook_ready && process_event_hook_ready &&
        resolution.FeatureAvailable("ue5.process-event") &&
        current_process_event_endpoint &&
        !PublishIfMissing(
            ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID,
            ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION,
            &current_process_event_endpoint->service,
            {}, std::static_pointer_cast<const void>(current_process_event_endpoint))) {
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
    if (framework_hook_ready && SemanticFeatureAvailable("nte.map-landmarks") &&
        !PublishIfMissing(
            ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID,
            ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_VERSION,
            &endpoint->map_landmarks_service,
            {}, semantic_lifetime)) {
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
    if (framework_hook_ready && SemanticFeatureAvailable("nte.pickup") &&
        !PublishIfMissing(
            ANOMALY_NTE_PICKUP_SERVICE_V1_ID,
            ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION,
            &endpoint->pickup_service,
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
    const bool publish_combat_service = framework_hook_ready &&
        NteCombatProfileAvailable() &&
        !IsPublished(ANOMALY_NTE_COMBAT_SERVICE_V1_ID);
    if (publish_combat_service &&
        !Publish(
            ANOMALY_NTE_COMBAT_SERVICE_V1_ID,
            ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION,
            &endpoint->combat_service,
            [self, observer_endpoint] {
                const auto locked = self.lock();
                const auto observed = observer_endpoint.lock();
                if (!locked || !observed ||
                    locked->semantic_endpoint.load(std::memory_order_acquire) != observed) {
                    return;
                }
                locked->player_demand.store(true, std::memory_order_release);
                locked->combat_demand.store(true, std::memory_order_release);
            },
            semantic_lifetime)) {
        return false;
    }
    const bool publish_skills_service = framework_hook_ready &&
        NteSkillsProfileAvailable() &&
        !IsPublished(ANOMALY_NTE_SKILLS_SERVICE_V1_ID);
    if (publish_skills_service &&
        !Publish(
            ANOMALY_NTE_SKILLS_SERVICE_V1_ID,
            ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION,
            &endpoint->skills_service,
            [self, observer_endpoint] {
                const auto locked = self.lock();
                const auto observed = observer_endpoint.lock();
                if (!locked || !observed ||
                    locked->semantic_endpoint.load(std::memory_order_acquire) != observed) {
                    return;
                }
                locked->player_demand.store(true, std::memory_order_release);
                locked->skill_demand.store(true, std::memory_order_release);
            },
            semantic_lifetime)) {
        return false;
    }
    if (framework_hook_ready && SemanticFeatureAvailable("nte.skill-invocation") &&
        !PublishIfMissing(
            ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
            ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION,
            &endpoint->skill_invocation_service,
            [self, observer_endpoint] {
                const auto locked = self.lock();
                const auto observed = observer_endpoint.lock();
                if (!locked || !observed ||
                    locked->semantic_endpoint.load(std::memory_order_acquire) != observed) {
                    return;
                }
                locked->player_demand.store(true, std::memory_order_release);
                locked->skill_demand.store(true, std::memory_order_release);
            },
            semantic_lifetime)) {
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
    state_->sampling.combat_tick_interval = (std::max)(1U, sampling.combat_tick_interval);
    state_->sampling.skill_tick_interval = (std::max)(1U, sampling.skill_tick_interval);
    state_->sampling.actor_tick_interval = (std::max)(1U, sampling.actor_tick_interval);
}

Ue5NteAdapter::~Ue5NteAdapter() {
    static_cast<void>(Stop(std::chrono::milliseconds::zero()));
}

bool Ue5NteAdapter::Start(
    bool framework_hook_ready, bool ahud_hook_ready,
    bool process_event_hook_ready) {
    const auto state = state_;
    std::shared_ptr<State::SemanticServiceEndpoint> retired_semantic_endpoint;
    std::shared_ptr<State::CallbackEndpoint> retired_callback_endpoint;
    std::shared_ptr<State::AhudServiceEndpoint> retired_ahud_endpoint;
    std::shared_ptr<State::ProcessEventServiceEndpoint> retired_process_event_endpoint;
    std::unique_lock<std::timed_mutex> lifecycle_lock(state->lifecycle_mutex);
    if (state->stopping.load(std::memory_order_acquire) ||
        state->semantic_endpoint.load(std::memory_order_acquire) ||
        state->callback_endpoint.load(std::memory_order_acquire) ||
        state->ahud_endpoint.load(std::memory_order_acquire) ||
        state->process_event_endpoint.load(std::memory_order_acquire) ||
        (state->draining_semantic_endpoint &&
            !state->draining_semantic_endpoint->IsDrained()) ||
        (state->draining_callback_endpoint &&
            !state->draining_callback_endpoint->IsDrained()) ||
        (state->draining_ahud_endpoint &&
            !state->draining_ahud_endpoint->IsDrained()) ||
        (state->draining_process_event_endpoint &&
            !state->draining_process_event_endpoint->DrainUntil(
                std::chrono::steady_clock::now()))) {
        return false;
    }
    std::unique_lock<std::timed_mutex> lock(state->mutex);
    if (state->started.load(std::memory_order_acquire)) {
        return false;
    }
    retired_semantic_endpoint = std::move(state->draining_semantic_endpoint);
    retired_callback_endpoint = std::move(state->draining_callback_endpoint);
    retired_ahud_endpoint = std::move(state->draining_ahud_endpoint);
    retired_process_event_endpoint = std::move(state->draining_process_event_endpoint);
    const auto lifecycle_generation =
        state->lifecycle_epoch.fetch_add(1, std::memory_order_acq_rel) + 1U;
    state->ResetForStartLocked();
    state->framework_hook_ready = framework_hook_ready;
    state->ahud_hook_ready = ahud_hook_ready;
    state->process_event_hook_ready = process_event_hook_ready;
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
    state->process_event_endpoint.store(
        std::make_shared<State::ProcessEventServiceEndpoint>(state, lifecycle_generation),
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
    const auto failed_process_event_endpoint = state->process_event_endpoint.exchange(
        std::shared_ptr<State::ProcessEventServiceEndpoint>{}, std::memory_order_acq_rel);
    if (failed_process_event_endpoint) failed_process_event_endpoint->Close();
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
    std::shared_ptr<State::ProcessEventServiceEndpoint> process_event_endpoint;
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
    process_event_endpoint = state->process_event_endpoint.exchange(
        std::shared_ptr<State::ProcessEventServiceEndpoint>{}, std::memory_order_acq_rel);
    if (process_event_endpoint) {
        process_event_endpoint->Close();
        if (!state->draining_process_event_endpoint) {
            state->draining_process_event_endpoint = process_event_endpoint;
        }
    } else {
        process_event_endpoint = state->draining_process_event_endpoint;
        if (process_event_endpoint) process_event_endpoint->Close();
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
    if (process_event_endpoint && !process_event_endpoint->DrainUntil(deadline)) return false;
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
    std::shared_ptr<State::ProcessEventServiceEndpoint> retired_process_event_endpoint;
    if (state->draining_semantic_endpoint == semantic_endpoint) {
        retired_semantic_endpoint = std::move(state->draining_semantic_endpoint);
    }
    if (state->draining_callback_endpoint == callback_endpoint) {
        retired_callback_endpoint = std::move(state->draining_callback_endpoint);
    }
    if (state->draining_ahud_endpoint == ahud_endpoint) {
        retired_ahud_endpoint = std::move(state->draining_ahud_endpoint);
    }
    if (state->draining_process_event_endpoint == process_event_endpoint) {
        retired_process_event_endpoint = std::move(state->draining_process_event_endpoint);
    }
    if (!state->semantic_endpoint.load(std::memory_order_acquire) &&
        !state->callback_endpoint.load(std::memory_order_acquire) &&
        !state->ahud_endpoint.load(std::memory_order_acquire) &&
        !state->process_event_endpoint.load(std::memory_order_acquire)) {
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
        state->RefreshMapLandmarksLocked(sequence);
        if (state->navigation_demand.load(std::memory_order_acquire)) {
            static_cast<void>(state->EnsureNavigationInputPolicyLocked());
            state->RefreshNavigationBindingLocked();
        }
        state->RefreshAhudBindingLocked();
        state->RefreshCombatSkillBindingsLocked();
        const bool combat_service_ready =
            state->NteCombatProfileAvailable() &&
            !state->IsPublished(ANOMALY_NTE_COMBAT_SERVICE_V1_ID);
        const bool skills_service_ready =
            state->NteSkillsProfileAvailable() &&
            !state->IsPublished(ANOMALY_NTE_SKILLS_SERVICE_V1_ID);
        const bool invocation_service_ready =
            state->SemanticFeatureAvailable("nte.skill-invocation") &&
            !state->IsPublished(ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID);
        if (combat_service_ready || skills_service_ready || invocation_service_ready) {
            static_cast<void>(state->PublishAvailableServices(state));
        }
        state->RefreshPickupConfirmationLocked();
        const bool pickup_requested = state->pickup_demand.exchange(
            false, std::memory_order_acq_rel);
        const bool entity_requested = state->entity_demand.load(std::memory_order_acquire);
        const bool player_requested = state->player_demand.load(std::memory_order_acquire);
        const bool combat_requested = state->combat_demand.load(std::memory_order_acquire);
        const bool skill_requested = state->skill_demand.load(std::memory_order_acquire);
        const bool entity_due = pickup_requested ||
            (entity_requested && State::SamplingDue(
                sequence, state->entity_attempt_sequence, state->sampling.entity_tick_interval));
        const bool player_due = pickup_requested || (player_requested && State::SamplingDue(
            sequence, state->player_attempt_sequence, state->sampling.player_tick_interval));
        const bool combat_due = combat_requested && State::SamplingDue(
            sequence, state->combat_attempt_sequence, state->sampling.combat_tick_interval);
        const bool skill_due = skill_requested && State::SamplingDue(
            sequence, state->skill_attempt_sequence, state->sampling.skill_tick_interval);
        if (entity_due || player_due || combat_due || skill_due) {
            state->RefreshPlayer(sequence);
            ++state->player_refresh_count;
        } else if (player_requested) {
            ++state->player_cache_hit_count;
        }
            state->DrainCombatCaptureQueueLocked();
            state->ResolveNextCombatEventNameLocked();
            state->ResolveNextCombatParticipantNameLocked();
            // One deferred name lookup per tick bounds game-thread FText work;
        // event capture itself has already completed in the hook queue.
        state->ResolveNextDamageSourceMappingLocked();
        state->ResolveNextPendingDamageSourceNameLocked(sequence);
        if (entity_due) {
            // One observed frame request is satisfied by one refresh. A later
            // frame() call requests another sample.
            static_cast<void>(state->entity_demand.exchange(false, std::memory_order_acq_rel));
            state->RefreshEntities(sequence);
            ++state->entity_refresh_count;
        } else if (entity_requested) {
            ++state->entity_cache_hit_count;
        }
        if (pickup_requested) state->PerformPickupLocked();
        if (combat_due) {
            static_cast<void>(state->combat_demand.exchange(false, std::memory_order_acq_rel));
            state->RefreshCombat(sequence);
        }
        if (skill_due) {
            static_cast<void>(state->skill_demand.exchange(false, std::memory_order_acq_rel));
            state->RefreshSkills(sequence);
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

void Ue5NteAdapter::OnDamageEvent(
    const std::uintptr_t damage_event,
    const std::uintptr_t victim,
    const std::uintptr_t attacker,
    const std::uintptr_t damage_causer) noexcept {
    const auto state = state_;
    const auto* const bindings = state->combat_capture_bindings.load(
        std::memory_order_acquire);
    if (bindings == nullptr) {
        state->damage_native_call_count.fetch_add(1, std::memory_order_relaxed);
        state->damage_capture_drop_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Capture the synchronous critical query result with owned damage/tag values.
    state->EnqueueCharacterDamage(
        *bindings, damage_event, victim, attacker, damage_causer);
}

void Ue5NteAdapter::OnCombatExecFunction(
    const std::uintptr_t receiver,
    const std::uintptr_t function,
    const std::uintptr_t stack) noexcept {
    const auto state = state_;
    if (function == 0 || stack == 0) {
        return;
    }
    const auto locals_offset = Layout(state->profile, "fstack.locals", -1);
    std::uintptr_t parameters{};
    if (locals_offset < 0 ||
        !ReadValue(*state->memory,
            stack + static_cast<std::uintptr_t>(locals_offset), parameters) ||
        parameters == 0) {
        return;
    }
    const auto* const bindings = state->combat_capture_bindings.load(
        std::memory_order_acquire);
    if (bindings == nullptr) return;
    const bool is_buff_function = std::ranges::any_of(
        bindings->buffs, [function](const auto& buff) {
            return buff.function != 0 && buff.function == function;
        });
    if (function != bindings->damage &&
        function != bindings->monster_damage &&
        function != bindings->player_damage_queue &&
        function != bindings->damage_widget &&
        !is_buff_function) {
        return;
    }
    state->EnqueueCombatProcessEvent(
        *bindings, function, reinterpret_cast<void*>(parameters));
    static_cast<void>(receiver);
}

void Ue5NteAdapter::OnProcessEvent(
    const std::uintptr_t object,
    const std::uintptr_t function,
    void* const parameters,
    const ProcessEventInvoker& process_event) noexcept {
    const auto state = state_;
    state->DispatchAhudFrame(object, function, parameters, process_event);
}

std::string JsonQuote(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(static_cast<char>(character)); break;
        }
    }
    result.push_back('"');
    return result;
}

void Ue5NteAdapter::OnProcessEventPre(
    const std::uintptr_t object, const std::uintptr_t function,
    void* const parameters) noexcept {
    const auto state = state_;
    // Keep the original ProcessEvent contract: only the game thread reaches
    // endpoint subscribers.  Besides preserving AHUD/plugin behavior, this
    // avoids running a subscriber snapshot on every worker-thread invocation.
    if (GetCurrentThreadId() != state->game_thread_id.load(std::memory_order_acquire)) {
        return;
    }
    const auto* const combat_bindings =
        state->combat_capture_bindings.load(std::memory_order_acquire);
    if (combat_bindings != nullptr) {
        state->EnqueueCombatProcessEvent(
            *combat_bindings, function, parameters);
    }
    const auto endpoint = state->process_event_endpoint.load(std::memory_order_acquire);
    if (endpoint) endpoint->Dispatch(object, function, parameters);
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

bool Ue5NteAdapter::CombatFeatureAvailable() const noexcept {
    const auto state = state_;
    std::scoped_lock lock(state->mutex);
    return state->SemanticFeatureAvailable("nte.combat");
}

NteCombatExecFunctionTargetsSnapshot
Ue5NteAdapter::CombatExecFunctionTargets() const noexcept {
    const auto state = state_;
    std::scoped_lock lock(state->mutex);
    NteCombatExecFunctionTargetsSnapshot snapshot;
    snapshot.discovery_complete =
        state->combat_skill_discovery.combat_event_bindings_attempted;
    return snapshot;
}

NteCombatDiagnosticsSnapshot Ue5NteAdapter::CombatDiagnostics() const noexcept {
    const auto state = state_;
    std::scoped_lock lock(state->mutex);
    std::uint32_t binding_mask{};
    if (const auto* bindings = state->combat_capture_bindings.load(
            std::memory_order_acquire)) {
        if (bindings->damage != 0) binding_mask |= 1U << 0U;
        if (bindings->monster_damage != 0) binding_mask |= 1U << 1U;
        if (bindings->player_damage_queue != 0) binding_mask |= 1U << 2U;
        if (bindings->damage_widget != 0) binding_mask |= 1U << 3U;
        if (std::ranges::any_of(bindings->buffs,
                [](const auto& buff) { return buff.function != 0; })) {
            binding_mask |= 1U << 5U;
        }
    }
    return {
        state->combat_skill_discovery.damage_event_layout_valid,
        state->combat_available,
        state->combat_partial,
        binding_mask,
        state->damage_floaties_call_count.load(std::memory_order_acquire),
        state->monster_damage_call_count.load(std::memory_order_acquire),
        state->player_damage_queue_call_count.load(std::memory_order_acquire),
        state->damage_widget_call_count.load(std::memory_order_acquire),
        state->buff_call_count.load(std::memory_order_acquire),
        state->crit_query_call_count.load(std::memory_order_acquire),
        state->crit_query_success_count.load(std::memory_order_acquire),
        state->crit_true_count.load(std::memory_order_acquire),
        state->damage_native_call_count.load(std::memory_order_acquire),
        state->damage_captured_event_count,
        state->damage_capture_drop_count.load(std::memory_order_acquire),
        state->damage_attacker_resolution_failure_count,
        state->damage_victim_resolution_failure_count,
        state->damage_source_resolution_failure_count,
        state->saved_trigger_skill_mapping_count,
        state->trigger_ability_handle_mapping_count,
        state->damage_source_mapping_failure_count,
        state->delayed_damage_name_completion_count,
        state->combat_sample_sequence,
        state->world_pointer,
        state->player_pawn,
        state->combat_character.id,
        state->combat_character.generation,
        state->combat_refresh_failure,
        state->reflection_fault_count,
        state->last_reflection_fault_function,
        state->last_reflection_fault_code};
}

std::string Ue5NteAdapter::CombatEventsJson(const bool buffs_only) const {
        const auto state = state_;
        state->player_demand.store(true, std::memory_order_release);
        state->combat_demand.store(true, std::memory_order_release);
        std::scoped_lock lock(state->mutex);
        std::string result{"{\"ok\":true,\"latest\":"};
        result += std::to_string(state->combat_event_sequence);
        result += ",\"localizedNameCount\":" +
            std::to_string(state->localized_names_by_fname.size()) +
            ",\"participantNameCount\":" +
            std::to_string(state->combat_participant_names.size()) +
            ",\"participantPendingCount\":" +
            std::to_string(state->pending_combat_participant_names.size()) +
            ",\"participantFailedCount\":" +
            std::to_string(state->failed_combat_participant_names.size()) +
            ",\"monsterStaticDataFunctionReady\":" +
            (state->NteFunctionReady(State::NteFunctionKind::GetMonsterStaticData)
                 ? "true"
                 : "false") +
            ",\"monsterStaticDataResolutionCalls\":" +
            std::to_string(state->monster_static_data_resolution_calls) +
            ",\"monsterStaticDataResolutionSuccesses\":" +
            std::to_string(state->monster_static_data_resolution_successes) +
            ",\"stringTableBindingAttempts\":" +
            std::to_string(state->string_table_binding_attempts) +
            ",\"stringTableBindingFailures\":" +
            std::to_string(state->string_table_binding_failures) +
            ",\"stringTableBindingFailureCode\":" +
            std::to_string(state->string_table_binding_failure_code) +
            ",\"stringTableCallCount\":" +
            std::to_string(state->string_table_call_count) +
            ",\"stringTableSuccessCount\":" +
            std::to_string(state->string_table_success_count) +
            ",\"stringTableThreadRejections\":" +
            std::to_string(state->string_table_thread_rejections) +
            ",\"stringTableLastKey\":" +
            JsonQuote(state->string_table_last_key) +
            ",\"stringTableLastValue\":" +
            JsonQuote(state->string_table_last_value) +
            ",\"participantLastHandle\":" +
            std::to_string(state->participant_last_handle) +
            ",\"participantLastPlayer\":" +
            (state->participant_last_player ? "true" : "false") +
            ",\"participantLastClassText\":" +
            JsonQuote(state->participant_last_class_text) +
            ",\"participantLastConfigText\":" +
            JsonQuote(state->participant_last_config_text) +
            ",\"sceneMonsterTableCount\":" +
            std::to_string(state->scene_monster_table_count) +
            ",\"sceneMonsterTableScanComplete\":" +
            (state->scene_monster_table_scan_complete ? "true" : "false") +
            ",\"displayTableMask\":" +
            std::to_string(state->display_table_loaded_mask) +
            ",\"displayTablesComplete\":" +
            (state->display_table_scan_complete ? "true" : "false");
        result += ",\"events\":[";
    bool first = true;
    for (std::size_t index{}; index < state->combat_event_count; ++index) {
        const auto& event = state->combat_events[
            (state->combat_event_start + index) % State::kCombatEventCapacity].event;
        const bool is_buff = event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD ||
            event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE;
        if (buffs_only && !is_buff) continue;
        if (!first) result.push_back(',');
        first = false;
        result += "{\"sequence\":" + std::to_string(event.sequence) +
            ",\"kind\":" + std::to_string(event.kind) +
            ",\"flags\":" + std::to_string(event.flags) +
            ",\"tick\":" + std::to_string(event.tick_sequence) +
            ",\"sourceId\":" + std::to_string(event.source.id) +
            ",\"sourceGeneration\":" + std::to_string(event.source.generation) +
            ",\"targetId\":" + std::to_string(event.target.id) +
            ",\"targetGeneration\":" + std::to_string(event.target.generation) +
            ",\"nameId\":" + std::to_string(event.name_id) +
            ",\"value\":" + std::to_string(event.value) +
            ",\"basic\":" + std::to_string(event.basic_value) +
            ",\"final\":" + std::to_string(event.final_value) +
            ",\"duration\":" + std::to_string(event.duration_seconds) +
            ",\"stack\":" + std::to_string(event.stack_count);
        auto name = state->combat_event_names.find(event.name_id);
        if (name != state->combat_event_names.end() && !name->second.empty()) {
            result += ",\"name\":" + JsonQuote(name->second);
            result += ",\"nameResolved\":true";
        } else {
            result += ",\"nameResolved\":false";
        }
        const auto participant_name = [&](const AnomalyGenerationHandleV1 handle) {
            if (handle.id == 0 || handle.generation != state->object_generation) return std::string{};
            const auto found = state->combat_participant_names.find(handle.id);
            return found != state->combat_participant_names.end()
                ? found->second : std::string{};
        };
        const std::string source_name = participant_name(event.source);
        if (!source_name.empty()) result += ",\"sourceName\":" + JsonQuote(source_name);
        const std::string target_name = participant_name(event.target);
        if (!target_name.empty()) result += ",\"targetName\":" + JsonQuote(target_name);
        if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE &&
            (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL) != 0) {
            result += ",\"critical\":true";
        }
        result += '}';
    }
    result += "]}";
    return result;
}

ProfileResolutionSnapshot Ue5NteAdapter::Resolution() const {
    const auto state = state_;
    std::scoped_lock lock(state->mutex);
    return state->resolution;
}

}  // namespace anomaly
