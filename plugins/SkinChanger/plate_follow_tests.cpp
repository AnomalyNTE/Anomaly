#include "plates.cpp"
#include <iostream>

namespace {
struct Memory { uintptr_t start; size_t size; };
struct TestHost {
    AnomalyCoreServiceV1 core{sizeof(core),1};
    AnomalyUe5ObjectsServiceV1 objects{sizeof(objects),1};
    AnomalyUe5FrameworkServiceV1 framework{sizeof(framework),1};
    AnomalyNteCombatServiceV1 combat{sizeof(combat),1};
    std::vector<Memory> memory;
    bool game_thread = true;
};
struct Event { uintptr_t mesh; uint32_t name; float value; };
std::vector<Event> restored;
void __fastcall MeshEvent(void* mesh,void*,void* parameters) {
    struct Params { FName name; float value; } params{};
    std::memcpy(&params,parameters,sizeof(params));
    restored.push_back({reinterpret_cast<uintptr_t>(mesh),params.name.id,params.value});
}
struct Ui {
    int change = -1;
};
AnomalyStatusV1 ANOMALY_CALL Query(void* user,AnomalyStringViewV1 id,uint32_t,const void** service) {
    auto& host = *static_cast<TestHost*>(user); const std::string_view name(id.data,id.size);
    if (name == ANOMALY_CORE_SERVICE_V1_ID) *service = &host.core;
    else if (name == ANOMALY_UE5_OBJECTS_SERVICE_V1_ID) *service = &host.objects;
    else if (name == ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID) *service = &host.framework;
    else if (name == ANOMALY_NTE_COMBAT_SERVICE_V1_ID) *service = &host.combat;
    else { *service = nullptr; return {ANOMALY_STATUS_V1_NOT_FOUND,0,{}}; }
    return Ok();
}
}
int main() {
    int failed{};
    const auto check = [&](bool ok,const char* label) { if (!ok) { ++failed; std::cerr << label << '\n'; } };
    TestHost host;
    host.core.user = &host;
    host.core.read_memory = [](void* user,uintptr_t address,AnomalyMutableByteSpanV1 destination) {
        for (const auto& range : static_cast<TestHost*>(user)->memory) if (address >= range.start &&
            address-range.start <= range.size && destination.size <= range.size-(address-range.start)) {
            std::memcpy(destination.data,reinterpret_cast<const void*>(address),destination.size); return Ok();
        }
        return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
    };
    host.objects.snapshot_by_handle = [](void*,AnomalyGenerationHandleV1 h,AnomalyUe5ObjectSnapshotV1* out) {
        out->handle = h; return Ok();
    };
    host.framework.user = &host;
    host.framework.is_game_thread = [](void* user) { return static_cast<TestHost*>(user)->game_thread ? 1 : 0; };
    host.combat.current_combatant = [](void*,AnomalyNteCombatantSnapshotV1*) {
        return AnomalyStatusV1{ANOMALY_STATUS_V1_NOT_FOUND,0,{}};
    };
    AnomalyHostApiV1 test_api{}; test_api.struct_size = sizeof(test_api); test_api.api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    test_api.host_context = &host; test_api.query_service = Query;
    lite_plates::Load(&test_api); lite_plates::Start();
    Ui ui_state;
    AnomalyUiServiceV1 ui{}; ui.struct_size = sizeof(ui); ui.user = &ui_state;
    ui.text = [](void*,AnomalyStringViewV1) {};
    ui.input_text = [](void*,AnomalyStringViewV1,char*,size_t,uint32_t) { return 0; };
    ui.button = [](void*,AnomalyStringViewV1,float,float) { return 0; };
    ui.checkbox = [](void* user,AnomalyStringViewV1 label,int* value) {
        auto& state = *static_cast<Ui*>(user);
        if (std::string_view(label.data,label.size) == "应用车牌" && state.change >= 0) {
            *value = state.change; state.change = -1; return 1;
        }
        return 0;
    };
    ui_state.change = 1; lite_plates::Draw(&ui);
    check(requested == 1 && !enabled,"render publishes the enable request without touching game state");
    registry = 1; // Symbols are available; this fixture exercises no vehicle discovery.
    host.game_thread = false; lite_plates::Update();
    check(requested == 1 && !enabled,"game work waits for the Game thread");
    host.game_thread = true; lite_plates::Update();
    check(enabled && published.load()->enabled,"toggle stays enabled before entering any vehicle");
    next_probe = {}; lite_plates::Update();
    check(enabled,"temporarily missing vehicle does not disable following");

    std::array<uint8_t,80> registry_bytes{};
    std::array<uintptr_t,1> chunks{};
    std::array<uint8_t,24*14> items{};
    std::array<std::array<uint8_t,96>,14> object_bytes{};
    std::array<uintptr_t,77> vtable{}; vtable[76] = reinterpret_cast<uintptr_t>(&MeshEvent);
    const auto add = [&](const auto& data) { host.memory.push_back({reinterpret_cast<uintptr_t>(&data),sizeof(data)}); };
    add(registry_bytes); add(chunks); add(items); add(object_bytes); add(vtable);
    const auto write = [](uintptr_t address,const auto& value) { std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(value)); };
    registry = reinterpret_cast<uintptr_t>(registry_bytes.data());
    chunks[0] = reinterpret_cast<uintptr_t>(items.data());
    write(registry+16,reinterpret_cast<uintptr_t>(chunks.data())); write(registry+36,uint32_t{14});
    for (int32_t i = 1; i < 14; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(object_bytes[i].data());
        write(address,reinterpret_cast<uintptr_t>(vtable.data())); write(address+12,i);
        write(chunks[0]+24*i,address); write(chunks[0]+24*i+16,uint32_t{9});
    }
    const auto handle = [](uint32_t index) { return AnomalyGenerationHandleV1{(uint64_t{9}<<32)|(index+1u),0}; };
    Vehicle first; first.actor = handle(1); Mesh a; a.handle = handle(3); a.values.push_back({{101,0},26,0}); first.meshes.push_back(a);
    Vehicle second; second.actor = handle(2); Mesh b; b.handle = handle(4); b.values.push_back({{202,0},15,0}); second.meshes.push_back(b);
    vehicles = {first,second}; scalar_function = 1;
    Publish(); ui_state.change = 0; lite_plates::Draw(&ui); lite_plates::Update();
    check(!enabled && vehicles.empty(),"disabling processes all remembered vehicles");
    check(restored.size() == 2 && restored[0].name == 202 && restored[0].value == 15 &&
        restored[1].name == 101 && restored[1].value == 26,"each previous vehicle gets its own original material values");
    check(!published.load()->enabled,"restoration publishes the disabled toggle state");
    const auto address = [&](size_t index) { return reinterpret_cast<uintptr_t>(object_bytes[index].data()); };
    cache.push_back({"/Script/Engine.Actor",handle(6)});
    cache.push_back({"/Script/Engine.SceneComponent",handle(7)});
    cache.push_back({"/Script/Engine.MeshComponent",handle(5)});
    cache.push_back({"/Script/Engine.ChildActorComponent",handle(8)});
    fields.push_back({address(6),"RootComponent",8,24});
    fields.push_back({address(7),"AttachChildren",16,40});
    fields.push_back({address(8),"ChildActor",8,56});
    write(address(1)+24,address(3));
    write(address(3)+16,address(5)); write(address(3)+32,address(1));
    write(address(4)+16,address(5)); write(address(4)+32,address(1));
    write(address(10)+16,address(5)); write(address(10)+32,address(9));
    write(address(11)+16,address(8)); write(address(11)+32,address(1));
    write(address(11)+56,address(12)); write(address(12)+24,address(13));
    write(address(13)+16,address(5)); write(address(13)+32,address(12));
    std::array<uintptr_t,3> children{address(4),address(10),address(11)}; add(children);
    write(address(3)+40,RowArray{reinterpret_cast<uintptr_t>(children.data()),3,3});
    const auto candidates = PlateMeshCandidates(Reader{nullptr},&host.objects,address(1));
    check(candidates.size() == 3 && candidates[0] == address(3) && candidates[1] == address(4) && candidates[2] == address(13),
        "discover main meshes and child-actor parts without a fixed car-module list");
    check(std::find(candidates.begin(),candidates.end(),address(10)) == candidates.end(),
        "attached rider meshes are excluded from vehicle plate discovery");
    lite_plates::Stop();
    std::cout << "Plate following toggle and multi-vehicle restoration: " << (failed ? "FAILED" : "passed") << '\n';
    return failed ? 1 : 0;
}
