#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/sdk/cpp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Landmark final {
    std::uint64_t sequence{};
    std::uint32_t index{};
    std::uint32_t flags{};
    std::uint32_t point_type{};
    std::int32_t floor{};
    std::array<double, 3> world_position{};
    std::array<double, 3> destination{};
    std::string teleport_id;
    std::string world;
};

struct PendingTeleport final {
    std::uint64_t sequence{};
    std::uint32_t index{};
    std::uint32_t mode{};
    bool queued{};
};

struct Context final {
    const AnomalyHostApiV1* host{};
    const AnomalyNteMapLandmarksServiceV1* landmarks_service{};
    std::atomic_bool developer_mode{};
    std::mutex mutex;
    std::vector<Landmark> landmarks;
    std::uint64_t catalog_sequence{};
    std::uint64_t selected_sequence{};
    std::uint32_t selected_index{};
    std::uint32_t transfer_mode{};
    PendingTeleport pending{};
    std::uint32_t result_code{ANOMALY_STATUS_V1_UNAVAILABLE};
    std::string result_message{"Waiting for map landmark catalog"};
    int window_open{1};
};

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host, const std::string_view id) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->query_service == nullptr) {
        return nullptr;
    }
    const void* value{};
    if (host->query_service(host->host_context, anomaly::sdk::StringView(id), 1, &value).code !=
            ANOMALY_STATUS_V1_OK || value == nullptr) {
        return nullptr;
    }
    const auto* service = static_cast<const Service*>(value);
    constexpr std::size_t prefix = offsetof(Service, user) + sizeof(void*);
    return service->struct_size >= prefix && service->service_version >= 1 ? service : nullptr;
}

bool LandmarksReady(const AnomalyNteMapLandmarksServiceV1* service) noexcept {
    return HasField<AnomalyNteMapLandmarksServiceV1,
               decltype(AnomalyNteMapLandmarksServiceV1::teleport)>(
               service, offsetof(AnomalyNteMapLandmarksServiceV1, teleport)) &&
        service->sequence != nullptr && service->count != nullptr && service->snapshot_at != nullptr &&
        service->teleport != nullptr;
}

void SetResult(Context& context, const std::uint32_t code, std::string message) noexcept {
    try {
        std::scoped_lock lock(context.mutex);
        context.result_code = code;
        context.result_message = std::move(message);
    } catch (...) {
    }
}

std::string PositionText(const std::array<double, 3>& position) {
    std::ostringstream text;
    text.setf(std::ios::fixed, std::ios::floatfield);
    text.precision(1);
    text << '(' << position[0] << ", " << position[1] << ", " << position[2] << ')';
    return text.str();
}

void RefreshCatalog(Context& context) {
    const auto* service = context.landmarks_service;
    if (!LandmarksReady(service)) {
        SetResult(context, ANOMALY_STATUS_V1_UNAVAILABLE, "Map landmark framework service is unavailable");
        return;
    }
    const std::uint64_t sequence = service->sequence(service->user);
    if (sequence == 0) return;
    {
        std::scoped_lock lock(context.mutex);
        if (context.catalog_sequence == sequence) return;
    }

    const std::uint32_t count = service->count(service->user);
    if (count > 4096) {
        SetResult(context, ANOMALY_STATUS_V1_FAILED, "Map landmark catalog exceeds the supported size");
        return;
    }
    std::vector<Landmark> snapshot;
    snapshot.reserve(count);
    for (std::uint32_t index{}; index < count; ++index) {
        AnomalyNteMapLandmarkSnapshotV1 value{sizeof(value)};
        if (service->snapshot_at(service->user, index, &value).code != ANOMALY_STATUS_V1_OK ||
            value.sequence != sequence ||
            (value.flags & ANOMALY_NTE_MAP_LANDMARK_V1_VALID) == 0) {
            SetResult(context, ANOMALY_STATUS_V1_CONFLICT,
                "Map landmark catalog changed while it was being read");
            return;
        }
        Landmark landmark;
        landmark.sequence = value.sequence;
        landmark.index = index;
        landmark.flags = value.flags;
        landmark.point_type = value.point_type;
        landmark.floor = value.floor;
        std::copy_n(value.world_position, landmark.world_position.size(),
            landmark.world_position.begin());
        std::copy_n(value.destination, landmark.destination.size(), landmark.destination.begin());
        landmark.teleport_id = value.teleport_id;
        landmark.world = value.world;
        snapshot.push_back(std::move(landmark));
    }
    if (service->sequence(service->user) != sequence) {
        SetResult(context, ANOMALY_STATUS_V1_CONFLICT,
            "Map landmark catalog changed while it was being read");
        return;
    }
    std::scoped_lock lock(context.mutex);
    context.catalog_sequence = sequence;
    context.landmarks = std::move(snapshot);
    if (context.selected_sequence != sequence) {
        context.selected_sequence = 0;
        context.selected_index = 0;
    }
    context.result_code = ANOMALY_STATUS_V1_OK;
    context.result_message = "Map landmark catalog refreshed";
}

void Update(Context& context) {
    RefreshCatalog(context);
    PendingTeleport pending;
    {
        std::scoped_lock lock(context.mutex);
        if (context.pending.queued) {
            pending = context.pending;
            context.pending = {};
        }
    }
    if (!pending.queued) return;
    if (!context.developer_mode.load(std::memory_order_acquire)) {
        SetResult(context, ANOMALY_STATUS_V1_PERMISSION_DENIED, "Developer mode is disabled");
        return;
    }
    const auto* service = context.landmarks_service;
    if (!LandmarksReady(service)) {
        SetResult(context, ANOMALY_STATUS_V1_UNAVAILABLE, "Map landmark framework service is unavailable");
        return;
    }
    AnomalyNteMapLandmarkTeleportRequestV1 request{sizeof(request)};
    request.mode = pending.mode;
    request.sequence = pending.sequence;
    request.index = pending.index;
    const AnomalyStatusV1 status = service->teleport(service->user, &request);
    SetResult(context, status.code, status.code == ANOMALY_STATUS_V1_OK
        ? "Map landmark transfer invoked"
        : "Map landmark transfer was rejected");
}

void Draw(Context& context, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr || !HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_window)>(
            ui, offsetof(AnomalyUiServiceV1, begin_window)) || ui->begin_window == nullptr ||
        !HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_window)>(
            ui, offsetof(AnomalyUiServiceV1, end_window)) || ui->end_window == nullptr) {
        return;
    }
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::developer_mode_enabled)>(
            ui, offsetof(AnomalyUiServiceV1, developer_mode_enabled)) &&
        ui->developer_mode_enabled != nullptr) {
        context.developer_mode.store(
            ui->developer_mode_enabled(ui->user) != 0, std::memory_order_release);
    }
    if (!ui->begin_window(ui->user, anomaly::sdk::StringView("Teleport landmarks probe"),
            &context.window_open, 0)) {
        ui->end_window(ui->user);
        return;
    }

    const bool can_button = HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button)>(
        ui, offsetof(AnomalyUiServiceV1, button)) && ui->button != nullptr;
    const bool can_text = HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::text)>(
        ui, offsetof(AnomalyUiServiceV1, text)) && ui->text != nullptr;
    const bool can_table =
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_table)>(
            ui, offsetof(AnomalyUiServiceV1, begin_table)) && ui->begin_table != nullptr &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::table_next_row)>(
            ui, offsetof(AnomalyUiServiceV1, table_next_row)) && ui->table_next_row != nullptr &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::table_next_column)>(
            ui, offsetof(AnomalyUiServiceV1, table_next_column)) &&
            ui->table_next_column != nullptr &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_table)>(
            ui, offsetof(AnomalyUiServiceV1, end_table)) && ui->end_table != nullptr;

    if (can_button && ui->button(ui->user, anomaly::sdk::StringView("Normal"), 0, 0)) {
        std::scoped_lock lock(context.mutex);
        context.transfer_mode = ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_NORMAL;
    }
    if (can_button && ui->button(ui->user, anomaly::sdk::StringView("SellingIndulgences"), 0, 0)) {
        std::scoped_lock lock(context.mutex);
        context.transfer_mode = ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_SELLING_INDULGENCES;
    }

    std::vector<Landmark> landmarks;
    std::uint64_t selected_sequence{};
    std::uint32_t selected_index{};
    std::uint32_t transfer_mode{};
    std::uint32_t status_code{};
    std::string status_message;
    {
        std::scoped_lock lock(context.mutex);
        landmarks = context.landmarks;
        selected_sequence = context.selected_sequence;
        selected_index = context.selected_index;
        transfer_mode = context.transfer_mode;
        status_code = context.result_code;
        status_message = context.result_message;
    }
    const auto select = [&](const Landmark& landmark) {
        selected_sequence = landmark.sequence;
        selected_index = landmark.index;
        std::scoped_lock lock(context.mutex);
        context.selected_sequence = landmark.sequence;
        context.selected_index = landmark.index;
    };
    if (can_button && can_text && can_table && ui->begin_table(ui->user,
            anomaly::sdk::StringView("teleport-landmarks"), 3,
            ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT, 0.0F, 0.0F)) {
        ui->table_next_row(ui->user);
        static_cast<void>(ui->table_next_column(ui->user));
        ui->text(ui->user, anomaly::sdk::StringView("ID"));
        static_cast<void>(ui->table_next_column(ui->user));
        ui->text(ui->user, anomaly::sdk::StringView("World"));
        static_cast<void>(ui->table_next_column(ui->user));
        ui->text(ui->user, anomaly::sdk::StringView("Coordinates"));
        for (const Landmark& landmark : landmarks) {
            ui->table_next_row(ui->user);
            static_cast<void>(ui->table_next_column(ui->user));
            if (ui->button(ui->user, anomaly::sdk::StringView(landmark.teleport_id), 0.0F, 0.0F)) {
                select(landmark);
            }
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, anomaly::sdk::StringView(
                landmark.world.empty() ? "<unknown>" : landmark.world));
            static_cast<void>(ui->table_next_column(ui->user));
            ui->text(ui->user, anomaly::sdk::StringView(PositionText(landmark.world_position)));
        }
        ui->end_table(ui->user);
    } else if (can_button) {
        for (const Landmark& landmark : landmarks) {
            if (ui->button(ui->user, anomaly::sdk::StringView(landmark.teleport_id), 0.0F, 0.0F)) {
                select(landmark);
            }
            if (can_text) {
                ui->text(ui->user, anomaly::sdk::StringView(
                    std::string("World: ") +
                        (landmark.world.empty() ? "<unknown>" : landmark.world)));
                ui->text(ui->user, anomaly::sdk::StringView(
                    std::string("Coordinates: ") + PositionText(landmark.world_position)));
            }
        }
    }

    const auto selected = std::find_if(landmarks.begin(), landmarks.end(),
        [&](const Landmark& landmark) {
            return landmark.sequence == selected_sequence && landmark.index == selected_index;
        });
    if (can_text) {
        ui->text(ui->user, anomaly::sdk::StringView(
            std::string("Mode: ") + (transfer_mode == ANOMALY_NTE_MAP_LANDMARK_TRANSFER_V1_NORMAL
                ? "Normal" : "SellingIndulgences")));
        if (selected != landmarks.end()) {
            ui->text(ui->user, anomaly::sdk::StringView(
                std::string("Selected world: ") +
                    (selected->world.empty() ? "<unknown>" : selected->world)));
            ui->text(ui->user, anomaly::sdk::StringView(
                std::string("Selected coordinates: ") + PositionText(selected->world_position)));
            ui->text(ui->user, anomaly::sdk::StringView(
                std::string("Floor: ") + std::to_string(selected->floor)));
            ui->text(ui->user, anomaly::sdk::StringView(
                std::string("Point type: ") + std::to_string(selected->point_type)));
            if ((selected->flags & ANOMALY_NTE_MAP_LANDMARK_V1_DESTINATION_OVERRIDDEN) != 0) {
                ui->text(ui->user, anomaly::sdk::StringView(
                    std::string("Override destination: ") + PositionText(selected->destination)));
            }
        }
    }
    if (can_button && selected != landmarks.end() &&
        ui->button(ui->user, anomaly::sdk::StringView("Teleport"), 0, 0)) {
        std::scoped_lock lock(context.mutex);
        context.pending = {selected->sequence, selected->index, transfer_mode, true};
    }
    if (can_text) {
        ui->text(ui->user, anomaly::sdk::StringView(
            std::string("Status: ") + std::to_string(status_code) + " " + status_message));
        ui->text(ui->user, anomaly::sdk::StringView(
            std::string("Landmarks: ") + std::to_string(landmarks.size())));
    }
    ui->end_window(ui->user);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    auto* context = new Context;
    context->host = host;
    context->landmarks_service = Query<AnomalyNteMapLandmarksServiceV1>(
        host, ANOMALY_NTE_MAP_LANDMARKS_SERVICE_V1_ID);
    *plugin_context = context;
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return {ANOMALY_STATUS_V1_OK, 0, {}}; }
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return {ANOMALY_STATUS_V1_OK, 0, {}}; }
void ANOMALY_CALL Unload(void* value) { delete static_cast<Context*>(value); }

void ANOMALY_CALL UpdateThunk(void* value, double) {
    if (value == nullptr) return;
    try {
        Update(*static_cast<Context*>(value));
    } catch (...) {
        SetResult(*static_cast<Context*>(value), ANOMALY_STATUS_V1_FAILED,
            "Developer probe update raised an exception");
    }
}

void ANOMALY_CALL DrawThunk(void* value, const AnomalyUiServiceV1* ui) {
    if (value == nullptr) return;
    try {
        Draw(*static_cast<Context*>(value), ui);
    } catch (...) {
        SetResult(*static_cast<Context*>(value), ANOMALY_STATUS_V1_FAILED,
            "Developer probe UI raised an exception");
    }
}

}  // namespace

extern "C" ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.teleport-landmarks-probe"),
        anomaly::sdk::StringView("Teleport Landmarks Probe"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("0.1.0"), Load, Start, Stop, Unload, UpdateThunk, DrawThunk};
    return {ANOMALY_STATUS_V1_OK, 0, {}};
}
