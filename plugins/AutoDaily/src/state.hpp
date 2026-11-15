#pragma once

// 自动日常：共享状态与服务表。
// 所有模块（反射层 / 界面 / 链路）只通过这里的 State 访问宿主服务与用户设置，
// 避免在多个文件里各存一份容易失配的副本。

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ipc.h"
#include "anomaly/sdk/services/json.h"
#include "anomaly/sdk/services/localization.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/ui_resources.h"
#include "anomaly/sdk/services/ue5.h"
#include "profile_service.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <string>

#include "native_layout.hpp"
#include "settings_model.hpp"
#include "text_view.hpp"

namespace daily {

using anomaly::sdk::Ok;

constexpr std::size_t kMaxLogLines = 24;
constexpr std::size_t kLogLineBytes = 192;
constexpr std::size_t kStatusBytes = 256;
constexpr std::size_t kNameBytes = 1024;
constexpr std::uint32_t kMaximumObjectCount = 16u * 1024u * 1024u;
constexpr std::uint32_t kMaximumObjectChunks = 4096u;
constexpr std::size_t kMaximumClaimTargets = 48;

// 「HAS」：服务表字段存在且可用。宿主按 struct_size 报告自己支持的字段，
// 老版本宿主尾部字段缺失时不调用，而不是把 null 函数指针当契约。
#define HAS(service, type, field)                                             \
    ((service) != nullptr && (service)->struct_size >=                        \
         offsetof(type, field) + sizeof((service)->field) &&                  \
         (service)->field != nullptr)

// 一条龙阶段。
enum class Stage {
    Idle = 0,
    WaitWorld = 1,
    Dungeon = 2,
    Cafe = 3,
    DailyRewards = 4,     // 每日活跃奖励（「全部」页 + 活跃度/最终领取）
    PeriodicRewards = 5,  // 环期任务（CombatAward -> 「环期任务」页 -> 全部领取）
    Done = 6,
};

// 停止来源，只用于展示/日志（不进入配置）。
enum class StopCause {
    None = 0,
    Hotkey = 1,
    Disabled = 2,
    Manual = 3,
};

struct State {
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyLocalizationServiceV1* localization{};
    const AnomalyConfigServiceV1* config{};
    const AnomalyJsonServiceV1* json{};
    const AnomalyProfileServiceV1* profile{};
    const AnomalyInputServiceV1* input{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyIpcServiceV1* ipc{};
    const AnomalySignatureServiceV1* signature{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyNteUiButtonsServiceV1* uibuttons{};
    const AnomalyNteSessionServiceV1* session{};

    Settings settings{};
    ConfigStore config_store{};

    // 兼容数据：偏移与签名来自 package 内的 profile/compatibility.profile.json。
    Layout layout{};
    bool layout_ready{};
    char layout_error[kLogLineBytes]{};
    char engine_error[kLogLineBytes]{};  // 引擎符号解析失败的具体原因（状态栏会带上它）

    // 反射层运行时
    std::uintptr_t registry{};
    std::uintptr_t gworld_slot{};
    bool engine_ready{};
    std::uintptr_t store_mgmt_rpc{};
    std::uintptr_t player_state_cls{};
    std::uintptr_t submit_daily_fn{};
    std::uintptr_t claim_activation_fn{};
    std::uintptr_t claim_chapter_fn{};
    std::uintptr_t claim_cultivation_fn{};
    std::uintptr_t claim_teaching_fn{};

    // 界面共享的请求位（Draw 置位，Update 消费）
    std::atomic_int request_chain_start{};
    std::atomic_int request_chain_stop{};
    std::atomic_int request_claim_all{};
    std::atomic_int request_fons{};
    // 1 = 领每日活跃奖励 2 = 领环期任务奖励（两条链路分开执行）
    std::atomic_int request_reward_run{};
    std::atomic_int request_dungeon_only{};
    std::atomic_int request_save{};
    std::atomic_int request_placeholders{};
    std::atomic_bool stop_requested{};
    std::atomic_bool settings_dirty{};

    // 热键
    AnomalyGenerationHandleV1 stop_hotkey{};

    // 一条龙
    Stage stage{Stage::Idle};
    StopCause stop_cause{StopCause::None};
    double stage_timer{};
    double dungeon_poll_timer{};
    bool stage_entered{};
    bool chain_automatic{};
    // 只跑某一个阶段（界面上的单阶段按钮）：Idle 表示整条链。
    Stage single_target{Stage::Idle};
    // 进游戏时点 UI_Button_Login 的节奏控制
    int login_wait{};
    bool login_clicked{};
    double world_ready_timer{};  // 真正进到游戏里之后计时，用于 world_settle_seconds
    int login_blocked_logs{};    // 「登录按钮存在但不可点击」的诊断日志配额
    int login_tries{};           // 本轮进游戏已经尝试点登录按钮的次数
    int wait_diag_logs{};        // 「等世界」诊断日志配额
    int wait_diag_wait{};
    std::uint32_t stage_polls{};
    std::uint32_t world_settle_polls{};
    std::uint32_t dungeon_polls{};
    std::uint32_t cafe_polls{};
    std::uint32_t reward_polls{};
    char stage_note[kStatusBytes]{};

    // 一咖舍链路（RPC + 清弹窗）
    int fons_chain{};
    int fons_wait{};
    int fons_retries{};  // 等弹窗出现时的重扫次数
    std::uint64_t updates{};
    std::uint32_t tick{};
    int auto_wait_ticks{};

    // 领奖链（两条链路共用一套步骤游标，同一时刻只会跑一条）
    int reward_table{};  // 0 = 每日活跃奖励 1 = 环期任务奖励
    int reward_step{-1};
    int reward_wait{};
    int reward_retries{};
    int reward_tab_rounds{};  // 前置步骤失败后退回切页步骤重试的次数
    int reward_caption_logs{};  // 诊断日志配额（避免每轮扫描刷屏）
    int reward_blocked_step{-1};  // 已经打过「控件不可点击」日志的步骤

    // ui-buttons 请求（同一时刻只保留一个在飞请求）
    AnomalyGenerationHandleV1 pending_request{};

    // 副本插件 IPC：完成回调可能来自任意线程，响应只在 Game 线程解析。
    std::atomic_bool save_in_flight{};
    std::atomic_bool hotkey_dirty{};
    AnomalyGenerationHandleV1 reload_task{};

    std::mutex ipc_mutex{};
    std::string ipc_response{};
    int ipc_status_code{};
    bool ipc_has_response{};
    bool ipc_in_flight{};
    AnomalyGenerationHandleV1 dungeon_call{};

    char status[kStatusBytes]{};
    char log_lines[kMaxLogLines][kLogLineBytes]{};
    int log_head{};
    int log_count{};
    bool first_run_popup{};
};

State& S();
void ResetState();

void LogInfo(const char* text);
void LogWarn(const char* text);
void WinLog(const char* text);
void WinLogf(const char* format, ...);
void SetStatus(const char* text);
void SetStatusText(const std::string& text);
inline void SetStatus(const std::string& text) { SetStatus(text.c_str()); }

}  // namespace daily