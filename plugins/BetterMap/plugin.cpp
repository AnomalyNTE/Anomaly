#include "map_icons.hpp"
#include "city_events.hpp"
#include "collection.hpp"
#include "teleport_hotkey.hpp"
#include "map_pin_guard.hpp"
#include "preferences.hpp"
#include <anomaly/sdk/localization.hpp>
#include <atomic>
#include <chrono>
#include <memory>

namespace collectible_map {
using namespace anomaly::sdk;
using Clock = std::chrono::steady_clock;
constexpr auto Id = "b1ank.better-map";
constexpr auto Title = "Better Map";
constexpr auto Version = "0.1.16";

enum class Status { Idle, Waiting, Unavailable, Ready, Incomplete };

struct Category {
    size_t next{}, accepted{};
    bool clear_pending{true};
};

struct Filter {
    using Results = std::array<std::vector<uint8_t>, 2>;
    Results completed, pending;
    size_t kind{}, index{}, unknown{};
    Clock::time_point next_scan{};
};

struct MonsterChoice {
    std::string id, name, label;
};
using MonsterChoices = std::vector<MonsterChoice>;

struct Context {
    explicit Context(const AnomalyHostApiV1* host) : services(host), localizer(host) {
        engine.core = services.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        window = services.Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID).get();
        if (window && (window->struct_size < sizeof(AnomalyWindowServiceV1) || !window->register_window ||
                       !window->release_window || !window->state || !window->begin || !window->end)) window = nullptr;
    }

    // The player service can be published only after the first playable world is ready.
    bool BindGameServices() {
        if (!framework) framework = services.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
        if (!worlds) worlds = services.Query<AnomalyUe5WorldServiceV1>(ANOMALY_UE5_WORLD_SERVICE_V1_ID).get();
        if (!engine.names) engine.names = services.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
        if (!engine.objects) engine.objects = services.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        if (!players) {
            const auto service = services.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID);
            if (service && service->struct_size >= sizeof(AnomalyNtePlayerServiceV1) && service->get_object_handles)
                players = service.get();
        }
        if (!engine.events) {
            const auto service = services.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID);
            if (service && service->struct_size >= sizeof(AnomalyUe5ProcessEventServiceV1) && service->invoke)
                engine.events = service.get();
        }
        return framework && worlds && players && engine.names && engine.objects && engine.events;
    }

    Host services;
    Localizer localizer;
    Engine engine;
    Profile profile;
    MapApi map;
    MapIcons icons;
    MapTeleport teleport;
    TeleportHotkey teleport_hotkey{teleport};
    MapPinGuard map_pin;
    PreferencesStore preferences_store;
    MapPreferences preferences;
    std::atomic<std::shared_ptr<const std::vector<std::string>>> hidden_monsters;
    Catalog catalog;
    Collection collection;
    Filter filter;
    MonsterCatalog monsters;
    std::vector<Marker> monster_markers;
    std::vector<Category> monster_progress;
    std::atomic<std::shared_ptr<MonsterChoices>> monster_choices;
    bool clear_monsters{true}, monster_update_pending{};
    size_t last_monster_count{}, last_monster_total{};
    size_t next_monster_group{}, next_monster_marker{};
    Clock::time_point next_monster_retry{};
    std::atomic<bool> show_monsters{}, monsters_available{}, monsters_unavailable{};
    std::atomic<size_t> monsters_added{}, monsters_total{};
    std::vector<Marker> city_events;
    Category city_progress;
    Clock::time_point next_city_retry{};
    bool city_icons_ready{};
    std::atomic<bool> show_city_events{}, city_unavailable{};
    std::atomic<size_t> city_added{}, city_total{};
    bool monster_filter_open{};
    std::array<char, 128> monster_search{};
    size_t monster_page{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyUe5WorldServiceV1* worlds{};
    const AnomalyNtePlayerServiceV1* players{};
    const AnomalyWindowServiceV1* window{};
    AnomalyGenerationHandleV1 window_handle{}, world{};
    Ref player;
    int32_t fallback_open{1};
    std::array<Category, 2> progress{};
    std::array<std::atomic<bool>, 2> enabled{};
    std::atomic<bool> hide_completed{}, filter_reading{}, filter_unknown{}, pin_guard_ready{};
    std::array<std::atomic<size_t>, 2> accepted{}, totals{};
    std::atomic<Status> status{Status::Idle};
    Clock::time_point next_update{}, next_service_check{};
    std::string_view last_log;
};

void PublishPreferences(Context& ctx) {
    const auto& settings = ctx.preferences;
    ctx.enabled[0] = settings.oracle;
    ctx.enabled[1] = settings.gift;
    ctx.show_monsters = settings.monsters;
    ctx.show_city_events = settings.city_events;
    ctx.hide_completed = settings.hide_completed;
    ctx.teleport.enabled = settings.teleport;
    ctx.hidden_monsters.store(std::make_shared<const std::vector<std::string>>(settings.hidden_monsters));
}

void Log(Context& ctx, std::string_view message) {
    if (ctx.last_log == message) return;
    ctx.last_log = message;
    if (const auto* core = ctx.engine.core; core && core->log)
        core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(message));
}

Ref LocalPlayer(Context& ctx) {
    AnomalyGenerationHandleV1 controller{};
    if (!ctx.players || !ctx.players->get_object_handles ||
        !Succeeded(ctx.players->get_object_handles(ctx.players->user, nullptr, &controller))) return {};
    const auto local_controller = ctx.engine.Resolve(controller);
    uintptr_t state{};
    if (!ctx.engine.Member(local_controller, "PlayerState", "ObjectProperty", state)) return {};
    const auto player = ctx.engine.Reference(state);
    return ctx.engine.IsA(player, "HTPlayerState") ? player : Ref{};
}

bool UpdateFilter(Context& ctx, Ref player, Clock::time_point now) {
    auto& filter = ctx.filter;
    if (!ctx.hide_completed.load()) {
        for (size_t kind = 0; kind < ctx.catalog.size(); ++kind) {
            if (std::ranges::any_of(filter.completed[kind], [](uint8_t done) { return done != 0; }))
                ctx.progress[kind].clear_pending = true;
            ctx.totals[kind] = ctx.catalog[kind].size();
        }
        if (!filter.completed[0].empty() || !filter.pending[0].empty()) filter = {};
        ctx.filter_reading = false;
        ctx.filter_unknown = false;
        return false;
    }
    if (now < filter.next_scan) return false;
    if (filter.pending[0].empty()) {
        ctx.collection.Bind(ctx.engine);
        for (size_t kind = 0; kind < ctx.catalog.size(); ++kind)
            filter.pending[kind].resize(ctx.catalog[kind].size());
        ctx.filter_reading = filter.completed[0].empty();
    }
    constexpr size_t queries_per_update = 12;
    for (size_t count = 0; count < queries_per_update && filter.kind < ctx.catalog.size(); ++count) {
        const auto result = ctx.collection.Completed(ctx.engine, player, filter.kind, ctx.catalog[filter.kind][filter.index]);
        if (!result) ++filter.unknown;
        auto& category = filter.pending[filter.kind];
        category[filter.index] = result.value_or(false);
        if (++filter.index == category.size()) {
            ++filter.kind;
            filter.index = 0;
        }
    }
    if (filter.kind < ctx.catalog.size()) return true;

    for (size_t kind = 0; kind < ctx.catalog.size(); ++kind) {
        const auto hidden = std::ranges::count(filter.pending[kind], uint8_t{1});
        if (filter.completed[kind] != filter.pending[kind] && (hidden || !filter.completed[kind].empty()))
            ctx.progress[kind].clear_pending = true;
        ctx.totals[kind] = ctx.catalog[kind].size() - hidden;
    }
    const bool unknown = filter.unknown != 0;
    if (filter.completed != filter.pending || ctx.filter_unknown.load() != unknown) {
        if (const auto* core = ctx.engine.core; core && core->log) {
            const auto message = "Collectible Map: filter snapshot, visible oracle=" + std::to_string(ctx.totals[0]) +
                                 " gift=" + std::to_string(ctx.totals[1]) + " unknown=" + std::to_string(filter.unknown);
            core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(message));
        }
    }
    filter.completed = std::move(filter.pending);
    filter.pending = {};
    filter.kind = filter.index = filter.unknown = 0;
    filter.next_scan = now + std::chrono::seconds(5);
    ctx.filter_reading = false;
    ctx.filter_unknown = unknown;
    return false;
}

bool UpdateMonsters(Context& ctx, Ref player, Clock::time_point now) {
    const bool show = ctx.show_monsters.load();
    if (now < ctx.next_monster_retry) return false;
    if (ctx.monsters.groups.empty()) {
        MonsterCatalog catalog;
        if (!ReadMonsterGroups(ctx.engine, ctx.profile, catalog)) {
            ctx.monsters_unavailable = true;
            ctx.next_monster_retry = now + std::chrono::seconds(2);
            return false;
        }
        ctx.monster_markers = MonsterMarkers(catalog);
        ctx.monsters = std::move(catalog);
        const auto& groups = ctx.monsters.groups;
        auto choices = std::make_shared<MonsterChoices>(groups.size());
        for (size_t index = 0; index < groups.size(); ++index) {
            auto& choice = (*choices)[index];
            choice.id = groups[index].id;
            choice.name = groups[index].label;
            choice.label = choice.name + " [" + std::to_string(groups[index].positions.size()) + "]###monster-" +
                           choice.id;
        }
        ctx.monster_progress.assign(groups.size(), {});
        ctx.monster_choices.store(std::move(choices));
        ctx.monsters_unavailable = false;
    }
    if (ctx.clear_monsters) {
        if (!ctx.map.Remove(ctx.engine, player, ctx.monster_markers)) return false;
        std::ranges::fill(ctx.monster_progress, Category{});
        ctx.clear_monsters = false;
        ctx.monsters_added = 0;
        ctx.monster_update_pending = true;
    }
    if (show && ctx.next_monster_group < ctx.monsters.groups.size()) {
        if (ctx.engine.Resolve(ctx.monsters.table.handle) != ctx.monsters.table) {
            ctx.monsters = {};
            ctx.monster_markers.clear();
            ctx.next_monster_group = ctx.next_monster_marker = 0;
            return true;
        }
        if (!ctx.icons.PrepareMonsters(ctx.engine, ctx.profile, ctx.monsters, ctx.monster_markers,
                                      ctx.next_monster_group, ctx.next_monster_marker)) {
            ctx.monsters_available = false;
            ctx.monsters_unavailable = true;
            ctx.next_monster_retry = now + std::chrono::seconds(2);
            return false;
        }
        ctx.monsters_unavailable = false;
        if (ctx.next_monster_group < ctx.monsters.groups.size()) return true;
    }

    const auto choices = ctx.monster_choices.load();
    const auto hidden = ctx.hidden_monsters.load();
    size_t offset{}, added{}, total{};
    constexpr size_t batch_size = 16;
    std::array<const Marker*, batch_size> batch{};
    std::array<size_t, batch_size> batch_groups{};
    size_t count{};
    bool pending{};
    std::vector<Array> remove_ids;
    std::vector<size_t> remove_groups;
    for (size_t group = 0; group < choices->size(); ++group) {
        auto& progress = ctx.monster_progress[group];
        const size_t size = ctx.monsters.groups[group].positions.size();
        const auto markers = std::span(ctx.monster_markers).subspan(offset, size);
        offset += size;
        const bool visible = show && (!hidden || std::ranges::find(*hidden, (*choices)[group].id) == hidden->end());
        if (!visible) {
            if (progress.next) {
                for (const auto& marker : markers.first(progress.next)) remove_ids.push_back(Borrow(marker.id));
                remove_groups.push_back(group);
            }
            continue;
        }
        total += size;
        added += progress.accepted;
        while (progress.next < size && count < batch.size()) {
            batch_groups[count] = group;
            batch[count++] = &markers[progress.next++];
        }
        pending |= progress.next < size;
    }
    if (!remove_ids.empty()) {
        if (ctx.map.RemoveIds(ctx.engine, player, remove_ids)) {
            for (const auto group : remove_groups) ctx.monster_progress[group] = {};
        } else pending = true;
    }
    if (count) {
        std::array<bool, batch_size> accepted{};
        added += ctx.map.Add(ctx.engine, player, std::span(batch).first(count), std::span(accepted).first(count));
        for (size_t index = 0; index < count; ++index)
            if (accepted[index]) ++ctx.monster_progress[batch_groups[index]].accepted;
    }
    ctx.monsters_added = added;
    ctx.monsters_total = total;
    ctx.monsters_available = show;
    ctx.monster_update_pending |= added != ctx.last_monster_count || total != ctx.last_monster_total;
    if (!pending && ctx.monster_update_pending) {
        ctx.monster_update_pending = false;
        ctx.last_monster_count = added;
        ctx.last_monster_total = total;
        if (ctx.engine.core && ctx.engine.core->log) {
            const auto message = "Collectible Map: monster points registered " + std::to_string(added) +
                                 "/" + std::to_string(total);
            ctx.engine.core->log(ctx.engine.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(message));
        }
    }
    return pending;
}

bool UpdateCityEvents(Context& ctx, Ref player, Clock::time_point now) {
    if (now < ctx.next_city_retry) return false;
    if (ctx.city_events.empty()) {
        if (!ReadCityEventMarkers(ctx.engine, ctx.profile, ctx.city_events)) {
            ctx.city_unavailable = true;
            ctx.next_city_retry = now + std::chrono::seconds(2);
            return false;
        }
        ctx.city_total = ctx.city_events.size();
        ctx.city_unavailable = false;
        if (const auto* core = ctx.engine.core; core && core->log) {
            const auto message = "Collectible Map: city event locations read " + std::to_string(ctx.city_events.size());
            core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(message));
        }
    }
    const bool show = ctx.show_city_events.load();
    auto& progress = ctx.city_progress;
    if (progress.clear_pending || (!show && progress.next)) {
        if (!ctx.map.Remove(ctx.engine, player, ctx.city_events)) return false;
        progress = {0, 0, false};
        ctx.city_added = 0;
    }
    if (!show) return false;

    if (!ctx.city_icons_ready) {
        if (const auto* failure = ctx.icons.PrepareCityEvents(ctx.engine, ctx.profile, ctx.city_events)) {
            Log(ctx, failure);
            ctx.city_unavailable = true;
            ctx.next_city_retry = now + std::chrono::seconds(2);
            return false;
        }
        ctx.city_icons_ready = true;
        ctx.city_unavailable = false;
    }
    if (progress.next == ctx.city_events.size()) return false;
    std::array<const Marker*, 16> batch{};
    const auto count = std::min(batch.size(), ctx.city_events.size() - progress.next);
    for (size_t index = 0; index < count; ++index) batch[index] = &ctx.city_events[progress.next + index];
    progress.accepted += ctx.map.Add(ctx.engine, player, std::span(batch).first(count));
    progress.next += count;
    ctx.city_added = progress.accepted;
    if (progress.next < ctx.city_events.size()) return true;
    if (const auto* core = ctx.engine.core; core && core->log) {
        const auto message = "Collectible Map: city event points registered " + std::to_string(progress.accepted) +
                             "/" + std::to_string(ctx.city_events.size());
        core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(message));
    }
    return false;
}

void UpdateMap(Context& ctx, Clock::time_point now) {
    if (!ctx.engine.registry) {
        ctx.status = Status::Unavailable;
        ctx.next_update = now + std::chrono::seconds(2);
        return;
    }
    AnomalyGenerationHandleV1 world{};
    const auto player = LocalPlayer(ctx);
    if (!player || !ctx.worlds->current || !Succeeded(ctx.worlds->current(ctx.worlds->user, &world)) || !world.id) {
        ctx.status = Status::Waiting;
        ctx.next_update = now + std::chrono::milliseconds(500);
        return;
    }
    if (ctx.world.id != world.id || ctx.world.generation != world.generation || !(ctx.player == player)) {
        ctx.world = world;
        ctx.player = player;
        ctx.progress = {};
        ctx.filter = {};
        ctx.clear_monsters = true;
        ctx.monsters_added = 0;
        ctx.monsters_available = false;
        ctx.monsters_unavailable = false;
        ctx.city_progress = {};
        ctx.city_events.clear();
        ctx.city_icons_ready = false;
        ctx.next_city_retry = {};
        ctx.city_added = ctx.city_total = 0;
        ctx.city_unavailable = false;
        for (size_t kind = 0; kind < ctx.catalog.size(); ++kind) ctx.totals[kind] = ctx.catalog[kind].size();
        for (auto& count : ctx.accepted) count = 0;
    }

    if (!ctx.map && !ctx.map.Bind(ctx.engine)) {
        Log(ctx, "Collectible Map: native map API validation is pending");
        ctx.status = Status::Unavailable;
        ctx.next_update = now + std::chrono::seconds(2);
        return;
    }
    if (ctx.catalog[0].empty()) {
        Catalog catalog;
        if (!ReadCatalog(ctx.engine, ctx.profile, catalog)) {
            Log(ctx, "Collectible Map: waiting for collectible and icon tables");
            ctx.status = Status::Waiting;
            ctx.next_update = now + std::chrono::seconds(2);
            return;
        }
        if (const auto* failure = ctx.icons.Prepare(ctx.engine, ctx.profile, catalog)) {
            Log(ctx, failure);
            ctx.status = Status::Unavailable;
            ctx.next_update = now + std::chrono::seconds(2);
            return;
        }
        ctx.catalog = std::move(catalog);
        for (size_t kind = 0; kind < ctx.catalog.size(); ++kind) ctx.totals[kind] = ctx.catalog[kind].size();
        Log(ctx, "Collectible Map: native map API and collectible catalog validated");
    }

    const bool updating_monsters = UpdateMonsters(ctx, player, now);
    const bool updating_city_events = UpdateCityEvents(ctx, player, now);
    const bool reading_progress = UpdateFilter(ctx, player, now);
    auto status = Status::Idle;
    bool incomplete = false;
    bool pending = false;
    for (size_t kind = 0; kind < ctx.catalog.size(); ++kind) {
        auto& progress = ctx.progress[kind];
        const auto& markers = ctx.catalog[kind];
        const bool show = ctx.enabled[kind].load();
        if (progress.clear_pending || (!show && progress.next)) {
            if (!ctx.map.Remove(ctx.engine, player, markers)) {
                incomplete = true;
                continue;
            }
            progress = {0, 0, false};
            ctx.accepted[kind] = 0;
        }
        if (!show) continue;
        status = Status::Ready;
        if (progress.next < markers.size()) {
            std::array<const Marker*, 16> batch{};
            const auto& completed = ctx.filter.completed[kind];
            size_t count{};
            while (progress.next < markers.size() && count < batch.size()) {
                const auto index = progress.next++;
                if (completed.empty() || !completed[index]) batch[count++] = &markers[index];
            }
            if (count) progress.accepted += ctx.map.Add(ctx.engine, player, std::span(batch).first(count));
            ctx.accepted[kind] = progress.accepted;
        }
        pending |= progress.next < markers.size();
        incomplete |= progress.next == markers.size() && progress.accepted != ctx.totals[kind].load();
    }
    ctx.status = incomplete ? Status::Incomplete : status;
    if (incomplete) Log(ctx, "Collectible Map: the game did not accept every marker");
    else if (!pending && status == Status::Ready) Log(ctx, "Collectible Map: enabled native markers registered");
    else if (status == Status::Idle) Log(ctx, "Collectible Map: all plugin markers are hidden");
    if (!pending && !reading_progress && !updating_monsters && !updating_city_events)
        ctx.next_update = now + std::chrono::milliseconds(250);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** output) noexcept {
    if (!host || !output) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *output = nullptr;
    try {
        auto profile = LoadProfile(Host(host), "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "b1ank-collectible-map-v1"});
        if (!profile) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, StringView("Collectible map profile is unavailable")};
        auto ctx = std::make_unique<Context>(host);
        ctx->profile = std::move(profile).value();
        if (!ctx->engine.ResolveProfile(host, ctx->profile)) Log(*ctx, "Collectible Map: game signatures are unavailable");
        ctx->collection.Resolve(ctx->services, ctx->engine, ctx->profile);
        ctx->icons.Resolve(ctx->services, ctx->engine, ctx->profile);
        ctx->teleport.Resolve(ctx->services, ctx->engine, ctx->profile);
        ctx->map_pin.Resolve(ctx->services, ctx->engine, ctx->profile);
        ctx->teleport_hotkey.Load(ctx->services);
        ctx->preferences = ctx->preferences_store.Load(ctx->services);
        *output = ctx.release();
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, {}}; }
}

AnomalyStatusV1 ANOMALY_CALL Start(void* raw) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    try {
        PublishPreferences(ctx);
        ctx.teleport_hotkey.Start();
        ctx.pin_guard_ready = ctx.map_pin.Start(ctx.teleport.enabled);
        Log(ctx, ctx.pin_guard_ready ? "Collectible Map: map pin dialog guard ready"
                                     : "Collectible Map: map pin dialog guard unavailable");
        if (!ctx.window) return Ok();
        const auto title = ctx.localizer.Text("window.title", "Better Map");
        AnomalyWindowSpecV1 spec{sizeof(spec)};
        spec.id = StringView("collectible-map-main");
        spec.title = StringView(title);
        spec.initial_width = 390;
        spec.initial_height = 340;
        spec.minimum_width = 290;
        spec.minimum_height = 135;
        spec.default_open = 1;
        static_cast<void>(ctx.window->register_window(ctx.window->user, &spec, &ctx.window_handle));
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, {}}; }
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* raw, uint32_t) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    for (auto& enabled : ctx.enabled) enabled = false;
    ctx.teleport.enabled = false;
    ctx.teleport_hotkey.Stop();
    ctx.preferences_store.Save();
    const auto pin_guard_stopped = ctx.map_pin.Stop();
    if (!Succeeded(pin_guard_stopped)) return pin_guard_stopped;
    const auto detached = ctx.teleport.Detach();
    if (!Succeeded(detached)) return detached;
    if (ctx.window && ctx.window_handle.id) {
        const auto result = ctx.window->release_window(ctx.window->user, ctx.window_handle);
        if (result.code != ANOMALY_STATUS_V1_OK && result.code != ANOMALY_STATUS_V1_NOT_FOUND) return result;
        ctx.window_handle = {};
    }
    return Ok();
}

void ANOMALY_CALL Unload(void* raw) noexcept { delete static_cast<Context*>(raw); }

void ANOMALY_CALL Update(void* raw, double) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    const auto now = Clock::now();
    if (!ctx.framework || !ctx.worlds || !ctx.players || !ctx.engine.names || !ctx.engine.objects || !ctx.engine.events) {
        if (now < ctx.next_service_check) return;
        ctx.next_service_check = now + std::chrono::milliseconds(500);
        if (!ctx.BindGameServices()) {
            Log(ctx, "Collectible Map: waiting for game services to be published");
            ctx.status = Status::Waiting;
            return;
        }
    }
    if (!ctx.framework->is_game_thread || !ctx.framework->is_game_thread(ctx.framework->user)) return;
    ctx.teleport.Update(ctx.services, ctx.engine, ctx.framework, ctx.worlds, ctx.players, now);
    if (now >= ctx.next_update) {
        ctx.next_update = now + std::chrono::milliseconds(50);
        try { UpdateMap(ctx, now); }
        catch (...) { ctx.status = Status::Unavailable; }
    }
}

void DrawMonsterFilter(Context& ctx, const AnomalyUiServiceV1* ui, bool& changed) {
    const auto choices = ctx.monster_choices.load();
    if (!choices || !ui->button) return;
    const auto& localizer = ctx.localizer;
    const auto toggle = ctx.monster_filter_open
        ? localizer.Text("map.monster_filter_close", "Close enemy filter")
        : localizer.Text("map.monster_filter", "Filter enemies");
    if (ui->button(ui->user, StringView(toggle), 0, 0))
        ctx.monster_filter_open = !ctx.monster_filter_open;
    if (!ctx.monster_filter_open) return;

    if (ui->struct_size >= offsetof(AnomalyUiServiceV1, input_text) + sizeof(ui->input_text) && ui->input_text) {
        const auto search = localizer.Label("map.monster_search", "Search enemies", "monster-search");
        if (ui->input_text(ui->user, StringView(search), ctx.monster_search.data(), ctx.monster_search.size(), 0))
            ctx.monster_page = 0;
    }

    const std::string_view search(ctx.monster_search.data());
    std::vector<MonsterChoice*> matches;
    matches.reserve(choices->size());
    for (auto& choice : *choices) {
        if (search.empty() || (ui->filter_match
                ? ui->filter_match(ui->user, StringView(search), StringView(choice.name)) != 0
                : choice.name.find(search) != std::string::npos))
            matches.push_back(&choice);
    }

    const auto same_line = [&] {
        if (ui->struct_size >= offsetof(AnomalyUiServiceV1, same_line) + sizeof(ui->same_line) && ui->same_line)
            ui->same_line(ui->user, 0, -1);
    };
    const auto all = localizer.Text("map.monster_all", "Select all");
    const auto none = localizer.Text("map.monster_none", "Clear selection");
    auto& hidden = ctx.preferences.hidden_monsters;
    if (ui->button(ui->user, StringView(all), 0, 0)) {
        hidden.clear();
        changed = true;
    }
    same_line();
    if (ui->button(ui->user, StringView(none), 0, 0)) {
        hidden.clear();
        for (const auto& choice : *choices) hidden.push_back(choice.id);
        changed = true;
    }
    const auto is_visible = [&](const MonsterChoice& choice) {
        return std::ranges::find(hidden, choice.id) == hidden.end();
    };
    const auto selected = std::to_string(std::ranges::count_if(*choices, is_visible));
    const auto total = std::to_string(choices->size());
    same_line();
    ui->text(ui->user, StringView(localizer.Format("map.monster_selected", "{0} / {1} types selected",
        std::array<std::string_view, 2>{selected, total})));

    if (matches.empty()) {
        ui->text(ui->user, StringView(localizer.Text("map.monster_no_matches", "No matching enemies.")));
        return;
    }

    constexpr size_t page_size = 8;
    constexpr int32_t columns = 2;
    const size_t pages = (matches.size() + page_size - 1) / page_size;
    ctx.monster_page = std::min(ctx.monster_page, pages - 1);
    if (pages > 1) {
        const auto page_button = [&](const std::string& label, bool enabled) {
            if (ui->struct_size >= offsetof(AnomalyUiServiceV1, button_enabled) + sizeof(ui->button_enabled)
                && ui->button_enabled)
                return ui->button_enabled(ui->user, StringView(label), 0, 0, enabled ? 1 : 0) && enabled;
            return ui->button(ui->user, StringView(label), 0, 0) && enabled;
        };
        if (page_button(localizer.Text("map.monster_previous", "Previous"), ctx.monster_page > 0))
            --ctx.monster_page;
        same_line();
        if (page_button(localizer.Text("map.monster_next", "Next"), ctx.monster_page + 1 < pages))
            ++ctx.monster_page;
        const auto page = std::to_string(ctx.monster_page + 1);
        const auto page_count = std::to_string(pages);
        same_line();
        ui->text(ui->user, StringView(localizer.Format("map.monster_page", "{0} / {1}",
            std::array<std::string_view, 2>{page, page_count})));
    }

    const bool table = ui->begin_table && ui->table_next_column && ui->end_table &&
        ui->begin_table(ui->user, StringView("monster-filter-grid"), columns, ANOMALY_UI_TABLE_V1_NONE, 0, 0);
    const size_t first = ctx.monster_page * page_size;
    const size_t end = std::min(first + page_size, matches.size());
    for (size_t index = first; index < end; ++index) {
        if (table && !ui->table_next_column(ui->user)) continue;
        auto& choice = *matches[index];
        int32_t visible = is_visible(choice) ? 1 : 0;
        if (ui->checkbox(ui->user, StringView(choice.label), &visible)) {
            if (visible) std::erase(hidden, choice.id);
            else hidden.push_back(choice.id);
            changed = true;
        }
    }
    if (table) ui->end_table(ui->user);
}

void DrawContent(Context& ctx, const AnomalyUiServiceV1* ui) noexcept {
    try {
        const auto oracle = ctx.localizer.Text("map.oracle", "Show Oracle Stones");
        const auto gift = ctx.localizer.Text("map.gift", "Show Twenty-One's Gifts");
        bool changed{};
        const auto checkbox = [&](const std::string& label, bool& setting) {
            int32_t value = setting ? 1 : 0;
            if (ui->checkbox(ui->user, StringView(label), &value)) {
                setting = value != 0;
                changed = true;
            }
        };
        auto& settings = ctx.preferences;
        checkbox(oracle, settings.oracle);
        checkbox(gift, settings.gift);
        const auto monsters = ctx.localizer.Text("map.monsters", "Show enemy spawn points");
        checkbox(monsters, settings.monsters);
        if (settings.monsters) {
            if (ctx.monsters_available.load()) {
                const auto added = std::to_string(ctx.monsters_added.load());
                const auto total = std::to_string(ctx.monsters_total.load());
                const auto count = ctx.localizer.Format("map.monster_count", "Enemy spawn points {0} / {1}",
                    std::array<std::string_view, 2>{added, total});
                ui->text(ui->user, StringView(count));
            } else if (ctx.monsters_unavailable.load()) {
                ui->text(ui->user, StringView(ctx.localizer.Text("map.monster_unavailable", "Enemy points are unavailable in the current scene.")));
            } else {
                ui->text(ui->user, StringView(ctx.localizer.Text("map.monster_wait", "Reading enemy spawn points…")));
            }
            DrawMonsterFilter(ctx, ui, changed);
        }
        const auto city_events = ctx.localizer.Text("map.city_events", "Show city crime locations");
        checkbox(city_events, settings.city_events);
        if (settings.city_events) {
            std::string city_status;
            if (ctx.city_unavailable.load()) {
                city_status = ctx.localizer.Text("map.city_unavailable", "City crime locations are currently unavailable.");
            } else if (const auto total = ctx.city_total.load()) {
                const auto added = std::to_string(ctx.city_added.load());
                const auto count = std::to_string(total);
                city_status = ctx.localizer.Format("map.city_count", "Possible city crime locations {0} / {1}",
                    std::array<std::string_view, 2>{added, count});
            } else {
                city_status = ctx.localizer.Text("map.city_wait", "Reading city crime locations…");
            }
            ui->text(ui->user, StringView(city_status));
        }
        const auto filter = ctx.localizer.Text("map.hide_completed", "Hide completed collectibles");
        checkbox(filter, settings.hide_completed);
        if (settings.hide_completed) {
            if (ctx.filter_reading.load())
                ui->text(ui->user, StringView(ctx.localizer.Text("map.reading_progress", "Reading collection progress…")));
            else if (ctx.filter_unknown.load())
                ui->text(ui->user, StringView(ctx.localizer.Text("map.unknown_progress", "Some progress is unavailable. Those points are still shown.")));
        }
        const auto click_teleport = ctx.localizer.Text("teleport.enable", "Enable map click teleport");
        checkbox(click_teleport, settings.teleport);
        if (changed) {
            PublishPreferences(ctx);
            ctx.preferences_store.Queue(settings);
        }
        if (!settings.teleport) ctx.teleport_hotkey.CancelCapture();
        if (settings.teleport) {
            ctx.teleport_hotkey.Draw(ui, ctx.localizer);
            if (!ctx.pin_guard_ready.load())
                (ui->text_wrapped ? ui->text_wrapped : ui->text)(ui->user,
                    StringView(ctx.localizer.Text("teleport.pin_guard_unavailable", "The pin dialog may still open on this game build.")));
            std::string hint;
            const auto teleport_status = ctx.teleport.status.load();
            const bool loading = teleport_status == TeleportStatus::Loading;
            const bool picking = ctx.teleport.pick_once.load();
            if (ui->button && teleport_status != TeleportStatus::Unavailable) {
                const auto button = (loading || picking)
                    ? ctx.localizer.Text("teleport.cancel", "Cancel map teleport")
                    : ctx.localizer.Text("teleport.pick", "Choose a point to teleport");
                if (ui->button(ui->user, StringView(button), 0, 0)) {
                    if (loading) ctx.teleport.cancel_pending = true;
                    else ctx.teleport.pick_once = !picking;
                }
            }
            if (picking && !loading)
                hint = ctx.localizer.Text("teleport.picking", "Left-click the target on the world map.");
            else switch (teleport_status) {
            case TeleportStatus::Waiting:
                hint = ctx.localizer.Text("teleport.waiting", "Waiting for map click teleport…"); break;
            case TeleportStatus::Ready:
                hint = ctx.localizer.Text("teleport.ready", "Use the shortcut on the world map to teleport to the cursor."); break;
            case TeleportStatus::Unavailable:
                hint = ctx.localizer.Text("teleport.unavailable", "Map click teleport is unavailable for this game build."); break;
            case TeleportStatus::OutOfScene:
                hint = ctx.localizer.Text("teleport.scene", "Point at the target on the main world map."); break;
            case TeleportStatus::InvalidPoint:
                hint = ctx.localizer.Text("teleport.point", "This map point could not be resolved."); break;
            case TeleportStatus::Loading:
                hint = ctx.localizer.Text("teleport.loading", "Loading the destination and checking for ground…"); break;
            case TeleportStatus::NoGround:
                hint = ctx.localizer.Text("teleport.ground", "No walkable ground found at the destination."); break;
            case TeleportStatus::Rejected:
                hint = ctx.localizer.Text("teleport.rejected", "The game did not accept the teleport request."); break;
            case TeleportStatus::Submitted:
                hint = ctx.localizer.Text("teleport.submitted", "Teleport requested."); break;
            }
            (ui->text_wrapped ? ui->text_wrapped : ui->text)(ui->user, StringView(hint));
        }
        const auto status = ctx.status.load();
        std::string message;
        switch (status) {
        case Status::Idle: return;
        case Status::Waiting: message = ctx.localizer.Text("map.waiting", "Waiting for the main world and map data…"); break;
        case Status::Unavailable: message = ctx.localizer.Text("map.unsupported", "The current game's map interface is unavailable."); break;
        case Status::Incomplete: message = ctx.localizer.Text("map.incomplete", "The game did not accept every marker. Turn the display off and try again."); break;
        case Status::Ready: {
            const std::array counts{std::to_string(ctx.accepted[0]), std::to_string(ctx.totals[0]),
                                    std::to_string(ctx.accepted[1]), std::to_string(ctx.totals[1])};
            message = ctx.localizer.Format("map.count", "Map registrations · Oracle Stones {0} / {1}, Twenty-One's Gifts {2} / {3}",
                                           std::array<std::string_view, 4>{counts[0], counts[1], counts[2], counts[3]});
            break;
        }
        }
        (ui->text_wrapped ? ui->text_wrapped : ui->text)(ui->user, StringView(message));
    } catch (...) {}
}

void ANOMALY_CALL Draw(void* raw, const AnomalyUiServiceV1* ui) noexcept {
    if (!raw || !ui || !ui->checkbox || !ui->text) return;
    auto& ctx = *static_cast<Context*>(raw);
    if (ctx.window && ctx.window_handle.id) {
        AnomalyWindowStateV1 state{sizeof(state)};
        if (!Succeeded(ctx.window->state(ctx.window->user, ctx.window_handle, &state)) || !state.open) {
            ctx.teleport_hotkey.CancelCapture();
            return;
        }
        int32_t visible{};
        if (!Succeeded(ctx.window->begin(ctx.window->user, ctx.window_handle, 0, &visible))) return;
        if (visible) DrawContent(ctx, ui);
        else ctx.teleport_hotkey.CancelCapture();
        static_cast<void>(ctx.window->end(ctx.window->user, ctx.window_handle));
        return;
    }
    if (!ctx.fallback_open || !ui->begin_window || !ui->end_window) {
        ctx.teleport_hotkey.CancelCapture();
        return;
    }
    try {
        const auto title = ctx.localizer.Text("window.title", "Better Map");
        UiWindow window(ui, title.c_str(), &ctx.fallback_open);
        if (window) DrawContent(ctx, ui);
        else ctx.teleport_hotkey.CancelCapture();
    } catch (...) {}
}
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    using namespace collectible_map;
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), 1, 0, StringView(Id), StringView(Title), StringView("b1ank"), StringView(Version),
                   Load, Start, Stop, Unload, Update, Draw, nullptr};
    return Ok();
}
