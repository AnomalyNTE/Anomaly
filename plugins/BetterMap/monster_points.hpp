#pragma once
#include "map_data.hpp"

namespace collectible_map {
using Portrait = std::array<uint8_t, 40>;

struct MonsterGroup {
    NameId key;
    uintptr_t row{};
    std::vector<Position> positions;
    std::string id, label;
};

inline std::string MonsterLabel(Engine& engine, uintptr_t text, NameId key) {
    const auto* names = engine.names;
    if (names && names->struct_size >= offsetof(AnomalyUe5NamesServiceV1, find_utf8) && names->resolve_ftext_utf8) {
        std::array<char, 256> buffer{};
        size_t length = buffer.size();
        if (anomaly::sdk::Succeeded(names->resolve_ftext_utf8(names->user, text, buffer.data(), &length))) {
            const auto end = std::ranges::find(buffer, '\0');
            if (end != buffer.begin() && end != buffer.end()) return {buffer.begin(), end};
        }
    }
    auto label = engine.NameById(key.index);
    if (label.empty()) label = std::to_string(key.index);
    if (key.number) label += "_" + std::to_string(key.number - 1);
    return label;
}

struct MonsterCatalog {
    Ref table;
    int portrait{}, name{}, description{};
    std::vector<MonsterGroup> groups;
};

inline std::wstring MonsterSuffix(NameId key) {
    return std::to_wstring(key.index) + L"-" + std::to_wstring(key.number);
}

inline std::vector<Marker> MonsterMarkers(const MonsterCatalog& catalog) {
    std::vector<Marker> markers;
    for (const auto& group : catalog.groups) {
        const auto prefix = L"b1ank-collectible-map-monster-" + MonsterSuffix(group.key) + L"-";
        for (size_t index = 0; index < group.positions.size(); ++index) {
            Marker marker{};
            marker.key = group.key;
            marker.location = group.positions[index];
            marker.level = L"XL_map_bigworld_test";
            marker.id = prefix + std::to_wstring(index);
            markers.push_back(std::move(marker));
        }
    }
    return markers;
}

inline bool ReadMonsterGroups(Engine& engine, const anomaly::sdk::Profile& profile,
                              MonsterCatalog& output) {
    const auto table = engine.Find("/Game/DataTable/DT_MonsterManualConfig.DT_MonsterManualConfig");
    const auto type = engine.Find("/Script/HTGame.MonsterManualConfig");
    uintptr_t row_type{};
    if (!engine.IsA(table, "DataTable") || !engine.IsA(type, "ScriptStruct") ||
        !engine.Read(table.address + profile.Value("DataTableRowStruct", 256, 8), row_type) ||
        row_type != type.address) return false;
    const auto trace = engine.Field(row_type, "TraceType", 1, "EnumProperty");
    const auto icon = engine.Field(row_type, "MapIconID", sizeof(NameId), "NameProperty");
    const auto coordinates = engine.Field(row_type, "ArrayWorldLocation", sizeof(Array), "ArrayProperty");
    const auto portrait = engine.Field(row_type, "EnemyWorldImage", sizeof(Portrait), "SoftObjectProperty");
    const auto name = engine.Field(row_type, "MonsterName", sizeof(Text), "TextProperty");
    const auto description = engine.Field(row_type, "DiscoveredDesc", sizeof(Text), "TextProperty");
    if (std::min({trace, icon, coordinates, portrait, name, description}) < 0) return false;

    std::vector<MonsterGroup> groups;
    size_t points{};
    const auto rows = table.address + profile.Value("DataTableRowMap", 256, 8);
    if (!VisitMap(engine, rows, 24, [&](const uint8_t* entry) {
        MonsterGroup group{};
        std::memcpy(&group.key, entry, sizeof(NameId));
        std::memcpy(&group.row, entry + 8, sizeof(group.row));
        const auto row = group.row;
        uint8_t kind{};
        if (!row || !engine.Read(row + trace, kind)) return false;
        if (kind != 0) return true; // EMonsterManualTraceType::BigWorldEnemy
        NameId native_icon{};
        Array locations{};
        if (!engine.Read(row + icon, native_icon) || !engine.Read(row + coordinates, locations) ||
            locations.count < 0 || locations.count > 512 || locations.capacity < locations.count) return false;
        if (!locations.count) return true;
        if (!group.key.index || !native_icon.index || engine.Name(row + icon) != "MonsterTrace") return true;
        group.positions.resize(locations.count);
        if (!engine.ReadBytes(locations.data, {reinterpret_cast<uint8_t*>(group.positions.data()),
                                               group.positions.size() * sizeof(Position)})) return false;
        if (!std::ranges::all_of(group.positions, [](const Position& position) {
                return position != Position{} && std::ranges::all_of(position, [](double axis) {
                    return std::isfinite(axis) && std::abs(axis) < 1.e8;
                });
            })) return false;
        points += group.positions.size();
        if (points > 4096) return false;
        group.id = engine.NameById(group.key.index);
        if (group.id.empty()) return false;
        if (group.key.number) group.id += "_" + std::to_string(group.key.number - 1);
        group.label = MonsterLabel(engine, row + name, group.key);
        groups.push_back(std::move(group));
        return true;
    }) || groups.empty()) return false;
    output = {table, portrait, name, description, std::move(groups)};
    return true;
}
}
