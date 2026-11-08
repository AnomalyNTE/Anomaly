#pragma once
#if 0 // Retired vehicle cleanup path; kept as source reference only.
#include "native_access.hpp"
#include <cstring>
#include <map>
#include <set>

namespace mod_manager {
// Invoked once for an explicit cleanup, on Game after the engine tick has completed.
// A missing Actor is insufficient: Mass entities can keep the same template at lower LOD.
class VehicleTemplates {
public:
    enum class Result { Released, InUse, Unavailable };
private:
    NativeAccess& native_;
    struct Array { uintptr_t data{}; int32_t count{}, capacity{}; };
    struct Shared { uintptr_t memory{}, controller{}; bool operator==(const Shared&) const = default; };
    struct SharedType { uintptr_t type{}; Array instances; int32_t next{}, hash{}; };
    struct HashIndex { uint32_t key{}; int32_t index{}, next{}, hash{}; };
    struct SharedRemoval { int32_t index{}, typed_index{}, hash_slot{}; };
    struct ActorTemplate { uintptr_t type{}; uint32_t references{}, owned{}; };
    struct Template { int32_t index{}; uintptr_t object{}, controller{}; int16_t high{-1}, low{-1}; uint16_t visual{UINT16_MAX}; };
    struct ObjectKey { int32_t index{-1}, serial{}; bool operator==(const ObjectKey&) const = default; };
    struct VisualIndex { ObjectKey key; int32_t index{}, next{}, bucket{}; };
    struct MeshKey { uint32_t hash{}; ObjectKey key; int32_t next{}, bucket{}; };
    struct DirtyBits { std::array<uint32_t, 4> words{}; uintptr_t data{}; int32_t count{}, capacity{}; };
    struct alignas(16) Transform { double rotation[4]{0, 0, 0, 1}, translation[4]{}, scale[4]{1, 1, 1, 0}; };
    struct VisualComponent { Handle handle{}; uintptr_t object{}, shared{}; int32_t component_slot{-1}, shared_slot{-1}, shared_index{-1}; };
    struct Visuals {
        uintptr_t component{}, type{}, dirty_words{};
        Array infos{};
        std::set<uint16_t> indices;
        std::vector<VisualComponent> components;
        std::vector<int32_t> mesh_slots;
    };
    struct Sparse {
        NativeAccess::ClassSet header{};
        std::vector<uint32_t> bits;
        bool Has(int32_t index) const {
            return index >= 0 && index < header.count && (bits[index / 32] & (uint32_t{1} << (index % 32)));
        }
    };
    template<class T> bool Read(uintptr_t at, T& value) const { return native_.Read(at, value); }
    bool Bytes(uintptr_t at, void* value, size_t size) const {
        return !size || (at > 65536 && native_.core->read_memory(native_.core->user, at,
            {static_cast<uint8_t*>(value), size}).code == 0);
    }
    bool ReadArray(uintptr_t at, Array& value, int32_t limit) const {
        return Read(at, value) && value.count >= 0 && value.count <= limit && value.capacity >= value.count &&
            (!value.count || value.data > 65536);
    }
    bool ReadSparse(uintptr_t at, Sparse& value) const {
        auto& h = value.header;
        if (!Read(at, h) || h.count < 0 || h.count > 4096 || h.capacity < h.count ||
            h.bit_count != h.count || h.bit_capacity < h.count || h.free_count < 0 || h.free_count > h.count ||
            (h.count && !h.data) || (!h.indirect_bits && h.count > sizeof(h.bits) * 8)) return false;
        value.bits.resize((h.count + 31) / 32);
        return Bytes(h.indirect_bits ? h.indirect_bits : at + offsetof(NativeAccess::ClassSet, bits),
            value.bits.data(), value.bits.size() * sizeof(uint32_t));
    }
    bool RepresentationLayout(uintptr_t type) const {
        const auto& l = native_.l;
        uint32_t size{};
        if (!type || !Read(type + l.StructSize, size) || size != l.RepresentationSize) return false;
        uintptr_t field{};
        if (!Read(type + l.StructProperties, field)) return false;
        unsigned found{};
        for (unsigned i = 0; field && i < 64; ++i) {
            const auto name = native_.Name(field + l.PropertyName);
            for (const auto& [key, offset] : std::array<std::pair<std::string_view, uint32_t>, 3>{{
                    {"HighResTemplateActorIndex", l.RepresentationHigh}, {"LowResTemplateActorIndex", l.RepresentationLow},
                    {"StaticMeshDescHandle", l.RepresentationVisual}}}) {
                if (name != key) continue;
                uint32_t actual{}, element{}, dimension{};
                if (!Read(field + l.PropertyOffset, actual) || actual != offset ||
                    !Read(field + l.PropertySize, element) || element != sizeof(uint16_t) ||
                    !Read(field + l.PropertyDim, dimension) || dimension != 1) return false;
                ++found;
            }
            if (!Read(field + l.PropertyNext, field)) return false;
        }
        return found == 3;
    }
    bool ReadRepresentation(uintptr_t at, Template& value) const {
        const auto& l = native_.l;
        return Read(at + l.RepresentationHigh, value.high) && Read(at + l.RepresentationLow, value.low) &&
            Read(at + l.RepresentationVisual, value.visual);
    }
    bool Property(uintptr_t type, std::string_view name, std::string_view kind, uint32_t offset, uint32_t size) const {
        const auto& l = native_.l;
        uintptr_t field{};
        if (!Read(type + l.StructProperties, field)) return false;
        for (unsigned i = 0; field && i < 128; ++i) {
            if (native_.Name(field + l.PropertyName) == name) {
                uintptr_t cls{}; uint32_t actual{}, element{}, dimension{};
                return Read(field + l.PropertyClass, cls) && native_.Name(cls + l.FieldClassName) == kind &&
                    Read(field + l.PropertyOffset, actual) && actual == offset &&
                    Read(field + l.PropertySize, element) && element == size &&
                    Read(field + l.PropertyDim, dimension) && dimension == 1;
            }
            if (!Read(field + l.PropertyNext, field)) return false;
        }
        return false;
    }
    bool VisualLayout(uintptr_t& info_type) const {
        const auto& l = native_.l;
        info_type = native_.Resolve(native_.Find("/Script/MassRepresentation.MassInstancedStaticMeshInfo"));
        const auto desc = native_.Resolve(native_.Find("/Script/MassRepresentation.StaticMeshInstanceVisualizationDesc"));
        const auto mesh = native_.Resolve(native_.Find("/Script/MassRepresentation.MassStaticMeshInstanceVisualizationMeshDesc"));
        const auto component = native_.Resolve(native_.Find("/Script/MassRepresentation.MassVisualizationComponent"));
        const auto subsystem = native_.Resolve(native_.Find("/Script/MassRepresentation.MassRepresentationSubsystem"));
        for (const auto& [type, expected] : std::array<std::pair<uintptr_t, uint32_t>, 4>{{
                {info_type, l.VisualInfoSize}, {desc, l.VisualDescSize}, {mesh, l.VisualMeshSize}, {component, l.VisualSize}}}) {
            uint32_t size{};
            if (!type || !Read(type + l.StructSize, size) || size != expected) return false;
        }
        uint16_t alignment{};
        return Read(info_type + l.StructAlignment, alignment) && alignment == alignof(Transform) &&
            l.VisualMapStride == sizeof(VisualIndex) && sizeof(VisualIndex) == sizeof(MeshKey) &&
            l.VisualMapValue == offsetof(VisualIndex, index) && l.VisualMapNext == offsetof(VisualIndex, next) &&
            l.VisualMapBucket == offsetof(VisualIndex, bucket) && l.VisualMeshMapKey == offsetof(MeshKey, key) &&
            l.VisualSharedUpdates == l.VisualSharedTransform + sizeof(Transform) &&
            l.VisualSharedRemoves == l.VisualSharedUpdates + 3 * sizeof(Array) &&
            l.VisualSharedKey == l.VisualSharedRemoves + 2 * sizeof(Array) + sizeof(int32_t) &&
            Property(subsystem, "VisualizationComponent", "ObjectProperty", l.VisualizationComponent, sizeof(uintptr_t)) &&
            Property(component, "InstancedStaticMeshInfos", "ArrayProperty", l.VisualInfos, sizeof(Array)) &&
            Property(info_type, "Desc", "StructProperty", l.VisualDesc, l.VisualDescSize) &&
            Property(info_type, "InstancedStaticMeshComponents", "ArrayProperty", l.VisualComponents, sizeof(Array)) &&
            Property(info_type, "LODSignificanceRanges", "ArrayProperty", l.VisualRanges, sizeof(Array)) &&
            Property(desc, "Meshes", "ArrayProperty", l.VisualMeshes, sizeof(Array)) &&
            Property(mesh, "Mesh", "ObjectProperty", l.VisualMeshObject, sizeof(uintptr_t));
    }
    Result ReadVisuals(uintptr_t subsystem, const std::vector<std::string>& tags, Visuals& out) const {
        const auto& l = native_.l;
        const auto visual_class = native_.Resolve(native_.Find("/Script/MassRepresentation.MassVisualizationComponent"));
        if (!VisualLayout(out.type) || !Read(subsystem + l.VisualizationComponent, out.component) ||
            !native_.IsA(out.component, visual_class) || !ReadArray(out.component + l.VisualInfos, out.infos, 4096)) return Result::Unavailable;
        std::map<uintptr_t, std::vector<uint16_t>> owners;
        for (int32_t i = 0; i < out.infos.count; ++i) {
            const auto info = out.infos.data + uint64_t(i) * l.VisualInfoSize;
            Array meshes{}, components{};
            if (!ReadArray(info + l.VisualDesc + l.VisualMeshes, meshes, 256) || !ReadArray(info + l.VisualComponents, components, 256)) return Result::Unavailable;
            for (int32_t m = 0; m < meshes.count; ++m) {
                uintptr_t mesh{};
                if (!Read(meshes.data + uint64_t(m) * l.VisualMeshSize + l.VisualMeshObject, mesh)) return Result::Unavailable;
                if (!mesh) continue;
                const auto name = native_.Name(mesh + l.ObjectName);
                if (std::any_of(tags.begin(), tags.end(), [&](const auto& tag) { return NativeAccess::MatchesVehicle(name, tag); }))
                    out.indices.insert(static_cast<uint16_t>(i));
            }
            for (int32_t c = 0; c < components.count; ++c) {
                uintptr_t component{};
                if (!Read(components.data + uint64_t(c) * sizeof(uintptr_t), component) || !component) return Result::Unavailable;
                owners[component].push_back(static_cast<uint16_t>(i));
            }
        }
        if (out.indices.empty()) return Result::Released;
        Array pending{}, shared{}, free_infos{}, free_shared{};
        DirtyBits dirty{}; Sparse component_map{}, shared_map{}, mesh_map{};
        if (!ReadArray(out.component + l.VisualPending, pending, 4096) || !ReadArray(out.component + l.VisualShared, shared, 4096) ||
            !ReadArray(out.component + l.VisualFree, free_infos, out.infos.count) || !ReadArray(out.component + l.VisualSharedFree, free_shared, shared.count) ||
            !Read(out.component + l.VisualDirty, dirty) || dirty.count != shared.count || dirty.capacity < dirty.count ||
            (!dirty.data && dirty.count > sizeof(dirty.words) * 8) ||
            !ReadSparse(out.component + l.VisualComponentMap, component_map) || !ReadSparse(out.component + l.VisualSharedMap, shared_map) ||
            !ReadSparse(out.component + l.VisualMeshMap, mesh_map)) return Result::Unavailable;
        // Construction can introduce a new borrower of an existing component at the next tick.
        if (pending.count) return Result::InUse;
        out.dirty_words = dirty.data ? dirty.data : out.component + l.VisualDirty;
        std::vector<uint16_t> free_visual_indices(free_infos.count);
        std::vector<int32_t> free_shared_indices(free_shared.count);
        std::vector<VisualIndex> component_entries(component_map.header.count), shared_entries(shared_map.header.count);
        std::vector<MeshKey> mesh_entries(mesh_map.header.count);
        if (!Bytes(free_infos.data, free_visual_indices.data(), free_visual_indices.size() * sizeof(uint16_t)) ||
            !Bytes(free_shared.data, free_shared_indices.data(), free_shared_indices.size() * sizeof(int32_t)) ||
            !Bytes(component_map.header.data, component_entries.data(), component_entries.size() * sizeof(VisualIndex)) ||
            !Bytes(shared_map.header.data, shared_entries.data(), shared_entries.size() * sizeof(VisualIndex)) ||
            !Bytes(mesh_map.header.data, mesh_entries.data(), mesh_entries.size() * sizeof(MeshKey))) return Result::Unavailable;
        for (const auto index : free_visual_indices) if (out.indices.contains(index)) return Result::Unavailable;
        const auto component_class = native_.Resolve(native_.Find("/Script/Engine.InstancedStaticMeshComponent"));
        std::set<int32_t> selected_shared;
        for (const auto& [component, references] : owners) {
            if (std::none_of(references.begin(), references.end(), [&](auto i) { return out.indices.contains(i); })) continue;
            if (std::any_of(references.begin(), references.end(), [&](auto i) { return !out.indices.contains(i); })) return Result::InUse;
            VisualComponent value{}; value.object = component;
            uint32_t index{}; AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
            if (!native_.IsA(component, component_class) || !Read(component + l.ObjectIndex, index) ||
                native_.objects->snapshot_at(native_.objects->user, index, &snapshot).code || native_.Resolve(snapshot.handle) != component) return Result::Unavailable;
            value.handle = snapshot.handle;
            const ObjectKey key{static_cast<int32_t>(ANOMALY_UE5_OBJECT_HANDLE_INDEX(value.handle)),
                static_cast<int32_t>(ANOMALY_UE5_OBJECT_HANDLE_SERIAL(value.handle))};
            for (int32_t i = 0; i < component_map.header.count; ++i) {
                if (!component_map.Has(i) || component_entries[i].key != key) continue;
                if (value.component_slot != -1 || !out.indices.contains(static_cast<uint16_t>(component_entries[i].index))) return Result::Unavailable;
                value.component_slot = i;
            }
            for (int32_t i = 0; i < shared_map.header.count; ++i) {
                if (!shared_map.Has(i) || shared_entries[i].key != key) continue;
                if (value.shared_slot != -1) return Result::Unavailable;
                value.shared_slot = i; value.shared_index = shared_entries[i].index;
            }
            if (value.component_slot == -1 || value.shared_slot == -1 || value.shared_index < 0 || value.shared_index >= shared.count ||
                !selected_shared.insert(value.shared_index).second ||
                std::find(free_shared_indices.begin(), free_shared_indices.end(), value.shared_index) != free_shared_indices.end()) return Result::Unavailable;
            value.shared = shared.data + uint64_t(value.shared_index) * l.VisualSharedSize;
            ObjectKey actual{}; int32_t count{}, instances{}; Array updates{}, removes{};
            if (!Read(value.shared + l.VisualSharedKey, actual) || actual != key ||
                !Read(value.shared + l.VisualSharedReferences, count) || count != references.size() ||
                !ReadArray(value.shared + l.VisualSharedUpdates, updates, 65536) || !ReadArray(value.shared + l.VisualSharedRemoves, removes, 65536) ||
                !native_.Invoke(value.handle, "/Script/Engine.InstancedStaticMeshComponent:GetInstanceCount", &instances, sizeof(instances),
                    "ReturnValue", "IntProperty", true)) return Result::Unavailable;
            if (instances || updates.count || removes.count) return Result::InUse;
            for (int32_t i = 0; i < mesh_map.header.count; ++i)
                if (mesh_map.Has(i) && mesh_entries[i].key == key) out.mesh_slots.push_back(i);
            out.components.push_back(value);
        }
        return Result::Released;
    }
    template<class T> static void ReserveIndices(uintptr_t grow, Array& array, size_t additional) {
        while (size_t(array.capacity) < size_t(array.count) + additional)
            reinterpret_cast<int32_t(__fastcall*)(uint16_t, uintptr_t*, int32_t*)>(grow)(
                uint16_t(sizeof(T) | (alignof(T) << 8)), &array.data, &array.capacity);
    }
    bool RetireVisuals(const Visuals& plan) const noexcept {
        __try {
            const auto& l = native_.l;
            auto& free_infos = *reinterpret_cast<Array*>(plan.component + l.VisualFree);
            auto& free_shared = *reinterpret_cast<Array*>(plan.component + l.VisualSharedFree);
            ReserveIndices<uint16_t>(native_.grow_array, free_infos, plan.indices.size());
            ReserveIndices<int32_t>(native_.grow_array, free_shared, plan.components.size());
            auto remove = reinterpret_cast<void(__fastcall*)(uintptr_t, int32_t)>(native_.remove_visual_hash);
            for (const auto slot : plan.mesh_slots) remove(plan.component + l.VisualMeshMap, slot);
            for (const auto& value : plan.components) {
                remove(plan.component + l.VisualComponentMap, value.component_slot);
                remove(plan.component + l.VisualSharedMap, value.shared_slot);
                reinterpret_cast<uint32_t*>(plan.dirty_words)[value.shared_index / 32] &= ~(uint32_t{1} << (value.shared_index % 32));
                reinterpret_cast<void(__fastcall*)(uintptr_t, int32_t)>(native_.destroy_visual_shared)(value.shared, 1);
                std::memset(reinterpret_cast<void*>(value.shared), 0, l.VisualSharedSize);
                const Transform identity{};
                std::memcpy(reinterpret_cast<void*>(value.shared + l.VisualSharedTransform), &identity, sizeof(identity));
                *reinterpret_cast<ObjectKey*>(value.shared + l.VisualSharedKey) = {};
                // The Robin Hood map contains inline storage and sentinels; zero bytes are not a valid empty map.
                reinterpret_cast<void(__fastcall*)(uintptr_t)>(native_.initialize_instance_map)(value.shared + l.VisualSharedInnerMap);
                reinterpret_cast<int32_t*>(free_shared.data)[free_shared.count++] = value.shared_index;
            }
            for (const auto index : plan.indices) {
                const auto info = plan.infos.data + uint64_t(index) * l.VisualInfoSize;
                reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, int32_t)>(native_.destroy_struct)(plan.type, info, 1);
                reinterpret_cast<void(__fastcall*)(uintptr_t, uintptr_t, int32_t)>(native_.initialize_struct)(plan.type, info, 1);
                reinterpret_cast<uint16_t*>(free_infos.data)[free_infos.count++] = index;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    Result ReleaseVisuals(const Visuals& plan) const {
        if (plan.indices.empty()) return Result::Released;
        if (!RetireVisuals(plan)) return Result::Unavailable;
        for (const auto& value : plan.components) {
            // K2 accepts the component itself as the authorized owner. DestroyComponent unregisters
            // rendering and detaches it from MassVisualizer, releasing its mesh/material references at GC.
            auto self = value.object;
            if (!native_.Invoke(value.handle, "/Script/Engine.ActorComponent:K2_DestroyComponent", &self, sizeof(self),
                    "Object", "ObjectProperty", false, "/Script/CoreUObject.Object")) return Result::Unavailable;
        }
        return Result::Released;
    }
    bool Unused(uintptr_t manager, uintptr_t representation, const std::map<int16_t, ActorTemplate>& targets,
                const std::set<uint16_t>& visuals, bool& in_use) const {
        const auto& l = native_.l;
        Array archetypes{};
        if (!manager || !ReadArray(manager + l.ManagerArchetypes, archetypes, 16384)) return false;
        for (int32_t i = 0; i < archetypes.count; ++i) {
            uintptr_t archetype{}, configs{}; int32_t count{};
            if (!Read(archetypes.data + uint64_t(i) * 2 * sizeof(uintptr_t), archetype) || !archetype ||
                !Read(archetype + l.ArchetypeFragmentData, configs) || !Read(archetype + l.ArchetypeFragmentCount, count) ||
                count < 0 || count > 256) return false;
            if (!configs) {
                if (uint64_t(count) * l.FragmentConfigStride > l.ArchetypeFragmentData - l.ArchetypeFragments) return false;
                configs = archetype + l.ArchetypeFragments;
            }
            int32_t fragment_offset{-1};
            for (int32_t f = 0; f < count; ++f) {
                uintptr_t type{};
                const auto config = configs + uint64_t(f) * l.FragmentConfigStride;
                if (!Read(config, type)) return false;
                if (type == representation && !Read(config + l.FragmentConfigOffset, fragment_offset)) return false;
            }
            if (fragment_offset == -1) continue;
            Array chunks{}; int32_t capacity{};
            if (fragment_offset < 0 || !ReadArray(archetype + l.ArchetypeChunks, chunks, 65536) ||
                !Read(archetype + l.ArchetypeCapacity, capacity) || capacity < 1 || capacity > 65536) return false;
            for (int32_t c = 0; c < chunks.count; ++c) {
                const auto chunk = chunks.data + uint64_t(c) * l.MassChunkStride;
                uintptr_t memory{}; uint64_t bytes{}; int32_t entities{};
                if (!Read(chunk, memory) || !Read(chunk + l.MassChunkBytes, bytes) ||
                    !Read(chunk + l.MassChunkCount, entities) || entities < 0 || entities > capacity) return false;
                if (!entities) continue;
                const auto length = uint64_t(entities) * l.RepresentationSize;
                if (!memory || length > 1024 * 1024 || uint64_t(fragment_offset) + length > bytes) return false;
                std::vector<uint8_t> fragments(static_cast<size_t>(length));
                if (!Bytes(memory + fragment_offset, fragments.data(), fragments.size())) return false;
                for (int32_t e = 0; e < entities; ++e) {
                    const auto* value = fragments.data() + uint64_t(e) * l.RepresentationSize;
                    int16_t high{}, low{}; uint16_t visual{};
                    std::memcpy(&high, value + l.RepresentationHigh, sizeof(high));
                    std::memcpy(&low, value + l.RepresentationLow, sizeof(low));
                    std::memcpy(&visual, value + l.RepresentationVisual, sizeof(visual));
                    if (targets.contains(high) || targets.contains(low) || visuals.contains(visual)) { in_use = true; return true; }
                }
            }
        }
        return true;
    }
    static bool Remove(uintptr_t function, uintptr_t map, int32_t index) noexcept {
        __try { reinterpret_cast<void(__fastcall*)(uintptr_t, int32_t)>(function)(map, index); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool Release(uintptr_t function, uintptr_t subsystem, uintptr_t type) noexcept {
        __try { reinterpret_cast<void(__fastcall*)(uintptr_t, const uintptr_t*)>(function)(subsystem, &type); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void RemoveSharedValue(uintptr_t destroy, uintptr_t type, Array& array, int32_t index) {
        auto* values = reinterpret_cast<Shared*>(array.data);
        reinterpret_cast<void(__fastcall*)(uintptr_t, void*, int32_t)>(destroy)(type, values + index, 1);
        // UE TArray relocates shared pointers without incrementing their reference counts.
        // Keep its allocation/capacity; only the live range owns these relocated values.
        std::memmove(values + index, values + index + 1, size_t(array.count - index - 1) * sizeof(Shared));
        --array.count;
    }
    static bool RetireShared(uintptr_t remove_hash, uintptr_t destroy, uintptr_t type,
                             uintptr_t container, uintptr_t hash, uintptr_t typed,
                             std::vector<HashIndex>& indices, const Sparse& slots,
                             std::vector<SharedRemoval>& removals) noexcept {
        __try {
            auto& instances = *reinterpret_cast<Array*>(container);
            auto& typed_instances = *reinterpret_cast<Array*>(typed);
            auto* hash_values = reinterpret_cast<HashIndex*>(slots.header.data);
            // Descending instance indices remain valid as the dense array contracts.
            for (size_t r = removals.size(); r-- > 0;) {
                const auto removed = removals[r];
                reinterpret_cast<void(__fastcall*)(uintptr_t, int32_t)>(remove_hash)(hash, removed.hash_slot);
                RemoveSharedValue(destroy, type, typed_instances, removed.typed_index);
                RemoveSharedValue(destroy, type, instances, removed.index);
                indices[removed.hash_slot].index = -1;
                for (int32_t i = 0; i < slots.header.count; ++i) {
                    if (slots.Has(i) && indices[i].index > removed.index)
                        hash_values[i].index = --indices[i].index;
                }
                for (size_t i = 0; i < r; ++i)
                    if (removals[i].typed_index > removed.typed_index) --removals[i].typed_index;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool PhysicsKeys(const std::set<uintptr_t>& classes, std::vector<uintptr_t>& keys) const {
        const auto& l = native_.l;
        const auto type = native_.Resolve(native_.Find("/Script/MassTraffic.MassTrafficSubsystem"));
        const auto subsystem = native_.WorldSubsystem("/Script/MassTraffic.MassTrafficSubsystem");
        uint32_t size{}; Array templates{};
        if (!type || !subsystem || !Read(type + l.StructSize, size) || size != l.TrafficSubsystemSize ||
            !ReadArray(subsystem + l.PhysicsTemplates, templates, 4096)) return false;
        for (int32_t i = 0; i < templates.count; ++i) {
            uintptr_t entry{}, cls{};
            if (!Read(templates.data + uint64_t(i) * sizeof(uintptr_t), entry) || !entry ||
                !Read(entry + l.PhysicsTemplateClass, cls)) return false;
            if (classes.contains(cls)) keys.push_back(entry + l.PhysicsTemplateClass);
        }
        return true;
    }
    static bool InvalidatePhysicsKeys(const std::vector<uintptr_t>& keys) noexcept {
        __try {
            // The traffic subsystem's physics cache borrows UClass without a GC reference.
            // Dropping the simulation parameters first leaves a dangling lookup key at GC.
            // Null keys are skipped by GetOrCreateVehiclePhysicsTemplate. Keep the template
            // allocation alive for raw borrowers in physics shared fragments; the native
            // subsystem destructor owns its eventual destruction when the world ends.
            for (const auto key : keys) *reinterpret_cast<uintptr_t*>(key) = 0;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    Result ClearPhysics(uintptr_t subsystem, const std::vector<std::string>& tags) const {
        const auto& l = native_.l;
        const auto simulation = native_.Resolve(native_.Find("/Script/MassTraffic.MassTrafficVehicleSimulationParameters"));
        const auto shared = native_.Resolve(native_.Find("/Script/CoreUObject.ConstSharedStruct"));
        uint32_t size{}, shared_size{}; uint16_t alignment{}; uintptr_t field{}, manager{};
        if (!simulation || !shared || l.SharedTypeStride != sizeof(SharedType) || l.SharedHashStride != sizeof(HashIndex) ||
            l.SharedTypeInstances != offsetof(SharedType, instances) || l.SharedHashIndex != offsetof(HashIndex, index) ||
            l.SharedValueController != offsetof(Shared, controller) ||
            !Read(simulation + l.StructSize, size) || size != l.SimulationSize ||
            !Read(shared + l.StructSize, shared_size) || shared_size != sizeof(Shared) ||
            !Read(simulation + l.StructAlignment, alignment) || alignment != alignof(uintptr_t) ||
            l.SharedMemoryData != sizeof(uintptr_t) || !Read(simulation + l.StructProperties, field) ||
            !Read(subsystem + l.RepresentationManager, manager) || !manager) return Result::Unavailable;
        bool property_found{};
        for (unsigned i = 0; field && i < 64; ++i) {
            if (native_.Name(field + l.PropertyName) == "PhysicsVehicleTemplateActor") {
                uintptr_t type{}; uint32_t offset{}, element{}, dimension{};
                if (!Read(field + l.PropertyClass, type) || native_.Name(type + l.FieldClassName) != "ClassProperty" ||
                    !Read(field + l.PropertyOffset, offset) || offset != l.SimulationClass ||
                    !Read(field + l.PropertySize, element) || element != sizeof(uintptr_t) ||
                    !Read(field + l.PropertyDim, dimension) || dimension != 1) return Result::Unavailable;
                property_found = true;
                break;
            }
            if (!Read(field + l.PropertyNext, field)) return Result::Unavailable;
        }
        if (!property_found) return Result::Unavailable;
        const auto container = manager + l.ManagerConstShared;
        Array instances{}, typed_instances{}; Sparse hashes{}, types{};
        if (!ReadArray(container, instances, 4096) || !ReadSparse(container + l.SharedHash, hashes) ||
            !ReadSparse(container + l.SharedTypes, types) ||
            hashes.header.count - hashes.header.free_count != instances.count) return Result::Unavailable;
        uintptr_t typed{};
        for (int32_t i = 0; i < types.header.count; ++i) {
            if (!types.Has(i)) continue;
            const auto at = types.header.data + uint64_t(i) * sizeof(SharedType);
            uintptr_t type{};
            if (!Read(at, type)) return Result::Unavailable;
            if (type != simulation) continue;
            if (typed || !ReadArray(at + offsetof(SharedType, instances), typed_instances, 4096)) return Result::Unavailable;
            typed = at + offsetof(SharedType, instances);
        }
        std::vector<Shared> values(instances.count), typed_values(typed_instances.count);
        std::vector<HashIndex> indices(hashes.header.count);
        std::vector<int32_t> hash_slots(instances.count, -1);
        if (!Bytes(instances.data, values.data(), values.size() * sizeof(Shared)) ||
            !Bytes(typed_instances.data, typed_values.data(), typed_values.size() * sizeof(Shared)) ||
            !Bytes(hashes.header.data, indices.data(), indices.size() * sizeof(HashIndex))) return Result::Unavailable;
        for (int32_t i = 0; i < hashes.header.count; ++i) {
            if (!hashes.Has(i)) continue;
            const auto index = indices[i].index;
            if (index < 0 || index >= instances.count || hash_slots[index] != -1) return Result::Unavailable;
            hash_slots[index] = i;
        }
        const auto class_type = native_.Resolve(native_.Find("/Script/CoreUObject.Class"));
        std::vector<SharedRemoval> removals;
        std::set<uintptr_t> retired_classes;
        for (int32_t i = 0; i < instances.count; ++i) {
            const auto value = values[i]; uintptr_t type{}, cls{};
            if (!value.memory || !value.controller || !Read(value.memory, type)) return Result::Unavailable;
            if (type != simulation) continue;
            if (!Read(value.memory + l.SharedMemoryData + l.SimulationClass, cls)) return Result::Unavailable;
            if (!cls) continue;
            if (!native_.IsA(cls, class_type)) return Result::Unavailable;
            const auto name = native_.Name(cls + l.ObjectName);
            if (std::none_of(tags.begin(), tags.end(), [&](const auto& tag) { return NativeAccess::MatchesVehicle(name, tag); })) continue;
            const auto found = std::find(typed_values.begin(), typed_values.end(), value);
            if (found == typed_values.end() || std::find(found + 1, typed_values.end(), value) != typed_values.end()) return Result::Unavailable;
            uint32_t references{};
            if (!Read(value.controller + l.SharedReferences, references)) return Result::Unavailable;
            // Instances and TypeToInstanceMap each own one reference. Any additional
            // owner can be a template, an entity chunk, or a processor's borrowed copy.
            if (references != 2) return Result::InUse;
            removals.push_back({i, static_cast<int32_t>(found - typed_values.begin()), hash_slots[i]});
            retired_classes.insert(cls);
        }
        if (removals.empty()) return Result::Released;
        std::vector<uintptr_t> physics_keys;
        if (!PhysicsKeys(retired_classes, physics_keys) || !InvalidatePhysicsKeys(physics_keys)) return Result::Unavailable;
        return RetireShared(native_.remove_shared_hash, native_.destroy_struct, shared, container,
            container + l.SharedHash, typed, indices, hashes, removals) ? Result::Released : Result::Unavailable;
    }
public:
    explicit VehicleTemplates(NativeAccess& native) : native_(native) {}
    Result Clear(const std::vector<std::string>& tags) const {
        const auto& l = native_.l;
        const auto subsystem = native_.WorldSubsystem("/Script/MassRepresentation.MassRepresentationSubsystem");
        const auto spawner = native_.WorldSubsystem("/Script/MassSpawner.MassSpawnerSubsystem");
        const auto fragment = native_.Resolve(native_.Find("/Script/MassRepresentation.MassRepresentationFragment"));
        Sparse actors{}, templates{};
        if (!subsystem || !spawner || !RepresentationLayout(fragment) ||
            !ReadSparse(subsystem + l.TemplateActors, actors) || !ReadSparse(spawner + l.TemplateMap, templates)) return Result::Unavailable;
        Visuals visual_plan;
        const auto visual_result = ReadVisuals(subsystem, tags, visual_plan);
        if (visual_result != Result::Released) return visual_result;
        const auto class_type = native_.Resolve(native_.Find("/Script/CoreUObject.Class"));
        std::map<int16_t, ActorTemplate> classes, targets;
        for (int32_t i = 0; i < actors.header.count; ++i) {
            if (!actors.Has(i)) continue;
            const auto at = actors.header.data + uint64_t(i) * l.TemplateActorStride;
            ActorTemplate value{};
            if (!Read(at, value.type) || !native_.IsA(value.type, class_type) ||
                !Read(at + l.TemplateActorReferences, value.references) || !value.references) return Result::Unavailable;
            classes.emplace(static_cast<int16_t>(i), value);
            const auto name = native_.Name(value.type + l.ObjectName);
            if (std::any_of(tags.begin(), tags.end(), [&](const auto& tag) { return NativeAccess::MatchesVehicle(name, tag); }))
                targets.emplace(static_cast<int16_t>(i), value);
        }
        std::vector<Template> selected;
        std::set<uint16_t> visuals = visual_plan.indices;
        for (int32_t i = 0; i < templates.header.count; ++i) {
            if (!templates.Has(i)) continue;
            const auto at = templates.header.data + uint64_t(i) * l.TemplateStride;
            Template value{}; Array fragments{};
            value.index = i;
            if (!Read(at + l.TemplateValue, value.object) || !Read(at + l.TemplateController, value.controller) ||
                !ReadArray(value.object + l.TemplateFragments, fragments, 256)) return Result::Unavailable;
            bool matches{};
            for (int32_t f = 0; f < fragments.count; ++f) {
                std::array<uintptr_t, 2> entry{};
                if (!Read(fragments.data + uint64_t(f) * l.FragmentValueStride, entry)) return Result::Unavailable;
                if (entry[0] != fragment) continue;
                if (!entry[1] || !ReadRepresentation(entry[1], value)) return Result::Unavailable;
                matches = targets.contains(value.high) || targets.contains(value.low) || visual_plan.indices.contains(value.visual);
                break;
            }
            if (!matches) continue;
            uint32_t references{};
            // Removing a registry key cannot invalidate an externally borrowed template.
            if (!value.controller || !Read(value.controller + l.SharedReferences, references)) return Result::Unavailable;
            if (references != 1) return Result::InUse;
            for (const auto index : {value.high, value.low}) {
                if (index == -1) continue;
                if (!classes.contains(index)) return Result::Unavailable;
                ++classes.at(index).owned;
                if (targets.contains(index)) ++targets.at(index).owned;
            }
            if (value.visual != UINT16_MAX) visuals.insert(value.visual);
            selected.push_back(value);
        }
        for (const auto& [index, value] : targets) if (value.owned != value.references) return Result::InUse;
        for (const auto& [index, value] : classes) if (value.owned > value.references) return Result::Unavailable;
        uintptr_t manager{}; bool in_use{};
        if (!Read(subsystem + l.RepresentationManager, manager) || !Unused(manager, fragment, targets, visuals, in_use)) return Result::Unavailable;
        if (in_use) return Result::InUse;
        // Retire the registry entries first, so another summon builds fresh fragments rather
        // than reusing indices removed by ReleaseTemplate.
        for (const auto& value : selected)
            if (!Remove(native_.remove_template, spawner + l.TemplateMap, value.index)) return Result::Unavailable;
        for (const auto& [index, value] : classes)
            for (uint32_t i = 0; i < value.owned; ++i)
                if (!Release(native_.release_template, subsystem, value.type)) return Result::Unavailable;
        const auto physics = ClearPhysics(subsystem, tags);
        return physics == Result::Released ? ReleaseVisuals(visual_plan) : physics;
    }
};
} // namespace mod_manager

#endif
