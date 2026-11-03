#include "studio.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace {
using namespace character_studio;
using anomaly::sdk::StringView;

template<class T> const T* Query(const AnomalyHostApiV1* host, const char* id,
                                 std::size_t minimum = sizeof(T)) {
    const auto* service = anomaly::sdk::Host(host).Query<T>(id, 1).get();
    return service && service->struct_size >= minimum ? service : nullptr;
}
bool Executable(Address address) {
    MEMORY_BASIC_INFORMATION info{};
    return address && VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) &&
        info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
// SEH guard contains no C++ object with an unwinding destructor.
bool InvokeNative(Address entry, Address object, Address function, void* data) noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(entry)(
            reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), data);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct LiveProcess final : Process {
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    Address entry{};
    bool faulted{};

    bool Read(Address address, void* data, std::size_t size) override {
        return core && core->read_memory && address &&
            core->read_memory(core->user, address,
                {static_cast<std::uint8_t*>(data), size}).code == ANOMALY_STATUS_V1_OK;
    }
    std::string ResolveName(std::uint32_t index) override {
        if (!names || !names->resolve_utf8) return {};
        return String([&](char* out, std::size_t* size) {
            return names->resolve_utf8(names->user, index, out, size);
        });
    }
    std::string ResolveText(Address address) override {
        if (!names || !names->resolve_ftext_utf8) return {};
        return String([&](char* out, std::size_t* size) {
            return names->resolve_ftext_utf8(names->user, address, out, size);
        });
    }
    template<class Copy> static std::string String(Copy copy) {
        std::array<char, 512> first{};
        std::size_t size = first.size();
        auto status = copy(first.data(), &size);
        if (status.code == ANOMALY_STATUS_V1_OK && size && size <= first.size())
            return {first.data(), size - 1};
        if (status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL || !size || size > 65536)
            return {};
        std::string result(size, '\0');
        const auto capacity = size;
        status = copy(result.data(), &size);
        if (status.code != ANOMALY_STATUS_V1_OK || !size || size > capacity) return {};
        result.resize(size - 1);
        return result;
    }
    Handle Find(std::string_view path) override {
        Handle result{};
        if (objects && objects->find_exact &&
            objects->find_exact(objects->user, StringView(path), &result).code == 0) return result;
        return {};
    }
    std::uint64_t Generation() override {
        return objects && objects->generation ? objects->generation(objects->user) : 0;
    }
    bool Invoke(Address object, Address function, void* data, std::size_t size) override {
        if (faulted || !entry || !framework || !framework->is_game_thread ||
            !framework->is_game_thread(framework->user)) return false;
        if (!InvokeNative(entry, object, function, size ? data : nullptr)) {
            // A native fault disables further calls in this generation. Never continue
            // stepping through a guessed initialization chain after an engine exception.
            faulted = true;
            return false;
        }
        return true;
    }
};
enum class Action { refresh, spawn, destroy, clear, move, cancel };
inline constexpr double DefaultSpacing = 120.0; // Unreal world units: centimeters.
struct Command {
    Action action{};
    Handle world{};
    Name character{};
    std::uint64_t model{};
    std::uint32_t count{1};
    Vector position{};
    double yaw{};
    double spacing{DefaultSpacing};
};
struct Batch {
    Name character{};
    std::uint32_t remaining{};
    Vector position{};
    double yaw{};
    Vector right{}, forward{};
    std::uint32_t placed{};
};
struct View {
    Handle world{};
    std::shared_ptr<const std::vector<Character>> characters =
        std::make_shared<const std::vector<Character>>();
    std::vector<Model> models;
    std::uint64_t queued{};
    bool ready{};
    // Language-independent "the last status is a failure" flag: the panel decides what
    // to show from this, never from the text, which changes with the active locale.
    bool failed{};
    std::string status;
};
struct RenderState {
    Handle world{};
    char filter[128]{};
    std::uint32_t count{1};
    Vector offset{200, 0, -90}, position{};
    double facing_offset{};
    double spacing{DefaultSpacing};
    double model_yaw{};
    Name character{};
    std::uint64_t selected{};
    std::size_t character_page{}, model_page{};
    bool window_initialized{};
};
struct Context {
    const AnomalyHostApiV1* host{};
    // 插件唯一的一份本地化来源：Render 域直接用它，Game 域把同一引用交给 Engine/Studio。
    anomaly::sdk::Localizer localizer;
    LiveProcess process;
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    std::unique_ptr<Engine> engine;
    std::unique_ptr<Studio> studio;
    Address world_slot{};
    Handle session_world{};
    std::atomic<std::shared_ptr<const View>> view{std::make_shared<const View>()};
    std::mutex mutex;
    std::vector<Command> requests;
    std::deque<Batch> batches;
    std::shared_ptr<const std::vector<Character>> catalog =
        std::make_shared<const std::vector<Character>>();
    std::string status;
    bool status_failed{};
    double until_renew{};
    bool needs_catalog{true}, active{}, player_ready{};
    RenderState render;
};
void Publish(Context& c, bool ready) {
    auto view = std::make_shared<View>();
    if (c.studio) {
        view->world = c.session_world;
        view->models = c.studio->models();
    }
    view->characters = c.catalog;
    for (const auto& batch : c.batches) view->queued += batch.remaining;
    view->ready = ready;
    view->failed = c.status_failed;
    view->status = c.status;
    c.view.store(std::move(view));
}
void Status(Context& c, std::string text, bool warning = false) {
    if (c.status == text) return;
    c.status = std::move(text);
    c.status_failed = warning;
    if (c.process.core && c.process.core->log)
        c.process.core->log(c.process.core->user,
            warning ? ANOMALY_CORE_LOG_LEVEL_V1_WARNING : ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            StringView("Character Studio: " + c.status));
}
Address ResolveSymbol(Context& c, const AnomalySignatureServiceV1* signatures,
                      const Json& spec) {
    Address site{};
    if (!signatures || !signatures->resolve ||
        signatures->resolve(signatures->user, StringView(spec.at("module").get<std::string>()),
            StringView(spec.at("section").get<std::string>()),
            StringView(spec.at("pattern").get<std::string>()), &site).code != 0) return 0;
    const auto& rule = spec.at("resolve");
    if (rule.at("kind") == "direct") return site;
    std::int32_t displacement{};
    if (!c.process.Read(site + rule.at("offset").get<Address>(), &displacement,
                        sizeof(displacement))) return 0;
    return site + rule.at("instructionSize").get<Address>() + displacement +
        rule.value("addend", std::int64_t{0});
}
Address ProcessEvent(Context& c, const AnomalySignatureServiceV1* signatures, Json spec) {
    auto entry = ResolveSymbol(c, signatures, spec);
    // The host observer uses a five-byte E9 hook. Match the surviving original
    // instructions, then validate the untouched prologue tail at the recovered entry.
    if (!entry && signatures && signatures->resolve) {
        auto pattern = spec.at("pattern").get<std::string>();
        std::size_t offset{};
        for (unsigned token = 0; token < 5; ++token) {
            offset = pattern.find(' ', offset);
            if (offset == std::string::npos) return 0;
            ++offset;
        }
        spec["pattern"] = pattern.substr(offset);
        if (const auto tail = ResolveSymbol(c, signatures, spec); tail >= 5) entry = tail - 5;
    }
    constexpr std::uint8_t prologue[]{
        0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
        0x48,0x81,0xec,0,1,0,0,0x48,0x8d,0x6c,0x24,0x30,0x48,0x89,0x9d,0x28,1,0,0};
    std::uint8_t observed[sizeof(prologue)]{};
    if (!Executable(entry) || !c.process.Read(entry, observed, sizeof(observed))) return 0;
    const std::size_t skip = observed[0] == 0xe9 ? 5 : 0;
    return std::memcmp(observed + skip, prologue + skip, sizeof(prologue) - skip) == 0
        ? entry : 0;
}
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** output) noexcept {
    if (!output) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *output = nullptr;
    try {
        auto c = std::make_unique<Context>();
        c->host = host;
        c->localizer = anomaly::sdk::Localizer(host);
        c->process.core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID);
        c->process.names = Query<AnomalyUe5NamesServiceV1>(host,
            ANOMALY_UE5_NAMES_SERVICE_V1_ID, offsetof(AnomalyUe5NamesServiceV1, find_utf8));
        c->process.objects = Query<AnomalyUe5ObjectsServiceV1>(host,
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);
        c->process.framework = Query<AnomalyUe5FrameworkServiceV1>(host,
            ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID);
        c->session = Query<AnomalyNteSessionServiceV1>(host, ANOMALY_NTE_SESSION_SERVICE_V1_ID);
        c->player = Query<AnomalyNtePlayerServiceV1>(host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
            offsetof(AnomalyNtePlayerServiceV1, hold_engage));
        const auto* signatures = Query<AnomalySignatureServiceV1>(host,
            ANOMALY_SIGNATURE_SERVICE_V1_ID);
        auto* core = c->process.core;
        if (!core || !core->plugin_directory) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
        const auto path = LiveProcess::String([&](char* out, std::size_t* size) {
            return core->plugin_directory(core->user, out, size);
        });
        const std::u8string utf8_path(path.begin(), path.end());
        std::ifstream file(std::filesystem::path(utf8_path) / "layout.json", std::ios::binary);
        if (!file)
            throw std::runtime_error(
                c->localizer.Text("error.layout_missing", "layout.json is missing"));
        auto resource = Json::parse(file);
        const auto registry = ResolveSymbol(*c, signatures, resource.at("symbols").at("ue5.GObjects"));
        c->world_slot = ResolveSymbol(*c, signatures, resource.at("symbols").at("ue5.GWorld"));
        c->process.entry = ProcessEvent(*c, signatures, resource.at("symbols").at("ue5.ProcessEvent"));
        if (registry && c->world_slot && c->process.entry && core->read_memory &&
            c->process.names && c->process.names->resolve_utf8 &&
            c->process.names->resolve_ftext_utf8 && c->process.objects &&
            c->process.objects->find_exact && c->process.objects->generation &&
            c->process.framework &&
            c->process.framework->is_game_thread && c->session && c->session->snapshot) {
            c->engine = std::make_unique<Engine>(c->process, std::move(resource), registry,
                                                 c->localizer);
            c->studio = std::make_unique<Studio>(*c->engine, c->localizer);
            Status(*c, c->localizer.Text("status.engine_ready",
                "Engine interfaces initialized; waiting for a world update"));
        } else {
            std::string missing;
            const auto require = [&](bool available, const char* label) {
                if (!available) { if (!missing.empty()) missing += ", "; missing += label; }
            };
            require(registry != 0, "GObjects");
            require(c->world_slot != 0, "GWorld");
            require(c->process.entry != 0, "ProcessEvent");
            require(core->read_memory != nullptr, "core.read_memory");
            require(c->process.names && c->process.names->resolve_utf8 &&
                    c->process.names->resolve_ftext_utf8, "ue5.names");
            require(c->process.objects && c->process.objects->find_exact &&
                    c->process.objects->generation,
                    "ue5.objects");
            require(c->process.framework && c->process.framework->is_game_thread, "ue5.framework");
            require(c->session && c->session->snapshot, "nte.session");
            const std::array arguments{std::string_view(missing)};
            Status(*c, c->localizer.Format("status.init_missing",
                "Initialization is missing: {0}; reload the plugin once the world is ready",
                arguments), true);
        }
        Publish(*c, false);
        *output = c.release();
        return anomaly::sdk::Ok();
    } catch (...) {
        // The status view must outlive this call, so the message stays a fixed literal.
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0,
            StringView("Character Studio failed to initialize: "
                       "check the layout resource and the log")};
    }
}
AnomalyStatusV1 ANOMALY_CALL Start(void* context) noexcept {
    if (!context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    static_cast<Context*>(context)->active = true;
    return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t) noexcept {
    if (!context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    // Host has already frozen/revoked Game callbacks. Calling UE or scheduling a
    // callback here is invalid. Native actor lifespan removes the remaining models
    // within LeaseSeconds of unpaused game time, with no code in this DLL involved.
    auto& c = *static_cast<Context*>(context);
    c.active = false;
    return anomaly::sdk::Ok();
}
void ANOMALY_CALL Unload(void* context) noexcept { delete static_cast<Context*>(context); }

void ANOMALY_CALL Update(void* context, double delta) noexcept {
    if (!context) return;
    auto& c = *static_cast<Context*>(context);
    if (!c.active || !c.studio || !c.process.framework->is_game_thread ||
        !c.process.framework->is_game_thread(c.process.framework->user)) return;
    try {
        AnomalyNteSessionSnapshotV1 session{sizeof(session)};
        AnomalyNtePlayerSnapshotV1 player{sizeof(player)};
        const bool ready = c.session->snapshot(c.session->user, &session).code == 0 &&
            session.state == ANOMALY_NTE_SESSION_V1_WORLD_READY && session.world.id;
        // Re-query after loading/world transitions: the table may be unpublished or
        // retired, and querying the current service arms the Host player sampler.
        if (ready)
            c.player = Query<AnomalyNtePlayerServiceV1>(c.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
                offsetof(AnomalyNtePlayerServiceV1, hold_engage));
        const bool player_ready = ready && c.player && c.player->snapshot &&
            c.player->snapshot(c.player->user, &player).code == 0 &&
            (player.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) &&
            !(player.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE);
        const Handle world = ready ? session.world : Handle{};
        // Session.world is {1, world_generation}, NOT an encoded UObject handle.
        // Keep that lifecycle token separate from the pinned native UWorld.
        if (!Same(world, c.session_world)) {
            c.studio->World({});
            c.session_world = world;
            c.catalog = std::make_shared<const std::vector<Character>>();
            c.batches.clear();
            c.needs_catalog = true;
            c.until_renew = 0;
            c.player_ready = false;
            Status(c, ready ? c.localizer.Text("status.reading_catalog",
                                "Reading the complete character catalog")
                            : c.localizer.Text("status.waiting_world",
                                "Waiting for the world to be ready"));
            Publish(c, false);
        }
        std::vector<Command> commands;
        {
            std::scoped_lock lock(c.mutex);
            commands.swap(c.requests);
        }
        if (!ready) {
            const std::string state = std::to_string(session.state);
            const std::array arguments{std::string_view(state)};
            Status(c, c.localizer.Format("status.waiting_world_session",
                "Waiting for the world to be ready; session state={0}", arguments));
            Publish(c, false);
            return; // Loading commands are discarded, not replayed into a new world.
        }
        const auto native_world = c.engine->Read<Address>(c.world_slot);
        if (!c.engine->IsA(native_world, "World"))
            throw std::runtime_error(c.localizer.Text("error.world_invalid",
                "GWorld does not expose a valid UWorld yet; refresh the catalog"));
        const auto object_world = c.engine->Pin(native_world);
        if (!object_world.id)
            throw std::runtime_error(c.localizer.Text("error.world_handle_invalid",
                "The UWorld object handle failed validation; refresh the catalog"));
        if (!Same(object_world, c.studio->world())) {
            c.studio->World(object_world);
            c.needs_catalog = true;
        }
        for (const auto& cmd : commands)
            if (cmd.action == Action::refresh && Same(cmd.world, world)) c.needs_catalog = true;
        bool changed = false;
        if (c.needs_catalog) {
            c.needs_catalog = false; // A failed catalog is retried only on explicit refresh.
            c.studio->Catalog();
            c.catalog = std::make_shared<const std::vector<Character>>(c.studio->characters());
            // Check the reported failing contract at world readiness, without
            // creating or moving an actor. Keep a usable catalog on failure.
            const auto& movement = c.engine->Bind("/Script/Engine.Actor.K2_SetActorLocationAndRotation");
            if (c.process.core && c.process.core->log)
                c.process.core->log(c.process.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    StringView("Character Studio: K2_SetActorLocationAndRotation contract "
                               "validated, ParmsSize=" + std::to_string(movement.size)));
            const std::string characters = std::to_string(c.catalog->size());
            const std::string catalog_state = player_ready
                ? c.localizer.Text("status.spawn_ready", "local models can be spawned repeatedly")
                : c.localizer.Text("status.player_pending",
                    "the player position is not ready, browsing stays available");
            const std::array arguments{std::string_view(characters), std::string_view(catalog_state)};
            Status(c, c.localizer.Format("status.catalog_loaded",
                "Catalog loaded: {0} characters; {1}", arguments));
            changed = true;
        }
        if (player_ready != c.player_ready) {
            c.player_ready = player_ready;
            Status(c, player_ready
                ? c.localizer.Text("status.player_ready",
                    "The player position is ready, so characters can be spawned")
                : c.localizer.Text("status.player_pending",
                    "the player position is not ready, browsing stays available"));
            changed = true;
        }
        for (const auto& cmd : commands) {
            if (!Same(cmd.world, world)) continue;
            changed = true;
            switch (cmd.action) {
            case Action::refresh:
                break;
            case Action::spawn: {
                if (!player_ready)
                    throw std::runtime_error(c.localizer.Text("error.player_not_ready",
                        "The player position is not ready; the catalog can be browsed, spawn later"));
                if (!cmd.count || !cmd.position.finite() || !std::isfinite(cmd.yaw) ||
                    !std::isfinite(cmd.spacing) || cmd.spacing <= 0)
                    throw std::runtime_error(c.localizer.Text("error.spawn_arguments_invalid",
                        "The count, position, angle or spacing is not valid"));
                AnomalyNteCameraSnapshotV1 camera{sizeof(camera)};
                if (!c.player->camera_snapshot ||
                    c.player->camera_snapshot(c.player->user, &camera).code != 0 ||
                    !(camera.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) ||
                    (camera.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) ||
                    !Same(camera.world, world) || !Same(camera.player, player.handle) ||
                    !std::isfinite(camera.rotation[1]))
                    throw std::runtime_error(c.localizer.Text("error.camera_not_ready",
                        "The player camera is not ready; spawn later"));
                const auto pawn = c.engine->Call(
                    "/Script/Engine.GameplayStatics.GetPlayerPawn",
                    c.engine->Find("/Script/Engine.Default__GameplayStatics"),
                    [&](Engine::Parameters& p) {
                        p.Put("WorldContextObject", native_world);
                        p.Put("PlayerIndex", std::int32_t{0});
                    }).Get<Address>("ReturnValue");
                if (!c.engine->IsA(pawn, "Character"))
                    throw std::runtime_error(c.localizer.Text("error.pawn_not_ready",
                        "The player character is not ready; spawn later"));
                const auto mesh = c.engine->ObjectField(pawn, "Mesh");
                if (!c.engine->IsA(mesh, "SkeletalMeshComponent"))
                    throw std::runtime_error(c.localizer.Text("error.player_mesh_not_ready",
                        "The player body mesh is not ready; spawn later"));
                // Match the visible mesh, including its rotation relative to the Pawn.
                // Placement and grid axes below remain camera-relative.
                const auto facing = c.engine->Call(
                    "/Script/Engine.SceneComponent.K2_GetComponentRotation", mesh)
                    .Get<Vector>("ReturnValue");
                if (!std::isfinite(facing.y))
                    throw std::runtime_error(c.localizer.Text("error.player_facing_not_ready",
                        "The player body facing is not ready; spawn later"));
                const auto yaw = std::remainder(
                    std::remainder(facing.y, 360.0) + std::remainder(cmd.yaw, 360.0), 360.0);
                const auto radians = std::remainder(camera.rotation[1], 360.0) *
                    3.14159265358979323846 / 180.0;
                const auto cosine = std::cos(radians), sine = std::sin(radians);
                c.batches.push_back({cmd.character, cmd.count,
                    {player.position[0] + cosine * cmd.position.x - sine * cmd.position.y,
                     player.position[1] + sine * cmd.position.x + cosine * cmd.position.y,
                     player.position[2] + cmd.position.z}, yaw,
                    {-sine * cmd.spacing, cosine * cmd.spacing, 0},
                    {cosine * cmd.spacing, sine * cmd.spacing, 0}});
                c.status_failed = false;
                c.status = c.localizer.Text("status.spawning",
                    "Spawning frame by frame; the plugin sets no total limit");
                break;
            }
            case Action::destroy:
                c.studio->Destroy(cmd.model);
                c.status_failed = false;
                c.status = c.localizer.Text("status.destroy_requested",
                    "Destruction of the selected model requested");
                break;
            case Action::clear:
                c.batches.clear();
                c.studio->DestroyAll();
                c.status_failed = false;
                c.status = c.localizer.Text("status.clear_requested",
                    "Queue cancelled; destroying every model from this plugin requested");
                break;
            case Action::move:
                c.studio->Move(cmd.model, cmd.position, cmd.yaw);
                c.status_failed = false;
                c.status = c.localizer.Text("status.move_confirmed",
                    "Model position read back and confirmed");
                break;
            case Action::cancel:
                c.batches.clear();
                c.status_failed = false;
                c.status = c.localizer.Text("status.cancel_requested",
                    "Pending spawn queue cancelled");
                break;
            }
        }
        if (!c.batches.empty()) {
            auto& batch = c.batches.front();
            const auto column = batch.placed % 3, row = batch.placed / 3;
            const Vector position{
                batch.position.x + column * batch.right.x + row * batch.forward.x,
                batch.position.y + column * batch.right.y + row * batch.forward.y,
                batch.position.z};
            c.studio->Spawn(batch.character, position, batch.yaw);
            ++batch.placed;
            if (!--batch.remaining) c.batches.pop_front();
            changed = true;
            if (c.batches.empty()) {
                const std::string models = std::to_string(c.studio->models().size());
                const std::array arguments{std::string_view(models)};
                Status(c, c.localizer.Format("status.spawn_done",
                    "Spawning finished: {0} models present", arguments));
            }
        }
        c.studio->Animate(delta);
        if (std::isfinite(delta) && delta > 0) c.until_renew -= delta;
        if (c.until_renew <= 0) {
            c.studio->Renew();
            c.until_renew = RenewSeconds;
            changed = true;
        }
        if (changed) Publish(c, !c.catalog->empty());
    } catch (const std::exception& error) {
        c.batches.clear();
        try {
            const std::string detail = error.what();
            const std::array arguments{std::string_view(detail)};
            Status(c, c.localizer.Format("status.failed", "Operation failed: {0}", arguments),
                   true);
            Publish(c, !c.catalog->empty() && !c.process.faulted);
        } catch (...) {}
    } catch (...) {
        c.batches.clear();
        try {
            c.status_failed = true;
            c.status = c.localizer.Text("status.exception",
                "A model operation raised an exception; the spawn queue was cancelled");
            Publish(c, false);
        } catch (...) {}
    }
}
void Request(Context& c, Command command) {
    std::scoped_lock lock(c.mutex);
    c.requests.push_back(command);
}
void Text(const AnomalyUiServiceV1* ui, const std::string& text) {
    ui->text(ui->user, StringView(text));
}
bool Button(const AnomalyUiServiceV1* ui, const std::string& label) {
    return ui->button(ui->user, StringView(label), 0, 0) != 0;
}
struct GridScope {
    const AnomalyUiServiceV1* ui;
    bool open;
    ~GridScope() { if (open) ui->end_table(ui->user); }
};
// Leave a small label gutter; only the numeric field grows, never its native step buttons.
void ModelCoordinate(const AnomalyUiServiceV1* ui, const char* label, double& value,
                     double step, double fast_step) {
    if (ui->struct_size >= offsetof(AnomalyUiServiceV1, set_next_item_width) +
            sizeof(ui->set_next_item_width) && ui->set_next_item_width)
        ui->set_next_item_width(ui->user, -52);
    ui->input_double(ui->user, StringView(label), &value, step, fast_step);
}
struct ChildScope {
    const AnomalyUiServiceV1* ui;
    bool open;
    ~ChildScope() { ui->end_child(ui->user); }
};
struct TabBarScope {
    const AnomalyUiServiceV1* ui;
    bool open;
    void Close() {
        if (!open) return;
        open = false;
        ui->end_tab_bar(ui->user);
    }
    ~TabBarScope() { Close(); }
};
struct TabItemScope {
    const AnomalyUiServiceV1* ui;
    bool open;
    ~TabItemScope() { if (open) ui->end_tab_item(ui->user); }
};
void ANOMALY_CALL Draw(void* context, const AnomalyUiServiceV1* ui) noexcept {
    if (!context || !ui || ui->struct_size < offsetof(AnomalyUiServiceV1, end_tab_bar) +
            sizeof(ui->end_tab_bar) ||
        !ui->begin_window || !ui->end_window || !ui->text || !ui->button ||
        !ui->begin_child || !ui->end_child || !ui->same_line ||
        !ui->checkbox || !ui->input_text || !ui->input_uint32 || !ui->input_double ||
        !ui->begin_tab_bar || !ui->begin_tab_item || !ui->end_tab_item || !ui->end_tab_bar) return;
    try {
        auto& c = *static_cast<Context*>(context);
        auto& r = c.render;
        const auto view = c.view.load();
        if (!r.window_initialized && ui->set_next_window_size) {
            ui->set_next_window_size(ui->user, 820, 460, 0);
            r.window_initialized = true;
        }
        int open = 1;
        anomaly::sdk::UiWindow window(ui, c.localizer.Text("window.title", "Character Studio"),
                                      &open);
        if (!window) return;
        if (!Same(r.world, view->world)) {
            r.world = view->world;
            r.selected = 0;
            r.character = {};
            r.character_page = r.model_page = 0;
        }
        const auto same_line = [&] {
            if (ui->struct_size >= offsetof(AnomalyUiServiceV1, same_line) +
                    sizeof(ui->same_line) && ui->same_line) ui->same_line(ui->user, 0, -1);
        };
        const auto enabled_button = [&](const std::string& label, bool enabled) {
            if (ui->struct_size >= offsetof(AnomalyUiServiceV1, button_enabled) +
                    sizeof(ui->button_enabled) && ui->button_enabled)
                return ui->button_enabled(ui->user, StringView(label), 0, 0, enabled) != 0;
            return Button(ui, label) && enabled;
        };
        constexpr std::size_t character_page_size = 12, model_page_size = 8;
        const auto pager = [&](std::size_t& page, std::size_t count, const char* id,
                               std::size_t page_size) {
            const auto pages = (count + page_size - 1) / page_size;
            if (page >= pages) page = pages ? pages - 1 : 0;
            if (enabled_button(c.localizer.Text("action.previous_page", "Previous") + "##" + id,
                               page > 0)) --page;
            same_line();
            Text(ui, std::to_string(pages ? page + 1 : 0) + " / " + std::to_string(pages));
            same_line();
            if (enabled_button(c.localizer.Text("action.next_page", "Next") + "##" + id,
                               page + 1 < pages)) ++page;
        };
        TabBarScope tabs{ui, ui->begin_tab_bar(ui->user, StringView("character-studio-tabs"),
                                             ANOMALY_UI_TAB_BAR_V1_NONE) != 0};
        if (!tabs.open) return;
        {
            TabItemScope tab{ui, ui->begin_tab_item(ui->user,
                                                   StringView(c.localizer.Text("tab.spawn", "Spawn")),
                                                   nullptr, 0, 1) != 0};
            if (tab.open) {
                if (ui->input_text(ui->user, StringView(c.localizer.Text("label.search", "Search")),
                                   r.filter, sizeof(r.filter), 0))
                    r.character_page = 0;
                same_line();
                if (Button(ui, c.localizer.Text("action.refresh", "Refresh")))
                    Request(c, {Action::refresh, view->world});
                std::vector<const Character*> filtered;
                const Character* selected{};
                for (const auto& item : *view->characters) {
                    if (item.chinese.find(r.filter) != std::string::npos ||
                        item.code.find(r.filter) != std::string::npos) {
                        filtered.push_back(&item);
                        if (item.id == r.character) selected = &item;
                    }
                }
                if (!selected && !filtered.empty()) {
                    selected = filtered.front();
                    r.character = selected->id;
                }
                pager(r.character_page, filtered.size(), "characters", character_page_size);
                {
                    GridScope grid{ui, ui->begin_table && ui->end_table && ui->table_next_row &&
                        ui->table_next_column && ui->begin_table(ui->user,
                            StringView("character-grid"), 3,
                            ANOMALY_UI_TABLE_V1_SIZING_STRETCH_PROP, 0, 0) != 0};
                    const auto start = r.character_page * character_page_size;
                    for (std::size_t i = start;
                         i < filtered.size() && i < start + character_page_size; ++i) {
                        if (grid.open) {
                            if ((i - start) % 3 == 0) ui->table_next_row(ui->user);
                            ui->table_next_column(ui->user);
                        } else if ((i - start) % 3 != 0) same_line();
                        const auto& item = *filtered[i];
                        const auto label = item.chinese + "##character-" + item.code;
                        int checked = item.id == r.character;
                        if (ui->checkbox(ui->user, StringView(label), &checked) && checked) {
                            selected = &item;
                            r.character = item.id;
                        }
                    }
                }
                if (filtered.empty()) {
                    Text(ui, view->characters->empty()
                        ? c.localizer.Text("status.catalog_empty", "The catalog has no characters")
                        : c.localizer.Text("status.no_match", "No character matches the filter"));
                }
                if (selected) {
                    const std::array arguments{std::string_view(selected->chinese)};
                    Text(ui, c.localizer.Format("text.selected", "Selected: {0}", arguments));
                }
                ui->input_uint32(ui->user, StringView(c.localizer.Text("label.count", "Count")),
                                 &r.count, 1, 10);
                if (enabled_button(c.localizer.Text("action.spawn", "Spawn"),
                                   view->ready && selected && r.count > 0))
                    Request(c, {Action::spawn, view->world, r.character, 0, r.count,
                                r.offset, r.facing_offset, r.spacing});
                if (view->queued) {
                    same_line();
                    const std::string queued = std::to_string(view->queued);
                    const std::array arguments{std::string_view(queued)};
                    if (Button(ui, c.localizer.Format("action.cancel_queue", "Cancel queue ({0})",
                            arguments)))
                        Request(c, {Action::cancel, view->world});
                }
            }
        }
        {
            const std::string model_count = std::to_string(view->models.size());
            const std::array tab_arguments{std::string_view(model_count)};
            const auto tab_label = c.localizer.Format("tab.models", "Spawned ({0})",
                tab_arguments) + "###models";
            TabItemScope tab{ui, ui->begin_tab_item(ui->user, StringView(tab_label),
                                                   nullptr, 0, 1) != 0};
            if (tab.open) {
                if (view->models.empty()) Text(ui, c.localizer.Text("status.no_models", "No models yet"));
                else {
                    const Model* selected{};
                    for (const auto& model : view->models)
                        if (model.id == r.selected) selected = &model;
                    {
                        ChildScope list{ui, ui->begin_child(ui->user,
                            StringView("model-list"), 220, 0, ANOMALY_UI_CHILD_V1_AUTO_RESIZE_Y) != 0};
                        if (list.open) {
                            pager(r.model_page, view->models.size(), "models", model_page_size);
                            for (std::size_t i = r.model_page * model_page_size;
                                 i < view->models.size() && i < (r.model_page + 1) * model_page_size; ++i) {
                                const auto& model = view->models[i];
                                const auto label = model.label.substr(0, model.label.find(" ["));
                                if (Button(ui, "#" + std::to_string(model.id) + " " + label)) {
                                    selected = &model;
                                    r.selected = model.id;
                                    r.position = model.position;
                                    r.model_yaw = model.yaw;
                                }
                            }
                            if (Button(ui, c.localizer.Text("action.clear_all", "Clear all")))
                                Request(c, {Action::clear, view->world});
                        }
                    }
                    same_line();
                    ChildScope editor{ui, ui->begin_child(ui->user,
                        StringView("model-editor"), 0, 0, ANOMALY_UI_CHILD_V1_AUTO_RESIZE_Y) != 0};
                    if (editor.open && selected) {
                        const std::string model_id = std::to_string(selected->id);
                        const std::string pose = selected->animation.starts_with("loop")
                            ? c.localizer.Text("label.idle", " · Idle")
                            : c.localizer.Text("label.reference_pose", " · Reference pose");
                        const std::array arguments{std::string_view(model_id),
                                                   std::string_view(pose)};
                        Text(ui, c.localizer.Format("text.editing", "Edit #{0}{1}", arguments));
                        ModelCoordinate(ui, "X", r.position.x, 10, 100);
                        ModelCoordinate(ui, "Y", r.position.y, 10, 100);
                        ModelCoordinate(ui, "Z", r.position.z, 10, 100);
                        ModelCoordinate(ui, c.localizer.Text("label.facing", "Facing").c_str(),
                                        r.model_yaw, 5, 45);
                        if (Button(ui, c.localizer.Text("action.apply", "Apply")))
                            Request(c, {Action::move, view->world, {}, r.selected, 1, r.position, r.model_yaw});
                        same_line();
                        if (Button(ui, c.localizer.Text("action.destroy_selected", "Destroy selected")))
                            Request(c, {Action::destroy, view->world, {}, r.selected});
                    } else if (editor.open) {
                        Text(ui, c.localizer.Text("hint.select_model",
                            "Select a model on the left to edit it"));
                    }
                }
            }
        }
        {
            TabItemScope tab{ui, ui->begin_tab_item(ui->user,
                                                   StringView(c.localizer.Text("tab.settings", "Settings")),
                                                   nullptr, 0, 1) != 0};
            if (tab.open) {
                Text(ui, c.localizer.Text("section.spawn_position",
                    "Spawn position (camera relative, centimeters)"));
                ui->input_double(ui->user, StringView(c.localizer.Text("label.forward_back", "Forward / back")),
                                 &r.offset.x, 10, 100);
                ui->input_double(ui->user, StringView(c.localizer.Text("label.left_right", "Left / right")),
                                 &r.offset.y, 10, 100);
                ui->input_double(ui->user, StringView(c.localizer.Text("label.height", "Height")),
                                 &r.offset.z, 10, 100);
                ui->input_double(ui->user,
                                 StringView(c.localizer.Text("label.spacing", "Spacing (centimeters)")),
                                 &r.spacing, 10, 50);
                Text(ui, c.localizer.Text("section.model_facing",
                    "Model body facing (matches the player by default)"));
                ui->input_double(ui->user,
                                 StringView(c.localizer.Text("label.facing_offset",
                                                             "Body rotation offset (degrees)")),
                                 &r.facing_offset, 5, 45);
            }
        }
        tabs.Close();
        if (!view->ready || view->failed) Text(ui, view->status);
    } catch (...) {
        // SDK callback boundary: no exception may cross into Host rendering.
    }
}
} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("anomaly.local.nte.character-studio"), StringView("Character Studio"),
        StringView("Anomaly"), StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
