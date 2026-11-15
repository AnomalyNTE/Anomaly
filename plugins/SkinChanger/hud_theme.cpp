#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/services/nte.h>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include "hud_theme.hpp"
#include "localization.hpp"
#include "profile.hpp"
#include "plate_access.hpp"

namespace {
std::atomic_bool enabled{};
std::atomic_bool waiting{};
bool restore_pending{};
Clock::time_point next_check{};
AnomalyGenerationHandleV1 last_world{}, last_controller{}, theme_subsystem{};
FName preview_theme{};

const AnomalyUe5ObjectsServiceV1* objects{};
const AnomalyUe5NamesServiceV1* names{};
const AnomalyUe5ProcessEventServiceV1* events{};
const AnomalyNtePlayerServiceV1* player{};
const AnomalyNteSessionServiceV1* session{};
const AnomalyConfigServiceV1* config{};
const AnomalySchedulerServiceV1* scheduler{};
std::atomic_bool settings_dirty{};
std::mutex settings_save_mutex;
bool available{};

constexpr auto settings_id = "hud-theme";
constexpr uint32_t settings_version = 1;
constexpr std::string_view settings_schema = R"json({
    "type":"object",
    "properties":{"enabled":{"type":"boolean"}},
    "required":["enabled"],
    "additionalProperties":false
})json";

AnomalyByteSpanV1 Bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

bool LoadSettings(const Host& host) noexcept {
    const auto json = host.Query<AnomalyJsonServiceV1>(ANOMALY_JSON_SERVICE_V1_ID).get();
    AnomalyGenerationHandleV1 schema{};
    if (!config || !config->register_schema || !config->read || !config->write_atomic ||
        !scheduler || !scheduler->schedule || !json || !json->parse || !json->object_find ||
        !json->boolean_value || !json->release ||
        !Succeeded(config->register_schema(config->user, StringView(settings_id), settings_version,
            Bytes(settings_schema), &schema))) return false;

    std::array<uint8_t, 128> buffer{};
    size_t size = buffer.size();
    uint32_t version{};
    const auto result = config->read(config->user, StringView(settings_id), &version,
        {buffer.data(), buffer.size()}, &size);
    if (Succeeded(result) && version == settings_version && size <= buffer.size()) {
        AnomalyGenerationHandleV1 document{}, field{};
        const std::string_view text(reinterpret_cast<const char*>(buffer.data()), size);
        if (Succeeded(json->parse(json->user, StringView(text), &document))) {
            int32_t value{};
            if (Succeeded(json->object_find(json->user, document, StringView("enabled"), &field))) {
                if (Succeeded(json->boolean_value(json->user, field, &value))) enabled.store(value != 0);
                json->release(json->user, field);
            }
            json->release(json->user, document);
        }
    }
    return true;
}

bool SaveSettings() noexcept {
    std::scoped_lock lock(settings_save_mutex);
    if (!settings_dirty.exchange(false)) return true;
    const auto data = enabled.load() ? R"({"enabled":true})" : R"({"enabled":false})";
    if (Succeeded(config->write_atomic(config->user, StringView(settings_id), settings_version, Bytes(data))))
        return true;
    settings_dirty.store(true);
    return false;
}

void ANOMALY_CALL SaveSettingsTask(void*, AnomalyGenerationHandleV1) {
    static_cast<void>(SaveSettings());
}

bool SameHandle(AnomalyGenerationHandleV1 first, AnomalyGenerationHandleV1 second) noexcept {
    return first.id == second.id && first.generation == second.generation;
}

struct HudLayout {
    uint32_t fashion_theme_id{};
    uint32_t theme_delegate{};
    uint32_t shortcut_row{};
    uint32_t panel_slots{};
    uint32_t slot_content{};
    uint32_t button_border{};
} hud_layout{};

bool LoadLayout() noexcept {
    try {
        const auto& profile = accessory::profile::compatibility;
        hud_layout = {
            profile.Value("hud.FashionThemeId", 65535, alignof(uint32_t)),
            profile.Value("hud.ThemeDelegate", 65535, alignof(uintptr_t)),
            profile.Value("hud.ShortcutRow", 65535, alignof(uintptr_t)),
            profile.Value("hud.PanelSlots", 65535, alignof(uintptr_t)),
            profile.Value("hud.SlotContent", 65535, alignof(uintptr_t)),
            profile.Value("hud.ButtonBorder", 65535, alignof(uintptr_t)),
        };
        return true;
    } catch (...) {
        return false;
    }
}

struct EngineFunction {
    const char* path;
    const char* name;
    uint8_t parameter_count;
    uint16_t parameter_size;
    AnomalyGenerationHandleV1 handle{};
};

AnomalyGenerationHandleV1 Find(const char* path) {
    AnomalyGenerationHandleV1 handle{};
    if (!Succeeded(objects->find_exact(objects->user, StringView(path), &handle))) return {};
    return handle;
}

bool Call(const Reader& reader, AnomalyGenerationHandleV1 receiver, EngineFunction& function,
        void* params, size_t size) {
    if (!receiver.id) return false;
    if (!function.handle.id) {
        const auto handle = Find(function.path);
        if (!reader.Function(Resolve(reader, objects, handle), function.name,
                function.parameter_count, function.parameter_size)) return false;
        function.handle = handle;
    }
    if (Succeeded(events->invoke(events->user, receiver, function.handle, params, size))) return true;
    function.handle = {};
    return false;
}

template<class Params>
bool Call(const Reader& reader, AnomalyGenerationHandleV1 receiver, EngineFunction& function, Params& params) {
    return Call(reader, receiver, function, &params, sizeof(params));
}

EngineFunction lookup{
    "/Script/Engine.SubsystemBlueprintLibrary.GetLocalPlayerSubSystemFromPlayerController",
    "GetLocalPlayerSubSystemFromPlayerController", 3, 24};
EngineFunction set_theme{
    "/Script/HTGame.HTUIThemeSubsystem.SetThemeID", "SetThemeID", 2, 9};
EngineFunction get_theme{
    "/Script/HTGame.HTUIThemeSubsystem.GetCurrentThemeID", "GetCurrentThemeID", 1, 8};
EngineFunction refresh_theme{
    "/Script/HTGame.HTUIThemeSubsystem.RefreshThemeFromContext", "RefreshThemeFromContext", 1, 8};
EngineFunction invalidate_widget{
    "/Script/UMG.Widget.InvalidateLayoutAndVolatility", "InvalidateLayoutAndVolatility", 0, 0};

AnomalyGenerationHandleV1 HandleAt(const Reader& reader, uintptr_t address) {
    int32_t index{};
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (!address || !reader.Read(address + accessory::profile::native_layout.ObjectIndex, index) || index < 0 ||
        objects->snapshot_at(objects->user, static_cast<uint32_t>(index), &snapshot).code != ANOMALY_STATUS_V1_OK ||
        Resolve(reader, objects, snapshot.handle) != address) return {};
    return snapshot.handle;
}

AnomalyGenerationHandleV1 ThemeSubsystem(const Reader& reader, uintptr_t controller) {
    struct Params { uintptr_t controller, type, result; };
    Params params{controller, Resolve(reader, objects, Find("/Script/HTGame.HTUIThemeSubsystem")), 0};
    const auto library = Find("/Script/Engine.Default__SubsystemBlueprintLibrary");
    if (!params.type || !Call(reader, library, lookup, params) ||
        !reader.IsA(params.result, "HTUIThemeSubsystem")) return {};
    return HandleAt(reader, params.result);
}

uintptr_t WidgetField(const Reader& reader, uintptr_t object, const char* name, uint32_t offset) {
    uintptr_t type{}, value{};
    return object && reader.Read(object + accessory::profile::native_layout.ObjectClass, type) &&
        reader.Property(type, name, offset, sizeof(value)) && reader.Read(object + offset, value)
        ? value : 0;
}

void RefreshShortcutIcons(const Reader& reader, uintptr_t entry) {
    const auto row = WidgetField(reader, entry, "HorizontalBoxButtons", hud_layout.shortcut_row);
    uintptr_t row_class{};
    RowArray slots{};
    if (!reader.IsA(row, "HorizontalBox") ||
        !reader.Read(row + accessory::profile::native_layout.ObjectClass, row_class) ||
        !reader.Property(row_class, "Slots", hud_layout.panel_slots, sizeof(RowArray)) ||
        !reader.Read(row + hud_layout.panel_slots, slots) || !slots.data || slots.count < 1 ||
        slots.count > 32 || slots.capacity < slots.count) return;

    unsigned refreshed{};
    for (int32_t i = 0; i < slots.count; ++i) {
        uintptr_t slot{};
        if (!reader.Read(slots.data + i * sizeof(slot), slot) || !reader.IsA(slot, "PanelSlot")) continue;
        const auto button = WidgetField(reader, slot, "Content", hud_layout.slot_content);
        if (!reader.IsA(button, "W_HTButton_MainFormFunction_C")) continue;
        const auto border = WidgetField(reader, button, "ButtonBorder", hud_layout.button_border);
        // Blueprint replaces the brush in place. Invalidate the border that draws it.
        if (reader.IsA(border, "Border") &&
            Call(reader, HandleAt(reader, border), invalidate_widget, nullptr, 0)) ++refreshed;
    }
    Log("HUD theme: shortcut icons invalidated=" + std::to_string(refreshed));
}

void RefreshShortcutBar(const Reader& reader, AnomalyGenerationHandleV1 subsystem) {
    struct Binding { int32_t index; uint32_t serial; FName function; };
    static_assert(sizeof(Binding) == 16);

    // The local player's theme delegate also identifies its live desktop shortcut bar.
    const auto& layout = accessory::profile::native_layout;
    const auto owner = Resolve(reader, objects, subsystem);
    uintptr_t owner_class{};
    RowArray listeners{};
    if (!reader.Read(owner + layout.ObjectClass, owner_class) ||
        !reader.Property(owner_class, "OnHUDThemeChanged", hud_layout.theme_delegate, sizeof(RowArray)) ||
        !reader.Read(owner + hud_layout.theme_delegate, listeners) || !listeners.data || listeners.count < 1 ||
        listeners.count > 128 || listeners.capacity < listeners.count) return;

    for (int32_t i = 0; i < listeners.count; ++i) {
        Binding binding{};
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (!reader.Read(listeners.data + i * sizeof(Binding), binding) ||
            binding.index < 0 || !binding.serial ||
            objects->snapshot_at(objects->user, static_cast<uint32_t>(binding.index), &snapshot).code !=
                ANOMALY_STATUS_V1_OK ||
            ANOMALY_UE5_OBJECT_HANDLE_SERIAL(snapshot.handle) != binding.serial) continue;
        const auto entry = Resolve(reader, objects, snapshot.handle);
        if (!reader.IsA(entry, "BPUI_MainFormFunctionEntry_C")) continue;

        RefreshShortcutIcons(reader, entry);
    }
}

bool ReadThemeId(const Reader& reader, FName& theme) {
    constexpr std::string_view outfit_id = "Fashion_1090_1";
    const auto& layout = accessory::profile::native_layout;
    const auto table = Resolve(reader, objects, Find(
        "/Game/DataTable/Character/Appearance/DT_AppearanceData.DT_AppearanceData"));
    uintptr_t row_type{}, flags{};
    RowArray rows{};
    int32_t bit_count{};
    const auto map = table + layout.RowMap;
    if (!reader.IsA(table, "DataTable") || !reader.Read(table + layout.RowStruct, row_type) ||
        !reader.Property(row_type, "AppearanceData", layout.AppearanceData, 16) ||
        !reader.Read(map, rows) || !rows.data || rows.count < 1 || rows.count > 4096 ||
        rows.capacity < rows.count || !reader.Read(map + layout.MapBitCount, bit_count) ||
        bit_count < rows.count || !reader.Read(map + layout.MapBitsData, flags)) return false;
    if (!flags) {
        if (bit_count > 128) return false;
        flags = map + layout.MapInlineBits;
    }
    for (int32_t i = 0; i < rows.count; ++i) {
        uint8_t allocated{};
        if (!reader.Read(flags + i / 8, allocated)) return false;
        if (!(allocated & (1u << (i % 8)))) continue;
        const auto entry = rows.data + layout.MapEntryStride * i;
        if (reader.NameAt(entry) != outfit_id) continue;
        uintptr_t row{}, fashion_type{}, fashion{};
        if (!reader.Read(entry + layout.MapValue, row) || !row ||
            !reader.Read(row + layout.AppearanceData, fashion_type) ||
            reader.NameAt(fashion_type + layout.ObjectName) != "FashionData" ||
            !reader.Property(fashion_type, "MainFormThemeID", hud_layout.fashion_theme_id, sizeof(FName)) ||
            !reader.Read(row + layout.AppearanceData + layout.InstancedStructData, fashion) || !fashion)
            return false;
        return reader.Read(fashion + hud_layout.fashion_theme_id, theme) && theme.id != 0;
    }
    return false;
}

bool UpdateTheme(bool requested) {
    const Reader reader{names};
    AnomalyNteSessionSnapshotV1 scene{sizeof(scene)};
    if (!Succeeded(session->snapshot(session->user, &scene)) ||
        scene.state != ANOMALY_NTE_SESSION_V1_WORLD_READY) {
        last_world = {};
        return false;
    }
    if (!Initialize(reader, objects)) return false;

    AnomalyGenerationHandleV1 controller_handle{};
    if (!Succeeded(player->get_object_handles(player->user, nullptr, &controller_handle))) return false;
    auto controller = Resolve(reader, objects, controller_handle);
    if (!controller) return false;

    const bool scene_changed = !SameHandle(last_world, scene.world) ||
        !SameHandle(last_controller, controller_handle);
    if (scene_changed || !Resolve(reader, objects, theme_subsystem)) {
        theme_subsystem = ThemeSubsystem(reader, controller);
        if (!theme_subsystem.id) return false;
    }

    if (!requested) {
        if (!Call(reader, theme_subsystem, refresh_theme, controller)) return false;
        RefreshShortcutBar(reader, theme_subsystem);
        restore_pending = false;
        last_world = {};
        last_controller = {};
        return true;
    }

    if (!preview_theme.id && !ReadThemeId(reader, preview_theme)) return false;
    FName current{};
    if (!Call(reader, theme_subsystem, get_theme, current)) return false;
    const bool needs_apply = current.id != preview_theme.id || current.number != preview_theme.number;
    if (needs_apply) {
        constexpr uint8_t player_equipped = 3;
        struct Params { FName theme; uint8_t source; };
        Params params{preview_theme, player_equipped};
        if (!Call(reader, theme_subsystem, set_theme, params)) return false;
        restore_pending = true;
        if (!Call(reader, theme_subsystem, get_theme, current) ||
            current.id != preview_theme.id || current.number != preview_theme.number) return false;
    }
    if (needs_apply || scene_changed) RefreshShortcutBar(reader, theme_subsystem);
    last_world = scene.world;
    last_controller = controller_handle;
    restore_pending = true;
    return true;
}
}

namespace lite_hud_theme {
void Load(const AnomalyHostApiV1* host) noexcept {
    const Host services(host);
    core = services.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
    objects = services.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
    names = services.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
    events = services.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID).get();
    player = services.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID).get();
    session = services.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
    config = services.Query<AnomalyConfigServiceV1>(ANOMALY_CONFIG_SERVICE_V1_ID).get();
    scheduler = services.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get();
    enabled.store(false);
    settings_dirty.store(false);
    available = LoadLayout() && objects && objects->find_exact && objects->snapshot_at &&
        names && names->resolve_utf8 &&
        events && events->struct_size >= offsetof(AnomalyUe5ProcessEventServiceV1, invoke) + sizeof(events->invoke) &&
        events->invoke &&
        player && player->struct_size >= offsetof(AnomalyNtePlayerServiceV1, get_object_handles) +
            sizeof(player->get_object_handles) && player->get_object_handles &&
        session && session->snapshot && LoadSettings(services);
    registry = 0;
    retry_init = {};
    restore_pending = false;
    next_check = {};
    last_world = last_controller = theme_subsystem = {};
    preview_theme = {};
    for (auto* function : {&lookup, &set_theme, &get_theme, &refresh_theme, &invalidate_widget})
        function->handle = {};
    waiting.store(false);
}

bool Available() noexcept { return available; }

AnomalyStatusV1 Stop() noexcept {
    return SaveSettings() ? Ok() : AnomalyStatusV1{ANOMALY_STATUS_V1_FAILED, 0, {}};
}

void Update() noexcept {
    if (!available) return;
    const auto requested = enabled.load();
    if (!requested && !restore_pending) {
        next_check = {};
        waiting.store(false);
        return;
    }
    const auto now = Clock::now();
    if (now < next_check) return;
    next_check = now + std::chrono::milliseconds(400);
    try {
        waiting.store(!UpdateTheme(requested));
        return;
    } catch (...) {}
    waiting.store(true);
}

void Draw(const AnomalyUiServiceV1* ui) {
    const auto& localizer = lite_i18n::localizer;
    if (!ui->checkbox) return;
    int32_t checked = enabled.load();
    if (ui->checkbox(ui->user, StringView(localizer.Text("hud.enabled", "Use Elyms's Y2K HUD")), &checked)) {
        enabled.store(checked != 0);
        settings_dirty.store(true);
        AnomalyGenerationHandleV1 save_task{};
        scheduler->schedule(scheduler->user, 0, SaveSettingsTask, nullptr, &save_task);
    }
    if (waiting.load())
        ui->text(ui->user, StringView(localizer.Text("hud.waiting", "Waiting for the game HUD.")));
}
}
