#include "anomaly/sdk/cpp.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kStoragePath = "bidking_round_pool.json";
constexpr std::string_view kStatusStoragePath = "bidking_round_pool_status.json";
constexpr std::string_view kDispatchPattern =
    "48 8B C4 55 53 56 48 8D 68 A8 48 81 EC 40 01 00 00 "
    "48 8B DA 48 8B F1 48 63 52 18 83 FA 14 0F 87 ?? ?? ?? ??";
constexpr std::string_view kActivityLookupPattern =
    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 75 06 "
    "48 83 C4 20 5B C3 48 8D 93 44 40 00 00 48 8B C8 48 83 C4 20 5B "
    "E9 ?? ?? ?? ??";
constexpr std::string_view kTurnWidgetStartLotteryPattern =
    "40 53 48 83 EC 50 44 8B 81 A4 06 00 00 48 8B D9 45 85 C0 0F 84 "
    "?? ?? ?? ?? 80 B9 90 06 00 00 01 0F 84 ?? ?? ?? ?? F3 0F 10 89 "
    "30 06 00 00 0F 57 C0 F3 0F 10 91 34 06 00 00 41 FF C8 F3 0F 5D "
    "15 ?? ?? ?? ??";
constexpr std::uint32_t kStartGameResponse = 0;
constexpr std::uint32_t kStartRoundResponse = 1;
constexpr std::uint32_t kTurnWidgetItemListCapture = (std::numeric_limits<std::uint32_t>::max)();
constexpr std::size_t kBidKingPoolOffset = 0xDB0;
constexpr std::size_t kBidKingRecordSize = 0x40;
constexpr std::size_t kBidKingPlayerRoundCacheOffset = 0x30;
constexpr std::size_t kBidKingRoundCacheRecordSize = 0x18;
constexpr std::size_t kTurnWidgetItemListOffset = 0x680;
constexpr std::size_t kRecordsPerCapture = 32;
constexpr std::size_t kMaximumPoolRecords = 4096;
constexpr std::size_t kMaximumPlausiblePoolRecords = 65536;
constexpr std::size_t kMaximumRoundCacheRecordsPerPlayer = 256;
constexpr std::size_t kMaximumRoundCacheRecordsPerSnapshot = 1024;
constexpr std::size_t kMaximumPlausibleRoundCacheRecords = 65536;
constexpr std::size_t kMaximumCapturedTurnWidgetItems = 256;
constexpr std::size_t kMaximumPlausibleTurnWidgetItems = 4096;
constexpr std::size_t kMaximumCapturedEnvelopePayloadBytes = 4096;
constexpr std::size_t kMaximumPlausibleEnvelopePayloadBytes = 1024 * 1024;
constexpr std::uint32_t kMaximumBidKingResponseType = 20;
constexpr std::size_t kMaximumHistoryRecords = 256;
constexpr std::size_t kRingCapacity = 256;
constexpr std::uint32_t kFlushDelayMilliseconds = 100;

enum CaptureFlags : std::uint32_t {
    CaptureEnvelopeRead = 1U << 0U,
    CaptureActivityFound = 1U << 1U,
    CapturePoolHeaderRead = 1U << 2U,
    CapturePoolHeaderPlausible = 1U << 3U,
    CaptureRecordReadFailure = 1U << 4U,
    CapturePoolTruncated = 1U << 5U,
    CaptureActivityLookupAttempted = 1U << 6U,
    CaptureRoundCacheHeaderCopied = 1U << 7U,
    CaptureRoundCacheHeaderPlausible = 1U << 8U,
    CaptureRoundCacheRecordReadFailure = 1U << 9U,
    CaptureRoundCacheTruncated = 1U << 10U,
    CaptureRoundCacheSnapshotTruncated = 1U << 11U,
    CaptureEnvelopePayloadHeaderPlausible = 1U << 12U,
    CaptureEnvelopePayloadCopied = 1U << 13U,
    CaptureEnvelopePayloadTruncated = 1U << 14U,
    CaptureEnvelopePayloadReadFailure = 1U << 15U,
    CaptureTurnWidgetItemListHeaderRead = 1U << 16U,
    CaptureTurnWidgetItemListHeaderPlausible = 1U << 17U,
    CaptureTurnWidgetItemListCopied = 1U << 18U,
    CaptureTurnWidgetItemListTruncated = 1U << 19U,
    CaptureTurnWidgetItemListReadFailure = 1U << 20U,
};

enum CaptureKind : std::uint32_t {
    CapturePlayers = 0,
    CaptureRoundCaches = 1,
    CaptureTurnWidgetItems = 2,
};

struct EnvelopeHeader final {
    std::uintptr_t data{};
    std::int32_t count{};
    std::int32_t capacity{};
    std::int32_t cursor{};
    std::int32_t reserved{};
    std::int32_t message_type{};
    std::int32_t trailing{};
};

static_assert(offsetof(EnvelopeHeader, count) == 0x8);
static_assert(offsetof(EnvelopeHeader, cursor) == 0x10);
static_assert(offsetof(EnvelopeHeader, message_type) == 0x18);

struct ArrayHeader final {
    std::uintptr_t data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

static_assert(kBidKingPlayerRoundCacheOffset + sizeof(ArrayHeader) <= kBidKingRecordSize);
static_assert(kBidKingRoundCacheRecordSize >= 0x18);

struct PoolCapture final {
    std::uint64_t sequence{};
    std::uint64_t snapshot_id{};
    std::uint64_t player_state{};
    std::uint64_t activity{};
    std::uint64_t pool_data{};
    std::uint64_t round_cache_data{};
    std::uint64_t envelope_data{};
    std::uint64_t turn_widget{};
    std::uint64_t turn_widget_item_data{};
    std::int64_t parent_role_id{};
    std::uint32_t message_type{};
    std::int32_t turn_widget_target_index{};
    std::int32_t envelope_count{};
    std::int32_t envelope_capacity{};
    std::int32_t envelope_cursor{};
    std::int32_t pool_count{};
    std::int32_t pool_capacity{};
    std::int32_t round_cache_count{};
    std::int32_t round_cache_capacity{};
    std::int32_t turn_widget_item_count{};
    std::int32_t turn_widget_item_capacity{};
    std::uint32_t capture_kind{};
    std::uint32_t parent_player_index{};
    std::uint32_t record_size{};
    std::uint32_t record_start_index{};
    std::uint32_t attempted_record_count{};
    std::uint32_t captured_record_count{};
    std::uint32_t valid_record_mask{};
    std::uint32_t record_read_failures{};
    std::uint32_t flags{};
    std::uint32_t captured_envelope_payload_bytes{};
    std::uint32_t captured_turn_widget_item_count{};
    std::array<std::array<std::uint8_t, kBidKingRecordSize>, kRecordsPerCapture> records{};
    std::array<std::uint8_t, kMaximumCapturedEnvelopePayloadBytes> envelope_payload{};
    std::array<std::int64_t, kMaximumCapturedTurnWidgetItems> turn_widget_items{};
};

static_assert(std::is_trivially_copyable_v<PoolCapture>);

template <typename T, std::size_t Capacity>
class BoundedMpmcRing final {
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1U)) == 0);

    struct Cell final {
        std::atomic<std::size_t> sequence{};
        T value{};
    };

public:
    BoundedMpmcRing() noexcept {
        for (std::size_t index = 0; index < Capacity; ++index) {
            cells_[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    bool Push(const T& value) noexcept {
        std::size_t position = enqueue_position_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[position & (Capacity - 1U)];
            const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue_position_.compare_exchange_weak(
                        position, position + 1U, std::memory_order_relaxed)) {
                    cell.value = value;
                    cell.sequence.store(position + 1U, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_position_.load(std::memory_order_relaxed);
            }
        }
    }

    bool Pop(T& value) noexcept {
        std::size_t position = dequeue_position_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[position & (Capacity - 1U)];
            const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position + 1U);
            if (difference == 0) {
                if (dequeue_position_.compare_exchange_weak(
                        position, position + 1U, std::memory_order_relaxed)) {
                    value = cell.value;
                    cell.sequence.store(position + Capacity, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue_position_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    std::array<Cell, Capacity> cells_{};
    std::atomic<std::size_t> enqueue_position_{};
    std::atomic<std::size_t> dequeue_position_{};
};

using ClientResponseFn = void(ANOMALY_CALL*)(void*, void*);
using ActivityLookupFn = void*(ANOMALY_CALL*)(void*);
using TurnWidgetStartLotteryFn = void(ANOMALY_CALL*)(void*, std::int32_t);

struct Context final {
    const AnomalyCoreServiceV1* core{};
    const AnomalyStorageServiceV1* storage{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyHookServiceV1* hook{};
    const AnomalyUe5BuildServiceV1* ue5_build{};
    AnomalyGenerationHandleV1 hook_handle{};
    AnomalyGenerationHandleV1 turn_widget_hook_handle{};
    AnomalyGenerationHandleV1 flush_task{};
    std::atomic_bool capture_open{};
    std::atomic_bool stopping{true};
    std::atomic_bool flush_queued{};
    std::atomic<std::uint64_t> next_sequence{};
    std::atomic<std::uint64_t> next_snapshot_id{};
    std::atomic<std::uint64_t> records_enqueued{};
    std::atomic<std::uint64_t> ring_dropped{};
    std::atomic<std::uint64_t> records_drained{};
    std::atomic<std::uint64_t> history_dropped{};
    std::mutex lifecycle_mutex;
    std::mutex persistence_mutex;
    std::mutex schedule_mutex;
    BoundedMpmcRing<PoolCapture, kRingCapacity> ring;
    std::vector<PoolCapture> history;
    std::string activation_state{"not-started"};
    std::array<char, 128> profile_hash{};
    std::size_t profile_hash_size{};
    std::uintptr_t dispatch_target{};
    std::uintptr_t activity_lookup_target{};
    std::uintptr_t turn_widget_target{};
    bool start_attempted{};
    bool stop_completed{};
};

std::atomic<Context*> g_active{};
std::atomic<const AnomalyHookServiceV1*> g_hook_service{};
std::atomic<std::uint64_t> g_hook_id{};
std::atomic<std::uint64_t> g_hook_generation{};
std::atomic<ClientResponseFn> g_original{};
std::atomic<ActivityLookupFn> g_activity_lookup{};
std::atomic<std::uint64_t> g_turn_widget_hook_id{};
std::atomic<std::uint64_t> g_turn_widget_hook_generation{};
std::atomic<TurnWidgetStartLotteryFn> g_turn_widget_original{};

constexpr AnomalyStatusV1 Status(const std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

template <typename Table, typename Field>
bool HasField(const Table* table, const std::size_t offset) noexcept {
    return table != nullptr && table->struct_size >= offset + sizeof(Field);
}

bool CoreReady(const AnomalyCoreServiceV1* core) noexcept {
    return HasField<AnomalyCoreServiceV1, decltype(AnomalyCoreServiceV1::read_memory)>(
               core, offsetof(AnomalyCoreServiceV1, read_memory)) &&
        core->read_memory != nullptr;
}

bool StorageReady(const AnomalyStorageServiceV1* storage) noexcept {
    return HasField<AnomalyStorageServiceV1, decltype(AnomalyStorageServiceV1::write_atomic)>(
               storage, offsetof(AnomalyStorageServiceV1, write_atomic)) &&
        storage->write_atomic != nullptr;
}

bool SchedulerReady(const AnomalySchedulerServiceV1* scheduler) noexcept {
    return HasField<AnomalySchedulerServiceV1, decltype(AnomalySchedulerServiceV1::schedule)>(
               scheduler, offsetof(AnomalySchedulerServiceV1, schedule)) &&
        scheduler->schedule != nullptr;
}

bool SchedulerCanCancel(const AnomalySchedulerServiceV1* scheduler) noexcept {
    return HasField<AnomalySchedulerServiceV1, decltype(AnomalySchedulerServiceV1::cancel)>(
               scheduler, offsetof(AnomalySchedulerServiceV1, cancel)) &&
        scheduler->cancel != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1* signature) noexcept {
    return HasField<AnomalySignatureServiceV1, decltype(AnomalySignatureServiceV1::resolve)>(
               signature, offsetof(AnomalySignatureServiceV1, resolve)) &&
        signature->resolve != nullptr;
}

bool HookReady(const AnomalyHookServiceV1* hook) noexcept {
    return HasField<AnomalyHookServiceV1, decltype(AnomalyHookServiceV1::create)>(
               hook, offsetof(AnomalyHookServiceV1, create)) &&
        HasField<AnomalyHookServiceV1, decltype(AnomalyHookServiceV1::release)>(
            hook, offsetof(AnomalyHookServiceV1, release)) &&
        HasField<AnomalyHookServiceV1, decltype(AnomalyHookServiceV1::begin_callback)>(
            hook, offsetof(AnomalyHookServiceV1, begin_callback)) &&
        HasField<AnomalyHookServiceV1, decltype(AnomalyHookServiceV1::end_callback)>(
            hook, offsetof(AnomalyHookServiceV1, end_callback)) &&
        hook->create != nullptr && hook->release != nullptr && hook->begin_callback != nullptr &&
        hook->end_callback != nullptr;
}

bool Ue5BuildReady(Context& context) noexcept {
    const auto* const build = context.ue5_build;
    if (!HasField<AnomalyUe5BuildServiceV1, decltype(AnomalyUe5BuildServiceV1::profile_hash)>(
            build, offsetof(AnomalyUe5BuildServiceV1, profile_hash)) ||
        build->profile_hash == nullptr) {
        return false;
    }

    std::array<char, 128> profile_hash{};
    std::size_t profile_hash_size = profile_hash.size();
    if (build->profile_hash(build->user, profile_hash.data(), &profile_hash_size).code !=
            ANOMALY_STATUS_V1_OK ||
        profile_hash_size <= 1U || profile_hash_size > profile_hash.size()) {
        return false;
    }
    context.profile_hash = profile_hash;
    context.profile_hash_size = profile_hash_size - 1U;
    return true;
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host, const char* id) noexcept {
    return anomaly::sdk::Host(host).Query<Service>(id, 1).get();
}

void SetActivationState(Context& context, const std::string_view state) noexcept {
    try {
        std::scoped_lock lock(context.persistence_mutex);
        context.activation_state.assign(state);
    } catch (...) {
    }
}

bool ReadMemory(const Context& context, const std::uintptr_t address, void* destination,
                const std::size_t size) noexcept {
    if (address == 0 || destination == nullptr || size == 0 || !CoreReady(context.core)) return false;
    const AnomalyMutableByteSpanV1 bytes{
        reinterpret_cast<std::uint8_t*>(destination), size};
    return context.core->read_memory(context.core->user, address, bytes).code ==
        ANOMALY_STATUS_V1_OK;
}

bool IsPayloadCaptureCandidate(const std::uint32_t message_type) noexcept {
    // The validated dispatcher rejects response types above 20. Capture every supported envelope so
    // terminal/result messages are retained alongside the match-start traffic.
    return message_type <= kMaximumBidKingResponseType;
}

void CaptureEnvelopePayload(
    Context& context, const EnvelopeHeader& header, PoolCapture& capture) noexcept {
    capture.envelope_data = header.data;
    capture.envelope_capacity = header.capacity;
    if (!IsPayloadCaptureCandidate(capture.message_type)) return;
    if (header.count < 0 || header.capacity < header.count ||
        header.count > static_cast<std::int32_t>(kMaximumPlausibleEnvelopePayloadBytes) ||
        header.capacity > static_cast<std::int32_t>(kMaximumPlausibleEnvelopePayloadBytes) ||
        (header.count != 0 && header.data == 0)) {
        return;
    }

    capture.flags |= CaptureEnvelopePayloadHeaderPlausible;
    const std::size_t bytes_to_copy = (std::min)(
        static_cast<std::size_t>(header.count), kMaximumCapturedEnvelopePayloadBytes);
    if (bytes_to_copy != static_cast<std::size_t>(header.count)) {
        capture.flags |= CaptureEnvelopePayloadTruncated;
    }
    if (bytes_to_copy == 0) return;
    if (!ReadMemory(context, header.data, capture.envelope_payload.data(), bytes_to_copy)) {
        capture.flags |= CaptureEnvelopePayloadReadFailure;
        return;
    }
    capture.captured_envelope_payload_bytes = static_cast<std::uint32_t>(bytes_to_copy);
    capture.flags |= CaptureEnvelopePayloadCopied;
}

Context* BeginCapture(const AnomalyHookServiceV1*& service,
                      AnomalyGenerationHandleV1& callback_lease,
                      const AnomalyGenerationHandleV1 hook_lease) noexcept {
    Context* const context = g_active.load(std::memory_order_acquire);
    service = g_hook_service.load(std::memory_order_acquire);
    if (context == nullptr || !context->capture_open.load(std::memory_order_acquire) ||
        service == nullptr || service->begin_callback == nullptr || service->end_callback == nullptr) {
        return nullptr;
    }
    if (hook_lease.id == 0 ||
        service->begin_callback(service->user, hook_lease, &callback_lease).code !=
            ANOMALY_STATUS_V1_OK) {
        return nullptr;
    }
    if (!context->capture_open.load(std::memory_order_acquire)) {
        static_cast<void>(service->end_callback(service->user, callback_lease));
        callback_lease = {};
        return nullptr;
    }
    return context;
}

void EndCapture(const AnomalyHookServiceV1* service,
                const AnomalyGenerationHandleV1 callback_lease) noexcept {
    if (service != nullptr && service->end_callback != nullptr && callback_lease.id != 0) {
        static_cast<void>(service->end_callback(service->user, callback_lease));
    }
}

const char* MessageName(const std::uint32_t message_type) noexcept {
    switch (message_type) {
    case kStartGameResponse: return "BidKing_StartGame_Response";
    case kStartRoundResponse: return "BidKing_StartRound_Response";
    case kTurnWidgetItemListCapture: return "HTUI_BidKingTurnWidget_StartLotteryWithTarget";
    default: return "unknown";
    }
}

void Record(Context& context, PoolCapture capture) noexcept {
    capture.sequence = context.next_sequence.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (context.ring.Push(capture)) {
        context.records_enqueued.fetch_add(1U, std::memory_order_relaxed);
    } else {
        context.ring_dropped.fetch_add(1U, std::memory_order_relaxed);
    }
}

void Drain(Context& context) {
    PoolCapture capture;
    while (context.ring.Pop(capture)) {
        context.records_drained.fetch_add(1U, std::memory_order_relaxed);
        if (context.history.size() < kMaximumHistoryRecords) {
            context.history.push_back(capture);
        } else {
            context.history_dropped.fetch_add(1U, std::memory_order_relaxed);
        }
    }
}

void AppendUnsigned(std::string& destination, const std::uint64_t value) {
    destination += std::to_string(value);
}

void AppendSigned(std::string& destination, const std::int64_t value) {
    destination += std::to_string(value);
}

void AppendAddress(std::string& destination, const std::uint64_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    destination += "\"0x";
    for (std::size_t index = 0; index < 16; ++index) {
        const unsigned shift = static_cast<unsigned>((15U - index) * 4U);
        destination.push_back(kHex[(value >> shift) & 0x0fU]);
    }
    destination.push_back('\"');
}

template <std::size_t Size>
void AppendBytes(std::string& destination, const std::array<std::uint8_t, Size>& bytes,
                 const std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    destination.push_back('\"');
    for (std::size_t index = 0; index < (std::min)(size, bytes.size()); ++index) {
        const std::uint8_t value = bytes[index];
        destination.push_back(kHex[value >> 4U]);
        destination.push_back(kHex[value & 0x0fU]);
    }
    destination.push_back('\"');
}

std::uint32_t ReadLittleEndianU32(
    const std::array<std::uint8_t, kBidKingRecordSize>& bytes, const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t ReadLittleEndianU64(
    const std::array<std::uint8_t, kBidKingRecordSize>& bytes, const std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::int64_t ReadLittleEndianI64(
    const std::array<std::uint8_t, kBidKingRecordSize>& bytes, const std::size_t offset) noexcept {
    const std::uint64_t raw = ReadLittleEndianU64(bytes, offset);
    std::int64_t value{};
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::int32_t ReadLittleEndianI32(
    const std::array<std::uint8_t, kBidKingRecordSize>& bytes, const std::size_t offset) noexcept {
    const std::uint32_t raw = ReadLittleEndianU32(bytes, offset);
    std::int32_t value{};
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

const char* CaptureKindName(const std::uint32_t capture_kind) noexcept {
    switch (capture_kind) {
    case CaptureRoundCaches: return "roundCaches";
    case CaptureTurnWidgetItems: return "turnWidgetItems";
    default: return "players";
    }
}

std::string Serialize(const Context& context) {
    std::string output;
    output.reserve(1024U + context.history.size() * 5000U);
    output += "{\"schemaVersion\":4,\"kind\":\"bidking-round-pool-probe\",\"activity\":\"BidKing\"";
    output += ",\"activation\":\"" + context.activation_state + "\"";
    output += ",\"profileHash\":\"";
    output.append(context.profile_hash.data(), context.profile_hash_size);
    output += "\"";
    output += ",\"dispatchTarget\":";
    AppendAddress(output, context.dispatch_target);
    output += ",\"activityLookupTarget\":";
    AppendAddress(output, context.activity_lookup_target);
    output += ",\"turnWidgetStartLotteryTarget\":";
    AppendAddress(output, context.turn_widget_target);
    output += ",\"integrity\":{\"recordsEnqueued\":";
    AppendUnsigned(output, context.records_enqueued.load(std::memory_order_relaxed));
    output += ",\"recordsDrained\":";
    AppendUnsigned(output, context.records_drained.load(std::memory_order_relaxed));
    output += ",\"ringDropped\":";
    AppendUnsigned(output, context.ring_dropped.load(std::memory_order_relaxed));
    output += ",\"historyDropped\":";
    AppendUnsigned(output, context.history_dropped.load(std::memory_order_relaxed));
    output += "},\"responses\":[";
    for (std::size_t capture_index = 0; capture_index < context.history.size(); ++capture_index) {
        if (capture_index != 0) output.push_back(',');
        const PoolCapture& capture = context.history[capture_index];
        output += "{\"sequence\":";
        AppendUnsigned(output, capture.sequence);
        output += ",\"snapshotId\":";
        AppendUnsigned(output, capture.snapshot_id);
        output += ",\"messageType\":";
        AppendUnsigned(output, capture.message_type);
        output += ",\"messageName\":\"";
        output += MessageName(capture.message_type);
        output += "\",\"playerState\":";
        AppendAddress(output, capture.player_state);
        output += ",\"activity\":";
        AppendAddress(output, capture.activity);
        output += ",\"captureKind\":\"";
        output += CaptureKindName(capture.capture_kind);
        output += "\"";
        output += ",\"activityLookupAttempted\":";
        output += (capture.flags & CaptureActivityLookupAttempted) != 0 ? "true" : "false";
        output += ",\"activityFound\":";
        output += (capture.flags & CaptureActivityFound) != 0 ? "true" : "false";
        output += ",\"envelope\":{\"data\":";
        AppendAddress(output, capture.envelope_data);
        output += ",\"count\":";
        AppendUnsigned(output, static_cast<std::uint32_t>((std::max)(capture.envelope_count, 0)));
        output += ",\"capacity\":";
        AppendUnsigned(output, static_cast<std::uint32_t>((std::max)(capture.envelope_capacity, 0)));
        output += ",\"cursor\":";
        AppendUnsigned(output, static_cast<std::uint32_t>((std::max)(capture.envelope_cursor, 0)));
        output += "}";
        if (capture.capture_kind == CapturePlayers) {
            output += ",\"payload\":{\"eligible\":";
            output += IsPayloadCaptureCandidate(capture.message_type) ? "true" : "false";
            output += ",\"headerPlausible\":";
            output += (capture.flags & CaptureEnvelopePayloadHeaderPlausible) != 0 ? "true" : "false";
            output += ",\"capturedBytes\":";
            AppendUnsigned(output, capture.captured_envelope_payload_bytes);
            output += ",\"truncated\":";
            output += (capture.flags & CaptureEnvelopePayloadTruncated) != 0 ? "true" : "false";
            output += ",\"readFailed\":";
            output += (capture.flags & CaptureEnvelopePayloadReadFailure) != 0 ? "true" : "false";
            if ((capture.flags & CaptureEnvelopePayloadCopied) != 0) {
                output += ",\"bytes\":";
                AppendBytes(output, capture.envelope_payload, capture.captured_envelope_payload_bytes);
            }
            output += "}";
        }
        output += ",\"pool\":{\"data\":";
        AppendAddress(output, capture.pool_data);
        output += ",\"count\":";
        AppendUnsigned(output, static_cast<std::uint32_t>((std::max)(capture.pool_count, 0)));
        output += ",\"capacity\":";
        AppendUnsigned(output, static_cast<std::uint32_t>((std::max)(capture.pool_capacity, 0)));
        output += ",\"recordSize\":";
        AppendUnsigned(output, kBidKingRecordSize);
        output += ",\"recordStartIndex\":";
        AppendUnsigned(output, capture.record_start_index);
        output += ",\"attemptedRecords\":";
        AppendUnsigned(output, capture.attempted_record_count);
        output += ",\"capturedRecords\":";
        AppendUnsigned(output, capture.captured_record_count);
        output += ",\"truncated\":";
        output += (capture.flags & CapturePoolTruncated) != 0 ? "true" : "false";
        output += "},\"flags\":";
        AppendUnsigned(output, capture.flags);
        output += ",\"recordReadFailures\":";
        AppendUnsigned(output, capture.record_read_failures);
        if (capture.capture_kind == CaptureRoundCaches) {
            output += ",\"roundCache\":{\"parentPlayerIndex\":";
            AppendUnsigned(output, capture.parent_player_index);
            output += ",\"parentRoleId\":";
            AppendSigned(output, capture.parent_role_id);
            output += ",\"data\":";
            AppendAddress(output, capture.round_cache_data);
            output += ",\"count\":";
            AppendUnsigned(output, static_cast<std::uint32_t>((std::max)(capture.round_cache_count, 0)));
            output += ",\"capacity\":";
            AppendUnsigned(
                output, static_cast<std::uint32_t>((std::max)(capture.round_cache_capacity, 0)));
            output += ",\"recordSize\":";
            AppendUnsigned(output, capture.record_size);
            output += ",\"recordStartIndex\":";
            AppendUnsigned(output, capture.record_start_index);
            output += ",\"attemptedRecords\":";
            AppendUnsigned(output, capture.attempted_record_count);
            output += ",\"capturedRecords\":";
            AppendUnsigned(output, capture.captured_record_count);
            output += ",\"truncated\":";
            output += (capture.flags & (CaptureRoundCacheTruncated |
                                        CaptureRoundCacheSnapshotTruncated)) != 0
                ? "true"
                : "false";
            output += "}";
        }
        if (capture.capture_kind == CaptureTurnWidgetItems) {
            output += ",\"turnWidget\":{\"instance\":";
            AppendAddress(output, capture.turn_widget);
            output += ",\"targetIndex\":";
            AppendSigned(output, capture.turn_widget_target_index);
            output += ",\"itemList\":{\"data\":";
            AppendAddress(output, capture.turn_widget_item_data);
            output += ",\"count\":";
            AppendUnsigned(output,
                static_cast<std::uint32_t>((std::max)(capture.turn_widget_item_count, 0)));
            output += ",\"capacity\":";
            AppendUnsigned(output,
                static_cast<std::uint32_t>((std::max)(capture.turn_widget_item_capacity, 0)));
            output += ",\"capturedItems\":";
            AppendUnsigned(output, capture.captured_turn_widget_item_count);
            output += ",\"truncated\":";
            output += (capture.flags & CaptureTurnWidgetItemListTruncated) != 0 ? "true" : "false";
            output += ",\"readFailed\":";
            output += (capture.flags & CaptureTurnWidgetItemListReadFailure) != 0 ? "true" : "false";
            output += "},\"items\":[";
            for (std::size_t item_index = 0;
                 item_index < capture.captured_turn_widget_item_count; ++item_index) {
                if (item_index != 0) output.push_back(',');
                AppendSigned(output, capture.turn_widget_items[item_index]);
            }
            output += "]}";
        }
        output += ",\"records\":[";
        const std::size_t record_size = (std::min)(
            static_cast<std::size_t>(capture.record_size), kBidKingRecordSize);
        for (std::size_t record_index = 0; record_index < capture.attempted_record_count;
             ++record_index) {
            if (record_index != 0) output.push_back(',');
            output += "{\"index\":";
            AppendUnsigned(output, capture.record_start_index + record_index);
            const bool valid = (capture.valid_record_mask & (1U << record_index)) != 0;
            output += ",\"valid\":";
            output += valid ? "true" : "false";
            if (!valid) {
                output += "}";
                continue;
            }
            output += ",\"bytes\":";
            AppendBytes(output, capture.records[record_index], record_size);
            output += ",\"u32\":[";
            for (std::size_t offset = 0; offset < record_size; offset += sizeof(std::uint32_t)) {
                if (offset != 0) output.push_back(',');
                AppendUnsigned(output, ReadLittleEndianU32(capture.records[record_index], offset));
            }
            output += "]";
            if (capture.capture_kind == CaptureRoundCaches &&
                record_size == kBidKingRoundCacheRecordSize) {
                output += ",\"roundId\":";
                AppendSigned(output, ReadLittleEndianI32(capture.records[record_index], 0x00));
                output += ",\"itemId\":{\"comparisonIndex\":";
                AppendUnsigned(output, ReadLittleEndianU32(capture.records[record_index], 0x04));
                output += ",\"number\":";
                AppendUnsigned(output, ReadLittleEndianU32(capture.records[record_index], 0x08));
                output += "},\"bidMoney\":";
                AppendSigned(output, ReadLittleEndianI64(capture.records[record_index], 0x10));
            }
            output += "}";
        }
        output += "]}";
    }
    output += "]}";
    return output;
}

bool Persist(Context& context) noexcept {
    if (!StorageReady(context.storage)) return false;
    try {
        std::scoped_lock lock(context.persistence_mutex);
        Drain(context);
        const std::string document = Serialize(context);
        const AnomalyByteSpanV1 bytes{
            reinterpret_cast<const std::uint8_t*>(document.data()), document.size()};
        const bool status_written = context.storage->write_atomic(
            context.storage->user, anomaly::sdk::StringView(kStatusStoragePath), bytes).code ==
            ANOMALY_STATUS_V1_OK;

        // A newly armed generation has no evidence yet. Keep the preceding capture intact so a
        // hot reload cannot replace a completed match with an empty response list.
        if (context.history.empty()) return status_written;

        const bool capture_written = context.storage->write_atomic(
            context.storage->user, anomaly::sdk::StringView(kStoragePath), bytes).code ==
            ANOMALY_STATUS_V1_OK;
        return status_written && capture_written;
    } catch (...) {
        return false;
    }
}

void ScheduleFlush(Context& context, std::uint32_t delay_milliseconds) noexcept;

void ANOMALY_CALL FlushTask(void* user, AnomalyGenerationHandleV1 task) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return;
    {
        std::scoped_lock lock(context->schedule_mutex);
        if (context->flush_task.id == task.id &&
            context->flush_task.generation == task.generation) {
            context->flush_task = {};
        }
        context->flush_queued.store(false, std::memory_order_release);
    }
    if (!context->stopping.load(std::memory_order_acquire)) {
        static_cast<void>(Persist(*context));
    }
}

void ScheduleFlush(Context& context, const std::uint32_t delay_milliseconds) noexcept {
    if (context.stopping.load(std::memory_order_acquire) || !SchedulerReady(context.scheduler) ||
        context.flush_queued.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    AnomalyGenerationHandleV1 task{};
    if (context.scheduler->schedule(
            context.scheduler->user, delay_milliseconds, FlushTask, &context, &task).code !=
        ANOMALY_STATUS_V1_OK) {
        context.flush_queued.store(false, std::memory_order_release);
        return;
    }
    bool cancel{};
    {
        std::scoped_lock lock(context.schedule_mutex);
        if (context.stopping.load(std::memory_order_acquire)) {
            cancel = true;
        } else {
            context.flush_task = task;
        }
    }
    if (cancel && SchedulerCanCancel(context.scheduler)) {
        static_cast<void>(context.scheduler->cancel(context.scheduler->user, task));
        context.flush_queued.store(false, std::memory_order_release);
    }
}

void CancelFlush(Context& context) noexcept {
    AnomalyGenerationHandleV1 task{};
    {
        std::scoped_lock lock(context.schedule_mutex);
        task = context.flush_task;
        context.flush_task = {};
        context.flush_queued.store(false, std::memory_order_release);
    }
    if (task.id != 0 && SchedulerCanCancel(context.scheduler)) {
        static_cast<void>(context.scheduler->cancel(context.scheduler->user, task));
    }
}

bool ResolveActivity(Context&, PoolCapture& capture) noexcept {
    const ActivityLookupFn lookup = g_activity_lookup.load(std::memory_order_acquire);
    if (lookup == nullptr || capture.player_state == 0) return false;
    capture.flags |= CaptureActivityLookupAttempted;
    void* activity{};
    __try {
        activity = lookup(reinterpret_cast<void*>(capture.player_state));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (activity == nullptr) return false;
    capture.activity = reinterpret_cast<std::uintptr_t>(activity);
    capture.flags |= CaptureActivityFound;
    return true;
}

bool ResolveRecordAddress(const std::uintptr_t data, const std::size_t index,
                          const std::size_t record_size,
                          std::uintptr_t& address) noexcept {
    if (data == 0 || record_size == 0 ||
        index > ((std::numeric_limits<std::uintptr_t>::max)() - data) / record_size) {
        return false;
    }
    address = data + index * record_size;
    return true;
}

void SnapshotRoundCache(Context& context, const PoolCapture& capture,
                        const std::uint32_t parent_player_index,
                        const std::array<std::uint8_t, kBidKingRecordSize>& player,
                        std::size_t& snapshot_remaining) noexcept {
    ArrayHeader header{};
    std::memcpy(
        &header, player.data() + kBidKingPlayerRoundCacheOffset, sizeof(header));

    PoolCapture child = capture;
    child.capture_kind = CaptureRoundCaches;
    child.parent_player_index = parent_player_index;
    child.parent_role_id = ReadLittleEndianI64(player, 0x00);
    child.record_size = static_cast<std::uint32_t>(kBidKingRoundCacheRecordSize);
    child.round_cache_data = header.data;
    child.round_cache_count = header.count;
    child.round_cache_capacity = header.capacity;
    child.record_start_index = 0;
    child.attempted_record_count = 0;
    child.captured_record_count = 0;
    child.valid_record_mask = 0;
    child.record_read_failures = 0;
    child.flags &= ~CaptureRecordReadFailure;
    child.flags |= CaptureRoundCacheHeaderCopied;

    if (header.count < 0 || header.capacity < header.count ||
        header.count > static_cast<std::int32_t>(kMaximumPlausibleRoundCacheRecords) ||
        header.capacity > static_cast<std::int32_t>(kMaximumPlausibleRoundCacheRecords) ||
        (header.count != 0 && header.data == 0)) {
        Record(context, child);
        return;
    }
    child.flags |= CaptureRoundCacheHeaderPlausible;
    if (header.count == 0) return;

    std::size_t records_to_copy = (std::min)(
        static_cast<std::size_t>(header.count), kMaximumRoundCacheRecordsPerPlayer);
    if (records_to_copy != static_cast<std::size_t>(header.count)) {
        child.flags |= CaptureRoundCacheTruncated;
    }
    if (records_to_copy > snapshot_remaining) {
        records_to_copy = snapshot_remaining;
        child.flags |= CaptureRoundCacheSnapshotTruncated;
    }
    if (records_to_copy == 0) {
        child.flags |= CaptureRoundCacheSnapshotTruncated;
        Record(context, child);
        return;
    }

    for (std::size_t start = 0; start < records_to_copy; start += kRecordsPerCapture) {
        PoolCapture batch = child;
        batch.record_start_index = static_cast<std::uint32_t>(start);
        batch.attempted_record_count = static_cast<std::uint32_t>((std::min)(
            kRecordsPerCapture, records_to_copy - start));
        for (std::size_t record_index = 0; record_index < batch.attempted_record_count;
             ++record_index) {
            std::uintptr_t address{};
            if (!ResolveRecordAddress(
                    header.data, start + record_index, kBidKingRoundCacheRecordSize, address) ||
                !ReadMemory(context, address, batch.records[record_index].data(),
                            kBidKingRoundCacheRecordSize)) {
                ++batch.record_read_failures;
                batch.flags |= CaptureRoundCacheRecordReadFailure;
                continue;
            }
            ++batch.captured_record_count;
            batch.valid_record_mask |= 1U << record_index;
        }
        Record(context, batch);
    }
    snapshot_remaining -= records_to_copy;
}

void SnapshotStartRound(Context& context, PoolCapture capture) noexcept {
    if (!ResolveActivity(context, capture)) {
        Record(context, capture);
        return;
    }

    ArrayHeader header{};
    if (!ReadMemory(context, capture.activity + kBidKingPoolOffset, &header, sizeof(header))) {
        Record(context, capture);
        return;
    }
    capture.flags |= CapturePoolHeaderRead;
    capture.pool_data = header.data;
    capture.pool_count = header.count;
    capture.pool_capacity = header.capacity;
    if (header.count < 0 || header.capacity < header.count ||
        header.count > static_cast<std::int32_t>(kMaximumPlausiblePoolRecords) ||
        header.capacity > static_cast<std::int32_t>(kMaximumPlausiblePoolRecords) ||
        (header.count != 0 && header.data == 0)) {
        Record(context, capture);
        return;
    }
    capture.flags |= CapturePoolHeaderPlausible;
    capture.capture_kind = CapturePlayers;
    capture.record_size = static_cast<std::uint32_t>(kBidKingRecordSize);
    const std::size_t records_to_copy = (std::min)(
        static_cast<std::size_t>(header.count), kMaximumPoolRecords);
    if (records_to_copy != static_cast<std::size_t>(header.count)) {
        capture.flags |= CapturePoolTruncated;
    }
    capture.snapshot_id = context.next_snapshot_id.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (records_to_copy == 0) {
        Record(context, capture);
        return;
    }
    std::size_t round_cache_snapshot_remaining = kMaximumRoundCacheRecordsPerSnapshot;
    for (std::size_t start = 0; start < records_to_copy; start += kRecordsPerCapture) {
        PoolCapture batch = capture;
        batch.record_start_index = static_cast<std::uint32_t>(start);
        batch.attempted_record_count = static_cast<std::uint32_t>((std::min)(
            kRecordsPerCapture, records_to_copy - start));
        for (std::size_t record_index = 0; record_index < batch.attempted_record_count;
             ++record_index) {
            std::uintptr_t address{};
            if (!ResolveRecordAddress(header.data, start + record_index, kBidKingRecordSize, address) ||
                !ReadMemory(context, address, batch.records[record_index].data(), kBidKingRecordSize)) {
                ++batch.record_read_failures;
                batch.flags |= CaptureRecordReadFailure;
                continue;
            }
            ++batch.captured_record_count;
            batch.valid_record_mask |= 1U << record_index;
        }
        Record(context, batch);
        for (std::size_t record_index = 0; record_index < batch.attempted_record_count;
             ++record_index) {
            if ((batch.valid_record_mask & (1U << record_index)) == 0) continue;
            SnapshotRoundCache(
                context, batch, static_cast<std::uint32_t>(start + record_index),
                batch.records[record_index], round_cache_snapshot_remaining);
        }
    }
}

void SnapshotTurnWidgetItemList(Context& context, PoolCapture capture) noexcept {
    capture.capture_kind = CaptureTurnWidgetItems;
    capture.record_size = 0;
    capture.turn_widget_item_count = 0;
    capture.turn_widget_item_capacity = 0;
    capture.captured_turn_widget_item_count = 0;
    if (capture.turn_widget == 0 ||
        capture.turn_widget > (std::numeric_limits<std::uintptr_t>::max)() -
            kTurnWidgetItemListOffset) {
        Record(context, capture);
        return;
    }

    ArrayHeader header{};
    if (!ReadMemory(
            context, capture.turn_widget + kTurnWidgetItemListOffset, &header, sizeof(header))) {
        Record(context, capture);
        return;
    }
    capture.flags |= CaptureTurnWidgetItemListHeaderRead;
    capture.turn_widget_item_data = header.data;
    capture.turn_widget_item_count = header.count;
    capture.turn_widget_item_capacity = header.capacity;
    if (header.count < 0 || header.capacity < header.count ||
        header.count > static_cast<std::int32_t>(kMaximumPlausibleTurnWidgetItems) ||
        header.capacity > static_cast<std::int32_t>(kMaximumPlausibleTurnWidgetItems) ||
        (header.count != 0 && header.data == 0)) {
        Record(context, capture);
        return;
    }
    capture.flags |= CaptureTurnWidgetItemListHeaderPlausible;
    const std::size_t items_to_copy = (std::min)(
        static_cast<std::size_t>(header.count), kMaximumCapturedTurnWidgetItems);
    if (items_to_copy != static_cast<std::size_t>(header.count)) {
        capture.flags |= CaptureTurnWidgetItemListTruncated;
    }
    if (items_to_copy != 0 &&
        !ReadMemory(context, header.data, capture.turn_widget_items.data(),
            items_to_copy * sizeof(capture.turn_widget_items[0]))) {
        capture.flags |= CaptureTurnWidgetItemListReadFailure;
        Record(context, capture);
        return;
    }
    capture.captured_turn_widget_item_count = static_cast<std::uint32_t>(items_to_copy);
    if (items_to_copy != 0) capture.flags |= CaptureTurnWidgetItemListCopied;
    Record(context, capture);
}

void ANOMALY_CALL ClientResponseDetour(void* player_state, void* envelope) {
    const ClientResponseFn original = g_original.load(std::memory_order_acquire);
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 callback_lease{};
    const AnomalyGenerationHandleV1 hook_lease{
        g_hook_id.load(std::memory_order_acquire),
        g_hook_generation.load(std::memory_order_acquire)};
    Context* const context = BeginCapture(service, callback_lease, hook_lease);
    PoolCapture capture{};
    bool should_record{};
    if (context != nullptr && envelope != nullptr) {
        EnvelopeHeader header{};
        if (ReadMemory(*context, reinterpret_cast<std::uintptr_t>(envelope), &header, sizeof(header))) {
            capture.flags |= CaptureEnvelopeRead;
            capture.message_type = static_cast<std::uint32_t>(header.message_type);
            capture.envelope_count = header.count;
            capture.envelope_cursor = header.cursor;
            capture.player_state = reinterpret_cast<std::uintptr_t>(player_state);
            // The dispatcher may reuse its input buffer. Copy the bounded response payload
            // before the original handler advances or clears it.
            CaptureEnvelopePayload(*context, header, capture);
            // The dispatcher is BidKing-specific. Capturing every response allows a
            // probe attached after round start to recover the current activity object.
            should_record = true;
        }
    }
    if (original != nullptr) original(player_state, envelope);
    if (context != nullptr && should_record) {
        // An attach can happen after StartRound. Each BidKing response therefore snapshots the
        // current activity while it is still live, with bounded history providing back-pressure.
        SnapshotStartRound(*context, capture);
        ScheduleFlush(*context, kFlushDelayMilliseconds);
    }
    if (context != nullptr) EndCapture(service, callback_lease);
}

void ANOMALY_CALL TurnWidgetStartLotteryDetour(void* widget, const std::int32_t target_index) {
    const TurnWidgetStartLotteryFn original = g_turn_widget_original.load(std::memory_order_acquire);
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 callback_lease{};
    const AnomalyGenerationHandleV1 hook_lease{
        g_turn_widget_hook_id.load(std::memory_order_acquire),
        g_turn_widget_hook_generation.load(std::memory_order_acquire)};
    Context* const context = BeginCapture(service, callback_lease, hook_lease);
    if (context != nullptr && widget != nullptr) {
        PoolCapture capture{};
        capture.message_type = kTurnWidgetItemListCapture;
        capture.turn_widget = reinterpret_cast<std::uintptr_t>(widget);
        capture.turn_widget_target_index = target_index;
        SnapshotTurnWidgetItemList(*context, capture);
        ScheduleFlush(*context, kFlushDelayMilliseconds);
    }
    if (original != nullptr) original(widget, target_index);
    if (context != nullptr) EndCapture(service, callback_lease);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host is invalid");
    }
    auto* const context = new (std::nothrow) Context();
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED, "allocation failed");
    context->core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID);
    context->storage = Query<AnomalyStorageServiceV1>(host, ANOMALY_STORAGE_SERVICE_V1_ID);
    context->scheduler = Query<AnomalySchedulerServiceV1>(host, ANOMALY_SCHEDULER_SERVICE_V1_ID);
    context->signature = Query<AnomalySignatureServiceV1>(host, ANOMALY_SIGNATURE_SERVICE_V1_ID);
    context->hook = Query<AnomalyHookServiceV1>(host, ANOMALY_HOOK_SERVICE_V1_ID);
    context->ue5_build = Query<AnomalyUe5BuildServiceV1>(host, ANOMALY_UE5_BUILD_SERVICE_V1_ID);
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* user) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "probe context is invalid");
    }
    std::scoped_lock lifecycle_lock(context->lifecycle_mutex);
    if (context->start_attempted || g_active.load(std::memory_order_acquire) != nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "probe context is invalid");
    }
    context->start_attempted = true;
    context->stop_completed = false;
    context->stopping.store(false, std::memory_order_release);
    SetActivationState(*context, "resolving");
    if (!CoreReady(context->core) || !StorageReady(context->storage) || !SchedulerReady(context->scheduler) ||
        !SignatureReady(context->signature) || !HookReady(context->hook) || !Ue5BuildReady(*context)) {
        SetActivationState(*context, "inactive-bidking-profile-unavailable");
        static_cast<void>(Persist(*context));
        return anomaly::sdk::Ok();
    }

    std::uintptr_t dispatch{};
    std::uintptr_t lookup{};
    std::uintptr_t turn_widget{};
    const auto dispatch_status = context->signature->resolve(
        context->signature->user, anomaly::sdk::StringView("HTGame.exe"),
        anomaly::sdk::StringView(".text"), anomaly::sdk::StringView(kDispatchPattern), &dispatch);
    const auto lookup_status = context->signature->resolve(
        context->signature->user, anomaly::sdk::StringView("HTGame.exe"),
        anomaly::sdk::StringView(".text"), anomaly::sdk::StringView(kActivityLookupPattern), &lookup);
    const auto turn_widget_status = context->signature->resolve(
        context->signature->user, anomaly::sdk::StringView("HTGame.exe"),
        anomaly::sdk::StringView(".text"),
        anomaly::sdk::StringView(kTurnWidgetStartLotteryPattern), &turn_widget);
    if (dispatch_status.code != ANOMALY_STATUS_V1_OK || lookup_status.code != ANOMALY_STATUS_V1_OK ||
        dispatch == 0 || lookup == 0) {
        SetActivationState(*context, "inactive-bidking-symbols-not-found");
        static_cast<void>(Persist(*context));
        return anomaly::sdk::Ok();
    }

    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = dispatch;
    request.detour = reinterpret_cast<void*>(&ClientResponseDetour);
    request.label = anomaly::sdk::StringView("bidking-client-response-dispatch");
    std::uintptr_t original{};
    if (context->hook->create(
        context->hook->user, &request, &original, &context->hook_handle).code !=
            ANOMALY_STATUS_V1_OK ||
        original == 0 || context->hook_handle.id == 0) {
        SetActivationState(*context, "hook-create-failed");
        return Status(ANOMALY_STATUS_V1_FAILED, "BidKing response hook could not be created");
    }
    context->dispatch_target = dispatch;
    context->activity_lookup_target = lookup;
    context->turn_widget_target =
        turn_widget_status.code == ANOMALY_STATUS_V1_OK ? turn_widget : 0;
    g_original.store(reinterpret_cast<ClientResponseFn>(original), std::memory_order_release);
    g_activity_lookup.store(reinterpret_cast<ActivityLookupFn>(lookup), std::memory_order_release);
    g_hook_service.store(context->hook, std::memory_order_release);
    g_hook_id.store(context->hook_handle.id, std::memory_order_release);
    g_hook_generation.store(context->hook_handle.generation, std::memory_order_release);
    context->capture_open.store(true, std::memory_order_release);
    g_active.store(context, std::memory_order_release);
    if (turn_widget_status.code == ANOMALY_STATUS_V1_OK && turn_widget != 0) {
        AnomalyHookRequestV1 turn_widget_request{};
        turn_widget_request.struct_size = sizeof(turn_widget_request);
        turn_widget_request.kind = ANOMALY_HOOK_V1_FUNCTION;
        turn_widget_request.target = turn_widget;
        turn_widget_request.detour = reinterpret_cast<void*>(&TurnWidgetStartLotteryDetour);
        turn_widget_request.label = anomaly::sdk::StringView("bidking-turn-widget-start-lottery");
        std::uintptr_t turn_widget_original{};
        if (context->hook->create(
                context->hook->user, &turn_widget_request, &turn_widget_original,
                &context->turn_widget_hook_handle).code == ANOMALY_STATUS_V1_OK &&
            turn_widget_original != 0 && context->turn_widget_hook_handle.id != 0) {
            g_turn_widget_original.store(
                reinterpret_cast<TurnWidgetStartLotteryFn>(turn_widget_original),
                std::memory_order_release);
            g_turn_widget_hook_id.store(
                context->turn_widget_hook_handle.id, std::memory_order_release);
            g_turn_widget_hook_generation.store(
                context->turn_widget_hook_handle.generation, std::memory_order_release);
            SetActivationState(*context, "armed");
        } else {
            context->turn_widget_hook_handle = {};
            SetActivationState(*context, "armed-dispatch-only-turn-widget-hook-create-failed");
        }
    } else {
        SetActivationState(*context, "armed-dispatch-only-turn-widget-symbol-not-found");
    }
    static_cast<void>(Persist(*context));
    return anomaly::sdk::Ok();
}

void ClearHookState() noexcept {
    g_active.store(nullptr, std::memory_order_release);
    g_original.store(nullptr, std::memory_order_release);
    g_activity_lookup.store(nullptr, std::memory_order_release);
    g_hook_id.store(0, std::memory_order_release);
    g_hook_generation.store(0, std::memory_order_release);
    g_turn_widget_original.store(nullptr, std::memory_order_release);
    g_turn_widget_hook_id.store(0, std::memory_order_release);
    g_turn_widget_hook_generation.store(0, std::memory_order_release);
    g_hook_service.store(nullptr, std::memory_order_release);
}

void Deactivate(Context& context) noexcept {
    context.capture_open.store(false, std::memory_order_release);
    Context* expected = &context;
    static_cast<void>(g_active.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel));
}

bool ReleaseHook(Context& context, AnomalyGenerationHandleV1& handle) noexcept {
    if (handle.id == 0) return true;
    if (context.hook == nullptr || context.hook->release == nullptr) return false;
    const AnomalyStatusV1 result = context.hook->release(context.hook->user, handle);
    if (result.code != ANOMALY_STATUS_V1_OK && result.code != ANOMALY_STATUS_V1_NOT_FOUND) {
        return false;
    }
    handle = {};
    return true;
}

bool ReleaseHooks(Context& context) noexcept {
    const bool turn_widget_released = ReleaseHook(context, context.turn_widget_hook_handle);
    const bool dispatcher_released = ReleaseHook(context, context.hook_handle);
    return turn_widget_released && dispatcher_released;
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* user, std::uint32_t) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "probe context is invalid");
    std::scoped_lock lifecycle_lock(context->lifecycle_mutex);
    if (context->stop_completed) return anomaly::sdk::Ok();
    if (!context->start_attempted) {
        context->stop_completed = true;
        return anomaly::sdk::Ok();
    }
    context->stopping.store(true, std::memory_order_release);
    Deactivate(*context);
    CancelFlush(*context);
    const bool released = ReleaseHooks(*context);
    SetActivationState(*context, released ? "stopped" : "hook-release-failed");
    if (released) ClearHookState();
    const bool persisted = Persist(*context);
    if (!released) return Status(ANOMALY_STATUS_V1_FAILED, "BidKing hooks did not quiesce");
    if (!persisted) return Status(ANOMALY_STATUS_V1_FAILED, "final persistence failed");
    context->stop_completed = true;
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* user) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return;
    if (Stop(context, 0).code == ANOMALY_STATUS_V1_OK) delete context;
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "descriptor is invalid");
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.diagnostics.nte.bidking-round-pool-probe"),
        anomaly::sdk::StringView("NTE BidKing round pool probe"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("0.1.0"),
        Load, Start, Stop, Unload, nullptr, nullptr};
    return anomaly::sdk::Ok();
}
