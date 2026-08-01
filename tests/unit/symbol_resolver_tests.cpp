#include "anomaly/symbol_resolver.hpp"

#include "pattern.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

class FixtureMemory final : public anomaly::SymbolMemory {
public:
    static constexpr std::uintptr_t kBase = 0x10000000;

    FixtureMemory() : bytes_(0x4000, 0x90) {
        module_.name = L"fixture.exe";
        module_.path = L"C:\\fixture.exe";
        module_.base = kBase;
        module_.size = bytes_.size();
        sections_.push_back({
            ".text", module_.base, 0x800,
            IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ});
        sections_.push_back({
            ".data", module_.base + 0x800, bytes_.size() - 0x800,
            IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE});
    }

    void Write(std::size_t offset, const void* data, std::size_t size) {
        std::memcpy(bytes_.data() + offset, data, size);
    }

    std::optional<ue5mem::ModuleInfo> FindModule(std::wstring_view name) const override {
        return name == module_.name ? std::optional(module_) : std::nullopt;
    }

    std::vector<ue5mem::SectionInfo> Sections(
        const ue5mem::ModuleInfo&) const override {
        return sections_;
    }

    std::vector<std::uintptr_t> Scan(
        const ue5mem::ModuleInfo&,
        std::string_view section,
        std::string_view pattern,
        std::size_t limit) const override {
        ++scan_count;
        const auto found = std::ranges::find_if(sections_, [&](const auto& item) {
            return item.name == section;
        });
        if (found == sections_.end()) return {};
        const auto parsed = ue5mem::Pattern::Parse(pattern);
        const auto offset = static_cast<std::size_t>(found->base - module_.base);
        const auto matches = parsed.FindAll(std::span(bytes_).subspan(offset, found->virtual_size), limit);
        std::vector<std::uintptr_t> result;
        for (const auto match : matches) result.push_back(found->base + match);
        return result;
    }

    bool Read(std::uintptr_t address, void* destination, std::size_t size) const override {
        if (address < module_.base || size > bytes_.size() ||
            address - module_.base > bytes_.size() - size) return false;
        std::memcpy(destination, bytes_.data() + (address - module_.base), size);
        return true;
    }

    std::optional<anomaly::SymbolMemoryRegion> Query(
        std::uintptr_t address) const override {
        const auto section = std::ranges::find_if(sections_, [&](const auto& item) {
            return address >= item.base && address - item.base < item.virtual_size;
        });
        if (section == sections_.end()) return std::nullopt;
        const DWORD protection = section->name == ".text" ? PAGE_EXECUTE_READ : PAGE_READWRITE;
        return anomaly::SymbolMemoryRegion{
            section->base, section->virtual_size, MEM_COMMIT, protection, MEM_IMAGE};
    }

    mutable std::size_t scan_count{};

private:
    ue5mem::ModuleInfo module_;
    std::vector<ue5mem::SectionInfo> sections_;
    std::vector<std::uint8_t> bytes_;
};

anomaly::BuildFingerprint Fingerprint() {
    anomaly::BuildFingerprint value;
    value.game = "nte";
    value.id = "nte-win64-test";
    value.module = L"fixture.exe";
    value.machine = IMAGE_FILE_MACHINE_AMD64;
    value.timestamp = 1;
    value.image_size = 0x1000;
    value.text_virtual_size = 0x800;
    value.text_sha256 = std::string(64, 'a');
    return value;
}

anomaly::BuildProfile Profile() {
    anomaly::BuildProfile profile;
    profile.game = "nte";
    profile.source_hash = std::string(64, 'b');
    anomaly::ProfileSymbol world;
    world.id = "ue5.GWorld";
    world.module = L"fixture.exe";
    world.section = ".text";
    world.pattern = "48 8B 1D ?? ?? ?? ??";
    world.resolve = {anomaly::ProfileResolveKind::RipRelative32, 3, 7, 0};
    world.validators = {"readable", "world-shape-v1"};
    world.required_by = {"anomaly.ue5.world"};
    profile.symbols.emplace(world.id, world);

    anomaly::ProfileSymbol tick;
    tick.id = "ue5.GameTick";
    tick.module = L"fixture.exe";
    tick.section = ".text";
    tick.pattern = "40 53 48 83 EC 20";
    tick.resolve = {anomaly::ProfileResolveKind::Direct, 0, 0, 0};
    tick.validators = {"address-in-module", "tick-anchor-v1"};
    tick.required_by = {"anomaly.ue5.framework"};
    profile.symbols.emplace(tick.id, tick);
    profile.features.emplace("ue5.world", std::vector<std::string>{"ue5.GWorld"});
    profile.features.emplace("ue5.framework", std::vector<std::string>{"ue5.GameTick"});
    return profile;
}

constexpr std::size_t kSemanticWorldStorage = 0x900;
constexpr std::size_t kSemanticWorld = 0x1000;
constexpr std::size_t kSemanticGameInstance = 0x1100;
constexpr std::size_t kSemanticLocalPlayers = 0x1200;
constexpr std::size_t kSemanticLocalPlayer = 0x1300;
constexpr std::size_t kSemanticController = 0x1400;
constexpr std::size_t kSemanticPawn = 0x1500;
constexpr std::size_t kSemanticCameraManager = 0x1600;
constexpr std::size_t kSemanticPlayerRoot = 0x1700;
constexpr std::size_t kSemanticLevel = 0x1800;
constexpr std::size_t kSemanticActors = 0x1900;
constexpr std::size_t kSemanticActor = 0x1A00;
constexpr std::size_t kSemanticActorRoot = 0x1B00;
constexpr std::size_t kSemanticActorClass = 0x1C00;
constexpr std::size_t kSemanticObjectRegistry = 0x890;
constexpr std::size_t kSemanticObjectTable = 0x1D00;
constexpr std::size_t kSemanticObjectChunk = 0x1E00;
constexpr std::size_t kSemanticCameraFov = kSemanticCameraManager + 0x50;

anomaly::BuildProfile SemanticProfile() {
    auto profile = Profile();
    profile.source_hash = std::string(64, 'e');
    anomaly::ProfileSymbol names;
    names.id = "ue5.FNamePool";
    names.module = L"fixture.exe";
    names.section = ".text";
    names.pattern = "48 8D 05 ?? ?? ?? ?? 90 90";
    names.resolve = {anomaly::ProfileResolveKind::RipRelative32, 3, 7, 0};
    names.validators = {"readable"};
    names.required_by = {"anomaly.ue5.names", "anomaly.ue5.actors", "anomaly.ue5.functions"};
    profile.symbols.emplace(names.id, names);

    anomaly::ProfileSymbol objects;
    objects.id = "ue5.GObjects";
    objects.module = L"fixture.exe";
    objects.section = ".text";
    objects.pattern = "48 8B 05 ?? ?? ?? ?? 90 90";
    objects.resolve = {anomaly::ProfileResolveKind::RipRelative32, 3, 7, 0};
    objects.validators = {"readable"};
    objects.required_by = {"anomaly.ue5.objects", "anomaly.ue5.functions"};
    profile.symbols.emplace(objects.id, objects);
    profile.features.emplace(
        "nte.player", std::vector<std::string>{"ue5.GWorld", "ue5.GameTick"});
    profile.features.emplace(
        "nte.player-esp", std::vector<std::string>{"ue5.GWorld", "ue5.GameTick"});
    profile.features.emplace("ue5.names", std::vector<std::string>{"ue5.FNamePool"});
    profile.features.emplace(
        "ue5.objects", std::vector<std::string>{"ue5.GObjects", "ue5.GameTick"});
    profile.features.emplace(
        "ue5.actors",
        std::vector<std::string>{"ue5.GWorld", "ue5.GameTick", "ue5.FNamePool"});
    profile.features.emplace(
        "ue5.functions",
        std::vector<std::string>{"ue5.GObjects", "ue5.GameTick", "ue5.FNamePool"});
    profile.features.emplace(
        "nte.player-teleport",
        std::vector<std::string>{"ue5.GWorld", "ue5.GameTick"});
    profile.features.emplace(
        "nte.entities", std::vector<std::string>{"ue5.GWorld", "ue5.GameTick"});
    profile.feature_layout_validators.emplace(
        "nte.player", std::vector<std::string>{"nte-player-layout-v1"});
    profile.feature_layout_validators.emplace(
        "nte.player-esp", std::vector<std::string>{"nte-player-esp-layout-v1"});
    profile.feature_layout_validators.emplace(
        "nte.player-teleport",
        std::vector<std::string>{"nte-player-teleport-layout-v1"});
    profile.feature_layout_validators.emplace(
        "nte.entities", std::vector<std::string>{"nte-entities-layout-v1"});
    profile.feature_layout_validators.emplace(
        "ue5.actors", std::vector<std::string>{"ue5-actors-reflection-v1"});
    profile.feature_layout_validators.emplace(
        "ue5.functions", std::vector<std::string>{"ue5-functions-reflection-v1"});
    profile.feature_dependencies.emplace(
        "ue5.actors", std::vector<std::string>{"ue5.world", "ue5.names"});
    profile.feature_dependencies.emplace(
        "ue5.functions", std::vector<std::string>{"ue5.objects", "ue5.names"});
    profile.feature_dependencies.emplace(
        "nte.player-esp", std::vector<std::string>{"nte.player"});
    profile.feature_dependencies.emplace(
        "nte.player-teleport",
        std::vector<std::string>{"nte.player", "ue5.names", "ue5.objects"});
    profile.optional_features = {
        "ue5.actors", "ue5.functions", "nte.player-esp", "nte.player-teleport"};
    profile.layout = {
        {"world.gameInstance", 0x10},
        {"world.persistentLevel", 0x18},
        {"gameInstance.localPlayers", 0x20},
        {"localPlayer.controller", 0x10},
        {"controller.pawn", 0x18},
        {"controller.cameraManager", 0x20},
        {"actor.rootComponent", 0x40},
        {"sceneComponent.location", 0x20},
        {"sceneComponent.boundsOrigin", 0x40},
        {"sceneComponent.boundsExtent", 0x58},
        {"sceneComponent.mobility", 0x70},
        {"cameraManager.location", 0x20},
        {"cameraManager.rotation", 0x38},
        {"cameraManager.fov", 0x50},
        {"level.actors", 0x20},
        {"object.internalIndex", 0x08},
        {"object.class", 0x10},
        {"object.nameOffset", 0x18},
        {"object.outer", 0x20},
        {"ustruct.propertyLink", 0x30},
        {"ufunction.numParms", 0x40},
        {"ufunction.parmsSize", 0x42},
        {"ufunction.returnValueOffset", 0x44},
        {"ffield.name", 0x08},
        {"fproperty.arrayDim", 0x10},
        {"fproperty.elementSize", 0x14},
        {"fproperty.offsetInternal", 0x18},
        {"fproperty.propertyLinkNext", 0x20},
        {"fstructProperty.struct", 0x28},
        {"fboolProperty.fieldSize", 0x28},
        {"fboolProperty.byteOffset", 0x29},
        {"fboolProperty.byteMask", 0x2A},
        {"fboolProperty.fieldMask", 0x2B},
        {"names.blocksOffset", 0x10},
        {"names.blockBits", 16},
        {"names.entryStride", 2},
        {"names.headerLengthShift", 6},
        {"objects.itemsOffset", 0x10},
        {"objects.maxCountOffset", 0x20},
        {"objects.countOffset", 0x24},
        {"objects.maxChunksOffset", 0x28},
        {"objects.numChunksOffset", 0x2C},
        {"objects.chunkCountSize", 4},
        {"objects.chunkSize", 2},
        {"objects.itemStride", 0x18},
        {"objects.objectOffset", 0},
        {"objects.serialOffset", 0x10},
    };
    return profile;
}

template <typename Value>
void WriteValue(FixtureMemory& memory, std::size_t offset, const Value& value) {
    memory.Write(offset, &value, sizeof(value));
}

void PopulateSemanticLayout(FixtureMemory& memory) {
    const auto address = [](std::size_t offset) { return FixtureMemory::kBase + offset; };
    const auto world = address(kSemanticWorld);
    const auto game_instance = address(kSemanticGameInstance);
    const auto local_players = address(kSemanticLocalPlayers);
    const auto local_player = address(kSemanticLocalPlayer);
    const auto controller = address(kSemanticController);
    const auto pawn = address(kSemanticPawn);
    const auto camera_manager = address(kSemanticCameraManager);
    const auto player_root = address(kSemanticPlayerRoot);
    const auto level = address(kSemanticLevel);
    const auto actors = address(kSemanticActors);
    const auto actor = address(kSemanticActor);
    const auto actor_root = address(kSemanticActorRoot);
    const auto actor_class = address(kSemanticActorClass);
    const auto object_table = address(kSemanticObjectTable);
    const auto object_chunk = address(kSemanticObjectChunk);
    const std::int32_t one = 1;
    const std::int32_t actor_index = 42;
    const std::int32_t class_index = 7;
    const std::uint32_t actor_name = 101;
    const std::uint32_t class_name = 202;
    const std::uint32_t object_serial = 7;
    const std::uint8_t mobility = 2;
    const std::array<double, 3> player_position{100.0, 200.0, 300.0};
    const std::array<double, 3> bounds_origin{110.0, 210.0, 310.0};
    const std::array<double, 3> bounds_extent{50.0, 60.0, 70.0};
    const std::array<double, 3> camera_position{10.0, 20.0, 30.0};
    const std::array<double, 3> camera_rotation{0.0, 90.0, 0.0};
    const float camera_fov = 90.0F;

    WriteValue(memory, kSemanticWorldStorage, world);
    WriteValue(memory, kSemanticWorld + 0x10, game_instance);
    WriteValue(memory, kSemanticWorld + 0x18, level);
    WriteValue(memory, kSemanticGameInstance + 0x20, local_players);
    WriteValue(memory, kSemanticGameInstance + 0x28, one);
    WriteValue(memory, kSemanticLocalPlayers, local_player);
    WriteValue(memory, kSemanticLocalPlayer + 0x10, controller);
    WriteValue(memory, kSemanticController + 0x18, pawn);
    WriteValue(memory, kSemanticController + 0x20, camera_manager);
    WriteValue(memory, kSemanticPawn + 0x40, player_root);
    WriteValue(memory, kSemanticPlayerRoot + 0x20, player_position);
    WriteValue(memory, kSemanticPlayerRoot + 0x40, bounds_origin);
    WriteValue(memory, kSemanticPlayerRoot + 0x58, bounds_extent);
    WriteValue(memory, kSemanticPlayerRoot + 0x70, mobility);
    WriteValue(memory, kSemanticCameraManager + 0x20, camera_position);
    WriteValue(memory, kSemanticCameraManager + 0x38, camera_rotation);
    WriteValue(memory, kSemanticCameraFov, camera_fov);
    WriteValue(memory, kSemanticLevel + 0x20, actors);
    WriteValue(memory, kSemanticLevel + 0x28, one);
    WriteValue(memory, kSemanticLevel + 0x2C, one);
    WriteValue(memory, kSemanticActors, actor);
    WriteValue(memory, kSemanticActor + 0x08, actor_index);
    WriteValue(memory, kSemanticActor + 0x10, actor_class);
    WriteValue(memory, kSemanticActor + 0x18, actor_name);
    WriteValue(memory, kSemanticActor + 0x20, level);
    WriteValue(memory, kSemanticActor + 0x40, actor_root);
    WriteValue(memory, kSemanticActorRoot + 0x40, bounds_origin);
    WriteValue(memory, kSemanticActorRoot + 0x58, bounds_extent);
    WriteValue(memory, kSemanticActorRoot + 0x70, mobility);
    WriteValue(memory, kSemanticActorClass + 0x08, class_index);
    WriteValue(memory, kSemanticActorClass + 0x18, class_name);

    // ue5.GObjects resolves to the registry header itself in this fixture.
    WriteValue(memory, kSemanticObjectRegistry + 0x10, object_table);
    const std::uint32_t object_max_count = 2;
    WriteValue(memory, kSemanticObjectRegistry + 0x20, object_max_count);
    WriteValue(memory, kSemanticObjectRegistry + 0x24, static_cast<std::uint32_t>(one));
    WriteValue(memory, kSemanticObjectRegistry + 0x28, static_cast<std::uint32_t>(one));
    WriteValue(memory, kSemanticObjectRegistry + 0x2C, static_cast<std::uint32_t>(one));
    WriteValue(memory, kSemanticObjectTable, object_chunk);
    WriteValue(memory, kSemanticObjectChunk, actor);
    WriteValue(memory, kSemanticObjectChunk + 0x10, object_serial);
}

std::filesystem::path TemporaryDirectory() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"anomaly-symbol-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(path);
    return path;
}

}  // namespace

int main() {
    auto memory = std::make_shared<FixtureMemory>();
    constexpr std::size_t world_instruction = 0x100;
    constexpr std::size_t world_storage = 0x900;
    const std::array<std::uint8_t, 7> instruction{0x48, 0x8B, 0x1D, 0, 0, 0, 0};
    memory->Write(world_instruction, instruction.data(), instruction.size());
    const auto displacement = static_cast<std::int32_t>(
        world_storage - (world_instruction + instruction.size()));
    memory->Write(world_instruction + 3, &displacement, sizeof(displacement));
    const std::uintptr_t null_world{};
    memory->Write(world_storage, &null_world, sizeof(null_world));
    const std::array<std::uint8_t, 6> tick{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
    memory->Write(0x200, tick.data(), tick.size());
    constexpr std::size_t name_pool_instruction = 0x300;
    constexpr std::size_t name_pool_storage = 0x880;
    const std::array<std::uint8_t, 9> name_pool_anchor{
        0x48, 0x8D, 0x05, 0, 0, 0, 0, 0x90, 0x90};
    const auto name_pool_displacement = static_cast<std::int32_t>(
        name_pool_storage - (name_pool_instruction + 7));
    memory->Write(name_pool_instruction, name_pool_anchor.data(), name_pool_anchor.size());
    memory->Write(
        name_pool_instruction + 3, &name_pool_displacement, sizeof(name_pool_displacement));
    const std::uintptr_t name_pool{};
    memory->Write(name_pool_storage, &name_pool, sizeof(name_pool));

    constexpr std::size_t objects_instruction = 0x380;
    constexpr std::size_t objects_storage = kSemanticObjectRegistry;
    const std::array<std::uint8_t, 9> objects_anchor{
        0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x90, 0x90};
    const auto objects_displacement = static_cast<std::int32_t>(
        objects_storage - (objects_instruction + 7));
    memory->Write(objects_instruction, objects_anchor.data(), objects_anchor.size());
    memory->Write(objects_instruction + 3, &objects_displacement, sizeof(objects_displacement));
    const std::uintptr_t objects{};
    memory->Write(objects_storage, &objects, sizeof(objects));

    const auto root = TemporaryDirectory();
    const anomaly::SymbolCache cache(root / L"profile-symbol-cache.json");
    const auto fingerprint = Fingerprint();
    const auto profile = Profile();
    const anomaly::SymbolResolver resolver(memory);
    const auto first = resolver.Resolve(fingerprint, &profile, &cache);
    bool result = Check(first.state == anomaly::ProfileResolutionState::Ready,
                        "known fixture did not reach Ready") &&
        Check(first.FindSymbol("ue5.GWorld") &&
                  first.FindSymbol("ue5.GWorld")->state == anomaly::SymbolResolutionState::Resolved &&
                  first.FindSymbol("ue5.GWorld")->address == 0x10000900 &&
                  first.FeatureAvailable("ue5.world") &&
                  first.FeatureAvailable("ue5.framework") && first.cache_written,
              "resolved symbol or feature matrix changed");
    const auto scan_count = memory->scan_count;
    const std::uint8_t corrupted_tick = 0xCC;
    memory->Write(0x200, &corrupted_tick, sizeof(corrupted_tick));
    const auto cache_started = std::chrono::steady_clock::now();
    const auto second = resolver.Resolve(fingerprint, &profile, &cache);
    memory->Write(0x200, tick.data(), tick.size());
    const double cache_milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cache_started).count();
    result = Check(second.cache_loaded && memory->scan_count == scan_count &&
                       second.FindSymbol("ue5.GWorld") &&
                       second.FindSymbol("ue5.GWorld")->state ==
                            anomaly::SymbolResolutionState::CacheTrusted,
                    "trusted RVA cache was scanned or revalidated") && result;
    result = Check(cache_milliseconds < 100.0,
                   "trusted RVA cache exceeded the 100 ms startup budget") && result;

    auto object_profile = profile;
    object_profile.layout = {
        {"objects.itemsOffset", 0x10},
        {"objects.maxCountOffset", 0x20},
        {"objects.countOffset", 0x24},
        {"objects.maxChunksOffset", 0x28},
        {"objects.numChunksOffset", 0x2C},
        {"objects.chunkSize", 2},
        {"objects.itemStride", 0x18},
        {"objects.objectOffset", 0},
        {"objects.serialOffset", 0x10},
    };
    anomaly::ProfileSymbol object_symbol;
    object_symbol.id = "ue5.GObjects";
    object_symbol.module = L"fixture.exe";
    object_symbol.section = ".text";
    object_symbol.pattern = "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3";
    object_symbol.resolve = {anomaly::ProfileResolveKind::RipRelative32, 3, 7, 0};
    object_symbol.validators = {"readable", "object-registry-v1"};
    object_symbol.required_by = {"anomaly.ue5.objects"};
    constexpr std::size_t object_instruction = 0x400;
    constexpr std::size_t object_registry = 0xA00;
    constexpr std::size_t object_table = 0xB00;
    constexpr std::size_t object_chunk0 = 0xC00;
    constexpr std::size_t object_chunk1 = 0xD00;
    const std::uintptr_t table_address = FixtureMemory::kBase + object_table;
    const std::uintptr_t chunk0_address = FixtureMemory::kBase + object_chunk0;
    const std::uintptr_t chunk1_address = FixtureMemory::kBase + object_chunk1;
    const std::uint32_t object_count = 3;
    const std::uint32_t object_max_count = 4;
    const std::uint32_t object_chunks = 2;
    const std::uintptr_t null_object{};
    const std::uint32_t first_serial = 10;
    const std::uint32_t last_serial = 30;
    const std::array<std::uint8_t, 16> object_anchor{
        0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48,
        0x8B, 0x0C, 0xC8, 0x48, 0x8B, 0x04, 0xD1, 0xC3};
    const auto object_displacement = static_cast<std::int32_t>(
        object_registry - (object_instruction + 7));
    memory->Write(object_instruction, object_anchor.data(), object_anchor.size());
    memory->Write(object_instruction + 3, &object_displacement, sizeof(object_displacement));
    memory->Write(object_registry + 0x10, &table_address, sizeof(table_address));
    memory->Write(object_registry + 0x20, &object_max_count, sizeof(object_max_count));
    memory->Write(object_registry + 0x24, &object_count, sizeof(object_count));
    memory->Write(object_registry + 0x28, &object_chunks, sizeof(object_chunks));
    memory->Write(object_registry + 0x2C, &object_chunks, sizeof(object_chunks));
    memory->Write(object_table, &chunk0_address, sizeof(chunk0_address));
    memory->Write(object_table + sizeof(std::uintptr_t), &chunk1_address, sizeof(chunk1_address));
    memory->Write(object_chunk0, &null_object, sizeof(null_object));
    memory->Write(object_chunk0 + 0x10, &first_serial, sizeof(first_serial));
    memory->Write(object_chunk1, &null_object, sizeof(null_object));
    memory->Write(object_chunk1 + 0x10, &last_serial, sizeof(last_serial));
    const auto module = memory->FindModule(L"fixture.exe");
    const anomaly::SymbolValidatorRegistry validators;
    const auto valid_objects = validators.Validate(
        "object-registry-v1", object_profile, object_symbol, *module,
        FixtureMemory::kBase + object_registry, *memory);
    result = Check(module.has_value() && valid_objects.valid,
                   "valid chunked object registry failed validation") && result;
    auto packed_chunk_profile = object_profile;
    packed_chunk_profile.layout["objects.chunkCountSize"] = 2;
    packed_chunk_profile.layout["objects.numChunksOffset"] = 0x2E;
    const std::uint16_t packed_chunks = 2;
    memory->Write(object_registry + 0x28, &packed_chunks, sizeof(packed_chunks));
    memory->Write(object_registry + 0x2E, &packed_chunks, sizeof(packed_chunks));
    const auto valid_packed_objects = validators.Validate(
        "object-registry-v1", packed_chunk_profile, object_symbol, *module,
        FixtureMemory::kBase + object_registry, *memory);
    result = Check(valid_packed_objects.valid,
                   "valid packed chunk-count object registry failed validation") && result;
    const std::uint32_t object_chunks_32 = object_chunks;
    memory->Write(object_registry + 0x28, &object_chunks_32, sizeof(object_chunks_32));
    memory->Write(object_registry + 0x2C, &object_chunks_32, sizeof(object_chunks_32));
    object_profile.symbols.emplace(object_symbol.id, object_symbol);
    object_profile.features.emplace(
        "ue5.objects", std::vector<std::string>{object_symbol.id});
    const auto resolved_objects = resolver.Resolve(fingerprint, &object_profile, nullptr);
    result = Check(resolved_objects.FeatureAvailable("ue5.objects") &&
                       resolved_objects.FindSymbol("ue5.GObjects") != nullptr &&
                       resolved_objects.FindSymbol("ue5.GObjects")->state ==
                           anomaly::SymbolResolutionState::Resolved,
                   "valid object registry did not publish its resolver feature") && result;

    const std::uint32_t excessive_count = 5;
    memory->Write(object_registry + 0x24, &excessive_count, sizeof(excessive_count));
    const auto excessive_objects = validators.Validate(
        "object-registry-v1", object_profile, object_symbol, *module,
        FixtureMemory::kBase + object_registry, *memory);
    result = Check(!excessive_objects.valid &&
                       excessive_objects.message.find("count exceeds") != std::string::npos,
                   "object count overflow did not report its validator reason") && result;
    const auto rejected_objects = resolver.Resolve(fingerprint, &object_profile, nullptr);
    const auto* rejected_object = rejected_objects.FindSymbol("ue5.GObjects");
    result = Check(rejected_object != nullptr &&
                       rejected_object->state ==
                           anomaly::SymbolResolutionState::ValidationFailed &&
                       !rejected_objects.FeatureAvailable("ue5.objects") &&
                       std::ranges::any_of(
                           rejected_object->diagnostics, [](const std::string& diagnostic) {
                               return diagnostic.find("count exceeds") != std::string::npos;
                           }),
                   "object validator failure did not reach resolver diagnostics") && result;
    memory->Write(object_registry + 0x24, &object_count, sizeof(object_count));
    auto deferred_objects = rejected_objects;
    const auto scans_before_deferred_revalidation = memory->scan_count;
    const bool deferred_revalidated = resolver.RevalidateDeferredCandidates(
        object_profile, deferred_objects);
    const auto* deferred_object = deferred_objects.FindSymbol("ue5.GObjects");
    result = Check(
                 deferred_revalidated && deferred_object != nullptr &&
                     deferred_object->state == anomaly::SymbolResolutionState::Resolved &&
                     deferred_object->address == FixtureMemory::kBase + object_registry &&
                     deferred_objects.FeatureAvailable("ue5.objects") &&
                     deferred_objects.state == anomaly::ProfileResolutionState::Ready &&
                     memory->scan_count == scans_before_deferred_revalidation,
                 "deferred object validator recovery rescanned or left the feature degraded") &&
        result;

    const std::uint32_t excessive_chunks = 3;
    memory->Write(object_registry + 0x2C, &excessive_chunks, sizeof(excessive_chunks));
    const auto excessive_page_table = validators.Validate(
        "object-registry-v1", object_profile, object_symbol, *module,
        FixtureMemory::kBase + object_registry, *memory);
    result = Check(!excessive_page_table.valid &&
                       excessive_page_table.message.find("numChunks") != std::string::npos,
                   "object chunk overflow did not report its validator reason") && result;
    memory->Write(object_registry + 0x2C, &object_chunks, sizeof(object_chunks));

    const std::uintptr_t unreadable_chunk = FixtureMemory::kBase + 0x5000;
    memory->Write(
        object_table + sizeof(std::uintptr_t), &unreadable_chunk, sizeof(unreadable_chunk));
    const auto unreadable_last = validators.Validate(
        "object-registry-v1", object_profile, object_symbol, *module,
        FixtureMemory::kBase + object_registry, *memory);
    result = Check(!unreadable_last.valid &&
                       unreadable_last.message.find("last object item is unreadable") !=
                           std::string::npos,
                   "unreadable object page did not report the failing boundary") && result;
    memory->Write(
        object_table + sizeof(std::uintptr_t), &chunk1_address, sizeof(chunk1_address));

    auto incomplete_object_profile = object_profile;
    incomplete_object_profile.layout.erase("objects.serialOffset");
    const auto incomplete_layout = validators.Validate(
        "object-registry-v1", incomplete_object_profile, object_symbol, *module,
        FixtureMemory::kBase + object_registry, *memory);
    result = Check(!incomplete_layout.valid &&
                       incomplete_layout.message.find("objects.serialOffset") != std::string::npos,
                   "incomplete object layout did not identify the missing field") && result;
    auto nonstandard_chunk_profile = object_profile;
    nonstandard_chunk_profile.layout["objects.chunkSize"] = 3;
    const auto nonstandard_chunk = validators.Validate(
        "object-registry-v1", nonstandard_chunk_profile, object_symbol, *module,
        FixtureMemory::kBase + object_registry, *memory);
    result = Check(!nonstandard_chunk.valid &&
                       nonstandard_chunk.message.find("power of two") != std::string::npos,
                   "nonstandard object chunk size passed strict validation") && result;
    auto invalid_chunk_count_size = object_profile;
    invalid_chunk_count_size.layout["objects.chunkCountSize"] = 3;
    const auto invalid_chunk_count = validators.Validate(
        "object-registry-v1", invalid_chunk_count_size, object_symbol, *module,
        FixtureMemory::kBase + object_registry, *memory);
    result = Check(!invalid_chunk_count.valid &&
                       invalid_chunk_count.message.find("chunkCountSize") != std::string::npos,
                   "unsupported packed chunk-count width passed validation") && result;

    const auto semantic_profile = SemanticProfile();
    auto ahud_profile = semantic_profile;
    ahud_profile.layout["ffield.class"] = 0x00;
    ahud_profile.layout["ffieldClass.name"] = 0x00;
    const anomaly::FeatureLayoutValidatorRegistry feature_validators;
    const anomaly::ProfileResolutionSnapshot empty_snapshot;
    const auto valid_ahud_layout = feature_validators.Validate(
        "ue5-ahud-reflection-v1",
        ahud_profile,
        "ue5.ahud",
        empty_snapshot,
        *memory);
    auto oversized_ahud_profile = ahud_profile;
    oversized_ahud_profile.layout["ffieldClass.name"] = 4097;
    const auto oversized_ahud_layout = feature_validators.Validate(
        "ue5-ahud-reflection-v1",
        oversized_ahud_profile,
        "ue5.ahud",
        empty_snapshot,
        *memory);
    result = Check(
                 valid_ahud_layout.valid && !oversized_ahud_layout.valid &&
                     oversized_ahud_layout.message.find("supported bound") !=
                         std::string::npos,
                 "AHUD reflection validator accepted an oversized owned offset") &&
        result;
    PopulateSemanticLayout(*memory);
    const auto semantic = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                  semantic.state == anomaly::ProfileResolutionState::Ready &&
                       semantic.FeatureAvailable("nte.player") &&
                       semantic.FeatureAvailable("nte.player-esp") &&
                       semantic.FeatureAvailable("nte.player-teleport") &&
                       semantic.FeatureAvailable("nte.entities") &&
                       semantic.FeatureAvailable("ue5.actors") &&
                       semantic.FeatureAvailable("ue5.functions"),
                   "valid NTE feature layout chains did not publish semantic features") &&
        result;

    const std::uintptr_t zero_pointer{};
    const std::uint32_t zero_u32{};
    memory->Write(kSemanticWorldStorage, &zero_pointer, sizeof(zero_pointer));
    const auto no_world = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                  !no_world.FeatureAvailable("ue5.actors") &&
                      no_world.FeatureAvailable("ue5.functions") &&
                      no_world.features.at("nte.player").deferred_validation &&
                      no_world.features.at("nte.entities").deferred_validation,
                  "uninitialized world was not marked for deferred semantic validation") &&
        result;

    PopulateSemanticLayout(*memory);
    memory->Write(kSemanticWorld + 0x18, &zero_pointer, sizeof(zero_pointer));
    const auto no_level = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 !no_level.FeatureAvailable("ue5.actors") &&
                     no_level.FeatureAvailable("ue5.functions"),
                 "uninitialized persistent level incorrectly published actor reflection") &&
        result;

    PopulateSemanticLayout(*memory);
    memory->Write(
        kSemanticObjectRegistry + 0x10, &zero_pointer, sizeof(zero_pointer));
    memory->Write(kSemanticObjectRegistry + 0x20, &zero_u32, sizeof(zero_u32));
    memory->Write(kSemanticObjectRegistry + 0x24, &zero_u32, sizeof(zero_u32));
    memory->Write(kSemanticObjectRegistry + 0x28, &zero_u32, sizeof(zero_u32));
    memory->Write(kSemanticObjectRegistry + 0x2C, &zero_u32, sizeof(zero_u32));
    const auto no_objects = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 no_objects.FeatureAvailable("ue5.objects") &&
                     no_objects.FeatureAvailable("ue5.actors") &&
                     !no_objects.FeatureAvailable("ue5.functions"),
                 "uninitialized object registry incorrectly published function reflection") &&
        result;

    PopulateSemanticLayout(*memory);
    const std::int32_t invalid_actor_capacity{};
    memory->Write(
        kSemanticLevel + 0x2C, &invalid_actor_capacity, sizeof(invalid_actor_capacity));
    const auto invalid_actor_array = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 !invalid_actor_array.FeatureAvailable("ue5.actors") &&
                     invalid_actor_array.FeatureAvailable("ue5.functions"),
                 "invalid actor array capacity did not withdraw actor reflection") &&
        result;

    PopulateSemanticLayout(*memory);
    auto oversized_ufunction_layout = semantic_profile;
    oversized_ufunction_layout.layout["ufunction.returnValueOffset"] = 4097;
    const auto oversized_ufunction = resolver.Resolve(
        fingerprint, &oversized_ufunction_layout, nullptr);
    result = Check(
                 oversized_ufunction.FeatureAvailable("ue5.actors") &&
                     !oversized_ufunction.FeatureAvailable("ue5.functions"),
                 "out-of-bounds UFunction layout offset was accepted") &&
        result;

    auto deferred_actor_reflection = semantic;
    memory->Write(
        kSemanticLevel + 0x2C, &invalid_actor_capacity, sizeof(invalid_actor_capacity));
    const auto scans_before_actor_revalidation = memory->scan_count;
    const bool actor_revalidated = resolver.RevalidateDeferredCandidates(
        semantic_profile, deferred_actor_reflection);
    result = Check(
                 actor_revalidated &&
                     !deferred_actor_reflection.FeatureAvailable("ue5.actors") &&
                     deferred_actor_reflection.FeatureAvailable("ue5.functions") &&
                     memory->scan_count == scans_before_actor_revalidation,
                 "actor reflection revalidation did not withdraw the malformed layout") &&
        result;

    PopulateSemanticLayout(*memory);

    auto missing_actor_reflection_layout = semantic_profile;
    missing_actor_reflection_layout.layout.erase("level.actors");
    const auto missing_actor_reflection = resolver.Resolve(
        fingerprint, &missing_actor_reflection_layout, nullptr);
    result = Check(
                 !missing_actor_reflection.FeatureAvailable("ue5.actors") &&
                     missing_actor_reflection.FeatureAvailable("ue5.functions") &&
                     std::ranges::any_of(
                         missing_actor_reflection.features.at("ue5.actors").validation_diagnostics,
                         [](const std::string& diagnostic) {
                             return diagnostic.find("level.actors") != std::string::npos;
                         }),
                 "actor reflection feature accepted an incomplete actor layout") &&
        result;

    auto missing_actor_name_layout = semantic_profile;
    missing_actor_name_layout.layout.erase("names.blocksOffset");
    const auto missing_actor_names = resolver.Resolve(
        fingerprint, &missing_actor_name_layout, nullptr);
    result = Check(
                 !missing_actor_names.FeatureAvailable("ue5.actors") &&
                     !missing_actor_names.FeatureAvailable("ue5.functions") &&
                     std::ranges::any_of(
                         missing_actor_names.features.at("ue5.actors").validation_diagnostics,
                         [](const std::string& diagnostic) {
                             return diagnostic.find("names.blocksOffset") != std::string::npos;
                         }),
                 "actor reflection feature accepted an incomplete name layout") &&
        result;

    auto missing_function_reflection_layout = semantic_profile;
    missing_function_reflection_layout.layout.erase("ufunction.returnValueOffset");
    const auto missing_function_reflection = resolver.Resolve(
        fingerprint, &missing_function_reflection_layout, nullptr);
    result = Check(
                 missing_function_reflection.FeatureAvailable("ue5.actors") &&
                     !missing_function_reflection.FeatureAvailable("ue5.functions") &&
                     std::ranges::any_of(
                         missing_function_reflection.features.at("ue5.functions").validation_diagnostics,
                         [](const std::string& diagnostic) {
                             return diagnostic.find("ufunction.returnValueOffset") !=
                                 std::string::npos;
                         }),
                 "function reflection feature accepted an incomplete UFunction layout") &&
        result;

    auto production_bool_layout_profile = semantic_profile;
    production_bool_layout_profile.layout["fboolProperty.byteOffset"] = 113;
    const auto production_bool_layout = resolver.Resolve(
        fingerprint, &production_bool_layout_profile, nullptr);
    result = Check(
                 production_bool_layout.state == anomaly::ProfileResolutionState::Ready &&
                     production_bool_layout.FeatureAvailable(
                         "nte.player-teleport"),
                 "teleport feature rejected a valid reflected bool field offset") &&
        result;

    const std::uintptr_t null_pointer{};
    auto incomplete_esp_layout = semantic_profile;
    incomplete_esp_layout.layout.erase("cameraManager.fov");
    PopulateSemanticLayout(*memory);
    memory->Write(kSemanticLocalPlayers, &null_pointer, sizeof(null_pointer));
    const auto invalid_esp_during_transient = resolver.Resolve(
        fingerprint, &incomplete_esp_layout, nullptr);
    const auto* invalid_esp_transient_feature =
        invalid_esp_during_transient.features.contains("nte.player-esp")
        ? &invalid_esp_during_transient.features.at("nte.player-esp")
        : nullptr;
    result = Check(
                 invalid_esp_during_transient.state == anomaly::ProfileResolutionState::Ready &&
                     invalid_esp_during_transient.FeatureAvailable("nte.player") &&
                     !invalid_esp_during_transient.FeatureAvailable("nte.player-esp") &&
                     invalid_esp_during_transient.FeatureAvailable("nte.entities") &&
                     invalid_esp_transient_feature != nullptr && std::ranges::any_of(
                         invalid_esp_transient_feature->validation_diagnostics,
                         [](const std::string& diagnostic) {
                             return diagnostic.find("cameraManager.fov") != std::string::npos;
                         }),
                 "optional ESP layout failure degraded required semantic features") &&
        result;

    memory->Write(kSemanticLocalPlayers, &null_pointer, sizeof(null_pointer));
    const auto empty_local_player = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 empty_local_player.state == anomaly::ProfileResolutionState::Ready &&
                     empty_local_player.FeatureAvailable("nte.player") &&
                     empty_local_player.FeatureAvailable("nte.player-esp") &&
                     empty_local_player.FeatureAvailable("nte.entities"),
                 "empty local player transient degraded semantic features") &&
        result;

    PopulateSemanticLayout(*memory);
    const std::int32_t empty_player_count{};
    memory->Write(kSemanticGameInstance + 0x28, &empty_player_count, sizeof(empty_player_count));
    memory->Write(kSemanticGameInstance + 0x20, &null_pointer, sizeof(null_pointer));
    const auto empty_player_array = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 empty_player_array.state == anomaly::ProfileResolutionState::Ready &&
                     empty_player_array.FeatureAvailable("nte.player") &&
                     empty_player_array.FeatureAvailable("nte.player-esp") &&
                     empty_player_array.FeatureAvailable("nte.entities"),
                 "null zero-count local player array degraded semantic features") &&
        result;

    PopulateSemanticLayout(*memory);
    const std::uintptr_t malformed_array = FixtureMemory::kBase + 1;
    memory->Write(kSemanticGameInstance + 0x28, &empty_player_count, sizeof(empty_player_count));
    memory->Write(kSemanticGameInstance + 0x20, &malformed_array, sizeof(malformed_array));
    const auto malformed_empty_player_array = resolver.Resolve(
        fingerprint, &semantic_profile, nullptr);
    result = Check(
                 malformed_empty_player_array.state == anomaly::ProfileResolutionState::Degraded &&
                     !malformed_empty_player_array.FeatureAvailable("nte.player") &&
                     !malformed_empty_player_array.FeatureAvailable("nte.player-esp"),
                 "malformed zero-count local player array did not degrade semantic features") &&
        result;

    PopulateSemanticLayout(*memory);
    memory->Write(kSemanticController + 0x18, &null_pointer, sizeof(null_pointer));
    const auto empty_pawn = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 empty_pawn.state == anomaly::ProfileResolutionState::Ready &&
                     empty_pawn.FeatureAvailable("nte.player") &&
                     empty_pawn.FeatureAvailable("nte.player-esp") &&
                     empty_pawn.FeatureAvailable("nte.entities"),
                 "empty pawn transient degraded semantic features") &&
        result;

    PopulateSemanticLayout(*memory);
    const std::int32_t empty_actor_count{};
    memory->Write(kSemanticLevel + 0x28, &empty_actor_count, sizeof(empty_actor_count));
    const auto empty_actor_array = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 empty_actor_array.state == anomaly::ProfileResolutionState::Ready &&
                     empty_actor_array.FeatureAvailable("nte.entities"),
                 "empty actor array transient degraded entity features") &&
        result;

    PopulateSemanticLayout(*memory);
    memory->Write(kSemanticLevel + 0x28, &empty_actor_count, sizeof(empty_actor_count));
    memory->Write(kSemanticLevel + 0x20, &null_pointer, sizeof(null_pointer));
    const auto empty_actor_storage = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 empty_actor_storage.state == anomaly::ProfileResolutionState::Ready &&
                     empty_actor_storage.FeatureAvailable("nte.entities"),
                 "null zero-count actor array degraded entity features") &&
        result;

    PopulateSemanticLayout(*memory);
    memory->Write(kSemanticLevel + 0x28, &empty_actor_count, sizeof(empty_actor_count));
    memory->Write(kSemanticLevel + 0x20, &malformed_array, sizeof(malformed_array));
    const auto malformed_empty_actor_array = resolver.Resolve(
        fingerprint, &semantic_profile, nullptr);
    result = Check(
                 malformed_empty_actor_array.state == anomaly::ProfileResolutionState::Degraded &&
                     malformed_empty_actor_array.FeatureAvailable("nte.player") &&
                     malformed_empty_actor_array.FeatureAvailable("nte.player-esp") &&
                     !malformed_empty_actor_array.FeatureAvailable("nte.entities"),
                 "malformed zero-count actor array did not degrade entity features") &&
        result;

    PopulateSemanticLayout(*memory);
    memory->Write(kSemanticActors, &null_pointer, sizeof(null_pointer));
    const auto empty_actor = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 empty_actor.state == anomaly::ProfileResolutionState::Ready &&
                     empty_actor.FeatureAvailable("nte.player") &&
                     empty_actor.FeatureAvailable("nte.player-esp") &&
                     empty_actor.FeatureAvailable("nte.entities"),
                 "empty actor transient degraded semantic features") &&
        result;

    PopulateSemanticLayout(*memory);
    memory->Write(kSemanticLocalPlayers, &malformed_array, sizeof(malformed_array));
    const auto malformed_semantic = resolver.Resolve(fingerprint, &semantic_profile, nullptr);
    result = Check(
                 malformed_semantic.state == anomaly::ProfileResolutionState::Degraded &&
                     !malformed_semantic.FeatureAvailable("nte.player") &&
                     !malformed_semantic.FeatureAvailable("nte.player-esp"),
                 "malformed local player pointer did not degrade semantic features") &&
        result;

    PopulateSemanticLayout(*memory);
    const float invalid_camera_fov = 180.0F;
    memory->Write(kSemanticCameraFov, &invalid_camera_fov, sizeof(invalid_camera_fov));
    auto invalid_semantic = semantic;
    const auto scans_before_layout_degradation = memory->scan_count;
    const bool layout_degraded = resolver.RevalidateDeferredCandidates(
        semantic_profile, invalid_semantic);
    const auto* invalid_esp = invalid_semantic.features.contains("nte.player-esp")
        ? &invalid_semantic.features.at("nte.player-esp")
        : nullptr;
    const auto* invalid_entities = invalid_semantic.features.contains("nte.entities")
        ? &invalid_semantic.features.at("nte.entities")
        : nullptr;
    result = Check(
                 layout_degraded &&
                     invalid_semantic.state == anomaly::ProfileResolutionState::Ready &&
                     invalid_semantic.FeatureAvailable("nte.player") &&
                     !invalid_semantic.FeatureAvailable("nte.player-esp") &&
                     invalid_semantic.FeatureAvailable("nte.entities") &&
                     invalid_esp != nullptr && std::ranges::any_of(
                         invalid_esp->validation_diagnostics, [](const std::string& diagnostic) {
                             return diagnostic.starts_with("nte-player-esp-layout-v1:");
                         }) &&
                     invalid_entities != nullptr &&
                     invalid_entities->unavailable_dependencies.empty() &&
                     memory->scan_count == scans_before_layout_degradation,
                 "deferred layout validation did not degrade dependent features") &&
        result;

    auto unchanged_semantic = invalid_semantic;
    const auto scans_before_unchanged_revalidation = memory->scan_count;
    const bool unchanged_revalidated = resolver.RevalidateDeferredCandidates(
        semantic_profile, unchanged_semantic);
    result = Check(
                 !unchanged_revalidated &&
                     !unchanged_semantic.FeatureAvailable("nte.player-esp") &&
                     unchanged_semantic.FeatureAvailable("nte.entities") &&
                     memory->scan_count == scans_before_unchanged_revalidation,
                 "unchanged deferred validation recomputed feature availability") &&
        result;

    const float valid_camera_fov = 90.0F;
    memory->Write(kSemanticCameraFov, &valid_camera_fov, sizeof(valid_camera_fov));
    auto recovered_semantic = invalid_semantic;
    const auto scans_before_feature_revalidation = memory->scan_count;
    const bool feature_revalidated = resolver.RevalidateDeferredCandidates(
        semantic_profile, recovered_semantic);
    result = Check(
                feature_revalidated &&
                    recovered_semantic.state == anomaly::ProfileResolutionState::Ready &&
                    recovered_semantic.FeatureAvailable("nte.player-esp") &&
                    recovered_semantic.FeatureAvailable("nte.entities") &&
                    memory->scan_count == scans_before_feature_revalidation,
                "deferred layout validation did not recover without rescanning") &&
        result;

    auto caller_owned_profile = profile;
    caller_owned_profile.features.clear();
    auto caller_owned_snapshot = first;
    caller_owned_snapshot.state = anomaly::ProfileResolutionState::Ready;
    caller_owned_snapshot.features = {
        {"nte.player", {"nte.player", true, {}}},
        {"nte.entities", {"nte.entities", true, {}}},
    };
    const bool caller_owned_revalidated = resolver.RevalidateDeferredCandidates(
        caller_owned_profile, caller_owned_snapshot);
    result = Check(
                 !caller_owned_revalidated &&
                     caller_owned_snapshot.state == anomaly::ProfileResolutionState::Ready &&
                     caller_owned_snapshot.FeatureAvailable("nte.player") &&
                     caller_owned_snapshot.FeatureAvailable("nte.entities"),
                 "deferred revalidation cleared a caller-owned feature snapshot") &&
        result;

    auto degraded = profile;
    degraded.symbols.at("ue5.GameTick").pattern = "DE AD BE EF";
    degraded.source_hash = std::string(64, 'c');
    const auto partial = resolver.Resolve(fingerprint, &degraded, nullptr);
    result = Check(partial.state == anomaly::ProfileResolutionState::Degraded &&
                       partial.FeatureAvailable("ue5.world") &&
                       !partial.FeatureAvailable("ue5.framework") &&
                       partial.FindSymbol("ue5.GameTick") &&
                       partial.FindSymbol("ue5.GameTick")->state ==
                           anomaly::SymbolResolutionState::NotFound,
                   "partial symbol failure did not degrade only its feature") && result;

    const auto unknown = resolver.Resolve(fingerprint, nullptr, &cache);
    result = Check(unknown.state == anomaly::ProfileResolutionState::NoProfile &&
                       unknown.symbols.empty() && unknown.features.empty(),
                   "unknown build published adapter symbols") && result;

    auto ambiguous = profile;
    const auto duplicate_tick = tick;
    memory->Write(0x300, duplicate_tick.data(), duplicate_tick.size());
    ambiguous.source_hash = std::string(64, 'd');
    const auto multiple = resolver.Resolve(fingerprint, &ambiguous, nullptr);
    result = Check(multiple.FindSymbol("ue5.GameTick") &&
                       multiple.FindSymbol("ue5.GameTick")->state ==
                           anomaly::SymbolResolutionState::Ambiguous &&
                       !multiple.FeatureAvailable("ue5.framework"),
                   "multiple valid candidates were not rejected") && result;

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result ? 0 : 1;
}
