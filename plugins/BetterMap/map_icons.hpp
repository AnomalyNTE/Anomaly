#pragma once
#include "map_api.hpp"
#include "monster_points.hpp"

namespace collectible_map {
class MapIcons {
public:
    void Resolve(const anomaly::sdk::Host& host, const Engine& engine, const anomaly::sdk::Profile& profile) {
        using namespace anomaly::sdk;
        const auto signatures = host.Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID);
        const auto result = ResolveProfileSymbol(signatures.get(), engine.core, profile, "AddMapIconTableRow");
        if (result) add_row_ = reinterpret_cast<AddRow>(result.address);
    }

    const char* Prepare(Engine& engine, const anomaly::sdk::Profile& profile, Catalog& catalog) const {
        const std::array<std::wstring, 2> names{L"B1ankCollectibleMap_Oracle", L"B1ankCollectibleMap_Gift"};
        return PrepareRows(engine, profile, catalog, names);
    }

    const char* PrepareCityEvents(Engine& engine, const anomaly::sdk::Profile& profile,
                                  std::vector<Marker>& markers) const {
        const std::array<std::wstring, 1> names{L"B1ankCollectibleMap_CityCrime"};
        return PrepareRows(engine, profile, {&markers, 1}, names, true);
    }

private:
    const char* PrepareRows(Engine& engine, const anomaly::sdk::Profile& profile,
                            std::span<std::vector<Marker>> catalog, std::span<const std::wstring> names,
                            bool display_only = false) const {
        const auto table = engine.Find("/Game/DataTable/MiniMap/DT_MiniMapIcon.DT_MiniMapIcon");
        const auto icon_type = engine.Find("/Script/HTGame.MiniMapIconData");
        const auto string_library = engine.Find("/Script/Engine.Default__KismetStringLibrary");
        const auto name_from_string = engine.Bind("Engine", "KismetStringLibrary", "Conv_StringToName", 2, 24,
            {{"InString", 0, 16, "StrProperty"}, {"ReturnValue", 16, 8, "NameProperty"}});
        uintptr_t row_type{}, vtable{}, method{};
        int32_t size{};
        if (!add_row_ || !engine.IsA(table, "DataTable") || !engine.IsA(icon_type, "ScriptStruct") ||
            !engine.Read(table.address + profile.Value("DataTableRowStruct", 256, 8), row_type) || row_type != icon_type.address ||
            !engine.Read(row_type + profile.Value("StructSize", 256, 4), size) ||
            size != static_cast<int32_t>(profile.Value("MapIconRowSize", 4096, 8)) ||
            !engine.Read(table.address, vtable) || !engine.Read(vtable + profile.Value("DataTableAddRowSlot", 4096, 8), method) ||
            method != reinterpret_cast<uintptr_t>(add_row_)) return "Collectible Map: map icon table binding unavailable";
        const auto distance = engine.Field(row_type, "ShowDistance", 4, "FloatProperty");
        const auto scale = engine.Field(row_type, "ShowMapScaleRange", 16, "StructProperty");
        const auto filter = engine.Field(row_type, "ShowInFilterList", 1, "BoolProperty");
        const auto range_type = engine.Find("/Script/CoreUObject.FloatRange");
        const auto bound_type = engine.Find("/Script/CoreUObject.FloatRangeBound");
        if (!Fits(distance, 4, size) || !Fits(scale, 16, size) || !Fits(filter, 1, size) ||
            engine.Field(range_type.address, "LowerBound", 8, "StructProperty") != 0 ||
            engine.Field(range_type.address, "UpperBound", 8, "StructProperty") != 8 ||
            engine.Field(bound_type.address, "Type", 1, "EnumProperty") != 0 ||
            engine.Field(bound_type.address, "Value", 4, "FloatProperty") != 4) return "Collectible Map: map icon row layout unavailable";

        const auto row_map = table.address + profile.Value("DataTableRowMap", 256, 8);
        constexpr std::array display_fields{"ShowInSearchList", "CanTrace", "CanClick", "CanChoose", "ShowIconDetails"};
        std::array<int, display_fields.size()> display_offsets{};
        if (display_only) {
            for (size_t i = 0; i < display_fields.size(); ++i) {
                display_offsets[i] = engine.Field(row_type, display_fields[i], 1, "BoolProperty");
                if (!Fits(display_offsets[i], 1, size))
                    return "Collectible Map: city event icon fields unavailable";
            }
        }
        std::vector<NameId> ids(names.size());
        for (size_t kind = 0; kind < names.size(); ++kind) {
            struct Params { Array string; NameId name; } params{Borrow(names[kind]), {}};
            if (!engine.Call(string_library, name_from_string, &params, sizeof(params)) || !params.name.index)
                return "Collectible Map: plugin map icon name unavailable";
            ids[kind] = params.name;
            const auto original = FindRow(engine, row_map, catalog[kind].front().icon);
            if (!original) return "Collectible Map: source map icon row missing";
            // Unreal deep-copies this borrowed row, retaining the game's own textures and widget class.
            alignas(16) std::array<uint8_t, 4096> row{};
            if (!engine.ReadBytes(original, {row.data(), static_cast<size_t>(size)}))
                return "Collectible Map: source icon row unreadable";
            Write(row, distance, 0.0f);
            Write(row, scale, std::array<uint32_t, 4>{2, 0, 2, 0}); // Open scale bounds
            Write(row, filter, uint8_t{0});
            if (display_only) {
                for (const auto offset : display_offsets) Write(row, offset, uint8_t{0});
            }
            auto installed = FindRow(engine, row_map, ids[kind]);
            if (!installed) {
                add_row_(table.address, ids[kind], row.data());
                installed = FindRow(engine, row_map, ids[kind]);
            }
            float actual_distance{};
            std::array<uint32_t, 4> actual_scale{};
            if (!installed || !engine.Read(installed + distance, actual_distance) || actual_distance != 0.0f ||
                !engine.Read(installed + scale, actual_scale) || actual_scale != Read<decltype(actual_scale)>(row, scale))
                return "Collectible Map: map icon copy not verified";
        }
        for (size_t kind = 0; kind < ids.size(); ++kind)
            for (auto& marker : catalog[kind]) marker.icon = ids[kind];
        return nullptr;
    }

public:
    bool PrepareMonsters(Engine& engine, const anomaly::sdk::Profile& profile,
                         const MonsterCatalog& catalog, std::span<Marker> markers,
                         size_t& next_group, size_t& next_marker) const {
        const auto table = engine.Find("/Game/DataTable/MiniMap/DT_MiniMapIcon.DT_MiniMapIcon");
        const auto icon_type = engine.Find("/Script/HTGame.MiniMapIconData");
        const auto image_type = engine.Find("/Script/HTGame.MiniMapIconImageData");
        const auto library = engine.Find("/Script/Engine.Default__KismetStringLibrary");
        const auto to_name = engine.Bind("Engine", "KismetStringLibrary", "Conv_StringToName", 2, 24,
            {{"InString", 0, 16, "StrProperty"}, {"ReturnValue", 16, 8, "NameProperty"}});
        uintptr_t row_type{}, table_vtable{}, method{};
        int32_t row_size{}, image_size{};
        if (!add_row_ || !to_name.function || !library || engine.Resolve(catalog.table.handle) != catalog.table ||
            !engine.IsA(table, "DataTable") ||
            !engine.IsA(icon_type, "ScriptStruct") || !engine.IsA(image_type, "ScriptStruct") ||
            !engine.Read(table.address + profile.Value("DataTableRowStruct", 256, 8), row_type) ||
            row_type != icon_type.address ||
            !engine.Read(row_type + profile.Value("StructSize", 256, 4), row_size) ||
            row_size != static_cast<int32_t>(profile.Value("MapIconRowSize", 4096, 8)) ||
            !engine.Read(image_type.address + profile.Value("StructSize", 256, 4), image_size) ||
            image_size != static_cast<int32_t>(profile.Value("MapIconImageSize", 1024, 8)) ||
            !engine.Read(table.address, table_vtable) ||
            !engine.Read(table_vtable + profile.Value("DataTableAddRowSlot", 4096, 8), method) ||
            method != reinterpret_cast<uintptr_t>(add_row_)) return false;

        const auto render = engine.Field(row_type, "IconRenderStateData", 80, "StructProperty");
        const auto render_override = engine.Field(row_type, "IconRenderStateDataTypeOverride", 80, "MapProperty");
        const auto filter = engine.Field(row_type, "ShowInFilterList", 1, "BoolProperty");
        const auto name = engine.Field(row_type, "IconName", sizeof(Text), "TextProperty");
        const auto description = engine.Field(row_type, "IconDesc", sizeof(Text), "TextProperty");
        const auto distance = engine.Field(row_type, "ShowDistance", 4, "FloatProperty");
        const auto scale = engine.Field(row_type, "ShowMapScaleRange", 16, "StructProperty");
        const auto details = engine.Field(row_type, "DetailsIcon", sizeof(Portrait), "SoftObjectProperty");
        const auto image = engine.Field(image_type.address, "IconImage", sizeof(Portrait), "SoftObjectProperty");
        if (!Fits(render, 80, row_size) || !Fits(render_override, 80, row_size) || !Fits(filter, 1, row_size) ||
            !Fits(name, sizeof(Text), row_size) || !Fits(description, sizeof(Text), row_size) ||
            !Fits(distance, 4, row_size) || !Fits(scale, 16, row_size) || !Fits(details, sizeof(Portrait), row_size) ||
            !Fits(image, sizeof(Portrait), image_size)) return false;

        const std::wstring template_name = L"MapExploreCustom";
        struct NameParams { Array string; NameId name; } params{Borrow(template_name), {}};
        const auto row_map = table.address + profile.Value("DataTableRowMap", 256, 8);
        if (!engine.Call(library, to_name, &params, sizeof(params)) || !params.name.index) return false;
        const auto source = FindRow(engine, row_map, params.name);
        alignas(16) std::array<uint8_t, 4096> original{};
        if (!source || !engine.ReadBytes(source, {original.data(), static_cast<size_t>(row_size)})) return false;

        // Keep the native image and details widgets. Redirect the deep copy's borrowed
        // render entries to the atlas portrait without editing the original map row.
        alignas(16) std::array<uint8_t, 96> big_map_entry{};
        const auto overrides = Read<SparseHeader>(original, render_override);
        if (!SingleSlot(overrides) || !engine.Read(overrides.slots.data, big_map_entry) || big_map_entry[0] != 1)
            return false;
        RenderEntry normal, big_map;
        if (!ReadRender(engine, Read<SparseHeader>(original, render), image_type, image_size, normal) ||
            !ReadRender(engine, Read<SparseHeader>(big_map_entry, 8), image_type, image_size, big_map)) return false;
        Write(big_map_entry, 8, reinterpret_cast<uintptr_t>(big_map.slot.data()));
        Write(original, render, reinterpret_cast<uintptr_t>(normal.slot.data()));
        Write(original, render_override, reinterpret_cast<uintptr_t>(big_map_entry.data()));
        Write(original, filter, uint8_t{0});
        Write(original, distance, 0.0f);
        Write(original, scale, std::array<uint32_t, 4>{2, 0, 2, 0});

        const auto end = std::min(next_group + 4, catalog.groups.size());
        for (; next_group < end; ++next_group) {
            const auto& group = catalog.groups[next_group];
            if (group.positions.size() > markers.size() - next_marker) return false;
            const auto icon_name = L"B1ankCollectibleMap_Monster_" + MonsterSuffix(group.key);
            NameParams icon{Borrow(icon_name), {}};
            if (!engine.Call(library, to_name, &icon, sizeof(icon)) || !icon.name.index) return false;
            if (!FindRow(engine, row_map, icon.name)) {
                Portrait portrait{};
                Text group_name{}, group_description{};
                if (!engine.Read(group.row + catalog.portrait, portrait) ||
                    !engine.Read(group.row + catalog.name, group_name) ||
                    !engine.Read(group.row + catalog.description, group_description)) return false;
                Write(normal.image, image, portrait);
                Write(big_map.image, image, portrait);
                Write(original, name, group_name);
                Write(original, description, group_description);
                Write(original, details, portrait);
                add_row_(table.address, icon.name, original.data());
                if (!FindRow(engine, row_map, icon.name)) return false;
            }
            for (auto& marker : markers.subspan(next_marker, group.positions.size())) marker.icon = icon.name;
            next_marker += group.positions.size();
        }
        return true;
    }

private:
    static bool Fits(int offset, int field_size, int row_size) {
        return offset >= 0 && offset <= row_size - field_size;
    }

    struct RenderEntry {
        alignas(8) std::array<uint8_t, 32> slot{};
        alignas(8) std::array<uint8_t, 1024> image{};
    };

    static bool SingleSlot(const SparseHeader& header) {
        return header.slots.count == 1 && header.slots.capacity >= 1 && !header.heap_flags &&
               header.flags_count == 1 && (header.inline_flags[0] & 1);
    }

    static bool ReadRender(const Engine& engine, const SparseHeader& map, Ref image_type, int image_size,
                           RenderEntry& result) {
        if (!SingleSlot(map) || !engine.Read(map.slots.data, result.slot) ||
            result.slot[0] != 1 || Read<uintptr_t>(result.slot, 8) != image_type.address ||
            !engine.ReadBytes(Read<uintptr_t>(result.slot, 16), {result.image.data(), static_cast<size_t>(image_size)}))
            return false;
        Write(result.slot, 16, reinterpret_cast<uintptr_t>(result.image.data()));
        return true;
    }

    static uintptr_t FindRow(const Engine& engine, uintptr_t map, NameId wanted) {
        uintptr_t result{};
        return VisitMap(engine, map, 24, [&](const uint8_t* entry) {
            NameId id{};
            std::memcpy(&id, entry, sizeof(id));
            if (id == wanted) std::memcpy(&result, entry + sizeof(id), sizeof(result));
            return true;
        }) ? result : 0;
    }

    using AddRow = void(ANOMALY_CALL*)(uintptr_t, NameId, const void*);
    AddRow add_row_{};
};
}
