#pragma once

// 反射层与游戏交互：内存读取、ProcessEvent、ui-buttons 代点、两条链路状态机。
// 只依赖 State 里的服务表与 profile 偏移；任何偏移缺失都直接失败，不回落硬编码。

#include "profile_layout.hpp"
#include "state.hpp"

#include <cstdint>

namespace daily {

// Load 阶段填好的兼容数据快照（偏移 + 两条签名）。
extern ProfileSnapshot g_profile;

// ---- 反射层 ----
bool EnsureEngine();
std::uintptr_t LocalPlayerState();
// only_target <0 表示五类全领，否则只领第 only_target 类。
bool ClaimDaily(int only_target);
bool ExtractFons();

// ---- ui-buttons ----
bool SubmitScan();
bool PollRequestDone(AnomalyNteUiButtonRequestSnapshotV1* done);
void CancelPendingRequest();
std::uint32_t ButtonCount();
bool FindButtonName(const char* name, AnomalyNteUiButtonSnapshotV1& out);
bool TryClickName(const char* name, bool force);
int ClickAllName(const char* name);

// ---- 领奖链（回退方案：控件链路）----
// 两种领取分开跑：每日活跃奖励 / 环期任务奖励，各有一套步骤。
enum class RewardGroup {
    Daily,
    Periodic,
};
void StartRewardChain(RewardGroup group);

// 当前游戏进程的会话标识（PID + 进程创建时间秒）。插件热重载不变，重启游戏才变。
struct SessionIdentity {
    std::uint32_t pid;
    std::uint32_t stamp;
};
SessionIdentity CurrentSession();
void TickRewardChain();
bool RewardChainBusy();
void AbortRewardChain();

// ---- 进游戏 ----
// 停在登录/公告页时点一下 UI_Button_Login；可点击才会点，返回是否发出了点击。
// 只在宿主报告可点击时才点（不做强制点击）。
bool NudgeLoginButton();
// 登录/公告页是否还在；主界面 HUD 是否已经出现（真正进到游戏里的判据）。
bool LoginPageVisible();
bool HudReady();
// 诊断用：打出登录/HUD 判据相关控件的 category。
void LogWaitCandidates();

// ---- 一咖舍链路（RPC 提取 + 清弹窗）----
void StartFonsChain();
void TickFonsChain();
bool FonsChainBusy();
void AbortFonsChain();

// 「一键日常」：优先反射领取；控件没出现就发 F1 打开面板，等一会儿再领。
void StartClaimAll();
void TickClaimAll();
bool ClaimAllBusy();
void AbortClaimAll();

}  // namespace daily
