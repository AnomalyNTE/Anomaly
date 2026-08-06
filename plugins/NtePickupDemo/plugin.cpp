#include "anomaly/sdk/cpp.hpp"
#include "plugins/common/localization.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr double kDefaultRadius = 600.0;
constexpr double kMinimumRadius = 50.0;
constexpr double kMaximumRadius = 5000.0;
constexpr std::uint32_t kDefaultMaximumItems = 32;
constexpr std::uint32_t kMaximumItemsPerRequest = 128;

struct Context final {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyNtePickupServiceV1* pickup{};
    double radius{kDefaultRadius};
    std::uint32_t maximum_items{kDefaultMaximumItems};
    bool queued{};
    bool started{};
    std::string last_error;
    std::mutex mutex;
};

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

AnomalyStatusV1 Status(
    const std::uint32_t code,
    const std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

template <typename Service>
const Service* Query(
    const AnomalyHostApiV1* host,
    const char* id,
    const std::uint32_t version) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->query_service == nullptr) {
        return nullptr;
    }
    const void* table{};
    if (host->query_service(
            host->host_context, anomaly::sdk::StringView(id), version, &table).code !=
            ANOMALY_STATUS_V1_OK ||
        table == nullptr) {
        return nullptr;
    }
    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t prefix = offsetof(Service, user) + sizeof(void*);
    return service->struct_size >= prefix && service->service_version >= version
        ? service
        : nullptr;
}

bool UiReady(const AnomalyUiServiceV1* service) noexcept {
    return HasField<AnomalyUiServiceV1,
               decltype(AnomalyUiServiceV1::button_enabled)>(
               service, offsetof(AnomalyUiServiceV1, button_enabled)) &&
        service->begin_window != nullptr && service->end_window != nullptr &&
        service->text != nullptr && service->input_double != nullptr &&
        service->input_uint32 != nullptr && service->button_enabled != nullptr;
}

bool PickupReady(const AnomalyNtePickupServiceV1* service) noexcept {
    return HasField<AnomalyNtePickupServiceV1,
               decltype(AnomalyNtePickupServiceV1::snapshot)>(
               service, offsetof(AnomalyNtePickupServiceV1, snapshot)) &&
        service->request_nearby != nullptr && service->snapshot != nullptr;
}

void Log(
    const Context& context,
    const std::uint32_t level,
    const std::string_view message) noexcept {
    if (context.core != nullptr && context.core->log != nullptr) {
        context.core->log(
            context.core->user, level, anomaly::sdk::StringView(message));
    }
}

const char* StatusName(const std::uint32_t status) noexcept {
    switch (status) {
    case ANOMALY_STATUS_V1_OK: return "OK";
    case ANOMALY_STATUS_V1_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "UNAVAILABLE";
    case ANOMALY_STATUS_V1_NOT_FOUND: return "NOT_FOUND";
    case ANOMALY_STATUS_V1_FAILED: return "FAILED";
    case ANOMALY_STATUS_V1_CONFLICT: return "CONFLICT";
    default: return "UNKNOWN_STATUS";
    }
}

const char* StateName(const std::uint32_t state) noexcept {
    switch (state) {
    case ANOMALY_NTE_PICKUP_V1_QUEUED: return "QUEUED";
    case ANOMALY_NTE_PICKUP_V1_CHECKING: return "CHECKING";
    case ANOMALY_NTE_PICKUP_V1_COMPLETE: return "COMPLETE";
    default: return "IDLE";
    }
}

void DrawText(const AnomalyUiServiceV1& ui, const std::string_view text) {
    ui.text(ui.user, anomaly::sdk::StringView(text));
}

void DrawSnapshot(
    Context& context,
    const AnomalyUiServiceV1& ui,
    const AnomalyStatusV1 status,
    const std::string_view error,
    const AnomalyNtePickupSnapshotV1& snapshot) {
    if (status.code != ANOMALY_STATUS_V1_OK) {
        const std::array arguments{
            std::string_view(StatusName(status.code)), error};
        const std::string text = context.localizer.Format(
            "status.error", "Pickup: {0} - {1}",
            arguments);
        DrawText(ui, text);
        return;
    }
    const std::string state = StateName(snapshot.state);
    const std::string result = StatusName(snapshot.status);
    const std::string nearby = std::to_string(snapshot.nearby);
    const std::string triggered = std::to_string(snapshot.triggered);
    const std::string confirmed = std::to_string(snapshot.confirmed);
    const std::string checking = std::to_string(snapshot.checking);
    const std::string unconfirmed = std::to_string(snapshot.unconfirmed);
    const std::string skipped = std::to_string(snapshot.skipped);
    const std::array arguments{
        std::string_view(state), std::string_view(result), std::string_view(nearby),
        std::string_view(triggered), std::string_view(confirmed), std::string_view(checking),
        std::string_view(unconfirmed), std::string_view(skipped)};
    DrawText(ui, context.localizer.Format(
        "status.result",
        "Pickup: {0} / result {1} / nearby {2} / triggered {3} / confirmed {4} / checking {5} / unconfirmed {6} / skipped {7}",
        arguments));
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host,
    void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host is invalid");
    }
    *plugin_context = nullptr;
    auto* context = new (std::nothrow) Context();
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED, "allocation failed");
    context->host = host;
    context->localizer = anomaly::plugins::Localizer(host);
    context->core = Query<AnomalyCoreServiceV1>(
        host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    context->ui = Query<AnomalyUiServiceV1>(
        host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    context->pickup = Query<AnomalyNtePickupServiceV1>(
        host, ANOMALY_NTE_PICKUP_SERVICE_V1_ID,
        ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION);
    if (!UiReady(context->ui) || !PickupReady(context->pickup)) {
        delete context;
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "required services are unavailable");
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* user) {
    auto* context = static_cast<Context*>(user);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(context->mutex);
    if (context->started) return Status(ANOMALY_STATUS_V1_CONFLICT, "plugin is already started");
    context->started = true;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* user, std::uint32_t) {
    auto* context = static_cast<Context*>(user);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    std::scoped_lock lock(context->mutex);
    context->started = false;
    context->queued = false;
    context->last_error.clear();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* user) {
    delete static_cast<Context*>(user);
}

void ANOMALY_CALL Update(void* user, double) {
    auto* context = static_cast<Context*>(user);
    if (context == nullptr) return;
    AnomalyNtePickupRequestV1 request{};
    {
        std::scoped_lock lock(context->mutex);
        if (!context->started || !context->queued) return;
        request = {
            sizeof(request), 0, context->radius, context->maximum_items, 0};
        context->queued = false;
    }
    const AnomalyStatusV1 status = context->pickup->request_nearby(
        context->pickup->user, &request);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        std::string error = StatusName(status.code);
        if (status.message.data != nullptr && status.message.size != 0) {
            error.append(": ");
            error.append(status.message.data, status.message.size);
        }
        {
            std::scoped_lock lock(context->mutex);
            context->last_error = std::move(error);
        }
        Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "NTE pickup request was rejected by the Host");
    }
}

void ANOMALY_CALL Draw(void* user, const AnomalyUiServiceV1* supplied_ui) {
    auto* context = static_cast<Context*>(user);
    if (context == nullptr) return;
    const AnomalyUiServiceV1* ui = UiReady(supplied_ui) ? supplied_ui : context->ui;
    if (!UiReady(ui)) return;
    AnomalyNtePickupSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 snapshot_status = context->pickup->snapshot(
        context->pickup->user, &snapshot);
    const std::string snapshot_error =
        snapshot_status.message.data == nullptr || snapshot_status.message.size == 0
        ? "service snapshot unavailable"
        : std::string(snapshot_status.message.data, snapshot_status.message.size);
    double radius{};
    std::uint32_t maximum_items{};
    bool queued{};
    std::string last_error;
    {
        std::scoped_lock lock(context->mutex);
        radius = context->radius;
        maximum_items = context->maximum_items;
        queued = context->queued;
        last_error = context->last_error;
    }
    int open = 1;
    const std::string title = context->localizer.Label(
        "window.title", "Nearby Pickup", "nearby-pickup");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;
    const std::string radius_label = context->localizer.Label(
        "setting.radius", "Radius", "pickup-radius");
    static_cast<void>(ui->input_double(
        ui->user, anomaly::sdk::StringView(radius_label), &radius,
        kMinimumRadius, kMaximumRadius));
    const std::string maximum_label = context->localizer.Label(
        "setting.maximum", "Maximum items", "pickup-maximum");
    static_cast<void>(ui->input_uint32(
        ui->user, anomaly::sdk::StringView(maximum_label), &maximum_items,
        1, kMaximumItemsPerRequest));
    const bool valid = std::isfinite(radius) && radius >= kMinimumRadius &&
        radius <= kMaximumRadius && maximum_items >= 1 &&
        maximum_items <= kMaximumItemsPerRequest;
    {
        std::scoped_lock lock(context->mutex);
        context->radius = radius;
        context->maximum_items = maximum_items;
    }
    const std::string pickup_label = context->localizer.Label(
        "action.pickup", "Pick up nearby items", "pickup-nearby");
    const bool host_busy = snapshot_status.code == ANOMALY_STATUS_V1_OK &&
        (snapshot.state == ANOMALY_NTE_PICKUP_V1_QUEUED ||
         snapshot.state == ANOMALY_NTE_PICKUP_V1_CHECKING);
    if (ui->button_enabled(
            ui->user, anomaly::sdk::StringView(pickup_label), 0.0F, 0.0F,
            valid && snapshot_status.code == ANOMALY_STATUS_V1_OK &&
                !queued && !host_busy ? 1 : 0) != 0) {
        std::scoped_lock lock(context->mutex);
        context->queued = true;
        context->last_error.clear();
        last_error.clear();
    }
    if (!last_error.empty()) {
        const std::array arguments{
            std::string_view("REQUEST"), std::string_view(last_error)};
        DrawText(*ui, context->localizer.Format(
            "status.error", "Pickup: {0} - {1}", arguments));
    }
    DrawSnapshot(
        *context, *ui, snapshot_status, snapshot_error, snapshot);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "descriptor is invalid");
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.local.nte-pickup-demo"),
        anomaly::sdk::StringView("Nearby Pickup"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.1.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
