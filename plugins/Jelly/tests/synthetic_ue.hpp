#pragma once
//
// A synthetic UE image for the offline fixture.
//
// The model module under test believes it is talking to a live process. This file
// builds that process out of the same layout resource the plugin ships: object
// registry, UClass/UStruct reflection chains, FProperty offsets, a reflected
// UFunction with a real parameter list, and a fake MemoryAccess/NameTable on top.
// Tests can then break the image in specific ways (a reused serial, a float FVector
// signature, a colliding body) and check what the module does about it.

#include "jelly_layout.hpp"
#include "jelly_model.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace jelly_tests {

extern int g_failures;

inline void Check(const int line, const char* text, const bool condition) {
    if (condition) return;
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, text);
}

#define CHECK(...) ::jelly_tests::Check(__LINE__, #__VA_ARGS__, (__VA_ARGS__))

inline void CheckSkip(const int line, const jelly::Skip actual, const jelly::Skip expected) {
    if (actual == expected) return;
    ++g_failures;
    std::printf(
        "  FAIL line %d: skip was %u, expected %u\n", line,
        static_cast<unsigned>(actual), static_cast<unsigned>(expected));
}

#define CHECK_SKIP(actual, expected) \
    ::jelly_tests::CheckSkip(__LINE__, (actual), (expected))

inline void Section(const char* name) {
    std::printf("- %s\n", name);
    // Flushed as it goes: a fixture that crashes shows which section it died in instead of
    // losing the whole buffer.
    std::fflush(stdout);
}

class SyntheticUe final : public jelly::MemoryAccess, public jelly::NameTable {
public:
    struct Options {
        // Emit a float FVector signature for the reflected write instead of the
        // double one, which is what an incompatible build looks like.
        bool scalar_float_scale{};
        bool omit_write_function{};
        // A process whose mesh class carries another name than the resource records.
        bool rename_mesh_component_class{};
        // A build where FBodyInstance carries another name, so the property the
        // plugin needs for its collision check cannot be found at all.
        bool rename_body_instance{};
        // A build that does not reflect SceneComponent::Mobility at all, so the plugin
        // cannot tell static scenery apart and has to fall back to allowing it.
        bool omit_mobility_property{};
        // A build that does not reflect AActor::bHidden, so the plugin cannot tell a
        // hidden actor from a visible one and has to fall back to allowing it.
        bool omit_hidden_property{};
        // A build that does not reflect the rotation write at all, so the spin has to give
        // up for that class while the jelly scale keeps running.
        bool omit_rotation_function{};
    };

    SyntheticUe(const jelly::Layout& layout, const Options options)
        : layout_(layout), options_(options) {
        process_event_prologue_ = layout.process_event_prologue;

        // The registry exists before any object does, so the module can be prepared
        // at any point and objects can be added to it afterwards.
        chunk_ = Allocate(static_cast<std::size_t>(kChunkSize) * kItemStride);
        const std::uintptr_t items = Allocate(sizeof(std::uintptr_t));
        PutPointer(items, chunk_);
        registry_ = Allocate(0x40);
        PutPointer(registry_ + Offset("objects.items"), items);
        Put32(registry_ + Offset("objects.max_count"), kChunkSize);
        Put32(registry_ + Offset("objects.count"), 0);
        Put32(registry_ + Offset("objects.max_chunks"), 1);
        Put32(registry_ + Offset("objects.num_chunks"), 1);
        process_event_ = Allocate(0x40);
        PutBytes(process_event_, process_event_prologue_.data(), process_event_prologue_.size());

        object_class_ = MakeClass("Object", 0);
        actor_class_ = MakeClass("Actor", object_class_);
        if (!options_.omit_hidden_property) {
            // AActor::bHidden: the byte at +0 and the top bit of it.
            AddBoolProperty(actor_class_, "bHidden", kHiddenOffset, 0, kHiddenMask);
        }
        character_class_ = MakeClass("Character", actor_class_);
        function_class_ = MakeClass("Function", object_class_);
        struct_class_ = MakeClass("Struct", object_class_);
        vector_struct_ = MakeObject("Vector", struct_class_);
        rotator_struct_ = MakeObject("Rotator", struct_class_);
        hit_result_struct_ = MakeObject("HitResult", struct_class_);
        body_struct_ = MakeObject("BodyInstance", struct_class_);

        // The names a live process has registered because the classes exist; the
        // module resolves them before it inspects any object.
        for (const auto& entry : layout.properties) Intern(entry.second.name);
        for (const auto& family : layout.functions) {
            for (const auto& spec : family.second) Intern(spec.name);
        }
        // The engine classes the resource names exist in the build, so their names
        // are registered too.
        for (const auto& family : layout.class_families) {
            for (const auto& name : family.second) Intern(name);
        }

        scene_component_class_ = MakeClass("SceneComponent", object_class_);
        primitive_component_class_ = MakeClass("PrimitiveComponent", scene_component_class_);
        mesh_component_class_ = MakeClass(
            options_.rename_mesh_component_class ? "MeshComponentX" : "MeshComponent",
            primitive_component_class_);
        skeletal_mesh_class_ = MakeClass("SkeletalMeshComponent", mesh_component_class_);

        AddProperty(
            scene_component_class_, "RelativeScale3D", "StructProperty", 24, kScaleOffset,
            vector_struct_);
        if (!options_.omit_mobility_property) {
            AddProperty(
                scene_component_class_, "Mobility", "ByteProperty", 1, kMobilityOffset, 0);
        }
        AddBoolProperty(scene_component_class_, "bVisible", kVisibleOffset, 0, 0x01);
        AddArrayProperty(
            scene_component_class_, "AttachChildren", kChildrenOffset, "ObjectProperty",
            scene_component_class_);
        // The component a component hangs under, which is how the module tells a part of a
        // vehicle actor (a spoiler, a lamp) from a component of the actor's own tree.
        AddProperty(
            scene_component_class_, "AttachParent", "ObjectProperty", 8, kAttachParentOffset,
            scene_component_class_);
        // The rotation the model carries, which the spin is added to.
        AddProperty(
            scene_component_class_, "RelativeRotation", "StructProperty", 24, kRotationOffset,
            rotator_struct_);
        AddProperty(
            primitive_component_class_,
            options_.rename_body_instance ? "BodyInstanceX" : "BodyInstance",
            "StructProperty", 0x80, kBodyOffset, body_struct_);
        // FBodyInstance: CollisionEnabled is the struct's first byte and
        // bSimulatePhysics the low bit of the byte after it.
        AddProperty(body_struct_, "CollisionEnabled", "ByteProperty", 1, 0, 0);
        AddBoolProperty(body_struct_, "bSimulatePhysics", 1, 0, 0x01);

        if (!options_.omit_write_function) {
            const std::uint32_t parms_size = options_.scalar_float_scale ? 12U : 24U;
            const std::uint32_t element_size = options_.scalar_float_scale ? 12U : 24U;
            const std::uintptr_t write_function = AddFunction(
                scene_component_class_, "SetRelativeScale3D", 1,
                static_cast<std::uint16_t>(parms_size), 0xFFFF);
            AddProperty(
                write_function, "NewScale3D", "StructProperty", element_size, 0,
                vector_struct_);
            scale_function_ = write_function;
        }
        if (!options_.omit_rotation_function) {
            // The shape the live build carries: K2_SetRelativeRotation(NewRotation,
            // bSweep, SweepHitResult, bTeleport), 297 bytes with the hit result inline.
            const std::uintptr_t rotation_function = AddFunction(
                scene_component_class_, "K2_SetRelativeRotation", 4,
                static_cast<std::uint16_t>(kRotationParmsSize), 0xFFFF);
            AddProperty(
                rotation_function, "NewRotation", "StructProperty", 24, 0, rotator_struct_);
            AddBoolProperty(rotation_function, "bSweep", 24, 0, 0x01);
            AddProperty(
                rotation_function, "SweepHitResult", "StructProperty", 264, 32,
                hit_result_struct_);
            AddBoolProperty(rotation_function, "bTeleport", 296, 0, 0x01);
            rotation_function_ = rotation_function;
        }
    }

    [[nodiscard]] std::uintptr_t registry_address() const noexcept { return registry_; }
    [[nodiscard]] std::uintptr_t process_event_address() const noexcept {
        return process_event_;
    }
    // The engine base classes an actor class is built on, so a test can build one that is
    // not a Character (the crowd NPCs are not).
    [[nodiscard]] std::uintptr_t ActorClass() const noexcept { return actor_class_; }
    [[nodiscard]] std::uintptr_t CharacterClass() const noexcept { return character_class_; }
    // The reflected functions the fake engine dispatches on.
    [[nodiscard]] std::uintptr_t scale_function() const noexcept { return scale_function_; }
    [[nodiscard]] std::uintptr_t rotation_function() const noexcept {
        return rotation_function_;
    }
    [[nodiscard]] std::uintptr_t index_of(const std::uintptr_t object) const {
        const auto found = registered_.find(object);
        return found == registered_.end() ? 0 : found->second;
    }

    // The class pointer a UObject carries, read back through the same override the
    // module under test uses.
    [[nodiscard]] std::uintptr_t ClassOf(const std::uintptr_t object) {
        std::uintptr_t klass{};
        static_cast<void>(
            Read(object + Offset("object.class"), &klass, sizeof(klass)));
        return klass;
    }

    // A class that belongs to the instanced-mesh family the resource names.
    [[nodiscard]] std::uintptr_t MakeInstancedClass() {
        return MakeClass("InstancedStaticMeshComponent", mesh_component_class_);
    }

    // A mesh-derived component that renders UI in the world rather than a model, which
    // is what the crowd NPCs carry next to their body mesh.
    [[nodiscard]] std::uintptr_t MakeWidgetClass() {
        return MakeClass("WidgetComponent", mesh_component_class_);
    }

    [[nodiscard]] std::uintptr_t MakeActor(const std::string& name) {
        return MakeActor(name, character_class_);
    }

    [[nodiscard]] std::uintptr_t MakeActor(
        const std::string& name, const std::uintptr_t super) {
        const std::uintptr_t klass = MakeClass(name, super);
        AddProperty(
            klass, "RootComponent", "ObjectProperty", 8, kRootOffset,
            scene_component_class_);
        return MakeObject(name + "_C", klass);
    }

    [[nodiscard]] std::uintptr_t MakeComponent(
        const std::string& name, const std::uintptr_t owner, const jelly::Vector3& scale,
        const bool visible = true, const std::uint8_t collision = 0,
        const bool simulate_physics = false, const std::uintptr_t klass = 0) {
        const std::uintptr_t object =
            MakeObject(name, klass == 0 ? skeletal_mesh_class_ : klass);
        PutPointer(object + Offset("object.outer"), owner);
        PutBytes(object + kScaleOffset, scale.data(), sizeof(scale));
        const jelly::Vector3 rotation{};
        PutBytes(object + kRotationOffset, rotation.data(), sizeof(rotation));
        Put8(object + kVisibleOffset, visible ? 1 : 0);
        Put8(object + kBodyOffset, collision);
        Put8(object + kBodyOffset + 1, simulate_physics ? 1 : 0);
        Put8(object + kMobilityOffset, kMobilityMovable);
        return object;
    }

    [[nodiscard]] std::uintptr_t MakePlainComponent(
        const std::string& name, const std::uintptr_t owner) {
        const std::uintptr_t object = MakeObject(name, scene_component_class_);
        PutPointer(object + Offset("object.outer"), owner);
        return object;
    }

    void SetRoot(const std::uintptr_t actor, const std::uintptr_t root) {
        PutPointer(actor + kRootOffset, root);
    }

    // Attaches a component tree to another actor's component, the way a child actor's
    // root ends up under the ChildActorComponent of the vehicle that owns it.
    void SetAttachParent(const std::uintptr_t component, const std::uintptr_t parent) {
        PutPointer(component + kAttachParentOffset, parent);
    }

    // The game hides the party members that stand next to the local player; this is the
    // flag that says so.
    void SetHidden(const std::uintptr_t actor, const bool hidden) {
        std::uint8_t value{};
        static_cast<void>(Read(actor + kHiddenOffset, &value, sizeof(value)));
        value = hidden ? static_cast<std::uint8_t>(value | kHiddenMask)
                       : static_cast<std::uint8_t>(value & ~kHiddenMask);
        Put8(actor + kHiddenOffset, value);
    }

    void Attach(const std::uintptr_t parent, const std::vector<std::uintptr_t>& children) {
        if (children.empty()) return;
        const std::uintptr_t data = Allocate(children.size() * sizeof(std::uintptr_t));
        for (std::size_t index = 0; index < children.size(); ++index) {
            PutPointer(data + index * sizeof(std::uintptr_t), children[index]);
        }
        PutPointer(parent + kChildrenOffset, data);
        Put32(parent + kChildrenOffset + 8, static_cast<std::uint32_t>(children.size()));
        Put32(parent + kChildrenOffset + 12, static_cast<std::uint32_t>(children.size()));
    }

    void SetScale(const std::uintptr_t component, const jelly::Vector3& scale) {
        PutBytes(component + kScaleOffset, scale.data(), sizeof(scale));
    }

    [[nodiscard]] jelly::Vector3 ScaleOf(const std::uintptr_t component) {
        jelly::Vector3 scale{};
        static_cast<void>(Read(component + kScaleOffset, scale.data(), sizeof(scale)));
        return scale;
    }

    void SetRotation(const std::uintptr_t component, const jelly::Vector3& rotation) {
        PutBytes(component + kRotationOffset, rotation.data(), sizeof(rotation));
    }

    [[nodiscard]] jelly::Vector3 RotationOf(const std::uintptr_t component) {
        jelly::Vector3 rotation{};
        static_cast<void>(Read(component + kRotationOffset, rotation.data(), sizeof(rotation)));
        return rotation;
    }

    // The slot serial is what tells a live object from one that was destroyed and
    // whose address was reused.
    void SetSerial(const std::uint32_t index, const std::uint32_t serial) {
        Put32(
            chunk_ + static_cast<std::uintptr_t>(index) * kItemStride + kSerialOffset,
            serial);
    }

    // EComponentMobility::Static == 0, Stationary == 1, Movable == 2. A component the
    // game keeps static is the level's scenery and must not be scaled.
    void SetMobility(const std::uintptr_t component, const std::uint8_t mobility) {
        Put8(component + kMobilityOffset, mobility);
    }

    void CorruptProcessEventEntry() {
        const std::uint8_t code = 0xCC;
        PutBytes(process_event_, &code, 1);
    }

    // What Anomaly's own process-wide observer leaves behind: the first bytes of the
    // entry are a jump and the prologue past them is untouched. `readable_target`
    // decides whether that jump lands in a mapped page, which is what the module
    // probes before it is willing to call through the address.
    void InstallProcessEventDetour(const bool readable_target = true) {
        constexpr std::uintptr_t kDetour =
            static_cast<std::uintptr_t>(jelly::kProcessEventDetourBytes);
        const std::uintptr_t relay = readable_target ? Allocate(0x10) : kUnallocatedTarget;
        const auto displacement = static_cast<std::int32_t>(
            static_cast<std::intptr_t>(relay) -
            static_cast<std::intptr_t>(process_event_ + kDetour));
        std::uint8_t code[kDetour]{0xE9};
        std::memcpy(code + 1, &displacement, sizeof(displacement));
        PutBytes(process_event_, code, sizeof(code));
    }

    // A jump over a body that is not the recorded prologue any more, which is what a
    // different patch or a different build looks like.
    void CorruptProcessEventDetourTail() {
        constexpr std::uintptr_t kDetour =
            static_cast<std::uintptr_t>(jelly::kProcessEventDetourBytes);
        const std::uint8_t code = 0xCC;
        PutBytes(process_event_ + kDetour, &code, 1);
    }

    void Finalize() {
        Put32(registry_ + Offset("objects.count"), static_cast<std::uint32_t>(objects_.size()));
    }

    // --- jelly::MemoryAccess ---
    bool Read(
        const std::uintptr_t address, void* const destination,
        const std::size_t size) noexcept override {
        if (address == 0 || destination == nullptr || size == 0) return false;
        auto* out = static_cast<std::uint8_t*>(destination);
        std::size_t remaining = size;
        std::uintptr_t current = address;
        while (remaining != 0) {
            const std::uintptr_t base = current & ~(kBlockSize - 1U);
            const auto block = blocks_.find(base);
            if (block == blocks_.end()) return false;
            const std::size_t offset = static_cast<std::size_t>(current - base);
            const std::size_t take =
                (std::min)(remaining, static_cast<std::size_t>(kBlockSize) - offset);
            std::memcpy(out, block->second.data() + offset, take);
            out += take;
            current += take;
            remaining -= take;
        }
        return true;
    }

    // --- jelly::NameTable ---
    bool Text(const std::uint32_t id, std::string& name) override {
        const auto found = names_.find(id);
        if (found == names_.end()) {
            name.clear();
            return false;
        }
        name = found->second;
        return true;
    }

private:
    static constexpr std::uintptr_t kBlockSize = 0x1000;
    static constexpr std::uintptr_t kScaleOffset = 0x180;
    static constexpr std::uintptr_t kMobilityOffset = 0x1C0;
    // EComponentMobility::Movable; a component the fixture builds is one the game moves.
    static constexpr std::uint8_t kMobilityMovable = 2;
    static constexpr std::uintptr_t kVisibleOffset = 0x1B0;
    static constexpr std::uintptr_t kChildrenOffset = 0x200;
    static constexpr std::uintptr_t kAttachParentOffset = 0xD8;
    static constexpr std::uintptr_t kRotationOffset = 0x168;
    // The live build's K2_SetRelativeRotation frame: NewRotation 24 + bSweep 1 (padded) +
    // an inline FHitResult of 264 bytes + bTeleport 1.
    static constexpr std::uint16_t kRotationParmsSize = 297;
    static constexpr std::uintptr_t kBodyOffset = 0x2C0;
    static constexpr std::uintptr_t kRootOffset = 0x1C8;
    static constexpr std::uintptr_t kHiddenOffset = 0x60;
    static constexpr std::uint8_t kHiddenMask = 0x80;
    static constexpr std::uint32_t kChunkSize = 65536;
    static constexpr std::uintptr_t kItemStride = 24;
    static constexpr std::uintptr_t kObjectOffset = 0;
    static constexpr std::uintptr_t kSerialOffset = 16;
    // Below every allocation this fixture makes, so a probe against it fails exactly
    // the way a jump into an unmapped page does.
    static constexpr std::uintptr_t kUnallocatedTarget = 0x8000;

    [[nodiscard]] std::uintptr_t Offset(const std::string_view key) const {
        return static_cast<std::uintptr_t>(jelly::LayoutOffset(layout_, key, 0));
    }

    std::uintptr_t Allocate(const std::size_t size) {
        const std::uintptr_t start = next_address_;
        const std::size_t rounded = ((size + kBlockSize - 1U) / kBlockSize) * kBlockSize;
        for (std::size_t offset = 0; offset < rounded; offset += kBlockSize) {
            blocks_.emplace(start + offset, std::vector<std::uint8_t>(kBlockSize, 0));
        }
        next_address_ += rounded;
        return start;
    }

    void PutBytes(
        const std::uintptr_t address, const void* const source, const std::size_t size) {
        const auto* in = static_cast<const std::uint8_t*>(source);
        for (std::size_t index = 0; index < size; ++index) {
            const std::uintptr_t current = address + index;
            const auto block = blocks_.find(current & ~(kBlockSize - 1U));
            if (block == blocks_.end()) return;
            block->second[static_cast<std::size_t>(current & (kBlockSize - 1U))] = in[index];
        }
    }

    void Put8(const std::uintptr_t address, const std::uint8_t value) {
        PutBytes(address, &value, 1);
    }

    void Put32(const std::uintptr_t address, const std::uint32_t value) {
        PutBytes(address, &value, sizeof(value));
    }

    void PutPointer(const std::uintptr_t address, const std::uintptr_t value) {
        PutBytes(address, &value, sizeof(value));
    }

    std::uint32_t Intern(const std::string& name) {
        const auto found = name_ids_.find(name);
        if (found != name_ids_.end()) return found->second;
        const std::uint32_t id = next_name_++;
        name_ids_.emplace(name, id);
        names_.emplace(id, name);
        return id;
    }

    std::uintptr_t MakeObject(const std::string& name, const std::uintptr_t klass) {
        const std::uintptr_t object = Allocate(0x400);
        const auto index = static_cast<std::uint32_t>(objects_.size());
        Put32(object + Offset("object.internal_index"), index);
        PutPointer(object + Offset("object.class"), klass);
        Put32(object + Offset("object.name_offset"), Intern(name));
        objects_.push_back(object);
        const std::uintptr_t item =
            chunk_ + static_cast<std::uintptr_t>(index) * kItemStride;
        PutPointer(item + kObjectOffset, object);
        Put32(item + kSerialOffset, 1000U + index);
        Put32(registry_ + Offset("objects.count"), index + 1U);
        registered_.emplace(object, static_cast<std::uintptr_t>(index));
        return object;
    }

    std::uintptr_t MakeClass(const std::string& name, const std::uintptr_t super) {
        const std::uintptr_t klass = MakeObject(name, 0);
        if (super != 0) PutPointer(klass + Offset("ustruct.super_struct"), super);
        return klass;
    }

    // One FField appended to the owner's child-property chain, which is how a
    // UClass or a UFunction exposes the members it declares itself.
    std::uintptr_t AddProperty(
        const std::uintptr_t owner, const std::string& name, const std::string& type,
        const std::uint32_t element_size, const std::uint32_t offset,
        const std::uintptr_t nested) {
        const std::uintptr_t field = Allocate(0x100);
        PutPointer(field + Offset("ffield.class"), FieldClass(type));
        Put32(field + Offset("ffield.name"), Intern(name));
        Put32(field + Offset("fproperty.element_size"), element_size);
        Put32(field + Offset("fproperty.array_dim"), 1);
        Put32(field + Offset("fproperty.offset_internal"), offset);
        if (nested != 0) {
            if (type == "StructProperty") {
                PutPointer(field + Offset("fstruct_property.struct"), nested);
            } else if (type == "ObjectProperty" || type == "ClassProperty") {
                PutPointer(field + Offset("fobject_property.property_class"), nested);
            }
        }
        AppendProperty(owner, field);
        return field;
    }

    std::uintptr_t AddArrayProperty(
        const std::uintptr_t owner, const std::string& name, const std::uint32_t offset,
        const std::string& inner_type, const std::uintptr_t inner_nested) {
        const std::uintptr_t field = Allocate(0x100);
        PutPointer(field + Offset("ffield.class"), FieldClass("ArrayProperty"));
        Put32(field + Offset("ffield.name"), Intern(name));
        Put32(field + Offset("fproperty.element_size"), 16);
        Put32(field + Offset("fproperty.array_dim"), 1);
        Put32(field + Offset("fproperty.offset_internal"), offset);
        const std::uintptr_t inner = Allocate(0x100);
        PutPointer(inner + Offset("ffield.class"), FieldClass(inner_type));
        Put32(inner + Offset("ffield.name"), Intern(name + "_Inner"));
        Put32(inner + Offset("fproperty.element_size"), 8);
        Put32(inner + Offset("fproperty.array_dim"), 1);
        if (inner_nested != 0) {
            PutPointer(inner + Offset("fobject_property.property_class"), inner_nested);
        }
        PutPointer(field + Offset("farray_property.inner"), inner);
        AppendProperty(owner, field);
        return field;
    }

    std::uintptr_t AddBoolProperty(
        const std::uintptr_t owner, const std::string& name, const std::uint32_t offset,
        const std::uint8_t byte_offset, const std::uint8_t byte_mask) {
        const std::uintptr_t field = Allocate(0x100);
        PutPointer(field + Offset("ffield.class"), FieldClass("BoolProperty"));
        Put32(field + Offset("ffield.name"), Intern(name));
        Put32(field + Offset("fproperty.element_size"), 1);
        Put32(field + Offset("fproperty.array_dim"), 1);
        Put32(field + Offset("fproperty.offset_internal"), offset);
        Put8(field + Offset("fbool_property.byte_offset"), byte_offset);
        Put8(field + Offset("fbool_property.byte_mask"), byte_mask);
        AppendProperty(owner, field);
        return field;
    }

    // A UFunction is a UStruct whose child properties are its parameters, and it
    // hangs off the owning UClass's UField chain.
    std::uintptr_t AddFunction(
        const std::uintptr_t owner, const std::string& name, const std::uint8_t num_parms,
        const std::uint16_t parms_size, const std::uint16_t return_value_offset) {
        const std::uintptr_t function = MakeObject(name, function_class_);
        Put8(function + Offset("ufunction.num_parms"), num_parms);
        PutBytes(function + Offset("ufunction.parms_size"), &parms_size, sizeof(parms_size));
        PutBytes(
            function + Offset("ufunction.return_value_offset"), &return_value_offset,
            sizeof(return_value_offset));
        AppendField(owner, function);
        return function;
    }

    void AppendProperty(const std::uintptr_t owner, const std::uintptr_t field) {
        const std::uintptr_t head = owner + Offset("ustruct.property_link");
        std::uintptr_t current{};
        static_cast<void>(Read(head, &current, sizeof(current)));
        if (current == 0) {
            PutPointer(head, field);
            return;
        }
        for (;;) {
            std::uintptr_t next{};
            static_cast<void>(Read(current + Offset("ffield.next"), &next, sizeof(next)));
            if (next == 0) break;
            current = next;
        }
        PutPointer(current + Offset("ffield.next"), field);
    }

    void AppendField(const std::uintptr_t owner, const std::uintptr_t field) {
        const std::uintptr_t head = owner + Offset("ustruct.children");
        std::uintptr_t current{};
        static_cast<void>(Read(head, &current, sizeof(current)));
        if (current == 0) {
            PutPointer(head, field);
            return;
        }
        for (;;) {
            std::uintptr_t next{};
            static_cast<void>(Read(current + Offset("ufield.next"), &next, sizeof(next)));
            if (next == 0) break;
            current = next;
        }
        PutPointer(current + Offset("ufield.next"), field);
    }

    std::uintptr_t FieldClass(const std::string& type) {
        const auto found = field_classes_.find(type);
        if (found != field_classes_.end()) return found->second;
        const std::uintptr_t object = Allocate(0x20);
        Put32(object + Offset("ffield_class.name"), Intern(type));
        field_classes_.emplace(type, object);
        return object;
    }

    const jelly::Layout& layout_;
    Options options_;
    std::map<std::uintptr_t, std::vector<std::uint8_t>> blocks_;
    std::uintptr_t next_address_{0x10000};
    std::map<std::string, std::uint32_t> name_ids_;
    std::map<std::uint32_t, std::string> names_;
    std::uint32_t next_name_{1};
    std::map<std::uintptr_t, std::uintptr_t> registered_;
    std::vector<std::uintptr_t> objects_;
    std::map<std::string, std::uintptr_t> field_classes_;
    std::vector<std::uint8_t> process_event_prologue_;
    std::uintptr_t object_class_{};
    std::uintptr_t actor_class_{};
    std::uintptr_t character_class_{};
    std::uintptr_t function_class_{};
    std::uintptr_t struct_class_{};
    std::uintptr_t vector_struct_{};
    std::uintptr_t rotator_struct_{};
    std::uintptr_t hit_result_struct_{};
    std::uintptr_t body_struct_{};
    std::uintptr_t scale_function_{};
    std::uintptr_t rotation_function_{};
    std::uintptr_t scene_component_class_{};
    std::uintptr_t primitive_component_class_{};
    std::uintptr_t mesh_component_class_{};
    std::uintptr_t skeletal_mesh_class_{};
    std::uintptr_t chunk_{};
    std::uintptr_t registry_{};
    std::uintptr_t process_event_{};
};

// The engine side of the reflected call: a real SetRelativeScale3D copies its
// parameter into the component's RelativeScale3D property.
class FakeCall final : public jelly::ReflectionCall {
public:
    explicit FakeCall(SyntheticUe& image) : image_(image) {}

    bool engine_writes{true};
    // Whether a rotation the engine is handed survives. A game that drives a component's
    // orientation itself takes it back, which is what this stands in for.
    bool rotation_sticks{true};
    int calls{};
    std::size_t last_parms_size{};

    bool ProcessEvent(
        const std::uintptr_t object, const std::uintptr_t function,
        void* const parms, const std::size_t parms_size) noexcept override {
        ++calls;
        last_parms_size = parms_size;
        if (!engine_writes || parms == nullptr || parms_size < sizeof(jelly::Vector3)) {
            return true;
        }
        if (function != 0 && function == image_.rotation_function()) {
            if (!rotation_sticks) return true;
            jelly::Vector3 rotation{};
            std::memcpy(rotation.data(), parms, sizeof(rotation));
            image_.SetRotation(object, rotation);
            return true;
        }
        jelly::Vector3 scale{};
        std::memcpy(scale.data(), parms, sizeof(scale));
        image_.SetScale(object, scale);
        return true;
    }

private:
    SyntheticUe& image_;
};

// Where the module's naming notes land: a refusal that names the key it could not
// find has to be observable without a game.
class FakeLog final : public jelly::DiagnosticSink {
public:
    void Note(const std::string_view message) noexcept override {
        notes.emplace_back(message);
    }

    std::vector<std::string> notes;
};

struct Harness {
    SyntheticUe image;
    FakeCall call{image};
    FakeLog log;
    jelly::ModelAccess model;

    explicit Harness(const jelly::Layout& layout, const SyntheticUe::Options options = {})
        : image(layout, options), model(layout, image, image, call, log) {}

    bool Prepare() {
        jelly::Skip skip = jelly::Skip::none;
        return model.Prepare(image.registry_address(), image.process_event_address(), skip);
    }
};

struct ActorFixture {
    std::uintptr_t actor{};
    std::uintptr_t mesh{};
    std::uint32_t index{};
};

inline ActorFixture BuildSimpleActor(
    SyntheticUe& image, const std::string& name, const jelly::Vector3& scale) {
    ActorFixture fixture;
    fixture.actor = image.MakeActor(name);
    fixture.mesh = image.MakeComponent(name + "_Mesh", fixture.actor, scale);
    image.SetRoot(fixture.actor, fixture.mesh);
    fixture.index = static_cast<std::uint32_t>(image.index_of(fixture.actor));
    return fixture;
}

// The resource that actually ships in the package.
inline jelly::Layout LoadShippedLayout() {
    std::ifstream stream(JELLY_LAYOUT_RESOURCE_PATH, std::ios::binary);
    const std::string document{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    jelly::Layout layout;
    std::string error;
    if (!jelly::ParseLayout(document, layout, error)) {
        std::printf("  FAIL: the shipped layout resource does not parse: %s\n", error.c_str());
        ++g_failures;
    }
    return layout;
}

inline const jelly::Layout& ShippedLayout() {
    static const jelly::Layout layout = LoadShippedLayout();
    return layout;
}

int RunLogicTests();
int RunModelTests();

}  // namespace jelly_tests
