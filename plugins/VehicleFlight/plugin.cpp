#include "vehicle.hpp"
#include "preferences.hpp"
#include "input_gate.hpp"
#include <anomaly/sdk/localization.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <Windows.h>

namespace vehicle_flight {
using namespace anomaly::sdk;
constexpr auto Id = "b1ank.vehicle-flight";
constexpr auto Name = "Vehicle Flight";
constexpr auto Version = "0.1.31";
using Clock = std::chrono::steady_clock;

enum class Status { Off, Starting, Flying, CameraUnavailable, Blocked, PositionCorrected, NoVehicle, SceneChanged, Unavailable, Restoring };
enum class InputState { Pending, Preparing, Ready, Failed };

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
    explicit Context(const AnomalyHostApiV1* api) : api(api), host(api), localizer(api), hotkey(this, ToggleFlight), input_gate(access.engine) {}
    const AnomalyHostApiV1* api;
    Host host;
    Localizer localizer;
    VehicleAccess access;
    FlightHotkey hotkey;
    InputGate input_gate;
    PreferencesStore preferences;
    Binding saved_toggle;
    const AnomalyUiServiceV1* ui{};
    const AnomalyHookServiceV1* hooks{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyInputServiceV1* input{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyWindowServiceV1* window{};
    AnomalyGenerationHandleV1 window_handle{}, world{};
    Ref vehicle, physics;
    Clock::time_point next_vehicle_check{}, next_move_log{};
    Vector anchor{};
    uint32_t measured_frames{};
    double update_milliseconds{}, longest_update{};
    std::atomic<Status> status{Status::Off};
    std::atomic<InputState> input_state{InputState::Pending};
    std::atomic<bool> developer_mode{};
    std::atomic<bool> requested{};
    std::atomic<bool> noclip{true};
    std::atomic<int> speed{DefaultSpeed};
    int fallback_open{1};
    bool reported_flying{};
    bool moving{};

    void SetRequested(bool enabled) {
        if (enabled && input_state.load() == InputState::Failed) input_state = InputState::Pending;
        status = enabled ? Status::Starting : Status::Restoring;
        requested = enabled;
    }

    void Unavailable(std::string_view reason) {
        if (access.engine.core && access.engine.core->log)
            access.engine.core->log(access.engine.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING, StringView(reason));
        End(Status::Unavailable);
    }

    static void ToggleFlight(void* owner) noexcept {
        auto& ctx = *static_cast<Context*>(owner);
        if (ctx.developer_mode.load() && HasFocus()) ctx.SetRequested(!ctx.requested.load());
    }

    static void PrepareInput(void* raw, AnomalyGenerationHandleV1) noexcept {
        auto& ctx = *static_cast<Context*>(raw);
        try {
            auto& engine = ctx.access.engine;
            engine.names = ctx.host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
            engine.objects = ctx.host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
            const auto events = ctx.host.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID);
            if (events && events->struct_size >= sizeof(*events.get())) engine.events = events.get();
            ctx.input_state = ctx.input_gate.Start(ctx.hooks, ctx.api) ? InputState::Ready : InputState::Failed;
        } catch (...) { ctx.input_state = InputState::Failed; }
    }

    bool ResetVehicle() {
        if (!access.RestoreFlightPhysics()) return false;
        input_gate.Release();
        reported_flying = false;
        vehicle = {};
        physics = {};
        next_move_log = {};
        anchor = {};
        moving = false;
        measured_frames = 0;
        update_milliseconds = 0.0;
        longest_update = 0.0;
        return true;
    }

    void WaitForVehicle(Clock::time_point now) {
        if (vehicle && !ResetVehicle()) {
            Unavailable("Vehicle Flight: previous vehicle physics could not be restored");
            return;
        }
        next_vehicle_check = now + std::chrono::milliseconds(120);
        status = Status::NoVehicle;
    }

    void End(Status reason) {
        const bool restored = ResetVehicle();
        world = {};
        next_vehicle_check = {};
        status = restored ? reason : Status::Restoring;
        requested = false;
    }
};

void Tick(Context& ctx, double seconds) {
    if (!ctx.requested.load()) {
        if (ctx.world.id || ctx.access.HasFlightPhysics()) ctx.End(Status::Off);
        else if (ctx.status.load() == Status::Restoring) ctx.status = Status::Off;
        return;
    }
    if (!ctx.framework)
        ctx.framework = ctx.host.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
    if (!ctx.framework || !ctx.framework->is_game_thread) {
        ctx.Unavailable("Vehicle Flight: Game-thread service is unavailable");
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
        !ctx.input || !ctx.input->snapshot || !ctx.scheduler || !ctx.scheduler->schedule) {
        ctx.Unavailable("Vehicle Flight: player, input or scheduler service is unavailable");
        return;
    }
    AnomalyGenerationHandleV1 controller{}, pawn{};
    if (!Succeeded(ctx.player->get_object_handles(ctx.player->user, &pawn, &controller))) {
        ctx.WaitForVehicle(now);
        return;
    }
    const auto input_state = ctx.input_state.load();
    if (input_state == InputState::Pending) {
        ctx.input_state = InputState::Preparing;
        AnomalyGenerationHandleV1 task{};
        if (!Succeeded(ctx.scheduler->schedule(ctx.scheduler->user, 0, Context::PrepareInput, &ctx, &task))) {
            ctx.input_state = InputState::Failed;
            ctx.Unavailable("Vehicle Flight: input preparation task was rejected");
        }
        return;
    }
    if (input_state == InputState::Preparing) return;
    if (input_state != InputState::Ready) {
        ctx.Unavailable("Vehicle Flight: driving input initialization failed");
        return;
    }
    if (!ctx.access.Bind()) {
        ctx.Unavailable("Vehicle Flight: vehicle reflection bindings are unavailable");
        return;
    }
    const auto vehicle = ctx.access.DrivenVehicle(controller, pawn, ctx.vehicle);
    if (!vehicle) {
        ctx.WaitForVehicle(now);
        return;
    }
    if (ctx.vehicle && !(vehicle == ctx.vehicle) && !ctx.ResetVehicle()) {
        ctx.Unavailable("Vehicle Flight: previous vehicle physics could not be restored");
        return;
    }
    const auto local_controller = ctx.access.engine.Resolve(controller);
    if (!ctx.input_gate.Hold(local_controller) || !ctx.access.SuspendDriving(vehicle)) {
        ctx.Unavailable("Vehicle Flight: driving input or drivetrain could not be suspended");
        return;
    }
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
    if (Length(Difference(ctx.anchor, observed)) > 80.0) ctx.anchor = observed;
    const bool noclip = ctx.noclip.load();
    if (!ctx.access.BeginFlightPhysics(vehicle, ctx.physics, noclip)) {
        ctx.Unavailable("Vehicle Flight: collision and chassis physics overrides failed");
        return;
    }
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    seconds = std::min(seconds, 0.05);

    AnomalyNteCameraSnapshotV1 camera{sizeof(camera)};
    Vector view{};
    bool has_camera = ctx.access.ViewRotation(local_controller, view);
    if (!has_camera && ctx.player->camera_snapshot &&
        Succeeded(ctx.player->camera_snapshot(ctx.player->user, &camera)) && Same(camera.world, scene.world) &&
        (camera.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) &&
        !(camera.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) &&
        std::isfinite(camera.rotation[0]) && std::isfinite(camera.rotation[1])) {
        std::copy_n(camera.rotation, view.size(), view.begin());
        has_camera = true;
    }
    AnomalyInputSnapshotV1 input{sizeof(input)};
    const bool controlling = HasFocus() && Succeeded(ctx.input->snapshot(ctx.input->user, &input)) &&
        !(input.capture_flags & (ANOMALY_INPUT_CAPTURE_V1_TEXT | ANOMALY_INPUT_CAPTURE_V1_KEYBOARD));
    Vector direction{};
    Vector facing{};
    const Vector* facing_pointer = nullptr;
    bool boost{};
    bool camera_unavailable{};
    if (controlling) {
        const int forward = static_cast<int>(Down(input, 'W')) - Down(input, 'S');
        const int sideways = static_cast<int>(Down(input, 'D')) - Down(input, 'A');
        const int vertical = static_cast<int>(Down(input, VK_SPACE)) -
            (Down(input, VK_CONTROL) || Down(input, VK_LCONTROL) || Down(input, VK_RCONTROL));
        camera_unavailable = (forward || sideways) && !has_camera;
        direction = Direction(has_camera ? view[1] : 0.0,
            has_camera ? forward : 0, has_camera ? sideways : 0, vertical);
        boost = (input.modifiers & ANOMALY_INPUT_MODIFIER_V1_SHIFT) != 0;
        if (has_camera && (forward || sideways)) {
            facing = {0.0, view[1], 0.0};
            facing_pointer = &facing;
        }
    }
    const bool moving = Length(direction) > 0.0;
    // Apply each movement delta to the position the engine actually accepted.
    // On release, hover at that position instead of snapping to a prior target.
    if (moving || ctx.moving) ctx.anchor = observed;
    ctx.moving = moving;
    const auto target = Advance(ctx.anchor, direction, seconds, ctx.speed.load(), boost);
    bool moved{};
    if (!ctx.access.Move(vehicle, ctx.physics, target, facing_pointer, !noclip, moved, &observed)) {
        ctx.End(Status::Unavailable);
        return;
    }
    // Teleport can adjust a destination or reject it when collision is enabled.
    // Noclip is checked against the next tick to avoid a second query on its fast path.
    if ((!noclip || !moved) && !ctx.access.Location(vehicle, observed)) {
        ctx.End(Status::Unavailable);
        return;
    }
    const bool blocked = !moved || (!noclip && Length(Difference(target, observed)) > 0.02);
    if (blocked) {
        ctx.anchor = observed;
        ctx.status = noclip ? Status::PositionCorrected : Status::Blocked;
        if (now >= ctx.next_move_log) {
            const auto* core = ctx.access.engine.core;
            if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                StringView("Vehicle Flight: movement limited; noclip=" + std::to_string(noclip) +
                    " moved=" + std::to_string(moved) + " remaining_cm=" + std::to_string(Length(Difference(target, observed)))));
            ctx.next_move_log = now + std::chrono::seconds(1);
        }
        return;
    }
    ctx.anchor = noclip ? target : observed;
    ctx.status = camera_unavailable ? Status::CameraUnavailable : Status::Flying;
    if (!ctx.reported_flying && ctx.status.load() == Status::Flying) {
        ctx.reported_flying = true;
        const auto* core = ctx.access.engine.core;
        if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            StringView("Vehicle Flight 0.1.31: flight controls are active using the standard host API"));
    }
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
        ctx->hooks = ctx->host.Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID).get();
        const auto scheduler = ctx->host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID);
        if (scheduler && scheduler->struct_size >= offsetof(AnomalySchedulerServiceV1, cancel) +
                sizeof(scheduler->cancel) && scheduler->schedule)
            ctx->scheduler = scheduler.get();
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
        ctx->input_gate.Load(profile.value());
        const auto preferences = ctx->preferences.Load(ctx->host);
        ctx->saved_toggle = preferences.toggle;
        ctx->speed = preferences.speed;
        ctx->noclip = preferences.noclip;
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
    if (ctx.access.HasFlightPhysics()) {
        // The standard host has already frozen Game callbacks. Do not invoke UE
        // from Lifecycle or wait for an update that can no longer be delivered.
        ctx.requested = false;
        ctx.input_gate.Release();
        const auto* core = ctx.access.engine.core;
        if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
            StringView("Vehicle Flight: active unload is unsupported. Turn flight off and wait for restoration before stopping the plugin. Restart the game if this generation is quarantined."));
        return {ANOMALY_STATUS_V1_CONFLICT, 0, StringView("Turn flight off and wait for restoration before unloading")};
    }
    ctx.End(Status::Off);
    const auto input_stopped = ctx.input_gate.Stop();
    const auto hotkey_stopped = ctx.hotkey.Stop();
    ctx.preferences.Flush();
    if (!Succeeded(input_stopped)) return input_stopped;
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
    if (!ctx.ui) ctx.ui = ctx.host.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID).get();
    if (!ctx.ui) return;
    const bool developer_mode = DeveloperModeEnabled(ctx.ui);
    ctx.developer_mode = developer_mode;
    if (!developer_mode) {
        ctx.hotkey.CancelCapture();
        if (ctx.requested.load() || ctx.world.id || ctx.access.HasFlightPhysics()) ctx.End(Status::Off);
        return;
    }
    const auto started = Clock::now();
    try { Tick(ctx, seconds); }
    catch (...) {
        ctx.End(Status::Unavailable);
    }
    if (ctx.vehicle && ctx.requested.load() && ctx.measured_frames < 180) {
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        ctx.update_milliseconds += elapsed;
        ctx.longest_update = std::max(ctx.longest_update, elapsed);
        if (++ctx.measured_frames == 180) {
            const auto* core = ctx.access.engine.core;
            const auto message = "Vehicle Flight: update average=" +
                std::to_string(ctx.update_milliseconds / ctx.measured_frames) + " ms, max=" +
                std::to_string(ctx.longest_update) + " ms over 180 frames";
            if (core && core->log) core->log(core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(message));
        }
    }
}

void DrawContent(Context& ctx, const AnomalyUiServiceV1& ui) noexcept {
    try {
        auto active = (ctx.requested.load() || ctx.status.load() == Status::Restoring) ? 1 : 0;
        const auto label = ctx.localizer.Text("flight.enable", "Enable vehicle flight");
        if (ui.checkbox(ui.user, StringView(label), &active)) ctx.SetRequested(active != 0);
        bool changed = ctx.hotkey.Draw(ui, ctx.localizer);
        int32_t noclip = ctx.noclip.load() ? 1 : 0;
        const auto collision_label = ctx.localizer.Text("flight.noclip", "No collision");
        if (ui.checkbox(ui.user, StringView(collision_label), &noclip)) {
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
            ctx.preferences.Queue({ctx.speed.load(), ctx.saved_toggle, ctx.noclip.load()});
        }
        const auto controls = ctx.localizer.Text("flight.controls_help",
            "WASD moves horizontally along the view. Space ascends, Ctrl descends and Shift triples speed (up to 1000 m/s). Release to hover. No collision only applies during flight; turn it off to fly with physical collision.");
        (ui.text_wrapped ? ui.text_wrapped : ui.text)(ui.user, StringView(controls));
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
