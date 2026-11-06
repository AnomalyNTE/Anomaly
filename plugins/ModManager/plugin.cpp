#include "asset_refresh.hpp"
#include <anomaly/sdk/localization.hpp>
#include <mutex>
#include <set>
#include <optional>

namespace {
using namespace anomaly::sdk;
using Refresh = mod_manager::AssetRefresh;
using Result = Refresh::Result;
struct Row {
    std::string key, name, vehicles, groups;
    bool enabled{}, disabled{}, complete{};
    bool operator==(const Row&) const = default;
};
struct RowCollection { std::vector<Row> values; bool failed{}; };
struct AssetCollection { std::set<std::string> values; bool failed{}; };
enum class Kind { Toggle, Release, Force };
struct Action { Kind kind; std::string key; bool enabled{}; };
struct Prepared { std::vector<std::string> vehicles, packages; bool destroy_only{}; };
struct Completion { std::vector<std::string> vehicles; bool destroy_only{}; Result result{}; };
struct Context {
    Localizer localizer;
    const AnomalyHostApiV1* api{};
    const AnomalyModsServiceV1* mods{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyWindowServiceV1* windows{};
    AnomalyGenerationHandleV1 task{}, window{};
    std::mutex mutex;
    bool stopping{true}, busy{}, compatibility{}, profile_valid{}, native_ready{};
    // Worker-owned: remember selections made in either UI while Game is finishing a cleanup.
    bool catalog_seen{};
    std::set<std::string> pending_release;
    std::set<std::string> blocked_release;
    std::optional<Refresh::Services> bindings;
    std::vector<Row> rows;
    std::optional<Action> action;
    std::optional<Prepared> prepared;
    std::optional<Completion> completed;
    std::string status;
    size_t page{}; // Render-owned.
    mod_manager::NativeAccess native;
    Refresh refresh;
} g;

std::string Copy(AnomalyStringViewV1 v) { return v.data ? std::string(v.data, v.size) : std::string{}; }
std::vector<std::string> Tags(std::string_view text) {
    std::vector<std::string> result;
    while (!text.empty()) {
        const auto at = text.find(", ");
        result.emplace_back(text.substr(0, at));
        if (at == std::string_view::npos) break;
        text.remove_prefix(at + 2);
    }
    return result;
}
void LocalizedStatus(std::string_view key, std::string_view fallback, std::string_view detail = {}) {
    const std::array<std::string_view, 1> args{detail};
    auto text = detail.empty() ? g.localizer.Text(key, fallback) : g.localizer.Format(key, fallback, args);
    std::scoped_lock lock(g.mutex); g.status = std::move(text);
}
std::string ErrorText(AnomalyStatusV1 status) {
    return status.message.size ? Copy(status.message) : std::to_string(status.code);
}
int ANOMALY_CALL Collect(void* user, const AnomalyModEntryV1* entry) noexcept {
    auto& result = *static_cast<RowCollection*>(user);
    try {
        if (!entry || entry->struct_size < offsetof(AnomalyModEntryV1, resource_groups)) {
            result.failed = true; return 0;
        }
        result.values.push_back({Copy(entry->key), Copy(entry->name), Copy(entry->vehicles),
            Copy(entry->struct_size >= offsetof(AnomalyModEntryV1, resource_groups) + sizeof(entry->resource_groups)
                ? entry->resource_groups : entry->vehicles),
            entry->enabled != 0, entry->disabled_files != 0, entry->index_complete != 0});
        return 1;
    } catch (...) { result.failed = true; return 0; }
}
int ANOMALY_CALL CollectAsset(void* user, AnomalyStringViewV1 path) noexcept {
    auto& result = *static_cast<AssetCollection*>(user);
    try { result.values.insert(Copy(path)); return 1; }
    catch (...) { result.failed = true; return 0; }
}
void ANOMALY_CALL Worker(void*, AnomalyGenerationHandleV1) noexcept {
    try {
        std::optional<Action> action;
        std::optional<Completion> completed;
        bool native_ready{}, bind_pending{}, busy{};
        std::vector<Row> previous;
        { std::scoped_lock lock(g.mutex);
            if (g.stopping) return;
            action = std::exchange(g.action, {});
            completed = std::exchange(g.completed, {});
            native_ready = g.native_ready; bind_pending = g.bindings.has_value();
            busy = g.busy; previous = g.rows;
        }
        if (completed) for (const auto& tag : completed->vehicles) {
            if (completed->destroy_only) {
                // A switch blocked by an active vehicle resumes once, after that vehicle
                // is gone. Cleaning before a switch never starts an extra resource release.
                if (completed->result == Result::VehiclesCleared && g.blocked_release.erase(tag))
                    g.pending_release.insert(tag);
            } else if (completed->result == Result::ActorPresent) g.blocked_release.insert(tag);
            else g.blocked_release.erase(tag);
        }
        if (g.profile_valid && !native_ready && !bind_pending) {
            const Host host(g.api);
            Refresh::Services services{
                host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get(),
                host.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID).get(),
                host.Query<AnomalyNteActorsServiceV1>(ANOMALY_NTE_ACTORS_SERVICE_V1_ID).get(),
                host.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get(), &g.native};
            if (services.objects && services.player && services.actors && services.session && g.native.BindServices(g.api)) {
                std::scoped_lock lock(g.mutex);
                g.bindings = services;
            }
        }
        bool ok = true;
        bool synchronized{};
        if (action && action->kind == Kind::Toggle) {
            const auto saved = g.compatibility ? AnomalyStatusV1{ANOMALY_STATUS_V1_CONFLICT}
                : g.mods->set_enabled(g.mods->user, StringView(action->key), action->enabled);
            ok = saved.code == 0;
            if (ok) {
                const auto mounted = g.mods->synchronize(g.mods->user);
                synchronized = mounted.code == 0;
                ok = synchronized;
                if (synchronized)
                    LocalizedStatus("status.saved", "Applied");
                else LocalizedStatus("status.mount_pending", "Container switch failed: {0}", ErrorText(mounted));
            } else LocalizedStatus("status.save_failed", "Save failed: {0}", ErrorText(saved));
        }
        RowCollection collection;
        auto listing = g.mods->enumerate(g.mods->user, Collect, &collection);
        if (collection.failed && !listing.code) listing = {ANOMALY_STATUS_V1_FAILED};
        const auto listed = listing.code == 0;
        auto rows = std::move(collection.values);
        if (!listed) { rows.clear(); LocalizedStatus("status.catalog_failed", "Read failed: {0}", ErrorText(listing)); }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return std::tie(a.groups, a.name) < std::tie(b.groups, b.name);
        });
        if (listed && !g.compatibility) {
            if (g.catalog_seen) for (const auto& row : rows) {
                const auto old = std::find_if(previous.begin(), previous.end(), [&](const Row& r) { return r.key == row.key; });
                if (old == previous.end() ? row.enabled : old->enabled != row.enabled) {
                    if (old != previous.end())
                        for (auto& tag : Tags(old->vehicles)) g.pending_release.insert(std::move(tag));
                    for (auto& tag : Tags(row.vehicles)) g.pending_release.insert(std::move(tag));
                }
            }
            if (g.catalog_seen) for (const auto& old : previous)
                if (old.enabled && std::none_of(rows.begin(), rows.end(), [&](const Row& row) { return row.key == old.key; }))
                    for (auto& tag : Tags(old.vehicles)) g.pending_release.insert(std::move(tag));
            g.catalog_seen = true;
        }
        std::optional<Prepared> prepared;
        const bool manual = action && action->kind != Kind::Toggle;
        bool automatic = !g.pending_release.empty() && native_ready && ok &&
            (!busy || (action && action->kind == Kind::Toggle));
        if (automatic && !action) {
            // Reserve the operation before preparing it: a UI request must not enter
            // between the catalog change and publishing the automatic release to Game.
            std::scoped_lock lock(g.mutex);
            if (g.busy) automatic = false;
            else g.busy = true;
        }
        if (manual || automatic) {
            Prepared value;
            value.destroy_only = manual && action->kind == Kind::Force;
            if (manual) {
                const auto selected = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.key == action->key; });
                if (selected != rows.end()) value.vehicles = Tags(selected->vehicles);
            } else value.vehicles.assign(g.pending_release.begin(), g.pending_release.end());
            ok = !g.compatibility && native_ready && listed && !value.vehicles.empty();
            if (ok && value.destroy_only) prepared = std::move(value);
            else if (ok) {
                ok = synchronized || g.mods->synchronize(g.mods->user).code == 0;
                AssetCollection packages;
                // Track the old and new variants: a smaller replacement may not contain every old package.
                for (const auto& row : rows) {
                    if (!ok) break;
                    const auto tags = Tags(row.vehicles);
                    if (std::none_of(tags.begin(), tags.end(), [&](const auto& tag) {
                            return std::find(value.vehicles.begin(), value.vehicles.end(), tag) != value.vehicles.end(); })) continue;
                    if (!row.complete || g.mods->assets(g.mods->user, StringView(row.key), CollectAsset, &packages).code || packages.failed) { ok = false; break; }
                }
                if (ok && !packages.values.empty()) { value.packages.assign(packages.values.begin(), packages.values.end()); prepared = std::move(value); }
                else ok = false;
            }
            // Failed cleanup is a manual fallback, not a once-per-second retry/GC loop.
            if (prepared && !prepared->destroy_only) for (const auto& tag : prepared->vehicles) g.pending_release.erase(tag);
            else if (automatic) g.pending_release.clear();
            if (!ok) LocalizedStatus("status.prepare_failed", "Cleanup unavailable.");
        }
        {
            std::scoped_lock lock(g.mutex);
            if (g.stopping) return;
            if (listed && g.rows != rows) g.rows.swap(rows);
            if (prepared) { g.prepared = std::move(prepared); g.busy = true; }
            else if (action || automatic) g.busy = false;
            if (g.scheduler->schedule(g.scheduler->user, 1000, Worker, nullptr, &g.task).code)
                g.status = g.localizer.Text("status.worker_failed", "Refresh stopped; reload plugin.");
        }
    } catch (...) {
        std::scoped_lock lock(g.mutex); g.busy = false;
        g.status = g.localizer.Text("status.worker_failed", "Refresh stopped; reload plugin.");
    }
}
void Request(Action action) {
    std::scoped_lock lock(g.mutex);
    if (g.stopping || g.busy || g.compatibility) return;
    g.action = std::move(action); g.busy = true;
    g.status = g.localizer.Text("status.queued", "Queued");
}
void Report(Result result) {
    switch (result) {
    case Result::WaitingActors: LocalizedStatus("status.destroy_wait", "Cleaning vehicles..."); break;
    case Result::Waiting: LocalizedStatus("status.gc_wait", "Releasing resources..."); break;
    case Result::VehiclesCleared: LocalizedStatus("status.vehicles_cleared", "Vehicles cleared."); break;
    case Result::Released: LocalizedStatus("status.released", "Released; summon again."); break;
    case Result::NotResident: LocalizedStatus("status.absent", "No resident resources."); break;
    case Result::Retained: LocalizedStatus("status.retained", "Partial release; still resident: {0}", g.refresh.Retained()); break;
    case Result::TemplateInUse: LocalizedStatus("status.template_in_use", "Vehicle data still in use."); break;
    case Result::ActorPresent: LocalizedStatus("status.actor_present", "Vehicle remains; recall or force cleanup."); break;
    case Result::Occupied: LocalizedStatus("status.occupied", "Exit the vehicle first."); break;
    case Result::DestroyRejected: LocalizedStatus("status.destroy_rejected", "Vehicle destruction failed."); break;
    case Result::ChangedWorld: LocalizedStatus("status.world_changed", "World changed; retry."); break;
    case Result::InvalidTag: LocalizedStatus("status.unknown_vehicle", "Vehicle unidentified."); break;
    default: LocalizedStatus("status.unavailable", "Game service unavailable."); break;
    }
}
void ANOMALY_CALL Update(void*, double) noexcept {
    try {
        std::optional<Prepared> prepared;
        { std::unique_lock lock(g.mutex, std::try_to_lock);
            if (!lock || g.stopping) return;
            if (g.bindings) {
                g.refresh.Bind(*g.bindings); g.bindings.reset(); g.native_ready = true;
            }
            if (!g.refresh.Busy()) prepared = std::exchange(g.prepared, {});
        }
        const auto before = g.refresh.State();
        const auto now = Refresh::Clock::now();
        const auto result = prepared ? g.refresh.Begin(prepared->vehicles, prepared->packages, prepared->destroy_only, now) : g.refresh.Poll(now);
        if (prepared || before != result) {
            Report(result);
            std::scoped_lock lock(g.mutex);
            if (!g.refresh.Busy()) {
                g.completed = Completion{g.refresh.Vehicles(), g.refresh.DestroyOnly(), result};
                g.busy = g.prepared.has_value() || g.action.has_value();
            }
        }
    } catch (...) {
        g.refresh.Cancel(); LocalizedStatus("status.unavailable", "Game service unavailable.");
        std::scoped_lock lock(g.mutex); g.busy = false;
    }
}
void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) noexcept {
    if (!ui || !g.windows || !g.window.id) return;
    try {
        AnomalyWindowStateV1 state{sizeof(state)};
        if (g.windows->state(g.windows->user, g.window, &state).code || !state.open) return;
        int visible{};
        if (g.windows->begin(g.windows->user, g.window, 0, &visible).code) return;
        struct End { ~End() { g.windows->end(g.windows->user, g.window); } } end;
        if (!visible) return;
        std::vector<Row> rows; std::string status; bool busy{}, native_ready{};
        { std::unique_lock lock(g.mutex, std::try_to_lock);
            if (!lock) return;
            rows = g.rows; status = g.status; busy = g.busy; native_ready = g.native_ready;
        }
        const auto TextLocalized = [&](std::string_view key, std::string_view fallback) { ui->text(ui->user, StringView(g.localizer.Text(key, fallback))); };
        if (g.compatibility) TextLocalized("mode.compatibility", "Compatibility mode (restart to change mods)");
        else TextLocalized("mode.hot", "Hot mode");
        if (!g.compatibility && !g.profile_valid) TextLocalized("status.profile_unavailable", "Profile mismatch; cleanup disabled.");
        else if (!g.compatibility && !native_ready) TextLocalized("status.services_pending", "Waiting for game services.");
        if (!status.empty()) ui->text(ui->user, StringView(status));
        constexpr size_t page_size = 10;
        const size_t pages = std::max(size_t{1}, (rows.size() + page_size - 1) / page_size);
        g.page = std::min(g.page, pages - 1);
        if (ui->button_enabled(ui->user, StringView(g.localizer.Text("action.previous_page", "Previous")),
                0, 0, g.page > 0)) --g.page;
        ui->same_line(ui->user, 0, -1);
        const auto page_number = std::to_string(g.page + 1), page_count = std::to_string(pages);
        ui->text(ui->user, StringView(g.localizer.Format("page.position", "{0} / {1}",
            std::array<std::string_view, 2>{page_number, page_count})));
        ui->same_line(ui->user, 0, -1);
        if (ui->button_enabled(ui->user, StringView(g.localizer.Text("action.next_page", "Next")),
                0, 0, g.page + 1 < pages)) ++g.page;
        std::optional<std::string> group;
        for (size_t i = g.page * page_size; i < std::min(rows.size(), (g.page + 1) * page_size); ++i) {
            const auto& row = rows[i];
            if (!group || row.groups != *group) {
                ui->separator(ui->user); group = row.groups;
                if (group->empty()) TextLocalized("group.unknown", "Unclassified resources");
                else ui->text(ui->user, StringView(*group));
                if (!row.vehicles.empty()) {
                    const bool can_release = !busy && !g.compatibility && native_ready;
                    const auto ScopedLabel = [&](std::string_view key, std::string_view fallback, std::string_view suffix) {
                        return ui->button_enabled(ui->user, StringView(g.localizer.Label(key, fallback,
                            *group + std::string(suffix))), 0, 0, can_release);
                    };
                    ui->same_line(ui->user, 0, -1);
                    if (ScopedLabel("action.release", "Release resources", "-release")) Request({Kind::Release, row.key});
                    ui->same_line(ui->user, 0, -1);
                    if (ScopedLabel("action.force", "Force cleanup", "-force")) Request({Kind::Force, row.key});
                }
            }
            int enabled = row.enabled;
            if (ui->checkbox(ui->user, StringView("##enabled-" + row.key), &enabled) &&
                !busy && !g.compatibility && (row.enabled || !row.disabled))
                Request({Kind::Toggle, row.key, enabled != 0});
            ui->same_line(ui->user, 0, -1);
            ui->text(ui->user, StringView(row.name));
            if (!row.complete) TextLocalized("row.incomplete", "Index unavailable.");
            if (row.disabled) TextLocalized("row.disabled_files", "Files disabled; restore in Launcher.");
        }
    } catch (...) { /* UI callback boundary; the window guard balances begin/end. */ }
}
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* api, void** context) noexcept {
    try {
        const Host host(api); g.api = api; g.localizer = Localizer(api);
        g.mods = host.Query<AnomalyModsServiceV1>(ANOMALY_MODS_SERVICE_V1_ID).get();
        g.scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get();
        g.windows = host.Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID).get();
        if (!context || !g.mods || g.mods->struct_size < sizeof(*g.mods) || !g.mods->synchronize || !g.mods->assets ||
            !g.mods->enumerate || !g.mods->set_enabled || !g.mods->compatibility_mode ||
            !g.scheduler || g.scheduler->struct_size < sizeof(*g.scheduler) || !g.scheduler->schedule || !g.scheduler->cancel ||
            !g.windows || g.windows->struct_size < sizeof(*g.windows) || !g.windows->register_window || !g.windows->release_window ||
            !g.windows->state || !g.windows->begin || !g.windows->end)
            return {ANOMALY_STATUS_V1_UNAVAILABLE};
        g.compatibility = g.mods->compatibility_mode(g.mods->user) != 0;
        try { g.profile_valid = !g.compatibility && g.native.Load(api); } catch (...) { g.profile_valid = false; }
        AnomalyWindowSpecV1 spec{sizeof(spec)};
        const auto title = g.localizer.Text("window.title", "Mod Manager");
        spec.id = StringView("mod-manager"); spec.title = StringView(title);
        spec.initial_width = 900; spec.initial_height = 650; spec.minimum_width = 650; spec.minimum_height = 400;
        const auto status = g.windows->register_window(g.windows->user, &spec, &g.window);
        if (status.code) return status;
        *context = &g; return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}
AnomalyStatusV1 ANOMALY_CALL Start(void*) noexcept {
    std::scoped_lock lock(g.mutex); g.stopping = false;
    return g.scheduler->schedule(g.scheduler->user, 0, Worker, nullptr, &g.task);
}
AnomalyStatusV1 ANOMALY_CALL Stop(void*, uint32_t) noexcept {
    AnomalyGenerationHandleV1 task{};
    { std::scoped_lock lock(g.mutex); g.stopping = true; task = g.task; }
    if (task.id) g.scheduler->cancel(g.scheduler->user, task);
    return Ok();
}
void ANOMALY_CALL Unload(void*) noexcept {
    if (g.window.id) g.windows->release_window(g.windows->user, g.window);
    g.window = {}; g.refresh.Cancel();
}
} // namespace
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("anomaly.mod-manager"), StringView("Mod Manager"), StringView("Anomaly"), StringView("0.1.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
