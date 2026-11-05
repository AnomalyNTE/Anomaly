#include "uid_detector.hpp"

#include <anomaly/sdk/cpp.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nte::account {
namespace {

// Read-only discovery of the UID widget. Signatures, object offsets, vtable
// slots and record strides all come from profile/compatibility.profile.json
// through `native::Snapshot`; only discovery cadence and the game-owned widget
// identity stay in code.
constexpr std::string_view kTargetWidgetName = "TextBlock_RoleID";
constexpr std::string_view kTargetWidgetTemplatePath =
    "/Game/UI/Blueprints/Common/BPUI_RoleID.BPUI_RoleID_C:WidgetTree.TextBlock_RoleID";

constexpr std::uint64_t kRuntimeBindingRetryInterval = 300;
constexpr std::uint64_t kWidgetVerifyInterval = 30;
constexpr std::uint64_t kWidgetStabilizationInterval = 20;
constexpr std::uint64_t kEmptyScanRetryInterval = 300;
constexpr std::uint32_t kNamedObjectBatchSize = 1024;
constexpr std::uint32_t kIncrementalObjectBatchSize = 128;
constexpr std::uint32_t kNeighborhoodObjectBatchSize = 512;
constexpr std::uint32_t kRoleIdNeighborhoodRadius = 8192;
constexpr std::size_t kMaximumTrackedWidgets = 64;

struct UnrealString {
    wchar_t* data{};
    std::int32_t count{};
    std::int32_t capacity{};
};
struct UnrealText {
    void* data{};
    std::uint32_t flags{};
    std::uint32_t padding{};
};
using FreeStringFn = void(ANOMALY_CALL*)(void* allocation);
using TextToStringFn = UnrealString*(ANOMALY_CALL*)(UnrealString*, const UnrealText*);

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host,
    const char* id, const std::uint32_t version) noexcept {
    if (host == nullptr || host->query_service == nullptr) return nullptr;
    const void* service{};
    if (host->query_service(host->host_context, {id, std::strlen(id)}, version,
            &service).code != ANOMALY_STATUS_V1_OK) return nullptr;
    return static_cast<const Service*>(service);
}

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1* service) noexcept {
    return service != nullptr &&
        service->struct_size >= offsetof(AnomalyUe5ObjectsServiceV1, find_exact) +
            sizeof(service->find_exact) && service->generation != nullptr &&
        service->count != nullptr && service->snapshot_at != nullptr &&
        service->snapshot_by_handle != nullptr && service->find_exact != nullptr;
}

bool NamesReady(const AnomalyUe5NamesServiceV1* service) noexcept {
    return service != nullptr &&
        service->struct_size >= offsetof(AnomalyUe5NamesServiceV1, resolve_utf8) +
            sizeof(service->resolve_utf8) && service->resolve_utf8 != nullptr;
}

bool ResolveName(const AnomalyUe5NamesServiceV1& names,
    const std::uint32_t name_id, std::string& value) {
    std::size_t size{};
    if (name_id == 0 || names.resolve_utf8(names.user, name_id, nullptr, &size).code !=
            ANOMALY_STATUS_V1_OK || size <= 1 || size > 1024) return false;
    value.assign(size, '\0');
    if (names.resolve_utf8(names.user, name_id, value.data(), &size).code !=
            ANOMALY_STATUS_V1_OK || size == 0 || size > value.size()) return false;
    const auto terminator = value.find('\0');
    if (terminator == std::string::npos) return false;
    value.resize(terminator);
    return true;
}

bool ReadWidgetText(const std::uintptr_t widget,
    const TextToStringFn text_to_string, const std::uint32_t text_field_offset,
    UnrealString& current) noexcept {
    __try {
        const auto* text = reinterpret_cast<const UnrealText*>(
            widget + text_field_offset);
        return text_to_string(&current, text) != nullptr &&
            current.data != nullptr && current.count > 1 && current.count <= 2048 &&
            current.capacity >= current.count && current.data[current.count - 1] == L'\0';
    } __except (1) {
        return false;
    }
}

std::uint64_t CandidateOrder(const AnomalyGenerationHandleV1 handle) noexcept {
    // UE object handles encode serial in the high 32 bits and index + 1 in the
    // low 32 bits. The separate generation field is the object-table generation.
    return handle.id;
}

}  // namespace

struct UidDetector::Impl {
    struct TrackedWidget {
        AnomalyGenerationHandleV1 handle{};
        std::uint64_t stable_after_tick{};
        std::uint64_t last_read_tick{};
    };

    Impl(const AnomalyHostApiV1* value, const native::Snapshot& profile)
        : host(value), layout(profile.layout), bindings(profile.bindings) {
        if (!profile.complete) {
            diagnostics.missing_bindings = profile.error;
            diagnostics.phase = UidDetectorPhase::WaitingForBindings;
            return;
        }
        free_string = reinterpret_cast<FreeStringFn>(bindings.free_string);
        text_to_string = reinterpret_cast<TextToStringFn>(bindings.text_to_string);
        // Both the GObjects accessor and its instruction shape come from the
        // profile; only the RIP-relative decoding itself stays in code.
        std::int32_t displacement{};
        std::memcpy(&displacement,
            reinterpret_cast<const void*>(
                bindings.gobjects_accessor + native::kRipDisplacementOffset),
            sizeof(displacement));
        object_registry = bindings.gobjects_accessor + native::kRipInstructionSize +
            displacement + layout.gobjects_addend;
        profile_bound = object_registry != 0 && free_string != nullptr &&
            text_to_string != nullptr && bindings.set_text != 0;
        if (!profile_bound) {
            diagnostics.missing_bindings = "compatibility profile snapshot is unusable";
            diagnostics.phase = UidDetectorPhase::WaitingForBindings;
        }
    }

    bool BindRuntime() noexcept {
        if (!profile_bound) {
            diagnostics.phase = UidDetectorPhase::WaitingForBindings;
            return false;
        }
        if (!ObjectsReady(objects)) {
            objects = Query<AnomalyUe5ObjectsServiceV1>(host,
                ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
        }
        if (!NamesReady(names)) {
            names = Query<AnomalyUe5NamesServiceV1>(host,
                ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
        }

        diagnostics.missing_services.clear();
        const auto AddMissing = [](std::string& destination, const std::string_view value) {
            if (!destination.empty()) destination += ',';
            destination.append(value);
        };
        if (!ObjectsReady(objects)) AddMissing(diagnostics.missing_services, "objects");
        if (!NamesReady(names)) AddMissing(diagnostics.missing_services, "names");
        if (!diagnostics.missing_services.empty()) {
            diagnostics.phase = UidDetectorPhase::WaitingForServices;
            return false;
        }

        diagnostics.missing_bindings.clear();
        ready = true;
        if (diagnostics.phase != UidDetectorPhase::Detected) {
            diagnostics.phase = widgets.empty() ? UidDetectorPhase::Scanning :
                UidDetectorPhase::WidgetPending;
        }
        return ready;
    }

    void BeginScan(const std::uint32_t start, const std::uint32_t end,
        const std::uint32_t batch, const bool names_only) noexcept {
        scan_start = (std::min)(start, end);
        scan_cursor = end;
        scan_batch = batch;
        named_scan = names_only;
        scan_active = scan_cursor > scan_start;
        diagnostics.phase = widgets.empty() ? UidDetectorPhase::Scanning :
            UidDetectorPhase::WidgetPending;
    }

    void BeginRecoveryScan(const std::uint32_t count) noexcept {
        if (recovery_anchor == 0 && !widgets.empty()) {
            recovery_anchor = static_cast<std::uint32_t>(widgets.front().handle.id) - 1U;
        }
        if (recovery_anchor != 0 && recovery_anchor < count) {
            const auto lower = recovery_anchor > kRoleIdNeighborhoodRadius ?
                recovery_anchor - kRoleIdNeighborhoodRadius : 0;
            const auto upper = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
                static_cast<std::uint64_t>(count),
                static_cast<std::uint64_t>(recovery_anchor) +
                    kRoleIdNeighborhoodRadius + 1ULL));
            BeginScan(lower, upper, kNeighborhoodObjectBatchSize,
                target_name_id != 0);
        } else {
            BeginScan(0, count, kNamedObjectBatchSize, target_name_id != 0);
        }
        scan_count = count;
        recovery_scan_requested = false;
    }

    void ArmTargetName() {
        if (target_name_id != 0) return;
        AnomalyGenerationHandleV1 handle{};
        if (objects->find_exact(objects->user,
                anomaly::sdk::StringView(kTargetWidgetTemplatePath), &handle).code !=
                ANOMALY_STATUS_V1_OK || handle.id == 0) return;
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        std::string name;
        if (objects->snapshot_by_handle(objects->user, handle, &snapshot).code ==
                ANOMALY_STATUS_V1_OK && ResolveName(*names, snapshot.name_id, name) &&
            (name == kTargetWidgetName || name.starts_with("TextBlock_RoleID_"))) {
            target_name_id = snapshot.name_id;
            template_handle = handle;
        }
    }

    void Track(const AnomalyUe5ObjectSnapshotV1& snapshot) {
        if (snapshot.handle.id == template_handle.id &&
            snapshot.handle.generation == template_handle.generation) return;
        for (const auto& widget : widgets) {
            if (widget.handle.id == snapshot.handle.id &&
                widget.handle.generation == snapshot.handle.generation) return;
        }
        TrackedWidget candidate{snapshot.handle,
            tick + kWidgetStabilizationInterval, 0};
        if (widgets.size() < kMaximumTrackedWidgets) {
            widgets.push_back(candidate);
        } else {
            const auto oldest = std::ranges::min_element(widgets, {},
                [](const TrackedWidget& widget) {
                    return CandidateOrder(widget.handle);
                });
            if (oldest == widgets.end() || CandidateOrder(snapshot.handle) <=
                    CandidateOrder(oldest->handle)) return;
            *oldest = candidate;
        }
        std::ranges::sort(widgets, std::greater{},
            [](const TrackedWidget& widget) {
                return CandidateOrder(widget.handle);
            });
        target_name_id = snapshot.name_id;
        diagnostics.phase = UidDetectorPhase::WidgetPending;
    }

    void Scan() {
        const auto generation = objects->generation(objects->user);
        const auto count = objects->count(objects->user);
        diagnostics.object_generation = generation;
        diagnostics.object_count = count;
        if (generation == 0 || count == 0) {
            diagnostics.current_observation_valid = false;
            diagnostics.last_failure_phase = UidDetectorPhase::Scanning;
            diagnostics.last_failure = "object registry is empty";
            return;
        }
        if (object_generation != generation) {
            diagnostics.current_observation_valid = false;
            recovery_anchor = widgets.empty() ? 0 :
                static_cast<std::uint32_t>(widgets.front().handle.id) - 1U;
            object_generation = generation;
            target_name_id = 0;
            template_handle = {};
            widgets.clear();
            scan_count = count;
            scan_active = false;
            bootstrap_started = false;
            recovery_scan_requested = false;
            next_empty_scan_tick = tick;
        }

        ArmTargetName();
        if (scan_active && count < scan_count) {
            scan_active = false;
            recovery_scan_requested = true;
        }
        if (recovery_scan_requested) {
            BeginRecoveryScan(count);
        } else if (!scan_active && !bootstrap_started) {
            bootstrap_started = true;
            BeginScan(0, count, kNamedObjectBatchSize, target_name_id != 0);
        } else if (!scan_active && count > scan_count) {
            BeginScan(scan_count, count, kIncrementalObjectBatchSize,
                target_name_id != 0);
        } else if (!scan_active && count < scan_count) {
            BeginRecoveryScan(count);
        } else if (!scan_active && widgets.empty() &&
            tick >= next_empty_scan_tick) {
            BeginScan(0, count, kNamedObjectBatchSize, target_name_id != 0);
        }
        if (!scan_active) return;
        if (scan_cursor > count) scan_cursor = count;
        if (scan_start > scan_cursor) scan_start = scan_cursor;
        const auto available = scan_cursor - scan_start;
        const auto begin = scan_cursor - (std::min)(available, scan_batch);
        while (scan_cursor > begin) {
            AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
            if (objects->snapshot_at(objects->user, --scan_cursor, &snapshot).code !=
                    ANOMALY_STATUS_V1_OK || snapshot.name_id == 0) continue;
            if (target_name_id != 0 && snapshot.name_id == target_name_id) {
                Track(snapshot);
                continue;
            }
            if (named_scan) continue;
            std::string name;
            if (ResolveName(*names, snapshot.name_id, name) &&
                (name == kTargetWidgetName || name.starts_with("TextBlock_RoleID_"))) {
                Track(snapshot);
            }
        }
        if (scan_cursor <= scan_start) {
            scan_active = false;
            scan_count = count;
            if (widgets.empty()) {
                next_empty_scan_tick = tick + kEmptyScanRetryInterval;
            }
        }
    }

    bool ResolveWidget(const AnomalyGenerationHandleV1 handle,
        std::uintptr_t& widget) const noexcept {
        widget = 0;
        if (object_registry == 0 || handle.id == 0) return false;
        const std::uint32_t encoded_index = static_cast<std::uint32_t>(handle.id);
        if (encoded_index == 0) return false;
        const std::uint32_t index = encoded_index - 1U;
        const std::uint32_t expected_serial = static_cast<std::uint32_t>(handle.id >> 32U);
        __try {
            const auto chunks = *reinterpret_cast<const std::uintptr_t* const*>(
                object_registry + layout.object_registry_items_offset);
            if (chunks == nullptr) return false;
            const auto chunk = chunks[index / layout.object_chunk_size];
            if (chunk == 0) return false;
            const auto item = chunk + static_cast<std::uintptr_t>(
                index % layout.object_chunk_size) * layout.object_item_stride;
            const auto serial = *reinterpret_cast<const std::uint32_t*>(
                item + layout.object_item_serial_offset);
            const auto object = *reinterpret_cast<const std::uintptr_t*>(item);
            if (serial != expected_serial || object == 0 ||
                *reinterpret_cast<const std::uint32_t*>(
                    object + layout.object_name_offset) != target_name_id) return false;
            const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
            if (vtable == 0 || *reinterpret_cast<const std::uintptr_t*>(
                    vtable + layout.set_text_vtable_offset) != bindings.set_text) return false;
            widget = object;
            return true;
        } __except (1) {
            return false;
        }
    }

    enum class ReadResult { Success, Retry, Stale };

    ReadResult ReadWidget(TrackedWidget& tracked,
        std::optional<AccountIdentity>& result) noexcept {
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (objects->snapshot_by_handle(objects->user, tracked.handle, &snapshot).code !=
                ANOMALY_STATUS_V1_OK || snapshot.name_id != target_name_id) {
            diagnostics.last_failure_phase = UidDetectorPhase::WidgetPending;
            diagnostics.last_failure = "widget handle is stale";
            return ReadResult::Stale;
        }
        std::uintptr_t widget{};
        if (!ResolveWidget(tracked.handle, widget)) {
            diagnostics.last_failure_phase = UidDetectorPhase::WidgetPending;
            diagnostics.last_failure = "widget address validation failed";
            return ReadResult::Stale;
        }
        UnrealString current{};
        const bool readable = ReadWidgetText(
            widget, text_to_string, layout.text_field_offset, current);
        if (!readable) {
            if (current.data != nullptr) free_string(current.data);
            ++diagnostics.failed_reads;
            diagnostics.last_failure_phase = UidDetectorPhase::WidgetPending;
            diagnostics.last_failure = "widget text is temporarily unreadable";
            return ReadResult::Retry;
        }
        const std::wstring_view value(current.data,
            static_cast<std::size_t>(current.count - 1));
        std::size_t end = value.size();
        while (end != 0 && (value[end - 1] < L'0' || value[end - 1] > L'9')) --end;
        std::size_t begin = end;
        while (begin != 0 && value[begin - 1] >= L'0' && value[begin - 1] <= L'9') --begin;
        std::string digits;
        if (end > begin && end - begin <= 20) {
            digits.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index) {
                digits.push_back(static_cast<char>('0' + value[index] - L'0'));
            }
        }
        free_string(current.data);
        result = FromUidText(digits);
        if (!result) {
            ++diagnostics.failed_reads;
            diagnostics.last_failure_phase = UidDetectorPhase::WidgetPending;
            diagnostics.last_failure = "widget text does not contain a valid UID";
            return ReadResult::Retry;
        }
        ++diagnostics.successful_reads;
        diagnostics.last_failure.clear();
        return ReadResult::Success;
    }

    std::optional<AccountIdentity> Update() noexcept {
        ++tick;
        diagnostics.update_tick = tick;
        if (!profile_bound) {
            diagnostics.current_observation_valid = false;
            diagnostics.retry_status = "compatibility profile unavailable";
            diagnostics.next_retry_tick = 0;
            return detected;
        }
        if (!ready && tick >= next_runtime_binding_tick) {
            next_runtime_binding_tick = tick + kRuntimeBindingRetryInterval;
            BindRuntime();
        }
        if (!ready) {
            diagnostics.current_observation_valid = false;
            diagnostics.retry_status = "runtime service retry";
            diagnostics.next_retry_tick = next_runtime_binding_tick;
            return detected;
        }
        diagnostics.retry_status.clear();
        diagnostics.next_retry_tick = 0;
        bool read_attempted{};
        bool read_succeeded{};
        try {
            Scan();
            for (std::size_t index{}; index < widgets.size();) {
                auto& widget = widgets[index];
                if (tick < widget.stable_after_tick ||
                    (widget.last_read_tick != 0 &&
                     tick - widget.last_read_tick < kWidgetVerifyInterval)) {
                    if (index == 0) break;
                    ++index;
                    continue;
                }
                widget.last_read_tick = tick;
                std::optional<AccountIdentity> found;
                read_attempted = true;
                const auto read = ReadWidget(widget, found);
                if (read == ReadResult::Success) {
                    read_succeeded = true;
                    detected = found;
                    diagnostics.detected_uid = found->uid;
                    diagnostics.phase = UidDetectorPhase::Detected;
                    diagnostics.retry_status.clear();
                    diagnostics.next_retry_tick = 0;
                    break;
                }
                if (read == ReadResult::Stale) {
                    recovery_anchor = static_cast<std::uint32_t>(widget.handle.id) - 1U;
                    widgets.erase(widgets.begin() + static_cast<std::ptrdiff_t>(index));
                    scan_active = false;
                    recovery_scan_requested = true;
                    continue;
                }
                diagnostics.retry_status = "widget text retry";
                diagnostics.next_retry_tick = widget.last_read_tick +
                    kWidgetVerifyInterval;
                ++index;
            }
        } catch (...) {
            diagnostics.current_observation_valid = false;
            diagnostics.last_failure_phase = diagnostics.phase;
            diagnostics.last_failure = "unexpected detector exception";
        }
        if (read_attempted) {
            diagnostics.current_observation_valid = read_succeeded;
        }
        diagnostics.scan_cursor = scan_cursor;
        diagnostics.scan_start = scan_start;
        diagnostics.tracked_widgets = widgets.size();
        if (diagnostics.retry_status.empty() && widgets.empty() && !scan_active) {
            diagnostics.retry_status = "periodic object rescan";
            diagnostics.next_retry_tick = next_empty_scan_tick;
        }
        if (!detected && diagnostics.phase != UidDetectorPhase::WaitingForServices &&
            diagnostics.phase != UidDetectorPhase::WaitingForBindings) {
            diagnostics.phase = widgets.empty() ? UidDetectorPhase::Scanning :
                UidDetectorPhase::WidgetPending;
        }
        return detected;
    }

    const AnomalyHostApiV1* host{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5NamesServiceV1* names{};
    native::Layout layout;
    native::Bindings bindings;
    FreeStringFn free_string{};
    TextToStringFn text_to_string{};
    std::uintptr_t object_registry{};
    std::uint64_t tick{};
    std::uint64_t next_runtime_binding_tick{1};
    std::uint64_t object_generation{};
    std::uint32_t target_name_id{};
    AnomalyGenerationHandleV1 template_handle{};
    std::uint32_t scan_count{};
    std::uint32_t scan_start{};
    std::uint32_t scan_cursor{};
    std::uint32_t scan_batch{kIncrementalObjectBatchSize};
    std::uint32_t recovery_anchor{};
    std::uint64_t next_empty_scan_tick{};
    bool scan_active{};
    bool named_scan{};
    bool bootstrap_started{};
    bool recovery_scan_requested{};
    bool profile_bound{};
    bool ready{};
    std::vector<TrackedWidget> widgets;
    std::optional<AccountIdentity> detected;
    UidDetectorDiagnostics diagnostics;
};

UidDetector::UidDetector(const AnomalyHostApiV1* host, const native::Snapshot& native)
    : impl_(std::make_unique<Impl>(host, native)) {}
UidDetector::~UidDetector() = default;
UidDetector::UidDetector(UidDetector&&) noexcept = default;
UidDetector& UidDetector::operator=(UidDetector&&) noexcept = default;
bool UidDetector::available() const noexcept { return impl_ && impl_->ready; }
std::optional<AccountIdentity> UidDetector::Update() noexcept {
    return impl_ ? impl_->Update() : std::nullopt;
}
UidDetectorDiagnostics UidDetector::diagnostics() const {
    return impl_ ? impl_->diagnostics : UidDetectorDiagnostics{};
}

}  // namespace nte::account
