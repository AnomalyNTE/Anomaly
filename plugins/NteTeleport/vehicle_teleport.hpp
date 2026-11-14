#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/profile.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ue5.h"

// Vehicle-aware teleport.
//
// The host teleport bridge only moves the local pawn, and a rider is attached to a
// seat socket on the vehicle actor, so the seat snaps the pawn back on the next tick.
// This path moves the vehicle actor instead and keeps the seat offset between the
// vehicle origin and the rider. The registry and the reflected K2_SetActorLocation
// layout are resolved on the Game thread from compatibility.profile.json; any missing
// piece disables this path and leaves the pawn teleport in charge.
namespace nteteleport_vehicle {

// Same bound the plugin uses for its own layout values: UE property offsets and
// struct sizes stay far below it, and anything larger is a corrupt profile.
constexpr std::uint32_t kMaximumLayoutValue = 1048576;
constexpr std::size_t kMaximumNameBytes = 256;

template <typename Struct, typename Field>
[[nodiscard]] bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

// Services queried once by the plugin on the Lifecycle thread.
struct Services final {
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNtePlayerServiceV1* player{};

    [[nodiscard]] bool Complete() const noexcept {
        return core != nullptr && core->read_memory != nullptr &&
            names != nullptr && names->resolve_utf8 != nullptr &&
            objects != nullptr && objects->generation != nullptr &&
            objects->count != nullptr && objects->snapshot_at != nullptr &&
            objects->snapshot_by_handle != nullptr && objects->find_exact != nullptr &&
            framework != nullptr && framework->is_game_thread != nullptr &&
            player != nullptr &&
            HasField<AnomalyNtePlayerServiceV1,
                decltype(AnomalyNtePlayerServiceV1::get_object_handles)>(
                player, offsetof(AnomalyNtePlayerServiceV1, get_object_handles)) &&
            player->get_object_handles != nullptr &&
            HasField<AnomalyUe5ProcessEventServiceV1,
                decltype(AnomalyUe5ProcessEventServiceV1::invoke)>(
                events, offsetof(AnomalyUe5ProcessEventServiceV1, invoke)) &&
            events->invoke != nullptr;
    }
};

// Layout keys the vehicle path reads from compatibility.profile.json. They
// describe the UE object/reflection ABI, so they belong to the profile and not
// to the code; a profile without them simply fails Load and disables the path.
struct Layout final {
    std::int32_t object_class{};
    std::int32_t object_internal_index{};
    std::int32_t object_name_offset{};
    std::int32_t ufunction_num_parms{};
    std::int32_t ufunction_parms_size{};
    std::int32_t ufunction_return_offset{};
    std::int32_t ustruct_property_link{};
    std::int32_t ustruct_super_struct{};
    std::int32_t ffield_name{};
    std::int32_t ffield_class{};
    std::int32_t ffield_class_name{};
    std::int32_t fproperty_element_size{};
    std::int32_t fproperty_offset{};
    std::int32_t fproperty_next{};
    std::int32_t fbool_byte_offset{};
    std::int32_t fbool_byte_mask{};
    std::int32_t fbool_field_mask{};
    std::int32_t objects_items{};
    std::int32_t objects_count{};
    std::int32_t objects_max_count{};
    std::int32_t objects_item_stride{};
    std::int32_t objects_object_offset{};
    std::int32_t objects_serial_offset{};
    std::int32_t objects_chunk_size{};
    std::int32_t objects_instruction_displacement{};
    std::int32_t objects_instruction_size{};
    std::int32_t objects_registry_subtract{};

    [[nodiscard]] bool Load(const anomaly::sdk::Profile& profile) noexcept {
        try {
            const auto value = [&profile](const std::string_view key) {
                return static_cast<std::int32_t>(profile.Value(key, kMaximumLayoutValue, 1));
            };
            object_class = value("object.class");
            object_internal_index = value("object.internalIndex");
            object_name_offset = value("object.nameOffset");
            ufunction_num_parms = value("ufunction.numParms");
            ufunction_parms_size = value("ufunction.parmsSize");
            ufunction_return_offset = value("ufunction.returnValueOffset");
            ustruct_property_link = value("ustruct.propertyLink");
            ustruct_super_struct = value("ustruct.superStruct");
            ffield_name = value("ffield.name");
            ffield_class = value("ffield.class");
            ffield_class_name = value("ffieldClass.name");
            fproperty_element_size = value("fproperty.elementSize");
            fproperty_offset = value("fproperty.offsetInternal");
            fproperty_next = value("fproperty.propertyLinkNext");
            fbool_byte_offset = value("fboolProperty.byteOffset");
            fbool_byte_mask = value("fboolProperty.byteMask");
            fbool_field_mask = value("fboolProperty.fieldMask");
            objects_items = value("objects.itemsOffset");
            objects_count = value("objects.countOffset");
            objects_max_count = value("objects.maxCountOffset");
            objects_item_stride = value("objects.itemStride");
            objects_object_offset = value("objects.objectOffset");
            objects_serial_offset = value("objects.serialOffset");
            objects_chunk_size = value("objects.chunkSize");
            objects_instruction_displacement = value("objects.instructionDisplacement");
            objects_instruction_size = value("objects.instructionSize");
            objects_registry_subtract = value("objects.registrySubtract");
            return objects_chunk_size > 0 && objects_item_stride > 0 &&
                objects_instruction_size > 0;
        } catch (const std::exception& error) {
            error_ = error.what();
            return false;
        } catch (...) {
            error_ = "the layout is unreadable";
            return false;
        }
    }

    [[nodiscard]] const std::string& Error() const noexcept { return error_; }

    std::string error_;
};

enum class Outcome : std::uint8_t { not_riding, moved, unchanged, failed };

// How a ride is asked to move. `anchored` adds `delta` to the vehicle's own
// position, which is what a directional teleport needs: a zero offset then
// means "stay where you are". Otherwise `target` names the spot the rider
// should end up at, and the seat offset is preserved.
struct VehicleMove final {
    bool anchored{};
    double target[3]{};
    double delta[3]{};
};

// One entry of a reflected parameter list.
struct Property final {
    std::uintptr_t address{};
    std::uintptr_t next{};
    std::string name;
    std::string type;
    std::int32_t offset{};
    std::int32_t size{};
};

// Bit mask of a reflected FBoolProperty inside the parameter buffer.
struct BoolField final {
    std::uint8_t byte_offset{};
    std::uint8_t field_mask{};
    std::uint8_t byte_mask{};
};

// Reflected K2_SetActorLocation parameter layout.
struct SetLocationBinding final {
    AnomalyGenerationHandleV1 function{};
    std::uint16_t parms_size{};
    std::int32_t location_offset{};
    std::int32_t return_offset{};
    std::int32_t b_sweep_offset{};
    std::int32_t b_teleport_offset{};
    BoolField b_sweep{};
    BoolField b_teleport{};
    bool ready{};
};

// A resolved UFunction together with its parameter size.
struct FunctionBinding final {
    AnomalyGenerationHandleV1 function{};
    std::uint16_t parms_size{};
    bool ready{};
};

class Access final {
public:
    [[nodiscard]] bool LoadLayout(const anomaly::sdk::Profile& profile) noexcept {
        layout_ = {};
        layout_loaded_ = layout_.Load(profile);
        if (!layout_loaded_) {
            error_ = "the vehicle layout is unusable: " + layout_.Error();
        }
        return layout_loaded_;
    }

    [[nodiscard]] bool LayoutLoaded() const noexcept { return layout_loaded_; }

    // Resolved from the profile symbol before the Game-thread prepare.
    void SetObjectSymbol(const std::uintptr_t address) noexcept { object_symbol_address_ = address; }

    // Game thread only. Resolves the object registry and every UFunction the
    // vehicle path uses; safe to call every tick because a successful
    // resolution is cached for the lifetime of the object generation.
    [[nodiscard]] bool Prepare(const Services& services) {
        services_ = services;
        error_.clear();
        if (!layout_loaded_) {
            error_ = "vehicle layout is not loaded";
            return false;
        }
        if (!services_.Complete()) {
            error_ = "vehicle services are incomplete";
            return false;
        }
        if (services_.framework->is_game_thread(services_.framework->user) == 0) {
            error_ = "vehicle access requires the Game thread";
            return false;
        }
        if (ready_) return true;
        if (!LoadRegistry()) return false;
        if (!BuildSetLocation()) return false;
        if (!ResolveFunction("/Script/Engine.Actor.K2_GetActorLocation", &get_location_)) {
            error_ = "K2_GetActorLocation is unresolved: " + error_;
            return false;
        }
        if (!ResolveFunction(
                "/Script/HTGame.HTPlayerController.BP_GetCurrentDriveVehicle",
                &drive_vehicle_)) {
            error_ = "BP_GetCurrentDriveVehicle is unresolved: " + error_;
            return false;
        }
        // The remaining lookups are optional: a build that drops one of them
        // still has the controller lookup above.
        static_cast<void>(ResolveFunction(
            "/Script/HTGame.HTPlayerCharacter.BP_GetCurrentDirvingVehicle",
            &character_vehicle_));
        static_cast<void>(ResolveFunction(
            "/Script/HTGame.HTPlayerCharacter.GetBoardWaterVehicle", &water_vehicle_));
        ready_ = true;
        return true;
    }

    // Moves the vehicle the local player rides. Returns not_riding when the
    // player is on foot, unchanged when the request resolves to the vehicle's
    // current position, moved when it was displaced, and failed with `status`
    // set otherwise.
    [[nodiscard]] Outcome MoveRiddenVehicle(
        const VehicleMove& move, AnomalyStatusV1& status) {
        status = {};
        if (!ready_ || services_.framework == nullptr ||
            services_.framework->is_game_thread(services_.framework->user) == 0) {
            note_ = "the vehicle access is not prepared on the game thread";
            return Outcome::not_riding;
        }
        AnomalyGenerationHandleV1 pawn{};
        AnomalyGenerationHandleV1 controller{};
        if (services_.player->get_object_handles(
                services_.player->user, &pawn, &controller).code != ANOMALY_STATUS_V1_OK) {
            note_ = "the local player handles are unavailable";
            return Outcome::not_riding;
        }
        if (pawn.id == 0) {
            note_ = "the local player has no pawn";
            return Outcome::not_riding;
        }

        std::array<double, 3> rider{};
        if (!ActorLocation(pawn, rider)) {
            note_ = "the pawn location is unreadable";
            return Outcome::not_riding;
        }

        std::uintptr_t vehicle_pointer{};
        if (!FindVehicle(controller, pawn, vehicle_pointer)) {
            note_ = "no driven vehicle is attached to the player";
            return Outcome::not_riding;
        }
        AnomalyGenerationHandleV1 vehicle{};
        std::array<double, 3> vehicle_position{};
        if (!HandleOfObject(vehicle_pointer, vehicle) || !ActorLocation(vehicle, vehicle_position)) {
            status = Status(ANOMALY_STATUS_V1_FAILED, "the ridden vehicle is unreadable");
            return Outcome::failed;
        }
        // The rider sits on a fixed seat offset. A fixed-point request names where the
        // rider should land, so preserving the offset is what makes the two arrive
        // together. A directional request is anchored on the vehicle instead, where a
        // zero offset means the ride stays put.
        std::array<double, 3> destination{};
        bool unchanged = true;
        for (std::size_t axis = 0; axis != 3; ++axis) {
            destination[axis] = move.anchored
                ? vehicle_position[axis] + move.delta[axis]
                : move.target[axis] + (vehicle_position[axis] - rider[axis]);
            if (std::fabs(destination[axis] - vehicle_position[axis]) > 0.001) {
                unchanged = false;
            }
        }
        if (unchanged) {
            status = Status(ANOMALY_STATUS_V1_OK, "the ride stayed in place");
            note_ = "the ride stayed in place";
            return Outcome::unchanged;
        }
        bool returned{};
        status = SetActorLocation(vehicle, destination, returned);
        if (status.code != ANOMALY_STATUS_V1_OK) return Outcome::failed;
        if (!returned) {
            status = Status(ANOMALY_STATUS_V1_FAILED, "the vehicle refused the move");
            return Outcome::failed;
        }
        note_ = "the ridden vehicle was moved";
        return Outcome::moved;
    }

    [[nodiscard]] const std::string& Error() const noexcept { return error_; }

    // Why the last MoveRiddenVehicle call did not move a vehicle: logged by the
    // caller so an on-foot teleport can be told apart from a failed detection.
    [[nodiscard]] const std::string& Note() const noexcept { return note_; }

private:
    [[nodiscard]] static AnomalyStatusV1 Status(
        const std::uint32_t code, const std::string_view message) noexcept {
        return {code, 0, anomaly::sdk::StringView(message)};
    }

    [[nodiscard]] bool Read(
        const std::uintptr_t address, void* destination, const std::size_t size) const noexcept {
        if (services_.core == nullptr || address == 0 || size == 0) return false;
        const AnomalyMutableByteSpanV1 span{static_cast<std::uint8_t*>(destination), size};
        return services_.core->read_memory(services_.core->user, address, span).code ==
            ANOMALY_STATUS_V1_OK;
    }

    template <typename T>
    [[nodiscard]] bool ReadValue(const std::uintptr_t address, T& value) const noexcept {
        return Read(address, &value, sizeof(value));
    }

    [[nodiscard]] bool ReadPointer(
        const std::uintptr_t base, const std::int64_t offset,
        std::uintptr_t& value) const noexcept {
        if (base == 0) return false;
        return ReadValue(static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(base) + offset), value);
    }

    [[nodiscard]] bool NameOf(const std::uint32_t id, std::string& value) {
        if (id == 0) return false;
        const auto cached = names_.find(id);
        if (cached != names_.end()) {
            value = cached->second;
            return true;
        }
        std::array<char, kMaximumNameBytes> buffer{};
        std::size_t size = buffer.size();
        if (services_.names->resolve_utf8(
                services_.names->user, id, buffer.data(), &size).code != ANOMALY_STATUS_V1_OK ||
            size == 0) {
            return false;
        }
        value.assign(buffer.data(), size - 1);
        names_.emplace(id, value);
        return true;
    }

    [[nodiscard]] bool ObjectName(const std::uintptr_t object, std::string& value) {
        std::uint32_t id{};
        if (object == 0 || !ReadValue(object + layout_.object_name_offset, id)) return false;
        return NameOf(id, value);
    }

    [[nodiscard]] bool FieldTypeName(const std::uintptr_t field, std::string& value) {
        std::uintptr_t field_class{};
        std::uint32_t id{};
        if (!ReadPointer(field, layout_.ffield_class, field_class) || field_class == 0 ||
            !ReadValue(field_class + layout_.ffield_class_name, id)) {
            return false;
        }
        return NameOf(id, value);
    }

    [[nodiscard]] bool LoadRegistry() noexcept {
        registry_ = 0;
        const std::uint64_t generation = services_.objects->generation(services_.objects->user);
        if (registry_generation_ == generation && registry_ != 0) return true;
        const std::uintptr_t entry = object_symbol_address_;
        std::int32_t displacement{};
        if (entry == 0 ||
            !ReadValue(entry + static_cast<std::uintptr_t>(
                layout_.objects_instruction_displacement), displacement)) {
            error_ = "the object registry symbol is unresolved";
            return false;
        }
        const auto next = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(entry) + layout_.objects_instruction_size);
        const auto candidate = static_cast<std::uintptr_t>(
            static_cast<std::int64_t>(next) + displacement - layout_.objects_registry_subtract);
        std::uintptr_t items{};
        std::uint32_t count{};
        std::uint32_t max_count{};
        if (!ReadPointer(candidate, layout_.objects_items, items) ||
            !ReadValue(candidate + static_cast<std::uintptr_t>(layout_.objects_count), count) ||
            !ReadValue(candidate + static_cast<std::uintptr_t>(layout_.objects_max_count),
                max_count) ||
            items == 0 || count == 0 || count > max_count ||
            max_count > 16u * 1024u * 1024u) {
            error_ = "the object registry is unreadable";
            return false;
        }
        registry_ = candidate;
        registry_generation_ = generation;
        return true;
    }

    [[nodiscard]] bool ReadSlot(
        const std::uint32_t index, std::uintptr_t& object, std::uint32_t& serial) const noexcept {
        object = 0;
        serial = 0;
        if (registry_ == 0) return false;
        std::uint32_t count{};
        if (!ReadValue(registry_ + static_cast<std::uintptr_t>(layout_.objects_count), count) ||
            index >= count) {
            return false;
        }
        const auto chunk_size = static_cast<std::uint32_t>(layout_.objects_chunk_size);
        const auto stride = static_cast<std::uint32_t>(layout_.objects_item_stride);
        std::uintptr_t chunk{};
        std::uintptr_t chunk_pointer{};
        if (!ReadPointer(registry_, layout_.objects_items, chunk) || chunk == 0 ||
            !ReadPointer(chunk, static_cast<std::int64_t>(index / chunk_size) * 8,
                chunk_pointer) ||
            chunk_pointer == 0) {
            return false;
        }
        const auto item = static_cast<std::uintptr_t>(
            chunk_pointer + static_cast<std::uintptr_t>(index % chunk_size) * stride);
        return ReadValue(item + static_cast<std::uintptr_t>(layout_.objects_object_offset),
                   object) &&
            ReadValue(item + static_cast<std::uintptr_t>(layout_.objects_serial_offset), serial);
    }

    [[nodiscard]] bool AddressOfHandle(
        const AnomalyGenerationHandleV1 handle, std::uintptr_t& object) const noexcept {
        object = 0;
        if (handle.id == 0) return false;
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (services_.objects->snapshot_by_handle(
                services_.objects->user, handle, &snapshot).code != ANOMALY_STATUS_V1_OK) {
            return false;
        }
        std::uint32_t serial{};
        std::int32_t index{};
        if (!ReadSlot(ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle), object, serial) || object == 0 ||
            serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ||
            !ReadValue(object + layout_.object_internal_index, index) || index < 0 ||
            static_cast<std::uint32_t>(index) != ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle)) {
            object = 0;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool HandleOfObject(
        const std::uintptr_t object, AnomalyGenerationHandleV1& handle) const noexcept {
        handle = {};
        std::int32_t index{};
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        std::uintptr_t resolved{};
        if (object == 0 || !ReadValue(object + layout_.object_internal_index, index) || index < 0 ||
            services_.objects->snapshot_at(
                services_.objects->user, static_cast<std::uint32_t>(index),
                &snapshot).code != ANOMALY_STATUS_V1_OK ||
            !AddressOfHandle(snapshot.handle, resolved) || resolved != object) {
            return false;
        }
        handle = snapshot.handle;
        return true;
    }

    [[nodiscard]] bool FindExact(
        const std::string_view path, AnomalyGenerationHandleV1& handle) noexcept {
        handle = {};
        const AnomalyStatusV1 status = services_.objects->find_exact(
            services_.objects->user, anomaly::sdk::StringView(path), &handle);
        if (status.code != ANOMALY_STATUS_V1_OK || handle.id == 0) {
            error_ = "find_exact "; error_ += path;
            error_ += " code=" + std::to_string(status.code);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool Invoke(
        const AnomalyGenerationHandleV1 receiver, const AnomalyGenerationHandleV1 function,
        void* parameters, const std::size_t size) const noexcept {
        if (receiver.id == 0 || function.id == 0 || services_.events == nullptr) return false;
        return services_.events->invoke(
            services_.events->user, receiver, function, parameters, size).code ==
            ANOMALY_STATUS_V1_OK;
    }

    [[nodiscard]] bool ActorLocation(
        const AnomalyGenerationHandleV1 actor, std::array<double, 3>& location) const noexcept {
        if (!get_location_.ready || get_location_.parms_size < 24) return false;
        std::array<std::uint8_t, 64> buffer{};
        if (!Invoke(actor, get_location_.function, buffer.data(),
                std::min<std::size_t>(get_location_.parms_size, buffer.size()))) {
            return false;
        }
        std::memcpy(location.data(), buffer.data(), 24);
        return std::isfinite(location[0]) && std::isfinite(location[1]) &&
            std::isfinite(location[2]);
    }

    // Returns the UObject pointer of the vehicle the local player rides, or 0
    // when the player is on foot.
    [[nodiscard]] std::uintptr_t ReturnedObject(
        const AnomalyGenerationHandleV1 receiver,
        const FunctionBinding& binding) const noexcept {
        if (!binding.ready || receiver.id == 0) return 0;
        std::array<std::uint8_t, 64> buffer{};
        if (!Invoke(receiver, binding.function, buffer.data(),
                std::min<std::size_t>(binding.parms_size, buffer.size()))) {
            return 0;
        }
        std::uintptr_t value{};
        std::memcpy(&value, buffer.data(), sizeof(value));
        return value;
    }

    [[nodiscard]] bool ClassChainContains(
        const std::uintptr_t object, const std::string_view name) {
        std::uintptr_t type{};
        if (!ReadPointer(object, layout_.object_class, type)) return false;
        for (int depth = 0; depth < 32 && type != 0; ++depth) {
            std::string type_name;
            if (ObjectName(type, type_name) && type_name == name) return true;
            std::uintptr_t parent{};
            if (!ReadPointer(type, layout_.ustruct_super_struct, parent)) return false;
            type = parent;
        }
        return false;
    }

    [[nodiscard]] bool FindVehicle(
        const AnomalyGenerationHandleV1 controller, const AnomalyGenerationHandleV1 pawn,
        std::uintptr_t& vehicle) {
        vehicle = 0;
        const std::uintptr_t candidates[] = {
            ReturnedObject(controller, drive_vehicle_),
            ReturnedObject(pawn, character_vehicle_),
            ReturnedObject(pawn, water_vehicle_),
        };
        for (const std::uintptr_t candidate : candidates) {
            if (candidate == 0) continue;
            if (ClassChainContains(candidate, "HTWheeledVehicleBase") ||
                ClassChainContains(candidate, "HTWaterVehicleBasePawn") ||
                ClassChainContains(candidate, "HTWheeledVehicleDrivable")) {
                vehicle = candidate;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool ReadProperty(
        const std::uintptr_t property, Property& value) {
        value = {};
        value.address = property;
        std::uint32_t name_id{};
        return ReadValue(property + layout_.ffield_name, name_id) && NameOf(name_id, value.name) &&
            ReadValue(property + layout_.fproperty_offset, value.offset) &&
            ReadValue(property + layout_.fproperty_element_size, value.size) &&
            ReadPointer(property, layout_.fproperty_next, value.next) &&
            FieldTypeName(property, value.type);
    }

    [[nodiscard]] bool ReadParameters(
        const std::uintptr_t function, std::vector<Property>& value) {
        value.clear();
        std::uintptr_t link{};
        if (!ReadPointer(function, layout_.ustruct_property_link, link)) return false;
        for (int depth = 0; depth < 64 && link != 0; ++depth) {
            Property property{};
            if (!ReadProperty(link, property)) return false;
            link = property.next;
            value.push_back(std::move(property));
        }
        return !value.empty();
    }

    [[nodiscard]] bool ReadBoolField(
        const Property& property, BoolField& value) const noexcept {
        return ReadValue(property.address + layout_.fbool_byte_offset, value.byte_offset) &&
            ReadValue(property.address + layout_.fbool_byte_mask, value.byte_mask) &&
            ReadValue(property.address + layout_.fbool_field_mask, value.field_mask);
    }

    [[nodiscard]] static const Property* FindParameter(
        const std::vector<Property>& parameters, const std::string_view name) noexcept {
        const auto found = std::ranges::find(parameters, name, &Property::name);
        return found == parameters.end() ? nullptr : &*found;
    }

    // Resolves a UFunction handle and its parameter size. `parameter` names an
    // argument that must exist, which keeps a renamed or replaced function from
    // being driven with the wrong buffer.
    [[nodiscard]] bool ResolveFunction(
        const std::string_view path, FunctionBinding* binding) {
        binding->function = {};
        binding->parms_size = 0;
        binding->ready = false;
        AnomalyGenerationHandleV1 function{};
        std::uintptr_t address{};
        std::uint16_t num_parms{};
        std::uint16_t parms_size{};
        if (!FindExact(path, function) || !AddressOfHandle(function, address) || address == 0 ||
            !ReadValue(address + layout_.ufunction_num_parms, num_parms) ||
            !ReadValue(address + layout_.ufunction_parms_size, parms_size) ||
            parms_size == 0) {
            return false;
        }
        static_cast<void>(num_parms);
        binding->function = function;
        binding->parms_size = parms_size;
        binding->ready = true;
        return true;
    }

    [[nodiscard]] bool BuildSetLocation() {
        set_location_ = {};
        AnomalyGenerationHandleV1 function{};
        std::uintptr_t address{};
        std::uint16_t num_parms{};
        std::uint16_t parms_size{};
        std::uint16_t return_offset{};
        if (!FindExact("/Script/Engine.Actor.K2_SetActorLocation", function)) {
            error_ = "K2_SetActorLocation is not found: " + error_;
            return false;
        }
        if (!AddressOfHandle(function, address) || address == 0) {
            error_ = "K2_SetActorLocation handle does not resolve";
            return false;
        }
        if (!ReadValue(address + layout_.ufunction_num_parms, num_parms) ||
            !ReadValue(address + layout_.ufunction_parms_size, parms_size) ||
            !ReadValue(address + layout_.ufunction_return_offset, return_offset)) {
            error_ = "K2_SetActorLocation reflection is unreadable";
            return false;
        }
        if (num_parms != 5 || parms_size == 0) {
            error_ = "K2_SetActorLocation signature changed: numParms=" +
                std::to_string(num_parms) + " parmsSize=" + std::to_string(parms_size) +
                " returnOffset=" + std::to_string(return_offset);
            return false;
        }
        std::vector<Property> parameters;
        if (!ReadParameters(address, parameters)) {
            error_ = "K2_SetActorLocation parameters are unreadable";
            return false;
        }
        const Property* location = FindParameter(parameters, "NewLocation");
        const Property* b_sweep = FindParameter(parameters, "bSweep");
        const Property* b_teleport = FindParameter(parameters, "bTeleport");
        const Property* returned = FindParameter(parameters, "ReturnValue");
        if (location == nullptr || b_sweep == nullptr || b_teleport == nullptr ||
            returned == nullptr || location->type != "StructProperty" || location->size != 24 ||
            b_sweep->type != "BoolProperty" || b_teleport->type != "BoolProperty" ||
            returned->type != "BoolProperty") {
            error_ = "K2_SetActorLocation parameter layout is unexpected";
            return false;
        }
        SetLocationBinding binding{};
        binding.function = function;
        binding.parms_size = static_cast<std::uint16_t>(parms_size);
        binding.location_offset = location->offset;
        binding.b_sweep_offset = b_sweep->offset;
        binding.b_teleport_offset = b_teleport->offset;
        binding.return_offset = returned->offset;
        if (!ReadBoolField(*b_sweep, binding.b_sweep) ||
            !ReadBoolField(*b_teleport, binding.b_teleport) ||
            binding.location_offset < 0 || binding.b_sweep_offset < 0 ||
            binding.b_teleport_offset < 0 || binding.return_offset < 0 ||
            binding.location_offset + 24 > binding.parms_size ||
            binding.b_sweep_offset + 1 > binding.parms_size ||
            binding.b_teleport_offset + 1 > binding.parms_size ||
            binding.return_offset + 1 > binding.parms_size) {
            error_ = "K2_SetActorLocation offsets exceed the parameter size";
            return false;
        }
        binding.ready = true;
        set_location_ = binding;
        return true;
    }

    static void SetBool(
        std::vector<std::uint8_t>& buffer, const std::int32_t offset, const BoolField field,
        const bool value) noexcept {
        const std::size_t index = static_cast<std::size_t>(offset) + field.byte_offset;
        if (index >= buffer.size()) return;
        buffer[index] = static_cast<std::uint8_t>(
            (buffer[index] & ~field.field_mask) | (value ? field.byte_mask : 0));
    }

    [[nodiscard]] AnomalyStatusV1 SetActorLocation(
        const AnomalyGenerationHandleV1 actor, const std::array<double, 3>& position,
        bool& returned) const noexcept {
        returned = false;
        if (!set_location_.ready) return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
            "the vehicle move binding is unavailable");
        std::vector<std::uint8_t> buffer(
            static_cast<std::size_t>(set_location_.parms_size) + 8, 0);
        std::memcpy(buffer.data() + set_location_.location_offset, position.data(), 24);
        SetBool(buffer, set_location_.b_sweep_offset, set_location_.b_sweep, false);
        SetBool(buffer, set_location_.b_teleport_offset, set_location_.b_teleport, true);
        if (!Invoke(actor, set_location_.function, buffer.data(),
                static_cast<std::size_t>(set_location_.parms_size))) {
            return Status(ANOMALY_STATUS_V1_FAILED, "the vehicle move invocation failed");
        }
        returned = buffer[static_cast<std::size_t>(set_location_.return_offset)] != 0;
        return {ANOMALY_STATUS_V1_OK, 0, {}};
    }

private:
    Services services_{};
    Layout layout_{};
    bool layout_loaded_{};
    bool ready_{};
    std::uintptr_t object_symbol_address_{};
    std::uintptr_t registry_{};
    std::uint64_t registry_generation_{};
    std::map<std::uint32_t, std::string> names_;
    FunctionBinding get_location_{};
    FunctionBinding drive_vehicle_{};
    FunctionBinding character_vehicle_{};
    FunctionBinding water_vehicle_{};
    SetLocationBinding set_location_{};
    std::string error_;
    std::string note_;
};

inline Access& Instance() noexcept {
    static Access access;
    return access;
}

}  // namespace nteteleport_vehicle
