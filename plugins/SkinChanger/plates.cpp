#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "plates.hpp"
#include "profile.hpp"
#include "plate_animation.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "plate_access.hpp"

namespace {
struct Field { uintptr_t type{}; std::string name; int32_t size{}, offset{-1}; };
std::vector<Field> fields;
int32_t Offset(const Reader& r, uintptr_t type, const char* name, int32_t size) {
    for (const auto& f : fields) if (f.type == type && f.name == name && f.size == size) return f.offset;
    const auto original_type = type; int32_t offset = -1;
    for (int depth = 0; type && depth < 32 && offset < 0; ++depth) {
        uintptr_t field{}; if (!r.Read(type+112,field)) break;
        for (int i = 0; field && i < 2048; ++i) {
            if (r.NameAt(field+32) == name) {
                int32_t actual{};
                if (r.Read(field+52,actual) && actual == size && r.Read(field+68,offset) && offset >= 0 && offset < 0x10000) break;
                offset = -1; break;
            }
            if (!r.Read(field+72,field)) break;
        }
        if (!r.Read(type+64,type)) break;
    }
    fields.push_back({original_type,name,size,offset}); return offset;
}
struct ObjectCache { std::string path; AnomalyGenerationHandleV1 handle{}; };
std::vector<ObjectCache> cache;
AnomalyGenerationHandleV1 Handle(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, uintptr_t address) {
    int32_t index{}; AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if (!address || !r.Read(address+12,index) || index < 0 || objects->snapshot_at(objects->user,index,&snapshot).code != 0 ||
        Resolve(r,objects,snapshot.handle) != address) return {};
    return snapshot.handle;
}
uintptr_t Find(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, const char* path) {
    for (auto& entry : cache) if (entry.path == path) return Resolve(r,objects,entry.handle);
    AnomalyGenerationHandleV1 handle{};
    if (objects->find_exact(objects->user,StringView(path),&handle).code != 0) return 0;
    cache.push_back({path,handle}); return Resolve(r,objects,handle);
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(size,'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr); return result;
}
std::wstring Wide(const std::string& text) {
    const auto size = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!size) return {};
    std::wstring result(size,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),size); return result;
}
struct Scalar { FName name; float before{}; int slot{-1}; };
struct Mesh {
    AnomalyGenerationHandleV1 handle{}, material{};
    int material_index{};
    std::vector<Scalar> values;
    size_t width{};
};
struct Vehicle {
    AnomalyGenerationHandleV1 actor{}, component{};
    std::wstring original, last_frame;
    std::vector<Mesh> meshes;
    size_t width{7};
    Clock::time_point reapply_at{};
};
std::vector<Vehicle> vehicles;
std::wstring applied;
uintptr_t scalar_function{};
bool enabled{};
Clock::time_point next_probe{}, next_frame{}, epoch{};
std::atomic_int requested{};
std::atomic_bool dynamic{};
std::atomic_bool restart_animation{};
std::atomic<float> interval{0.15F};
std::atomic<std::shared_ptr<const std::string>> pending;
struct View { std::string status{"开启后自动应用到你乘坐的载具。"}; size_t width{7}; bool enabled{}; };
std::atomic<std::shared_ptr<const View>> published;
std::string status;
size_t width = 7;
std::array<char,128> editor{'B','1','A','N','K'};

void Publish() {
    auto view = std::make_shared<View>(); view->width = width; view->enabled = enabled;
    if (!status.empty()) view->status = status;
    published.store(std::move(view));
}
bool ScalarCall(const Reader& r, uintptr_t mesh, FName name, float value) {
    struct Params { FName name; float value; } params{name,value};
    static_assert(sizeof(Params) == 12);
    return scalar_function && Invoke(r,mesh,scalar_function,&params);
}
bool Restore(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    enabled = false; bool ok = true;
    for (size_t i = vehicles.size(); i > 0; --i) {
        bool restored = true;
        if (Resolve(r,objects,vehicles[i-1].actor)) for (auto& saved : vehicles[i-1].meshes) {
            const auto mesh = Resolve(r,objects,saved.handle);
            if (mesh) for (const auto& value : saved.values)
                if (!ScalarCall(r,mesh,value.name,value.before)) restored = false;
        }
        if (restored) vehicles.erase(vehicles.begin()+static_cast<std::ptrdiff_t>(i-1));
        else ok = false;
    }
    return ok;
}
bool CaptureMesh(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, uintptr_t mesh, Mesh& saved) {
    uintptr_t cls{}; RowArray materials{};
    if (!r.Read(mesh+16,cls)) return false;
    const auto offset = Offset(r,cls,"OverrideMaterials",16);
    if (offset < 0 || !r.Read(mesh+offset,materials) || materials.count < 1 || materials.count > 64 || materials.capacity < materials.count) return false;
    for (int i = 0; i < materials.count; ++i) {
        uintptr_t material{}; RowArray values{};
        if (!r.Read(materials.data+i*8,material) || !r.Read(material+16,cls)) continue;
        const auto scalars = Offset(r,cls,"ScalarParameterValues",16);
        if (scalars < 0 || !r.Read(material+scalars,values) || values.count < 1 || values.count > 256 || values.capacity < values.count) continue;
        std::map<int,Scalar> slots; std::optional<Scalar> mode;
        // FScalarParameterValue: reflected ParameterInfo and float ParameterValue.
        const auto type = Find(r,objects,"/Script/Engine.ScalarParameterValue"); int32_t stride{};
        if (!type || !r.Read(type+0x58,stride) || stride != 36 || Offset(r,type,"ParameterInfo",16) != 0 ||
            Offset(r,type,"ParameterValue",4) != 16) return false;
        for (int j = 0; j < values.count; ++j) {
            FName name{}; float value{};
            if (!r.Read(values.data+stride*j,name) || !r.Read(values.data+stride*j+16,value)) return false;
            const auto label = r.Name(name);
            if (label == "UseErrorCodePlate") mode = Scalar{name,value,-1};
            else for (int k = 0; k < 16; ++k) if (label == "CarPlate"+std::to_string(k)) slots[k] = {name,value,k};
        }
        if (slots.empty() || !mode || slots.begin()->first != 0 || slots.rbegin()->first+1 != static_cast<int>(slots.size())) continue;
        saved.handle = Handle(r,objects,mesh); if (!saved.handle.id) return false;
        saved.material = Handle(r,objects,material); saved.material_index = i;
        saved.width = slots.size(); saved.values.push_back(*mode);
        for (const auto& [index,value] : slots) saved.values.push_back(value);
        return true;
    }
    return false;
}
bool Select(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, Vehicle& target) {
    const auto combat = Host(api).Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID).get();
    AnomalyNteCombatantSnapshotV1 player{sizeof(player)};
    if (!combat || !combat->current_combatant || combat->current_combatant(combat->user,&player).code != 0) return false;
    const auto character = Resolve(r,objects,player.character);
    if (!character) return false;
    uintptr_t cls{}, vehicle{}, component{};
    if (!r.Read(character+16,cls)) return false;
    const auto land_type = Find(r,objects,"/Script/HTGame.CharacterVehicleState");
    const auto water_type = Find(r,objects,"/Script/HTGame.CharacterWaterVehicleState");
    const auto land = Offset(r,cls,"DrivingState",0x68);
    const auto current_land = land_type ? Offset(r,land_type,"CurrentDrivingVehicle",8) : -1;
    if (land >= 0 && current_land >= 0) r.Read(character+land+current_land,vehicle);
    const auto water = Offset(r,cls,"WaterVehicleDrivingState",16);
    const auto current_water = water_type ? Offset(r,water_type,"CurrentDrivingVehicle",8) : -1;
    if (!vehicle && water >= 0 && current_water >= 0) r.Read(character+water+current_water,vehicle);
    if (!vehicle) {
        const auto get_vehicle = Find(r,objects,"/Script/HTGame.HTPlayerCharacter.GetPossibleLastDriveVehicle");
        if (!get_vehicle || !r.Function(get_vehicle,"GetPossibleLastDriveVehicle",1,8) ||
            !Invoke(r,character,get_vehicle,&vehicle)) return false;
    }
    target.actor = Handle(r,objects,vehicle);
    if (!target.actor.id || !r.IsA(vehicle,"Actor") || !r.Read(vehicle+16,cls)) return false;
    const auto manager = Offset(r,cls,"ModuleManagerComponent",8);
    if (manager >= 0) r.Read(vehicle+manager,component);
    if (component && r.IsA(component,"HTVehicleModificationComponent")) target.component = Handle(r,objects,component);
    // A vehicle with plate-bearing meshes can work without the car modification component.
    const auto source = target.component.id ? component : vehicle;
    if (!r.Read(source+16,cls)) return false;
    const auto number = Offset(r,cls,"VehicleNumberPlate",16); RowArray text{};
    if (number >= 0 && r.Read(source+number,text) && text.count > 0 && text.count <= 64 && text.capacity >= text.count) {
        std::wstring value(text.count,L'\0');
        if (core->read_memory(core->user,text.data,{reinterpret_cast<uint8_t*>(value.data()),value.size()*2}).code == 0) {
            if (!value.empty() && value.back() == L'\0') value.pop_back(); target.original = std::move(value);
        }
    }
    return true;
}
bool MaterialUnchanged(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, const Mesh& saved) {
    const auto mesh = Resolve(r,objects,saved.handle); uintptr_t cls{}, material{}; RowArray array{};
    if (!mesh || !r.Read(mesh+16,cls)) return false;
    const auto offset = Offset(r,cls,"OverrideMaterials",16);
    return offset >= 0 && r.Read(mesh+offset,array) && saved.material_index < array.count &&
        r.Read(array.data+8*saved.material_index,material) && material == Resolve(r,objects,saved.material);
}
bool IsType(const Reader& r,uintptr_t object,uintptr_t type) {
    uintptr_t cls{}; if (!type || !r.Read(object+16,cls)) return false;
    for (int depth = 0; cls && depth < 64; ++depth) {
        if (cls == type) return true;
        if (!r.Read(cls+64,cls)) return false;
    }
    return false;
}
std::vector<uintptr_t> PlateMeshCandidates(const Reader& r,const AnomalyUe5ObjectsServiceV1* objects,uintptr_t actor) {
    std::vector<uintptr_t> result,queue,seen,owners{actor};
    const auto actor_type = Find(r,objects,"/Script/Engine.Actor");
    const auto scene_type = Find(r,objects,"/Script/Engine.SceneComponent");
    const auto mesh_type = Find(r,objects,"/Script/Engine.MeshComponent");
    const auto child_type = Find(r,objects,"/Script/Engine.ChildActorComponent");
    const auto root_offset = actor_type ? Offset(r,actor_type,"RootComponent",8) : -1;
    const auto children_offset = scene_type ? Offset(r,scene_type,"AttachChildren",16) : -1;
    const auto child_offset = child_type ? Offset(r,child_type,"ChildActor",8) : -1;
    uintptr_t root{};
    if (root_offset < 0 || children_offset < 0 || !r.Read(actor+root_offset,root) || !root) return result;
    queue.push_back(root);
    for (size_t index = 0; index < queue.size() && seen.size() < 256; ++index) {
        const auto node = queue[index];
        if (!node || std::find(seen.begin(),seen.end(),node) != seen.end()) continue;
        seen.push_back(node);
        // Attached riders and their equipment belong to another actor, not the vehicle.
        uintptr_t owner{};
        if (!r.Read(node+32,owner) || std::find(owners.begin(),owners.end(),owner) == owners.end()) continue;
        if (IsType(r,node,mesh_type)) result.push_back(node);
        if (child_offset >= 0 && IsType(r,node,child_type)) {
            uintptr_t child{}, child_root{};
            if (r.Read(node+child_offset,child) && child && r.Read(child+root_offset,child_root) && child_root) {
                owners.push_back(child); queue.push_back(child_root);
            }
        }
        RowArray children{};
        if (r.Read(node+children_offset,children) && children.count >= 0 && children.count <= 256 && children.capacity >= children.count)
            for (int i = 0; i < children.count && queue.size() < 512; ++i) {
                uintptr_t next{}; if (r.Read(children.data+i*8,next)) queue.push_back(next);
            }
    }
    return result;
}
bool RefreshVehicle(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects, Vehicle& target) {
    scalar_function = Find(r,objects,"/Script/Engine.MeshComponent.SetScalarParameterValueOnMaterials");
    if (!scalar_function || !r.Function(scalar_function,"SetScalarParameterValueOnMaterials",2,12)) return false;
    std::vector<Mesh> meshes;
    const auto actor = Resolve(r,objects,target.actor); if (!actor) return false;
    bool replaced = false;
    for (const auto mesh : PlateMeshCandidates(r,objects,actor)) {
        const auto handle = Handle(r,objects,mesh);
        const auto cached = std::find_if(target.meshes.begin(),target.meshes.end(),[&](const Mesh& m){return m.handle.id == handle.id;});
        Mesh saved;
        if (cached != target.meshes.end() && MaterialUnchanged(r,objects,*cached)) saved = *cached;
        else { if (!CaptureMesh(r,objects,mesh,saved)) continue; replaced = true; }
        if (std::any_of(meshes.begin(),meshes.end(),[&](const Mesh& m){return m.handle.id == saved.handle.id;})) continue;
        // The game's unchanged plate remains the restoration source across remounts.
        if (const auto baseline = plate_animation::Normalize(target.original,saved.width)) {
            const auto frame = plate_animation::Frame(*baseline,saved.width,0,0.15,false);
            for (auto& value : saved.values) value.before = value.slot < 0 ? 0.0F :
                static_cast<float>(plate_animation::Glyphs.find(frame[value.slot]));
        }
        meshes.push_back(std::move(saved));
    }
    if (meshes.empty()) return false;
    target.width = meshes.front().width;
    for (const auto& mesh : meshes) target.width = std::min(target.width,mesh.width);
    if (!plate_animation::Normalize(applied,target.width)) return false;
    if (replaced || meshes.size() != target.meshes.size()) { target.last_frame.clear(); target.reapply_at = {}; }
    target.meshes = std::move(meshes); return true;
}
void FollowVehicle(const Reader& r, const AnomalyUe5ObjectsServiceV1* objects) {
    vehicles.erase(std::remove_if(vehicles.begin(),vehicles.end(),[&](const Vehicle& v) {
        return !Resolve(r,objects,v.actor);
    }),vehicles.end());
    Vehicle candidate;
    if (!Select(r,objects,candidate)) {
        status = vehicles.empty() ? "已开启，等待上车。" : "已开启，自动跟随上车。"; return;
    }
    auto found = std::find_if(vehicles.begin(),vehicles.end(),[&](const Vehicle& v){return v.actor.id == candidate.actor.id;});
    if (found == vehicles.end()) {
        if (!RefreshVehicle(r,objects,candidate)) { status = "该载具车牌暂未就绪，正在等待。"; return; }
        width = candidate.width; vehicles.push_back(std::move(candidate));
    } else {
        if (found->component.id != candidate.component.id) { found->meshes.clear(); found->last_frame.clear(); }
        found->component = candidate.component; found->original = std::move(candidate.original);
        if (!RefreshVehicle(r,objects,*found)) { status = "车牌模型刷新中，正在等待。"; return; }
        width = found->width;
    }
    status = "已开启，自动跟随上车。";
}

}

namespace lite_plates {
void Load(const AnomalyHostApiV1* host) noexcept {
    api = host; core = Host(host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
}
void Start() noexcept {
    registry = 0; retry_init = {}; fields.clear(); cache.clear(); vehicles.clear();
    scalar_function = 0; enabled = false; requested = 0; dynamic = false;
    interval = 0.15F; restart_animation = false; applied.clear(); pending.store({});
    next_probe = next_frame = epoch = {}; status.clear(); width = 7; Publish();
    const Reader r{Host(api).Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get()};
    Initialize(r,Host(api).Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get());
}
void Stop() noexcept {
    enabled = false; requested = 0; dynamic = false;
    // Game refresh functions cannot run from the lifecycle thread. Appearance
    // overrides disappear when the vehicle rebuilds; the Restore button runs on Game.
    pending.store({}); vehicles.clear();
}
void Update() noexcept {
    try {
        if (!requested.load() && !enabled) return;
        Host host(api); const auto objects = host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        const auto framework = host.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
        const Reader r{host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get()};
        if (!objects || !framework || !framework->is_game_thread || !framework->is_game_thread(framework->user) || !Initialize(r,objects)) return;
        if (const auto command = requested.exchange(0)) {
            if (command == 2) {
                // After a plugin reload, restore the current vehicle from its
                // unchanged game plate even though the old material cache is gone.
                if (vehicles.empty()) {
                    Vehicle current;
                    if (Select(r,objects,current)) {
                        const auto previous = applied; applied = current.original;
                        if (RefreshVehicle(r,objects,current)) vehicles.push_back(std::move(current));
                        applied = previous;
                    }
                }
                status = Restore(r,objects) ? "已关闭，恢复原车牌。" : "恢复失败，请重试。";
            } else if (command == 1) {
                const auto text = pending.exchange({});
                const auto normalized = text ? plate_animation::Normalize(Wide(*text),width) : std::nullopt;
                if (normalized) {
                    applied = *normalized; enabled = true; epoch = Clock::now(); next_probe = next_frame = {};
                    for (auto& vehicle : vehicles) { vehicle.last_frame.clear(); vehicle.reapply_at = {}; }
                } else status = "字符不支持或超出车牌长度。";
            }
        }
        if (restart_animation.exchange(false)) {
            epoch = Clock::now(); next_frame = {};
            for (auto& vehicle : vehicles) vehicle.last_frame.clear();
        }
        if (enabled && Clock::now() >= next_probe) {
            next_probe = Clock::now()+std::chrono::milliseconds(500);
            FollowVehicle(r,objects);
        }
        if (enabled && Clock::now() >= next_frame) {
            next_frame = Clock::now()+std::chrono::milliseconds(30);
            const auto elapsed = std::chrono::duration<double>(Clock::now()-epoch).count();
            for (auto& vehicle : vehicles) {
                if (!Resolve(r,objects,vehicle.actor)) continue;
                const auto frame = plate_animation::Frame(applied,vehicle.width,elapsed,interval.load(),dynamic.load());
                // Getting on a vehicle can overwrite plate material parameters even
                // when the mesh is unchanged. Refresh static plates at a bounded rate.
                if (frame == vehicle.last_frame && Clock::now() < vehicle.reapply_at) continue;
                bool ok = true;
                for (auto& saved : vehicle.meshes) {
                    const auto mesh = Resolve(r,objects,saved.handle); if (!mesh) { ok = false; continue; }
                    for (const auto& value : saved.values) {
                        const float glyph = value.slot < 0 ? 0.0F : static_cast<float>(plate_animation::Glyphs.find(
                            static_cast<size_t>(value.slot) < frame.size() ? frame[value.slot] : L' '));
                        if (!ScalarCall(r,mesh,value.name,glyph)) ok = false;
                    }
                }
                if (ok) vehicle.last_frame = frame;
                vehicle.reapply_at = Clock::now()+std::chrono::milliseconds(500);
            }
        }
        Publish();
    } catch (...) { status = "车牌暂未就绪，正在等待。"; Publish(); }
}
void Draw(const AnomalyUiServiceV1* ui) noexcept {
    const auto view = published.load(); if (!view || !ui || !ui->input_text || !ui->text || !ui->checkbox) return;
    ui->text(ui->user,StringView("仅修改本机车牌外观。"));
    const bool edited = ui->input_text(ui->user,StringView("车牌文字"),editor.data(),editor.size(),0) != 0;
    ui->text(ui->user,StringView("最多 "+std::to_string(view->width)+" 个字符：A-Z、0-9、·、-、空格、♥"));
    const auto command = requested.load();
    int use = command ? command == 1 : view->enabled;
    if (ui->checkbox && ui->checkbox(ui->user,StringView("应用车牌"),&use)) {
        if (use) { pending.store(std::make_shared<const std::string>(editor.data())); requested = 1; }
        else requested = 2;
    } else if (edited && use) { pending.store(std::make_shared<const std::string>(editor.data())); requested = 1; }
    int motion = dynamic.load();
    if (ui->checkbox && ui->checkbox(ui->user,StringView("动态车牌"),&motion)) { dynamic = motion != 0; restart_animation = true; }
    if (motion && ui->slider_float) { float seconds = interval.load();
        if (ui->slider_float(ui->user,StringView("滚动间隔（秒）"),&seconds,0.05F,1.0F)) interval = seconds;
    }
    ui->text(ui->user,StringView(view->status));
}
}
