#pragma once
// 「按钮刷新」：通过 anomaly.nte.ui-buttons 服务点游戏自己的界面按钮，不碰内存。
// 流程：InteractButton（有几个点几个）→ BtnResetMonsterSpawners → BtnSure（可选）→ BtnReset。
// 判定：点完后按钮消失 / 隐藏 / 变不可点即生效，否则重试，最多 3 次；BtnReset 成功后停止扫描。
// 面板里拿不到可点的刷新按钮：点 BtnClose 退出，等 10 秒再点交互按钮，最多重试 6 次。
// 只在 Game 回调域推进；服务是异步的，请求句柄留到下一 tick 用 request_snapshot 轮询。

namespace {

constexpr char kButtonInteract[] = "InteractButton";
constexpr char kButtonResetSpawners[] = "BtnResetMonsterSpawners";
constexpr char kButtonReset[] = "BtnReset";
// 点完 BtnResetMonsterSpawners 后可能弹出的确认按钮。
constexpr char kButtonSure[] = "BtnSure";
// 面板里拿不到刷新按钮时用它退出去；玩家给的名字里 l/I 不分，两种写法都试。
constexpr char kButtonClose[] = "BtnClose";
constexpr char kButtonCloseAlt[] = "BtnCIose";

constexpr std::chrono::milliseconds kButtonStepInterval{200};
// 首发 + 重试 2 次。
constexpr int kButtonMaxClicks = 3;
// 点完到验证的间隔：游戏侧有防连点，太急会收到 did not arm。
constexpr std::chrono::milliseconds kButtonClickGuard{400};
// 等按钮出现的扫描轮数（一轮约 200ms + 扫描耗时）。
constexpr int kButtonWaitScans = 15;
constexpr int kButtonInteractWait = 6;
constexpr int kButtonSureWait = 10;
constexpr int kButtonResetWait = 5;
// 关闭面板后的等待，以及一次刷新最多重开几次。
constexpr std::chrono::seconds kButtonCloseWait{10};
constexpr int kButtonCampfireRetries = 6;
// 一次刷新最多点几个 InteractButton，防止同名按钮一直挂着时无限点。
constexpr std::size_t kButtonInteractLimit = 8;
constexpr std::chrono::seconds kButtonFlowTimeout{150};

// 刷新方式：0 = 内存刷新、1 = 按钮刷新。界面线程写、Game 线程读。
std::atomic_int g_refresh_mode{0};

enum class ButtonStage : int {
    idle = 0,
    interact = 1,
    reset_spawners = 2,
    // 确认按钮是可选的：出现才点，不出现就直接去点 BtnReset。
    sure = 3,
    reset = 4,
    // 面板里没有刷新按钮：点关闭退出，等 10 秒再重新点交互按钮。
    close = 5,
    done = 6,
};

enum class ButtonPhase : int {
    scan = 0,
    decide = 1,
    click = 2,
    // 点完等一个间隔再重扫：界面刷新有延迟，立刻扫会看到同一个按钮还能点。
    verify_wait = 3,
    // 关闭面板后等 10 秒，再回去点交互按钮。
    reopen_wait = 4,
};

struct ButtonState {
    ButtonStage stage{ButtonStage::idle};
    ButtonPhase phase{ButtonPhase::scan};
    // 这一轮扫描是在确认上一次点击是否生效。
    bool verifying{};
    bool has_scan{};
    bool has_click{};
    // 本阶段已点击次数（含首发）与已点掉的 InteractButton 个数。
    int clicks{};
    std::size_t interact_clicks{};
    // 已等待的扫描轮数、已重开营火的次数、重开时刻。
    int waits{};
    int campfire_retries{};
    std::chrono::steady_clock::time_point reopen_at{};
    // 点击前按钮是否本来可点：只有这种情况才能把「变成不可点」当成点击生效。
    bool clicked_when_clickable{};
    std::chrono::steady_clock::time_point next_at{};
    std::chrono::steady_clock::time_point deadline{};
    AnomalyGenerationHandleV1 scan_request{};
    AnomalyGenerationHandleV1 click_request{};
    // 扫描请求发出的时刻，用于提示慢扫描（只提示一次）。
    std::chrono::steady_clock::time_point scan_at{};
    bool scan_warned{};
    std::string target;
    std::string detail;
};

ButtonState g_button;

std::string ButtonStatusText(const AnomalyStatusV1& status) {
    if (status.message.data == nullptr || status.message.size == 0) return {};
    return std::string(status.message.data, status.message.size);
}

const char* ButtonName(const ButtonStage stage) {
    switch (stage) {
    case ButtonStage::interact: return kButtonInteract;
    case ButtonStage::reset_spawners: return kButtonResetSpawners;
    case ButtonStage::sure: return kButtonSure;
    case ButtonStage::close: return kButtonClose;
    case ButtonStage::reset: return kButtonReset;
    default: return "";
    }
}

bool ButtonServiceReady(const AnomalyNteUiButtonsServiceV1* buttons) {
    return buttons != nullptr && buttons->status != nullptr && buttons->button_at != nullptr &&
        buttons->find != nullptr && buttons->request_scan != nullptr &&
        buttons->request_click != nullptr && buttons->request_snapshot != nullptr;
}

// 流程出口：写回脚本循环用的 handoff；done 必须在这里置位（按钮流程没有 UeAdvance 的阶段推进）。
void ButtonFinish(const UeRefreshOutcome outcome, const UeRefreshFailure failure,
                  const std::string& detail) {
    g_button.stage = ButtonStage::done;
    if (outcome == UeRefreshOutcome::failed) {
        UeFailRefresh(failure, "buttons: " + detail);
    } else {
        UeFinishRefresh(outcome, "buttons: " + detail);
    }
    g_ue_refresh_done.store(1);
}

void ButtonEnterStage(const ButtonStage stage, const std::chrono::steady_clock::time_point now) {
    g_button.stage = stage;
    g_button.phase = ButtonPhase::scan;
    g_button.verifying = false;
    g_button.has_scan = false;
    g_button.has_click = false;
    g_button.clicks = 0;
    g_button.waits = 0;
    g_button.target = ButtonName(stage);
    g_button.next_at = now;
    UeLog("refresh: buttons stage " + g_button.target);
}

bool ButtonRequestScan(const AnomalyNteUiButtonsServiceV1* buttons,
                       const std::chrono::steady_clock::time_point now) {
    AnomalyGenerationHandleV1 request{};
    const AnomalyStatusV1 status = buttons->request_scan(buttons->user, &request);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        g_button.detail = ButtonStatusText(status);
        return false;
    }
    g_button.scan_request = request;
    g_button.has_scan = true;
    g_button.phase = ButtonPhase::scan;
    g_button.scan_at = now;
    g_button.scan_warned = false;
    g_button.next_at = now;
    return true;
}

bool ButtonRequestClick(const AnomalyNteUiButtonsServiceV1* buttons, const std::string& name,
                        const AnomalyGenerationHandleV1 button, const bool force) {
    AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
    click.button = button;
    // 被遮挡的按钮会被服务以 CONFLICT 拒绝，只有 FORCE 能绕过；变灰（冷却中）点了也没用，
    // 所以只有「不处于 CLICKABLE 类」时才带这个标志。
    click.flags = force ? ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE : 0u;
    AnomalyGenerationHandleV1 request{};
    const AnomalyStatusV1 status = buttons->request_click(buttons->user, &click, &request);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        g_button.detail = ButtonStatusText(status);
        return false;
    }
    g_button.click_request = request;
    g_button.has_click = true;
    g_button.has_scan = false;
    g_button.phase = ButtonPhase::click;
    // 点击请求入队后就开始计时：处理完再重扫，保证两次动作之间至少一个间隔。
    g_button.next_at = std::chrono::steady_clock::now() + kButtonClickGuard;
    UeLog("refresh: buttons clicked " + name);
    return true;
}

// 0 = 还在跑、1 = 成功、2 = 失败。
int ButtonPoll(const AnomalyNteUiButtonsServiceV1* buttons,
               const AnomalyGenerationHandleV1 request) {
    AnomalyNteUiButtonRequestSnapshotV1 snapshot{sizeof(snapshot)};
    const AnomalyStatusV1 status = buttons->request_snapshot(buttons->user, request, &snapshot);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        g_button.detail = ButtonStatusText(status);
        return 2;
    }
    if (snapshot.state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) return 0;
    g_button.detail = snapshot.detail;
    return snapshot.status == ANOMALY_STATUS_V1_OK ? 1 : 2;
}

// 按钮此刻能不能点。服务分 CLICKABLE / BLOCKED / HIDDEN 三类，这里按名字查任意类别：
// 只查 CLICKABLE 会把「存在但点不动」误判成「没有这个按钮」。
struct ButtonProbe {
    bool present{};
    bool clickable{};
    bool hidden{};
    std::uint32_t category{};
    std::uint32_t reasons{};
    AnomalyGenerationHandleV1 handle{};
};

std::string ButtonProbeText(const ButtonProbe& probe) {
    if (!probe.present) return " (absent)";
    return " (category " + std::to_string(probe.category) + ", reasons " +
        std::to_string(probe.reasons) + ")";
}

// 目录没发布前 find 一律返回 NOT_FOUND，别把它当成「按钮不存在」。
bool ButtonCatalogReady(const AnomalyNteUiButtonsServiceV1* buttons) {
    AnomalyNteUiButtonsStatusV1 catalog{sizeof(catalog)};
    if (buttons->status(buttons->user, &catalog).code != ANOMALY_STATUS_V1_OK) return false;
    return (catalog.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) != 0;
}

// false = 查询本身失败（服务错误），true = 查询完成，结果放在 probe 里。
bool ButtonProbeTarget(const AnomalyNteUiButtonsServiceV1* buttons, const std::string& name,
                       ButtonProbe& probe) {
    probe = ButtonProbe{};
    AnomalyNteUiButtonQueryV1 query{sizeof(query)};
    query.category_mask = 0;  // 0 = 任意类别
    query.name = StringView(std::string_view(name));
    AnomalyNteUiButtonSnapshotV1 first{sizeof(first)};
    std::uint32_t matches = 0;
    const AnomalyStatusV1 status = buttons->find(buttons->user, &query, &first, &matches);
    if (status.code == ANOMALY_STATUS_V1_NOT_FOUND || matches == 0) return true;
    if (status.code != ANOMALY_STATUS_V1_OK) {
        g_button.detail = ButtonStatusText(status);
        return false;
    }
    if (first.button.id == 0) return true;
    probe.present = true;
    probe.category = first.category;
    probe.reasons = first.reasons;
    probe.handle = first.button;
    probe.hidden = first.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN;
    probe.clickable = first.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE;
    return true;
}

// 目标按钮找不到时把目录里现成的按钮打出来：名字对不上时靠这一行排查。
void ButtonDumpCatalog(const AnomalyNteUiButtonsServiceV1* buttons, const std::string& missing) {
    AnomalyNteUiButtonsStatusV1 catalog{sizeof(catalog)};
    if (buttons->status(buttons->user, &catalog).code != ANOMALY_STATUS_V1_OK) return;
    UeLog("refresh: buttons catalog #" + std::to_string(catalog.catalog_sequence) +
          " total=" + std::to_string(catalog.button_count) +
          " clickable=" + std::to_string(catalog.clickable_count) +
          " blocked=" + std::to_string(catalog.blocked_count) +
          " hidden=" + std::to_string(catalog.hidden_count) + ", no " + missing);
    AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
    std::uint32_t shown = 0;
    for (std::uint32_t index = 0; index != catalog.button_count && shown != 12; ++index) {
        if (buttons->button_at(buttons->user, catalog.catalog_sequence, index, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            return;
        }
        if (button.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN) continue;
        std::string line = std::string("refresh: buttons   ") + button.name + " [" +
            std::to_string(button.category) + "]";
        if (button.text[0] != '\0') line += std::string(" \"") + button.text + "\"";
        if (button.window[0] != '\0') line += std::string(" in ") + button.window;
        UeLog(line);
        ++shown;
    }
}

// 拿不到可点的刷新按钮：关面板、等 10 秒重来，超过上限就报错收尾。
bool ButtonCampfireRetry(const AnomalyNteUiButtonsServiceV1* buttons,
                         const std::chrono::steady_clock::time_point now,
                         const std::string& reason) {
    if (g_button.campfire_retries >= kButtonCampfireRetries) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::no_reset_spawners,
                     reason + " after " + std::to_string(kButtonCampfireRetries) +
                         " campfire retries");
        return true;
    }
    ++g_button.campfire_retries;
    UeLog("refresh: buttons " + reason + ", closing the campfire (retry " +
          std::to_string(g_button.campfire_retries) + "/" +
          std::to_string(kButtonCampfireRetries) + ")");
    ButtonEnterStage(ButtonStage::close, now);
    if (!ButtonRequestScan(buttons, now)) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                     "cannot scan the button catalog: " + g_button.detail);
    }
    return true;
}

void ButtonNextStage(const AnomalyNteUiButtonsServiceV1* buttons,
                     const std::chrono::steady_clock::time_point now) {
    switch (g_button.stage) {
    case ButtonStage::interact:
        ButtonEnterStage(ButtonStage::reset_spawners, now);
        break;
    case ButtonStage::reset_spawners:
        ButtonEnterStage(ButtonStage::sure, now);
        break;
    case ButtonStage::sure:
        ButtonEnterStage(ButtonStage::reset, now);
        break;
    case ButtonStage::close:
        // 面板已经关掉（或者本来就没有关闭按钮）：等 10 秒再重新点交互按钮。
        g_button.phase = ButtonPhase::reopen_wait;
        g_button.reopen_at = now + kButtonCloseWait;
        g_button.next_at = g_button.reopen_at;
        UeLog("refresh: buttons campfire closed, waiting 10s before retrying");
        return;
    case ButtonStage::reset:
        // BtnReset 点成功：刷新已经交回游戏，停止扫描。
        ButtonFinish(UeRefreshOutcome::ok, UeRefreshFailure::none, "the reset button was pressed");
        return;
    default:
        return;
    }
    if (!ButtonRequestScan(buttons, now)) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                     "cannot scan the button catalog: " + g_button.detail);
    }
}

// 脚本进入刷新步骤时调用（Game 线程）。
void ButtonRefreshBegin(const AnomalyNteUiButtonsServiceV1* buttons,
                        const std::chrono::steady_clock::time_point now) {
    g_button = ButtonState{};
    g_button.deadline = now + kButtonFlowTimeout;
    ButtonEnterStage(ButtonStage::interact, now);
    UeLog("refresh: buttons started");
    // 第一阶段的扫描要在这里就发出去：ButtonEnterStage 只改状态，不会自己排请求。
    if (buttons == nullptr) return;  // 没有服务：Advance 会在同一 tick 报 button_service。
    if (!ButtonRequestScan(buttons, now)) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                     "cannot scan the button catalog: " + g_button.detail);
    }
}

// 脚本离开刷新步骤或停止插件时调用。
void ButtonRefreshStop() {
    g_button = ButtonState{};
}

bool ButtonRefreshActive() {
    return g_button.stage != ButtonStage::idle && g_button.stage != ButtonStage::done;
}

// 每 tick 推进一次（Game 线程）。
void ButtonRefreshAdvance(const AnomalyNteUiButtonsServiceV1* buttons) {
    if (!ButtonRefreshActive()) return;

    const auto now = std::chrono::steady_clock::now();
    if (now >= g_button.deadline) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::timeout,
                     "the button flow did not finish in 150s");
        return;
    }
    if (g_button.phase == ButtonPhase::reopen_wait) {
        if (now < g_button.reopen_at) return;
        UeLog("refresh: buttons reopening the campfire, retry " +
              std::to_string(g_button.campfire_retries) + "/" +
              std::to_string(kButtonCampfireRetries));
        ButtonEnterStage(ButtonStage::interact, now);
        if (!ButtonRequestScan(buttons, now)) {
            ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                         "cannot scan the button catalog: " + g_button.detail);
        }
        return;
    }

    if (!ButtonServiceReady(buttons)) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                     "the ui-buttons service is unavailable");
        return;
    }

    // 1. 等扫描结果。
    if (g_button.has_scan) {
        const int result = ButtonPoll(buttons, g_button.scan_request);
        if (result == 0) {
            // 扫描一直不完成是最容易被当成「按钮点不了」的故障：留一行证据。
            if (!g_button.scan_warned && now - g_button.scan_at > std::chrono::seconds(5)) {
                g_button.scan_warned = true;
                UeLog("refresh: buttons scan still running after 5s");
            }
            return;
        }
        g_button.has_scan = false;
        if (result == 2) {
            ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                         "button scan failed: " + g_button.detail);
            return;
        }
        g_button.phase = ButtonPhase::decide;
        // 与上一次点击之间留出 200ms，避免连着点。
        if (g_button.next_at < now) g_button.next_at = now + kButtonStepInterval;
    }

    // 2. 等点击结果；处理完隔一个间隔再重扫，用「按钮还在不在」判断这次点击是否生效。
    if (g_button.has_click) {
        const int result = ButtonPoll(buttons, g_button.click_request);
        if (result == 0) return;
        g_button.has_click = false;
        if (result == 2) {
            UeLog("refresh: buttons click request failed: " + g_button.detail);
            // did not arm（点击间隔 / 不可交互）时这一下根本没按下去，别算进重试次数。
            if (g_button.clicks > 0) --g_button.clicks;
        }
        g_button.verifying = true;
        g_button.phase = ButtonPhase::verify_wait;
        return;
    }

    // 3. 点击后等界面稳定，再扫一次确认按钮状态。
    if (g_button.phase == ButtonPhase::verify_wait) {
        if (now < g_button.next_at) return;
        if (!ButtonRequestScan(buttons, now)) {
            ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                         "cannot scan the button catalog: " + g_button.detail);
        }
        return;
    }

    // 4. 该决定了：看得见就点，看不见就等或收尾。
    if (g_button.phase != ButtonPhase::decide) return;
    if (now < g_button.next_at) return;

    if (!ButtonCatalogReady(buttons)) {
        // 目录还没发布（第一次扫描要跑好几个 tick），重扫一次，不算进等待预算。
        if (!ButtonRequestScan(buttons, now)) {
            ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                         "cannot scan the button catalog: " + g_button.detail);
        }
        return;
    }

    ButtonProbe probe;
    if (!ButtonProbeTarget(buttons, g_button.target, probe)) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                     "cannot query the button catalog: " + g_button.detail);
        return;
    }
    if (g_button.stage == ButtonStage::close && !probe.present &&
        g_button.target != kButtonCloseAlt) {
        // 名字里的 l / I 在游戏资源里不一定是我们以为的那个，两种都认。
        ButtonProbe alternative;
        if (!ButtonProbeTarget(buttons, kButtonCloseAlt, alternative)) {
            ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                         "cannot query the button catalog: " + g_button.detail);
            return;
        }
        if (alternative.present) {
            probe = alternative;
            g_button.target = kButtonCloseAlt;
        }
    }
    const bool verifying = g_button.verifying;
    g_button.verifying = false;
    // 隐藏视同不在；被遮挡 / 置灰仍算「在」，点击要带 FORCE。
    const bool active = probe.present && !probe.hidden;
    // 点了生效 = 按钮消失、隐藏或变成不可点（弹确认框时常见：按钮只是被盖住）。
    const bool click_landed =
        !active || (!probe.clickable && g_button.clicked_when_clickable);

    // 刷新按钮置灰：面板状态不对，直接重开，别在这里空点。
    if (g_button.stage == ButtonStage::reset_spawners && active && !verifying &&
        !probe.clickable &&
        (probe.reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED) != 0) {
        ButtonCampfireRetry(buttons, now, "BtnResetMonsterSpawners is not clickable");
        return;
    }

    if (verifying && click_landed) {
        UeLog("refresh: buttons click ok on " + g_button.target + ButtonProbeText(probe));
        g_button.clicks = 0;
        if (g_button.stage == ButtonStage::interact) {
            ++g_button.interact_clicks;
            if (g_button.interact_clicks >= kButtonInteractLimit) {
                UeLog("refresh: buttons interact limit reached");
                ButtonNextStage(buttons, now);
                return;
            }
            // 可能还挂着同名按钮：再扫一轮，直到一个都不剩。
            if (!ButtonRequestScan(buttons, now)) {
                ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                             "cannot scan the button catalog: " + g_button.detail);
            }
            return;
        }
        ButtonNextStage(buttons, now);
        return;
    }

    if (active) {
        if (verifying) {
            UeLog("refresh: buttons click had no effect on " + g_button.target +
                  ButtonProbeText(probe));
        }
        // 置灰多半是冷却没到：先等，别把点击机会浪费在点不动的按钮上。
        if (g_button.stage != ButtonStage::reset_spawners && !probe.clickable &&
            (probe.reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED) != 0 &&
            g_button.waits < kButtonWaitScans) {
            ++g_button.waits;
            if (!ButtonRequestScan(buttons, now)) {
                ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                             "cannot scan the button catalog: " + g_button.detail);
            }
            return;
        }
        if (g_button.clicks >= kButtonMaxClicks) {
            if (g_button.stage == ButtonStage::interact) {
                // 交互按钮点不掉也无所谓，继续点营火按钮。
                UeLog("refresh: buttons giving up on " + g_button.target);
                ButtonNextStage(buttons, now);
                return;
            }
            ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_refused,
                         g_button.target + " is still there after 3 clicks" +
                             ButtonProbeText(probe));
            return;
        }
        ++g_button.clicks;
        g_button.waits = 0;
        g_button.clicked_when_clickable = probe.clickable;
        // 确实可点时不带 FORCE；遮挡 / 置灰必须带，否则服务直接以 CONFLICT 拒绝。
        if (!ButtonRequestClick(buttons, g_button.target, probe.handle, !probe.clickable)) {
            ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_refused,
                         "cannot click " + g_button.target + ": " + g_button.detail);
        }
        return;
    }

    if (g_button.stage == ButtonStage::interact) {
        if (g_button.interact_clicks >= kButtonInteractLimit) {
            UeLog("refresh: buttons interact limit reached");
            ButtonNextStage(buttons, now);
            return;
        }
        // 点掉过交互按钮，剩下的都点完了：进营火按钮。
        if (g_button.interact_clicks > 0) {
            ButtonNextStage(buttons, now);
            return;
        }
        // 一个都没看到：面板可能还没弹出来，等几轮再放行。
        ++g_button.waits;
        if (g_button.waits == 1) ButtonDumpCatalog(buttons, g_button.target);
        if (g_button.waits <= kButtonInteractWait) {
            if (!ButtonRequestScan(buttons, now)) {
                ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                             "cannot scan the button catalog: " + g_button.detail);
            }
            return;
        }
        UeLog("refresh: buttons no InteractButton, going for the campfire buttons");
        ButtonNextStage(buttons, now);
        return;
    }

    if (g_button.stage == ButtonStage::reset_spawners &&
        g_button.waits > kButtonResetWait) {
        ButtonCampfireRetry(buttons, now, "no BtnResetMonsterSpawners");
        return;
    }

    // 目标按钮还没出现：再扫一轮，最多等 kButtonWaitScans 轮。
    ++g_button.waits;
    if (g_button.stage == ButtonStage::close) {
        // 关不掉也照样等 10 秒重来。
        UeLog("refresh: buttons no BtnClose, waiting anyway");
        ButtonNextStage(buttons, now);
        return;
    }
    if (g_button.stage == ButtonStage::sure) {
        // 确认框可选：没弹就直接去点 BtnReset。
        if (g_button.waits > kButtonSureWait) {
            UeLog("refresh: buttons no BtnSure, going for the reset button");
            ButtonNextStage(buttons, now);
            return;
        }
    } else if (g_button.waits == 1) {
        ButtonDumpCatalog(buttons, g_button.target);
    }
    if (g_button.stage != ButtonStage::sure && g_button.waits > kButtonWaitScans) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::no_reset_button,
                     g_button.target + " never appeared");
        return;
    }
    if (!ButtonRequestScan(buttons, now)) {
        ButtonFinish(UeRefreshOutcome::failed, UeRefreshFailure::button_service,
                     "cannot scan the button catalog: " + g_button.detail);
    }
}

}  // namespace
