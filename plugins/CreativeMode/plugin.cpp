#include "engine.hpp"
#include "persistence.hpp"
#include "model_preview.hpp"
#include "asset_catalog.hpp"
#include <Windows.h>
#include <d3d12.h>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <filesystem>

namespace creative_mode {
bool InvokeNative(Address entry, Address receiver, Address function, void* parameters) noexcept {
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(entry)(
            reinterpret_cast<void*>(receiver), reinterpret_cast<void*>(function), parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool InvokeAccessor(Address entry,Address receiver,Address* result) noexcept {
    __try { *result = reinterpret_cast<Address(__fastcall*)(Address)>(entry)(receiver); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool InvokeDeferredCapture(Address entry,Address receiver) noexcept {
    __try { reinterpret_cast<void(__fastcall*)(Address)>(entry)(receiver); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::wstring NativeWide(std::string_view text) {
    const auto size = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!size) return {};
    std::wstring value(size,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),value.data(),size); return value;
}
std::string NativeContentVersion() {
    std::wstring name(32768,L'\0'); const auto length = GetModuleFileNameW(nullptr,name.data(),static_cast<DWORD>(name.size()));
    if (!length || length >= name.size()) return {}; name.resize(length);
    const std::filesystem::path executable(name);
    std::vector<std::filesystem::path> files{executable};
    const auto archives = executable.parent_path().parent_path().parent_path()/L"Content"/L"Paks";
    std::error_code error;
    if (std::filesystem::is_directory(archives,error)) {
        for (const auto& item : std::filesystem::recursive_directory_iterator(archives,std::filesystem::directory_options::skip_permission_denied)) {
            const auto extension = item.path().extension().wstring();
            if (item.is_regular_file() && (extension == L".pak" || extension == L".utoc" || extension == L".ucas")) files.push_back(item.path());
        }
    }
    std::sort(files.begin(),files.end()); std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](const void* data,std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    };
    // Version identity for disposable catalog data only. Archive metadata also
    // invalidates patches that update content without replacing the executable.
    for (const auto& file : files) {
        const auto path = file.native(); const auto size = std::filesystem::file_size(file);
        const auto changed = std::filesystem::last_write_time(file).time_since_epoch().count();
        mix(path.data(),path.size()*sizeof(wchar_t)); mix(&size,sizeof(size)); mix(&changed,sizeof(changed));
    }
    return "content-"+std::to_string(hash);
}
bool QueryAssetRecords(Address entry,Address registry,const NativeAssetPath& type,NativeArray& records) noexcept {
    __try { return reinterpret_cast<bool(__fastcall*)(Address,const NativeAssetPath*,NativeArray*,bool)>(entry)(registry,&type,&records,true); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FreeNativeMemory(Address free_memory,Address allocation) noexcept {
    if (!allocation) return true;
    __try { reinterpret_cast<void(__fastcall*)(Address)>(free_memory)(allocation); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReleaseAssetRecords(Address destroy,Address free_memory,NativeArray records) noexcept {
    if (!records.data) return true;
    __try {
        reinterpret_cast<void(__fastcall*)(Address,std::int32_t)>(destroy)(records.data,records.count);
        reinterpret_cast<void(__fastcall*)(Address)>(free_memory)(records.data); return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ExportSharedNative(Address resource,Address* shared_handle) noexcept {
    __try {
        ID3D12Device* device{};
        auto* texture = reinterpret_cast<ID3D12Resource*>(resource);
        if (FAILED(texture->GetDevice(__uuidof(ID3D12Device),reinterpret_cast<void**>(&device))) || !device) return false;
        HANDLE shared{};
        const auto result = device->CreateSharedHandle(texture,nullptr,GENERIC_ALL,nullptr,&shared);
        device->Release();
        if (FAILED(result)) return false;
        *shared_handle = reinterpret_cast<Address>(shared); return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void CloseSharedNative(Address shared_handle) noexcept {
    if (shared_handle) CloseHandle(reinterpret_cast<HANDLE>(shared_handle));
}

}

namespace {
using namespace creative_mode;
using anomaly::sdk::StringView;
template<class T> const T* Query(const AnomalyHostApiV1* host, const char* id, std::size_t minimum = sizeof(T)) {
    const auto* table = anomaly::sdk::Host(host).Query<T>(id).get();
    return table && table->struct_size >= minimum ? table : nullptr;
}
enum class Action { refresh, preview, place, cancel, move, duplicate, remove, clear, save, restore,
                    select_aim, select_screen, freeze_preview, drop_asset, stop_restore };
struct Command {
    Action action{};
    Handle world;
    Settings settings;
    std::uint64_t id{};
    Pose pose;
    bool collision{true};
    editor::Point cursor{};
    Vector surface_normal{0,0,1};
    bool has_surface{};
};
struct View {
    Handle world;
    std::string world_path, status;
    std::string catalog_error;
    bool ready{}, scanning{}, has_preview{}, restoring{}, camera_valid{};
    std::uint32_t scan_index{}, scan_count{};
    std::uint64_t focus{};
    std::shared_ptr<const std::vector<Asset>> catalog = std::make_shared<const std::vector<Asset>>();
    std::vector<LiveItem> items;
    std::optional<LiveItem> preview;
    SavedLayout saved;
    std::uint64_t edit_revision{};
    bool preview_follow{};
};
struct RenderState {
    Settings settings;
    char filter[129]{}, asset[513]{};
    Pose pose;
    bool collision{true};
    std::uint64_t selected{}, last_focus{};
    std::size_t catalog_page{}, item_page{};
    std::uint64_t last_edit_revision{};
    editor::Drag drag;
    AnomalyInputSnapshotV1 previous_input{sizeof(previous_input)};
    int hover_axis{-1};
    editor::Mode hover_mode{};
    PreviewTextures textures;
    bool orbit_dragging{},preview_reported{};
    editor::Point orbit_mouse{};
    std::uint64_t viewer_input_sequence{},next_viewer_capture{};
    std::string dragging_asset;
    AnomalyInputSnapshotV1 input{sizeof(input)};
    bool left_pressed{},left_released{};
    std::shared_ptr<const std::vector<Asset>> filtered_catalog;
    std::vector<std::size_t> filtered;
    std::string filtered_text,filtered_folder;
    std::uint32_t filtered_category{99};
};
struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::sdk::Localizer localizer;
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyWindowServiceV1* window{};
    const AnomalyInputServiceV1* input{};
    const AnomalyUe5AhudServiceV1* ahud{};
    Handle window_handle{}, toolbar_handle{}, ahud_subscription{};
    std::unique_ptr<CatalogNames> names;
    std::unique_ptr<Engine> engine;
    Persistence persistence;
    AssetCatalog assets;
    RenderState render;
    std::atomic<std::shared_ptr<const View>> view{std::make_shared<const View>()};
    std::atomic<std::uint64_t> highlight{};
    std::atomic<std::shared_ptr<const editor::Frame>> editor_frame;
    std::atomic<std::shared_ptr<const Settings>> editor_settings;
    std::atomic<std::shared_ptr<const editor::TransformIntent>> drag_intent;
    std::atomic<std::shared_ptr<const BrowserRequests>> browser_requests;
    std::atomic<std::shared_ptr<const GpuCapture>> gpu_capture;
    std::atomic<std::uint64_t> gpu_ack{};
    ModelPreview model_preview;
    std::mutex requests_mutex;
    std::deque<Command> requests;
    std::deque<Item> restore_queue;
    Handle world{};
    std::string world_path, status;
    SavedLayout saved;
    std::shared_ptr<const std::vector<Asset>> catalog = std::make_shared<const std::vector<Asset>>();
    std::vector<LiveItem> items;
    std::optional<LiveItem> preview;
    std::optional<editor::Point> pending_pick;
    LiveItem load_anchor;
    std::optional<Command> waiting_command;
    std::string loading_path;
    std::uint64_t loading_deadline{};
    std::int32_t load_uuid{};
    Settings preview_settings;
    AnomalyNteCameraSnapshotV1 camera{sizeof(camera)};
    std::uint64_t next_id{1}, focus{}, edit_revision{};
    double renew_in{};
    bool active{}, prepared{}, bindings_valid{}, scanning{}, ready{}, camera_valid{}, preview_follow{};
};
void Status(Context& c, std::string message, bool failed = false) {
    if (c.status == message) return;
    c.status = std::move(message);
    if (c.core && c.core->log)
        c.core->log(c.core->user, failed ? ANOMALY_CORE_LOG_LEVEL_V1_WARNING : ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            StringView("CreativeMode: " + c.status));
}
bool EnsureWorldAsset(Context& c,std::string_view path) {
    if (c.engine->MeshLoaded(path)) return true;
    if (!AssetPath(path)) throw std::runtime_error(c.localizer.Text("error.mesh","The engine did not accept this mesh"));
    if (c.loading_path != path) {
        if (!c.load_anchor.actor.id || !c.engine->Alive(c.load_anchor))
            c.load_anchor = c.engine->SpawnAuxiliary("/Script/Engine.StaticMeshActor",Pose{});
        c.engine->RequestMesh(path,c.load_anchor.actor,++c.load_uuid);
        c.loading_path = path; c.loading_deadline = GetTickCount64()+30000;
        const std::array args{path}; Status(c,c.localizer.Format("status.asset_loading","Loading the selected asset asynchronously: {0}",args));
    }
    if (GetTickCount64() > c.loading_deadline) {
        c.waiting_command.reset(); c.restore_queue.clear();
        throw std::runtime_error(c.localizer.Text("error.asset_timeout","The asset could not be loaded in time; select it again to retry"));
    }
    return false;
}
void Publish(Context& c) {
    auto v = std::make_shared<View>();
    v->world = c.world; v->world_path = c.world_path; v->status = c.status; v->ready = c.ready;
    v->catalog = c.catalog; v->items = c.items; v->preview = c.preview; v->has_preview = c.preview.has_value();
    v->scanning = c.scanning; v->restoring = !c.restore_queue.empty(); v->focus = c.focus; v->saved = c.saved;
    v->camera_valid = c.camera_valid;
    v->edit_revision = c.edit_revision; v->preview_follow = c.preview_follow;
    const auto catalog = c.assets.State(); v->catalog_error = catalog->error;
    c.view.store(std::move(v));
}
Pose Placement(Context& c, const Settings& settings) {
    if (!c.camera_valid || !Valid(settings))
        throw std::runtime_error(c.localizer.Text("error.camera", "A current camera and valid placement settings are required"));
    Pose pose;
    pose.rotation = settings.rotation; pose.scale = settings.scale;
    const auto direction = Forward({c.camera.rotation[0], c.camera.rotation[1], c.camera.rotation[2]});
    for (std::size_t axis = 0; axis < 3; ++axis) pose.position[axis] = c.camera.position[axis] + direction[axis] * settings.distance;
    pose.position[2] += settings.height;
    if (settings.snap) Snap(pose, settings.grid, settings.angle_step);
    return pose;
}
std::pair<Vector,Vector> DropSurface(Context& c,const Command& command) {
    if (command.has_surface) return {command.pose.position,command.surface_normal};
    if (!Valid(command.settings) || !c.player ||
        c.player->struct_size < offsetof(AnomalyNtePlayerServiceV1,get_object_handles)+sizeof(c.player->get_object_handles) || !c.player->get_object_handles)
        throw std::runtime_error(c.localizer.Text("error.drop","The cursor could not be projected into the current world"));
    Handle pawn{},controller{};
    if (c.player->get_object_handles(c.player->user,&pawn,&controller).code != 0)
        throw std::runtime_error(c.localizer.Text("error.drop","The cursor could not be projected into the current world"));
    return c.engine->DropPoint(controller,pawn,command.cursor,command.settings.distance);
}
LiveItem& Selected(Context& c, std::uint64_t id) {
    const auto found = std::find_if(c.items.begin(), c.items.end(), [&](const LiveItem& item) { return item.id == id; });
    if (found == c.items.end() || !c.engine->Alive(*found))
        throw std::runtime_error(c.localizer.Text("error.stale", "This placed object no longer exists"));
    return *found;
}
void CancelPreview(Context& c) {
    if (c.preview) { c.engine->Destroy(*c.preview); c.preview.reset(); }
}
void Add(Context& c, const Item& item) {
    if (c.items.size() >= MaximumItems)
        throw std::runtime_error(c.localizer.Text("error.limit", "The limit is 512 placed objects"));
    // Reserve first so a vector allocation cannot lose an engine-owned actor.
    if (c.items.size() == c.items.capacity()) c.items.reserve(std::min(MaximumItems, std::max(std::size_t{16}, c.items.size() * 2)));
    c.items.push_back(c.engine->Spawn(item, c.next_id++, false));
    c.focus = c.items.back().id; c.highlight = c.focus; ++c.edit_revision;
}
void Execute(Context& c, const Command& command) {
    if (!Same(command.world, c.world))
        throw std::runtime_error(c.localizer.Text("error.changed", "The world changed; submit the action again"));
    switch (command.action) {
    case Action::refresh:
        c.assets.Refresh(); c.scanning = true;
        break;
    case Action::preview:
        if (!c.restore_queue.empty()) break;
        CancelPreview(c);
        c.preview_settings = command.settings;
        c.preview = c.engine->Spawn({command.settings.asset, Placement(c, command.settings), command.settings.collision}, 0, true);
        c.preview_follow = true;
        break;
    case Action::place:
        if (!c.restore_queue.empty()) break;
        if (c.preview) {
            if (c.items.size() >= MaximumItems)
                throw std::runtime_error(c.localizer.Text("error.limit", "The limit is 512 placed objects"));
            c.items.reserve(c.items.size() + 1);
            c.engine->SetCollision(*c.preview, c.preview_settings.collision);
            c.preview->id = c.next_id++;
            c.items.push_back(std::move(*c.preview)); c.preview.reset(); c.focus = c.items.back().id;
            c.highlight = c.focus; ++c.edit_revision;
        } else Add(c, {command.settings.asset, Placement(c, command.settings), command.settings.collision});
        Status(c, c.localizer.Text("status.placed", "Object placed in the current world"));
        break;
    case Action::cancel: CancelPreview(c); break;
    case Action::move: {
        auto& item = Selected(c, command.id);
        c.engine->Move(item, command.pose); c.engine->SetCollision(item, command.collision);
        ++c.edit_revision;
        Status(c, c.localizer.Text("status.updated", "Placement updated"));
        break;
    }
    case Action::duplicate: {
        auto copy = Selected(c, command.id).item;
        copy.pose = command.pose;
        copy.pose.position[0] += command.settings.move_step;
        if (command.settings.snap) Snap(copy.pose, command.settings.grid, command.settings.angle_step);
        Add(c, copy);
        Status(c, c.localizer.Text("status.duplicated", "Object duplicated along world X"));
        break;
    }
    case Action::remove:
        c.engine->Destroy(Selected(c, command.id));
        std::erase_if(c.items, [&](const LiveItem& item) { return item.id == command.id; });
        break;
    case Action::clear:
        c.restore_queue.clear(); CancelPreview(c);
        for (const auto& item : c.items) c.engine->Destroy(item);
        c.items.clear();
        Status(c, c.localizer.Text("status.cleared", "All objects created by this plugin have been removed"));
        break;
    case Action::save:
        c.saved.world = c.world_path; c.saved.items.clear();
        for (const auto& item : c.items) c.saved.items.push_back(item.item);
        c.persistence.LayoutChanged(c.saved);
        Status(c, c.localizer.Text("status.saved", "Layout queued for saving; use Restore to place it again in this world"));
        break;
    case Action::restore:
        if (!c.items.empty() || c.preview || !c.restore_queue.empty() || c.saved.world != c.world_path)
            throw std::runtime_error(c.localizer.Text("error.restore", "Clear placed objects and return to the saved world before restoring"));
        c.restore_queue.assign(c.saved.items.begin(), c.saved.items.end());
        break;
    case Action::stop_restore:
        c.restore_queue.clear();
        Status(c, c.localizer.Text("status.cancelled", "Restore stopped; completed placements have been kept"));
        break;
    case Action::select_screen:
        c.pending_pick = command.cursor;
        break;
    case Action::select_aim: {
        const auto frame = c.editor_frame.load();
        if (frame && Same(frame->world,c.world)) c.pending_pick = editor::Point{frame->width/2.0f,frame->height/2.0f};
        break;
    }
    case Action::freeze_preview:
        c.preview_follow = false;
        break;
    case Action::drop_asset: {
        if (c.items.size() >= MaximumItems) throw std::runtime_error(c.localizer.Text("error.limit","The limit is 512 placed objects"));
        CancelPreview(c);
        const auto [point,normal] = DropSurface(c,command);
        Pose pose; pose.position = point; pose.rotation = command.settings.rotation; pose.scale = command.settings.scale;
        if (command.settings.snap) Snap(pose,command.settings.grid,command.settings.angle_step);
        c.items.reserve(c.items.size()+1);
        auto item = c.engine->Spawn({command.settings.asset,pose,false},c.next_id++,true);
        try {
            double support = std::numeric_limits<double>::max();
            for (const auto& corner : item.corners) support = std::min(support,editor::geometry::Dot(editor::geometry::Sub(corner,point),normal));
            pose.position = editor::geometry::Add(pose.position,editor::geometry::Scale(normal,-support));
            c.engine->Move(item,pose); c.engine->SetCollision(item,command.settings.collision);
            c.focus = item.id; c.highlight = item.id; ++c.edit_revision; c.items.push_back(std::move(item));
        } catch (...) { try { c.engine->Destroy(item); } catch (...) {} throw; }
        Status(c,c.localizer.Text("status.dropped","Asset placed on the surface under the cursor"));
        break;
    }
    }
}
void Request(Context& c, Command command) {
    std::scoped_lock lock(c.requests_mutex); c.requests.push_back(std::move(command));
}
void ANOMALY_CALL DrawAhud(void* raw, const AnomalyUe5AhudFrameV1* frame) noexcept;
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** output) noexcept {
    if (!output) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *output = nullptr;
    try {
        auto c = std::make_unique<Context>(); c->host = host; c->localizer = anomaly::sdk::Localizer(host);
        c->names = std::make_unique<CatalogNames>(c->localizer);
        c->core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID);
        c->framework = Query<AnomalyUe5FrameworkServiceV1>(host, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID);
        c->session = Query<AnomalyNteSessionServiceV1>(host, ANOMALY_NTE_SESSION_SERVICE_V1_ID);
        c->input = Query<AnomalyInputServiceV1>(host, ANOMALY_INPUT_SERVICE_V1_ID);
        c->player = Query<AnomalyNtePlayerServiceV1>(host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
            offsetof(AnomalyNtePlayerServiceV1, camera_snapshot) + sizeof(AnomalyNtePlayerServiceV1::camera_snapshot));
        const auto* objects = Query<AnomalyUe5ObjectsServiceV1>(host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);
        const auto* names = Query<AnomalyUe5NamesServiceV1>(host, ANOMALY_UE5_NAMES_SERVICE_V1_ID);
        c->persistence.Initialize(host);
        c->assets.Initialize(host,*c->names,c->localizer);
        c->render.textures.SetService(Query<AnomalyTextureServiceV1>(host,ANOMALY_TEXTURE_SERVICE_V1_ID),
            Query<AnomalyGpuTextureServiceV1>(host,ANOMALY_GPU_TEXTURE_SERVICE_V1_ID),c->core);
        const auto document = c->persistence.Snapshot(); c->saved = document.saved; c->render.settings = document.settings;
        c->editor_settings.store(std::make_shared<const Settings>(document.settings));
        std::copy(document.settings.filter.begin(), document.settings.filter.end(), c->render.filter);
        std::copy(document.settings.asset.begin(), document.settings.asset.end(), c->render.asset);
        try {
            if (!c->core || !c->core->read_memory || !c->framework || !c->framework->is_game_thread ||
                !c->session || !c->session->snapshot || !objects || !objects->generation || !objects->count ||
                !objects->find_exact || !names || !names->resolve_utf8)
                throw std::runtime_error(c->localizer.Text("error.services", "The required engine services are unavailable"));
            c->engine = std::make_unique<Engine>(*c->core, *objects, *names, *c->framework, c->localizer, *c->names);
            c->engine->Initialize(host);
            Status(*c, c->localizer.Text("status.waiting", "Waiting for the current world"));
        } catch (const std::exception& error) { c->engine.reset(); Status(*c, error.what(), true); }
        c->window = Query<AnomalyWindowServiceV1>(host, ANOMALY_WINDOW_SERVICE_V1_ID);
        if (c->window && c->window->register_window && c->window->begin && c->window->end && c->window->state) {
            AnomalyWindowSpecV1 spec{sizeof(spec)};
            const auto title = c->localizer.Text("window.title", "CreativeMode");
            spec.id = StringView("creative-mode"); spec.title = StringView(title);
            spec.initial_width = 1180; spec.initial_height = 780; spec.minimum_width = 950; spec.minimum_height = 520;
            spec.default_open = 1;
            static_cast<void>(c->window->register_window(c->window->user, &spec, &c->window_handle));
            const auto tools_title = c->localizer.Text("editor.title","World editor");
            spec.id = StringView("creative-mode-tools"); spec.title = StringView(tools_title);
            spec.initial_width = 540; spec.initial_height = 160; spec.minimum_width = 350; spec.minimum_height = 100;
            static_cast<void>(c->window->register_window(c->window->user,&spec,&c->toolbar_handle));
        }
        Publish(*c); *output = c.release(); return anomaly::sdk::Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED, 0, StringView("CreativeMode initialization failed")}; }
}
AnomalyStatusV1 ANOMALY_CALL Start(void* raw) noexcept {
    if (!raw) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto& c = *static_cast<Context*>(raw);
    if (c.engine && !c.ahud_subscription.id) {
        c.ahud = Query<AnomalyUe5AhudServiceV1>(c.host,ANOMALY_UE5_AHUD_SERVICE_V1_ID);
        if (!c.ahud || !c.ahud->subscribe || !c.ahud->unsubscribe)
            return {ANOMALY_STATUS_V1_UNAVAILABLE,0,StringView("World editor requires the native AHUD projection")};
        const auto status = c.ahud->subscribe(c.ahud->user,DrawAhud,&c,&c.ahud_subscription);
        if (status.code != 0) return status;
    }
    c.active = true; return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Stop(void* raw, std::uint32_t) noexcept {
    if (!raw) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    auto& c = *static_cast<Context*>(raw); c.active = false;
    if (c.ahud && c.ahud_subscription.id) {
        static_cast<void>(c.ahud->unsubscribe(c.ahud->user,c.ahud_subscription)); c.ahud_subscription = {};
    }
    c.editor_frame.store({}); c.drag_intent.store({});
    c.gpu_capture.store({});
    c.assets.Stop();
    // Game/Render and scope-owned Worker callbacks have already drained.
    // Engine lifespans remove generated objects within five unpaused seconds.
    c.persistence.Flush(); return anomaly::sdk::Ok();
}
void ANOMALY_CALL Unload(void* raw) noexcept { try { delete static_cast<Context*>(raw); } catch (...) {} }
void ANOMALY_CALL Update(void* raw, double delta) noexcept {
    if (!raw) return;
    auto& c = *static_cast<Context*>(raw);
    if (!c.active || !c.engine || !c.framework->is_game_thread(c.framework->user)) return;
    try {
        AnomalyNteSessionSnapshotV1 session{sizeof(session)};
        const bool ready = c.session->snapshot(c.session->user, &session).code == 0 &&
            session.state == ANOMALY_NTE_SESSION_V1_WORLD_READY && session.world.id;
        const Handle world = ready ? session.world : Handle{};
        if (!Same(world, c.world)) {
            c.items.clear(); c.preview.reset(); c.restore_queue.clear(); c.waiting_command.reset();
            c.load_anchor = {}; c.loading_path.clear(); c.world = world; c.world_path.clear();
            c.focus = 0; c.highlight = 0; c.scanning = false; c.prepared = false; c.bindings_valid = false;
            c.pending_pick.reset(); c.editor_frame.store({}); c.drag_intent.store({}); ++c.edit_revision;
            c.model_preview.ResetWorld(); c.gpu_capture.store({}); c.gpu_ack = 0;
            std::scoped_lock lock(c.requests_mutex); c.requests.clear();
        }
        c.ready = ready && c.bindings_valid; c.camera_valid = false;
        if (!ready) { Publish(c); return; }
        if (!c.prepared) {
            c.prepared = true;
            c.engine->Preflight(); c.world_path = c.engine->Path(c.engine->World());
            c.bindings_valid = true; c.ready = true;
            Status(c, c.localizer.Text("status.ready", "Ready to place objects in the current world"));
        }
        if (!c.bindings_valid) { Publish(c); return; }
        // The player service may be retired across a world transition.
        c.player = Query<AnomalyNtePlayerServiceV1>(c.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
            offsetof(AnomalyNtePlayerServiceV1, camera_snapshot) + sizeof(AnomalyNtePlayerServiceV1::camera_snapshot));
        c.camera = {sizeof(c.camera)};
        c.camera_valid = c.player && c.player->camera_snapshot &&
            c.player->camera_snapshot(c.player->user, &c.camera).code == 0 &&
            (c.camera.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) && !(c.camera.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) &&
            Finite({c.camera.position[0], c.camera.position[1], c.camera.position[2]}) &&
            Finite({c.camera.rotation[0], c.camera.rotation[1], c.camera.rotation[2]});
        if (c.assets.State()->phase == AssetCatalog::Phase::Prepare) c.assets.Prepare(*c.engine);
        const auto asset_state = c.assets.State();
        if (asset_state->phase == AssetCatalog::Phase::Failed && !asset_state->error.empty()) Status(c,asset_state->error,true);
        if (c.catalog != asset_state->assets && asset_state->phase == AssetCatalog::Phase::Ready) {
            const auto count = std::to_string(asset_state->assets->size()); const std::array args{std::string_view(count)};
            Status(c,asset_state->cached ? c.localizer.Format("status.catalog_cached","Loaded {0} model assets from the version cache",args) :
                c.localizer.Format("status.catalog_ready","Prepared and cached {0} model assets for this game version",args));
        }
        c.catalog = asset_state->assets;
        c.scanning = asset_state->phase != AssetCatalog::Phase::Ready && asset_state->phase != AssetCatalog::Phase::Failed;
        if (std::isfinite(delta) && delta > 0) c.renew_in -= delta;
        if (c.renew_in <= 0) {
            std::erase_if(c.items, [&](const LiveItem& item) {
                if (!c.engine->Alive(item)) return true;
                c.engine->Renew(item); return false;
            });
            if (c.preview) {
                if (c.engine->Alive(*c.preview)) c.engine->Renew(*c.preview);
                else c.preview.reset();
            }
            c.renew_in = 1;
            c.model_preview.Renew(*c.engine);
            if (c.load_anchor.actor.id && c.engine->Alive(c.load_anchor)) c.engine->Renew(c.load_anchor);
        }
        if (const auto edit = c.drag_intent.exchange({}); edit && Same(edit->world,c.world)) {
            if (edit->preview && c.preview) {
                c.preview_follow = false; c.engine->Move(*c.preview,edit->pose);
            } else if (!edit->preview) {
                c.engine->Move(Selected(c,edit->id),edit->pose); ++c.edit_revision;
            }
        }
        if (c.preview && c.camera_valid && c.preview_follow) c.engine->Move(*c.preview, Placement(c, c.preview_settings));
        std::optional<Command> command;
        { std::scoped_lock lock(c.requests_mutex); if (!c.requests.empty()) {
            command = std::move(c.requests.front()); c.requests.pop_front();
        } }
        if (command) {
            if (command->action == Action::cancel || command->action == Action::clear) c.waiting_command.reset();
            const bool needs_asset = command->action == Action::preview || command->action == Action::drop_asset ||
                (command->action == Action::place && !c.preview);
            if (needs_asset) {
                c.waiting_command.reset();
                if (!c.engine->MeshLoaded(command->settings.asset)) {
                    if (command->action == Action::drop_asset) {
                        const auto surface = DropSurface(c,*command); command->pose.position = surface.first;
                        command->surface_normal = surface.second; command->has_surface = true;
                    }
                    c.waiting_command = *command;
                }
                else Execute(c,*command);
            } else Execute(c,*command);
        }
        if (c.waiting_command) {
            if (EnsureWorldAsset(c,c.waiting_command->settings.asset)) {
                auto completed = std::move(*c.waiting_command); c.waiting_command.reset(); Execute(c,completed);
            }
        } else if (!command && !c.restore_queue.empty()) {
            const auto item = c.restore_queue.front();
            try { if (EnsureWorldAsset(c,item.asset)) { Add(c,item); c.restore_queue.pop_front(); } }
            catch (...) { c.restore_queue.clear(); throw; }
            if (c.restore_queue.empty()) Status(c, c.localizer.Text("status.restored", "Saved objects restored in the current world"));
        }
        if (!c.waiting_command && c.restore_queue.empty() && c.load_anchor.actor.id) {
            c.engine->Destroy(c.load_anchor); c.load_anchor = {}; c.loading_path.clear();
        }
        if (const auto browser = c.browser_requests.load()) {
            const auto image = c.model_preview.Tick(*c.engine,*browser,c.world,c.gpu_ack.load(),c.localizer);
            if (image && !image->error.empty()) {
                const std::array args{std::string_view(image->request.asset),std::string_view(image->error)};
                Status(c,c.localizer.Format("status.preview_error","Preview failed: {0} ({1})",args),true);
            }
            c.gpu_capture.store(image);
        }
        Publish(c);
    } catch (const std::exception& error) {
        if (c.engine->Faulted()) { c.bindings_valid = false; c.ready = false; c.restore_queue.clear(); }
        Status(c, error.what(), true);
        try { Publish(c); } catch (...) {}
    } catch (...) {
        try { Status(c, c.localizer.Text("error.update", "The editor operation failed"), true); Publish(c); } catch (...) {}
    }
}

#include "world_editor.hpp"

struct Tab {
    const AnomalyUiServiceV1* ui; bool open;
    ~Tab() { if (open) ui->end_tab_item(ui->user); }
};
struct Tabs {
    const AnomalyUiServiceV1* ui; bool open;
    ~Tabs() { if (open) ui->end_tab_bar(ui->user); }
};
#include "browser_ui.hpp"

void ANOMALY_CALL Draw(void* raw, const AnomalyUiServiceV1* ui) noexcept {
    if (!raw || !ui || ui->struct_size < offsetof(AnomalyUiServiceV1, end_tab_bar) + sizeof(ui->end_tab_bar) ||
        !ui->text || !ui->button_enabled || !ui->checkbox || !ui->input_double || !ui->input_text || !ui->separator ||
        !ui->same_line || !ui->filter_match || !ui->begin_tab_bar || !ui->end_tab_bar || !ui->begin_tab_item || !ui->end_tab_item ||
        !ui->button || !ui->begin_child || !ui->end_child || !ui->begin_table || !ui->end_table ||
        !ui->table_next_row || !ui->table_next_column) return;
    auto& c = *static_cast<Context*>(raw);
    try {
        c.render.textures.BeginFrame(GetTickCount64());
        struct PublishBrowser {
            Context& c;
            ~PublishBrowser() { c.browser_requests.store(std::make_shared<const BrowserRequests>(c.render.textures.FinishFrame())); }
        } publish_browser{c};
        if (const auto capture = c.gpu_capture.load(); capture && c.engine &&
            c.gpu_ack.load() != capture->request.ticket && Same(capture->world,c.view.load()->world)) {
            if (c.render.textures.Accept(*capture,*c.engine,c.localizer)) c.gpu_ack = capture->request.ticket;
        }
        const auto save = [&] {
            if (Valid(c.render.settings)) {
                c.persistence.SettingsChanged(c.render.settings);
                c.editor_settings.store(std::make_shared<const Settings>(c.render.settings));
            }
        };
        bool changed = SceneInput(c,*ui);
        DrawSceneEditor(c,*ui);
        changed |= DrawEditorToolbar(c,*ui);
        if (changed) save();
        if (c.window_handle.id) {
            AnomalyWindowStateV1 state{sizeof(state)};
            if (c.window->state(c.window->user, c.window_handle, &state).code != 0 || !state.open) return;
            int visible{};
            if (c.window->begin(c.window->user, c.window_handle, 0, &visible).code != 0) return;
            struct End { Context& c; ~End() { c.window->end(c.window->user, c.window_handle); } } end{c};
            if (visible) DrawContents(c, *ui);
        } else if (ui->begin_window && ui->end_window) {
            anomaly::sdk::UiWindow window(ui, c.localizer.Text("window.title", "CreativeMode"));
            if (window) DrawContents(c, *ui);
        }
        if (Valid(c.render.settings)) c.editor_settings.store(std::make_shared<const Settings>(c.render.settings));
    } catch (...) {}
}
}
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* output) {
    if (!output || output->struct_size < sizeof(*output)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *output = {sizeof(*output), 1, 0, StringView("anomaly.creative-mode"), StringView("CreativeMode"),
        StringView("Anomaly"), StringView("1.0.10"), Load, Start, Stop, Unload, Update, Draw, nullptr};
    return anomaly::sdk::Ok();
}
