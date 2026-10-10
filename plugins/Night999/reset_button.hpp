#pragma once
// 刷新怪物：通过 anomaly.nte.ui-buttons 服务点击游戏内「刷新」按钮。
//
// 服务是异步的：request_scan / request_click 只排队并返回请求 handle，Host 在之后的 Game
// tick 里按时间片执行，请求走 QUEUED → RUNNING → COMPLETE，完成后的 status 就是结果。
// 按钮有冷却，冷却期间按钮被禁用；宿主在点击前会现读界面层重新判定可点击性，不可点击时
// 请求以 CONFLICT 完成并给出 reasons，所以「禁用就等待」只需要按周期重试，不需要自己猜 CD。
//
// 本头文件依赖 plugin.cpp 中先定义好的全局状态与辅助函数，必须在它们之后包含。

namespace {

// kResetButtonDefault / kResetButtonNameCapacity 由 plugin.cpp 定义（设置文件布局要用到）。

// 按钮冷却中 / 未找到按钮 / 句柄失效时的重试间隔。
constexpr std::chrono::milliseconds kResetAttemptInterval{600};
// 目录刷新间隔：营火界面随时可能开关，目录按周期重建。
constexpr std::chrono::milliseconds kResetScanInterval{700};
// 单个点击请求的最长等待，超时按冷却处理并重试。
constexpr std::chrono::milliseconds kResetClickTimeout{2500};
// 整轮刷新的最长等待。
constexpr std::chrono::milliseconds kResetDeadline{60000};

struct ResetReasonEntry {
    std::uint32_t bit;
    const char* name;
};

// 冷却的判定：按钮被禁用 / 不可交互 / 锁定，都属于「还没冷却完，等着」。
constexpr std::uint32_t kResetCooldownReasons =
    ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED |
    ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_INTERACTABLE |
    ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED;

std::string ResetReasons(const std::uint32_t reasons) {
    static constexpr std::array<ResetReasonEntry, 13> kEntries{{
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED, "禁用"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED, "锁定"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_INTERACTABLE, "不可交互"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_HIT_TESTABLE, "不接受点击"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED, "被其他界面遮挡"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE, "非当前页"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_CLOSING, "界面关闭中"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_QUERY_FAILED, "查询失败"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_SELF, "自身隐藏"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_ANCESTOR, "父控件隐藏"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_TRANSPARENT, "完全透明"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_IN_VIEWPORT, "界面未显示"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED, "未挂到界面"},
    }};
    std::string text;
    for (const auto& entry : kEntries) {
        if ((reasons & entry.bit) == 0) continue;
        if (!text.empty()) text += " / ";
        text += entry.name;
    }
    return text.empty() ? std::string("未知原因") : text;
}

// 刷新流程状态（on_update / Game 回调域独占；两个 handoff 标志由 on_draw 置位）。
struct ResetRuntime {
    std::atomic_bool start_requested{};
    std::atomic_bool cancel_requested{};
    bool running{};
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::steady_clock::time_point next_scan{};
    std::chrono::steady_clock::time_point next_attempt{};
    std::chrono::steady_clock::time_point click_sent{};
    std::uint64_t catalog_sequence{};
    bool scan_pending{};
    AnomalyGenerationHandleV1 scan_request{};
    AnomalyGenerationHandleV1 click_request{};
    std::uint32_t presses{};
};

ResetRuntime g_reset;
std::mutex g_reset_mutex;
std::string g_reset_button_name{kResetButtonDefault};
std::string g_reset_status{"未开始"};
std::atomic_bool g_reset_busy{false};
std::atomic_bool g_reset_succeeded{false};
std::array<char, kResetButtonNameCapacity> g_reset_name_text{};
std::atomic_bool g_reset_text_stale{true};

const AnomalyNteUiButtonsServiceV1* ResetService(const AnomalyHostApiV1* host) noexcept {
    if (host == nullptr) return nullptr;
    return anomaly::sdk::Host(host)
        .Query<AnomalyNteUiButtonsServiceV1>(
            ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
            ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION)
        .get();
}

bool ResetServiceUsable(const AnomalyNteUiButtonsServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION &&
        service->find != nullptr && service->request_scan != nullptr &&
        service->request_click != nullptr && service->request_snapshot != nullptr;
}

void ResetPublish(std::string text) {
    std::scoped_lock lock(g_reset_mutex);
    g_reset_status = std::move(text);
}

std::string ResetStatus() {
    std::scoped_lock lock(g_reset_mutex);
    return g_reset_status;
}

std::string ResetButtonName() {
    std::scoped_lock lock(g_reset_mutex);
    return g_reset_button_name;
}

void ResetSetButtonName(const std::string& name) {
    std::scoped_lock lock(g_reset_mutex);
    if (g_reset_button_name == name) return;
    g_reset_button_name = name;
    g_settings_dirty.store(true);
}

void ResetFinish(std::string text, const bool succeeded) {
    g_reset.running = false;
    g_reset.scan_pending = false;
    g_reset.scan_request = {};
    g_reset.click_request = {};
    g_reset.catalog_sequence = 0;
    g_reset.next_scan = {};
    g_reset.next_attempt = {};
    g_reset.click_sent = {};
    g_reset_busy.store(false);
    g_reset_succeeded.store(succeeded);
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 reset: " + text);
    ResetPublish(std::move(text));
}

void ResetBegin(const AnomalyHostApiV1* host) {
    const auto* service = ResetService(host);
    if (!ResetServiceUsable(service)) {
        ResetFinish("UI 按钮服务不可用：请确认 Profile 已启用 nte-ui-buttons", false);
        return;
    }
    g_reset.running = true;
    g_reset.deadline = std::chrono::steady_clock::now() + kResetDeadline;
    g_reset.next_scan = {};
    g_reset.next_attempt = {};
    g_reset.click_sent = {};
    g_reset.catalog_sequence = 0;
    g_reset.scan_pending = false;
    g_reset.scan_request = {};
    g_reset.click_request = {};
    g_reset_busy.store(true);
    g_reset_succeeded.store(false);
    ResetPublish("开始刷新怪物…");
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 reset: begin");
}

std::string ResetDetail(const char* detail) {
    return detail != nullptr && detail[0] != '\0' ? std::string("（") + detail + "）"
                                                  : std::string();
}

std::string ResetLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z'
                ? character - 'A' + 'a'
                : character);
    });
    return value;
}

// 目录里任一可见字段包含关键词就算命中：控件对象名可能带 `_1` 这类去重后缀，而 find 只做
// 精确匹配；用户从面板复制的标识也常常比裸控件名长。
bool ResetSnapshotContains(const std::string& needle, const AnomalyNteUiButtonSnapshotV1& button) {
    const char* const fields[]{
        button.name, button.text, button.window, button.owner,
        button.root, button.class_name, button.path,
    };
    for (const char* field : fields) {
        if (field == nullptr || field[0] == '\0') continue;
        if (ResetLower(field).find(needle) != std::string::npos) return true;
    }
    return false;
}

// 先用 find 精确匹配（命中即返回，开销最小）；落空再枚举目录做包含匹配。
// 目录顺序本身就是 可点击 → 不可点击 → 隐藏，所以第一个命中就是最优候选。
bool ResetFindButton(
    const AnomalyNteUiButtonsServiceV1* service, const std::string& name,
    AnomalyNteUiButtonSnapshotV1& found, std::string& how) {
    AnomalyNteUiButtonQueryV1 query{sizeof(query)};
    query.catalog_sequence = 0;
    query.name = StringView(name);
    AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
    std::uint32_t matches{};
    if (service->find(service->user, &query, &button, &matches).code == ANOMALY_STATUS_V1_OK &&
        button.button.id != 0) {
        found = button;
        how = "名称精确匹配";
        return true;
    }
    if (service->status == nullptr || service->button_at == nullptr) return false;

    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK ||
        (status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) == 0 ||
        status.catalog_sequence == 0 || status.button_count == 0) {
        return false;
    }
    const std::string needle = ResetLower(name);
    for (std::uint32_t index = 0; index < status.button_count; ++index) {
        button = AnomalyNteUiButtonSnapshotV1{sizeof(button)};
        // 枚举途中目录被替换：放弃这一份，下一轮重扫。
        if (service->button_at(service->user, status.catalog_sequence, index, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            return false;
        }
        if (!ResetSnapshotContains(needle, button)) continue;
        found = button;
        how = "名称包含匹配";
        return true;
    }
    return false;
}

void AdvanceReset(const AnomalyHostApiV1* host) {
    if (!g_reset.running) {
        static_cast<void>(g_reset.cancel_requested.exchange(false));
        if (g_reset.start_requested.exchange(false)) ResetBegin(host);
        return;
    }
    if (g_reset.cancel_requested.exchange(false)) {
        ResetFinish("已取消刷新", false);
        return;
    }

    const auto* service = ResetService(host);
    if (!ResetServiceUsable(service)) {
        ResetFinish("UI 按钮服务已不可用", false);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now > g_reset.deadline) {
        ResetFinish("刷新超时（60 秒内未成功），已停止；关闭营火界面后重试", false);
        return;
    }

    // 1) 扫描请求：完成前不查找，避免拿半成品目录。
    if (g_reset.scan_pending) {
        AnomalyNteUiButtonRequestSnapshotV1 scan{sizeof(scan)};
        if (service->request_snapshot(service->user, g_reset.scan_request, &scan).code !=
                ANOMALY_STATUS_V1_OK ||
            scan.state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
            g_reset.scan_pending = false;
            g_reset.scan_request = {};
            if (scan.state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE &&
                scan.status == ANOMALY_STATUS_V1_OK) {
                g_reset.catalog_sequence = scan.catalog_sequence;
                g_reset.next_attempt = {};
            } else if (scan.status != ANOMALY_STATUS_V1_OK) {
                ResetPublish("扫描按钮失败" + ResetDetail(scan.detail) + "，重试中…");
            }
        } else {
            return;
        }
    }

    // 2) 点击请求：COMPLETE 的 status 就是结果。
    if (g_reset.click_request.id) {
        AnomalyNteUiButtonRequestSnapshotV1 click{sizeof(click)};
        const AnomalyStatusV1 polled =
            service->request_snapshot(service->user, g_reset.click_request, &click);
        const bool completed = polled.code != ANOMALY_STATUS_V1_OK ||
            click.state == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE;
        if (!completed) {
            if (now - g_reset.click_sent > kResetClickTimeout) {
                g_reset.click_request = {};
                g_reset.next_attempt = now + kResetAttemptInterval;
                ResetPublish("点击请求超时，等待重试…");
            }
            return;
        }
        g_reset.click_request = {};
        if (polled.code == ANOMALY_STATUS_V1_OK && click.status == ANOMALY_STATUS_V1_OK) {
            ++g_reset.presses;
            ResetFinish("刷新怪物已按下（第 " + std::to_string(g_reset.presses) + " 次）", true);
            return;
        }
        if (polled.code != ANOMALY_STATUS_V1_OK || click.status == ANOMALY_STATUS_V1_NOT_FOUND ||
            click.status == ANOMALY_STATUS_V1_UNAVAILABLE) {
            g_reset.catalog_sequence = 0;
            g_reset.next_scan = now;
            g_reset.next_attempt = now + kResetAttemptInterval;
            ResetPublish("按钮句柄已失效，正在重新扫描…");
            return;
        }
        if (click.status == ANOMALY_STATUS_V1_CONFLICT) {
            ResetPublish("按钮当前不可点击（" + ResetReasons(click.reasons) +
                         "），等待冷却…");
        } else {
            ResetPublish("游戏未接受这次点击" + ResetDetail(click.detail) + "，等待重试…");
        }
        g_reset.next_attempt = now + kResetAttemptInterval;
        return;
    }

    // 3) 目录刷新（营火界面随时可能开关）。
    if (g_reset.catalog_sequence == 0 || now >= g_reset.next_scan) {
        AnomalyGenerationHandleV1 request{};
        if (service->request_scan(service->user, &request).code == ANOMALY_STATUS_V1_OK &&
            request.id != 0) {
            g_reset.scan_pending = true;
            g_reset.scan_request = request;
        }
        g_reset.next_scan = now + kResetScanInterval;
        return;
    }

    // 4) 查找按钮：find 是精确名称匹配，返回目录顺序里第一个匹配（可点击 → 不可点击 → 隐藏）。
    if (now < g_reset.next_attempt) return;

    const std::string name = ResetButtonName();
    if (name.empty()) {
        ResetFinish("按钮名为空，已停止", false);
        return;
    }

    AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
    std::string how;
    if (!ResetFindButton(service, name, button, how)) {
        g_reset.next_attempt = now + kResetAttemptInterval;
        g_reset.next_scan = now;
        ResetPublish("未找到按钮「" + name + "」——请先打开营火界面");
        return;
    }

    const std::string label = std::string(button.name) +
        (button.text[0] != '\0' ? std::string("「") + button.text + "」" : std::string()) +
        "（窗口 " + (button.window[0] != '\0' ? button.window : "-") + "，" + how + "）";

    if (button.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE) {
        AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
        click.button = button.button;
        AnomalyGenerationHandleV1 request{};
        if (service->request_click(service->user, &click, &request).code != ANOMALY_STATUS_V1_OK ||
            request.id == 0) {
            g_reset.next_attempt = now + kResetAttemptInterval;
            ResetPublish("点击请求被拒绝，等待重试…");
            return;
        }
        g_reset.click_request = request;
        g_reset.click_sent = now;
        ResetPublish("已提交点击：" + label);
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 reset: click " + label);
        return;
    }

    // 不可点击：禁用 / 不可交互 / 锁定就是冷却中，等下一个周期再看。
    g_reset.next_attempt = now + kResetAttemptInterval;
    const bool cooling = (button.reasons & kResetCooldownReasons) != 0;
    ResetPublish(std::string(cooling ? "按钮冷却中，等待…（" : "按钮不可点击，等待…（") +
                 ResetReasons(button.reasons) + "）");
}

void ResetStop() noexcept {
    g_reset.start_requested.store(false);
    g_reset.cancel_requested.store(false);
    g_reset.running = false;
    g_reset.scan_pending = false;
    g_reset.scan_request = {};
    g_reset.click_request = {};
    g_reset.catalog_sequence = 0;
    g_reset_busy.store(false);
}

void ResetSyncNameText() {
    if (!g_reset_text_stale.exchange(false)) return;
    const std::string name = ResetButtonName();
    std::snprintf(g_reset_name_text.data(), g_reset_name_text.size(), "%s", name.c_str());
}

void ResetContent(const AnomalyHostApiV1* host, const AnomalyUiServiceV1* ui) {
    ResetSyncNameText();

    DrawText(ui, g_context.localizer.Text("reset.title", "刷新怪物"));
    ui->text(ui->user, StringView("按钮名"));
    if (ui->same_line != nullptr) ui->same_line(ui->user, 96.0F, 8.0F);
    if (ui->input_text != nullptr) {
        ui->input_text(ui->user, StringView("##night999-reset-name"), g_reset_name_text.data(),
                       g_reset_name_text.size(), ANOMALY_UI_TEXT_INPUT_V1_NONE);
    } else {
        DrawText(ui, g_reset_name_text.data());
    }
    ResetSetButtonName(g_reset_name_text.data());

    const bool busy = g_reset_busy.load();
    if (ui->button != nullptr) {
        if (ui->button_enabled != nullptr) {
            if (ui->button_enabled(ui->user, StringView("刷新一次"), 0.0F, 0.0F, busy ? 0 : 1) !=
                0) {
                g_reset.start_requested.store(true);
            }
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
            if (ui->button_enabled(ui->user, StringView("取消"), 0.0F, 0.0F, busy ? 1 : 0) != 0) {
                g_reset.cancel_requested.store(true);
            }
        } else {
            if (!busy && ui->button(ui->user, StringView("刷新一次"), 0.0F, 0.0F) != 0) {
                g_reset.start_requested.store(true);
            }
        }
    }

    const auto* service = ResetService(host);
    if (!ResetServiceUsable(service)) {
        DrawText(ui, "UI 按钮服务不可用（需要 nte-ui-buttons）");
    }
    DrawText(ui, ResetStatus());
    if (g_reset.presses != 0) {
        DrawText(ui, "本轮已刷新 " + std::to_string(g_reset.presses) + " 次");
    }
}

}  // namespace
