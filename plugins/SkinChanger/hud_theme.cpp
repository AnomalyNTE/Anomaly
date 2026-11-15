#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/services/nte.h>
#include <atomic>
#include <chrono>
#include <string>
#include "hud_theme.hpp"
#include "localization.hpp"
#include "profile.hpp"
#include "plate_access.hpp"

namespace {
enum class Request { None, Apply, Restore };
enum class Status { Ready, Applied, Restored, Failed };
std::atomic<Request> request{};
std::atomic<Status> hud_status{};
const AnomalyUe5ObjectsServiceV1* objects{};
const AnomalyUe5NamesServiceV1* names{};
const AnomalyUe5ProcessEventServiceV1* events{};
const AnomalyNtePlayerServiceV1* player{};
bool available{};
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
};

bool Failed(std::string_view operation) {
    Log("HUD theme: " + std::string(operation));
    return false;
}

AnomalyGenerationHandleV1 Find(const char* path) {
    AnomalyGenerationHandleV1 handle{};
    if (objects->find_exact(objects->user, StringView(path), &handle).code != ANOMALY_STATUS_V1_OK) {
        Failed(path);
        return {};
    }
    return handle;
}

bool Call(const Reader& reader, AnomalyGenerationHandleV1 receiver, const EngineFunction& function,
        void* params, size_t size) {
    const auto handle = Find(function.path);
    if (!receiver.id || !handle.id) return false;
    if (!reader.Function(Resolve(reader, objects, handle), function.name,
            function.parameter_count, function.parameter_size))
        return Failed(std::string(function.name) + " parameters did not match");
    const auto result = events->invoke(events->user, receiver, handle, params, size);
    if (result.code != ANOMALY_STATUS_V1_OK)
        return Failed(std::string(function.name) + " returned " + std::to_string(result.code));
    return true;
}

template<class Params>
bool Call(const Reader& reader, AnomalyGenerationHandleV1 receiver, const EngineFunction& function, Params& params) {
    return Call(reader, receiver, function, &params, sizeof(params));
}

constexpr EngineFunction lookup{
    "/Script/Engine.SubsystemBlueprintLibrary.GetLocalPlayerSubSystemFromPlayerController",
    "GetLocalPlayerSubSystemFromPlayerController", 3, 24};
constexpr EngineFunction set_theme{
    "/Script/HTGame.HTUIThemeSubsystem.SetThemeID", "SetThemeID", 2, 9};
constexpr EngineFunction get_theme{
    "/Script/HTGame.HTUIThemeSubsystem.GetCurrentThemeID", "GetCurrentThemeID", 1, 8};
constexpr EngineFunction refresh_theme{
    "/Script/HTGame.HTUIThemeSubsystem.RefreshThemeFromContext", "RefreshThemeFromContext", 1, 8};
constexpr EngineFunction invalidate_widget{
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

bool ChangeTheme(Request action) {
    const Reader reader{names};
    if (!Initialize(reader, objects)) return Failed("object registry unavailable");
    AnomalyGenerationHandleV1 controller_handle{};
    if (player->get_object_handles(player->user, nullptr, &controller_handle).code != ANOMALY_STATUS_V1_OK)
        return Failed("local controller handle unavailable");
    auto controller = Resolve(reader, objects, controller_handle);
    if (!controller) return Failed("local controller unavailable");
    const auto subsystem = ThemeSubsystem(reader, controller);
    if (!subsystem.id) return Failed("local HUD theme subsystem unavailable");

    FName current{};
    if (action == Request::Apply) {
        constexpr uint8_t player_equipped = 3;
        struct Params { FName theme; uint8_t source; };
        Params params{{}, player_equipped};
        if (!ReadThemeId(reader, params.theme)) return Failed("outfit theme data unavailable");
        if (!Call(reader, subsystem, set_theme, params) ||
            !Call(reader, subsystem, get_theme, current)) return false;
        if (current.id != params.theme.id || current.number != params.theme.number)
            return Failed("theme read-back did not match");
    } else {
        // Let the game recalculate from the current scene and actual outfit.
        if (!Call(reader, subsystem, refresh_theme, controller) ||
            !Call(reader, subsystem, get_theme, current)) return false;
    }
    RefreshShortcutBar(reader, subsystem);
    Log("HUD theme: " + reader.Name(current));
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
    available = LoadLayout() && objects && objects->find_exact && objects->snapshot_at &&
        names && names->resolve_utf8 &&
        events && events->struct_size >= offsetof(AnomalyUe5ProcessEventServiceV1, invoke) + sizeof(events->invoke) &&
        events->invoke &&
        player && player->struct_size >= offsetof(AnomalyNtePlayerServiceV1, get_object_handles) +
            sizeof(player->get_object_handles) && player->get_object_handles;
    registry = 0;
    retry_init = {};
    request.store(Request::None);
    hud_status.store(Status::Ready);
}

bool Available() noexcept { return available; }

void Update() noexcept {
    const auto pending = request.exchange(Request::None);
    if (pending == Request::None) return;
    try {
        if (available && ChangeTheme(pending)) {
            hud_status.store(pending == Request::Apply ? Status::Applied : Status::Restored);
            return;
        }
    } catch (...) {}
    hud_status.store(Status::Failed);
}

void Draw(const AnomalyUiServiceV1* ui) {
    const auto& localizer = lite_i18n::localizer;
    if (!ui->button) return;
    if (ui->button(ui->user, StringView(localizer.Text("hud.apply", "Apply Elyms's Y2K HUD")), 0, 0))
        request.store(Request::Apply);
    if (ui->button(ui->user, StringView(localizer.Text("hud.restore", "Restore game HUD")), 0, 0))
        request.store(Request::Restore);
    switch (hud_status.load()) {
    case Status::Applied:
        ui->text(ui->user, StringView(localizer.Text("hud.applied", "Applied the Y2K HUD to the current game interface.")));
        break;
    case Status::Restored:
        ui->text(ui->user, StringView(localizer.Text("hud.restored", "Restored the theme for the current outfit and scene.")));
        break;
    case Status::Failed:
        ui->text(ui->user, StringView(localizer.Text("hud.failed", "HUD theme change failed. Enter the world and retry.")));
        break;
    case Status::Ready:
        break;
    }
}
}
