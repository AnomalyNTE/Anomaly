#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/services/core.h>
#include <anomaly/sdk/services/nte.h>
#include <anomaly/sdk/services/ue5.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include "purchase.hpp"
#include "product.hpp"
#include "native_layout.hpp"
#include <anomaly/sdk/localization.hpp>

namespace shop {
using Handle = AnomalyGenerationHandleV1;
using anomaly::sdk::StringView;
using anomaly::sdk::Succeeded;
inline bool Same(Handle a, Handle b) { return a.id == b.id && a.generation == b.generation; }

inline bool ActorDispatch(uintptr_t entry, uintptr_t actor, uintptr_t function, void* parameters) {
    using Call = void(__fastcall*)(void*, void*, void*);
    __try {
        reinterpret_cast<Call>(entry)(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(function), parameters);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline uintptr_t TextDisplay(uintptr_t entry, uintptr_t data) {
    using Call = uintptr_t(__fastcall*)(uintptr_t);
    __try { return reinterpret_cast<Call>(entry)(data); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

struct Parameter { const char* name; int offset, size; };
struct Row { Name key; uintptr_t address{}; };
struct Array { uintptr_t data{}; int32_t count{}, capacity{}; };
struct SoftAsset { int32_t index{-1}, serial{}; Name package{}, asset{}; Array subpath{}; };
static_assert(sizeof(SoftAsset) == 40);
struct SparseHeader {
    Array slots;
    std::array<uint32_t, 4> inline_flags{};
    uintptr_t heap_flags{};
    int32_t flags_count{}, flags_capacity{};
};
static_assert(sizeof(SparseHeader) == 48);

class Engine {
public:
    anomaly::sdk::Localizer localizer;
    NativeLayout layout;
    anomaly::sdk::Profile profile;
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyNtePlayerServiceV1* players{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyUe5BuildServiceV1* build{};

    bool ReadBytes(uintptr_t at, void* destination, size_t size) const {
        return at >= 0x10000 && size && size <= 16 * 1024 * 1024 && at <= 0x00007fffffffffffull - size &&
            Succeeded(core->read_memory(core->user, at, {static_cast<uint8_t*>(destination), size}));
    }
    template<class T> bool Read(uintptr_t at, T& value) const { return ReadBytes(at, &value, sizeof(value)); }
    std::string Text(uintptr_t address) const {
        std::array<char, 2048> text{};
        size_t size = text.size();
        if (names->resolve_ftext_utf8 && Succeeded(names->resolve_ftext_utf8(names->user, address, text.data(), &size)) &&
            size <= text.size() && text[0]) return {text.begin(), std::find(text.begin(), text.end(), '\0')};
        return StringTableDisplay(address);
    }
    std::string StringTableDisplay(uintptr_t address) const {
        // Some string-table entries have no cached display string. Ask the engine's
        // ITextData getter on the Game thread, then copy its borrowed FString.
        if (!build || !framework || !framework->is_game_thread(framework->user) ||
            build->feature_state(build->user, StringView("ue5.ftext")) != ANOMALY_FEATURE_V1_AVAILABLE) return {};
        uintptr_t data{}, table{}, getter{}, shared_getter{};
        MEMORY_BASIC_INFORMATION region{};
        if (!Read(address, data) || !Read(data, table) || !Read(table + layout.textDisplayGetter, getter) || !Read(table + layout.textSharedGetter, shared_getter) ||
            !anomaly::sdk::MatchesProfileEntry(core,profile,"text-shared-getter",shared_getter) ||
            !VirtualQuery(reinterpret_cast<const void*>(getter), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return {};
        Array value{};
        if (!Read(TextDisplay(getter, data), value) || value.count <= 1 || value.count > 2048 || value.capacity < value.count) return {};
        std::array<wchar_t, 2048> wide{};
        if (!ReadBytes(value.data, wide.data(), size_t(value.count) * sizeof(wchar_t)) || wide[value.count - 1]) return {};
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), value.count - 1, nullptr, 0, nullptr, nullptr);
        if (count <= 0) return {};
        std::string result(static_cast<size_t>(count), '\0');
        return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), value.count - 1, result.data(), count, nullptr, nullptr) == count
            ? result : std::string{};
    }
    std::string NameText(Name name) const {
        if (!name.id) return {};
        std::array<char, 512> text{};
        size_t size = text.size();
        if (!Succeeded(names->resolve_utf8(names->user, name.id, text.data(), &size)) || size > text.size()) return {};
        std::string value(text.begin(), std::find(text.begin(), text.end(), '\0'));
        if (name.number) value += "_" + std::to_string(name.number - 1);
        return value;
    }
    std::string ObjectName(uintptr_t at) const {
        Name name{};
        return Read(at + layout.objectName, name) ? NameText(name) : std::string{};
    }
    bool IsA(uintptr_t object, std::string_view type) const {
        uintptr_t cls{};
        if (!Read(object + layout.objectClass, cls)) return false;
        for (int depth = 0; cls && depth < 48; ++depth) {
            if (ObjectName(cls) == type) return true;
            uintptr_t parent{};
            if (!Read(cls + layout.structSuper, parent) || parent == cls) break;
            cls = parent;
        }
        return false;
    }
    Handle ObjectHandle(uintptr_t object) const {
        int32_t index{};
        uint32_t flags{};
        Name name{};
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        if (!Read(object + layout.objectIndex, index) || index < 0 || !Read(object + layout.objectFlags, flags) || (flags & 0x30) ||
            !Read(object + layout.objectName, name) || !Succeeded(objects->snapshot_at(objects->user, static_cast<uint32_t>(index), &snapshot)) ||
            snapshot.name_id != name.id) return {};
        return snapshot.handle;
    }
    Handle Find(std::string_view path) const {
        Handle handle{};
        objects->find_exact(objects->user, StringView(path), &handle);
        return handle;
    }
    uintptr_t LoadAsset(Name package, Name asset) const {
        if (!package.id || !asset.id) return 0;
        struct Load { SoftAsset asset; uintptr_t result{}; } load;
        static_assert(sizeof(Load) == 48);
        load.asset.package = package; load.asset.asset = asset;
        return Invoke(Find("/Script/Engine.Default__KismetSystemLibrary"),Find("/Script/Engine.KismetSystemLibrary.LoadAsset_Blocking"),&load,sizeof(load)) &&
            ObjectHandle(load.result).id ? load.result : 0;
    }
    uintptr_t LoadAsset(std::string_view package, std::string_view asset) const {
        Name package_name{}, asset_name{};
        if (names->struct_size >= offsetof(AnomalyUe5NamesServiceV1,find_utf8) + sizeof(names->find_utf8) && names->find_utf8 &&
            Succeeded(names->find_utf8(names->user,StringView(package),&package_name.id)) &&
            Succeeded(names->find_utf8(names->user,StringView(asset),&asset_name.id))) return LoadAsset(package_name,asset_name);
        std::wstring path;
        for (unsigned char c : std::string(package) + "." + std::string(asset)) {
            if (c >= 128 || c == ':') return 0;
            path.push_back(c);
        }
        struct Make { Array text; Name package{},asset{}; Array subpath{}; } make;
        static_assert(sizeof(Make) == 48);
        make.text = {reinterpret_cast<uintptr_t>(path.data()),int32_t(path.size() + 1),int32_t(path.size() + 1)};
        if (!Invoke(Find("/Script/Engine.Default__KismetSystemLibrary"),Find("/Script/Engine.KismetSystemLibrary.MakeSoftObjectPath"),&make,sizeof(make)) || make.subpath.count) return 0;
        package_name = make.package; asset_name = make.asset;
        return LoadAsset(package_name,asset_name);
    }
    int Field(uintptr_t type, std::string_view name, int size) const {
        for (int depth = 0; type && depth < 48; ++depth) {
            uintptr_t field{};
            if (!Read(type + layout.structProperties, field)) return -1;
            for (int index = 0; field && index < 4096; ++index) {
                Name field_name{};
                if (!Read(field + layout.propertyName, field_name)) return -1;
                if (NameText(field_name) == name) {
                    int actual_size{}, offset{};
                    if (Read(field + layout.propertySize, actual_size) && actual_size == size && Read(field + layout.propertyOffset, offset) &&
                        offset >= 0 && offset < 0x40000) return offset;
                    return -1;
                }
                uintptr_t next{};
                if (!Read(field + layout.propertyLinkNext, next) || next == field) return -1;
                field = next;
            }
            uintptr_t parent{};
            if (!Read(type + layout.structSuper, parent) || parent == type) break;
            type = parent;
        }
        return -1;
    }
    uintptr_t Child(uintptr_t object, std::string_view field) const {
        uintptr_t cls{}, result{};
        if (!Read(object + layout.objectClass, cls)) return 0;
        const int offset = Field(cls, field, 8);
        return offset >= 0 && Read(object + offset, result) ? result : 0;
    }
    uintptr_t Function(uintptr_t object, std::string_view name, int count, int size,
                       std::initializer_list<Parameter> parameters, uint32_t required_flags = 0x400) const {
        uintptr_t cls{};
        if (!Read(object + layout.objectClass, cls)) return 0;
        for (int depth = 0; cls && depth < 48; ++depth) {
            uintptr_t child{};
            if (!Read(cls + layout.structChildren, child)) return 0;
            for (int i = 0; child && i < 4096; ++i) {
                if (ObjectName(child) == name && IsA(child, "Function")) {
                    uint8_t actual_count{};
                    uint16_t actual_size{};
                    uint32_t flags{};
                    if (!Read(child + layout.functionParameterCount, actual_count) || actual_count != count || !Read(child + layout.functionParameterSize, actual_size) ||
                        actual_size != size || !Read(child + layout.functionFlags, flags) || (flags & required_flags) != required_flags) return 0;
                    for (const auto& parameter : parameters) {
                        if (Field(child, parameter.name, parameter.size) != parameter.offset) return 0;
                    }
                    return child;
                }
                uintptr_t next{};
                if (!Read(child + layout.fieldNext, next) || next == child) return 0;
                child = next;
            }
            uintptr_t parent{};
            if (!Read(cls + layout.structSuper, parent) || parent == cls) break;
            cls = parent;
        }
        return 0;
    }
    bool Invoke(Handle object, Handle function, void* parameters, size_t size) const {
        return object.id && function.id && Succeeded(events->invoke(events->user, object, function, parameters, size));
    }
    bool Invoke(uintptr_t object, uintptr_t function, void* parameters, size_t size) const {
        return Invoke(ObjectHandle(object), ObjectHandle(function), parameters, size);
    }
    bool InvokePurchase(uintptr_t actor, uintptr_t function, BuyParameters& parameters) const {
        return InvokeActor(actor,function,&parameters);
    }
    bool InvokeActor(uintptr_t actor, uintptr_t function, void* parameters) const {
        if (!framework->is_game_thread(framework->user) || !IsA(actor, "HTPlayerState") ||
            !ObjectHandle(actor).id || !ObjectHandle(function).id ||
            build->feature_state(build->user, StringView("ue5.actor-process-event")) != ANOMALY_FEATURE_V1_AVAILABLE) return false;
        // Actor dispatch uses the validated profile slot and handles network callspace.
        uintptr_t table{}, entry{};
        MEMORY_BASIC_INFORMATION region{};
        if (!Read(actor, table) || !Read(table + layout.actorProcessEvent, entry) ||
            !VirtualQuery(reinterpret_cast<const void*>(entry), &region, sizeof(region)) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        return ActorDispatch(entry, actor, function, parameters);
    }
    uintptr_t PlayerState(std::string& error) const {
        Handle controller{};
        if (!Succeeded(players->get_object_handles(players->user, nullptr, &controller))) { error = localizer.Text("player.wait_handle","Waiting for the local player handle."); return 0; }
        const auto getter = Find("/Script/Engine.Controller.K2_GetPawn");
        uintptr_t pawn{};
        if (!Invoke(controller, getter, &pawn, sizeof(pawn))) { error = localizer.Text("player.read_failed","Could not read the local character. Check ProcessEvent."); return 0; }
        if (!IsA(pawn, "HTPlayerCharacter")) { error = localizer.Text("player.loading","Waiting for the local character."); return 0; }
        const auto state = Child(pawn, "PlayerState");
        if (!IsA(state, "HTPlayerState")) { error = localizer.Text("player.no_state","PlayerState is unavailable."); return 0; }
        if (!ObjectHandle(state).id) { error = localizer.Text("player.invalid_state","PlayerState handle validation failed."); return 0; }
        return state;
    }
    uintptr_t GameData() const {
        uintptr_t result{};
        if (!Invoke(Find("/Script/HTGame.Default__HTGameData"), Find("/Script/HTGame.HTGameData.GetGameData"),
                    &result, sizeof(result)) || !IsA(result, "HTGameData")) return 0;
        return result;
    }
    bool Table(uintptr_t table, std::string_view expected_type, uintptr_t& row_type, std::vector<Row>& rows) const {
        if (!IsA(table, "DataTable") || !Read(table + layout.tableRowStruct, row_type) || (!expected_type.empty() && ObjectName(row_type) != expected_type) ||
            !IsA(row_type, "ScriptStruct")) return false;
        // UE5 TMap<FName, uint8*> uses sparse 24-byte set elements. Skip free-list slots.
        SparseHeader header{};
        if (!Read(table + layout.tableRowMap, header) || header.slots.count < 0 || header.slots.count > 131072 ||
            header.slots.capacity < header.slots.count || header.flags_count != header.slots.count ||
            header.flags_capacity < header.flags_count) return false;
        rows.clear();
        if (!header.slots.count) return true;
        std::vector<uint32_t> flags((header.flags_count + 31) / 32);
        if (header.heap_flags) {
            if (!ReadBytes(header.heap_flags, flags.data(), flags.size() * 4)) return false;
        } else {
            if (flags.size() > header.inline_flags.size()) return false;
            std::copy_n(header.inline_flags.begin(), flags.size(), flags.begin());
        }
        std::vector<uint8_t> entries(static_cast<size_t>(header.slots.count) * layout.mapStride);
        if (!ReadBytes(header.slots.data, entries.data(), entries.size())) return false;
        for (int i = 0; i < header.slots.count; ++i) {
            if (!(flags[i / 32] & (1u << (i % 32)))) continue;
            Row row{};
            std::memcpy(&row.key, entries.data() + size_t(i) * layout.mapStride + layout.mapKey, sizeof(row.key));
            std::memcpy(&row.address, entries.data() + size_t(i) * layout.mapStride + layout.mapValue, sizeof(row.address));
            if (!row.key.id || !row.address) return false;
            rows.push_back(row);
        }
        return true;
    }
};

struct IconSource {
    Name icon_package{}, icon_asset{};
    std::string icon_path;
};
inline IconSource ReadIcon(Engine& engine, uintptr_t address) {
    IconSource icon;
    if (!engine.Read(address + engine.layout.iconPackage,icon.icon_package) || !engine.Read(address + engine.layout.iconAsset,icon.icon_asset) ||
        !icon.icon_package.id || !icon.icon_asset.id) return {};
    const auto package = engine.NameText(icon.icon_package), asset = engine.NameText(icon.icon_asset);
    if (package.empty() || asset.empty()) return {};
    icon.icon_path = package + "." + asset;
    return icon;
}
inline IconSource LoadIcon(Engine& engine,std::string_view package,std::string_view asset) {
    const auto texture = engine.LoadAsset(package,asset);
    IconSource icon; uintptr_t outer{};
    if (!engine.IsA(texture,"Texture2D") || !engine.Read(texture + engine.layout.objectName,icon.icon_asset) ||
        !engine.Read(texture + engine.layout.objectOuter,outer) || !engine.IsA(outer,"Package") || !engine.Read(outer + engine.layout.objectName,icon.icon_package) ||
        engine.NameText(icon.icon_package) != package || engine.NameText(icon.icon_asset) != asset) return {};
    icon.icon_path = std::string(package) + "." + std::string(asset);
    return icon;
}
struct Shop { Name key; std::string id, label; uint8_t kind{}; IconSource icon; };
struct Goods {
    Name key, shop, currency;
    std::string id, label, shop_id, shop_label, currency_id, currency_label;
    int64_t price{}, minimum{1}, maximum{}, selector_max{}, limit{};
    bool locked{};
    uint8_t shop_kind{};
    Name icon_package{}, icon_asset{};
    std::string icon_path;
    std::optional<int64_t> stock;
    bool variable_limit{};
    IconSource shop_icon;
    std::string item_id, display_item_id;
    ProductKey Product() const {
        // Missing item identity must not merge unrelated offers with the same name.
        return {item_id.empty() ? "offer:" + std::to_string(shop_id.size()) + ":" + shop_id + id : "item:" + item_id,
            display_item_id,currency_id,price};
    }
};

class CatalogReader {
public:
    Engine& engine;
    std::map<uint64_t, Shop> shops;
    std::vector<Row> rows;
    std::vector<Goods> goods;
    size_t cursor{};
    bool ready{};
    std::array<int, 13> fields{};
    std::map<uint64_t, uintptr_t> item_names;
    std::map<uint64_t, std::pair<Name,Name>> item_icons;
    IconSource shop_fallback_icon;

    void MapShopIcons() {
        const auto data = engine.LoadAsset("/Game/DataAssets/MiniMapDataAsset","MiniMapDataAsset");
        uintptr_t type{}; std::vector<Row> map_rows;
        if (!engine.IsA(data,"MiniMapDataAsset") || !engine.Table(engine.Child(data,"MiniMapIconDataTable"),"MiniMapIconData",type,map_rows)) {
            if (engine.core->log) engine.core->log(engine.core->user,ANOMALY_CORE_LOG_LEVEL_V1_WARNING,StringView("RemoteShop map icon table unavailable asset=" + std::to_string(data)));
            return;
        }
        const auto name = engine.Field(type,"IconName",16);
        const auto details = engine.Field(type,"DetailsIcon",40);
        const auto listing = engine.Field(type,"ListIcon",40);
        if (name < 0) return;
        std::map<std::string,IconSource> icons;
        for (const auto& row : map_rows) {
            auto icon = details >= 0 ? ReadIcon(engine,row.address + details) : IconSource{};
            if (icon.icon_path.empty() && listing >= 0) icon = ReadIcon(engine,row.address + listing);
            if (icon.icon_path.empty()) continue;
            const auto label = engine.Text(row.address + name);
            if (!label.empty()) icons.try_emplace(label,std::move(icon));
        }
        size_t matched{};
        for (auto& [key,shop] : shops) if (shop.icon.icon_path.empty()) {
            const auto found = icons.find(shop.label);
            if (found != icons.end()) { shop.icon = found->second; ++matched; }
        }
        if (engine.core->log) engine.core->log(engine.core->user,ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            StringView("RemoteShop map shop icons matched=" + std::to_string(matched) + " labels=" + std::to_string(icons.size())));
    }

    explicit CatalogReader(Engine& value) : engine(value) {}
    static uint64_t Key(Name name) { return (uint64_t(name.number) << 32) | name.id; }
    void IndexItemTable(uintptr_t table) {
        uintptr_t type{};
        std::vector<Row> table_rows;
        if (!engine.Table(table, {}, type, table_rows)) return;
        const auto name_offset = engine.Field(type, "ItemName", 16);
        if (name_offset < 0) return;
        for (const auto& row : table_rows) item_names.try_emplace(Key(row.key), row.address + name_offset);
        const auto icon_offset = engine.Field(type, "ItemIcon", 40);
        if (icon_offset >= 0) for (const auto& row : table_rows) {
            Name package{}, asset{};
            if (engine.Read(row.address + icon_offset + engine.layout.iconPackage,package) && engine.Read(row.address + icon_offset + engine.layout.iconAsset,asset) && package.id && asset.id)
                item_icons.try_emplace(Key(row.key),std::pair{package,asset});
        }
    }
    std::string ItemName(Name key) const {
        const auto found = item_names.find(Key(key));
        return found == item_names.end() ? std::string{} : engine.Text(found->second);
    }
    bool Begin() {
        ready = false;
        shops.clear(); rows.clear(); goods.clear(); item_names.clear(); item_icons.clear(); cursor = 0;
        const auto data = engine.GameData();
        uintptr_t shop_type{}, goods_type{};
        std::vector<Row> shop_rows;
        if (!data || !engine.Table(engine.Child(data, "ShopDataTable"), "ShopData", shop_type, shop_rows) ||
            !engine.Table(engine.Child(data, "GoodsDataTable"), "GoodsData", goods_type, rows)) return false;
        const int shop_name = engine.Field(shop_type, "ShopName", 16);
        const int shop_kind = engine.Field(shop_type, "ShopType", 1);
        const int shop_icon = engine.Field(shop_type, "ShopIcon", 40);
        if (shop_name < 0 || shop_kind < 0) return false;
        IndexItemTable(engine.Child(data, "ItemDataTable"));
        // Food, furniture, currency and other item kinds use their own tables.
        uintptr_t item_type{};
        std::vector<Row> item_types;
        if (engine.Table(engine.Child(data, "ItemTypeDataTable"), "ItemTypeData", item_type, item_types)) {
            const auto table_offset = engine.Field(item_type, "ItemDataTable", 8);
            if (table_offset >= 0) for (const auto& row : item_types) {
                uintptr_t table{};
                if (engine.Read(row.address + table_offset, table)) IndexItemTable(table);
            }
        }
        for (const auto& row : shop_rows) {
            uint8_t kind{};
            if (!engine.Read(row.address + shop_kind, kind)) return false;
            if (kind != 0 && kind != 2 && kind != 3 && kind != 4 && kind != 5) continue;
            Shop shop{row.key, engine.NameText(row.key), engine.Text(row.address + shop_name), kind};
            if (shop.label.empty()) shop.label = shop.id;
            if (shop_icon >= 0) shop.icon = ReadIcon(engine,row.address + shop_icon);
            shops.emplace(Key(row.key), std::move(shop));
        }
        MapShopIcons();
        shop_fallback_icon = LoadIcon(engine,"/Game/UI/UI/common/function/shangdian","shangdian");
        if (engine.core->log) engine.core->log(engine.core->user,ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            StringView("RemoteShop shop fallback icon " + (shop_fallback_icon.icon_path.empty() ? std::string("unavailable") : shop_fallback_icon.icon_path)));
        if (engine.core->log) engine.core->log(engine.core->user,ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            StringView("RemoteShop shop icons " + std::to_string(std::ranges::count_if(shops,[](const auto& row){return !row.second.icon.icon_path.empty();})) +
                "/" + std::to_string(shops.size()) + " field=" + std::to_string(shop_icon)));
        constexpr std::array<Parameter, 13> specs{{
            {"OwnerShopID", 0, 8}, {"ItemName", 0, 16}, {"Price", 0, 8}, {"CurrencyID", 0, 8},
            {"MixAmount", 0, 8}, {"MaxAmount", 0, 8}, {"MaxSelectorAmount", 0, 8}, {"LimitAmount", 0, 8},
            {"ProductId", 0, 8}, {"ItemID", 0, 8}, {"BaseGoodsID", 0, 8}, {"ShowItemID", 0, 8}, {"LimitsFunctionAmount",0,16}}};
        for (size_t i = 0; i < fields.size(); ++i) {
            fields[i] = engine.Field(goods_type, specs[i].name, specs[i].size);
            if (fields[i] < 0) return false;
        }
        return true;
    }
    bool Step() {
        const auto end = (std::min)(rows.size(), cursor + 96);
        for (; cursor < end; ++cursor) {
            const auto& row = rows[cursor];
            Name owner{}, product{};
            if (!engine.Read(row.address + fields[0], owner) || !engine.Read(row.address + fields[8], product)) return false;
            const auto found = shops.find(Key(owner));
            if (found == shops.end() || product.id) continue;
            const auto& shop = found->second;
            Goods item{row.key, owner};
            item.id = engine.NameText(row.key);
            Name item_id{},display_id{};
            if (!engine.Read(row.address + fields[9],item_id) || !engine.Read(row.address + fields[11],display_id)) return false;
            if (item_id.id || item_id.number) item.item_id = engine.NameText(item_id);
            if (display_id.id || display_id.number) item.display_item_id = engine.NameText(display_id);
            for (const auto field : {fields[11],fields[9]}) {
                Name id{};
                if (!engine.Read(row.address + field,id)) continue;
                const auto icon = item_icons.find(Key(id));
                if (icon == item_icons.end()) continue;
                item.icon_package = icon->second.first; item.icon_asset = icon->second.second;
                item.icon_path = engine.NameText(item.icon_package) + "." + engine.NameText(item.icon_asset);
                break;
            }
            item.label = engine.Text(row.address + fields[1]);
            if (item.label.empty()) {
                item.label = ItemName(display_id);
                if (item.label.empty()) item.label = ItemName(item_id);
            }
            item.shop_id = shop.id;
            item.shop_label = shop.label;
            item.shop_kind = shop.kind;
            item.shop_icon = shop.icon;
            if (item.label.empty()) item.label = item.id;
            if (!engine.Read(row.address + fields[2], item.price) || !engine.Read(row.address + fields[3], item.currency) ||
                !engine.Read(row.address + fields[4], item.minimum) || !engine.Read(row.address + fields[5], item.maximum) ||
                !engine.Read(row.address + fields[6], item.selector_max) || !engine.Read(row.address + fields[7], item.limit)) return false;
            item.minimum = (std::max)(int64_t(1), item.minimum);
            Array boosts{};
            if (!engine.Read(row.address + fields[12],boosts)) return false;
            item.variable_limit = boosts.count != 0;
            item.currency_id = engine.NameText(item.currency);
            item.currency_label = ItemName(item.currency);
            if (item.currency_label.empty()) item.currency_label = item.currency_id;
            // Static list prices may differ from a live time-limited discount. The server settles the order.
            goods.push_back(std::move(item));
        }
        ready = cursor == rows.size();
        if (ready) std::ranges::sort(goods, [](const Goods& a, const Goods& b) {
            return std::tie(a.shop_label, a.shop_id, a.label, a.id) < std::tie(b.shop_label, b.shop_id, b.label, b.id);
        });
        return true;
    }
};
} // namespace shop
