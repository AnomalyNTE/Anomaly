#include "anomaly/sdk/cpp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kMaximumObjectsPerUpdate = 32;
constexpr std::uint32_t kServiceRetryInterval = 120;
constexpr std::size_t kMaximumResolvedNameBytes = 1024;
constexpr std::size_t kMaximumMatches = 2048;
constexpr std::string_view kStoragePath = "bidking_objects.json";
constexpr auto kUpdateBudget = std::chrono::milliseconds(1);

template <typename Table, typename Field>
bool HasField(const Table* table, const std::size_t offset) noexcept {
    return table != nullptr && table->struct_size >= offset + sizeof(Field);
}

struct Match final {
    std::uint64_t handle_id{};
    std::uint32_t object_index{};
    std::uint32_t object_serial{};
    std::uint32_t name_id{};
    std::uint32_t flags{};
    std::string name;
};

struct Catalog final {
    std::uint64_t generation{};
    std::uint32_t object_count{};
    std::uint32_t next_object_index{};
    std::uint64_t complete_passes{};
    std::uint64_t slots_scanned{};
    std::uint64_t empty_slots{};
    std::uint64_t snapshot_failures{};
    std::uint64_t name_resolve_failures{};
    std::uint64_t match_overflow{};
    std::uint64_t revision{1};
    std::uint64_t persisted_revision{};
    std::vector<Match> matches;
    std::unordered_set<std::uint64_t> seen_handles;
};

struct PersistSnapshot final {
    std::uint64_t generation{};
    std::uint32_t object_count{};
    std::uint32_t next_object_index{};
    std::uint64_t complete_passes{};
    std::uint64_t slots_scanned{};
    std::uint64_t empty_slots{};
    std::uint64_t snapshot_failures{};
    std::uint64_t name_resolve_failures{};
    std::uint64_t match_overflow{};
    std::uint64_t revision{};
    std::vector<Match> matches;
};

struct Context final {
    const AnomalyHostApiV1* host{};
    const AnomalyStorageServiceV1* storage{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5NamesServiceV1* names{};
    std::atomic_bool stopping{true};
    std::uint64_t update_count{};
    std::mutex mutex;
    AnomalyGenerationHandleV1 flush_task{};
    Catalog catalog;
};

Context g_context;

constexpr AnomalyStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {nullptr, 0}};
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

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* objects) noexcept {
    return HasField<AnomalyUe5ObjectsServiceV1, decltype(AnomalyUe5ObjectsServiceV1::generation)>(
               objects, offsetof(AnomalyUe5ObjectsServiceV1, generation)) &&
        HasField<AnomalyUe5ObjectsServiceV1, decltype(AnomalyUe5ObjectsServiceV1::count)>(
            objects, offsetof(AnomalyUe5ObjectsServiceV1, count)) &&
        HasField<AnomalyUe5ObjectsServiceV1, decltype(AnomalyUe5ObjectsServiceV1::snapshot_at)>(
            objects, offsetof(AnomalyUe5ObjectsServiceV1, snapshot_at)) &&
        objects->generation != nullptr && objects->count != nullptr && objects->snapshot_at != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* names) noexcept {
    return HasField<AnomalyUe5NamesServiceV1, decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
               names, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
        names->resolve_utf8 != nullptr;
}

char LowerAscii(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

bool ContainsInsensitive(const std::string_view value, const std::string_view needle) noexcept {
    if (needle.empty() || value.size() < needle.size()) return false;
    for (std::size_t offset = 0; offset + needle.size() <= value.size(); ++offset) {
        bool match = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
            if (LowerAscii(value[offset + index]) != LowerAscii(needle[index])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool IsTargetName(const std::string_view name) noexcept {
    return ContainsInsensitive(name, "bidking");
}

bool ResolveName(
    const AnomalyUe5NamesServiceV1* names, const std::uint32_t name_id,
    std::string* value, bool* service_unavailable) noexcept {
    if (value == nullptr || service_unavailable == nullptr || !NamesReady(names) || name_id == 0) {
        return false;
    }
    *service_unavailable = false;
    std::size_t size{};
    const AnomalyStatusV1 first = names->resolve_utf8(names->user, name_id, nullptr, &size);
    if (first.code != ANOMALY_STATUS_V1_OK) {
        *service_unavailable = first.code == ANOMALY_STATUS_V1_UNAVAILABLE;
        return false;
    }
    if (size <= 1 || size > kMaximumResolvedNameBytes) return false;
    try {
        std::string resolved(size, '\0');
        const AnomalyStatusV1 second = names->resolve_utf8(
            names->user, name_id, resolved.data(), &size);
        if (second.code != ANOMALY_STATUS_V1_OK || size == 0 || size > resolved.size()) {
            *service_unavailable = second.code == ANOMALY_STATUS_V1_UNAVAILABLE;
            return false;
        }
        const std::size_t terminator = resolved.find('\0');
        if (terminator == std::string::npos) return false;
        resolved.resize(terminator);
        if (resolved.empty()) return false;
        *value = std::move(resolved);
        return true;
    } catch (...) {
        return false;
    }
}

void EscapeJsonString(std::string& destination, const std::string_view value) {
    constexpr char kHex[] = "0123456789abcdef";
    destination.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '"': destination += "\\\""; break;
        case '\\': destination += "\\\\"; break;
        case '\b': destination += "\\b"; break;
        case '\f': destination += "\\f"; break;
        case '\n': destination += "\\n"; break;
        case '\r': destination += "\\r"; break;
        case '\t': destination += "\\t"; break;
        default: {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20U) {
                destination += "\\u00";
                destination.push_back(kHex[(byte >> 4U) & 0x0fU]);
                destination.push_back(kHex[byte & 0x0fU]);
            } else {
                destination.push_back(character);
            }
        }
        }
    }
    destination.push_back('"');
}

void AppendUnsigned(std::string& destination, const std::uint64_t value) {
    destination += std::to_string(value);
}

std::string Serialize(const PersistSnapshot& snapshot) {
    std::string result;
    result.reserve(512U + snapshot.matches.size() * 160U);
    result += "{\"schemaVersion\":1,\"kind\":\"bidking-object-catalog\",\"objectRegistryGeneration\":";
    AppendUnsigned(result, snapshot.generation);
    result += ",\"objectCount\":";
    AppendUnsigned(result, snapshot.object_count);
    result += ",\"nextObjectIndex\":";
    AppendUnsigned(result, snapshot.next_object_index);
    result += ",\"completePasses\":";
    AppendUnsigned(result, snapshot.complete_passes);
    result += ",\"slotsScanned\":";
    AppendUnsigned(result, snapshot.slots_scanned);
    result += ",\"emptySlots\":";
    AppendUnsigned(result, snapshot.empty_slots);
    result += ",\"snapshotFailures\":";
    AppendUnsigned(result, snapshot.snapshot_failures);
    result += ",\"nameResolveFailures\":";
    AppendUnsigned(result, snapshot.name_resolve_failures);
    result += ",\"matchOverflow\":";
    AppendUnsigned(result, snapshot.match_overflow);
    result += ",\"catalogRevision\":";
    AppendUnsigned(result, snapshot.revision);
    result += ",\"matches\":[";
    for (std::size_t index = 0; index < snapshot.matches.size(); ++index) {
        if (index != 0) result.push_back(',');
        const Match& match = snapshot.matches[index];
        result += "{\"handle\":";
        AppendUnsigned(result, match.handle_id);
        result += ",\"objectIndex\":";
        AppendUnsigned(result, match.object_index);
        result += ",\"objectSerial\":";
        AppendUnsigned(result, match.object_serial);
        result += ",\"nameId\":";
        AppendUnsigned(result, match.name_id);
        result += ",\"flags\":";
        AppendUnsigned(result, match.flags);
        result += ",\"name\":";
        EscapeJsonString(result, match.name);
        result.push_back('}');
    }
    result += "]}";
    return result;
}

void ResetCatalog(Context& context) {
    std::scoped_lock lock(context.mutex);
    context.flush_task = {};
    context.catalog = {};
    context.catalog.revision = 1;
}

bool SnapshotForPersistence(Context& context, PersistSnapshot* snapshot) {
    if (snapshot == nullptr) return false;
    std::scoped_lock lock(context.mutex);
    if (context.catalog.revision == context.catalog.persisted_revision) return false;
    snapshot->generation = context.catalog.generation;
    snapshot->object_count = context.catalog.object_count;
    snapshot->next_object_index = context.catalog.next_object_index;
    snapshot->complete_passes = context.catalog.complete_passes;
    snapshot->slots_scanned = context.catalog.slots_scanned;
    snapshot->empty_slots = context.catalog.empty_slots;
    snapshot->snapshot_failures = context.catalog.snapshot_failures;
    snapshot->name_resolve_failures = context.catalog.name_resolve_failures;
    snapshot->match_overflow = context.catalog.match_overflow;
    snapshot->revision = context.catalog.revision;
    snapshot->matches = context.catalog.matches;
    return true;
}

void RecordPersistenceResult(
    Context& context, const PersistSnapshot& snapshot, const bool succeeded) noexcept {
    std::scoped_lock lock(context.mutex);
    if (succeeded && context.catalog.persisted_revision < snapshot.revision) {
        context.catalog.persisted_revision = snapshot.revision;
    }
}

void Persist(Context& context) noexcept {
    if (!StorageReady(context.storage)) return;
    try {
        PersistSnapshot snapshot;
        if (!SnapshotForPersistence(context, &snapshot)) return;
        const std::string document = Serialize(snapshot);
        const auto status = context.storage->write_atomic(
            context.storage->user, anomaly::sdk::StringView(kStoragePath),
            {reinterpret_cast<const std::uint8_t*>(document.data()), document.size()});
        RecordPersistenceResult(context, snapshot, status.code == ANOMALY_STATUS_V1_OK);
    } catch (...) {
    }
}

void ScheduleFlush(Context& context, std::uint32_t delay_milliseconds) noexcept;

void ANOMALY_CALL FlushTask(void* user, const AnomalyGenerationHandleV1 task) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return;
    {
        std::scoped_lock lock(context->mutex);
        if (context->flush_task.id == task.id &&
            context->flush_task.generation == task.generation) {
            context->flush_task = {};
        }
    }
    if (!context->stopping.load(std::memory_order_acquire)) Persist(*context);
}

void ScheduleFlush(Context& context, const std::uint32_t delay_milliseconds) noexcept {
    if (context.stopping.load(std::memory_order_acquire) || !SchedulerReady(context.scheduler)) {
        return;
    }
    {
        std::scoped_lock lock(context.mutex);
        if (context.flush_task.id != 0) return;
    }
    AnomalyGenerationHandleV1 task{};
    const AnomalyStatusV1 status = context.scheduler->schedule(
        context.scheduler->user, delay_milliseconds, FlushTask, &context, &task);
    if (status.code != ANOMALY_STATUS_V1_OK) return;
    std::scoped_lock lock(context.mutex);
    context.flush_task = task;
}

void RefreshUe5Services(Context& context) noexcept {
    const anomaly::sdk::Host host(context.host);
    context.objects = host.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
        ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    context.names = host.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID,
        ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
}

void ResetForGeneration(Context& context, const std::uint64_t generation) {
    std::scoped_lock lock(context.mutex);
    if (context.catalog.generation == generation) return;
    context.catalog = {};
    context.catalog.generation = generation;
    context.catalog.revision = 1;
}

bool CommitBatch(
    Context& context, const std::uint64_t generation, const std::uint32_t object_count,
    const std::uint32_t next_object_index, const std::uint32_t slots_scanned,
    const std::uint32_t empty_slots, const std::uint32_t snapshot_failures,
    const std::uint32_t name_resolve_failures, std::vector<Match> matches) {
    std::scoped_lock lock(context.mutex);
    if (context.catalog.generation != generation) return false;
    bool changed{};
    if (context.catalog.object_count != object_count) {
        context.catalog.object_count = object_count;
        changed = true;
    }
    context.catalog.next_object_index = next_object_index;
    context.catalog.slots_scanned += slots_scanned;
    context.catalog.empty_slots += empty_slots;
    context.catalog.snapshot_failures += snapshot_failures;
    context.catalog.name_resolve_failures += name_resolve_failures;
    if (next_object_index == 0 && slots_scanned != 0) {
        ++context.catalog.complete_passes;
        changed = true;
    }
    for (Match& match : matches) {
        if (!context.catalog.seen_handles.insert(match.handle_id).second) continue;
        if (context.catalog.matches.size() == kMaximumMatches) {
            ++context.catalog.match_overflow;
            changed = true;
            continue;
        }
        context.catalog.matches.push_back(std::move(match));
        changed = true;
    }
    if (changed) ++context.catalog.revision;
    return changed;
}

void ANOMALY_CALL Update(void* user, double) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr || context->stopping.load(std::memory_order_acquire)) return;
    ++context->update_count;
    if (context->objects == nullptr || context->names == nullptr ||
        context->update_count % kServiceRetryInterval == 0) {
        RefreshUe5Services(*context);
    }
    if (!ObjectsReady(context->objects) || !NamesReady(context->names)) return;

    const std::uint64_t generation = context->objects->generation(context->objects->user);
    const std::uint32_t object_count = context->objects->count(context->objects->user);
    if (generation == 0 || object_count == 0) return;
    ResetForGeneration(*context, generation);

    std::uint32_t start{};
    {
        std::scoped_lock lock(context->mutex);
        start = context->catalog.next_object_index;
        if (start >= object_count) start = 0;
    }
    const std::uint32_t end = start + (std::min)(kMaximumObjectsPerUpdate, object_count - start);
    std::uint32_t cursor = start;
    std::uint32_t empty_slots{};
    std::uint32_t snapshot_failures{};
    std::uint32_t name_resolve_failures{};
    std::vector<Match> matches;
    matches.reserve(kMaximumObjectsPerUpdate);
    bool object_service_unavailable{};
    bool name_service_unavailable{};
    const auto batch_started = std::chrono::steady_clock::now();

    while (cursor < end) {
        if (cursor != start && std::chrono::steady_clock::now() - batch_started >= kUpdateBudget) {
            break;
        }
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        const AnomalyStatusV1 object_status = context->objects->snapshot_at(
            context->objects->user, cursor, &snapshot);
        if (object_status.code == ANOMALY_STATUS_V1_UNAVAILABLE) {
            object_service_unavailable = true;
            break;
        }
        ++cursor;
        if (object_status.code != ANOMALY_STATUS_V1_OK) {
            if (object_status.code == ANOMALY_STATUS_V1_NOT_FOUND) ++empty_slots;
            else ++snapshot_failures;
            continue;
        }
        std::string name;
        bool unavailable{};
        if (!ResolveName(context->names, snapshot.name_id, &name, &unavailable)) {
            ++name_resolve_failures;
            if (unavailable) {
                name_service_unavailable = true;
                break;
            }
            continue;
        }
        if (!IsTargetName(name) || snapshot.handle.id == 0) continue;
        matches.push_back({
            snapshot.handle.id,
            ANOMALY_UE5_OBJECT_HANDLE_INDEX(snapshot.handle),
            ANOMALY_UE5_OBJECT_HANDLE_SERIAL(snapshot.handle),
            snapshot.name_id,
            snapshot.flags,
            std::move(name)});
    }

    const std::uint32_t next = cursor == object_count ? 0U : cursor;
    const bool catalog_changed = CommitBatch(
        *context, generation, object_count, next, cursor - start, empty_slots,
        snapshot_failures, name_resolve_failures, std::move(matches));
    if (object_service_unavailable) context->objects = nullptr;
    if (name_service_unavailable) context->names = nullptr;
    if (catalog_changed) ScheduleFlush(*context, 0);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_context.stopping.store(true, std::memory_order_release);
    g_context.host = host;
    const anomaly::sdk::Host view(host);
    g_context.storage = view.Query<AnomalyStorageServiceV1>(
        ANOMALY_STORAGE_SERVICE_V1_ID, ANOMALY_STORAGE_SERVICE_V1_VERSION).get();
    g_context.scheduler = view.Query<AnomalySchedulerServiceV1>(
        ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION).get();
    g_context.objects = nullptr;
    g_context.names = nullptr;
    g_context.update_count = 0;
    ResetCatalog(g_context);
    *plugin_context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* user) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->update_count = 0;
    ResetCatalog(*context);
    context->stopping.store(false, std::memory_order_release);
    ScheduleFlush(*context, 0);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* user, std::uint32_t) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->stopping.store(true, std::memory_order_release);
    AnomalyGenerationHandleV1 task{};
    {
        std::scoped_lock lock(context->mutex);
        task = context->flush_task;
        context->flush_task = {};
    }
    if (task.id != 0 && SchedulerCanCancel(context->scheduler)) {
        static_cast<void>(context->scheduler->cancel(context->scheduler->user, task));
    }
    // The lifecycle callback is a permitted final persistence point even if a
    // host scheduler task has not run yet.
    Persist(*context);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* user) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return;
    context->stopping.store(true, std::memory_order_release);
    context->host = nullptr;
    context->storage = nullptr;
    context->scheduler = nullptr;
    context->objects = nullptr;
    context->names = nullptr;
    context->update_count = 0;
    ResetCatalog(*context);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.diagnostics.nte.bidking-object-catalog"),
        anomaly::sdk::StringView("NTE BidKing object catalog"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("0.1.0"),
        Load, Start, Stop, Unload, Update, nullptr};
    return anomaly::sdk::Ok();
}
