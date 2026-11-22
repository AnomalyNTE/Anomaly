#include "game.hpp"

#include <windows.h>

#include "i18n.hpp"
#include "profile_layout.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace daily {

// 兼容数据：Load 阶段填好，之后只读。
ProfileSnapshot g_profile;

namespace {

// ---------------------------------------------------------------------------
// 游戏数据：类名、函数名、控件名、RPC 路径（不是界面文案，不进词条表）
// ---------------------------------------------------------------------------

constexpr const char* kPathStoreMgmtRpc = "/Script/HTGame.HTPlayerState.ServerStoreManagementRPC";
constexpr const char* kAwardSureName = "Button_Sure";
constexpr const char* kAwardCloseName = "ButtonDeactivate";
// 领奖弹窗最外层是 UI_CommonAwardsPopUpWindows_C_<实例号>/WB_CommonDeactivate：
// 实例号每次都不一样，只按控件名匹配（名字或 path 最后一段）。
constexpr const char* kAwardExitName = "WB_CommonDeactivate";
// 进游戏后的登录/进入按钮。
constexpr const char* kLoginButtonName = "UI_Button_Login";
// 「已经在游戏里」的判据：这些控件只在游戏内 HUD/面板上出现（登录页没有）。
// 登录页的判定不可靠（进世界后 UI_Button_Login 还会留在目录里，甚至仍报可点击），
// 所以反过来用「游戏内才有的控件」来判断。
constexpr const char* kHudMarkerName = "W_HTButton_AdventureManual";
constexpr const char* kIngameMarkers[] = {
    "W_HTButton_AdventureManual",
    "W_BoundActionButton_Close_C",
};
// 弹窗有打开动画，每一步最多重扫这么多次再放弃。
constexpr int kFonsDialogTries = 20;

struct ClaimTarget {
    const char* base;
    const char* function;
    const char* fallback;
};

constexpr ClaimTarget kClaimTargets[] = {
    {"HTUI_AdventureManualDailyRow", "SubmitDailyQuest", "SubmitQuest"},
    {"HTUI_AdventureManualDailyActivationItem", "OnReceiveRewardsButtonClicked", nullptr},
    {"HTUI_AdventureManualWizard", "OnReceiveChapterRewardClicked", nullptr},
    {"HTUI_AdventureManualCultivationItem", "OnAcceptButtonClicked", nullptr},
    {"HTUI_AdventureManualTeachingAwardRow", "OnTeachingAwardButtonClicked", nullptr},
};
constexpr int kClaimTargetCount = static_cast<int>(sizeof(kClaimTargets) / sizeof(kClaimTargets[0]));

struct RewardStep {
    // 0=按名字点第一个 1=点全部同名 2=按 class 子串点 3=只等页面就绪（不点击）
    // 5=按名字前缀点一次，有没有都立刻过（清残留窗口用）
    // 6=按名字前缀点第一个（控件名带实例号后缀，如 W_BoundActionButton_Close_C_2147458308）
    // 7=验证式关窗：弹窗与面板都从屏幕上消失才算过，关不掉就停下并报原因
    int mode;
    const char* target;
    int wait;
    const char* label;
    // 前置步骤：没做成不能往下走，要退回「全部」页重新切一次。
    bool required;
    // 同一控件名对应多个 tab 时按 caption 区分（游戏文案，UTF-8 字节转义，不是插件词条）。
    const char* caption;
    // 幂等领取步骤：控件在但不可点击 ⇒ 多半是已经领过了，直接跳过，不要在这里耗重试。
    bool idempotent;
};

// caption 是游戏数据：和运行时按钮文本逐字比较，不能改写成别的语言。
// 「全部」
constexpr const char* kCaptionAll = "\xE5\x85\xA8\xE9\x83\xA8";
// 「环期任务」
constexpr const char* kCaptionPeriodic = "\xE7\x8E\xAF\xE6\x9C\x9F\xE4\xBB\xBB\xE5\x8A\xA1";
// 探索指南面板的 tab 行与关闭键：用它们判断「面板还开在屏幕上」，而不是靠点击成功与否。
constexpr const char* kPanelTabName = "BPUI_RadioBox_Table_1";
constexpr const char* kPanelClosePrefix = "W_BoundActionButton_Close";

// 两种领取是两条独立链路，各自一套步骤，配置里也是两个开关。
// 每日活跃奖励：探索指南 -> 「全部」(每日活跃页) -> 活跃度奖励 -> 最终领取 -> 关窗（验证关掉）
constexpr RewardStep kDailyRewardSteps[] = {
    {0, "W_HTButton_AdventureManual", 120, "open-panel", true, nullptr, false},
    {0, "BPUI_RadioBox_Table_1", 120, "daily-tab-all", true, kCaptionAll, false},
    {3, "", 60, "daily-page-ready", true, nullptr, false},
    {1, "ReceiveButton", 30, "activity-reward", false, nullptr, true},
    {1, "ButtonAccept", 30, "final-claim", false, nullptr, true},
    // 关窗是一步「验证式收尾」：弹窗和面板都从屏幕上消失才算完，关不掉就停在这里，
    // 绝不带着开着的面板进下一条链路（面板会把 HUD 上的入口按钮挡成不可点击）。
    {7, "", 30, "close-reward-ui", true, nullptr, false},
};

// 环期任务奖励：探索指南 -> 战斗奖励入口 -> 「环期任务」页 -> 全部领取 -> 关弹窗 -> 关面板
// 两个 tab 用的是同一个控件名 BPUI_RadioBox_Table_1，只能靠 caption 区分。
// 注意：环期任务入口 W_HTButton_CombatAward 在主界面 HUD 上，**不能先开探索指南面板**——
// 面板一开就把它挡成不可点击，点了也只会看到上次停留的每日活跃页。
constexpr RewardStep kPeriodicRewardSteps[] = {
    // 先把上一条链路可能留下的面板关掉（同样要验证关干净），否则它会挡住 HUD 上的入口按钮。
    {7, "", 15, "clear-leftover-panel", true, nullptr, false},
    {0, "W_HTButton_CombatAward", 90, "combat-award", true, nullptr, false},
    {0, "BPUI_RadioBox_Table_1", 90, "periodic-tab", false, kCaptionPeriodic, false},
    {3, "BtnSubmitAllQuest", 60, "periodic-page-ready", true, nullptr, false},
    // 已经领完时这个按钮是 HIDDEN，点了也没用：跳过就好，后面照样把窗口关掉退出。
    {0, "BtnSubmitAllQuest", 30, "claim-all-quests", false, nullptr, true},
    {7, "", 30, "close-reward-ui", true, nullptr, false},
};

struct RewardTableDef {
    const RewardStep* steps;
    int count;
    int tab_step;       // 前置步骤失败时退回这一步重新切页
    const char* label;  // 日志里的链路名
};

constexpr RewardTableDef kRewardTables[] = {
    {kDailyRewardSteps,
     static_cast<int>(sizeof(kDailyRewardSteps) / sizeof(kDailyRewardSteps[0])), 1, "daily"},
    {kPeriodicRewardSteps,
     static_cast<int>(sizeof(kPeriodicRewardSteps) / sizeof(kPeriodicRewardSteps[0])), 2, "periodic"},
};
constexpr int kRewardTableDaily = 0;
constexpr int kRewardTablePeriodic = 1;
// 前置步骤失败时最多重切这么多次页（一次就够：再不行就是真有问题）。
constexpr int kRewardTabRounds = 2;
// 单个步骤的重试次数：领过的控件会立刻跳过，这里只给「页面正在切」留点余量。
constexpr int kRewardRetries = 5;
// 关窗步骤的耐心要大一点：点完关闭键后窗口有收起动画，目录也要重新扫一次才看得到结果。
constexpr int kRewardCloseRetries = 12;

// ---------------------------------------------------------------------------
// 内存读取
// ---------------------------------------------------------------------------

bool ReadMemory(const std::uintptr_t address, void* destination, const std::size_t size) {
    State& state = S();
    if (address == 0 || destination == nullptr || size == 0) return false;
    if (state.core == nullptr || state.core->read_memory == nullptr) return false;
    return state.core->read_memory(
               state.core->user, address,
               AnomalyMutableByteSpanV1{static_cast<std::uint8_t*>(destination), size})
               .code == ANOMALY_STATUS_V1_OK;
}

template <typename T>
bool ReadValue(const std::uintptr_t address, T& value) {
    value = T{};
    return ReadMemory(address, &value, sizeof(T));
}

bool ReadPointer(const std::uintptr_t address, std::uintptr_t& value) {
    value = 0;
    return ReadValue(address, value) && value != 0;
}

bool ReadDisplacement(std::uintptr_t instruction, std::uintptr_t& target, bool adjust) {
    target = 0;
    std::int32_t displacement = 0;
    if (!ReadValue(instruction + 3, displacement)) return false;
    const auto base = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(instruction) + 7 + displacement);
    const std::uintptr_t subtract = adjust ? S().layout.objects_base_subtract : 0;
    if (base <= subtract) return false;
    target = base - subtract;
    return target != 0;
}

bool ObjectNameOf(const std::uintptr_t object, char* out, const std::size_t capacity) {
    State& state = S();
    if (object == 0 || out == nullptr || capacity < 2) return false;
    if (state.names == nullptr || state.names->resolve_utf8 == nullptr) return false;
    std::uint32_t name_id = 0;
    if (!ReadValue(object + state.layout.object_name, name_id) || name_id == 0) return false;
    std::size_t size = capacity;
    if (state.names->resolve_utf8(state.names->user, name_id, out, &size).code !=
        ANOMALY_STATUS_V1_OK) {
        out[0] = '\0';
        return false;
    }
    out[capacity - 1] = '\0';
    return out[0] != '\0';
}

std::uintptr_t ClassOf(const std::uintptr_t object) {
    std::uintptr_t cls = 0;
    if (object == 0) return 0;
    if (!ReadValue(object + S().layout.object_class, cls) || cls == 0) return 0;
    return cls & ~static_cast<std::uintptr_t>(7);
}

bool IsDefaultObject(const std::uintptr_t object) {
    char name[96]{};
    return ObjectNameOf(object, name, sizeof(name)) && std::strncmp(name, "Default__", 9) == 0;
}

bool ResolveRegistry(std::uintptr_t& out) {
    State& state = S();
    out = 0;
    if (!state.layout_ready) return false;
    std::uintptr_t instruction = 0;
    if (!ResolveProfileSymbol(state.signature, state.core, g_profile.gobjects_pattern.c_str(),
                              g_profile.gobjects_entry.c_str(), instruction)) {
        return false;
    }
    return ReadDisplacement(instruction, out, true);
}

bool ResolveGWorldSlot(std::uintptr_t& out) {
    State& state = S();
    out = 0;
    if (!state.layout_ready) return false;
    std::uintptr_t instruction = 0;
    if (!ResolveProfileSymbol(state.signature, state.core, g_profile.gworld_pattern.c_str(),
                              g_profile.gworld_entry.c_str(), instruction)) {
        return false;
    }
    return ReadDisplacement(instruction, out, false);
}

std::uintptr_t CurrentWorld() {
    State& state = S();
    if (state.gworld_slot == 0) {
        std::uintptr_t slot = 0;
        if (!ResolveGWorldSlot(slot)) return 0;
        state.gworld_slot = slot;
    }
    std::uintptr_t world = 0;
    ReadValue(state.gworld_slot, world);
    return world;
}

std::uintptr_t ResolveHandle(const AnomalyGenerationHandleV1 handle) {
    State& state = S();
    if (handle.id == 0 || state.registry == 0) return 0;
    const std::uint32_t index = static_cast<std::uint32_t>(handle.id) - 1u;
    const std::uint32_t serial = static_cast<std::uint32_t>(handle.id >> 32u);
    std::uint32_t count = 0;
    if (!ReadValue(state.registry + state.layout.objects_count, count) || count == 0 ||
        count > kMaximumObjectCount || index >= count) {
        return 0;
    }
    std::uintptr_t chunks = 0;
    std::uintptr_t chunk = 0;
    if (!ReadPointer(state.registry + state.layout.objects_items, chunks)) return 0;
    if (!ReadPointer(chunks + 8u * (index / state.layout.objects_chunk_size), chunk)) return 0;
    const std::uintptr_t item =
        chunk + state.layout.object_item_stride * (index % state.layout.objects_chunk_size);
    std::uintptr_t object = 0;
    if (!ReadPointer(item, object)) return 0;
    std::uint32_t actual_serial = 0;
    if (!ReadValue(item + state.layout.object_item_stride - 8u, actual_serial) ||
        actual_serial != serial) {
        return 0;
    }
    return object;
}

std::uintptr_t FindObjectByPath(const char* path) {
    State& state = S();
    if (path == nullptr || state.objects == nullptr || state.objects->find_exact == nullptr) return 0;
    AnomalyGenerationHandleV1 handle{};
    if (state.objects->find_exact(state.objects->user, Str(path, std::strlen(path)),
                                 &handle)
            .code != ANOMALY_STATUS_V1_OK) {
        return 0;
    }
    return ResolveHandle(handle);
}

bool ProcessEvent(const std::uintptr_t object, const std::uintptr_t function, void* parameters) {
    if (object == 0 || function == 0) return false;
    std::uintptr_t vtable = 0;
    std::uintptr_t entry = 0;
    if (!ReadPointer(object, vtable)) return false;
    if (!ReadPointer(vtable + 8u * S().layout.process_event_vtable_slot, entry)) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(entry)(
            reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), parameters);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t FindFunctionOnClass(std::uintptr_t cls, const char* name) {
    State& state = S();
    if (cls == 0 || name == nullptr) return 0;
    for (int depth = 0; cls != 0 && depth < 32; ++depth) {
        std::uintptr_t child = 0;
        if (ReadValue(cls + state.layout.ustruct_children, child) && child != 0) {
            for (int i = 0; child != 0 && i < 2048; ++i) {
                const std::uintptr_t child_class = ClassOf(child);
                if (child_class != 0) {
                    char class_name[64]{};
                    if (ObjectNameOf(child_class, class_name, sizeof(class_name)) &&
                        std::strcmp(class_name, "Function") == 0) {
                        char function_name[96]{};
                        if (ObjectNameOf(child, function_name, sizeof(function_name)) &&
                            std::strcmp(function_name, name) == 0) {
                            return child;
                        }
                    }
                }
                std::uintptr_t next = 0;
                if (!ReadValue(child + state.layout.ufield_next, next) || next == child) break;
                child = next;
            }
        }
        std::uintptr_t super = 0;
        if (!ReadValue(cls + state.layout.ustruct_super, super) || super == 0) break;
        super &= ~static_cast<std::uintptr_t>(7);
        if (super == cls) break;
        cls = super;
    }
    return 0;
}

bool InheritsFrom(std::uintptr_t cls, const char* base,
                  std::unordered_map<std::uintptr_t, std::uint8_t>& cache) {
    State& state = S();
    if (cls == 0) return false;
    const auto found = cache.find(cls);
    if (found != cache.end()) return found->second != 0;
    bool hit = false;
    std::uintptr_t cursor = cls;
    for (int depth = 0; cursor != 0 && depth < 16; ++depth) {
        char name[128]{};
        if (ObjectNameOf(cursor, name, sizeof(name)) && std::strcmp(name, base) == 0) {
            hit = true;
            break;
        }
        std::uintptr_t super = 0;
        if (!ReadValue(cursor + state.layout.ustruct_super, super) || super == 0) break;
        super &= ~static_cast<std::uintptr_t>(7);
        if (super == cursor) break;
        cursor = super;
    }
    cache[cls] = hit ? 1 : 0;
    return hit;
}

bool CallNoArg(const std::uintptr_t object, const std::uintptr_t function) {
    if (object == 0 || function == 0) return false;
    std::uint8_t parameters[16]{};
    return ProcessEvent(object, function, parameters);
}

// ---------------------------------------------------------------------------
// ui-buttons 辅助
// ---------------------------------------------------------------------------

const AnomalyNteUiButtonsServiceV1* Buttons() {
    const AnomalyNteUiButtonsServiceV1* service = S().uibuttons;
    if (service == nullptr || service->request_scan == nullptr) return nullptr;
    return service;
}

}  // namespace

// 解析失败时把原因留下：状态栏和面板都会带上它，不必再靠猜。
void SetEngineError(const char* reason) {
    State& state = S();
    std::snprintf(state.engine_error, sizeof(state.engine_error), "%s",
                  reason != nullptr ? reason : "unknown");
}

void ReportEngineNotReady() {
    const State& state = S();
    const char* reason = state.engine_error[0] != '\0'
        ? state.engine_error
        : (state.layout_error[0] != '\0' ? state.layout_error : "profile not loaded");
    const std::string argument = reason;
    SetStatusText(TextLocalized("status.engine_not_ready_reason",
                     "Engine symbols are not ready: {0}. Enter the game and retry.", &argument, 1));
    WinLogf("engine not ready: %s", reason);
}

bool EnsureEngine() {
    State& state = S();
    if (!state.layout_ready) {
        SetEngineError(state.layout_error[0] != '\0' ? state.layout_error
                                                      : "compatibility profile is not loaded");
        return false;
    }
    if (state.registry == 0) {
        std::uintptr_t registry = 0;
        if (ResolveRegistry(registry)) state.registry = registry;
    }
    if (state.gworld_slot == 0) {
        std::uintptr_t slot = 0;
        if (ResolveGWorldSlot(slot)) state.gworld_slot = slot;
    }
    if (state.registry == 0 || state.gworld_slot == 0) {
        SetEngineError(state.registry == 0 ? "ue5.GObjects signature did not resolve"
                                           : "ue5.GWorld signature did not resolve");
        return false;
    }
    state.engine_error[0] = '\0';
    if (!state.engine_ready) {
        state.engine_ready = true;
        WinLogf("engine ready registry=0x%llX gworld=0x%llX",
                static_cast<unsigned long long>(state.registry),
                static_cast<unsigned long long>(state.gworld_slot));
    }
    return true;
}

std::uintptr_t LocalPlayerState() {
    State& state = S();
    const std::uintptr_t world = CurrentWorld();
    if (world == 0) return 0;
    std::uintptr_t game_instance = 0;
    std::uintptr_t local_players = 0;
    std::uintptr_t local_player = 0;
    std::uintptr_t controller = 0;
    std::uintptr_t player_state = 0;
    if (!ReadPointer(world + state.layout.world_game_instance, game_instance)) return 0;
    if (!ReadPointer(game_instance + state.layout.game_instance_local_players, local_players))
        return 0;
    if (!ReadPointer(local_players, local_player)) return 0;
    if (!ReadPointer(local_player + state.layout.local_player_controller, controller)) return 0;
    if (!ReadPointer(controller + state.layout.controller_player_state, player_state)) return 0;
    return player_state;
}

bool ClaimDaily(const int only_target) {
    State& state = S();
    if (!EnsureEngine()) {
        ReportEngineNotReady();
        return false;
    }
    if (state.registry == 0 || state.names == nullptr) {
        WinLog("claim failed: registry or names unavailable");
        return false;
    }
    std::uint32_t count = 0;
    std::uint32_t num_chunks = 0;
    if (!ReadValue(state.registry + state.layout.objects_count, count) || count == 0 ||
        count > kMaximumObjectCount) {
        WinLog("claim failed: object table head is invalid");
        return false;
    }
    if (!ReadValue(state.registry + state.layout.objects_num_chunks, num_chunks) ||
        num_chunks == 0 || num_chunks > kMaximumObjectChunks) {
        WinLog("claim failed: object table chunk count is invalid");
        return false;
    }
    std::uintptr_t chunks = 0;
    if (!ReadPointer(state.registry + state.layout.objects_items, chunks)) {
        WinLog("claim failed: object table items are unavailable");
        return false;
    }

    std::unordered_map<std::uintptr_t, std::uint8_t> caches[kClaimTargetCount];
    std::uintptr_t hits[kClaimTargetCount][kMaximumClaimTargets]{};
    int hit_counts[kClaimTargetCount]{};

    for (std::uint32_t chunk_index = 0; chunk_index < num_chunks; ++chunk_index) {
        std::uintptr_t chunk = 0;
        if (!ReadPointer(chunks + 8u * chunk_index, chunk)) continue;
        for (std::uint32_t i = 0; i < state.layout.objects_chunk_size; ++i) {
            std::uintptr_t object = 0;
            if (!ReadPointer(chunk + state.layout.object_item_stride * i, object)) continue;
            const std::uintptr_t cls = ClassOf(object);
            if (cls == 0) continue;
            for (int t = 0; t < kClaimTargetCount; ++t) {
                if (!InheritsFrom(cls, kClaimTargets[t].base, caches[t])) continue;
                if (IsDefaultObject(object)) break;
                bool duplicate = false;
                for (int k = 0; k < hit_counts[t]; ++k) {
                    if (hits[t][k] == object) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate && hit_counts[t] < static_cast<int>(kMaximumClaimTargets)) {
                    hits[t][hit_counts[t]++] = object;
                }
                break;
            }
        }
    }

    int total_claimed = 0;
    int total_failed = 0;
    for (int t = 0; t < kClaimTargetCount; ++t) {
        if (only_target >= 0 && only_target != t) continue;
        const int found = hit_counts[t];
        if (found == 0) continue;
        int claimed = 0;
        int failed = 0;
        for (int k = 0; k < found; ++k) {
            std::uintptr_t function =
                FindFunctionOnClass(ClassOf(hits[t][k]), kClaimTargets[t].function);
            if (function == 0 && kClaimTargets[t].fallback != nullptr) {
                function = FindFunctionOnClass(ClassOf(hits[t][k]), kClaimTargets[t].fallback);
            }
            if (function != 0 && CallNoArg(hits[t][k], function)) {
                ++claimed;
            } else {
                ++failed;
            }
        }
        total_claimed += claimed;
        total_failed += failed;
        WinLogf("claim %s: found=%d ok=%d failed=%d", kClaimTargets[t].base, found, claimed,
                failed);
    }
    if (total_claimed + total_failed == 0) {
        WinLog("claim: no claimable widget found");
        SetStatus(TextLocalized("status.claim_nothing", "No claimable entry was found; the panel may not be available."));
        return false;
    }
    WinLogf("claim finished: ok=%d failed=%d", total_claimed, total_failed);
    SetStatus(TextLocalized("status.claim_sent", "Claim calls were sent; confirm the rewards in game."));
    return true;
}

bool ExtractFons() {
    State& state = S();
    if (!EnsureEngine()) {
        ReportEngineNotReady();
        return false;
    }
    if (state.store_mgmt_rpc == 0) {
        state.store_mgmt_rpc = FindObjectByPath(kPathStoreMgmtRpc);
        if (state.store_mgmt_rpc == 0) {
            SetStatus(TextLocalized("status.fons_rpc_missing", "ServerStoreManagementRPC was not resolved yet."));
            return false;
        }
    }
    const std::uintptr_t player_state = LocalPlayerState();
    if (player_state == 0) {
        SetStatus(TextLocalized("status.fons_no_player", "PlayerState is unavailable; make sure you are in game."));
        return false;
    }
    // 报文布局与命令字都来自 profile，避免在代码里留第二份数值副本。
    const std::int32_t receive_type = static_cast<std::int32_t>(
        state.layout.store_management_receive_type);
    std::vector<std::uint8_t> message(state.layout.rpc_message_type_offset + sizeof(receive_type), 0);
    std::memcpy(message.data() + state.layout.rpc_message_type_offset, &receive_type,
                sizeof(receive_type));
    if (!ProcessEvent(player_state, state.store_mgmt_rpc, message.data())) {
        SetStatus(TextLocalized("status.fons_rpc_failed", "The receive RPC call failed."));
        return false;
    }
    WinLogf("fons: receive RPC sent (type=%d)", static_cast<int>(receive_type));
    SetStatus(TextLocalized("status.fons_rpc_sent", "The receive RPC was sent."));
    return true;
}

// ---------------------------------------------------------------------------
// ui-buttons
// ---------------------------------------------------------------------------

bool SubmitScan() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr) return false;
    State& state = S();
    if (state.pending_request.id != 0) return true;
    return service->request_scan(service->user, &state.pending_request).code ==
           ANOMALY_STATUS_V1_OK;
}

bool PollRequestDone(AnomalyNteUiButtonRequestSnapshotV1* done) {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    State& state = S();
    if (service == nullptr || done == nullptr || state.pending_request.id == 0) return false;
    if (service->request_snapshot(service->user, state.pending_request, done).code !=
        ANOMALY_STATUS_V1_OK) {
        state.pending_request = {};
        return true;
    }
    if (done->state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
        state.pending_request = {};
        return true;
    }
    return false;
}

void CancelPendingRequest() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    State& state = S();
    if (service == nullptr || state.pending_request.id == 0) return;
    if (service->cancel != nullptr) {
        static_cast<void>(service->cancel(service->user, state.pending_request));
    }
    state.pending_request = {};
}

std::uint32_t ButtonCount() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr) return 0;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return 0;
    return status.button_count;
}

bool FindButtonName(const char* name, AnomalyNteUiButtonSnapshotV1& out) {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->find == nullptr || name == nullptr) return false;
    AnomalyNteUiButtonQueryV1 query{sizeof(query)};
    query.name = Str(name, std::strlen(name));
    std::uint32_t matches = 0;
    return service->find(service->user, &query, &out, &matches).code == ANOMALY_STATUS_V1_OK &&
           matches > 0;
}

bool TryClickName(const char* name, const bool force) {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->request_click == nullptr) return false;
    AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
    if (!FindButtonName(name, button)) return false;
    AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
    click.button = button.button;
    click.flags = 0u;
    static_cast<void>(force);  // 保留参数兼容旧调用：不强制点击，只点可点击的
    State& state = S();
    CancelPendingRequest();
    return service->request_click(service->user, &click, &state.pending_request).code ==
           ANOMALY_STATUS_V1_OK;
}

// 只碰宿主判定为可点击的按钮：切页 tab、领取按钮在页面未就绪 / 条件未满足时都不可点，
// 这时候点下去不会发生任何事，只会把重试次数白白耗掉。
bool ButtonClickable(const AnomalyNteUiButtonSnapshotV1& button) {
    return button.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE;
}

// 控件匹配：名字本身，或 path 的最后一段（弹窗根节点带实例号，如
// UI_CommonAwardsPopUpWindows_C_2147456120/WB_CommonDeactivate）。
bool MatchWidget(const AnomalyNteUiButtonSnapshotV1& button, const char* target) {
    if (target == nullptr || target[0] == '\0') return false;
    if (std::strcmp(button.name, target) == 0) return true;
    const char* slash = std::strrchr(button.path, '/');
    return slash != nullptr && std::strcmp(slash + 1, target) == 0;
}

// 进游戏时会停在登录/公告页上，需要点一下 UI_Button_Login 才真正进世界。
// 只点宿主判定可点击的那个，点不到就下次再试（不占用链路状态）。
bool NudgeLoginButton() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    State& state = S();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr ||
        service->request_click == nullptr) {
        return false;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return false;
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (!MatchWidget(button, kLoginButtonName)) continue;
        if (!ButtonClickable(button)) {
            // 只在宿主明确报可点击时才点（不做强制点击）：加载遮罩还没撤就继续等。
            if (state.login_blocked_logs < 6) {
                WinLogf("login button present but not clickable yet (category=%d)",
                        static_cast<int>(button.category));
                ++state.login_blocked_logs;
            }
            continue;
        }
        AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
        click.button = button.button;
        click.flags = 0u;
        CancelPendingRequest();
        return service->request_click(service->user, &click, &state.pending_request).code ==
               ANOMALY_STATUS_V1_OK;
    }
    return false;
}

// 诊断：把与登录/HUD 判据相关的控件连同 category 打出来，避免再靠猜。
void LogWaitCandidates() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr) return;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return;
    int shown = 0;
    std::uint32_t clickable = 0;
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (ButtonClickable(button)) ++clickable;
        const bool interesting = std::strstr(button.name, "Login") != nullptr ||
                                 std::strstr(button.name, "AdventureManual") != nullptr ||
                                 std::strstr(button.name, "BoundAction") != nullptr;
        if (!interesting || shown >= 8) continue;
        WinLogf("wait candidate: %s cat=%d clickable=%d", button.name,
                static_cast<int>(button.category), ButtonClickable(button) ? 1 : 0);
        ++shown;
    }
    WinLogf("wait catalog: total=%u clickable=%u", status.button_count, clickable);
}

// 登录/公告页是否还盖在屏幕上：只有「能点的登录按钮」才算还没进去。
// 进世界后控件树上可能还留着它，但那时不可点击。
bool LoginPageVisible() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr) {
        return false;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return false;
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (!MatchWidget(button, kLoginButtonName)) continue;
        if (ButtonClickable(button)) return true;
    }
    return false;
}

// 真正进到游戏里的判据：主界面 HUD 的探索指南按钮已经出现在按钮目录里。
// （nte.session 在登录页也可能报 WORLD_READY，不能只靠它。）
bool HudReady() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr) {
        return false;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return false;
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        // 任一「游戏内才有」的控件在目录里且不是隐藏状态，就算进了游戏。
        if (button.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN) continue;
        for (const char* marker : kIngameMarkers) {
            if (MatchWidget(button, marker)) return true;
        }
    }
    return false;
}

// 目录扫描 + 只点可点击：弹窗按钮常带实例号，按 MatchWidget 匹配（名字或 path 末段）。
bool TryClickWidget(const char* target) {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    State& state = S();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr ||
        service->request_click == nullptr) {
        return false;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return false;
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (!MatchWidget(button, target) || !ButtonClickable(button)) continue;
        AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
        click.button = button.button;
        click.flags = 0u;
        CancelPendingRequest();
        return service->request_click(service->user, &click, &state.pending_request).code ==
               ANOMALY_STATUS_V1_OK;
    }
    return false;
}

int ClickAllName(const char* name) {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->button_at == nullptr || name == nullptr) return 0;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status == nullptr ||
        service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) {
        return 0;
    }
    int clicked = 0;
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (std::strcmp(button.name, name) != 0) continue;
        AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
        click.button = button.button;
        click.flags = 0u;
        State& state = S();
        if (service->request_click(service->user, &click, &state.pending_request).code ==
            ANOMALY_STATUS_V1_OK) {
            ++clicked;
        }
    }
    return clicked;
}

// ---------------------------------------------------------------------------
// 领奖链（控件回退方案）
// ---------------------------------------------------------------------------

namespace {

// 当前在跑哪条领取链路。
const RewardTableDef& RewardTable() {
    const int table = S().reward_table == kRewardTablePeriodic ? kRewardTablePeriodic : kRewardTableDaily;
    return kRewardTables[table];
}

const RewardTableDef& RewardTableFor(const int table) {
    return kRewardTables[table == kRewardTablePeriodic ? kRewardTablePeriodic : kRewardTableDaily];
}

// 页面就绪检查：目标按钮出现在目录里就算切过去了。target 为空时认每日活跃页的两个领取按钮。
bool ButtonPresent(const char* target) {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr) {
        return false;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return false;
    const bool any_target = target == nullptr || target[0] == '\0';
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (any_target) {
            if (std::strcmp(button.name, "ReceiveButton") == 0 ||
                std::strcmp(button.name, "ButtonAccept") == 0) {
                return true;
            }
            continue;
        }
        if (std::strcmp(button.name, target) == 0) return true;
    }
    return false;
}

struct StepOutcome {
    std::uint32_t clicked;
    bool blocked;  // 控件在、但宿主说不可点击（多半已经领过 / 条件不满足）
};

// 宿主目录会保留已经关掉的窗口控件，只有 HIDDEN 才代表它不在屏幕上了——
// 领取后「ButtonDeactivate present but not clickable (category=3)」正是这个来源。
bool WidgetVisible(const char* name, const bool prefix) {
    if (name == nullptr || name[0] == '\0') return false;
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr) {
        return false;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return false;
    const std::size_t length = std::strlen(name);
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        const bool hit = prefix ? std::strncmp(button.name, name, length) == 0
                                : std::strcmp(button.name, name) == 0;
        if (!hit) continue;
        if (button.category != ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN) return true;
    }
    return false;
}

// 探索指南面板还开在屏幕上吗：面板自己的 tab 行或关闭键还在就算开着。
bool RewardPanelVisible() {
    return WidgetVisible(kPanelTabName, false) || WidgetVisible(kPanelClosePrefix, true);
}

// 奖励弹窗还盖着吗：只认**可点击**的确定/关闭控件——弹窗开着时它们必然可点，
// 关掉之后目录里会残留成 HIDDEN/BLOCKED，那都不算弹窗还在。
bool RewardPopupVisible() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr) {
        return false;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return false;
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (!ButtonClickable(button)) continue;
        if (MatchWidget(button, kAwardSureName) || MatchWidget(button, kAwardCloseName) ||
            MatchWidget(button, kAwardExitName)) {
            return true;
        }
    }
    return false;
}

bool RewardDismissPending() { return RewardPanelVisible() || RewardPopupVisible(); }

// 按名字前缀点一个可点击的控件（窗口关闭键带实例号后缀）。只点宿主报 CLICKABLE 的。
bool TryClickWidgetPrefix(const char* prefix) {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr ||
        service->request_click == nullptr || prefix == nullptr || prefix[0] == '\0') {
        return false;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return false;
    State& state = S();
    const std::size_t length = std::strlen(prefix);
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (std::strncmp(button.name, prefix, length) != 0 || !ButtonClickable(button)) continue;
        AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
        click.button = button.button;
        click.flags = 0u;
        CancelPendingRequest();
        return service->request_click(service->user, &click, &state.pending_request).code ==
               ANOMALY_STATUS_V1_OK;
    }
    return false;
}

// 关不掉的时候，把屏幕上到底还有什么打出来——下次就不用再猜关闭键叫什么。
void LogRewardUiCandidates() {
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr) return;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return;
    int shown = 0;
    for (std::uint32_t i = 0; i < status.button_count && shown < 12; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (button.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN) continue;
        const char* slash = std::strrchr(button.path, '/');
        WinLogf("reward ui candidate: %s cat=%d text=%s path=%s", button.name,
                static_cast<int>(button.category), button.text[0] != '\0' ? button.text : "-",
                slash != nullptr ? slash + 1 : "-");
        ++shown;
    }
    WinLogf("reward ui catalog: total=%u", status.button_count);
}

// mode 1 会把目录里所有同名按钮都点一遍。
StepOutcome RewardClickStep(const RewardStep& step) {
    StepOutcome outcome{0, false};
    if (step.mode == 7) {
        // 验证式关窗：依次试「确定 → 弹窗关闭 → 最外层关闭 → 关闭键（前缀）」，
        // 一次只点一个，点完等目录刷新再看窗口是不是真的消失了。
        State& state = S();
        if (TryClickWidget(kAwardSureName) || TryClickWidget(kAwardCloseName) ||
            TryClickWidget(kAwardExitName) || TryClickWidgetPrefix(kPanelClosePrefix)) {
            outcome.clicked = 1;
            return outcome;
        }
        outcome.blocked = RewardDismissPending();
        if (outcome.blocked && state.reward_blocked_step != state.reward_step) {
            state.reward_blocked_step = state.reward_step;
            WinLogf("reward step %s: nothing clickable to close the window", step.label);
            LogRewardUiCandidates();
        }
        return outcome;
    }
    const AnomalyNteUiButtonsServiceV1* service = Buttons();
    if (service == nullptr || service->status == nullptr || service->button_at == nullptr) {
        return outcome;
    }
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return outcome;
    State& state = S();
    std::uint32_t clicked = 0;
    int blocked_category = -1;
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        bool hit = false;
        if (step.mode == 2) {
            hit = std::strstr(button.class_name, step.target) != nullptr;
        } else if (step.mode == 5 || step.mode == 6) {
            // 前缀匹配：同一个控件在不同实例上带不同的数字后缀。
            hit = std::strncmp(button.name, step.target, std::strlen(step.target)) == 0;
        } else {
            hit = std::strcmp(button.name, step.target) == 0;
        }
        if (!hit) continue;
        if (step.caption != nullptr && std::strcmp(button.text, step.caption) != 0) {
            // 同名控件的另一个 tab：记下实际 caption，方便核对是不是点在了同一页。
            if (state.reward_caption_logs < 8) {
                WinLogf("reward step %s skipped %s (caption=%s)", step.label, button.name,
                        button.text[0] != '\0' ? button.text : "-");
                ++state.reward_caption_logs;
            }
            continue;
        }
        if (!ButtonClickable(button)) {
            if (blocked_category < 0) blocked_category = static_cast<int>(button.category);
            continue;
        }
        AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
        click.button = button.button;
        click.flags = 0u;
        if (service->request_click(service->user, &click, &state.pending_request).code !=
            ANOMALY_STATUS_V1_OK) {
            break;
        }
        ++clicked;
        if (step.mode == 0 || step.mode == 5 || step.mode == 6) {
            WinLogf("reward step %s clicked %s (caption=%s)", step.label, button.name,
                    button.text[0] != '\0' ? button.text : "-");
            break;  // 单点模式只点第一个匹配
        }
    }
    if (clicked > 1) {
        WinLogf("reward step %s clicked %u buttons", step.label, clicked);
    }
    if (clicked == 1 && step.mode == 2) {
        WinLogf("reward step %s clicked %s", step.label, step.target);
    }
    if (clicked == 0 && blocked_category >= 0) {
        outcome.blocked = true;
        // 每一步只吼一次，避免整屏同一条诊断。
        if (state.reward_blocked_step != state.reward_step) {
            state.reward_blocked_step = state.reward_step;
            WinLogf("reward step %s: %s present but not clickable (category=%d)", step.label,
                    step.target, blocked_category);
        }
    }
    outcome.clicked = clicked;
    return outcome;
}

void AdvanceRewardStep(const std::uint32_t clicked_in, const bool blocked) {
    State& state = S();
    const RewardTableDef& table = RewardTable();
    const std::uint32_t clicked = clicked_in;
    if (clicked) {
        state.reward_blocked_step = -1;
        state.reward_retries = kRewardRetries;
        state.reward_tab_rounds = 0;
        ++state.reward_step;
        if (state.reward_step >= table.count) {
            state.reward_step = -1;
            WinLogf("reward chain finished (%s)", table.label);
            SetStatus(table.label == nullptr || std::strcmp(table.label, "daily") == 0
                          ? TextLocalized("status.reward_done", "Daily rewards were claimed; confirm them in game.")
                          : TextLocalized("status.periodic_done",
                              "Periodic quest rewards were claimed; confirm them in game."));
            return;
        }
        WinLogf("reward step done: %s", table.steps[state.reward_step - 1].label);
        if (!SubmitScan()) {
            state.reward_step = -1;
            WinLog("reward chain aborted: scan request rejected");
            SetStatus(TextLocalized("status.reward_broken", "The reward chain stopped because the scan was rejected."));
            return;
        }
        state.reward_wait = table.steps[state.reward_step - 1].wait;
        return;
    }
    const RewardStep& failed = table.steps[state.reward_step];
    if (blocked && failed.idempotent) {
        // 已经领过：控件还在但不可点击。直接放过，不耗重试、不回退页签。
        WinLogf("reward step %s: nothing to claim, skipping", failed.label);
        state.reward_blocked_step = -1;
        state.reward_retries = kRewardRetries;
        ++state.reward_step;
        if (state.reward_step >= table.count) {
            state.reward_step = -1;
            WinLogf("reward chain finished (%s)", table.label);
            SetStatus(table.label != nullptr && std::strcmp(table.label, "daily") == 0
                          ? TextLocalized("status.reward_done", "Daily rewards were claimed; confirm them in game.")
                          : TextLocalized("status.periodic_done",
                              "Periodic quest rewards were claimed; confirm them in game."));
            return;
        }
        if (SubmitScan()) state.reward_wait = table.steps[state.reward_step - 1].wait;
        return;
    }
    if (state.reward_retries > 0) {
        --state.reward_retries;
        if (SubmitScan()) state.reward_wait = 15;
        return;
    }
    if (failed.required && state.reward_tab_rounds < kRewardTabRounds) {
        // 面板没开出来或页面没切过去：退回切页那一步重来，别把后面的领取步骤空点掉。
        ++state.reward_tab_rounds;
        WinLogf("reward step %s not actionable; back to the tab step %d (%d/%d)", failed.label,
                table.tab_step, state.reward_tab_rounds, kRewardTabRounds);
        state.reward_step = table.tab_step;
        state.reward_retries = kRewardRetries;
        state.reward_blocked_step = -1;
        if (SubmitScan()) state.reward_wait = 30;
        return;
    }
    WinLogf("reward step skipped: %s", failed.label);
    state.reward_retries = kRewardRetries;
    state.reward_blocked_step = -1;
    ++state.reward_step;
    if (state.reward_step >= table.count) {
        state.reward_step = -1;
        WinLogf("reward chain finished with skipped steps (%s)", table.label);
        SetStatus(TextLocalized("status.reward_partial", "Rewards were claimed with skipped steps; confirm them in game."));
        return;
    }
    if (!SubmitScan()) {
        state.reward_step = -1;
        WinLog("reward chain aborted: scan request rejected");
        SetStatus(TextLocalized("status.reward_broken", "The reward chain stopped because the scan was rejected."));
        return;
    }
    state.reward_wait = 15;
}

}  // namespace

SessionIdentity CurrentSession() {
    // PID + 进程创建时间（秒，低 32 位）：同一个游戏进程里热重载插件两者都不变，
    // 只有真的重启游戏才会变。
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return SessionIdentity{GetCurrentProcessId(), 0};
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = created.dwLowDateTime;
    ticks.HighPart = created.dwHighDateTime;
    const auto seconds = static_cast<std::uint32_t>((ticks.QuadPart / 10000000ull) & 0xFFFFFFFFull);
    return SessionIdentity{GetCurrentProcessId(), seconds};
}

void StartRewardChain(const RewardGroup group) {
    State& state = S();
    // 控件链由宿主 ui-buttons 服务完成反射，插件自身不需要引擎签名：
    // profile 不可用时这条链路仍然可用。
    if (Buttons() == nullptr) {
        SetStatus(TextLocalized("status.ui_buttons_missing", "The ui-buttons service is unavailable."));
        return;
    }
    if (state.reward_step >= 0) return;
    const int table_index = group == RewardGroup::Periodic ? kRewardTablePeriodic : kRewardTableDaily;
    const RewardTableDef& table = RewardTableFor(table_index);
    CancelPendingRequest();
    if (!SubmitScan()) {
        SetStatus(TextLocalized("status.scan_rejected", "The scan request was rejected; try again later."));
        return;
    }
    state.reward_table = table_index;
    state.reward_step = 0;
    state.reward_retries = kRewardRetries;
    state.reward_tab_rounds = 0;
    state.reward_caption_logs = 0;
    state.reward_blocked_step = -1;
    state.reward_stuck = false;
    WinLogf("reward chain started (%s, %d steps)", table.label, table.count);
    SetStatus(group == RewardGroup::Periodic
                  ? TextLocalized("status.periodic_start", "Claiming periodic quest rewards...")
                  : TextLocalized("status.reward_start", "Claiming daily rewards..."));
}

void TickRewardChain() {
    State& state = S();
    if (state.reward_step < 0) return;
    AnomalyNteUiButtonRequestSnapshotV1 done{sizeof(done)};
    if (state.reward_wait > 0) {
        --state.reward_wait;
        return;
    }
    if (!PollRequestDone(&done)) return;
    const RewardStep& step = RewardTable().steps[state.reward_step];
    if (step.mode == 3) {
        AdvanceRewardStep(ButtonPresent(step.target) ? 1u : 0u, false);
        return;
    }
    if (step.mode == 5) {
        // 清残留窗口：有没有都立刻往下走，别为它耗掉重试次数。
        if (RewardClickStep(step).clicked == 0) {
            WinLogf("reward step %s: nothing to close", step.label);
        }
        AdvanceRewardStep(1u, false);
        return;
    }
    if (step.mode == 7) {
        // 弹窗和面板都已经不在屏幕上：这一步才算过。
        if (!RewardDismissPending()) {
            WinLogf("reward step done: %s", step.label);
            AdvanceRewardStep(1u, false);
            return;
        }
        const StepOutcome outcome = RewardClickStep(step);
        if (outcome.clicked > 0) {
            // 点到了：给它收起的时间，预算重置，等目录刷新后再判断关没关掉。
            state.reward_retries = kRewardCloseRetries;
            if (SubmitScan()) state.reward_wait = 20;
            return;
        }
        if (state.reward_retries > 0) {
            --state.reward_retries;
            if (SubmitScan()) state.reward_wait = 30;
            return;
        }
        // 关不掉：停在这里，绝不带着开着的面板进下一条链路（面板会挡死 HUD 入口按钮）。
        state.reward_step = -1;
        state.reward_stuck = true;
        WinLogf("reward chain stopped: the reward window is still open (%s)", step.label);
        SetStatus(TextLocalized("status.reward_window_stuck",
            "Rewards are claimed but the reward window is still open; close it and the chain can continue."));
        return;
    }
    const StepOutcome outcome = RewardClickStep(step);
    AdvanceRewardStep(outcome.clicked, outcome.blocked);
}

bool RewardChainBusy() { return S().reward_step >= 0; }

bool RewardChainStuck() { return S().reward_stuck; }

void AbortRewardChain() {
    State& state = S();
    state.reward_step = -1;
    state.reward_wait = 0;
    state.reward_retries = 0;
    state.reward_tab_rounds = 0;
    state.reward_stuck = false;
}

// ---------------------------------------------------------------------------
// 一咖舍链路
// ---------------------------------------------------------------------------

void StartFonsChain() {
    State& state = S();
    if (state.fons_chain != 0) return;
    CancelPendingRequest();
    if (!ExtractFons()) {
        state.fons_chain = 0;
        return;
    }
    state.fons_chain = 1;
    state.fons_wait = 20;
}

void TickFonsChain() {
    State& state = S();
    if (state.fons_chain == 0) return;
    if (state.fons_wait > 0) {
        --state.fons_wait;
        return;
    }
    // 必须有在飞的扫描请求：RPC 发完之后目录还没扫过，PollRequestDone 会一直返回 false，
    // 链条就永远停在第一步（就是「一咖舍没反应」的现场）。
    if (state.pending_request.id == 0) {
        if (SubmitScan()) state.fons_wait = 15;
        return;
    }
    AnomalyNteUiButtonRequestSnapshotV1 done{sizeof(done)};
    if (!PollRequestDone(&done)) return;

    // 弹窗逐层关：确定 -> ButtonDeactivate -> WB_CommonDeactivate（最外层窗口）。
    // 每一步先等它出现（重扫 kFonsDialogTries 次），还没出现就跳过并留日志。
    const auto advance = [&state](int next) {
        state.fons_chain = next;
        state.fons_retries = 0;
        state.fons_wait = 30;
    };
    const auto wait_or_skip = [&state](int next, const char* what) {
        if (state.fons_retries < kFonsDialogTries) {
            ++state.fons_retries;
            if (SubmitScan()) state.fons_wait = 30;
            return false;
        }
        WinLogf("fons: %s never showed up; skipping", what);
        state.fons_retries = 0;
        state.fons_chain = next;
        state.fons_wait = 30;
        return false;
    };

    switch (state.fons_chain) {
        case 1:
            if (TryClickWidget(kAwardSureName)) {
                WinLog("fons: award dialog confirmed");
                advance(2);
            } else {
                static_cast<void>(wait_or_skip(2, "the award confirm button"));
            }
            break;
        case 2:
            if (TryClickWidget(kAwardCloseName)) {
                WinLog("fons: award dialog dismissed");
                advance(3);
            } else {
                static_cast<void>(wait_or_skip(3, "the award dialog close button"));
            }
            break;
        case 3:
            if (TryClickWidget(kAwardExitName)) {
                WinLog("fons: common awards window closed");
                advance(4);
            } else {
                static_cast<void>(wait_or_skip(4, "the common awards window"));
            }
            break;
        case 4:
            state.fons_chain = 0;
            state.fons_wait = 0;
            WinLog("fons chain finished");
            SetStatus(TextLocalized("status.fons_done", "The one-shop withdrawal is done; confirm it in game."));
            break;
        default:
            state.fons_chain = 0;
            break;
    }
}

bool FonsChainBusy() { return S().fons_chain != 0; }

void AbortFonsChain() {
    State& state = S();
    state.fons_chain = 0;
    state.fons_wait = 0;
    state.fons_retries = 0;
}

// ---------------------------------------------------------------------------
// 一键日常
// ---------------------------------------------------------------------------

void StartClaimAll() {
    State& state = S();
    if (state.auto_wait_ticks > 0) return;
    ClaimDaily(-1);
}

void TickClaimAll() {
    State& state = S();
    if (state.auto_wait_ticks > 0) --state.auto_wait_ticks;
}

bool ClaimAllBusy() { return S().auto_wait_ticks > 0; }

void AbortClaimAll() { S().auto_wait_ticks = 0; }

}  // namespace daily
