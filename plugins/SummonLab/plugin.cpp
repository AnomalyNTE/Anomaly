#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/plugin.h>
#include <anomaly/sdk/services/core.h>
#include <anomaly/sdk/services/interop.h>
#include <anomaly/sdk/services/nte.h>
#include <anomaly/sdk/services/ui.h>
#include <anomaly/sdk/services/ue5.h>

#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/profile.hpp"
#include "attribute_catalog.hpp"
#include "attribute_descriptions.hpp"
#include "battle_graph.hpp"
#include "native_layout.hpp"
#include "blueprint_names.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- 常量

constexpr std::size_t kMaxRecords = 24;
constexpr std::size_t kManagementPageSize = 10;
// SummonAI accepts signed 32-bit levels; do not impose a gameplay cap.
constexpr std::uint32_t kMaxLevel = static_cast<std::uint32_t>(
    std::numeric_limits<std::int32_t>::max());
constexpr std::size_t kMaxPending = 16;
constexpr std::size_t kTextCapacity = 256;
constexpr std::size_t kPathCapacity = 512;
constexpr std::size_t kAttributeNameCapacity = 96;
constexpr std::size_t kDisplayNameCapacity = 96;
using summon_lab::kBirthAttributeNames;
using summon_lab::kMaxHpAttribute;

// Parsed once per DLL generation; callbacks only read this snapshot.
summon_lab::NativeLayout g_layout;
bool g_layout_ready{};
using summon_lab::NativeIdentity;
using summon_lab::MirrorEntry;
using summon_lab::ConfigureSummon;

// 枚举
constexpr std::uint8_t kAttackAggressive = 0;
constexpr std::uint8_t kAttackDefensive = 1;

// 路径
constexpr const char* kGameplayStaticsCdo = "/Script/Engine.Default__GameplayStatics";
constexpr const char* kPathMakeSoftClassPath =
    "/Script/Engine.KismetSystemLibrary:MakeSoftClassPath";
constexpr const char* kPathConvSoftClassRef =
    "/Script/Engine.KismetSystemLibrary:Conv_SoftClassPathToSoftClassRef";
constexpr const char* kPathGetTransform = "/Script/Engine.Actor:GetTransform";
constexpr const char* kPathDestroyActor = "/Script/Engine.Actor:K2_DestroyActor";
constexpr const char* kPathSummonAI = "/Script/HTGame.HTAbilityCharacter:SummonAI";
constexpr const char* kPathAddSummonedAI = "/Script/HTGame.HTAbilityCharacter:AddSummonedAI";
constexpr const char* kPathGetHP = "/Script/HTGame.HTAbilityCharacter:GetHP";
constexpr const char* kPathGetHPMax = "/Script/HTGame.HTAbilityCharacter:GetHPMax";

constexpr const char* kDefaultClassPath =
    "/Game/Blueprints/Character/Monster/BP_WoodTest.BP_WoodTest_C";

constexpr std::uint32_t kHealthRefreshInterval = 60;
constexpr float kBattleChaseRange = 1000000.0F;


// ---------------------------------------------------------------- 类型

enum class OpKind : std::uint32_t {
    Summon = 1,
    Remove = 2,
    ClearAll = 3,
    RefreshHP = 4,
    SetBossHpBar = 5,
    SetCollision = 6,
    SetHidden = 7,
    SetBattleEdge = 8,
    StopDuel = 9,
    SelectAllOpponents = 10
};

struct PendingOp {
    OpKind kind{};
    std::uint64_t record_id{};
    bool enabled{};
    std::uint64_t target_id{};
};

struct ClassEntry {
    std::string label;
    std::string path;
    std::string search_text;
};

enum class BlueprintConfigState { Pending, Unavailable, NotApplicable, NoHealth, Available };

struct BlueprintConfigCheck {
    std::string path;
    BlueprintConfigState state{BlueprintConfigState::Pending};
    std::string source;
};

struct AttributeValue {
    std::string name;
    float base{};
    float current{};
};

struct BirthPreset {
    int enabled{};
    double value{};
};

struct SummonIdentity {
    AnomalyGenerationHandleV1 controller{};
    NativeIdentity leader{};
    std::int32_t index{-1};
};

struct Record {
    std::uint64_t id{};
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};      // SummonAI 返回的 Actor 指针，Dismiss 需要它
    float hp{};
    float max_hp{};
    bool pet{};
    SummonIdentity registration;
    std::string server_combat_source;
    bool battle_tuned{};
    float original_chase_range{};
    std::uint8_t original_leave_battle{};
    bool boss_hp_bar_visible{true};
    std::vector<AttributeValue> attributes;
    char label[96]{};
};

struct NativeCombatHook {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t original{};
};

struct NameTextConversion {
    const wchar_t* input{};
    std::int32_t count{};
    std::int32_t capacity{};
    std::array<std::uint8_t, 16> text{};
};
static_assert(sizeof(NameTextConversion) == 32);

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::sdk::Localizer localizer;
    std::array<summon_lab::AttributeDescription, kBirthAttributeNames.size()> attribute_descriptions;
    const AnomalyUiServiceV1* ui{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    AnomalyGenerationHandleV1 health_observer{};
    std::array<AnomalyGenerationHandleV1, 4> health_events{};
    bool health_event_lookup_pending{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyHookServiceV1* hooks{};
    std::array<NativeCombatHook, 4> local_combat_hooks{};
    std::uint32_t mirror_record_size{};
    std::vector<std::uint8_t> mirror_getter_entry;
    std::uint32_t camp_count{};
    std::uint32_t current_chase_range_offset{};
    std::size_t available_birth_attributes{};
    AnomalyGenerationHandleV1 attribute_hook{};
    std::uintptr_t attribute_original{};
    std::atomic<bool> birth_attributes_ready{false};
    std::uintptr_t monster_level_query{};
    std::string final_tower_scene_asset;
    std::map<std::string, std::string, std::less<>> final_tower_aliases;
    std::string final_tower_combat_blueprint;

    bool has_invoke{};

    std::mutex mutex;
    std::array<Record, kMaxRecords> records{};
    std::size_t record_count{};
    std::uint64_t next_record_id{1};

    std::array<PendingOp, kMaxPending> pending{};
    std::size_t pending_count{};

    bool session_world_ready{};
    std::uint64_t session_event_cursor{};
    std::uint32_t frame_counter{};

    // 可编辑参数
    std::array<char, kPathCapacity> class_path{};
    BlueprintConfigCheck blueprint_config;
    double blueprint_check_delay{};
    std::array<char, kDisplayNameCapacity> display_name{};
    std::uint32_t level{};
    std::uint32_t skill_level{};
    std::array<BirthPreset, kBirthAttributeNames.size()> birth_presets{};
    std::array<char, 64> attribute_filter{};
    std::size_t attribute_help{kBirthAttributeNames.size()};
    int only_overrides{};
    std::size_t catalog_page{};
    int hostile{};
    int pet{};
    int aggressive{};
    int ft_compatibility{};

    // Game-thread-only scratch. Native Conv_StringToText replaces its previous
    // FText through UE's move assignment. Retain the final reference for this
    // DLL generation rather than guessing an ITextData destructor on unload.
    NameTextConversion name_text;

    // 离线生成的蓝图目录（assets/summonable.json）
    std::vector<ClassEntry> catalog;
    char catalog_filter[64]{};
    bool catalog_loaded{};
    std::uint64_t selected_record_id{};
    std::size_t management_page{};
    summon_lab::BattleGraph battles;

    std::array<char, kTextCapacity> last_result{};
    bool last_result_failed{};
} g_context;

std::atomic<std::uint32_t> g_pending_hint{0};

// ---------------------------------------------------------------- 安全读 / 日志

bool ReadBytes(std::uintptr_t address, void* destination, std::size_t size) {
    if (g_context.core == nullptr || g_context.core->read_memory == nullptr || address == 0 ||
        destination == nullptr || size == 0)
        return false;
    AnomalyMutableByteSpanV1 span{};
    span.data = static_cast<std::uint8_t*>(destination);
    span.size = size;
    return g_context.core->read_memory(g_context.core->user, address, span).code ==
           ANOMALY_STATUS_V1_OK;
}

template <typename T>
bool ReadValue(std::uintptr_t address, T* out) {
    return ReadBytes(address, static_cast<void*>(out), sizeof(T));
}

// ---------------------------------------------------------------- invoke

bool HasInvoke(const AnomalyUe5ProcessEventServiceV1* service) {
    if (service == nullptr) return false;
    const std::size_t required = offsetof(AnomalyUe5ProcessEventServiceV1, invoke) +
                                 sizeof(AnomalyUe5ProcessEventServiceV1::invoke);
    return service->struct_size >= required && service->invoke != nullptr;
}

bool FindHandle(const char* path, AnomalyGenerationHandleV1* handle) {
    if (g_context.objects == nullptr || g_context.objects->find_exact == nullptr) return false;
    handle->id = 0;
    handle->generation = 0;
    return g_context.objects->find_exact(g_context.objects->user, anomaly::sdk::StringView(path),
                                         handle).code == ANOMALY_STATUS_V1_OK &&
           handle->id != 0;
}

bool InvokeByPath(AnomalyGenerationHandleV1 object, const char* path, void* parms,
                  std::size_t size) {
    if (!g_context.has_invoke || g_context.events == nullptr) return false;
    AnomalyGenerationHandleV1 function{};
    if (!FindHandle(path, &function)) return false;
    const auto status =
        g_context.events->invoke(g_context.events->user, object, function, parms, size);
    return status.code == ANOMALY_STATUS_V1_OK;
}

// ---------------------------------------------------------------- 对象工具

bool ProbeHandle(AnomalyGenerationHandleV1 handle) {
    if (g_context.objects == nullptr || g_context.objects->snapshot_by_handle == nullptr) return false;
    AnomalyUe5ObjectSnapshotV1 probe{};
    probe.struct_size = sizeof(probe);
    return g_context.objects->snapshot_by_handle(g_context.objects->user, handle, &probe).code ==
           ANOMALY_STATUS_V1_OK;
}

// 裸指针 -> ue5.objects handle：读 InternalIndex 后由 snapshot_at 取回带正确 serial 的 handle
bool HandleFromPointer(std::uintptr_t pointer, AnomalyGenerationHandleV1* out) {
    if (pointer == 0 || g_context.objects == nullptr || g_context.objects->snapshot_at == nullptr)
        return false;
    std::uint32_t internal_index{};
    if (!ReadValue(pointer + static_cast<std::uintptr_t>(g_layout.objectInternalIndex), &internal_index)) return false;
    AnomalyUe5ObjectSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (g_context.objects->snapshot_at(g_context.objects->user, internal_index, &snapshot).code !=
        ANOMALY_STATUS_V1_OK)
        return false;
    *out = snapshot.handle;
    return out->id != 0;
}

// ---------------------------------------------------------------- session 就绪门

void ClearSummonedState();

void UpdateSessionState() {
    if (g_context.session == nullptr) return;

    bool clear_state = false;
    if (g_context.session->snapshot != nullptr) {
        AnomalyNteSessionSnapshotV1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        const bool ready = g_context.session->snapshot(g_context.session->user, &snapshot).code ==
            ANOMALY_STATUS_V1_OK && snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY;
        std::scoped_lock lock(g_context.mutex);
        clear_state = g_context.session_world_ready && !ready;
        g_context.session_world_ready = ready;
    }

    if (g_context.session->next_event != nullptr &&
        g_context.session->latest_event_sequence != nullptr) {
        const std::uint64_t latest =
            g_context.session->latest_event_sequence(g_context.session->user);
        for (int guard = 0; guard < 16 && g_context.session_event_cursor < latest; ++guard) {
            AnomalyNteSessionEventV1 event{};
            event.struct_size = sizeof(event);
            if (g_context.session->next_event(g_context.session->user,
                                              g_context.session_event_cursor, &event).code !=
                ANOMALY_STATUS_V1_OK)
                break;
            g_context.session_event_cursor = event.sequence;
            if (event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_CHANGED ||
                event.kind == ANOMALY_NTE_SESSION_EVENT_V1_WORLD_UNAVAILABLE) {
                clear_state = true;
            }
        }
    }
    if (clear_state) ClearSummonedState();
}

// ---------------------------------------------------------------- 本地玩家

bool FindLocalPawn(AnomalyGenerationHandleV1* out) {
    out->id = 0;
    out->generation = 0;
    if (g_context.player == nullptr) return false;

    constexpr std::size_t kRequired =
        offsetof(AnomalyNtePlayerServiceV1, get_object_handles) +
        sizeof(AnomalyNtePlayerServiceV1::get_object_handles);
    if (g_context.player->struct_size < kRequired ||
        g_context.player->get_object_handles == nullptr) {
        return false;
    }

    AnomalyGenerationHandleV1 pawn{}, controller{};
    const auto status =
        g_context.player->get_object_handles(g_context.player->user, &pawn, &controller);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    if (pawn.id == 0) {
        // SummonAI 是 HTAbilityCharacter 的方法，Controller 不能当 receiver
        return false;
    }
    *out = pawn;
    return true;
}

// ---------------------------------------------------------------- 蓝图目录

// 目录来自离线导出：FModel 把 HT/Content/Blueprints 全量导成 JSON，脚本按
// Super ／ SuperStruct 链结合 dump 出来的 SDK 头，挑出继承 HTAICharacter 的蓝图，
// 落成 assets/summonable.json。运行时只读这份目录，不再扫内存注册表——
// 未加载的蓝图类根本不在注册表里，扫不出完整清单。
// 宿主把整个插件包原样复制到 .cache\<pid>\packages\<id>\<generation>\ 再加载 DLL，
// assets/ 在 DLL 旁边；profile/ 由宿主的 anomaly.profile 服务加载。
std::wstring PackagePath(const wchar_t* relative_path) {
    std::array<wchar_t, 4096> buffer{};
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&PackagePath), &module) == FALSE ||
        module == nullptr)
        return {};
    const DWORD size = GetModuleFileNameW(module, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return {};
    std::filesystem::path path =
        std::filesystem::path(std::wstring(buffer.data(), size)).parent_path() / relative_path;
    // The host may load an extended-length (\\?\) path, which does not normalize '/'.
    path.make_preferred();
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) ? path.wstring() : std::wstring{};
}

bool LoadCatalog() {
    const std::wstring path = PackagePath(L"assets/summonable.json");
    if (path.empty()) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string document((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    in.close();

    const auto root = nlohmann::json::parse(document, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return false;
    }
    if (root.value("schemaVersion", 0) != 1) {
        return false;
    }
    const auto groups = root.find("groups");
    if (groups == root.end() || !groups->is_array()) {
        return false;
    }

    std::vector<ClassEntry> parsed;
    parsed.reserve(static_cast<std::size_t>(root.value("count", 0)) + 16);
    for (const auto& group : *groups) {
        if (!group.is_object()) continue;
        const std::string prefix = group.value("prefix", std::string{});
        const auto items = group.find("entries");
        if (items == group.end() || !items->is_array()) continue;
        for (const auto& item : *items) {
            if (!item.is_object()) continue;
            const std::string name = item.value("n", std::string{});
            const std::string package = item.value("p", std::string{});
            if (name.empty() || package.empty()) continue;
            const auto names = summon_lab::ResolveBlueprintNames(name, g_context.localizer);
            const std::string& display_name = names.specific.empty() ? names.family : names.specific;
            const std::string label = display_name.empty() ? name : display_name + " | " + name;
            const std::string class_path = prefix + package + "." + name;
            parsed.push_back(ClassEntry{label, class_path,
                                        label + "\n" + class_path + "\n" + names.family});
        }
    }

    {
        std::scoped_lock lock(g_context.mutex);
        g_context.catalog = std::move(parsed);
        const auto ft = root.find("finalTower");
        if (ft != root.end()) {
            // Asset paths and row aliases are game data, alongside the blueprint catalog.
            g_context.final_tower_scene_asset = ft->at("sceneDataAsset").get<std::string>();
            g_context.final_tower_aliases = ft->at("configAliases").get<
                std::map<std::string, std::string, std::less<>>>();
            g_context.final_tower_combat_blueprint = ft->value("serverCombatBlueprint", std::string{});
        }
        g_context.catalog_loaded = true;
    }
    return true;
}

// ---------------------------------------------------------------- 软类引用

bool BuildSoftClassRef(AnomalyGenerationHandleV1 lib_cdo, const char* utf8_path,
                       summon_lab::SoftClassRef* output) {
    static wchar_t path_buffer[kPathCapacity];
    std::size_t length = 0;
    while (utf8_path[length] != '\0' && length + 1 < kPathCapacity) {
        path_buffer[length] = static_cast<wchar_t>(static_cast<unsigned char>(utf8_path[length]));
        ++length;
    }
    path_buffer[length] = L'\0';
    const std::int32_t count = static_cast<std::int32_t>(length) + 1;

    summon_lab::MakeSoftClassPath make{{path_buffer, count, count}};
    if (!InvokeByPath(lib_cdo, kPathMakeSoftClassPath, &make, sizeof(make))) return false;

    summon_lab::ConvertSoftClass conv{make.result};
    if (!InvokeByPath(lib_cdo, kPathConvSoftClassRef, &conv, sizeof(conv))) return false;

    *output = conv.result;
    return true;
}

// ---------------------------------------------------------------- 属性读写

bool ReadHealth(AnomalyGenerationHandleV1 target, float* hp, float* max_hp) {
    float hp_parms{};
    const bool hp_ok = InvokeByPath(target, kPathGetHP, &hp_parms, sizeof(hp_parms));
    summon_lab::HealthMaximum max_parms{};
    const bool max_ok = InvokeByPath(target, kPathGetHPMax, &max_parms, sizeof(max_parms));

    if (hp_ok) *hp = hp_parms;
    if (max_ok) *max_hp = max_parms.result;
    return hp_ok && max_ok;
}

bool IsActorUsable(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer) {
    // A readable address may already belong to another UObject after GC.
    // Validate the saved serial/generation before inspecting or passing that address.
    if (pointer == 0 || !ProbeHandle(handle)) return false;
    std::uint32_t index{};
    if (!ReadValue(pointer + g_layout.objectInternalIndex, &index) ||
        index != ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle)) return false;

    // Registry membership alone includes actors pending destruction. Use the
    // engine's lifetime checks before calling gameplay methods on such actors.
    AnomalyGenerationHandleV1 library{};
    summon_lab::ObjectBool valid{pointer};
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) ||
        !InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:IsValid",
                      &valid, sizeof(valid)) || valid.value == 0) return false;
    std::uint8_t destroying{};
    return InvokeByPath(handle, "/Script/Engine.Actor:IsActorBeingDestroyed",
                        &destroying, sizeof(destroying)) && destroying == 0;
}

bool IsActorAlive(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer) {
    if (!IsActorUsable(handle, pointer)) return false;
    std::uint8_t dead{};
    return InvokeByPath(handle, "/Script/HTGame.HTAbilityCharacter:GetIsDead",
                        &dead, sizeof(dead)) && dead == 0;
}

bool ResolveName(std::uint32_t name_id, char* destination, std::size_t capacity) {
    if (name_id == 0 || destination == nullptr || capacity == 0 || g_context.names == nullptr ||
        g_context.names->resolve_utf8 == nullptr)
        return false;
    std::size_t size = capacity;
    return g_context.names->resolve_utf8(g_context.names->user, name_id, destination, &size).code ==
           ANOMALY_STATUS_V1_OK;
}

bool IsGameplayAttributeProperty(std::uintptr_t field, int32_t* offset_out) {
    if (field == 0 || offset_out == nullptr) return false;
    std::uintptr_t field_class{};
    std::uint32_t field_class_name{};
    std::int32_t array_dim{};
    std::int32_t element_size{};
    std::int32_t offset{};
    if (!ReadValue(field + g_layout.fieldClass, &field_class) || field_class == 0 ||
        !ReadValue(field_class + g_layout.fieldClassName, &field_class_name) || !ReadValue(field + g_layout.propertyArrayDim, &array_dim) ||
        !ReadValue(field + g_layout.propertyElementSize, &element_size) ||
        !ReadValue(field + g_layout.propertyOffset, &offset))
        return false;

    char field_class_text[64]{};
    if (!ResolveName(field_class_name, field_class_text, sizeof(field_class_text)) ||
        std::strcmp(field_class_text, "StructProperty") != 0)
        return false;

    std::uintptr_t value_struct{};
    std::uint32_t value_struct_name{};
    if (!ReadValue(field + g_layout.propertyStruct, &value_struct) || value_struct == 0 ||
        !ReadValue(value_struct + g_layout.objectName, &value_struct_name))
        return false;
    char value_struct_text[64]{};
    if (!ResolveName(value_struct_name, value_struct_text, sizeof(value_struct_text)) ||
        std::strcmp(value_struct_text, "GameplayAttributeData") != 0 || array_dim != 1 ||
        element_size != static_cast<std::int32_t>(g_layout.attributeDataSize) || offset < 0 || offset >= 0x4000)
        return false;
    *offset_out = offset;
    return true;
}

std::uintptr_t FindProperty(std::uintptr_t uclass, const char* wanted,
                            std::uint32_t name_number = 0) {
    if (uclass == 0 || wanted == nullptr) return 0;
    for (int class_guard = 0; class_guard < 16 && uclass != 0; ++class_guard) {
        std::uintptr_t field{};
        if (!ReadValue(uclass + g_layout.structProperties, &field)) return 0;
        for (int field_guard = 0; field_guard < 512 && field != 0; ++field_guard) {
            std::uint64_t property_name{};
            char name[kAttributeNameCapacity]{};
            if (ReadValue(field + g_layout.fieldName, &property_name) &&
                static_cast<std::uint32_t>(property_name >> 32) == name_number &&
                ResolveName(static_cast<std::uint32_t>(property_name), name, sizeof(name)) &&
                std::strcmp(name, wanted) == 0)
                return field;
            if (!ReadValue(field + g_layout.fieldNext, &field)) return 0;
        }
        if (!ReadValue(uclass + g_layout.structSuper, &uclass)) return 0;
    }
    return 0;
}

bool RebaseWuchaoArena(AnomalyGenerationHandleV1 actor, std::uintptr_t pointer,
                      std::uintptr_t actor_class) {
    auto parent = actor_class;
    bool wuchao{};
    for (int guard = 0; guard < 16 && parent != 0; ++guard) {
        std::uint32_t id{};
        char name[kAttributeNameCapacity]{};
        if (!ReadValue(parent + g_layout.objectName, &id) ||
            !ResolveName(id, name, sizeof(name))) return false;
        // Game identifiers: this blueprint's skills use absolute arena points.
        if (std::strcmp(name, "Boss_32_Wuchao_BP_C") == 0) { wuchao = true; break; }
        if (!ReadValue(parent + g_layout.structSuper, &parent)) return false;
    }
    if (!wuchao) return true;

    // The two Act10_LOC properties differ in FName::Number. Preserve their
    // identities and relative positions; only rebase this summoned instance.
    struct Point {
        const char* name;
        std::uint32_t number;
        std::uint64_t property_name{};
        std::int32_t offset{};
        std::array<double, 3> value{};
    };
    std::array<Point, 4> points{{{"Scene_center", 0}, {"Act28_LOC", 0},
                                {"Act10_LOC", 2}, {"Act10_LOC", 3}}};
    for (auto& point : points) {
        const auto property = FindProperty(actor_class, point.name, point.number);
        std::uintptr_t field_class{};
        std::uint32_t type_id{};
        std::int32_t size{}, dimension{};
        char type[64]{};
        if (property == 0 || !ReadValue(property + g_layout.fieldClass, &field_class) ||
            !ReadValue(field_class + g_layout.fieldClassName, &type_id) || !ResolveName(type_id, type, sizeof(type)) ||
            std::strcmp(type, "StructProperty") != 0 ||
            !ReadValue(property + g_layout.propertyElementSize, &size) || size != sizeof(point.value) ||
            !ReadValue(property + g_layout.propertyArrayDim, &dimension) || dimension != 1 ||
            !ReadValue(property + g_layout.propertyOffset, &point.offset) || point.offset < 0 ||
            !ReadValue(property + g_layout.fieldName, &point.property_name) ||
            !ReadValue(pointer + point.offset, &point.value) ||
            !std::all_of(point.value.begin(), point.value.end(), [](double v) { return std::isfinite(v); }))
            return false;
    }
    AnomalyGenerationHandleV1 library{};
    std::array<double, 3> location{};
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) ||
        !InvokeByPath(actor, "/Script/Engine.Actor:K2_GetActorLocation",
                       location.data(), sizeof(location)) ||
        !std::all_of(location.begin(), location.end(), [](double v) { return std::isfinite(v); }))
        return false;

    const auto center = points[0].value;
    for (const auto& point : points) {
        struct {
            std::uintptr_t object;
            std::uint64_t property_name;
            std::array<double, 3> value;
        } params{pointer, point.property_name, {}};
        static_assert(sizeof(params) == 40);
        for (std::size_t axis = 0; axis < location.size(); ++axis)
            params.value[axis] = location[axis] + (point.value[axis] - center[axis]);
        // The native setter verifies FVector's reflected type and owns the
        // assignment. No blueprint offsets, class defaults or script bytes change.
        std::array<double, 3> actual{};
        if (!InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:SetVectorPropertyByName",
                           &params, sizeof(params)) ||
            !ReadValue(pointer + point.offset, &actual) || actual != params.value) return false;
    }
    if (g_context.core != nullptr && g_context.core->log != nullptr) {
        char detail[160]{};
        std::snprintf(detail, sizeof(detail), "SummonLab arena: actor=%p center=(%.1f, %.1f, %.1f)",
            reinterpret_cast<void*>(pointer), location[0], location[1], location[2]);
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                            anomaly::sdk::StringView(detail));
    }
    return true;
}

bool SetSpawnDisplayName(AnomalyGenerationHandleV1 actor, std::uintptr_t pointer,
                         std::string_view name) {
    if (name.empty()) return true;
    if (!IsActorAlive(actor, pointer)) return false;
    std::uintptr_t attributes{};
    if (!InvokeByPath(actor, "/Script/HTGame.HTAbilityCharacter:GetHTCharacterAttributeSet",
                      &attributes, sizeof(attributes)) || attributes == 0) return false;
    // Only the new actor's instance may be renamed; shared defaults stay owned
    // by the engine. Obtain the outer through reflection, not a new layout.
    AnomalyGenerationHandleV1 system_library{}, text_library{}, conversion{}, setter{};
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &system_library) ||
        !FindHandle("/Script/Engine.Default__KismetTextLibrary", &text_library) ||
        !FindHandle("/Script/Engine.KismetTextLibrary:Conv_StringToText", &conversion) ||
        !FindHandle("/Script/Engine.KismetSystemLibrary:SetTextPropertyByName", &setter)) return false;
    std::array<std::uintptr_t, 2> outer{attributes, 0};
    if (!InvokeByPath(system_library, "/Script/Engine.KismetSystemLibrary:GetOuterObject",
                      outer.data(), sizeof(outer)) || outer[1] != pointer) return false;

    std::uintptr_t uclass{}, field_class{};
    std::uint32_t field_class_name{};
    std::int32_t offset{}, element_size{}, array_dim{};
    std::uint64_t property_name{};
    if (!ReadValue(attributes + g_layout.objectClass, &uclass)) return false;
    const auto property = FindProperty(uclass, "DescriptiveName");
    char type[64]{};
    if (property == 0 || !ReadValue(property + g_layout.fieldClass, &field_class) ||
        !ReadValue(field_class + g_layout.fieldClassName, &field_class_name) ||
        !ResolveName(field_class_name, type, sizeof(type)) || std::strcmp(type, "TextProperty") != 0 ||
        !ReadValue(property + g_layout.propertyElementSize, &element_size) || element_size != 16 ||
        !ReadValue(property + g_layout.propertyArrayDim, &array_dim) || array_dim != 1 ||
        !ReadValue(property + g_layout.propertyOffset, &offset) || offset < 0 ||
        !ReadValue(property + g_layout.fieldName, &property_name) ||
        g_context.names->resolve_ftext_utf8 == nullptr) return false;

    std::array<wchar_t, kDisplayNameCapacity> wide{};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(name.size()), wide.data(), static_cast<int>(wide.size() - 1));
    if (length <= 0) return false;
    auto& value = g_context.name_text;
    value.input = wide.data();
    value.count = length + 1;
    value.capacity = length + 1;
    const auto converted = g_context.events->invoke(g_context.events->user, text_library,
        conversion, &value, sizeof(value));
    value.input = nullptr;
    value.count = value.capacity = 0;
    if (converted.code != ANOMALY_STATUS_V1_OK) return false;

    // SetTextPropertyByName performs the FText reference-counted assignment.
    // The argument borrows the scratch text; no UObject/FText bytes are patched.
    summon_lab::TextProperty parameters{attributes, property_name, value.text};
    if (g_context.events->invoke(g_context.events->user, system_library, setter,
            &parameters, sizeof(parameters)).code != ANOMALY_STATUS_V1_OK) return false;
    std::array<char, kDisplayNameCapacity> actual{};
    std::size_t size = actual.size();
    return g_context.names->resolve_ftext_utf8(g_context.names->user,
        attributes + static_cast<std::uintptr_t>(offset), actual.data(), &size).code ==
            ANOMALY_STATUS_V1_OK && name == actual.data();
}

bool ReadAttributeValues(std::uintptr_t attribute_set, std::vector<AttributeValue>* values) {
    if (attribute_set == 0 || values == nullptr) return false;
    std::uintptr_t uclass{};
    if (!ReadValue(attribute_set + g_layout.objectClass, &uclass) || uclass == 0) return false;

    std::vector<AttributeValue> parsed;
    for (int class_guard = 0; class_guard < 16 && uclass != 0; ++class_guard) {
        std::uintptr_t field{};
        if (!ReadValue(uclass + g_layout.structProperties, &field)) return false;
        for (int field_guard = 0; field_guard < 512 && field != 0; ++field_guard) {
            int32_t offset{};
            std::uint32_t name_id{};
            if (IsGameplayAttributeProperty(field, &offset) &&
                ReadValue(field + g_layout.fieldName, &name_id) && name_id != 0) {
                AttributeValue value;
                char name[kAttributeNameCapacity]{};
                if (ResolveName(name_id, name, sizeof(name)) &&
                    ReadValue(attribute_set + static_cast<std::uintptr_t>(offset) +
                                  g_layout.attributeBaseValue,
                              &value.base) &&
                    ReadValue(attribute_set + static_cast<std::uintptr_t>(offset) +
                                  g_layout.attributeCurrentValue,
                              &value.current)) {
                    value.name = name;
                    parsed.push_back(std::move(value));
                }
            }
            if (!ReadValue(field + g_layout.fieldNext, &field)) return false;
        }
        if (!ReadValue(uclass + g_layout.structSuper, &uclass)) return false;
    }
    *values = std::move(parsed);
    return true;
}

// Resolve the fields that moved between the supplied 1.4b and 1.5a SDKs by
// reflected name/type/size, rather than trying multiple offsets on an object.
template <typename T>
bool ReadReflectedValue(std::uintptr_t object, const char* name,
                        std::string_view expected_type, T* value) {
    std::uintptr_t uclass{}, field_class{};
    std::uint32_t type_id{};
    std::int32_t offset{}, size{}, dimension{};
    char type[64]{};
    if (object == 0 || !ReadValue(object + g_layout.objectClass, &uclass)) return false;
    const auto property = FindProperty(uclass, name);
    return property != 0 && ReadValue(property + g_layout.fieldClass, &field_class) &&
        ReadValue(field_class + g_layout.fieldClassName, &type_id) && ResolveName(type_id, type, sizeof(type)) &&
        (expected_type == type || (expected_type == "ByteProperty" && std::strcmp(type, "EnumProperty") == 0)) &&
        ReadValue(property + g_layout.propertyElementSize, &size) && size == sizeof(T) &&
        ReadValue(property + g_layout.propertyArrayDim, &dimension) && dimension == 1 &&
        ReadValue(property + g_layout.propertyOffset, &offset) && offset >= 0 &&
        ReadValue(object + offset, value);
}

template <typename T>
bool ReadRowValue(std::uintptr_t row, std::uintptr_t row_struct, const char* name,
                  std::string_view expected_type, T* value) {
    const auto property = FindProperty(row_struct, name);
    std::uintptr_t field_class{};
    std::uint32_t type_id{};
    std::int32_t offset{}, size{}, dimension{};
    char type[64]{};
    return property != 0 && ReadValue(property + g_layout.fieldClass, &field_class) &&
        ReadValue(field_class + g_layout.fieldClassName, &type_id) && ResolveName(type_id, type, sizeof(type)) &&
        expected_type == type && ReadValue(property + g_layout.propertyElementSize, &size) &&
        size == sizeof(T) && ReadValue(property + g_layout.propertyArrayDim, &dimension) &&
        dimension == 1 && ReadValue(property + g_layout.propertyOffset, &offset) &&
        offset >= 0 && ReadValue(row + offset, value);
}

bool ReadCamp(std::uintptr_t actor, std::uint8_t* camp) {
    return ReadReflectedValue(actor, "CampType", "ByteProperty", camp);
}

bool ResolveActorAttributeSet(std::uintptr_t actor, std::uintptr_t* ability_system,
                              std::uintptr_t* attribute_set) {
    if (actor == 0 || ability_system == nullptr || attribute_set == nullptr) return false;
    *ability_system = 0;
    *attribute_set = 0;
    if (!ReadValue(actor + g_layout.characterAbilitySystem, ability_system)) return false;
    if (ReadValue(actor + g_layout.characterAttributeSet, attribute_set) && *attribute_set != 0)
        return true;
    if (*ability_system == 0) return false;
    if (ReadReflectedValue(*ability_system, "HTAttribute", "ObjectProperty", attribute_set) && *attribute_set != 0)
        return true;
    return ReadReflectedValue(*ability_system, "StandaloneHTAttribute", "ObjectProperty", attribute_set) &&
           *attribute_set != 0;
}

bool HealthMatches(float actual, float expected) {
    if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
    return actual == expected;
}

bool ReadMatchingHealth(AnomalyGenerationHandleV1 actor, float expected,
                        float* hp, float* max_hp) {
    if (!ReadHealth(actor, hp, max_hp)) return false;
    summon_lab::HealthMaximum fixed_max{1};
    if (!InvokeByPath(actor, kPathGetHPMax, &fixed_max, sizeof(fixed_max))) return false;
    return HealthMatches(*hp, expected) && HealthMatches(*max_hp, expected) &&
           HealthMatches(fixed_max.result, expected);
}

// ---------------------------------------------------------------- 出生属性（只改变 GAS 初始化输入）

struct BirthAttribute {
    std::string name;
    float value{};
    bool applied{};
};

struct CombatConfigSource {
    std::string config_name;
    std::string row_name;
    std::uint64_t config{};
    std::uint64_t pack{};
    std::uintptr_t row{};
    std::uintptr_t property_pack{};
    std::int32_t level{};
};

CombatConfigSource PrepareFinalTowerConfig(std::string_view config_name);

struct BirthInitialization {
    std::uintptr_t actor_class{};
    std::uintptr_t actor{};
    std::uintptr_t attribute_set{};
    std::vector<BirthAttribute> attributes;
    float exact_hp{};
    std::uintptr_t registration_controller{};
    NativeIdentity registration_leader{};
    std::int32_t registration_index{-1};
    std::uintptr_t registration_cdo{};
    bool registration_substituted{};
};

using AttributeBaseInputFn = void (ANOMALY_CALL*)(void*, const void*, float);
std::atomic<DWORD> g_birth_thread{0};
BirthInitialization* g_birth_initialization{}; // Owned exclusively by g_birth_thread.

struct LocalBirthBinding {
    AnomalyGenerationHandleV1 actor_handle{};
    std::uintptr_t actor{};
    std::uintptr_t ability_system{};
    std::uintptr_t attribute_set{};
    float exact_hp{};
    CombatConfigSource combat_config;
    SummonIdentity registration;
    std::uint32_t health_observations{}, mirror_observations{};
};
struct PendingBirthConfiguration {
    AnomalyGenerationHandleV1 actor_handle{};
    AnomalyGenerationHandleV1 controller_handle{};
    std::uintptr_t ability_system{};
    ConfigureSummon parameters{};
    float exact_hp{};
    float native_max_hp{};
    bool invalid_max_hp{};
    bool require_native_capacity{};
};
std::mutex g_local_birth_mutex;
std::vector<LocalBirthBinding> g_local_births;
std::vector<PendingBirthConfiguration> g_birth_configurations;
std::atomic<bool> g_birth_configuration_ready{false};
std::vector<LocalBirthBinding> g_pending_local_births;
std::atomic<bool> g_restore_local_births{false};
std::atomic<bool> g_observe_ft_health{false};

void ForgetLocalBirth(AnomalyGenerationHandleV1 handle) {
    const auto same_actor = [&](const auto& entry) {
        return entry.actor_handle.id == handle.id &&
            entry.actor_handle.generation == handle.generation;
    };
    std::scoped_lock lock(g_local_birth_mutex);
    std::erase_if(g_local_births, same_actor);
    std::erase_if(g_birth_configurations, same_actor);
    g_observe_ft_health.store(std::any_of(g_local_births.begin(), g_local_births.end(),
        [](const auto& entry) { return entry.combat_config.config_name.starts_with("FT_"); }),
        std::memory_order_release);
}

void ClearSummonedState() {
    {
        std::scoped_lock lock(g_local_birth_mutex);
        g_local_births.clear();
        g_birth_configurations.clear();
    }
    g_birth_configuration_ready.store(false, std::memory_order_release);
    g_pending_local_births.clear();
    g_restore_local_births.store(false, std::memory_order_release);
    g_observe_ft_health.store(false, std::memory_order_release);
    g_context.health_events = {};
    g_context.health_event_lookup_pending = g_context.health_observer.id != 0;
    std::scoped_lock lock(g_context.mutex);
    g_context.records = {};
    g_context.record_count = 0;
    g_context.selected_record_id = 0;
    g_context.management_page = 0;
    g_context.battles = {};
    g_context.pending_count = 0;
    g_context.blueprint_config = {};
    g_context.blueprint_check_delay = 0;
    g_pending_hint.store(0, std::memory_order_release);
}

void RegisterLocalBirth(AnomalyGenerationHandleV1 handle, std::uintptr_t actor,
                        std::uintptr_t ability_system, std::uintptr_t attribute_set,
                        float exact_hp, const CombatConfigSource& config = {},
                        const SummonIdentity& registration = {}) {
    std::scoped_lock lock(g_local_birth_mutex);
    g_local_births.erase(std::remove_if(g_local_births.begin(), g_local_births.end(),
        [](const LocalBirthBinding& binding) { return !ProbeHandle(binding.actor_handle); }),
        g_local_births.end());
    g_local_births.push_back({handle, actor, ability_system, attribute_set, exact_hp, config,
        registration});
    if (config.config_name.starts_with("FT_"))
        g_observe_ft_health.store(true, std::memory_order_release);
}

// Passive FT diagnostics: observe native replies before the engine consumes
// them. Never invoke gameplay code or retain a borrowed RPC parameter buffer.
bool DiagnosticStruct(std::uintptr_t property, const char* expected_name,
                      std::uintptr_t* type, std::int32_t* size) {
    std::uintptr_t field_class{};
    std::uint32_t id{};
    std::int32_t dimension{}, declared_size{};
    char name[96]{};
    return property != 0 && ReadValue(property + g_layout.fieldClass, &field_class) &&
        ReadValue(field_class + g_layout.fieldClassName, &id) && ResolveName(id, name, sizeof(name)) &&
        std::strcmp(name, "StructProperty") == 0 &&
        ReadValue(property + g_layout.propertyArrayDim, &dimension) && dimension == 1 &&
        ReadValue(property + g_layout.propertyElementSize, size) && *size > 0 && *size <= 512 &&
        ReadValue(property + g_layout.propertyStruct, type) && *type != 0 &&
        ReadValue(*type + g_layout.objectName, &id) && ResolveName(id, name, sizeof(name)) &&
        std::strcmp(name, expected_name) == 0 &&
        ReadValue(*type + g_layout.structSize, &declared_size) && declared_size == *size;
}

bool DiagnosticNestedStruct(std::uintptr_t data, std::uintptr_t owner, const char* field,
                            const char* expected_type, std::uintptr_t* nested_data,
                            std::uintptr_t* nested_type) {
    const auto property = FindProperty(owner, field);
    std::int32_t size{}, offset{}, owner_size{};
    if (!DiagnosticStruct(property, expected_type, nested_type, &size) ||
        !ReadValue(property + g_layout.propertyOffset, &offset) || offset < 0 ||
        !ReadValue(owner + g_layout.structSize, &owner_size) ||
        offset > owner_size || size > owner_size - offset) return false;
    *nested_data = data + offset;
    return true;
}

void ObserveFinalTowerHealth(const char* event, AnomalyGenerationHandleV1 controller,
                            std::uintptr_t data, std::uintptr_t type, const char* hp_field,
                            bool has_dead_state) {
    NativeIdentity identity{};
    float hp{};
    std::int32_t dead_state{-1};
    if (!ReadRowValue(data, type, "NetTarget", "StructProperty", &identity)) return;
    if (hp_field != nullptr) {
        if (!ReadRowValue(data, type, hp_field, "FloatProperty", &hp)) return;
    } else if (!ReadRowValue(data, type, "fCurHealth", "FloatProperty", &hp)) {
        // 1.5 nests CurrentHp in DamageDisplayInfo; 1.4 exposes fCurHealth.
        std::uintptr_t display{}, display_type{};
        if (!DiagnosticNestedStruct(data, type, "DamageDisplayInfo", "DamageDisplayInfo",
                &display, &display_type) ||
            !ReadRowValue(display, display_type, "CurrentHp", "FloatProperty", &hp)) return;
    }
    if (!std::isfinite(hp) ||
        (has_dead_state && !ReadRowValue(data, type, "DeadState", "IntProperty", &dead_state))) return;
    std::int32_t index{};
    std::memcpy(&index, identity.data() + g_layout.diagnosticSummonIndex, sizeof(index));
    std::uintptr_t actor{}, ability_system{};
    AnomalyGenerationHandleV1 handle{};
    {
        std::scoped_lock lock(g_local_birth_mutex);
        for (auto& binding : g_local_births) {
            const auto& registration = binding.registration;
            if (!binding.combat_config.config_name.starts_with("FT_") ||
                binding.health_observations >= 16 || index != registration.index ||
                controller.id != registration.controller.id ||
                controller.generation != registration.controller.generation ||
                std::memcmp(identity.data(), registration.leader.data(),
                    g_layout.diagnosticSummonIndex) != 0) continue;
            ++binding.health_observations;
            actor = binding.actor;
            ability_system = binding.ability_system;
            handle = binding.actor_handle;
            break;
        }
    }
    if (handle.id == 0 || !ProbeHandle(handle)) return;
    float local_hp{}, local_max{};
    const bool local_read = ReadReflectedValue(ability_system, "HPCurrent", "FloatProperty", &local_hp) &&
        ReadReflectedValue(ability_system, "MaxHPTemp", "FloatProperty", &local_max);
    char message[320]{};
    std::snprintf(message, sizeof(message),
        "SummonLab FT-health-reply: event=%s actor=%p index=%d incomingHP=%.9g deadState=%d localBefore=%.9g/%.9g localReadable=%s",
        event, reinterpret_cast<void*>(actor), index, hp, dead_state, local_hp, local_max,
        local_read ? "yes" : "no");
    g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        anomaly::sdk::StringView(message));
}

void ObserveFinalTowerHealthArray(const char* event, AnomalyGenerationHandleV1 controller,
                                 std::uintptr_t data, std::uintptr_t type, const char* field,
                                 const char* element_name) {
    struct Array { std::uintptr_t data; std::int32_t count, capacity; } array{};
    if (!ReadRowValue(data, type, field, "ArrayProperty", &array) || array.data == 0 ||
        array.count <= 0 || array.count > array.capacity || array.count > 64) return;
    const auto property = FindProperty(type, field);
    std::uintptr_t inner{}, element_type{};
    std::int32_t stride{};
    if (!ReadValue(property + g_layout.arrayInner, &inner) ||
        !DiagnosticStruct(inner, element_name, &element_type, &stride)) return;
    for (std::int32_t i = 0; i < array.count; ++i)
        ObserveFinalTowerHealth(event, controller, array.data + static_cast<std::size_t>(i) * stride,
            element_type, nullptr, true);
}

void ANOMALY_CALL ObserveFinalTowerEvent(void*, std::uintptr_t object,
                                       std::uintptr_t function, void* parameters) noexcept {
    try {
        if (parameters == nullptr || !g_observe_ft_health.load(std::memory_order_acquire)) return;
        std::uint32_t index{};
        if (!ReadValue(function + g_layout.objectInternalIndex, &index)) return;
        for (std::size_t i = 0; i < g_context.health_events.size(); ++i) {
            const auto expected = g_context.health_events[i];
            if (expected.id == 0 || index != ANOMALY_UE5_OBJECT_HANDLE_INDEX(expected)) continue;
            AnomalyGenerationHandleV1 actual{}, controller{};
            if (!HandleFromPointer(function, &actual) || actual.id != expected.id ||
                actual.generation != expected.generation || !HandleFromPointer(object, &controller)) return;
            const auto data = reinterpret_cast<std::uintptr_t>(parameters);
            if (i < 2) {
                ObserveFinalTowerHealth(i == 0 ? "restore" : "set-current", controller, data,
                    function, i == 0 ? "CurrentHp" : "NewHP", false);
            } else if (i == 2) {
                std::uintptr_t container{}, container_type{};
                if (!DiagnosticNestedStruct(data, function, "TargetDataContainer",
                        "ClientReplicatedTargetDataContainer", &container, &container_type)) return;
                ObserveFinalTowerHealthArray("fight", controller, container, container_type,
                    "ClientFightDataArray", "ClientRepFightData");
                ObserveFinalTowerHealthArray("fight-extra", controller, container, container_type,
                    "ClientExtraDamageInfos", "ClientRepExtraDamageInfo");
            } else {
                ObserveFinalTowerHealthArray("extra", controller, data, function,
                    "TargetDataArray", "ClientRepExtraDamageInfo");
            }
            return;
        }
    } catch (...) {}
}

void ResolveFinalTowerHealthEvents() {
    if (!g_context.health_event_lookup_pending) return;
    g_context.health_event_lookup_pending = false;
    constexpr std::array<const char*, 4> paths{
        "/Script/HTGame.HTPlayerController:ClientRestoreServerAbilityAttributes",
        "/Script/HTGame.HTPlayerController:ClientServerAbilityActorSetCurrentHP",
        "/Script/HTGame.HTPlayerController:ClientSetReplicatedTargetData",
        "/Script/HTGame.HTPlayerController:ClientUpdateTargetExtraDamageInfos"};
    std::array<AnomalyGenerationHandleV1, 4> events{};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (!FindHandle(paths[i], &events[i])) {
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView("SummonLab: FT health observer function unavailable"));
            return;
        }
    }
    g_context.health_events = events;
    g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        anomaly::sdk::StringView("SummonLab: passive FT health observer ready"));
}

void StartFinalTowerHealthObserver() {
    if (g_context.events == nullptr || g_context.events->subscribe == nullptr ||
        g_context.events->unsubscribe == nullptr || g_context.core == nullptr ||
        g_context.core->log == nullptr) return;
    const auto status = g_context.events->subscribe(g_context.events->user,
        ObserveFinalTowerEvent, nullptr, &g_context.health_observer);
    // Exact UObject lookup belongs to the next ready Game tick, not Start.
    g_context.health_event_lookup_pending = status.code == ANOMALY_STATUS_V1_OK;
    if (!g_context.health_event_lookup_pending)
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            anomaly::sdk::StringView("SummonLab: FT health observer unavailable"));
}

bool ReadNativeMirrorFloat(const MirrorEntry& entry, float* value,
                           bool require_positive = true) {
    std::uintptr_t data{};
    std::int32_t size{}, capacity{};
    std::memcpy(&data, entry.data() + g_layout.mirrorValueData, sizeof(data));
    std::memcpy(&size, entry.data() + g_layout.mirrorValueBytes, sizeof(size));
    std::memcpy(&capacity, entry.data() + g_layout.mirrorValueDataCapacity, sizeof(capacity));
    // 1.4's memory reader and 1.5's float NetSerializeItem/SerializeItem path
    // both consume four uncompressed bytes for this scalar.
    return entry[g_layout.mirrorValueCompressed] == 0 && size == sizeof(float) && capacity >= size &&
        ReadValue(data, value) && std::isfinite(*value) && (!require_positive || *value > 0.0F);
}

void ObserveFinalTowerMirror(std::uintptr_t ability_system, const char* name,
                            const MirrorEntry& entry) {
    float value{};
    if (!ReadNativeMirrorFloat(entry, &value, false)) return;
    std::uintptr_t actor{};
    {
        std::scoped_lock lock(g_local_birth_mutex);
        for (auto& binding : g_local_births) {
            if (binding.ability_system == ability_system && binding.mirror_observations < 12 &&
                binding.combat_config.config_name.starts_with("FT_")) {
                ++binding.mirror_observations;
                actor = binding.actor;
                break;
            }
        }
    }
    if (actor == 0 || g_context.core == nullptr || g_context.core->log == nullptr) return;
    char message[192]{};
    std::snprintf(message, sizeof(message), "SummonLab FT-health-mirror: actor=%p field=%s incoming=%.9g",
        reinterpret_cast<void*>(actor), name, value);
    g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        anomaly::sdk::StringView(message));
}

void CaptureNativeBirthMaximum(std::uintptr_t ability_system,
                              const MirrorEntry& entry) {
    std::scoped_lock lock(g_local_birth_mutex);
    for (auto& pending : g_birth_configurations) {
        if (pending.ability_system != ability_system) continue;
        pending.invalid_max_hp = !ReadNativeMirrorFloat(entry, &pending.native_max_hp);
        g_birth_configuration_ready.store(true, std::memory_order_release);
        break;
    }
}

void ConfigureNativeBirthsOnGameThread() {
    if (!g_birth_configuration_ready.exchange(false, std::memory_order_acq_rel)) return;
    std::vector<PendingBirthConfiguration> ready;
    {
        std::scoped_lock lock(g_local_birth_mutex);
        for (auto it = g_birth_configurations.begin(); it != g_birth_configurations.end();) {
            if (it->native_max_hp > 0.0F || it->invalid_max_hp) {
                ready.push_back(*it);
                it = g_birth_configurations.erase(it);
            } else if (!ProbeHandle(it->actor_handle)) {
                it = g_birth_configurations.erase(it);
            } else ++it;
        }
    }
    for (auto& pending : ready) {
        if (!ProbeHandle(pending.actor_handle)) continue;
        // InitialHPPercent is relative to the native registration's maximum,
        // not the locally overridden GAS maximum. Sending 1 would seed the
        // native HP ledger with millions of HP and every reply would clamp to
        // the custom cap until all that invisible HP had been exhausted.
        const float percent = pending.invalid_max_hp ? 0.0F : pending.exact_hp / pending.native_max_hp;
        pending.parameters.hp_percent = percent;
        const bool within_capacity = !pending.require_native_capacity ||
            (std::isfinite(percent) && percent > 0.0F && percent <= 1.0F);
        const bool configured = !pending.invalid_max_hp && within_capacity &&
            InvokeByPath(pending.controller_handle,
                "/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode",
                &pending.parameters, sizeof(pending.parameters));
        if (!configured) {
            static_cast<void>(InvokeByPath(pending.actor_handle, kPathDestroyActor, nullptr, 0));
            if (pending.require_native_capacity) {
                const auto message = g_context.localizer.Text("summon.compat_health_failed",
                    "Native source HP could not be synchronized; summon removed");
                std::scoped_lock lock(g_context.mutex);
                g_context.last_result_failed = true;
                std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s", message.c_str());
            }
        }
        if (g_context.core != nullptr) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                "SummonLab birth-configuration: nativeMax=%.3f requestedHP=%.3f initialHPPercent=%.9g submitted=%s",
                pending.native_max_hp, pending.exact_hp, percent, configured ? "yes" : "no");
            g_context.core->log(g_context.core->user, configured ? ANOMALY_CORE_LOG_LEVEL_V1_INFO :
                ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(message));
        }
    }
}

// Lifecycle-only state transfer: an actor survives a plugin DLL generation.
// Never adopt by class/name. Require the same process creation time and the
// engine's exact object serial/generation before restoring a local birth.
std::filesystem::path LocalBirthStatePath() {
    std::array<char, 4096> package{};
    std::size_t size = package.size();
    if (g_context.core == nullptr || g_context.core->plugin_directory == nullptr ||
        g_context.core->plugin_directory(g_context.core->user, package.data(), &size).code !=
            ANOMALY_STATUS_V1_OK) return {};
    const std::string utf8(package.data());
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())).parent_path() /
           L"summon-lab-local-births.json";
}

std::uint64_t ProcessCreationTime() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

void TransferLocalBirths(bool save) noexcept {
    try {
        const auto path = LocalBirthStatePath();
        const auto created = ProcessCreationTime();
        if (path.empty() || created == 0) return;
        if (save) {
            nlohmann::json bindings = nlohmann::json::array();
            {
                std::scoped_lock lock(g_local_birth_mutex);
                for (const auto& binding : g_local_births)
                    bindings.push_back({{"id", binding.actor_handle.id},
                        {"generation", binding.actor_handle.generation},
                        {"actor", binding.actor}, {"abilitySystem", binding.ability_system},
                        {"attributeSet", binding.attribute_set},
                        {"exactHP", binding.exact_hp},
                        {"combatConfig", binding.combat_config.config_name}});
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << nlohmann::json{{"schemaVersion", 1}, {"pid", GetCurrentProcessId()},
                {"processCreated", created}, {"bindings", std::move(bindings)}}.dump(2);
            return;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) return;
        const auto state = nlohmann::json::parse(file, nullptr, false);
        if (state.is_discarded() || state.value("schemaVersion", 0) != 1 ||
            state.value("pid", 0UL) != GetCurrentProcessId() ||
            state.value("processCreated", std::uint64_t{}) != created ||
            !state.contains("bindings") || !state["bindings"].is_array()) return;
        for (const auto& item : state["bindings"]) {
            LocalBirthBinding binding{{item.at("id").get<std::uint64_t>(),
                item.at("generation").get<std::uint64_t>()},
                item.at("actor").get<std::uintptr_t>(),
                item.at("abilitySystem").get<std::uintptr_t>(),
                item.value("attributeSet", static_cast<std::uintptr_t>(0)),
                item.value("exactHP", 0.0F)};
            binding.combat_config.config_name = item.value("combatConfig", std::string{});
            g_pending_local_births.push_back(std::move(binding));
        }
        g_restore_local_births.store(!g_pending_local_births.empty(), std::memory_order_release);
    } catch (...) {
        if (g_context.core != nullptr)
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView("SummonLab: local birth state transfer failed"));
    }
}

void RestoreLocalBirthsOnGameThread() {
    if (!g_restore_local_births.exchange(false, std::memory_order_acq_rel)) return;
    for (const auto& binding : g_pending_local_births) {
        AnomalyGenerationHandleV1 current{};
        std::uintptr_t asc{}, set{};
        if (IsActorAlive(binding.actor_handle, binding.actor) && HandleFromPointer(binding.actor, &current) &&
            current.id == binding.actor_handle.id &&
            current.generation == binding.actor_handle.generation &&
            ResolveActorAttributeSet(binding.actor, &asc, &set) && asc == binding.ability_system)
        {
            RegisterLocalBirth(current, binding.actor, asc, set, binding.exact_hp,
                PrepareFinalTowerConfig(binding.combat_config.config_name));
        }
    }
    g_pending_local_births.clear();
}

bool IsBirthAttributeSet(std::uintptr_t self, BirthInitialization& birth) {
    if (birth.attribute_set != 0) return self == birth.attribute_set;
    std::uintptr_t owner = self;
    for (int depth = 0; depth < 4; ++depth) {
        if (!ReadValue(owner + g_layout.objectOuter, &owner) || owner == 0)
            return false;
        std::uintptr_t owner_class{};
        if (!ReadValue(owner + g_layout.objectClass, &owner_class)) return false;
        if (owner_class != birth.actor_class) continue;
        std::uint32_t flags{};
        if (!ReadValue(owner + g_layout.objectFlags, &flags) || (flags & 0x30U) != 0) return false;
        birth.actor = owner;
        birth.attribute_set = self;
        return true;
    }
    return false;
}

void OverrideBirthBaseInput(std::uintptr_t container, const void* attribute, float& value) {
    auto* birth = g_birth_initialization;
    if (birth == nullptr || attribute == nullptr) return;

    std::uintptr_t property{};
    int32_t offset{};
    std::uint32_t name_id{};
    char name[kAttributeNameCapacity]{};
    if (!ReadValue(reinterpret_cast<std::uintptr_t>(attribute) +
                       g_layout.attributeProperty, &property) ||
        !IsGameplayAttributeProperty(property, &offset) ||
        !ReadValue(property + g_layout.fieldName, &name_id) ||
        !ResolveName(name_id, name, sizeof(name)))
        return;

    const auto preset = std::find_if(birth->attributes.begin(), birth->attributes.end(),
        [&](const BirthAttribute& input) { return input.name == name; });
    float requested{};
    if (preset != birth->attributes.end()) requested = preset->value;
    else if (birth->exact_hp > 0.0F &&
        (std::strcmp(name, "HPMaxUp") == 0 || std::strcmp(name, "HPMaxAdd") == 0 ||
         std::strcmp(name, "HPMaxUpTemp") == 0 || std::strcmp(name, "HPMaxAddTemp") == 0))
        requested = 0.0F;
    else if (birth->exact_hp > 0.0F && std::strcmp(name, "OnlineMaxHPRatio") == 0)
        requested = 1.0F;
    else return;

    // The container owns the ASC. Use its registered AttributeSets because
    // HTAttribute/OwnerAbilityCharacter can still be unset during construction.
    std::uintptr_t asc{};
    struct AttributeSets {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    } sets;
    if (!ReadValue(container + g_layout.containerOwner, &asc) || asc == 0 ||
        !ReadValue(asc + g_layout.spawnedAttributes, &sets) || sets.data == 0 ||
        sets.count <= 0 || sets.count > 64 || sets.capacity < sets.count)
        return;
    for (std::int32_t i = 0; i < sets.count; ++i) {
        std::uintptr_t set{}, set_class{};
        if (!ReadValue(sets.data + static_cast<std::uintptr_t>(i) * sizeof(set), &set) ||
            set == 0 || !ReadValue(set + g_layout.objectClass, &set_class) ||
            FindProperty(set_class, name) != property || !IsBirthAttributeSet(set, *birth))
            continue;
        value = requested;
        if (preset != birth->attributes.end()) preset->applied = true;
        return;
    }
}

void ANOMALY_CALL AttributeBaseInputDetour(void* self, const void* attribute,
                                         float new_base) noexcept {
    const auto original = reinterpret_cast<AttributeBaseInputFn>(g_context.attribute_original);
    AnomalyGenerationHandleV1 lease{};
    bool leased = false;
    try {
        if (g_context.hooks != nullptr && g_context.attribute_hook.id != 0)
            leased = g_context.hooks->begin_callback(g_context.hooks->user,
                         g_context.attribute_hook, &lease).code == ANOMALY_STATUS_V1_OK;
        if (leased && g_birth_thread.load(std::memory_order_acquire) == GetCurrentThreadId())
            OverrideBirthBaseInput(reinterpret_cast<std::uintptr_t>(self), attribute, new_base);
    } catch (...) {
    }
    // Supply the value before GAS stores the base and updates its aggregator.
    // Never override an evaluated CurrentValue: that leaves the original base
    // inside the aggregator and it resurfaces when damage causes reevaluation.
    try {
        if (original != nullptr) original(self, attribute, new_base);
    } catch (...) {
    }
    if (leased) {
        try { static_cast<void>(g_context.hooks->end_callback(g_context.hooks->user, lease)); }
        catch (...) {}
    }
}

bool ValidateSummonProfile(const anomaly::sdk::Profile& profile, summon_lab::NativeLayout* output) {
    if (profile.game != "nte" || profile.module != "HTGame.exe" ||
        profile.section != ".text" || profile.contract != "summon-lab-v2") return false;
    try {
        summon_lab::NativeLayout layout;
        if (!summon_lab::LoadNativeLayout(profile, &layout)) return false;
        static_cast<void>(profile.symbol("attribute-base-input"));
        static_cast<void>(profile.symbol("summon-registration"));
        static_cast<void>(profile.symbol("monster-default-level"));
        // This address comes from the validated object's vtable, not a global
        // signature scan: the short accessor body is not globally unique.
        const auto& getters = profile.symbol("mirror-owner-getter").candidates;
        if (getters.size() != 1 || getters.front().call_offset || getters.front().entry.empty() ||
            getters.front().entry.size() > 256) return false;
        for (const auto& candidate : profile.symbol("local-attribute-mirror").candidates) {
            const auto size = candidate.values.at("recordSize");
            const auto camps = candidate.values.at("campCount");
            const auto attributes = candidate.values.at("birthAttributeCount");
            // The palette search enumerates subsets; bound its work and keep
            // every 1U << camp_count operation within the unsigned bit width.
            if (!summon_lab::ValidMirrorRecord(layout, size) || camps < 2 || camps > 16 ||
                attributes == 0 || attributes > kBirthAttributeNames.size()) return false;
        }
        *output = layout;
        return true;
    } catch (const std::out_of_range&) {
        return false;
    }
}

bool InstallBirthAttributeHook(const anomaly::sdk::Host& query, const anomaly::sdk::Profile& profile) {
    const auto signature = query.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    g_context.hooks = query.Query<AnomalyHookServiceV1>(
        ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION).get();
    if (!signature || signature->resolve == nullptr || g_context.hooks == nullptr ||
        g_context.hooks->create == nullptr || g_context.hooks->release == nullptr ||
        g_context.hooks->begin_callback == nullptr || g_context.hooks->end_callback == nullptr)
        return false;
    const auto resolved = anomaly::sdk::ResolveProfileSymbol(
        signature.get(), g_context.core, profile, "attribute-base-input");
    if (!resolved) return false;
    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = resolved.address;
    request.detour = reinterpret_cast<void*>(&AttributeBaseInputDetour);
    request.label = anomaly::sdk::StringView("summon-birth-base-input");
    return g_context.hooks->create(g_context.hooks->user, &request,
        &g_context.attribute_original, &g_context.attribute_hook).code == ANOMALY_STATUS_V1_OK &&
        g_context.attribute_hook.id != 0 && g_context.attribute_original != 0;
}


// Scoped callbacks must finish before the plugin generation can be unloaded.
class LocalCombatLease {
public:
    explicit LocalCombatLease(std::size_t index) : hooks_(g_context.hooks) {
        if (hooks_ != nullptr && g_context.local_combat_hooks[index].handle.id != 0)
            active_ = hooks_->begin_callback(hooks_->user,
                g_context.local_combat_hooks[index].handle, &lease_).code == ANOMALY_STATUS_V1_OK;
    }
    ~LocalCombatLease() noexcept {
        if (active_) {
            try { static_cast<void>(hooks_->end_callback(hooks_->user, lease_)); }
            catch (...) {}
        }
    }
    explicit operator bool() const noexcept { return active_; }
private:
    const AnomalyHookServiceV1* hooks_{};
    AnomalyGenerationHandleV1 lease_{};
    bool active_{};
};

using SummonRegistrationFn = void (ANOMALY_CALL*)(void*, const void*, void*, std::int32_t);
void ANOMALY_CALL LocalRegistrationDetour(void* controller, const void* leader,
                                        void* class_cdo, std::int32_t index) noexcept {
    const auto original = reinterpret_cast<SummonRegistrationFn>(
        g_context.local_combat_hooks[0].original);
    try {
        LocalCombatLease lease(0);
        if (lease && g_birth_thread.load(std::memory_order_acquire) == GetCurrentThreadId()) {
            auto* birth = g_birth_initialization;
            std::uintptr_t uclass{};
            if (birth != nullptr && birth->actor_class != 0 && original != nullptr &&
                ReadValue(reinterpret_cast<std::uintptr_t>(class_cdo) + g_layout.objectClass, &uclass) &&
                uclass == birth->actor_class && index >= 0 && leader != nullptr) {
                // Preserve the exact native leader identity and summon index.
                // Configuration must use the same registration, never a fabricated actor ID.
                if (ReadBytes(reinterpret_cast<std::uintptr_t>(leader),
                        birth->registration_leader.data(), birth->registration_leader.size())) {
                    birth->registration_controller = reinterpret_cast<std::uintptr_t>(controller);
                    birth->registration_index = index;
                    // Only this explicit FT birth may borrow a server CDO.
                    // Keep the actor class, leader bytes and summon index intact.
                    if (birth->registration_cdo != 0) {
                        class_cdo = reinterpret_cast<void*>(birth->registration_cdo);
                        birth->registration_substituted = true;
                    }
                }
            }
        }
        if (original != nullptr) original(controller, leader, class_cdo, index);
    } catch (...) {}
}

AnomalyStatusV1 ReleaseLocalCombatHooks() {
    for (std::size_t i = g_context.local_combat_hooks.size(); i != 0; --i) {
        auto& hook = g_context.local_combat_hooks[i - 1];
        if (g_context.hooks != nullptr && hook.handle.id != 0) {
            const auto status = g_context.hooks->release(g_context.hooks->user, hook.handle);
            if (status.code != ANOMALY_STATUS_V1_OK &&
                status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
        }
        hook = {};
    }
    return anomaly::sdk::Ok();
}

// The native mirror deserializes cached fields after GAS has initialized them.
// Local custom births own their MaxHP caches; replicated players do not.
// Filter a borrowed local view before assignment, never rewrite HP or RPC data.
using LocalAttributeMirrorFn = void (ANOMALY_CALL*)(void*, void*, const void*, void*);
void ANOMALY_CALL PreserveLocalBirthMaxHP(void* context, void* owner,
                                       const void* record, void* scratch) noexcept {
    const auto original = reinterpret_cast<LocalAttributeMirrorFn>(g_context.local_combat_hooks[1].original);
    try {
        LocalCombatLease lease(1);
        // 1.5 adds GameplayModuleId to the end of the native container.
        alignas(void*) summon_lab::MirrorBuffer view{};
        std::vector<MirrorEntry> filtered;
        const void* input = record;
        try {
            std::uintptr_t target{};
            std::uintptr_t actor{}, current_ability_system{};
            bool protect_max{}, observe_ft{};
            AnomalyGenerationHandleV1 handle{};
            if (lease && record != nullptr &&
                ReadValue(reinterpret_cast<std::uintptr_t>(record) + g_layout.mirrorObject, &target)) {
                std::scoped_lock lock(g_local_birth_mutex);
                for (const auto& binding : g_local_births) {
                    if (binding.ability_system == target &&
                        (binding.exact_hp > 0.0F || binding.combat_config.config_name.starts_with("FT_"))) {
                        handle = binding.actor_handle;
                        actor = binding.actor;
                        protect_max = binding.exact_hp > 0.0F;
                        observe_ft = binding.combat_config.config_name.starts_with("FT_");
                        break;
                    }
                }
            }
            if (handle.id != 0 && ProbeHandle(handle) &&
                ReadValue(actor + g_layout.characterAbilitySystem, &current_ability_system) &&
                current_ability_system == target &&
                ReadBytes(reinterpret_cast<std::uintptr_t>(record), view.data(), g_context.mirror_record_size)) {
                std::uintptr_t vtable{}, getter{}, values{};
                std::int32_t count{}, capacity{};
                std::memcpy(&vtable, view.data() + g_layout.mirrorVtable, sizeof(vtable));
                std::memcpy(&values, view.data() + g_layout.mirrorValues, sizeof(values));
                std::memcpy(&count, view.data() + g_layout.mirrorValueCount, sizeof(count));
                std::memcpy(&capacity, view.data() + g_layout.mirrorValueCapacity, sizeof(capacity));
                const auto& getter_entry = g_context.mirror_getter_entry;
                std::array<std::uint8_t, 256> actual{};
                if (values != 0 && count > 0 && count <= capacity && count <= 512 &&
                    ReadValue(vtable + g_layout.mirrorGetterSlot, &getter) &&
                    ReadBytes(getter, actual.data(), getter_entry.size()) &&
                    std::equal(getter_entry.begin(), getter_entry.end(), actual.begin())) {
                    std::vector<MirrorEntry> entries(static_cast<std::size_t>(count));
                    if (ReadBytes(values, entries.data(), entries.size() * sizeof(entries[0]))) {
                        for (const auto& entry : entries) {
                            std::uint32_t id{};
                            char name[64]{};
                            std::memcpy(&id, entry.data() + g_layout.mirrorValueName, sizeof(id));
                            const bool named = ResolveName(id, name, sizeof(name));
                            const bool max_field = named &&
                                (std::strcmp(name, "MaxHP") == 0 || std::strcmp(name, "MaxHPTemp") == 0);
                            if (observe_ft && named && (max_field || std::strcmp(name, "HPCurrent") == 0))
                                ObserveFinalTowerMirror(target, name, entry);
                            const bool protected_max = protect_max && max_field;
                            if (protected_max && std::strcmp(name, "MaxHPTemp") == 0)
                                CaptureNativeBirthMaximum(target, entry);
                            if (!protected_max) filtered.push_back(entry);
                        }
                        if (filtered.size() != entries.size()) {
                            const auto pointer = reinterpret_cast<std::uintptr_t>(filtered.data());
                            const auto size = static_cast<std::int32_t>(filtered.size());
                            std::memcpy(view.data() + g_layout.mirrorValues, &pointer, sizeof(pointer));
                            std::memcpy(view.data() + g_layout.mirrorValueCount, &size, sizeof(size));
                            std::memcpy(view.data() + g_layout.mirrorValueCapacity, &size, sizeof(size));
                            input = view.data();
                        }
                    }
                }
            }
        } catch (...) { input = record; }
        if (original != nullptr) original(context, owner, input, scratch);
    } catch (...) {}
}

bool InstallLocalCombatHooks(const anomaly::sdk::Host& query, const anomaly::sdk::Profile& profile) {
    const auto signature = query.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    if (!signature || signature->resolve == nullptr || g_context.hooks == nullptr ||
        g_context.hooks->create == nullptr || g_context.hooks->release == nullptr ||
        g_context.hooks->begin_callback == nullptr || g_context.hooks->end_callback == nullptr)
        return false;
    constexpr std::array<std::string_view, 2> names{
        "summon-registration", "local-attribute-mirror"};
    const auto registration = anomaly::sdk::ResolveProfileSymbol(
        signature.get(), g_context.core, profile, names[0]);
    const auto mirror = anomaly::sdk::ResolveProfileSymbol(
        signature.get(), g_context.core, profile, names[1]);
    if (!registration || !mirror) return false;
    const std::array<std::uintptr_t, 2> targets{registration.address, mirror.address};
    g_context.mirror_record_size = mirror.candidate->values.at("recordSize");
    g_context.camp_count = mirror.candidate->values.at("campCount");
    g_context.available_birth_attributes = mirror.candidate->values.at("birthAttributeCount");
    g_context.mirror_getter_entry = profile.symbol("mirror-owner-getter").candidates.front().entry;

    // Resolve and validate everything before publishing either callback.
    const std::array<void*, 2> detours{
        reinterpret_cast<void*>(&LocalRegistrationDetour),
        reinterpret_cast<void*>(&PreserveLocalBirthMaxHP)};
    for (std::size_t i = 0; i < names.size(); ++i) {
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION;
        request.target = targets[i];
        request.detour = detours[i];
        request.label = anomaly::sdk::StringView(names[i]);
        auto& hook = g_context.local_combat_hooks[i];
        if (g_context.hooks->create(g_context.hooks->user, &request, &hook.original,
                &hook.handle).code != ANOMALY_STATUS_V1_OK || hook.handle.id == 0 ||
                hook.original == 0) {
            static_cast<void>(ReleaseLocalCombatHooks());
            return false;
        }
    }
    return true;
}

class BirthScope {
public:
    explicit BirthScope(BirthInitialization& birth) {
        g_birth_initialization = &birth;
        g_birth_thread.store(GetCurrentThreadId(), std::memory_order_release);
    }
    ~BirthScope() {
        g_birth_thread.store(0, std::memory_order_release);
        g_birth_initialization = nullptr;
    }
};

bool DismissSummoned(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer,
                     float previous_max_hp = 0.0F);
void RefreshHealth();

// ---------------------------------------------------------------- 召唤

struct SummonResult {
    bool ok{};
    bool pet{};
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    std::uintptr_t ability_system{};
    std::uintptr_t attribute_set{};
    SummonIdentity registration;
    std::string server_combat_source;
    std::vector<AttributeValue> attributes;
    char label[96]{};
    char detail[kTextCapacity]{};
};

bool UnregisterNativeSummon(const SummonIdentity& identity) {
    if (identity.controller.id == 0 || identity.index < 0) return true;
    // The native opponent-summoner explicitly sends this before destroying
    // its actor. DestroyActor alone does not release the hybrid combat entry.
    // Use the captured identity even after the actor has died or disappeared.
    struct {
        NativeIdentity leader;
        std::int32_t index;
    } parameters{identity.leader, identity.index};
    static_assert(sizeof(parameters) == 44);
    const bool released = InvokeByPath(identity.controller,
        "/Script/HTGame.HTPlayerController:ServerDismissSummonedAI_HybridMode",
        &parameters, sizeof(parameters));
    if (g_context.core != nullptr) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "SummonLab native-dismiss: index=%d submitted=%s",
            identity.index, released ? "yes" : "no");
        g_context.core->log(g_context.core->user, released ? ANOMALY_CORE_LOG_LEVEL_V1_INFO :
            ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(message));
    }
    return released;
}

struct SummonFailureCleanup {
    const SummonResult& result;
    ~SummonFailureCleanup() noexcept {
        if (result.ok) return;
        try {
            ForgetLocalBirth(result.handle);
            static_cast<void>(UnregisterNativeSummon(result.registration));
        } catch (...) {}
    }
};

std::string MonsterConfigName(std::string_view path) {
    // SummonAI derives its config ID from the complete class name without _C.
    // Keep quest/difficulty suffixes: variants do not share the same config row.
    const auto start = path.find('/');
    if (start == std::string_view::npos) return {};
    path.remove_prefix(start);
    path = path.substr(0, path.find_first_of(":' \t\r\n"));
    const auto separator = path.find_last_of("/.");
    auto name = path.substr(separator + 1);
    const auto starts_with = [](std::string_view value, std::string_view prefix) {
        return value.size() >= prefix.size() &&
            _strnicmp(value.data(), prefix.data(), prefix.size()) == 0;
    };
    // FinalTower bosses inherit BossCharacterBase but live under Maps_4N.
    // Their FT-prefixed config IDs belong to the scene's own monster table.
    if (!starts_with(path, "/Game/Blueprints/Character/Monster/") &&
        !starts_with(path, "/Game/Maps_4N/Blueprints/Character/Monster/") &&
        !starts_with(name, "Boss_") && !starts_with(name, "mon_")) return {};
    if (name.size() >= 2 && _strnicmp(name.data() + name.size() - 2, "_C", 2) == 0)
        name.remove_suffix(2);
    return std::string(name);
}

bool MakeConfigName(std::string_view config_name, std::uint64_t* result) {
    AnomalyGenerationHandleV1 string_library{};
    if (!FindHandle("/Script/Engine.Default__KismetStringLibrary", &string_library)) return false;
    // POD parameters from the supplied SDK. Native FName conversion preserves
    // numeric suffixes; searching the name pool alone loses FName::Number.
    std::array<wchar_t, kPathCapacity> wide_name{};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        config_name.data(), static_cast<int>(config_name.size()), wide_name.data(),
        static_cast<int>(wide_name.size() - 1));
    if (count == 0) return false;
    struct NameParameters {
        const wchar_t* data;
        std::int32_t count;
        std::int32_t capacity;
        std::uint64_t result{};
    } name{wide_name.data(), count + 1, count + 1};
    static_assert(sizeof(NameParameters) == 24);
    if (!InvokeByPath(string_library, "/Script/Engine.KismetStringLibrary:Conv_StringToName",
                      &name, sizeof(name))) return false;
    *result = name.result;
    return true;
}

std::uintptr_t ResolveMonsterLevelQuery(
    const anomaly::sdk::Host& query, const anomaly::sdk::Profile& profile) {
    const auto signature = query.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    // Read-only query taking HTPlayerState*, UWorld* and an eight-byte FName.
    return anomaly::sdk::ResolveProfileSymbol(
        signature.get(), g_context.core, profile, "monster-default-level").address;
}

std::uint32_t ResolveDuelChaseRange(const anomaly::sdk::Host& query,
                                    const anomaly::sdk::Profile& profile) {
    const auto signature = query.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    const auto resolved = anomaly::sdk::ResolveProfileSymbol(
        signature.get(), g_context.core, profile, "duel-current-chase-range");
    if (!resolved) return 0;
    try {
        const auto offset = resolved.candidate->values.at("currentChaseRange");
        return offset > 0 && offset <= 16384 && offset % alignof(float) == 0 ? offset : 0;
    } catch (const std::out_of_range&) { return 0; }
}

// FinalTower separates scene monster rows from its own property-pack provider.
// Borrow those native rows only for our scoped queries and registered summons.
// Keep their owning assets rooted until all native callbacks have drained.
struct FinalTowerRuntime {
    struct Root {
        AnomalyGenerationHandleV1 handle{};
        std::uintptr_t object{};
        std::uintptr_t item{};
        bool owned{};
    };
    std::uintptr_t objects{};
    std::uintptr_t set_flags{};
    std::uintptr_t pack_query{};
    std::uintptr_t scene{};
    std::uintptr_t data{};
    std::uintptr_t monster_struct{};
    std::uintptr_t pack_struct{};
    std::uint32_t count_offset{}, chunk_size{}, item_size{}, object_offset{}, flags_offset{}, root_flag{};
    std::vector<Root> roots;
    bool ready{};
} g_final_tower;

std::atomic<DWORD> g_config_thread{0};
const CombatConfigSource* g_scoped_config{};

class CombatConfigScope {
public:
    explicit CombatConfigScope(const CombatConfigSource& source) {
        g_scoped_config = &source;
        g_config_thread.store(GetCurrentThreadId(), std::memory_order_release);
    }
    ~CombatConfigScope() {
        g_config_thread.store(0, std::memory_order_release);
        g_scoped_config = nullptr;
    }
};

std::uintptr_t FindLocalCombatConfig(std::uint64_t name, bool pack) {
    const auto matches = [&](const CombatConfigSource& config) {
        return name != 0 && name == (pack ? config.pack : config.config);
    };
    if (g_config_thread.load(std::memory_order_acquire) == GetCurrentThreadId() &&
        g_scoped_config != nullptr && matches(*g_scoped_config))
        return pack ? g_scoped_config->property_pack : g_scoped_config->row;
    std::scoped_lock lock(g_local_birth_mutex);
    for (const auto& binding : g_local_births) {
        if (matches(binding.combat_config))
            return pack ? binding.combat_config.property_pack : binding.combat_config.row;
    }
    return 0;
}

using MonsterRowFn = std::uintptr_t (ANOMALY_CALL*)(std::uintptr_t, std::uint64_t);
using PropertyPackFn = std::uintptr_t (ANOMALY_CALL*)(std::uintptr_t, std::uint64_t, int, std::uintptr_t);

std::uintptr_t ANOMALY_CALL FinalTowerMonsterRow(std::uintptr_t scene, std::uint64_t name) noexcept {
    try {
        LocalCombatLease lease(2);
        const auto original = reinterpret_cast<MonsterRowFn>(g_context.local_combat_hooks[2].original);
        const auto row = original != nullptr ? original(scene, name) : 0;
        return row != 0 || !lease ? row : FindLocalCombatConfig(name, false);
    } catch (...) { return 0; }
}

std::uintptr_t ANOMALY_CALL FinalTowerPropertyPack(std::uintptr_t data, std::uint64_t name,
                                                int kind, std::uintptr_t context) noexcept {
    try {
        LocalCombatLease lease(3);
        const auto original = reinterpret_cast<PropertyPackFn>(g_context.local_combat_hooks[3].original);
        const auto pack = original != nullptr ? original(data, name, kind, context) : 0;
        return pack != 0 || kind != 1 || !lease ? pack : FindLocalCombatConfig(name, true);
    } catch (...) { return 0; }
}

void ReleaseFinalTowerData() {
    for (const auto& root : g_final_tower.roots) {
        std::uintptr_t object{};
        if (root.owned && ProbeHandle(root.handle) &&
            ReadValue(root.item + g_final_tower.object_offset, &object) &&
            object == root.object) {
            // The native setter registers the GC barrier; removing our root is
            // an atomic flag clear after the hooks have stopped borrowing rows.
            InterlockedAnd(reinterpret_cast<volatile LONG*>(root.item + g_final_tower.flags_offset),
                           ~static_cast<LONG>(g_final_tower.root_flag));
        }
    }
    g_final_tower = {};
}

bool PinFinalTowerAsset(std::uintptr_t object) {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t chunks{}, chunk{}, actual{};
    std::int32_t count{};
    if (!HandleFromPointer(object, &handle) ||
        !ReadValue(g_final_tower.objects, &chunks) || chunks == 0 ||
        !ReadValue(g_final_tower.objects + g_final_tower.count_offset, &count)) return false;
    const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
    if (count <= 0 || index >= static_cast<std::uint32_t>(count) ||
        !ReadValue(chunks + sizeof(std::uintptr_t) * (index / g_final_tower.chunk_size), &chunk) ||
        chunk == 0) return false;
    const auto item = chunk + g_final_tower.item_size * static_cast<std::uintptr_t>(
        index % g_final_tower.chunk_size);
    std::uint32_t flags{};
    if (!ReadValue(item + g_final_tower.object_offset, &actual) || actual != object ||
        !ReadValue(item + g_final_tower.flags_offset, &flags)) return false;
    const bool owned = (flags & g_final_tower.root_flag) == 0;
    // Allocate the ledger entry before changing the engine's root flag.
    g_final_tower.roots.push_back({handle, object, item, owned});
    if (owned) {
        using SetFlags = std::uint8_t (ANOMALY_CALL*)(std::uintptr_t, std::int32_t);
        static_cast<void>(reinterpret_cast<SetFlags>(g_final_tower.set_flags)(
            item, static_cast<std::int32_t>(g_final_tower.root_flag)));
    }
    return ReadValue(item + g_final_tower.flags_offset, &flags) &&
           (flags & g_final_tower.root_flag) != 0;
}

bool LoadFinalTowerData() {
    if (!g_final_tower.ready || g_context.final_tower_scene_asset.empty()) return false;
    if (g_final_tower.scene != 0) return true;
    // A failed partial load is kept for orderly release, never repeatedly rooted.
    if (!g_final_tower.roots.empty()) return false;
    AnomalyGenerationHandleV1 util{}, data_library{};
    if (!FindHandle("/Script/HTGame.Default__HTUtil", &util) ||
        !FindHandle("/Script/HTGame.Default__HTGameData", &data_library)) return false;
    std::array<wchar_t, kPathCapacity> path{};
    const auto& asset = g_context.final_tower_scene_asset;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, asset.data(),
        static_cast<int>(asset.size()), path.data(), static_cast<int>(path.size() - 1));
    if (length == 0) return false;
    struct LoadParameters {
        const wchar_t* data;
        std::int32_t count, capacity;
        std::uintptr_t result{};
    } load{path.data(), length + 1, length + 1};
    static_assert(sizeof(LoadParameters) == 24);
    std::uintptr_t ft_data{};
    if (!InvokeByPath(util, "/Script/HTGame.HTUtil:LoadObjectByPath", &load, sizeof(load)) ||
        load.result == 0 || !PinFinalTowerAsset(load.result) ||
        !InvokeByPath(data_library, "/Script/HTGame.HTGameData:GetFinalTowerDataAsset",
                      &ft_data, sizeof(ft_data)) || ft_data == 0 || !PinFinalTowerAsset(ft_data)) return false;
    struct TableArray { std::uintptr_t data; std::int32_t count, capacity; } tables{};
    std::uintptr_t monster_table{}, pack_table{}, monster_struct{}, pack_struct{};
    if (!ReadReflectedValue(load.result, "MonsterArrayDataTable", "ArrayProperty", &tables) ||
        tables.count != 1 || tables.capacity < tables.count || tables.data == 0 ||
        !ReadValue(tables.data, &monster_table) ||
        !ReadReflectedValue(monster_table, "RowStruct", "ObjectProperty", &monster_struct) ||
        !ReadReflectedValue(ft_data, "MonsterFTPackDataTable", "ObjectProperty", &pack_table) ||
        !ReadReflectedValue(pack_table, "RowStruct", "ObjectProperty", &pack_struct)) return false;
    std::uint32_t monster_name{}, pack_name{};
    char monster_type[64]{}, pack_type[64]{};
    if (!ReadValue(monster_struct + g_layout.objectName, &monster_name) ||
        !ReadValue(pack_struct + g_layout.objectName, &pack_name) ||
        !ResolveName(monster_name, monster_type, sizeof(monster_type)) ||
        !ResolveName(pack_name, pack_type, sizeof(pack_type)) ||
        std::strcmp(monster_type, "MonsterStaticData") != 0 ||
        std::strcmp(pack_type, "PackIDStaticData") != 0) return false;
    g_final_tower.scene = load.result;
    g_final_tower.data = ft_data;
    g_final_tower.monster_struct = monster_struct;
    g_final_tower.pack_struct = pack_struct;
    return true;
}

CombatConfigSource PrepareFinalTowerConfig(std::string_view config_name) {
    CombatConfigSource source;
    if (!config_name.starts_with("FT_") || !LoadFinalTowerData()) return source;
    source.config_name = config_name;
    const auto alias = g_context.final_tower_aliases.find(config_name);
    source.row_name = alias == g_context.final_tower_aliases.end() ? source.config_name : alias->second;
    std::uint64_t row_name{};
    if (!MakeConfigName(source.config_name, &source.config) ||
        !MakeConfigName(source.row_name, &row_name)) return {};
    const auto query = reinterpret_cast<MonsterRowFn>(g_context.local_combat_hooks[2].original);
    source.row = query(g_final_tower.scene, row_name);
    if (source.row == 0 ||
        !ReadRowValue(source.row, g_final_tower.monster_struct, "PropModifyID", "NameProperty", &source.pack) ||
        !ReadRowValue(source.row, g_final_tower.monster_struct, "MonsterLevel", "IntProperty", &source.level) ||
        source.pack == 0 || source.level <= 0) return {};
    source.property_pack = reinterpret_cast<MonsterRowFn>(g_final_tower.pack_query)(g_final_tower.data, source.pack);
    float hp{};
    if (source.property_pack == 0 ||
        !ReadRowValue(source.property_pack, g_final_tower.pack_struct, "HPMaxBase", "FloatProperty", &hp) ||
        !std::isfinite(hp) || hp <= 0) return {};
    return source;
}

bool InstallFinalTowerHooks(const anomaly::sdk::Host& host, const anomaly::sdk::Profile& profile) {
    const auto signature = host.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    if (!signature || g_context.hooks == nullptr) return false;
    try {
        const auto& layout = profile.layout;
        const auto count_offset = layout.at("objectArrayCount");
        const auto chunk_size = layout.at("objectsPerChunk");
        const auto item_size = layout.at("objectItemSize");
        const auto object_offset = layout.at("objectItemObject");
        const auto flags_offset = layout.at("objectItemFlags");
        // The native setter and atomic clear operate on the same aligned flag
        // word; the object identity must occupy a separate field in that item.
        if (count_offset < sizeof(std::uintptr_t) || count_offset > 128 ||
            count_offset % alignof(std::int32_t) != 0 || chunk_size == 0 ||
            chunk_size > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            item_size > 256 || item_size % alignof(std::uintptr_t) != 0 ||
            object_offset % alignof(std::uintptr_t) != 0 || flags_offset % alignof(LONG) != 0 ||
            !summon_lab::ValidBuffer(item_size, {{object_offset, sizeof(std::uintptr_t)},
                {flags_offset, sizeof(LONG)}}) || layout.at("objectRootFlag") != 0x40000000) return false;
        const std::array<std::string_view, 5> names{"monster-scene-row", "monster-property-pack",
            "final-tower-property-pack", "object-item-set-flags", "object-array-reference"};
        std::array<std::uintptr_t, 5> addresses{};
        std::uint32_t displacement_offset{}, instruction_size{};
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto resolved = anomaly::sdk::ResolveProfileSymbol(signature.get(), g_context.core, profile, names[i]);
            if (!resolved) return false;
            addresses[i] = resolved.address;
            if (i == 4) {
                displacement_offset = resolved.candidate->values.at("displacementOffset");
                instruction_size = resolved.candidate->values.at("instructionSize");
                if (displacement_offset != 3 || instruction_size != 7) return false;
            }
        }
        // Validated MOV RAX,[RIP+disp32] entry, not a callable function entry.
        std::int32_t displacement{};
        if (!ReadValue(addresses[4] + displacement_offset, &displacement)) return false;
        g_final_tower.objects = addresses[4] + instruction_size + displacement;
        g_final_tower.set_flags = addresses[3];
        g_final_tower.pack_query = addresses[2];
        g_final_tower.count_offset = count_offset;
        g_final_tower.chunk_size = chunk_size;
        g_final_tower.item_size = item_size;
        g_final_tower.object_offset = object_offset;
        g_final_tower.flags_offset = flags_offset;
        g_final_tower.root_flag = layout.at("objectRootFlag");
        const std::array<void*, 2> callbacks{reinterpret_cast<void*>(&FinalTowerMonsterRow),
                                           reinterpret_cast<void*>(&FinalTowerPropertyPack)};
        for (std::size_t i = 0; i < callbacks.size(); ++i) {
            AnomalyHookRequestV1 request{sizeof(request)};
            request.kind = ANOMALY_HOOK_V1_FUNCTION;
            request.target = addresses[i];
            request.detour = callbacks[i];
            request.label = anomaly::sdk::StringView(names[i]);
            auto& hook = g_context.local_combat_hooks[i + 2];
            if (g_context.hooks->create(g_context.hooks->user, &request, &hook.original, &hook.handle).code !=
                ANOMALY_STATUS_V1_OK || hook.handle.id == 0 || hook.original == 0) return false;
        }
        g_final_tower.ready = true;
        return true;
    } catch (const std::out_of_range&) { return false; }
}

bool ResolveSummonLevel(AnomalyGenerationHandleV1 pawn, std::string_view path,
                        std::uint32_t requested, std::int32_t* level) {
    *level = static_cast<std::int32_t>(std::min(requested, kMaxLevel));
    if (*level > 0) return true;
    const auto config_name = MonsterConfigName(path);
    if (config_name.empty()) return true;
    if (g_context.monster_level_query == 0) return false;

    std::uint64_t config{};
    std::uintptr_t player_state{};
    AnomalyGenerationHandleV1 ht_util{};
    if (!MakeConfigName(config_name, &config) ||
        !FindHandle("/Script/HTGame.Default__HTUtil", &ht_util) ||
        !InvokeByPath(pawn, "/Script/HTGame.HTCharacter:GetHTPlayerState",
                      &player_state, sizeof(player_state)) || player_state == 0) return false;
    // PlayerState is an Actor in the same world as the summoning player.
    // Use the reflected getter instead of reading World/PlayerState offsets.
    std::array<std::uintptr_t, 2> world{player_state, 0};
    if (!InvokeByPath(ht_util, "/Script/HTGame.HTUtil:K2_GetWorld",
                      world.data(), sizeof(world)) || world[1] == 0) return false;
    using Query = std::int32_t (ANOMALY_CALL*)(std::uintptr_t, std::uintptr_t, std::uint64_t);
    *level = reinterpret_cast<Query>(g_context.monster_level_query)(player_state, world[1], config);
    if (*level <= 0) return false;
    if (g_context.core != nullptr && g_context.core->log != nullptr) {
        char detail[kPathCapacity + 64]{};
        std::snprintf(detail, sizeof(detail), "SummonLab default-level: %s level=%d",
                      config_name.c_str(), *level);
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                            anomaly::sdk::StringView(detail));
    }
    return true;
}

BlueprintConfigState CheckBlueprintConfig(std::string_view path, CombatConfigSource* source = nullptr,
                                         AnomalyGenerationHandleV1 pawn = {}, float* default_hp = nullptr) {
    if (default_hp != nullptr) *default_hp = 0.0F;
    const auto config_name = MonsterConfigName(path);
    if (config_name.empty()) return BlueprintConfigState::NotApplicable;

    const auto prepared = PrepareFinalTowerConfig(config_name);
    if (source != nullptr) *source = prepared;
    CombatConfigScope config_scope(prepared);
    AnomalyGenerationHandleV1 ht_util{};
    std::uint64_t config{};
    if ((pawn.id == 0 && !FindLocalPawn(&pawn)) || !MakeConfigName(config_name, &config) ||
        !FindHandle("/Script/HTGame.Default__HTUtil", &ht_util))
        return BlueprintConfigState::Unavailable;

    struct HealthParameters {
        std::uintptr_t world_context{};
        std::uint64_t config_name{};
        float result{};
    } health;
    static_assert(sizeof(HealthParameters) == 24);
    if (!InvokeByPath(pawn, "/Script/Engine.Actor:GetLevel",
                      &health.world_context, sizeof(health.world_context)) ||
        health.world_context == 0) return BlueprintConfigState::Unavailable;
    health.config_name = config;
    // Verified against the running 1.4b: this queries the current scene's
    // monster row and property pack, including world-level overrides. It does
    // not load the selected class or return caller-owned UE allocations.
    if (!InvokeByPath(ht_util, "/Script/HTGame.HTUtil:GetMonsterStaticHPMaxByBPName",
                      &health, sizeof(health)) || !std::isfinite(health.result))
        return BlueprintConfigState::Unavailable;
    if (default_hp != nullptr) *default_hp = health.result;
    if (g_context.core != nullptr && g_context.core->log != nullptr) {
        char detail[kPathCapacity + 64]{};
        std::snprintf(detail, sizeof(detail), "SummonLab preflight: %s defaultHP=%.9g",
                      config_name.c_str(), static_cast<double>(health.result));
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                            anomaly::sdk::StringView(detail));
    }
    // Zero also covers intentionally noncombat variants and unavailable scene
    // data. It is a hint, not proof that loading or spawning a class is safe.
    return health.result > 0 ? BlueprintConfigState::Available : BlueprintConfigState::NoHealth;
}

void UpdateBlueprintConfig(double delta_seconds) {
    std::string path;
    {
        std::scoped_lock lock(g_context.mutex);
        auto& check = g_context.blueprint_config;
        if (check.path != g_context.class_path.data()) {
            check = {g_context.class_path.data(), BlueprintConfigState::Pending};
            // Do not create FNames for every intermediate keystroke.
            g_context.blueprint_check_delay = 0.25;
            return;
        }
        if (check.state != BlueprintConfigState::Pending || check.path.empty()) return;
        g_context.blueprint_check_delay -= delta_seconds;
        if (g_context.blueprint_check_delay > 0) return;
        path = check.path;
    }
    CombatConfigSource source;
    const auto state = CheckBlueprintConfig(path, &source);
    std::scoped_lock lock(g_context.mutex);
    // A render callback may have selected another blueprint during the query.
    if (path == g_context.class_path.data()) {
        g_context.blueprint_config.state = state;
        g_context.blueprint_config.source = source.row_name;
    }
}

std::string BirthPresetError(const std::array<BirthPreset, kBirthAttributeNames.size()>& presets) {
    const auto& localizer = g_context.localizer;
    const auto& max_hp = presets[kMaxHpAttribute];
    if (max_hp.enabled && (!std::isfinite(max_hp.value) || max_hp.value <= 0.0 ||
        max_hp.value > 10000000.0 || static_cast<float>(max_hp.value) <= 0.0F)) {
        return localizer.Text("settings.invalid_hp", "HPMaxBase must be greater than 0 and at most 10000000");
    }
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (!presets[i].enabled) continue;
        const auto name = kBirthAttributeNames[i];
        const std::array<std::string_view, 1> args{name};
        if (!std::isfinite(presets[i].value) ||
            std::abs(presets[i].value) > std::numeric_limits<float>::max()) {
            return localizer.Format("settings.invalid_value", "{0} must be a finite float value", args);
        }
        if (max_hp.enabled && (name == "HPMaxUp" || name == "HPMaxAdd" ||
            name == "HPMaxUpTemp" || name == "HPMaxAddTemp" || name == "OnlineMaxHPRatio")) {
            return localizer.Format("settings.hp_conflict", "Disable {0} when setting HPMaxBase", args);
        }
    }
    return {};
}

std::uintptr_t LoadServerCombatCdo(AnomalyGenerationHandleV1 library, const std::string& path) {
    if (g_layout.classDefaultObject == 0 || path.size() >= kPathCapacity) return 0;
    summon_lab::LoadClass load{};
    if (!BuildSoftClassRef(library, path.c_str(), &load.input) ||
        !InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking",
                      &load, sizeof(load))) return 0;
    const auto uclass = load.result;
    std::uintptr_t cdo{}, cdo_class{};
    std::uint32_t flags{};
    AnomalyGenerationHandleV1 handle{};
    // ClassDefaultObject is profile-validated; require a real CDO of this exact
    // loaded class. Neither the class nor its defaults are ever modified.
    if (uclass == 0 || !ReadValue(uclass + g_layout.classDefaultObject, &cdo) ||
        cdo == 0 || !ReadValue(cdo + g_layout.objectClass, &cdo_class) || cdo_class != uclass ||
        !ReadValue(cdo + g_layout.objectFlags, &flags) || (flags & 0x10U) == 0 ||
        !HandleFromPointer(cdo, &handle)) return 0;
    return cdo;
}

SummonResult SummonOne() {
    SummonResult result;
    {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.record_count >= kMaxRecords) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: record limit reached");
            return result;
        }
    }

    AnomalyGenerationHandleV1 pawn{};
    if (!FindLocalPawn(&pawn)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: no local pawn");
        return result;
    }

    char path[kPathCapacity];
    {
        std::scoped_lock lock(g_context.mutex);
        std::memcpy(path, g_context.class_path.data(), sizeof(path));
        path[sizeof(path) - 1] = '\0';
    }
    if (path[0] == '\0') {
        std::snprintf(result.detail, sizeof(result.detail), "summon: empty class path");
        return result;
    }
    AnomalyGenerationHandleV1 lib_cdo{};
    if (!FindHandle(kGameplayStaticsCdo, &lib_cdo)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: GameplayStatics CDO missing");
        return result;
    }

    summon_lab::SoftClassRef soft_class{};
    if (!BuildSoftClassRef(lib_cdo, path, &soft_class)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: soft class path failed");
        return result;
    }

    summon_lab::Transform transform{};
    if (!InvokeByPath(pawn, kPathGetTransform, &transform, sizeof(transform))) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: GetTransform failed");
        return result;
    }

    std::uint32_t level{};
    std::uint32_t skill_level{};
    bool hostile{};
    bool pet{};
    bool aggressive{};
    bool compatibility{};
    std::string server_combat_blueprint;
    std::array<char, kDisplayNameCapacity> display_name{};
    std::array<BirthPreset, kBirthAttributeNames.size()> presets{};
    {
        std::scoped_lock lock(g_context.mutex);
        level = g_context.level;
        skill_level = g_context.skill_level;
        hostile = g_context.hostile != 0;
        pet = g_context.pet != 0;
        aggressive = g_context.aggressive != 0;
        compatibility = g_context.ft_compatibility != 0 && MonsterConfigName(path).starts_with("FT_");
        if (compatibility)
            server_combat_blueprint = g_context.final_tower_combat_blueprint;
        display_name = g_context.display_name;
        presets = g_context.birth_presets;
    }

    CombatConfigSource combat_config;
    const auto config_state = CheckBlueprintConfig(path, &combat_config, pawn);
    if (config_state == BlueprintConfigState::NoHealth) {
        const auto message = g_context.localizer.Text("summon.config_missing",
            "Combat config missing in this scene; HP overrides cannot initialize it");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }
    if (compatibility) {
        float source_hp{};
        if (server_combat_blueprint.empty() || g_layout.classDefaultObject == 0 ||
            MonsterConfigName(server_combat_blueprint).starts_with("FT_") ||
            CheckBlueprintConfig(server_combat_blueprint, nullptr, pawn, &source_hp) !=
                BlueprintConfigState::Available) {
            const auto message = g_context.localizer.Text("summon.compat_source_unavailable",
                "Server combat source unavailable in this scene");
            std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
            return result;
        }
        auto& hp = presets[kMaxHpAttribute];
        if (!hp.enabled) hp = {1, source_hp};
        if (hp.value > source_hp) {
            char capacity[48]{};
            std::snprintf(capacity, sizeof(capacity), "%.9g", source_hp);
            const std::array<std::string_view, 1> args{capacity};
            const auto message = g_context.localizer.Format("summon.compat_hp_limit",
                "Compatibility HP must not exceed the source maximum {0}", args);
            std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
            return result;
        }
        result.server_combat_source = MonsterConfigName(server_combat_blueprint);
    }
    CombatConfigScope config_scope(combat_config);
    std::int32_t ai_level{};
    if (!ResolveSummonLevel(pawn, path, level, &ai_level)) {
        const auto message = g_context.localizer.Text("summon.level_unavailable",
            "Default level unavailable; enter a level");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }

    BirthInitialization birth;
    const auto preset_error = BirthPresetError(presets);
    if (!preset_error.empty()) {
        std::snprintf(result.detail, sizeof(result.detail), "%s", preset_error.c_str());
        return result;
    }
    // Resolve exact HP first; the native registration and GAS base still share
    // this one input, independently of the order of the other presets.
    const auto& hp_preset = presets[kMaxHpAttribute];
    if (hp_preset.enabled) {
        birth.exact_hp = static_cast<float>(hp_preset.value);
    }
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (!presets[i].enabled || i == kMaxHpAttribute) continue;
        const auto name = kBirthAttributeNames[i];
        const double input = presets[i].value;
        birth.attributes.push_back(BirthAttribute{std::string(name), static_cast<float>(input)});
    }
    if (birth.exact_hp > 0)
        birth.attributes.push_back(BirthAttribute{"HPMaxBase", birth.exact_hp});
    // Local combat identity is independent of optional attribute overrides.
    // Resolve the exact class even when using the boss's native attributes.
    {
        if (!g_context.birth_attributes_ready.load(std::memory_order_acquire)) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: native local combat initializer unavailable");
            return result;
        }
        summon_lab::LoadClass load_class{soft_class};
        if (!InvokeByPath(lib_cdo, "/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking",
                          &load_class, sizeof(load_class))) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: class load failed");
            return result;
        }
        birth.actor_class = load_class.result;
        if (birth.actor_class == 0) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: class load returned null");
            return result;
        }
    }
    if (compatibility) {
        birth.registration_cdo = LoadServerCombatCdo(lib_cdo, server_combat_blueprint);
        if (birth.registration_cdo == 0) {
            const auto message = g_context.localizer.Text("summon.compat_source_unavailable",
                "Server combat source unavailable in this scene");
            std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
            return result;
        }
    }

    summon_lab::SummonParameters parms{};
    parms.uclass = soft_class;
    parms.pet = pet ? 1 : 0;
    parms.attack_mode = aggressive ? kAttackAggressive : kAttackDefensive;
    // A player leader skips the native default-level assignment. Pass the
    // resolved scene level explicitly, before construction and registration.
    parms.level = ai_level;
    // Zero still preserves the blueprint's InheritedSkillLevel.
    parms.skill_level = static_cast<std::int32_t>(std::min(skill_level, kMaxLevel));
    parms.transform = transform;
    parms.same_camp = hostile ? 0 : 1;

    // Retire dead registrations before SummonAI can reuse their vacant slots.
    // Keep this at the Game update boundary, outside the registration hook.
    RefreshHealth();
    bool summoned{};
    {
        // SummonAI itself defers construction, configures level/leader/camp,
        // then finishes spawning. GAS receives our inputs inside that call;
        // no actor attributes or HP caches are patched after it returns.
        BirthScope scope(birth);
        summoned = InvokeByPath(pawn, kPathSummonAI, &parms, sizeof(parms));
    }
    SummonFailureCleanup cleanup{result};
    if (birth.registration_controller != 0 &&
        HandleFromPointer(birth.registration_controller, &result.registration.controller)) {
        result.registration.leader = birth.registration_leader;
        result.registration.index = birth.registration_index;
    }
    if (!summoned) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: SummonAI rejected");
        return result;
    }

    auto ai_pointer = parms.result;
    if (ai_pointer == 0) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: returned null");
        return result;
    }

    AnomalyGenerationHandleV1 ai_handle{};
    if (!HandleFromPointer(ai_pointer, &ai_handle)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: AI handle unresolved");
        return result;
    }
    result.handle = ai_handle;
    result.pointer = ai_pointer;
    if (result.registration.controller.id == 0 || result.registration.index < 0) {
        static_cast<void>(InvokeByPath(ai_handle, kPathDestroyActor, nullptr, 0));
        std::snprintf(result.detail, sizeof(result.detail), "summon: native registration unresolved");
        return result;
    }
    if (birth.registration_cdo != 0 && !birth.registration_substituted) {
        static_cast<void>(InvokeByPath(ai_handle, kPathDestroyActor, nullptr, 0));
        const auto message = g_context.localizer.Text("summon.compat_registration_failed",
            "Server combat source was not applied; summon removed");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }

    if (!ResolveActorAttributeSet(ai_pointer, &result.ability_system, &result.attribute_set) ||
        result.ability_system == 0) {
        static_cast<void>(InvokeByPath(ai_handle, kPathDestroyActor, nullptr, 0));
        std::snprintf(result.detail, sizeof(result.detail), "summon: local combat attributes unresolved");
        return result;
    }
    RegisterLocalBirth(ai_handle, ai_pointer, result.ability_system,
                       result.attribute_set, birth.exact_hp, combat_config, result.registration);

    // 未登记进召唤列表的 AI 没有归属，敌对化之后会在 AI tick 触发访问违例
    {
        if (!InvokeByPath(pawn, kPathAddSummonedAI, &ai_pointer, sizeof(ai_pointer))) {
            static_cast<void>(InvokeByPath(ai_handle, kPathDestroyActor, nullptr, 0));
            std::snprintf(result.detail, sizeof(result.detail),
                          "summon: AddSummonedAI failed");
            return result;
        }
    }

    if (!RebaseWuchaoArena(ai_handle, ai_pointer, birth.actor_class)) {
        static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
        const auto message = g_context.localizer.Text("summon.arena_failed",
            "Arena initialization failed; summon removed");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }

    result.pet = pet;
    float initial_hp{}, initial_max{};
    if (!ReadHealth(ai_handle, &initial_hp, &initial_max) || !std::isfinite(initial_hp) ||
        !std::isfinite(initial_max) || initial_hp <= 0 || initial_max <= 0) {
        static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
        const auto message = g_context.localizer.Text("summon.health_uninitialized",
            "Combat health was not initialized; summon removed");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
        return result;
    }
    bool birth_ok = true;
    if (!birth.attributes.empty()) {
        birth_ok = birth.actor == ai_pointer && birth.attribute_set == result.attribute_set &&
                   result.attribute_set != 0;
        static_cast<void>(ReadAttributeValues(result.attribute_set, &result.attributes));
        for (const auto& preset : birth.attributes) {
            const auto found = std::find_if(result.attributes.begin(), result.attributes.end(),
                [&](const AttributeValue& value) { return value.name == preset.name; });
            birth_ok = birth_ok && preset.applied && found != result.attributes.end() &&
                       HealthMatches(found->base, preset.value);
        }
        if (birth.exact_hp > 0) {
            float hp{}, max_hp{};
            birth_ok = birth_ok && ReadMatchingHealth(ai_handle, birth.exact_hp, &hp, &max_hp);
        }
    } else if (result.attribute_set != 0) {
        static_cast<void>(ReadAttributeValues(result.attribute_set, &result.attributes));
    }
    if (!birth_ok) {
        static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
        result.ok = false;
        std::snprintf(result.detail, sizeof(result.detail),
                      "summon rejected: birth attributes were not initialized; actor removed");
        return result;
    }
    if (birth.registration_controller != 0) {
        // SummonAI registers the server ability data, but unlike the native
        // opponent-summoner path it does not configure the resulting camp.
        // Mirror that native second step using the actual finished actor's camp.
        const auto controller = result.registration.controller;
        std::uint8_t camp{};
        ConfigureSummon configuration{birth.registration_leader, birth.registration_index, 0, 1.0F};
        bool configured = ReadCamp(ai_pointer, &camp);
        configuration.camp = camp;
        if (configured && birth.exact_hp > 0.0F) {
            std::scoped_lock lock(g_local_birth_mutex);
            g_birth_configurations.push_back({ai_handle, controller, result.ability_system,
                configuration, birth.exact_hp, 0.0F, false, birth.registration_substituted});
        } else {
            configured = configured && InvokeByPath(controller,
                "/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode",
                &configuration, sizeof(configuration));
        }
        if (!configured) {
            static_cast<void>(DismissSummoned(ai_handle, ai_pointer));
            result.ok = false;
            std::snprintf(result.detail, sizeof(result.detail), "summon: SummonAI rejected");
            return result;
        }
        if (g_context.core != nullptr) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                "SummonLab native-configuration: actor=%p index=%d camp=%u %s",
                reinterpret_cast<void*>(ai_pointer), birth.registration_index, static_cast<unsigned>(camp),
                birth.exact_hp > 0.0F ? "waiting for native maximum" : "initialHPPercent=1 submitted");
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                anomaly::sdk::StringView(message));
        }
    }
    if (birth.registration_substituted && g_context.core != nullptr) {
        char message[kPathCapacity + 160]{};
        std::snprintf(message, sizeof(message),
            "SummonLab FT-compatibility: actor=%p index=%d blueprint=%s serverSource=%s requestedHP=%.9g",
            reinterpret_cast<void*>(ai_pointer), birth.registration_index, path,
            result.server_combat_source.c_str(), birth.exact_hp);
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            anomaly::sdk::StringView(message));
    }
    // 取蓝图短名做显示
    const char* separator = std::strrchr(path, '.');
    if (separator == nullptr) separator = std::strrchr(path, '/');
    const char* name = separator != nullptr ? separator + 1 : path;
    const bool named = SetSpawnDisplayName(ai_handle, ai_pointer, display_name.data());
    std::snprintf(result.label, sizeof(result.label), "%s",
                  named && display_name[0] != '\0' ? display_name.data() : name);
    if (!named) {
        const auto message = g_context.localizer.Text("summon.name_failed",
            "Summoned, but the name could not be set");
        std::snprintf(result.detail, sizeof(result.detail), "%s", message.c_str());
    } else {
        std::snprintf(result.detail, sizeof(result.detail),
                      "summoned %s (level %d, %s)", name, ai_level,
                      birth.attributes.empty() ? "native attributes" : "birth attributes initialized");
    }
    result.ok = true;
    return result;
}

bool DismissSummoned(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer,
                     float previous_max_hp) {
    // Dead actors already follow the game's death cleanup. A stale list entry
    // must never send their cached address back into the summon/destroy paths.
    if (!IsActorAlive(handle, pointer)) return true;
    float hp{}, max_hp{};
    if (!ReadHealth(handle, &hp, &max_hp)) return false;
    // A living actor can lack a native combat config and remain at 0/0 from
    // birth. Allow removing it, but retain the death guard for an actor that
    // had initialized health before its attributes were cleared during death.
    if (hp <= 0.0F && (max_hp > 0.0F || previous_max_hp > 0.0F)) return true;

    // DismissSummonedAI compacts the leader's array without updating the
    // survivors' native summon indices. Destroy just this actor instead;
    // UE clears its object reference and native AddSummonedAI reuses that
    // vacant slot without moving the other registrations.
    return InvokeByPath(handle, kPathDestroyActor, nullptr, 0) &&
        !IsActorUsable(handle, pointer);
}

struct DuelActor {
    std::uint64_t id{};
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    AnomalyGenerationHandleV1 controller{};
    bool pet{};
    SummonIdentity registration;
    bool involved{};
    bool alive{};
    std::uint8_t camp{};
    std::uintptr_t previous_target{};
    std::uintptr_t controller_pointer{};
    bool battle_tuned{};
    float original_chase_range{};
    std::uint8_t original_leave_battle{};
    bool boss_hp_bar_visible{true};
};

bool ResolveDuelController(DuelActor& actor) {
    std::uint8_t finding{};
    return InvokeByPath(actor.handle, "/Script/Engine.Pawn:GetController",
                        &actor.controller_pointer, sizeof(actor.controller_pointer)) &&
        HandleFromPointer(actor.controller_pointer, &actor.controller) &&
        InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:GetUseFindAggroTarget",
                      &finding, sizeof(finding));
}

void DuelResult(bool ok, std::string_view detail) {
    if (g_context.core != nullptr && g_context.core->log != nullptr)
        g_context.core->log(g_context.core->user, ok ? ANOMALY_CORE_LOG_LEVEL_V1_INFO :
            ANOMALY_CORE_LOG_LEVEL_V1_WARNING, anomaly::sdk::StringView(detail));
    std::scoped_lock lock(g_context.mutex);
    g_context.last_result_failed = !ok;
    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%.*s",
                  static_cast<int>(detail.size()), detail.data());
}

bool PauseDuelActor(const DuelActor& actor) {
    std::uint8_t disabled = 1, searching = 0, forced = 0;
    std::array<std::uint8_t, 2> paused{1, 1};
    std::uintptr_t target{};
    // Try each independent stop even if another interface fails. The ignore
    // list intentionally remains in effect after the duel ends.
    bool ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:ForceSetAttackDisable",
                           &disabled, sizeof(disabled));
    ok = InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetUseFindAggroTarget",
                       &searching, sizeof(searching)) && ok;
    ok = InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetAggroPaused",
                       paused.data(), paused.size()) && ok;
    ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetCurrentAggroTarget",
                       &target, sizeof(target)) && ok;
    ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetForceBattleBehavior",
                       &forced, sizeof(forced)) && ok;
    return ok;
}

bool FindDuelCamps(std::uint8_t player_camp, std::vector<std::uint8_t>& camps) {
    AnomalyGenerationHandleV1 cdo{}, data{};
    std::uintptr_t pointer{};
    if (!FindHandle("/Script/HTGame.Default__HTGameData", &cdo) ||
        !InvokeByPath(cdo, "/Script/HTGame.HTGameData:GetGameData", &pointer, sizeof(pointer)) ||
        !HandleFromPointer(pointer, &data)) return false;
    const auto hostile = [&](std::uint8_t from, std::uint8_t to) {
        std::array<std::uint8_t, 3> params{from, to, 1};
        return InvokeByPath(data, "/Script/HTGame.HTGameData:GetTeamAttitude",
                             params.data(), params.size()) && params[2] == 2;
    };
    // Select the largest mutually hostile palette from the active game data.
    // Same-camp hostility is rejected by native code, even if its table says
    // otherwise. Never edit the global table to force an unsupported graph.
    const auto count = g_context.camp_count;
    std::vector<bool> eligible(count);
    std::vector<std::vector<bool>> enemies(count, std::vector<bool>(count));
    for (std::uint8_t a = 0; a < count; ++a) {
        eligible[a] = hostile(a, player_camp) && hostile(player_camp, a);
        for (std::uint8_t b = a + 1; b < count; ++b) {
            enemies[a][b] = enemies[b][a] = hostile(a, b) && hostile(b, a);
        }
    }
    for (unsigned mask = 1; mask < (1U << count); ++mask) {
        std::vector<std::uint8_t> candidate;
        bool valid = true;
        for (std::uint8_t a = 0; a < count && valid; ++a) {
            if ((mask & (1U << a)) == 0) continue;
            valid = eligible[a] && std::all_of(candidate.begin(), candidate.end(),
                [&](auto b) { return enemies[a][b]; });
            candidate.push_back(a);
        }
        if (valid && candidate.size() > camps.size()) camps = std::move(candidate);
    }
    return camps.size() >= 2;
}

bool FindLocalPlayerState(AnomalyGenerationHandleV1* player_state) {
    AnomalyGenerationHandleV1 pawn{}, controller{};
    std::uintptr_t controller_pointer{}, player_state_pointer{};
    return FindLocalPawn(&pawn) &&
        InvokeByPath(pawn, "/Script/Engine.Pawn:GetController",
                     &controller_pointer, sizeof(controller_pointer)) &&
        HandleFromPointer(controller_pointer, &controller) &&
        InvokeByPath(controller, "/Script/HTGame.HTPlayerController:GetHTPlayerState",
                     &player_state_pointer, sizeof(player_state_pointer)) &&
        HandleFromPointer(player_state_pointer, player_state);
}

bool ShowDuelBossHpBar(const DuelActor& actor, AnomalyGenerationHandleV1 player_state) {
    // SetDisableBossHPBar only notifies an aggro target that is a PlayerState.
    // A duel targets another AI, so register and show each Boss for the local
    // viewer explicitly. The native HUD owns the widgets and their lifetime.
    std::array<std::uint8_t, 2> enabled{0, 0};
    summon_lab::ObjectBool show{actor.pointer};
    // bIncludeDescUI stays false: opening a fight should only show its HP bars.
    summon_lab::ObjectBool unhide{actor.pointer};
    return InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetDisableBossHPBar",
                         enabled.data(), enabled.size()) &&
        InvokeByPath(player_state, "/Script/HTGame.HTPlayerState:ClientRegisterBossHPBarCandidate",
                         &show.object, sizeof(show.object)) &&
        InvokeByPath(player_state, "/Script/HTGame.HTPlayerState:ClientHideBossHPBarByCharacter",
                         &unhide, sizeof(unhide)) &&
        InvokeByPath(player_state, "/Script/HTGame.HTPlayerState:ClientShowBossHPBar",
                         &show, sizeof(show));
}

bool HideDuelBossHpBar(const DuelActor& actor, AnomalyGenerationHandleV1 player_state) {
    std::array<std::uint8_t, 2> disabled{1, 1};
    summon_lab::ObjectBool hide{actor.pointer, 1};
    return InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetDisableBossHPBar",
                        disabled.data(), disabled.size()) &&
        InvokeByPath(player_state, "/Script/HTGame.HTPlayerState:ClientHideBossHPBarByCharacter",
                     &hide, sizeof(hide));
}

bool SetReflectedBool(std::uintptr_t object, const char* name, std::uint8_t value) {
    std::uintptr_t actor_class{}, field_class{};
    std::uint32_t type_id{};
    std::int32_t offset{}, size{}, dimension{};
    std::uint64_t property_name{};
    char type[64]{};
    if (!ReadValue(object + g_layout.objectClass, &actor_class)) return false;
    const auto property = FindProperty(actor_class, name);
    if (property == 0 || !ReadValue(property + g_layout.fieldClass, &field_class) ||
        !ReadValue(field_class + g_layout.fieldClassName, &type_id) || !ResolveName(type_id, type, sizeof(type)) ||
        std::strcmp(type, "BoolProperty") != 0 ||
        !ReadValue(property + g_layout.propertyElementSize, &size) || size != 1 ||
        !ReadValue(property + g_layout.propertyArrayDim, &dimension) || dimension != 1 ||
        !ReadValue(property + g_layout.propertyOffset, &offset) || offset < 0 ||
        !ReadValue(property + g_layout.fieldName, &property_name)) return false;
    AnomalyGenerationHandleV1 library{};
    summon_lab::BoolProperty params{object, property_name, value};
    std::uint8_t actual{};
    return FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) &&
        InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:SetBoolPropertyByName",
                       &params, sizeof(params)) &&
        ReadValue(object + offset, &actual) && actual == value;
}

bool DisableLeaderFollowing(const DuelActor& actor) {
    // Retain the leader registration; only disable movement following.
    return SetReflectedBool(actor.pointer, "bFollowMyLeader", 0);
}

bool SetControllerChaseRange(const DuelActor& actor, float range) {
    if (g_context.current_chase_range_offset == 0) return false;
    float actual{};
    return InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetCurChaseRange",
                        &range, sizeof(range)) &&
        ReadValue(actor.controller_pointer + g_context.current_chase_range_offset, &actual) &&
        actual == range;
}

bool TuneDuelController(DuelActor& actor) {
    if (!actor.battle_tuned) {
        if (g_context.current_chase_range_offset == 0 ||
            !ReadValue(actor.controller_pointer + g_context.current_chase_range_offset,
                       &actor.original_chase_range) ||
            !std::isfinite(actor.original_chase_range) || actor.original_chase_range <= 0 ||
            !ReadReflectedValue(actor.controller_pointer, "bUseLeaveBattle", "BoolProperty",
                                &actor.original_leave_battle)) return false;
        actor.battle_tuned = true;
    }
    return SetReflectedBool(actor.controller_pointer, "bUseLeaveBattle", 0) &&
        SetControllerChaseRange(actor, std::max(actor.original_chase_range, kBattleChaseRange));
}

bool RestoreDuelController(DuelActor& actor) {
    if (!actor.battle_tuned) return true;
    const bool range_restored = SetControllerChaseRange(actor, actor.original_chase_range);
    const bool leave_restored = SetReflectedBool(actor.controller_pointer, "bUseLeaveBattle",
                                                actor.original_leave_battle);
    const bool restored = range_restored && leave_restored;
    if (restored) actor.battle_tuned = false;
    return restored;
}

bool SetBattleCamp(const DuelActor& actor, std::uint8_t camp) {
    std::uint8_t actual{};
    if (!InvokeByPath(actor.handle, "/Script/HTGame.HTAbilityCharacter:SetCharacterCampType",
                      &camp, sizeof(camp)) ||
        !ReadCamp(actor.pointer, &actual) || actual != camp) return false;
    if (actor.registration.controller.id == 0) return actor.registration.index < 0;
    if (actor.registration.index < 0) return false;
    // Verified in 1.4b live code and 1.5a IDA: -1 updates
    // the registered combat camp and skips HP assignment. Never refill HP
    // while editing battle links, or leave server damage on the birth camp.
    ConfigureSummon configuration{actor.registration.leader, actor.registration.index, camp, -1.0F};
    return InvokeByPath(actor.registration.controller,
            "/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode",
            &configuration, sizeof(configuration));
}

void ChangeBattle(const PendingOp& op, bool forgetting = false) {
    const auto first = op.record_id;
    const auto second = op.target_id;
    const auto fail = [] {
        DuelResult(false, g_context.localizer.Text("duel.failed",
            "Battle unavailable: check that the Bosses are alive and ready"));
    };
    if (first == 0 || first == second ||
        (op.kind == OpKind::SetBattleEdge && second == 0)) { fail(); return; }
    summon_lab::BattleGraph previous;
    std::vector<DuelActor> actors;
    {
        std::scoped_lock lock(g_context.mutex);
        previous = g_context.battles;
        for (std::size_t i = 0; i < g_context.record_count; ++i) {
            const auto& record = g_context.records[i];
            DuelActor actor{record.id, record.handle, record.pointer, {}, record.pet, record.registration};
            actor.battle_tuned = record.battle_tuned;
            actor.original_chase_range = record.original_chase_range;
            actor.original_leave_battle = record.original_leave_battle;
            actor.boss_hp_bar_visible = record.boss_hp_bar_visible;
            actors.push_back(actor);
        }
    }
    for (auto& actor : actors) {
        if (forgetting && actor.id == first) continue;
        float hp{}, max_hp{};
        actor.alive = IsActorAlive(actor.handle, actor.pointer) &&
            ReadHealth(actor.handle, &hp, &max_hp) && hp > 0 && max_hp > 0;
    }
    auto next = previous;
    if (op.kind == OpKind::SelectAllOpponents) {
        // Build one graph from the complete management list, independent of
        // pagination. Validate and apply its camp assignment only once.
        for (const auto& actor : actors)
            if (actor.alive && !actor.pet) next.Set(first, actor.id, true);
    } else if (op.kind == OpKind::StopDuel) next.Remove(first);
    else next.Set(first, second, op.enabled);
    if (next.edges == previous.edges) return;
    auto combined = previous;
    for (const auto& [a, b] : next.edges) combined.Set(a, b, true);
    const auto component = combined.Component(first);
    for (auto& actor : actors)
        actor.involved = std::find(component.begin(), component.end(), actor.id) != component.end();
    // The death path must detach a missing node even when another actor is
    // unavailable. Pause only validated survivors; unrelated components stay put.
    const auto stop_component = [&] {
        for (auto& actor : actors) {
            if (actor.involved && actor.alive && actor.controller.id != 0) {
                static_cast<void>(PauseDuelActor(actor));
                static_cast<void>(RestoreDuelController(actor));
            }
        }
        for (const auto id : component) previous.Remove(id);
        std::scoped_lock lock(g_context.mutex);
        g_context.battles = previous;
        for (auto& record : g_context.records)
            for (const auto& actor : actors)
                if (record.id == actor.id) record.battle_tuned = actor.battle_tuned;
    };
    bool ready = g_context.current_chase_range_offset != 0;
    std::vector<summon_lab::BattleCamp> assignments;
    for (auto& actor : actors) {
        if (!actor.involved || (forgetting && actor.id == first)) continue;
        std::uint8_t role{};
        const bool resolved = actor.alive && !actor.pet &&
            InvokeByPath(actor.handle, "/Script/Engine.Actor:GetLocalRole", &role, sizeof(role)) &&
            role == 3 && ResolveDuelController(actor);
        ready = resolved && ready;
        if (!resolved || !next.Contains(actor.id)) continue;
        ready = ReadCamp(actor.pointer, &actor.camp) &&
            InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:GetCurrentAggroTarget",
                           &actor.previous_target, sizeof(actor.previous_target)) && ready;
        assignments.push_back({actor.id, actor.camp});
    }
    for (const auto& [a, b] : next.edges) {
        if (std::find(component.begin(), component.end(), a) == component.end()) continue;
        for (const auto id : {a, b})
            ready = std::any_of(assignments.begin(), assignments.end(),
                [id](const auto& actor) { return actor.id == id; }) && ready;
    }
    {
        std::scoped_lock lock(g_local_birth_mutex);
        for (const auto& actor : actors) {
            if (!actor.involved) continue;
            ready = std::none_of(g_birth_configurations.begin(), g_birth_configurations.end(),
                [&](const auto& pending) { return pending.actor_handle.id == actor.handle.id &&
                    pending.actor_handle.generation == actor.handle.generation; }) && ready;
        }
    }
    if (!ready) {
        if (forgetting) stop_component();
        fail();
        return;
    }

    AnomalyGenerationHandleV1 pawn{}, controller{}, player_state{};
    std::uintptr_t player_pointer{}, controller_pointer{}, player_state_pointer{};
    std::uint8_t player_camp{};
    std::vector<std::uint8_t> camps;
    if (!assignments.empty() && (!FindLocalPawn(&pawn) ||
        !InvokeByPath(pawn, "/Script/Engine.Pawn:GetController", &controller_pointer, sizeof(controller_pointer)) ||
        !HandleFromPointer(controller_pointer, &controller) ||
        !InvokeByPath(controller, "/Script/Engine.Controller:K2_GetPawn", &player_pointer, sizeof(player_pointer)) ||
        !InvokeByPath(controller, "/Script/HTGame.HTPlayerController:GetHTPlayerState",
                       &player_state_pointer, sizeof(player_state_pointer)) ||
        !HandleFromPointer(player_state_pointer, &player_state) ||
        !ReadCamp(player_pointer, &player_camp) ||
        !FindDuelCamps(player_camp, camps))) {
        if (forgetting) stop_component();
        fail(); return;
    }
    if (!summon_lab::AssignBattleCamps(next, camps, assignments)) {
        DuelResult(false, g_context.localizer.Text("duel.camps_unavailable",
            "This opponent graph exceeds the native camp limit"));
        return;
    }
    bool ok = true;
    for (auto& actor : actors) {
        if (!actor.involved || !actor.alive) continue;
        ok = PauseDuelActor(actor) && DisableLeaderFollowing(actor) && ok;
        if (!next.Contains(actor.id)) {
            ok = RestoreDuelController(actor) && ok;
            continue;
        }
        ok = TuneDuelController(actor) && ok;
        // Native 1.4b converts a player pawn to its PlayerState here, so the
        // exclusion survives switching the active player character.
        ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetIgnoredAggroTarget",
                           &player_pointer, sizeof(player_pointer)) && ok;
        for (const auto& other : actors) {
            if (!other.alive || other.id == actor.id) continue;
            const char* path = next.Linked(actor.id, other.id)
                ? "/Script/HTGame.HTAICharacter:RemoveIgnoredAggroTarget"
                : "/Script/HTGame.HTAICharacter:SetIgnoredAggroTarget";
            auto target = other.pointer;
            ok = InvokeByPath(actor.handle, path, &target, sizeof(target)) && ok;
        }
    }
    for (const auto& assignment : assignments) {
        if (!ok) break;
        const auto actor = std::find_if(actors.begin(), actors.end(),
            [&](const auto& value) { return value.id == assignment.id; });
        ok = SetBattleCamp(*actor, static_cast<std::uint8_t>(assignment.assigned));
    }
    // All participants must be attackable before assigning targets: forcing
    // a target also updates native battle behavior synchronously.
    for (const auto& actor : actors) {
        if (!ok) break;
        if (!actor.involved || !next.Contains(actor.id)) continue;
        std::uint8_t disabled = 0;
        ok = InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:ForceSetAttackDisable",
                           &disabled, sizeof(disabled));
    }
    for (const auto& actor : actors) {
        if (!ok) break;
        if (!actor.involved || !next.Contains(actor.id)) continue;
        const DuelActor* opponent{};
        for (const auto& candidate : actors) {
            if (!next.Linked(actor.id, candidate.id)) continue;
            if (opponent == nullptr || candidate.pointer == actor.previous_target) opponent = &candidate;
        }
        std::array<std::uint8_t, 2> resumed{0, 0};
        std::uint8_t forced = 1, searching = 1;
        struct { std::uintptr_t target; float aggro_percent; } target{opponent->pointer, 1.0F};
        std::uintptr_t actual_target{};
        ok = InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetAggroPaused",
                           resumed.data(), resumed.size()) &&
            // Native skills can temporarily clear a target. Allow the AI to
            // select it again; the ignored PlayerState remains excluded.
            InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetUseFindAggroTarget",
                           &searching, sizeof(searching)) &&
            InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetForceBattleBehavior",
                           &forced, sizeof(forced)) &&
            InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:ForceSetCurrentAggroTarget",
                           &target, sizeof(target)) &&
            InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:GetCurrentAggroTarget",
                           &actual_target, sizeof(actual_target)) && actual_target == opponent->pointer &&
            (actor.boss_hp_bar_visible ? ShowDuelBossHpBar(actor, player_state) :
                HideDuelBossHpBar(actor, player_state));
    }
    if (!ok) {
        stop_component();
        DuelResult(false, g_context.localizer.Text("duel.apply_failed",
            "Battle update failed; connected participants were stopped"));
        return;
    }
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.battles = std::move(next);
        for (auto& record : g_context.records)
            for (const auto& actor : actors)
                if (record.id == actor.id) {
                    record.battle_tuned = actor.battle_tuned;
                    record.original_chase_range = actor.original_chase_range;
                    record.original_leave_battle = actor.original_leave_battle;
                }
    }
    DuelResult(true, g_context.localizer.Text("duel.updated", "Opponents updated"));
}

void ExcludeNewSummon(const SummonResult& result) {
    std::vector<DuelActor> fighters;
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.record_count; ++i) {
            const auto& record = g_context.records[i];
            if (g_context.battles.Contains(record.id))
                fighters.push_back({record.id, record.handle, record.pointer});
        }
    }
    // An existing fight must not acquire a newly summoned, unlinked Boss.
    for (const auto& actor : fighters) {
        auto target = result.pointer;
        if (IsActorAlive(actor.handle, actor.pointer))
            static_cast<void>(InvokeByPath(actor.handle,
                "/Script/HTGame.HTAICharacter:SetIgnoredAggroTarget",
                &target, sizeof(target)));
    }
}

void ForgetRecord(std::uint64_t record_id) {
    ChangeBattle(PendingOp{OpKind::StopDuel, record_id}, true);
    AnomalyGenerationHandleV1 handle{};
    SummonIdentity registration;
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.record_count; ++i) {
            if (g_context.records[i].id != record_id) continue;
            handle = g_context.records[i].handle;
            registration = g_context.records[i].registration;
            for (std::size_t k = i + 1; k < g_context.record_count; ++k)
                g_context.records[k - 1] = std::move(g_context.records[k]);
            g_context.records[--g_context.record_count] = {};
            if (g_context.selected_record_id == record_id) {
                const auto next = g_context.record_count == 0 ? 0 :
                    std::min(i, g_context.record_count - 1);
                g_context.selected_record_id =
                    g_context.record_count == 0 ? 0 : g_context.records[next].id;
                g_context.management_page = next / kManagementPageSize;
            }
            break;
        }
        const auto end = std::remove_if(g_context.pending.begin(),
            g_context.pending.begin() + g_context.pending_count,
            [&](const PendingOp& op) { return op.record_id == record_id || op.target_id == record_id; });
        g_context.pending_count = static_cast<std::size_t>(end - g_context.pending.begin());
        g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending_count),
                             std::memory_order_release);
    }
    if (handle.id != 0) {
        ForgetLocalBirth(handle);
        static_cast<void>(UnregisterNativeSummon(registration));
    }
}

void RemoveOne(std::uint64_t record_id) {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    float previous_max_hp{};
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.record_count; ++i) {
            if (g_context.records[i].id != record_id) continue;
            handle = g_context.records[i].handle;
            pointer = g_context.records[i].pointer;
            previous_max_hp = g_context.records[i].max_hp;
            break;
        }
    }
    if (!DismissSummoned(handle, pointer, previous_max_hp)) {
        DuelResult(false, g_context.localizer.Text("manage.failed",
            "Action failed: the entity or function is unavailable"));
        return;
    }
    ForgetRecord(record_id);
}

void ClearAll() {
    std::array<std::uint64_t, kMaxRecords> snapshot{};
    std::size_t count{};
    {
        std::scoped_lock lock(g_context.mutex);
        count = g_context.record_count;
        for (std::size_t i = 0; i < count; ++i)
            snapshot[i] = g_context.records[i].id;
    }
    for (std::size_t i = 0; i < count; ++i)
        RemoveOne(snapshot[i]);
}

void SetActorOption(const PendingOp& op) {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.record_count; ++i) {
            if (g_context.records[i].id == op.record_id) {
                handle = g_context.records[i].handle;
                pointer = g_context.records[i].pointer;
                break;
            }
        }
    }
    bool applied = false;
    if (IsActorAlive(handle, pointer)) {
        // Reflected parameter layouts from the 1.4b SDK; all calls run in DrainOne.
        if (op.kind == OpKind::SetBossHpBar) {
            AnomalyGenerationHandleV1 player_state{};
            const DuelActor actor{op.record_id, handle, pointer};
            applied = FindLocalPlayerState(&player_state) &&
                (op.enabled ? ShowDuelBossHpBar(actor, player_state) :
                    HideDuelBossHpBar(actor, player_state));
        } else {
            std::uint8_t enabled = op.enabled ? 1 : 0;
            const char* path = op.kind == OpKind::SetCollision
                ? "/Script/Engine.Actor:SetActorEnableCollision"
                : "/Script/Engine.Actor:SetActorHiddenInGame";
            applied = InvokeByPath(handle, path, &enabled, sizeof(enabled));
        }
    } else {
        ForgetRecord(op.record_id);
    }
    const auto detail = applied ? std::string{} : g_context.localizer.Text(
        "manage.failed", "Action failed: the entity or function is unavailable");
    std::scoped_lock lock(g_context.mutex);
    if (applied && op.kind == OpKind::SetBossHpBar)
        for (auto& record : g_context.records)
            if (record.id == op.record_id) record.boss_hp_bar_visible = op.enabled;
    g_context.last_result_failed = !applied;
    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                  detail.c_str());
}


void RefreshHealth() {

    struct HealthTarget {
        std::uint64_t id{};
        AnomalyGenerationHandleV1 handle{};
        std::uintptr_t pointer{};
        float previous_max_hp{};
    };
    HealthTarget snapshot[kMaxRecords]{};
    std::size_t count{};
    std::uint64_t selected_id{};
    {
        std::scoped_lock lock(g_context.mutex);
        count = g_context.record_count;
        selected_id = g_context.selected_record_id;
        for (std::size_t i = 0; i < count && i < kMaxRecords; ++i)
            snapshot[i] = {g_context.records[i].id, g_context.records[i].handle,
                           g_context.records[i].pointer, g_context.records[i].max_hp};
    }
    for (std::size_t i = 0; i < count && i < kMaxRecords; ++i) {
        if (!IsActorAlive(snapshot[i].handle, snapshot[i].pointer)) {
            ForgetRecord(snapshot[i].id);
            continue;
        }
        float hp{};
        float max_hp{};
        if (!ReadHealth(snapshot[i].handle, &hp, &max_hp)) continue;
        if (hp <= 0.0F && (max_hp > 0.0F || snapshot[i].previous_max_hp > 0.0F)) {
            ForgetRecord(snapshot[i].id);
            continue;
        }
        std::vector<AttributeValue> attributes;
        std::uintptr_t ability_system{}, attribute_set{};
        const bool attributes_read = snapshot[i].id == selected_id &&
            ResolveActorAttributeSet(snapshot[i].pointer, &ability_system, &attribute_set) &&
            ReadAttributeValues(attribute_set, &attributes);
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t k = 0; k < g_context.record_count; ++k) {
            auto& record = g_context.records[k];
            if (record.id != snapshot[i].id) continue;
            record.hp = hp;
            record.max_hp = max_hp;
            if (attributes_read) {
                record.attributes = std::move(attributes);
            }
            break;
        }
    }
}

void MaintainBattles() {
    summon_lab::BattleGraph graph;
    std::vector<DuelActor> actors;
    {
        std::scoped_lock lock(g_context.mutex);
        graph = g_context.battles;
        for (std::size_t i = 0; i < g_context.record_count; ++i) {
            const auto& record = g_context.records[i];
            if ((!graph.Contains(record.id) && !record.battle_tuned) ||
                record.hp <= 0 || record.max_hp <= 0) continue;
            DuelActor actor{record.id, record.handle, record.pointer};
            actor.battle_tuned = record.battle_tuned;
            actor.original_chase_range = record.original_chase_range;
            actor.original_leave_battle = record.original_leave_battle;
            actor.boss_hp_bar_visible = record.boss_hp_bar_visible;
            actors.push_back(actor);
        }
    }
    for (auto& actor : actors) {
        actor.alive = IsActorAlive(actor.handle, actor.pointer) && ResolveDuelController(actor);
    }
    for (auto& actor : actors) {
        if (!actor.alive || !actor.battle_tuned) continue;
        if (!graph.Contains(actor.id)) {
            if (RestoreDuelController(actor)) {
                std::scoped_lock lock(g_context.mutex);
                for (auto& record : g_context.records)
                    if (record.id == actor.id) record.battle_tuned = false;
            }
            continue;
        }
        float range{};
        std::uint8_t leave_battle{};
        if (!ReadValue(actor.controller_pointer + g_context.current_chase_range_offset, &range) ||
            !ReadReflectedValue(actor.controller_pointer, "bUseLeaveBattle", "BoolProperty",
                                &leave_battle)) continue;
        if (range < std::max(actor.original_chase_range, kBattleChaseRange) || leave_battle != 0)
            if (!TuneDuelController(actor)) continue;
        std::uintptr_t current_target{};
        if (!InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:GetCurrentAggroTarget",
                          &current_target, sizeof(current_target))) continue;
        const auto linked_target = std::find_if(actors.begin(), actors.end(), [&](const auto& candidate) {
            return candidate.alive && graph.Linked(actor.id, candidate.id) &&
                candidate.pointer == current_target;
        });
        if (linked_target != actors.end()) continue;
        const auto opponent = std::find_if(actors.begin(), actors.end(), [&](const auto& candidate) {
            return candidate.alive && graph.Linked(actor.id, candidate.id);
        });
        if (opponent == actors.end()) continue;
        std::array<std::uint8_t, 2> resumed{0, 0};
        std::uint8_t searching = 1, forced = 1;
        struct { std::uintptr_t target; float aggro_percent; } target{opponent->pointer, 1.0F};
        const bool retargeted = InvokeByPath(actor.controller,
                "/Script/HTGame.HTAIController:SetAggroPaused", resumed.data(), resumed.size()) &&
            InvokeByPath(actor.controller, "/Script/HTGame.HTAIController:SetUseFindAggroTarget",
                         &searching, sizeof(searching)) &&
            InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:SetForceBattleBehavior",
                         &forced, sizeof(forced)) &&
            InvokeByPath(actor.handle, "/Script/HTGame.HTAICharacter:ForceSetCurrentAggroTarget",
                         &target, sizeof(target));
        if (retargeted && !actor.boss_hp_bar_visible) {
            AnomalyGenerationHandleV1 player_state{};
            if (FindLocalPlayerState(&player_state))
                static_cast<void>(HideDuelBossHpBar(actor, player_state));
        }
    }
}

// ---------------------------------------------------------------- 队列

bool PushOp(const PendingOp& op) {
    std::scoped_lock lock(g_context.mutex);
    if (g_context.pending_count >= kMaxPending) {
        g_context.last_result_failed = true;
        std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                      "operation queue full");
        return false;
    }
    g_context.pending[g_context.pending_count++] = op;
    g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending_count),
                         std::memory_order_release);
    return true;
}

// An explicitly supplied request is consumed once on plugin start. Queueing uses
// the same game-thread path as the summon button; no ProcessEvent runs here.
void ConsumeSpawnRequest() {
    // Core exposes the original package directory, not the disposable shadow.
    // Keep the request outside that package so it cannot be copied or reloaded.
    std::array<char, 4096> package{};
    std::size_t size = package.size();
    if (g_context.core == nullptr || g_context.core->plugin_directory == nullptr ||
        g_context.core->plugin_directory(g_context.core->user, package.data(), &size).code !=
            ANOMALY_STATUS_V1_OK) return;
    const auto path = std::filesystem::u8path(package.data()).parent_path() /
                      L"summon-lab-spawn-once.json";
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    const auto request = nlohmann::json::parse(file, nullptr, false);
    file.close();
    if (request.is_discarded() || !request.is_object() ||
        !request.contains("classPath") || !request["classPath"].is_string() ||
        !request.contains("hp") || !request["hp"].is_number_unsigned() ||
        !request.contains("level") || !request["level"].is_number_unsigned() ||
        !request.contains("hostile") || !request["hostile"].is_boolean()) return;
    const auto class_path = request["classPath"].get<std::string>();
    const auto hp = request["hp"].get<std::uint64_t>();
    const auto level = request["level"].get<std::uint64_t>();
    if (class_path.empty() || class_path.size() >= g_context.class_path.size() ||
        hp > 10000000U || level > kMaxLevel ||
        !g_context.birth_attributes_ready.load(std::memory_order_acquire) ||
        !g_context.has_invoke) return;
    std::error_code error;
    if (!std::filesystem::remove(path, error) || error) return;
    {
        std::scoped_lock lock(g_context.mutex);
        std::snprintf(g_context.class_path.data(), g_context.class_path.size(),
                      "%s", class_path.c_str());
        g_context.birth_presets[kMaxHpAttribute] = {hp != 0 ? 1 : 0, static_cast<double>(hp)};
        g_context.level = static_cast<std::uint32_t>(level);
        g_context.hostile = request["hostile"].get<bool>() ? 1 : 0;
        g_context.pet = 0;
        g_context.aggressive = 1;
    }
    static_cast<void>(PushOp(PendingOp{OpKind::Summon, 0}));
}

void DrainOne() {
    PendingOp op{};
    {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.pending_count == 0) {
            g_pending_hint.store(0, std::memory_order_release);
            return;
        }
        op = g_context.pending[0];
        for (std::size_t i = 1; i < g_context.pending_count; ++i)
            g_context.pending[i - 1] = g_context.pending[i];
        --g_context.pending_count;
        g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending_count),
                             std::memory_order_release);
    }

    switch (op.kind) {
    case OpKind::Summon: {
        const SummonResult result = SummonOne();
        if (result.ok) ExcludeNewSummon(result);
        if (g_context.core != nullptr) {
            g_context.core->log(g_context.core->user,
                result.ok ? ANOMALY_CORE_LOG_LEVEL_V1_INFO : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView(result.detail));
        }
        {
            std::scoped_lock lock(g_context.mutex);
            if (result.ok) {
                if (g_context.record_count < kMaxRecords) {
                    auto& record = g_context.records[g_context.record_count++];
                    record = {};
                    record.id = g_context.next_record_id++;
                    record.handle = result.handle;
                    record.pointer = result.pointer;
                    record.pet = result.pet;
                    record.registration = result.registration;
                    record.server_combat_source = result.server_combat_source;
                    record.attributes = std::move(result.attributes);
                    std::snprintf(record.label, sizeof(record.label), "%s", result.label);
                    g_context.selected_record_id = record.id;
                    g_context.management_page = (g_context.record_count - 1) / kManagementPageSize;
                }
                g_context.last_result_failed = false;
            } else {
                g_context.last_result_failed = true;
            }
            std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                          result.detail);
        }
        break;
    }
    case OpKind::Remove:
        RemoveOne(op.record_id);
        break;
    case OpKind::ClearAll:
        ClearAll();
        break;
    case OpKind::RefreshHP:
        RefreshHealth();
        MaintainBattles();
        break;
    case OpKind::SetBossHpBar:
    case OpKind::SetCollision:
    case OpKind::SetHidden:
        SetActorOption(op);
        break;
    case OpKind::SetBattleEdge:
    case OpKind::StopDuel:
    case OpKind::SelectAllOpponents:
        ChangeBattle(op);
        break;
    }
}

// ---------------------------------------------------------------- 服务补查

void EnsureServices() {
    if (g_context.host == nullptr) return;
    if (g_context.objects != nullptr && g_context.session != nullptr &&
        g_context.player != nullptr && g_context.names != nullptr)
        return;

    const anomaly::sdk::Host query(g_context.host);
    if (g_context.objects == nullptr) {
        g_context.objects = query.Query<AnomalyUe5ObjectsServiceV1>(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    }
    if (g_context.session == nullptr) {
        g_context.session = query.Query<AnomalyNteSessionServiceV1>(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    }
    if (g_context.player == nullptr) {
        g_context.player = query.Query<AnomalyNtePlayerServiceV1>(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    }
    if (g_context.names == nullptr) {
        g_context.names = query.Query<AnomalyUe5NamesServiceV1>(
            ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();
    }
}

}  // namespace

// ---------------------------------------------------------------- ABI

namespace {

void ResetState() {
    {
        std::scoped_lock lock(g_local_birth_mutex);
        g_local_births.clear();
        g_birth_configurations.clear();
    }
    g_birth_configuration_ready.store(false, std::memory_order_release);
    g_restore_local_births.store(false, std::memory_order_release);
    g_observe_ft_health.store(false, std::memory_order_release);
    g_pending_local_births.clear();
    std::scoped_lock lock(g_context.mutex);
    g_context.records = {};
    g_context.record_count = 0;
    g_context.next_record_id = 1;
    g_context.pending = {};
    g_context.pending_count = 0;
    g_context.session_world_ready = false;
    g_context.session_event_cursor = 0;
    g_context.frame_counter = 0;

    g_context.class_path = {};
    g_context.blueprint_config = {};
    g_context.blueprint_check_delay = 0;
    g_context.display_name = {};
    std::snprintf(g_context.class_path.data(), g_context.class_path.size(), "%s",
                  kDefaultClassPath);
    g_context.level = 0;
    g_context.skill_level = 0;
    g_context.monster_level_query = 0;
    g_layout = {};
    g_layout_ready = false;
    g_context.mirror_getter_entry.clear();
    g_context.final_tower_scene_asset.clear();
    g_context.final_tower_aliases.clear();
    g_context.final_tower_combat_blueprint.clear();
    g_context.mirror_record_size = 0;
    g_context.camp_count = 0;
    g_context.current_chase_range_offset = 0;
    g_context.available_birth_attributes = 0;
    g_context.birth_presets = {};
    g_context.attribute_filter = {};
    g_context.attribute_help = kBirthAttributeNames.size();
    g_context.only_overrides = 0;
    g_context.catalog_page = 0;
    g_context.hostile = 0;
    g_context.pet = 0;
    g_context.aggressive = 0;
    g_context.ft_compatibility = 0;
    g_context.last_result = {};
    g_context.last_result_failed = false;
    g_context.catalog.clear();
    g_context.catalog_filter[0] = '\0';
    g_context.catalog_loaded = false;
    g_context.selected_record_id = 0;
    g_context.management_page = 0;
    g_context.battles = {};
    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "idle");
    g_pending_hint.store(0, std::memory_order_release);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    const anomaly::sdk::Host query(host);
    const auto ui = query.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID,
                                                    ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr)
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};

    g_context.host = host;
    g_context.ui = ui.get();
    g_context.localizer = anomaly::sdk::Localizer(host);
    g_context.attribute_descriptions = summon_lab::LoadAttributeDescriptions(g_context.localizer);
    ResetState();
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    ResetState();

    const anomaly::sdk::Host query(g_context.host);
    g_context.core = query.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID,
                                                       ANOMALY_CORE_SERVICE_V1_VERSION).get();
    g_context.objects = query.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    g_context.session = query.Query<AnomalyNteSessionServiceV1>(
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    g_context.player = query.Query<AnomalyNtePlayerServiceV1>(
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    g_context.names = query.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION).get();

    TransferLocalBirths(false);

    const auto events = query.Query<AnomalyUe5ProcessEventServiceV1>(
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID, ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION);
    g_context.events = events.get();
    g_context.has_invoke = HasInvoke(g_context.events);

    const auto loaded = anomaly::sdk::LoadProfile(query, "compatibility.profile.json");
    if (loaded && ValidateSummonProfile(loaded.value(), &g_layout)) {
        const auto& profile = loaded.value();
        g_layout_ready = true;
        g_context.monster_level_query = ResolveMonsterLevelQuery(query, profile);
        g_context.current_chase_range_offset = ResolveDuelChaseRange(query, profile);
        const bool attributes_ready = InstallBirthAttributeHook(query, profile);
        const bool combat_ready = attributes_ready && InstallLocalCombatHooks(query, profile);
        g_context.birth_attributes_ready.store(attributes_ready && combat_ready, std::memory_order_release);
        if (combat_ready && !InstallFinalTowerHooks(query, profile) && g_context.core != nullptr)
            g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView("SummonLab: FinalTower config bridge unavailable"));
        if (combat_ready) StartFinalTowerHealthObserver();
    } else if (g_context.core != nullptr && g_context.core->log != nullptr) {
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            anomaly::sdk::StringView(loaded ? "Unsupported SummonLab profile contract or layout" : loaded.error()));
    }
    static_cast<void>(LoadCatalog());
    ConsumeSpawnRequest();

    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (g_context.events != nullptr && g_context.health_observer.id != 0) {
        const auto status = g_context.events->unsubscribe(g_context.events->user, g_context.health_observer);
        if (status.code != ANOMALY_STATUS_V1_OK && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
    }
    g_context.health_observer = {};
    g_context.health_events = {};
    g_context.health_event_lookup_pending = false;
    g_context.events = nullptr;
    g_context.monster_level_query = 0;
    g_context.birth_attributes_ready.store(false, std::memory_order_release);
    const auto local_status = ReleaseLocalCombatHooks();
    if (local_status.code != ANOMALY_STATUS_V1_OK) return local_status;
    ReleaseFinalTowerData();
    if (g_context.hooks != nullptr && g_context.attribute_hook.id != 0) {
        const auto status = g_context.hooks->release(g_context.hooks->user, g_context.attribute_hook);
        // PluginScope may revoke and drain the hook before on_stop runs.
        // NOT_FOUND means that cleanup is already complete, not an unload fault.
        if (status.code != ANOMALY_STATUS_V1_OK &&
            status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
    }
    g_context.attribute_hook = {};
    g_context.attribute_original = 0;
    g_context.hooks = nullptr;
    TransferLocalBirths(true);
    ResetState();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* context) {
    if (context != &g_context) return;
    ResetState();
    g_context.host = nullptr;
    g_context.ui = nullptr;
    g_context.core = nullptr;
    g_context.objects = nullptr;
    g_context.session = nullptr;
    g_context.player = nullptr;
    g_context.names = nullptr;
    g_context.has_invoke = false;
}

void ANOMALY_CALL Update(void* /*context*/, double delta_seconds) {
    ++g_context.frame_counter;
    EnsureServices();
    UpdateSessionState();
    if (!g_context.session_world_ready || !g_layout_ready) return;
    ResolveFinalTowerHealthEvents();
    // The host runs on_update at its Game tick boundary. Do not invoke spawn
    // or configuration from arbitrary ProcessEvent callbacks: those may be
    // nested inside UI scripts or a native attribute mirror's deserialization.
    RestoreLocalBirthsOnGameThread();
    ConfigureNativeBirthsOnGameThread();
    UpdateBlueprintConfig(delta_seconds);
    if (g_pending_hint.load(std::memory_order_relaxed) != 0) DrainOne();
    if ((g_context.frame_counter % kHealthRefreshInterval) == 0)
        (void)PushOp(PendingOp{OpKind::RefreshHP, 0});

}

// width 传 -1 表示填满当前可用宽度，避免列表按钮只占住左侧一小条。
bool ActionButton(const AnomalyUiServiceV1* ui, const char* label, bool enabled,
                  float width = 0.0F) {
    if (ui == nullptr) return false;
    if (ui->button_enabled != nullptr)
        return ui->button_enabled(ui->user, anomaly::sdk::StringView(label), width, 0.0F,
                                  enabled ? 1 : 0) != 0;
    return enabled && ui->button != nullptr &&
           ui->button(ui->user, anomaly::sdk::StringView(label), width, 0.0F) != 0;
}

void WrappedText(const AnomalyUiServiceV1* ui, std::string_view text) {
    if (ui->struct_size >= offsetof(AnomalyUiServiceV1, text_wrapped) + sizeof(ui->text_wrapped) &&
        ui->text_wrapped != nullptr)
        ui->text_wrapped(ui->user, anomaly::sdk::StringView(text));
    else
        ui->text(ui->user, anomaly::sdk::StringView(text));
}

void DrawBlueprintConfig(const AnomalyUiServiceV1* ui) {
    BlueprintConfigState state;
    std::string source;
    {
        std::scoped_lock lock(g_context.mutex);
        if (g_context.class_path[0] == '\0') return;
        const auto& check = g_context.blueprint_config;
        state = !g_context.session_world_ready ? BlueprintConfigState::Unavailable :
            check.path != g_context.class_path.data() ? BlueprintConfigState::Pending : check.state;
        if (check.path == g_context.class_path.data()) source = check.source;
    }
    const auto& localizer = g_context.localizer;
    std::string text;
    switch (state) {
    case BlueprintConfigState::Pending:
        text = localizer.Text("catalog.config_pending", "Checking combat config...");
        break;
    case BlueprintConfigState::Unavailable:
        text = localizer.Text("catalog.config_unavailable", "Combat config check unavailable");
        break;
    case BlueprintConfigState::NotApplicable:
        text = localizer.Text("catalog.config_not_applicable", "Combat config check is not supported for this blueprint");
        break;
    case BlueprintConfigState::NoHealth:
        text = localizer.Text("catalog.config_no_health", "No valid HP config found in the current scene");
        break;
    case BlueprintConfigState::Available:
        text = localizer.Text("catalog.config_available", "Combat config found");
        break;
    }
    WrappedText(ui, text);
    if (!source.empty()) {
        const std::array<std::string_view, 1> args{source};
        WrappedText(ui, localizer.Format("catalog.config_source", "FT config: {0}", args));
    }
    if (state == BlueprintConfigState::Unavailable) {
        const auto retry = localizer.Text("catalog.config_retry", "Recheck");
        if (ActionButton(ui, retry.c_str(), true)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.blueprint_config = {};
        }
    }
}

void DrawBlueprintTab(const AnomalyUiServiceV1* ui) {
    const auto& localizer = g_context.localizer;
    char line[kTextCapacity];
    bool loaded{};
    std::size_t catalog_total{};
    {
        std::scoped_lock lock(g_context.mutex);
        loaded = g_context.catalog_loaded;
        catalog_total = g_context.catalog.size();
    }
    // ================= 目标 =================
    if (ui->input_text != nullptr) {
        const std::string label = localizer.Text("target.title", "Class path");
        std::scoped_lock lock(g_context.mutex);
        ui->input_text(ui->user, anomaly::sdk::StringView(label), g_context.class_path.data(),
                       g_context.class_path.size(), 0);
    }

    if (!loaded) {
        const std::string missing = localizer.Text("catalog.missing", "catalog not loaded");
        ui->text(ui->user, anomaly::sdk::StringView(missing));
    } else {
        // 过滤 + 匹配统计放在同一行
        if (ui->input_text != nullptr) {
            const std::string filter_label = localizer.Text("catalog.filter", "Search");
            {
                std::scoped_lock lock(g_context.mutex);
                if (ui->input_text(ui->user, anomaly::sdk::StringView(filter_label),
                                   g_context.catalog_filter, sizeof(g_context.catalog_filter), 0))
                    g_context.catalog_page = 0;
            }
        }

        constexpr std::size_t kPageSize = 100;
        std::vector<ClassEntry> visible;
        std::size_t matched{};
        std::size_t page{};
        std::size_t page_count{};
        {
            std::scoped_lock lock(g_context.mutex);
            // 先统计匹配总数，再按页截取，页码越界时钳回最后一页
            for (const auto& entry : g_context.catalog) {
                bool keep = true;
                if (g_context.catalog_filter[0] != '\0') {
                    keep = ui->filter_match != nullptr
                               ? ui->filter_match(ui->user,
                                                  anomaly::sdk::StringView(g_context.catalog_filter),
                                                  anomaly::sdk::StringView(entry.search_text)) != 0
                               : entry.search_text.find(g_context.catalog_filter) != std::string::npos;
                }
                if (keep) ++matched;
            }
            page_count = matched == 0 ? 1 : (matched + kPageSize - 1) / kPageSize;
            page = g_context.catalog_page < page_count ? g_context.catalog_page : page_count - 1;
            std::size_t seen{};
            for (const auto& entry : g_context.catalog) {
                bool keep = true;
                if (g_context.catalog_filter[0] != '\0') {
                    keep = ui->filter_match != nullptr
                               ? ui->filter_match(ui->user,
                                                  anomaly::sdk::StringView(g_context.catalog_filter),
                                                  anomaly::sdk::StringView(entry.search_text)) != 0
                               : entry.search_text.find(g_context.catalog_filter) != std::string::npos;
                }
                if (!keep) continue;
                if (seen >= page * kPageSize) {
                    if (visible.size() < kPageSize) visible.push_back(entry);
                    else break;
                }
                ++seen;
            }
        }

        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 10.0F);
        const std::string total_arg = std::to_string(catalog_total);
        const std::string matched_arg = std::to_string(matched);
        const std::array<std::string_view, 2> args{matched_arg, total_arg};
        const std::string stats = localizer.Format("catalog.stats", "{0} / {1}", args);
        std::snprintf(line, sizeof(line), "%s", stats.c_str());
        ui->text(ui->user, anomaly::sdk::StringView(line));

        // 分页：每页 kPageSize 条，页码在过滤变化时自动钳回有效范围
        {
            const std::string prev_label = localizer.Text("page.prev", "<");
            if (ActionButton(ui, prev_label.c_str(), page > 0)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.catalog_page = page > 0 ? page - 1 : 0;
            }
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 10.0F);
            const std::string page_arg = std::to_string(page + 1);
            const std::string pages_arg = std::to_string(page_count);
            const std::array<std::string_view, 2> page_args{page_arg, pages_arg};
            const std::string page_text = localizer.Format("page.indicator", "{0} / {1}", page_args);
            std::snprintf(line, sizeof(line), "%s", page_text.c_str());
            ui->text(ui->user, anomaly::sdk::StringView(line));
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 10.0F);
            const std::string next_label = localizer.Text("page.next", ">");
            if (ActionButton(ui, next_label.c_str(), page + 1 < page_count)) {
                std::scoped_lock lock(g_context.mutex);
                g_context.catalog_page = page + 1 < page_count ? page + 1 : page;
            }
        }

        // 列表：按钮填满整行，选中项的完整路径在下面单独一行展示
        const bool has_child = ui->begin_child != nullptr && ui->end_child != nullptr;
        bool child_open = true;
        if (has_child) {
            // 列表用固定高度、内部滚动；窗口高度交给宿主按内容贴合。
            // 之前用「窗口高 - 底部估算」会因字体/DPI 算不准，内容一超出窗口
            // 宿主就给它加外层滚动条，越拉越难看。固定高度不会撑破窗口。
            constexpr float kListHeight = 360.0F;
            child_open = ui->begin_child(ui->user, anomaly::sdk::StringView("##catalog"), 0.0F,
                                         kListHeight, 0) != 0;
        }
        if (child_open) {
            for (const auto& entry : visible) {
                const std::string label = entry.label + "###blueprint-" + entry.path;
                if (ActionButton(ui, label.c_str(), true, -1.0F)) {
                    std::scoped_lock lock(g_context.mutex);
                    std::snprintf(g_context.class_path.data(), g_context.class_path.size(), "%s",
                                  entry.path.c_str());
                    g_context.last_result_failed = false;
                    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                                  entry.path.c_str());
                }
            }
        }
        if (has_child) ui->end_child(ui->user);
    }
    DrawBlueprintConfig(ui);
}

void DrawSettingsTab(const AnomalyUiServiceV1* ui) {
    const auto& localizer = g_context.localizer;
    std::scoped_lock lock(g_context.mutex);
    if (ui->input_uint32 != nullptr) {
        const auto level_label = localizer.Text("param.level", "Level (0 = default)");
        ui->input_uint32(ui->user, anomaly::sdk::StringView(level_label), &g_context.level, 0, 0);
        g_context.level = std::min(g_context.level, kMaxLevel);
        const auto skill_label = localizer.Text("param.skill_level", "Skill level (0 = default)");
        ui->input_uint32(ui->user, anomaly::sdk::StringView(skill_label), &g_context.skill_level, 0, 0);
        g_context.skill_level = std::min(g_context.skill_level, kMaxLevel);
    }
    WrappedText(ui, localizer.Text("settings.hint",
        "Checked fields override defaults for the next summon"));
    if (ui->separator != nullptr) ui->separator(ui->user);
    if (ui->input_text != nullptr) {
        const auto label = localizer.Text("settings.filter", "Find attribute");
        ui->input_text(ui->user, anomaly::sdk::StringView(label), g_context.attribute_filter.data(),
                       g_context.attribute_filter.size(), 0);
    }
    if (ui->checkbox != nullptr) {
        const auto label = localizer.Text("settings.only_overrides", "Only customized");
        ui->checkbox(ui->user, anomaly::sdk::StringView(label), &g_context.only_overrides);
    }
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 10.0F);
    const auto reset = localizer.Text("settings.reset", "Reset all attributes");
    if (ActionButton(ui, reset.c_str(), true)) {
        g_context.level = 0;
        g_context.skill_level = 0;
        g_context.birth_presets = {};
    }

    const auto enabled_count = std::count_if(g_context.birth_presets.begin(), g_context.birth_presets.end(),
        [](const BirthPreset& preset) { return preset.enabled != 0; });
    const std::string total = std::to_string(g_context.available_birth_attributes);
    const std::string enabled = std::to_string(enabled_count);
    const std::array<std::string_view, 2> args{total, enabled};
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
        "settings.count", "{0} attributes / {1} customized", args)));

    const bool has_child = ui->begin_child != nullptr && ui->end_child != nullptr;
    const bool visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##birth-attributes"), 0.0F, 360.0F, 0) != 0;
    if (visible) {
        const bool has_table = ui->begin_table != nullptr && ui->table_next_row != nullptr &&
            ui->table_next_column != nullptr && ui->end_table != nullptr;
        const bool table = has_table && ui->begin_table(ui->user,
            anomaly::sdk::StringView("##birth-attribute-table"), 2,
            ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT, 0.0F, 0.0F) != 0;
        std::size_t matched{};
        for (std::size_t i = 0; i < g_context.available_birth_attributes; ++i) {
            auto& preset = g_context.birth_presets[i];
            const auto name = kBirthAttributeNames[i];
            const auto& description = g_context.attribute_descriptions[i];
            if (g_context.only_overrides && !preset.enabled) continue;
            if (g_context.attribute_filter[0] != '\0') {
                const bool match = ui->filter_match != nullptr
                    ? ui->filter_match(ui->user, anomaly::sdk::StringView(g_context.attribute_filter.data()),
                                       anomaly::sdk::StringView(description.search_text)) != 0
                    : description.search_text.find(g_context.attribute_filter.data()) != std::string::npos;
                if (!match) continue;
            }
            ++matched;
            if (table) {
                ui->table_next_row(ui->user);
                ui->table_next_column(ui->user);
            }
            const auto help_id = localizer.Label("settings.help", "?", "birth-help-" + std::string(name));
            if (ActionButton(ui, help_id.c_str(), true))
                g_context.attribute_help = g_context.attribute_help == i ? kBirthAttributeNames.size() : i;
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 6.0F);
            // The localized label never changes the reflected field or preset identity.
            const std::string label = description.label + "###birth-enabled-" + std::string(name);
            if (ui->checkbox != nullptr)
                ui->checkbox(ui->user, anomaly::sdk::StringView(label), &preset.enabled);
            else
                ui->text(ui->user, anomaly::sdk::StringView(description.label));
            if (table) ui->table_next_column(ui->user);
            if (preset.enabled && ui->input_double != nullptr) {
                if (ui->struct_size >= offsetof(AnomalyUiServiceV1, set_next_item_width) +
                        sizeof(ui->set_next_item_width) && ui->set_next_item_width != nullptr)
                    ui->set_next_item_width(ui->user, 160.0F);
                const std::string value_id = "##birth-value-" + std::string(name);
                ui->input_double(ui->user, anomaly::sdk::StringView(value_id), &preset.value, 0.0, 0.0);
            } else {
                ui->text(ui->user, anomaly::sdk::StringView(localizer.Text("settings.default", "Default")));
            }
        }
        if (table) ui->end_table(ui->user);
        if (matched == 0)
            ui->text(ui->user, anomaly::sdk::StringView(localizer.Text("settings.no_match", "No matching attributes")));
    }
    if (has_child) ui->end_child(ui->user);
    if (g_context.attribute_help < g_context.available_birth_attributes) {
        const auto& help = g_context.attribute_descriptions[g_context.attribute_help];
        WrappedText(ui, help.label);
        WrappedText(ui, help.description);
    }
    const auto error = BirthPresetError(g_context.birth_presets);
    if (!error.empty()) WrappedText(ui, error);
}

void DrawSummonTab(const AnomalyUiServiceV1* ui, bool ready) {
    const auto& localizer = g_context.localizer;
    bool can_summon{};
    {
        std::scoped_lock lock(g_context.mutex);
        const std::string_view path(g_context.class_path.data());
        const auto separator = path.find_last_of("/.");
        const auto name = separator == std::string_view::npos ? path : path.substr(separator + 1);
        const std::array<std::string_view, 1> target_args{name};
        WrappedText(ui, localizer.Format("summon.target", "Blueprint: {0}", target_args));
        if (ui->input_text != nullptr) {
            const auto label = localizer.Text("param.name", "Name (blank = default)");
            ui->input_text(ui->user, anomaly::sdk::StringView(label), g_context.display_name.data(),
                           g_context.display_name.size(), 0);
        }
        const std::string level = g_context.level == 0 ? localizer.Text("settings.default", "Default") :
            std::to_string(g_context.level);
        const std::string skill = g_context.skill_level == 0 ? localizer.Text("settings.default", "Default") :
            std::to_string(g_context.skill_level);
        const std::string count = std::to_string(std::count_if(
            g_context.birth_presets.begin(), g_context.birth_presets.end(),
            [](const BirthPreset& preset) { return preset.enabled != 0; }));
        const std::array<std::string_view, 3> args{level, skill, count};
        WrappedText(ui, localizer.Format("summon.summary",
            "Level {0} / Skill level {1} / {2} custom attributes", args));
        const auto& hp = g_context.birth_presets[kMaxHpAttribute];
        if (hp.enabled) {
            char value[64]{};
            std::snprintf(value, sizeof(value), "%.9g", hp.value);
            const std::array<std::string_view, 1> value_args{value};
            ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
                "summon.hp", "Max HP: {0}", value_args)));
        }
        if (ui->separator != nullptr) ui->separator(ui->user);
        if (ui->checkbox != nullptr) {
            const auto hostile = localizer.Text("param.hostile", "Hostile");
            ui->checkbox(ui->user, anomaly::sdk::StringView(hostile), &g_context.hostile);
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 12.0F);
            const auto pet = localizer.Text("param.pet", "Pet");
            ui->checkbox(ui->user, anomaly::sdk::StringView(pet), &g_context.pet);
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 12.0F);
            const auto aggressive = localizer.Text("param.aggressive", "Aggressive");
            ui->checkbox(ui->user, anomaly::sdk::StringView(aggressive), &g_context.aggressive);
        }
        if (MonsterConfigName(path).starts_with("FT_")) {
            if (ui->checkbox != nullptr) {
                const auto label = localizer.Text("summon.compatibility", "FT compatibility (experimental)");
                ui->checkbox(ui->user, anomaly::sdk::StringView(label), &g_context.ft_compatibility);
            }
            if (g_context.ft_compatibility != 0) {
                const auto source = MonsterConfigName(g_context.final_tower_combat_blueprint);
                const std::array<std::string_view, 1> source_args{source};
                WrappedText(ui, localizer.Format("summon.server_source", "Server combat: {0}", source_args));
                WrappedText(ui, localizer.Text("summon.compatibility_hint",
                    "Uses source HP, defense and passives; custom HP cannot exceed its native maximum"));
            }
        }
        const auto error = BirthPresetError(g_context.birth_presets);
        if (!error.empty()) WrappedText(ui, error);
        can_summon = ready && g_context.class_path[0] != '\0' &&
            g_context.record_count < kMaxRecords && error.empty();
    }
    DrawBlueprintConfig(ui);
    if (!g_context.birth_attributes_ready.load(std::memory_order_acquire)) {
        WrappedText(ui, localizer.Text("param.birth_unavailable",
            "The native summon initializer is unavailable"));
        can_summon = false;
    }
    const auto label = localizer.Text("action.summon", "Summon");
    if (ActionButton(ui, label.c_str(), can_summon, -1.0F))
        (void)PushOp(PendingOp{OpKind::Summon, 0});
}

void DrawManagementTab(const AnomalyUiServiceV1* ui, bool ready) {
    const auto& localizer = g_context.localizer;
    struct RecordView {
        std::uint64_t id{};
        float hp{};
        float max_hp{};
        bool pet{};
        std::array<char, 96> label{};
    };
    std::array<RecordView, kMaxRecords> records{};
    std::vector<AttributeValue> attributes;
    std::string server_combat_source;
    std::size_t count{};
    std::size_t page{};
    std::size_t page_count{};
    summon_lab::BattleGraph battles;
    const RecordView* selected{};
    {
        std::scoped_lock lock(g_context.mutex);
        count = g_context.record_count;
        page_count = (count + kManagementPageSize - 1) / kManagementPageSize;
        g_context.management_page = page_count == 0 ? 0 :
            std::min(g_context.management_page, page_count - 1);
        page = g_context.management_page;
        battles = g_context.battles;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& source = g_context.records[i];
            records[i].id = source.id;
            records[i].hp = source.hp;
            records[i].max_hp = source.max_hp;
            records[i].pet = source.pet;
            std::memcpy(records[i].label.data(), source.label, records[i].label.size());
            if (source.id == g_context.selected_record_id) {
                selected = &records[i];
                attributes = source.attributes;
                server_combat_source = source.server_combat_source;
            }
        }
    }
    if (count == 0) {
        WrappedText(ui, localizer.Text("list.empty", "No summoned entities"));
        return;
    }
    const bool has_child = ui->begin_child != nullptr && ui->end_child != nullptr &&
        ui->same_line != nullptr;
    const bool list_visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##summoned-list"), 260.0F, 0.0F,
        ANOMALY_UI_CHILD_V1_AUTO_RESIZE_Y) != 0;
    if (list_visible) {
        const std::string count_text = std::to_string(count);
        const std::array<std::string_view, 1> count_args{count_text};
        ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
            "list.header", "Summoned ({0})", count_args)));
        const auto previous = localizer.Label("page.prev", "<", "entity-prev");
        if (ActionButton(ui, previous.c_str(), page > 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.management_page = --page;
        }
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        const std::string page_text = std::to_string(page + 1);
        const std::string pages_text = std::to_string(page_count);
        const std::array<std::string_view, 2> page_args{page_text, pages_text};
        ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
            "manage.page", "{0} / {1}", page_args)));
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        const auto next = localizer.Label("page.next", ">", "entity-next");
        if (ActionButton(ui, next.c_str(), page + 1 < page_count)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.management_page = ++page;
        }
        for (std::size_t i = page * kManagementPageSize;
             i < count && i < (page + 1) * kManagementPageSize; ++i) {
            const auto& record = records[i];
            char line[256]{};
            std::snprintf(line, sizeof(line), "#%llu %s###entity-%llu",
                static_cast<unsigned long long>(record.id), record.label.data(),
                static_cast<unsigned long long>(record.id));
            if (ActionButton(ui, line, selected == nullptr || record.id != selected->id, -1.0F)) {
                std::scoped_lock lock(g_context.mutex);
                selected = nullptr;
                attributes.clear();
                // Copy the selected entity's snapshot; no game reads happen in Draw.
                for (std::size_t k = 0; k < g_context.record_count; ++k) {
                    if (g_context.records[k].id != record.id) continue;
                    g_context.selected_record_id = record.id;
                    selected = &record;
                    attributes = g_context.records[k].attributes;
                    break;
                }
            }
        }
        const auto clear = localizer.Text("action.clear", "Clear all");
        if (ActionButton(ui, clear.c_str(), ready, -1.0F))
            (void)PushOp(PendingOp{OpKind::ClearAll, 0});
    }
    if (has_child) ui->end_child(ui->user);
    if (has_child) ui->same_line(ui->user, 0.0F, 12.0F);
    const bool editor_visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##summoned-editor"), 0.0F, 0.0F,
        ANOMALY_UI_CHILD_V1_AUTO_RESIZE_Y) != 0;
    if (!editor_visible || selected == nullptr) {
        if (has_child) ui->end_child(ui->user);
        return;
    }
    const std::string selected_id = std::to_string(selected->id);
    const std::array<std::string_view, 1> selected_args{selected_id};
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
        "manage.selected", "Manage entity #{0}", selected_args)));
    WrappedText(ui, selected->label.data());
    if (!server_combat_source.empty()) {
        const std::array<std::string_view, 1> source_args{server_combat_source};
        WrappedText(ui, localizer.Format("summon.server_source", "Server combat: {0}", source_args));
    }
    char hp[64]{}, max_hp[64]{};
    std::snprintf(hp, sizeof(hp), "%.0f", static_cast<double>(selected->hp));
    std::snprintf(max_hp, sizeof(max_hp), "%.0f", static_cast<double>(selected->max_hp));
    const std::array<std::string_view, 2> hp_args{hp, max_hp};
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Format(
        "manage.hp", "HP {0} / {1}", hp_args)));
    const auto remove = localizer.Text("action.remove", "Remove");
    if (ActionButton(ui, remove.c_str(), ready))
        (void)PushOp(PendingOp{OpKind::Remove, selected->id});
    if (ui->separator != nullptr) ui->separator(ui->user);
    WrappedText(ui, localizer.Text("duel.opponents", "Opponents (check to fight)"));
    const bool has_unselected_opponent = std::any_of(records.begin(), records.begin() + count,
        [&](const auto& candidate) {
            return candidate.id != selected->id && !candidate.pet && candidate.hp > 0 &&
                candidate.max_hp > 0 && !battles.Linked(selected->id, candidate.id);
        });
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    const auto select_all = localizer.Text("duel.select_all", "Select all");
    if (ActionButton(ui, select_all.c_str(), ready && !selected->pet && selected->hp > 0 &&
        selected->max_hp > 0 && has_unselected_opponent))
        (void)PushOp(PendingOp{OpKind::SelectAllOpponents, selected->id});
    if (selected->pet) WrappedText(ui, localizer.Text("duel.no_pets",
        "Summon combatants with Pet unchecked"));
    const bool opponents_visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##battle-opponents"), 0.0F, 130.0F, 0) != 0;
    if (opponents_visible) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto& candidate = records[i];
            if (candidate.id == selected->id || candidate.pet) continue;
            const std::string text = "#" + std::to_string(candidate.id) + " " + candidate.label.data();
            const std::string label = text + "###battle-" + selected_id + "-" + std::to_string(candidate.id);
            int linked = battles.Linked(selected->id, candidate.id) ? 1 : 0;
            if (ready && !selected->pet && ui->checkbox != nullptr) {
                if (ui->checkbox(ui->user, anomaly::sdk::StringView(label), &linked))
                    (void)PushOp(PendingOp{OpKind::SetBattleEdge, selected->id, linked != 0, candidate.id});
            } else WrappedText(ui, text);
        }
    }
    if (has_child) ui->end_child(ui->user);
    const auto stop_duel = localizer.Text("duel.stop", "Stop this Boss");
    if (ActionButton(ui, stop_duel.c_str(), ready && battles.Contains(selected->id)))
        (void)PushOp(PendingOp{OpKind::StopDuel, selected->id});
    if (ui->separator != nullptr) ui->separator(ui->user);
    const auto action = [&](std::string_view label, OpKind kind, bool enabled) {
        if (ActionButton(ui, std::string(label).c_str(), ready))
            (void)PushOp(PendingOp{kind, selected->id, enabled});
    };
    action(localizer.Text("manage.show_hp", "Show boss HP bar"), OpKind::SetBossHpBar, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.hide_hp", "Hide boss HP bar"), OpKind::SetBossHpBar, false);
    action(localizer.Text("manage.enable_collision", "Enable collision"), OpKind::SetCollision, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.disable_collision", "Disable collision"), OpKind::SetCollision, false);
    action(localizer.Text("manage.hide_model", "Hide model"), OpKind::SetHidden, true);
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    action(localizer.Text("manage.show_model", "Show model"), OpKind::SetHidden, false);
    if (ui->separator != nullptr) ui->separator(ui->user);
    WrappedText(ui, localizer.Text("manage.attributes_hint",
        "Current attributes (read only)"));
    const bool attributes_visible = !has_child || ui->begin_child(ui->user,
        anomaly::sdk::StringView("##current-attributes"), 0.0F, 260.0F, 0) != 0;
    if (attributes_visible) {
        if (attributes.empty())
            WrappedText(ui, localizer.Text("editor.unavailable", "No readable attributes for this entity"));
        for (const auto& attribute : attributes) {
            char base[64]{}, current[64]{};
            std::snprintf(base, sizeof(base), "%.6g", static_cast<double>(attribute.base));
            std::snprintf(current, sizeof(current), "%.6g", static_cast<double>(attribute.current));
            const auto found = std::find(kBirthAttributeNames.begin(), kBirthAttributeNames.end(), attribute.name);
            const std::string_view label = found == kBirthAttributeNames.end() ? attribute.name :
                g_context.attribute_descriptions[static_cast<std::size_t>(found - kBirthAttributeNames.begin())].label;
            const std::array<std::string_view, 3> args{label, base, current};
            WrappedText(ui, localizer.Format(
                "manage.attribute", "{0}  (Base {1} / Current {2})", args));
        }
    }
    if (has_child) ui->end_child(ui->user);
    if (has_child) ui->end_child(ui->user);
}

void ANOMALY_CALL Draw(void* /*context*/, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr)
        return;
    const auto& localizer = g_context.localizer;
    int open = 1;
    // FirstUseEver keeps the window resizable after its first appearance.
    if (ui->set_next_window_size != nullptr)
        ui->set_next_window_size(ui->user, 820.0F, 0.0F, 4U);
    if (ui->set_next_window_size_constraints != nullptr)
        ui->set_next_window_size_constraints(ui->user, 640.0F, 240.0F, 100000.0F, 100000.0F);
    anomaly::sdk::UiWindow window(ui, localizer.Text("window.title", "Summon Lab"), &open);
    if (!window) return;
    const bool has_tabs = ui->struct_size >= offsetof(AnomalyUiServiceV1, end_tab_bar) +
        sizeof(ui->end_tab_bar) && ui->begin_tab_bar != nullptr && ui->begin_tab_item != nullptr &&
        ui->end_tab_item != nullptr && ui->end_tab_bar != nullptr;
    if (!has_tabs) {
        WrappedText(ui, localizer.Text("status.tabs_unavailable", "This host does not provide tab controls"));
        return;
    }
    bool world_ready{}, invoke_ready{}, failed{};
    std::uint32_t pending{};
    std::array<char, kTextCapacity> last_result{};
    {
        std::scoped_lock lock(g_context.mutex);
        world_ready = g_context.session_world_ready;
        invoke_ready = g_context.has_invoke;
        failed = g_context.last_result_failed;
        last_result = g_context.last_result;
        for (std::size_t i = 0; i < g_context.pending_count; ++i)
            if (g_context.pending[i].kind != OpKind::RefreshHP) ++pending;
    }
    if (ui->begin_tab_bar(ui->user, anomaly::sdk::StringView("##summon-lab-tabs"),
                         ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN)) {
        const auto blueprint = localizer.Label("tab.blueprint", "Blueprint", "blueprint");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(blueprint), nullptr, 0, 1)) {
            DrawBlueprintTab(ui);
            ui->end_tab_item(ui->user);
        }
        const auto settings = localizer.Label("tab.attributes", "Attributes", "attributes");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(settings), nullptr, 0, 1)) {
            DrawSettingsTab(ui);
            ui->end_tab_item(ui->user);
        }
        const auto summon = localizer.Label("tab.summon", "Summon", "summon");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(summon), nullptr, 0, 1)) {
            DrawSummonTab(ui, world_ready && invoke_ready);
            ui->end_tab_item(ui->user);
        }
        const auto manage = localizer.Label("tab.manage", "Manage", "manage");
        if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(manage), nullptr, 0, 1)) {
            DrawManagementTab(ui, world_ready && invoke_ready);
            ui->end_tab_item(ui->user);
        }
        ui->end_tab_bar(ui->user);
    }
    if (ui->separator != nullptr) ui->separator(ui->user);
    ui->text(ui->user, anomaly::sdk::StringView(localizer.Text(
        world_ready ? "status.ready" : "status.not_ready", world_ready ? "World ready" : "World not ready")));
    if (pending != 0) {
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 12.0F);
        const std::string pending_text = std::to_string(pending);
        const std::array<std::string_view, 1> args{pending_text};
        ui->text(ui->user, anomaly::sdk::StringView(localizer.Format("status.pending", "{0} queued", args)));
    }
    if (failed) {
        const std::array<std::string_view, 1> args{last_result.data()};
        WrappedText(ui, localizer.Format("status.failed", "Last failure: {0}", args));
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   anomaly::sdk::StringView("local.nte.summon-lab"),
                   anomaly::sdk::StringView("Summon Lab"),
                   anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
                   Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
