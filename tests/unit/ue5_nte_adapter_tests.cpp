#include "anomaly/ue5_nte_adapter.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace anomaly::test {
class AdapterServiceRegistryTestAccess final {
public:
    [[nodiscard]] static std::unique_lock<std::timed_mutex> HoldWriteLock(
        AdapterServiceRegistry& registry) {
        return std::unique_lock<std::timed_mutex>(registry.mutex_);
    }
};
}  // namespace anomaly::test

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

bool HasMessage(const AnomalyStatusV1& status, std::string_view expected) {
    return status.message.data != nullptr &&
        std::string_view(status.message.data, status.message.size) == expected;
}

constexpr char kNteMetricsFeatureId[] = "nte.metrics";
constexpr AnomalyStringViewV1 kNteMetricsFeature{
    kNteMetricsFeatureId, sizeof(kNteMetricsFeatureId) - 1U};

class FixtureMemory final : public anomaly::SymbolMemory {
public:
    FixtureMemory() : bytes_(0x4000) {
        module_.name = L"fixture.exe";
        module_.base = kBase;
        module_.size = bytes_.size();
        sections_.push_back({".text", kBase, 0x800, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE});
        sections_.push_back({".data", kBase + 0x800, 0x3800, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE});
    }

    template <typename T>
    void Put(std::uintptr_t address, const T& value) {
        std::memcpy(bytes_.data() + (address - kBase), &value, sizeof(value));
    }

    void PutBytes(std::uintptr_t address, const void* value, std::size_t size) {
        std::memcpy(bytes_.data() + (address - kBase), value, size);
    }

    void BlockRead(std::uintptr_t address) noexcept {
        blocked_address_.store(address, std::memory_order_release);
    }

    void ClearBlockedRead() noexcept {
        blocked_address_.store(0, std::memory_order_release);
    }

    void BlockWrite(std::uintptr_t address) noexcept {
        blocked_write_address_.store(address, std::memory_order_release);
    }

    void ClearBlockedWrite() noexcept {
        blocked_write_address_.store(0, std::memory_order_release);
    }

    void SuspendRead(std::uintptr_t address) noexcept {
        suspended_read_entered_.store(false, std::memory_order_release);
        suspended_read_released_.store(false, std::memory_order_release);
        suspended_address_.store(address, std::memory_order_release);
    }

    void ReleaseSuspendedRead() noexcept {
        suspended_read_released_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool SuspendedReadEntered() const noexcept {
        return suspended_read_entered_.load(std::memory_order_acquire);
    }

    std::uint64_t PlayerChainReads() const noexcept {
        return player_chain_reads_.load(std::memory_order_acquire);
    }

    std::uint64_t EntityChainReads() const noexcept {
        return entity_chain_reads_.load(std::memory_order_acquire);
    }

    std::uint64_t WorldZeroOffsetReads() const noexcept {
        return world_zero_offset_reads_.load(std::memory_order_acquire);
    }

    void ResetReadCount() const noexcept {
        total_reads_.store(0, std::memory_order_release);
    }

    std::uint64_t ReadCount() const noexcept {
        return total_reads_.load(std::memory_order_acquire);
    }

    void SetQueryProtection(DWORD protection) noexcept {
        query_protection_.store(protection, std::memory_order_release);
    }

    std::uint64_t WriteCount() const noexcept {
        return write_count_.load(std::memory_order_acquire);
    }

    std::optional<ue5mem::ModuleInfo> FindModule(std::wstring_view name) const override {
        return name == module_.name ? std::optional(module_) : std::nullopt;
    }
    std::vector<ue5mem::SectionInfo> Sections(const ue5mem::ModuleInfo&) const override {
        return sections_;
    }
    std::vector<std::uintptr_t> Scan(
        const ue5mem::ModuleInfo&, std::string_view, std::string_view, std::size_t) const override {
        return {};
    }
    bool Read(std::uintptr_t address, void* destination, std::size_t size) const override {
        total_reads_.fetch_add(1, std::memory_order_relaxed);
        if (address == blocked_address_.load(std::memory_order_acquire)) return false;
        if (address == suspended_address_.load(std::memory_order_acquire)) {
            suspended_read_entered_.store(true, std::memory_order_release);
            while (!suspended_read_released_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            suspended_address_.store(0, std::memory_order_release);
        }
        if (address < kBase || size > bytes_.size() || address - kBase > bytes_.size() - size) {
            return false;
        }
        if (address == kBase + 0x1010) {
            player_chain_reads_.fetch_add(1, std::memory_order_relaxed);
        } else if (address == kBase + 0x2420) {
            entity_chain_reads_.fetch_add(1, std::memory_order_relaxed);
        } else if (address == kBase + 0x1000) {
            world_zero_offset_reads_.fetch_add(1, std::memory_order_relaxed);
        }
        std::memcpy(destination, bytes_.data() + (address - kBase), size);
        return true;
    }
    bool Write(std::uintptr_t address, const void* source, std::size_t size) const override {
        if (source == nullptr || address < kBase || size > bytes_.size() ||
            address - kBase > bytes_.size() - size) {
            return false;
        }
        if (address == blocked_write_address_.load(std::memory_order_acquire)) return false;
        write_count_.fetch_add(1, std::memory_order_relaxed);
        std::memcpy(bytes_.data() + (address - kBase), source, size);
        return true;
    }
    std::optional<anomaly::SymbolMemoryRegion> Query(std::uintptr_t address) const override {
        if (address < kBase || address - kBase >= bytes_.size()) return std::nullopt;
        return anomaly::SymbolMemoryRegion{
            kBase,
            bytes_.size(),
            MEM_COMMIT,
            address < kBase + 0x800 ? PAGE_EXECUTE_READ :
                                     query_protection_.load(std::memory_order_acquire),
            MEM_PRIVATE};
    }

    static constexpr std::uintptr_t kBase = 0x10000000;

private:
    ue5mem::ModuleInfo module_;
    std::vector<ue5mem::SectionInfo> sections_;
    mutable std::vector<std::uint8_t> bytes_;
    mutable std::atomic<std::uintptr_t> blocked_address_{};
    mutable std::atomic<std::uintptr_t> blocked_write_address_{};
    mutable std::atomic<std::uintptr_t> suspended_address_{};
    mutable std::atomic_bool suspended_read_entered_{};
    mutable std::atomic_bool suspended_read_released_{};
    mutable std::atomic<std::uint64_t> player_chain_reads_{};
    mutable std::atomic<std::uint64_t> entity_chain_reads_{};
    mutable std::atomic<std::uint64_t> world_zero_offset_reads_{};
    mutable std::atomic<std::uint64_t> total_reads_{};
    mutable std::atomic<DWORD> query_protection_{PAGE_READWRITE};
    mutable std::atomic<std::uint64_t> write_count_{};
};

struct BlockingCallbackDestructionState {
    std::atomic_bool block{};
    std::atomic_bool entered{};
    std::atomic_bool release{};
};

struct BlockingCallbackTarget {
    std::shared_ptr<BlockingCallbackDestructionState> state;

    void operator()(double) const noexcept {}

    ~BlockingCallbackTarget() {
        if (state == nullptr || !state->block.load(std::memory_order_acquire)) return;
        state->entered.store(true, std::memory_order_release);
        while (!state->release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
};

constexpr std::uintptr_t kObjectRegistry = FixtureMemory::kBase + 0x980;
constexpr std::uintptr_t kObjectPageTable = FixtureMemory::kBase + 0x1800;
constexpr std::uintptr_t kObjectPageTableReplacement = FixtureMemory::kBase + 0x1E00;
constexpr std::uintptr_t kObjectChunk0 = FixtureMemory::kBase + 0x1B00;
constexpr std::uintptr_t kObjectChunk1 = FixtureMemory::kBase + 0x1C00;
constexpr std::uintptr_t kPlayerSceneLocation = FixtureMemory::kBase + 0x1660;
constexpr std::uintptr_t kTeleportFunction = FixtureMemory::kBase + 0x2800;
constexpr std::uintptr_t kTeleportFunctionClass = FixtureMemory::kBase + 0x2900;
constexpr std::uintptr_t kTeleportActorClass = FixtureMemory::kBase + 0x2A00;
constexpr std::uintptr_t kTeleportVectorStruct = FixtureMemory::kBase + 0x2B00;
constexpr std::uintptr_t kTeleportHitResultStruct = FixtureMemory::kBase + 0x2C00;
constexpr std::uintptr_t kTeleportNewLocationProperty = FixtureMemory::kBase + 0x2D00;
constexpr std::uintptr_t kTeleportSweepProperty = FixtureMemory::kBase + 0x2E00;
constexpr std::uintptr_t kTeleportSweepHitResultProperty = FixtureMemory::kBase + 0x2F00;
constexpr std::uintptr_t kTeleportFlagProperty = FixtureMemory::kBase + 0x3000;
constexpr std::uintptr_t kTeleportReturnProperty = FixtureMemory::kBase + 0x3100;
constexpr std::uintptr_t kPlayerState = FixtureMemory::kBase + 0x3800;
constexpr std::uintptr_t kPlayerStateVtable = FixtureMemory::kBase + 0x400;

anomaly::ResolvedSymbol Available(std::string id, std::uintptr_t address) {
    anomaly::ResolvedSymbol symbol;
    symbol.id = std::move(id);
    symbol.module = L"fixture.exe";
    symbol.address = address;
    symbol.state = anomaly::SymbolResolutionState::Resolved;
    return symbol;
}

anomaly::BuildProfile Profile() {
    anomaly::BuildProfile profile;
    profile.game = "nte";
    profile.source_hash = std::string(64, 'b');
    profile.layout = {
        {"world.nameOffset", 0x08},
        {"world.gameInstance", 0x10},
        {"world.persistentLevel", 0x18},
        {"world.levels", 0x38},
        {"level.actors", 0x20},
        {"entities.maxLevels", 16},
        {"entities.maxCount", 128},
        {"gameInstance.localPlayers", 0x20},
        {"localPlayer.controller", 0x30},
        {"controller.pawn", 0x40},
        {"controller.cameraManager", 0x48},
        {"actor.rootComponent", 0x50},
        {"object.internalIndex", 0x0C},
        {"object.class", 0x10},
        {"sceneComponent.mobility", 0x58},
        {"sceneComponent.location", 0x60},
        {"sceneComponent.boundsOrigin", 0x60},
        {"sceneComponent.boundsExtent", 0x78},
        {"cameraManager.location", 0x100},
        {"cameraManager.rotation", 0x118},
        {"cameraManager.fov", 0x130},
        {"objects.itemsOffset", 0x10},
        {"objects.maxCountOffset", 0x20},
        {"objects.countOffset", 0x24},
        {"objects.maxChunksOffset", 0x28},
        {"objects.numChunksOffset", 0x2E},
        {"objects.chunkCountSize", 2},
        {"objects.chunkSize", 2},
        {"objects.itemStride", 0x18},
        {"objects.objectOffset", 0x00},
        {"objects.serialOffset", 0x10},
        {"object.nameOffset", 0x08},
        {"names.blocksOffset", 0x00},
        {"names.blockBits", 16},
        {"names.entryStride", 2},
        {"names.headerLengthShift", 6},
    };
    return profile;
}

anomaly::BuildProfile SemanticProfile() {
    auto profile = Profile();
    profile.features = {
        {"ue5.framework", {"ue5.GameTick"}},
        {"ue5.world", {"ue5.GWorld", "ue5.GameTick"}},
        {"ue5.names", {"ue5.FNamePool"}},
        {"ue5.objects", {"ue5.GObjects", "ue5.GameTick"}},
        {"ue5.process-event", {"ue5.ProcessEvent"}},
        {"ue5.actors", {"ue5.GWorld", "ue5.GameTick", "ue5.FNamePool"}},
        {"nte.session", {"ue5.GWorld", "ue5.GameTick"}},
        {"nte.player", {"ue5.GWorld", "ue5.GameTick"}},
        {"nte.player-esp", {"ue5.GWorld", "ue5.GameTick"}},
        {"nte.player-teleport",
         {"ue5.GWorld", "ue5.GameTick", "ue5.FNamePool", "ue5.GObjects"}},
        {"nte.entities", {"ue5.GWorld", "ue5.GameTick"}},
    };
    profile.feature_layout_validators = {
        {"nte.player", {"nte-player-layout-v1"}},
        {"nte.player-esp", {"nte-player-esp-layout-v1"}},
        {"nte.player-teleport", {"nte-player-teleport-layout-v1"}},
        {"ue5.process-event", {"ue5-process-event-abi-v1"}},
        {"nte.entities", {"nte-entities-layout-v1"}},
        {"ue5.actors", {"ue5-actors-reflection-v1"}},
    };
    profile.feature_dependencies = {
        {"nte.player-esp", {"nte.player"}},
        {"nte.player-teleport",
         {"nte.player", "ue5.names", "ue5.objects", "ue5.process-event"}},
    };
    profile.optional_features = {
        "ue5.process-event", "nte.player-esp", "nte.player-teleport"};
    profile.layout.insert({"object.outer", 0x20});
    profile.layout.insert({"ustruct.propertyLink", 0x30});
    profile.layout.insert({"ufunction.numParms", 0x40});
    profile.layout.insert({"ufunction.parmsSize", 0x42});
    profile.layout.insert({"ufunction.returnValueOffset", 0x44});
    profile.layout.insert({"ffield.name", 0x08});
    profile.layout.insert({"fproperty.arrayDim", 0x10});
    profile.layout.insert({"fproperty.elementSize", 0x14});
    profile.layout.insert({"fproperty.offsetInternal", 0x18});
    profile.layout.insert({"fproperty.propertyLinkNext", 0x20});
    profile.layout.insert({"fstructProperty.struct", 0x28});
    profile.layout.insert({"fboolProperty.fieldSize", 0x28});
    profile.layout.insert({"fboolProperty.byteOffset", 0x29});
    profile.layout.insert({"fboolProperty.byteMask", 0x2A});
    profile.layout.insert({"fboolProperty.fieldMask", 0x2B});
    return profile;
}

anomaly::ProfileResolutionSnapshot Resolution() {
    anomaly::ProfileResolutionSnapshot resolution;
    resolution.state = anomaly::ProfileResolutionState::Ready;
    resolution.build_id = "nte-win64-fixture";
    resolution.profile_hash = std::string(64, 'b');
    resolution.symbols.emplace("ue5.GWorld", Available("ue5.GWorld", FixtureMemory::kBase + 0x900));
    resolution.symbols.emplace("ue5.GObjects", Available("ue5.GObjects", kObjectRegistry));
    resolution.symbols.emplace("ue5.FNamePool", Available("ue5.FNamePool", FixtureMemory::kBase + 0xA00));
    resolution.symbols.emplace("ue5.GameTick", Available("ue5.GameTick", FixtureMemory::kBase + 0x200));
    resolution.symbols.emplace(
        "ue5.ProcessEvent", Available("ue5.ProcessEvent", FixtureMemory::kBase + 0x400));
    for (const std::string id : {
             "ue5.framework", "ue5.world", "ue5.names", "ue5.objects",
             "ue5.actors", "ue5.process-event",
              "nte.session", "nte.player", "nte.player-esp",
              "nte.player-teleport", "nte.entities"}) {
        resolution.features.emplace(id, anomaly::FeatureResolution{id, true, {}});
    }
    return resolution;
}

anomaly::FeatureLayoutValidatorRegistry FixtureFeatureLayoutValidators() {
    anomaly::FeatureLayoutValidatorRegistry validators;
    validators.Register(
        std::string(anomaly::kUe5ProcessEventAbiValidator),
        [](const anomaly::BuildProfile&,
           const std::string_view feature,
           const anomaly::ProfileResolutionSnapshot&,
           const anomaly::SymbolMemory&) {
            return anomaly::FeatureValidationResult{
                feature == anomaly::kUe5ProcessEventFeature,
                "fixture validator only accepts the UE5 ProcessEvent capability"};
        });
    return validators;
}

anomaly::BuildFingerprint Fingerprint() {
    anomaly::BuildFingerprint fingerprint;
    fingerprint.game = "nte";
    fingerprint.id = "nte-win64-fixture";
    fingerprint.module = L"fixture.exe";
    fingerprint.text_sha256 = std::string(64, 'a');
    return fingerprint;
}

void Populate(const std::shared_ptr<FixtureMemory>& memory) {
    using F = FixtureMemory;
    const std::uintptr_t world = F::kBase + 0x1000;
    const std::uintptr_t game_instance = F::kBase + 0x1100;
    const std::uintptr_t players = F::kBase + 0x1200;
    const std::uintptr_t local_player = F::kBase + 0x1300;
    const std::uintptr_t controller = F::kBase + 0x1400;
    const std::uintptr_t pawn = F::kBase + 0x1500;
    const std::uintptr_t root = F::kBase + 0x1600;
    const std::uintptr_t camera_manager = F::kBase + 0x2000;
    const std::uintptr_t level = F::kBase + 0x2400;
    const std::uintptr_t levels = F::kBase + 0x2480;
    const std::uintptr_t actors = F::kBase + 0x2500;
    const std::uintptr_t pawn_class = F::kBase + 0x2600;
    memory->Put(F::kBase + 0x900, world);
    const std::uint32_t world_name = 2;
    memory->Put(world + 0x08, world_name);
    memory->Put(world + 0x10, game_instance);
    memory->Put(world + 0x18, level);
    memory->Put(world + 0x38, levels);
    const std::int32_t level_count = 1;
    memory->Put(world + 0x40, level_count);
    memory->Put(levels, level);
    memory->Put(level + 0x20, actors);
    const std::int32_t actor_count = 1;
    memory->Put(level + 0x28, actor_count);
    memory->Put(actors, pawn);
    memory->Put(game_instance + 0x20, players);
    const std::int32_t player_count = 1;
    memory->Put(game_instance + 0x28, player_count);
    memory->Put(players, local_player);
    memory->Put(local_player + 0x30, controller);
    memory->Put(controller + 0x40, pawn);
    memory->Put(controller + 0x48, camera_manager);
    memory->Put(pawn + 0x50, root);
    memory->Put(pawn + 0x10, pawn_class);
    const std::int32_t pawn_index = 11;
    const std::int32_t class_index = 22;
    memory->Put(pawn + 0x0C, pawn_index);
    memory->Put(pawn_class + 0x0C, class_index);
    const std::uint32_t pawn_name = 16;
    const std::uint32_t class_name = 8;
    memory->Put(pawn + 0x08, pawn_name);
    memory->Put(pawn_class + 0x08, class_name);
    const std::uint8_t mobility = 2;
    memory->Put(root + 0x58, mobility);
    const std::array<double, 3> position{12.5, -7.25, 99.0};
    const std::array<double, 3> extent{42.0, 42.0, 88.0};
    const std::array<double, 3> camera_position{-300.0, -7.25, 140.0};
    const std::array<double, 3> camera_rotation{-5.0, 0.0, 0.0};
    const float camera_fov = 80.0F;
    memory->PutBytes(root + 0x60, position.data(), sizeof(position));
    memory->PutBytes(root + 0x90, position.data(), sizeof(position));
    memory->PutBytes(root + 0x78, extent.data(), sizeof(extent));
    memory->PutBytes(camera_manager + 0x100, camera_position.data(), sizeof(camera_position));
    memory->PutBytes(camera_manager + 0x118, camera_rotation.data(), sizeof(camera_rotation));
    memory->Put(camera_manager + 0x130, camera_fov);

    const std::uintptr_t object = F::kBase + 0x1900;
    const std::uintptr_t last_object = F::kBase + 0x1D00;
    memory->Put(kObjectRegistry + 0x10, kObjectPageTable);
    const std::uint32_t object_max_count = 4;
    const std::uint32_t object_count = 3;
    const std::uint16_t object_max_chunks = 2;
    const std::uint16_t object_num_chunks = 2;
    memory->Put(kObjectRegistry + 0x20, object_max_count);
    memory->Put(kObjectRegistry + 0x24, object_count);
    memory->Put(kObjectRegistry + 0x28, object_max_chunks);
    memory->Put(kObjectRegistry + 0x2E, object_num_chunks);
    memory->Put(kObjectPageTable, kObjectChunk0);
    memory->Put(kObjectPageTable + sizeof(std::uintptr_t), kObjectChunk1);
    memory->Put(kObjectChunk0, object);
    const std::uint32_t first_serial = 101;
    memory->Put(kObjectChunk0 + 0x10, first_serial);
    const std::uint32_t teleport_serial = 202;
    memory->Put(kObjectChunk0 + 0x18, kTeleportFunction);
    memory->Put(kObjectChunk0 + 0x28, teleport_serial);
    memory->Put(kObjectChunk1, last_object);
    const std::uint32_t last_serial = 303;
    memory->Put(kObjectChunk1 + 0x10, last_serial);
    const std::uint32_t object_name = 2;
    memory->Put(object + 0x08, object_name);
    const std::uint32_t last_object_name = 16;
    memory->Put(last_object + 0x08, last_object_name);

    const std::uintptr_t name_block = F::kBase + 0x1A00;
    memory->Put(F::kBase + 0xA00, name_block);
    const auto put_name = [&](std::uint32_t id, std::string_view value) {
        const std::uintptr_t entry = name_block + static_cast<std::uintptr_t>(id) * 2;
        const auto header = static_cast<std::uint16_t>(value.size() << 6U);
        memory->Put(entry, header);
        memory->PutBytes(entry + 2, value.data(), value.size());
    };
    put_name(2, "World");
    put_name(class_name, "PawnClass");
    put_name(pawn_name, "Pawn_1");
    constexpr std::uint32_t box_property_name = 94;
    constexpr std::uint32_t bool_property_name = 100;
    constexpr std::uint32_t fname_property_name = 108;
    put_name(box_property_name, "Box");
    put_name(bool_property_name, "CanOpen");
    put_name(fname_property_name, "Extract_ID");

    const std::uintptr_t box_property = F::kBase + 0x3800;
    const std::uintptr_t bool_property = F::kBase + 0x3900;
    const std::uintptr_t fname_property = F::kBase + 0x3A00;
    memory->Put(pawn_class + 0x30, box_property);
    const auto put_entity_property = [&](const std::uintptr_t property, const std::uint32_t name_id,
                                  const std::int32_t element_size, const std::int32_t offset,
                                  const std::uintptr_t next) {
        const std::int32_t array_dim = 1;
        memory->Put(property + 0x08, name_id);
        memory->Put(property + 0x10, array_dim);
        memory->Put(property + 0x14, element_size);
        memory->Put(property + 0x18, offset);
        memory->Put(property + 0x20, next);
    };
    put_entity_property(box_property, box_property_name, 8, 0xA0, bool_property);
    put_entity_property(bool_property, bool_property_name, 1, 0xA8, fname_property);
    put_entity_property(fname_property, fname_property_name, 8, 0xB0, 0);
    memory->Put(pawn + 0xA0, root);
    const std::uint8_t can_open = 1;
    memory->Put(pawn + 0xA8, can_open);
    const std::array<std::uint32_t, 2> extraction_id{2, 0};
    memory->PutBytes(pawn + 0xB0, extraction_id.data(), sizeof(extraction_id));
    const std::uint8_t bool_field_size = 1;
    const std::uint8_t bool_byte_offset = 0;
    const std::uint8_t bool_mask = 1;
    memory->Put(bool_property + 0x28, bool_field_size);
    memory->Put(bool_property + 0x29, bool_byte_offset);
    memory->Put(bool_property + 0x2A, bool_mask);
    memory->Put(bool_property + 0x2B, bool_mask);

    constexpr std::uint32_t kFunctionName = 20;
    // FName ids are word offsets into a variable-length pool, not sequential
    // ordinals. Keep the fixture entries non-overlapping so reflection sees
    // the actual property names.
    constexpr std::uint32_t kFunctionClassName = 32;
    constexpr std::uint32_t kActorClassName = 40;
    constexpr std::uint32_t kVectorName = 44;
    constexpr std::uint32_t kHitResultName = 48;
    constexpr std::uint32_t kNewLocationName = 56;
    constexpr std::uint32_t kSweepName = 64;
    constexpr std::uint32_t kSweepHitResultName = 68;
    constexpr std::uint32_t kTeleportName = 76;
    constexpr std::uint32_t kReturnValueName = 84;
    put_name(kFunctionName, "K2_SetActorLocation");
    put_name(kFunctionClassName, "Function");
    put_name(kActorClassName, "Actor");
    put_name(kVectorName, "Vector");
    put_name(kHitResultName, "HitResult");
    put_name(kNewLocationName, "NewLocation");
    put_name(kSweepName, "bSweep");
    put_name(kSweepHitResultName, "SweepHitResult");
    put_name(kTeleportName, "bTeleport");
    put_name(kReturnValueName, "ReturnValue");

    memory->Put(kTeleportFunction + 0x08, kFunctionName);
    memory->Put(kTeleportFunction + 0x10, kTeleportFunctionClass);
    memory->Put(kTeleportFunction + 0x20, kTeleportActorClass);
    memory->Put(kTeleportFunction + 0x30, kTeleportNewLocationProperty);
    const std::uint8_t kTeleportParameterCount = 5;
    const std::uint16_t kTeleportParametersSize = 96;
    const std::uint16_t kTeleportReturnValueOffset = 90;
    memory->Put(kTeleportFunction + 0x40, kTeleportParameterCount);
    memory->Put(kTeleportFunction + 0x42, kTeleportParametersSize);
    memory->Put(kTeleportFunction + 0x44, kTeleportReturnValueOffset);
    memory->Put(kTeleportFunctionClass + 0x08, kFunctionClassName);
    memory->Put(kTeleportActorClass + 0x08, kActorClassName);
    memory->Put(kTeleportVectorStruct + 0x08, kVectorName);
    memory->Put(kTeleportHitResultStruct + 0x08, kHitResultName);

    const auto put_property = [&](const std::uintptr_t property,
                                  const std::uint32_t name,
                                  const std::int32_t element_size,
                                  const std::int32_t offset,
                                  const std::uintptr_t next) {
        const std::int32_t array_dim = 1;
        memory->Put(property + 0x08, name);
        memory->Put(property + 0x10, array_dim);
        memory->Put(property + 0x14, element_size);
        memory->Put(property + 0x18, offset);
        memory->Put(property + 0x20, next);
    };
    put_property(kTeleportNewLocationProperty, kNewLocationName, 24, 0, kTeleportSweepProperty);
    memory->Put(kTeleportNewLocationProperty + 0x28, kTeleportVectorStruct);
    put_property(kTeleportSweepProperty, kSweepName, 1, 88, kTeleportSweepHitResultProperty);
    put_property(kTeleportSweepHitResultProperty, kSweepHitResultName, 64, 24, kTeleportFlagProperty);
    memory->Put(kTeleportSweepHitResultProperty + 0x28, kTeleportHitResultStruct);
    put_property(kTeleportFlagProperty, kTeleportName, 1, 89, kTeleportReturnProperty);
    put_property(kTeleportReturnProperty, kReturnValueName, 1, 90, 0);
    const auto put_bool = [&](const std::uintptr_t property,
                              const std::uint8_t byte_mask,
                              const std::uint8_t field_mask) {
        const std::uint8_t field_size = 1;
        const std::uint8_t byte_offset = 0;
        memory->Put(property + 0x28, field_size);
        memory->Put(property + 0x29, byte_offset);
        memory->Put(property + 0x2A, byte_mask);
        memory->Put(property + 0x2B, field_mask);
    };
    put_bool(kTeleportSweepProperty, 0x01, 0x01);
    put_bool(kTeleportFlagProperty, 0x01, 0x01);
    put_bool(kTeleportReturnProperty, 0x01, 0xFF);
}

bool SemanticNteServicesAreHardGated(
    anomaly::BuildFingerprint fingerprint,
    anomaly::BuildProfile profile,
    anomaly::ProfileResolutionSnapshot resolution) {
    auto memory = std::make_shared<FixtureMemory>();
    Populate(memory);
    anomaly::AdapterServiceRegistry registry;
    anomaly::Ue5NteAdapter adapter(
        std::move(fingerprint), std::move(profile), std::move(resolution), memory, registry);
    const bool started = adapter.Start(true);
    const auto* nte_build = static_cast<const AnomalyNteBuildServiceV1*>(
        registry.Query(ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION));
    const bool semantic_services_are_absent =
        registry.Query(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID,
            ANOMALY_NTE_SESSION_SERVICE_V1_VERSION) == nullptr &&
        registry.Query(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION) == nullptr &&
        registry.Query(
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION) == nullptr &&
        registry.Query(
            ANOMALY_NTE_ENTITIES_SERVICE_V1_ID,
            ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION) == nullptr &&
        registry.Query(
            ANOMALY_NTE_METRICS_SERVICE_V1_ID,
            ANOMALY_NTE_METRICS_SERVICE_V1_VERSION) == nullptr;
    const bool metrics_feature_is_unavailable =
        nte_build != nullptr && nte_build->feature_state != nullptr &&
        nte_build->feature_state(nte_build->user, kNteMetricsFeature) ==
            ANOMALY_FEATURE_V1_UNAVAILABLE;
    const auto feature_is_unavailable = [nte_build](std::string_view id) {
        return nte_build != nullptr && nte_build->feature_state != nullptr &&
            nte_build->feature_state(nte_build->user, {id.data(), id.size()}) ==
                ANOMALY_FEATURE_V1_UNAVAILABLE;
    };
    const bool semantic_features_are_unavailable =
        feature_is_unavailable("nte.session") &&
        feature_is_unavailable("nte.player") &&
        feature_is_unavailable("nte.player-esp") &&
        feature_is_unavailable("nte.player-teleport") &&
        feature_is_unavailable("nte.entities");
    const bool stopped = adapter.Stop();
    return started && semantic_services_are_absent && metrics_feature_is_unavailable &&
        semantic_features_are_unavailable && stopped &&
        registry.Snapshot().empty();
}

}  // namespace

int main() {
    struct UndersizedServiceTable {
        std::uint32_t struct_size;
        std::uint32_t service_version;
    };
    UndersizedServiceTable undersized_service{
        sizeof(undersized_service), ANOMALY_NTE_SESSION_SERVICE_V1_VERSION};
    anomaly::AdapterServiceRegistry undersized_service_registry;
    const bool accepted_undersized_service = undersized_service_registry.Publish(
        "anomaly.fixture.undersized-session",
        ANOMALY_NTE_SESSION_SERVICE_V1_VERSION,
        &undersized_service);
    bool result = Check(
        !accepted_undersized_service && undersized_service_registry.Snapshot().empty() &&
            undersized_service_registry.Query("anomaly.fixture.undersized-session", 1) == nullptr,
        "adapter service registry accepted an undersized callable table");
    AnomalyNteSessionServiceV1 mismatched_session_service{
        sizeof(mismatched_session_service), ANOMALY_NTE_SESSION_SERVICE_V1_VERSION, nullptr,
        nullptr};
    anomaly::AdapterServiceRegistry mismatched_service_registry;
    const bool accepted_mismatched_service = mismatched_service_registry.Publish(
        "anomaly.fixture.mismatched-session",
        ANOMALY_NTE_SESSION_SERVICE_V1_VERSION,
        &mismatched_session_service);
    result = Check(
        !accepted_mismatched_service && mismatched_service_registry.Snapshot().empty() &&
            mismatched_service_registry.Query("anomaly.fixture.mismatched-session", 1) == nullptr,
        "adapter service registry accepted a mismatched table header");

    anomaly::AdapterServiceRegistry registry;
    auto memory = std::make_shared<FixtureMemory>();
    Populate(memory);
    std::atomic_uint32_t process_event_calls{};
    std::atomic_bool process_event_parameters_valid{};
    const anomaly::Ue5NteAdapter::ProcessEventInvoker process_event_invoker =
        [memory, &process_event_calls, &process_event_parameters_valid](
            const std::uintptr_t object,
            const std::uintptr_t function,
            void* parameters,
            const std::size_t parameter_size) {
            ++process_event_calls;
            if (object != FixtureMemory::kBase + 0x1500 ||
                function != kTeleportFunction || parameters == nullptr ||
                parameter_size != 96) {
                return false;
            }
            auto* bytes = static_cast<std::uint8_t*>(parameters);
            std::array<double, 3> target{};
            std::memcpy(target.data(), bytes, sizeof(target));
            bool hit_result_is_zero = true;
            for (std::size_t index = 24; index != 88; ++index) {
                hit_result_is_zero = hit_result_is_zero && bytes[index] == 0;
            }
            process_event_parameters_valid.store(
                bytes[88] == 0 && (bytes[89] & 0x01) != 0 && hit_result_is_zero,
                std::memory_order_release);
            // Return a native bool byte whose truth is expressed by FieldMask,
            // not by the bit mask used for a packed bool property.
            bytes[90] = 0x80;
            memory->PutBytes(kPlayerSceneLocation, target.data(), sizeof(target));
            return true;
        };
    anomaly::Ue5NteAdapter adapter(
        Fingerprint(), SemanticProfile(), Resolution(), memory, registry, {},
        FixtureFeatureLayoutValidators(),
        process_event_invoker);
    result = Check(adapter.Start(true), "adapter did not start") &&
        Check(registry.Snapshot().size() == 12, "adapter did not publish feature-gated services");

    std::atomic_bool callback_on_game_thread{};
    adapter.SetTickCallback([&](double delta) {
        callback_on_game_thread = delta == 0.25 && GetCurrentThreadId() == adapter.GameThreadId();
    });
    adapter.OnGameTick(0.25);
    result = Check(callback_on_game_thread && adapter.TickSequence() == 1,
                   "framework tick did not bind/call on the game thread") && result;
    const DWORD initial_game_thread_id = adapter.GameThreadId();
    // The exact-Profile validators sample once during initial revalidation. Later
    // service queries must only set demand; the next Game tick performs the sample.
    const auto player_reads_after_initialization = memory->PlayerChainReads();
    const auto entity_reads_after_initialization = memory->EntityChainReads();

    const auto* framework = static_cast<const AnomalyUe5FrameworkServiceV1*>(
        registry.Query(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1));
    result = Check(framework && framework->is_game_thread(framework->user) == 1 &&
                       framework->tick_sequence(framework->user) == 1,
                   "framework service thread contract failed") && result;

    const auto* world = static_cast<const AnomalyUe5WorldServiceV1*>(
        registry.Query(ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1));
    AnomalyGenerationHandleV1 world_handle{};
    AnomalyUe5WorldSnapshotV1 world_snapshot{sizeof(world_snapshot)};
    result = Check(world && world->current(world->user, &world_handle).code == ANOMALY_STATUS_V1_OK &&
                       world->snapshot(world->user, world_handle, &world_snapshot).code == ANOMALY_STATUS_V1_OK &&
                       world_snapshot.name_id == 2 && world_snapshot.change_sequence == 1,
                   "world generation snapshot failed") && result;

    const auto* names = static_cast<const AnomalyUe5NamesServiceV1*>(
        registry.Query(ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1));
    std::size_t name_size{};
    result = Check(names && names->resolve_utf8(names->user, 2, nullptr, &name_size).code == ANOMALY_STATUS_V1_OK &&
                       name_size == 6,
                   "name service size query failed") && result;
    std::array<char, 16> name{};
    result = Check(names->resolve_utf8(names->user, 2, name.data(), &name_size).code == ANOMALY_STATUS_V1_OK &&
                       std::string_view(name.data()) == "World",
                   "name service decode failed") && result;

    const auto* objects = static_cast<const AnomalyUe5ObjectsServiceV1*>(
        registry.Query(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1));
    AnomalyUe5ObjectSnapshotV1 object{sizeof(object)};
    AnomalyUe5ObjectSnapshotV1 teleport_object{sizeof(teleport_object)};
    AnomalyUe5ObjectSnapshotV1 last_object{sizeof(last_object)};
    AnomalyUe5ObjectSnapshotV1 object_by_handle{sizeof(object_by_handle)};
    result = Check(objects && objects->struct_size >= sizeof(*objects) &&
                       objects->snapshot_by_handle != nullptr &&
                       objects->count(objects->user) == 3 &&
                       objects->snapshot_at(objects->user, 0, &object).code == ANOMALY_STATUS_V1_OK &&
                       ANOMALY_UE5_OBJECT_HANDLE_INDEX(object.handle) == 0 &&
                        ANOMALY_UE5_OBJECT_HANDLE_SERIAL(object.handle) == 101 &&
                        object.handle.generation == 1 && object.name_id == 2 &&
                        objects->snapshot_at(objects->user, 1, &teleport_object).code ==
                            ANOMALY_STATUS_V1_OK &&
                        ANOMALY_UE5_OBJECT_HANDLE_SERIAL(teleport_object.handle) == 202 &&
                        teleport_object.name_id == 20 &&
                        objects->snapshot_at(objects->user, 2, &last_object).code ==
                           ANOMALY_STATUS_V1_OK &&
                       ANOMALY_UE5_OBJECT_HANDLE_INDEX(last_object.handle) == 2 &&
                       ANOMALY_UE5_OBJECT_HANDLE_SERIAL(last_object.handle) == 303 &&
                       last_object.name_id == 16 &&
                       objects->snapshot_by_handle(
                           objects->user, object.handle, &object_by_handle).code ==
                           ANOMALY_STATUS_V1_OK &&
                       object_by_handle.handle.id == object.handle.id,
                   "chunked object generation snapshot failed") && result;
    const auto outside_status = objects->snapshot_at(objects->user, 3, &teleport_object);
    result = Check(outside_status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                       HasMessage(outside_status, "object index is not found"),
                   "object index bounds did not return a typed result") && result;

    const auto* session = static_cast<const AnomalyNteSessionServiceV1*>(
        registry.Query(ANOMALY_NTE_SESSION_SERVICE_V1_ID, 1));
    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    const auto* player = static_cast<const AnomalyNtePlayerServiceV1*>(
        registry.Query(ANOMALY_NTE_PLAYER_SERVICE_V1_ID, 1));
    const auto* player_teleport = static_cast<const AnomalyNtePlayerTeleportServiceV1*>(
        registry.Query(
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION));
    const auto* entities = static_cast<const AnomalyNteEntitiesServiceV1*>(
        registry.Query(ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, 1));
    const auto* actors = static_cast<const AnomalyNteActorsServiceV1*>(
        registry.Query(ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION));
    const auto* metrics = static_cast<const AnomalyNteMetricsServiceV1*>(
        registry.Query(ANOMALY_NTE_METRICS_SERVICE_V1_ID, ANOMALY_NTE_METRICS_SERVICE_V1_VERSION));
    const auto* nte_build = static_cast<const AnomalyNteBuildServiceV1*>(
        registry.Query(ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION));
    result = Check(
                 nte_build != nullptr && nte_build->feature_state != nullptr &&
                     nte_build->feature_state(
                         nte_build->user,
                         {"ue5.process-event", sizeof("ue5.process-event") - 1U}) ==
                         ANOMALY_FEATURE_V1_AVAILABLE,
                 "validated ProcessEvent framework feature is unavailable") && result;
    result = Check(
                 nte_build != nullptr && nte_build->feature_state != nullptr &&
                     nte_build->feature_state(
                         nte_build->user,
                         {"nte.player-teleport", sizeof("nte.player-teleport") - 1U}) ==
                         ANOMALY_FEATURE_V1_AVAILABLE,
                 "ProcessEvent dependency did not activate player teleport") && result;
    AnomalyStatusV1 component_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    AnomalyStatusV1 bool_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    AnomalyStatusV1 fname_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    AnomalyStatusV1 actor_frame_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    AnomalyStatusV1 actor_snapshot_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    AnomalyStatusV1 actor_page_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    AnomalyNteEntityComponentBoundsV1 component_bounds{sizeof(component_bounds)};
    AnomalyNteEntityBoolPropertyV1 bool_property{sizeof(bool_property)};
    AnomalyNteEntityFrameV1 actor_callback_frame{sizeof(actor_callback_frame)};
    AnomalyNteEntitySnapshotV1 actor_callback_entity{sizeof(actor_callback_entity)};
    std::array<AnomalyNteEntitySnapshotV1, 1> actor_callback_page{};
    AnomalyNteEntityPageResultV1 actor_callback_page_result{sizeof(actor_callback_page_result)};
    std::array<char, 32> reflected_fname{};
    std::size_t reflected_fname_size = reflected_fname.size();
    adapter.SetTickCallback([&](double) {
        if (actors == nullptr) return;
        AnomalyNteEntityFrameV1 entity_demand_frame{sizeof(entity_demand_frame)};
        static_cast<void>(entities->frame(entities->user, &entity_demand_frame));
        actor_callback_frame = AnomalyNteEntityFrameV1{sizeof(actor_callback_frame)};
        actor_callback_entity = AnomalyNteEntitySnapshotV1{sizeof(actor_callback_entity)};
        actor_callback_page[0] = AnomalyNteEntitySnapshotV1{sizeof(actor_callback_page[0])};
        actor_callback_page_result =
            AnomalyNteEntityPageResultV1{sizeof(actor_callback_page_result)};
        actor_frame_status = actors->frame(actors->user, &actor_callback_frame);
        actor_snapshot_status = actors->snapshot_at(
            actors->user, actor_callback_frame.generation, 0, &actor_callback_entity);
        AnomalyNteEntityPageRequestV1 actor_page_request{
            sizeof(actor_page_request), 0, actor_callback_frame.generation, 0, 1,
            0, 0, 0, 0, 0};
        actor_page_status = actors->page(
            actors->user, &actor_page_request, actor_callback_page.data(),
            &actor_callback_page_result);
        if (actor_frame_status.code != ANOMALY_STATUS_V1_OK ||
            actor_snapshot_status.code != ANOMALY_STATUS_V1_OK ||
            actor_page_status.code != ANOMALY_STATUS_V1_OK) {
            return;
        }
        component_status = actors->component_bounds(
            actors->user, actor_callback_entity.handle,
            AnomalyStringViewV1{"Box", 3}, &component_bounds);
        bool_status = actors->bool_property(
            actors->user, actor_callback_entity.handle,
            AnomalyStringViewV1{"CanOpen", 7}, &bool_property);
        fname_status = actors->fname_property_utf8(
            actors->user, actor_callback_entity.handle,
            AnomalyStringViewV1{"Extract_ID", 10}, reflected_fname.data(),
            &reflected_fname_size);
    });
    const auto player_reads_before_demand_tick = memory->PlayerChainReads();
    const auto entity_reads_before_demand_tick = memory->EntityChainReads();
    adapter.OnGameTick(0.5);
    result = Check(
                 player_reads_before_demand_tick == player_reads_after_initialization &&
                     entity_reads_before_demand_tick == entity_reads_after_initialization &&
                     memory->PlayerChainReads() == player_reads_before_demand_tick + 1 &&
                     memory->EntityChainReads() == entity_reads_before_demand_tick + 2,
                 "service query sampled before the next Game tick") && result;
    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    AnomalyNtePlayerEspSnapshotV1 player_esp_snapshot{sizeof(player_esp_snapshot)};
    result = Check(session && session->snapshot(session->user, &session_snapshot).code == ANOMALY_STATUS_V1_OK &&
                       session_snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY &&
                       player && player->snapshot(player->user, &player_snapshot).code == ANOMALY_STATUS_V1_OK &&
                       player_snapshot.sequence == 2 &&
                       (player_snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
                       (player_snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0 &&
                       player_snapshot.position[0] == 12.5 && player_snapshot.position[1] == -7.25 &&
                       player_snapshot.position[2] == 99.0 &&
                       player->struct_size >= sizeof(AnomalyNtePlayerServiceV1) &&
                       player->esp_snapshot != nullptr &&
                       player->esp_snapshot(player->user, &player_esp_snapshot).code ==
                           ANOMALY_STATUS_V1_OK &&
                       player_esp_snapshot.bounds_center[0] == 12.5 &&
                       player_esp_snapshot.bounds_extent[2] == 88.0 &&
                       player_esp_snapshot.camera_position[0] == -300.0 &&
                       player_esp_snapshot.camera_rotation[0] == -5.0 &&
                       player_esp_snapshot.horizontal_fov_degrees == 80.0F,
                   "NTE session/player snapshot failed") && result;

    const std::array<double, 3> teleport_position{400.5, -300.25, 99.75};
    AnomalyNtePlayerTeleportRequestV1 teleport_request{
        sizeof(teleport_request), 0, session_snapshot.world, player_snapshot.handle,
        {teleport_position[0], teleport_position[1], teleport_position[2]}};
    const auto invalid_teleport = [&] {
        auto request = teleport_request;
        request.position[1] = std::numeric_limits<double>::quiet_NaN();
        return player_teleport == nullptr
            ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
            : player_teleport->teleport(player_teleport->user, &request);
    }();
    const auto stale_world_teleport = [&] {
        auto request = teleport_request;
        ++request.world.generation;
        return player_teleport == nullptr
            ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
            : player_teleport->teleport(player_teleport->user, &request);
    }();
    std::atomic<std::uint32_t> wrong_thread_teleport{};
    std::thread wrong_thread_control([&] {
        wrong_thread_teleport.store(
            player_teleport == nullptr
                ? ANOMALY_STATUS_V1_UNAVAILABLE
                : player_teleport->teleport(
                       player_teleport->user, &teleport_request).code,
            std::memory_order_release);
    });
    wrong_thread_control.join();
    const auto teleport_status = player_teleport == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : player_teleport->teleport(player_teleport->user, &teleport_request);
    std::array<double, 3> teleported_location{};
    const bool teleported_location_read = memory->Read(
        kPlayerSceneLocation, teleported_location.data(), sizeof(teleported_location));
    result = Check(
                  player_teleport != nullptr &&
                      player_teleport->struct_size >= sizeof(*player_teleport) &&
                      player_teleport->teleport != nullptr &&
                      invalid_teleport.code == ANOMALY_STATUS_V1_INVALID_ARGUMENT &&
                      stale_world_teleport.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                      wrong_thread_teleport.load(std::memory_order_acquire) ==
                          ANOMALY_STATUS_V1_CONFLICT &&
                      teleport_status.code == ANOMALY_STATUS_V1_OK &&
                      process_event_calls.load(std::memory_order_acquire) == 1 &&
                      process_event_parameters_valid.load(std::memory_order_acquire) &&
                      teleported_location_read &&
                      teleported_location[0] == teleport_position[0] &&
                      teleported_location[1] == teleport_position[1] &&
                      teleported_location[2] == teleport_position[2],
                  "teleport did not validate and invoke ProcessEvent") && result;

    AnomalyNteSessionEventV1 first_session_event{sizeof(first_session_event)};
    AnomalyNteCameraSnapshotV1 camera_snapshot{sizeof(camera_snapshot)};
    result = Check(
                 session != nullptr && session->service_version ==
                         ANOMALY_NTE_SESSION_SERVICE_V1_VERSION &&
                     session->struct_size >= sizeof(AnomalyNteSessionServiceV1) &&
                     session->next_event != nullptr &&
                     session->latest_event_sequence != nullptr &&
                     session->next_event(
                         session->user, 0, &first_session_event).code ==
                         ANOMALY_STATUS_V1_OK &&
                     first_session_event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY &&
                     first_session_event.sequence == 1 && first_session_event.tick_sequence == 1 &&
                     first_session_event.previous_world.id == 0 &&
                     first_session_event.world.id == 1 &&
                     first_session_event.world.generation == session_snapshot.world.generation &&
                     session->latest_event_sequence(session->user) ==
                         first_session_event.sequence &&
                     player != nullptr && player->service_version ==
                         ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION &&
                     player->struct_size >= sizeof(AnomalyNtePlayerServiceV1) &&
                     player->camera_snapshot != nullptr &&
                     player->camera_snapshot(player->user, &camera_snapshot).code ==
                         ANOMALY_STATUS_V1_OK &&
                      camera_snapshot.world.id == 1 &&
                      camera_snapshot.world.generation == session_snapshot.world.generation &&
                      camera_snapshot.player.id == player_snapshot.handle.id &&
                      camera_snapshot.player.generation == player_snapshot.handle.generation &&
                      camera_snapshot.sequence == player_snapshot.sequence &&
                     camera_snapshot.position[0] == -300.0 &&
                     camera_snapshot.rotation[0] == -5.0 &&
                     camera_snapshot.horizontal_fov_degrees == 80.0F,
                 "NTE lifecycle and camera snapshots failed") && result;

    AnomalyNteEntityFrameV1 entity_frame{sizeof(entity_frame)};
    AnomalyNteEntitySnapshotV1 entity_snapshot{sizeof(entity_snapshot)};
    result = Check(
        entities && entities->frame(entities->user, &entity_frame).code == ANOMALY_STATUS_V1_OK &&
            entity_frame.entity_count == 1 && entity_frame.generation == 1 &&
            entity_frame.sequence == 2 &&
            (entity_frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
            (entity_frame.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0 &&
            entity_frame.camera_position[0] == -300.0 &&
            entities->snapshot_at(
                entities->user, entity_frame.generation, 0, &entity_snapshot).code ==
                ANOMALY_STATUS_V1_OK &&
            entity_snapshot.entity_id == 12 && entity_snapshot.class_id == 23 &&
            entity_snapshot.entity_name_id == 16 && entity_snapshot.class_name_id == 8 &&
            (entity_snapshot.flags & ANOMALY_NTE_ENTITY_V1_MOVABLE) != 0 &&
            (entity_snapshot.flags & ANOMALY_NTE_ENTITY_V1_LOCAL_PLAYER) != 0 &&
            (entity_snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
            entity_snapshot.bounds_center[0] == 12.5 &&
            entity_snapshot.bounds_extent[2] == 88.0,
        "NTE entity frame snapshot failed") && result;

    std::array<char, 32> entity_class_name{};
    std::array<char, 32> entity_name{};
    std::size_t entity_class_name_size = entity_class_name.size();
    std::size_t entity_name_size = entity_name.size();
    AnomalyStatusV1 entity_class_name_status{};
    AnomalyStatusV1 entity_name_status{};
    memory->ResetReadCount();
    std::thread render_name_lookup([&] {
        entity_class_name_status = entities->class_name_utf8(
            entities->user, entity_snapshot.class_id, entity_class_name.data(),
            &entity_class_name_size);
        entity_name_status = entities->entity_name_utf8(
            entities->user, entity_snapshot.entity_id, entity_name.data(),
            &entity_name_size);
    });
    render_name_lookup.join();
    const auto render_name_lookup_reads = memory->ReadCount();
    result = Check(
        entities->struct_size >= sizeof(AnomalyNteEntitiesServiceV1) &&
            entities->class_name_utf8 != nullptr && entities->entity_name_utf8 != nullptr &&
            entity_class_name_status.code == ANOMALY_STATUS_V1_OK &&
            entity_name_status.code == ANOMALY_STATUS_V1_OK &&
            std::string_view(entity_class_name.data()) == "PawnClass" &&
            std::string_view(entity_name.data()) == "Pawn_1" &&
            render_name_lookup_reads == 0,
        "NTE entity string lookup was not served from the immutable frame") && result;

    std::array<AnomalyNteEntitySnapshotV1, 1> entity_page{};
    entity_page[0].struct_size = sizeof(entity_page[0]);
    AnomalyNteEntityPageRequestV1 page_request{
        sizeof(page_request), 0, 0, 0, 1, entity_snapshot.class_id,
        entity_snapshot.class_name_id, entity_snapshot.entity_name_id,
        ANOMALY_NTE_ENTITY_V1_MOVABLE, ANOMALY_NTE_ENTITY_V1_STATIC};
    AnomalyNteEntityPageResultV1 page_result{sizeof(page_result)};
    const auto player_reads_before_page = memory->PlayerChainReads();
    const auto entity_reads_before_page = memory->EntityChainReads();
    const auto page_status = entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : entities->page(entities->user, &page_request, entity_page.data(), &page_result);
    const auto first_page_generation = page_result.generation;
    AnomalyNteEntityPageRequestV1 continuation_request = page_request;
    continuation_request.generation = page_result.generation;
    continuation_request.offset = page_result.next_offset;
    AnomalyNteEntityPageResultV1 continuation_result{sizeof(continuation_result)};
    const auto continuation_status = entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : entities->page(
              entities->user, &continuation_request, entity_page.data(), &continuation_result);
    AnomalyNteEntityPageRequestV1 filtered_out_request = page_request;
    filtered_out_request.capacity = 0;
    filtered_out_request.class_name_id += 1;
    AnomalyNteEntityPageResultV1 filtered_out_result{sizeof(filtered_out_result)};
    const auto filtered_out_status = entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : entities->page(entities->user, &filtered_out_request, nullptr, &filtered_out_result);
    AnomalyNteEntityPageRequestV1 clamped_page_request = page_request;
    clamped_page_request.offset = 2;
    AnomalyNteEntityPageResultV1 clamped_page_result{sizeof(clamped_page_result)};
    const auto clamped_page_status = entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : entities->page(
              entities->user, &clamped_page_request, entity_page.data(), &clamped_page_result);
    AnomalyNteEntityPageRequestV1 too_large_request = page_request;
    too_large_request.capacity = 257;
    AnomalyNteEntityPageResultV1 too_large_result{sizeof(too_large_result)};
    const auto too_large_status = entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : entities->page(entities->user, &too_large_request, nullptr, &too_large_result);
    std::array<AnomalyNteEntitySnapshotV1, 2> invalid_page{};
    std::memset(invalid_page.data(), 0xA5, sizeof(invalid_page));
    invalid_page[0].struct_size = sizeof(invalid_page[0]);
    invalid_page[1].struct_size = 0;
    const auto invalid_page_before = invalid_page;
    AnomalyNteEntityPageResultV1 invalid_page_result{};
    std::memset(&invalid_page_result, 0x5A, sizeof(invalid_page_result));
    invalid_page_result.struct_size = sizeof(invalid_page_result);
    const auto invalid_page_result_before = invalid_page_result;
    AnomalyNteEntityPageRequestV1 invalid_destination_request = page_request;
    invalid_destination_request.capacity = static_cast<std::uint32_t>(invalid_page.size());
    const auto invalid_destination_status = entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : entities->page(
              entities->user, &invalid_destination_request, invalid_page.data(), &invalid_page_result);
    AnomalyNteSnapshotMetricsV1 initial_metrics{sizeof(initial_metrics)};
    result = Check(
                 entities != nullptr && entities->service_version ==
                          ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION &&
                      entities->struct_size >= sizeof(AnomalyNteEntitiesServiceV1) &&
                      actors != nullptr && actors->service_version ==
                          ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION &&
                      actors->struct_size >= sizeof(AnomalyNteActorsServiceV1) &&
                      actors->component_bounds != nullptr &&
                      actors->bool_property != nullptr &&
                      actors->fname_property_utf8 != nullptr &&
                      component_status.code == ANOMALY_STATUS_V1_OK &&
                      component_bounds.entity.id != 0 &&
                      component_bounds.bounds_center[0] == 12.5 &&
                      component_bounds.bounds_center[1] == -7.25 &&
                      component_bounds.bounds_center[2] == 99.0 &&
                      bool_status.code == ANOMALY_STATUS_V1_OK && bool_property.value == 1 &&
                      fname_status.code == ANOMALY_STATUS_V1_OK &&
                      std::string_view(reflected_fname.data()) == "World" &&
                     entities->page != nullptr && page_status.code == ANOMALY_STATUS_V1_OK &&
                     page_result.generation == entity_frame.generation &&
                     page_result.sequence == entity_frame.sequence && page_result.total_matches == 1 &&
                     page_result.returned == 1 && page_result.next_offset == 1 &&
                     entity_page[0].entity_id == entity_snapshot.entity_id &&
                     entity_page[0].class_id == entity_snapshot.class_id &&
                     continuation_status.code == ANOMALY_STATUS_V1_OK &&
                     continuation_result.generation == page_result.generation &&
                     continuation_result.total_matches == 1 && continuation_result.returned == 0 &&
                     continuation_result.next_offset == 1 &&
                     filtered_out_status.code == ANOMALY_STATUS_V1_OK &&
                     filtered_out_result.total_matches == 0 && filtered_out_result.returned == 0 &&
                     filtered_out_result.next_offset == 0 &&
                     clamped_page_status.code == ANOMALY_STATUS_V1_OK &&
                     clamped_page_result.total_matches == 1 && clamped_page_result.returned == 0 &&
                     clamped_page_result.next_offset == clamped_page_result.total_matches &&
                      too_large_status.code == ANOMALY_STATUS_V1_INVALID_ARGUMENT &&
                      invalid_destination_status.code == ANOMALY_STATUS_V1_INVALID_ARGUMENT &&
                      std::memcmp(
                          invalid_page.data(), invalid_page_before.data(), sizeof(invalid_page)) == 0 &&
                      std::memcmp(
                          &invalid_page_result, &invalid_page_result_before,
                          sizeof(invalid_page_result)) == 0 &&
                      memory->PlayerChainReads() == player_reads_before_page &&
                      memory->EntityChainReads() == entity_reads_before_page &&
                      metrics != nullptr && metrics->snapshot != nullptr &&
                      metrics->snapshot(metrics->user, &initial_metrics).code == ANOMALY_STATUS_V1_OK &&
                      initial_metrics.flags == ANOMALY_NTE_METRICS_V1_VALID &&
                      nte_build != nullptr && nte_build->feature_state != nullptr &&
                      nte_build->feature_state(nte_build->user, kNteMetricsFeature) ==
                          ANOMALY_FEATURE_V1_AVAILABLE &&
                      initial_metrics.tick_sequence == adapter.TickSequence() &&
                     initial_metrics.snapshot_tick_count >= 2 &&
                     initial_metrics.player_refresh_count >= 1 &&
                     initial_metrics.entity_refresh_count >= 1 &&
                     initial_metrics.entity_page_request_count >= 2 &&
                     initial_metrics.entity_page_request_count ==
                         initial_metrics.entity_page_cache_hit_count,
                 "NTE entity pages or snapshot metrics failed") && result;

    const auto first_handle = object.handle;
    const std::uint32_t replacement_serial = 102;
    memory->Put(kObjectChunk0 + 0x10, replacement_serial);
    adapter.OnGameTick(0.5);
    result = Check(
                 actor_frame_status.code == ANOMALY_STATUS_V1_OK &&
                     actor_snapshot_status.code == ANOMALY_STATUS_V1_OK &&
                     actor_page_status.code == ANOMALY_STATUS_V1_OK &&
                     (actor_callback_frame.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
                     (actor_callback_frame.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0 &&
                     (actor_callback_entity.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
                     (actor_callback_entity.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0 &&
                     (actor_callback_page_result.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0 &&
                     (actor_callback_page[0].flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0 &&
                     component_status.code == ANOMALY_STATUS_V1_OK &&
                     component_bounds.sequence == adapter.TickSequence() &&
                     (component_bounds.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0 &&
                     bool_status.code == ANOMALY_STATUS_V1_OK &&
                     bool_property.sequence == adapter.TickSequence() &&
                     (bool_property.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0,
                 "cached actor topology became stale on a later tick") && result;
    AnomalyNteEntitySnapshotV1 stale_entity_snapshot{sizeof(stale_entity_snapshot)};
    const auto stale_entity_frame = entities->snapshot_at(
        entities->user, entity_frame.generation, 0, &stale_entity_snapshot);
    AnomalyNteEntityPageRequestV1 stale_page_request{
        sizeof(stale_page_request), 0, first_page_generation, 0, 1, 0, 0, 0, 0, 0};
    std::array<AnomalyNteEntitySnapshotV1, 1> stale_page{};
    std::memset(stale_page.data(), 0xC3, sizeof(stale_page));
    stale_page[0].struct_size = sizeof(stale_page[0]);
    const auto stale_page_before = stale_page;
    AnomalyNteEntityPageResultV1 stale_page_result{};
    std::memset(&stale_page_result, 0x3C, sizeof(stale_page_result));
    stale_page_result.struct_size = sizeof(stale_page_result);
    const auto stale_page_result_before = stale_page_result;
    const auto stale_entity_page = entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : entities->page(
              entities->user, &stale_page_request, stale_page.data(), &stale_page_result);
    AnomalyUe5ObjectSnapshotV1 stale_serial_snapshot{sizeof(stale_serial_snapshot)};
    const auto stale_serial = objects->snapshot_by_handle(
        objects->user, first_handle, &stale_serial_snapshot);
    AnomalyUe5ObjectSnapshotV1 replacement_object{sizeof(replacement_object)};
    result = Check(stale_serial.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                       HasMessage(stale_serial, "stale object serial") &&
                       objects->snapshot_at(
                           objects->user, 0, &replacement_object).code == ANOMALY_STATUS_V1_OK &&
                        replacement_object.handle.generation == first_handle.generation &&
                        ANOMALY_UE5_OBJECT_HANDLE_SERIAL(replacement_object.handle) ==
                            replacement_serial &&
                        stale_entity_frame.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                        stale_entity_page.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                        std::memcmp(stale_page.data(), stale_page_before.data(), sizeof(stale_page)) == 0 &&
                        std::memcmp(
                            &stale_page_result, &stale_page_result_before,
                            sizeof(stale_page_result)) == 0,
                    "reused object slot did not invalidate its old serial") && result;

    memory->Put(kObjectPageTableReplacement, kObjectChunk0);
    memory->Put(
        kObjectPageTableReplacement + sizeof(std::uintptr_t), kObjectChunk1);
    memory->Put(kObjectRegistry + 0x10, kObjectPageTableReplacement);
    adapter.OnGameTick(0.5);
    const auto stale_registry = objects->snapshot_by_handle(
        objects->user, replacement_object.handle, &stale_serial_snapshot);
    result = Check(stale_registry.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                       HasMessage(stale_registry, "stale object registry generation") &&
                       objects->generation(objects->user) > replacement_object.handle.generation,
                   "object page-table replacement did not invalidate its generation") && result;

    std::thread wrong_thread([&] { adapter.OnGameTick(1.0); });
    wrong_thread.join();
    result = Check(adapter.TickSequence() == 4 && adapter.RejectedThreadTicks() == 1,
                   "wrong-thread tick was accepted") && result;

    const auto player_identity_generation = player_snapshot.handle.generation;
    const std::uintptr_t missing_pawn{};
    memory->Put(FixtureMemory::kBase + 0x1440, missing_pawn);
    adapter.OnGameTick(0.5);
    AnomalyNtePlayerSnapshotV1 invalid_player{sizeof(invalid_player)};
    AnomalyNteEntityFrameV1 invalid_entities{sizeof(invalid_entities)};
    result = Check(
                 player->snapshot(player->user, &invalid_player).code ==
                         ANOMALY_STATUS_V1_UNAVAILABLE &&
                     entities->frame(entities->user, &invalid_entities).code ==
                         ANOMALY_STATUS_V1_OK &&
                     invalid_entities.entity_count != 0 &&
                     (invalid_entities.flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) != 0 &&
                     invalid_entities.horizontal_fov_degrees == 0.0F,
                 "broken player chain disabled independent entity enumeration") && result;
    const std::uintptr_t restored_pawn = FixtureMemory::kBase + 0x1500;
    memory->Put(FixtureMemory::kBase + 0x1440, restored_pawn);
    adapter.OnGameTick(0.5);
    AnomalyNtePlayerSnapshotV1 restored_player{sizeof(restored_player)};
    AnomalyNteCameraSnapshotV1 restored_camera{sizeof(restored_camera)};
    result = Check(
                   player->snapshot(player->user, &restored_player).code ==
                           ANOMALY_STATUS_V1_OK &&
                      restored_player.handle.generation > player_identity_generation &&
                      player != nullptr &&
                      player->camera_snapshot(player->user, &restored_camera).code ==
                          ANOMALY_STATUS_V1_OK &&
                      restored_camera.player.id == restored_player.handle.id &&
                      restored_camera.player.generation == restored_player.handle.generation &&
                      restored_camera.player.generation > camera_snapshot.player.generation,
                  "camera snapshot reused the invalid player identity") && result;

    // A replacement controller can retain the same Pawn while supplying a different camera.
    // Both snapshots must therefore advance their shared player identity generation.
    const std::uintptr_t replacement_controller = FixtureMemory::kBase + 0x1700;
    const std::uintptr_t replacement_camera_manager = FixtureMemory::kBase + 0x2100;
    const std::array<double, 3> replacement_camera_position{310.0, -17.0, 180.0};
    const std::array<double, 3> replacement_camera_rotation{12.0, 4.0, 0.0};
    const float replacement_camera_fov = 75.0F;
    memory->Put(replacement_controller + 0x40, restored_pawn);
    memory->Put(replacement_controller + 0x48, replacement_camera_manager);
    memory->PutBytes(
        replacement_camera_manager + 0x100, replacement_camera_position.data(),
        sizeof(replacement_camera_position));
    memory->PutBytes(
        replacement_camera_manager + 0x118, replacement_camera_rotation.data(),
        sizeof(replacement_camera_rotation));
    memory->Put(replacement_camera_manager + 0x130, replacement_camera_fov);
    memory->Put(FixtureMemory::kBase + 0x1330, replacement_controller);
    adapter.OnGameTick(0.5);
    AnomalyNtePlayerSnapshotV1 replacement_controller_player{
        sizeof(replacement_controller_player)};
    AnomalyNteCameraSnapshotV1 replacement_controller_camera{
        sizeof(replacement_controller_camera)};
    result = Check(
                 player != nullptr &&
                     player->snapshot(
                         player->user, &replacement_controller_player).code ==
                         ANOMALY_STATUS_V1_OK &&
                     replacement_controller_player.handle.id == restored_player.handle.id &&
                     replacement_controller_player.handle.generation >
                         restored_player.handle.generation &&
                     player->camera_snapshot(
                         player->user, &replacement_controller_camera).code ==
                         ANOMALY_STATUS_V1_OK &&
                     replacement_controller_camera.player.id ==
                         replacement_controller_player.handle.id &&
                     replacement_controller_camera.player.generation ==
                         replacement_controller_player.handle.generation &&
                     replacement_controller_camera.player.generation >
                         restored_camera.player.generation &&
                     replacement_controller_camera.position[0] == replacement_camera_position[0] &&
                     replacement_controller_camera.rotation[0] == replacement_camera_rotation[0] &&
                     replacement_controller_camera.horizontal_fov_degrees == replacement_camera_fov,
                 "controller replacement reused the prior player/camera identity") && result;

    // Real persistent levels contain heterogeneous actors that are not valid
    // render candidates. Null roots, unusable bounds, unknown mobility values,
    // and missing optional identity metadata must not poison the complete frame.
    const auto base = FixtureMemory::kBase;
    const std::uintptr_t null_root_actor = base + 0x2700;
    const std::uintptr_t invalid_bounds_actor = base + 0x2800;
    const std::uintptr_t invalid_bounds_root = base + 0x2900;
    const std::uintptr_t unknown_mobility_actor = base + 0x2A00;
    const std::uintptr_t unknown_mobility_root = base + 0x2B00;
    const std::uintptr_t fallback_metadata_actor = base + 0x2C00;
    const std::uintptr_t fallback_metadata_root = base + 0x2D00;
    const std::uintptr_t pawn_class = base + 0x2600;
    const std::uintptr_t actor_array = base + 0x2500;
    const std::uintptr_t null_root{};
    memory->Put(null_root_actor + 0x50, null_root);
    memory->Put(invalid_bounds_actor + 0x50, invalid_bounds_root);
    memory->Put(invalid_bounds_actor + 0x10, pawn_class);
    memory->Put(unknown_mobility_actor + 0x50, unknown_mobility_root);
    memory->Put(unknown_mobility_actor + 0x10, pawn_class);
    memory->Put(fallback_metadata_actor + 0x50, fallback_metadata_root);
    const std::uintptr_t missing_class{};
    memory->Put(fallback_metadata_actor + 0x10, missing_class);
    const std::array<double, 3> candidate_center{100.0, 200.0, 300.0};
    const std::array<double, 3> candidate_extent{20.0, 30.0, 40.0};
    memory->PutBytes(
        unknown_mobility_root + 0x60, candidate_center.data(), sizeof(candidate_center));
    memory->PutBytes(
        unknown_mobility_root + 0x78, candidate_extent.data(), sizeof(candidate_extent));
    memory->PutBytes(
        fallback_metadata_root + 0x60, candidate_center.data(), sizeof(candidate_center));
    memory->PutBytes(
        fallback_metadata_root + 0x78, candidate_extent.data(), sizeof(candidate_extent));
    const std::uint8_t unknown_mobility = 9;
    const std::uint8_t movable_mobility = 2;
    memory->Put(unknown_mobility_root + 0x58, unknown_mobility);
    memory->Put(fallback_metadata_root + 0x58, movable_mobility);
    const std::int32_t unknown_entity_index = 40;
    memory->Put(unknown_mobility_actor + 0x0C, unknown_entity_index);
    memory->Put(actor_array + 0x08, null_root_actor);
    memory->Put(actor_array + 0x10, invalid_bounds_actor);
    memory->Put(actor_array + 0x18, unknown_mobility_actor);
    memory->Put(actor_array + 0x20, fallback_metadata_actor);
    const std::int32_t heterogeneous_actor_count = 5;
    memory->Put(base + 0x2428, heterogeneous_actor_count);
    memory->BlockRead(fallback_metadata_actor + 0x0C);
    adapter.OnGameTick(0.5);
    memory->ClearBlockedRead();
    AnomalyNteEntityFrameV1 heterogeneous_frame{sizeof(heterogeneous_frame)};
    AnomalyNteEntitySnapshotV1 unknown_mobility_snapshot{
        sizeof(unknown_mobility_snapshot)};
    AnomalyNteEntitySnapshotV1 fallback_metadata_snapshot{
        sizeof(fallback_metadata_snapshot)};
    result = Check(
                 entities->frame(entities->user, &heterogeneous_frame).code ==
                         ANOMALY_STATUS_V1_OK &&
                     heterogeneous_frame.entity_count == 3 &&
                     (heterogeneous_frame.flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) == 0 &&
                     entities->snapshot_at(
                         entities->user, heterogeneous_frame.generation, 1,
                         &unknown_mobility_snapshot).code == ANOMALY_STATUS_V1_OK &&
                     (unknown_mobility_snapshot.flags & ANOMALY_NTE_ENTITY_V1_STATIC) == 0 &&
                     (unknown_mobility_snapshot.flags & ANOMALY_NTE_ENTITY_V1_STATIONARY) == 0 &&
                     (unknown_mobility_snapshot.flags & ANOMALY_NTE_ENTITY_V1_MOVABLE) == 0 &&
                     entities->snapshot_at(
                         entities->user, heterogeneous_frame.generation, 2,
                         &fallback_metadata_snapshot).code == ANOMALY_STATUS_V1_OK &&
                     fallback_metadata_snapshot.entity_id == 5 &&
                     fallback_metadata_snapshot.class_id == 1 &&
                     (fallback_metadata_snapshot.flags & ANOMALY_NTE_ENTITY_V1_MOVABLE) != 0,
                 "normal non-renderable actors or metadata fallback marked the frame partial") &&
        result;

    const std::int32_t partial_actor_count = 2;
    memory->Put(FixtureMemory::kBase + 0x2428, partial_actor_count);
    memory->BlockRead(FixtureMemory::kBase + 0x2508);
    adapter.OnGameTick(0.5);
    memory->ClearBlockedRead();
    const std::int32_t complete_actor_count = 1;
    memory->Put(FixtureMemory::kBase + 0x2428, complete_actor_count);
    AnomalyNteEntityFrameV1 partial_frame{sizeof(partial_frame)};
    result = Check(
                 entities->frame(entities->user, &partial_frame).code ==
                         ANOMALY_STATUS_V1_OK &&
                     partial_frame.entity_count == 1 &&
                     (partial_frame.flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) != 0,
                 "unreadable non-empty actor slot did not mark the frame partial") && result;

    // A bounded stop revokes services and detaches the callback even when an
    // already-entered game tick does not finish before the deadline. The
    // second tick below proves that no new callback enters while the old
    // invocation is still being released.
    anomaly::AdapterServiceRegistry bounded_registry;
    anomaly::Ue5NteAdapter bounded_adapter(
        Fingerprint(), Profile(), Resolution(), memory, bounded_registry);
    result = Check(bounded_adapter.Start(true), "bounded adapter did not start") && result;
    std::atomic_bool callback_entered{};
    std::atomic_bool release_callback{};
    std::atomic_int callback_calls{};
    bounded_adapter.SetTickCallback([&](double) {
        callback_calls.fetch_add(1, std::memory_order_relaxed);
        callback_entered.store(true, std::memory_order_release);
        while (!release_callback.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    std::thread blocked_tick([&] {
        bounded_adapter.OnGameTick(0.5);
        bounded_adapter.OnGameTick(0.75);
    });
    const auto callback_entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!callback_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < callback_entry_deadline) {
        std::this_thread::yield();
    }
    result = Check(callback_entered.load(std::memory_order_acquire),
                   "bounded tick callback did not start") && result;
    result = Check(
                 !bounded_adapter.Stop(std::chrono::milliseconds(5)),
                 "bounded adapter stop did not report its in-flight tick") && result;
    result = Check(
                 !bounded_adapter.Started() && bounded_registry.Snapshot().empty(),
                 "bounded adapter stop did not revoke its published generation") && result;
    release_callback.store(true, std::memory_order_release);
    blocked_tick.join();
    result = Check(callback_calls.load(std::memory_order_relaxed) == 1,
                   "stopped tick callback was entered again") && result;
    result = Check(
                 bounded_adapter.Stop(std::chrono::milliseconds(100)),
                 "adapter generation did not drain after callback release") && result;

    // A callback target can have an arbitrary destructor. Bounded lifecycle
    // operations defer that destruction so a blocked target cannot turn a
    // successful Stop into an unbounded wait.
    auto deferred_destruction_memory = std::make_shared<FixtureMemory>();
    Populate(deferred_destruction_memory);
    anomaly::AdapterServiceRegistry deferred_destruction_registry;
    anomaly::Ue5NteAdapter deferred_destruction_adapter(
        Fingerprint(), Profile(), Resolution(), deferred_destruction_memory,
        deferred_destruction_registry);
    const auto destruction_state = std::make_shared<BlockingCallbackDestructionState>();
    result = Check(
                 deferred_destruction_adapter.Start(true),
                 "deferred-destruction adapter did not start") && result;
    deferred_destruction_adapter.SetTickCallback(BlockingCallbackTarget{destruction_state});
    destruction_state->block.store(true, std::memory_order_release);
    const auto deferred_stop_started = std::chrono::steady_clock::now();
    const bool deferred_stop_completed =
        deferred_destruction_adapter.Stop(std::chrono::milliseconds(5));
    const auto deferred_stop_elapsed =
        std::chrono::steady_clock::now() - deferred_stop_started;
    const auto deferred_destruction_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!destruction_state->entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deferred_destruction_deadline) {
        std::this_thread::yield();
    }
    const bool deferred_destruction_entered =
        destruction_state->entered.load(std::memory_order_acquire);
    destruction_state->release.store(true, std::memory_order_release);
    result = Check(
                 deferred_stop_completed && deferred_stop_elapsed < std::chrono::milliseconds(500) &&
                     deferred_destruction_entered &&
                     deferred_destruction_registry.Snapshot().empty(),
                 "bounded stop synchronously destroyed a callback target") && result;

    // Registry revocation uses the same Stop deadline as endpoint/callback
    // draining. A timed-out revocation leaves the closed generation stopping,
    // rejects a restart, and can be retried after the registry lock is free.
    auto registry_blocked_memory = std::make_shared<FixtureMemory>();
    Populate(registry_blocked_memory);
    anomaly::AdapterServiceRegistry registry_blocked_registry;
    anomaly::Ue5NteAdapter registry_blocked_adapter(
        Fingerprint(), Profile(), Resolution(), registry_blocked_memory, registry_blocked_registry);
    result = Check(
                 registry_blocked_adapter.Start(true),
                 "registry-blocked adapter did not start") && result;
    const auto* registry_blocked_names = static_cast<const AnomalyUe5NamesServiceV1*>(
        registry_blocked_registry.Query(ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1));
    std::atomic_bool registry_lock_held{};
    std::atomic_bool release_registry_lock{};
    std::thread registry_locker([&] {
        auto lock = anomaly::test::AdapterServiceRegistryTestAccess::HoldWriteLock(
            registry_blocked_registry);
        registry_lock_held.store(true, std::memory_order_release);
        while (!release_registry_lock.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    const auto registry_lock_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!registry_lock_held.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < registry_lock_deadline) {
        std::this_thread::yield();
    }
    const auto registry_stop_started = std::chrono::steady_clock::now();
    const bool registry_stop_timed_out =
        !registry_blocked_adapter.Stop(std::chrono::milliseconds(5));
    const auto registry_stop_elapsed =
        std::chrono::steady_clock::now() - registry_stop_started;
    std::size_t registry_blocked_name_size{};
    const auto registry_blocked_cached_status = registry_blocked_names == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : registry_blocked_names->resolve_utf8(
              registry_blocked_names->user, 2, nullptr, &registry_blocked_name_size);
    const bool registry_restart_rejected = !registry_blocked_adapter.Start(true);
    release_registry_lock.store(true, std::memory_order_release);
    registry_locker.join();
    const bool registry_stop_completed =
        registry_blocked_adapter.Stop(std::chrono::milliseconds(100));
    result = Check(
                 registry_lock_held.load(std::memory_order_acquire) &&
                     registry_stop_timed_out &&
                     registry_stop_elapsed < std::chrono::milliseconds(500) &&
                     !registry_blocked_adapter.Started() &&
                     registry_blocked_cached_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     registry_restart_rejected && registry_stop_completed &&
                     registry_blocked_registry.Snapshot().empty(),
                 "bounded stop did not retain a retryable closed generation on registry contention") &&
        result;

    // An external Stop must release the lifecycle lock before it waits for an
    // entered callback. Otherwise the callback's own Stop call blocks on that
    // lock while the external stopper waits for the callback to leave.
    auto concurrent_stop_memory = std::make_shared<FixtureMemory>();
    Populate(concurrent_stop_memory);
    anomaly::AdapterServiceRegistry concurrent_stop_registry;
    anomaly::Ue5NteAdapter concurrent_stop_adapter(
        Fingerprint(), Profile(), Resolution(), concurrent_stop_memory, concurrent_stop_registry);
    result = Check(concurrent_stop_adapter.Start(true), "concurrent-stop adapter did not start") && result;
    std::atomic_bool concurrent_callback_entered{};
    std::atomic_bool allow_callback_stop{};
    std::atomic_bool callback_stop_returned{};
    std::atomic_bool callback_stop_result{true};
    std::atomic_bool release_concurrent_callback{};
    std::atomic_bool external_stop_result{};
    concurrent_stop_adapter.SetTickCallback([&](double) {
        concurrent_callback_entered.store(true, std::memory_order_release);
        while (!allow_callback_stop.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        callback_stop_result.store(
            concurrent_stop_adapter.Stop(std::chrono::milliseconds::max()),
            std::memory_order_release);
        callback_stop_returned.store(true, std::memory_order_release);
        while (!release_concurrent_callback.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    std::thread concurrent_tick([&] { concurrent_stop_adapter.OnGameTick(0.5); });
    const auto concurrent_entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!concurrent_callback_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < concurrent_entry_deadline) {
        std::this_thread::yield();
    }
    std::thread external_stopper([&] {
        external_stop_result.store(
            concurrent_stop_adapter.Stop(std::chrono::milliseconds(750)),
            std::memory_order_release);
    });
    const auto concurrent_transition_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((concurrent_stop_adapter.Started() || !concurrent_stop_registry.Snapshot().empty()) &&
           std::chrono::steady_clock::now() < concurrent_transition_deadline) {
        std::this_thread::yield();
    }
    const bool concurrent_transition_started =
        !concurrent_stop_adapter.Started() && concurrent_stop_registry.Snapshot().empty();
    allow_callback_stop.store(true, std::memory_order_release);
    const auto callback_stop_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!callback_stop_returned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < callback_stop_deadline) {
        std::this_thread::yield();
    }
    const bool callback_stop_returned_promptly =
        callback_stop_returned.load(std::memory_order_acquire);
    release_concurrent_callback.store(true, std::memory_order_release);
    concurrent_tick.join();
    external_stopper.join();
    const bool concurrent_stop_cleaned =
        concurrent_stop_adapter.Stop(std::chrono::milliseconds(100));
    result = Check(
                 concurrent_callback_entered.load(std::memory_order_acquire) &&
                     concurrent_transition_started && callback_stop_returned_promptly &&
                     !callback_stop_result.load(std::memory_order_acquire) &&
                     external_stop_result.load(std::memory_order_acquire) &&
                     concurrent_stop_cleaned && concurrent_stop_registry.Snapshot().empty(),
                 "concurrent stop left a callback waiting on the lifecycle lock") && result;

    // Endpoint leases must also make Stop bounded when a cached ABI table is
    // mid-call. The table is closed before waiting for State::mutex, so a
    // caller can observe an unavailable old table without waiting on the read.
    auto draining_memory = std::make_shared<FixtureMemory>();
    Populate(draining_memory);
    anomaly::AdapterServiceRegistry draining_registry;
    anomaly::Ue5NteAdapter draining_adapter(
        Fingerprint(), Profile(), Resolution(), draining_memory, draining_registry);
    result = Check(draining_adapter.Start(true), "draining adapter did not start") && result;
    const auto* draining_names = static_cast<const AnomalyUe5NamesServiceV1*>(
        draining_registry.Query(ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1));
    draining_memory->SuspendRead(FixtureMemory::kBase + 0xA00);
    std::array<char, 16> draining_name{};
    std::size_t draining_name_size = draining_name.size();
    AnomalyStatusV1 draining_name_status{};
    std::thread draining_call([&] {
        if (draining_names != nullptr) {
            draining_name_status = draining_names->resolve_utf8(
                draining_names->user, 2, draining_name.data(), &draining_name_size);
        }
    });
    const auto draining_entry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!draining_memory->SuspendedReadEntered() &&
           std::chrono::steady_clock::now() < draining_entry_deadline) {
        std::this_thread::yield();
    }
    const auto draining_stop_started = std::chrono::steady_clock::now();
    const bool draining_stop_timed_out =
        !draining_adapter.Stop(std::chrono::milliseconds(5));
    const auto draining_stop_elapsed = std::chrono::steady_clock::now() - draining_stop_started;
    std::size_t retired_name_size{};
    const auto retired_while_draining = draining_names == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : draining_names->resolve_utf8(
              draining_names->user, 2, nullptr, &retired_name_size);
    const bool restart_rejected_while_draining = !draining_adapter.Start(true);
    draining_memory->ReleaseSuspendedRead();
    draining_call.join();
    const bool draining_stop_completed =
        draining_adapter.Stop(std::chrono::milliseconds(100));
    const bool draining_adapter_restarted = draining_adapter.Start(true);
    const auto retired_after_restart = draining_names == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : draining_names->resolve_utf8(
              draining_names->user, 2, nullptr, &retired_name_size);
    result = Check(
                 draining_names != nullptr && draining_memory->SuspendedReadEntered() &&
                     draining_stop_timed_out &&
                     draining_stop_elapsed < std::chrono::milliseconds(500) &&
                     retired_while_draining.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     restart_rejected_while_draining &&
                     draining_name_status.code == ANOMALY_STATUS_V1_OK &&
                     draining_stop_completed && draining_adapter_restarted &&
                     retired_after_restart.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     draining_adapter.Stop() && draining_registry.Snapshot().empty(),
                 "in-flight semantic ABI calls did not drain across stop/restart") && result;

    // Registry entries are revoked before a bounded Stop tries to acquire the
    // State lock, so destroying the adapter during an in-flight ABI call does
    // not prevent a later adapter from publishing through the same registry.
    auto destruction_memory = std::make_shared<FixtureMemory>();
    Populate(destruction_memory);
    anomaly::AdapterServiceRegistry destruction_registry;
    const AnomalyUe5NamesServiceV1* destroyed_names{};
    std::array<char, 16> destruction_name{};
    std::size_t destruction_name_size = destruction_name.size();
    AnomalyStatusV1 destruction_name_status{};
    std::thread destruction_call;
    bool destruction_stop_timed_out{};
    bool destruction_registry_revoked{};
    {
        anomaly::Ue5NteAdapter destruction_adapter(
            Fingerprint(), Profile(), Resolution(), destruction_memory, destruction_registry);
        result = Check(destruction_adapter.Start(true), "destruction adapter did not start") && result;
        destroyed_names = static_cast<const AnomalyUe5NamesServiceV1*>(
            destruction_registry.Query(ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1));
        destruction_memory->SuspendRead(FixtureMemory::kBase + 0xA00);
        destruction_call = std::thread([&] {
            if (destroyed_names != nullptr) {
                destruction_name_status = destroyed_names->resolve_utf8(
                    destroyed_names->user, 2, destruction_name.data(), &destruction_name_size);
            }
        });
        const auto destruction_entry_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!destruction_memory->SuspendedReadEntered() &&
               std::chrono::steady_clock::now() < destruction_entry_deadline) {
            std::this_thread::yield();
        }
        destruction_stop_timed_out = !destruction_adapter.Stop(std::chrono::milliseconds::zero());
        destruction_registry_revoked = destruction_registry.Snapshot().empty();
    }
    destruction_memory->ReleaseSuspendedRead();
    destruction_call.join();
    std::size_t destroyed_name_size{};
    const auto destroyed_table_status = destroyed_names == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : destroyed_names->resolve_utf8(destroyed_names->user, 2, nullptr, &destroyed_name_size);
    anomaly::Ue5NteAdapter replacement_adapter(
        Fingerprint(), Profile(), Resolution(), destruction_memory, destruction_registry);
    const bool replacement_adapter_started = replacement_adapter.Start(true);
    const bool replacement_adapter_stopped = replacement_adapter.Stop();
    result = Check(
                 destroyed_names != nullptr && destruction_memory->SuspendedReadEntered() &&
                     destruction_stop_timed_out && destruction_registry_revoked &&
                     destruction_name_status.code == ANOMALY_STATUS_V1_OK &&
                     destroyed_table_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     replacement_adapter_started && replacement_adapter_stopped &&
                     destruction_registry.Snapshot().empty(),
                 "timed-out adapter destruction left the service registry poisoned") && result;

    // Callback initiated lifecycle operations must detach first and return to
    // the caller rather than waiting for their own active callback count.
    auto reentrant_memory = std::make_shared<FixtureMemory>();
    Populate(reentrant_memory);
    anomaly::AdapterServiceRegistry reentrant_registry;
    anomaly::Ue5NteAdapter reentrant_adapter(
        Fingerprint(), Profile(), Resolution(), reentrant_memory, reentrant_registry);
    result = Check(reentrant_adapter.Start(true), "reentrant adapter did not start") && result;
    std::atomic_bool clear_from_callback{true};
    reentrant_adapter.SetTickCallback([&](double) {
        clear_from_callback.store(
            reentrant_adapter.ClearTickCallback(std::chrono::milliseconds::max()),
            std::memory_order_release);
    });
    reentrant_adapter.OnGameTick(0.5);
    const bool clear_drained = reentrant_adapter.ClearTickCallback(std::chrono::milliseconds::zero());
    std::atomic_bool stop_from_callback{true};
    reentrant_adapter.SetTickCallback([&](double) {
        stop_from_callback.store(
            reentrant_adapter.Stop(std::chrono::milliseconds::max()),
            std::memory_order_release);
    });
    reentrant_adapter.OnGameTick(0.5);
    result = Check(
                 !clear_from_callback.load(std::memory_order_acquire) && clear_drained &&
                     !stop_from_callback.load(std::memory_order_acquire) &&
                     !reentrant_adapter.Started() && reentrant_registry.Snapshot().empty() &&
                     reentrant_adapter.Stop(std::chrono::milliseconds(100)),
                 "callback-owned lifecycle operation waited for itself") && result;

    // A profile-owned feature validator can withdraw a service after startup.
    // Registry queries and previously cached tables must agree on the new
    // unavailable state rather than leaking a stale player/entity snapshot.
    auto validator_memory = std::make_shared<FixtureMemory>();
    Populate(validator_memory);
    anomaly::AdapterServiceRegistry validator_registry;
    auto validator_profile = SemanticProfile();
    validator_profile.feature_layout_validators["ue5.framework"] = {"nte-player-layout-v1"};
    validator_profile.feature_layout_validators["ue5.names"] = {"nte-player-layout-v1"};
    validator_profile.feature_layout_validators["ue5.objects"] = {"nte-player-layout-v1"};
    validator_profile.feature_layout_validators["ue5.world"] = {"nte-player-layout-v1"};
    anomaly::Ue5NteAdapter validator_adapter(
        Fingerprint(), std::move(validator_profile), Resolution(), validator_memory, validator_registry);
    result = Check(validator_adapter.Start(true), "validator adapter did not start") && result;
    const auto* validator_framework = static_cast<const AnomalyUe5FrameworkServiceV1*>(
        validator_registry.Query(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1));
    const auto* validator_names = static_cast<const AnomalyUe5NamesServiceV1*>(
        validator_registry.Query(ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1));
    const auto* validator_objects = static_cast<const AnomalyUe5ObjectsServiceV1*>(
        validator_registry.Query(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1));
    const auto* validator_world = static_cast<const AnomalyUe5WorldServiceV1*>(
        validator_registry.Query(ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1));
    const auto* validator_player = static_cast<const AnomalyNtePlayerServiceV1*>(
        validator_registry.Query(ANOMALY_NTE_PLAYER_SERVICE_V1_ID, 1));
    const auto* validator_entities = static_cast<const AnomalyNteEntitiesServiceV1*>(
        validator_registry.Query(ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, 1));
    const auto* validator_build = static_cast<const AnomalyNteBuildServiceV1*>(
        validator_registry.Query(ANOMALY_NTE_BUILD_SERVICE_V1_ID, 1));
    validator_adapter.OnGameTick(0.5);
    AnomalyGenerationHandleV1 validator_world_handle{};
    const auto validator_world_before_withdrawal = validator_world == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : validator_world->current(validator_world->user, &validator_world_handle);
    constexpr std::uintptr_t kInvalidValidatorWorld = FixtureMemory::kBase + 0x5000;
    validator_memory->Put(FixtureMemory::kBase + 0x900, kInvalidValidatorWorld);
    for (std::uint32_t tick{}; tick != 60; ++tick) {
        validator_adapter.OnGameTick(0.5);
    }
    std::array<AnomalyNteEntitySnapshotV1, 1> validator_page_destination{};
    std::memset(validator_page_destination.data(), 0x7E, sizeof(validator_page_destination));
    validator_page_destination[0].struct_size = sizeof(validator_page_destination[0]);
    const auto validator_page_destination_before = validator_page_destination;
    AnomalyNteEntityPageRequestV1 validator_page_request{
        sizeof(validator_page_request), 0, 0, 0, 1, 0, 0, 0, 0, 0};
    AnomalyNteEntityPageResultV1 validator_page_result{};
    std::memset(&validator_page_result, 0x6D, sizeof(validator_page_result));
    validator_page_result.struct_size = sizeof(validator_page_result);
    const auto validator_page_result_before = validator_page_result;
    const auto validator_page_status = validator_entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : validator_entities->page(
              validator_entities->user, &validator_page_request,
              validator_page_destination.data(), &validator_page_result);
    AnomalyNtePlayerSnapshotV1 validator_player_snapshot{sizeof(validator_player_snapshot)};
    AnomalyNteEntityFrameV1 validator_entity_frame{sizeof(validator_entity_frame)};
    const auto validator_player_status = validator_player == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : validator_player->snapshot(validator_player->user, &validator_player_snapshot);
    const auto validator_entities_status = validator_entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : validator_entities->frame(validator_entities->user, &validator_entity_frame);
    const auto validator_feature_state = validator_build == nullptr
        ? ANOMALY_FEATURE_V1_UNAVAILABLE
        : validator_build->feature_state(
              validator_build->user,
              {"nte.player", sizeof("nte.player") - 1U});
    std::size_t validator_name_size{};
    AnomalyUe5ObjectSnapshotV1 validator_object_snapshot{sizeof(validator_object_snapshot)};
    AnomalyUe5WorldSnapshotV1 validator_world_snapshot{sizeof(validator_world_snapshot)};
    AnomalyGenerationHandleV1 validator_world_after_withdrawal{};
    const auto validator_name_status = validator_names == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : validator_names->resolve_utf8(validator_names->user, 2, nullptr, &validator_name_size);
    const auto validator_object_status = validator_objects == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : validator_objects->snapshot_at(
              validator_objects->user, 0, &validator_object_snapshot);
    const auto validator_world_current_status = validator_world == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : validator_world->current(validator_world->user, &validator_world_after_withdrawal);
    const auto validator_world_snapshot_status = validator_world == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : validator_world->snapshot(
              validator_world->user, validator_world_handle, &validator_world_snapshot);
    result = Check(
                 validator_framework != nullptr && validator_names != nullptr &&
                     validator_objects != nullptr && validator_world != nullptr &&
                     validator_player != nullptr && validator_entities != nullptr &&
                     validator_build != nullptr &&
                     validator_world_before_withdrawal.code == ANOMALY_STATUS_V1_OK &&
                     validator_feature_state == ANOMALY_FEATURE_V1_UNAVAILABLE &&
                     validator_registry.Query(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1) == nullptr &&
                     validator_registry.Query(ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1) == nullptr &&
                     validator_registry.Query(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1) == nullptr &&
                     validator_registry.Query(ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1) == nullptr &&
                     validator_registry.Query(ANOMALY_NTE_PLAYER_SERVICE_V1_ID, 1) == nullptr &&
                     validator_registry.Query(ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, 1) == nullptr &&
                     validator_framework->game_thread_id(validator_framework->user) == 0 &&
                     validator_framework->tick_sequence(validator_framework->user) == 0 &&
                     validator_framework->is_game_thread(validator_framework->user) == 0 &&
                     validator_name_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     validator_objects->generation(validator_objects->user) == 0 &&
                     validator_objects->count(validator_objects->user) == 0 &&
                     validator_object_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     validator_world_current_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     validator_world_snapshot_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     validator_player_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     validator_entities_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     validator_page_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     std::memcmp(
                         validator_page_destination.data(), validator_page_destination_before.data(),
                         sizeof(validator_page_destination)) == 0 &&
                     std::memcmp(
                         &validator_page_result, &validator_page_result_before,
                         sizeof(validator_page_result)) == 0 &&
                     validator_adapter.Stop() && validator_registry.Snapshot().empty(),
                 "runtime layout validation did not withdraw cached service tables") && result;

    // Session events are retained in a bounded cursor stream. Exercise world replacement,
    // unavailable transitions, and an expired cursor without exposing a raw World pointer.
    const std::uintptr_t session_replacement_world = FixtureMemory::kBase + 0x3000;
    memory->Put(FixtureMemory::kBase + 0x900, session_replacement_world);
    adapter.OnGameTick(0.5);
    AnomalyNteSessionEventV1 changed_event{sizeof(changed_event)};
    const auto changed_status = session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : session->next_event(session->user, first_session_event.sequence, &changed_event);
    const std::uintptr_t unavailable_world{};
    memory->Put(FixtureMemory::kBase + 0x900, unavailable_world);
    adapter.OnGameTick(0.5);
    AnomalyNteSessionEventV1 unavailable_event{sizeof(unavailable_event)};
    const auto unavailable_status = session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : session->next_event(session->user, changed_event.sequence, &unavailable_event);
    for (std::uint32_t index = 0; index != 65; ++index) {
        const std::uintptr_t next_world = index % 2 == 0
            ? FixtureMemory::kBase + 0x3100
            : FixtureMemory::kBase + 0x3200;
        memory->Put(FixtureMemory::kBase + 0x900, next_world);
        adapter.OnGameTick(0.5);
    }
    AnomalyNteSessionEventV1 expired_event{sizeof(expired_event)};
    const auto expired_status = session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : session->next_event(session->user, first_session_event.sequence, &expired_event);
    result = Check(
                 changed_status.code == ANOMALY_STATUS_V1_OK &&
                     changed_event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED &&
                     changed_event.previous_world.id == 1 && changed_event.world.id == 1 &&
                     changed_event.previous_world.generation != changed_event.world.generation &&
                     unavailable_status.code == ANOMALY_STATUS_V1_OK &&
                     unavailable_event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE &&
                     unavailable_event.previous_world.id == 1 && unavailable_event.world.id == 0 &&
                     expired_status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                     session->latest_event_sequence(session->user) > 64,
                 "NTE lifecycle event stream did not preserve cursor semantics") && result;

    const auto event_sequence_before_restart = session == nullptr
        ? 0U
        : session->latest_event_sequence(session->user);
    const bool adapter_stopped = adapter.Stop();
    AnomalyNteSessionSnapshotV1 stopped_session_snapshot{sizeof(stopped_session_snapshot)};
    AnomalyNtePlayerSnapshotV1 stopped_player_snapshot{sizeof(stopped_player_snapshot)};
    AnomalyNteEntityFrameV1 stopped_entity_frame{sizeof(stopped_entity_frame)};
    AnomalyNteSnapshotMetricsV1 stopped_metrics{sizeof(stopped_metrics)};
    const auto stopped_session_status = session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : session->snapshot(session->user, &stopped_session_snapshot);
    const auto stopped_player_status = player == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : player->snapshot(player->user, &stopped_player_snapshot);
    const auto stopped_entities_status = entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : entities->frame(entities->user, &stopped_entity_frame);
    const auto stopped_metrics_status = metrics == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : metrics->snapshot(metrics->user, &stopped_metrics);
    result = Check(
                 adapter_stopped && registry.Snapshot().empty() &&
                     stopped_session_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     stopped_player_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     stopped_entities_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                     stopped_metrics_status.code == ANOMALY_STATUS_V1_UNAVAILABLE,
                 "cached semantic service tables remained usable after stop") && result;

    // A reused adapter generation must rebind its game thread, discard all old
    // handles, reject prior lifecycle cursors, and expose a new cursor-zero event stream.
    Populate(memory);
    result = Check(adapter.Start(true), "adapter did not restart") && result;
    const auto* restart_world = static_cast<const AnomalyUe5WorldServiceV1*>(
        registry.Query(ANOMALY_UE5_WORLD_SERVICE_V1_ID, ANOMALY_UE5_WORLD_SERVICE_V1_VERSION));
    const auto* restart_session = static_cast<const AnomalyNteSessionServiceV1*>(
        registry.Query(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID,
            ANOMALY_NTE_SESSION_SERVICE_V1_VERSION));
    const auto* restart_metrics = static_cast<const AnomalyNteMetricsServiceV1*>(
        registry.Query(
            ANOMALY_NTE_METRICS_SERVICE_V1_ID,
            ANOMALY_NTE_METRICS_SERVICE_V1_VERSION));
    AnomalyNteSnapshotMetricsV1 restart_metrics_before{sizeof(restart_metrics_before)};
    const auto restart_metrics_before_status = restart_metrics == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : restart_metrics->snapshot(restart_metrics->user, &restart_metrics_before);
    std::atomic<DWORD> restart_tick_thread{};
    std::atomic_bool restart_tick_ready{};
    std::atomic_bool allow_restart_followup{};
    std::thread restart_tick([&] {
        restart_tick_thread.store(GetCurrentThreadId(), std::memory_order_release);
        adapter.OnGameTick(0.125);
        restart_tick_ready.store(true, std::memory_order_release);
        while (!allow_restart_followup.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        adapter.OnGameTick(0.125);
    });
    const auto restart_tick_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!restart_tick_ready.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < restart_tick_deadline) {
        std::this_thread::yield();
    }
    AnomalyGenerationHandleV1 restart_world_handle{};
    AnomalyUe5WorldSnapshotV1 stale_restart_world{sizeof(stale_restart_world)};
    AnomalyNteSessionEventV1 stale_restart_cursor_event{sizeof(stale_restart_cursor_event)};
    AnomalyNteSessionEventV1 restart_event{sizeof(restart_event)};
    AnomalyNteSessionSnapshotV1 retired_session_snapshot{sizeof(retired_session_snapshot)};
    AnomalyNteSnapshotMetricsV1 restart_metrics_after{sizeof(restart_metrics_after)};
    const auto stale_restart_world_status = restart_world == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : restart_world->snapshot(restart_world->user, world_handle, &stale_restart_world);
    const auto stale_restart_cursor_status = restart_session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : restart_session->next_event(
              restart_session->user, event_sequence_before_restart, &stale_restart_cursor_event);
    const auto restart_event_status = restart_session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : restart_session->next_event(restart_session->user, 0, &restart_event);
    const auto retired_session_status = session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : session->snapshot(session->user, &retired_session_snapshot);
    const auto restart_metrics_after_status = restart_metrics == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : restart_metrics->snapshot(restart_metrics->user, &restart_metrics_after);
    result = Check(
                 restart_world != nullptr && restart_session != nullptr && restart_metrics != nullptr &&
                     restart_metrics_before_status.code == ANOMALY_STATUS_V1_OK &&
                     restart_metrics_before.flags == ANOMALY_NTE_METRICS_V1_VALID &&
                     restart_metrics_before.tick_sequence == 0 &&
                     restart_metrics_before.snapshot_tick_count == 0 &&
                     adapter.TickSequence() == 1 &&
                     adapter.GameThreadId() == restart_tick_thread.load(std::memory_order_acquire) &&
                     adapter.GameThreadId() != initial_game_thread_id &&
                     restart_world->current(restart_world->user, &restart_world_handle).code ==
                         ANOMALY_STATUS_V1_OK &&
                      restart_world_handle.generation > world_handle.generation &&
                      stale_restart_world_status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                      event_sequence_before_restart != 0 &&
                       stale_restart_cursor_status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                       restart_event_status.code == ANOMALY_STATUS_V1_OK &&
                       retired_session_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                       restart_event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_READY &&
                      restart_event.sequence > event_sequence_before_restart &&
                      restart_event.tick_sequence == 1 && restart_event.previous_world.id == 0 &&
                      restart_event.world.id == restart_world_handle.id &&
                      restart_event.world.generation == restart_world_handle.generation &&
                      restart_session->latest_event_sequence(restart_session->user) ==
                          restart_event.sequence &&
                      restart_metrics_after_status.code == ANOMALY_STATUS_V1_OK &&
                     restart_metrics_after.flags == ANOMALY_NTE_METRICS_V1_VALID &&
                     restart_metrics_after.tick_sequence == 1 &&
                     restart_metrics_after.snapshot_tick_count == 1,
                 "adapter restart did not reset its generation lifecycle") && result;

    // The prior stream's first cursor is deliberately small. It would be
    // accepted after a reset once this generation emits a second event unless
    // lifecycle transitions reserve a cursor discontinuity.
    const std::uintptr_t restart_replacement_world = FixtureMemory::kBase + 0x3300;
    memory->Put(FixtureMemory::kBase + 0x900, restart_replacement_world);
    allow_restart_followup.store(true, std::memory_order_release);
    restart_tick.join();
    AnomalyNteSessionEventV1 stale_first_restart_cursor_event{
        sizeof(stale_first_restart_cursor_event)};
    AnomalyNteSessionEventV1 restart_changed_event{sizeof(restart_changed_event)};
    const auto stale_first_restart_cursor_status = restart_session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : restart_session->next_event(
              restart_session->user,
              first_session_event.sequence,
              &stale_first_restart_cursor_event);
    const auto restart_changed_event_status = restart_session == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : restart_session->next_event(
              restart_session->user, restart_event.sequence, &restart_changed_event);
    result = Check(
                 stale_first_restart_cursor_status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                     restart_changed_event_status.code == ANOMALY_STATUS_V1_OK &&
                     restart_changed_event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED &&
                     restart_changed_event.sequence > restart_event.sequence,
                 "adapter restart accepted a cursor from the prior lifecycle") && result;
    result = Check(adapter.Stop() && registry.Snapshot().empty(),
                   "restarted adapter services survived stop") && result;

    // Layout offsets are profile data. Values adjacent to INT64_MAX must take the
    // normal unavailable path instead of overflowing while locating count fields.
    constexpr std::int64_t kNearMaximumOffset =
        (std::numeric_limits<std::int64_t>::max)() - 7;
    auto malformed_player_memory = std::make_shared<FixtureMemory>();
    Populate(malformed_player_memory);
    auto malformed_player_profile = Profile();
    malformed_player_profile.layout["gameInstance.localPlayers"] =
        (std::numeric_limits<std::int64_t>::max)();
    anomaly::AdapterServiceRegistry malformed_player_registry;
    anomaly::Ue5NteAdapter malformed_player_adapter(
        Fingerprint(), std::move(malformed_player_profile), Resolution(), malformed_player_memory,
        malformed_player_registry);
    result = Check(
                 malformed_player_adapter.Start(true),
                 "malformed player-layout adapter did not start") && result;
    const auto* malformed_player_service = static_cast<const AnomalyNtePlayerServiceV1*>(
        malformed_player_registry.Query(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION));
    const auto* malformed_player_build = static_cast<const AnomalyNteBuildServiceV1*>(
        malformed_player_registry.Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION));
    malformed_player_adapter.OnGameTick(0.25);
    AnomalyNtePlayerSnapshotV1 malformed_player_snapshot{sizeof(malformed_player_snapshot)};
    const auto malformed_player_status = malformed_player_service == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : malformed_player_service->snapshot(
              malformed_player_service->user, &malformed_player_snapshot);
    result = Check(
                  malformed_player_adapter.TickSequence() == 1 &&
                      malformed_player_service == nullptr &&
                      malformed_player_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                      malformed_player_build != nullptr &&
                      malformed_player_build->feature_state(
                          malformed_player_build->user,
                          {"nte.player", sizeof("nte.player") - 1U}) ==
                          ANOMALY_FEATURE_V1_UNAVAILABLE &&
                      malformed_player_memory->PlayerChainReads() == 0 &&
                      malformed_player_memory->EntityChainReads() == 0 &&
                      malformed_player_adapter.Stop() && malformed_player_registry.Snapshot().empty(),
                 "INT64_MAX player layout did not fail cleanly") && result;

    auto malformed_entity_memory = std::make_shared<FixtureMemory>();
    Populate(malformed_entity_memory);
    auto malformed_entity_profile = Profile();
    malformed_entity_profile.layout["level.actors"] = kNearMaximumOffset;
    anomaly::AdapterServiceRegistry malformed_entity_registry;
    anomaly::Ue5NteAdapter malformed_entity_adapter(
        Fingerprint(), std::move(malformed_entity_profile), Resolution(), malformed_entity_memory,
        malformed_entity_registry);
    result = Check(
                 malformed_entity_adapter.Start(true),
                 "malformed entity-layout adapter did not start") && result;
    static_cast<void>(malformed_entity_registry.Query(
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION));
    const auto* malformed_entities_service = static_cast<const AnomalyNteEntitiesServiceV1*>(
        malformed_entity_registry.Query(
            ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION));
    const auto* malformed_entity_build = static_cast<const AnomalyNteBuildServiceV1*>(
        malformed_entity_registry.Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION));
    malformed_entity_adapter.OnGameTick(0.25);
    AnomalyNteEntityFrameV1 malformed_entity_frame{sizeof(malformed_entity_frame)};
    const auto malformed_entity_status = malformed_entities_service == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : malformed_entities_service->frame(
              malformed_entities_service->user, &malformed_entity_frame);
    result = Check(
                  malformed_entity_adapter.TickSequence() == 1 &&
                      malformed_entities_service == nullptr &&
                      malformed_entity_status.code == ANOMALY_STATUS_V1_UNAVAILABLE &&
                      malformed_entity_build != nullptr &&
                      malformed_entity_build->feature_state(
                          malformed_entity_build->user,
                          {"nte.entities", sizeof("nte.entities") - 1U}) ==
                          ANOMALY_FEATURE_V1_UNAVAILABLE &&
                      malformed_entity_memory->EntityChainReads() == 0 &&
                      malformed_entity_adapter.Stop() && malformed_entity_registry.Snapshot().empty(),
                 "near-INT64_MAX entity layout did not fail cleanly") && result;

    anomaly::AdapterServiceRegistry frequency_registry;
    auto frequency_memory = std::make_shared<FixtureMemory>();
    Populate(frequency_memory);
    anomaly::Ue5NteAdapter frequency_adapter(
        Fingerprint(), Profile(), Resolution(), frequency_memory, frequency_registry,
        anomaly::NteSnapshotSamplingOptions{2, 2});
    result = Check(frequency_adapter.Start(true), "frequency adapter did not start") && result;
    const auto* frequency_player = static_cast<const AnomalyNtePlayerServiceV1*>(
        frequency_registry.Query(ANOMALY_NTE_PLAYER_SERVICE_V1_ID, 1));
    const auto* frequency_entities = static_cast<const AnomalyNteEntitiesServiceV1*>(
        frequency_registry.Query(ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, 1));
    frequency_adapter.OnGameTick(0.25);
    AnomalyNtePlayerSnapshotV1 frequency_player_first{sizeof(frequency_player_first)};
    AnomalyNteEntityFrameV1 frequency_frame_first{sizeof(frequency_frame_first)};
    result = Check(
                 frequency_player && frequency_entities &&
                     frequency_player->snapshot(
                         frequency_player->user, &frequency_player_first).code ==
                         ANOMALY_STATUS_V1_OK &&
                     frequency_entities->frame(
                         frequency_entities->user, &frequency_frame_first).code ==
                         ANOMALY_STATUS_V1_OK &&
                     frequency_player_first.sequence == 1 &&
                     frequency_frame_first.sequence == 1 &&
                     frequency_memory->PlayerChainReads() == 1,
                 "initial configured-frequency snapshot failed") && result;
    const std::array<double, 3> changed_camera{777.0, 0.0, 0.0};
    frequency_memory->PutBytes(
        FixtureMemory::kBase + 0x2100, changed_camera.data(), sizeof(changed_camera));
    frequency_adapter.OnGameTick(0.25);
    AnomalyNtePlayerSnapshotV1 frequency_player_stale{sizeof(frequency_player_stale)};
    AnomalyNteEntityFrameV1 frequency_frame_stale{sizeof(frequency_frame_stale)};
    result = Check(
                 frequency_player->snapshot(
                     frequency_player->user, &frequency_player_stale).code ==
                         ANOMALY_STATUS_V1_OK &&
                     frequency_entities->frame(
                         frequency_entities->user, &frequency_frame_stale).code ==
                         ANOMALY_STATUS_V1_OK &&
                     frequency_player_stale.sequence == 1 &&
                     frequency_frame_stale.sequence == 1 &&
                     frequency_frame_stale.generation == frequency_frame_first.generation &&
                     frequency_frame_stale.camera_position[0] == -300.0 &&
                     (frequency_player_stale.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) != 0 &&
                     (frequency_frame_stale.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) != 0 &&
                     frequency_memory->PlayerChainReads() == 1,
                 "configured interval did not retain a coherent stale frame") && result;
    frequency_adapter.OnGameTick(0.25);
    AnomalyNteEntityFrameV1 frequency_frame_next{sizeof(frequency_frame_next)};
    AnomalyNteEntitySnapshotV1 frequency_old_snapshot{sizeof(frequency_old_snapshot)};
    result = Check(
                 frequency_entities->frame(
                     frequency_entities->user, &frequency_frame_next).code ==
                         ANOMALY_STATUS_V1_OK &&
                     frequency_frame_next.sequence == 3 &&
                     frequency_frame_next.generation != frequency_frame_first.generation &&
                     frequency_frame_next.camera_position[0] == 777.0 &&
                     frequency_entities->snapshot_at(
                         frequency_entities->user, frequency_frame_first.generation, 0,
                         &frequency_old_snapshot).code == ANOMALY_STATUS_V1_NOT_FOUND &&
                     frequency_memory->PlayerChainReads() == 2,
                 "new entity frame did not invalidate the prior generation") && result;
    frequency_adapter.Stop();

    anomaly::AdapterServiceRegistry pull_registry;
    auto pull_memory = std::make_shared<FixtureMemory>();
    Populate(pull_memory);
    anomaly::Ue5NteAdapter pull_adapter(
        Fingerprint(), Profile(), Resolution(), pull_memory, pull_registry,
        anomaly::NteSnapshotSamplingOptions{1, 1});
    result = Check(pull_adapter.Start(true), "pull-driven adapter did not start") && result;
    const auto* pull_entities = static_cast<const AnomalyNteEntitiesServiceV1*>(
        pull_registry.Query(ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, 1));
    pull_adapter.OnGameTick(0.25);
    const auto reads_after_initial_sample = pull_memory->EntityChainReads();
    AnomalyNteEntityFrameV1 pull_frame{sizeof(pull_frame)};
    const bool requested = pull_entities != nullptr &&
        pull_entities->frame(pull_entities->user, &pull_frame).code == ANOMALY_STATUS_V1_OK;
    pull_adapter.OnGameTick(0.25);
    const auto reads_after_request = pull_memory->EntityChainReads();
    pull_adapter.OnGameTick(0.25);
    pull_adapter.OnGameTick(0.25);
    result = Check(
                 requested && reads_after_initial_sample > 0 &&
                     reads_after_request > reads_after_initial_sample &&
                     pull_memory->EntityChainReads() == reads_after_request &&
                     pull_adapter.Stop(),
                 "entity sampling continued without a new frame request") && result;

    // Page results are served from one immutable Host frame: continuation pages must not
    // traverse live memory or mix entities from a later sample.
    auto paged_memory = std::make_shared<FixtureMemory>();
    Populate(paged_memory);
    const auto paged_base = FixtureMemory::kBase;
    const std::uintptr_t paged_actor_array = paged_base + 0x2500;
    const std::uintptr_t paged_class = paged_base + 0x2600;
    const std::array<double, 3> paged_extent{20.0, 30.0, 40.0};
    for (std::uint32_t index = 0; index != 3; ++index) {
        const std::uintptr_t actor = paged_base + 0x2700 +
            static_cast<std::uintptr_t>(index) * 0x100;
        const std::uintptr_t root = paged_base + 0x2A00 +
            static_cast<std::uintptr_t>(index) * 0x100;
        const std::int32_t actor_index = 100 + static_cast<std::int32_t>(index);
        const std::uint32_t actor_name = 16;
        const std::uint8_t mobility = 2;
        const std::array<double, 3> center{
            100.0 + static_cast<double>(index), 200.0, 300.0};
        paged_memory->Put(
            paged_actor_array + static_cast<std::uintptr_t>(index + 1U) *
                sizeof(std::uintptr_t),
            actor);
        paged_memory->Put(actor + 0x50, root);
        paged_memory->Put(actor + 0x10, paged_class);
        paged_memory->Put(actor + 0x0C, actor_index);
        paged_memory->Put(actor + 0x08, actor_name);
        paged_memory->Put(root + 0x58, mobility);
        paged_memory->PutBytes(root + 0x60, center.data(), sizeof(center));
        paged_memory->PutBytes(root + 0x78, paged_extent.data(), sizeof(paged_extent));
    }
    const std::int32_t paged_actor_count = 4;
    paged_memory->Put(paged_base + 0x2428, paged_actor_count);
    anomaly::AdapterServiceRegistry paged_registry;
    anomaly::Ue5NteAdapter paged_adapter(
        Fingerprint(), Profile(), Resolution(), paged_memory, paged_registry);
    result = Check(paged_adapter.Start(true), "paged adapter did not start") && result;
    const auto* paged_entities = static_cast<const AnomalyNteEntitiesServiceV1*>(
        paged_registry.Query(
            ANOMALY_NTE_ENTITIES_SERVICE_V1_ID,
            ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION));
    const auto* paged_metrics = static_cast<const AnomalyNteMetricsServiceV1*>(
        paged_registry.Query(
            ANOMALY_NTE_METRICS_SERVICE_V1_ID,
            ANOMALY_NTE_METRICS_SERVICE_V1_VERSION));
    const auto* paged_build = static_cast<const AnomalyNteBuildServiceV1*>(
        paged_registry.Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID,
            ANOMALY_NTE_BUILD_SERVICE_V1_VERSION));
    paged_adapter.OnGameTick(0.25);
    AnomalyNteEntityFrameV1 paged_frame{sizeof(paged_frame)};
    std::array<AnomalyNteEntitySnapshotV1, 2> first_paged_entities{};
    std::array<AnomalyNteEntitySnapshotV1, 2> second_paged_entities{};
    for (auto& entity : first_paged_entities) entity.struct_size = sizeof(entity);
    for (auto& entity : second_paged_entities) entity.struct_size = sizeof(entity);
    AnomalyNteEntityPageRequestV1 first_paged_request{
        sizeof(first_paged_request), 0, 0, 0,
        static_cast<std::uint32_t>(first_paged_entities.size()), 0, 0, 0, 0, 0};
    AnomalyNteEntityPageResultV1 first_paged_result{sizeof(first_paged_result)};
    const auto paged_player_reads_before_pages = paged_memory->PlayerChainReads();
    const auto paged_entity_reads_before_pages = paged_memory->EntityChainReads();
    const auto first_paged_status = paged_entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : paged_entities->page(
              paged_entities->user, &first_paged_request, first_paged_entities.data(),
              &first_paged_result);
    AnomalyNteEntityPageRequestV1 second_paged_request = first_paged_request;
    second_paged_request.generation = first_paged_result.generation;
    second_paged_request.offset = first_paged_result.next_offset;
    AnomalyNteEntityPageResultV1 second_paged_result{sizeof(second_paged_result)};
    const auto second_paged_status = paged_entities == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : paged_entities->page(
              paged_entities->user, &second_paged_request, second_paged_entities.data(),
              &second_paged_result);
    AnomalyNteSnapshotMetricsV1 paged_metrics_snapshot{sizeof(paged_metrics_snapshot)};
    const auto paged_metrics_status = paged_metrics == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : paged_metrics->snapshot(paged_metrics->user, &paged_metrics_snapshot);
    result = Check(
                 paged_entities != nullptr && paged_metrics != nullptr && paged_build != nullptr &&
                     paged_entities->frame(paged_entities->user, &paged_frame).code ==
                         ANOMALY_STATUS_V1_OK &&
                     paged_frame.entity_count == 4 &&
                     first_paged_status.code == ANOMALY_STATUS_V1_OK &&
                     first_paged_result.generation == paged_frame.generation &&
                     first_paged_result.sequence == paged_frame.sequence &&
                     first_paged_result.total_matches == 4 && first_paged_result.returned == 2 &&
                     first_paged_result.next_offset == 2 &&
                     second_paged_status.code == ANOMALY_STATUS_V1_OK &&
                     second_paged_result.generation == first_paged_result.generation &&
                     second_paged_result.sequence == first_paged_result.sequence &&
                     second_paged_result.total_matches == 4 && second_paged_result.returned == 2 &&
                     second_paged_result.next_offset == 4 &&
                     first_paged_entities[0].entity_id != first_paged_entities[1].entity_id &&
                     first_paged_entities[0].entity_id != second_paged_entities[0].entity_id &&
                     first_paged_entities[0].entity_id != second_paged_entities[1].entity_id &&
                     first_paged_entities[1].entity_id != second_paged_entities[0].entity_id &&
                     first_paged_entities[1].entity_id != second_paged_entities[1].entity_id &&
                     second_paged_entities[0].entity_id != second_paged_entities[1].entity_id &&
                     paged_memory->PlayerChainReads() == paged_player_reads_before_pages &&
                     paged_memory->EntityChainReads() == paged_entity_reads_before_pages &&
                     paged_metrics_status.code == ANOMALY_STATUS_V1_OK &&
                     paged_metrics_snapshot.flags == ANOMALY_NTE_METRICS_V1_VALID &&
                     paged_metrics_snapshot.entity_page_request_count == 2 &&
                     paged_metrics_snapshot.entity_page_cache_hit_count == 2 &&
                     paged_build->feature_state(paged_build->user, kNteMetricsFeature) ==
                         ANOMALY_FEATURE_V1_AVAILABLE,
                 "entity pages did not share a stable cached frame") && result;
    result = Check(paged_adapter.Stop() && paged_registry.Snapshot().empty(),
                   "paged adapter services survived stop") && result;

    auto validated_profile_memory = std::make_shared<FixtureMemory>();
    Populate(validated_profile_memory);
    auto validated_profile_resolution = Resolution();
    validated_profile_resolution.build_id = "diagnostic-build-changed";
    anomaly::AdapterServiceRegistry validated_profile_registry;
    anomaly::Ue5NteAdapter validated_profile_adapter(
        Fingerprint(), SemanticProfile(), std::move(validated_profile_resolution), validated_profile_memory,
        validated_profile_registry, {}, {}, process_event_invoker);
    result = Check(
                 validated_profile_adapter.Start(true) &&
                     validated_profile_registry.Query(
                          ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
                          ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION) != nullptr,
                 "validated Profile did not publish the player-teleport service") && result;
    const auto* validated_profile_build = static_cast<const AnomalyNteBuildServiceV1*>(
        validated_profile_registry.Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION));
    result = Check(
                 validated_profile_build != nullptr &&
                      validated_profile_build->feature_state(
                         validated_profile_build->user,
                         {"nte.player-teleport",
                          sizeof("nte.player-teleport") - 1U}) ==
                         ANOMALY_FEATURE_V1_AVAILABLE &&
                     validated_profile_adapter.Stop() &&
                     validated_profile_registry.Snapshot().empty(),
                 "validated Profile did not expose player teleport state") && result;

    auto no_trusted_bridge_memory = std::make_shared<FixtureMemory>();
    Populate(no_trusted_bridge_memory);
    anomaly::AdapterServiceRegistry no_trusted_bridge_registry;
    anomaly::Ue5NteAdapter no_trusted_bridge_adapter(
        Fingerprint(), SemanticProfile(), Resolution(), no_trusted_bridge_memory,
        no_trusted_bridge_registry, {}, {});
    const bool no_trusted_bridge_started = no_trusted_bridge_adapter.Start(true);
    const auto* no_trusted_bridge_build = static_cast<const AnomalyNteBuildServiceV1*>(
        no_trusted_bridge_registry.Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION));
    const bool no_trusted_bridge_service_absent =
        no_trusted_bridge_registry.Query(
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION) == nullptr;
    const bool no_trusted_bridge_feature_unavailable =
        no_trusted_bridge_build != nullptr &&
        no_trusted_bridge_build->feature_state(
            no_trusted_bridge_build->user,
            {"nte.player-teleport",
             sizeof("nte.player-teleport") - 1U}) ==
            ANOMALY_FEATURE_V1_UNAVAILABLE;
    const bool no_trusted_bridge_stopped = no_trusted_bridge_adapter.Stop();
    result = Check(
                 no_trusted_bridge_started &&
                     no_trusted_bridge_service_absent &&
                     no_trusted_bridge_feature_unavailable &&
                     no_trusted_bridge_stopped && no_trusted_bridge_registry.Snapshot().empty(),
                 "production adapter published teleport without a trusted invocation bridge") &&
        result;

    auto unavailable_control_memory = std::make_shared<FixtureMemory>();
    Populate(unavailable_control_memory);
    auto unavailable_control_resolution = Resolution();
    unavailable_control_resolution.features["nte.player-teleport"] = {
        "nte.player-teleport", false, false, {"ue5.GameTick"}};
    anomaly::AdapterServiceRegistry unavailable_control_registry;
    anomaly::Ue5NteAdapter unavailable_control_adapter(
        Fingerprint(), SemanticProfile(), std::move(unavailable_control_resolution),
        unavailable_control_memory, unavailable_control_registry, {}, {},
        process_event_invoker);
    result = Check(
                 unavailable_control_adapter.Start(true) &&
                      unavailable_control_registry.Query(
                          ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
                          ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION) == nullptr &&
                      unavailable_control_adapter.Stop() &&
                      unavailable_control_registry.Snapshot().empty(),
                 "unavailable teleport feature published player teleport") && result;

    auto unvalidated_profile = SemanticProfile();
    unvalidated_profile.feature_layout_validators.erase(
        "nte.player-teleport");
    auto unvalidated_memory = std::make_shared<FixtureMemory>();
    Populate(unvalidated_memory);
    anomaly::AdapterServiceRegistry unvalidated_registry;
    anomaly::Ue5NteAdapter unvalidated_adapter(
        Fingerprint(), std::move(unvalidated_profile), Resolution(), unvalidated_memory,
        unvalidated_registry, {}, {}, process_event_invoker);
    result = Check(
                 unvalidated_adapter.Start(true) &&
                      unvalidated_registry.Query(
                          ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
                          ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION) == nullptr &&
                      unvalidated_adapter.Stop() && unvalidated_registry.Snapshot().empty(),
                 "Profile without the teleport validator published player teleport") && result;

    auto missing_dependency_profile = SemanticProfile();
    missing_dependency_profile.feature_dependencies.erase(
        "nte.player-teleport");
    auto missing_dependency_memory = std::make_shared<FixtureMemory>();
    Populate(missing_dependency_memory);
    anomaly::AdapterServiceRegistry missing_dependency_registry;
    anomaly::Ue5NteAdapter missing_dependency_adapter(
        Fingerprint(), std::move(missing_dependency_profile), Resolution(), missing_dependency_memory,
        missing_dependency_registry, {}, {}, process_event_invoker);
    result = Check(
                 missing_dependency_adapter.Start(true) &&
                       missing_dependency_registry.Query(
                          ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
                          ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION) == nullptr &&
                      missing_dependency_adapter.Stop() &&
                      missing_dependency_registry.Snapshot().empty(),
                  "Profile without the teleport dependency published player teleport") && result;

    auto missing_process_event_profile = SemanticProfile();
    auto& missing_process_event_symbols = missing_process_event_profile.features.at(
        "ue5.process-event");
    std::erase(missing_process_event_symbols, "ue5.ProcessEvent");
    auto missing_process_event_memory = std::make_shared<FixtureMemory>();
    Populate(missing_process_event_memory);
    anomaly::AdapterServiceRegistry missing_process_event_registry;
    anomaly::Ue5NteAdapter missing_process_event_adapter(
        Fingerprint(), std::move(missing_process_event_profile), Resolution(),
        missing_process_event_memory, missing_process_event_registry, {}, {},
        process_event_invoker);
    result = Check(
                 missing_process_event_adapter.Start(true) &&
                       missing_process_event_registry.Query(
                           ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
                           ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION) == nullptr &&
                       missing_process_event_adapter.Stop() &&
                       missing_process_event_registry.Snapshot().empty(),
                  "Profile without the ProcessEvent symbol published player teleport") && result;

    auto missing_bool_mask_profile = SemanticProfile();
    missing_bool_mask_profile.layout.erase("fboolProperty.fieldMask");
    auto missing_bool_mask_memory = std::make_shared<FixtureMemory>();
    Populate(missing_bool_mask_memory);
    anomaly::AdapterServiceRegistry missing_bool_mask_registry;
    anomaly::Ue5NteAdapter missing_bool_mask_adapter(
        Fingerprint(), std::move(missing_bool_mask_profile), Resolution(),
        missing_bool_mask_memory, missing_bool_mask_registry, {}, {},
        process_event_invoker);
    result = Check(
                 missing_bool_mask_adapter.Start(true) &&
                     missing_bool_mask_registry.Query(
                         ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
                         ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION) == nullptr &&
                     missing_bool_mask_adapter.Stop() &&
                     missing_bool_mask_registry.Snapshot().empty(),
                 "Profile without a bool mask published player teleport") && result;

    auto no_esp_memory = std::make_shared<FixtureMemory>();
    Populate(no_esp_memory);
    auto no_esp_resolution = Resolution();
    no_esp_resolution.features["nte.player-esp"] = {
        "nte.player-esp", false, false, {"cameraManager.location"}};
    anomaly::AdapterServiceRegistry no_esp_registry;
    anomaly::Ue5NteAdapter no_esp_adapter(
        Fingerprint(), Profile(), std::move(no_esp_resolution), no_esp_memory, no_esp_registry);
    result = Check(no_esp_adapter.Start(true), "no-ESP adapter did not start") && result;
    const auto* no_esp_player = static_cast<const AnomalyNtePlayerServiceV1*>(
        no_esp_registry.Query(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION));
    const auto* no_esp_entities = static_cast<const AnomalyNteEntitiesServiceV1*>(
        no_esp_registry.Query(
            ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION));
    const auto* no_esp_build = static_cast<const AnomalyNteBuildServiceV1*>(
        no_esp_registry.Query(
            ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION));
    no_esp_adapter.OnGameTick(0.25);
    AnomalyNtePlayerSnapshotV1 no_esp_snapshot{sizeof(no_esp_snapshot)};
    AnomalyNtePlayerEspSnapshotV1 no_esp_esp{sizeof(no_esp_esp)};
    AnomalyNteCameraSnapshotV1 no_esp_camera{sizeof(no_esp_camera)};
    AnomalyNteEntityFrameV1 no_esp_frame{sizeof(no_esp_frame)};
    result = Check(
                 no_esp_player != nullptr &&
                     no_esp_entities != nullptr && no_esp_build != nullptr &&
                     no_esp_build->feature_state(
                         no_esp_build->user,
                         {"nte.entities", sizeof("nte.entities") - 1U}) ==
                         ANOMALY_FEATURE_V1_AVAILABLE &&
                     no_esp_player->snapshot(no_esp_player->user, &no_esp_snapshot).code ==
                         ANOMALY_STATUS_V1_OK &&
                     no_esp_player->esp_snapshot(no_esp_player->user, &no_esp_esp).code ==
                         ANOMALY_STATUS_V1_UNAVAILABLE &&
                     no_esp_player->camera_snapshot(no_esp_player->user, &no_esp_camera).code ==
                         ANOMALY_STATUS_V1_UNAVAILABLE &&
                     no_esp_entities->frame(no_esp_entities->user, &no_esp_frame).code ==
                         ANOMALY_STATUS_V1_OK &&
                     no_esp_frame.entity_count != 0 &&
                     (no_esp_frame.flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) != 0 &&
                     no_esp_frame.horizontal_fov_degrees == 0.0F,
                 "optional player ESP failure disabled entity enumeration or leaked camera data") && result;
    no_esp_adapter.Stop();

    auto fallback_memory = std::make_shared<FixtureMemory>();
    Populate(fallback_memory);
    auto fallback_profile = Profile();
    fallback_profile.layout.erase("world.nameOffset");
    anomaly::AdapterServiceRegistry fallback_registry;
    anomaly::Ue5NteAdapter fallback_adapter(
        Fingerprint(), std::move(fallback_profile), Resolution(), fallback_memory,
        fallback_registry);
    result = Check(fallback_adapter.Start(true), "world fallback adapter did not start") && result;
    fallback_adapter.OnGameTick(0.25);
    const auto* fallback_world = static_cast<const AnomalyUe5WorldServiceV1*>(
        fallback_registry.Query(ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1));
    AnomalyGenerationHandleV1 fallback_handle{};
    AnomalyUe5WorldSnapshotV1 fallback_snapshot{sizeof(fallback_snapshot)};
    result = Check(
                 fallback_world &&
                     fallback_world->current(fallback_world->user, &fallback_handle).code ==
                         ANOMALY_STATUS_V1_OK &&
                     fallback_world->snapshot(
                         fallback_world->user, fallback_handle, &fallback_snapshot).code ==
                         ANOMALY_STATUS_V1_OK &&
                     fallback_snapshot.name_id == 2 &&
                     fallback_memory->WorldZeroOffsetReads() == 0,
                 "world name did not fall back to object.nameOffset") && result;
    fallback_adapter.Stop();

    auto missing_layout_memory = std::make_shared<FixtureMemory>();
    Populate(missing_layout_memory);
    auto missing_layout_profile = Profile();
    missing_layout_profile.layout.erase("world.nameOffset");
    missing_layout_profile.layout.erase("object.nameOffset");
    anomaly::AdapterServiceRegistry missing_layout_registry;
    anomaly::Ue5NteAdapter missing_layout_adapter(
        Fingerprint(), std::move(missing_layout_profile), Resolution(),
        missing_layout_memory, missing_layout_registry);
    result = Check(
                 missing_layout_adapter.Start(true),
                 "missing-layout adapter did not start") && result;
    missing_layout_adapter.OnGameTick(0.25);
    const auto* missing_layout_world = static_cast<const AnomalyUe5WorldServiceV1*>(
        missing_layout_registry.Query(ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1));
    AnomalyGenerationHandleV1 missing_layout_handle{};
    AnomalyUe5WorldSnapshotV1 missing_layout_snapshot{sizeof(missing_layout_snapshot)};
    const auto missing_layout_status = missing_layout_world == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : [&] {
              if (missing_layout_world->current(
                      missing_layout_world->user, &missing_layout_handle).code !=
                  ANOMALY_STATUS_V1_OK) {
                  return AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE};
              }
              return missing_layout_world->snapshot(
                  missing_layout_world->user, missing_layout_handle,
                  &missing_layout_snapshot);
          }();
    result = Check(
                 missing_layout_status.code == ANOMALY_STATUS_V1_OK &&
                     HasMessage(
                         missing_layout_status,
                         "world.nameOffset and object.nameOffset are unavailable") &&
                     missing_layout_memory->WorldZeroOffsetReads() == 0,
                 "missing world-name layout did not skip offset-zero reads") && result;
    missing_layout_adapter.Stop();

    auto world_switch_memory = std::make_shared<FixtureMemory>();
    Populate(world_switch_memory);
    anomaly::AdapterServiceRegistry world_switch_registry;
    anomaly::Ue5NteAdapter world_switch_adapter(
        Fingerprint(), Profile(), Resolution(), world_switch_memory,
        world_switch_registry);
    result = Check(
                 world_switch_adapter.Start(true),
                 "world-switch adapter did not start") && result;
    world_switch_adapter.OnGameTick(0.25);
    const auto* world_switch_service = static_cast<const AnomalyUe5WorldServiceV1*>(
        world_switch_registry.Query(ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1));
    AnomalyGenerationHandleV1 old_world_handle{};
    result = Check(
                 world_switch_service != nullptr &&
                     world_switch_service->current(
                         world_switch_service->user, &old_world_handle).code ==
                         ANOMALY_STATUS_V1_OK,
                 "world-switch fixture did not publish its initial world") && result;
    const std::uintptr_t replacement_world = FixtureMemory::kBase + 0x2800;
    const std::uint32_t replacement_world_name = 2;
    world_switch_memory->Put(replacement_world + 0x08, replacement_world_name);
    world_switch_memory->Put(FixtureMemory::kBase + 0x900, replacement_world);
    world_switch_adapter.OnGameTick(0.25);
    AnomalyGenerationHandleV1 replacement_world_handle{};
    AnomalyUe5WorldSnapshotV1 stale_world_snapshot{sizeof(stale_world_snapshot)};
    const auto stale_world_status = world_switch_service == nullptr
        ? AnomalyStatusV1{ANOMALY_STATUS_V1_UNAVAILABLE}
        : world_switch_service->snapshot(
              world_switch_service->user, old_world_handle, &stale_world_snapshot);
    result = Check(
                 world_switch_service != nullptr &&
                     world_switch_service->current(
                         world_switch_service->user, &replacement_world_handle).code ==
                         ANOMALY_STATUS_V1_OK &&
                     replacement_world_handle.generation > old_world_handle.generation &&
                     stale_world_status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                     HasMessage(stale_world_status, "stale world handle"),
                 "GWorld replacement did not invalidate the prior world handle") && result;
    world_switch_adapter.Stop();

    auto missing_objects_memory = std::make_shared<FixtureMemory>();
    Populate(missing_objects_memory);
    auto missing_objects_profile = Profile();
    anomaly::ProfileSymbol deferred_objects_symbol;
    deferred_objects_symbol.id = "ue5.GObjects";
    deferred_objects_symbol.module = L"fixture.exe";
    deferred_objects_symbol.section = ".text";
    deferred_objects_symbol.pattern = "48 8B 05 ?? ?? ?? ??";
    deferred_objects_symbol.resolve = {
        anomaly::ProfileResolveKind::RipRelative32, 3, 7, 0};
    deferred_objects_symbol.validators = {"readable", "object-registry-v1"};
    missing_objects_profile.symbols.emplace(
        deferred_objects_symbol.id, deferred_objects_symbol);
    missing_objects_profile.features = {
        {"ue5.framework", {"ue5.GameTick"}},
        {"ue5.world", {"ue5.GWorld", "ue5.GameTick"}},
        {"ue5.names", {"ue5.FNamePool"}},
        {"ue5.objects", {"ue5.GObjects", "ue5.GameTick"}},
        {"nte.session", {"ue5.GWorld", "ue5.GameTick"}},
        {"nte.player", {"ue5.GWorld", "ue5.GameTick"}},
        {"nte.player-esp", {"ue5.GWorld", "ue5.GameTick"}},
        {"nte.entities", {"ue5.GWorld", "ue5.GameTick"}},
    };
    auto missing_objects_resolution = Resolution();
    missing_objects_resolution.state = anomaly::ProfileResolutionState::Degraded;
    auto& deferred_objects = missing_objects_resolution.symbols.at("ue5.GObjects");
    deferred_objects.state = anomaly::SymbolResolutionState::ValidationFailed;
    deferred_objects.instruction = FixtureMemory::kBase + 0x400;
    deferred_objects.address = kObjectRegistry;
    deferred_objects.rva = 0;
    deferred_objects.candidate_count = 1;
    deferred_objects.diagnostics = {"object registry was not initialized"};
    missing_objects_resolution.features["ue5.objects"] = {
        "ue5.objects", false, false, {"ue5.GObjects"}};
    anomaly::AdapterServiceRegistry missing_objects_registry;
    anomaly::Ue5NteAdapter missing_objects_adapter(
        Fingerprint(), std::move(missing_objects_profile), std::move(missing_objects_resolution),
        missing_objects_memory, missing_objects_registry);
    result = Check(
                 missing_objects_adapter.Start(true),
                 "missing-objects adapter did not start") && result;
    const auto* missing_objects_player = static_cast<const AnomalyNtePlayerServiceV1*>(
        missing_objects_registry.Query(ANOMALY_NTE_PLAYER_SERVICE_V1_ID, 1));
    result = Check(
                 missing_objects_registry.Query(
                     ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1) != nullptr &&
                     missing_objects_registry.Query(
                         ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1) != nullptr &&
                     missing_objects_registry.Query(
                         ANOMALY_NTE_SESSION_SERVICE_V1_ID, 1) != nullptr &&
                     missing_objects_player != nullptr &&
                     missing_objects_registry.Query(
                         ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, 1) != nullptr &&
                     missing_objects_registry.Query(
                         ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1) == nullptr,
                 "missing GObjects suppressed unrelated tick-backed services") && result;
    missing_objects_adapter.OnGameTick(0.25);
    const auto* recovered_objects = static_cast<const AnomalyUe5ObjectsServiceV1*>(
        missing_objects_registry.Query(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1));
    result = Check(
                 recovered_objects != nullptr &&
                     recovered_objects->count(recovered_objects->user) == 3 &&
                     missing_objects_adapter.Resolution().FeatureAvailable("ue5.objects"),
                 "deferred object validation did not publish the objects service") &&
        result;
    AnomalyNtePlayerSnapshotV1 missing_objects_player_snapshot{
        sizeof(missing_objects_player_snapshot)};
    result = Check(
                 missing_objects_player != nullptr &&
                     missing_objects_player->snapshot(
                         missing_objects_player->user,
                         &missing_objects_player_snapshot).code ==
                         ANOMALY_STATUS_V1_OK,
                 "missing GObjects left the published player service unusable") && result;
    missing_objects_adapter.Stop();

    auto unknown_resolution = Resolution();
    unknown_resolution.state = anomaly::ProfileResolutionState::NoProfile;
    result = Check(
                 unknown_resolution.FeatureAvailable("nte.session") &&
                     unknown_resolution.FeatureAvailable("nte.player") &&
                     unknown_resolution.FeatureAvailable("nte.entities") &&
                     SemanticNteServicesAreHardGated(
                         Fingerprint(), Profile(), std::move(unknown_resolution)),
                 "unknown build published semantic NTE services from a retained feature map") && result;
    auto mismatched_fingerprint = Fingerprint();
    mismatched_fingerprint.id = "nte-win64-other";
    result = Check(
                 !SemanticNteServicesAreHardGated(
                     std::move(mismatched_fingerprint), Profile(), Resolution()),
                 "mismatched fingerprint blocked semantic NTE services") && result;
    auto mismatched_profile = Profile();
    mismatched_profile.source_hash = std::string(64, 'c');
    result = Check(
                 SemanticNteServicesAreHardGated(
                     Fingerprint(), std::move(mismatched_profile), Resolution()),
                 "mismatched profile hash published semantic NTE services") && result;

    auto all_levels_memory = std::make_shared<FixtureMemory>();
    Populate(all_levels_memory);
    auto all_levels_profile = SemanticProfile();
    all_levels_profile.layout["world.levels"] = 0x38;
    all_levels_profile.layout["entities.maxLevels"] = 16;
    all_levels_profile.layout["entities.maxCount"] = 128;
    const std::uintptr_t world_address = FixtureMemory::kBase + 0x1000;
    const std::uintptr_t levels = FixtureMemory::kBase + 0x3200;
    const std::uintptr_t persistent_level = FixtureMemory::kBase + 0x2400;
    const std::uintptr_t streamed_level = FixtureMemory::kBase + 0x3300;
    const std::uintptr_t streamed_actors = FixtureMemory::kBase + 0x3400;
    const std::uintptr_t streamed_actor = FixtureMemory::kBase + 0x3500;
    const std::uintptr_t streamed_root = FixtureMemory::kBase + 0x3600;
    const std::uintptr_t streamed_class = FixtureMemory::kBase + 0x3700;
    all_levels_memory->Put(world_address + 0x38, levels);
    const std::int32_t loaded_level_count = 2;
    all_levels_memory->Put(world_address + 0x40, loaded_level_count);
    all_levels_memory->Put(levels, persistent_level);
    all_levels_memory->Put(levels + sizeof(std::uintptr_t), streamed_level);
    all_levels_memory->Put(streamed_level + 0x20, streamed_actors);
    const std::int32_t streamed_actor_count = 1;
    all_levels_memory->Put(streamed_level + 0x28, streamed_actor_count);
    all_levels_memory->Put(streamed_actors, streamed_actor);
    all_levels_memory->Put(streamed_actor + 0x50, streamed_root);
    all_levels_memory->Put(streamed_actor + 0x10, streamed_class);
    const std::int32_t streamed_actor_index = 44;
    const std::int32_t streamed_class_index = 45;
    all_levels_memory->Put(streamed_actor + 0x0C, streamed_actor_index);
    all_levels_memory->Put(streamed_class + 0x0C, streamed_class_index);
    const std::uint32_t streamed_actor_name = 16;
    const std::uint32_t streamed_class_name = 8;
    all_levels_memory->Put(streamed_actor + 0x08, streamed_actor_name);
    all_levels_memory->Put(streamed_class + 0x08, streamed_class_name);
    const std::uint8_t streamed_mobility = 1;
    all_levels_memory->Put(streamed_root + 0x58, streamed_mobility);
    const std::array<double, 3> streamed_center{300.0, 400.0, 500.0};
    const std::array<double, 3> streamed_extent{10.0, 20.0, 30.0};
    all_levels_memory->PutBytes(
        streamed_root + 0x60, streamed_center.data(), sizeof(streamed_center));
    all_levels_memory->PutBytes(
        streamed_root + 0x78, streamed_extent.data(), sizeof(streamed_extent));
    anomaly::AdapterServiceRegistry all_levels_registry;
    anomaly::Ue5NteAdapter all_levels_adapter(
        Fingerprint(), std::move(all_levels_profile), Resolution(),
        all_levels_memory, all_levels_registry);
    result = Check(all_levels_adapter.Start(true), "all-levels adapter did not start") && result;
    const auto* all_levels_entities = static_cast<const AnomalyNteEntitiesServiceV1*>(
        all_levels_registry.Query(ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, 1));
    const auto* all_levels_actors = static_cast<const AnomalyNteActorsServiceV1*>(
        all_levels_registry.Query(ANOMALY_NTE_ACTORS_SERVICE_V1_ID, 1));
    AnomalyNteEntityFrameV1 all_levels_actor_frame{sizeof(all_levels_actor_frame)};
    all_levels_adapter.SetTickCallback([&](double) {
        if (all_levels_actors != nullptr) {
            static_cast<void>(all_levels_actors->frame(
                all_levels_actors->user, &all_levels_actor_frame));
        }
    });
    all_levels_adapter.OnGameTick(0.5);
    AnomalyNteEntityFrameV1 all_levels_frame{sizeof(all_levels_frame)};
    const auto all_levels_entity_status = all_levels_entities->frame(
        all_levels_entities->user, &all_levels_frame);
    const auto first_actor_generation = all_levels_actor_frame.generation;
    all_levels_adapter.OnGameTick(0.5);
    result = Check(
                 all_levels_entities != nullptr &&
                     all_levels_entity_status.code == ANOMALY_STATUS_V1_OK &&
                     all_levels_frame.entity_count == 1 &&
                     all_levels_actor_frame.entity_count == 2 &&
                     all_levels_actor_frame.generation == first_actor_generation,
                 "entity and actor services did not keep their level traversal boundaries") && result;
    result = Check(all_levels_adapter.Stop(), "all-levels adapter did not stop") && result;

    anomaly::Ue5NteAdapter no_hook_adapter(
        Fingerprint(), Profile(), Resolution(), memory, registry);
    result = Check(no_hook_adapter.Start(false), "no-hook adapter did not start") &&
        Check(
            registry.Snapshot().size() == 3 &&
                registry.Query(ANOMALY_UE5_BUILD_SERVICE_V1_ID, 1) != nullptr &&
                registry.Query(ANOMALY_NTE_BUILD_SERVICE_V1_ID, 1) != nullptr &&
                registry.Query(ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1) != nullptr &&
                registry.Query(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID, 1) == nullptr &&
                registry.Query(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, 1) == nullptr &&
                registry.Query(ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1) == nullptr &&
                registry.Query(ANOMALY_NTE_SESSION_SERVICE_V1_ID, 1) == nullptr &&
                registry.Query(ANOMALY_NTE_PLAYER_SERVICE_V1_ID, 1) == nullptr &&
                registry.Query(ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, 1) == nullptr &&
                registry.Query(ANOMALY_NTE_METRICS_SERVICE_V1_ID, 1) == nullptr,
            "hook-dependent services were published without a hook") && result;
    no_hook_adapter.Stop();
    result = Check(registry.Snapshot().empty(),
                   "no-hook adapter services survived stop") && result;
    return result ? 0 : 1;
}
