#include "../rob_bank_runtime.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
AnomalyStatusV1 Result(std::uint32_t code = ANOMALY_STATUS_V1_OK) { return {code, 0, {}}; }
std::string_view View(AnomalyStringViewV1 text) { return {text.data, text.size}; }

// SDK-level fixture of the independently observed bank entry: an active
// BPUI_SideDialogue/UI_Dialogue with BtnSkip, plus an inactive HTUI_Cinema.
// Exercise the public runtime, reflection validation and ProcessEvent dispatch.
struct BankEntry final {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(1024 * 1024);
    std::size_t used{};
    std::vector<std::string> name_pool{"None"};
    std::unordered_map<std::string, AnomalyGenerationHandleV1> paths;
    std::unordered_map<std::uintptr_t, std::string> functions;
    std::uint32_t object_count{};
    std::uintptr_t slots{}, registry{}, vtable{}, class_class{}, function_class{};
    std::uintptr_t world_instruction{}, objects_instruction{}, pickup{}, scratch{};
    std::uintptr_t controller{}, actor{}, bank_asset{}, other_asset{}, dialogue{}, button{}, parent{}, second{};
    std::uintptr_t player_state{}, key_doors_property{}, key_doors_inner{};
    std::uintptr_t dispatched_widget{};
    unsigned dispatches{};
    bool playing{true}, game_thread{true};
    inline static BankEntry* current{};
    AnomalyHostApiV1 host{sizeof(host), 1, 0, this};
    AnomalyCoreServiceV1 core{sizeof(core), 1, this};
    AnomalySignatureServiceV1 signatures{sizeof(signatures), 1, this};
    AnomalyUe5NamesServiceV1 names{sizeof(names), ANOMALY_UE5_NAMES_SERVICE_V1_VERSION, this};
    AnomalyUe5FrameworkServiceV1 framework{sizeof(framework), 1, this};
    AnomalyUe5ObjectsServiceV1 objects{sizeof(objects), 1, this};

    std::uintptr_t Allocate(std::size_t size) {
        const auto result = reinterpret_cast<std::uintptr_t>(memory.data() + used);
        used += (size + 7) & ~std::size_t{7};
        Require(used <= memory.size(), "Bank fixture arena exhausted");
        return result;
    }
    template<class T> static void Put(std::uintptr_t address, T value) { std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value)); }
    template<class T> static T Get(std::uintptr_t address) { T value; std::memcpy(&value, reinterpret_cast<void*>(address), sizeof(value)); return value; }
    std::uint32_t Name(std::string name) {
        name_pool.push_back(std::move(name));
        return static_cast<std::uint32_t>(name_pool.size() - 1);
    }
    std::uintptr_t Object(std::string name, std::uintptr_t type, std::string path = {}, std::size_t size = 0x2000) {
        auto object = Allocate(size);
        Put(object, vtable);
        Put(object + 12, object_count);
        Put(object + 16, type);
        Put(object + 24, Name(std::move(name)));
        Put(slots + object_count * 24, object);
        Put(slots + object_count * 24 + 16, std::uint32_t{1});
        if (!path.empty()) paths.emplace(std::move(path), AnomalyGenerationHandleV1{(std::uint64_t{1} << 32) | (object_count + 1), 1});
        ++object_count;
        Put(registry + 36, object_count);
        return object;
    }
    std::uintptr_t Property(std::uintptr_t owner, std::string name, std::string type, std::int32_t offset, std::int32_t size) {
        const auto field_type = Allocate(16), field = Allocate(128);
        Put(field_type, Name(std::move(type)));
        Put(field + 8, field_type);
        Put(field + 32, Name(std::move(name)));
        Put(field + 48, std::int32_t{1});
        Put(field + 52, size);
        Put(field + 68, offset);
        Put(field + 72, Get<std::uintptr_t>(owner + 112));
        Put(owner + 112, field);
        return field;
    }
    std::uintptr_t Function(std::uintptr_t owner, std::string name, std::string path, std::uint16_t size) {
        auto function = Object(name, function_class, std::move(path));
        functions.emplace(function, std::move(name));
        Put(function + 32, owner);
        Put(function + 180, static_cast<std::uint8_t>(size ? 1 : 0));
        Put(function + 182, size);
        if (size) Property(function, "ReturnValue", size == 8 ? "ObjectProperty" : "BoolProperty", 0, size);
        return function;
    }
    static void __fastcall ProcessEvent(void* object, void* function, void* parameters) {
        auto& fixture = *current;
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        const auto name = fixture.functions.at(reinterpret_cast<std::uintptr_t>(function));
        if (name == "DoSkipDialog") { ++fixture.dispatches; fixture.dispatched_widget = address; return; }
        Require(parameters != nullptr, "Queries require a return buffer");
        if (name == "GetParent") {
            const auto slot = Get<std::uintptr_t>(address + 0x30);
            *static_cast<std::uintptr_t*>(parameters) = slot ? Get<std::uintptr_t>(slot + 0x28) : 0;
            return;
        }
        bool value{};
        if (name == "IsPlayingSequence") value = fixture.playing;
        else if (name == "IsActivated") value = Get<std::uint8_t>(address + 0x3E0) != 0;
        else if (name == "IsVisible") { auto visibility = Get<std::uint8_t>(address + 0xDC); value = visibility != 1 && visibility != 2; }
        else if (name == "GetIsEnabled") value = (Get<std::uint8_t>(address + 0xD9) & 4) != 0;
        else if (name == "IsInteractionEnabled") value = (Get<std::uint8_t>(address + 0x479) & 6) == 6;
        else Require(false, "Unexpected game action in intro skip fixture");
        *static_cast<std::uint8_t*>(parameters) = value;
    }
    BankEntry() {
        current = this;
        registry = Allocate(48);
        const auto chunks = Allocate(8);
        slots = Allocate(512 * 24);
        Put(registry + 16, chunks); Put(chunks, slots);
        Put(registry + 32, std::uint32_t{512});
        Put(registry + 40, std::uint32_t{1}); Put(registry + 44, std::uint32_t{1});
        vtable = Allocate(80 * 8);
        Put(vtable + 0x4C * 8, reinterpret_cast<std::uintptr_t>(&ProcessEvent));
        class_class = Object("Class", 0);
        Put(class_class + 16, class_class);
        function_class = Object("Function", class_class);
        const auto controller_class = Object("HTPlayerController", class_class);
        const auto actor_class = Object("HTLevelSequenceActor", class_class);
        const auto dialogue_class = Object("UI_Dialogue", class_class, "/Script/HTGame.UI_Dialogue");
        const auto blueprint_class = Object("BPUI_SideDialogue_C", class_class);
        Put(blueprint_class + 64, dialogue_class);
        const auto button_class = Object("CommonButtonBase", class_class);
        const auto activatable = Object("CommonActivatableWidget", class_class);
        const auto widget_class = Object("Widget", class_class);
        const auto cinema_class = Object("HTUI_Cinema", class_class, "/Script/HTGame.HTUI_Cinema");
        Object("UI_Cinema_BP_C", cinema_class);
        Property(controller_class, "LevelSequenceActor", "ObjectProperty", 0xEB8, 8);
        Property(actor_class, "LevelSequenceAsset", "ObjectProperty", 0x400, 8);
        Property(dialogue_class, "BtnSkip", "ObjectProperty", 0xFD0, 8);
        Function(controller_class, "IsPlayingSequence", "/Script/HTGame.HTPlayerController.IsPlayingSequence", 1);
        Function(dialogue_class, "DoSkipDialog", "/Script/HTGame.UI_Dialogue.DoSkipDialog", 0);
        Function(activatable, "IsActivated", "/Script/CommonUI.CommonActivatableWidget.IsActivated", 1);
        Function(widget_class, "IsVisible", "/Script/UMG.Widget.IsVisible", 1);
        Function(widget_class, "GetIsEnabled", "/Script/UMG.Widget.GetIsEnabled", 1);
        Function(widget_class, "GetParent", "/Script/UMG.Widget.GetParent", 8);
        Function(button_class, "IsInteractionEnabled", "/Script/CommonUI.CommonButtonBase.IsInteractionEnabled", 1);
        controller = Object("HTPlayerController", controller_class);
        const auto player_state_class = Object("HTPlayerState", class_class);
        player_state = Object("HTPlayerState", player_state_class, {}, 0xA000);
        Put(controller + 720, player_state);
        key_doors_property = Property(player_state_class, "ClientRobBankKeyDoorDataArray", "ArrayProperty", 0x9A50, 16);
        const auto inner_class = Allocate(16);
        Put(inner_class, Name("NameProperty"));
        key_doors_inner = Allocate(128);
        Put(key_doors_inner + 8, inner_class);
        Put(key_doors_inner + 52, std::int32_t{8});
        Put(key_doors_property + 120, key_doors_inner);
        actor = Object("HTLevelSequenceActor", actor_class);
        bank_asset = Object("2p_xiaozhi01", class_class, "/Game/Blueprints/Level/Quest/JYF/2p_xiaozhi01.2p_xiaozhi01");
        other_asset = Object("OtherQuest", class_class);
        Put(controller + 0xEB8, actor); Put(actor + 0x400, bank_asset);
        dialogue = Object("BPUI_SideDialogue_C", blueprint_class);
        second = Object("BPUI_SideDialogue_C", blueprint_class);
        const auto cdo = Object("Default__BPUI_SideDialogue_C", blueprint_class);
        button = Object("BtnSkip", button_class);
        parent = Object("CanvasPanel", widget_class);
        const auto slot = Allocate(64);
        Put(button + 0x30, slot); Put(slot + 0x28, parent);
        for (auto object : {dialogue, second, cdo}) {
            Put(object + 0xFD0, button); Put(object + 0x3E0, std::uint8_t{1});
        }
        Put(button + 0xD9, std::uint8_t{4}); Put(button + 0x479, std::uint8_t{6});
        const auto world = Allocate(1024), instance = Allocate(128), players = Allocate(8), player = Allocate(64);
        Put(world + 560, instance); Put(instance + 56, players); Put(instance + 64, std::int32_t{1});
        Put(players, player); Put(player + 48, controller);
        const auto world_storage = Allocate(8); Put(world_storage, world);
        world_instruction = Allocate(8); objects_instruction = Allocate(8);
        Put(world_instruction + 3, static_cast<std::int32_t>(world_storage - world_instruction - 7));
        Put(objects_instruction + 3, static_cast<std::int32_t>(registry + 16 - objects_instruction - 7));
        pickup = Allocate(256); scratch = Allocate(8);
        const std::array<std::uint8_t, 16> prologue{0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x40,0x48,0x8B,0xDA,0x48,0x8B,0xF9};
        const std::array<std::uint8_t, 13> success{0xB0,0x01,0x48,0x8B,0x5C,0x24,0x58,0x48,0x83,0xC4,0x40,0x5F,0xC3};
        const std::array<std::uint8_t, 13> failure{0x48,0x8B,0x5C,0x24,0x58,0x32,0xC0,0x48,0x83,0xC4,0x40,0x5F,0xC3};
        std::memcpy(reinterpret_cast<void*>(pickup), prologue.data(), prologue.size());
        std::memcpy(reinterpret_cast<void*>(pickup + 0xD6), success.data(), success.size());
        std::memcpy(reinterpret_cast<void*>(pickup + 0xE3), failure.data(), failure.size());
        core.read_memory = [](void* user, std::uintptr_t address, AnomalyMutableByteSpanV1 out) {
            auto& f = *static_cast<BankEntry*>(user);
            const auto begin = reinterpret_cast<std::uintptr_t>(f.memory.data());
            if (address < begin || address - begin > f.used || out.size > f.used - (address - begin)) return Result(ANOMALY_STATUS_V1_NOT_FOUND);
            std::memcpy(out.data, reinterpret_cast<const void*>(address), out.size); return Result();
        };
        signatures.resolve = [](void* user, AnomalyStringViewV1, AnomalyStringViewV1, AnomalyStringViewV1 pattern, std::uintptr_t* out) {
            auto& f = *static_cast<BankEntry*>(user); const auto text = View(pattern);
            *out = text.starts_with("48 8B 1D") ? f.world_instruction : text.starts_with("48 8B 05") ? f.objects_instruction :
                text.starts_with("48 89 5C") ? f.pickup : f.scratch;
            return Result();
        };
        names.resolve_utf8 = [](void* user, std::uint32_t id, char* out, std::size_t* size) {
            auto& f = *static_cast<BankEntry*>(user);
            if (id >= f.name_pool.size()) return Result(ANOMALY_STATUS_V1_NOT_FOUND);
            const auto& name = f.name_pool[id]; const auto capacity = *size; *size = name.size() + 1;
            if (capacity < *size) return Result(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
            std::memcpy(out, name.c_str(), *size); return Result();
        };
        framework.is_game_thread = [](void* user) { return static_cast<BankEntry*>(user)->game_thread ? 1 : 0; };
        objects.find_exact = [](void* user, AnomalyStringViewV1 path, AnomalyGenerationHandleV1* out) {
            auto& f = *static_cast<BankEntry*>(user); const auto found = f.paths.find(std::string(View(path)));
            if (found == f.paths.end()) return Result(ANOMALY_STATUS_V1_NOT_FOUND);
            *out = found->second; return Result();
        };
        host.query_service = [](void* user, AnomalyStringViewV1 id, std::uint32_t, const void** out) {
            auto& f = *static_cast<BankEntry*>(user); const auto name = View(id);
            *out = name == ANOMALY_CORE_SERVICE_V1_ID ? static_cast<void*>(&f.core) :
                name == ANOMALY_SIGNATURE_SERVICE_V1_ID ? static_cast<void*>(&f.signatures) :
                name == ANOMALY_UE5_NAMES_SERVICE_V1_ID ? static_cast<void*>(&f.names) :
                name == ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID ? static_cast<void*>(&f.framework) :
                name == ANOMALY_UE5_OBJECTS_SERVICE_V1_ID ? static_cast<void*>(&f.objects) : nullptr;
            return Result(*out ? ANOMALY_STATUS_V1_OK : ANOMALY_STATUS_V1_NOT_FOUND);
        };
    }
};
}

void RunBankIntroRuntimeTests() {
    using namespace std::chrono_literals;
    BankEntry f;
    pink_paw_heist_esp::RobBankRuntime runtime;
    Require(runtime.Start(&f.host), "The public runtime must start with its SDK contracts");
    std::uint32_t requests{};
    const auto skip = [&] {
        const auto status = runtime.SkipCutscenes(requests);
        if (status.code != ANOMALY_STATUS_V1_OK) std::cerr.write(status.message.data, status.message.size);
        Require(status.code == ANOMALY_STATUS_V1_OK, "Bank intro reflection/dispatch failed");
    };
    Require(runtime.Refresh(), "The public runtime must bind the current player context");
    BankEntry::Put(f.player_state + 0x9A90 + 8, std::int32_t{-1});
    Require(runtime.RefreshPickabilityContext() == pink_paw_heist_esp::RobBankContextRefresh::unchanged,
        "Read the observed 0x9A50 array, not the invalid header at the upstream offset");
    const auto key = f.Allocate(8);
    BankEntry::Put(key, f.Name("BankDoor01"));
    BankEntry::Put(f.player_state + 0x9A90, key);
    BankEntry::Put(f.player_state + 0x9A90 + 8, std::int32_t{1});
    BankEntry::Put(f.player_state + 0x9A90 + 12, std::int32_t{1});
    BankEntry::Put(f.player_state + 0x9A50 + 8, std::int32_t{-1});
    BankEntry::Put(f.key_doors_property + 68, std::int32_t{0x9A90});
    Require(runtime.RefreshPickabilityContext() == pink_paw_heist_esp::RobBankContextRefresh::changed,
        "Follow the reflected 0x9A90 layout and observe newly unlocked doors");
    const auto inner_class = BankEntry::Get<std::uintptr_t>(f.key_doors_inner + 8);
    const auto name_type = BankEntry::Get<std::uint32_t>(inner_class);
    BankEntry::Put(inner_class, f.Name("IntProperty"));
    Require(runtime.RefreshPickabilityContext() == pink_paw_heist_esp::RobBankContextRefresh::unavailable,
        "An array of a different field type must disable key-door inspection");
    BankEntry::Put(inner_class, name_type);
    BankEntry::Put(f.key_doors_inner + 52, std::int32_t{4});
    Require(runtime.RefreshPickabilityContext() == pink_paw_heist_esp::RobBankContextRefresh::unavailable,
        "A mismatched FName element size must disable key-door inspection");
    BankEntry::Put(f.key_doors_inner + 52, std::int32_t{8});
    Require(runtime.RefreshPickabilityContext() == pink_paw_heist_esp::RobBankContextRefresh::changed,
        "Restoring the reflected field contract must restore inspection");
    BankEntry::Put(f.actor + 0x400, f.other_asset);
    skip(); Require(requests == 0 && f.dispatches == 0, "Another quest cannot authorize dialogue skipping");
    BankEntry::Put(f.actor + 0x400, f.bank_asset);
    skip(); Require(requests == 0, "Do not dispatch before the bank intro settles");
    std::this_thread::sleep_for(2100ms);
    skip(); Require(requests == 0 && f.dispatches == 0, "Ambiguous active dialogue windows must not be skipped");
    BankEntry::Put(f.second + 0x3E0, std::uint8_t{0});
    BankEntry::Put(f.parent + 0xDC, std::uint8_t{2});
    skip(); Require(requests == 0, "A hidden button ancestor must block skipping");
    BankEntry::Put(f.parent + 0xDC, std::uint8_t{4});
    BankEntry::Put(f.button + 0xD9, std::uint8_t{0});
    skip(); Require(requests == 0, "A disabled skip button must block skipping");
    BankEntry::Put(f.button + 0xD9, std::uint8_t{4});
    BankEntry::Put(f.button + 0x479, std::uint8_t{0});
    skip(); Require(requests == 0, "A non-interactive skip button must block skipping");
    BankEntry::Put(f.button + 0x479, std::uint8_t{6});
    skip(); Require(requests == 1 && f.dispatches == 1 && f.dispatched_widget == f.dialogue,
        "Dispatch DoSkipDialog on the live SideDialogue, never the Cinema or class default object");
    skip(); Require(requests == 0 && f.dispatches == 1, "A playback must not dispatch twice");
    runtime.ResetCutsceneSkipping();
    skip(); Require(requests == 0 && f.dispatches == 1, "A reset must restore the settling interval");
    f.game_thread = false;
    Require(runtime.SkipCutscenes(requests).code == ANOMALY_STATUS_V1_CONFLICT && f.dispatches == 1,
        "Never call the dialogue handler off the Game thread");
    runtime.Stop();
    std::cout << "bank intro runtime dispatch: passed\n";
}
