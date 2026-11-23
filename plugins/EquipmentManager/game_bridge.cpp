#include "game_bridge.hpp"
#include "catalog_data.hpp"
#include "model_data.hpp"
#include "progression_data.hpp"
#include "avatar_export.hpp"

#include <Windows.h>
#include <anomaly/sdk/profile.hpp>
#include <anomaly/sdk/services/nte.h>
#include <anomaly/sdk/services/ue5.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace equipment_manager {
namespace {
using Json = nlohmann::json;
using Handle = AnomalyGenerationHandleV1;
using anomaly::sdk::StringView;
using anomaly::sdk::Succeeded;

// Fixed UE call ABI. Memory layouts and compatibility values come from Profile.
struct Uid {
    std::uint32_t slot{}, serial{};
    auto operator<=>(const Uid&) const = default;
    bool Valid() const { return slot != 0 && serial != 0 && slot != UINT32_MAX && serial != UINT32_MAX; }
};
struct Array { std::uintptr_t data{}; std::int32_t count{}, capacity{}; };
struct PairParameters { Uid character, equipment; };
struct InlayParameters { Uid character, equipment; std::int32_t row{}, column{}; };
struct Placement { Uid equipment; std::int32_t row{}, column{}; };
struct OneKeyParameters { Uid character; Array placements; Uid core; };
struct StateParameters { Uid equipment; std::uint8_t state{}; std::array<std::uint8_t, 3> padding{}; };
struct NotifyParameters {
    std::uintptr_t inventory{};
    std::uint8_t type{};
    std::array<std::uint8_t, 7> padding{};
    Array items;
    std::array<std::uint8_t, 24> parameters{};
};
struct CurveParameters {
    std::uintptr_t table{};
    std::uint64_t row{};
    float level{};
    std::uint8_t result{1};
    std::array<std::uint8_t, 3> result_padding{};
    float value{};
    std::array<std::uint8_t, 4> value_padding{};
    Array context;
};
struct SkillParameters {
    std::uintptr_t character{};
    Uid uid;
    std::uint64_t ability{};
    std::int32_t base{}, awakening{}, effective{};
};
struct TotalParameters {
    std::uint64_t item{};
    std::uint8_t contains_bound{1};
    std::array<std::uint8_t, 7> padding{};
    std::int64_t value{};
};
static_assert(sizeof(Uid) == 8 && sizeof(Array) == 16 && sizeof(PairParameters) == 16);
static_assert(sizeof(InlayParameters) == 24 && sizeof(Placement) == 16 && sizeof(OneKeyParameters) == 32);
static_assert(offsetof(OneKeyParameters, placements) == 8 && offsetof(OneKeyParameters, core) == 24);
static_assert(sizeof(StateParameters) == 12 && offsetof(StateParameters, state) == 8);
static_assert(sizeof(NotifyParameters) == 56);
static_assert(sizeof(CurveParameters) == 48 && offsetof(CurveParameters, result) == 20 &&
    offsetof(CurveParameters, value) == 24 && offsetof(CurveParameters, context) == 32);
static_assert(sizeof(SkillParameters) == 40 && offsetof(SkillParameters, base) == 24 &&
    offsetof(SkillParameters, awakening) == 28 && offsetof(SkillParameters, effective) == 32);
static_assert(sizeof(TotalParameters) == 24 && offsetof(TotalParameters, contains_bound) == 8 && offsetof(TotalParameters, value) == 16);

[[noreturn]] void Fail(std::string_view detail) { throw std::runtime_error(std::string(detail)); }
class NotReady final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
Json Encode(Uid uid) { return uid.Valid() ? Json{{"slot", uid.slot}, {"serial", uid.serial}} : Json(nullptr); }
Uid Decode(const Json& source) {
    if (!source.is_object() || !source.contains("slot") || !source.contains("serial")) Fail("A formal item UID is required");
    const auto component = [](const Json& value) {
        if (!value.is_number_integer() || value.get<std::int64_t>() <= 0 ||
            value.get<std::uint64_t>() >= UINT32_MAX) Fail("Invalid formal item UID");
        return value.get<std::uint32_t>();
    };
    return {component(source.at("slot")), component(source.at("serial"))};
}
std::string StatusDetail(const AnomalyStatusV1& status, std::string_view operation) {
    std::string message(operation);
    message += " failed (status " + std::to_string(status.code) + ")";
    if (status.message.data && status.message.size) message.append(": ").append(status.message.data, status.message.size);
    return message;
}
void Check(const AnomalyStatusV1& status, std::string_view operation) {
    if (!Succeeded(status)) Fail(StatusDetail(status, operation));
}
bool Same(Handle a, Handle b) { return a.id == b.id && a.generation == b.generation; }
template<class Service> bool HasTable(const Service* service, std::size_t size) {
    return service && service->service_version >= 1 && service->struct_size >= size;
}
}

struct GameBridge::Impl {
    using NativeArray = Array;
    anomaly::sdk::ProfileSnapshot profile;
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyNteAccountServiceV1* account{};
    const AnomalyNteSessionServiceV1* session{};
    Handle subscription{}, pawn_handle{}, controller_handle{}, state_handle{};
    std::uintptr_t registry{}, controller{}, player_state{}, inventory{}, item_table{}, equipment_table{};
    std::uint64_t binding_generation{};
    std::atomic<std::uint64_t> context{}, notifications{};
    std::atomic<std::uint64_t> source_notifications{};
    std::atomic<bool> running{};
    std::map<std::string, std::uintptr_t, std::less<>> classes;
    mutable std::map<std::uint64_t, std::string> resolved_names;
    struct Function { Handle handle{}; std::uintptr_t pointer{}; };
    std::map<std::string, Function, std::less<>> functions;
    std::uintptr_t notify_function{};
    struct SnapshotProbe {
        std::array<std::uint64_t, 13> identity{};
        std::uint64_t inventory_sequence{};
        std::optional<std::string> account;
        bool operator==(const SnapshotProbe&) const = default;
    };
    std::optional<SnapshotProbe> last_snapshot_probe;
    std::optional<AnomalyNteSessionSnapshotV1> observed_session;
    bool layouts_valid{}, static_schema_valid{};
    struct Metadata {
        std::string name, quality, kind, shape, suit;
        Json cells = Json::array();
        std::int32_t max_level{}, grid_count{};
        std::uintptr_t row{}, data{};
    };
    std::map<std::uint64_t, std::uintptr_t> static_rows;
    std::map<std::uint64_t, std::uintptr_t> equipment_rows;
    std::map<std::uint64_t, Metadata> metadata;
    std::uintptr_t current_game_data{}, current_character_asset{};
    std::uintptr_t attribute_table{}, curve_table{}, suit_table{}, curve_function{}, curve_receiver{};
    bool statistics_schema_checked{}, statistics_schema_valid{};
    bool curve_binding_checked{}, curve_binding_valid{};
    std::string statistics_diagnostic;
    struct AttributeDefinition { std::string name; bool percent{}; };
    std::map<std::uint64_t, std::uintptr_t> attribute_rows, suit_rows;
    std::map<std::uint64_t, AttributeDefinition> attribute_definitions;
    std::map<std::string, std::pair<Json, bool>, std::less<>> suit_definitions;
    std::map<std::string, std::string, std::less<>> suit_names;
    std::uint32_t suit_title_field{};
    std::optional<std::uint32_t> plan_core_field;
    std::map<std::string, std::uint64_t, std::less<>> curve_rows;
    std::map<std::pair<std::string, std::int32_t>, std::optional<double>> curve_values;
    struct StatisticsCache {
        std::uint64_t item_id{};
        std::int32_t level{};
        std::optional<std::int32_t> gold_core_level;
        std::vector<std::uint8_t> bases, randoms;
        Json value;
    };
    std::map<Uid, StatisticsCache> statistics_cache;
    struct CatalogTable {
        std::uintptr_t pointer{};
        std::vector<std::pair<std::uint64_t, std::uintptr_t>> rows;
        mutable std::map<std::string, model_data::Row, std::less<>> name_index;
        mutable bool names_ready{};
    };
    std::map<std::string, CatalogTable, std::less<>> catalog_tables;
    std::set<std::uintptr_t> catalog_element_layouts;
    bool progression_layouts_valid{};
    bool skill_binding_checked{}, skill_binding_valid{};
    std::uintptr_t skill_function{}, skill_receiver{};
    std::optional<catalog_data::FloatPack> float_pack_layout;
    std::optional<catalog_data::Modifiers> modifier_layout;
    std::optional<catalog_data::Recipes> recipe_layout;
    std::optional<catalog_data::SkillCosts> skill_cost_layout;
    std::map<std::string, std::uint32_t, std::less<>> catalog_source_fields;
    std::optional<model_data::Layout> model_layout;
    std::map<std::string, std::map<std::int64_t, std::string>, std::less<>> enum_names;
    bool total_binding_checked{}, total_binding_valid{};
    std::uintptr_t total_function{};
    std::optional<catalog_data::ImagePaths> image_layout;
    std::map<std::uintptr_t, Json> item_images, equipment_images, appearance_images;
    struct Appearance {
        std::uintptr_t row{};
        std::uint64_t character{};
        bool fashion{}, is_default{};
        std::vector<std::uint64_t> exclusions;
    };
    std::map<std::uint64_t, Appearance> appearance_rows;
    std::vector<std::uint64_t> appearance_order;
    std::map<std::uint64_t, std::uint64_t> default_appearances;
    std::uintptr_t appearance_table{};
    std::map<std::string, std::uintptr_t, std::less<>> avatar_functions, avatar_default_objects;
    std::map<std::string, std::uint8_t, std::less<>> avatar_enum_values;
    struct AvatarResourceLayout { std::uint32_t resource{}, rhi{}; };
    std::optional<AvatarResourceLayout> avatar_resource_layout;
    struct AvatarRequest {
        std::string role, asset;
        bool loading_requested{}, submitted{};
        Handle target{};
        std::uint64_t fashion{};
        std::size_t variant{};
    };
    std::map<std::wstring, AvatarRequest, std::less<>> avatar_requests;

    std::uint32_t V(std::string_view name) const {
        const auto& layout = profile.Document().layout;
        const auto found = layout.find(name);
        if (found == layout.end()) Fail("Missing Profile layout field: " + std::string(name));
        return found->second;
    }
    void ReadBytes(std::uintptr_t address, void* destination, std::size_t size) const {
        if (!address || (!destination && size)) Fail("Unreadable game data");
        Check(core->read_memory(core->user, address, {static_cast<std::uint8_t*>(destination), size}), "Read game data");
    }
    template<class T> T ReadValue(std::uintptr_t address) const {
        T value{}; ReadBytes(address, &value, sizeof(value)); return value;
    }
    template<class T> T Field(std::uintptr_t object, std::string_view name) const { return ReadValue<T>(object + V(name)); }
    std::vector<std::uint8_t> Block(std::uintptr_t address, std::size_t size) const {
        std::vector<std::uint8_t> result(size); ReadBytes(address, result.data(), result.size()); return result;
    }
    template<class T> static T ExtractAt(std::span<const std::uint8_t> bytes, std::size_t offset) {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) Fail("Profile field exceeds its record");
        T result{}; std::memcpy(&result, bytes.data() + offset, sizeof(result)); return result;
    }
    template<class T> T Extract(std::span<const std::uint8_t> bytes, std::string_view name) const {
        return ExtractAt<T>(bytes, V(name));
    }
    void GameOnly() const {
        if (!running.load(std::memory_order_acquire) || !framework->is_game_thread(framework->user))
            Fail("Equipment access requires an active Game callback");
    }
    std::string Name(std::uint64_t name) const {
        if (const auto cached = resolved_names.find(name); cached != resolved_names.end()) return cached->second;
        std::array<char, 512> buffer{}; std::size_t size = buffer.size();
        Check(names->resolve_utf8(names->user, static_cast<std::uint32_t>(name), buffer.data(), &size), "Resolve game name");
        if (!size || size > buffer.size()) Fail("Invalid game name size");
        std::string result(buffer.data(), size - 1);
        if (const auto number = static_cast<std::uint32_t>(name >> 32U); number != 0) result += "_" + std::to_string(number - 1U);
        resolved_names.emplace(name, result);
        return result;
    }
    std::string ObjectName(std::uintptr_t object) const { return Name(Field<std::uint64_t>(object, "objectName")); }
    std::uintptr_t Pointer(Handle handle) const {
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        Check(objects->snapshot_by_handle(objects->user, handle, &snapshot), "Resolve object handle");
        const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
        const auto count = Field<std::int32_t>(registry, "registryCount");
        if (count < 0 || index >= static_cast<std::uint32_t>(count)) Fail("Object index is outside the current registry");
        const auto chunks = Field<std::uintptr_t>(registry, "registryItems");
        const auto chunk = ReadValue<std::uintptr_t>(chunks + (index / V("registryChunkSize")) * sizeof(std::uintptr_t));
        const auto slot = chunk + (index % V("registryChunkSize")) * V("registryItemStride");
        const auto pointer = Field<std::uintptr_t>(slot, "registryItemPointer");
        if (!pointer || Field<std::uint32_t>(slot, "registryItemSerial") != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ||
            Field<std::int32_t>(pointer, "objectInternalIndex") != static_cast<std::int32_t>(index))
            Fail("Object identity changed");
        return pointer;
    }
    Handle HandleFor(std::uintptr_t pointer) const {
        const auto index = Field<std::int32_t>(pointer, "objectInternalIndex");
        if (index < 0) Fail("Game object has no registry identity");
        AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
        Check(objects->snapshot_at(objects->user, static_cast<std::uint32_t>(index), &snapshot), "Register game object");
        if (Pointer(snapshot.handle) != pointer) Fail("Game object registry identity changed");
        return snapshot.handle;
    }
    std::pair<Handle, std::uintptr_t> Exact(std::string_view path) const {
        Handle handle{}; Check(objects->find_exact(objects->user, StringView(path), &handle), path);
        return {handle, Pointer(handle)};
    }
    std::uintptr_t Class(std::string_view name) {
        auto found = classes.find(name);
        if (found != classes.end()) return found->second;
        const auto pointer = Exact("/Script/HTGame." + std::string(name)).second;
        classes.emplace(name, pointer); return pointer;
    }
    bool Derived(std::uintptr_t type, std::uintptr_t expected) const {
        for (unsigned guard{}; type && guard < 64; ++guard) {
            if (type == expected) return true;
            type = Field<std::uintptr_t>(type, "structSuper");
        }
        return false;
    }
    std::uintptr_t FindProperty(std::uintptr_t type, std::string_view wanted) const {
        for (unsigned depth{}; type && depth < 64; ++depth) {
            auto field = Field<std::uintptr_t>(type, "structProperties");
            for (unsigned guard{}; field && guard < 1024; ++guard) {
                if (Name(Field<std::uint64_t>(field, "fieldName")) == wanted) return field;
                field = Field<std::uintptr_t>(field, "fieldNext");
            }
            type = Field<std::uintptr_t>(type, "structSuper");
        }
        Fail("Required reflected property is missing: " + std::string(wanted));
    }
    std::uintptr_t Property(std::uintptr_t type, std::string_view name, std::string_view kind,
                            std::size_t offset, std::size_t width, std::string_view value_type = {}) const {
        const auto field = FindProperty(type, name);
        const auto field_class = Field<std::uintptr_t>(field, "fieldClass");
        const auto actual_kind = Name(Field<std::uint32_t>(field_class, "fieldClassName"));
        const bool enum_kind = kind == "EnumProperty" && actual_kind == "ByteProperty";
        if ((!enum_kind && actual_kind != kind) || Field<std::int32_t>(field, "propertyArrayDim") != 1 ||
            Field<std::int32_t>(field, "propertyElementSize") != static_cast<std::int32_t>(width) ||
            Field<std::int32_t>(field, "propertyOffset") != static_cast<std::int32_t>(offset))
            Fail("Reflected property contract mismatch: " + std::string(name));
        if (!value_type.empty()) {
            auto inner = field;
            if (kind == "ArrayProperty") inner = Field<std::uintptr_t>(field, "arrayInner");
            const auto struct_type = Field<std::uintptr_t>(inner, "propertyStruct");
            if (!struct_type || ObjectName(struct_type) != value_type)
                Fail("Reflected value type mismatch: " + std::string(name));
        }
        return field;
    }
    void MemoryProperty(std::uintptr_t type, std::string_view name, std::string_view kind,
                        std::string_view offset, std::size_t width, std::string_view value_type = {}) const {
        Property(type, name, kind, V(offset), width, value_type);
    }
    void StructSize(std::uintptr_t type, std::string_view key) const {
        if (Field<std::int32_t>(type, "structSize") != static_cast<std::int32_t>(V(key)))
            Fail("Reflected structure size mismatch: " + ObjectName(type));
    }
    void ValidateLayouts() {
        if (layouts_valid) return;
        const auto item = Class("HTItem"), equipment = Class("HTEquipmentItem"), character = Class("HTCharacterItem");
        StructSize(item, "itemSize"); StructSize(equipment, "equipmentSize"); StructSize(character, "characterSize");
        MemoryProperty(item, "ItemID", "NameProperty", "itemId", sizeof(std::uint64_t));
        MemoryProperty(item, "UniqueID", "StructProperty", "itemUid", sizeof(Uid), "HTItemNetID");
        MemoryProperty(item, "Amount", "Int64Property", "itemAmount", sizeof(std::int64_t));
        MemoryProperty(item, "bIsTemporary", "BoolProperty", "itemTemporary", sizeof(std::uint8_t));
        MemoryProperty(item, "RandomBaseModifyData", "ArrayProperty", "itemRandomBase", sizeof(Array));
        MemoryProperty(item, "RandomModifyData", "ArrayProperty", "itemRandom", sizeof(Array), "RandomModifyDataSaveStruct");
        MemoryProperty(equipment, "StrengthenLevel", "IntProperty", "equipmentLevel", sizeof(std::int32_t));
        MemoryProperty(equipment, "EXP", "IntProperty", "equipmentExperience", sizeof(std::int32_t));
        MemoryProperty(equipment, "CharacterNetID", "StructProperty", "equipmentCharacter", sizeof(Uid), "HTItemNetID");
        MemoryProperty(equipment, "IsLocked", "BoolProperty", "equipmentLocked", sizeof(std::uint8_t));
        MemoryProperty(equipment, "IsDiscarded", "BoolProperty", "equipmentDiscarded", sizeof(std::uint8_t));
        MemoryProperty(character, "EquipmentSlots", "ArrayProperty", "characterSlots", sizeof(Array), "EquipmentStruct");
        MemoryProperty(character, "EquipCoreSlots", "ArrayProperty", "characterCoreSlots", sizeof(Array), "EquipmentSlotInfo");
        MemoryProperty(Class("HTPlayerState"), "InventoryComponent", "ObjectProperty", "playerStateInventory", sizeof(std::uintptr_t));
        MemoryProperty(Class("HTInventoryComponent"), "InventoryContainerMap", "MapProperty", "inventoryContainerMap", V("mapSize"));
        const auto slot = Class("EquipmentSlotInfo"); StructSize(slot, "slotSize");
        MemoryProperty(slot, "State", "IntProperty", "slotState", sizeof(std::int32_t));
        MemoryProperty(slot, "EquipNetID", "StructProperty", "slotUid", sizeof(Uid), "HTItemNetID");
        MemoryProperty(slot, "bFirstStep", "BoolProperty", "slotAnchor", sizeof(std::uint8_t));
        MemoryProperty(slot, "Row", "IntProperty", "slotRow", sizeof(std::int32_t));
        MemoryProperty(slot, "Column", "IntProperty", "slotColumn", sizeof(std::int32_t));
        const auto row = Class("EquipmentStruct"); StructSize(row, "rowSize");
        MemoryProperty(row, "SlotList", "ArrayProperty", "rowSlots", sizeof(Array), "EquipmentSlotInfo");
        layouts_valid = true;
    }
    void Parameter(std::uintptr_t function, std::string_view name, std::string_view type,
                   std::size_t offset, std::size_t width, std::string_view value_type = {}) const {
        Property(function, name, type, offset, width, value_type);
    }
    template<class Validate> std::uintptr_t AvatarFunction(std::string_view path, std::string_view symbol,
                                                          std::size_t size, Validate&& validate) {
        if (const auto found = avatar_functions.find(path); found != avatar_functions.end()) return found->second;
        const auto separator = path.find(':');
        if (separator == std::string_view::npos) Fail("The avatar function owner path is invalid");
        const auto function = Exact(path).second;
        if (ObjectName(Field<std::uintptr_t>(function, "objectClass")) != "Function" ||
            Field<std::uintptr_t>(function, "objectOuter") != Exact(path.substr(0, separator)).second ||
            (Field<std::uint32_t>(function, "functionFlags") & V("functionNativeFlag")) == 0 ||
            Field<std::uint16_t>(function, "functionParameterSize") != size ||
            !profile.MatchesEntry(core, symbol, Field<std::uintptr_t>(function, "functionExec")))
            Fail("The avatar engine function does not match the active Profile: " + std::string(path));
        validate(function);
        return avatar_functions.emplace(std::string(path), function).first->second;
    }
    std::uintptr_t AvatarDefaultObject(std::string_view class_path) {
        if (const auto found = avatar_default_objects.find(class_path); found != avatar_default_objects.end()) return found->second;
        const auto separator = class_path.find_last_of('.');
        if (separator == std::string_view::npos) Fail("The avatar function library path is invalid");
        const auto path = std::string(class_path.substr(0, separator + 1)) + "Default__" + std::string(class_path.substr(separator + 1));
        const auto object = Exact(path).second;
        if (Field<std::uintptr_t>(object, "objectClass") != Exact(class_path).second)
            Fail("The avatar function library default object has an unsupported type");
        return avatar_default_objects.emplace(std::string(class_path), object).first->second;
    }
    void AvatarInvoke(std::uintptr_t receiver, std::uintptr_t function, void* parameters, std::size_t size) const {
        Check(events->invoke(events->user, HandleFor(receiver), HandleFor(function), parameters, size), "Invoke avatar engine function");
    }
    std::uint8_t AvatarEnumValue(std::string_view path, std::string_view entry) {
        const auto key = std::string(path) + ":" + std::string(entry);
        if (const auto found = avatar_enum_values.find(key); found != avatar_enum_values.end()) return found->second;
        const auto type = Exact(path).second;
        if (ObjectName(Field<std::uintptr_t>(type, "objectClass")) != "Enum") Fail("The avatar engine enum schema is unsupported");
        const auto qualified = ObjectName(type) + "::" + std::string(entry);
        std::optional<std::uint8_t> value;
        VisitArray(Field<Array>(type, "enumNames"), V("enumEntryStride"), 1024, [&](auto row) {
            const auto spelling = Name(ExtractAt<std::uint64_t>(row, 0));
            if (spelling != entry && spelling != qualified) return;
            const auto number = Extract<std::int64_t>(row, "tableValue");
            if (value || number < 0 || number > UINT8_MAX) Fail("The avatar engine enum entry is ambiguous or out of range");
            value = static_cast<std::uint8_t>(number);
        });
        if (!value) Fail("The required avatar engine enum entry is unavailable");
        return avatar_enum_values.emplace(key, *value).first->second;
    }
    struct AvatarTextureState { bool resource_present{}, rhi_ready{}; };
    AvatarTextureState AvatarTextureReadiness(std::uintptr_t texture) {
        if (!avatar_resource_layout) {
            const auto resource = profile.Value("avatarTextureGameResource", 65536, alignof(std::uintptr_t));
            const auto rhi = profile.Value("avatarResourceTextureRhi", 4096, alignof(std::uintptr_t));
            const auto size = catalog_data::Size(*this, Exact("/Script/Engine.Texture").second);
            if (resource > size || sizeof(std::uintptr_t) > size - resource)
                Fail("The avatar texture resource field exceeds its native owner");
            avatar_resource_layout = AvatarResourceLayout{resource, rhi};
        }
        const auto resource = ReadValue<std::uintptr_t>(texture + avatar_resource_layout->resource);
        return {resource != 0, resource && ReadValue<std::uintptr_t>(resource + avatar_resource_layout->rhi) != 0};
    }
    Function FunctionFor(std::string_view symbol, std::string_view method, bool rpc) {
        if (const auto found = functions.find(symbol); found != functions.end()) {
            // UE can allocate a weak-object serial on the first net RPC. The
            // reflected function is unchanged, but its previous handle expires.
            found->second.handle = HandleFor(found->second.pointer);
            return found->second;
        }
        const bool controller_method = symbol == "get-player-state" || symbol == "inventory-notify";
        const auto owner_name = controller_method ? "HTPlayerController" : symbol == "get-game-data" ? "HTGameData" : "HTPlayerState";
        const auto [handle, pointer] = Exact("/Script/HTGame." + std::string(owner_name) + ":" + std::string(method));
        if (ObjectName(Field<std::uintptr_t>(pointer, "objectClass")) != "Function" ||
            Field<std::uintptr_t>(pointer, "objectOuter") != Class(owner_name)) Fail("RPC declaring class mismatch");
        const auto flags = Field<std::uint32_t>(pointer, "functionFlags");
        const auto required_flags = V(rpc ? "functionRpcFlags" : "functionNativeFlag");
        if ((flags & required_flags) != required_flags || !profile.MatchesEntry(core, symbol,
                Field<std::uintptr_t>(pointer, "functionExec"))) Fail("RPC native entry does not match the active Profile");
        const auto size = Field<std::uint16_t>(pointer, "functionParameterSize");
        std::size_t expected{};
        if (symbol == "get-player-state" || symbol == "get-game-data") {
            expected = sizeof(std::uintptr_t); Parameter(pointer, "ReturnValue", "ObjectProperty", 0, sizeof(std::uintptr_t));
        } else if (symbol == "inventory-notify") {
            expected = sizeof(NotifyParameters);
            Parameter(pointer, "pInventory", "ObjectProperty", offsetof(NotifyParameters, inventory), sizeof(std::uintptr_t));
            Parameter(pointer, "NotifyType", "EnumProperty", offsetof(NotifyParameters, type), sizeof(std::uint8_t));
            Parameter(pointer, "Items", "ArrayProperty", offsetof(NotifyParameters, items), sizeof(Array), "HTItemNetInfo");
            Parameter(pointer, "Parameters", "StructProperty", offsetof(NotifyParameters, parameters), sizeof(NotifyParameters::parameters), "ParameterWrapperArray");
        } else if (symbol == "set-locked" || symbol == "set-discarded") {
            // UFunction ParmsSize ends at the bool, before C++ tail padding.
            expected = offsetof(StateParameters, state) + sizeof(StateParameters::state);
            Parameter(pointer, "ItemNetID", "StructProperty", 0, sizeof(Uid), "HTItemNetID");
            Parameter(pointer, symbol == "set-locked" ? "bLocked" : "bDiscarded", "BoolProperty", offsetof(StateParameters, state), 1);
        } else {
            Parameter(pointer, "InCharacterID", "StructProperty", 0, sizeof(Uid), "HTItemNetID");
            if (symbol == "clear") expected = sizeof(Uid);
            else if (symbol == "equip-one-key") {
                expected = sizeof(OneKeyParameters);
                Parameter(pointer, "InPlaceList", "ArrayProperty", offsetof(OneKeyParameters, placements), sizeof(Array), "EquipPlaceData");
                Parameter(pointer, "InCoreNetID", "StructProperty", offsetof(OneKeyParameters, core), sizeof(Uid), "HTItemNetID");
                const auto placement = Class("EquipPlaceData");
                if (Field<std::int32_t>(placement, "structSize") != sizeof(Placement)) Fail("Placement ABI size mismatch");
                Parameter(placement, "UniqueID", "StructProperty", offsetof(Placement, equipment), sizeof(Uid), "HTItemNetID");
                Parameter(placement, "Row", "IntProperty", offsetof(Placement, row), sizeof(std::int32_t));
                Parameter(placement, "Column", "IntProperty", offsetof(Placement, column), sizeof(std::int32_t));
            } else {
                const bool position = symbol == "equip-module" || symbol == "move-module";
                expected = position ? sizeof(InlayParameters) : sizeof(PairParameters);
                Parameter(pointer, symbol == "replace-core" ? "NewEquipmentNetID" : "InEquipmentID", "StructProperty", offsetof(PairParameters, equipment), sizeof(Uid), "HTItemNetID");
                if (position) {
                    Parameter(pointer, "InRow", "IntProperty", offsetof(InlayParameters, row), sizeof(std::int32_t));
                    Parameter(pointer, "InColumn", "IntProperty", offsetof(InlayParameters, column), sizeof(std::int32_t));
                }
            }
        }
        if (size != expected) Fail("RPC parameter buffer size mismatch for " + std::string(method) +
            ": reflected=" + std::to_string(size) + "; expected=" + std::to_string(expected));
        const Function result{handle, pointer}; functions.emplace(symbol, result); return result;
    }
    void Invoke(Handle receiver, const Function& function, void* parameters, std::size_t size) const {
        Check(events->invoke(events->user, receiver, function.handle, parameters, size), "Dispatch equipment RPC");
    }
    void EnsureContext() {
        GameOnly();
        const auto generation = objects->generation(objects->user);
        if (binding_generation != generation) {
            binding_generation = generation; classes.clear(); resolved_names.clear(); functions.clear(); layouts_valid = false; static_schema_valid = false;
            static_rows.clear(); equipment_rows.clear(); metadata.clear(); item_table = 0; equipment_table = 0; notify_function = 0;
            last_snapshot_probe.reset();
            statistics_schema_checked = false; statistics_schema_valid = false;
            attribute_table = 0; curve_table = 0; suit_table = 0; curve_function = 0; curve_receiver = 0;
            attribute_rows.clear(); suit_rows.clear(); attribute_definitions.clear(); curve_values.clear(); curve_rows.clear(); suit_definitions.clear();
            curve_binding_checked = false; curve_binding_valid = false;
            statistics_cache.clear();
            catalog_tables.clear(); catalog_element_layouts.clear(); progression_layouts_valid = false;
            skill_binding_checked = false; skill_binding_valid = false; skill_function = 0; skill_receiver = 0;
            float_pack_layout.reset(); modifier_layout.reset(); recipe_layout.reset();
            skill_cost_layout.reset();
            catalog_source_fields.clear();
            model_layout.reset(); enum_names.clear();
            total_binding_checked = false; total_binding_valid = false; total_function = 0;
            image_layout.reset(); item_images.clear(); equipment_images.clear(); appearance_images.clear();
            appearance_rows.clear(); appearance_order.clear(); default_appearances.clear(); appearance_table = 0;
            avatar_functions.clear(); avatar_default_objects.clear(); avatar_enum_values.clear(); avatar_requests.clear(); avatar_resource_layout.reset();
        }
        Handle next_pawn{}, next_controller{};
        const auto controller_status = player->get_object_handles(player->user, &next_pawn, &next_controller);
        if (controller_status.code == ANOMALY_STATUS_V1_UNAVAILABLE || controller_status.code == ANOMALY_STATUS_V1_NOT_FOUND)
            throw NotReady(StatusDetail(controller_status, "Get local controller"));
        Check(controller_status, "Get local controller");
        pawn_handle = next_pawn;
        const auto controller_pointer = Pointer(next_controller);
        if (!Derived(Field<std::uintptr_t>(controller_pointer, "objectClass"), Class("HTPlayerController"))) Fail("Local controller has an unsupported type");
        std::uintptr_t next_state{};
        Invoke(next_controller, FunctionFor("get-player-state", "GetHTPlayerState", false), &next_state, sizeof(next_state));
        if (!next_state) throw NotReady("The local player state is not ready");
        const auto next_handle = HandleFor(next_state);
        if (!Derived(Field<std::uintptr_t>(next_state, "objectClass"), Class("HTPlayerState"))) Fail("Local player state has an unsupported type");
        ValidateLayouts();
        const auto next_inventory = Field<std::uintptr_t>(next_state, "playerStateInventory");
        if (!next_inventory) throw NotReady("The local inventory is not ready");
        if (Field<std::uintptr_t>(next_inventory, "objectClass") != Class("HTInventoryComponent")) Fail("The local inventory has an unsupported type");
        HandleFor(next_inventory);
        if (!Same(controller_handle, next_controller) || !Same(state_handle, next_handle) || inventory != next_inventory) {
            controller_handle = next_controller; state_handle = next_handle;
            controller = controller_pointer; player_state = next_state; inventory = next_inventory;
            statistics_cache.clear();
            avatar_requests.clear();
            context.fetch_add(1, std::memory_order_release);
        }
        if (!notify_function) notify_function = FunctionFor("inventory-notify", "ClientInventoryNotifyMessage", false).pointer;
    }
    SnapshotProbe ProbeSnapshot() {
        GameOnly();
        AnomalyNteSessionSnapshotV1 current{sizeof(current)};
        const auto status = session->snapshot(session->user, &current);
        if (status.code == ANOMALY_STATUS_V1_UNAVAILABLE || status.code == ANOMALY_STATUS_V1_NOT_FOUND)
            throw NotReady(StatusDetail(status, "Probe game session"));
        Check(status, "Probe game session");
        if (observed_session && (observed_session->sequence != current.sequence || observed_session->state != current.state ||
            !Same(observed_session->world, current.world))) context.fetch_add(1, std::memory_order_release);
        observed_session = current;
        if (current.state != ANOMALY_NTE_SESSION_V1_WORLD_READY) throw NotReady("The game session is not yet ready");
        EnsureContext();
        return {{binding_generation, context.load(std::memory_order_acquire), current.sequence, current.state,
            current.world.id, current.world.generation, pawn_handle.id, pawn_handle.generation,
            controller_handle.id, controller_handle.generation, state_handle.id, state_handle.generation, inventory},
            notifications.load(std::memory_order_acquire), AccountIdentity()};
    }
    std::vector<std::uintptr_t> SparseEntries(std::uintptr_t map, std::uint32_t stride) const {
        const auto header = Block(map, V("mapSize"));
        const auto data = Extract<std::uintptr_t>(header, "mapData");
        const auto count = Extract<std::int32_t>(header, "mapNum"), capacity = Extract<std::int32_t>(header, "mapMax");
        const auto free = Extract<std::int32_t>(header, "mapFreeCount");
        const auto bits = Extract<std::int32_t>(header, "mapBitCount"), max_bits = Extract<std::int32_t>(header, "mapBitCapacity");
        if (count < 0 || capacity < count || free < 0 || free > count || count > 1000000 || bits < count || max_bits < bits)
            Fail("Invalid sparse inventory map");
        if (!count) return {};
        if (!data || !stride) Fail("Sparse inventory data is missing");
        const auto heap = Extract<std::uintptr_t>(header, "mapHeapBits");
        if (!heap && static_cast<std::uint32_t>(bits) > V("mapInlineCapacity")) Fail("Sparse inventory allocation flags are missing");
        const auto flag_bytes = static_cast<std::size_t>((bits + 31) / 32) * sizeof(std::uint32_t);
        const auto flags = Block(heap ? heap : map + V("mapInlineBits"), flag_bytes);
        std::vector<std::uintptr_t> entries; entries.reserve(static_cast<std::size_t>(count - free));
        for (std::int32_t index{}; index < count; ++index) {
            if ((flags[static_cast<std::size_t>(index) / 8U] & (1U << (index % 8))) != 0)
                entries.push_back(data + static_cast<std::size_t>(index) * stride);
        }
        if (entries.size() != static_cast<std::size_t>(count - free)) Fail("Inventory allocation counts disagree");
        return entries;
    }
    template<class Visitor> void VisitSparse(std::uintptr_t map, std::uint32_t stride, Visitor&& visitor) const {
        const auto entries = SparseEntries(map, stride);
        // These slots share one TArray allocation. Copy bounded contiguous spans,
        // keeping the allocation bitmap authoritative when there are holes.
        constexpr std::size_t maximum_copy = 64U * 1024U;
        if (!stride || stride > maximum_copy) Fail("Unsupported sparse record size");
        for (std::size_t first{}; first < entries.size();) {
            std::size_t end = first + 1;
            while (end < entries.size() && entries[end] - entries[first] <= maximum_copy - stride) ++end;
            const auto bytes = Block(entries[first], entries[end - 1] - entries[first] + stride);
            for (auto index = first; index < end; ++index)
                visitor(std::span<const std::uint8_t>(bytes).subspan(entries[index] - entries[first], stride));
            first = end;
        }
    }
    Array CheckedArray(Array array, std::size_t maximum) const {
        if (array.count < 0 || array.capacity < array.count || static_cast<std::size_t>(array.count) > maximum || (array.count && !array.data))
            Fail("Invalid inventory array");
        return array;
    }
    void PrepareStatics() {
        std::uintptr_t game_data{};
        const auto function = FunctionFor("get-game-data", "GetGameData", false);
        const auto receiver = Exact("/Script/HTGame.Default__HTGameData").first;
        Invoke(receiver, function, &game_data, sizeof(game_data));
        if (!game_data) throw NotReady("Game item definitions are not ready");
        if (!Derived(Field<std::uintptr_t>(game_data, "objectClass"), Class("HTGameData")))
            Fail("Game item definitions have an unsupported type");
        const auto character_type = Class("CharacterDataAsset");
        if (!static_schema_valid) {
            MemoryProperty(Class("HTGameData"), "ItemDataTable", "ObjectProperty", "gameDataItemTable", sizeof(std::uintptr_t));
            MemoryProperty(Class("HTGameData"), "CharacterDataAsset", "ObjectProperty", "gameDataCharacterAsset", sizeof(std::uintptr_t));
            MemoryProperty(character_type, "EquipmentDataTable", "ObjectProperty", "characterEquipmentTable", sizeof(std::uintptr_t));
            static_schema_valid = true;
        }
        const auto table = Field<std::uintptr_t>(game_data, "gameDataItemTable");
        if (!table) throw NotReady("Game item table is not ready");
        HandleFor(table);
        const auto character_asset = Field<std::uintptr_t>(game_data, "gameDataCharacterAsset");
        if (!character_asset) throw NotReady("Character item definitions are not ready");
        if (!Derived(Field<std::uintptr_t>(character_asset, "objectClass"), character_type))
            Fail("Character data asset has an unsupported type");
        const auto equipment = Field<std::uintptr_t>(character_asset, "characterEquipmentTable");
        if (!equipment) throw NotReady("Equipment item table is unavailable");
        HandleFor(equipment);
        current_game_data = game_data; current_character_asset = character_asset;
        if (table == item_table && equipment == equipment_table) return;
        const auto row_type = Field<std::uintptr_t>(table, "tableRowStruct");
        if (!row_type || ObjectName(row_type) != "StaticItemData") Fail("Unsupported game item table row type");
        StructSize(row_type, "staticItemSize");
        MemoryProperty(row_type, "ItemName", "TextProperty", "staticItemName", 16);
        MemoryProperty(row_type, "ItemQuality", "EnumProperty", "staticItemQuality", 1);
        MemoryProperty(row_type, "ElementData", "StructProperty", "staticItemElement", 16, "InstancedStruct");
        const auto equip_type = Class("EquipmentItemData"); StructSize(equip_type, "equipmentDataSize");
        MemoryProperty(equip_type, "EquipmentGeometryType", "EnumProperty", "equipmentGeometry", 1);
        MemoryProperty(equip_type, "IsCore", "BoolProperty", "equipmentIsCore", 1);
        MemoryProperty(equip_type, "MaxStrengthenLevel", "IntProperty", "equipmentMaxLevel", sizeof(std::int32_t));
        MemoryProperty(equip_type, "OwnGridNum", "NameProperty", "equipmentGridCount", sizeof(std::uint64_t));
        MemoryProperty(equip_type, "SuitPackID", "NameProperty", "equipmentSuit", sizeof(std::uint64_t));
        std::map<std::uint64_t, std::uintptr_t> rows;
        VisitSparse(table + V("tableRowMap"), V("tableStride"), [&](auto entry) {
            rows.emplace(ExtractAt<std::uint64_t>(entry, 0), Extract<std::uintptr_t>(entry, "tableValue"));
        });
        const auto equipment_row_type = Field<std::uintptr_t>(equipment, "tableRowStruct");
        if (equipment_row_type != row_type) Fail("Equipment and item tables have incompatible row contracts");
        std::map<std::uint64_t, std::uintptr_t> equipment_definitions;
        VisitSparse(equipment + V("tableRowMap"), V("tableStride"), [&](auto entry) {
            equipment_definitions.emplace(ExtractAt<std::uint64_t>(entry, 0), Extract<std::uintptr_t>(entry, "tableValue"));
        });
        item_table = table; equipment_table = equipment;
        static_rows = std::move(rows); equipment_rows = std::move(equipment_definitions); metadata.clear(); statistics_cache.clear();
        item_images.clear(); equipment_images.clear(); appearance_images.clear();
        appearance_rows.clear(); appearance_order.clear(); default_appearances.clear(); appearance_table = 0;
    }
    std::string DisplayName(std::uint64_t item_id, bool equipment = false) const {
        auto result = Name(item_id);
        const auto& rows = equipment ? equipment_rows : static_rows;
        const auto found = rows.find(item_id);
        if (found == rows.end() || !found->second) return result;
        if (names->struct_size >= offsetof(AnomalyUe5NamesServiceV1, resolve_ftext_utf8) + sizeof(names->resolve_ftext_utf8) && names->resolve_ftext_utf8) {
            std::array<char, 2048> text{}; std::size_t size = text.size();
            if (Succeeded(names->resolve_ftext_utf8(names->user, found->second + V("staticItemName"), text.data(), &size)) && size && size <= text.size())
                result.assign(text.data(), size - 1);
        }
        return result;
    }
    const Metadata& Meta(std::uint64_t item_id) {
        if (const auto found = metadata.find(item_id); found != metadata.end()) return found->second;
        const auto found = equipment_rows.find(item_id);
        if (found == equipment_rows.end() || !found->second)
            Fail("Equipment item definition is missing: " + Name(item_id) +
                 " (equipment table rows=" + std::to_string(equipment_rows.size()) + ")");
        const auto row = found->second;
        const auto element = row + V("staticItemElement");
        const auto type = Field<std::uintptr_t>(element, "instanceStructType");
        const auto data = Field<std::uintptr_t>(element, "instanceStructMemory");
        if (type != Class("EquipmentItemData") || !data) Fail("Equipment item definition has an unsupported element type");
        Metadata result;
        result.row = row; result.data = data;
        result.name = DisplayName(item_id, true);
        constexpr std::array<std::string_view, 7> qualities{"White", "Green", "Blue", "Purple", "Gold", "Red", "Unknown"};
        const auto quality = Field<std::uint8_t>(row, "staticItemQuality");
        result.quality = quality < qualities.size() ? qualities[quality] : "Unknown";
        const auto geometry = Field<std::uint8_t>(data, "equipmentGeometry");
        const bool is_core = Field<std::uint8_t>(data, "equipmentIsCore") != 0;
        result.kind = is_core ? "core" : "module";
        result.max_level = Field<std::int32_t>(data, "equipmentMaxLevel");
        if (!is_core) {
            const auto grid_name = Name(Field<std::uint64_t>(data, "equipmentGridCount"));
            const auto parsed = std::from_chars(grid_name.data(), grid_name.data() + grid_name.size(), result.grid_count);
            if (parsed.ec != std::errc{} || parsed.ptr != grid_name.data() + grid_name.size()) result.grid_count = 0;
        }
        if (is_core) {
            const auto suit = Field<std::uint64_t>(data, "equipmentSuit");
            if (suit != 0) result.suit = Name(suit);
        }
        if (is_core) result.shape = "TAPE_15";
        else {
            auto [shape, cells] = Shape(geometry);
            result.shape = std::move(shape); result.cells = std::move(cells);
        }
        return metadata.emplace(item_id, std::move(result)).first->second;
    }
    void PrepareImageLayout() {
        if (!image_layout) image_layout = catalog_data::DescribeImagePaths(*this);
    }
    Json ItemImages(std::uintptr_t row) {
        Json result{{"icon_asset_path", nullptr}, {"icon_small_asset_path", nullptr}, {"icon_big_asset_path", nullptr},
            {"image_metadata_complete", false}};
        if (!image_layout || !row) return result;
        if (const auto cached = item_images.find(row); cached != item_images.end()) return cached->second;
        constexpr std::array fields{"icon_asset_path", "icon_small_asset_path", "icon_big_asset_path"};
        for (std::size_t index{}; index < fields.size(); ++index)
            result[fields[index]] = catalog_data::ReadImagePath(*this, *image_layout, row + image_layout->item_icons[index]);
        result["image_metadata_complete"] = true;
        return item_images.emplace(row, std::move(result)).first->second;
    }
    Json EquipmentImages(const Metadata& meta) {
        auto result = ItemImages(meta.row);
        result["plan_icon_asset_path"] = nullptr;
        if (!image_layout) return result;
        if (const auto cached = equipment_images.find(meta.row); cached != equipment_images.end()) return cached->second;
        result["plan_icon_asset_path"] = catalog_data::ReadImagePath(*this, *image_layout, meta.data + image_layout->plan_icon);
        return equipment_images.emplace(meta.row, std::move(result)).first->second;
    }
    Json AppearanceImages(std::uintptr_t row) {
        Json result{{"head_icon_asset_path", nullptr}, {"head_icon_big_asset_path", nullptr},
            {"portrait_asset_path", nullptr}, {"small_portrait_asset_path", nullptr}};
        if (!image_layout || !row) return result;
        if (const auto cached = appearance_images.find(row); cached != appearance_images.end()) return cached->second;
        constexpr std::array fields{"head_icon_asset_path", "head_icon_big_asset_path", "portrait_asset_path", "small_portrait_asset_path"};
        for (std::size_t index{}; index < fields.size(); ++index)
            result[fields[index]] = catalog_data::ReadImagePath(*this, *image_layout, row + image_layout->appearance_icons[index]);
        return appearance_images.emplace(row, std::move(result)).first->second;
    }
    void PrepareAppearanceImages() {
        PrepareImageLayout();
        const auto table = ReadValue<std::uintptr_t>(current_character_asset + image_layout->appearance_table);
        if (!table) throw NotReady("The appearance image table is not yet loaded");
        if (table == appearance_table) return;
        HandleFor(table);
        if (!Derived(Field<std::uintptr_t>(table, "objectClass"), Exact("/Script/Engine.DataTable").second) ||
            Field<std::uintptr_t>(table, "tableRowStruct") != Class("StaticAppearanceData"))
            Fail("The appearance image table row schema is unsupported");
        std::map<std::uint64_t, Appearance> rows;
        std::vector<std::uint64_t> order;
        VisitSparse(table + V("tableRowMap"), V("tableStride"), [&](auto entry) {
            const auto id = ExtractAt<std::uint64_t>(entry, 0);
            const auto row = Extract<std::uintptr_t>(entry, "tableValue");
            const auto bytes = Block(row, image_layout->appearance_size);
            const auto kind = EnumName("EAppearanceType", ExtractAt<std::uint8_t>(bytes, image_layout->appearance_type));
            // Native UEnum entries may retain their owning enum qualification.
            const bool fashion = kind == "Fashion" || kind == "EAppearanceType::Fashion";
            const auto character = ExtractAt<std::uint64_t>(bytes, image_layout->appearance_character);
            const bool is_default = ExtractAt<std::uint8_t>(bytes, image_layout->appearance_default) != 0;
            Appearance appearance{row, character, fashion, is_default, {}};
            if (fashion && is_default) VisitArray(ExtractAt<Array>(bytes, image_layout->appearance_exclusions), sizeof(std::uint64_t), 1024, [&](auto value) {
                appearance.exclusions.push_back(ExtractAt<std::uint64_t>(value, 0));
            });
            if (!id || !rows.emplace(id, std::move(appearance)).second)
                Fail("The appearance image catalog identity is missing or duplicated");
            order.push_back(id);
        });
        appearance_rows = std::move(rows); appearance_order = std::move(order); default_appearances.clear();
        appearance_images.clear(); appearance_table = table;
    }
    std::uint64_t DefaultFashion(std::uint64_t character) {
        if (const auto cached = default_appearances.find(character); cached != default_appearances.end()) return cached->second;
        std::uint64_t selected{};
        // Matches the native CharacterItem appearance getter's default branch:
        // same type, IsDefault, no exclusion, CharacterID unset or matching.
        // Its DataTable visitor retains the last match in sparse iteration order.
        for (const auto id : appearance_order) {
            const auto& row = appearance_rows.at(id);
            if (!row.fashion || !row.is_default || (row.character && row.character != character) ||
                std::find(row.exclusions.begin(), row.exclusions.end(), character) != row.exclusions.end()) continue;
            selected = id;
        }
        return default_appearances.emplace(character, selected).first->second;
    }
    Json CurrentCharacterImages(std::span<const std::uint8_t> character, bool appearance_ready, std::uint64_t* selected_fashion = nullptr) {
        if (selected_fashion) *selected_fashion = 0;
        Json result = AppearanceImages(0);
        result.update({{"appearance_equip_ids", nullptr}, {"fashion_id", nullptr}, {"appearance_character_id", nullptr},
            {"appearance_complete", false}, {"appearance_source", nullptr},
            {"appearance_equip_source", "HTCharacterItem.AppearanceEquipData"}});
        if (!image_layout) return result;
        result["appearance_equip_ids"] = Json::array();
        std::uint64_t fashion{};
        VisitArray(ExtractAt<Array>(character, image_layout->character_appearances), sizeof(std::uint64_t), 1024, [&](auto entry) {
            const auto id = ExtractAt<std::uint64_t>(entry, 0);
            result["appearance_equip_ids"].push_back(OptionalName(id));
            if (!id || !appearance_ready) return;
            const auto found = appearance_rows.find(id);
            if (found != appearance_rows.end() && found->second.fashion && !fashion) fashion = id;
        });
        if (!appearance_ready) return result;
        const bool explicit_fashion = fashion != 0;
        if (!fashion) fashion = DefaultFashion(Extract<std::uint64_t>(character, "itemId"));
        if (!fashion) return result;
        const auto& selected = appearance_rows.at(fashion);
        result.update(AppearanceImages(selected.row));
        result["fashion_id"] = Name(fashion);
        result["appearance_character_id"] = OptionalName(selected.character);
        result["appearance_complete"] = true;
        result["appearance_source"] = explicit_fashion ? "equipped_fashion" : "default_fashion";
        if (selected_fashion) *selected_fashion = fashion;
        return result;
    }
    Json DefaultCharacterImages(std::uint64_t character) {
        Json result{{"default_appearance_id", nullptr}, {"default_head_icon_asset_path", nullptr},
            {"default_head_icon_big_asset_path", nullptr}, {"default_portrait_asset_path", nullptr},
            {"default_small_portrait_asset_path", nullptr}, {"default_appearance_complete", false}};
        const auto selected = DefaultFashion(character);
        if (!selected) return result;
        const auto& row = appearance_rows.at(selected);
        const auto images = AppearanceImages(row.row);
        for (const auto& [key, value] : images.items()) result["default_" + key] = value;
        result["default_appearance_id"] = Name(selected);
        result["default_appearance_complete"] = true;
        return result;
    }
    std::vector<std::uint8_t> AvatarCharacter(std::uint64_t role_id) {
        std::vector<std::uint8_t> selected;
        const auto type = Class("HTCharacterItem");
        for (const auto container : SparseEntries(inventory + V("inventoryContainerMap"), V("containerStride"))) {
            VisitSparse(container + V("containerValue"), V("itemMapStride"), [&](auto entry) {
                const auto pointer = Extract<std::uintptr_t>(entry, "itemMapValue");
                if (!pointer || Field<std::uintptr_t>(pointer, "objectClass") != type) return;
                auto bytes = Block(pointer, V("characterSize"));
                if (Extract<std::uint64_t>(bytes, "itemId") != role_id || Extract<std::uint8_t>(bytes, "itemTemporary") ||
                    Extract<std::int64_t>(bytes, "itemAmount") <= 0) return;
                const auto uid = ExtractAt<Uid>(entry, 0);
                if (!uid.Valid() || Extract<Uid>(bytes, "itemUid") != uid)
                    throw NotReady("The avatar role inventory identity is not yet consistent");
                if (!selected.empty()) Fail("The avatar role inventory identity is ambiguous");
                selected = std::move(bytes);
            });
        }
        if (selected.empty()) Fail("The requested avatar role is not present in the current inventory");
        return selected;
    }
    Json AvatarMetadata() {
        EnsureContext(); PrepareStatics(); PrepareAppearanceImages();
        const auto type = Class("HTCharacterItem");
        const auto& definitions = CatalogTableFor("characters");
        std::set<std::uint64_t> observed;
        Json characters = Json::array();
        // One inventory traversal, restricted to current character appearance.
        // No statistics, equipment snapshots, native models or RPCs are read.
        for (const auto container : SparseEntries(inventory + V("inventoryContainerMap"), V("containerStride"))) {
            VisitSparse(container + V("containerValue"), V("itemMapStride"), [&](auto entry) {
                const auto pointer = Extract<std::uintptr_t>(entry, "itemMapValue");
                if (!pointer || Field<std::uintptr_t>(pointer, "objectClass") != type) return;
                const auto bytes = Block(pointer, V("characterSize"));
                if (Extract<std::uint8_t>(bytes, "itemTemporary") || Extract<std::int64_t>(bytes, "itemAmount") <= 0) return;
                const auto uid = ExtractAt<Uid>(entry, 0);
                if (!uid.Valid() || Extract<Uid>(bytes, "itemUid") != uid)
                    throw NotReady("The avatar role inventory identity is not yet consistent");
                const auto id = Extract<std::uint64_t>(bytes, "itemId");
                if (!observed.insert(id).second) Fail("The avatar role inventory identity is ambiguous");
                if (std::none_of(definitions.rows.begin(), definitions.rows.end(), [&](const auto& row) { return row.first == id; }))
                    Fail("The avatar role has no official character definition");
                Json character{{"item_id", Name(id)}};
                character.update(CurrentCharacterImages(bytes, true));
                characters.push_back(std::move(character));
            });
        }
        if (characters.empty()) throw NotReady("The avatar role inventory is not yet ready");
        return characters;
    }
    AvatarExportResult ExportAvatar(std::string_view role_id, std::string_view expected_asset_path, std::wstring_view filename) {
        EnsureContext();
        if (filename.empty() || filename.find(L'\0') != std::wstring_view::npos || expected_asset_path.empty())
            Fail("A controlled avatar output path and current head asset are required");
        auto request = avatar_requests.find(filename);
        if (request != avatar_requests.end()) {
            if (model_data::IdentifierKey(request->second.role) != model_data::IdentifierKey(role_id) ||
                request->second.asset != expected_asset_path)
                Fail("The avatar output path is already bound to a different role or head asset");
            if (request->second.submitted) return AvatarExportResult::exported;
        } else {
            PrepareStatics(); PrepareAppearanceImages();
            const auto definition = NamedCatalogRow("characters", role_id);
            if (!definition) Fail("The requested avatar role has no official identity");
            const auto character = AvatarCharacter(definition.id);
            std::uint64_t fashion{};
            const auto images = CurrentCharacterImages(character, true, &fashion);
            if (!fashion || !images.value("appearance_complete", false))
                Fail("The requested role has no verified current fashion head asset");
            std::optional<std::size_t> variant;
            constexpr std::array fields{"head_icon_asset_path", "head_icon_big_asset_path"};
            for (std::size_t index{}; index < fields.size(); ++index)
                if (images.at(fields[index]).is_string() && images.at(fields[index]).get_ref<const std::string&>() == expected_asset_path)
                    variant = index;
            if (!variant) Fail("The requested head asset no longer matches the role's current fashion");
            // This request owns a verified source snapshot. Async loading polls
            // reuse it instead of rescanning character inventory every frame.
            request = avatar_requests.emplace(std::wstring(filename), AvatarRequest{
                Name(definition.id), std::string(expected_asset_path), false, false, {}, fashion, *variant}).first;
        }
        Handle texture{};
        const auto status = objects->find_exact(objects->user, StringView(expected_asset_path), &texture);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            if (!request->second.loading_requested) {
                if (image_layout->soft_reference_size != sizeof(avatar_export::SoftObjectReference))
                    Fail("The current avatar soft reference does not match the asynchronous loading ABI");
                avatar_export::SoftObjectReference asset;
                const auto reference = appearance_rows.at(request->second.fashion).row + image_layout->appearance_icons[request->second.variant];
                ReadBytes(reference, asset.bytes.data(), asset.bytes.size());
                // The controlled output filename carries the root's unique job identity.
                std::uint32_t uuid = 2166136261U;
                for (const auto character_value : filename) {
                    uuid ^= static_cast<std::uint16_t>(character_value); uuid *= 16777619U;
                }
                request->second.loading_requested = true;
                avatar_export::RequestLoad(*this, controller, asset, controller, static_cast<std::int32_t>(uuid & INT32_MAX));
            }
            return AvatarExportResult::waiting;
        }
        Check(status, "Find the requested avatar head texture");
        const auto source = Pointer(texture);
        const auto pending_flags = V("avatarPendingObjectFlags");
        if (!pending_flags) Fail("The avatar asynchronous loading flag contract is unavailable");
        if ((Field<std::uint32_t>(source, "objectFlags") & pending_flags) != 0)
            return AvatarExportResult::waiting;
        if (!Derived(Field<std::uintptr_t>(source, "objectClass"), Exact("/Script/Engine.Texture2D").second))
            Fail("The current fashion head asset is not a Texture2D");
        const auto source_state = AvatarTextureReadiness(source);
        std::uintptr_t target{};
        AvatarTextureState target_state;
        if (!request->second.target.id) {
            target = avatar_export::CreateTarget(*this, controller);
            request->second.target = HandleFor(target);
            target_state = AvatarTextureReadiness(target);
        } else {
            target = Pointer(request->second.target);
            target_state = AvatarTextureReadiness(target);
        }
        if (!source_state.rhi_ready || !target_state.rhi_ready) return AvatarExportResult::waiting;
        static_cast<void>(avatar_export::Submit(*this, source, controller, filename, target));
        request->second.submitted = true;
        return AvatarExportResult::exported;
    }
    void ReleaseAvatar(std::wstring_view filename) {
        GameOnly();
        const auto request = avatar_requests.find(filename);
        if (request == avatar_requests.end()) return;
        const auto target = request->second.target;
        avatar_requests.erase(request);
        if (!target.id) return;
        std::uintptr_t pointer{};
        try { pointer = Pointer(target); }
        catch (...) { return; } // Ordinary UE GC may already have retired this transient RT.
        avatar_export::Release(*this, pointer);
    }
    std::pair<std::string, Json> Shape(std::uint32_t geometry) const {
            struct Shape { const char* geometry; const char* mask; const char* id; const char* anchor_row; const char* anchor_column; };
            constexpr std::array shapes{
                Shape{"geometryHen2", "maskHen2", "H_2", "anchorHen2Row", "anchorHen2Column"},
                Shape{"geometryShu2", "maskShu2", "V_2", "anchorShu2Row", "anchorShu2Column"},
                Shape{"geometryHen3", "maskHen3", "H_3", "anchorHen3Row", "anchorHen3Column"},
                Shape{"geometryShu3", "maskShu3", "V_3", "anchorShu3Row", "anchorShu3Column"},
                Shape{"geometryHen4", "maskHen4", "H_4", "anchorHen4Row", "anchorHen4Column"},
                Shape{"geometryShu4", "maskShu4", "V_4", "anchorShu4Row", "anchorShu4Column"},
                Shape{"geometryCorner1", "maskCorner1", "L_3_BL", "anchorCorner1Row", "anchorCorner1Column"},
                Shape{"geometryCorner2", "maskCorner2", "L_3_TL", "anchorCorner2Row", "anchorCorner2Column"},
                Shape{"geometryCorner3", "maskCorner3", "L_3_TR", "anchorCorner3Row", "anchorCorner3Column"},
                Shape{"geometryCorner4", "maskCorner4", "L_3_BR", "anchorCorner4Row", "anchorCorner4Column"},
                Shape{"geometryZ3", "maskZ3", "Trap_4_H", "anchorZ3Row", "anchorZ3Column"},
                Shape{"geometryZ4", "maskZ4", "Trap_4_V", "anchorZ4Row", "anchorZ4Column"}};
            for (const auto& shape : shapes) if (geometry == V(shape.geometry)) {
                Json cells = Json::array(); const auto mask = V(shape.mask), width = V("gridMaximum");
                const auto anchor_row = static_cast<std::int32_t>(V(shape.anchor_row));
                const auto anchor_column = static_cast<std::int32_t>(V(shape.anchor_column));
                for (std::uint32_t bit{}; bit < width * width; ++bit)
                    if ((mask & (1U << bit)) != 0) cells.push_back({
                        static_cast<std::int32_t>(bit / width) - anchor_row,
                        static_cast<std::int32_t>(bit % width) - anchor_column});
                return {shape.id, std::move(cells)};
            }
        return {{}, Json::array()};
    }
    void PrepareStatistics() {
        if (!statistics_schema_checked) {
            statistics_schema_checked = true;
            MemoryProperty(Class("HTGameData"), "AttributeStaticDataTable", "ObjectProperty", "gameDataAttributeTable", sizeof(std::uintptr_t));
            const auto character = Class("CharacterDataAsset");
            MemoryProperty(character, "EquipmentBaseAttributeCurveTable", "ObjectProperty", "characterCurveTable", sizeof(std::uintptr_t));
            MemoryProperty(character, "EquipmentSuitDataTable", "ObjectProperty", "characterSuitTable", sizeof(std::uintptr_t));
            const auto attributes = Class("AttributeStaticData"); StructSize(attributes, "attributeDataSize");
            MemoryProperty(attributes, "AttributeText", "TextProperty", "attributeText", 16);
            MemoryProperty(attributes, "bShowPercent", "BoolProperty", "attributePercent", 1);
            MemoryProperty(attributes, "EquipmentRandomAttributeName", "TextProperty", "attributeRandomName", 16);
            const auto suits = Class("EquipmentSuitStaticData"); StructSize(suits, "suitDataSize");
            MemoryProperty(suits, "SuitGeometryCondition", "ArrayProperty", "suitGeometry", sizeof(Array));
            suit_title_field = catalog_data::Field(*this, suits, "SuitTitle", "TextProperty", 16);
            statistics_schema_valid = true;
        }
        if (!statistics_schema_valid) return;
        const auto attributes = Field<std::uintptr_t>(current_game_data, "gameDataAttributeTable");
        const auto curves = Field<std::uintptr_t>(current_character_asset, "characterCurveTable");
        const auto suits = Field<std::uintptr_t>(current_character_asset, "characterSuitTable");
        if (attributes == attribute_table && curves == curve_table && suits == suit_table) return;
        attribute_table = 0; curve_table = 0; suit_table = 0;
        attribute_rows.clear(); suit_rows.clear(); attribute_definitions.clear(); curve_values.clear(); curve_rows.clear(); suit_definitions.clear(); suit_names.clear(); statistics_cache.clear();
        if (!attributes || !curves || !suits) throw NotReady("Equipment attribute definitions are not ready");
        HandleFor(attributes); HandleFor(curves); HandleFor(suits);
        if (Field<std::uintptr_t>(attributes, "tableRowStruct") != Class("AttributeStaticData") ||
            Field<std::uintptr_t>(suits, "tableRowStruct") != Class("EquipmentSuitStaticData") ||
            !Derived(Field<std::uintptr_t>(curves, "objectClass"), Exact("/Script/Engine.CurveTable").second))
            Fail("Equipment attribute tables have unsupported contracts");
        std::map<std::uint64_t, std::uintptr_t> attribute_entries, suit_entries;
        VisitSparse(attributes + V("tableRowMap"), V("tableStride"), [&](auto entry) {
            attribute_entries.emplace(ExtractAt<std::uint64_t>(entry, 0), Extract<std::uintptr_t>(entry, "tableValue"));
        });
        VisitSparse(suits + V("tableRowMap"), V("tableStride"), [&](auto entry) {
            suit_entries.emplace(ExtractAt<std::uint64_t>(entry, 0), Extract<std::uintptr_t>(entry, "tableValue"));
        });
        attribute_rows = std::move(attribute_entries); suit_rows = std::move(suit_entries);
        attribute_table = attributes; curve_table = curves; suit_table = suits;
        statistics_diagnostic.clear();
    }
    std::string Text(std::uintptr_t address, const std::string& fallback) const {
        if (!HasTable(names, offsetof(AnomalyUe5NamesServiceV1, resolve_ftext_utf8) + sizeof(names->resolve_ftext_utf8)) || !names->resolve_ftext_utf8) return fallback;
        std::array<char, 2048> buffer{}; std::size_t size = buffer.size();
        if (!Succeeded(names->resolve_ftext_utf8(names->user, address, buffer.data(), &size)) || size < 2 || size > buffer.size()) return fallback;
        return {buffer.data(), size - 1};
    }
    const AttributeDefinition* Attribute(std::uint64_t id) {
        if (const auto found = attribute_definitions.find(id); found != attribute_definitions.end()) return &found->second;
        const auto row = attribute_rows.find(id);
        if (row == attribute_rows.end() || !row->second) return nullptr;
        AttributeDefinition result;
        result.percent = Field<std::uint8_t>(row->second, "attributePercent") != 0;
        result.name = Text(row->second + V("attributeText"), Name(id));
        return &attribute_definitions.emplace(id, std::move(result)).first->second;
    }
    std::optional<double> CurveValue(const std::string& row, std::int32_t level) {
        const auto key = std::make_pair(row, level);
        if (const auto found = curve_values.find(key); found != curve_values.end()) return found->second;
        std::optional<double> value;
        try {
            if (!curve_table) Fail("Equipment curves are unavailable");
            if (!curve_binding_checked) {
                curve_binding_checked = true;
                const auto function = Exact("/Script/Engine.DataTableFunctionLibrary:EvaluateCurveTableRow").second;
                const auto owner = Exact("/Script/Engine.DataTableFunctionLibrary").second;
                const auto flags = Field<std::uint32_t>(function, "functionFlags");
                const auto required = V("functionNativeFlag") | V("functionStaticFlag");
                if (ObjectName(Field<std::uintptr_t>(function, "objectClass")) != "Function" ||
                    Field<std::uintptr_t>(function, "objectOuter") != owner || (flags & required) != required ||
                    Field<std::uint16_t>(function, "functionParameterSize") != sizeof(CurveParameters) ||
                    !profile.MatchesEntry(core, "evaluate-equipment-curve", Field<std::uintptr_t>(function, "functionExec")))
                    Fail("Equipment curve getter does not match the active Profile");
                Parameter(function, "CurveTable", "ObjectProperty", offsetof(CurveParameters, table), sizeof(std::uintptr_t));
                Parameter(function, "RowName", "NameProperty", offsetof(CurveParameters, row), sizeof(std::uint64_t));
                Parameter(function, "InXY", "FloatProperty", offsetof(CurveParameters, level), sizeof(float));
                Parameter(function, "OutResult", "EnumProperty", offsetof(CurveParameters, result), sizeof(std::uint8_t));
                Parameter(function, "OutXY", "FloatProperty", offsetof(CurveParameters, value), sizeof(float));
                Parameter(function, "ContextString", "StrProperty", offsetof(CurveParameters, context), sizeof(Array));
                curve_receiver = Exact("/Script/Engine.Default__DataTableFunctionLibrary").second;
                curve_function = function; curve_binding_valid = true;
            }
            if (!curve_binding_valid) Fail("Equipment curve getter validation failed");
            if (curve_rows.empty()) {
                // FindCurve in the validated evaluator reads this native RowMap.
                // Preserve the table's complete FName rather than searching a
                // bounded suffix of the global name pool for a constructed name.
                std::map<std::string, std::uint64_t, std::less<>> entries;
                VisitSparse(curve_table + V("curveRowMap"), V("tableStride"), [&](auto entry) {
                    const auto id = ExtractAt<std::uint64_t>(entry, 0);
                    if (!id || !Extract<std::uintptr_t>(entry, "tableValue") || !entries.emplace(Name(id), id).second)
                        Fail("Equipment curve table has an invalid row key");
                });
                curve_rows = std::move(entries);
            }
            const auto found = curve_rows.find(row);
            if (found == curve_rows.end()) Fail("The official equipment curve row is missing: " + row);
            CurveParameters parameters;
            parameters.table = curve_table; parameters.row = found->second; parameters.level = static_cast<float>(level);
            // This is an engine-owned read-only evaluator. Refresh weak object
            // serials just before each invocation, as with the verified RPC bridge.
            Check(events->invoke(events->user, HandleFor(curve_receiver), HandleFor(curve_function), &parameters, sizeof(parameters)), "Evaluate equipment curve");
            if (parameters.result != V("curveResultFound") || !std::isfinite(parameters.value))
                Fail("The official equipment curve row is missing or invalid");
            value = parameters.value;
        } catch (const std::exception& exception) {
            if (statistics_diagnostic.empty()) statistics_diagnostic = exception.what();
        }
        curve_values.emplace(key, value);
        return value;
    }
    std::string SuitName(const std::string& suit, const std::string& fallback = {}) {
        if (suit.empty() || !statistics_schema_valid || !suit_table) return fallback;
        auto found = suit_names.find(suit);
        if (found == suit_names.end()) {
            std::string name;
            for (const auto& [id, row] : suit_rows) if (Name(id) == suit) {
                name = Text(row + suit_title_field, {}); break;
            }
            found = suit_names.emplace(suit, std::move(name)).first;
        }
        return found->second.empty() ? fallback : found->second;
    }
    Json RecommendedCore(std::uint64_t character_id, const CatalogTable& plans) {
        const auto row = std::find_if(plans.rows.begin(), plans.rows.end(), [character_id](const auto& entry) { return entry.first == character_id; });
        if (row == plans.rows.end()) return nullptr;
        const auto core_id = ReadValue<std::uint64_t>(row->second + *plan_core_field);
        const auto& definition = Meta(core_id);
        if (definition.kind != "core" || definition.suit.empty()) Fail("The official character plan does not identify a cartridge set");
        return {{"item_id", Name(core_id)}, {"name", definition.name}, {"suit_id", definition.suit}, {"suit_name", SuitName(definition.suit, definition.name)}};
    }
    Json RequiredShapes(const Metadata& meta, bool& complete) {
        complete = false;
        if (meta.kind != "core") { complete = true; return Json::array(); }
        if (meta.suit.empty()) return Json::array();
        if (const auto cached = suit_definitions.find(meta.suit); cached != suit_definitions.end()) {
            complete = cached->second.second; return cached->second.first;
        }
        for (const auto& [id, row] : suit_rows) {
            if (Name(id) != meta.suit) continue;
            const auto geometry = CheckedArray(ReadValue<Array>(row + V("suitGeometry")), 64);
            if (!geometry.count) break;
            const auto values = Block(geometry.data, static_cast<std::size_t>(geometry.count));
            Json result = Json::array();
            for (const auto value : values) {
                auto [shape, cells] = Shape(value);
                if (shape.empty()) { result.clear(); break; }
                result.push_back(std::move(shape));
            }
            complete = result.size() == static_cast<std::size_t>(geometry.count);
            suit_definitions.emplace(meta.suit, std::make_pair(result, complete)); return result;
        }
        suit_definitions.emplace(meta.suit, std::make_pair(Json::array(), false));
        return Json::array();
    }
    Json Attributes(std::span<const std::uint8_t> records, std::map<std::uint64_t, std::string>& attribute_names, bool& complete) {
        complete = true;
        Json result = Json::array();
        const auto stride = V("randomStride");
        for (std::size_t offset{}; offset < records.size(); offset += stride) {
            const auto record = records.subspan(offset, stride);
            const auto id = Extract<std::uint64_t>(record, "randomName");
            auto found = attribute_names.find(id);
            if (found == attribute_names.end()) found = attribute_names.emplace(id, Name(id)).first;
            const auto& name = found->second;
            const auto value = Extract<float>(record, "randomValue");
            if (!std::isfinite(value)) Fail("Equipment attribute is not finite");
            const auto* definition = Attribute(id);
            if (!definition) complete = false;
            Json stat{{"id", name}, {"name", definition ? definition->name : name}, {"source", "sub"},
                {"raw_value", value}, {"value", nullptr}, {"percent", nullptr}};
            if (definition) {
                stat["percent"] = definition->percent;
                stat["value"] = value;
            }
            result.push_back(std::move(stat));
        }
        return result;
    }
    const Json& Statistics(std::span<const std::uint8_t> item, const Metadata& meta,
                    std::optional<std::int32_t> gold_core_level, std::map<std::uint64_t, std::string>& attribute_names) {
        const auto bases = CheckedArray(Extract<Array>(item, "itemRandomBase"), 16);
        auto records = bases.count ? Block(bases.data, static_cast<std::size_t>(bases.count) * sizeof(std::uint64_t)) : std::vector<std::uint8_t>{};
        const auto random_array = CheckedArray(Extract<Array>(item, "itemRandom"), 64);
        auto randoms = random_array.count ? Block(random_array.data, static_cast<std::size_t>(random_array.count) * V("randomStride")) : std::vector<std::uint8_t>{};
        const auto level = Extract<std::int32_t>(item, "equipmentLevel");
        const auto uid = Extract<Uid>(item, "itemUid");
        const auto item_id = Extract<std::uint64_t>(item, "itemId");
        // These bytes are copied from the current observation every time. Array
        // pointers alone cannot detect the server replacing a roll in place.
        if (const auto cached = statistics_cache.find(uid); cached != statistics_cache.end() &&
            cached->second.item_id == item_id && cached->second.level == level && cached->second.gold_core_level == gold_core_level &&
            cached->second.bases == records && cached->second.randoms == randoms) return cached->second.value;
        bool sub_complete{};
        auto sub = Attributes(randoms, attribute_names, sub_complete);
        Json main = Json::array();
        bool main_complete = bases.count == (meta.kind == "core" ? 1 : 2);
        const auto quality = meta.quality == "Gold" ? "ORANGE" : meta.quality == "Purple" ? "PURPLE" : meta.quality == "Blue" ? "BLUE" : "";
        const auto category = meta.kind == "core" ? "Core" : meta.grid_count > 0 ? std::to_string(meta.grid_count) : "";
        std::set<std::uint64_t> seen;
        for (std::int32_t index{}; index < bases.count; ++index) {
            const auto id = ExtractAt<std::uint64_t>(records, static_cast<std::size_t>(index) * sizeof(std::uint64_t));
            auto found_name = attribute_names.find(id);
            if (found_name == attribute_names.end()) found_name = attribute_names.emplace(id, Name(id)).first;
            const auto& name = found_name->second;
            const auto* definition = Attribute(id);
            std::optional<double> current, maximum, gold;
            if (definition && quality[0] && !category.empty() && level >= 0 && meta.max_level > 0) {
                // This exact row naming contract is also used by the reference
                // project's equipment_level_projection_service, not a stat guess.
                const auto prefix = name + "_" + category + "_ITEM_QUALITY_";
                current = CurveValue(prefix + quality, level);
                maximum = CurveValue(prefix + quality, meta.max_level);
                if (meta.kind == "core") {
                    if (meta.quality == "Gold") gold = maximum;
                    else if (gold_core_level) gold = CurveValue(prefix + "ORANGE", *gold_core_level);
                }
            }
            if (!definition || !current || !maximum || !seen.insert(id).second) main_complete = false;
            Json stat{{"id", name}, {"name", definition ? definition->name : name}, {"source", "main"},
                {"percent", nullptr}, {"raw_value", nullptr}, {"value", nullptr}, {"max_value", nullptr}, {"gold_max_value", nullptr}};
            if (definition) {
                stat["percent"] = definition->percent;
                if (current) { stat["raw_value"] = *current; stat["value"] = *current; }
                if (maximum) stat["max_value"] = *maximum;
                if (gold) stat["gold_max_value"] = *gold;
            }
            main.push_back(std::move(stat));
        }
        Json all = main;
        for (const auto& stat : sub) all.push_back(stat);
        Json result{{"main_stats", main}, {"sub_stats", std::move(sub)}, {"attributes", std::move(all)},
            {"main_stats_complete", main_complete}, {"sub_stats_complete", true}, {"sub_stats_values_complete", sub_complete},
            {"equipment_stats_complete", main_complete && sub_complete}, {"attributes_complete", false},
            {"attributes_scope", "equipment_main_and_sub"}};
        if (meta.kind == "core" && main.size() == 1) result["main_stat"] = main.front();
        bool shapes_complete{};
        result["required_shapes"] = RequiredShapes(meta, shapes_complete);
        result["required_shapes_complete"] = shapes_complete;
        return statistics_cache.insert_or_assign(uid, StatisticsCache{item_id, level, gold_core_level,
            std::move(records), std::move(randoms), std::move(result)}).first->second.value;
    }
    struct Location { Uid character; std::int32_t row{}, column{}; bool anchor{}; };
    void CharacterGrid(std::span<const std::uint8_t> character, Uid uid, Json& target, std::map<Uid, Location>& locations) const {
        Json allowed = Json::array();
        const auto rows = CheckedArray(Extract<Array>(character, "characterSlots"), 16);
        const auto row_size = V("rowSize"), slot_size = V("slotSize");
        const auto minimum = static_cast<std::int32_t>(V("gridMinimum")), maximum = static_cast<std::int32_t>(V("gridMaximum"));
        const auto row_records = rows.count ? Block(rows.data, static_cast<std::size_t>(rows.count) * row_size) : std::vector<std::uint8_t>{};
        for (std::int32_t r{}; r < rows.count; ++r) {
            const auto row_record = std::span<const std::uint8_t>(row_records).subspan(static_cast<std::size_t>(r) * row_size, row_size);
            const auto slots = CheckedArray(Extract<Array>(row_record, "rowSlots"), 16);
            const auto slot_records = slots.count ? Block(slots.data, static_cast<std::size_t>(slots.count) * slot_size) : std::vector<std::uint8_t>{};
            for (std::int32_t c{}; c < slots.count; ++c) {
                const auto slot = std::span<const std::uint8_t>(slot_records).subspan(static_cast<std::size_t>(c) * slot_size, slot_size);
                const auto row = Extract<std::int32_t>(slot, "slotRow"), column = Extract<std::int32_t>(slot, "slotColumn");
                const bool playable = row >= minimum && row <= maximum && column >= minimum && column <= maximum;
                if (playable && Extract<std::int32_t>(slot, "slotState") >= 0) allowed.push_back({row, column});
                const auto equipment = Extract<Uid>(slot, "slotUid");
                if (!equipment.Valid()) continue;
                const bool anchor = Extract<std::uint8_t>(slot, "slotAnchor") != 0;
                if (anchor && !playable) Fail("Equipped module anchor is outside the playable board");
                if (anchor) {
                    if (!locations.emplace(equipment, Location{uid, row, column, true}).second)
                        Fail("Equipment UID occupies multiple anchors");
                }
            }
        }
        target["allowed_cells"] = std::move(allowed);
        const auto cores = CheckedArray(Extract<Array>(character, "characterCoreSlots"), 16);
        const auto core_records = cores.count ? Block(cores.data, static_cast<std::size_t>(cores.count) * slot_size) : std::vector<std::uint8_t>{};
        for (std::int32_t i{}; i < cores.count; ++i) {
            const auto slot = std::span<const std::uint8_t>(core_records).subspan(static_cast<std::size_t>(i) * slot_size, slot_size);
            const auto equipment = Extract<Uid>(slot, "slotUid");
            if (equipment.Valid() && !locations.emplace(equipment, Location{uid}).second)
                Fail("Equipment UID occupies multiple roles");
        }
    }
    void ProgressionLayouts() {
        if (progression_layouts_valid) return;
        const auto character = Class("HTCharacterItem"), fork = Class("HTForkItem");
        StructSize(fork, "forkSize");
        MemoryProperty(character, "CharacterLevel", "IntProperty", "characterLevel", sizeof(std::int32_t));
        MemoryProperty(character, "BreakthroughLevel", "IntProperty", "characterBreakthrough", sizeof(std::int32_t));
        MemoryProperty(character, "AwakenLevel", "IntProperty", "characterAwakening", sizeof(std::int32_t));
        MemoryProperty(character, "EXP", "IntProperty", "characterExperience", sizeof(std::int32_t));
        MemoryProperty(character, "ForkItemNetId", "StructProperty", "characterFork", sizeof(Uid), "HTItemNetID");
        MemoryProperty(character, "AwakenEffectDataLists", "ArrayProperty", "characterAwakenEffects", sizeof(Array), "AwakenEffectData");
        MemoryProperty(character, "SkillLevelData", "ArrayProperty", "characterSkills", sizeof(Array), "SkillLevelData");
        MemoryProperty(character, "bUnSaved", "BoolProperty", "characterUnsaved", sizeof(std::uint8_t));
        const auto skill = Class("SkillLevelData"); StructSize(skill, "skillDataSize");
        MemoryProperty(skill, "SkillID", "NameProperty", "skillId", sizeof(std::uint64_t));
        MemoryProperty(skill, "SkillTag", "NameProperty", "skillTag", sizeof(std::uint64_t));
        MemoryProperty(skill, "SkillLevel", "IntProperty", "skillLevel", sizeof(std::int32_t));
        const auto awaken = Class("AwakenEffectData"); StructSize(awaken, "awakenDataSize");
        MemoryProperty(awaken, "Unlocked", "BoolProperty", "awakenUnlocked", sizeof(std::uint8_t));
        MemoryProperty(awaken, "EffectID", "NameProperty", "awakenId", sizeof(std::uint64_t));
        MemoryProperty(fork, "StrengthenLevel", "IntProperty", "forkLevel", sizeof(std::int32_t));
        MemoryProperty(fork, "BreakthroughLevel", "IntProperty", "forkBreakthrough", sizeof(std::int32_t));
        MemoryProperty(fork, "StarLevel", "IntProperty", "forkRefinement", sizeof(std::int32_t));
        MemoryProperty(fork, "EXP", "IntProperty", "forkExperience", sizeof(std::int32_t));
        MemoryProperty(fork, "IsLocked", "BoolProperty", "forkLocked", sizeof(std::uint8_t));
        MemoryProperty(fork, "CharacterNetID", "StructProperty", "forkCharacter", sizeof(Uid), "HTItemNetID");
        MemoryProperty(Class("HTPlayerState"), "LikeabilityInfos", "ArrayProperty", "playerLikeability", sizeof(Array), "LikeabilityInfo");
        const auto likeability = Class("LikeabilityInfo"); StructSize(likeability, "likeabilityDataSize");
        MemoryProperty(likeability, "LikeabilityID", "NameProperty", "likeabilityId", sizeof(std::uint64_t));
        MemoryProperty(likeability, "LikeabilityValue", "IntProperty", "likeabilityValue", sizeof(std::int32_t));
        MemoryProperty(likeability, "LikeabilityLevel", "IntProperty", "likeabilityLevel", sizeof(std::int32_t));
        progression_layouts_valid = true;
    }
    Json OptionalName(std::uint64_t id) const { return id ? Json(Name(id)) : Json(nullptr); }
    template<class Visitor> void VisitArray(Array array, std::uint32_t stride, std::size_t maximum, Visitor&& visitor) const {
        array = CheckedArray(array, maximum);
        if (!array.count) return;
        const auto data = Block(array.data, static_cast<std::size_t>(array.count) * stride);
        for (std::int32_t i{}; i < array.count; ++i)
            visitor(std::span<const std::uint8_t>(data).subspan(static_cast<std::size_t>(i) * stride, stride));
    }
    Json CharacterProgression(std::span<const std::uint8_t> bytes) const {
        Json skills = Json::array(), effects = Json::array();
        VisitArray(Extract<Array>(bytes, "characterSkills"), V("skillDataSize"), 64, [&](auto row) {
            skills.push_back({{"id", OptionalName(Extract<std::uint64_t>(row, "skillId"))},
                {"tag", OptionalName(Extract<std::uint64_t>(row, "skillTag"))}, {"native_level", Extract<std::int32_t>(row, "skillLevel")}});
        });
        VisitArray(Extract<Array>(bytes, "characterAwakenEffects"), V("awakenDataSize"), 64, [&](auto row) {
            effects.push_back({{"id", OptionalName(Extract<std::uint64_t>(row, "awakenId"))},
                {"unlocked", Extract<std::uint8_t>(row, "awakenUnlocked") != 0}});
        });
        return {{"level", Extract<std::int32_t>(bytes, "characterLevel")},
            {"breakthrough_stage", Extract<std::int32_t>(bytes, "characterBreakthrough")},
            {"awakening_level", Extract<std::int32_t>(bytes, "characterAwakening")},
            {"experience", Extract<std::int32_t>(bytes, "characterExperience")},
            {"growth_complete", true}, {"unsaved", Extract<std::uint8_t>(bytes, "characterUnsaved") != 0},
            {"fork_uid", Encode(Extract<Uid>(bytes, "characterFork"))},
            {"native_skills", std::move(skills)}, {"native_skills_complete", true},
            {"skill_levels", nullptr}, {"skills_complete", false},
            {"native_awaken_effects", std::move(effects)}, {"native_awaken_effects_complete", true},
            {"awakening_selection_complete", false}, {"permanent_attributes_complete", false}};
    }
    Json ForkProgression(std::span<const std::uint8_t> bytes) const {
        return {{"level", Extract<std::int32_t>(bytes, "forkLevel")},
            {"breakthrough_stage", Extract<std::int32_t>(bytes, "forkBreakthrough")},
            {"refinement_level", Extract<std::int32_t>(bytes, "forkRefinement")},
            {"experience", Extract<std::int32_t>(bytes, "forkExperience")},
            {"locked", Extract<std::uint8_t>(bytes, "forkLocked") != 0},
            {"character", Encode(Extract<Uid>(bytes, "forkCharacter"))}, {"growth_complete", true}};
    }
    Json VerifiedSkills(Uid uid, std::span<const std::uint8_t> bytes) {
        if (!skill_binding_checked) {
            skill_binding_checked = true;
            const auto function = Exact("/Script/HTGame.CharacterAbilityUpgradeStatic:K2_GetCharacterAbilityLevelWithNetID").second;
            const auto owner = Class("CharacterAbilityUpgradeStatic");
            const auto flags = Field<std::uint32_t>(function, "functionFlags");
            const auto required = V("functionNativeFlag") | V("functionStaticFlag");
            if (ObjectName(Field<std::uintptr_t>(function, "objectClass")) != "Function" ||
                Field<std::uintptr_t>(function, "objectOuter") != owner || (flags & required) != required ||
                Field<std::uint16_t>(function, "functionParameterSize") != offsetof(SkillParameters, effective) + sizeof(SkillParameters::effective) ||
                !profile.MatchesEntry(core, "get-character-ability-level", Field<std::uintptr_t>(function, "functionExec")))
                Fail("The character skill getter does not match the active Profile");
            Parameter(function, "HTPlayerCharacter", "ObjectProperty", offsetof(SkillParameters, character), sizeof(std::uintptr_t));
            Parameter(function, "InUniqueID", "StructProperty", offsetof(SkillParameters, uid), sizeof(Uid), "HTItemNetID");
            Parameter(function, "AbilityId", "NameProperty", offsetof(SkillParameters, ability), sizeof(std::uint64_t));
            Parameter(function, "OutSkillLevel", "IntProperty", offsetof(SkillParameters, base), sizeof(std::int32_t));
            Parameter(function, "OutAwakenSkillLevel", "IntProperty", offsetof(SkillParameters, awakening), sizeof(std::int32_t));
            Parameter(function, "ReturnValue", "IntProperty", offsetof(SkillParameters, effective), sizeof(std::int32_t));
            skill_function = function; skill_receiver = Exact("/Script/HTGame.Default__CharacterAbilityUpgradeStatic").second;
            skill_binding_valid = true;
        }
        if (!skill_binding_valid) Fail("The character skill getter validation failed");
        if (!pawn_handle.id) throw NotReady("A local character is required to query permanent skill levels");
        const auto pawn = Pointer(pawn_handle);
        if (!Derived(Field<std::uintptr_t>(pawn, "objectClass"), Class("HTPlayerCharacter")))
            throw NotReady("The local pawn is not a character skill query source");
        struct SavedSkill { std::int32_t level{}; std::uint64_t tag{}; };
        std::map<std::uint64_t, SavedSkill> saved;
        VisitArray(Extract<Array>(bytes, "characterSkills"), V("skillDataSize"), 64, [&](auto value) {
            const auto id = Extract<std::uint64_t>(value, "skillId");
            if (!id || !saved.emplace(id, SavedSkill{Extract<std::int32_t>(value, "skillLevel"),
                    Extract<std::uint64_t>(value, "skillTag")}).second)
                Fail("An observed permanent skill identity is missing or duplicated");
        });
        const auto item_id = Extract<std::uint64_t>(bytes, "itemId");
        const auto& definitions = CatalogTableFor("character_skills");
        std::uintptr_t definition{};
        for (const auto& [id, row] : definitions.rows) if (id == item_id) { definition = row; break; }
        if (!definition) throw NotReady("The official role skill definition is not loaded");
        Json result = Json::array();
        VisitSparse(definition + skill_cost_layout->abilities, V("abilityMapStride"), [&](auto entry) {
            const auto id = ExtractAt<std::uint64_t>(entry, 0);
            if (!id) Fail("An official character skill has no formal identity");
            const auto native = saved.find(id);
            SkillParameters parameters; parameters.character = pawn; parameters.uid = uid; parameters.ability = id;
            Check(events->invoke(events->user, HandleFor(skill_receiver), HandleFor(skill_function), &parameters, sizeof(parameters)), "Read official character skill levels");
            if ((native != saved.end() && parameters.base != native->second.level) || parameters.base < 1 || parameters.awakening < 0 ||
                parameters.effective != static_cast<std::int64_t>(parameters.base) + parameters.awakening)
                throw NotReady("The official skill getter and saved character levels are not yet consistent");
            result.push_back({{"id", Name(id)}, {"tag", native != saved.end() ? OptionalName(native->second.tag) : Json(nullptr)},
                {"native_level", native != saved.end() ? Json(native->second.level) : Json(nullptr)}, {"native_observed", native != saved.end()},
                {"base_level", parameters.base}, {"awakening_bonus", parameters.awakening},
                {"effective_level", parameters.effective}, {"complete", true}});
        });
        return result;
    }
    std::string GameString(std::uintptr_t address) const {
        const auto value = CheckedArray(ReadValue<Array>(address), 16384);
        if (!value.count) return {};
        std::vector<wchar_t> text(static_cast<std::size_t>(value.count));
        ReadBytes(value.data, text.data(), text.size() * sizeof(wchar_t));
        if (text.back() != L'\0') Fail("The game string is not terminated");
        if (value.count == 1) return {};
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), value.count - 1, nullptr, 0, nullptr, nullptr);
        if (!size) Fail("The game string is not valid UTF-16");
        std::string result(static_cast<std::size_t>(size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), value.count - 1, result.data(), size, nullptr, nullptr))
            Fail("The game string could not be decoded");
        return result;
    }
    std::string GameUtf8String(Array value) const {
        value = CheckedArray(value, 16384);
        if (!value.count) return {};
        const auto bytes = Block(value.data, static_cast<std::size_t>(value.count));
        if (bytes.back() != 0) Fail("The image subobject path is not terminated");
        if (value.count == 1) return {};
        const auto* text = reinterpret_cast<const char*>(bytes.data());
        if (std::find(bytes.begin(), bytes.end() - 1, std::uint8_t{}) != bytes.end() - 1 ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, value.count - 1, nullptr, 0))
            Fail("The image subobject path is not valid UTF-8");
        return std::string(text, static_cast<std::size_t>(value.count - 1));
    }
    std::string ModifierOperation(std::uint8_t value) const {
        if (value == V("modifierAdditive")) return "additive";
        if (value == V("modifierDecrement")) return "decrement";
        if (value == V("modifierOverride")) return "override";
        return {};
    }
    std::string EnumName(std::string_view type_name, std::int64_t value) {
        auto found = enum_names.find(type_name);
        if (found == enum_names.end()) {
            const auto type = Class(type_name);
            if (ObjectName(Field<std::uintptr_t>(type, "objectClass")) != "Enum") Fail("The requested role model enum is unsupported");
            std::map<std::int64_t, std::string> entries;
            VisitArray(Field<Array>(type, "enumNames"), V("enumEntryStride"), 1024, [&](auto entry) {
                const auto id = ExtractAt<std::uint64_t>(entry, 0);
                const auto number = Extract<std::int64_t>(entry, "tableValue");
                if (!entries.emplace(number, Name(id)).second) Fail("The role model enum has duplicate values");
            });
            found = enum_names.emplace(std::string(type_name), std::move(entries)).first;
        }
        const auto name = found->second.find(value);
        if (name == found->second.end()) Fail("The role model enum value is unknown");
        return name->second;
    }
    std::int64_t OwnedAmount(std::string_view item_id) {
        auto item = model_data::ByName(*this, "items", item_id);
        if (!item) item = model_data::ByName(*this, "currencies", item_id);
        if (!item) Fail("The requested inventory amount has no official item or currency identity");
        if (!total_binding_checked) {
            total_binding_checked = true;
            const auto function = Exact("/Script/HTGame.HTInventoryComponent:GetItemTotalAmount").second;
            if (ObjectName(Field<std::uintptr_t>(function, "objectClass")) != "Function" ||
                Field<std::uintptr_t>(function, "objectOuter") != Class("HTInventoryComponent") ||
                (Field<std::uint32_t>(function, "functionFlags") & V("functionNativeFlag")) == 0 ||
                Field<std::uint16_t>(function, "functionParameterSize") != sizeof(TotalParameters) ||
                !profile.MatchesEntry(core, "get-item-total-amount", Field<std::uintptr_t>(function, "functionExec")))
                Fail("The official inventory amount getter does not match the active Profile");
            Parameter(function, "ItemID", "NameProperty", offsetof(TotalParameters, item), sizeof(std::uint64_t));
            Parameter(function, "ContainsBind", "BoolProperty", offsetof(TotalParameters, contains_bound), sizeof(std::uint8_t));
            Parameter(function, "ReturnValue", "Int64Property", offsetof(TotalParameters, value), sizeof(std::int64_t));
            total_function = function; total_binding_valid = true;
        }
        if (!total_binding_valid) Fail("The official inventory amount getter validation failed");
        TotalParameters parameters; parameters.item = item.id;
        Check(events->invoke(events->user, HandleFor(inventory), HandleFor(total_function), &parameters, sizeof(parameters)), "Read official inventory amount");
        if (parameters.value < 0) throw NotReady("The official inventory amount is not yet consistent");
        return parameters.value;
    }
    std::vector<std::uint64_t> ExperienceCardIds(bool fork) {
        const std::string_view property_name = fork ? "ForkExpCards" : "CharacterExpCards";
        auto field = catalog_source_fields.find(property_name);
        if (field == catalog_source_fields.end()) {
            const auto owner = Class("CharacterDataAsset"), property = FindProperty(owner, property_name);
            const auto offset = catalog_data::Field(*this, owner, property_name, "ArrayProperty", sizeof(Array), {}, property);
            const auto inner = Field<std::uintptr_t>(property, "arrayInner");
            const auto kind = Field<std::uintptr_t>(inner, "fieldClass");
            if (Name(Field<std::uint32_t>(kind, "fieldClassName")) != "NameProperty" ||
                Field<std::int32_t>(inner, "propertyArrayDim") != 1 ||
                Field<std::int32_t>(inner, "propertyElementSize") != static_cast<std::int32_t>(sizeof(std::uint64_t)))
                Fail("The official experience card list element schema is unsupported");
            field = catalog_source_fields.emplace(std::string(property_name), offset).first;
        }
        std::vector<std::uint64_t> ids;
        VisitArray(ReadValue<Array>(current_character_asset + field->second), sizeof(std::uint64_t), 256, [&](auto value) {
            const auto id = ExtractAt<std::uint64_t>(value, 0);
            if (!id || std::find(ids.begin(), ids.end(), id) != ids.end()) Fail("The official experience card identities are missing or duplicated");
            ids.push_back(id);
        });
        return ids;
    }
    const CatalogTable& CatalogTableFor(std::string_view section) {
        struct Source { const char* section; const char* property; const char* offset; const char* row_type; const char* size; };
        constexpr std::array sources{
            Source{"items", "ItemDataTable", "gameDataItemTable", "StaticItemData", "staticItemSize"},
            Source{"characters", "CharacterDataTable", "gameDataCharacterTable", "StaticItemData", "staticItemSize"},
            Source{"character_upgrade", "CharacterUpgradeDataTable", "gameDataCharacterUpgrade", "CharacterUpgradeData", "characterUpgradeSize"},
            Source{"character_breakthrough", "CharacterBreakthroughDataTable", "gameDataCharacterBreakthrough", "CharacterBreakthroughData", "characterBreakthroughSize"},
            Source{"fork_upgrade", "ForkUpgradeDataTable", "gameDataForkUpgrade", "StaticForkUpgradeData", "forkUpgradeSize"},
            Source{"fork_breakthrough", "ForkBreakthroughDataTable", "gameDataForkBreakthrough", "ForkBreakthroughData", "forkBreakthroughSize"},
            Source{"fork_refinement", "ForkUpgradeStarDataTable", "gameDataForkRefinement", "StaticForkUpgradeStarData", "forkRefinementSize"}};
        std::uintptr_t table{}, row_type{};
        const Source* source{};
        for (const auto& candidate : sources) if (section == candidate.section) { source = &candidate; break; }
        if (source) {
            if (!catalog_source_fields.contains(section)) {
                MemoryProperty(Class("HTGameData"), source->property, "ObjectProperty", source->offset, sizeof(std::uintptr_t));
                catalog_source_fields.emplace(std::string(section), V(source->offset));
            }
            table = Field<std::uintptr_t>(current_game_data, source->offset); row_type = Class(source->row_type);
        } else if (section == "equipment") { table = equipment_table; row_type = Class("StaticItemData"); }
        else if (section == "fork_templates" || section == "currencies") {
            const auto path = section == "fork_templates" ? "/Game/DataTable/Fork/DT_ForkItemData.DT_ForkItemData" :
                "/Game/DataTable/Inventory/DT_CapitalItemConfig.DT_CapitalItemConfig";
            table = Exact(path).second; row_type = Class("StaticItemData");
        } else if (section == "player_pack" || section == "material_recipes") {
            const auto property = section == "player_pack" ? "PlayerPackDataTable" : "ItemComposeDataTable";
            auto found = catalog_source_fields.find(section);
            if (found == catalog_source_fields.end()) {
                const auto field = catalog_data::Field(*this, Class("HTGameData"), property, "ObjectProperty", sizeof(std::uintptr_t));
                found = catalog_source_fields.emplace(std::string(section), field).first;
            }
            const auto offset = found->second;
            table = ReadValue<std::uintptr_t>(current_game_data + offset);
            row_type = Class(section == "player_pack" ? "PackIDStaticData" : "ComposeItemData");
        } else if (section == "player_modify" || section == "fork_modify" || section == "equipment_modify" || section == "likeability_modify") {
            const auto path = section == "player_modify" ? "/Game/DataTable/PackData/ModifyData/DT_PlayerModifyPackData.DT_PlayerModifyPackData" :
                section == "fork_modify" ? "/Game/DataTable/PackData/ModifyData/DT_ForkModifyData.DT_ForkModifyData" :
                section == "equipment_modify" ? "/Game/DataTable/PackData/ModifyData/DT_EquipmentModifyData.DT_EquipmentModifyData" :
                "/Game/DataTable/PackData/ModifyData/DT_LikeabilityModifyData.DT_LikeabilityModifyData";
            table = Exact(path).second; row_type = Class("ModifyStaticData");
        } else if (section == "character_skills") {
            table = Exact("/Game/DataTable/Character/DT_CharacterAbilityConfig.DT_CharacterAbilityConfig").second;
            row_type = Class("CharacterAbilityConfig");
        } else if (section == "equipment_plans") {
            auto found = catalog_source_fields.find(section);
            if (found == catalog_source_fields.end())
                found = catalog_source_fields.emplace(std::string(section), catalog_data::Field(*this, Class("CharacterDataAsset"), "EquipmentPlanDataTable", "ObjectProperty", sizeof(std::uintptr_t))).first;
            table = ReadValue<std::uintptr_t>(current_character_asset + found->second);
            row_type = Class("EquipmentPlanData");
            if (!plan_core_field) plan_core_field = catalog_data::Field(*this, row_type, "CoreID", "NameProperty", sizeof(std::uint64_t));
        } else if (section == "character_slots" || section == "likeability_roles") {
            const bool slots = section == "character_slots";
            const auto owner = slots ? Class("CharacterDataAsset") : Class("HTGameData");
            const auto property = slots ? "CharacterEquipmentSlotDataTable" : "LikeabilityRoleInfoDataTable";
            auto found = catalog_source_fields.find(section);
            if (found == catalog_source_fields.end())
                found = catalog_source_fields.emplace(std::string(section), catalog_data::Field(*this, owner, property, "ObjectProperty", sizeof(std::uintptr_t))).first;
            table = ReadValue<std::uintptr_t>((slots ? current_character_asset : current_game_data) + found->second);
            row_type = Class(slots ? "CharacterEquipmentSlotsData" : "LikeabilityRoleData");
        } else if (section == "appearances") {
            PrepareImageLayout();
            table = ReadValue<std::uintptr_t>(current_character_asset + image_layout->appearance_table);
            row_type = Class("StaticAppearanceData");
        } else if (section == "equipment_slot_modify") {
            table = Exact("/Game/DataTable/PackData/ModifyData/DT_EquipmentModifySlotsEffect.DT_EquipmentModifySlotsEffect").second;
            row_type = Class("ModifyStaticData");
        } else if (section == "attributes" || section == "equipment_suits") {
            PrepareStatistics();
            table = section == "attributes" ? attribute_table : suit_table;
            row_type = Class(section == "attributes" ? "AttributeStaticData" : "EquipmentSuitStaticData");
        } else Fail("Unsupported live catalog section");
        if (!table) throw NotReady("The requested live catalog is not loaded");
        if (const auto cached = catalog_tables.find(section); cached != catalog_tables.end() && cached->second.pointer == table)
            return cached->second;
        HandleFor(table);
        if (!Derived(Field<std::uintptr_t>(table, "objectClass"), Exact("/Script/Engine.DataTable").second) ||
            Field<std::uintptr_t>(table, "tableRowStruct") != row_type)
            Fail("The live catalog row schema is not supported");
        if (source) StructSize(row_type, source->size);
        if (section == "player_pack") {
            if (!float_pack_layout) float_pack_layout = catalog_data::DescribeFloatPack(*this);
        } else if (section == "material_recipes") {
            if (!recipe_layout) recipe_layout = catalog_data::DescribeRecipes(*this);
        } else if (section == "player_modify" || section == "fork_modify" || section == "equipment_modify" || section == "likeability_modify" || section == "equipment_slot_modify") {
            if (!modifier_layout) modifier_layout = catalog_data::DescribeModifiers(*this);
        } else if (section == "character_skills") {
            if (!skill_cost_layout) skill_cost_layout = catalog_data::DescribeSkillCosts(*this);
        } else if (row_type == Class("StaticItemData")) {
            StructSize(row_type, "staticItemSize");
            MemoryProperty(row_type, "ItemType", "EnumProperty", "staticItemType", sizeof(std::uint8_t));
            MemoryProperty(row_type, "Description", "TextProperty", "staticItemDescription", 16);
            MemoryProperty(row_type, "ItemTags", "ArrayProperty", "staticItemTags", sizeof(Array));
        } else if (section == "character_upgrade" || section == "fork_upgrade") {
            MemoryProperty(row_type, "NeedExp", "IntProperty", "growthNeedExperience", sizeof(std::int32_t));
            if (section == "fork_upgrade") MemoryProperty(row_type, "ModifyPack", "NameProperty", "forkUpgradeModify", sizeof(std::uint64_t));
        } else if (section == "character_breakthrough" || section == "fork_breakthrough") {
            MemoryProperty(row_type, section == "character_breakthrough" ? "MaxCharacterLevel" : "MaxForkLevel", "IntProperty", "breakthroughMaxLevel", sizeof(std::int32_t));
            MemoryProperty(row_type, "NeedItems", "StrProperty", "breakthroughNeedItems", sizeof(Array));
            MemoryProperty(row_type, "NeedGolds", "StrProperty", "breakthroughNeedGolds", sizeof(Array));
            MemoryProperty(row_type, "ModifyPackID", "NameProperty", "breakthroughModify", sizeof(std::uint64_t));
            if (section == "character_breakthrough") MemoryProperty(row_type, "NeedMaxWorldLevel", "IntProperty", "breakthroughWorldLevel", sizeof(std::int32_t));
        } else if (section == "fork_refinement") {
            MemoryProperty(row_type, "Title", "TextProperty", "forkRefinementTitle", 16);
            MemoryProperty(row_type, "Description", "TextProperty", "forkRefinementDescription", 16);
            MemoryProperty(row_type, "NeedGolds", "StrProperty", "forkRefinementNeedGolds", sizeof(Array));
            MemoryProperty(row_type, "DataList", "ArrayProperty", "forkRefinementData", sizeof(Array), "ForkStarDataStruct");
            const auto data = Class("ForkStarDataStruct"); StructSize(data, "forkRefinementDataSize");
            MemoryProperty(data, "NameId", "NameProperty", "forkRefinementDataName", sizeof(std::uint64_t));
            MemoryProperty(data, "bIsPercent", "BoolProperty", "forkRefinementDataPercent", sizeof(std::uint8_t));
        }
        CatalogTable result; result.pointer = table;
        VisitSparse(table + V("tableRowMap"), V("tableStride"), [&](auto entry) {
            const auto name = ExtractAt<std::uint64_t>(entry, 0), pointer = Extract<std::uintptr_t>(entry, "tableValue");
            if (!name || !pointer) Fail("An allocated live catalog row is empty");
            result.rows.emplace_back(name, pointer);
        });
        return catalog_tables.insert_or_assign(std::string(section), std::move(result)).first->second;
    }
    model_data::Row NamedCatalogRow(std::string_view section, std::string_view name) {
        const auto& table = CatalogTableFor(section);
        if (!table.names_ready) {
            std::map<std::string, model_data::Row, std::less<>> index;
            for (const auto& [id, row] : table.rows)
                if (!index.emplace(model_data::IdentifierKey(Name(id)), model_data::Row{id, row}).second)
                    Fail("The official catalog contains ambiguous FName identities");
            table.name_index = std::move(index); table.names_ready = true;
        }
        const auto found = table.name_index.find(model_data::IdentifierKey(name));
        return found == table.name_index.end() ? model_data::Row{} : found->second;
    }
    void CatalogElementLayout(std::uintptr_t type, std::string_view name) {
        if (catalog_element_layouts.contains(type)) return;
        if (name == "CharacterItemData") {
            StructSize(type, "characterElementSize");
            MemoryProperty(type, "CharacterElementType", "EnumProperty", "characterElement", 1);
            MemoryProperty(type, "CharacterGroupType", "EnumProperty", "characterGroup", 1);
            MemoryProperty(type, "PropModifyID", "NameProperty", "characterBaseModify", sizeof(std::uint64_t));
            MemoryProperty(type, "UpgradePackId", "NameProperty", "characterUpgradePack", sizeof(std::uint64_t));
            MemoryProperty(type, "UpgradeModifyPackId", "NameProperty", "characterUpgradeModifyPack", sizeof(std::uint64_t));
            MemoryProperty(type, "BreakthroughPackId", "NameProperty", "characterBreakthroughPack", sizeof(std::uint64_t));
            MemoryProperty(type, "AwakenCostItem", "StrProperty", "characterAwakenCost", sizeof(Array));
            MemoryProperty(type, "MaxAwakenLevel", "IntProperty", "characterMaxAwaken", sizeof(std::int32_t));
            MemoryProperty(type, "ForkId", "NameProperty", "characterDefaultFork", sizeof(std::uint64_t));
            MemoryProperty(type, "EquipmentSlotID", "NameProperty", "characterEquipmentSlot", sizeof(std::uint64_t));
            MemoryProperty(type, "LikeabilityID", "NameProperty", "characterLikeability", sizeof(std::uint64_t));
        } else if (name == "EquipmentItemData") {
            StructSize(type, "equipmentDataSize");
            MemoryProperty(type, "EXP", "IntProperty", "equipmentMaterialExperience", sizeof(std::int32_t));
            MemoryProperty(type, "CostGold", "StrProperty", "equipmentMaterialGoldCost", sizeof(Array));
        } else if (name == "ConsumableItemData") {
            StructSize(type, "consumableElementSize");
            MemoryProperty(type, "EXP", "IntProperty", "consumableExperience", sizeof(std::int32_t));
            MemoryProperty(type, "CostGold", "StrProperty", "consumableGoldCost", sizeof(Array));
        } else if (name == "ForkItemData") {
            StructSize(type, "forkElementSize");
            MemoryProperty(type, "UpgradePackId", "NameProperty", "forkUpgradePack", sizeof(std::uint64_t));
            MemoryProperty(type, "BreakthroughPackId", "NameProperty", "forkBreakthroughPack", sizeof(std::uint64_t));
            MemoryProperty(type, "UpgradeStarPackID", "NameProperty", "forkRefinementPack", sizeof(std::uint64_t));
            MemoryProperty(type, "ApplyGroupType", "EnumProperty", "forkGroup", 1);
            MemoryProperty(type, "MaxBreakthrough", "IntProperty", "forkMaxBreakthrough", sizeof(std::int32_t));
            MemoryProperty(type, "MaxUpgradeStar", "IntProperty", "forkMaxRefinement", sizeof(std::int32_t));
            MemoryProperty(type, "EXP", "IntProperty", "forkMaterialExperience", sizeof(std::int32_t));
            MemoryProperty(type, "CostGold", "StrProperty", "forkMaterialGoldCost", sizeof(Array));
            MemoryProperty(type, "Story", "TextProperty", "forkStory", 16);
            MemoryProperty(type, "ExclusiveCharacterIDArray", "ArrayProperty", "forkExclusiveCharacters", sizeof(Array));
            MemoryProperty(type, "ExclusiveDesc", "TextProperty", "forkExclusiveDescription", 16);
        }
        catalog_element_layouts.insert(type);
    }
    Json CatalogRow(std::string_view section, std::uint64_t id, std::uintptr_t row) {
        Json result{{"id", Name(id)}};
        if (section == "appearances") {
            PrepareImageLayout();
            result.update(AppearanceImages(row));
            result["character_id"] = OptionalName(ReadValue<std::uint64_t>(row + image_layout->appearance_character));
            result["appearance_type"] = EnumName("EAppearanceType", ReadValue<std::uint8_t>(row + image_layout->appearance_type));
            result["is_default"] = ReadValue<std::uint8_t>(row + image_layout->appearance_default) != 0;
            result["image_metadata_complete"] = true;
            return result;
        }
        if (section == "player_pack") {
            result.update(catalog_data::ReadFloatPack(*this, *float_pack_layout, row)); return result;
        }
        if (section == "player_modify" || section == "fork_modify" || section == "equipment_modify" || section == "likeability_modify" || section == "equipment_slot_modify") {
            result.update(catalog_data::ReadModifiers(*this, *modifier_layout, row)); return result;
        }
        if (section == "material_recipes") {
            result.update(catalog_data::ReadRecipe(*this, *recipe_layout, row)); return result;
        }
        if (section == "character_skills") {
            result.update(catalog_data::ReadSkillCosts(*this, *skill_cost_layout, row)); return result;
        }
        if (section == "character_slots" || section == "likeability_roles") {
            if (!model_layout) model_layout = model_data::Describe(*this);
            if (section == "character_slots") {
                result["area"] = ReadValue<std::int32_t>(row + model_layout->slot_area);
                result["modify_pack_id"] = OptionalName(ReadValue<std::uint64_t>(row + model_layout->slot_modify));
                result["description"] = Text(row + model_layout->slot_description, {});
            } else {
                result["name"] = Text(row + model_layout->likeability_name, Name(id));
                result["level_modifiers"] = Json::array();
                VisitSparse(row + model_layout->likeability_modifiers, V("tableStride"), [&](auto entry) {
                    result["level_modifiers"].push_back({{"level", ExtractAt<std::int32_t>(entry, 0)},
                        {"modify_pack_id", OptionalName(Extract<std::uint64_t>(entry, "tableValue"))}});
                });
            }
            return result;
        }
        if (section == "attributes") {
            const auto* definition = Attribute(id);
            if (!definition) Fail("The attribute catalog row is missing");
            result["name"] = definition->name; result["percent"] = definition->percent; return result;
        }
        if (section == "equipment_suits") {
            Metadata meta; meta.kind = "core"; meta.suit = Name(id); bool complete{};
            result["name"] = SuitName(meta.suit);
            result["required_shapes"] = RequiredShapes(meta, complete); result["required_shapes_complete"] = complete;
            return result;
        }
        if (section == "character_upgrade" || section == "fork_upgrade") {
            result["need_experience"] = Field<std::int32_t>(row, "growthNeedExperience");
            if (section == "fork_upgrade") result["modify_pack_id"] = OptionalName(Field<std::uint64_t>(row, "forkUpgradeModify"));
            return result;
        }
        if (section == "character_breakthrough" || section == "fork_breakthrough") {
            result["max_level"] = Field<std::int32_t>(row, "breakthroughMaxLevel");
            result["need_items_raw"] = GameString(row + V("breakthroughNeedItems"));
            result["need_gold_raw"] = GameString(row + V("breakthroughNeedGolds"));
            result["modify_pack_id"] = OptionalName(Field<std::uint64_t>(row, "breakthroughModify"));
            if (section == "character_breakthrough") result["world_level"] = Field<std::int32_t>(row, "breakthroughWorldLevel");
            return result;
        }
        if (section == "fork_refinement") {
            result["name"] = Text(row + V("forkRefinementTitle"), Name(id));
            result["description"] = Text(row + V("forkRefinementDescription"), {});
            result["need_gold_raw"] = GameString(row + V("forkRefinementNeedGolds"));
            result["parameters"] = Json::array();
            VisitArray(Field<Array>(row, "forkRefinementData"), V("forkRefinementDataSize"), 64, [&](auto value) {
                result["parameters"].push_back({{"id", OptionalName(Extract<std::uint64_t>(value, "forkRefinementDataName"))},
                    {"percent", Extract<std::uint8_t>(value, "forkRefinementDataPercent") != 0}});
            });
            result["permanent_attributes_complete"] = false; return result;
        }
        constexpr std::array<std::string_view, 7> qualities{"White", "Green", "Blue", "Purple", "Gold", "Red", "Unknown"};
        const auto quality = Field<std::uint8_t>(row, "staticItemQuality");
        result["name"] = Text(row + V("staticItemName"), Name(id));
        result["description"] = Text(row + V("staticItemDescription"), {});
        result["item_type"] = Field<std::uint8_t>(row, "staticItemType");
        result["quality"] = quality < qualities.size() ? qualities[quality] : "Unknown";
        result["item_tags"] = Json::array();
        VisitArray(Field<Array>(row, "staticItemTags"), sizeof(std::uint64_t), 512, [&](auto value) {
            result["item_tags"].push_back(OptionalName(ExtractAt<std::uint64_t>(value, 0)));
        });
        const auto element = row + V("staticItemElement");
        const auto type = Field<std::uintptr_t>(element, "instanceStructType");
        const auto data = Field<std::uintptr_t>(element, "instanceStructMemory");
        result["element_type"] = type ? Json(ObjectName(type)) : Json(nullptr);
        if (!type || !data) { result["element_complete"] = type == 0 && data == 0; return result; }
        const auto name = ObjectName(type);
        CatalogElementLayout(type, name);
        if (name == "EquipmentItemData") {
            result["material_experience"] = Field<std::int32_t>(data, "equipmentMaterialExperience");
            result["material_gold_cost_raw"] = GameString(data + V("equipmentMaterialGoldCost"));
            if (section == "equipment") {
                const auto& meta = Meta(id); bool required_complete{};
                result.update({{"kind", meta.kind}, {"shape_id", meta.shape}, {"cells", meta.cells}, {"max_level", meta.max_level},
                    {"grid_count", meta.grid_count}, {"suit_id", meta.suit}, {"suit_name", SuitName(meta.suit, meta.name)}, {"required_shapes", RequiredShapes(meta, required_complete)}});
                result["required_shapes_complete"] = required_complete;
            }
            result["element_complete"] = true;
        } else if (name == "ConsumableItemData") {
            result["material_experience"] = Field<std::int32_t>(data, "consumableExperience");
            result["material_gold_cost_raw"] = GameString(data + V("consumableGoldCost"));
            result["element_complete"] = true;
        } else if (name == "CharacterItemData") {
            result.update({{"character_id", Name(id)}, {"element", Field<std::uint8_t>(data, "characterElement")},
                {"group", Field<std::uint8_t>(data, "characterGroup")}, {"base_modify_id", OptionalName(Field<std::uint64_t>(data, "characterBaseModify"))},
                {"upgrade_pack_id", OptionalName(Field<std::uint64_t>(data, "characterUpgradePack"))},
                {"upgrade_modify_pack_id", OptionalName(Field<std::uint64_t>(data, "characterUpgradeModifyPack"))},
                {"breakthrough_pack_id", OptionalName(Field<std::uint64_t>(data, "characterBreakthroughPack"))},
                {"max_awakening", Field<std::int32_t>(data, "characterMaxAwaken")}, {"awakening_cost_raw", GameString(data + V("characterAwakenCost"))},
                {"default_fork_id", OptionalName(Field<std::uint64_t>(data, "characterDefaultFork"))},
                {"equipment_slot_id", OptionalName(Field<std::uint64_t>(data, "characterEquipmentSlot"))},
                {"likeability_id", OptionalName(Field<std::uint64_t>(data, "characterLikeability"))}, {"element_complete", true}});
        } else if (name == "ForkItemData") {
            result.update({{"upgrade_pack_id", OptionalName(Field<std::uint64_t>(data, "forkUpgradePack"))},
                {"breakthrough_pack_id", OptionalName(Field<std::uint64_t>(data, "forkBreakthroughPack"))},
                {"refinement_pack_id", OptionalName(Field<std::uint64_t>(data, "forkRefinementPack"))},
                {"group", Field<std::uint8_t>(data, "forkGroup")}, {"max_breakthrough", Field<std::int32_t>(data, "forkMaxBreakthrough")},
                {"max_refinement", Field<std::int32_t>(data, "forkMaxRefinement")},
                {"material_experience", Field<std::int32_t>(data, "forkMaterialExperience")},
                {"material_gold_cost_raw", GameString(data + V("forkMaterialGoldCost"))},
                {"story", Text(data + V("forkStory"), {})}, {"exclusive_description", Text(data + V("forkExclusiveDescription"), {})},
                {"element_complete", true}, {"permanent_attributes_complete", false}});
            result["exclusive_characters"] = Json::array();
            VisitArray(Field<Array>(data, "forkExclusiveCharacters"), sizeof(std::uint64_t), 64, [&](auto value) {
                result["exclusive_characters"].push_back(OptionalName(ExtractAt<std::uint64_t>(value, 0)));
            });
        } else result["element_complete"] = false;
        return result;
    }
    Json CatalogPage(std::string_view section, std::size_t offset, std::size_t limit) {
        EnsureContext(); PrepareStatics();
        if (!limit || limit > 64) Fail("Catalog page size must be between one and sixty-four");
        Json rows = Json::array(); std::size_t total{}; std::string source;
        const bool dynamic = section == "inventory" || section == "forks" || section == "role_skills";
        if (dynamic) {
            ProgressionLayouts();
            struct Entry { std::uintptr_t pointer{}; Uid uid; std::uint8_t container{}; std::uintptr_t type{}; std::vector<std::uint8_t> bytes; };
            std::vector<Entry> entries;
            const auto fork_type = Class("HTForkItem"), character_type = Class("HTCharacterItem"), item_type = Class("HTItem");
            for (const auto container : SparseEntries(inventory + V("inventoryContainerMap"), V("containerStride"))) {
                const auto container_id = ReadValue<std::uint8_t>(container);
                VisitSparse(container + V("containerValue"), V("itemMapStride"), [&](auto entry) {
                    const auto pointer = Extract<std::uintptr_t>(entry, "itemMapValue");
                    if (!pointer) throw NotReady("An observed inventory item is not yet replicated");
                    const auto type = Field<std::uintptr_t>(pointer, "objectClass");
                    if (!Derived(type, item_type)) Fail("The observed inventory contains an unsupported object type");
                    if (section == "forks" && type != fork_type) return;
                    if (section == "role_skills" && type != character_type) return;
                    std::vector<std::uint8_t> bytes;
                    if (section == "forks" || section == "role_skills") {
                        bytes = Block(pointer, V(section == "forks" ? "forkSize" : "characterSize"));
                        if (Extract<std::uint8_t>(bytes, "itemTemporary") || Extract<std::int64_t>(bytes, "itemAmount") <= 0) return;
                    }
                    entries.push_back({pointer, ExtractAt<Uid>(entry, 0), container_id, type, std::move(bytes)});
                });
            }
            total = entries.size();
            if (offset > total) Fail("Catalog page offset exceeds the observed collection");
            for (auto index = offset; index < std::min(total, offset + limit); ++index) {
                const auto& entry = entries[index];
                const auto copied = entry.bytes.empty() ? Block(entry.pointer, V(entry.type == fork_type ? "forkSize" : "itemSize")) : std::vector<std::uint8_t>{};
                const auto bytes = entry.bytes.empty() ? std::span<const std::uint8_t>(copied) : std::span<const std::uint8_t>(entry.bytes);
                if (Extract<Uid>(bytes, "itemUid") != entry.uid) throw NotReady("An observed inventory item identity is not yet consistent");
                const auto id = Extract<std::uint64_t>(bytes, "itemId");
                Json item{{"uid", Encode(entry.uid)}, {"uid_complete", entry.uid.Valid()}, {"item_id", Name(id)},
                    {"name", DisplayName(id)}, {"class", ObjectName(entry.type)}, {"container", entry.container},
                    {"amount", std::to_string(Extract<std::int64_t>(bytes, "itemAmount"))},
                    {"temporary", Extract<std::uint8_t>(bytes, "itemTemporary") != 0}};
                if (entry.type == fork_type) item.update(ForkProgression(bytes));
                if (section == "role_skills") {
                    item["skills"] = VerifiedSkills(entry.uid, bytes);
                    item["skills_scope"] = "official_character_ability_list";
                    item["skills_observed_complete"] = true;
                    item["skills_complete"] = true;
                }
                rows.push_back(std::move(item));
            }
            source = "all_observed_InventoryContainerMap_containers";
        } else {
            if (section == "equipment" || section == "player_pack") try { PrepareStatistics(); } catch (const std::exception& exception) { statistics_diagnostic = exception.what(); }
            const auto& table = CatalogTableFor(section); total = table.rows.size(); source = ObjectName(table.pointer);
            if (offset > total) Fail("Catalog page offset exceeds the live table");
            const bool item_images_requested = section == "items" || section == "equipment" || section == "characters" ||
                section == "fork_templates" || section == "currencies";
            if (item_images_requested) PrepareImageLayout();
            if (section == "characters") PrepareAppearanceImages();
            for (auto index = offset; index < std::min(total, offset + limit); ++index) {
                const auto [id, address] = table.rows[index];
                auto row = CatalogRow(section, id, address);
                if (item_images_requested) row.update(section == "equipment" ? EquipmentImages(Meta(id)) : ItemImages(address));
                if (section == "characters") row.update(DefaultCharacterImages(id));
                rows.push_back(std::move(row));
            }
        }
        const auto end = offset + rows.size();
        Json page{{"phase", "ready"}, {"section", section}, {"source", source},
            {"source_kind", dynamic ? "live_inventory" : "live_data_table"}, {"source_coverage", dynamic ? "unknown" : "loaded_table"},
            {"context", context.load(std::memory_order_acquire)}, {"source_generation", binding_generation},
            {"notification_sequence", notifications.load(std::memory_order_acquire)},
            {"offset", offset}, {"total", total}, {"rows", std::move(rows)}, {"collection_complete", true},
            {"complete", end == total}, {"next_offset", nullptr}};
        if (end < total) page["next_offset"] = end;
        return page;
    }
    Json CharacterModel(std::string_view role_id, const Json& selected_profile) {
        EnsureContext(); PrepareStatics(); PrepareStatistics();
        if (!model_layout) model_layout = model_data::Describe(*this);
        auto result = model_data::Build(*this, *model_layout, role_id, selected_profile);
        Json descriptions = Json::array();
        for (const auto& entry : attribute_rows) {
            const auto* definition = Attribute(entry.first);
            if (definition) descriptions.push_back({{"id", Name(entry.first)}, {"name", definition->name}, {"percent", definition->percent}});
        }
        result["attributes_metadata"] = std::move(descriptions);
        result["attributes_metadata_source"] = ObjectName(attribute_table);
        result["attributes_metadata_scope"] = "loaded_attribute_table";
        result["context"] = context.load(std::memory_order_acquire);
        result["source_generation"] = binding_generation;
        result["notification_sequence"] = notifications.load(std::memory_order_acquire);
        return result;
    }
    Json ObservedCultivationProfile(std::string_view role_id) {
        ProgressionLayouts();
        const auto definition = NamedCatalogRow("characters", role_id);
        if (!definition) Fail("The observed cultivation role has no official identity");
        std::vector<std::uint8_t> character;
        Uid character_uid;
        std::map<Uid, std::uintptr_t> fork_items;
        const auto character_type = Class("HTCharacterItem"), fork_type = Class("HTForkItem");
        for (const auto container : SparseEntries(inventory + V("inventoryContainerMap"), V("containerStride"))) {
            VisitSparse(container + V("containerValue"), V("itemMapStride"), [&](auto entry) {
                const auto pointer = Extract<std::uintptr_t>(entry, "itemMapValue");
                if (!pointer) throw NotReady("The observed cultivation role is not yet replicated");
                const auto type = Field<std::uintptr_t>(pointer, "objectClass");
                if (type == fork_type) { fork_items.emplace(ExtractAt<Uid>(entry, 0), pointer); return; }
                if (type != character_type) return;
                const auto bytes = Block(pointer, V("characterSize"));
                if (Extract<std::uint64_t>(bytes, "itemId") != definition.id || Extract<std::uint8_t>(bytes, "itemTemporary") ||
                    Extract<std::int64_t>(bytes, "itemAmount") <= 0) return;
                const auto uid = ExtractAt<Uid>(entry, 0);
                if (!uid.Valid() || Extract<Uid>(bytes, "itemUid") != uid) throw NotReady("The observed cultivation role identity is not yet consistent");
                if (!character.empty()) Fail("The observed cultivation role identity is ambiguous");
                character = bytes; character_uid = uid;
            });
        }
        if (character.empty()) throw NotReady("The requested cultivation role is not present in the observed inventory");
        Json result{{"character_level", Extract<std::int32_t>(character, "characterLevel")},
            {"character_breakthrough_stage", Extract<std::int32_t>(character, "characterBreakthrough")},
            {"character_experience", Extract<std::int32_t>(character, "characterExperience")},
            {"awakening_level", Extract<std::int32_t>(character, "characterAwakening")}, {"skill_levels", Json::object()},
            {"fork_id", ""}, {"fork_level", nullptr}, {"fork_breakthrough_stage", nullptr}, {"fork_refinement", nullptr}, {"fork_experience", nullptr}};
        for (const auto& skill : VerifiedSkills(character_uid, character))
            result["skill_levels"][skill.at("id").get<std::string>()] = skill.at("base_level");
        const auto fork_uid = Extract<Uid>(character, "characterFork");
        if (fork_uid.Valid()) {
            const auto item = fork_items.find(fork_uid);
            if (item == fork_items.end()) throw NotReady("The equipped cultivation fork is not yet present in inventory");
            const auto bytes = Block(item->second, V("forkSize"));
            if (Extract<Uid>(bytes, "itemUid") != fork_uid || Extract<Uid>(bytes, "forkCharacter") != character_uid ||
                Extract<std::uint8_t>(bytes, "itemTemporary") || Extract<std::int64_t>(bytes, "itemAmount") <= 0)
                throw NotReady("The role and equipped cultivation fork are not yet consistent");
            result.update({{"fork_id", Name(Extract<std::uint64_t>(bytes, "itemId"))}, {"fork_level", Extract<std::int32_t>(bytes, "forkLevel")},
                {"fork_breakthrough_stage", Extract<std::int32_t>(bytes, "forkBreakthrough")}, {"fork_refinement", Extract<std::int32_t>(bytes, "forkRefinement")},
                {"fork_experience", Extract<std::int32_t>(bytes, "forkExperience")}});
        }
        return result;
    }
    Json CultivationModel(const Json& request) {
        EnsureContext(); PrepareStatics();
        auto result = progression_data::Build(*this, request);
        result["context"] = context.load(std::memory_order_acquire);
        result["source_generation"] = binding_generation;
        result["notification_sequence"] = notifications.load(std::memory_order_acquire);
        return result;
    }
    std::optional<std::string> AccountIdentity() {
        std::array<char, 64> buffer{}; std::size_t size = buffer.size();
        if (account && Succeeded(account->uid_utf8(account->user, buffer.data(), &size)) && size > 1 && size <= buffer.size())
            return std::string(buffer.data(), size - 1);
        return std::nullopt;
    }
    Json Snapshot(const SnapshotProbe& probe) {
        PrepareStatics();
        std::string image_diagnostic;
        bool appearance_ready{};
        try { PrepareAppearanceImages(); appearance_ready = true; }
        catch (const std::exception& exception) { image_diagnostic = exception.what(); }
        try { PrepareStatistics(); }
        catch (const std::exception& exception) { statistics_diagnostic = exception.what(); }
        std::string progression_diagnostic;
        try { ProgressionLayouts(); }
        catch (const std::exception& exception) { progression_diagnostic = exception.what(); }
        const CatalogTable* character_definitions{};
        try { character_definitions = &CatalogTableFor("characters"); }
        catch (const std::exception& exception) { if (progression_diagnostic.empty()) progression_diagnostic = exception.what(); }
        const CatalogTable* equipment_plans{};
        std::string recommendation_diagnostic;
        try { equipment_plans = &CatalogTableFor("equipment_plans"); }
        catch (const std::exception& exception) { recommendation_diagnostic = exception.what(); }
        Json characters = Json::array(), items = Json::array();
        struct ItemRecord { bool equipment, fork; std::vector<std::uint8_t> bytes; };
        std::map<Uid, ItemRecord> inventory_items;
        const auto equipment_class = Class("HTEquipmentItem"), character_class = Class("HTCharacterItem");
        const auto fork_class = progression_layouts_valid ? Class("HTForkItem") : 0;
        for (const auto container : SparseEntries(inventory + V("inventoryContainerMap"), V("containerStride"))) {
            VisitSparse(container + V("containerValue"), V("itemMapStride"), [&](auto entry) {
                const auto pointer = Extract<std::uintptr_t>(entry, "itemMapValue");
                if (!pointer) Fail("An allocated inventory entry has no item");
                const auto type = Field<std::uintptr_t>(pointer, "objectClass");
                // Exact native types intentionally exclude temporary FinalTower subclasses.
                if (type != equipment_class && type != character_class && type != fork_class) return;
                auto record = Block(pointer, V(type == equipment_class ? "equipmentSize" : type == fork_class ? "forkSize" : "characterSize"));
                if (Extract<std::uint8_t>(record, "itemTemporary")) return;
                const auto uid = ExtractAt<Uid>(entry, 0);
                if (!uid.Valid() || Extract<Uid>(record, "itemUid") != uid) Fail("Inventory entry UID disagrees with its object");
                if (Extract<std::int64_t>(record, "itemAmount") <= 0) return;
                if (!inventory_items.emplace(uid, ItemRecord{type == equipment_class, type == fork_class, std::move(record)}).second)
                    Fail("A formal equipment UID occurs in multiple inventory containers");
            });
        }
        std::map<Uid, Location> locations;
        for (const auto& [uid, record] : inventory_items) {
            if (record.equipment || record.fork) continue;
            const auto raw_id = Extract<std::uint64_t>(record.bytes, "itemId");
            const auto item_id = Name(raw_id);
            std::uintptr_t definition{};
            if (character_definitions) for (const auto& [id, row] : character_definitions->rows) if (id == raw_id) { definition = row; break; }
            const bool formal_identity = definition && !item_id.empty() &&
                std::all_of(item_id.begin(), item_id.end(), [](char value) { return value >= '0' && value <= '9'; });
            Json character{{"uid", Encode(uid)}, {"name", definition ? Text(definition + V("staticItemName"), item_id) : DisplayName(raw_id)}, {"item_id", item_id},
                {"character_id", formal_identity ? Json(item_id) : Json(nullptr)}, {"character_identity_complete", formal_identity},
                {"level", Extract<std::int32_t>(record.bytes, "characterLevel")}};
            character["recommended_core"] = nullptr;
            if (equipment_plans) {
                try { character["recommended_core"] = RecommendedCore(raw_id, *equipment_plans); }
                catch (const std::exception& exception) { if (recommendation_diagnostic.empty()) recommendation_diagnostic = exception.what(); }
            }
            character["recommended_core_complete"] = character["recommended_core"].is_object();
            character.update(ItemImages(definition));
            character.update(CurrentCharacterImages(record.bytes, appearance_ready));
            character["growth_complete"] = false;
            character["skills_complete"] = false;
            character["awakening_selection_complete"] = false;
            character["fork_observed"] = false;
            if (progression_layouts_valid) {
                character.update(CharacterProgression(record.bytes));
                character["fork"] = nullptr;
                const auto fork_uid = Extract<Uid>(record.bytes, "characterFork");
                if (fork_uid.Valid()) {
                    const auto fork = inventory_items.find(fork_uid);
                    if (fork == inventory_items.end() || !fork->second.fork ||
                        Extract<Uid>(fork->second.bytes, "forkCharacter") != uid)
                        throw NotReady("The role and fork relationship updates are not yet consistent");
                    Json data{{"uid", Encode(fork_uid)}, {"item_id", Name(Extract<std::uint64_t>(fork->second.bytes, "itemId"))}};
                    data.update(ForkProgression(fork->second.bytes)); character["fork"] = std::move(data);
                }
                character["fork_observed"] = true;
            }
            CharacterGrid(record.bytes, uid, character, locations); characters.push_back(std::move(character));
        }
        if (characters.empty()) throw NotReady("The formal character inventory is not yet ready");
        if (progression_layouts_valid) for (const auto& [uid, record] : inventory_items) {
            if (!record.fork) continue;
            const auto owner = Extract<Uid>(record.bytes, "forkCharacter");
            if (!owner.Valid()) continue;
            const auto character = inventory_items.find(owner);
            if (character == inventory_items.end() || character->second.equipment || character->second.fork ||
                Extract<Uid>(character->second.bytes, "characterFork") != uid)
                throw NotReady("The fork owner and role association updates are not yet consistent");
        }
        for (const auto& [uid, location] : locations) {
            if (!inventory_items.contains(uid))
                throw NotReady("Role equipment references an item not yet present in the inventory");
        }
        std::set<std::int32_t> observed_gold_core_caps;
        for (const auto& [uid, record] : inventory_items) {
            if (!record.equipment) continue;
            const auto& meta = Meta(Extract<std::uint64_t>(record.bytes, "itemId"));
            if (meta.kind == "core" && meta.quality == "Gold") observed_gold_core_caps.insert(meta.max_level);
        }
        std::optional<std::int32_t> gold_core_level;
        if (observed_gold_core_caps.size() == 1 && *observed_gold_core_caps.begin() > 0)
            gold_core_level = *observed_gold_core_caps.begin();
        std::map<std::uint64_t, std::string> attribute_names, item_names;
        for (const auto& [uid, record] : inventory_items) {
            if (!record.equipment) continue;
            const auto item_id = Extract<std::uint64_t>(record.bytes, "itemId");
            const auto& meta = Meta(item_id);
            auto item_name = item_names.find(item_id);
            if (item_name == item_names.end()) item_name = item_names.emplace(item_id, Name(item_id)).first;
            const auto owner = Extract<Uid>(record.bytes, "equipmentCharacter");
            Json item{{"uid", Encode(uid)}, {"item_id", item_name->second}, {"name", meta.name}, {"kind", meta.kind},
                {"quality", meta.quality}, {"shape_id", meta.shape}, {"cells", meta.cells}, {"character", Encode(owner)},
                {"row", nullptr}, {"column", nullptr}, {"locked", Extract<std::uint8_t>(record.bytes, "equipmentLocked") != 0},
                {"discarded", Extract<std::uint8_t>(record.bytes, "equipmentDiscarded") != 0},
                {"level", Extract<std::int32_t>(record.bytes, "equipmentLevel")}, {"max_level", meta.max_level},
                {"experience", Extract<std::int32_t>(record.bytes, "equipmentExperience")},
                {"grid_count", meta.grid_count}, {"suit_id", meta.suit}, {"suit_name", meta.kind == "core" ? SuitName(meta.suit, meta.name) : std::string{}}};
            item.update(EquipmentImages(meta));
            item.update(Statistics(record.bytes, meta, gold_core_level, attribute_names));
            if (const auto found = locations.find(uid); found != locations.end()) {
                if (found->second.character != owner) throw NotReady("Equipment and role relationship updates are not yet consistent");
                if (meta.kind == "module") {
                    if (!found->second.anchor) throw NotReady("The equipped module position is not yet consistent with its role");
                    item["row"] = found->second.row; item["column"] = found->second.column;
                }
            } else if (owner.Valid()) throw NotReady("The equipped item is not yet present in its role equipment slots");
            items.push_back(std::move(item));
        }
        for (auto entry = statistics_cache.begin(); entry != statistics_cache.end();) {
            if (!inventory_items.contains(entry->first)) entry = statistics_cache.erase(entry); else ++entry;
        }
        Json result{{"phase", "ready"}, {"complete", true}, {"characters", std::move(characters)}, {"items", std::move(items)}};
        result["game_account_id"] = nullptr;
        result["account_identity_complete"] = false;
        result["account_identity_source"] = nullptr;
        result["account_service_available"] = account != nullptr;
        if (const auto& account_id = probe.account) {
            result["game_account_id"] = *account_id;
            result["account_identity_complete"] = true;
            result["account_identity_source"] = "anomaly.nte.account";
        }
        if (!statistics_diagnostic.empty()) result["attribute_diagnostic"] = statistics_diagnostic;
        if (!recommendation_diagnostic.empty()) result["recommendation_diagnostic"] = recommendation_diagnostic;
        if (!image_diagnostic.empty()) result["image_diagnostic"] = image_diagnostic;
        result["likeability_complete"] = false;
        result["likeability"] = nullptr;
        if (progression_layouts_valid) {
            Json likeability = Json::array();
            VisitArray(Field<Array>(player_state, "playerLikeability"), V("likeabilityDataSize"), 1024, [&](auto row) {
                likeability.push_back({{"id", OptionalName(Extract<std::uint64_t>(row, "likeabilityId"))},
                    {"level", Extract<std::int32_t>(row, "likeabilityLevel")}, {"value", Extract<std::int32_t>(row, "likeabilityValue")}});
            });
            result["likeability"] = std::move(likeability); result["likeability_complete"] = true;
        }
        if (!progression_diagnostic.empty()) result["progression_diagnostic"] = progression_diagnostic;
        source_notifications.store(probe.inventory_sequence, std::memory_order_release);
        return result;
    }
    static void ANOMALY_CALL Observe(void* user, std::uintptr_t object, std::uintptr_t function, void* parameters) noexcept {
        auto& self = *static_cast<Impl*>(user);
        try {
            if (!self.running.load(std::memory_order_acquire)) return;
            if (function != self.notify_function || object != self.controller || !parameters) return;
            const auto address = reinterpret_cast<std::uintptr_t>(parameters);
            if (self.Field<std::uintptr_t>(address, "notificationInventory") == self.inventory)
                self.notifications.fetch_add(1, std::memory_order_release);
        } catch (...) {}
    }
};

GameBridge::GameBridge() : impl_(std::make_unique<Impl>()) {}
GameBridge::~GameBridge() { Stop(); }

bool GameBridge::Load(const AnomalyHostApiV1* api, std::string& error) {
    Stop();
    try {
        const anomaly::sdk::Host host(api);
        auto& self = *impl_;
        self.core = host.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID).get();
        self.names = host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
        self.objects = host.Query<AnomalyUe5ObjectsServiceV1>(ANOMALY_UE5_OBJECTS_SERVICE_V1_ID).get();
        self.events = host.Query<AnomalyUe5ProcessEventServiceV1>(ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID).get();
        self.framework = host.Query<AnomalyUe5FrameworkServiceV1>(ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID).get();
        self.player = host.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID).get();
        self.account = host.Query<AnomalyNteAccountServiceV1>(ANOMALY_NTE_ACCOUNT_SERVICE_V1_ID).get();
        self.session = host.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
        if (!HasTable(self.core, offsetof(AnomalyCoreServiceV1, read_memory) + sizeof(self.core->read_memory)) || !self.core->read_memory ||
            !HasTable(self.names, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8) + sizeof(self.names->resolve_utf8)) || !self.names->resolve_utf8 ||
            !HasTable(self.objects, sizeof(AnomalyUe5ObjectsServiceV1)) || !self.objects->generation ||
            !self.objects->snapshot_at || !self.objects->snapshot_by_handle || !self.objects->find_exact ||
            !HasTable(self.framework, sizeof(AnomalyUe5FrameworkServiceV1)) || !self.framework->is_game_thread ||
            !HasTable(self.events, offsetof(AnomalyUe5ProcessEventServiceV1, invoke) + sizeof(self.events->invoke)) ||
            !self.events->invoke || !self.events->subscribe || !self.events->unsubscribe ||
            !HasTable(self.player, offsetof(AnomalyNtePlayerServiceV1, get_object_handles) + sizeof(self.player->get_object_handles)) ||
            !self.player->get_object_handles ||
            !HasTable(self.session, offsetof(AnomalyNteSessionServiceV1, snapshot) + sizeof(self.session->snapshot)) || !self.session->snapshot)
            Fail("Required Game and reflection services are unavailable");
        if (!HasTable(self.account, sizeof(AnomalyNteAccountServiceV1)) || !self.account->uid_utf8) self.account = nullptr;
        if (!self.profile.Load(api, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "equipment-manager-v1"},
                {"get-player-state", "get-game-data", "evaluate-equipment-curve", "get-character-ability-level", "get-item-total-amount", "inventory-notify", "clear", "equip-module", "equip-core", "replace-core",
                 "unequip-module", "unequip-core", "equip-one-key", "move-module", "move-core", "set-locked", "set-discarded",
                 "avatar-load-asset", "avatar-create-target", "avatar-begin-canvas", "avatar-draw-texture", "avatar-end-canvas", "avatar-export-png", "avatar-release-target"},
                false, "object-registry", &error)) return false;
        const auto instruction = self.profile.Address("object-registry");
        const auto displacement = self.ReadValue<std::int32_t>(instruction + self.V("registryRipOffset"));
        self.registry = instruction + self.V("registryInstructionSize") + displacement - self.V("registrySubtract");
        if (self.V("registryChunkSize") == 0 || self.V("mapSize") < sizeof(Array) ||
            self.V("gridMaximum") != 5 || self.V("gridMinimum") != 1)
            Fail("Unsupported equipment Profile contract");
        self.running.store(true, std::memory_order_release);
        Check(self.events->subscribe(self.events->user, &Impl::Observe, &self, &self.subscription), "Subscribe inventory notifications");
        error.clear(); return true;
    } catch (const std::exception& exception) { error = exception.what(); Stop(); return false; }
    catch (...) { error = "Unknown equipment bridge initialization failure"; Stop(); return false; }
}

void GameBridge::Stop() {
    if (!impl_) return;
    auto& self = *impl_; self.running.store(false, std::memory_order_release);
    if (self.subscription.id && self.events && self.events->unsubscribe)
        self.events->unsubscribe(self.events->user, std::exchange(self.subscription, {}));
    self.pawn_handle = {}; self.controller_handle = {}; self.state_handle = {}; self.controller = 0; self.player_state = 0; self.inventory = 0;
    self.binding_generation = 0; self.layouts_valid = false; self.static_schema_valid = false; self.notify_function = 0;
    self.last_snapshot_probe.reset(); self.observed_session.reset();
    self.source_notifications.store(0, std::memory_order_release);
    self.classes.clear(); self.resolved_names.clear(); self.functions.clear(); self.metadata.clear(); self.static_rows.clear(); self.equipment_rows.clear();
    self.item_table = 0; self.equipment_table = 0;
    self.current_game_data = 0; self.current_character_asset = 0;
    self.attribute_table = 0; self.curve_table = 0; self.suit_table = 0; self.curve_function = 0; self.curve_receiver = 0;
    self.statistics_schema_checked = false; self.statistics_schema_valid = false;
    self.curve_binding_checked = false; self.curve_binding_valid = false; self.statistics_diagnostic.clear();
    self.attribute_rows.clear(); self.suit_rows.clear(); self.attribute_definitions.clear(); self.curve_values.clear(); self.curve_rows.clear(); self.suit_definitions.clear();
    self.catalog_tables.clear(); self.catalog_element_layouts.clear(); self.progression_layouts_valid = false;
    self.statistics_cache.clear();
    self.skill_binding_checked = false; self.skill_binding_valid = false; self.skill_function = 0; self.skill_receiver = 0;
    self.float_pack_layout.reset(); self.modifier_layout.reset(); self.recipe_layout.reset();
    self.skill_cost_layout.reset();
    self.catalog_source_fields.clear();
    self.model_layout.reset(); self.enum_names.clear();
    self.total_binding_checked = false; self.total_binding_valid = false; self.total_function = 0;
    self.image_layout.reset(); self.item_images.clear(); self.equipment_images.clear(); self.appearance_images.clear();
    self.appearance_rows.clear(); self.appearance_order.clear(); self.default_appearances.clear(); self.appearance_table = 0;
    self.avatar_functions.clear(); self.avatar_default_objects.clear(); self.avatar_enum_values.clear(); self.avatar_requests.clear(); self.avatar_resource_layout.reset();
}

bool GameBridge::Read(Json& state, std::string& error) {
    return ReadIfDirty(state, error, true) == SnapshotReadResult::updated;
}

SnapshotReadResult GameBridge::ReadIfDirty(Json& state, std::string& error, bool force) {
    try {
        auto probe = impl_->ProbeSnapshot();
        if (!force && impl_->last_snapshot_probe && *impl_->last_snapshot_probe == probe) {
            error.clear(); return SnapshotReadResult::unchanged;
        }
        state = impl_->Snapshot(probe);
        impl_->last_snapshot_probe = std::move(probe);
        error.clear(); return SnapshotReadResult::updated;
    }
    catch (const NotReady& exception) { impl_->last_snapshot_probe.reset(); state = {{"phase", "waiting"}}; error = exception.what(); return SnapshotReadResult::waiting; }
    catch (const std::exception& exception) { impl_->last_snapshot_probe.reset(); state = {{"phase", "unavailable"}}; error = exception.what(); return SnapshotReadResult::unavailable; }
    catch (...) { impl_->last_snapshot_probe.reset(); state = {{"phase", "unavailable"}}; error = "Unknown equipment inventory read failure"; return SnapshotReadResult::unavailable; }
}

bool GameBridge::ReadCatalogPage(std::string_view section, std::size_t offset, std::size_t limit, Json& page, std::string& error) {
    try { page = impl_->CatalogPage(section, offset, limit); error.clear(); return true; }
    catch (const NotReady& exception) { page = {{"phase", "waiting"}, {"collection_complete", false}}; error = exception.what(); return false; }
    catch (const std::exception& exception) { page = {{"phase", "unavailable"}, {"collection_complete", false}}; error = exception.what(); return false; }
    catch (...) { page = {{"phase", "unavailable"}, {"collection_complete", false}}; error = "Unknown live catalog read failure"; return false; }
}

bool GameBridge::ReadCharacterModel(std::string_view role_id, const Json& selected_profile, Json& model, std::string& error) {
    try { model = impl_->CharacterModel(role_id, selected_profile); error.clear(); return true; }
    catch (const NotReady& exception) { model = {{"phase", "waiting"}, {"complete", false}}; error = exception.what(); return false; }
    catch (const std::exception& exception) { model = {{"phase", "unavailable"}, {"complete", false}}; error = exception.what(); return false; }
    catch (...) { model = {{"phase", "unavailable"}, {"complete", false}}; error = "Unknown official character model read failure"; return false; }
}

bool GameBridge::ReadCultivationModel(const Json& request, Json& model, std::string& error) {
    try { model = impl_->CultivationModel(request); error.clear(); return true; }
    catch (const NotReady& exception) { model = {{"phase", "waiting"}, {"complete", false}}; error = exception.what(); return false; }
    catch (const std::exception& exception) { model = {{"phase", "unavailable"}, {"complete", false}}; error = exception.what(); return false; }
    catch (...) { model = {{"phase", "unavailable"}, {"complete", false}}; error = "Unknown official cultivation model read failure"; return false; }
}

bool GameBridge::ReadAvatarMetadata(Json& characters, std::string& error) {
    try { characters = impl_->AvatarMetadata(); error.clear(); return true; }
    catch (const std::exception& exception) { characters = Json::array(); error = exception.what(); return false; }
    catch (...) { characters = Json::array(); error = "Unknown avatar metadata read failure"; return false; }
}

AvatarExportResult GameBridge::ExportAvatar(std::string_view role_id, std::string_view expected_asset_path,
                                          std::wstring_view filename, std::string& error) {
    try { const auto result = impl_->ExportAvatar(role_id, expected_asset_path, filename); error.clear(); return result; }
    catch (const NotReady& exception) { error = exception.what(); return AvatarExportResult::waiting; }
    catch (const std::exception& exception) { error = exception.what(); return AvatarExportResult::rejected; }
    catch (...) { error = "Unknown avatar export failure"; return AvatarExportResult::rejected; }
}

void GameBridge::ReleaseAvatar(std::wstring_view filename) {
    try { impl_->ReleaseAvatar(filename); }
    catch (...) {}
}

std::uint64_t GameBridge::Context() const { return impl_->context.load(std::memory_order_acquire); }
std::uint64_t GameBridge::NotificationSequence() const { return impl_->notifications.load(std::memory_order_acquire); }
std::uint64_t GameBridge::SourceNotificationSequence() const { return impl_->source_notifications.load(std::memory_order_acquire); }

DispatchResult GameBridge::Dispatch(const Json& command, const Json& snapshot, std::string& error) {
    bool attempted{};
    auto failure_result = DispatchResult::rejected;
    try {
        auto& self = *impl_; self.GameOnly();
        const auto dispatch = [&](const Impl::Function& function, void* parameters, std::size_t size) {
            attempted = true;
            failure_result = DispatchResult::unknown;
            const auto status = self.events->invoke(self.events->user, self.state_handle, function.handle, parameters, size);
            if (!Succeeded(status)) {
                switch (status.code) {
                case ANOMALY_STATUS_V1_UNAVAILABLE:
                case ANOMALY_STATUS_V1_INVALID_ARGUMENT:
                case ANOMALY_STATUS_V1_NOT_FOUND:
                case ANOMALY_STATUS_V1_CONFLICT:
                case ANOMALY_STATUS_V1_BUFFER_TOO_SMALL:
                    failure_result = DispatchResult::rejected;
                    break;
                default: break;
                }
                Check(status, "Dispatch equipment RPC");
            }
        };
        const auto action = command.at("action").get<std::string>();
        const auto find = [&](std::string_view collection, Uid uid) -> const Json& {
            for (const auto& value : snapshot.at(collection)) if (Decode(value.at("uid")) == uid) return value;
            Fail("The selected role or equipment is absent from the current inventory");
        };
        const bool state_action = action == "set_item_locked" || action == "set_item_discarded";
        Uid character{};
        if (!state_action) { character = Decode(command.at("character")); find("characters", character); }
        if (action == "unequip_all") {
            dispatch(self.FunctionFor("clear", "ServerEquipmentClear", true), &character, sizeof(character));
        } else if (state_action) {
            const auto equipment = Decode(command.at("equipment")); const auto& item = find("items", equipment);
            const bool locked = action == "set_item_locked";
            const auto& value = command.at(locked ? "locked" : "discarded");
            if (!value.is_boolean()) Fail("Equipment state must be a boolean");
            if (value.get<bool>() && item.at(locked ? "discarded" : "locked").get<bool>())
                Fail("Clear the mutually exclusive equipment state first");
            StateParameters parameters{equipment, static_cast<std::uint8_t>(value.get<bool>())};
            dispatch(self.FunctionFor(locked ? "set-locked" : "set-discarded",
                locked ? "ServerEquipmentItemLocked" : "ServerEquipmentItemDiscard", true), &parameters, sizeof(parameters));
        } else {
            const auto position = [&](const Json& placement, const Json& item, std::set<std::pair<int, int>>& used) {
                const auto row = placement.at("row").get<std::int32_t>(), column = placement.at("column").get<std::int32_t>();
                if (item.at("kind") != "module" || item.at("cells").empty()) Fail("Module placement requires a verified shape");
                const auto& target = find("characters", character);
                for (const auto& cell : item.at("cells")) {
                    const int r = row + cell.at(0).get<int>(), c = column + cell.at(1).get<int>();
                    const auto coordinate = Json::array({r, c});
                    if (r < static_cast<int>(self.V("gridMinimum")) || r > static_cast<int>(self.V("gridMaximum")) ||
                        c < static_cast<int>(self.V("gridMinimum")) || c > static_cast<int>(self.V("gridMaximum")) ||
                        std::find(target.at("allowed_cells").begin(), target.at("allowed_cells").end(), coordinate) == target.at("allowed_cells").end() ||
                        !used.emplace(r, c).second) Fail("Module placement overlaps or exceeds the active board");
                }
                return Placement{Decode(item.at("uid")), row, column};
            };
            if (action == "equip_one_key") {
                const auto& requested = command.at("placements");
                if (!requested.is_array() || requested.empty() || requested.size() > 64) Fail("An equipment plan requires 1..64 module placements");
                std::vector<Placement> placements; std::set<Uid> unique; std::set<std::pair<int, int>> used;
                for (const auto& request : requested) {
                    const auto uid = Decode(request.at("equipment"));
                    if (!unique.insert(uid).second) Fail("An equipment plan repeats a formal UID");
                    placements.push_back(position(request, find("items", uid), used));
                }
                const auto core_uid = Decode(command.at("core"));
                if (find("items", core_uid).at("kind") != "core" || !unique.insert(core_uid).second) Fail("An equipment plan requires a distinct core");
                OneKeyParameters parameters{character, {reinterpret_cast<std::uintptr_t>(placements.data()),
                    static_cast<std::int32_t>(placements.size()), static_cast<std::int32_t>(placements.size())}, core_uid};
                // The validated native thunk copies this borrowed array synchronously.
                dispatch(self.FunctionFor("equip-one-key", "ServerEquipmentInlayOneKey", true), &parameters, sizeof(parameters));
            } else {
                const auto uid = Decode(command.at("equipment")); const auto& item = find("items", uid);
                const bool module = action == "equip_module" || action == "unequip_module" || action == "move_module_to_character";
                if (item.at("kind") != (module ? "module" : "core")) Fail("Equipment type does not match the operation");
                const bool unequip = action == "unequip_module" || action == "unequip_core";
                const bool moving = action == "move_module_to_character" || action == "move_core_to_character";
                if (unequip && (item.at("character").is_null() || Decode(item.at("character")) != character)) Fail("Equipment is not assigned to the selected role");
                if (moving && item.at("character").is_null()) Fail("Only equipped items can be transferred");
                if (action == "equip_module" || action == "equip_core") {
                    if (!item.at("character").is_null()) Fail("Use a transfer operation for already equipped items");
                }
                std::string_view symbol, method;
                if (action == "equip_module") { symbol = "equip-module"; method = "ServerEquipmentInlay"; }
                else if (action == "equip_core") { symbol = "equip-core"; method = "ServerEquipmentInlayCore"; }
                else if (action == "replace_core") { symbol = "replace-core"; method = "ServerEquipmentCoreReplace"; }
                else if (action == "unequip_module") { symbol = "unequip-module"; method = "ServerEquipmentErase"; }
                else if (action == "unequip_core") { symbol = "unequip-core"; method = "ServerEquipmentEraseCore"; }
                else if (action == "move_module_to_character") { symbol = "move-module"; method = "ServerEquipmentEraseAndInlayToOther"; }
                else if (action == "move_core_to_character") { symbol = "move-core"; method = "ServerEquipmentCoreEraseAndInlayToOther"; }
                else Fail("Unknown equipment action");
                if (module && !unequip) {
                    std::set<std::pair<int, int>> occupied;
                    for (const auto& other : snapshot.at("items")) {
                        if (other.at("kind") != "module" || other.at("character").is_null() || Decode(other.at("character")) != character || Decode(other.at("uid")) == uid) continue;
                        if (other.at("row").is_null() || other.at("column").is_null() || other.at("cells").empty()) Fail("Current board occupancy is not fully known");
                        for (const auto& cell : other.at("cells")) occupied.emplace(other.at("row").get<int>() + cell.at(0).get<int>(), other.at("column").get<int>() + cell.at(1).get<int>());
                    }
                    const auto target = position(command, item, occupied);
                    InlayParameters parameters{character, uid, target.row, target.column};
                    dispatch(self.FunctionFor(symbol, method, true), &parameters, sizeof(parameters));
                } else {
                    PairParameters parameters{character, uid};
                    dispatch(self.FunctionFor(symbol, method, true), &parameters, sizeof(parameters));
                }
            }
        }
        error.clear(); return DispatchResult::dispatched;
    } catch (const std::exception& exception) { error = exception.what(); return attempted ? failure_result : DispatchResult::rejected; }
    catch (...) { error = "Unknown equipment RPC failure"; return attempted ? DispatchResult::unknown : DispatchResult::rejected; }
}

}  // namespace equipment_manager
