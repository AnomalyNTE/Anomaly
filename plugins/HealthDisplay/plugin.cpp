#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/profile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Settings {
    int enabled{1}, monsters{1}, clones{1}, bars{1}, numbers{1};
    float distance{100}, width{120}, height{7}, scale{0.8F};
};
struct HealthBar { std::array<double, 3> position{}; float fraction{}; std::string text; };
struct Layout {
    std::uint32_t count{}, chunk_size{}, item_size{}, item_object{}, object_class{}, object_index{},
        super{}, parameter_size{}, properties{}, struct_size{}, field_next{}, field_name{}, field_class{},
        field_class_name{}, property_size{}, property_dim{}, property_offset{}, bool_byte{}, bool_mask{},
        level_actors{}, component_bounds_parameters{};
};
struct Context {
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyUe5AhudServiceV1* ahud{};
    const AnomalyConfigServiceV1* config{};
    const AnomalyJsonServiceV1* json{};
    const AnomalySchedulerServiceV1* scheduler{};
    anomaly::sdk::Localizer localizer;
    Layout layout;
    std::uintptr_t object_array{};
    AnomalyGenerationHandleV1 subscription{};
    Settings editing;
    Clock::time_point next_refresh{};
} g;
std::atomic<std::shared_ptr<const Settings>> g_settings;
std::atomic<std::shared_ptr<const std::vector<HealthBar>>> g_bars;

// Render publishes settings; only Worker/Lifecycle access the config service.
std::mutex config_mutex;
struct Configuration {
    AnomalyGenerationHandleV1 schema{}, task{};
    std::shared_ptr<const Settings> pending;
    bool writable{}, stopping{};
    std::string error;
} configuration;
constexpr std::string_view kSettingsId = "settings";
constexpr std::string_view kSettingsSchema = R"json({
    "type":"object","additionalProperties":false,
    "required":["enabled","monsters","characters","bars","numbers","distance","width","height","scale"],
    "properties":{
        "enabled":{"type":"boolean"},"monsters":{"type":"boolean"},"characters":{"type":"boolean"},
        "bars":{"type":"boolean"},"numbers":{"type":"boolean"},
        "distance":{"type":"number","minimum":5,"maximum":500},
        "width":{"type":"number","minimum":40,"maximum":300},
        "height":{"type":"number","minimum":2,"maximum":30},
        "scale":{"type":"number","minimum":0.4,"maximum":2}
    }
})json";

AnomalyByteSpanV1 Bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

bool LoadConfiguration() {
    std::uint32_t version{};
    std::size_t size{};
    const auto id = anomaly::sdk::StringView(kSettingsId);
    const auto status = g.config->read(g.config->user, id, &version, {}, &size);
    if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) return true;
    if (status.code || version != 1 || !size || size > 4096) return false;
    std::string document(size, '\0');
    if (g.config->read(g.config->user, id, &version,
            {reinterpret_cast<std::uint8_t*>(document.data()), document.size()}, &size).code ||
        version != 1 || !size || size > document.size()) return false;
    document.resize(size);
    AnomalyGenerationHandleV1 root{};
    if (g.json->parse(g.json->user, anomaly::sdk::StringView(document), &root).code) return false;
    Settings loaded;
    const auto boolean = [&](std::string_view key, int& out) {
        AnomalyGenerationHandleV1 field{};
        if (g.json->object_find(g.json->user, root, anomaly::sdk::StringView(key), &field).code) return false;
        std::int32_t value{};
        const bool ok = !g.json->boolean_value(g.json->user, field, &value).code;
        g.json->release(g.json->user, field);
        if (ok) out = value != 0;
        return ok;
    };
    const auto number = [&](std::string_view key, float& out, double min, double max) {
        AnomalyGenerationHandleV1 field{};
        if (g.json->object_find(g.json->user, root, anomaly::sdk::StringView(key), &field).code) return false;
        double value{};
        const bool ok = !g.json->number_value(g.json->user, field, &value).code &&
            std::isfinite(value) && value >= min && value <= max;
        g.json->release(g.json->user, field);
        if (ok) out = static_cast<float>(value);
        return ok;
    };
    const bool ok = boolean("enabled", loaded.enabled) && boolean("monsters", loaded.monsters) &&
        boolean("characters", loaded.clones) && boolean("bars", loaded.bars) && boolean("numbers", loaded.numbers) &&
        number("distance", loaded.distance, 5, 500) && number("width", loaded.width, 40, 300) &&
        number("height", loaded.height, 2, 30) && number("scale", loaded.scale, 0.4, 2);
    g.json->release(g.json->user, root);
    if (ok) g.editing = loaded;
    return ok;
}

void InitializeConfiguration(const anomaly::sdk::Host& host) {
    g.config = host.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID).get();
    g.json = host.Query<AnomalyJsonServiceV1>(ANOMALY_JSON_SERVICE_V1_ID).get();
    g.scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get();
    if (g.config && g.config->struct_size >= offsetof(AnomalyConfigServiceV1, write_atomic) + sizeof(g.config->write_atomic) &&
        g.config->register_schema && g.config->read && g.config->write_atomic &&
        g.json && g.json->struct_size >= sizeof(*g.json) && g.json->parse && g.json->release &&
        g.json->object_find && g.json->boolean_value && g.json->number_value &&
        g.scheduler && g.scheduler->struct_size >= sizeof(*g.scheduler) && g.scheduler->schedule && g.scheduler->cancel &&
        !g.config->register_schema(g.config->user, anomaly::sdk::StringView(kSettingsId), 1,
            Bytes(kSettingsSchema), &configuration.schema).code && configuration.schema.id && LoadConfiguration()) {
        configuration.writable = true;
        return;
    }
    // Preserve unreadable settings instead of overwriting them with defaults.
    configuration.error = g.localizer.Text("config.load_failed", "Settings could not be loaded; automatic saving is disabled");
}

bool SaveConfiguration() {
    std::shared_ptr<const Settings> settings;
    {
        std::scoped_lock lock(config_mutex);
        if (!configuration.writable || !configuration.pending) return true;
        settings = configuration.pending;
    }
    char document[512]{};
    const auto& s = *settings;
    const int length = std::snprintf(document, sizeof(document),
        "{\"enabled\":%s,\"monsters\":%s,\"characters\":%s,\"bars\":%s,\"numbers\":%s,"
        "\"distance\":%.6g,\"width\":%.6g,\"height\":%.6g,\"scale\":%.6g}",
        s.enabled ? "true" : "false", s.monsters ? "true" : "false", s.clones ? "true" : "false",
        s.bars ? "true" : "false", s.numbers ? "true" : "false", s.distance, s.width, s.height, s.scale);
    const bool saved = length > 0 && static_cast<std::size_t>(length) < sizeof(document) &&
        !g.config->write_atomic(g.config->user, anomaly::sdk::StringView(kSettingsId), 1,
            Bytes({document, static_cast<std::size_t>(length)})).code;
    std::scoped_lock lock(config_mutex);
    if (saved) {
        if (configuration.pending == settings) configuration.pending.reset();
        configuration.error.clear();
    } else configuration.error = g.localizer.Text("config.save_failed", "Settings could not be saved");
    return saved;
}

void ANOMALY_CALL SaveConfigurationWorker(void*, AnomalyGenerationHandleV1) noexcept;
void ScheduleConfigurationLocked() {
    if (!configuration.writable || configuration.stopping || configuration.task.id) return;
    if (g.scheduler->schedule(g.scheduler->user, 500, SaveConfigurationWorker, nullptr, &configuration.task).code)
        configuration.error = g.localizer.Text("config.save_failed", "Settings could not be saved");
}
void ANOMALY_CALL SaveConfigurationWorker(void*, AnomalyGenerationHandleV1) noexcept {
    try {
        const bool saved = SaveConfiguration();
        std::scoped_lock lock(config_mutex);
        configuration.task = {};
        if (saved && configuration.pending) ScheduleConfigurationLocked();
    } catch (...) {
        std::scoped_lock lock(config_mutex);
        configuration.task = {};
    }
}

template<class T> bool Read(std::uintptr_t address, T& value) {
    return g.core->read_memory(g.core->user, address,
        {reinterpret_cast<std::uint8_t*>(&value), sizeof(value)}).code == ANOMALY_STATUS_V1_OK;
}

// Convert only live, serial-checked UObject handles. Actor frame handles are a
// different domain and are never passed directly to ProcessEvent.
std::uintptr_t Pointer(AnomalyGenerationHandleV1 handle) {
    AnomalyUe5ObjectSnapshotV1 check{sizeof(check)};
    if (!handle.id || g.objects->snapshot_by_handle(g.objects->user, handle, &check).code) return 0;
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    std::int32_t count{};
    std::uintptr_t chunks{}, chunk{}, object{};
    std::uint32_t actual_index{};
    if (!Read(g.object_array + g.layout.count, count) || count <= 0 || index >= static_cast<std::uint32_t>(count) ||
        !Read(g.object_array, chunks) || !chunks ||
        !Read(chunks + sizeof(std::uintptr_t) * (index / g.layout.chunk_size), chunk) || !chunk ||
        !Read(chunk + g.layout.item_size * static_cast<std::uintptr_t>(index % g.layout.chunk_size) +
            g.layout.item_object, object) || !object ||
        !Read(object + g.layout.object_index, actual_index) || actual_index != index) return 0;
    return object;
}

AnomalyGenerationHandleV1 Find(const char* path) {
    AnomalyGenerationHandleV1 result{};
    if (g.objects->find_exact(g.objects->user, anomaly::sdk::StringView(path), &result).code) return {};
    return result;
}

bool IsSubclass(std::uintptr_t type, std::uintptr_t base) {
    if (!base) return false;
    for (int depth = 0; type && depth < 64; ++depth) {
        if (type == base) return true;
        if (!Read(type + g.layout.super, type)) return false;
    }
    return false;
}
bool IsA(std::uintptr_t object, std::uintptr_t base) {
    std::uintptr_t type{};
    return Read(object + g.layout.object_class, type) && IsSubclass(type, base);
}

// ProcessEvent's parameter span ends after SphereRadius; trailing C++ alignment
// padding is not part of UFunction::ParmsSize.
struct ComponentBounds {
    std::uintptr_t component{};
    std::array<double, 3> origin{}, extent{};
    float radius{};
};
constexpr std::size_t kComponentBoundsParameters = offsetof(ComponentBounds, radius) + sizeof(float);
static_assert(sizeof(ComponentBounds) == 64 && kComponentBoundsParameters == 60);

struct Functions {
    AnomalyGenerationHandleV1 hp{}, maximum{}, location{}, clone{}, level{}, half_height{}, component_bounds{}, library{};
    std::uintptr_t ai_class{}, player_class{}, world_class{}, level_class{}, capsule_class{}, scene_class{};
    std::uint32_t world_levels{}, owning_world{}, capsule{}, mesh{}, relative_scale{}, hidden{};
    std::uint8_t hidden_mask{};
};
bool Function(const char* path, std::size_t size, AnomalyGenerationHandleV1& handle) {
    handle = Find(path);
    const auto pointer = Pointer(handle);
    std::uint16_t actual{};
    return pointer && Read(pointer + g.layout.parameter_size, actual) && actual == size;
}
// Fixed ProcessEvent ABI from HTAbilityCharacter::GetHPMax: bool input, float return.
struct Maximum { std::uint8_t fixed{}; float value{}; };
static_assert(sizeof(Maximum) == 8 && offsetof(Maximum, value) == 4);
bool Named(std::uint32_t id, const char* expected) {
    char text[96]{}; std::size_t size = sizeof(text);
    return !g.names->resolve_utf8(g.names->user, id, text, &size).code && std::strcmp(text, expected) == 0;
}

// Resolve shared engine properties once per object-registry generation. Names,
// field types, widths and owning struct bounds are checked before caching them.
bool Property(std::uintptr_t owner, const char* name, const char* type, std::uint32_t width,
              std::uint32_t& offset, std::uint8_t* mask = nullptr) {
    std::int32_t owner_size{};
    std::uintptr_t field{};
    if (!Read(owner + g.layout.struct_size, owner_size) || owner_size <= 0 ||
        !Read(owner + g.layout.properties, field)) return false;
    for (int n = 0; field && n < 1024; ++n) {
        std::uint32_t id{};
        if (!Read(field + g.layout.field_name, id)) return false;
        if (Named(id, name)) {
            std::uintptr_t field_class{};
            std::uint32_t type_id{};
            std::int32_t size{}, dimension{}, at{};
            if (!Read(field + g.layout.field_class, field_class) || !field_class ||
                !Read(field_class + g.layout.field_class_name, type_id) || !Named(type_id, type) ||
                !Read(field + g.layout.property_size, size) || size != static_cast<std::int32_t>(width) ||
                !Read(field + g.layout.property_dim, dimension) || dimension != 1 ||
                !Read(field + g.layout.property_offset, at) || at < 0 || at > owner_size - size) return false;
            offset = static_cast<std::uint32_t>(at);
            if (mask) {
                std::uint8_t byte{};
                if (!Read(field + g.layout.bool_byte, byte) || byte != 0 ||
                    !Read(field + g.layout.bool_mask, *mask) || !*mask) return false;
            }
            return true;
        }
        if (!Read(field + g.layout.field_next, field)) return false;
    }
    return false;
}

bool ResolveFunctions(Functions& f) {
    f.ai_class = Pointer(Find("/Script/HTGame.HTAICharacter"));
    f.player_class = Pointer(Find("/Script/HTGame.HTPlayerCharacter"));
    f.world_class = Pointer(Find("/Script/Engine.World"));
    f.level_class = Pointer(Find("/Script/Engine.Level"));
    f.capsule_class = Pointer(Find("/Script/Engine.CapsuleComponent"));
    const auto actor = Pointer(Find("/Script/Engine.Actor"));
    const auto character = Pointer(Find("/Script/Engine.Character"));
    const bool ready = f.ai_class && f.player_class && f.world_class && f.level_class && f.capsule_class && actor && character &&
        Property(f.world_class, "Levels", "ArrayProperty", 16, f.world_levels) &&
        Property(f.level_class, "OwningWorld", "ObjectProperty", sizeof(std::uintptr_t), f.owning_world) &&
        g.layout.level_actors + 16 <= f.owning_world &&
        Property(character, "CapsuleComponent", "ObjectProperty", sizeof(std::uintptr_t), f.capsule) &&
        Property(actor, "bHidden", "BoolProperty", 1, f.hidden, &f.hidden_mask) &&
        Function("/Script/HTGame.HTAbilityCharacter:GetHP", sizeof(float), f.hp) &&
        Function("/Script/HTGame.HTAbilityCharacter:GetHPMax", sizeof(Maximum), f.maximum) &&
        Function("/Script/Engine.Actor:K2_GetActorLocation", sizeof(double) * 3, f.location) &&
        Function("/Script/Engine.Actor:GetLevel", sizeof(std::uintptr_t), f.level) &&
        Function("/Script/Engine.CapsuleComponent:GetScaledCapsuleHalfHeight", sizeof(float), f.half_height) &&
        Function("/Script/HTGame.HTCharacter:IsSummonPlayerCharacter", sizeof(std::uint8_t), f.clone);
    if (!ready) return false;
    f.scene_class = Pointer(Find("/Script/Engine.SceneComponent"));
    f.library = Find("/Script/Engine.Default__KismetSystemLibrary");
    if (f.scene_class && f.library.id && g.layout.component_bounds_parameters == kComponentBoundsParameters &&
        Property(character, "Mesh", "ObjectProperty", sizeof(std::uintptr_t), f.mesh) &&
        Property(f.scene_class, "RelativeScale3D", "StructProperty", sizeof(double) * 3, f.relative_scale)) {
        if (!Function("/Script/Engine.KismetSystemLibrary:GetComponentBounds",
                g.layout.component_bounds_parameters, f.component_bounds)) f.component_bounds = {};
        g.core->log(g.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(
            f.component_bounds.id ? "HealthDisplay: mesh bounds anchor ready" :
                "HealthDisplay: mesh bounds unavailable; using capsule anchor"));
    }
    return true;
}
template<class T> bool Invoke(AnomalyGenerationHandleV1 object, AnomalyGenerationHandleV1 fn, T& params) {
    return g.events->invoke(g.events->user, object, fn, &params, sizeof(params)).code == ANOMALY_STATUS_V1_OK;
}

struct Target {
    AnomalyGenerationHandleV1 handle{};
    int kind{};
    double head_height{};
    std::uintptr_t pointer{};
};
struct Runtime {
    Functions functions;
    std::uint64_t object_generation{}, world_generation{};
    bool functions_ready{}, discovering{};
    Clock::time_point next_discovery{};
    std::uintptr_t world{};
    AnomalyGenerationHandleV1 world_handle{};
    std::int32_t level_index{}, actor_index{};
    std::unordered_map<std::uint64_t, int> kinds;
    std::vector<Target> pending, targets;
    std::uint32_t samples{}, discovery_calls{};
    double sample_sum{}, sample_max{}, discovery_sum{}, discovery_max{};
} runtime;

double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

AnomalyGenerationHandleV1 Handle(std::uintptr_t pointer) {
    std::uint32_t index{};
    AnomalyUe5ObjectSnapshotV1 object{sizeof(object)};
    if (!pointer || !Read(pointer + g.layout.object_index, index) ||
        g.objects->snapshot_at(g.objects->user, index, &object).code || Pointer(object.handle) != pointer) return {};
    return object.handle;
}

// Borrowed UE TArray header; no UE allocation is owned by this plugin.
struct ObjectArray { std::uintptr_t data{}; std::int32_t count{}, capacity{}; };
static_assert(sizeof(ObjectArray) == 16);
bool Array(std::uintptr_t address, std::int32_t limit, ObjectArray& out) {
    return Read(address, out) && out.count >= 0 && out.count <= limit && out.capacity >= out.count &&
        out.capacity <= limit && (!out.count || out.data);
}

void DiscoverTarget(std::uintptr_t pointer, std::uintptr_t level) {
    std::uintptr_t type{};
    if (!pointer || !Read(pointer + g.layout.object_class, type) || !type) return;
    auto [kind, inserted] = runtime.kinds.try_emplace(type, 0);
    if (inserted) {
        if (!Handle(type).id) return;
        if (IsSubclass(type, runtime.functions.ai_class)) kind->second = 1;
        else if (IsSubclass(type, runtime.functions.player_class)) kind->second = 2;
    }
    if (!kind->second) return;
    const auto handle = Handle(pointer);
    if (!handle.id || !IsA(pointer, kind->second == 1 ? runtime.functions.ai_class : runtime.functions.player_class)) return;
    std::uintptr_t actual_level{};
    if (!Invoke(handle, runtime.functions.level, actual_level) || actual_level != level) return;
    if (kind->second == 2) {
        std::uint8_t clone{};
        if (!Invoke(handle, runtime.functions.clone, clone) || !clone) return;
    }
    std::uintptr_t capsule{};
    float height{};
    if (!Read(pointer + runtime.functions.capsule, capsule) || !capsule ||
        !IsA(capsule, runtime.functions.capsule_class)) return;
    const auto capsule_handle = Handle(capsule);
    if (!capsule_handle.id || !Invoke(capsule_handle, runtime.functions.half_height, height) ||
        !std::isfinite(height) || height <= 0) return;
    runtime.pending.push_back({handle, kind->second, height + 20.0, pointer});
}

bool BeginDiscovery() {
    AnomalyGenerationHandleV1 pawn{}, controller{};
    std::uintptr_t level{};
    if (g.player->get_object_handles(g.player->user, &pawn, &controller).code || !pawn.id ||
        !Invoke(pawn, runtime.functions.level, level) || !Handle(level).id || !IsA(level, runtime.functions.level_class) ||
        !Read(level + runtime.functions.owning_world, runtime.world) || !runtime.world) return false;
    runtime.world_handle = Handle(runtime.world);
    if (!runtime.world_handle.id || !IsA(runtime.world, runtime.functions.world_class)) return false;
    runtime.level_index = runtime.actor_index = 0;
    runtime.pending.clear(); runtime.kinds.clear();
    return true;
}

void Discover(Clock::time_point now) {
    const auto started = Clock::now();
    if (!runtime.discovering) {
        if (now < runtime.next_discovery) return;
        runtime.next_discovery = now + std::chrono::seconds(1);
        if (!runtime.functions_ready) {
            runtime.functions_ready = ResolveFunctions(runtime.functions);
            if (!runtime.functions_ready) return;
        }
        if (!BeginDiscovery()) { runtime.targets.clear(); return; }
        runtime.discovering = true;
    }
    // Read only scene actor references, never request the host's synchronous
    // all-level geometry/name snapshot. Re-read array headers each slice so
    // streaming and actor-array reallocations cannot leave borrowed stale data.
    AnomalyUe5ObjectSnapshotV1 world{sizeof(world)};
    ObjectArray levels;
    if (g.objects->snapshot_by_handle(g.objects->user, runtime.world_handle, &world).code ||
        !Array(runtime.world + runtime.functions.world_levels, 4096, levels)) {
        runtime.targets.clear(); runtime.discovering = false; return;
    }
    do {
        if (runtime.level_index >= levels.count) {
            runtime.targets.swap(runtime.pending);
            runtime.discovering = false;
            return;
        }
        std::uintptr_t level{};
        ObjectArray actors;
        if (!Read(levels.data + sizeof(std::uintptr_t) * runtime.level_index, level) ||
            !Handle(level).id || !IsA(level, runtime.functions.level_class) ||
            !Array(level + g.layout.level_actors, 1048576, actors)) {
            ++runtime.level_index; runtime.actor_index = 0; continue;
        }
        // Process a small batch before checking the time budget. No saved array
        // storage or naked actor pointer is reused across Game ticks here.
        for (int batch = 0; batch < 16 && runtime.actor_index < actors.count; ++batch) {
            std::uintptr_t actor{};
            if (Read(actors.data + sizeof(std::uintptr_t) * runtime.actor_index++, actor)) DiscoverTarget(actor, level);
        }
        if (runtime.actor_index >= actors.count) { ++runtime.level_index; runtime.actor_index = 0; }
    } while (Milliseconds(started) < 0.5);
}

void UpdateAnchor(const Target& target, std::array<double, 3>& position) {
    const auto& f = runtime.functions;
    double height = target.head_height;
    std::uintptr_t mesh{};
    // Re-read the owned mesh every sample: character construction may replace it,
    // and scale can change after discovery without a new actor being spawned.
    if (f.mesh && f.scene_class && Read(target.pointer + f.mesh, mesh) && mesh &&
        IsA(mesh, f.scene_class) && Handle(mesh).id) {
        ComponentBounds bounds{mesh};
        if (f.library.id && f.component_bounds.id &&
            !g.events->invoke(g.events->user, f.library, f.component_bounds,
                &bounds, g.layout.component_bounds_parameters).code &&
            std::ranges::all_of(bounds.origin, [](double v) { return std::isfinite(v); }) &&
            std::ranges::all_of(bounds.extent, [](double v) { return std::isfinite(v) && v >= 0; }) &&
            bounds.extent[2] > 0) {
            const double top = bounds.origin[2] + bounds.extent[2] + 12.0;
            if (std::isfinite(top)) {
                position = bounds.origin;
                position[2] = top;
                return;
            }
        }
        std::array<double, 3> scale{};
        if (f.relative_scale && Read(mesh + f.relative_scale, scale) &&
            std::isfinite(scale[2]) && scale[2] > 0 && std::isfinite(height * scale[2]))
            height *= scale[2];
    }
    position[2] += height;
}

void Collect(const Settings& settings) {
    AnomalyNteCameraSnapshotV1 camera{sizeof(camera)};
    if (g.player->camera_snapshot(g.player->user, &camera).code ||
        !(camera.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) || camera.world.generation != runtime.world_generation) {
        g_bars.store({}); return;
    }
    auto bars = std::make_shared<std::vector<HealthBar>>();
    bars->reserve(runtime.targets.size());
    for (const auto& target : runtime.targets) {
        if (target.kind == 1 ? !settings.monsters : !settings.clones) continue;
        HealthBar bar;
        float hp{};
        Maximum maximum;
        // Invoke checks the saved UObject identity before reading its cached hidden field.
        std::uint8_t hidden{};
        if (!Invoke(target.handle, runtime.functions.location, bar.position) ||
            !Read(target.pointer + runtime.functions.hidden, hidden) || (hidden & runtime.functions.hidden_mask) ||
            !std::ranges::all_of(bar.position, [](double v) { return std::isfinite(v); }) ||
            std::hypot(bar.position[0] - camera.position[0], bar.position[1] - camera.position[1],
                bar.position[2] - camera.position[2]) > settings.distance * 100.0) continue;
        if (!Invoke(target.handle, runtime.functions.hp, hp) || !Invoke(target.handle, runtime.functions.maximum, maximum) ||
            !std::isfinite(hp) || !std::isfinite(maximum.value) || hp <= 0 || maximum.value <= 0) continue;
        UpdateAnchor(target, bar.position);
        bar.fraction = std::clamp(hp / maximum.value, 0.0F, 1.0F);
        if (settings.numbers) {
            char current[32]{}, total[32]{};
            std::snprintf(current, sizeof(current), "%.0f", hp);
            std::snprintf(total, sizeof(total), "%.0f", maximum.value);
            const std::array<std::string_view, 2> args{current, total};
            bar.text = g.localizer.Format("health.values", "{0} / {1}", args);
        }
        bars->push_back(std::move(bar));
    }
    g_bars.store(std::move(bars));
}

void ANOMALY_CALL DrawBars(void*, const AnomalyUe5AhudFrameV1* frame) noexcept {
    try {
        const auto settings = g_settings.load();
        const auto bars = g_bars.load();
        if (!settings || !settings->enabled || !bars || !frame || frame->struct_size < sizeof(*frame) ||
            !frame->project || !frame->draw_rect || !frame->draw_text || !frame->measure_text) return;
        for (const auto& bar : *bars) {
            float screen[2]{}; double depth{};
            if (!frame->project(frame->user, bar.position.data(), screen, &depth) || depth <= 0 ||
                screen[0] < 0 || screen[0] > frame->viewport_width || screen[1] < 0 || screen[1] > frame->viewport_height) continue;
            const float x = screen[0] - settings->width * 0.5F, y = screen[1];
            if (settings->bars) {
                frame->draw_rect(frame->user, x - 1, y - 1, settings->width + 2, settings->height + 2, ANOMALY_RGBA_V1(15, 18, 24, 220));
                frame->draw_rect(frame->user, x, y, settings->width * bar.fraction, settings->height, ANOMALY_RGBA_V1(225, 70, 80, 245));
            }
            if (settings->numbers) {
                float width{}, height{};
                const auto text = anomaly::sdk::StringView(bar.text);
                if (frame->measure_text(frame->user, text, settings->scale, &width, &height))
                    frame->draw_text(frame->user, text, screen[0] - width * 0.5F, y - height - 3,
                        ANOMALY_RGBA_V1(255, 255, 255, 255), settings->scale);
            }
        }
    } catch (...) {}
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    try {
        if (!context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
        const anomaly::sdk::Host view(host);
        g.localizer = anomaly::sdk::Localizer(host);
        g.core = view.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        g.objects = view.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        g.events = view.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID).get();
        g.names = view.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
        g.session = view.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
        g.player = view.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID).get();
        g.ahud = view.Query<AnomalyUe5AhudServiceV1>(ANOMALY_UE5_AHUD_SERVICE_V1_ID).get();
        if (!g.core || g.core->struct_size < sizeof(*g.core) || !g.core->read_memory || !g.core->log ||
            !g.objects || g.objects->struct_size < sizeof(*g.objects) || !g.objects->find_exact ||
            !g.objects->snapshot_at || !g.objects->snapshot_by_handle || !g.objects->generation ||
            !g.session || g.session->struct_size < sizeof(*g.session) || !g.session->snapshot ||
            !g.player || g.player->struct_size < offsetof(AnomalyNtePlayerServiceV1, get_object_handles) + sizeof(g.player->get_object_handles) ||
            !g.player->camera_snapshot || !g.player->get_object_handles ||
            !g.events || g.events->struct_size < sizeof(*g.events) || !g.events->invoke ||
            !g.names || g.names->struct_size < sizeof(*g.names) || !g.names->resolve_utf8 ||
            !g.ahud || g.ahud->struct_size < sizeof(*g.ahud) || !g.ahud->subscribe || !g.ahud->unsubscribe)
            return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
        const auto profile = anomaly::sdk::LoadProfile(view, "compatibility.profile.json",
            {"nte", "HTGame.exe", ".text", "health-display-v1"});
        if (!profile) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
        const auto& p = profile.value();
        g.layout = {p.Value("objectArrayCount", 128, 4), p.Value("objectsPerChunk", 1048576),
            p.Value("objectItemSize", 256, 8), p.Value("objectItemObject", 248, 8),
            p.Value("objectClass", 128, 8), p.Value("objectInternalIndex", 128, 4),
            p.Value("structSuper", 256, 8), p.Value("functionParameterSize", 512, 2),
            p.Value("structProperties", 256, 8), p.Value("structSize", 256, 4), p.Value("fieldNext", 256, 8),
            p.Value("fieldName", 256, 4), p.Value("fieldClass", 256, 8), p.Value("fieldClassName", 256, 4),
            p.Value("propertyElementSize", 256, 4), p.Value("propertyArrayDim", 256, 4), p.Value("propertyOffset", 256, 4),
            p.Value("boolByteOffset", 256), p.Value("boolFieldMask", 256), p.Value("levelActors", 4096, 8),
            p.Value("componentBoundsParameters", sizeof(ComponentBounds))};
        if (!g.layout.chunk_size || !g.layout.item_size || g.layout.item_object + sizeof(std::uintptr_t) > g.layout.item_size ||
            g.layout.object_class < g.layout.object_index + sizeof(std::uint32_t))
            return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
        const auto signature = view.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
        const auto resolved = anomaly::sdk::ResolveProfileSymbol(signature.get(), g.core, p, "object-array-reference");
        if (!resolved || resolved.candidate->values.at("displacementOffset") != 3 ||
            resolved.candidate->values.at("instructionSize") != 7) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
        std::int32_t displacement{};
        if (!Read(resolved.address + resolved.candidate->values.at("displacementOffset"), displacement))
            return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
        g.object_array = resolved.address + resolved.candidate->values.at("instructionSize") + displacement;
        InitializeConfiguration(view);
        *context = &g;
        return anomaly::sdk::Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, {}}; }
}
AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    try {
        g.next_refresh = {}; runtime = {};
        configuration.stopping = false;
        g_settings.store(std::make_shared<Settings>(g.editing));
        return g.ahud->subscribe(g.ahud->user, DrawBars, nullptr, &g.subscription);
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, {}}; }
}
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    try {
        // The host drains Worker/Render callbacks before on_stop and keeps schemas
        // available for this final Lifecycle flush.
        configuration.stopping = true;
        if (g.scheduler && configuration.task.id)
            g.scheduler->cancel(g.scheduler->user, configuration.task);
        configuration.task = {};
        if (!SaveConfiguration())
            g.core->log(g.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView("HealthDisplay: configuration flush failed"));
        if (g.subscription.id) {
            const auto status = g.ahud->unsubscribe(g.ahud->user, g.subscription);
            if (status.code != ANOMALY_STATUS_V1_OK && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
            g.subscription = {};
        }
        g_bars.store({}); g_settings.store({}); runtime = {};
        return anomaly::sdk::Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, {}}; }
}
void ANOMALY_CALL Unload(void*) { configuration = {}; g = {}; }
void ANOMALY_CALL Update(void*, double) noexcept {
    try {
        const auto settings = g_settings.load();
        if (!settings || !settings->enabled || (!settings->bars && !settings->numbers)) { g_bars.store({}); return; }
        AnomalyNteSessionSnapshotV1 session{sizeof(session)};
        if (g.session->snapshot(g.session->user, &session).code || session.state != ANOMALY_NTE_SESSION_V1_WORLD_READY) {
            runtime = {}; g_bars.store({}); return;
        }
        const auto generation = g.objects->generation(g.objects->user);
        if (runtime.world_generation != session.world.generation || runtime.object_generation != generation) {
            runtime = {};
            runtime.world_generation = session.world.generation;
            runtime.object_generation = generation;
            g_bars.store({});
        }
        const auto now = Clock::now();
        if (runtime.discovering || now >= runtime.next_discovery) {
            const auto started = Clock::now();
            Discover(now);
            const auto elapsed = Milliseconds(started);
            if (runtime.samples < 100) {
                ++runtime.discovery_calls;
                runtime.discovery_sum += elapsed;
                runtime.discovery_max = std::max(runtime.discovery_max, elapsed);
            }
        }
        if (now < g.next_refresh || !runtime.functions_ready) return;
        g.next_refresh = now + std::chrono::milliseconds(100);
        const auto started = Clock::now();
        Collect(*settings);
        if (runtime.samples < 100) {
            runtime.sample_sum += Milliseconds(started);
            runtime.sample_max = std::max(runtime.sample_max, Milliseconds(started));
            if (++runtime.samples == 100) {
                char message[320]{};
                std::snprintf(message, sizeof(message),
                    "HealthDisplay: 100 samples avg=%.3fms max=%.3fms; discovery calls=%u avg=%.3fms max=%.3fms targets=%zu",
                    runtime.sample_sum / runtime.samples, runtime.sample_max, runtime.discovery_calls,
                    runtime.discovery_calls ? runtime.discovery_sum / runtime.discovery_calls : 0.0,
                    runtime.discovery_max, runtime.targets.size());
                g.core->log(g.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, anomaly::sdk::StringView(message));
            }
        }
    } catch (...) { g_bars.store({}); }
}
void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) noexcept {
    try {
        if (!ui || ui->struct_size < offsetof(AnomalyUiServiceV1, slider_float) + sizeof(ui->slider_float) ||
            !ui->begin_window || !ui->end_window || !ui->checkbox || !ui->slider_float || !ui->text) return;
        int open = 1;
        const auto title = g.localizer.Label("window.title", "Health Display", "health-display");
        anomaly::sdk::UiWindow window(ui, title, &open);
        if (!window) return;
        bool changed{};
        const auto check = [&](const std::string& label, int& value) {
            changed |= ui->checkbox(ui->user, anomaly::sdk::StringView(label), &value) != 0;
        };
        check(g.localizer.Text("display.enabled", "Enable health display"), g.editing.enabled);
        check(g.localizer.Text("display.monsters", "Monsters"), g.editing.monsters);
        check(g.localizer.Text("display.clones", "Characters"), g.editing.clones);
        check(g.localizer.Text("display.bars", "Show health bars"), g.editing.bars);
        check(g.localizer.Text("display.numbers", "Show current / maximum HP"), g.editing.numbers);
        const auto slider = [&](const std::string& label, float& value, float min, float max) {
            changed |= ui->slider_float(ui->user, anomaly::sdk::StringView(label), &value, min, max) != 0;
        };
        slider(g.localizer.Text("display.distance", "Maximum distance (m)"), g.editing.distance, 5, 500);
        slider(g.localizer.Text("display.width", "Bar width"), g.editing.width, 40, 300);
        slider(g.localizer.Text("display.height", "Bar height"), g.editing.height, 2, 30);
        slider(g.localizer.Text("display.scale", "Text size"), g.editing.scale, 0.4F, 2);
        if (changed) {
            auto settings = std::make_shared<Settings>(g.editing);
            g_settings.store(settings);
            std::scoped_lock lock(config_mutex);
            configuration.pending = std::move(settings);
            ScheduleConfigurationLocked();
        }
        std::string error;
        { std::scoped_lock lock(config_mutex); error = configuration.error; }
        if (!error.empty()) ui->text(ui->user, anomaly::sdk::StringView(error));
        const auto bars = g_bars.load();
        const auto count = std::to_string(bars ? bars->size() : 0);
        const std::array<std::string_view, 1> args{count};
        ui->text(ui->user, anomaly::sdk::StringView(g.localizer.Format("display.count", "Tracked entities: {0}", args)));
    } catch (...) {}
}
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.health-display"), anomaly::sdk::StringView("Health Display"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"), Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
