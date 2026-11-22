#pragma once
#include <Windows.h>
#include "native_layout.hpp"
#include <anomaly/sdk/localization.hpp>
#include <anomaly/sdk/services/nte.h>
#include <anomaly/sdk/services/ue5.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace nte_vehicle {

struct NativeMemory {
    const AnomalyCoreServiceV1* core{};
    bool Read(std::uintptr_t address, void* out, std::size_t size) const noexcept {
        return core && core->read_memory(core->user, address,
            {static_cast<std::uint8_t*>(out), size}).code == ANOMALY_STATUS_V1_OK;
    }
    bool Write(std::uintptr_t address, const void* data, std::size_t size) const noexcept {
        return core && core->write_memory(core->user, address,
            {static_cast<const std::uint8_t*>(data), size}).code == ANOMALY_STATUS_V1_OK;
    }
};
template<class T> bool ReadValue(const NativeMemory& memory, std::uintptr_t address, T& value) {
    return memory.Read(address, &value, sizeof(value));
}
inline bool AddAddress(std::uintptr_t base, std::int64_t offset, std::uintptr_t& out) {
    if (!base || offset < 0 || static_cast<std::uint64_t>(offset) > UINTPTR_MAX - base) return false;
    out = base + static_cast<std::uintptr_t>(offset);
    return true;
}
inline bool ReadNullablePointerAt(const NativeMemory& memory, std::uintptr_t base,
                                 std::int64_t offset, std::uintptr_t& out) {
    std::uintptr_t address{};
    return AddAddress(base, offset, address) && ReadValue(memory, address, out);
}
inline bool ReadPointerAt(const NativeMemory& memory, std::uintptr_t base,
                          std::int64_t offset, std::uintptr_t& out) {
    return ReadNullablePointerAt(memory, base, offset, out) && out != 0;
}
inline bool ReadableRange(const NativeMemory& memory, std::uintptr_t address, std::size_t size) {
    std::array<std::uint8_t, 512> buffer{};
    if (!address || size > UINTPTR_MAX - address) return false;
    while (size) {
        const auto bytes = (std::min)(size, buffer.size());
        if (!memory.Read(address, buffer.data(), bytes)) return false;
        address += bytes;
        size -= bytes;
    }
    return true;
}
inline AnomalyStatusV1 Status(std::uint32_t code, const char* message = nullptr) {
    return {code, 0, {message, message ? std::strlen(message) : 0}};
}
inline AnomalyStatusV1 CopyString(std::string_view text, char* out, std::size_t* size) {
    if (!size) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    const auto capacity = *size;
    *size = text.size() + 1;
    if (!out) return Status(ANOMALY_STATUS_V1_OK);
    if (capacity < *size) return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = 0;
    return Status(ANOMALY_STATUS_V1_OK);
}
template<class Invoke> bool InvokeProcessEventGuarded(const Invoke& invoke,
    std::uintptr_t object, std::uintptr_t function, void* parameters, std::size_t size) {
    return invoke && invoke(object, function, parameters, size);
}

struct ObjectRegistryState {
    std::uintptr_t items{};
    std::uint32_t count{}, max_count{}, chunk_size{}, item_stride{}, object_offset{}, serial_offset{};
};
inline bool ReadObjectSlot(const NativeMemory& memory, const ObjectRegistryState& registry,
                          std::uint32_t index, std::uintptr_t& object, std::uint32_t& serial) {
    std::uintptr_t chunk{};
    if (!registry.items || !registry.chunk_size || index >= registry.count ||
        !ReadValue(memory, registry.items + sizeof(void*) * (index / registry.chunk_size), chunk) || !chunk)
        return false;
    const auto item = chunk + registry.item_stride * static_cast<std::size_t>(index % registry.chunk_size);
    return ReadValue(memory, item + registry.object_offset, object) &&
           ReadValue(memory, item + registry.serial_offset, serial);
}

// Only public SDK services cross the plugin boundary. Addresses are resolved from
// object handles on the Game thread and are never exposed by the plugin UI.
struct NativeAccess {
    NativeMemory storage;
    NativeMemory* memory{&storage};
    anomaly::sdk::ProfileSnapshot compatibility;
    NativeLayout layout;
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNtePlayerServiceV1* player{};
    ObjectRegistryState object_registry;
    std::uintptr_t registry_address{}, player_pawn{}, player_controller{}, world_pointer{};
    std::uint64_t object_generation{};
    std::atomic<DWORD> game_thread_id{};
    bool ready{};
    std::uint64_t layout_generation{};
    std::string result_message;
    std::function<bool(std::uintptr_t, std::uintptr_t, void*, std::size_t)> process_event_invoker;

#include "reflection.inl"

    bool Load(const AnomalyHostApiV1* api);
    bool Refresh();
    bool ResolveVehicleLayout();
    const char* Remember(std::string_view value) {
        result_message = value;
        return result_message.c_str();
    }
    bool LoadRegistry(ObjectRegistryState& output) const;
    bool ResolveObjectHandleLocked(AnomalyGenerationHandleV1 handle, std::uintptr_t& object) const;
    bool ObjectHandleLocked(std::uintptr_t object, AnomalyGenerationHandleV1& handle) const;
    bool IsCurrentObject(AnomalyGenerationHandleV1 handle, std::uintptr_t expected) const {
        std::uintptr_t current{};
        return ResolveObjectHandleLocked(handle, current) && current == expected;
    }
    static bool SameHandle(AnomalyGenerationHandleV1 a, AnomalyGenerationHandleV1 b) {
        return a.id == b.id && a.generation == b.generation;
    }

    std::string ResolveNameSnapshotLocked(std::uint32_t id) const {
        std::array<char, 1025> text{};
        std::size_t size = text.size();
        if (!names || names->resolve_utf8(names->user, id, text.data(), &size).code != 0 ||
            size <= 1 || size > text.size()) return {};
        return {text.data(), size - 1};
    }
    bool ResolveFNameLocked(std::uint32_t id, std::uint32_t number, std::string& text) const {
        text = ResolveNameSnapshotLocked(id);
        if (text.empty()) return false;
        if (number) text += "_" + std::to_string(number - 1);
        return true;
    }
    bool ReadReflectedObjectNameLocked(std::uintptr_t object, std::string& text) const {
        std::uint32_t id{};
        if (!ReadValue(*memory, object + layout.object_nameOffset, id)) return false;
        text = ResolveNameSnapshotLocked(id);
        return !text.empty();
    }
    bool ReadReflectedFieldClassNameLocked(std::uintptr_t field, std::string& text) const {
        std::uintptr_t type{};
        std::uint32_t id{};
        if (!ReadPointerAt(*memory, field, layout.ffield_class, type) ||
            !ReadValue(*memory, type + layout.ffieldClass_name, id)) return false;
        text = ResolveNameSnapshotLocked(id);
        return !text.empty();
    }
    bool FindExactObjectLocked(std::wstring_view path, std::uintptr_t& object) const {
        std::string utf8;
        utf8.reserve(path.size());
        for (const auto value : path) {
            if (value > 127) return false;
            utf8.push_back(static_cast<char>(value));
        }
        AnomalyGenerationHandleV1 handle{};
        return objects->find_exact(objects->user, anomaly::sdk::StringView(utf8), &handle).code == 0 &&
            ResolveObjectHandleLocked(handle, object);
    }
    bool FindReflectedPropertyLocked(std::uintptr_t structure, std::string_view name,
                                    ReflectedPropertyInfo& result, bool include_super) const {
        for (std::size_t depth{}; structure && depth < 128; ++depth) {
            std::uintptr_t field{};
            if (!ReadValue(*memory, structure + layout.ustruct_propertyLink, field)) return false;
            for (std::size_t count{}; field && count < 4096; ++count) {
                ReflectedPropertyInfo candidate;
                if (!ReadReflectedPropertyLocked(field, candidate)) return false;
                if (candidate.name == name) {
                    if (candidate.offset < 0 || candidate.offset > 1024 * 1024 ||
                        candidate.element_size <= 0 || candidate.array_dim != 1) return false;
                    result = std::move(candidate);
                    return true;
                }
                if (candidate.next == field) return false;
                field = candidate.next;
            }
            if (!include_super) return false;
            std::uintptr_t parent{};
            if (!ReadValue(*memory, structure + layout.ustruct_superStruct, parent) ||
                parent == structure) return false;
            structure = parent;
        }
        return false;
    }
    bool FindHeldPropertyLocked(std::uintptr_t structure, std::string_view name,
                                ReflectedPropertyInfo& result) const {
        return FindReflectedPropertyLocked(structure, name, result, true);
    }
    bool ResolveText(std::uintptr_t address, std::string& text) const {
        std::size_t size{};
        if (!names || !names->resolve_ftext_utf8 ||
            names->resolve_ftext_utf8(names->user, address, nullptr, &size).code != 0 ||
            size < 2 || size > 64 * 1024) return false;
        text.resize(size);
        if (names->resolve_ftext_utf8(names->user, address, text.data(), &size).code != 0) return false;
        text.resize(size - 1);
        return true;
    }
};
} // namespace nte_vehicle
