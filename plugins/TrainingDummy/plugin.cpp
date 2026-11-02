#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/plugin.h>
#include <anomaly/sdk/services/core.h>
#include <anomaly/sdk/services/nte.h>
#include <anomaly/sdk/services/ui.h>
#include <anomaly/sdk/services/ue5.h>

#include "../common/localization.hpp"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

// ---------------------------------------------------------------- 常量

constexpr std::size_t kMaxDummies = 32;
constexpr std::size_t kMaxPending = 16;
constexpr std::size_t kTextCapacity = 248;
constexpr std::size_t kPageCapacity = 128;

constexpr std::uint32_t kActorRefreshInterval = 30;

constexpr int32_t L_object_class = 16;
constexpr int32_t L_object_name = 24;
constexpr int32_t L_object_internalIndex = 12;


// 实测 ParmsSize
constexpr std::size_t kParmSummonAI = 208;
constexpr std::size_t kParmMakeSoftClassPath = 48;
constexpr std::size_t kParmConvSoftClassRef = 72;
constexpr std::size_t kParmGetTransform = 96;

// SummonAI 参数偏移（TSoftClassPtr=40, FTransform 对齐到 96, ret 200）
constexpr std::size_t kSummonAIClass = 0;
constexpr std::size_t kSummonBPet = 40;
constexpr std::size_t kSummonOffsetLocation = 48;
constexpr std::size_t kSummonAttackMode = 72;
constexpr std::size_t kSummonAILevel = 76;
constexpr std::size_t kSummonSkillLevel = 80;
constexpr std::size_t kSummonTransform = 96;
constexpr std::size_t kSummonSameCamp = 192;
constexpr std::size_t kSummonReturn = 200;

// MakeSoftClassPath: FString(16) + ret(16)
constexpr std::size_t kMakeSoftInString = 0;
constexpr std::size_t kMakeSoftReturn = 16;
constexpr std::size_t kFSoftClassPathSize = 32;

// Conv_SoftClassPathToSoftClassRef: FSoftClassPath(32) + ret(32)
constexpr std::size_t kConvSoftIn = 0;
constexpr std::size_t kConvSoftReturn = 32;
constexpr std::size_t kTSoftClassPtrSize = 40;

constexpr const char* kGameplayStaticsCdo = "/Script/Engine.Default__GameplayStatics";
constexpr const char* kPathMakeSoftClassPath =
    "/Script/Engine.KismetSystemLibrary:MakeSoftClassPath";
constexpr const char* kPathConvSoftClassRef =
    "/Script/Engine.KismetSystemLibrary:Conv_SoftClassPathToSoftClassRef";
constexpr const char* kPathGetTransform = "/Script/Engine.Actor:GetTransform";
constexpr const char* kPathDestroyActor = "/Script/Engine.Actor:K2_DestroyActor";

struct DummyTemplate {
    const char* label;
    const char* class_path;
    int nominal_hp;
};

constexpr DummyTemplate kTemplates[] = {
    {"WoodTest", "/Game/Blueprints/Character/Monster/BP_WoodTest.BP_WoodTest_C", 0},
    {"WoodTest_50", "/Game/Blueprints/Character/Monster/BP_WoodTest_50.BP_WoodTest_50_C", 0},
    {"WoodTest_60", "/Game/Blueprints/Character/Monster/BP_WoodTest_60.BP_WoodTest_60_C", 0},
    {"WoodTest_70", "/Game/Blueprints/Character/Monster/BP_WoodTest_70.BP_WoodTest_70_C", 0},
    {"WoodTest_80", "/Game/Blueprints/Character/Monster/BP_WoodTest_80.BP_WoodTest_80_C", 0},
    {"LimitedHP_600",
     "/Game/Blueprints/Character/Monster/BP_WoodTest_LimitedHP_600.BP_WoodTest_LimitedHP_600_C", 600},
    {"LimitedHP_1200",
     "/Game/Blueprints/Character/Monster/BP_WoodTest_LimitedHP_1200.BP_WoodTest_LimitedHP_1200_C", 1200},
};
constexpr std::size_t kTemplateCount = sizeof(kTemplates) / sizeof(kTemplates[0]);

constexpr std::array<int, 8> kRadialSlotAction = {103, 104, 105, 106, -1, 100, 101, 102};

// ---------------------------------------------------------------- 类型

enum class OpKind : std::uint32_t { Spawn = 1, Destroy = 2, DestroyAll = 3 };

struct PendingOp {
    OpKind kind{};
    std::size_t template_index{};
    std::uint64_t dummy_id{};
};

struct DummyRecord {
    std::uint64_t id{};
    std::size_t template_index{};
    AnomalyGenerationHandleV1 handle{};
    bool alive{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    anomaly::plugins::Localizer localizer;
    const AnomalyUiServiceV1* ui{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyNteSessionServiceV1* session;
    const AnomalyNtePlayerServiceV1* player;
    const AnomalyNteActorsServiceV1* actors;

    bool has_invoke{};
    bool subscribed{};
    AnomalyGenerationHandleV1 events_handle;

    std::mutex mutex;
    std::array<DummyRecord, kMaxDummies> dummies{};
    std::size_t dummy_count{};
    std::uint64_t next_dummy_id{1};

    std::array<PendingOp, kMaxPending> pending{};
    std::size_t pending_count{};
    std::uint32_t dropped_ops{};
    std::uint32_t drain_count{};
    std::uint32_t callback_count{};

    bool session_world_ready{};
    std::uint64_t session_event_cursor{};

    std::size_t selected_template{};

    std::array<char, kTextCapacity> last_result{};
    bool last_result_failed{};
    std::uint32_t spawn_ok{};
    std::uint32_t spawn_fail{};
    std::uint32_t destroy_ok{};
    std::uint32_t world_dummy_count{};

    std::uint32_t frame_counter{};
} g_context;

std::atomic<std::uint32_t> g_pending_hint{0};

// ---------------------------------------------------------------- 安全读

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

void LogLine(std::uint32_t level, const char* text) {
    if (g_context.core == nullptr || g_context.core->log == nullptr) return;
    g_context.core->log(g_context.core->user, level, anomaly::sdk::StringView(text));
}

void LogFormat(std::uint32_t level, const char* format, ...) {
    char buffer[kTextCapacity];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    LogLine(level, buffer);
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

bool InvokeRaw(AnomalyGenerationHandleV1 object, AnomalyGenerationHandleV1 function, void* parms,
               std::size_t size) {
    if (!g_context.has_invoke || g_context.events == nullptr) return false;
    const auto status = g_context.events->invoke(g_context.events->user, object, function, parms, size);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        LogFormat(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "[TrainingDummy] invoke failed code=%u size=%zu",
                  status.code, size);
        return false;
    }
    return true;
}

bool InvokeByPath(AnomalyGenerationHandleV1 object, const char* path, void* parms,
                  std::size_t size) {
    AnomalyGenerationHandleV1 function{};
    if (!FindHandle(path, &function)) {
        LogFormat(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "[TrainingDummy] UFunction missing: %s", path);
        return false;
    }
    return InvokeRaw(object, function, parms, size);
}

// ---------------------------------------------------------------- 对象工具

bool SnapshotByHandle(AnomalyGenerationHandleV1 handle, AnomalyUe5ObjectSnapshotV1* out) {
    if (g_context.objects == nullptr || g_context.objects->snapshot_by_handle == nullptr) return false;
    out->struct_size = sizeof(*out);
    return g_context.objects->snapshot_by_handle(g_context.objects->user, handle, out).code ==
           ANOMALY_STATUS_V1_OK;
}


// 裸指针 → handle：读 InternalIndex，再由 snapshot_at 取回带正确 serial 的 handle。
// 全插件唯一一处裸读，必过 core::read_memory。
bool HandleFromPointer(std::uintptr_t pointer, AnomalyGenerationHandleV1* out) {
    if (pointer == 0 || g_context.objects == nullptr || g_context.objects->snapshot_at == nullptr)
        return false;
    std::uint32_t internal_index{};
    if (!ReadValue(pointer + static_cast<std::uintptr_t>(L_object_internalIndex), &internal_index))
        return false;
    AnomalyUe5ObjectSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (g_context.objects->snapshot_at(g_context.objects->user, internal_index, &snapshot).code !=
        ANOMALY_STATUS_V1_OK)
        return false;
    *out = snapshot.handle;
    return out->id != 0;
}

// ---------------------------------------------------------------- session 就绪门

void UpdateSessionState() {
    if (g_context.session == nullptr) return;

    if (g_context.session->snapshot != nullptr) {
        AnomalyNteSessionSnapshotV1 session_snapshot{};
        session_snapshot.struct_size = sizeof(session_snapshot);
        if (g_context.session->snapshot(g_context.session->user, &session_snapshot).code ==
            ANOMALY_STATUS_V1_OK) {
            const bool ready = session_snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY;
            if (ready && !g_context.session_world_ready) {
                g_context.session_world_ready = true;
                LogLine(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "[TrainingDummy] world ready");
            } else if (!ready && g_context.session_world_ready) {
                g_context.session_world_ready = false;
                std::scoped_lock lock(g_context.mutex);
                g_context.dummy_count = 0;
                g_context.world_dummy_count = 0;
                LogLine(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "[TrainingDummy] world unavailable");
            }
        }
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
                std::scoped_lock lock(g_context.mutex);
                g_context.dummy_count = 0;
                g_context.world_dummy_count = 0;
            }
        }
    }
}

// ---------------------------------------------------------------- 软类引用（NpcSummon 模式）

bool BuildSoftClassRef(AnomalyGenerationHandleV1 lib_cdo, const char* utf8_path, std::uint8_t* out40) {
    // UTF-8 -> UTF-16（路径都是 ASCII）
    static wchar_t path_buffer[600];
    std::size_t length = 0;
    while (utf8_path[length] != '\0' && length < 599) {
        path_buffer[length] = static_cast<wchar_t>(static_cast<unsigned char>(utf8_path[length]));
        ++length;
    }
    path_buffer[length] = L'\0';
    const std::int32_t count = static_cast<std::int32_t>(length) + 1;

    // MakeSoftClassPath(FString) -> FSoftClassPath
    std::uint8_t make[kParmMakeSoftClassPath]{};
    std::uintptr_t data = reinterpret_cast<std::uintptr_t>(path_buffer);
    std::memcpy(make + kMakeSoftInString + 0, &data, 8);
    std::memcpy(make + kMakeSoftInString + 8, &count, 4);
    std::memcpy(make + kMakeSoftInString + 12, &count, 4);
    if (!InvokeByPath(lib_cdo, kPathMakeSoftClassPath, make, sizeof(make))) return false;

    // Conv_SoftClassPathToSoftClassRef(FSoftClassPath) -> TSoftClassPtr
    std::uint8_t conv[kParmConvSoftClassRef]{};
    std::memcpy(conv + kConvSoftIn, make + kMakeSoftReturn, kFSoftClassPathSize);
    if (!InvokeByPath(lib_cdo, kPathConvSoftClassRef, conv, sizeof(conv))) return false;

    std::memcpy(out40, conv + kConvSoftReturn, kTSoftClassPtrSize);
    return true;
}

// ---------------------------------------------------------------- 定位本地玩家

// invoke 的 object handle 必须来自 ue5.objects 注册表（宿主 ResolveObjectHandleLocked 校验），
// 而 nte.player 的 handle 属于另一套，传进去只会得到 NOT_FOUND(3)。
// 这里从 actors 快照里挑一个能通过 snapshot_by_handle 校验的玩家角色/控制器。
bool ProbeHandle(AnomalyGenerationHandleV1 handle) {
    if (g_context.objects == nullptr || g_context.objects->snapshot_by_handle == nullptr) return false;
    AnomalyUe5ObjectSnapshotV1 probe{};
    probe.struct_size = sizeof(probe);
    return g_context.objects->snapshot_by_handle(g_context.objects->user, handle, &probe).code ==
           ANOMALY_STATUS_V1_OK;
}

// require_pawn=true 时只接受 Pawn：SummonAI 是 HTAbilityCharacter 的方法，
// 用 Controller 当 receiver 会让引擎按错误布局取字段（访问违例）。
bool FindLocalPlayerHandle(AnomalyGenerationHandleV1* out, char* class_out,
                           std::size_t class_capacity, bool require_pawn) {
    out->id = 0;
    out->generation = 0;
    if (class_out != nullptr && class_capacity > 0) class_out[0] = '\0';

    if (g_context.player == nullptr) {
        LogLine(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                "[TrainingDummy] local player: nte.player service unavailable");
        return false;
    }
    // get_object_handles 是 V1 表尾扩展，必须校验 struct_size
    constexpr std::size_t kRequired =
        offsetof(AnomalyNtePlayerServiceV1, get_object_handles) +
        sizeof(AnomalyNtePlayerServiceV1::get_object_handles);
    if (g_context.player->struct_size < kRequired ||
        g_context.player->get_object_handles == nullptr) {
        LogLine(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                "[TrainingDummy] local player: get_object_handles not published by host");
        return false;
    }

    AnomalyGenerationHandleV1 pawn{}, controller{};
    const auto status =
        g_context.player->get_object_handles(g_context.player->user, &pawn, &controller);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        LogFormat(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                  "[TrainingDummy] local player: get_object_handles code=%u", status.code);
        return false;
    }
    LogFormat(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
              "[TrainingDummy] local player: pawn=%llu/%llu controller=%llu/%llu require_pawn=%d",
              static_cast<unsigned long long>(pawn.id),
              static_cast<unsigned long long>(pawn.generation),
              static_cast<unsigned long long>(controller.id),
              static_cast<unsigned long long>(controller.generation), require_pawn ? 1 : 0);

    if (pawn.id != 0) {
        *out = pawn;
        if (class_out != nullptr && class_capacity > 0)
            std::snprintf(class_out, class_capacity, "%s", "pawn");
        return true;
    }
    if (require_pawn) {
        LogLine(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                "[TrainingDummy] local player: pawn handle empty, controller not usable for SummonAI");
        return false;
    }
    if (controller.id != 0) {
        *out = controller;
        if (class_out != nullptr && class_capacity > 0)
            std::snprintf(class_out, class_capacity, "%s", "controller");
        return true;
    }
    LogLine(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "[TrainingDummy] local player: both handles empty");
    return false;
}


// ---------------------------------------------------------------- 业务

struct OpResult {
    bool ok{};
    AnomalyGenerationHandleV1 actor{};
    char detail[kTextCapacity]{};
};

bool ReadPawnTransform(AnomalyGenerationHandleV1 pawn, std::uint8_t* transform96) {
    std::uint8_t parms[kParmGetTransform]{};
    if (!InvokeByPath(pawn, kPathGetTransform, parms, sizeof(parms))) return false;
    std::memcpy(transform96, parms, 96);
    return true;
}

// 主路径：HTAbilityCharacter::SummonAI，与竞品一致，不依赖战斗 Leader
bool SpawnViaSummonAI(std::size_t template_index, OpResult& result) {
    AnomalyGenerationHandleV1 local{};
    char local_class[128]{};
    if (!FindLocalPlayerHandle(&local, local_class, sizeof(local_class), true)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: no local pawn handle");
        return false;
    }

    AnomalyGenerationHandleV1 lib_cdo{};
    if (!FindHandle(kGameplayStaticsCdo, &lib_cdo)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: GameplayStatics CDO missing");
        return false;
    }

    std::uint8_t soft_class[kTSoftClassPtrSize]{};
    if (!BuildSoftClassRef(lib_cdo, kTemplates[template_index].class_path, soft_class)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: soft class path failed");
        return false;
    }

    std::uint8_t transform[96]{};
    if (!ReadPawnTransform(local, transform)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: GetTransform failed on %s",
                      local_class);
        return false;
    }

    std::uint8_t parms[kParmSummonAI]{};
    std::memcpy(parms + kSummonAIClass, soft_class, kTSoftClassPtrSize);
    const std::uint8_t b_pet = 0;
    std::memcpy(parms + kSummonBPet, &b_pet, 1);
    const double offset_location[3] = {0.0, 0.0, 0.0};
    std::memcpy(parms + kSummonOffsetLocation, offset_location, sizeof(offset_location));
    const std::uint8_t attack_mode = 1;   // ESummonedAIAttackMode::Defensive
    std::memcpy(parms + kSummonAttackMode, &attack_mode, 1);
    const std::int32_t ai_level = 1;
    std::memcpy(parms + kSummonAILevel, &ai_level, 4);
    const std::int32_t skill_level = 1;
    std::memcpy(parms + kSummonSkillLevel, &skill_level, 4);
    std::memcpy(parms + kSummonTransform, transform, 96);
    const std::uint8_t same_camp = 0;   // 异阵营：木桩才会被玩家伤害结算
    std::memcpy(parms + kSummonSameCamp, &same_camp, 1);

    if (!InvokeByPath(local, "/Script/HTGame.HTAbilityCharacter:SummonAI", parms, sizeof(parms))) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: SummonAI rejected");
        return false;
    }

    std::uintptr_t ai_pointer{};
    std::memcpy(&ai_pointer, parms + kSummonReturn, sizeof(ai_pointer));
    if (ai_pointer == 0) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: returned null");
        return false;
    }

    AnomalyGenerationHandleV1 ai_handle{};
    if (!HandleFromPointer(ai_pointer, &ai_handle)) {
        std::snprintf(result.detail, sizeof(result.detail), "summon: AI handle unresolved");
        return false;
    }

    // 生成的 AI 必须登记进召唤列表，否则 AI 系统拿不到归属：
    // 敌对化之后会在下一次 AI tick 触发访问违例。
    // AddSummonedAI 的参数是裸对象指针（UE 反射参数不是 handle）。
    {
        std::uint8_t add_parms[8]{};
        std::memcpy(add_parms, &ai_pointer, sizeof(ai_pointer));
        if (InvokeByPath(local, "/Script/HTGame.HTAbilityCharacter:AddSummonedAI", add_parms,
                         sizeof(add_parms))) {
            LogFormat(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                      "[TrainingDummy] registered summoned AI 0x%llX",
                      static_cast<unsigned long long>(ai_pointer));
        } else {
            LogFormat(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                      "[TrainingDummy] AddSummonedAI unavailable for 0x%llX",
                      static_cast<unsigned long long>(ai_pointer));
        }
    }

    result.ok = true;
    result.actor = ai_handle;
    std::snprintf(result.detail, sizeof(result.detail), "summoned %s",
                  kTemplates[template_index].label);
    return true;
}

OpResult SpawnDummy(std::size_t template_index) {
    OpResult result;
    if (template_index >= kTemplateCount) {
        std::snprintf(result.detail, sizeof(result.detail), "template index out of range");
        return result;
    }
    if (!g_context.session_world_ready) {
        std::snprintf(result.detail, sizeof(result.detail), "world not ready");
        return result;
    }
    if (!g_context.has_invoke) {
        std::snprintf(result.detail, sizeof(result.detail), "invoke unavailable (host too old)");
        return result;
    }

    if (!SpawnViaSummonAI(template_index, result) && result.detail[0] == '\0')
        std::snprintf(result.detail, sizeof(result.detail), "summon failed");
    return result;
}

bool DestroyDummy(const DummyRecord& record) {
    if (record.handle.id == 0) return false;
    // 只校验 handle 还能被 ue5.objects 解析；刚生成的 Actor 名字尚未可解析，
    // 用名字判定存活会把正常目标误判为已销毁。
    if (!ProbeHandle(record.handle)) return false;
    return InvokeByPath(record.handle, kPathDestroyActor, nullptr, 0);
}

// ---------------------------------------------------------------- 服务补查

// Start() 在 loadPhase=game-ready 时执行，但那时 dom=unknown，nte.player / nte.actors
// 可能尚未发布（实测首次加载时为 0）。这里按需重查，避免一次失败就永久失效。
void EnsureServices() {
    if (g_context.host == nullptr) return;
    if (g_context.player != nullptr && g_context.actors != nullptr &&
        g_context.session != nullptr && g_context.objects != nullptr)
        return;

    const anomaly::sdk::Host query(g_context.host);
    if (g_context.player == nullptr) {
        g_context.player = query.Query<AnomalyNtePlayerServiceV1>(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    }
    if (g_context.actors == nullptr) {
        g_context.actors = query.Query<AnomalyNteActorsServiceV1>(
            ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION).get();
    }
    if (g_context.session == nullptr) {
        g_context.session = query.Query<AnomalyNteSessionServiceV1>(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    }
    if (g_context.objects == nullptr) {
        g_context.objects = query.Query<AnomalyUe5ObjectsServiceV1>(
            ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).get();
    }
}

// ---------------------------------------------------------------- 队列

bool PushOp(const PendingOp& op) {
    std::scoped_lock lock(g_context.mutex);
    if (g_context.pending_count >= kMaxPending) {
        ++g_context.dropped_ops;
        return false;
    }
    g_context.pending[g_context.pending_count++] = op;
    g_pending_hint.store(static_cast<std::uint32_t>(g_context.pending_count),
                         std::memory_order_release);
    return true;
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

    {
        std::scoped_lock lock(g_context.mutex);
        ++g_context.drain_count;
    }
    switch (op.kind) {
    case OpKind::Spawn: {
        const OpResult result = SpawnDummy(op.template_index);
        {
            std::scoped_lock lock(g_context.mutex);
            if (result.ok) {
                if (g_context.dummy_count < kMaxDummies) {
                    auto& record = g_context.dummies[g_context.dummy_count++];
                    record = {};
                    record.id = g_context.next_dummy_id++;
                    record.template_index = op.template_index;
                    record.handle = result.actor;
                    record.alive = true;
                                }
                ++g_context.spawn_ok;
            } else {
                ++g_context.spawn_fail;
            }
            std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "%s",
                          result.detail);
            g_context.last_result_failed = !result.ok;
        }
        LogLine(result.ok ? ANOMALY_CORE_LOG_LEVEL_V1_WARNING : ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                result.detail);
        break;
    }
    case OpKind::Destroy: {
        DummyRecord target{};
        bool found{};
        {
            std::scoped_lock lock(g_context.mutex);
            for (std::size_t i = 0; i < g_context.dummy_count; ++i) {
                if (g_context.dummies[i].id != op.dummy_id) continue;
                target = g_context.dummies[i];
                found = true;
                break;
            }
        }
        const bool ok = found && DestroyDummy(target);
        {
            std::scoped_lock lock(g_context.mutex);
            if (ok) {
                for (std::size_t i = 0; i < g_context.dummy_count; ++i) {
                    if (g_context.dummies[i].id != op.dummy_id) continue;
                    g_context.dummies[i].alive = false;
                    // 已销毁的条目从列表里摘掉，避免留下死条目
                    for (std::size_t k = i + 1; k < g_context.dummy_count; ++k)
                        g_context.dummies[k - 1] = g_context.dummies[k];
                    --g_context.dummy_count;
                    break;
                }
                ++g_context.destroy_ok;
                g_context.last_result_failed = false;
                std::snprintf(g_context.last_result.data(), g_context.last_result.size(),
                              "destroyed #%llu", static_cast<unsigned long long>(op.dummy_id));
            } else {
                g_context.last_result_failed = true;
                std::snprintf(g_context.last_result.data(), g_context.last_result.size(),
                              found ? "destroy failed #%llu" : "not found #%llu",
                              static_cast<unsigned long long>(op.dummy_id));
            }
        }
        LogFormat(ok ? ANOMALY_CORE_LOG_LEVEL_V1_WARNING : ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                  "[TrainingDummy] destroy #%llu found=%d ok=%d",
                  static_cast<unsigned long long>(op.dummy_id), found, ok);
        break;
    }
    case OpKind::DestroyAll: {
        std::array<DummyRecord, kMaxDummies> snapshot{};
        std::size_t count{};
        {
            std::scoped_lock lock(g_context.mutex);
            count = g_context.dummy_count;
            for (std::size_t i = 0; i < count; ++i) snapshot[i] = g_context.dummies[i];
        }
        std::size_t destroyed{};
        std::size_t failed{};
        for (std::size_t i = 0; i < count; ++i) {
            if (!snapshot[i].alive) continue;
            if (DestroyDummy(snapshot[i])) {
                ++destroyed;
            } else {
                ++failed;
            }
        }
        {
            std::scoped_lock lock(g_context.mutex);
            if (failed == 0) {
                g_context.dummy_count = 0;
            } else {
                // 只保留销毁失败的条目，便于再点一次
                std::size_t write = 0;
                for (std::size_t i = 0; i < g_context.dummy_count; ++i) {
                    if (!g_context.dummies[i].alive) continue;
                    g_context.dummies[write++] = g_context.dummies[i];
                }
                g_context.dummy_count = write;
            }
            g_context.destroy_ok += static_cast<std::uint32_t>(destroyed);
            g_context.last_result_failed = failed != 0;
            if (failed == 0) {
                std::snprintf(g_context.last_result.data(), g_context.last_result.size(),
                              "cleared %zu", destroyed);
            } else {
                std::snprintf(g_context.last_result.data(), g_context.last_result.size(),
                              "cleared %zu, %zu failed", destroyed, failed);
            }
        }
        LogFormat(failed == 0 ? ANOMALY_CORE_LOG_LEVEL_V1_WARNING : ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                  "[TrainingDummy] clear all: destroyed=%zu failed=%zu", destroyed, failed);
        break;
    }
    }
}

// ---------------------------------------------------------------- 回调：一次 relaxed 读

void ANOMALY_CALL OnGameEvent(void* /*user*/, std::uintptr_t /*object*/, std::uintptr_t /*function*/,
                              void* /*parameters*/) {
    g_context.callback_count = g_context.callback_count + 1;
    if (g_pending_hint.load(std::memory_order_relaxed) == 0) return;
    DrainOne();
}

// ---------------------------------------------------------------- 限流刷新

void RefreshWorldDummies() {
    if (!g_context.session_world_ready) return;
    if (g_context.actors == nullptr || g_context.actors->frame == nullptr ||
        g_context.actors->page == nullptr || g_context.actors->class_name_utf8 == nullptr)
        return;

    static std::array<AnomalyNteEntitySnapshotV1, kPageCapacity> page;
    AnomalyNteEntityFrameV1 frame{};
    frame.struct_size = sizeof(frame);
    if (g_context.actors->frame(g_context.actors->user, &frame).code != ANOMALY_STATUS_V1_OK) return;

    std::uint32_t alive{};
    std::uint32_t offset{};
    for (int guard = 0; guard < 8; ++guard) {
        for (auto& entry : page) {
            entry = {};
            entry.struct_size = sizeof(AnomalyNteEntitySnapshotV1);
        }
        AnomalyNteEntityPageRequestV1 request{};
        request.struct_size = sizeof(request);
        request.generation = frame.generation;
        request.offset = offset;
        request.capacity = static_cast<std::uint32_t>(kPageCapacity);
        AnomalyNteEntityPageResultV1 result{};
        result.struct_size = sizeof(result);
        if (g_context.actors->page(g_context.actors->user, &request, page.data(), &result).code !=
            ANOMALY_STATUS_V1_OK)
            break;
        for (std::uint32_t i = 0; i < result.returned && i < kPageCapacity; ++i) {
            char cls[128]{};
            std::size_t size = sizeof(cls);
            if (g_context.actors->class_name_utf8(g_context.actors->user, page[i].class_id, cls,
                                                  &size).code != ANOMALY_STATUS_V1_OK)
                continue;
            if (std::strstr(cls, "WoodTest") != nullptr) ++alive;
        }
        if (result.returned == 0 || result.next_offset >= result.total_matches) break;
        offset = result.next_offset;
    }

    std::scoped_lock lock(g_context.mutex);
    g_context.world_dummy_count = alive;
}


}  // namespace

// ---------------------------------------------------------------- ABI

namespace {

void ResetState() {
    std::scoped_lock lock(g_context.mutex);
    g_context.dummies = {};
    g_context.dummy_count = 0;
    g_context.next_dummy_id = 1;
    g_context.pending = {};
    g_context.pending_count = 0;
    g_context.dropped_ops = 0;
    g_context.drain_count = 0;
    g_context.callback_count = 0;
    g_context.session_world_ready = false;
    g_context.session_event_cursor = 0;
    g_context.selected_template = 0;
    g_context.spawn_ok = g_context.spawn_fail = g_context.destroy_ok = 0;
    g_context.world_dummy_count = 0;
    g_context.frame_counter = 0;
    g_context.last_result = {};
    g_context.last_result_failed = false;
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
    g_context.localizer = anomaly::plugins::Localizer(host);
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
    g_context.actors = query.Query<AnomalyNteActorsServiceV1>(
        ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION).get();
    g_context.session = query.Query<AnomalyNteSessionServiceV1>(
        ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    g_context.player = query.Query<AnomalyNtePlayerServiceV1>(
        ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();

    const auto events = query.Query<AnomalyUe5ProcessEventServiceV1>(
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID, ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION);
    g_context.events = events.get();
    g_context.has_invoke = HasInvoke(g_context.events);

    if (g_context.events != nullptr && g_context.events->subscribe != nullptr) {
        AnomalyGenerationHandleV1 handle{};
        if (g_context.events->subscribe(g_context.events->user, OnGameEvent, &g_context, &handle)
                .code == ANOMALY_STATUS_V1_OK) {
            g_context.events_handle = handle;
            g_context.subscribed = true;
        }
    }

    LogFormat(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
              "[TrainingDummy] start core=%d objects=%d session=%d player=%d actors=%d "
              "events=%d invoke=%d sub=%d",
              g_context.core != nullptr, g_context.objects != nullptr,
              g_context.session != nullptr, g_context.player != nullptr,
              g_context.actors != nullptr, g_context.events != nullptr, g_context.has_invoke,
              g_context.subscribed);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (g_context.events != nullptr && g_context.events->unsubscribe != nullptr &&
        g_context.subscribed) {
        (void)g_context.events->unsubscribe(g_context.events->user, g_context.events_handle);
    }
    g_context.events = nullptr;
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
    g_context.actors = nullptr;
    g_context.session = nullptr;
    g_context.player = nullptr;
    g_context.has_invoke = false;
}

void ANOMALY_CALL Update(void* /*context*/, double /*delta_seconds*/) {
    ++g_context.frame_counter;
    EnsureServices();
    UpdateSessionState();

    if ((g_context.frame_counter % kActorRefreshInterval) == 0) RefreshWorldDummies();
}

bool ActionButton(const AnomalyUiServiceV1* ui, const char* label, bool enabled) {
    if (ui == nullptr) return false;
    if (ui->button_enabled != nullptr)
        return ui->button_enabled(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F,
                                  enabled ? 1 : 0) != 0;
    return enabled && ui->button != nullptr &&
           ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0;
}

void ANOMALY_CALL Draw(void* /*context*/, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr)
        return;

    DummyRecord records[kMaxDummies];
    std::size_t count{}, selected{};
    std::uint32_t pending{};
    bool world_ready{}, invoke_ready{}, prefer_summon{}, failed{};
    std::array<char, kTextCapacity> last_result{};
    bool last_result_failed{};
    {
        std::scoped_lock lock(g_context.mutex);
        count = g_context.dummy_count < kMaxDummies ? g_context.dummy_count : kMaxDummies;
        for (std::size_t i = 0; i < count; ++i) records[i] = g_context.dummies[i];
        selected = g_context.selected_template;
        pending = static_cast<std::uint32_t>(g_context.pending_count);
        world_ready = g_context.session_world_ready;
        invoke_ready = g_context.has_invoke;
        last_result = g_context.last_result;
        failed = g_context.last_result_failed;
    }

    const auto& l = g_context.localizer;
    int open = 1;
    const std::string title = l.Text("window.title", "Training Dummy");
    anomaly::sdk::UiWindow window(ui, title.c_str(), &open);
    if (!window) return;

    // ---- 模板 ----
    const std::string template_title = l.Text("template.title", "Template");
    ui->text(ui->user, anomaly::sdk::StringView(template_title));
    for (std::size_t i = 0; i < kTemplateCount; ++i) {
        if (ui->checkbox == nullptr) continue;
        int value = (i == selected) ? 1 : 0;
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(kTemplates[i].label), &value) != 0) {
            std::scoped_lock lock(g_context.mutex);
            g_context.selected_template = i;
        }
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    // ---- 操作 ----
    const std::string spawn_label = l.Text("action.spawn", "Spawn");
    if (ActionButton(ui, spawn_label.c_str(), world_ready && invoke_ready)) {
        if (!PushOp(PendingOp{OpKind::Spawn, selected, 0})) {
            std::scoped_lock lock(g_context.mutex);
            g_context.last_result_failed = true;
            std::snprintf(g_context.last_result.data(), g_context.last_result.size(), "queue full");
        }
    }
    if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
    const std::string clear_label = l.Text("action.clear", "Clear all");
    if (ActionButton(ui, clear_label.c_str(), count > 0)) {
        (void)PushOp(PendingOp{OpKind::DestroyAll, 0, 0});
    }

    if (ui->separator != nullptr) ui->separator(ui->user);

    // ---- 已生成列表 ----
    const std::string list_title = l.Text("list.title", "Spawned");
    ui->text(ui->user, anomaly::sdk::StringView(list_title));
    if (count == 0) {
        const std::string empty = l.Text("list.empty", "(none)");
        ui->text(ui->user, anomaly::sdk::StringView(empty));
    }

    const std::string destroy_label = l.Text("action.destroy", "X");
    char line[kTextCapacity];
    for (std::size_t i = 0; i < count; ++i) {
        std::snprintf(line, sizeof(line), "#%llu %s",
                      static_cast<unsigned long long>(records[i].id),
                      kTemplates[records[i].template_index].label);
        ui->text(ui->user, anomaly::sdk::StringView(line));
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        std::snprintf(line, sizeof(line), "%s##d%llu", destroy_label.c_str(),
                      static_cast<unsigned long long>(records[i].id));
        if (ActionButton(ui, line, records[i].alive))
            (void)PushOp(PendingOp{OpKind::Destroy, 0, records[i].id});
    }

    // ---- 底部状态（唯一保留的关键信息） ----
    if (ui->separator != nullptr) ui->separator(ui->user);

    const std::string world_state =
        l.Text(world_ready ? "status.ready" : "status.not_ready",
               world_ready ? "World ready" : "World not ready");
    const std::string count_arg = std::to_string(count);
    const std::array<std::string_view, 1> args{count_arg};
    const std::string summary = l.Format("status.summary", "{0} spawned", args);
    std::snprintf(line, sizeof(line), "%s  %s", world_state.c_str(), summary.c_str());
    ui->text(ui->user, anomaly::sdk::StringView(line));

    if (pending != 0) {
        const std::string pending_arg = std::to_string(pending);
        const std::array<std::string_view, 1> pending_args{pending_arg};
        const std::string pending_text = l.Format("status.pending", "{0} queued", pending_args);
        ui->text(ui->user, anomaly::sdk::StringView(pending_text));
    }
    if (failed) {
        const std::array<std::string_view, 1> failure_args{last_result.data()};
        const std::string failure = l.Format("status.failed", "last: {0}", failure_args);
        ui->text(ui->user, anomaly::sdk::StringView(failure));
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   anomaly::sdk::StringView("local.nte.training-dummy"),
                   anomaly::sdk::StringView("Training Dummy"),
                   anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
                   Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
