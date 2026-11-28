#include "chain.hpp"

#include "clone_enter.hpp"
#include "dungeon.hpp"
#include "game.hpp"
#include "i18n.hpp"

#include <cstdio>

namespace daily {
namespace {

constexpr double kDungeonPollSeconds = 1.0;

bool WorldReady() {
    const State& state = S();
    if (state.session == nullptr || state.session->snapshot == nullptr) return false;
    AnomalyNteSessionSnapshotV1 snapshot{sizeof(snapshot)};
    if (state.session->snapshot(state.session->user, &snapshot).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    return snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY;
}

void Enter(Stage stage, const char* key, const char* fallback) {
    State& state = S();
    state.stage = stage;
    state.stage_timer = 0.0;
    state.stage_entered = false;
    std::snprintf(state.stage_note, sizeof(state.stage_note), "%s", TextLocalized(key, fallback).c_str());
    WinLogf("stage -> %s", StageFallback(stage));
}

void SetNote(const char* key, const char* fallback) {
    State& state = S();
    std::snprintf(state.stage_note, sizeof(state.stage_note), "%s", TextLocalized(key, fallback).c_str());
}

bool StageTimedOut() {
    const State& state = S();
    return state.stage_timer >= static_cast<double>(state.settings.stage_timeout_seconds);
}

// 固定顺序：副本 -> 一咖舍 -> 每日活跃奖励 -> 环期任务奖励。
// 从 from 之后找第一个开着的阶段；都没有就直接收尾。
Stage PickNext(const Stage from) {
    const Settings& settings = S().settings;
    struct Entry {
        Stage stage;
        bool on;
    };
    const Entry order[] = {
        {Stage::Dungeon, settings.stage_dungeon},
        {Stage::Cafe, settings.stage_cafe},
        {Stage::DailyRewards, settings.stage_daily_rewards},
        {Stage::PeriodicRewards, settings.stage_periodic_rewards},
    };
    constexpr int count = static_cast<int>(sizeof(order) / sizeof(order[0]));
    int start = 0;
    for (int i = 0; i < count; ++i) {
        if (order[i].stage == from) start = i + 1;
    }
    for (int i = start; i < count; ++i) {
        if (order[i].on) return order[i].stage;
    }
    return Stage::Done;
}

// 阶段收尾后该去哪：单阶段模式直接收尾，否则按开关顺序走下一个。
void EnterNext(const Stage from) {
    State& state = S();
    if (state.single_target != Stage::Idle) {
        Enter(Stage::Done, StageKey(Stage::Done), StageFallback(Stage::Done));
        return;
    }
    const Stage next = PickNext(from);
    Enter(next, StageKey(next), StageFallback(next));
}

// ---- 副本插件（Clone Enter）的开关：打副本前确保启用，打完关掉 ----
//
// 宿主对 config/plugin-enablement.json 有 750ms debounce 的文件监视器：
// 写 false 等于停用（卸载），写 true 等于启用（加载），false→true 就是重新加载。
// 写盘在 Worker 线程，这里只负责推进「等写完 → 等卸载 → 等重新加载」的节奏。

constexpr double kCloneEnterHostSettleSeconds = 1.5;
constexpr double kCloneEnterWaitSeconds = 20.0;

enum class PrepareOutcome {
    Waiting,
    Ready,
    GiveUp,
};

void CloseCloneEnter() {
    if (!CloneEnterControlAvailable()) return;
    WinLog("dungeon stage: disabling clone-enter");
    CloneEnterRequestEnabled(false);
}

PrepareOutcome TickCloneEnterPrepare(const double delta_seconds) {
    State& state = S();
    const bool controllable = CloneEnterControlAvailable();
    const double delta = delta_seconds > 0.0 ? delta_seconds : 0.0;
    if (state.dungeon_probe_timer > 0.0) {
        state.dungeon_probe_timer -= delta;
        return PrepareOutcome::Waiting;
    }
    // 探测要发 IPC，节流到 4Hz，免得等对方加载的 20 秒里每帧都打一次。
    const auto probe_endpoint = [&state] {
        state.dungeon_probe_timer = 0.25;
        return DungeonProbe();
    };
    state.dungeon_prepare_timer += delta;
    const bool timed_out = state.dungeon_prepare_timer >= kCloneEnterWaitSeconds;

    if (state.dungeon_prepare_phase == 0) {
        if (!DungeonIpcAvailable()) {
            WinLog("dungeon stage skipped: the IPC service is unavailable");
            SetNote("stage.dungeon_unavailable", "Dungeon plugin is unavailable; skipped");
            return PrepareOutcome::GiveUp;
        }
        // 先探测对方端点是否在线：在线 = 已启用，需要关闭再打开（重新加载）。
        const DungeonProbeResult probe = probe_endpoint();
        if (probe == DungeonProbeResult::Pending) return PrepareOutcome::Waiting;
        if (!controllable) {
            if (probe == DungeonProbeResult::Ready) {
                WinLog("dungeon stage: clone-enter control unavailable; using it as-is");
                return PrepareOutcome::Ready;
            }
            WinLog("dungeon stage skipped: clone-enter is unavailable and cannot be enabled");
            SetNote("stage.dungeon_unavailable", "Dungeon plugin is unavailable; skipped");
            return PrepareOutcome::GiveUp;
        }
        if (probe == DungeonProbeResult::Missing) {
            WinLog("dungeon stage: clone-enter is not running; enabling it");
            SetNote("stage.dungeon_enabling", "Enabling the dungeon plugin");
            CloneEnterRequestEnabled(true);
            state.dungeon_prepare_phase = 2;
            state.dungeon_prepare_timer = 0.0;
            return PrepareOutcome::Waiting;
        }
        WinLog("dungeon stage: clone-enter is enabled; reloading it");
        SetNote("stage.dungeon_reloading", "Reloading the dungeon plugin");
        CloneEnterRequestEnabled(false);
        state.dungeon_prepare_phase = 1;
        state.dungeon_prepare_timer = 0.0;
        return PrepareOutcome::Waiting;
    }

    if (state.dungeon_prepare_phase == 1) {
        // 等停用生效（写完 + 宿主应用 + 对方真的从 IPC 上消失），再写启用。
        if (CloneEnterWriteInFlight() ||
            state.dungeon_prepare_timer < kCloneEnterHostSettleSeconds) {
            return PrepareOutcome::Waiting;
        }
        const DungeonProbeResult probe = probe_endpoint();
        if (probe == DungeonProbeResult::Pending) return PrepareOutcome::Waiting;
        if (probe == DungeonProbeResult::Ready && !timed_out) return PrepareOutcome::Waiting;
        if (probe == DungeonProbeResult::Ready) {
            WinLog("dungeon stage: clone-enter did not stop in time; re-enabling anyway");
        }
        WinLog("dungeon stage: clone-enter is stopped; enabling it again");
        SetNote("stage.dungeon_enabling", "Enabling the dungeon plugin");
        CloneEnterRequestEnabled(true);
        state.dungeon_prepare_phase = 2;
        state.dungeon_prepare_timer = 0.0;
        return PrepareOutcome::Waiting;
    }

    // 等启用生效：写完 + 宿主加载，直到 IPC 端点能正常响应。
    if (CloneEnterWriteInFlight() ||
        state.dungeon_prepare_timer < kCloneEnterHostSettleSeconds) {
        return PrepareOutcome::Waiting;
    }
    if (probe_endpoint() == DungeonProbeResult::Ready) {
        WinLog("dungeon stage: clone-enter is ready");
        return PrepareOutcome::Ready;
    }
    if (timed_out) {
        WinLog("dungeon stage skipped: clone-enter did not become ready");
        SetNote("stage.dungeon_plugin_timeout", "The dungeon plugin did not become ready");
        return PrepareOutcome::GiveUp;
    }
    return PrepareOutcome::Waiting;
}

}  // namespace

const char* StageKey(Stage stage) {
    switch (stage) {
        case Stage::WaitWorld:
            return "stage.wait_world";
        case Stage::Dungeon:
            return "stage.dungeon";
        case Stage::Cafe:
            return "stage.cafe";
        case Stage::DailyRewards:
            return "stage.daily_rewards";
        case Stage::PeriodicRewards:
            return "stage.periodic_rewards";
        case Stage::Done:
            return "stage.done";
        case Stage::Idle:
        default:
            return "stage.idle";
    }
}

const char* StageFallback(Stage stage) {
    switch (stage) {
        case Stage::WaitWorld:
            return "Waiting for world";
        case Stage::Dungeon:
            return "Dungeon";
        case Stage::Cafe:
            return "One-shop withdrawal";
        case Stage::DailyRewards:
            return "Claiming daily rewards";
        case Stage::PeriodicRewards:
            return "Claiming periodic quest rewards";
        case Stage::Done:
            return "Finished";
        case Stage::Idle:
        default:
            return "Idle";
    }
}

bool ChainRunning() {
    const Stage stage = S().stage;
    return stage != Stage::Idle;
}

void ChainStart(const bool automatic) {
    State& state = S();
    if (state.stage != Stage::Idle) return;
    state.stop_cause = StopCause::None;
    state.stage_polls = 0;
    state.dungeon_polls = 0;
    state.chain_automatic = automatic;
    state.single_target = Stage::Idle;
    state.login_wait = 0;
    state.login_clicked = false;
    state.login_tries = 0;
    state.world_ready_timer = 0.0;
    state.wait_diag_logs = 0;
    state.wait_diag_wait = 0;
    // 自动运行的开关不再由这里清理：它是「每次启动都自动跑」的常驻开关，
    // 由 plugin.cpp 在触发时把会话标记更新为当前会话（同一次启动内不会重复触发）。
    static_cast<void>(automatic);
    Enter(Stage::WaitWorld, StageKey(Stage::WaitWorld), StageFallback(Stage::WaitWorld));
    WinLog(automatic ? "one-stop chain started (auto)" : "one-stop chain started (manual)");
    SetStatusText(TextLocalized("status.chain_start", "One-stop chain started."));
}

void ChainStartSingle(const Stage target) {
    State& state = S();
    if (state.stage != Stage::Idle) {
        SetStatusText(TextLocalized("status.stage_busy", "The chain is already running; stop it first."));
        return;
    }
    ChainStart(false);
    state.single_target = target;
    WinLogf("single stage run requested: %s", StageFallback(target));
}

void ChainStop(const StopCause cause, const char* note_key, const char* note_fallback) {
    State& state = S();
    if (state.stage == Stage::Idle && cause == StopCause::None) return;
    const Stage previous = state.stage;
    AbortRewardChain();
    AbortFonsChain();
    AbortClaimAll();
    DungeonCancelPending();
    if (previous == Stage::Dungeon) {
        static_cast<void>(DungeonStop());
        // 副本阶段中断也要把副本插件关掉，别让它留在启用状态。
        CloseCloneEnter();
    }
    state.stage = Stage::Idle;
    state.stage_timer = 0.0;
    state.stage_entered = false;
    state.single_target = Stage::Idle;
    state.stop_cause = cause;
    if (note_key != nullptr) SetNote(note_key, note_fallback);
    WinLogf("one-stop chain stopped (cause=%d, stage=%s)", static_cast<int>(cause),
            StageFallback(previous));
    if (cause == StopCause::Disabled) {
        SetStatusText(TextLocalized("status.stopped_disabled", "Stopped because the master switch is off."));
    } else if (cause == StopCause::Manual) {
        SetStatusText(TextLocalized("status.stopped_manual", "Stopped."));
    }
}

void ChainTick(const double delta_seconds) {
    State& state = S();
    if (state.stage == Stage::Idle) return;
    state.stage_timer += delta_seconds > 0.0 ? delta_seconds : 0.0;
    ++state.stage_polls;

    if (!state.settings.enabled) {
        ChainStop(StopCause::Disabled, "stage.stopped_disabled", "Master switch is off");
        return;
    }

    switch (state.stage) {
        case Stage::WaitWorld: {
            // 「进游戏」要三件事同时成立：nte.session 报世界就绪、登录页已经消失、
            // 主界面 HUD 已经出现。只靠 session 会在登录页就报告就绪，结果角色还没进游戏
            // 就跑去打副本了。
            const bool world_ready = WorldReady();
            const bool login_page = LoginPageVisible();
            const bool hud_ready = HudReady();
            // 只认「游戏内才有的控件」：登录页那套判断（UI_Button_Login）实测会在进世界后
            // 继续残留、甚至仍报可点击，用它当门会永远进不去。
            const bool in_game = world_ready && hud_ready;
            if (!in_game) {
                state.world_ready_timer = 0.0;
                // 卡在等世界时把三个条件打出来，方便判断是没扫到控件还是真的还没进游戏。
                if (state.wait_diag_wait > 0) {
                    --state.wait_diag_wait;
                } else if (state.wait_diag_logs < 6 && state.stage_polls > 120) {
                    WinLogf("waiting to enter the game: session_ready=%d login_page=%d hud=%d buttons=%u",
                            world_ready ? 1 : 0, login_page ? 1 : 0, hud_ready ? 1 : 0,
                            ButtonCount());
                    LogWaitCandidates();
                    ++state.wait_diag_logs;
                    state.wait_diag_wait = 300;
                }
                if (StageTimedOut()) {
                    ChainStop(StopCause::Manual, "stage.world_timeout", "World did not become ready");
                }
                if (state.login_wait > 0) {
                    --state.login_wait;
                    return;
                }
                // 按钮目录只有「扫描请求完成」之后才会更新：没扫过时 button_count 恒为 0，
                // 这时候直接读目录当然找不到登录按钮。所以每轮先扫、扫完再代点。
                if (state.pending_request.id == 0) {
                    if (SubmitScan()) state.login_wait = 3;
                    return;
                }
                AnomalyNteUiButtonRequestSnapshotV1 done{sizeof(done)};
                if (!PollRequestDone(&done)) return;
                // 上一次点击到底有没有被宿主执行：被拒绝会带 reasons，HTUI 按钮还会报
                // press/click 是否被游戏接受。登录点不动时先看这一行。
                if (done.kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_CLICK && done.request.id != 0 &&
                    state.login_click_logs < 12) {
                    ++state.login_click_logs;
                    WinLogf("login click result: status=%u outcome=0x%x reasons=0x%x detail=%s",
                            done.status, done.outcome, done.reasons,
                            done.detail[0] != '\0' ? done.detail : "-");
                }
                // 只点宿主判定为 CLICKABLE 的登录按钮，不做强制点击。
                // 登录页加载/淡入期间按钮可能已经可点、但点击会被游戏丢掉（画面还在转场），
                // 所以不设「只点 8 次」的死上限：只要还没进游戏就一秒一轮地补点（最多 60 次），
                // 一旦 HUD 就绪这一步就不再执行，不会在进游戏后重复弹登录。
                if (state.login_tries < 60 && NudgeLoginButton()) {
                    ++state.login_tries;
                    if (!state.login_clicked) {
                        state.login_clicked = true;
                        WinLog("login button clicked to enter the game");
                    }
                    if (state.login_tries <= 3 || state.login_tries % 10 == 0) {
                        WinLogf("login click attempt %d", state.login_tries);
                    }
                    state.login_wait = 20;
                } else {
                    if (state.login_tries >= 60 && state.login_blocked_logs < 7) {
                        WinLog("login button no longer pressed: waiting for the in-game widgets");
                        ++state.login_blocked_logs;
                    }
                    state.login_wait = 30;
                }
                return;
            }
            if (!state.login_clicked) {
                state.login_clicked = true;
                WinLog("entered the game (hud ready)");
            }
            // 静置计时从「真的在游戏里」开始算。
            state.world_ready_timer += delta_seconds;
            if (state.world_ready_timer <
                static_cast<double>(state.settings.world_settle_seconds)) {
                return;
            }
            if (state.single_target != Stage::Idle) {
                // 单阶段运行：直接去用户点的那一站。
                const Stage target = state.single_target;
                Enter(target, StageKey(target), StageFallback(target));
                return;
            }
            EnterNext(Stage::WaitWorld);
            return;
        }
        case Stage::Dungeon: {
            if (!state.stage_entered) {
                // 兜底：还没真正进游戏就不发副本请求，退回等世界那一站继续等。
                if (LoginPageVisible() || !HudReady()) {
                    WinLog("dungeon stage deferred: not in game yet");
                    Enter(Stage::WaitWorld, StageKey(Stage::WaitWorld),
                          StageFallback(Stage::WaitWorld));
                    return;
                }
                state.stage_entered = true;
                state.dungeon_phase = 0;  // 先准备副本插件
                state.dungeon_prepare_phase = 0;
                state.dungeon_prepare_timer = 0.0;
                state.dungeon_probe_timer = 0.0;
                state.dungeon_poll_timer = 0.0;
                state.dungeon_polls = 0;
                state.dungeon_log_phase = -1;
                state.dungeon_log_runs = 0;
                SetNote("stage.dungeon_preparing", "Preparing the dungeon plugin");
                WinLog("dungeon stage: checking the dungeon plugin");
                return;
            }
            if (state.dungeon_phase == 0) {
                const PrepareOutcome outcome = TickCloneEnterPrepare(delta_seconds);
                if (outcome == PrepareOutcome::Waiting) return;
                if (outcome == PrepareOutcome::GiveUp) {
                    // 统一在这里把可能被打开的副本插件关回去，再跳过副本阶段。
                    CloseCloneEnter();
                    EnterNext(Stage::Dungeon);
                    return;
                }
                if (!DungeonStart(state.settings)) {
                    WinLog("dungeon stage failed: start request rejected");
                    SetNote("stage.dungeon_rejected", "The dungeon start request was rejected");
                    CloseCloneEnter();
                    ChainStop(StopCause::Manual, "stage.dungeon_rejected",
                              "The dungeon start request was rejected");
                    return;
                }
                state.dungeon_phase = 1;  // 副本进行中
                state.dungeon_poll_timer = 0.0;
                SetNote("stage.dungeon_running", "Dungeon running");
                return;
            }
            state.dungeon_poll_timer += delta_seconds;
            if (state.dungeon_poll_timer >= kDungeonPollSeconds) {
                state.dungeon_poll_timer = 0.0;
                static_cast<void>(DungeonStatusRequest());
            }
            DungeonStatus status{};
            if (DungeonTakeStatus(status)) {
                ++state.dungeon_polls;
                // 只在「轮次或阶段变了」时打一行，避免每秒一行刷屏。
                if (status.runs_total > 0 &&
                    (status.phase != state.dungeon_log_phase ||
                     status.runs_done != state.dungeon_log_runs)) {
                    state.dungeon_log_phase = status.phase;
                    state.dungeon_log_runs = status.runs_done;
                    WinLogf("dungeon progress %u/%u phase=%d active=%d result=%d reason=%s",
                            status.runs_done, status.runs_total, status.phase,
                            status.active ? 1 : 0, status.result, status.reason.c_str());
                }
                if (status.result == static_cast<int>(DungeonResult::Complete)) {
                    WinLog("dungeon stage finished");
                    SetNote("stage.dungeon_done", "Dungeon finished");
                    // 打完就把副本插件关掉（用户要求：只在打副本时启用）。
                    CloseCloneEnter();
                    EnterNext(Stage::Dungeon);
                    return;
                }
                if (status.result == static_cast<int>(DungeonResult::Aborted)) {
                    WinLogf("dungeon stage aborted by the dungeon plugin (reason=%s)",
                            status.reason.c_str());
                    CloseCloneEnter();
                    ChainStop(StopCause::Manual, "stage.dungeon_aborted",
                              "The dungeon plugin aborted the run");
                    return;
                }
            }
            if (StageTimedOut()) {
                static_cast<void>(DungeonStop());
                CloseCloneEnter();
                ChainStop(StopCause::Manual, "stage.dungeon_timeout", "The dungeon stage timed out");
            }
            return;
        }
        case Stage::Cafe: {
            if (!state.stage_entered) {
                state.stage_entered = true;
                StartFonsChain();
                return;
            }
            TickFonsChain();
            if (FonsChainBusy()) return;
            // 一条龙顺序固定：副本 -> 一咖舍 -> 每日活跃奖励 -> 环期任务奖励。
            WinLog("cafe stage finished");
            SetNote("stage.cafe_done", "One-shop withdrawal finished");
            EnterNext(Stage::Cafe);
            return;
        }
        case Stage::DailyRewards: {
            if (!state.stage_entered) {
                state.stage_entered = true;
                // 反射先领探索指南那一批，再走控件链切「全部」页：
                // 先领下方单独活跃任务（活跃度到账），再点上方「全部奖励」。
                static_cast<void>(ClaimDaily(-1));
                StartRewardChain(RewardGroup::Daily);
                return;
            }
            TickRewardChain();
            if (RewardChainBusy()) {
                if (StageTimedOut()) {
                    AbortRewardChain();
                    ChainStop(StopCause::Manual, "stage.rewards_timeout",
                              "The daily reward stage timed out");
                }
                return;
            }
            WinLog("daily reward stage finished");
            SetNote("stage.daily_rewards_done", "Daily rewards finished");
            EnterNext(Stage::DailyRewards);
            return;
        }
        case Stage::PeriodicRewards: {
            if (!state.stage_entered) {
                state.stage_entered = true;
                StartRewardChain(RewardGroup::Periodic);
                return;
            }
            TickRewardChain();
            if (RewardChainBusy()) {
                if (StageTimedOut()) {
                    AbortRewardChain();
                    ChainStop(StopCause::Manual, "stage.periodic_timeout",
                              "The periodic quest stage timed out");
                }
                return;
            }
            WinLog("periodic reward stage finished");
            SetNote("stage.periodic_rewards_done", "Periodic quest rewards finished");
            EnterNext(Stage::PeriodicRewards);
            return;
        }
        case Stage::Done: {
            WinLog("one-stop chain finished");
            ChainStop(StopCause::None, "stage.finished", "One-stop chain finished");
            SetStatusText(TextLocalized("status.chain_done", "One-stop chain finished."));
            return;
        }
        case Stage::Idle:
        default:
            return;
    }
}

}  // namespace daily
