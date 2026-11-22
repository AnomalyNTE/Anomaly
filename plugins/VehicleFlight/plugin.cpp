#include "vehicle.hpp"
#include "preferences.hpp"
#include <anomaly/sdk/localization.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <Windows.h>

namespace vehicle_flight {
using namespace anomaly::sdk;
constexpr auto Id = "b1ank.vehicle-flight";
constexpr auto Name = "Vehicle Flight";
constexpr auto Version = "0.1.6";
using Clock = std::chrono::steady_clock;

enum class Status { Off, Starting, Flying, CameraUnavailable, Blocked, PositionCorrected, NoVehicle, SceneChanged, Unavailable };

bool Same(AnomalyGenerationHandleV1 a, AnomalyGenerationHandleV1 b) {
    return a.id && a.id == b.id && a.generation == b.generation;
}

bool Down(const AnomalyInputSnapshotV1& input, unsigned key) {
    return (input.keys[key / 8] & (1u << (key % 8))) != 0;
}

bool HasFocus() {
    DWORD pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

bool DeveloperModeEnabled(const AnomalyUiServiceV1* ui) noexcept {
    return ui && ui->struct_size >= offsetof(AnomalyUiServiceV1, developer_mode_enabled) + sizeof(ui->developer_mode_enabled) &&
        ui->developer_mode_enabled && ui->developer_mode_enabled(ui->user) != 0;
}

struct Context {
    explicit Context(const AnomalyHostApiV1* api) : host(api), localizer(api), hotkey(this, ToggleFlight) {}
    Host host;
    Localizer localizer;
    VehicleAccess access;
    FlightHotkey hotkey;
    PreferencesStore preferences;
    Binding saved_toggle;
    const AnomalyUiServiceV1* ui{};
    const AnomalyInputServiceV1* input{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyWindowServiceV1* window{};
    AnomalyGenerationHandleV1 window_handle{}, world{};
    Ref vehicle, physics;
    Clock::time_point next_vehicle_check{};
    Vector anchor{}, velocity{}, previous_move{};
    std::optional<Vector> facing;
    std::atomic<Status> status{Status::Off};
    std::atomic<bool> developer_mode{};
    std::atomic<bool> requested{};
    std::atomic<bool> noclip{};
    std::atomic<int> speed{DefaultSpeed};
    std::atomic<Mode> mode{Mode::Keyboard};
    Mode applied_mode{Mode::Keyboard};
    int fallback_open{1};
    bool restore_mode_tab{true};

    void SetRequested(bool enabled) {
        status = enabled ? Status::Starting : Status::Off;
        requested = enabled;
    }

    static void ToggleFlight(void* owner) noexcept {
        auto& ctx = *static_cast<Context*>(owner);
        if (ctx.developer_mode.load() && HasFocus()) ctx.SetRequested(!ctx.requested.load());
    }

    void ResetVehicle() {
        vehicle = {};
        physics = {};
        anchor = {};
        velocity = {};
        previous_move = {};
        facing.reset();
    }

    void WaitForVehicle(Clock::time_point now) {
        if (vehicle) ResetVehicle();
        next_vehicle_check = now + std::chrono::milliseconds(120);
        status = Status::NoVehicle;
    }

    void End(Status reason) {
        ResetVehicle();
        world = {};
        next_vehicle_check = {};
        status = reason;
        requested = false;
    }
};

void Tick(Context& ctx, double seconds) {
    if (!ctx.requested.load()) {
        if (ctx.world.id) ctx.End(Status::Off);
        return;
    }
    if (!ctx.framework)
        ctx.framework = ctx.host.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
    if (!ctx.framework || !ctx.framework->is_game_thread) {
        ctx.End(Status::Unavailable);
        return;
    }
    if (!ctx.framework->is_game_thread(ctx.framework->user)) return;
    if (!ctx.session)
        ctx.session = ctx.host.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
    AnomalyNteSessionSnapshotV1 scene{sizeof(scene)};
    if (!ctx.session || !ctx.session->snapshot ||
        !Succeeded(ctx.session->snapshot(ctx.session->user, &scene)) ||
        scene.state != ANOMALY_NTE_SESSION_V1_WORLD_READY || !scene.world.id ||
        (ctx.world.id && !Same(ctx.world, scene.world))) {
        ctx.End(Status::SceneChanged);
        return;
    }
    ctx.world = scene.world;
    const auto now = Clock::now();
    if (!ctx.vehicle && now < ctx.next_vehicle_check) return;

    if (!ctx.player)
        ctx.player = ctx.host.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID).get();
    if (!ctx.player || ctx.player->struct_size < sizeof(*ctx.player) || !ctx.player->get_object_handles ||
        !ctx.input || !ctx.input->snapshot || !ctx.access.Bind()) {
        ctx.End(Status::Unavailable);
        return;
    }
    AnomalyGenerationHandleV1 controller{}, pawn{};
    if (!Succeeded(ctx.player->get_object_handles(ctx.player->user, &pawn, &controller))) {
        ctx.WaitForVehicle(now);
        return;
    }
    const auto vehicle = ctx.access.DrivenVehicle(controller, pawn, ctx.vehicle);
    if (!vehicle) {
        ctx.WaitForVehicle(now);
        return;
    }
    if (ctx.vehicle && !(vehicle == ctx.vehicle)) ctx.ResetVehicle();
    Vector observed{};
    if (!ctx.access.Location(vehicle, observed)) {
        if (ctx.vehicle) ctx.End(Status::Unavailable);
        else ctx.WaitForVehicle(now);
        return;
    }
    if (!ctx.vehicle) {
        ctx.vehicle = vehicle;
        ctx.physics = ctx.access.PhysicsRoot(vehicle);
        ctx.anchor = observed;
    }
    constexpr double correction_distance = 600.0;
    constexpr double drift_tolerance = 30.0;
    const auto correction = Difference(ctx.anchor, observed);
    const auto drift = Length(correction);
    const auto lost_distance = Along(correction, ctx.previous_move);
    const bool sweep = !ctx.noclip.load();
    const bool position_corrected = drift > correction_distance || lost_distance > drift_tolerance;
    if (position_corrected) {
        if (ctx.status.load() != Status::PositionCorrected) {
            const auto* core = ctx.access.engine.core;
            if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                StringView("Vehicle Flight: position corrected within the same world and vehicle; flight remains enabled"));
        }
        ctx.anchor = observed;
        ctx.previous_move = {};
    } else if (sweep && drift > drift_tolerance) {
        ctx.anchor = observed;
        ctx.velocity = {};
    }
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    seconds = std::min(seconds, 0.05);
    const auto mode = ctx.mode.load();
    if (mode != ctx.applied_mode) {
        ctx.applied_mode = mode;
        ctx.velocity = {};
        ctx.facing.reset();
    }

    AnomalyNteCameraSnapshotV1 camera{sizeof(camera)};
    const bool has_camera = ctx.player->camera_snapshot &&
        Succeeded(ctx.player->camera_snapshot(ctx.player->user, &camera)) && Same(camera.world, scene.world) &&
        (camera.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) &&
        !(camera.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) &&
        std::isfinite(camera.rotation[0]) && std::isfinite(camera.rotation[1]);
    AnomalyInputSnapshotV1 input{sizeof(input)};
    const bool controlling = HasFocus() && Succeeded(ctx.input->snapshot(ctx.input->user, &input)) &&
        !(input.capture_flags & (ANOMALY_INPUT_CAPTURE_V1_TEXT | ANOMALY_INPUT_CAPTURE_V1_KEYBOARD));
    Vector direction{};
    const Vector* facing = nullptr;
    if (controlling && mode == Mode::Keyboard) {
        const int forward = static_cast<int>(Down(input, 'W')) - Down(input, 'S');
        const int sideways = static_cast<int>(Down(input, 'D')) - Down(input, 'A');
        const int vertical = static_cast<int>(Down(input, VK_SPACE)) -
            (Down(input, VK_CONTROL) || Down(input, VK_LCONTROL) || Down(input, VK_RCONTROL));
        direction = Direction(mode, has_camera ? camera.rotation[0] : 0.0,
                              has_camera ? camera.rotation[1] : 0.0, forward, sideways, vertical);
    } else if (controlling && mode == Mode::Camera && has_camera &&
               !(input.capture_flags & ANOMALY_INPUT_CAPTURE_V1_MOUSE)) {
        if (!ctx.facing && !ctx.access.Rotation(vehicle, ctx.facing.emplace())) {
            ctx.End(Status::Unavailable);
            return;
        }
        *ctx.facing = FollowView(*ctx.facing, camera.rotation[0], camera.rotation[1], seconds);
        facing = &*ctx.facing;
        direction = CameraTravel(ctx.velocity, camera.rotation[0], camera.rotation[1], Down(input, 'W'), Down(input, 'S'));
    } else {
        ctx.velocity = {};
        ctx.facing.reset();
    }
    const auto target = Advance(ctx.anchor, ctx.velocity, direction, seconds, ctx.speed.load());
    const auto step = Difference(target, observed);
    bool moved{};
    if (!ctx.access.Move(vehicle, ctx.physics, target, facing, sweep, moved)) {
        ctx.End(Status::Unavailable);
        return;
    }
    const auto distance = Length(step);
    bool blocked = !moved && distance > 4.0;
    if (moved && distance > drift_tolerance) {
        // A move can report success after travelling only part of the requested distance.
        if (!ctx.access.Location(vehicle, observed)) {
            ctx.End(Status::Unavailable);
            return;
        }
        blocked = Along(Difference(target, observed), step) > drift_tolerance;
    }
    if (blocked) {
        ctx.anchor = observed;
        ctx.previous_move = {};
        if (sweep) ctx.velocity = {};
        ctx.status = sweep ? Status::Blocked : Status::PositionCorrected;
        return;
    }
    ctx.anchor = target;
    ctx.previous_move = step;
    if (mode == Mode::Camera && !has_camera) ctx.status = Status::CameraUnavailable;
    else ctx.status = position_corrected ? Status::PositionCorrected : Status::Flying;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** output) noexcept {
    if (!host || !output) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *output = nullptr;
    try {
        auto ctx = std::make_unique<Context>(host);
        auto& engine = ctx->access.engine;
        engine.core = ctx->host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        engine.names = ctx->host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
        engine.objects = ctx->host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        const auto events = ctx->host.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID);
        if (events && events->struct_size >= sizeof(*events.get())) engine.events = events.get();
        ctx->ui = ctx->host.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID).get();
        ctx->framework = ctx->host.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
        ctx->session = ctx->host.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
        const auto input = ctx->host.Query<AnomalyInputServiceV1>(ANOMALY_INPUT_SERVICE_V1_ID);
        if (input && input->struct_size >= sizeof(*input.get())) ctx->input = input.get();
        const auto window = ctx->host.Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID);
        if (window && window->struct_size >= sizeof(*window.get()) && window->register_window &&
            window->begin && window->end && window->state && window->release_window) ctx->window = window.get();
        const auto profile = LoadProfile(ctx->host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text",
            "b1ank-vehicle-flight-v1"});
        if (!profile) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, StringView("Vehicle Flight profile unavailable")};
        static_cast<void>(engine.ResolveProfile(host, profile.value()));
        const auto preferences = ctx->preferences.Load(ctx->host);
        ctx->saved_toggle = preferences.toggle;
        ctx->speed = preferences.speed;
        ctx->noclip = preferences.noclip;
        ctx->mode = preferences.mode;
        *output = ctx.release();
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, {}}; }
}

AnomalyStatusV1 ANOMALY_CALL Start(void* raw) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    ctx.developer_mode = false;
    ctx.End(Status::Off);
    try {
        ctx.hotkey.Start(ctx.input, ctx.saved_toggle);
        if (ctx.window) {
            const auto title = ctx.localizer.Text("window.title", "Vehicle Flight");
            AnomalyWindowSpecV1 spec{sizeof(spec)};
            spec.id = StringView("vehicle-flight-main");
            spec.title = StringView(title);
            spec.initial_width = 410;
            spec.initial_height = 265;
            spec.minimum_width = 300;
            spec.minimum_height = 120;
            spec.default_open = 1;
            static_cast<void>(ctx.window->register_window(ctx.window->user, &spec, &ctx.window_handle));
        }
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, {}}; }
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* raw, uint32_t) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    ctx.developer_mode = false;
    ctx.End(Status::Off);
    const auto hotkey_stopped = ctx.hotkey.Stop();
    ctx.preferences.Flush();
    if (!Succeeded(hotkey_stopped)) return hotkey_stopped;
    if (ctx.window_handle.id) {
        const auto status = ctx.window->release_window(ctx.window->user, ctx.window_handle);
        if (!Succeeded(status) && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
        ctx.window_handle = {};
    }
    return Ok();
}

void ANOMALY_CALL Unload(void* raw) noexcept { delete static_cast<Context*>(raw); }

void ANOMALY_CALL Update(void* raw, double seconds) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    const bool developer_mode = DeveloperModeEnabled(ctx.ui);
    ctx.developer_mode = developer_mode;
    if (!developer_mode) {
        ctx.hotkey.CancelCapture();
        if (ctx.requested.load() || ctx.world.id) ctx.End(Status::Off);
        return;
    }
    try { Tick(ctx, seconds); }
    catch (...) { ctx.End(Status::Unavailable); }
}

void DrawModeSelector(Context& ctx, const AnomalyUiServiceV1& ui) {
    const auto keyboard = ctx.localizer.Text("flight.keyboard_mode", "Keyboard mode");
    const auto mouse = ctx.localizer.Text("flight.mouse_mode", "Mouse mode");
    const auto selected = ctx.mode.load();
    const bool tabs = ui.struct_size >= offsetof(AnomalyUiServiceV1, end_tab_bar) + sizeof(ui.end_tab_bar) &&
        ui.begin_tab_bar && ui.begin_tab_item && ui.end_tab_item && ui.end_tab_bar;
    if (tabs) {
        if (!ui.begin_tab_bar(ui.user, StringView("##vehicle-flight-modes"), ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN))
            return;
        const auto tab = [&](const std::string& label, Mode mode) {
            const auto flags = ctx.restore_mode_tab && mode == selected
                ? ANOMALY_UI_TAB_ITEM_V1_SET_SELECTED : ANOMALY_UI_TAB_ITEM_V1_NONE;
            if (ui.begin_tab_item(ui.user, StringView(label), nullptr, flags, 1)) {
                if (!ctx.restore_mode_tab) ctx.mode = mode;
                ui.end_tab_item(ui.user);
            }
        };
        tab(keyboard, Mode::Keyboard);
        tab(mouse, Mode::Camera);
        ui.end_tab_bar(ui.user);
        ctx.restore_mode_tab = false;
        return;
    }

    if (!ui.button) {
        ui.text(ui.user, StringView(selected == Mode::Keyboard ? keyboard : mouse));
        return;
    }
    const auto button = [&](std::string label, Mode mode, std::string_view id) {
        if (selected == mode) label = ctx.localizer.Format("flight.mode_selected", "{0} (selected)",
            std::array<std::string_view, 1>{label});
        label.append(id);
        if (ui.button(ui.user, StringView(label), 0.0f, 0.0f)) ctx.mode = mode;
    };
    button(keyboard, Mode::Keyboard, "###vehicle-flight-keyboard");
    if (ui.struct_size >= offsetof(AnomalyUiServiceV1, same_line) + sizeof(ui.same_line) && ui.same_line)
        ui.same_line(ui.user, 0.0f, -1.0f);
    button(mouse, Mode::Camera, "###vehicle-flight-mouse");
}

void DrawContent(Context& ctx, const AnomalyUiServiceV1& ui) noexcept {
    try {
        auto active = ctx.requested.load() ? 1 : 0;
        const auto label = ctx.localizer.Text("flight.enable", "Enable vehicle flight");
        if (ui.checkbox(ui.user, StringView(label), &active)) ctx.SetRequested(active != 0);
        const auto previous_mode = ctx.mode.load();
        DrawModeSelector(ctx, ui);
        bool changed = ctx.hotkey.Draw(ui, ctx.localizer);
        changed |= ctx.mode.load() != previous_mode;
        int noclip = ctx.noclip.load() ? 1 : 0;
        const auto noclip_label = ctx.localizer.Text("flight.noclip", "Pass through objects");
        if (ui.checkbox(ui.user, StringView(noclip_label), &noclip)) {
            ctx.noclip = noclip != 0;
            changed = true;
        }
        float speed = static_cast<float>(ctx.speed.load());
        const auto speed_label = ctx.localizer.Text("flight.speed", "Speed (m/s)");
        if (ui.slider_float && ui.slider_float(ui.user, StringView(speed_label), &speed,
                static_cast<float>(MinSpeed), static_cast<float>(MaxSpeed)) && std::isfinite(speed)) {
            const auto value = static_cast<int>(std::lround(std::clamp(speed, static_cast<float>(MinSpeed), static_cast<float>(MaxSpeed))));
            changed |= ctx.speed.exchange(value) != value;
        }
        if (changed) {
            ctx.saved_toggle = ctx.hotkey.Current();
            ctx.preferences.Queue({ctx.speed.load(), ctx.saved_toggle, ctx.noclip.load(), ctx.mode.load()});
        }
        if (ctx.preferences.Failed())
            ui.text(ui.user, StringView(ctx.localizer.Text("flight.save_failed", "The current flight settings could not be saved.")));

        const auto controls = ctx.mode.load() == Mode::Camera
            ? ctx.localizer.Text("flight.mouse_help", "Aim with the camera. Hold W to fly forward or S to reverse along the view. Release both to hover.")
            : ctx.localizer.Text("flight.keyboard_help", "WASD moves horizontally. Space ascends and Ctrl descends.");
        (ui.text_wrapped ? ui.text_wrapped : ui.text)(ui.user, StringView(controls));
        std::string status;
        switch (ctx.status.load()) {
        case Status::Off: break;
        case Status::Starting: status = ctx.localizer.Text("flight.starting", "Checking the current vehicle…"); break;
        case Status::Flying: status = ctx.localizer.Text("flight.active", "Vehicle flight active"); break;
        case Status::CameraUnavailable: status = ctx.localizer.Text("flight.camera_unavailable", "Waiting for camera direction. The vehicle is hovering."); break;
        case Status::Blocked: status = ctx.localizer.Text("flight.blocked", "The current path is blocked. Change direction to continue."); break;
        case Status::PositionCorrected: status = ctx.localizer.Text("flight.position_corrected", "The game adjusted the vehicle position. Flight stays enabled."); break;
        case Status::NoVehicle: status = ctx.localizer.Text("flight.no_vehicle", "Flight is enabled. Waiting for you to take the driver's seat."); break;
        case Status::SceneChanged: status = ctx.localizer.Text("flight.scene_changed", "Flight stopped after a scene change."); break;
        case Status::Unavailable: status = ctx.localizer.Text("flight.unavailable", "Vehicle flight is unavailable in the current scene."); break;
        }
        if (!status.empty()) (ui.text_wrapped ? ui.text_wrapped : ui.text)(ui.user, StringView(status));
    } catch (...) {}
}

void ANOMALY_CALL Draw(void* raw, const AnomalyUiServiceV1* ui) noexcept {
    if (!raw || !DeveloperModeEnabled(ui) || !ui->checkbox || !ui->text) return;
    auto& ctx = *static_cast<Context*>(raw);
    if (ctx.window_handle.id) {
        AnomalyWindowStateV1 state{sizeof(state)};
        if (!Succeeded(ctx.window->state(ctx.window->user, ctx.window_handle, &state)) || !state.open) {
            ctx.hotkey.CancelCapture();
            return;
        }
        int32_t visible{};
        if (!Succeeded(ctx.window->begin(ctx.window->user, ctx.window_handle, 0, &visible))) return;
        if (visible) DrawContent(ctx, *ui);
        else ctx.hotkey.CancelCapture();
        static_cast<void>(ctx.window->end(ctx.window->user, ctx.window_handle));
        return;
    }
    if (!ctx.fallback_open || !ui->begin_window || !ui->end_window) {
        ctx.hotkey.CancelCapture();
        return;
    }
    try {
        const auto title = ctx.localizer.Text("window.title", "Vehicle Flight");
        UiWindow window(ui, title.c_str(), &ctx.fallback_open);
        if (window) DrawContent(ctx, *ui);
        else ctx.hotkey.CancelCapture();
    } catch (...) {}
}
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* output) {
    using namespace vehicle_flight;
    if (!output || output->struct_size < sizeof(*output)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *output = {sizeof(*output), 1, 0, StringView(Id), StringView(Name), StringView("b1ank"), StringView(Version),
        Load, Start, Stop, Unload, Update, Draw, nullptr};
    return Ok();
}
