#pragma once
#include "engine.hpp"
#include <cmath>
#include <span>
#include <vector>

namespace collectible_map {
struct NameId {
    uint32_t index{}, number{};
    bool operator==(const NameId&) const = default;
};

struct Array {
    uintptr_t data{};
    int32_t count{}, capacity{};
};

using Position = std::array<double, 3>;
using Text = std::array<uint8_t, 16>;

struct Marker {
    NameId key, icon, gameplay_area, floor_area, area;
    int32_t floor{};
    Position location{};
    std::wstring level, id;
};

using Catalog = std::array<std::vector<Marker>, 2>;

struct SparseHeader {
    Array slots;
    std::array<uint32_t, 4> inline_flags{};
    uintptr_t heap_flags{};
    int32_t flags_count{}, flags_capacity{};
};
static_assert(sizeof(SparseHeader) == 48);
static_assert(sizeof(wchar_t) == 2);

// A sparse Unreal map stores free-list entries alongside live entries.
inline bool VisitMap(const Engine& engine, uintptr_t address, size_t stride, auto visit) {
    SparseHeader header{};
    if (!engine.Read(address, header) || header.slots.count < 0 || header.slots.count > 4096 ||
        header.slots.capacity < header.slots.count || header.flags_count != header.slots.count ||
        header.flags_capacity < header.flags_count) return false;
    if (!header.slots.count) return true;

    std::vector<uint32_t> flags((header.flags_count + 31) / 32);
    if (header.heap_flags) {
        if (!engine.ReadBytes(header.heap_flags, {reinterpret_cast<uint8_t*>(flags.data()), flags.size() * 4})) return false;
    } else {
        if (flags.size() > header.inline_flags.size()) return false;
        std::copy_n(header.inline_flags.begin(), flags.size(), flags.begin());
    }
    std::vector<uint8_t> entries(header.slots.count * stride);
    if (!engine.ReadBytes(header.slots.data, {entries.data(), entries.size()})) return false;
    for (int32_t index = 0; index < header.slots.count; ++index) {
        if ((flags[index / 32] & (1u << (index % 32))) && !visit(entries.data() + index * stride)) return false;
    }
    return true;
}

inline Array Borrow(const std::wstring& value) {
    const auto length = static_cast<int32_t>(value.size() + 1);
    return {reinterpret_cast<uintptr_t>(value.c_str()), length, length};
}

inline bool ReadString(const Engine& engine, const Array& array, std::wstring& value) {
    if (array.count <= 1 || array.count > 128 || array.capacity < array.count) return false;
    std::array<wchar_t, 128> text{};
    if (!engine.ReadBytes(array.data, {reinterpret_cast<uint8_t*>(text.data()), array.count * sizeof(wchar_t)}) ||
        text[array.count - 1] != 0) return false;
    value.assign(text.data(), array.count - 1);
    return value.find(L'\0') == std::wstring::npos;
}

inline bool ReadCategory(Engine& engine, Ref table, uint32_t struct_offset, uint32_t map_offset,
                         bool oracle, NameId icon, std::vector<Marker>& output) {
    uintptr_t row_type{};
    if (!engine.IsA(table, "DataTable") || !engine.Read(table.address + struct_offset, row_type) ||
        !engine.IsA(engine.Reference(row_type), "ScriptStruct")) return false;
    const auto level = engine.Field(row_type, "LevelName", sizeof(Array), "StrProperty");
    const auto location = engine.Field(row_type, "Location", sizeof(Position), "StructProperty");
    const auto area = engine.Field(row_type, "AreaId", sizeof(NameId), "NameProperty");
    const auto type = oracle ? 0 : engine.Field(row_type, "TreasureType", 1, "EnumProperty");
    const auto gameplay = oracle ? engine.Field(row_type, "GameplayAreaID", sizeof(NameId), "NameProperty") : 0;
    const auto floor_area = oracle ? engine.Field(row_type, "FloorArea", sizeof(NameId), "NameProperty") : 0;
    const auto floor = oracle ? engine.Field(row_type, "floor", sizeof(int32_t), "IntProperty") : 0;
    if (std::min({level, location, area, type, gameplay, floor_area, floor}) < 0) return false;

    return VisitMap(engine, table.address + map_offset, 24, [&](const uint8_t* entry) {
        NameId key{};
        uintptr_t row{};
        std::memcpy(&key, entry, sizeof(key));
        std::memcpy(&row, entry + 8, sizeof(row));
        if (!row || !key.index) return false;
        if (!oracle) {
            uint8_t treasure_type{};
            if (!engine.Read(row + type, treasure_type)) return false;
            if (treasure_type != 6) return true; // TreasureBoxType_TwentyOne
        }
        Marker marker{};
        Array level_text{};
        if (!engine.Read(row + level, level_text) || !ReadString(engine, level_text, marker.level) ||
            !engine.Read(row + location, marker.location) || !engine.Read(row + area, marker.area) ||
            !std::ranges::all_of(marker.location, [](double v) { return std::isfinite(v) && std::abs(v) < 1.e8; })) return false;
        if (oracle && (!engine.Read(row + gameplay, marker.gameplay_area) ||
                       !engine.Read(row + floor_area, marker.floor_area) || !engine.Read(row + floor, marker.floor))) return false;
        marker.key = key;
        marker.icon = icon;
        marker.id = (oracle ? L"b1ank-collectible-map-oracle-" : L"b1ank-collectible-map-gift-") +
                    std::to_wstring(key.index) + L"-" + std::to_wstring(key.number);
        output.push_back(std::move(marker));
        return true;
    }) && !output.empty();
}

inline bool ReadCatalog(Engine& engine, const anomaly::sdk::Profile& profile, Catalog& result) {
    const auto asset = engine.Find("/Game/DataAssets/TreasureboxDataAsset.TreasureboxDataAsset");
    if (!engine.IsA(asset, "HTTreasureboxDataAsset")) return false;
    uintptr_t type{}, oracle_table{}, treasure_table{};
    if (!engine.Read(asset.address + engine.layout.object_class, type) ||
        !engine.Member(asset, "OracleStoneConfigTable", "ObjectProperty", oracle_table) ||
        !engine.Member(asset, "TreasureboxConfigTable", "ObjectProperty", treasure_table)) return false;
    const auto icon_map = engine.Field(type, "TreasureMiniMapIconIDMap", 80, "MapProperty");
    std::array<NameId, 2> icons{};
    if (icon_map < 0 || !VisitMap(engine, asset.address + icon_map, 20, [&](const uint8_t* entry) {
        if (entry[0] == 4) std::memcpy(&icons[0], entry + 4, sizeof(NameId));
        if (entry[0] == 6) std::memcpy(&icons[1], entry + 4, sizeof(NameId));
        return true;
    }) || !icons[0].index || !icons[1].index) return false;

    Catalog rows;
    const auto row_struct = profile.Value("DataTableRowStruct", 256, 8);
    const auto row_map = profile.Value("DataTableRowMap", 256, 8);
    if (!ReadCategory(engine, engine.Reference(oracle_table), row_struct, row_map, true, icons[0], rows[0]) ||
        !ReadCategory(engine, engine.Reference(treasure_table), row_struct, row_map, false, icons[1], rows[1])) return false;
    result = std::move(rows);
    return true;
}
}
