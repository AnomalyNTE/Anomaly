#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/localization.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <set>
#include <string>
#include <vector>
#include "plan.hpp"
#include "access.hpp"

namespace {
const AnomalyUe5NamesServiceV1* names{};
const AnomalyUe5ObjectsServiceV1* objects{};
const AnomalyUe5FrameworkServiceV1* framework{};
const AnomalyUe5BuildServiceV1* build_service{};
const AnomalyWindowServiceV1* window_service{};
anomaly::sdk::Localizer localizer;
AnomalyGenerationHandleV1 window_handle{};
int fallback_open{1};
Clock::time_point next_services{};
std::map<std::string, AnomalyGenerationHandleV1> functions;
std::map<std::string, Clock::time_point> missing_functions;
std::map<std::string, int> offsets;
#include "ue.hpp"

std::atomic_bool enabled{};
std::atomic_uint64_t intent{};
uint64_t applied_intent{};
Clock::time_point next_poll{};
taxi::Run run;
taxi::ConfirmedOrders confirmed;
struct View { std::string status, order; uint64_t completed{}; };
std::atomic<std::shared_ptr<const View>> published;

struct SaveData {
    FName order{}, pickup{}, destination{};
    taxi::Stage stage{};
    uint8_t pad[3]{};
    FName character{};
    float satisfaction{};
};
struct StateParameters { uint8_t state{}, end_type{}; };
struct ObjectQuery { uintptr_t context{}, result{}; };
struct UiQuery { RowArray label{}; uintptr_t widget{}; };
static_assert(sizeof(SaveData) == 40 && offsetof(SaveData, character) == 28);
static_assert(sizeof(StateParameters) == 2 && sizeof(UiQuery) == 24);
struct Runtime {
    uintptr_t world{}, player{}, controller{}, vehicle{}, hud{};
    SaveData data{};
    int32_t finished{};
    taxi::Observation observation{};
};
struct Parameter { const char* name; int32_t offset, size; const char* kind{}; };

void Publish(std::string status, std::string order = {}) {
    const auto old = published.load();
    if (old && old->status == status && old->order == order && old->completed == confirmed.added) return;
    published.store(std::make_shared<const View>(View{std::move(status), std::move(order), confirmed.added}));
}
std::string_view MissingRuntimeFeature(const AnomalyUe5BuildServiceV1* service) noexcept {
    if (!service || service->struct_size < offsetof(AnomalyUe5BuildServiceV1, feature_state) + sizeof(service->feature_state) || !service->feature_state)
        return "anomaly.ue5.build.feature_state";
    for (const auto key : {"ue5.objects", "ue5.names", "ue5.functions", "ue5.process-event"})
        if (service->feature_state(service->user, StringView(key)) != ANOMALY_FEATURE_V1_AVAILABLE) return key;
    return {};
}
bool ParametersMatch(const Reader& r, uintptr_t fn, std::span<const Parameter> parameters) {
    uint32_t flags{};
    // Use named native gameplay/UI wrappers, never a raw ServerTaxiGameplayRPC payload.
    return fn && r.Read(fn + taxi::profile::snapshot.layout.FunctionFlags, flags) && (flags & 0x400) && !(flags & 0x40) &&
        std::all_of(parameters.begin(), parameters.end(), [&](const auto& p) { return p.kind && r.Property(fn, p.name, p.offset, p.size,p.kind); });
}
uintptr_t Checked(const Reader& r, const char* package, const char* owner, const char* name,
                  int count, int size, std::initializer_list<Parameter> parameters) {
    const auto fn = Fn(r, package, owner, name, count, size);
    return ParametersMatch(r, fn, {parameters.begin(), parameters.size()}) ? fn : 0;
}
bool SaveLayout(const Reader& r) {
    const auto data = Find(r, "/Script/HTGame.TaxiGamePlaySaveData");
    const auto player = Find(r, "/Script/HTGame.TaxiGameplayPlayerSaveData");
    return data && player && r.Property(data, "ExecutingTaxiOrderID", offsetof(SaveData,order), sizeof(FName),"NameProperty") &&
        r.Property(data, "GuestLocationID", offsetof(SaveData,pickup), sizeof(FName),"NameProperty") && r.Property(data, "GuestTargetLocationID", offsetof(SaveData,destination), sizeof(FName),"NameProperty") &&
        r.Property(data, "TaxiGameplayState", offsetof(SaveData,stage), sizeof(taxi::Stage),"EnumProperty") && r.Property(data, "FortuitousMeetingCharacterID", offsetof(SaveData,character), sizeof(FName),"NameProperty") &&
        r.Property(data, "TaxiGameplaySatisfaction", offsetof(SaveData,satisfaction), sizeof(float),"FloatProperty") && r.Property(player, "TaxiFinishedCnt", taxi::profile::snapshot.layout.PlayerFinished, sizeof(int32_t),"IntProperty") &&
        r.Property(player, "bIsInit", taxi::profile::snapshot.layout.PlayerInitialized, sizeof(uint8_t),"BoolProperty");
}
uintptr_t OwningWorld(const Reader& r, uintptr_t object) {
    for (int depth = 0; object && depth < 8; ++depth) {
        if (r.IsA(object, "Level")) {
            const auto at = Field(r, object, "OwningWorld", sizeof(uintptr_t),"ObjectProperty"); uintptr_t world{};
            return at >= 0 && r.Read(object + at, world) && r.IsA(world, "World") ? world : 0;
        }
        if (r.IsA(object, "World")) return object;
        if (!r.Read(object + taxi::profile::snapshot.layout.ObjectOuter, object)) break;
    }
    return 0;
}
bool LocalDriver(const Reader& r, const Runtime& value) {
    if (!r.IsA(value.world, "World") || !r.IsA(value.player, "HTPlayerState") ||
        !r.IsA(value.controller, "HTPlayerController") || !r.IsA(value.vehicle, "HTWheeledVehicleDrivable") ||
        Child(r, value.controller, "PlayerState") != value.player || Child(r, value.vehicle, "Owner") != value.controller)
        return false;
    for (const auto object : {value.player, value.controller, value.vehicle}) {
        uint32_t flags{};
        if (!r.Read(object + taxi::profile::snapshot.layout.ObjectFlags, flags) || (flags & 0x30) || OwningWorld(r, object) != value.world) return false;
    }
    return true;
}
uint64_t Pack(FName value) { return uint64_t{value.id} | (uint64_t{value.number} << 32); }
bool ReadRuntime(const Reader& r, Runtime& value) {
    value.world = World(r);
    const auto util = Find(r, "/Script/HTGame.Default__HTUtil");
    const auto player = Checked(r, "HTGame", "HTUtil", "GetMainPlayerState", 2, 16, {{"WorldContextObject",0,8,"ObjectProperty"}, {"ReturnValue",8,8,"ObjectProperty"}});
    const auto controller = Checked(r, "HTGame", "HTUtil", "GetMainPlayerController", 2, 16, {{"WorldContextObject",0,8,"ObjectProperty"}, {"ReturnValue",8,8,"ObjectProperty"}});
    const auto vehicle = Checked(r, "HTGame", "HTPlayerController", "BP_GetCurrentDriveVehicle", 1, 8, {{"ReturnValue",0,8,"ObjectProperty"}});
    const auto data = Checked(r, "HTGame", "HTPlayerState", "GetTaxiGameplayCurData", 1, 40, {{"ReturnValue",0,40,"StructProperty"}});
    ObjectQuery ps{value.world}, pc{value.world};
    if (!value.world || !util || !player || !controller || !vehicle || !data || !SaveLayout(r) ||
        !Invoke(r, util, player, &ps) || !Invoke(r, util, controller, &pc)) return false;
    value.player = ps.result; value.controller = pc.result;
    if (!r.IsA(value.player, "HTPlayerState") || !r.IsA(value.controller, "HTPlayerController") ||
        !Invoke(r, value.controller, vehicle, &value.vehicle) || !LocalDriver(r, value) ||
        !Invoke(r, value.player, data, &value.data)) return false;
    const auto at = Field(r, value.player, "TaxiGameplayPlayerSaveData", taxi::profile::snapshot.layout.PlayerSaveSize,"StructProperty");
    uint8_t initialized{};
    if (at < 0 || !r.Read(value.player + at + taxi::profile::snapshot.layout.PlayerFinished, value.finished) || value.finished < 0 ||
        !r.Read(value.player + at + taxi::profile::snapshot.layout.PlayerInitialized, initialized) || initialized != 1 || static_cast<uint8_t>(value.data.stage) > 5) return false;
    const std::array addresses{value.world, value.player, value.controller, value.vehicle};
    for (size_t i = 0; i < addresses.size(); ++i) {
        const auto handle = Handle(r, addresses[i]); if (!handle.id) return false;
        value.observation.identity[2 * i] = handle.id;
        value.observation.identity[2 * i + 1] = handle.generation;
    }
    value.observation.order = {Pack(value.data.order), Pack(value.data.pickup), Pack(value.data.destination)};
    value.observation.stage = value.data.stage;
    value.hud = Child(r, value.controller, "MyHUD");
    return true;
}

bool Derives(const Reader& r, uintptr_t cls, const char* base) {
    for (int i = 0; cls && i < 32; ++i) {
        if (r.NameAt(cls + taxi::profile::snapshot.layout.ObjectName) == base) return true;
        if (!r.Read(cls + taxi::profile::snapshot.layout.Super, cls)) break;
    }
    return false;
}
bool ActiveWidget(const Reader& r, uintptr_t widget, const char* type) {
    uint32_t flags{}; uint8_t active{}, visibility{};
    const auto at = Field(r, widget, "bIsActive", 1,"BoolProperty"), visible = Field(r, widget, "Visibility", 1,"EnumProperty");
    return r.IsA(widget, type) && r.Read(widget + taxi::profile::snapshot.layout.ObjectFlags, flags) && !(flags & 0x30) && at >= 0 && visible >= 0 &&
        r.Read(widget + at, active) && active == 1 && r.Read(widget + visible, visibility) &&
        (visibility == 0 || visibility == 3 || visibility == 4);
}
uintptr_t FindActiveWidget(const Reader& r, const Runtime& value, const char* type) {
    if (!r.IsA(value.hud, "HTHUD") || Child(r, value.hud, "PlayerOwner") != value.controller) return 0;
    const auto& layout=taxi::profile::snapshot.layout;
    const auto offset = Field(r, value.hud, "ActivedWidgetMap", layout.MapSize,"MapProperty");
    RowArray map{}; uintptr_t extra{}; int32_t bit_count{}, max_bits{};
    const auto getter = Checked(r, "HTGame", "HTHUD", "BP_GetUIByClass", 2, 24, {{"UIClass",0,16,"StrProperty"}, {"ReturnValue",16,8,"ObjectProperty"}});
    const auto owner_get = Checked(r, "UMG", "Widget", "GetOwningPlayer", 1, 8, {{"ReturnValue",0,8,"ObjectProperty"}});
    if (!getter || !owner_get || offset < 0 || !r.Read(value.hud + offset, map) || map.count < 0 || map.count > 256 ||
        map.capacity < map.count ||
        !r.Read(value.hud+offset+layout.MapBitsData,extra) || !r.Read(value.hud+offset+layout.MapBitCount,bit_count) ||
        !r.Read(value.hud+offset+layout.MapBitCapacity,max_bits) || max_bits<bit_count || bit_count < map.count || bit_count > 512 ||
        (!extra && map.count > 128) || (map.count && !map.data)) return 0;
    for (int i = 0; i < map.count; ++i) {
        uint32_t bits{};
        const auto flags=extra ? extra : value.hud+offset+layout.MapInlineBits;
        if (!r.Read(flags + sizeof(uint32_t)*(i/32),bits)) return 0;
        if (!(bits & (1u << (i % 32)))) continue;
        uintptr_t cls{}; FName key{};
        if (!r.Read(map.data + layout.MapEntryStride * i, cls) || !Derives(r, cls, type) || !r.Read(map.data + layout.MapEntryStride * i + layout.MapValue, key)) continue;
        const auto label = r.Name(key);
        if (label.empty() || label.size() > 128) continue;
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, label.data(), static_cast<int>(label.size()), nullptr, 0);
        if (length <= 0) continue;
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, label.data(), static_cast<int>(label.size()), wide.data(), length);
        UiQuery query{{reinterpret_cast<uintptr_t>(wide.c_str()), length + 1, length + 1}};
        uintptr_t owner{};
        if (Invoke(r, value.hud, getter, &query) && ActiveWidget(r, query.widget, type) &&
            Invoke(r, query.widget, owner_get, &owner) && owner == value.controller && Handle(r, query.widget).id)
            return query.widget;
    }
    return 0;
}
bool EnabledButton(const Reader& r, uintptr_t button) {
    if (!r.IsA(button, "CommonButtonBase") || !Handle(r, button).id) return false;
    const auto get = Checked(r, "UMG", "Widget", "GetIsEnabled", 1, 1, {{"ReturnValue",0,1,"BoolProperty"}});
    const auto interact = Checked(r, "CommonUI", "CommonButtonBase", "IsInteractionEnabled", 1, 1, {{"ReturnValue",0,1,"BoolProperty"}});
    uint8_t allowed{}, interaction{};
    return get && interact && Invoke(r, button, get, &allowed) && allowed == 1 &&
        Invoke(r, button, interact, &interaction) && interaction == 1;
}
bool CurrentIntent(uint64_t token) { return enabled.load() && intent.load() == token; }
bool SameOrder(const Runtime& a, const Runtime& b) {
    return a.observation.identity == b.observation.identity && a.observation.order == b.observation.order &&
        a.observation.stage == b.observation.stage && Pack(a.data.character)==Pack(b.data.character);
}
bool ReadDialogue(const Reader& r,uintptr_t controller,bool& active) {
    const auto fn=Checked(r,"HTGame","HTPlayerController","IsInDialogue",1,1,{{"ReturnValue",0,1,"BoolProperty"}});
    uint8_t value{};
    if(!r.IsA(controller,"HTPlayerController") || !fn || !Invoke(r,controller,fn,&value) || value>1) return false;
    active=value!=0;return true;
}
// A missing/disabled settlement widget is a wait, not an accepted completion.
enum class Dispatch { Waiting, Sent, Failed, Stale, Dialogue };
Dispatch Submit(const Reader& r, const Runtime& value, taxi::Action action, uint64_t token) {
    uintptr_t receiver = value.player, fn{}, button{};
    StateParameters parameters{};
    if (action == taxi::Action::Pickup || action == taxi::Action::Deliver) {
        const auto expected = action == taxi::Action::Pickup ? taxi::Stage::Pickup : taxi::Stage::Destination;
        if (value.data.stage != expected) return Dispatch::Stale;
        parameters.state = action == taxi::Action::Pickup ? 3 : 5;
        // NormalEnd=0. The native wrapper submits the request and owns all game bookkeeping.
        fn = Checked(r, "HTGame", "HTPlayerState", "SetTaxiGameplayOrderState", 2, 2, {{"InOrderState",0,1,"EnumProperty"}, {"InEndType",1,1,"EnumProperty"}});
    } else if (action == taxi::Action::Continue) {
        if (value.data.stage != taxi::Stage::Settle) return Dispatch::Stale;
        receiver = FindActiveWidget(r, value, "HTUI_TaxiGameplaySettleMain");
        button = Child(r, receiver, "BtnContinueTaxiGameplay");
        // The native callback performs SettleOrder(continue, current vehicle ID), then closes the UI.
        if (!receiver || !EnabledButton(r, button)) return Dispatch::Waiting;
        fn = Checked(r, "HTGame", "HTUI_TaxiGameplaySettleMain", "OnBtnContinueTaxiGameplayClicked", 1, 8, {{"Button",0,8,"ObjectProperty"}});
    } else return Dispatch::Stale;
    if (!fn) return Dispatch::Failed;
    Runtime current;
    if (!CurrentIntent(token) || !ReadRuntime(r, current) || !SameOrder(value, current)) return Dispatch::Stale;
    if(current.data.character.id) {
        bool dialogue{};
        if(!ReadDialogue(r,current.controller,dialogue)) return Dispatch::Failed;
        if(dialogue) return Dispatch::Dialogue;
    }
    if (action == taxi::Action::Continue && (!ActiveWidget(r, receiver, "HTUI_TaxiGameplaySettleMain") ||
        Child(r, receiver, "BtnContinueTaxiGameplay") != button || !EnabledButton(r, button))) return Dispatch::Stale;
    if (!CurrentIntent(token)) return Dispatch::Stale;
    return Invoke(r, receiver, fn, action == taxi::Action::Continue ? static_cast<void*>(&button) : static_cast<void*>(&parameters)) ? Dispatch::Sent : Dispatch::Failed;
}

#include "encounters.hpp"

bool EnsureWindow() noexcept {
    if (window_handle.id) return true;
    if (!window_service || window_service->struct_size < offsetof(AnomalyWindowServiceV1,end)+sizeof(window_service->end) || !window_service->register_window || !window_service->release_window ||
        !window_service->state || !window_service->begin || !window_service->end) return false;
    AnomalyWindowSpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.id = StringView("b1ank-autotaxi-main");
    const auto title = localizer.Text("window.title", "AutoTaxi");
    spec.title = StringView(title);
    spec.initial_width = 650; spec.initial_height = 560;
    spec.minimum_width = 440; spec.minimum_height = 300;
    spec.maximum_width = 900; spec.maximum_height = 760;
    spec.default_open = 1;
    return window_service->register_window(window_service->user, &spec, &window_handle).code == ANOMALY_STATUS_V1_OK && window_handle.id;
}
void ReleaseWindow() noexcept {
    if (window_handle.id && window_service && window_service->release_window &&
        window_service->release_window(window_service->user, window_handle).code == ANOMALY_STATUS_V1_OK)
        window_handle = {};
}
void RefreshRuntimeServices() {
    if (!api) return;
    const Host services(api);
    names = services.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
    objects = services.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
    framework = services.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
    build_service = services.Query<AnomalyUe5BuildServiceV1>(ANOMALY_UE5_BUILD_SERVICE_V1_ID).get();
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) noexcept {
    if (!host || !context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr; registry=world_slot=0;
    try {
        api=host;
        localizer=anomaly::sdk::Localizer(host);
        if (!taxi::profile::Load(host)) {
            static thread_local std::string message;
            message=localizer.Text("error.profile", "The taxi compatibility profile is unavailable or incompatible. No taxi actions were started.");
            return {ANOMALY_STATUS_V1_UNAVAILABLE,0,StringView(message)};
        }
        const Host services(host);
        window_service=services.Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID).get();
        core=services.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        encounter_hooks=services.Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID).get();
        RefreshRuntimeServices();
        return core ? Ok() : AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED,0,{}}; }
}
AnomalyStatusV1 ANOMALY_CALL Start(void*) noexcept {
    try {
        enabled = false; applied_intent = ++intent; registry = world_slot = 0; next_poll = {};
        ResetBindings(); run.Reset(); confirmed.Reset(); published.store({}); next_services = {};
        fallback_open = 1;
        registry=taxi::profile::snapshot.registry; world_slot=taxi::profile::snapshot.world;
        StartEncounters();
        Publish(localizer.Text("status.startup", "Drive a taxi and start taking orders in the game, then enable automatic completion."));
        static_cast<void>(EnsureWindow()); return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED,0,{}}; }
}
AnomalyStatusV1 ANOMALY_CALL Stop(void*, uint32_t) noexcept {
    enabled=false;++intent;
    if(!StopEncounters()) return {ANOMALY_STATUS_V1_UNAVAILABLE,0,{}};
    registry=world_slot=0;ReleaseWindow();return Ok();
}
void ANOMALY_CALL Unload(void*) noexcept { registry = world_slot = 0; taxi::profile::snapshot={}; encounter_hooks=nullptr; ReleaseWindow(); }
void ANOMALY_CALL Update(void*, double) noexcept {
    try {
        if (!names || !objects || !framework || !build_service) {
            const auto now = Clock::now();
            if (now >= next_services) { next_services = now + std::chrono::seconds(2); RefreshRuntimeServices(); }
            if (!names || !objects || !framework || !build_service) { Publish(localizer.Text("status.game_services", "Waiting for the game taxi interfaces.")); return; }
        }
        if (!framework || !framework->is_game_thread || !framework->is_game_thread(framework->user)) return;
        if(registry && world_slot && MissingRuntimeFeature(build_service).empty()) UpdateEncounters(Reader{names});
        const auto token = intent.load();
        if (applied_intent != token) { run.Reset(); applied_intent = token; next_poll = {}; }
        if (!enabled.load()) { run.Reset(); return; }
        const auto now = Clock::now(); if (now < next_poll) return;
        next_poll = now + std::chrono::milliseconds(500);
        const auto missing = MissingRuntimeFeature(build_service);
        if (!missing.empty()) { Publish(localizer.Format("status.interface_missing", "Waiting for taxi interface: {0}", std::array<std::string_view, 1>{missing})); next_poll = now + std::chrono::seconds(2); return; }
        const Reader r{names};
        if (!registry || !world_slot) { Publish(localizer.Text("status.signatures", "Taxi signatures could not be resolved. Reload the plugin after the framework is ready.")); next_poll = now + std::chrono::seconds(2); return; }
        const auto milliseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        Runtime value;
        // Keep only request identity/ack state across a temporary read failure. No
        // native work is performed; a recovered same order must not be resubmitted.
        if (!ReadRuntime(r, value)) {
            bool dialogue{};
            if(run.bound && value.controller && ReadDialogue(r,value.controller,dialogue) && dialogue) {
                run.Suspend(milliseconds);Publish(localizer.Text("status.dialogue","Waiting for the game dialogue to finish; automatic completion will resume afterwards."));return;
            }
            Publish(localizer.Text("status.driver", "Drive a taxi and start taking orders; waiting for mission data.")); return;
        }
        confirmed.Observe({value.observation.identity[2], value.observation.identity[3]}, value.finished);
        if (value.data.stage != taxi::Stage::Idle && value.data.stage != taxi::Stage::Waiting &&
            (!value.data.order.id || !value.data.pickup.id || !value.data.destination.id)) {
            Publish(localizer.Text("status.order_data", "Waiting for complete taxi order information.")); return;
        }
        if(value.data.character.id || encounter_pending.load()) {
            bool dialogue{};
            if(!ReadDialogue(r,value.controller,dialogue)) {
                Publish(localizer.Text("status.dialogue_interface","Waiting for the character dialogue interface."),r.Name(value.data.order));return;
            }
            if(dialogue) {
                run.Suspend(milliseconds);static_cast<void>(run.Next(value.observation,milliseconds));
                Publish(localizer.Text("status.dialogue","Waiting for the game dialogue to finish; automatic completion will resume afterwards."),r.Name(value.data.order));return;
            }
        }
        const auto action = run.Next(value.observation, milliseconds);
        const auto order = r.Name(value.data.order);
        if (run.failed) { Publish(localizer.Text("status.unconfirmed", "This order was not confirmed. Retries stopped; toggle the option off and on to retry."), order); return; }
        if (action == taxi::Action::None) {
            std::string text = localizer.Text("status.waiting", "Waiting for a taxi mission.");
            switch (value.data.stage) {
            case taxi::Stage::Idle: text = localizer.Text("status.start_orders", "Click Start Taking Orders in the game."); break;
            case taxi::Stage::Waiting: text = localizer.Text("status.next_order", "Waiting for the game to assign the next order."); break;
            case taxi::Stage::Pickup: text = localizer.Text("status.pickup_pending", "Pickup requested; waiting for game confirmation."); break;
            case taxi::Stage::Destination: text = localizer.Text("status.delivery_pending", "Delivery requested; waiting for game confirmation."); break;
            case taxi::Stage::SpecialEvent: text = localizer.Text("status.special_event", "Waiting for the passenger event or dialogue to finish."); break;
            case taxi::Stage::Settle: text = localizer.Text("status.settlement_pending", "Settlement and continuation requested; waiting for game confirmation."); break;
            }
            Publish(text, order); return;
        }
        if(action==taxi::Action::Pickup) {
            const auto preparation=PrepareEncounterPickup(r,value,token);
            if(preparation==EncounterPreparation::Stale) return;
            if(preparation==EncounterPreparation::Prepared) {
                Publish(localizer.Text("status.character_prepared","The game assigned the preferred character; automatic pickup will resume."),order);return;
            }
            if(preparation==EncounterPreparation::Failed) {
                run.Submitted(false,milliseconds);
                Publish(localizer.Text("status.character_failed","Character assignment was not confirmed. This order is paused; complete or end it in the game before selecting again."),order);return;
            }
        }
        const auto result = Submit(r, value, action, token);
        if (result == Dispatch::Stale) return;
        if (result == Dispatch::Waiting) { Publish(localizer.Text("status.settlement_ui", "Waiting for the settlement screen and Continue Taking Orders button."), order); return; }
        if(result==Dispatch::Dialogue) {run.Suspend(milliseconds);Publish(localizer.Text("status.dialogue","Waiting for the game dialogue to finish; automatic completion will resume afterwards."),order);return;}
        run.Submitted(result == Dispatch::Sent, milliseconds);
        if (result == Dispatch::Failed) Publish(localizer.Text("status.interface_rejected", "The native taxi interface did not match; this order has stopped."), order);
        else Publish(action == taxi::Action::Pickup ? localizer.Text("status.pickup_pending", "Pickup requested; waiting for game confirmation.") :
            action == taxi::Action::Deliver ? localizer.Text("status.delivery_pending", "Delivery requested; waiting for game confirmation.") : localizer.Text("status.settlement_pending", "Settlement and continuation requested; waiting for game confirmation."), order);
    } catch (...) { enabled = false; ++intent; run.Reset(); Publish(localizer.Text("status.data_changed", "Taxi data changed. Automation stopped; enable it again to retry.")); }
}
void DrawControls(const AnomalyUiServiceV1* ui) noexcept {
    try {
        if (!ui || !ui->text) return;
        int checked = enabled.load() ? 1 : 0;
        if (ui->checkbox && ui->checkbox(ui->user, StringView(localizer.Label("option.enabled", "Enable automatic taxi completion", "autotaxi-enabled")), &checked)) { enabled = checked != 0; ++intent; }
        ui->text(ui->user, StringView(localizer.Text("instructions.start", "Drive a taxi and start taking orders in the game. Pickup, delivery and continuation requests are automatic.")));
        ui->text(ui->user, StringView(localizer.Text("instructions.settlement", "Settlement follows the game rules. Passenger events and dialogues remain with the game.")));
        if (const auto view = published.load()) {
            ui->text(ui->user, StringView(enabled.load() ? view->status : localizer.Text("status.disabled", "Automatic taxi completion is off.")));
            if (enabled.load() && !view->order.empty()) ui->text(ui->user, StringView(localizer.Format("label.order", "Current order: {0}", std::array<std::string_view, 1>{view->order})));
            const auto completed = std::to_string(view->completed);
            ui->text(ui->user, StringView(localizer.Format("label.completed", "Game-confirmed new orders: {0}", std::array<std::string_view, 1>{completed})));
        }
        DrawEncounters(ui);
    } catch (...) {}
}
void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) noexcept {
    try {
        if (!ui || !ui->text) return;
        if (EnsureWindow()) {
            AnomalyWindowStateV1 state{sizeof(state)};
            if (window_service->state(window_service->user, window_handle, &state).code != ANOMALY_STATUS_V1_OK || !state.open) return;
            int32_t visible{};
            if (window_service->begin(window_service->user, window_handle, 0, &visible).code != ANOMALY_STATUS_V1_OK) return;
            if (visible) DrawControls(ui);
            static_cast<void>(window_service->end(window_service->user, window_handle));
            return;
        }
        if (!fallback_open || !ui->begin_window || !ui->end_window) return;
        anomaly::sdk::UiWindow window(ui, localizer.Text("window.title", "AutoTaxi"), &fallback_open);
        if (window) DrawControls(ui);
    } catch (...) {}
}
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("b1ank.autotaxi"), StringView("AutoTaxi"), StringView("b1ank"), StringView("0.1.11"), Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
