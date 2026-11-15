#pragma once
#include "map_data.hpp"

namespace collectible_map {
struct CityEventIcon {
    NameId id;
    Position offset{};
};

inline bool ReadCityEventIcon(Engine& engine, const anomaly::sdk::Profile& profile, Ref table,
                              CityEventIcon& icon) {
    const auto config = engine.Find("/Script/HTGame.CityEventUIConfig");
    const auto crimes = engine.Find("/Script/HTGame.CityEventUI_Crimes");
    const auto father = engine.Find("/Script/HTGame.CityEventUI_Father");
    uintptr_t row_type{};
    if (!engine.IsA(table, "DataTable") || !config || !crimes || !father ||
        !engine.Read(table.address + profile.Value("DataTableRowStruct", 256, 8), row_type) ||
        row_type != config.address) return false;

    using InstancedStruct = std::array<uintptr_t, 2>;
    const auto ui_data = engine.Field(row_type, "UIData", sizeof(InstancedStruct), "StructProperty");
    int32_t father_size{};
    if (!engine.Read(father.address + profile.Value("StructSize", 256, 4), father_size) ||
        father_size <= 0 || father_size > 1024) return false;
    const auto father_info = engine.Field(crimes.address, "FatherInfo", father_size, "StructProperty");
    const auto icon_id = engine.Field(father.address, "MapIconID", sizeof(NameId), "NameProperty");
    const auto icon_offset = engine.Field(father.address, "MapIconOffset", sizeof(Position), "StructProperty");
    if (std::min({ui_data, father_info, icon_id, icon_offset}) < 0) return false;

    return VisitMap(engine, table.address + profile.Value("DataTableRowMap", 256, 8), 24,
        [&](const uint8_t* entry) {
            uintptr_t row{};
            std::memcpy(&row, entry + sizeof(NameId), sizeof(row));
            InstancedStruct data{};
            if (!row || !engine.Read(row + ui_data, data)) return false;
            if (data[0] != crimes.address) return true;
            return data[1] && engine.Read(data[1] + father_info + icon_id, icon.id) &&
                engine.Read(data[1] + father_info + icon_offset, icon.offset);
        }) && icon.id.index && engine.NameById(icon.id.index) == "CityEvent_Crime";
}

inline bool ReadCityEventMarkers(Engine& engine, const anomaly::sdk::Profile& profile,
                                 std::vector<Marker>& output) {
    const auto asset = engine.Find("/Game/DataTable/CityEvent/DA_CityEvent.DA_CityEvent");
    uintptr_t library_address{}, ui_address{};
    if (!engine.IsA(asset, "CityEventDataAsset") ||
        !engine.Member(asset, "CityEventLibraryDataTable", "ObjectProperty", library_address) ||
        !engine.Member(asset, "CityEventUIDataTable", "ObjectProperty", ui_address)) return false;

    CityEventIcon icon{};
    if (!ReadCityEventIcon(engine, profile, engine.Reference(ui_address), icon)) return false;

    const auto library = engine.Reference(library_address);
    const auto definition = engine.Find("/Script/HTGame.CityEventDefine");
    const auto crimes = engine.Find("/Script/HTGame.CiryEvent_Crimes");
    uintptr_t row_type{};
    if (!engine.IsA(library, "DataTable") || !definition || !crimes ||
        !engine.Read(library.address + profile.Value("DataTableRowStruct", 256, 8), row_type) ||
        row_type != definition.address) return false;

    using InstancedStruct = std::array<uintptr_t, 2>;
    const auto event_data = engine.Field(row_type, "EventData", sizeof(InstancedStruct), "StructProperty");
    const auto level = engine.Field(row_type, "EventBelongsLevel", sizeof(Array), "StrProperty");
    const auto area = engine.Field(row_type, "EventBelongsAreaID", sizeof(NameId), "NameProperty");
    const auto center = engine.Field(row_type, "SendRewardCenter", sizeof(Position), "StructProperty");
    if (std::min({event_data, level, area, center}) < 0) return false;

    std::vector<Marker> markers;
    if (!VisitMap(engine, library.address + profile.Value("DataTableRowMap", 256, 8), 24,
        [&](const uint8_t* entry) {
            Marker marker{};
            uintptr_t row{};
            std::memcpy(&marker.key, entry, sizeof(marker.key));
            std::memcpy(&row, entry + sizeof(NameId), sizeof(row));
            InstancedStruct data{};
            if (!row || !marker.key.index || !engine.Read(row + event_data, data)) return false;
            if (data[0] != crimes.address || !data[1]) return true;
            Array level_text{};
            if (!engine.Read(row + level, level_text) || !engine.Read(row + area, marker.area)) return false;
            if (!ReadString(engine, level_text, marker.level) || marker.level != L"XL_map_bigworld_test" ||
                !marker.area.index || engine.NameById(marker.area.index) == "Test") return true;
            if (!engine.Read(row + center, marker.location)) return false;
            if (marker.location == Position{}) return true;
            for (size_t axis = 0; axis < marker.location.size(); ++axis) marker.location[axis] += icon.offset[axis];
            if (!std::ranges::all_of(marker.location, [](double v) { return std::isfinite(v) && std::abs(v) < 1.e8; }))
                return true;
            marker.icon = icon.id;
            marker.id = L"b1ank-collectible-map-city-crime-" + std::to_wstring(marker.key.index) +
                        L"-" + std::to_wstring(marker.key.number);
            markers.push_back(std::move(marker));
            return true;
        }) || markers.empty()) return false;
    output = std::move(markers);
    return true;
}
}
