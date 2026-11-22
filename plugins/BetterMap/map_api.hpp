#pragma once
#include "map_data.hpp"

namespace collectible_map {
using MarkerParams = std::array<uint8_t, 352>;

template<class T, size_t Size>
void Write(std::array<uint8_t, Size>& buffer, size_t offset, const T& value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

template<class T, size_t Size>
T Read(const std::array<uint8_t, Size>& buffer, size_t offset) {
    T value{};
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return value;
}

template<class T>
Array Borrow(std::span<const T> values) {
    const auto count = static_cast<int32_t>(values.size());
    return {reinterpret_cast<uintptr_t>(values.data()), count, count};
}

inline MarkerParams MakeParams(const Marker& marker, const Text& empty) {
    static constexpr std::array<uint8_t, 1> map_types{1}; // BigMap
    static constexpr std::array<uint8_t, 2> map_modes{0, 1}; // Normal, Explored
    MarkerParams params{};
    Write(params, 0, marker.icon);
    Write(params, 16, uint8_t{1}); // Transform
    Write(params, 32, std::array<double, 4>{0., 0., 0., 1.});
    Write(params, 64, marker.location);
    Write(params, 96, Position{1., 1., 1.});
    Write(params, 144, int32_t{-1}); // Invalid weak actor and component indices
    Write(params, 152, int32_t{-1});
    Write(params, 160, Borrow(marker.level));
    Write(params, 176, marker.gameplay_area);
    Write(params, 184, marker.floor_area);
    Write(params, 192, marker.floor);
    Write(params, 200, marker.area);
    Write(params, 208, empty);
    Write(params, 224, empty);
    Write(params, 240, empty);
    Write(params, 256, Borrow(marker.id));
    Write(params, 288, uint8_t{1}); // MapIcons
    Write(params, 296, Borrow(std::span<const uint8_t>(map_types)));
    Write(params, 312, Borrow(std::span<const uint8_t>(map_modes)));
    Write(params, 344, uint8_t{1}); // Default icon state
    return params;
}

class MapApi {
public:
    bool Bind(Engine& engine) {
        const auto add = engine.Bind("HTGame", "HTPlayerState", "AddMapIcons", 3, 40,
            {{"AddParams", 0, 16, "ArrayProperty"}, {"bBroadcastChangeEvent", 16, 1, "BoolProperty"},
             {"ReturnValue", 24, 16, "ArrayProperty"}});
        const auto remove = engine.Bind("HTGame", "HTPlayerState", "RemoveMapIconByUniqueIDs", 2, 17,
            {{"InUniqueIDs", 0, 16, "ArrayProperty"}, {"bBroadcastChangeEvent", 16, 1, "BoolProperty"}});
        if (!add.function || !remove.function || !ValidateLayout(engine)) return false;

        const auto block = engine.Find("/Script/UMG.Default__TextBlock");
        const auto library = engine.Find("/Script/Engine.Default__KismetTextLibrary");
        const auto is_empty = engine.Bind("Engine", "KismetTextLibrary", "TextIsEmpty", 2, 17,
            {{"InText", 0, 16, "TextProperty"}, {"ReturnValue", 16, 1, "BoolProperty"}});
        Text text{};
        alignas(8) std::array<uint8_t, 17> params{};
        if (!engine.Member(block, "Text", "TextProperty", text) || !Read<uintptr_t>(text, 0)) return false;
        Write(params, 0, text);
        if (!engine.Call(library, is_empty, params.data(), params.size()) || !params[16]) return false;
        add_ = add;
        remove_ = remove;
        empty_ = text;
        return true;
    }

    explicit operator bool() const { return static_cast<bool>(add_.function); }

    size_t Add(Engine& engine, Ref player, std::span<const Marker* const> markers,
               std::span<bool> accepted = {}) const {
        constexpr size_t maximum = 24;
        alignas(16) std::array<MarkerParams, maximum> batch{};
        if (markers.empty() || markers.size() > batch.size() ||
            (!accepted.empty() && accepted.size() != markers.size())) return 0;
        for (size_t index = 0; index < markers.size(); ++index)
            batch[index] = MakeParams(*markers[index], empty_);
        alignas(8) std::array<uint8_t, 40> params{};
        Write(params, 0, Borrow(std::span<const MarkerParams>(batch.data(), markers.size())));
        Write(params, 16, uint8_t{1});
        if (!engine.Call(player, add_, params.data(), params.size())) return 0;

        const auto returned = Read<Array>(params, 24);
        if (!returned.data || returned.data == reinterpret_cast<uintptr_t>(batch.data())) return 0;
        std::array<Array, maximum> ids{};
        size_t added{};
        if (returned.count >= 0 && static_cast<size_t>(returned.count) <= maximum && returned.capacity >= returned.count &&
            engine.ReadBytes(returned.data, {reinterpret_cast<uint8_t*>(ids.data()), returned.count * sizeof(Array)})) {
            for (int32_t index = 0; index < returned.count; ++index) {
                const auto& id = ids[index];
                if (!id.data) continue;
                const auto borrowed = std::ranges::any_of(markers, [&](const Marker* marker) {
                    return id.data == Borrow(marker->id).data;
                });
                std::wstring key;
                if (ReadString(engine, id, key)) {
                    const auto match = std::ranges::find(markers, key, &Marker::id);
                    if (match != markers.end()) {
                        if (accepted.empty()) ++added;
                        else if (auto& registered = accepted[match - markers.begin()]; !registered) {
                            registered = true;
                            ++added;
                        }
                    }
                }
                if (!borrowed) engine.free_memory(reinterpret_cast<void*>(id.data));
            }
        }
        engine.free_memory(reinterpret_cast<void*>(returned.data));
        return added;
    }

    bool Remove(Engine& engine, Ref player, std::span<const Marker> markers) const {
        std::vector<Array> ids;
        ids.reserve(markers.size());
        for (const auto& marker : markers) ids.push_back(Borrow(marker.id));
        return RemoveIds(engine, player, ids);
    }

    bool RemoveIds(Engine& engine, Ref player, std::span<const Array> ids) const {
        alignas(8) std::array<uint8_t, 17> params{};
        Write(params, 0, Borrow(ids));
        Write(params, 16, uint8_t{1});
        return engine.Call(player, remove_, params.data(), params.size());
    }

private:
    static bool ValidateLayout(Engine& engine) {
        const auto add = engine.Find("/Script/HTGame.MiniMapIconAddParams");
        const auto transform = engine.Find("/Script/HTGame.MiniMapIconTransformInfo");
        const auto native_transform = engine.Find("/Script/CoreUObject.Transform");
        if (!engine.IsA(add, "ScriptStruct") || !engine.IsA(transform, "ScriptStruct") ||
            !engine.IsA(native_transform, "ScriptStruct")) return false;
        const Param fields[] = {
            {"MapIconID", 0, 8, "NameProperty"}, {"IconType", 8, 1, "EnumProperty"},
            {"TransformInfo", 16, 144, "StructProperty"}, {"BelongsLevelName", 160, 16, "StrProperty"},
            {"BelongsGameplayAreaID", 176, 8, "NameProperty"}, {"BelongsFloorArea", 184, 8, "NameProperty"},
            {"floor", 192, 4, "IntProperty"}, {"BelongsAreaID", 200, 8, "NameProperty"},
            {"OverrideIconName", 208, 16, "TextProperty"}, {"OverrideIconDesc", 224, 16, "TextProperty"},
            {"IconRegion", 240, 16, "TextProperty"}, {"UniqueID", 256, 16, "StrProperty"},
            {"IconInfoType", 288, 1, "EnumProperty"}, {"ShowTypes", 296, 16, "ArrayProperty"},
            {"ShowModes", 312, 16, "ArrayProperty"}, {"IconState", 344, 1, "EnumProperty"}
        };
        for (const auto& field : fields)
            if (engine.Field(add.address, field.name, field.size, field.kind) != field.offset) return false;
        return engine.Field(transform.address, "TransformType", 1, "EnumProperty") == 0 &&
               engine.Field(transform.address, "Transform", 96, "StructProperty") == 16 &&
               engine.Field(native_transform.address, "Rotation", 32, "StructProperty") == 0 &&
               engine.Field(native_transform.address, "Translation", 24, "StructProperty") == 32 &&
               engine.Field(native_transform.address, "Scale3D", 24, "StructProperty") == 64;
    }

    Bound add_, remove_;
    Text empty_{};
};
}
