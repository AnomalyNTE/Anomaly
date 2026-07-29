#include "anomaly/sdk/cpp.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kPageCapacity = 12;
constexpr std::uint32_t kKnownSnapshotFlags =
    ANOMALY_NTE_SNAPSHOT_V1_VALID |
    ANOMALY_NTE_SNAPSHOT_V1_STALE |
    ANOMALY_NTE_SNAPSHOT_V1_PARTIAL;

struct Context {
    const AnomalyHostApiV1* host{};
    std::uint64_t session_cursor{};
    std::uint64_t entity_generation{};
    std::uint32_t entity_total_matches{};
    std::vector<std::uint32_t> page_offsets;
    std::size_t page_index{};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
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
    switch (flags & kKnownSnapshotFlags) {
    case ANOMALY_NTE_SNAPSHOT_V1_VALID: return "VALID";
    case ANOMALY_NTE_SNAPSHOT_V1_STALE: return "STALE";
    case ANOMALY_NTE_SNAPSHOT_V1_PARTIAL: return "PARTIAL";
    case ANOMALY_NTE_SNAPSHOT_V1_VALID | ANOMALY_NTE_SNAPSHOT_V1_STALE:
        return "VALID|STALE";
    case ANOMALY_NTE_SNAPSHOT_V1_VALID | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL:
        return "VALID|PARTIAL";
    case ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL:
        return "STALE|PARTIAL";
    case ANOMALY_NTE_SNAPSHOT_V1_VALID | ANOMALY_NTE_SNAPSHOT_V1_STALE |
        ANOMALY_NTE_SNAPSHOT_V1_PARTIAL:
        return "VALID|STALE|PARTIAL";
    default: return "UNAVAILABLE";
    }
}

bool IsCurrentSnapshot(const std::uint32_t flags) noexcept {
    return (flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
        (flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) == 0;
}

void DrawUnavailable(const AnomalyUiServiceV1* ui, const char* label) {
    char text[192]{};
    std::snprintf(text, sizeof(text), "%s: UNAVAILABLE", label);
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

void DrawStatus(const AnomalyUiServiceV1* ui, const char* label, const AnomalyStatusV1 status) {
    char text[224]{};
    const std::string_view message = status.message.data == nullptr
        ? std::string_view{}
        : std::string_view(status.message.data, status.message.size);
    if (message.empty()) {
        std::snprintf(text, sizeof(text), "%s: %s", label, StatusName(status.code));
    } else {
        const std::size_t displayed_size = (std::min)(message.size(), std::size_t{128});
        std::snprintf(
            text, sizeof(text), "%s: %s - %.*s", label, StatusName(status.code),
            static_cast<int>(displayed_size), message.data());
    }
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

void DrawSnapshotState(
    const AnomalyUiServiceV1* ui, const char* label, const std::uint32_t flags) {
    char text[192]{};
    std::snprintf(text, sizeof(text), "%s: %s", label, SnapshotState(flags));
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

bool EnsureFirstPage(Context& context) noexcept {
    if (!context.page_offsets.empty()) return true;
    try {
        context.page_offsets.push_back(0);
    } catch (...) {
        return false;
    }
    context.page_index = 0;
    return true;
}

void NormalizePageIndex(Context& context) noexcept {
    if (!context.page_offsets.empty() && context.page_index >= context.page_offsets.size()) {
        context.page_index = context.page_offsets.size() - 1;
    }
}

bool ResetEntityPagination(Context& context, const std::uint64_t generation) noexcept {
    context.entity_generation = generation;
    context.entity_total_matches = 0;
    if (!EnsureFirstPage(context)) return false;
    context.page_offsets.resize(1);
    context.page_offsets[0] = 0;
    context.page_index = 0;
    return true;
}

std::uint32_t CurrentPageOffset(const Context& context) noexcept {
    if (context.page_offsets.empty()) return 0;
    const std::size_t index = (std::min)(context.page_index, context.page_offsets.size() - 1);
    return context.page_offsets[index];
}

bool ClampPageToTotal(
    Context& context, const std::uint32_t total_matches, bool* changed) noexcept {
    if (changed == nullptr || !EnsureFirstPage(context)) return false;
    NormalizePageIndex(context);
    const std::uint32_t before = CurrentPageOffset(context);
    if (total_matches == 0) {
        context.page_offsets.resize(1);
        context.page_offsets[0] = 0;
        context.page_index = 0;
    } else {
        while (context.page_index != 0 &&
            context.page_offsets[context.page_index] >= total_matches) {
            --context.page_index;
        }
        context.page_offsets.resize(context.page_index + 1);
    }
    *changed = CurrentPageOffset(context) != before;
    return true;
}

bool MoveToNextPage(Context& context, const std::uint32_t next_offset) noexcept {
    if (!EnsureFirstPage(context)) return false;
    NormalizePageIndex(context);
    context.page_offsets.resize(context.page_index + 1);
    try {
        context.page_offsets.push_back(next_offset);
    } catch (...) {
        return false;
    }
    ++context.page_index;
    return true;
}

bool MoveToPreviousPage(Context& context) noexcept {
    if (!EnsureFirstPage(context)) return false;
    NormalizePageIndex(context);
    if (context.page_index == 0) return false;
    --context.page_index;
    context.page_offsets.resize(context.page_index + 1);
    return true;
}

std::string ResolveClassName(
    const AnomalyNteEntitiesServiceV1* entities,
    const std::uint64_t class_id) {
    if (entities == nullptr ||
        !HasField<AnomalyNteEntitiesServiceV1,
            decltype(AnomalyNteEntitiesServiceV1::class_name_utf8)>(
            entities, offsetof(AnomalyNteEntitiesServiceV1, class_name_utf8)) ||
        entities->class_name_utf8 == nullptr) {
        return {};
    }
    std::size_t size{};
    if (entities->class_name_utf8(entities->user, class_id, nullptr, &size).code !=
            ANOMALY_STATUS_V1_OK ||
        size <= 1 || size > 1024) {
        return {};
    }
    std::string value(size, '\0');
    if (entities->class_name_utf8(entities->user, class_id, value.data(), &size).code !=
        ANOMALY_STATUS_V1_OK) {
        return {};
    }
    if (const std::size_t end = value.find('\0'); end != std::string::npos) value.resize(end);
    return value;
}

void DrawSessionSnapshot(
    const AnomalyNteSessionServiceV1* session, const AnomalyUiServiceV1* ui) {
    if (!HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::snapshot)>(
            session, offsetof(AnomalyNteSessionServiceV1, snapshot)) ||
        session->snapshot == nullptr) {
        DrawUnavailable(ui, "Session snapshot");
        return;
    }

    AnomalyNteSessionSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status = session->snapshot(session->user, &snapshot);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        DrawStatus(ui, "Session snapshot", status);
        return;
    }

    char text[192]{};
    std::snprintf(
        text, sizeof(text), "Session %s / world generation %llu / change %llu",
        snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY ? "WORLD_READY" : "LOADING",
        static_cast<unsigned long long>(snapshot.world.generation),
        static_cast<unsigned long long>(snapshot.sequence));
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

void DrawSessionEvents(
    const AnomalyNteSessionServiceV1* session, const AnomalyUiServiceV1* ui) {
    if (!HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::next_event)>(
            session, offsetof(AnomalyNteSessionServiceV1, next_event)) ||
        session->next_event == nullptr) {
        DrawUnavailable(ui, "Session event stream");
        return;
    }

    AnomalyNteSessionEventV1 event{sizeof(event)};
    const AnomalyStatusV1 status = session->next_event(
        session->user, g_context.session_cursor, &event);
    if (status.code == ANOMALY_STATUS_V1_OK) {
        g_context.session_cursor = event.sequence;
        char text[192]{};
        std::snprintf(
            text, sizeof(text), "Event #%llu at tick %llu / kind %u / world generation %llu",
            static_cast<unsigned long long>(event.sequence),
            static_cast<unsigned long long>(event.tick_sequence), event.kind,
            static_cast<unsigned long long>(event.world.generation));
        ui->text(ui->user, anomaly::sdk::StringView(text));
        return;
    }
    if (status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
        DrawStatus(ui, "Session event stream", status);
        return;
    }
    if (g_context.session_cursor == 0) return;

    if (!HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::latest_event_sequence)>(
            session, offsetof(AnomalyNteSessionServiceV1, latest_event_sequence)) ||
        session->latest_event_sequence == nullptr) {
        DrawUnavailable(ui, "Session event resynchronization");
        return;
    }

    const std::uint64_t latest = session->latest_event_sequence(session->user);
    if (latest == g_context.session_cursor) return;
    g_context.session_cursor = latest;
    char text[192]{};
    std::snprintf(
        text, sizeof(text), "Session event cursor: STALE; resynchronized to #%llu",
        static_cast<unsigned long long>(latest));
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

void DrawSession(const anomaly::sdk::Host& host, const AnomalyUiServiceV1* ui) {
    const auto session = host.Query<AnomalyNteSessionServiceV1>(
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    if (!session) {
        DrawUnavailable(ui, "Session service");
        return;
    }
    DrawSessionSnapshot(session.get(), ui);
    DrawSessionEvents(session.get(), ui);
}

void DrawMetrics(const anomaly::sdk::Host& host, const AnomalyUiServiceV1* ui) {
    const auto metrics = host.Query<AnomalyNteMetricsServiceV1>(
        ANOMALY_NTE_METRICS_SERVICE_V1_ID, ANOMALY_NTE_METRICS_SERVICE_V1_VERSION);
    if (!metrics ||
        !HasField<AnomalyNteMetricsServiceV1,
            decltype(AnomalyNteMetricsServiceV1::snapshot)>(
            metrics.get(), offsetof(AnomalyNteMetricsServiceV1, snapshot)) ||
        metrics->snapshot == nullptr) {
        DrawUnavailable(ui, "Snapshot metrics");
        return;
    }

    AnomalyNteSnapshotMetricsV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status = metrics->snapshot(metrics->user, &snapshot);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        DrawStatus(ui, "Snapshot metrics", status);
        return;
    }
    if ((snapshot.flags & ANOMALY_NTE_METRICS_V1_VALID) == 0) {
        DrawUnavailable(ui, "Snapshot metrics");
        return;
    }

    char text[256]{};
    std::snprintf(
        text, sizeof(text),
        "Snapshot metrics: VALID / ticks %llu / latest %llu us / entity refresh %llu / cache hits %llu / pages %llu / page cache hits %llu",
        static_cast<unsigned long long>(snapshot.snapshot_tick_count),
        static_cast<unsigned long long>(snapshot.latest_snapshot_cost_micros),
        static_cast<unsigned long long>(snapshot.entity_refresh_count),
        static_cast<unsigned long long>(snapshot.entity_cache_hit_count),
        static_cast<unsigned long long>(snapshot.entity_page_request_count),
        static_cast<unsigned long long>(snapshot.entity_page_cache_hit_count));
    ui->text(ui->user, anomaly::sdk::StringView(text));
}

struct EntityPageData {
    std::array<AnomalyNteEntitySnapshotV1, kPageCapacity> items{};
    AnomalyNteEntityPageResultV1 result{};
    std::uint32_t offset{};
};

AnomalyStatusV1 FetchEntityPage(
    const AnomalyNteEntitiesServiceV1* entities, const std::uint64_t generation,
    const std::uint32_t offset, EntityPageData* page) {
    page->offset = offset;
    for (auto& item : page->items) item = {sizeof(item)};
    page->result = {sizeof(page->result)};
    const AnomalyNteEntityPageRequestV1 request{
        sizeof(request), 0, generation, offset, kPageCapacity, 0, 0, 0, 0, 0};
    return entities->page(entities->user, &request, page->items.data(), &page->result);
}

bool IsSaneEntityPage(const EntityPageData& page) noexcept {
    if (page.result.returned > page.items.size()) {
        return false;
    }
    if (page.offset > page.result.total_matches) {
        // A shrinking frame can invalidate the saved page offset. This terminal empty page
        // is consumed only to clamp the page stack before the next fetch.
        return page.result.returned == 0 &&
            page.result.next_offset == page.result.total_matches;
    }
    if (page.result.returned > page.result.total_matches - page.offset) return false;
    if (page.result.returned == 0 && page.offset != page.result.total_matches) {
        return false;
    }
    const std::uint64_t expected_next_offset =
        static_cast<std::uint64_t>(page.offset) + page.result.returned;
    return page.result.next_offset == expected_next_offset;
}

bool HasNextPage(const EntityPageData& page) noexcept {
    return IsSaneEntityPage(page) && page.result.next_offset < page.result.total_matches;
}

void DrawEntities(const anomaly::sdk::Host& host, const AnomalyUiServiceV1* ui) {
    const auto entities = host.Query<AnomalyNteEntitiesServiceV1>(
        ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION);
    if (!entities ||
        !HasField<AnomalyNteEntitiesServiceV1, decltype(AnomalyNteEntitiesServiceV1::frame)>(
            entities.get(), offsetof(AnomalyNteEntitiesServiceV1, frame)) ||
        entities->frame == nullptr ||
        !HasField<AnomalyNteEntitiesServiceV1, decltype(AnomalyNteEntitiesServiceV1::page)>(
            entities.get(), offsetof(AnomalyNteEntitiesServiceV1, page)) ||
        entities->page == nullptr) {
        DrawUnavailable(ui, "Entity page service");
        return;
    }

    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    const AnomalyStatusV1 frame_status = entities->frame(entities->user, &frame);
    if (frame_status.code != ANOMALY_STATUS_V1_OK) {
        DrawStatus(ui, "Entity frame", frame_status);
        return;
    }
    DrawSnapshotState(ui, "Entity frame", frame.flags);
    if (!IsCurrentSnapshot(frame.flags)) return;

    if (g_context.entity_generation != frame.generation &&
        !ResetEntityPagination(g_context, frame.generation)) {
        DrawStatus(ui, "Entity pagination", {ANOMALY_STATUS_V1_FAILED, 0, {}});
        return;
    }

    EntityPageData page{};
    bool page_ready{};
    bool controls_checked{};
    for (std::uint32_t attempt = 0; attempt != 4; ++attempt) {
        EntityPageData candidate{};
        const AnomalyStatusV1 page_status = FetchEntityPage(
            entities.get(), frame.generation, CurrentPageOffset(g_context), &candidate);
        if (page_status.code != ANOMALY_STATUS_V1_OK) {
            if (page_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
                static_cast<void>(ResetEntityPagination(g_context, 0));
            }
            DrawStatus(ui, "Entity page", page_status);
            return;
        }
        if (candidate.result.generation != frame.generation) {
            static_cast<void>(ResetEntityPagination(g_context, 0));
            ui->text(
                ui->user,
                anomaly::sdk::StringView("Entity page: generation changed; reset to first page"));
            return;
        }
        if (!IsSaneEntityPage(candidate)) {
            static_cast<void>(ResetEntityPagination(g_context, frame.generation));
            ui->text(ui->user, anomaly::sdk::StringView("Entity page: invalid result"));
            return;
        }
        if (!IsCurrentSnapshot(candidate.result.flags)) {
            DrawSnapshotState(ui, "Entity page", candidate.result.flags);
            return;
        }

        const std::uint32_t current_offset = CurrentPageOffset(g_context);
        const bool total_shrank = candidate.result.total_matches < g_context.entity_total_matches;
        const bool offset_no_longer_exists = candidate.result.total_matches != 0 &&
            current_offset >= candidate.result.total_matches;
        if (total_shrank || offset_no_longer_exists) {
            bool clamped{};
            if (!ClampPageToTotal(g_context, candidate.result.total_matches, &clamped)) {
                DrawStatus(ui, "Entity pagination", {ANOMALY_STATUS_V1_FAILED, 0, {}});
                return;
            }
            g_context.entity_total_matches = candidate.result.total_matches;
            if (clamped) continue;
        }

        g_context.entity_total_matches = candidate.result.total_matches;
        page = candidate;
        if (!controls_checked) {
            controls_checked = true;
            const bool has_buttons = HasField<AnomalyUiServiceV1,
                decltype(AnomalyUiServiceV1::button)>(
                ui, offsetof(AnomalyUiServiceV1, button)) && ui->button != nullptr;
            if (has_buttons) {
                const bool previous_clicked = ui->button(
                    ui->user, anomaly::sdk::StringView("Previous##nte-inspector"), 0, 0) != 0;
                const bool next_clicked = ui->button(
                    ui->user, anomaly::sdk::StringView("Next##nte-inspector"), 0, 0) != 0;
                if (previous_clicked && g_context.page_index != 0) {
                    static_cast<void>(MoveToPreviousPage(g_context));
                    continue;
                }
                if (next_clicked && HasNextPage(page)) {
                    if (!MoveToNextPage(g_context, page.result.next_offset)) {
                        DrawStatus(ui, "Entity pagination", {ANOMALY_STATUS_V1_FAILED, 0, {}});
                        return;
                    }
                    continue;
                }
            }
        }
        page_ready = true;
        break;
    }

    if (!page_ready) {
        ui->text(
            ui->user,
            anomaly::sdk::StringView("Entity page: changed while navigating; retry next frame"));
        return;
    }

    DrawSnapshotState(ui, "Entity page", page.result.flags);
    const std::uint32_t pages = page.result.total_matches == 0
        ? 1
        : (page.result.total_matches - 1) / kPageCapacity + 1;
    char summary[160]{};
    std::snprintf(
        summary, sizeof(summary), "Entities %u / page %zu/%u / generation %llu",
        page.result.total_matches, g_context.page_index + 1, pages,
        static_cast<unsigned long long>(page.result.generation));
    ui->text(ui->user, anomaly::sdk::StringView(summary));

    for (std::uint32_t index = 0; index < page.result.returned; ++index) {
        const auto& item = page.items[index];
        if (!IsCurrentSnapshot(item.flags) || item.handle.generation != page.result.generation) {
            char state[192]{};
            std::snprintf(
                state, sizeof(state), "Entity item %u: %s%s", index,
                SnapshotState(item.flags), item.handle.generation == page.result.generation
                    ? ""
                    : " / generation mismatch");
            ui->text(ui->user, anomaly::sdk::StringView(state));
            continue;
        }

        const std::string class_name = ResolveClassName(entities.get(), item.class_id);
        char line[224]{};
        if (class_name.empty()) {
            std::snprintf(
                line, sizeof(line), "#%llu / class #%llu / flags 0x%X",
                static_cast<unsigned long long>(item.entity_id),
                static_cast<unsigned long long>(item.class_id), item.flags);
        } else {
            std::snprintf(
                line, sizeof(line), "#%llu / %s / flags 0x%X",
                static_cast<unsigned long long>(item.entity_id), class_name.c_str(), item.flags);
        }
        ui->text(ui->user, anomaly::sdk::StringView(line));
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    const anomaly::sdk::Host view(host);
    if (!view.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_context = {};
    g_context.host = host;
    if (!ResetEntityPagination(g_context, 0)) {
        return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    }
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto& state = *static_cast<Context*>(context);
    state.session_cursor = 0;
    if (!ResetEntityPagination(state, 0)) return {ANOMALY_STATUS_V1_FAILED, 0, {}};
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return anomaly::sdk::Ok(); }
void ANOMALY_CALL Unload(void*) { g_context = {}; }

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (g_context.host == nullptr) return;
    const anomaly::sdk::Host host(g_context.host);
    const auto queried_ui = host.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (ui == nullptr) ui = queried_ui.get();
    if (ui == nullptr ||
        !HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::text)>(
            ui, offsetof(AnomalyUiServiceV1, text)) ||
        ui->text == nullptr) {
        return;
    }
    int open = 1;
    anomaly::sdk::UiWindow window(ui, "NTE Inspector", &open);
    if (!window) return;
    DrawSession(host, ui);
    DrawMetrics(host, ui);
    DrawEntities(host, ui);
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.example.nte-inspector"),
        anomaly::sdk::StringView("NTE Inspector"), anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView("1.0.0"), Load, Start, Stop, Unload, nullptr, Draw};
    return anomaly::sdk::Ok();
}
