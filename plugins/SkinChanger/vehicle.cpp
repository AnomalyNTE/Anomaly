#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "localization.hpp"
#include "vehicle.hpp"
#include "cosmetics.hpp"
#include "profile.hpp"
#include "vehicle_rules.hpp"
#include "vehicle_paint.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "plate_access.hpp"

namespace {
uint64_t Pack(FName n) {return (uint64_t{n.number}<<32)|n.id;}
FName Unpack(uint64_t n) {return {static_cast<uint32_t>(n),static_cast<uint32_t>(n>>32)};}
AnomalyGenerationHandleV1 Handle(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,uintptr_t p) {
    int32_t index{};AnomalyUe5ObjectSnapshotV1 s{sizeof(s)};
    if(!p || !r.Read(p+accessory::profile::native_layout.ObjectIndex,index) || index<0 || o->snapshot_at(o->user,index,&s).code!=0 || Resolve(r,o,s.handle)!=p) return {};
    return s.handle;
}
std::map<std::string,AnomalyGenerationHandleV1> objects_cache;
std::map<std::tuple<uint64_t,std::string,int>,int> offsets;
uintptr_t Find(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,const std::string& path) {
    auto& h=objects_cache[path];if(auto p=Resolve(r,o,h)) return p;
    h={};return o->find_exact(o->user,StringView(path),&h).code==0?Resolve(r,o,h):0;
}
int Offset(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,uintptr_t cls,const char* key,int size) {
    const auto h=Handle(r,o,cls);if(!h.id) return -1;
    const auto cache_key=std::tuple{h.id,std::string(key),size};
    if(auto f=offsets.find(cache_key);f!=offsets.end()) return f->second;
    for(int depth=0;cls && depth<32;++depth) {
        uintptr_t field{};if(!r.Read(cls+accessory::profile::native_layout.PropertyLink,field)) break;
        for(int n=0;field && n<2048;++n) {
            if(r.NameAt(field+accessory::profile::native_layout.FieldName)==key) {
                int bytes{},at{};
                if(r.Read(field+accessory::profile::native_layout.PropertySize,bytes) && bytes==size && r.Read(field+accessory::profile::native_layout.PropertyOffset,at) && at>=0 && at<0x20000) {
                    offsets[cache_key]=at;return at;
                }
                return -1;
            }
            if(!r.Read(field+accessory::profile::native_layout.PropertyNext,field)) break;
        }
        if(!r.Read(cls+accessory::profile::native_layout.Super,cls)) break;
    }
    return -1;
}
bool LocalFunction(const Reader& r,uintptr_t f,const char* name,uint8_t count,uint16_t bytes) {
    uint32_t flags{};return r.Function(f,name,count,bytes) && r.Read(f+accessory::profile::native_layout.FunctionFlags,flags) && (flags&0x400) && !(flags&0x40);
}
bool ReadIds(const Reader& r,uintptr_t at,std::vector<uint64_t>& result) {
    RowArray array{};if(!r.Read(at,array) || array.count<0 || array.count>128 || array.capacity<array.count || array.capacity>4096) return false;
    result.clear();for(int i=0;i<array.count;++i) {FName n{};if(!r.Read(array.data+sizeof(FName)*i,n) || !n.id) return false;result.push_back(Pack(n));}
    return true;
}
struct Target {AnomalyGenerationHandleV1 actor{},component{};FName vehicle{};std::vector<uint64_t> modules;int32_t color{-1};uint64_t entity{};};
bool Current(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,Target& t) {
    auto combat=Host(api).Query<AnomalyNteCombatServiceV1>(ANOMALY_NTE_COMBAT_SERVICE_V1_ID).get();
    AnomalyNteCombatantSnapshotV1 snapshot{sizeof(snapshot)};
    if(!combat || combat->current_combatant(combat->user,&snapshot).code!=0) return false;
    const auto actor=Resolve(r,o,snapshot.character);uintptr_t cls{},car{},component{};int32_t seat{-1};
    if(!actor || !r.Read(actor+accessory::profile::native_layout.ObjectClass,cls)) return false;
    const auto ds=Offset(r,o,cls,"DrivingState",104);
    const auto type=Find(r,o,"/Script/HTGame.CharacterVehicleState");
    const auto current=Offset(r,o,type,"CurrentDrivingVehicle",8),seat_at=Offset(r,o,type,"TargetSeatIndex",4);
    if(ds<0 || current<0 || seat_at<0 || !r.Read(actor+ds+current,car) || !car || !r.Read(actor+ds+seat_at,seat) || seat!=0 ||
       !r.IsA(car,"HTWheeledVehicleDrivable") || !r.Read(car+accessory::profile::native_layout.ObjectClass,cls)) return false;
    const auto manager=Offset(r,o,cls,"ModuleManagerComponent",8),id=Offset(r,o,cls,"VehicleID",8);
    const auto color=Offset(r,o,cls,"ColorID",4);
    const auto entity=Offset(r,o,cls,"OwnerEntityHandle",8);
    if(color<0 || entity<0 || !r.Read(car+color,t.color) || !r.Read(car+entity,t.entity)) return false;
    if(manager<0 || id<0 || !r.Read(car+manager,component) || !r.IsA(component,"HTVehicleModificationComponent") ||
       !r.Read(car+id,t.vehicle) || !t.vehicle.id || !r.Read(component+accessory::profile::native_layout.ObjectClass,cls)) return false;
    uintptr_t owner{};if(!r.Read(component+accessory::profile::native_layout.ObjectOuter,owner) || owner!=car) return false;
    const auto list=Offset(r,o,cls,"CurrentModuleIDList",16);
    t.actor=Handle(r,o,car);t.component=Handle(r,o,component);
    return t.actor.id && t.component.id && list>=0 && ReadIds(r,component+list,t.modules);
}
struct Entry {vehicle_rules::Row rule;std::string id,label;};
struct View {std::string status{lite_i18n::localizer.Text("vehicle.instructions", "Enter the driver seat to read this vehicle's appearance.")},vehicle;Target target;std::shared_ptr<const std::vector<Entry>> rows;bool scanning{};};
std::atomic<std::shared_ptr<const View>> published;
struct Request {AnomalyGenerationHandleV1 actor{},component{};uint64_t selected{};bool restore{};};
std::atomic<std::shared_ptr<const Request>> request;
std::atomic_bool visible{};
Clock::time_point next_tick{},retry_catalog{};
uintptr_t array_copy{};
AnomalyGenerationHandleV1 table_handle{};
uint64_t catalog_vehicle{};uintptr_t rows_data{};int rows_count{},row_cursor{};
std::vector<uint32_t> allocation;
std::vector<Entry> entries;
std::shared_ptr<const std::vector<Entry>> catalog_snapshot;
struct Saved {
    Target target;std::vector<uint64_t> desired;std::vector<Entry> catalog;
    uint64_t signature{};bool signature_valid{},refresh{};
    float paint{};bool paint_valid{};
    Clock::time_point observe{},settle{};
};
std::map<uint64_t,Saved> saved;
uint64_t last_driver{};
uint64_t monitor_cursor{};
std::string status;
bool submit_message{},catalog_notice{};
bool scanning{};
std::array<std::string,5> categories;
std::array<std::string,7> attachment_groups;
int category{},attachment_group{};uint64_t selected{};char search[128]{};

struct FloatView {const float* data{};int32_t count{};uint32_t padding{};};
static_assert(sizeof(FloatView)==16);
struct PaintBinding {
    uintptr_t mesh{},entity_address{},pool_address{};
    AnomalyGenerationHandleV1 handle{};uint64_t entity{};float paint{};
};
std::atomic<std::shared_ptr<const std::vector<PaintBinding>>> paint_bindings;
const AnomalyHookServiceV1* paint_hooks{};
AnomalyGenerationHandleV1 paint_hook{};
uintptr_t paint_original{};
std::atomic<uint64_t> paint_overrides{};
uint64_t last_paint_report{};Clock::time_point next_paint_report{};
bool BindingLive(const Reader& r,const PaintBinding& b) {
    const auto index=ANOMALY_UE5_OBJECT_HANDLE_INDEX(b.handle);
    uintptr_t chunks{},chunk{},object{};uint32_t count{},serial{};uint64_t entity{};uint8_t pool{};
    return r.Read(registry+accessory::profile::native_layout.RegistryCount,count) && index<count && r.Read(registry+accessory::profile::native_layout.RegistryItems,chunks) && r.Read(chunks+sizeof(uintptr_t)*(index/accessory::profile::native_layout.ChunkSize),chunk) &&
        r.Read(chunk+accessory::profile::native_layout.ItemStride*(index%accessory::profile::native_layout.ChunkSize),object) && object==b.mesh && r.Read(chunk+accessory::profile::native_layout.ItemStride*(index%accessory::profile::native_layout.ChunkSize)+accessory::profile::native_layout.Serial,serial) &&
        serial==ANOMALY_UE5_OBJECT_HANDLE_SERIAL(b.handle) && r.Read(b.entity_address,entity) && entity==b.entity &&
        r.Read(b.pool_address,pool) && !pool;
}
void __fastcall PaintDetour(void* component,int32_t index,const FloatView* values) noexcept {
    const auto original=reinterpret_cast<void(__fastcall*)(void*,int32_t,const FloatView*)>(paint_original);
    if(!original) return;
    AnomalyGenerationHandleV1 lease{};
    const bool leased=paint_hooks && paint_hooks->begin_callback(paint_hooks->user,paint_hook,&lease).code==0;
    FloatView replacement{};std::array<float,vehicle_paint::Maximum> input{},output{};
    const FloatView* argument=values;
    if(leased && index>=0 && index<=static_cast<int>(accessory::profile::native_layout.PaintColorIndex)) {
        const auto bindings=paint_bindings.load();
        if(bindings) {
            const auto address=reinterpret_cast<uintptr_t>(component);
            const auto found=std::lower_bound(bindings->begin(),bindings->end(),address,[](const PaintBinding& b,uintptr_t a){return b.mesh<a;});
            if(found!=bindings->end() && found->mesh==address) {
                const Reader r{};FloatView source{};
                if(BindingLive(r,*found) && r.Read(reinterpret_cast<uintptr_t>(values),source) && source.count>0 && source.count<=vehicle_paint::Maximum &&
                   core->read_memory(core->user,reinterpret_cast<uintptr_t>(source.data),{reinterpret_cast<uint8_t*>(input.data()),static_cast<size_t>(source.count)*4}).code==0 &&
                   vehicle_paint::Override(index,{input.data(),static_cast<size_t>(source.count)},found->paint,output,accessory::profile::native_layout.PaintColorIndex)) {
                    replacement={output.data(),source.count,0};argument=&replacement;
                    if(input[accessory::profile::native_layout.PaintColorIndex-index]!=found->paint) ++paint_overrides;
                }
            }
        }
    }
    original(component,index,argument);
    if(leased) paint_hooks->end_callback(paint_hooks->user,lease);
}
uintptr_t RelativeCall(const Reader& r,uintptr_t call) {
    uint8_t opcode{};int32_t displacement{};
    return r.Read(call,opcode) && opcode==0xe8 && r.Read(call+1,displacement)?
        static_cast<uintptr_t>(static_cast<intptr_t>(call)+5+displacement):0;
}
bool EnsurePaintHook(const Reader& r,const AnomalyUe5ObjectsServiceV1* o) {
    if(paint_hook.id) return true;
    if(!paint_hooks || !paint_hooks->create || !paint_hooks->release || !paint_hooks->begin_callback || !paint_hooks->end_callback) return false;
    const auto scalar=Find(r,o,"/Script/Engine.PrimitiveComponent.SetCustomPrimitiveDataFloat");
    const auto array=Find(r,o,"/Script/Engine.PrimitiveComponent.SetCustomPrimitiveDataFloatArray");
    uintptr_t scalar_exec{},array_exec{};
    if(!LocalFunction(r,scalar,"SetCustomPrimitiveDataFloat",2,8) || !LocalFunction(r,array,"SetCustomPrimitiveDataFloatArray",2,24) ||
       !r.Read(scalar+accessory::profile::native_layout.FunctionNative,scalar_exec) || !r.Read(array+accessory::profile::native_layout.FunctionNative,array_exec)) return false;
    const auto scalar_body=RelativeCall(r,scalar_exec+accessory::profile::native_layout.PaintScalarThunkCall),array_body=RelativeCall(r,array_exec+accessory::profile::native_layout.PaintArrayThunkCall);
    const auto entry=RelativeCall(r,scalar_body+accessory::profile::native_layout.PaintScalarBodyCall);
    if(!entry || RelativeCall(r,array_body+accessory::profile::native_layout.PaintArrayBodyCall)!=entry) return false;
    if(!accessory::profile::compatibility.MatchesEntry(core,"Paint.entry",entry)) return false;
    AnomalyHookRequestV1 spec{sizeof(spec),ANOMALY_HOOK_V1_FUNCTION,entry,reinterpret_cast<void*>(&PaintDetour),StringView("vehicle-paint-data")};
    if(paint_hooks->create(paint_hooks->user,&spec,&paint_original,&paint_hook).code!=0) return false;
    Log("Vehicle appearance: scoped native paint writer installed");return true;
}
void PublishPaintBindings(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,uint64_t exclude=0) {
    auto bindings=std::make_shared<std::vector<PaintBinding>>();
    for(const auto& [id,s]:saved) {
        if(id==exclude || !s.paint_valid) continue;
        const auto car=Resolve(r,o,s.target.actor);uintptr_t cls{};
        if(!car || !r.Read(car+accessory::profile::native_layout.ObjectClass,cls)) continue;
        const auto entity=Offset(r,o,cls,"OwnerEntityHandle",8),pool=Offset(r,o,cls,"bInMassPool",1);
        if(entity<0 || pool<0) continue;
        std::vector<uintptr_t> meshes;
        for(const auto key:{"Mesh","VehicleStaticMesh"}) {const auto at=Offset(r,o,cls,key,8);uintptr_t mesh{};if(at>=0 && r.Read(car+at,mesh) && mesh) meshes.push_back(mesh);}
        const auto component=Resolve(r,o,s.target.component);
        if(component && r.Read(component+accessory::profile::native_layout.ObjectClass,cls)) {
            const auto at=Offset(r,o,cls,"ModuleComponentMap",accessory::profile::native_layout.MapSize);RowArray map{};
            if(at>=0 && r.Read(component+at,map) && map.count>=0 && map.count<=64 && map.capacity>=map.count) {
                for(int n=0;n<map.count;++n) {uintptr_t mesh{};if(r.Read(map.data+accessory::profile::native_layout.MapEntryStride*n+accessory::profile::native_layout.MapValue,mesh) && mesh && r.IsA(mesh,"PrimitiveComponent")) meshes.push_back(mesh);}
            }
        }
        for(auto mesh:meshes) if(const auto handle=Handle(r,o,mesh);handle.id) bindings->push_back({mesh,car+entity,car+pool,handle,s.target.entity,s.paint});
    }
    std::sort(bindings->begin(),bindings->end(),[](const PaintBinding& a,const PaintBinding& b){return a.mesh<b.mesh;});
    bindings->erase(std::unique(bindings->begin(),bindings->end(),[](const PaintBinding& a,const PaintBinding& b){return a.mesh==b.mesh;}),bindings->end());
    paint_bindings.store(std::move(bindings));
}

bool PaintData(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,const Target& t,float paint,bool check_only) {
    const auto car=Resolve(r,o,t.actor),component=Resolve(r,o,t.component);uintptr_t cls{},mesh{};
    if(!car || !component || !std::isfinite(paint) || !r.Read(car+accessory::profile::native_layout.ObjectClass,cls)) return false;
    const auto mesh_at=Offset(r,o,cls,"Mesh",8);
    if(mesh_at<0 || !r.Read(car+mesh_at,mesh) || !r.IsA(mesh,"PrimitiveComponent") || !r.Read(mesh+accessory::profile::native_layout.ObjectClass,cls)) return false;
    const auto at=Offset(r,o,cls,"CustomPrimitiveDataInternal",16);RowArray array{};float current{};
    if(at<0 || !r.Read(mesh+at,array) || array.count<0 || array.count>256 || array.capacity<array.count) return false;
    const bool matches=array.count>static_cast<int>(accessory::profile::native_layout.PaintColorIndex) && r.Read(array.data+sizeof(float)*accessory::profile::native_layout.PaintColorIndex,current) && std::isfinite(current) && std::abs(current-paint)<0.0001f;
    if(check_only) return matches;
    const auto set=Find(r,o,"/Script/Engine.PrimitiveComponent.SetCustomPrimitiveDataFloat");
    const auto modules=Find(r,o,"/Script/HTGame.HTVehicleModificationComponent.ApplyPrimitiveDataToModules");
    if(!LocalFunction(r,set,"SetCustomPrimitiveDataFloat",2,8) || !r.Property(set,"DataIndex",0,4) || !r.Property(set,"Value",4,4) ||
       !LocalFunction(r,modules,"ApplyPrimitiveDataToModules",2,8) || !r.Property(modules,"DataIndex",0,4) || !r.Property(modules,"Value",4,4)) return false;
    struct Parameters {int32_t index;float value;} parameters{static_cast<int32_t>(accessory::profile::native_layout.PaintColorIndex),paint};static_assert(sizeof(parameters)==8);
    // The captured before/after transition changes only the configured colour slot.
    // The native setter also marks rendering dirty; entity/damage slots stay intact.
    return Invoke(r,mesh,set,&parameters) && Invoke(r,component,modules,&parameters);
}

bool CopyNamesCall(uintptr_t destination,const RowArray* source) noexcept {
    __try {
        reinterpret_cast<void*(__fastcall*)(void*,const void*)>(array_copy)(reinterpret_cast<void*>(destination),source);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool PrepareArrayCopy(const Reader&) {
    if(array_copy) return true;
    // The getter prologue is hooked after CosmeticsStart. Reuse its validated
    // array helper rather than trying to rescan the already-patched getter.
    auto target=CosmeticsNameArrayCopyAddress();
    if(!target) target=accessory::profile::compatibility.Address("CopyNames");
    MEMORY_BASIC_INFORMATION region{};
    if(!target || target!=accessory::profile::compatibility.Address("CopyNames") ||
       !VirtualQuery(reinterpret_cast<void*>(target),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
       !(region.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
    array_copy=target;return true;
}

bool StartCatalog(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,uint64_t vehicle) {
    const auto table=Find(r,o,"/Game/DataTable/Vehicle/DT_vehicleModuleData.DT_vehicleModuleData");
    uintptr_t schema{};RowArray map{};uintptr_t flags{};int free{},bits{};
    if(!table || !r.IsA(table,"DataTable") || !r.Read(table+accessory::profile::native_layout.RowStruct,schema) || r.NameAt(schema+accessory::profile::native_layout.ObjectName)!="VehicleModuleData" ||
       !r.Property(schema,"ModuleID",accessory::profile::native_layout.VehicleRowId,8) || !r.Property(schema,"VehicleID",accessory::profile::native_layout.VehicleRowOwner,8) || !r.Property(schema,"ModuleName",accessory::profile::native_layout.VehicleRowName,16) ||
       !r.Property(schema,"ModuleType",accessory::profile::native_layout.VehicleRowType,1) || !r.Read(table+accessory::profile::native_layout.RowMap,map) || map.count<0 || map.count>8192 || map.capacity<map.count ||
       !r.Read(table+accessory::profile::native_layout.RowMap+accessory::profile::native_layout.MapBitsData,flags) || !r.Read(table+accessory::profile::native_layout.RowMap+accessory::profile::native_layout.MapBitCount,bits) || bits<map.count || !r.Read(table+accessory::profile::native_layout.RowMap+accessory::profile::native_layout.MapFreeCount,free) || free<0 || free>map.count) return false;
    if(!flags) {if(map.count>128) return false;flags=table+accessory::profile::native_layout.RowMap+accessory::profile::native_layout.MapInlineBits;}
    allocation.resize((map.count+31)/32);
    if(map.count && core->read_memory(core->user,flags,{reinterpret_cast<uint8_t*>(allocation.data()),allocation.size()*4}).code!=0) return false;
    table_handle=Handle(r,o,table);rows_data=map.data;rows_count=map.count;row_cursor=0;catalog_vehicle=vehicle;entries.clear();catalog_snapshot.reset();scanning=true;
    return true;
}
void CatalogSlice(const Reader& r,const AnomalyUe5ObjectsServiceV1* o) {
    if(!scanning) return;
    const auto previous_size=entries.size();
    const auto table=Resolve(r,o,table_handle);RowArray now{};
    if(!table || !r.Read(table+accessory::profile::native_layout.RowMap,now) || now.data!=rows_data || now.count!=rows_count) {scanning=false;catalog_vehicle=0;return;}
    const auto until=Clock::now()+std::chrono::milliseconds(2);int processed{};
    while(row_cursor<rows_count && processed++<24 && Clock::now()<until) {
        const auto i=row_cursor++;
        if(!(allocation[i/32]&(1u<<(i%32)))) continue;
        uintptr_t row{};FName key{},id{},vehicle{};uint8_t type{};
        if(!r.Read(rows_data+accessory::profile::native_layout.MapEntryStride*i,key) || !r.Read(rows_data+accessory::profile::native_layout.MapEntryStride*i+accessory::profile::native_layout.MapValue,row) || !row || !r.Read(row+accessory::profile::native_layout.VehicleRowId,id) || Pack(id)!=Pack(key) ||
           !r.Read(row+accessory::profile::native_layout.VehicleRowOwner,vehicle) || Pack(vehicle)!=catalog_vehicle || !r.Read(row+accessory::profile::native_layout.VehicleRowType,type)) continue;
        Entry e{{Pack(id),Pack(vehicle),type},r.Name(id),{}};
        if(vehicle_rules::Category(type)>=0 && r.names && r.names->resolve_ftext_utf8) {
            char buffer[1024]{};size_t bytes=sizeof(buffer);
            if(r.names->resolve_ftext_utf8(r.names->user,row+accessory::profile::native_layout.VehicleRowName,buffer,&bytes).code==0) e.label=buffer;
        }
        entries.push_back(std::move(e));
    }
    if(entries.size()!=previous_size) catalog_snapshot.reset();
    if(row_cursor>=rows_count) {scanning=false;Log("Vehicle appearance: catalog ready entries="+std::to_string(entries.size()));}
}
bool SameInstance(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,const Target& target) {
    const auto car=Resolve(r,o,target.actor);uintptr_t cls{};uint64_t entity{};uint8_t pooled{};
    if(!car || !r.Read(car+accessory::profile::native_layout.ObjectClass,cls)) return false;
    const auto entity_at=Offset(r,o,cls,"OwnerEntityHandle",8),pool_at=Offset(r,o,cls,"bInMassPool",1);
    return entity_at>=0 && pool_at>=0 && r.Read(car+entity_at,entity) && entity==target.entity && r.Read(car+pool_at,pooled) && !pooled;
}
bool Submit(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,const Target& target,const std::vector<uint64_t>& ids,const std::vector<Entry>& catalog) {
    submit_message=false;
    if(!SameInstance(r,o,target)) return false;
    const auto component=Resolve(r,o,target.component);if(!component || ids.size()>128) return false;
    const auto car=Resolve(r,o,target.actor);uintptr_t car_class{},component_class{};
    if(!car || !r.Read(car+accessory::profile::native_layout.ObjectClass,car_class) || !r.Read(component+accessory::profile::native_layout.ObjectClass,component_class)) return false;
    const auto body_color=Offset(r,o,car_class,"ColorID",4),module_color=Offset(r,o,component_class,"VehicleColorID",4);
    const auto color_notify=Find(r,o,"/Script/HTGame.HTWheeledVehicleBase.OnRep_ColorID");
    if(body_color<0 || module_color<0 || !LocalFunction(r,color_notify,"OnRep_ColorID",0,0)) return false;
    const auto save_mass=Find(r,o,"/Script/HTGame.HTVehicleModificationComponent.SaveModuleInfoToMass");
    const auto owner_at=Offset(r,o,component_class,"OwnerVehicle_High",8);uintptr_t high_owner{};
    if(!LocalFunction(r,save_mass,"SaveModuleInfoToMass",0,0) || owner_at<0 || !r.Read(component+owner_at,high_owner) || high_owner!=car) return false;
    const auto fn=Find(r,o,"/Script/HTGame.HTVehicleModificationComponent.UpdateAllModulesWithIDs");
    if(!LocalFunction(r,fn,"UpdateAllModulesWithIDs",5,64) ||
       !r.Property(fn,"VehicleModuleIDList",0,16) || !r.Property(fn,"bForceUpdate",16,1) ||
       !r.Property(fn,"VehicleID",20,8) || !r.Property(fn,"SkinID",28,8) || !r.Property(fn,"DefaultModuleData",36,28)) return false;
    std::vector<FName> names;FName skin{};
    for(auto id:ids) {
        names.push_back(Unpack(id));
        const auto row=std::find_if(catalog.begin(),catalog.end(),[&](const Entry& e){return e.rule.id==id;});
        if(row!=catalog.end() && row->rule.type==19) skin=Unpack(id);
    }
    alignas(8) std::array<uint8_t,64> parameters{};
    // Preserve the currently displayed fish when the full native rebuild
    // includes the stock hook slot. An empty hook is valid too.
    const auto fish_fn=Find(r,o,"/Script/HTGame.HTVehicleModificationComponent.GetCurrentFishHookModule");
    uintptr_t fish{};
    if(!LocalFunction(r,fish_fn,"GetCurrentFishHookModule",1,8) || !r.Property(fish_fn,"ReturnValue",0,8) ||
       !Invoke(r,component,fish_fn,&fish)) {submit_message=true;status=lite_i18n::localizer.Text("vehicle.fish_unavailable", "Could not read hook display data. Nothing was applied.");return false;}
    if(fish) {
        uintptr_t cls{};std::array<uint8_t,20> display{};
        if(!r.IsA(fish,"HTVehicleFishHookActor") || !r.Read(fish+accessory::profile::native_layout.ObjectClass,cls)) return false;
        const auto at=Offset(r,o,cls,"CurrentFishDisplayData",20);
        if(at<0 || !r.Read(fish+at,display)) return false;
        std::memcpy(parameters.data()+44,display.data(),20);
    }
    RowArray array{reinterpret_cast<uintptr_t>(names.data()),static_cast<int32_t>(names.size()),static_cast<int32_t>(names.size())};
    std::memcpy(parameters.data(),&array,16);parameters[16]=1;
    std::memcpy(parameters.data()+20,&target.vehicle,8);std::memcpy(parameters.data()+28,&skin,8);
    uintptr_t cls{};std::vector<uint64_t> previous;
    if(!r.Read(component+accessory::profile::native_layout.ObjectClass,cls)) return false;
    const auto list=Offset(r,o,cls,"CurrentModuleIDList",16);
    if(list<0 || !ReadIds(r,component+list,previous)) return false;
    if(!PrepareArrayCopy(r)) {Log("Vehicle appearance: name array interface unavailable");return false;}
    // Use the game's FName-array assignment helper: the engine allocates and
    // owns the destination buffer. Never install a pointer into plugin memory.
    if(!CopyNamesCall(component+list,&array)) return false;
    std::vector<uint64_t> verified;
    if(!ReadIds(r,component+list,verified) || verified!=ids || !Invoke(r,component,fn,parameters.data())) {
        RowArray original{reinterpret_cast<uintptr_t>(previous.data()),static_cast<int32_t>(previous.size()),static_cast<int32_t>(previous.size())};
        CopyNamesCall(component+list,&original);Log("Vehicle appearance: local module assignment verification failed");return false;
    }
    float paint{};
    if(!r.Read(component+module_color,paint) || !std::isfinite(paint) || paint<0 || paint>1000000) return false;
    const auto color=static_cast<int32_t>(std::lround(paint));
    if(!core->write_memory || core->write_memory(core->user,car+body_color,{reinterpret_cast<const uint8_t*>(&color),4}).code!=0 ||
       !Invoke(r,car,color_notify,nullptr)) return false;
    // The Mass representation is rebuilt when the player gets off. The native
    // local synchronizer copies CurrentModuleIDList into this exact entity's
    // module fragment; it performs no inventory or purchase operation.
    if(!Invoke(r,component,save_mass,nullptr)) return false;
    if(!PaintData(r,o,target,paint,false)) return false;
    Log("Vehicle appearance: submitted modules vehicle="+r.Name(target.vehicle)+" count="+std::to_string(ids.size()));return true;
}
bool VisualSignature(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,const Target& t,uint64_t& hash) {
    const auto car=Resolve(r,o,t.actor),component=Resolve(r,o,t.component);
    uintptr_t cls{},owner{};FName id{};
    if(!car || !component || !r.Read(component+accessory::profile::native_layout.ObjectOuter,owner) || owner!=car || !r.Read(car+accessory::profile::native_layout.ObjectClass,cls)) return false;
    const auto id_at=Offset(r,o,cls,"VehicleID",8);
    if(id_at<0 || !r.Read(car+id_at,id) || Pack(id)!=Pack(t.vehicle) || !r.Read(component+accessory::profile::native_layout.ObjectClass,cls)) return false;
    hash=1469598103934665603ull;
    for(const auto& [key,size]:std::array<std::pair<const char*,int>,4>{{{"VehicleColorID",4},{"CurrentDecalTexture",8},{"CachedWheelSmokeData",80},{"CachedWheelMarkData",240}}}) {
        const auto at=Offset(r,o,cls,key,size);std::array<uint8_t,240> bytes{};
        if(at<0 || core->read_memory(core->user,component+at,{bytes.data(),static_cast<size_t>(size)}).code!=0) return false;
        for(int n=0;n<size;++n) {hash^=bytes[n];hash*=1099511628211ull;}
    }
    const auto components=Offset(r,o,cls,"ModuleComponentMap",accessory::profile::native_layout.MapSize);RowArray map{};
    if(components<0 || !r.Read(component+components,map) || map.count<0 || map.count>64 || map.capacity<map.count) return false;
    // Include installed component identities so an attachment rebuild is seen
    // even when its paint and particle settings stayed the same.
    std::vector<uint8_t> rows(static_cast<size_t>(map.count)*accessory::profile::native_layout.MapEntryStride);const auto length=rows.size();
    if(length && core->read_memory(core->user,map.data,{rows.data(),length}).code!=0) return false;
    for(size_t n=0;n<length;++n) {hash^=rows[n];hash*=1099511628211ull;}
    const auto list=Offset(r,o,cls,"CurrentModuleIDList",16);std::vector<uint64_t> ids;
    if(list<0 || !ReadIds(r,component+list,ids)) return false;
    for(auto module_id:ids) {hash^=module_id;hash*=1099511628211ull;}
    uintptr_t car_class{};int32_t color{};RowArray materials{};
    if(!r.Read(car+accessory::profile::native_layout.ObjectClass,car_class)) return false;
    const auto color_at=Offset(r,o,car_class,"ColorID",4),mids_at=Offset(r,o,car_class,"MaterialInstanceDynamics",16);
    if(color_at<0 || mids_at<0 || !r.Read(car+color_at,color) || !r.Read(car+mids_at,materials) || materials.count<0 || materials.count>128) return false;
    hash^=static_cast<uint32_t>(color);hash*=1099511628211ull;
    for(int i=0;i<materials.count;++i) {uintptr_t material{};if(!r.Read(materials.data+sizeof(uintptr_t)*i,material)) return false;hash^=material;hash*=1099511628211ull;}
    return true;
}
void Monitor(const Reader& r,const AnomalyUe5ObjectsServiceV1* o,uint64_t driver) {
    const auto now=Clock::now();
    if(last_driver && last_driver!=driver) if(auto old=saved.find(last_driver);old!=saved.end()) {
        old->second.refresh=true;old->second.observe=now+std::chrono::milliseconds(600);
    }
    last_driver=driver;
    if(!saved.empty()) {
        // Bound every maintenance pass, including paint probes and pruning.
        // Continue after the last visited ID so busy low IDs cannot starve
        // vehicles later in the map.
        auto it=saved.upper_bound(monitor_cursor);
        if(it==saved.end()) it=saved.begin();
        monitor_cursor=it->first;
        auto& s=it->second;
        if(!SameInstance(r,o,s.target) || !Resolve(r,o,s.target.component)) {saved.erase(it);return;}
        if(s.paint_valid && !PaintData(r,o,s.target,s.paint,true) && PaintData(r,o,s.target,s.paint,false))
            Log("Vehicle appearance: repaired paint primitive data vehicle="+r.Name(s.target.vehicle));
        if(now<s.observe) return;
        s.observe=now+std::chrono::seconds(1);
        uint64_t current{};const bool valid=VisualSignature(r,o,s.target,current);
        if(s.refresh || (valid && s.signature_valid && current!=s.signature && now>=s.settle)) {
            if(Submit(r,o,s.target,s.desired,s.catalog)) {
                s.refresh=false;s.settle=now+std::chrono::seconds(2);
                s.signature_valid=VisualSignature(r,o,s.target,s.signature);
                Log("Vehicle appearance: maintained vehicle="+r.Name(s.target.vehicle));
            }
        } else if(valid && !s.signature_valid) {s.signature=current;s.signature_valid=true;}
    }
}
void Publish(const Reader& r,const Target& t) {
    auto view=std::make_shared<View>();view->target=t;view->scanning=scanning;
    if(t.actor.id) {
        if(!catalog_snapshot) catalog_snapshot=std::make_shared<const std::vector<Entry>>(entries);
        view->vehicle=r.Name(t.vehicle);view->rows=catalog_snapshot;
    }
    if(!status.empty()) view->status=status;
    published.store(std::move(view));
}
}
namespace lite_vehicle {
void Load(const AnomalyHostApiV1* host) noexcept {categories={lite_i18n::localizer.Text("vehicle.category.skin", "Livery"),lite_i18n::localizer.Text("vehicle.category.attachment", "Attachments"),lite_i18n::localizer.Text("vehicle.category.smoke", "Tire smoke"),lite_i18n::localizer.Text("vehicle.category.marks", "Tire marks"),lite_i18n::localizer.Text("vehicle.category.paint", "Paint")};attachment_groups={lite_i18n::localizer.Text("vehicle.group.front", "Front lip"),lite_i18n::localizer.Text("vehicle.group.rear", "Rear lip"),lite_i18n::localizer.Text("vehicle.group.spoiler", "Spoiler"),lite_i18n::localizer.Text("vehicle.group.wheels", "Wheels"),lite_i18n::localizer.Text("vehicle.group.hook", "Hook"),lite_i18n::localizer.Text("vehicle.group.suspension", "Suspension"),lite_i18n::localizer.Text("vehicle.group.other", "Other attachments")};api=host;core=Host(host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();}
void Start() noexcept {
    registry=0;array_copy=0;retry_init={};next_tick=retry_catalog={};objects_cache.clear();offsets.clear();entries.clear();saved.clear();catalog_notice=false;
    visible=false;request.store({});published.store(std::make_shared<View>());catalog_vehicle=0;scanning=false;selected=0;attachment_group=0;last_driver=monitor_cursor=0;catalog_snapshot.reset();status.clear();
    paint_bindings.store({});paint_hook={};paint_original=0;paint_overrides=0;last_paint_report=0;next_paint_report={};
    paint_hooks=Host(api).Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID).get();
}
AnomalyStatusV1 Stop() noexcept {
    visible=false;request.store({});paint_bindings.store({});
    if(paint_hook.id && paint_hooks) {
        const auto result=paint_hooks->release(paint_hooks->user,paint_hook);
        if(result.code!=0 && result.code!=ANOMALY_STATUS_V1_NOT_FOUND) return result;
    }
    paint_hook={};paint_original=0;saved.clear();return Ok();
}
void SetVisible(bool value) noexcept {visible=value;}
void Update() noexcept {
    try {
        if((!visible.load() && !request.load() && saved.empty()) || Clock::now()<next_tick) return;
        next_tick=Clock::now()+std::chrono::milliseconds(100);
        const auto o=Host(api).Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        const auto framework=Host(api).Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
        const Reader r{Host(api).Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get()};
        if(!o || !framework || !framework->is_game_thread || !framework->is_game_thread(framework->user) || !Initialize(r,o)) return;
        Target target;
        const bool driving=Current(r,o,target);
        Monitor(r,o,driving?target.actor.id:0);
        PublishPaintBindings(r,o);
        if(Clock::now()>=next_paint_report && paint_overrides.load()!=last_paint_report) {
            next_paint_report=Clock::now()+std::chrono::seconds(5);last_paint_report=paint_overrides.load();
            Log("Vehicle appearance: native paint writes preserved="+std::to_string(last_paint_report));
        }
        if(!driving) {status=saved.empty()?lite_i18n::localizer.Text("vehicle.enter", "Enter the driver seat of a land vehicle."):lite_i18n::localizer.Text("vehicle.maintained", "Vehicle appearance is maintained. Enter the driver seat to make changes.");request.store({});Publish(r,{});return;}
        if(catalog_vehicle!=Pack(target.vehicle) && Clock::now()>=retry_catalog) {
            retry_catalog=Clock::now()+std::chrono::seconds(2);
            catalog_notice=StartCatalog(r,o,Pack(target.vehicle));
            status=catalog_notice?lite_i18n::localizer.Text("vehicle.loading", "Reading this vehicle's appearance catalog..."):lite_i18n::localizer.Text("vehicle.wait_catalog", "Waiting for the vehicle appearance catalog.");
        }
        CatalogSlice(r,o);
        if(!scanning && catalog_vehicle==Pack(target.vehicle) && catalog_notice) {catalog_notice=false;status=lite_i18n::localizer.Text("vehicle.choose", "Select an appearance and apply it to the vehicle you are driving.");}
        if(auto command=request.exchange({})) {
            if(command->actor.id!=target.actor.id || command->component.id!=target.component.id || scanning || catalog_vehicle!=Pack(target.vehicle)) {
                status=lite_i18n::localizer.Text("vehicle.target_changed", "The vehicle or catalog changed. Select again.");Publish(r,target);return;
            }
            auto found=saved.find(target.actor.id);
            if(!EnsurePaintHook(r,o)) {status=lite_i18n::localizer.Text("vehicle.paint_unavailable", "The paint maintenance interface did not match. Nothing was applied.");Publish(r,target);return;}
            PublishPaintBindings(r,o,target.actor.id);
            if(command->restore) {
                if(found==saved.end()) status=lite_i18n::localizer.Text("vehicle.no_changes", "This vehicle has no changes from the current plugin session.");
                else if(Submit(r,o,target,found->second.target.modules,found->second.catalog)) {
                    const auto car=Resolve(r,o,target.actor);uintptr_t cls{};
                    if(car && r.Read(car+accessory::profile::native_layout.ObjectClass,cls)) {
                        const auto at=Offset(r,o,cls,"ColorID",4);
                        const auto notify=Find(r,o,"/Script/HTGame.HTWheeledVehicleBase.OnRep_ColorID");
                        if(at>=0 && core->write_memory(core->user,car+at,{reinterpret_cast<const uint8_t*>(&found->second.target.color),4}).code==0)
                            Invoke(r,car,notify,nullptr);
                    }
                    saved.erase(found);status=lite_i18n::localizer.Text("vehicle.restored", "Original vehicle appearance restoration submitted.");
                }
                else status=lite_i18n::localizer.Text("vehicle.restore_failed", "Restoration did not complete. Please retry.");
            } else {
                std::vector<vehicle_rules::Row> catalog;for(const auto& row:entries) catalog.push_back(row.rule);
                const auto& base=found==saved.end()?target.modules:found->second.desired;
                const auto plan=vehicle_rules::Replace(base,catalog,Pack(target.vehicle),command->selected);
                if(!plan) status=lite_i18n::localizer.Text("vehicle.selection_invalid", "The selected resource does not match this vehicle model.");
                else if(Submit(r,o,target,*plan,entries)) {
                    if(found==saved.end()) found=saved.emplace(target.actor.id,Saved{target,*plan,entries}).first;
                    else {found->second.desired=*plan;found->second.catalog=entries;}
                    const auto component=Resolve(r,o,target.component);uintptr_t cls{};
                    if(component && r.Read(component+accessory::profile::native_layout.ObjectClass,cls)) {
                        const auto at=Offset(r,o,cls,"VehicleColorID",4);
                        found->second.paint_valid=at>=0 && r.Read(component+at,found->second.paint) && std::isfinite(found->second.paint);
                    }
                    found->second.signature_valid=VisualSignature(r,o,target,found->second.signature);
                    found->second.observe=Clock::now()+std::chrono::seconds(2);found->second.settle=found->second.observe;
                    status=lite_i18n::localizer.Text("vehicle.submitted", "Local appearance submitted. Waiting for models and effects to load.");
                } else if(!submit_message) status=lite_i18n::localizer.Text("vehicle.apply_failed", "Appearance application did not complete. Please retry.");
            }
            PublishPaintBindings(r,o);
        }
        Publish(r,target);
    } catch(...) {status=lite_i18n::localizer.Text("vehicle.read_failed", "Could not read the vehicle appearance. Enter the vehicle again and retry.");}
}
void Draw(const AnomalyUiServiceV1* ui) noexcept {
    try {
        const auto view=published.load();if(!view) return;
        static const std::vector<Entry> empty_rows;
        const auto& rows=view->rows?*view->rows:empty_rows;
        ui->text(ui->user,StringView(view->status));if(!view->vehicle.empty()) ui->text(ui->user,StringView(lite_i18n::localizer.Format("vehicle.model", "Current model: {0}", std::array<std::string_view,1>{view->vehicle})));
        for(int i=0;i<5;++i) {
            if(ui->button(ui->user,StringView(categories[i]),0,0)) {category=i;selected=0;}
            if(i<4) ui->same_line(ui->user,0,6);
        }
        if(category==1) {
            for(int i=0;i<7;++i) {
                const auto label=(attachment_group==i?lite_i18n::localizer.Format("vehicle.group_active", "[ {0} ]", std::array<std::string_view,1>{attachment_groups[i]}):attachment_groups[i])+"###vehicle-attachment-"+std::to_string(i);
                if(ui->button(ui->user,StringView(label),0,0)) {attachment_group=i;selected=0;}
                if(i<6 && i%4!=3) ui->same_line(ui->user,0,6);
            }
        }
        ui->input_text(ui->user,StringView(lite_i18n::localizer.Text("vehicle.search", "Search name or ID")),search,sizeof(search),0);
        if(ui->begin_child(ui->user,StringView("vehicle-appearance-list"),0,category==1?220.f:260.f,0)) {
            size_t count{};
            for(const auto& row:rows) {
                if(!vehicle_rules::Matches(row.rule.type,category,attachment_group) || (search[0] && row.label.find(search)==std::string::npos && row.id.find(search)==std::string::npos)) continue;
                ++count;const auto label=lite_i18n::ResourceLabel(row.label,row.id,{},selected==row.rule.id,"vehicle-");
                if(ui->button(ui->user,StringView(label),0,0)) selected=row.rule.id;
            }
            if(!count && !view->scanning) ui->text(ui->user,StringView(lite_i18n::localizer.Text("vehicle.empty", "No resources in this category are available for this vehicle model.")));
        }
        ui->end_child(ui->user);
        const bool valid=view->target.actor.id && !view->scanning && std::any_of(rows.begin(),rows.end(),[](const Entry& e){return e.rule.id==selected && vehicle_rules::Matches(e.rule.type,category,attachment_group);});
        if(ui->button_enabled(ui->user,StringView(lite_i18n::localizer.Text("vehicle.apply", "Apply selected appearance")),0,0,valid)) request.store(std::make_shared<Request>(Request{view->target.actor,view->target.component,selected,false}));
        ui->same_line(ui->user,0,8);
        if(ui->button_enabled(ui->user,StringView(lite_i18n::localizer.Text("vehicle.restore", "Restore original vehicle appearance")),0,0,view->target.actor.id && !view->scanning)) request.store(std::make_shared<Request>(Request{view->target.actor,view->target.component,0,true}));
    } catch(...) {}
}
}
