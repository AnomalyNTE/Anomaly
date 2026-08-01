#include "anomaly/symbol_resolver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace anomaly {
namespace {

bool IsReadableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xffU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool IsExecutableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protection & 0xffU) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool Contains(
    std::uintptr_t base,
    std::size_t size,
    std::uintptr_t address,
    std::size_t bytes = 1) noexcept {
    if (address < base || bytes > size) return false;
    return address - base <= size - bytes;
}

bool EqualWideInsensitive(std::wstring_view left, std::wstring_view right) noexcept {
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

SymbolValidationResult ReadableAddress(
    std::uintptr_t address,
    const SymbolMemory& memory) {
    const auto region = memory.Query(address);
    std::uint8_t byte{};
    return region && region->state == MEM_COMMIT &&
        IsReadableProtection(region->protection) && memory.Read(address, &byte, sizeof(byte))
        ? SymbolValidationResult{true, {}}
        : SymbolValidationResult{false, "address is not readable"};
}

SymbolValidationResult PointerShape(
    std::uintptr_t address,
    const SymbolMemory& memory,
    bool null_allowed) {
    const auto readable = ReadableAddress(address, memory);
    if (!readable.valid) return readable;
    std::uintptr_t pointer{};
    if (!memory.Read(address, &pointer, sizeof(pointer))) {
        return {false, "pointer storage is unreadable"};
    }
    if (pointer == 0) {
        return null_allowed
            ? SymbolValidationResult{true, {}}
            : SymbolValidationResult{false, "pointer is null"};
    }
    if ((pointer & (alignof(void*) - 1U)) != 0) return {false, "pointer is misaligned"};
    return ReadableAddress(pointer, memory);
}

bool LayoutValue(
    const BuildProfile& profile,
    std::string_view key,
    std::uint64_t& value,
    std::string& error) {
    const auto found = profile.layout.find(key);
    if (found == profile.layout.end() || found->second < 0) {
        error = "profile layout is missing " + std::string(key);
        return false;
    }
    value = static_cast<std::uint64_t>(found->second);
    return true;
}

bool AddAddress(
    std::uintptr_t base,
    std::uint64_t offset,
    std::uintptr_t& result) noexcept {
    if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    result = base + static_cast<std::uintptr_t>(offset);
    return true;
}

constexpr std::uint64_t kMaximumSemanticLayoutOffset = 64ULL * 1024ULL * 1024ULL;
constexpr std::int32_t kMaximumSemanticLocalPlayers = 16;
constexpr std::int32_t kMaximumSemanticActors = 16384;
constexpr std::int32_t kMaximumSemanticActorProbe = 32;
constexpr std::uint64_t kMaximumUFunctionHeaderOffset = 4096;

SymbolValidationResult ValidateObjectRegistry(
    const BuildProfile& profile,
    std::uintptr_t address,
    const SymbolMemory& memory);

bool SemanticLayoutValue(
    const BuildProfile& profile,
    std::string_view key,
    std::uint64_t& value,
    std::string& error) {
    if (!LayoutValue(profile, key, value, error)) return false;
    if (value > kMaximumSemanticLayoutOffset) {
        error = "profile layout " + std::string(key) + " exceeds the supported bound";
        return false;
    }
    return true;
}

template <typename Value>
bool ReadAt(
    const SymbolMemory& memory,
    std::uintptr_t base,
    std::uint64_t offset,
    Value& value,
    std::string_view label,
    std::string& error) {
    std::uintptr_t address{};
    if (!AddAddress(base, offset, address) || !memory.Read(address, &value, sizeof(value))) {
        error = std::string(label) + " is unreadable";
        return false;
    }
    return true;
}

bool ValidatePointer(
    const SymbolMemory& memory,
    std::uintptr_t pointer,
    std::string_view label,
    std::string& error) {
    if (pointer == 0) {
        error = std::string(label) + " is null";
        return false;
    }
    if ((pointer & (alignof(std::uintptr_t) - 1U)) != 0) {
        error = std::string(label) + " is misaligned";
        return false;
    }
    const auto readable = ReadableAddress(pointer, memory);
    if (!readable.valid) {
        error = std::string(label) + " is unreadable";
        return false;
    }
    return true;
}

bool ReadPointerAt(
    const SymbolMemory& memory,
    std::uintptr_t base,
    std::uint64_t offset,
    std::uintptr_t& pointer,
    std::string_view label,
    std::string& error) {
    return ReadAt(memory, base, offset, pointer, label, error) &&
        ValidatePointer(memory, pointer, label, error);
}

bool FiniteVector(const std::array<double, 3>& values) noexcept {
    return std::ranges::all_of(values, [](double value) { return std::isfinite(value); });
}

bool RuntimeObjectInitializationPending(const std::string_view message) noexcept {
    return message == "world pointer is null" ||
        message == "world game instance is null" ||
        message == "world persistent level is null" ||
        message == "world is not initialized" ||
        message == "persistent level is not initialized" ||
        message == "object registry is not initialized" ||
        message == "local player chain is incomplete";
}

FeatureValidationResult FeatureFailure(std::string message) {
    const bool deferred = RuntimeObjectInitializationPending(message);
    return {false, std::move(message), deferred};
}

struct NtePlayerLayout {
    std::uint64_t world_game_instance{};
    std::uint64_t game_instance_local_players{};
    std::uint64_t local_player_controller{};
    std::uint64_t controller_pawn{};
    std::uint64_t actor_root_component{};
    std::uint64_t scene_component_location{};
};

struct NtePlayerChain {
    std::uintptr_t world{};
    std::uintptr_t controller{};
    std::uintptr_t pawn{};
    std::uintptr_t root{};
};

bool ReadNtePlayerLayout(
    const BuildProfile& profile,
    NtePlayerLayout& layout,
    std::string& error) {
    return SemanticLayoutValue(profile, "world.gameInstance", layout.world_game_instance, error) &&
        SemanticLayoutValue(
            profile, "gameInstance.localPlayers", layout.game_instance_local_players, error) &&
        SemanticLayoutValue(
            profile, "localPlayer.controller", layout.local_player_controller, error) &&
        SemanticLayoutValue(profile, "controller.pawn", layout.controller_pawn, error) &&
        SemanticLayoutValue(profile, "actor.rootComponent", layout.actor_root_component, error) &&
        SemanticLayoutValue(
            profile, "sceneComponent.location", layout.scene_component_location, error);
}

bool ReadNteWorld(
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory,
    std::uintptr_t& world,
    std::string& error) {
    const ResolvedSymbol* world_symbol = snapshot.FindSymbol("ue5.GWorld");
    if (world_symbol == nullptr || !world_symbol->Available()) {
        error = "ue5.GWorld is unavailable";
        return false;
    }
    return ReadPointerAt(memory, world_symbol->address, 0, world, "world pointer", error);
}

bool ReadNtePlayerChain(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory,
    NtePlayerChain& chain,
    bool& complete,
    std::string& error) {
    complete = false;
    NtePlayerLayout layout;
    if (!ReadNtePlayerLayout(profile, layout, error) ||
        !ReadNteWorld(snapshot, memory, chain.world, error)) {
        return false;
    }
    std::uintptr_t game_instance{};
    std::uintptr_t local_players{};
    std::uintptr_t players_field{};
    std::int32_t player_count{};
    std::uintptr_t local_player{};
    if (!ReadPointerAt(
            memory, chain.world, layout.world_game_instance, game_instance,
            "world game instance", error) ||
        !AddAddress(game_instance, layout.game_instance_local_players, players_field) ||
        !ReadAt(memory, game_instance, layout.game_instance_local_players, local_players,
                "game instance local players", error) ||
        !ReadAt(
            memory, players_field, sizeof(std::uintptr_t), player_count,
            "game instance local player count", error)) {
        if (error.empty()) error = "game instance local players address overflows";
        return false;
    }
    if (player_count < 0 || player_count > kMaximumSemanticLocalPlayers) {
        error = "game instance local player count is outside the supported bound";
        return false;
    }
    if (player_count == 0) {
        if (local_players != 0 &&
            !ValidatePointer(memory, local_players, "game instance local players", error)) {
            return false;
        }
        return true;
    }
    if (!ValidatePointer(memory, local_players, "game instance local players", error) ||
        !ReadAt(memory, local_players, 0, local_player, "local player", error)) {
        return false;
    }
    if (local_player == 0) return true;
    if (!ValidatePointer(memory, local_player, "local player", error) ||
        !ReadAt(memory, local_player, layout.local_player_controller, chain.controller,
                "local player controller", error)) {
        return false;
    }
    if (chain.controller == 0) return true;
    if (!ValidatePointer(memory, chain.controller, "local player controller", error) ||
        !ReadAt(memory, chain.controller, layout.controller_pawn, chain.pawn,
                "controller pawn", error)) {
        return false;
    }
    if (chain.pawn == 0) return true;
    if (!ValidatePointer(memory, chain.pawn, "controller pawn", error) ||
        !ReadAt(memory, chain.pawn, layout.actor_root_component, chain.root,
                "pawn root component", error)) {
        return false;
    }
    if (chain.root == 0) return true;
    if (!ValidatePointer(memory, chain.root, "pawn root component", error)) return false;
    std::array<double, 3> position{};
    if (!ReadAt(
            memory, chain.root, layout.scene_component_location, position,
            "player scene location", error)) {
        return false;
    }
    if (!FiniteVector(position)) {
        error = "player scene location is not finite";
        return false;
    }
    complete = true;
    return true;
}

FeatureValidationResult ValidateNtePlayerLayout(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    NtePlayerChain chain;
    bool complete{};
    std::string error;
    return ReadNtePlayerChain(profile, snapshot, memory, chain, complete, error)
        ? FeatureValidationResult{true, {}}
        : FeatureFailure(std::move(error));
}

FeatureValidationResult ValidateNtePlayerTeleport(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    NtePlayerChain chain;
    bool complete{};
    std::string error;
    if (!ReadNtePlayerChain(profile, snapshot, memory, chain, complete, error)) {
        return FeatureFailure(std::move(error));
    }
    if (!complete) return FeatureFailure("local player chain is incomplete");

    // This validates only the reflected parameter layout. Invocation itself
    // requires a separately verified engine-owned bridge in the adapter.
    constexpr std::array<std::string_view, 17> kRequiredLayout{
        "object.class",
        "object.nameOffset",
        "object.outer",
        "ustruct.propertyLink",
        "ufunction.numParms",
        "ufunction.parmsSize",
        "ufunction.returnValueOffset",
        "ffield.name",
        "fproperty.arrayDim",
        "fproperty.elementSize",
        "fproperty.offsetInternal",
        "fproperty.propertyLinkNext",
        "fstructProperty.struct",
        "fboolProperty.fieldSize",
        "fboolProperty.byteOffset",
        "fboolProperty.byteMask",
        "fboolProperty.fieldMask"};
    for (const std::string_view key : kRequiredLayout) {
        std::uint64_t value{};
        if (!SemanticLayoutValue(profile, key, value, error)) {
            return FeatureFailure(std::move(error));
        }
    }

    return {true, {}};
}

FeatureValidationResult ValidateUe5ActorsReflection(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    constexpr std::array<std::string_view, 9> kRequiredLayout{
        "world.persistentLevel",
        "level.actors",
        "object.class",
        "object.nameOffset",
        "object.outer",
        "names.blocksOffset",
        "names.blockBits",
        "names.entryStride",
        "names.headerLengthShift",
    };
    std::string error;
    for (const std::string_view key : kRequiredLayout) {
        std::uint64_t value{};
        if (!SemanticLayoutValue(profile, key, value, error)) {
            return {false, std::move(error)};
        }
    }

    const auto* const world_symbol = snapshot.FindSymbol("ue5.GWorld");
    if (world_symbol == nullptr || !world_symbol->Available()) {
        return {false, "ue5.GWorld is unavailable"};
    }

    std::uintptr_t world{};
    if (!memory.Read(world_symbol->address, &world, sizeof(world))) {
        return {false, "world pointer storage is unreadable"};
    }
    // The proxy can initialize before UE has constructed a world. Keep the
    // optional feature unavailable until the game-thread revalidation runs.
    if (world == 0) return FeatureFailure("world is not initialized");
    if (!ValidatePointer(memory, world, "world pointer", error)) {
        return FeatureFailure(std::move(error));
    }

    std::uint64_t persistent_level_offset{};
    std::uint64_t actors_offset{};
    std::uint64_t class_offset{};
    std::uint64_t name_offset{};
    std::uint64_t outer_offset{};
    if (!SemanticLayoutValue(
            profile, "world.persistentLevel", persistent_level_offset, error) ||
        !SemanticLayoutValue(profile, "level.actors", actors_offset, error) ||
        !SemanticLayoutValue(profile, "object.class", class_offset, error) ||
        !SemanticLayoutValue(profile, "object.nameOffset", name_offset, error) ||
        !SemanticLayoutValue(profile, "object.outer", outer_offset, error)) {
        return FeatureFailure(std::move(error));
    }

    std::uintptr_t level{};
    if (!ReadAt(
            memory, world, persistent_level_offset, level, "world persistent level", error)) {
        return FeatureFailure(std::move(error));
    }
    if (level == 0) return FeatureFailure("persistent level is not initialized");
    if (!ValidatePointer(memory, level, "world persistent level", error)) {
        return FeatureFailure(std::move(error));
    }

    struct ActorArrayHeader final {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    } actors;
    if (!ReadAt(memory, level, actors_offset, actors, "level actor array", error)) {
        return FeatureFailure(std::move(error));
    }
    if (actors.count < 0 || actors.count > kMaximumSemanticActors ||
        actors.capacity < actors.count) {
        return FeatureFailure("level actor array is outside the supported bound");
    }
    if (actors.data == 0) {
        if (actors.count != 0) return FeatureFailure("level actor array data is null");
        return {true, {}};
    }
    if (!ValidatePointer(memory, actors.data, "level actor array data", error)) {
        return FeatureFailure(std::move(error));
    }

    const auto probe_count = (std::min)(actors.count, kMaximumSemanticActorProbe);
    for (std::int32_t index{}; index < probe_count; ++index) {
        std::uintptr_t actor{};
        if (!ReadAt(
                memory, actors.data,
                static_cast<std::uint64_t>(index) * sizeof(std::uintptr_t), actor,
                "actor array slot", error)) {
            return FeatureFailure(std::move(error));
        }
        if (actor == 0) continue;
        if (!ValidatePointer(memory, actor, "actor", error)) return FeatureFailure(std::move(error));

        std::uintptr_t class_object{};
        std::uintptr_t outer{};
        std::uint32_t name{};
        if (!ReadAt(memory, actor, class_offset, class_object, "actor class", error) ||
            !ReadAt(memory, actor, name_offset, name, "actor name", error) ||
            !ReadAt(memory, actor, outer_offset, outer, "actor outer", error)) {
            return FeatureFailure(std::move(error));
        }
        if (!ValidatePointer(memory, class_object, "actor class", error)) {
            return FeatureFailure(std::move(error));
        }
        if (outer != 0 && !ValidatePointer(memory, outer, "actor outer", error)) {
            return FeatureFailure(std::move(error));
        }
        static_cast<void>(name);
        break;
    }
    return {true, {}};
}

FeatureValidationResult ValidateUe5FunctionsReflection(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    constexpr std::array<std::string_view, 20> kRequiredLayout{
        "object.class",
        "object.nameOffset",
        "object.outer",
        "ufunction.numParms",
        "ufunction.parmsSize",
        "ufunction.returnValueOffset",
        "names.blocksOffset",
        "names.blockBits",
        "names.entryStride",
        "names.headerLengthShift",
        "objects.itemsOffset",
        "objects.maxCountOffset",
        "objects.countOffset",
        "objects.maxChunksOffset",
        "objects.numChunksOffset",
        "objects.chunkCountSize",
        "objects.chunkSize",
        "objects.itemStride",
        "objects.objectOffset",
        "objects.serialOffset",
    };
    std::string error;
    for (const std::string_view key : kRequiredLayout) {
        std::uint64_t value{};
        if (!SemanticLayoutValue(profile, key, value, error)) {
            return {false, std::move(error)};
        }
    }

    for (const std::string_view key : {
             "ufunction.numParms", "ufunction.parmsSize", "ufunction.returnValueOffset"}) {
        std::uint64_t value{};
        if (!SemanticLayoutValue(profile, key, value, error)) {
            return {false, std::move(error)};
        }
        if (value > kMaximumUFunctionHeaderOffset) {
            return {false, "UFunction layout offset exceeds the supported bound"};
        }
    }

    const auto* const objects = snapshot.FindSymbol("ue5.GObjects");
    if (objects == nullptr || !objects->Available()) {
        return {false, "ue5.GObjects is unavailable"};
    }
    const auto registry = ValidateObjectRegistry(profile, objects->address, memory);
    if (!registry.valid) {
        return {false, "object registry validation failed: " + registry.message};
    }

    std::uint64_t items_offset{};
    std::uint64_t count_offset{};
    std::uint64_t max_count_offset{};
    std::uint64_t max_chunks_offset{};
    std::uint64_t num_chunks_offset{};
    std::uint64_t chunk_count_size{};
    if (!SemanticLayoutValue(profile, "objects.itemsOffset", items_offset, error) ||
        !SemanticLayoutValue(profile, "objects.countOffset", count_offset, error) ||
        !SemanticLayoutValue(profile, "objects.maxCountOffset", max_count_offset, error) ||
        !SemanticLayoutValue(profile, "objects.maxChunksOffset", max_chunks_offset, error) ||
        !SemanticLayoutValue(profile, "objects.numChunksOffset", num_chunks_offset, error) ||
        !SemanticLayoutValue(profile, "objects.chunkCountSize", chunk_count_size, error)) {
        return {false, std::move(error)};
    }
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
    if (!ReadAt(memory, objects->address, items_offset, items, "objects.items", error) ||
        !ReadAt(memory, objects->address, count_offset, count, "objects.count", error) ||
        !ReadAt(memory, objects->address, max_count_offset, max_count, "objects.maxCount", error)) {
        return {false, std::move(error)};
    }
    if (chunk_count_size == sizeof(std::uint16_t)) {
        std::uint16_t packed_max_chunks{};
        std::uint16_t packed_num_chunks{};
        if (!ReadAt(
                memory, objects->address, max_chunks_offset, packed_max_chunks,
                "objects.maxChunks", error) ||
            !ReadAt(
                memory, objects->address, num_chunks_offset, packed_num_chunks,
                "objects.numChunks", error)) {
            return {false, std::move(error)};
        }
        max_chunks = packed_max_chunks;
        num_chunks = packed_num_chunks;
    } else if (chunk_count_size == sizeof(std::uint32_t)) {
        if (!ReadAt(
                memory, objects->address, max_chunks_offset, max_chunks,
                "objects.maxChunks", error) ||
            !ReadAt(
                memory, objects->address, num_chunks_offset, num_chunks,
                "objects.numChunks", error)) {
            return {false, std::move(error)};
        }
    } else {
        return {false, "objects.chunkCountSize must be 2 or 4"};
    }
    if (items == 0 && count == 0 && max_count == 0 && max_chunks == 0 && num_chunks == 0) {
        return {false, "object registry is not initialized"};
    }
    return {true, {}};
}

FeatureValidationResult ValidateUe5AhudReflection(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot&,
    const SymbolMemory&) {
    constexpr std::array<std::string_view, 13> kRequiredLayout{
        "ustruct.propertyLink",
        "ffield.class",
        "ffield.name",
        "ffieldClass.name",
        "fproperty.arrayDim",
        "fproperty.elementSize",
        "fproperty.offsetInternal",
        "fproperty.propertyLinkNext",
        "fstructProperty.struct",
        "fboolProperty.fieldSize",
        "fboolProperty.byteOffset",
        "fboolProperty.byteMask",
        "fboolProperty.fieldMask",
    };
    std::string error;
    for (const std::string_view key : kRequiredLayout) {
        std::uint64_t value{};
        if (!SemanticLayoutValue(profile, key, value, error)) {
            return {false, std::move(error)};
        }
        if (value > kMaximumUFunctionHeaderOffset) {
            return {false, "AHUD reflection layout offset exceeds the supported bound"};
        }
    }
    return {true, {}};
}

FeatureValidationResult ValidateNtePlayerEspLayout(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    std::uint64_t camera_manager_offset{};
    std::uint64_t bounds_origin_offset{};
    std::uint64_t bounds_extent_offset{};
    std::uint64_t camera_location_offset{};
    std::uint64_t camera_rotation_offset{};
    std::uint64_t camera_fov_offset{};
    std::string error;
    if (!SemanticLayoutValue(
            profile, "controller.cameraManager", camera_manager_offset, error) ||
        !SemanticLayoutValue(
            profile, "sceneComponent.boundsOrigin", bounds_origin_offset, error) ||
        !SemanticLayoutValue(
            profile, "sceneComponent.boundsExtent", bounds_extent_offset, error) ||
        !SemanticLayoutValue(
            profile, "cameraManager.location", camera_location_offset, error) ||
        !SemanticLayoutValue(
            profile, "cameraManager.rotation", camera_rotation_offset, error) ||
        !SemanticLayoutValue(profile, "cameraManager.fov", camera_fov_offset, error)) {
        return {false, std::move(error)};
    }

    NtePlayerChain chain;
    bool complete{};
    if (!ReadNtePlayerChain(profile, snapshot, memory, chain, complete, error)) {
        return {false, std::move(error)};
    }
    if (!complete) return {true, {}};
    std::uintptr_t camera_manager{};
    std::array<double, 3> bounds_origin{};
    std::array<double, 3> bounds_extent{};
    std::array<double, 3> camera_location{};
    std::array<double, 3> camera_rotation{};
    float camera_fov{};
    if (!ReadPointerAt(
            memory, chain.controller, camera_manager_offset, camera_manager,
            "controller camera manager", error) ||
        !ReadAt(memory, chain.root, bounds_origin_offset, bounds_origin,
                "player bounds origin", error) ||
        !ReadAt(memory, chain.root, bounds_extent_offset, bounds_extent,
                "player bounds extent", error) ||
        !ReadAt(memory, camera_manager, camera_location_offset, camera_location,
                "camera location", error) ||
        !ReadAt(memory, camera_manager, camera_rotation_offset, camera_rotation,
                "camera rotation", error) ||
        !ReadAt(memory, camera_manager, camera_fov_offset, camera_fov,
                "camera horizontal FOV", error)) {
        return {false, std::move(error)};
    }
    if (!FiniteVector(bounds_origin) || !FiniteVector(bounds_extent) ||
        !FiniteVector(camera_location) || !FiniteVector(camera_rotation) ||
        !std::isfinite(camera_fov) ||
        std::ranges::any_of(bounds_extent, [](double value) { return value <= 0.0; }) ||
        camera_fov <= 5.0F || camera_fov >= 175.0F) {
        return {false, "camera or bounds shape is implausible"};
    }
    return {true, {}};
}

FeatureValidationResult ValidateNteEntitiesLayout(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    std::string error;
    std::uintptr_t world{};
    if (!ReadNteWorld(snapshot, memory, world, error)) return FeatureFailure(std::move(error));

    std::uint64_t persistent_level_offset{};
    std::uint64_t actors_offset{};
    std::uint64_t root_component_offset{};
    std::uint64_t bounds_origin_offset{};
    std::uint64_t bounds_extent_offset{};
    std::uint64_t mobility_offset{};
    std::uint64_t internal_index_offset{};
    std::uint64_t class_offset{};
    std::uint64_t name_offset{};
    if (!SemanticLayoutValue(profile, "world.persistentLevel", persistent_level_offset, error) ||
        !SemanticLayoutValue(profile, "level.actors", actors_offset, error) ||
        !SemanticLayoutValue(profile, "actor.rootComponent", root_component_offset, error) ||
        !SemanticLayoutValue(profile, "sceneComponent.boundsOrigin", bounds_origin_offset, error) ||
        !SemanticLayoutValue(profile, "sceneComponent.boundsExtent", bounds_extent_offset, error) ||
        !SemanticLayoutValue(profile, "sceneComponent.mobility", mobility_offset, error) ||
        !SemanticLayoutValue(profile, "object.internalIndex", internal_index_offset, error) ||
        !SemanticLayoutValue(profile, "object.class", class_offset, error) ||
        !SemanticLayoutValue(profile, "object.nameOffset", name_offset, error)) {
        return FeatureFailure(std::move(error));
    }

    std::uintptr_t level{};
    std::uintptr_t actors{};
    std::uintptr_t actors_field{};
    std::int32_t actor_count{};
    if (!ReadPointerAt(memory, world, persistent_level_offset, level,
                       "world persistent level", error) ||
        !AddAddress(level, actors_offset, actors_field) ||
        !ReadAt(memory, level, actors_offset, actors, "level actors", error) ||
        !ReadAt(memory, actors_field, sizeof(std::uintptr_t), actor_count,
                "level actor count", error)) {
        if (error.empty()) error = "level actor array address overflows";
        return FeatureFailure(std::move(error));
    }
    if (actor_count < 0 || actor_count > kMaximumSemanticActors) {
        return FeatureFailure("level actor count is outside the supported bound");
    }
    if (actor_count == 0) {
        if (actors != 0 && !ValidatePointer(memory, actors, "level actors", error)) {
            return FeatureFailure(std::move(error));
        }
        return {true, {}};
    }
    if (!ValidatePointer(memory, actors, "level actors", error)) {
        return FeatureFailure(std::move(error));
    }

    const auto probe_count = (std::min)(actor_count, kMaximumSemanticActorProbe);
    for (std::int32_t index{}; index < probe_count; ++index) {
        const std::uint64_t slot = static_cast<std::uint64_t>(index) * sizeof(std::uintptr_t);
        std::uintptr_t actor{};
        if (!ReadAt(memory, actors, slot, actor, "actor array slot", error)) {
            return FeatureFailure(std::move(error));
        }
        if (actor == 0) continue;
        if (!ValidatePointer(memory, actor, "actor", error)) return FeatureFailure(std::move(error));

        std::uintptr_t root{};
        if (!ReadAt(memory, actor, root_component_offset, root, "actor root component", error)) {
            return FeatureFailure(std::move(error));
        }
        if (root == 0) continue;
        if (!ValidatePointer(memory, root, "actor root component", error)) {
            return FeatureFailure(std::move(error));
        }

        std::array<double, 3> bounds_origin{};
        std::array<double, 3> bounds_extent{};
        std::uint8_t mobility{};
        std::int32_t internal_index{};
        std::uint32_t actor_name{};
        std::uintptr_t class_object{};
        if (!ReadAt(memory, root, bounds_origin_offset, bounds_origin,
                    "actor bounds origin", error) ||
            !ReadAt(memory, root, bounds_extent_offset, bounds_extent,
                    "actor bounds extent", error) ||
            !ReadAt(memory, root, mobility_offset, mobility, "actor mobility", error) ||
            !ReadAt(memory, actor, internal_index_offset, internal_index,
                    "actor internal index", error) ||
            !ReadAt(memory, actor, name_offset, actor_name, "actor name", error) ||
            !ReadAt(memory, actor, class_offset, class_object, "actor class", error)) {
            return FeatureFailure(std::move(error));
        }
        if (class_object == 0) continue;
        if (!ValidatePointer(memory, class_object, "actor class", error)) {
            return {false, std::move(error)};
        }
        std::int32_t class_index{};
        std::uint32_t class_name{};
        if (!ReadAt(memory, class_object, internal_index_offset, class_index,
                    "actor class internal index", error) ||
            !ReadAt(memory, class_object, name_offset, class_name,
                    "actor class name", error)) {
            return {false, std::move(error)};
        }
        if (!FiniteVector(bounds_origin) || !FiniteVector(bounds_extent) ||
            std::ranges::any_of(bounds_extent, [](double value) {
                return value <= 0.0 || value > 1000000000.0;
            })) {
            return FeatureFailure("actor bounds shape is implausible");
        }
        static_cast<void>(mobility);
        static_cast<void>(internal_index);
        static_cast<void>(actor_name);
        static_cast<void>(class_index);
        static_cast<void>(class_name);
        return {true, {}};
    }
    return {true, {}};
}

FeatureValidationResult ValidateNteEntitiesAllLevelsLayout(
    const BuildProfile& profile,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) {
    const FeatureValidationResult base = ValidateNteEntitiesLayout(profile, snapshot, memory);
    if (!base.valid) return base;

    std::string error;
    std::uintptr_t world{};
    if (!ReadNteWorld(snapshot, memory, world, error)) return FeatureFailure(std::move(error));
    std::uint64_t levels_offset{};
    std::uint64_t actors_offset{};
    std::uint64_t maximum_levels{};
    std::uint64_t maximum_actors{};
    if (!SemanticLayoutValue(profile, "world.levels", levels_offset, error) ||
        !SemanticLayoutValue(profile, "level.actors", actors_offset, error) ||
        !SemanticLayoutValue(profile, "entities.maxLevels", maximum_levels, error) ||
        !SemanticLayoutValue(profile, "entities.maxCount", maximum_actors, error)) {
        return FeatureFailure(std::move(error));
    }
    if (maximum_levels == 0 || maximum_levels > 4096 || maximum_actors == 0 ||
        maximum_actors > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        return FeatureFailure("entity traversal bounds are outside the supported range");
    }

    std::uintptr_t levels{};
    std::int32_t level_count{};
    if (!ReadAt(memory, world, levels_offset, levels, "world levels", error) ||
        !ReadAt(
            memory, world, levels_offset + sizeof(std::uintptr_t), level_count,
            "world level count", error)) {
        return FeatureFailure(std::move(error));
    }
    if (level_count < 0 || static_cast<std::uint64_t>(level_count) > maximum_levels ||
        (level_count != 0 && !ValidatePointer(memory, levels, "world levels", error))) {
        return FeatureFailure(
            error.empty() ? "world level count is outside the supported bound" : std::move(error));
    }

    std::uint64_t total_actors{};
    for (std::int32_t index = 0; index < level_count; ++index) {
        std::uintptr_t level{};
        if (!ReadAt(
                memory, levels,
                static_cast<std::uint64_t>(index) * sizeof(std::uintptr_t), level,
                "world level entry", error) ||
            level == 0) {
            return FeatureFailure(error.empty() ? "world level entry is null" : std::move(error));
        }
        std::uintptr_t actors{};
        std::int32_t actor_count{};
        if (!ReadAt(memory, level, actors_offset, actors, "level actors", error) ||
            !ReadAt(
                memory, level, actors_offset + sizeof(std::uintptr_t), actor_count,
                "level actor count", error)) {
            return FeatureFailure(std::move(error));
        }
        if (actor_count < 0 ||
            static_cast<std::uint64_t>(actor_count) > maximum_actors - total_actors ||
            (actor_count != 0 && !ValidatePointer(memory, actors, "level actors", error))) {
            return FeatureFailure(
                error.empty() ? "loaded-level actor count exceeds the supported bound" :
                                std::move(error));
        }
        total_actors += static_cast<std::uint64_t>(actor_count);
    }

    constexpr std::array<std::string_view, 11> kReflectionLayout{
        "object.class",
        "ustruct.propertyLink",
        "ffield.name",
        "fproperty.arrayDim",
        "fproperty.elementSize",
        "fproperty.offsetInternal",
        "fproperty.propertyLinkNext",
        "fboolProperty.fieldSize",
        "fboolProperty.byteOffset",
        "fboolProperty.byteMask",
        "fboolProperty.fieldMask"};
    for (const std::string_view key : kReflectionLayout) {
        std::uint64_t value{};
        if (!SemanticLayoutValue(profile, key, value, error)) {
            return FeatureFailure(std::move(error));
        }
    }
    return {true, {}};
}

SymbolValidationResult ValidateObjectRegistry(
    const BuildProfile& profile,
    std::uintptr_t address,
    const SymbolMemory& memory) {
    std::uint64_t items_offset{};
    std::uint64_t count_offset{};
    std::uint64_t max_count_offset{};
    std::uint64_t max_chunks_offset{};
    std::uint64_t num_chunks_offset{};
    std::uint64_t chunk_count_size{sizeof(std::uint32_t)};
    std::uint64_t chunk_size{};
    std::uint64_t item_stride{};
    std::uint64_t object_offset{};
    std::uint64_t serial_offset{};
    std::string error;
    const std::array<std::pair<std::string_view, std::uint64_t*>, 9> layout_values{{
        {"objects.itemsOffset", &items_offset},
        {"objects.countOffset", &count_offset},
        {"objects.maxCountOffset", &max_count_offset},
        {"objects.maxChunksOffset", &max_chunks_offset},
        {"objects.numChunksOffset", &num_chunks_offset},
        {"objects.chunkSize", &chunk_size},
        {"objects.itemStride", &item_stride},
        {"objects.objectOffset", &object_offset},
        {"objects.serialOffset", &serial_offset},
    }};
    for (const auto [key, output] : layout_values) {
        if (!LayoutValue(profile, key, *output, error)) return {false, std::move(error)};
    }
    if (const auto found = profile.layout.find("objects.chunkCountSize");
        found != profile.layout.end()) {
        if (found->second < 0) {
            return {false, "objects.chunkCountSize is negative"};
        }
        chunk_count_size = static_cast<std::uint64_t>(found->second);
    }

    constexpr std::uint64_t kMaximumHeaderOffset = 4096;
    constexpr std::uint64_t kMaximumObjects = 16ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kMaximumChunks = 4096;
    if (items_offset > kMaximumHeaderOffset || count_offset > kMaximumHeaderOffset ||
        max_count_offset > kMaximumHeaderOffset || max_chunks_offset > kMaximumHeaderOffset ||
        num_chunks_offset > kMaximumHeaderOffset) {
        return {false, "object registry header offset exceeds the supported bound"};
    }
    if (chunk_size == 0 || chunk_size > kMaximumObjects) {
        return {false, "objects.chunkSize is outside the supported bound"};
    }
    if ((chunk_size & (chunk_size - 1U)) != 0) {
        return {false, "objects.chunkSize is not a power of two"};
    }
    if (item_stride < sizeof(std::uintptr_t) || item_stride > 4096) {
        return {false, "objects.itemStride is outside the supported bound"};
    }
    if (chunk_count_size != sizeof(std::uint16_t) &&
        chunk_count_size != sizeof(std::uint32_t)) {
        return {false, "objects.chunkCountSize must be 2 or 4"};
    }
    if (object_offset > item_stride - sizeof(std::uintptr_t)) {
        return {false, "objects.objectOffset exceeds the item stride"};
    }
    if (serial_offset > item_stride - sizeof(std::uint32_t)) {
        return {false, "objects.serialOffset exceeds the item stride"};
    }

    const auto read_header = [&](std::uint64_t offset, auto& value, std::string_view label) {
        std::uintptr_t field{};
        if (!AddAddress(address, offset, field) || !memory.Read(field, &value, sizeof(value))) {
            error = std::string(label) + " is unreadable";
            return false;
        }
        return true;
    };
    std::uintptr_t items{};
    std::uint32_t count{};
    std::uint32_t max_count{};
    std::uint32_t max_chunks{};
    std::uint32_t num_chunks{};
    const auto read_chunk_count = [&](std::uint64_t offset, std::uint32_t& value,
                                      std::string_view label) {
        if (chunk_count_size == sizeof(std::uint16_t)) {
            std::uint16_t packed{};
            if (!read_header(offset, packed, label)) return false;
            value = packed;
            return true;
        }
        return read_header(offset, value, label);
    };
    if (!read_header(items_offset, items, "objects.items") ||
        !read_header(count_offset, count, "objects.count") ||
        !read_header(max_count_offset, max_count, "objects.maxCount") ||
        !read_chunk_count(max_chunks_offset, max_chunks, "objects.maxChunks") ||
        !read_chunk_count(num_chunks_offset, num_chunks, "objects.numChunks")) {
        return {false, std::move(error)};
    }
    // FObjectArray is zero-initialized before the engine starts publishing
    // objects. The symbol remains valid; the optional reflection feature
    // reports itself unavailable until a later game-thread revalidation.
    if (items == 0 && count == 0 && max_count == 0 && max_chunks == 0 && num_chunks == 0) {
        return {true, {}};
    }
    if (items == 0 || (items & (alignof(std::uintptr_t) - 1U)) != 0) {
        return {false, "objects.items is null or misaligned"};
    }
    if (max_count == 0 || max_count > kMaximumObjects) {
        return {false, "objects.maxCount is outside the supported bound"};
    }
    if (count > max_count) return {false, "objects.count exceeds objects.maxCount"};
    if (max_chunks == 0 || max_chunks > kMaximumChunks) {
        return {false, "objects.maxChunks is outside the supported bound"};
    }
    if (num_chunks > max_chunks) {
        return {false, "objects.numChunks exceeds objects.maxChunks"};
    }
    if (chunk_size > (std::numeric_limits<std::uint64_t>::max)() / max_chunks ||
        max_count > chunk_size * max_chunks) {
        return {false, "objects.maxCount exceeds chunk capacity"};
    }
    const std::uint64_t required_chunks =
        (static_cast<std::uint64_t>(count) + chunk_size - 1U) / chunk_size;
    if (required_chunks > num_chunks) {
        return {false, "objects.count requires more chunks than objects.numChunks"};
    }
    if (count == 0) return {true, {}};

    const auto read_chunk = [&](std::uint64_t page, std::string_view label,
                                std::uintptr_t& chunk) {
        if (page > (std::numeric_limits<std::uint64_t>::max)() / sizeof(std::uintptr_t)) {
            error = std::string(label) + " page-table offset overflows";
            return false;
        }
        std::uintptr_t entry{};
        if (!AddAddress(items, page * sizeof(std::uintptr_t), entry) ||
            !memory.Read(entry, &chunk, sizeof(chunk))) {
            error = std::string(label) + " chunk pointer is unreadable";
            return false;
        }
        if (chunk == 0 || (chunk & (alignof(std::uintptr_t) - 1U)) != 0) {
            error = std::string(label) + " chunk pointer is null or misaligned";
            return false;
        }
        return true;
    };
    std::uintptr_t first_chunk{};
    std::uintptr_t last_chunk{};
    if (!read_chunk(0, "first", first_chunk) ||
        !read_chunk(required_chunks - 1U, "last", last_chunk)) {
        return {false, std::move(error)};
    }

    const auto read_slot = [&](std::uintptr_t chunk, std::uint64_t slot,
                               std::string_view label) {
        if (slot >= chunk_size || slot >
                (std::numeric_limits<std::uint64_t>::max)() / item_stride) {
            error = std::string(label) + " slot offset exceeds its chunk";
            return false;
        }
        std::uintptr_t item{};
        if (!AddAddress(chunk, slot * item_stride, item)) {
            error = std::string(label) + " item address overflows";
            return false;
        }
        std::uintptr_t object_field{};
        std::uintptr_t serial_field{};
        std::uintptr_t item_end{};
        std::uintptr_t object{};
        std::uint32_t serial{};
        std::uint8_t tail{};
        if (!AddAddress(item, object_offset, object_field) ||
            !AddAddress(item, serial_offset, serial_field) ||
            !AddAddress(item, item_stride - 1U, item_end) ||
            !memory.Read(object_field, &object, sizeof(object)) ||
            !memory.Read(serial_field, &serial, sizeof(serial)) ||
            !memory.Read(item_end, &tail, sizeof(tail))) {
            error = std::string(label) + " object item is unreadable";
            return false;
        }
        return true;
    };
    if (!read_slot(first_chunk, 0, "first") ||
        !read_slot(last_chunk, (count - 1U) % chunk_size, "last")) {
        return {false, std::move(error)};
    }
    return {true, {}};
}

std::optional<std::uintptr_t> ResolveAddress(
    const ProfileSymbol& symbol,
    std::uintptr_t instruction,
    const SymbolMemory& memory) {
    if (symbol.resolve.kind == ProfileResolveKind::Direct) {
        const auto value = static_cast<std::intptr_t>(instruction) + symbol.resolve.addend;
        if (value < 0) return std::nullopt;
        return static_cast<std::uintptr_t>(value);
    }
    if (symbol.resolve.instruction_size == 0 || symbol.resolve.offset > 64) {
        return std::nullopt;
    }
    std::int32_t displacement{};
    if (!memory.Read(
            instruction + symbol.resolve.offset,
            &displacement,
            sizeof(displacement))) {
        return std::nullopt;
    }
    const auto base = static_cast<std::intptr_t>(instruction) +
        static_cast<std::intptr_t>(symbol.resolve.instruction_size);
    const auto target = base + displacement + symbol.resolve.addend;
    return target < 0 ? std::nullopt
                      : std::optional<std::uintptr_t>(static_cast<std::uintptr_t>(target));
}

SymbolValidationResult ValidateAll(
    const BuildProfile& profile,
    const ProfileSymbol& symbol,
    const ue5mem::ModuleInfo& module,
    std::uintptr_t address,
    const SymbolMemory& memory,
    const SymbolValidatorRegistry& validators,
    std::vector<std::string>* diagnostics) {
    for (const auto& validator : symbol.validators) {
        auto result = validators.Validate(
            validator, profile, symbol, module, address, memory);
        if (!result.valid) {
            if (diagnostics != nullptr) {
                diagnostics->push_back(validator + ": " + result.message);
            }
            return result;
        }
    }
    return {true, {}};
}

bool FeatureResolutionsEqual(
    const std::map<std::string, FeatureResolution, std::less<>>& left,
    const std::map<std::string, FeatureResolution, std::less<>>& right) {
    if (left.size() != right.size()) return false;
    for (const auto& [id, expected] : left) {
        const auto actual = right.find(id);
        if (actual == right.end() || expected.id != actual->second.id ||
            expected.available != actual->second.available ||
            expected.deferred_validation != actual->second.deferred_validation ||
            expected.missing_symbols != actual->second.missing_symbols ||
            expected.unavailable_dependencies != actual->second.unavailable_dependencies ||
            expected.validation_diagnostics != actual->second.validation_diagnostics) {
            return false;
        }
    }
    return true;
}

void ResolveFeatures(
    const BuildProfile& profile,
    ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory,
    const FeatureLayoutValidatorRegistry& layout_validators) {
    enum class VisitState : std::uint8_t { Unvisited, Visiting, Completed };

    snapshot.features.clear();
    std::map<std::string, VisitState, std::less<>> visits;
    std::function<void(const std::string&)> resolve_feature;
    resolve_feature = [&](const std::string& id) {
        const auto definition = profile.features.find(id);
        if (definition == profile.features.end()) return;
        const VisitState visit = visits[id];
        if (visit == VisitState::Completed) return;
        if (visit == VisitState::Visiting) return;

        visits[id] = VisitState::Visiting;
        FeatureResolution feature;
        feature.id = id;
        bool validator_failed{};
        bool every_failed_validator_deferred = true;
        for (const auto& requirement : definition->second) {
            const auto found = snapshot.symbols.find(requirement);
            if (found == snapshot.symbols.end() || !found->second.Available()) {
                feature.missing_symbols.push_back(requirement);
            }
        }

        if (const auto dependencies = profile.feature_dependencies.find(id);
            dependencies != profile.feature_dependencies.end()) {
            for (const auto& dependency : dependencies->second) {
                if (!profile.features.contains(dependency)) {
                    feature.unavailable_dependencies.push_back(dependency);
                    feature.validation_diagnostics.push_back(
                        "unknown feature dependency " + dependency);
                    continue;
                }
                if (visits[dependency] == VisitState::Visiting) {
                    feature.unavailable_dependencies.push_back(dependency);
                    feature.validation_diagnostics.push_back(
                        "feature dependency cycle through " + dependency);
                    continue;
                }
                resolve_feature(dependency);
                const auto resolved = snapshot.features.find(dependency);
                if (resolved == snapshot.features.end() || !resolved->second.available) {
                    feature.unavailable_dependencies.push_back(dependency);
                }
            }
        }

        if (feature.missing_symbols.empty() && feature.unavailable_dependencies.empty()) {
            if (const auto validators = profile.feature_layout_validators.find(id);
                validators != profile.feature_layout_validators.end()) {
                for (const auto& validator : validators->second) {
                    const auto result = layout_validators.Validate(
                        validator, profile, id, snapshot, memory);
                    if (!result.valid) {
                        validator_failed = true;
                        every_failed_validator_deferred =
                            every_failed_validator_deferred && result.deferred;
                        feature.validation_diagnostics.push_back(
                            validator + ": " + result.message);
                    }
                }
            }
        }
        feature.available = feature.missing_symbols.empty() &&
            feature.unavailable_dependencies.empty() &&
            feature.validation_diagnostics.empty();
        feature.deferred_validation = !feature.available &&
            feature.missing_symbols.empty() && feature.unavailable_dependencies.empty() &&
            validator_failed && every_failed_validator_deferred;
        snapshot.features.insert_or_assign(id, std::move(feature));
        visits[id] = VisitState::Completed;
    };

    bool has_required_feature{};
    bool every_required_feature = true;
    for (const auto& [id, unused] : profile.features) {
        static_cast<void>(unused);
        resolve_feature(id);
    }
    for (const auto& [id, feature] : snapshot.features) {
        if (!profile.optional_features.contains(id)) {
            has_required_feature = true;
            every_required_feature = every_required_feature && feature.available;
        }
    }
    snapshot.state = has_required_feature && every_required_feature
        ? ProfileResolutionState::Ready
        : ProfileResolutionState::Degraded;
}

}  // namespace

LiveSymbolMemory::LiveSymbolMemory(CoreMemoryServices services)
    : services_(NormalizeCoreMemoryServices(std::move(services))) {}

std::optional<ue5mem::ModuleInfo> LiveSymbolMemory::FindModule(
    std::wstring_view name) const {
    return services_.memory->FindModule(name);
}

std::vector<ue5mem::SectionInfo> LiveSymbolMemory::Sections(
    const ue5mem::ModuleInfo& module) const {
    return services_.memory->EnumerateSections(module);
}

std::vector<std::uintptr_t> LiveSymbolMemory::Scan(
    const ue5mem::ModuleInfo& module,
    std::string_view section,
    std::string_view pattern,
    std::size_t limit) const {
    return services_.patterns->ScanSection(
        module, section, services_.patterns->Parse(pattern), limit);
}

bool LiveSymbolMemory::Read(
    std::uintptr_t address,
    void* destination,
    std::size_t size) const {
    return services_.memory->ReadMemoryInto(address, destination, size);
}

bool LiveSymbolMemory::Write(
    std::uintptr_t address,
    const void* source,
    std::size_t size) const {
    return services_.memory->WriteMemory(address, source, size);
}

std::optional<SymbolMemoryRegion> LiveSymbolMemory::Query(
    std::uintptr_t address) const {
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address),
            &information,
            sizeof(information)) == 0) {
        return std::nullopt;
    }
    return SymbolMemoryRegion{
        reinterpret_cast<std::uintptr_t>(information.BaseAddress),
        information.RegionSize,
        information.State,
        information.Protect,
        information.Type,
    };
}

SymbolValidatorRegistry::SymbolValidatorRegistry() {
    Register("address-in-module", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo& module,
        std::uintptr_t address, const SymbolMemory&) {
        return Contains(module.base, module.size, address)
            ? SymbolValidationResult{true, {}}
            : SymbolValidationResult{false, "address is outside the declared module"};
    });
    Register("readable", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        return ReadableAddress(address, memory);
    });
    Register("readable-pointer", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        return PointerShape(address, memory, false);
    });
    Register("nullable-readable-pointer", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        return PointerShape(address, memory, true);
    });
    Register("executable", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        const auto region = memory.Query(address);
        return region && region->state == MEM_COMMIT &&
            IsExecutableProtection(region->protection)
            ? SymbolValidationResult{true, {}}
            : SymbolValidationResult{false, "address is not executable"};
    });
    Register("function-prologue-v1", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        const auto region = memory.Query(address);
        std::array<std::uint8_t, 4> bytes{};
        if (!region || !IsExecutableProtection(region->protection) ||
            !memory.Read(address, bytes.data(), bytes.size())) {
            return SymbolValidationResult{false, "function prefix is unreadable or not executable"};
        }
        return bytes[0] != 0x00 && bytes[0] != 0xCC && bytes[0] != 0xC3
            ? SymbolValidationResult{true, {}}
            : SymbolValidationResult{false, "function prefix is implausible"};
    });
    Register("tick-anchor-v1", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        const auto region = memory.Query(address);
        std::array<std::uint8_t, 8> bytes{};
        if (!region || !IsExecutableProtection(region->protection) ||
            !memory.Read(address, bytes.data(), bytes.size())) {
            return SymbolValidationResult{false, "tick anchor is unreadable or not executable"};
        }
        const bool plausible = bytes[0] != 0x00 && bytes[0] != 0xCC && bytes[0] != 0xC3 &&
            std::ranges::any_of(bytes, [](std::uint8_t value) { return value != 0x90; });
        return plausible ? SymbolValidationResult{true, {}}
                         : SymbolValidationResult{false, "tick anchor prefix is implausible"};
    });
    Register("world-shape-v1", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        return PointerShape(address, memory, true);
    });
    Register("object-registry-v1", [](
        const BuildProfile& profile, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        return ValidateObjectRegistry(profile, address, memory);
    });
    Register("name-pool-v1", [](
        const BuildProfile&, const ProfileSymbol&, const ue5mem::ModuleInfo&,
        std::uintptr_t address, const SymbolMemory& memory) {
        return ReadableAddress(address, memory);
    });
}

void SymbolValidatorRegistry::Register(std::string id, Validator validator) {
    if (id.empty() || !validator) throw std::invalid_argument("invalid symbol validator");
    validators_.insert_or_assign(std::move(id), std::move(validator));
}

SymbolValidationResult SymbolValidatorRegistry::Validate(
    std::string_view id,
    const BuildProfile& profile,
    const ProfileSymbol& symbol,
    const ue5mem::ModuleInfo& module,
    std::uintptr_t address,
    const SymbolMemory& memory) const {
    constexpr std::string_view prefix = "address-in-section:";
    if (id.starts_with(prefix)) {
        const std::string_view section_name = id.substr(prefix.size());
        const auto sections = memory.Sections(module);
        const auto section = std::ranges::find_if(sections, [&](const auto& candidate) {
            return candidate.name == section_name;
        });
        if (section == sections.end()) return {false, "declared section is unavailable"};
        return Contains(section->base, section->virtual_size, address)
            ? SymbolValidationResult{true, {}}
            : SymbolValidationResult{false, "address is outside the declared section"};
    }
    const auto found = validators_.find(id);
    if (found == validators_.end()) return {false, "validator is not registered"};
    return found->second(profile, symbol, module, address, memory);
}

FeatureLayoutValidatorRegistry::FeatureLayoutValidatorRegistry() {
    Register("nte-player-layout-v1", [](
        const BuildProfile& profile,
        std::string_view,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) {
        return ValidateNtePlayerLayout(profile, snapshot, memory);
    });
    Register("nte-player-teleport-layout-v1", [](
        const BuildProfile& profile,
        std::string_view,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) {
        return ValidateNtePlayerTeleport(profile, snapshot, memory);
    });
    Register("nte-player-esp-layout-v1", [](
        const BuildProfile& profile,
        std::string_view,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) {
        return ValidateNtePlayerEspLayout(profile, snapshot, memory);
    });
    Register("nte-entities-layout-v1", [](
        const BuildProfile& profile,
        std::string_view,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) {
        return ValidateNteEntitiesLayout(profile, snapshot, memory);
    });
    Register("nte-entities-layout-v2", [](
        const BuildProfile& profile,
        std::string_view,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) {
        return ValidateNteEntitiesAllLevelsLayout(profile, snapshot, memory);
    });
    Register("ue5-actors-reflection-v1", [](
        const BuildProfile& profile,
        std::string_view,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) {
        return ValidateUe5ActorsReflection(profile, snapshot, memory);
    });
    Register("ue5-functions-reflection-v1", [](
        const BuildProfile& profile,
        std::string_view,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) {
        return ValidateUe5FunctionsReflection(profile, snapshot, memory);
    });
    Register("ue5-ahud-reflection-v1", [](
        const BuildProfile& profile,
        std::string_view,
        const ProfileResolutionSnapshot& snapshot,
        const SymbolMemory& memory) {
        return ValidateUe5AhudReflection(profile, snapshot, memory);
    });
}

void FeatureLayoutValidatorRegistry::Register(std::string id, Validator validator) {
    if (id.empty() || !validator) throw std::invalid_argument("invalid feature layout validator");
    validators_.insert_or_assign(std::move(id), std::move(validator));
}

FeatureValidationResult FeatureLayoutValidatorRegistry::Validate(
    std::string_view id,
    const BuildProfile& profile,
    std::string_view feature_id,
    const ProfileResolutionSnapshot& snapshot,
    const SymbolMemory& memory) const {
    const auto found = validators_.find(id);
    if (found == validators_.end()) return {false, "validator is not registered"};
    return found->second(profile, feature_id, snapshot, memory);
}

const ResolvedSymbol* ProfileResolutionSnapshot::FindSymbol(std::string_view id) const noexcept {
    const auto found = symbols.find(id);
    return found == symbols.end() ? nullptr : &found->second;
}

bool ProfileResolutionSnapshot::FeatureAvailable(std::string_view id) const noexcept {
    const auto found = features.find(id);
    return found != features.end() && found->second.available;
}

SymbolResolver::SymbolResolver(
    std::shared_ptr<const SymbolMemory> memory,
    SymbolValidatorRegistry validators,
    SymbolResolverOptions options,
    FeatureLayoutValidatorRegistry feature_layout_validators)
    : memory_(std::move(memory)),
      validators_(std::move(validators)),
      options_(options),
      feature_layout_validators_(std::move(feature_layout_validators)) {
    if (!memory_) throw std::invalid_argument("SymbolResolver requires memory");
    if (options_.maximum_candidates == 0) {
        throw std::invalid_argument("maximum_candidates must be positive");
    }
}

ProfileResolutionSnapshot SymbolResolver::Resolve(
    const BuildFingerprint& fingerprint,
    const BuildProfile* profile,
    const SymbolCache* cache) const {
    const auto started = std::chrono::steady_clock::now();
    ProfileResolutionSnapshot snapshot;
    snapshot.build_id = fingerprint.id;
    if (profile == nullptr) {
        snapshot.duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
        return snapshot;
    }
    snapshot.state = ProfileResolutionState::ProfileLoaded;
    snapshot.profile_hash = profile->source_hash;
    const auto cached = cache == nullptr ? std::optional<SymbolCacheRecord>{} : cache->Load();
    snapshot.cache_loaded = cached.has_value();
    SymbolCacheRecord next_cache;

    for (const auto& [id, definition] : profile->symbols) {
        ResolvedSymbol symbol;
        symbol.id = id;
        symbol.module = definition.module;
        const auto module = memory_->FindModule(definition.module);
        if (!module) {
            symbol.state = SymbolResolutionState::ModuleMissing;
            symbol.diagnostics.push_back("module is unavailable");
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        }
        const auto sections = memory_->Sections(*module);
        if (std::ranges::none_of(sections, [&](const auto& section) {
                return section.name == definition.section;
            })) {
            symbol.state = SymbolResolutionState::SectionMissing;
            symbol.diagnostics.push_back("section is unavailable");
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        }

        bool accepted_cache{};
        if (cached) {
            const auto cached_symbol = cached->symbols.find(id);
            if (cached_symbol != cached->symbols.end() &&
                EqualWideInsensitive(cached_symbol->second.module, module->name) &&
                cached_symbol->second.rva < module->size) {
                symbol.state = SymbolResolutionState::CacheTrusted;
                symbol.address = module->base + cached_symbol->second.rva;
                symbol.rva = cached_symbol->second.rva;
                next_cache.symbols.emplace(id, CachedSymbol{module->name, symbol.rva});
                accepted_cache = true;
            }
        }
        if (accepted_cache) {
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        }

        std::vector<std::uintptr_t> matches;
        try {
            matches = memory_->Scan(
                *module, definition.section, definition.pattern,
                options_.maximum_candidates + 1);
        } catch (const std::invalid_argument& exception) {
            symbol.state = SymbolResolutionState::PatternInvalid;
            symbol.diagnostics.push_back(exception.what());
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        } catch (const std::exception& exception) {
            symbol.state = SymbolResolutionState::NotFound;
            symbol.diagnostics.push_back(exception.what());
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        }
        symbol.candidate_count = matches.size();
        if (matches.empty()) {
            symbol.state = SymbolResolutionState::NotFound;
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        }
        if (matches.size() > options_.maximum_candidates) {
            symbol.state = SymbolResolutionState::Ambiguous;
            symbol.diagnostics.push_back("candidate limit exceeded");
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        }
        struct Candidate {
            std::uintptr_t instruction{};
            std::uintptr_t address{};
        };
        std::vector<Candidate> valid;
        std::optional<Candidate> deferred_candidate;
        bool address_failed{};
        bool validation_failed{};
        for (const auto instruction : matches) {
            const auto address = ResolveAddress(definition, instruction, *memory_);
            if (!address) {
                address_failed = true;
                continue;
            }
            std::vector<std::string> diagnostics;
            if (!ValidateAll(
                    *profile, definition, *module, *address, *memory_, validators_,
                    &diagnostics).valid) {
                validation_failed = true;
                if (matches.size() == 1) {
                    deferred_candidate = Candidate{instruction, *address};
                }
                for (auto& diagnostic : diagnostics) {
                    symbol.diagnostics.push_back(
                        "candidate " + std::to_string(instruction - module->base) + ": " +
                        std::move(diagnostic));
                }
                continue;
            }
            valid.push_back({instruction, *address});
        }
        if (valid.size() != 1) {
            symbol.state = valid.size() > 1
                ? SymbolResolutionState::Ambiguous
                : validation_failed
                    ? SymbolResolutionState::ValidationFailed
                    : address_failed
                        ? SymbolResolutionState::AddressResolutionFailed
                        : SymbolResolutionState::NotFound;
            if (symbol.state == SymbolResolutionState::ValidationFailed &&
                deferred_candidate.has_value()) {
                symbol.instruction = deferred_candidate->instruction;
                symbol.address = deferred_candidate->address;
            }
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        }
        symbol.state = SymbolResolutionState::Resolved;
        symbol.instruction = valid.front().instruction;
        symbol.address = valid.front().address;
        if (symbol.address < module->base || symbol.address - module->base >= module->size) {
            symbol.state = SymbolResolutionState::ValidationFailed;
            symbol.diagnostics.push_back("resolved address cannot be cached as an RVA");
            snapshot.symbols.emplace(id, std::move(symbol));
            continue;
        }
        symbol.rva = symbol.address - module->base;
        next_cache.symbols.emplace(id, CachedSymbol{module->name, symbol.rva});
        snapshot.symbols.emplace(id, std::move(symbol));
    }

    ResolveFeatures(*profile, snapshot, *memory_, feature_layout_validators_);
    if (cache != nullptr && !next_cache.symbols.empty()) {
        snapshot.cache_written = cache->Store(next_cache);
    }
    snapshot.duration = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    return snapshot;
}

bool SymbolResolver::RevalidateDeferredCandidates(
    const BuildProfile& profile,
    ProfileResolutionSnapshot& snapshot) const {
    if (snapshot.profile_hash != profile.source_hash) {
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto previous_features = snapshot.features;
    const ProfileResolutionState previous_state = snapshot.state;
    bool changed{};
    for (const auto& [id, definition] : profile.symbols) {
        const auto found = snapshot.symbols.find(id);
        if (found == snapshot.symbols.end()) continue;
        auto& symbol = found->second;
        if (symbol.state != SymbolResolutionState::ValidationFailed ||
            symbol.candidate_count != 1 || symbol.address == 0) {
            continue;
        }

        const auto module = memory_->FindModule(definition.module);
        if (!module || symbol.address < module->base ||
            symbol.address - module->base >= module->size) {
            continue;
        }
        std::vector<std::string> diagnostics;
        if (!ValidateAll(
                profile, definition, *module, symbol.address, *memory_, validators_,
                &diagnostics).valid) {
            continue;
        }

        symbol.state = SymbolResolutionState::Resolved;
        symbol.rva = symbol.address - module->base;
        symbol.diagnostics.clear();
        symbol.diagnostics.push_back("deferred candidate revalidated");
        changed = true;
    }
    if (profile.features.empty()) {
        if (!changed) return false;
    } else {
        ResolveFeatures(profile, snapshot, *memory_, feature_layout_validators_);
    }
    if (!changed && previous_state == snapshot.state &&
        FeatureResolutionsEqual(previous_features, snapshot.features)) {
        return false;
    }
    snapshot.cache_written = false;
    snapshot.duration += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    return true;
}

}  // namespace anomaly
