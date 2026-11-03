#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/plugin.h>
#include <anomaly/sdk/services/core.h>
#include <anomaly/sdk/services/interop.h>
#include <anomaly/sdk/services/nte.h>
#include <anomaly/sdk/services/ui.h>
#include <anomaly/sdk/services/ue5.h>

#include "anomaly/sdk/localization.hpp"
#include "attribute_catalog.hpp"
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
#include <mutex>

#include <nlohmann/json.hpp>

#include <windows.h>
#include <sstream>
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
using summon_lab::kBirthAttributeNames;
using summon_lab::kMaxHpAttribute;

constexpr int32_t L_object_class = 16;
constexpr int32_t L_object_name = 24;
constexpr int32_t L_object_internalIndex = 12;
// AHTAbilityCharacter and HTAttributeComponent fields from the supplied SDK.
constexpr int32_t L_character_abilitySystem = 0x0940;
constexpr int32_t L_character_attributeSet = 0x0948;
constexpr int32_t L_attribute_set = 0x1C08;
constexpr int32_t L_standalone_attribute_set = 0x1AF8;
constexpr int32_t L_attribute_baseValue = 8;
constexpr int32_t L_attribute_currentValue = 12;
// UE5 reflection metadata and FGameplayAttributeData layout from the supplied CppSDK.
constexpr int32_t L_struct_superStruct = 0x40;
constexpr int32_t L_struct_childProperties = 0x50;
constexpr int32_t L_ffield_next = 0x18;
constexpr int32_t L_ffield_class = 0x08;
constexpr int32_t L_ffield_name = 0x20;
constexpr int32_t L_fproperty_arrayDim = 0x30;
constexpr int32_t L_fproperty_elementSize = 0x34;
constexpr int32_t L_property_offsetInternal = 0x44;

// Function parameter layout from the supplied CppSDK.
constexpr std::size_t kParmSummonAI = 208;
constexpr std::size_t kParmAddSummonedAI = 8;
constexpr std::size_t kParmGetHP = 4;
constexpr std::size_t kParmGetHPMax = 8;
constexpr std::size_t kParmMakeSoftClassPath = 48;
constexpr std::size_t kParmConvSoftClassRef = 72;
constexpr std::size_t kParmGetTransform = 96;

// SummonAI 参数偏移
constexpr std::size_t kSummonClass = 0;
constexpr std::size_t kSummonPet = 40;
constexpr std::size_t kSummonOffsetLocation = 48;
constexpr std::size_t kSummonAttackMode = 72;
constexpr std::size_t kSummonAILevel = 76;
constexpr std::size_t kSummonSkillLevel = 80;
constexpr std::size_t kSummonTransform = 96;
constexpr std::size_t kSummonSameCamp = 192;
constexpr std::size_t kSummonReturn = 200;

// GetHPMax 参数偏移
constexpr std::size_t kGetHpMaxFix = 0;      // bool
constexpr std::size_t kGetHpMaxReturn = 4;   // float

// 软类引用
constexpr std::size_t kMakeSoftInString = 0;
constexpr std::size_t kMakeSoftReturn = 16;
constexpr std::size_t kFSoftClassPathSize = 32;
constexpr std::size_t kConvSoftIn = 0;
constexpr std::size_t kConvSoftReturn = 32;
constexpr std::size_t kTSoftClassPtrSize = 40;

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
constexpr const char* kPathDismissSummonedAI =
    "/Script/HTGame.HTAbilityCharacter:DismissSummonedAI";
// 注意 owner 是 HTAICharacter（AI 自己），receiver 必须是被解散的那个 AI
constexpr const char* kPathSetLifeSpan =
    "/Script/HTGame.HTAICharacter:SetLifeSpanWithReason";
constexpr const char* kPathGetHP = "/Script/HTGame.HTAbilityCharacter:GetHP";
constexpr const char* kPathGetHPMax = "/Script/HTGame.HTAbilityCharacter:GetHPMax";

constexpr const char* kDefaultClassPath =
    "/Game/Blueprints/Character/Monster/BP_WoodTest.BP_WoodTest_C";

constexpr std::uint32_t kHealthRefreshInterval = 60;


// ---------------------------------------------------------------- 类型

enum class OpKind : std::uint32_t {
    Summon = 1,
    Remove = 2,
    ClearAll = 3,
    RefreshHP = 4,
    SetBossHpBar = 5,
    SetCollision = 6,
    SetHidden = 7
};

struct PendingOp {
    OpKind kind{};
    std::uint64_t record_id{};
    bool enabled{};
};

struct ClassEntry {
    std::string label;
    std::string path;
    std::string search_text;
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

struct Record {
    std::uint64_t id{};
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};      // SummonAI 返回的 Actor 指针，Dismiss 需要它
    float hp{};
    float max_hp{};
    std::vector<AttributeValue> attributes;
    char label[96]{};
};

struct NativeCombatHook {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t original{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::sdk::Localizer localizer;
    const AnomalyUiServiceV1* ui{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyHookServiceV1* hooks{};
    std::array<NativeCombatHook, 2> local_combat_hooks{};
    std::uint32_t character_camp_offset{};
    AnomalyGenerationHandleV1 attribute_hook{};
    std::uintptr_t attribute_original{};
    std::uint32_t attribute_property_offset{};
    std::uint32_t object_outer_offset{};
    std::uint32_t container_owner_offset{};
    std::uint32_t spawned_attributes_offset{};
    std::atomic<bool> birth_attributes_ready{false};

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
    std::uint32_t level{1};
    std::uint32_t skill_level{1};
    std::array<BirthPreset, kBirthAttributeNames.size()> birth_presets{};
    std::array<char, 64> attribute_filter{};
    int only_overrides{};
    std::size_t catalog_page{};
    int hostile{};
    int pet{};
    int aggressive{};

    // 离线生成的蓝图目录（assets/summonable.json）
    std::vector<ClassEntry> catalog;
    char catalog_filter[64]{};
    bool catalog_loaded{};
    std::uint64_t selected_record_id{};
    std::size_t management_page{};

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
    if (!ReadValue(pointer + static_cast<std::uintptr_t>(L_object_internalIndex), &internal_index)) return false;
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
// 所以 assets/ 始终在 DLL 旁边，按自身模块路径定位即可。
std::wstring AssetPath(const wchar_t* name) {
    std::array<wchar_t, 4096> buffer{};
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&AssetPath), &module) == FALSE ||
        module == nullptr)
        return {};
    const DWORD size = GetModuleFileNameW(module, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return {};
    const std::filesystem::path path =
        std::filesystem::path(std::wstring(buffer.data(), size)).parent_path() / L"assets" / name;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) ? path.wstring() : std::wstring{};
}

bool LoadCatalog() {
    const std::wstring path = AssetPath(L"summonable.json");
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
        g_context.catalog_loaded = true;
    }
    return true;
}

// ---------------------------------------------------------------- 软类引用

bool BuildSoftClassRef(AnomalyGenerationHandleV1 lib_cdo, const char* utf8_path,
                       std::uint8_t* out40) {
    static wchar_t path_buffer[kPathCapacity];
    std::size_t length = 0;
    while (utf8_path[length] != '\0' && length + 1 < kPathCapacity) {
        path_buffer[length] = static_cast<wchar_t>(static_cast<unsigned char>(utf8_path[length]));
        ++length;
    }
    path_buffer[length] = L'\0';
    const std::int32_t count = static_cast<std::int32_t>(length) + 1;

    std::uint8_t make[kParmMakeSoftClassPath]{};
    std::uintptr_t data = reinterpret_cast<std::uintptr_t>(path_buffer);
    std::memcpy(make + kMakeSoftInString + 0, &data, 8);
    std::memcpy(make + kMakeSoftInString + 8, &count, 4);
    std::memcpy(make + kMakeSoftInString + 12, &count, 4);
    if (!InvokeByPath(lib_cdo, kPathMakeSoftClassPath, make, sizeof(make))) return false;

    std::uint8_t conv[kParmConvSoftClassRef]{};
    std::memcpy(conv + kConvSoftIn, make + kMakeSoftReturn, kFSoftClassPathSize);
    if (!InvokeByPath(lib_cdo, kPathConvSoftClassRef, conv, sizeof(conv))) return false;

    std::memcpy(out40, conv + kConvSoftReturn, kTSoftClassPtrSize);
    return true;
}

// ---------------------------------------------------------------- 属性读写

bool ReadHealth(AnomalyGenerationHandleV1 target, float* hp, float* max_hp) {
    std::uint8_t hp_parms[kParmGetHP]{};
    const bool hp_ok = InvokeByPath(target, kPathGetHP, hp_parms, sizeof(hp_parms));

    std::uint8_t max_parms[kParmGetHPMax]{};
    const std::uint8_t fix = 0;
    std::memcpy(max_parms + kGetHpMaxFix, &fix, 1);
    const bool max_ok = InvokeByPath(target, kPathGetHPMax, max_parms, sizeof(max_parms));

    if (hp_ok) std::memcpy(hp, hp_parms, sizeof(float));
    if (max_ok) std::memcpy(max_hp, max_parms + kGetHpMaxReturn, sizeof(float));
    return hp_ok && max_ok;
}

bool IsActorUsable(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer) {
    // A readable address may already belong to another UObject after GC.
    // Validate the saved serial/generation before inspecting or passing that address.
    if (pointer == 0 || !ProbeHandle(handle)) return false;
    std::uint32_t index{};
    if (!ReadValue(pointer + L_object_internalIndex, &index) ||
        index != ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle)) return false;

    // Registry membership alone includes actors pending destruction. Use the
    // engine's lifetime checks before calling gameplay methods on such actors.
    AnomalyGenerationHandleV1 library{};
    std::array<std::uint8_t, 16> valid{};
    std::memcpy(valid.data(), &pointer, sizeof(pointer));
    if (!FindHandle("/Script/Engine.Default__KismetSystemLibrary", &library) ||
        !InvokeByPath(library, "/Script/Engine.KismetSystemLibrary:IsValid",
                      valid.data(), valid.size()) || valid[8] == 0) return false;
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
    if (!ReadValue(field + L_ffield_class, &field_class) || field_class == 0 ||
        !ReadValue(field_class, &field_class_name) || !ReadValue(field + L_fproperty_arrayDim, &array_dim) ||
        !ReadValue(field + L_fproperty_elementSize, &element_size) ||
        !ReadValue(field + L_property_offsetInternal, &offset))
        return false;

    char field_class_text[64]{};
    if (!ResolveName(field_class_name, field_class_text, sizeof(field_class_text)) ||
        std::strcmp(field_class_text, "StructProperty") != 0)
        return false;

    std::uintptr_t value_struct{};
    std::uint32_t value_struct_name{};
    if (!ReadValue(field + 0x70, &value_struct) || value_struct == 0 ||
        !ReadValue(value_struct + L_object_name, &value_struct_name))
        return false;
    char value_struct_text[64]{};
    if (!ResolveName(value_struct_name, value_struct_text, sizeof(value_struct_text)) ||
        std::strcmp(value_struct_text, "GameplayAttributeData") != 0 || array_dim != 1 ||
        element_size != 16 || offset < 0 || offset >= 0x4000)
        return false;
    *offset_out = offset;
    return true;
}

std::uintptr_t FindProperty(std::uintptr_t uclass, const char* wanted) {
    if (uclass == 0 || wanted == nullptr) return 0;
    for (int class_guard = 0; class_guard < 16 && uclass != 0; ++class_guard) {
        std::uintptr_t field{};
        if (!ReadValue(uclass + L_struct_childProperties, &field)) return 0;
        for (int field_guard = 0; field_guard < 512 && field != 0; ++field_guard) {
            std::uint32_t name_id{};
            char name[kAttributeNameCapacity]{};
            if (ReadValue(field + L_ffield_name, &name_id) &&
                ResolveName(name_id, name, sizeof(name)) && std::strcmp(name, wanted) == 0)
                return field;
            if (!ReadValue(field + L_ffield_next, &field)) return 0;
        }
        if (!ReadValue(uclass + L_struct_superStruct, &uclass)) return 0;
    }
    return 0;
}

bool ReadAttributeValues(std::uintptr_t attribute_set, std::vector<AttributeValue>* values) {
    if (attribute_set == 0 || values == nullptr) return false;
    std::uintptr_t uclass{};
    if (!ReadValue(attribute_set + L_object_class, &uclass) || uclass == 0) return false;

    std::vector<AttributeValue> parsed;
    for (int class_guard = 0; class_guard < 16 && uclass != 0; ++class_guard) {
        std::uintptr_t field{};
        if (!ReadValue(uclass + L_struct_childProperties, &field)) return false;
        for (int field_guard = 0; field_guard < 512 && field != 0; ++field_guard) {
            int32_t offset{};
            std::uint32_t name_id{};
            if (IsGameplayAttributeProperty(field, &offset) &&
                ReadValue(field + L_ffield_name, &name_id) && name_id != 0) {
                AttributeValue value;
                char name[kAttributeNameCapacity]{};
                if (ResolveName(name_id, name, sizeof(name)) &&
                    ReadValue(attribute_set + static_cast<std::uintptr_t>(offset) +
                                  L_attribute_baseValue,
                              &value.base) &&
                    ReadValue(attribute_set + static_cast<std::uintptr_t>(offset) +
                                  L_attribute_currentValue,
                              &value.current)) {
                    value.name = name;
                    parsed.push_back(std::move(value));
                }
            }
            if (!ReadValue(field + L_ffield_next, &field)) return false;
        }
        if (!ReadValue(uclass + L_struct_superStruct, &uclass)) return false;
    }
    *values = std::move(parsed);
    return true;
}

bool ResolveActorAttributeSet(std::uintptr_t actor, std::uintptr_t* ability_system,
                              std::uintptr_t* attribute_set) {
    if (actor == 0 || ability_system == nullptr || attribute_set == nullptr) return false;
    *ability_system = 0;
    *attribute_set = 0;
    if (!ReadValue(actor + L_character_abilitySystem, ability_system)) return false;
    if (ReadValue(actor + L_character_attributeSet, attribute_set) && *attribute_set != 0)
        return true;
    if (*ability_system == 0) return false;
    if (ReadValue(*ability_system + L_attribute_set, attribute_set) && *attribute_set != 0)
        return true;
    return ReadValue(*ability_system + L_standalone_attribute_set, attribute_set) &&
           *attribute_set != 0;
}

bool HealthMatches(float actual, float expected) {
    if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
    return actual == expected;
}

bool ReadMatchingHealth(AnomalyGenerationHandleV1 actor, float expected,
                        float* hp, float* max_hp) {
    if (!ReadHealth(actor, hp, max_hp)) return false;
    std::uint8_t fixed_max[kParmGetHPMax]{};
    fixed_max[kGetHpMaxFix] = 1;
    if (!InvokeByPath(actor, kPathGetHPMax, fixed_max, sizeof(fixed_max))) return false;
    float fixed_value{};
    std::memcpy(&fixed_value, fixed_max + kGetHpMaxReturn, sizeof(fixed_value));
    return HealthMatches(*hp, expected) && HealthMatches(*max_hp, expected) &&
           HealthMatches(fixed_value, expected);
}

// ---------------------------------------------------------------- 出生属性（只改变 GAS 初始化输入）

struct BirthAttribute {
    std::string name;
    float value{};
    bool applied{};
};

struct BirthInitialization {
    std::uintptr_t actor_class{};
    std::uintptr_t actor{};
    std::uintptr_t attribute_set{};
    std::vector<BirthAttribute> attributes;
    float exact_hp{};
    std::uintptr_t registration_controller{};
    std::array<std::uint8_t, 40> registration_leader{};
    std::int32_t registration_index{-1};
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
};
struct PendingBirthConfiguration {
    AnomalyGenerationHandleV1 actor_handle{};
    AnomalyGenerationHandleV1 controller_handle{};
    std::uintptr_t ability_system{};
    std::array<std::uint8_t, 0x34> parameters{};
    float exact_hp{};
    float native_max_hp{};
    bool invalid_max_hp{};
};
std::mutex g_local_birth_mutex;
std::vector<LocalBirthBinding> g_local_births;
std::vector<PendingBirthConfiguration> g_birth_configurations;
std::atomic<bool> g_birth_configuration_ready{false};
std::vector<LocalBirthBinding> g_pending_local_births;
std::atomic<bool> g_restore_local_births{false};

void ForgetLocalBirth(AnomalyGenerationHandleV1 handle) {
    const auto same_actor = [&](const auto& entry) {
        return entry.actor_handle.id == handle.id &&
            entry.actor_handle.generation == handle.generation;
    };
    std::scoped_lock lock(g_local_birth_mutex);
    std::erase_if(g_local_births, same_actor);
    std::erase_if(g_birth_configurations, same_actor);
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
    std::scoped_lock lock(g_context.mutex);
    g_context.records = {};
    g_context.record_count = 0;
    g_context.selected_record_id = 0;
    g_context.management_page = 0;
    g_context.pending_count = 0;
    g_pending_hint.store(0, std::memory_order_release);
}

void RegisterLocalBirth(AnomalyGenerationHandleV1 handle, std::uintptr_t actor,
                        std::uintptr_t ability_system, std::uintptr_t attribute_set,
                        float exact_hp) {
    std::scoped_lock lock(g_local_birth_mutex);
    g_local_births.erase(std::remove_if(g_local_births.begin(), g_local_births.end(),
        [](const LocalBirthBinding& binding) { return !ProbeHandle(binding.actor_handle); }),
        g_local_births.end());
    g_local_births.push_back({handle, actor, ability_system, attribute_set, exact_hp});
}

bool ReadNativeMirrorFloat(const std::array<std::uint8_t, 0x20>& entry, float* value) {
    std::uintptr_t data{};
    std::int32_t size{}, capacity{};
    std::memcpy(&data, entry.data() + 8, sizeof(data));
    std::memcpy(&size, entry.data() + 0x10, sizeof(size));
    std::memcpy(&capacity, entry.data() + 0x14, sizeof(capacity));
    // The native mirror uses an uncompressed FMemoryReader for scalar floats.
    return entry[0x18] == 0 && size == sizeof(float) && capacity >= size &&
        ReadValue(data, value) && std::isfinite(*value) && *value > 0.0F;
}

void CaptureNativeBirthMaximum(std::uintptr_t ability_system,
                              const std::array<std::uint8_t, 0x20>& entry) {
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
        std::memcpy(pending.parameters.data() + 0x30, &percent, sizeof(percent));
        const bool configured = !pending.invalid_max_hp &&
            InvokeByPath(pending.controller_handle,
                "/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode",
                pending.parameters.data(), pending.parameters.size());
        if (!configured) {
            static_cast<void>(InvokeByPath(pending.actor_handle, kPathDestroyActor, nullptr, 0));
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
                        {"exactHP", binding.exact_hp}});
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
            g_pending_local_births.push_back(binding);
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
            RegisterLocalBirth(current, binding.actor, asc, set, binding.exact_hp);
        }
    }
    g_pending_local_births.clear();
}

bool IsBirthAttributeSet(std::uintptr_t self, BirthInitialization& birth) {
    if (birth.attribute_set != 0) return self == birth.attribute_set;
    std::uintptr_t owner = self;
    for (int depth = 0; depth < 4; ++depth) {
        if (!ReadValue(owner + g_context.object_outer_offset, &owner) || owner == 0)
            return false;
        std::uintptr_t owner_class{};
        if (!ReadValue(owner + L_object_class, &owner_class)) return false;
        if (owner_class != birth.actor_class) continue;
        std::uint32_t flags{};
        if (!ReadValue(owner + 8, &flags) || (flags & 0x30U) != 0) return false;
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
                       g_context.attribute_property_offset, &property) ||
        !IsGameplayAttributeProperty(property, &offset) ||
        !ReadValue(property + L_ffield_name, &name_id) ||
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
    if (!ReadValue(container + g_context.container_owner_offset, &asc) || asc == 0 ||
        !ReadValue(asc + g_context.spawned_attributes_offset, &sets) || sets.data == 0 ||
        sets.count <= 0 || sets.count > 64 || sets.capacity < sets.count)
        return;
    for (std::int32_t i = 0; i < sets.count; ++i) {
        std::uintptr_t set{}, set_class{};
        if (!ReadValue(sets.data + static_cast<std::uintptr_t>(i) * sizeof(set), &set) ||
            set == 0 || !ReadValue(set + L_object_class, &set_class) ||
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

bool InstallBirthAttributeHook(const anomaly::sdk::Host& query) {
    const auto signature = query.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    g_context.hooks = query.Query<AnomalyHookServiceV1>(
        ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION).get();
    if (!signature || signature->resolve == nullptr || g_context.hooks == nullptr ||
        g_context.hooks->create == nullptr || g_context.hooks->release == nullptr ||
        g_context.hooks->begin_callback == nullptr || g_context.hooks->end_callback == nullptr)
        return false;
    const auto path = AssetPath(L"spawn-attributes.profile.json");
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const auto profile = nlohmann::json::parse(file, nullptr, false);
    if (profile.is_discarded() || profile.value("schemaVersion", 0) != 1 ||
        profile.value("validator", std::string{}) != "gas-base-input-v1")
        return false;
    const auto pattern = profile.value("pattern", std::string{});
    const auto module = profile.value("module", std::string{});
    const auto section = profile.value("section", std::string{});
    g_context.attribute_property_offset = profile.at("layout").at("attributeProperty");
    g_context.object_outer_offset = profile.at("layout").at("objectOuter");
    g_context.container_owner_offset = profile.at("layout").at("containerOwner");
    g_context.spawned_attributes_offset = profile.at("layout").at("spawnedAttributes");
    if (pattern.empty() || module.empty() || section != ".text" ||
        g_context.attribute_property_offset != 0x10 || g_context.object_outer_offset != 0x20 ||
        g_context.container_owner_offset != 0x108 ||
        g_context.spawned_attributes_offset != 0x10B0)
        return false;
    std::uintptr_t target{};
    if (signature->resolve(signature->user, anomaly::sdk::StringView(module),
            anomaly::sdk::StringView(section), anomaly::sdk::StringView(pattern), &target).code !=
                ANOMALY_STATUS_V1_OK || target == 0)
        return false;
    // Validated live entry: XMM2 holds float NewBaseValue, and RCX is the
    // FActiveGameplayEffectsContainer whose Owner ASC is loaded at +0x108.
    constexpr std::array<std::uint8_t, 23> entry{
        0xF3, 0x0F, 0x11, 0x54, 0x24, 0x18, 0x53, 0x56, 0x57,
        0x48, 0x81, 0xEC, 0x90, 0, 0, 0, 0x48, 0x8B, 0xB1, 0x08, 0x01, 0, 0};
    std::array<std::uint8_t, entry.size()> bytes{};
    if (!ReadBytes(target, bytes.data(), bytes.size()) || bytes != entry) return false;
    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
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
                ReadValue(reinterpret_cast<std::uintptr_t>(class_cdo) + L_object_class, &uclass) &&
                uclass == birth->actor_class && index >= 0 && leader != nullptr) {
                // Preserve the exact native leader identity and summon index.
                // Configuration must use the same registration, never a fabricated actor ID.
                if (ReadBytes(reinterpret_cast<std::uintptr_t>(leader),
                        birth->registration_leader.data(), birth->registration_leader.size())) {
                    birth->registration_controller = reinterpret_cast<std::uintptr_t>(controller);
                    birth->registration_index = index;
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
        alignas(void*) std::array<std::uint8_t, 0x58> view{};
        std::vector<std::array<std::uint8_t, 0x20>> filtered;
        const void* input = record;
        try {
            std::uintptr_t target{};
            std::uintptr_t actor{}, current_ability_system{};
            AnomalyGenerationHandleV1 handle{};
            if (lease && record != nullptr &&
                ReadValue(reinterpret_cast<std::uintptr_t>(record) + 0x18, &target)) {
                std::scoped_lock lock(g_local_birth_mutex);
                for (const auto& binding : g_local_births) {
                    if (binding.ability_system == target && binding.exact_hp > 0.0F) {
                        handle = binding.actor_handle;
                        actor = binding.actor;
                        break;
                    }
                }
            }
            if (handle.id != 0 && ProbeHandle(handle) &&
                ReadValue(actor + L_character_abilitySystem, &current_ability_system) &&
                current_ability_system == target &&
                ReadBytes(reinterpret_cast<std::uintptr_t>(record), view.data(), view.size())) {
                std::uintptr_t vtable{}, getter{}, values{};
                std::int32_t count{}, capacity{};
                std::memcpy(&vtable, view.data(), sizeof(vtable));
                std::memcpy(&values, view.data() + 8, sizeof(values));
                std::memcpy(&count, view.data() + 0x10, sizeof(count));
                std::memcpy(&capacity, view.data() + 0x14, sizeof(capacity));
                constexpr std::array<std::uint8_t, 5> getter_entry{0x48, 0x8B, 0x41, 0x18, 0xC3};
                std::array<std::uint8_t, getter_entry.size()> actual{};
                if (values != 0 && count > 0 && count <= capacity && count <= 512 &&
                    ReadValue(vtable + 8, &getter) && ReadBytes(getter, actual.data(), actual.size()) &&
                    actual == getter_entry) {
                    std::vector<std::array<std::uint8_t, 0x20>> entries(static_cast<std::size_t>(count));
                    if (ReadBytes(values, entries.data(), entries.size() * sizeof(entries[0]))) {
                        for (const auto& entry : entries) {
                            std::uint32_t id{};
                            char name[64]{};
                            std::memcpy(&id, entry.data(), sizeof(id));
                            const bool protected_max = ResolveName(id, name, sizeof(name)) &&
                                (std::strcmp(name, "MaxHP") == 0 || std::strcmp(name, "MaxHPTemp") == 0);
                            if (protected_max && std::strcmp(name, "MaxHPTemp") == 0)
                                CaptureNativeBirthMaximum(target, entry);
                            if (!protected_max) filtered.push_back(entry);
                        }
                        if (filtered.size() != entries.size()) {
                            const auto pointer = reinterpret_cast<std::uintptr_t>(filtered.data());
                            const auto size = static_cast<std::int32_t>(filtered.size());
                            std::memcpy(view.data() + 8, &pointer, sizeof(pointer));
                            std::memcpy(view.data() + 0x10, &size, sizeof(size));
                            std::memcpy(view.data() + 0x14, &size, sizeof(size));
                            input = view.data();
                        }
                    }
                }
            }
        } catch (...) { input = record; }
        if (original != nullptr) original(context, owner, input, scratch);
    } catch (...) {}
}

bool InstallLocalCombatHooks(const anomaly::sdk::Host& query) {
    const auto signature = query.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    if (!signature || g_context.hooks == nullptr) return false;
    std::ifstream file(AssetPath(L"local-combat.profile.json"), std::ios::binary);
    const auto profile = nlohmann::json::parse(file, nullptr, false);
    if (profile.is_discarded() || profile.value("schemaVersion", 0) != 1 ||
        profile.value("game", std::string{}) != "nte" ||
        profile.value("module", std::string{}) != "HTGame.exe" ||
        profile.value("section", std::string{}) != ".text" ||
        profile.value("validator", std::string{}) != "local-birth-combat-v2" ||
        !profile.contains("symbols") || profile["symbols"].size() != 2 ||
        !profile.contains("layout")) return false;
    const auto& layout = profile.at("layout");
    g_context.character_camp_offset = layout.at("characterCamp");
    if (g_context.character_camp_offset != 0xFCC ||
        layout.value("characterForNetSize", 0) != 40 ||
        layout.value("mirrorRecordSize", 0) != 0x58 ||
        layout.value("mirrorObject", 0) != 0x18 ||
        layout.value("mirrorValues", 0) != 8 ||
        layout.value("mirrorValueCount", 0) != 0x10 ||
        layout.value("mirrorValueStride", 0) != 0x20 ||
        layout.value("mirrorValueData", 0) != 8 ||
        layout.value("mirrorValueBytes", 0) != 0x10 ||
        layout.value("mirrorValueCapacity", 0) != 0x14 ||
        layout.value("mirrorValueCompressed", 0) != 0x18) return false;
    // Custom HP is established through native birth configuration. Combat,
    // authority checks, reporting and HP commits all use the game's own path.
    constexpr std::array<std::string_view, 2> names{
        "summon-registration", "local-attribute-mirror"};
    const std::array<void*, 2> detours{
        reinterpret_cast<void*>(&LocalRegistrationDetour),
        reinterpret_cast<void*>(&PreserveLocalBirthMaxHP)};
    bool installed = true;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& symbol = profile["symbols"][i];
        std::uintptr_t target{};
        const auto pattern = symbol.at("pattern").get<std::string>();
        if (symbol.value("name", std::string{}) != names[i] ||
            signature->resolve(signature->user, anomaly::sdk::StringView("HTGame.exe"),
                anomaly::sdk::StringView(".text"), anomaly::sdk::StringView(pattern), &target).code !=
                    ANOMALY_STATUS_V1_OK || target == 0) { installed = false; break; }
        if (i == 0) {
            if (symbol.value("callOffset", -1) != 14) { installed = false; break; }
            std::array<std::uint8_t, 5> call{};
            if (!ReadBytes(target + 14, call.data(), call.size()) || call[0] != 0xE8) {
                installed = false; break;
            }
            std::int32_t displacement{};
            std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
            target = target + 19 + displacement;
        }
        std::istringstream entry(symbol.at("entry").get<std::string>());
        std::vector<std::uint8_t> expected;
        unsigned byte{};
        while (entry >> std::hex >> byte) {
            if (byte > 255) { installed = false; break; }
            expected.push_back(static_cast<std::uint8_t>(byte));
        }
        std::vector<std::uint8_t> actual(expected.size());
        if (!installed || expected.empty() || !ReadBytes(target, actual.data(), actual.size()) ||
            actual != expected) { installed = false; break; }
        AnomalyHookRequestV1 request{sizeof(request)};
        request.kind = ANOMALY_HOOK_V1_FUNCTION;
        request.target = target;
        request.detour = detours[i];
        request.label = anomaly::sdk::StringView(names[i]);
        auto& hook = g_context.local_combat_hooks[i];
        if (g_context.hooks->create(g_context.hooks->user, &request, &hook.original,
                &hook.handle).code != ANOMALY_STATUS_V1_OK || hook.handle.id == 0 ||
                hook.original == 0) { installed = false; break; }
    }
    if (!installed) static_cast<void>(ReleaseLocalCombatHooks());
    return installed;
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

bool DismissSummoned(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer);

// ---------------------------------------------------------------- 召唤

struct SummonResult {
    bool ok{};
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    std::uintptr_t ability_system{};
    std::uintptr_t attribute_set{};
    std::vector<AttributeValue> attributes;
    char label[96]{};
    char detail[kTextCapacity]{};
};

std::string BirthPresetError(const std::array<BirthPreset, kBirthAttributeNames.size()>& presets) {
    const auto& localizer = g_context.localizer;
    const auto& max_hp = presets[kMaxHpAttribute];
    if (max_hp.enabled && (!std::isfinite(max_hp.value) || max_hp.value <= 0.0 ||
        max_hp.value > 10000000.0 || static_cast<float>(max_hp.value) <= 0.0F)) {
        return localizer.Text("settings.invalid_hp", "HPMaxBase must be greater than 0 and at most 10000000.");
    }
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (!presets[i].enabled) continue;
        const auto name = kBirthAttributeNames[i];
        const std::array<std::string_view, 1> args{name};
        if (!std::isfinite(presets[i].value) ||
            std::abs(presets[i].value) > std::numeric_limits<float>::max()) {
            return localizer.Format("settings.invalid_value", "{0} must be a finite float value.", args);
        }
        if (max_hp.enabled && (name == "HPMaxUp" || name == "HPMaxAdd" ||
            name == "HPMaxUpTemp" || name == "HPMaxAddTemp" || name == "OnlineMaxHPRatio")) {
            return localizer.Format("settings.hp_conflict", "Disable {0} when setting HPMaxBase.", args);
        }
    }
    return {};
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

    std::uint8_t soft_class[kTSoftClassPtrSize]{};
    if (!BuildSoftClassRef(lib_cdo, path, soft_class)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: soft class path failed");
        return result;
    }

    std::uint8_t transform[kParmGetTransform]{};
    if (!InvokeByPath(pawn, kPathGetTransform, transform, sizeof(transform))) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: GetTransform failed");
        return result;
    }

    std::uint32_t level{};
    std::uint32_t skill_level{};
    bool hostile{};
    bool pet{};
    bool aggressive{};
    std::array<BirthPreset, kBirthAttributeNames.size()> presets{};
    {
        std::scoped_lock lock(g_context.mutex);
        level = g_context.level;
        skill_level = g_context.skill_level;
        hostile = g_context.hostile != 0;
        pet = g_context.pet != 0;
        aggressive = g_context.aggressive != 0;
        presets = g_context.birth_presets;
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
        std::uint8_t load_class[48]{};
        std::memcpy(load_class, soft_class, sizeof(soft_class));
        if (!InvokeByPath(lib_cdo, "/Script/Engine.KismetSystemLibrary:LoadClassAsset_Blocking",
                          load_class, sizeof(load_class))) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: class load failed");
            return result;
        }
        std::memcpy(&birth.actor_class, load_class + 40, sizeof(birth.actor_class));
        if (birth.actor_class == 0) {
            std::snprintf(result.detail, sizeof(result.detail), "summon: class load returned null");
            return result;
        }
    }

    std::uint8_t parms[kParmSummonAI]{};
    std::memcpy(parms + kSummonClass, soft_class, kTSoftClassPtrSize);
    const std::uint8_t pet_flag = pet ? 1 : 0;
    std::memcpy(parms + kSummonPet, &pet_flag, 1);
    const double offset_location[3] = {0.0, 0.0, 0.0};
    std::memcpy(parms + kSummonOffsetLocation, offset_location, sizeof(offset_location));
    const std::uint8_t attack_mode = aggressive ? kAttackAggressive : kAttackDefensive;
    std::memcpy(parms + kSummonAttackMode, &attack_mode, 1);
    const auto ai_level = static_cast<std::int32_t>(std::clamp(level, 1U, kMaxLevel));
    std::memcpy(parms + kSummonAILevel, &ai_level, 4);
    const auto ai_skill_level = static_cast<std::int32_t>(std::clamp(skill_level, 1U, kMaxLevel));
    std::memcpy(parms + kSummonSkillLevel, &ai_skill_level, 4);
    std::memcpy(parms + kSummonTransform, transform, 96);
    const std::uint8_t same_camp = hostile ? 0 : 1;
    std::memcpy(parms + kSummonSameCamp, &same_camp, 1);

    bool summoned{};
    {
        // SummonAI itself defers construction, configures level/leader/camp,
        // then finishes spawning. GAS receives our inputs inside that call;
        // no actor attributes or HP caches are patched after it returns.
        BirthScope scope(birth);
        summoned = InvokeByPath(pawn, kPathSummonAI, parms, sizeof(parms));
    }
    if (!summoned) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: SummonAI rejected");
        return result;
    }

    std::uintptr_t ai_pointer{};
    std::memcpy(&ai_pointer, parms + kSummonReturn, sizeof(ai_pointer));
    if (ai_pointer == 0) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: returned null");
        return result;
    }

    AnomalyGenerationHandleV1 ai_handle{};
    if (!HandleFromPointer(ai_pointer, &ai_handle)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: AI handle unresolved");
        return result;
    }

    if (!ResolveActorAttributeSet(ai_pointer, &result.ability_system, &result.attribute_set) ||
        result.ability_system == 0) {
        static_cast<void>(InvokeByPath(ai_handle, kPathDestroyActor, nullptr, 0));
        std::snprintf(result.detail, sizeof(result.detail), "summon: local combat attributes unresolved");
        return result;
    }
    RegisterLocalBirth(ai_handle, ai_pointer, result.ability_system,
                       result.attribute_set, birth.exact_hp);

    // 未登记进召唤列表的 AI 没有归属，敌对化之后会在 AI tick 触发访问违例
    {
        std::uint8_t add_parms[kParmAddSummonedAI]{};
        std::memcpy(add_parms, &ai_pointer, sizeof(ai_pointer));
        if (!InvokeByPath(pawn, kPathAddSummonedAI, add_parms, sizeof(add_parms))) {
            static_cast<void>(InvokeByPath(ai_handle, kPathDestroyActor, nullptr, 0));
            std::snprintf(result.detail, sizeof(result.detail),
                          "summon: AddSummonedAI failed");
            return result;
        }
    }

    result.ok = true;
    result.handle = ai_handle;
    result.pointer = ai_pointer;
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
        AnomalyGenerationHandleV1 controller{};
        std::uint8_t camp{};
        std::array<std::uint8_t, 0x34> configuration{};
        std::memcpy(configuration.data(), birth.registration_leader.data(), 0x28);
        std::memcpy(configuration.data() + 0x28, &birth.registration_index, sizeof(birth.registration_index));
        const float initial_hp_percent = 1.0F;
        std::memcpy(configuration.data() + 0x30, &initial_hp_percent, sizeof(initial_hp_percent));
        bool configured = HandleFromPointer(birth.registration_controller, &controller) &&
            ReadValue(ai_pointer + g_context.character_camp_offset, &camp);
        configuration[0x2C] = camp;
        if (configured && birth.exact_hp > 0.0F) {
            std::scoped_lock lock(g_local_birth_mutex);
            g_birth_configurations.push_back({ai_handle, controller, result.ability_system,
                configuration, birth.exact_hp});
        } else {
            configured = configured && InvokeByPath(controller,
                "/Script/HTGame.HTPlayerController:ServerConfigureSummonedAI_HybridMode",
                configuration.data(), configuration.size());
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
    // 取蓝图短名做显示
    const char* separator = std::strrchr(path, '.');
    if (separator == nullptr) separator = std::strrchr(path, '/');
    const char* name = separator != nullptr ? separator + 1 : path;
    std::snprintf(result.label, sizeof(result.label), "%s", name);
    std::snprintf(result.detail, sizeof(result.detail),
                  "summoned %s (level %d, %s)", name, ai_level,
                  birth.attributes.empty() ? "native attributes" : "birth attributes initialized");
    return result;
}

bool DismissSummoned(AnomalyGenerationHandleV1 handle, std::uintptr_t pointer) {
    ForgetLocalBirth(handle);
    // Dead actors already follow the game's death cleanup. A stale list entry
    // must never send their cached address back into the summon/destroy paths.
    if (!IsActorAlive(handle, pointer)) return true;
    float hp{}, max_hp{};
    if (!ReadHealth(handle, &hp, &max_hp)) return false;
    if (hp <= 0.0F || max_hp <= 0.0F) return true;

    bool dismissed = false;
    AnomalyGenerationHandleV1 pawn{};
    if (FindLocalPawn(&pawn)) {
        std::uint8_t parms[8]{};
        std::memcpy(parms, &pointer, sizeof(pointer));
        dismissed = InvokeByPath(pawn, kPathDismissSummonedAI, parms, sizeof(parms));
    }

    // Each engine mutation can begin destruction synchronously. Only advance
    // to the next cleanup step while the original actor is still usable.
    if (IsActorUsable(handle, pointer)) {
        std::uint8_t lifespan_parms[8]{};
        const float lifespan = 0.01F;
        const std::uint8_t reason = 0;   // EHTAISetLifeSpanReason 首项
        std::memcpy(lifespan_parms, &lifespan, sizeof(lifespan));
        std::memcpy(lifespan_parms + 4, &reason, 1);
        static_cast<void>(InvokeByPath(handle, kPathSetLifeSpan, lifespan_parms,
                                       sizeof(lifespan_parms)));
        if (IsActorUsable(handle, pointer))
            static_cast<void>(InvokeByPath(handle, kPathDestroyActor, nullptr, 0));
    }

    return dismissed;
}

void ForgetRecord(std::uint64_t record_id) {
    AnomalyGenerationHandleV1 handle{};
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.record_count; ++i) {
            if (g_context.records[i].id != record_id) continue;
            handle = g_context.records[i].handle;
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
            [&](const PendingOp& op) { return op.record_id == record_id; });
        g_context.pending_count = static_cast<std::size_t>(end - g_context.pending.begin());
        g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending_count),
                             std::memory_order_release);
    }
    if (handle.id != 0) ForgetLocalBirth(handle);
}

void RemoveOne(std::uint64_t record_id) {
    AnomalyGenerationHandleV1 handle{};
    std::uintptr_t pointer{};
    {
        std::scoped_lock lock(g_context.mutex);
        for (std::size_t i = 0; i < g_context.record_count; ++i) {
            if (g_context.records[i].id != record_id) continue;
            handle = g_context.records[i].handle;
            pointer = g_context.records[i].pointer;
            break;
        }
    }
    static_cast<void>(DismissSummoned(handle, pointer));
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
            std::uint8_t parameters[2]{op.enabled ? std::uint8_t{0} : std::uint8_t{1}, 1};
            applied = InvokeByPath(handle, "/Script/HTGame.HTAICharacter:SetDisableBossHPBar",
                                   parameters, sizeof(parameters));
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
        "manage.failed", "Action failed: the entity or function is unavailable.");
    std::scoped_lock lock(g_context.mutex);
    g_context.last_result_failed = !applied;
    std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                  detail.c_str());
}


void RefreshHealth() {

    struct HealthTarget {
        std::uint64_t id{};
        AnomalyGenerationHandleV1 handle{};
        std::uintptr_t pointer{};
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
                           g_context.records[i].pointer};
    }
    for (std::size_t i = 0; i < count && i < kMaxRecords; ++i) {
        if (!IsActorAlive(snapshot[i].handle, snapshot[i].pointer)) {
            ForgetRecord(snapshot[i].id);
            continue;
        }
        float hp{};
        float max_hp{};
        if (!ReadHealth(snapshot[i].handle, &hp, &max_hp)) continue;
        if (hp <= 0.0F || max_hp <= 0.0F) {
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
        hp > 10000000U || level < 1 || level > kMaxLevel ||
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
        break;
    case OpKind::SetBossHpBar:
    case OpKind::SetCollision:
    case OpKind::SetHidden:
        SetActorOption(op);
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
    std::snprintf(g_context.class_path.data(), g_context.class_path.size(), "%s",
                  kDefaultClassPath);
    g_context.level = 1;
    g_context.skill_level = 1;
    g_context.birth_presets = {};
    g_context.attribute_filter = {};
    g_context.only_overrides = 0;
    g_context.catalog_page = 0;
    g_context.hostile = 0;
    g_context.pet = 0;
    g_context.aggressive = 0;
    g_context.last_result = {};
    g_context.last_result_failed = false;
    g_context.catalog.clear();
    g_context.catalog_filter[0] = '\0';
    g_context.catalog_loaded = false;
    g_context.selected_record_id = 0;
    g_context.management_page = 0;
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

    g_context.birth_attributes_ready.store(InstallBirthAttributeHook(query) &&
        InstallLocalCombatHooks(query), std::memory_order_release);
    static_cast<void>(LoadCatalog());
    ConsumeSpawnRequest();

    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    g_context.events = nullptr;
    g_context.birth_attributes_ready.store(false, std::memory_order_release);
    const auto local_status = ReleaseLocalCombatHooks();
    if (local_status.code != ANOMALY_STATUS_V1_OK) return local_status;
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

void ANOMALY_CALL Update(void* /*context*/, double /*delta_seconds*/) {
    ++g_context.frame_counter;
    EnsureServices();
    UpdateSessionState();
    if (!g_context.session_world_ready) return;
    // The host runs on_update at its Game tick boundary. Do not invoke spawn
    // or configuration from arbitrary ProcessEvent callbacks: those may be
    // nested inside UI scripts or a native attribute mirror's deserialization.
    RestoreLocalBirthsOnGameThread();
    ConfigureNativeBirthsOnGameThread();
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

}

void DrawSettingsTab(const AnomalyUiServiceV1* ui) {
    const auto& localizer = g_context.localizer;
    std::scoped_lock lock(g_context.mutex);
    if (ui->input_uint32 != nullptr) {
        const auto level_label = localizer.Text("param.level", "Level");
        ui->input_uint32(ui->user, anomaly::sdk::StringView(level_label), &g_context.level, 0, 0);
        g_context.level = std::clamp(g_context.level, 1U, kMaxLevel);
        const auto skill_label = localizer.Text("param.skill_level", "Skill level");
        ui->input_uint32(ui->user, anomaly::sdk::StringView(skill_label), &g_context.skill_level, 0, 0);
        g_context.skill_level = std::clamp(g_context.skill_level, 1U, kMaxLevel);
    }
    WrappedText(ui, localizer.Text("settings.hint",
        "Checked fields override defaults for the next summon."));
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
    if (ActionButton(ui, reset.c_str(), true)) g_context.birth_presets = {};

    const auto enabled_count = std::count_if(g_context.birth_presets.begin(), g_context.birth_presets.end(),
        [](const BirthPreset& preset) { return preset.enabled != 0; });
    const std::string total = std::to_string(kBirthAttributeNames.size());
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
        for (std::size_t i = 0; i < kBirthAttributeNames.size(); ++i) {
            auto& preset = g_context.birth_presets[i];
            const auto name = kBirthAttributeNames[i];
            if (g_context.only_overrides && !preset.enabled) continue;
            if (g_context.attribute_filter[0] != '\0') {
                const bool match = ui->filter_match != nullptr
                    ? ui->filter_match(ui->user, anomaly::sdk::StringView(g_context.attribute_filter.data()),
                                       anomaly::sdk::StringView(name)) != 0
                    : name.find(g_context.attribute_filter.data()) != std::string_view::npos;
                if (!match) continue;
            }
            ++matched;
            if (table) {
                ui->table_next_row(ui->user);
                ui->table_next_column(ui->user);
            }
            // Field names are the raw SDK identifiers used by native reflection.
            if (ui->checkbox != nullptr)
                ui->checkbox(ui->user, anomaly::sdk::StringView(name), &preset.enabled);
            else
                ui->text(ui->user, anomaly::sdk::StringView(name));
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
            ui->text(ui->user, anomaly::sdk::StringView(localizer.Text("settings.no_match", "No matching attributes.")));
    }
    if (has_child) ui->end_child(ui->user);
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
        const std::string level = std::to_string(g_context.level);
        const std::string skill = std::to_string(g_context.skill_level);
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
        const auto error = BirthPresetError(g_context.birth_presets);
        if (!error.empty()) WrappedText(ui, error);
        can_summon = ready && g_context.class_path[0] != '\0' &&
            g_context.record_count < kMaxRecords && error.empty();
    }
    if (!g_context.birth_attributes_ready.load(std::memory_order_acquire)) {
        WrappedText(ui, localizer.Text("param.birth_unavailable",
            "The native summon initializer is unavailable."));
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
        std::array<char, 96> label{};
    };
    std::array<RecordView, kMaxRecords> records{};
    std::vector<AttributeValue> attributes;
    std::size_t count{};
    std::size_t page{};
    std::size_t page_count{};
    const RecordView* selected{};
    {
        std::scoped_lock lock(g_context.mutex);
        count = g_context.record_count;
        page_count = (count + kManagementPageSize - 1) / kManagementPageSize;
        g_context.management_page = page_count == 0 ? 0 :
            std::min(g_context.management_page, page_count - 1);
        page = g_context.management_page;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& source = g_context.records[i];
            records[i].id = source.id;
            records[i].hp = source.hp;
            records[i].max_hp = source.max_hp;
            std::memcpy(records[i].label.data(), source.label, records[i].label.size());
            if (source.id == g_context.selected_record_id) {
                selected = &records[i];
                attributes = source.attributes;
            }
        }
    }
    if (count == 0) {
        WrappedText(ui, localizer.Text("list.empty", "No summoned entities."));
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
            WrappedText(ui, localizer.Text("editor.unavailable", "No readable attributes for this entity."));
        for (const auto& attribute : attributes) {
            char base[64]{}, current[64]{};
            std::snprintf(base, sizeof(base), "%.6g", static_cast<double>(attribute.base));
            std::snprintf(current, sizeof(current), "%.6g", static_cast<double>(attribute.current));
            const std::array<std::string_view, 3> args{attribute.name, base, current};
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
        WrappedText(ui, localizer.Text("status.tabs_unavailable", "This host does not provide tab controls."));
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
