#include "anomaly/sdk/cpp.hpp"
#include "plugins/common/localization.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    std::uint64_t session_cursor{};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

constexpr AnomalyStatusV1 StatusCode(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
struct ServiceQuery {
    const Service* service{};
    AnomalyStatusV1 status{StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};

    [[nodiscard]] explicit operator bool() const noexcept { return service != nullptr; }
};

template <typename Service>
ServiceQuery<Service> QueryService(
    const AnomalyHostApiV1* host, const std::string_view id,
    const std::uint32_t minimum_version) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->api_major != ANOMALY_PLUGIN_API_V1_MAJOR || host->query_service == nullptr) {
        return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};
    }

    const void* table{};
    const AnomalyStatusV1 status = host->query_service(
        host->host_context, anomaly::sdk::StringView(id), minimum_version, &table);
    if (status.code != ANOMALY_STATUS_V1_OK) return {nullptr, status};
    if (table == nullptr) return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};

    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t kServicePrefixSize = offsetof(Service, user) + sizeof(void*);
    if (service->struct_size < kServicePrefixSize || service->service_version < minimum_version) {
        return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};
    }
    return {service, StatusCode(ANOMALY_STATUS_V1_OK)};
}

const char* StatusName(const std::uint32_t code) noexcept {
    switch (code) {
    case ANOMALY_STATUS_V1_OK: return "OK";
    case ANOMALY_STATUS_V1_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "UNAVAILABLE";
    case ANOMALY_STATUS_V1_NOT_FOUND: return "NOT_FOUND";
    case ANOMALY_STATUS_V1_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case ANOMALY_STATUS_V1_FAILED: return "FAILED";
    case ANOMALY_STATUS_V1_TIMEOUT: return "TIMEOUT";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case ANOMALY_STATUS_V1_CONFLICT: return "CONFLICT";
    case ANOMALY_STATUS_V1_CANCELLED: return "CANCELLED";
    default: return "UNKNOWN_STATUS";
    }
}

const char* SnapshotState(const std::uint32_t flags) noexcept {
    if ((flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) return "UNAVAILABLE";
    const bool stale = (flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) != 0;
    const bool partial = (flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) != 0;
    if (stale && partial) return "STALE|PARTIAL";
    if (stale) return "STALE";
    if (partial) return "PARTIAL";
    return "VALID";
}

const char* SessionStateName(const std::uint32_t state) noexcept {
    switch (state) {
    case ANOMALY_NTE_SESSION_V1_WORLD_READY: return "WORLD_READY";
    case ANOMALY_NTE_SESSION_V1_LOADING: return "LOADING";
    default: return "UNKNOWN";
    }
}

std::string SessionEventName(const std::uint32_t kind) {
    switch (kind) {
    case ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY:
        return g_context.localizer.Text("session.event.world_ready", "World ready");
    case ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED:
        return g_context.localizer.Text("session.event.world_changed", "World changed");
    case ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE:
        return g_context.localizer.Text("session.event.world_unavailable", "World unavailable");
    default:
        return g_context.localizer.Text("session.event.unknown", "Unknown session event");
    }
}

bool HasUiFrameFunctions(const AnomalyUiServiceV1* ui) noexcept {
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::begin_window)>(
               ui, offsetof(AnomalyUiServiceV1, begin_window)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_window)>(
            ui, offsetof(AnomalyUiServiceV1, end_window)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::text)>(
            ui, offsetof(AnomalyUiServiceV1, text)) &&
        ui->begin_window != nullptr && ui->end_window != nullptr && ui->text != nullptr;
}

void DrawText(const AnomalyUiServiceV1* ui, const std::string_view text) {
    if (!HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::text)>(
            ui, offsetof(AnomalyUiServiceV1, text)) ||
        ui->text == nullptr) {
        return;
    }
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

void DrawUnavailable(
    const AnomalyUiServiceV1* ui, const std::string_view label_key,
    const std::string_view english_label) {
    const std::string label = g_context.localizer.Text(label_key, english_label);
    const std::array arguments{std::string_view(label)};
    DrawText(ui, g_context.localizer.Format(
        "status.unavailable", "{0}: UNAVAILABLE", arguments));
}

void DrawStatus(
    const AnomalyUiServiceV1* ui, const std::string_view label_key,
    const std::string_view english_label, const AnomalyStatusV1 status) {
    const std::string label = g_context.localizer.Text(label_key, english_label);
    const std::string_view status_name = StatusName(status.code);
    const std::string_view message = status.message.data == nullptr
        ? std::string_view{}
        : std::string_view(status.message.data, status.message.size);
    if (message.empty()) {
        const std::array arguments{std::string_view(label), status_name};
        DrawText(ui, g_context.localizer.Format(
            "status.result", "{0}: {1}", arguments));
    } else {
        const std::size_t displayed_size = message.size() < 112 ? message.size() : 112;
        const std::array arguments{
            std::string_view(label), status_name, message.substr(0, displayed_size)};
        DrawText(ui, g_context.localizer.Format(
            "status.result.detail", "{0}: {1} - {2}", arguments));
    }
}

bool DrawSnapshotState(
    const AnomalyUiServiceV1* ui, const std::string_view label_key,
    const std::string_view english_label, const std::uint32_t flags,
    const char** state) {
    *state = SnapshotState(flags);
    const std::string label = g_context.localizer.Text(label_key, english_label);
    const std::array arguments{std::string_view(label), std::string_view(*state)};
    DrawText(ui, g_context.localizer.Format(
        "status.result", "{0}: {1}", arguments));
    return (flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    const auto ui = QueryService<AnomalyUiServiceV1>(
        host,
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui) return ui.status;
    if (!HasUiFrameFunctions(ui.service)) return StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE);
    g_context = {host, anomaly::plugins::Localizer(host), 0};
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    static_cast<Context*>(context)->session_cursor = 0;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return anomaly::sdk::Ok(); }

void ANOMALY_CALL Unload(void*) { g_context = {}; }

void DrawSessionSnapshot(
    const AnomalyNteSessionServiceV1* session, const AnomalyUiServiceV1* ui) {
    if (!HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::snapshot)>(
            session, offsetof(AnomalyNteSessionServiceV1, snapshot)) ||
        session->snapshot == nullptr) {
        DrawUnavailable(ui, "session.snapshot", "Session snapshot");
        return;
    }

    AnomalyNteSessionSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status = session->snapshot(session->user, &snapshot);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        DrawStatus(ui, "session.snapshot", "Session snapshot", status);
        return;
    }

    const std::string generation = std::to_string(snapshot.world.generation);
    const std::string sequence = std::to_string(snapshot.sequence);
    const std::array arguments{
        std::string_view(SessionStateName(snapshot.state)), std::string_view(generation),
        std::string_view(sequence)};
    DrawText(ui, g_context.localizer.Format(
        "session.summary", "Session {0} / world generation {1} / change {2}", arguments));
}

void DrawSessionEvents(
    const AnomalyNteSessionServiceV1* session, const AnomalyUiServiceV1* ui) {
    if (!HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::next_event)>(
            session, offsetof(AnomalyNteSessionServiceV1, next_event)) ||
        session->next_event == nullptr) {
        DrawUnavailable(ui, "session.event_stream", "Session event stream");
        return;
    }

    AnomalyNteSessionEventV1 event{sizeof(event)};
    const AnomalyStatusV1 status = session->next_event(
        session->user, g_context.session_cursor, &event);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        g_context.session_cursor = event.sequence;
        const std::string sequence = std::to_string(event.sequence);
        const std::string name = SessionEventName(event.kind);
        const std::string tick = std::to_string(event.tick_sequence);
        const std::array arguments{
            std::string_view(sequence), std::string_view(name), std::string_view(tick)};
        DrawText(ui, g_context.localizer.Format(
            "session.event", "Session event #{0}: {1} at tick {2}", arguments));
        return;
    }
    if (status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
        DrawStatus(ui, "session.event_stream", "Session event stream", status);
        return;
    }
    if (!HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::latest_event_sequence)>(
            session, offsetof(AnomalyNteSessionServiceV1, latest_event_sequence)) ||
        session->latest_event_sequence == nullptr) {
        DrawUnavailable(
            ui, "session.event_resynchronization", "Session event resynchronization");
        return;
    }

    const std::uint64_t latest = session->latest_event_sequence(session->user);
    if (latest == g_context.session_cursor) return;

    g_context.session_cursor = latest;
    const std::string sequence = std::to_string(latest);
    const std::array arguments{std::string_view(sequence)};
    DrawText(ui, g_context.localizer.Format(
        "session.event_resynchronized",
        "Session event cursor: STALE; resynchronized to #{0}", arguments));
}

void DrawSession(const AnomalyHostApiV1* host, const AnomalyUiServiceV1* ui) {
    const auto session = QueryService<AnomalyNteSessionServiceV1>(
        host,
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    if (!session) {
        DrawStatus(ui, "session.service", "Session service", session.status);
        return;
    }

    DrawSessionSnapshot(session.service, ui);
    DrawSessionEvents(session.service, ui);
}

void DrawPlayerSnapshot(
    const AnomalyNtePlayerServiceV1* player, const AnomalyUiServiceV1* ui) {
    if (!HasField<AnomalyNtePlayerServiceV1,
            decltype(AnomalyNtePlayerServiceV1::snapshot)>(
            player, offsetof(AnomalyNtePlayerServiceV1, snapshot)) ||
        player->snapshot == nullptr) {
        DrawUnavailable(ui, "player.snapshot", "Player snapshot");
        return;
    }

    AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status = player->snapshot(player->user, &snapshot);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        DrawStatus(ui, "player.snapshot", "Player snapshot", status);
        return;
    }

    const char* state{};
    if (!DrawSnapshotState(
            ui, "player.snapshot", "Player snapshot", snapshot.flags, &state)) return;

    char x[32]{}, y[32]{}, z[32]{};
    std::snprintf(x, sizeof(x), "%.3f", snapshot.position[0]);
    std::snprintf(y, sizeof(y), "%.3f", snapshot.position[1]);
    std::snprintf(z, sizeof(z), "%.3f", snapshot.position[2]);
    const std::string generation = std::to_string(snapshot.handle.generation);
    const std::string sequence = std::to_string(snapshot.sequence);
    const std::array arguments{
        std::string_view(state), std::string_view(x), std::string_view(y), std::string_view(z),
        std::string_view(generation), std::string_view(sequence)};
    DrawText(ui, g_context.localizer.Format(
        "player.position",
        "Position {0} {1}, {2}, {3} / generation {4} / sequence {5}", arguments));
}

void DrawCameraSnapshot(
    const AnomalyNtePlayerServiceV1* player, const AnomalyUiServiceV1* ui) {
    if (!HasField<AnomalyNtePlayerServiceV1,
            decltype(AnomalyNtePlayerServiceV1::camera_snapshot)>(
            player, offsetof(AnomalyNtePlayerServiceV1, camera_snapshot)) ||
        player->camera_snapshot == nullptr) {
        DrawUnavailable(ui, "camera.snapshot", "Camera snapshot");
        return;
    }

    AnomalyNteCameraSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status = player->camera_snapshot(player->user, &snapshot);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        DrawStatus(ui, "camera.snapshot", "Camera snapshot", status);
        return;
    }

    const char* state{};
    if (!DrawSnapshotState(
            ui, "camera.snapshot", "Camera snapshot", snapshot.flags, &state)) return;

    char x[32]{}, y[32]{}, z[32]{}, fov[32]{};
    std::snprintf(x, sizeof(x), "%.3f", snapshot.position[0]);
    std::snprintf(y, sizeof(y), "%.3f", snapshot.position[1]);
    std::snprintf(z, sizeof(z), "%.3f", snapshot.position[2]);
    std::snprintf(fov, sizeof(fov), "%.1f", snapshot.horizontal_fov_degrees);
    const std::string world_generation = std::to_string(snapshot.world.generation);
    const std::string player_generation = std::to_string(snapshot.player.generation);
    const std::string sequence = std::to_string(snapshot.sequence);
    const std::array arguments{
        std::string_view(state), std::string_view(x), std::string_view(y), std::string_view(z),
        std::string_view(fov), std::string_view(world_generation),
        std::string_view(player_generation), std::string_view(sequence)};
    DrawText(ui, g_context.localizer.Format(
        "camera.position",
        "Camera {0} {1}, {2}, {3} / FOV {4} / world generation {5} / player generation {6} / sequence {7}",
        arguments));
}

void DrawPlayer(const AnomalyHostApiV1* host, const AnomalyUiServiceV1* ui) {
    const auto player = QueryService<AnomalyNtePlayerServiceV1>(
        host,
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!player) {
        DrawStatus(ui, "player.service", "Player service", player.status);
        return;
    }

    DrawPlayerSnapshot(player.service, ui);
    DrawCameraSnapshot(player.service, ui);
}

void DrawMetrics(const AnomalyHostApiV1* host, const AnomalyUiServiceV1* ui) {
    const auto metrics = QueryService<AnomalyNteMetricsServiceV1>(
        host,
        ANOMALY_NTE_METRICS_SERVICE_V1_ID, ANOMALY_NTE_METRICS_SERVICE_V1_VERSION);
    if (!metrics) {
        DrawStatus(ui, "metrics.snapshot", "Snapshot metrics", metrics.status);
        return;
    }
    if (!HasField<AnomalyNteMetricsServiceV1,
            decltype(AnomalyNteMetricsServiceV1::snapshot)>(
            metrics.service, offsetof(AnomalyNteMetricsServiceV1, snapshot)) ||
        metrics.service->snapshot == nullptr) {
        DrawUnavailable(ui, "metrics.snapshot", "Snapshot metrics");
        return;
    }

    AnomalyNteSnapshotMetricsV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status = metrics.service->snapshot(metrics.service->user, &snapshot);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        DrawStatus(ui, "metrics.snapshot", "Snapshot metrics", status);
        return;
    }
    if ((snapshot.flags & ANOMALY_NTE_METRICS_V1_VALID) == 0) {
        DrawUnavailable(ui, "metrics.snapshot", "Snapshot metrics");
        return;
    }

    const std::array values{
        std::to_string(snapshot.tick_sequence),
        std::to_string(snapshot.latest_snapshot_cost_micros),
        std::to_string(snapshot.player_refresh_count),
        std::to_string(snapshot.player_cache_hit_count),
        std::to_string(snapshot.entity_page_request_count),
        std::to_string(snapshot.entity_page_cache_hit_count)};
    const std::array arguments{
        std::string_view(values[0]), std::string_view(values[1]), std::string_view(values[2]),
        std::string_view(values[3]), std::string_view(values[4]), std::string_view(values[5])};
    DrawText(ui, g_context.localizer.Format(
        "metrics.summary",
        "Snapshot metrics: VALID / tick {0} / latest {1} us / player refresh {2} / player cache hits {3} / entity pages {4} / page cache hits {5}",
        arguments));
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (g_context.host == nullptr) return;
    if (!HasUiFrameFunctions(ui)) {
        const auto queried_ui = QueryService<AnomalyUiServiceV1>(
            g_context.host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
        ui = queried_ui.service;
    }
    if (!HasUiFrameFunctions(ui)) return;
    int open = 1;
    const std::string title = g_context.localizer.Label(
        "window.title", "Coordinate Display", "coordinate-display");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;
    DrawSession(g_context.host, ui);
    DrawPlayer(g_context.host, ui);
    DrawMetrics(g_context.host, ui);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.nte-position"),
        anomaly::sdk::StringView("Coordinate Display"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.1.0"), Load, Start, Stop, Unload, nullptr, Draw};
    return anomaly::sdk::Ok();
}
