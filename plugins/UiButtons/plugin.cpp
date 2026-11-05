// UI 按钮调试面板：anomaly.nte.ui-buttons 服务的消费者。
//
// 扫描、可点击判定、遮挡判定、鼠标拾取和点击全部由 Host 完成；插件只提交请求、轮询请求状态，
// 并把 Host 发布的按钮目录复制成面板快照。
// 线程边界：服务调用都在 Game 域 on_update；on_draw 只读快照并把点击意图写进队列。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/localization.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kPluginId = "anomaly.ui-buttons";
constexpr std::string_view kPluginVersion = "0.2.0";
constexpr std::size_t kMaxRowsPerTab = 400;
constexpr std::chrono::milliseconds kRescanAfterClick{600};
constexpr std::chrono::seconds kDelayedPick{3};

struct Row {
    AnomalyGenerationHandleV1 button{};
    std::uint32_t kind{};
    std::uint32_t category{};
    std::string name, text, window, class_name, reasons, path;
    std::string search;  // 过滤用的拼接串
};

struct LayerRow {
    std::string layer, window, flags;
    bool showing{};
    bool blocking{};
};

struct Snapshot {
    std::vector<Row> rows;
    std::vector<LayerRow> layers;
    std::array<std::uint32_t, 4> counts{};
    std::uint64_t sequence{};
};

struct PickSnapshot {
    std::vector<Row> hits;  // 最内层在前
    std::uint32_t checked{};
    std::uint32_t calls{};
    std::string error;
    std::uint64_t sequence{};
};

struct ClickIntent {
    AnomalyGenerationHandleV1 button{};
    bool force{};
    std::string label;
};

// 已提交、尚未完成的请求。
struct Pending {
    AnomalyGenerationHandleV1 handle{};
    std::uint32_t kind{};
    std::string label;
    bool force{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyInputServiceV1* input{};
    anomaly::sdk::Localizer localizer;

    // Game 域独占
    const AnomalyNteUiButtonsServiceV1* buttons{};
    std::vector<Pending> pending;
    Clock::time_point next_auto_scan{};
    Clock::time_point rescan_at{};
    bool rescan_scheduled{};
    Clock::time_point pick_at{};
    bool delayed_pick_pending{};
    std::uint64_t pick_sequence{};
    std::uint64_t shown_catalog{};
    // 上一次记进日志的遮挡情况；变化时才记，自动刷新不刷屏。
    std::string logged_occlusion;
    AnomalyGenerationHandleV1 pick_hotkey{};
    std::uint32_t pick_hotkey_registered{};

    // 跨域
    std::mutex mutex;
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<const PickSnapshot> pick;
    std::vector<ClickIntent> clicks;
    std::string status;
    std::string progress;
    std::atomic_bool scan_requested{};
    std::atomic_bool pick_requested{};
    std::atomic_bool delayed_pick_requested{};
    std::atomic_bool capturing_pick_key{};
    std::atomic_uint32_t pick_key{VK_F8};
    std::atomic_bool auto_refresh{};
    std::atomic_uint32_t auto_interval_ms{1000};

    // Render 域独占
    std::array<char, 128> filter{};
    int force{};
    int show_layers{};
    int show_umg{1};
    std::uint32_t interval_input{1000};
};

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

#define UI_HAS(ui, member)                                                              \
    (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::member)>(                \
         (ui), offsetof(AnomalyUiServiceV1, member)) &&                                 \
     (ui)->member != nullptr)

AnomalyStatusV1 Status(std::uint32_t code, std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

void Log(Context& context, const std::string& message) {
    if (context.core != nullptr && context.core->log != nullptr) {
        context.core->log(context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                          anomaly::sdk::StringView("[UiButtons] " + message));
    }
}

void SetStatus(Context& context, std::string text) {
    std::scoped_lock lock(context.mutex);
    context.status = std::move(text);
}

std::string Lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return result;
}

std::string_view KindName(std::uint32_t kind) noexcept {
    switch (kind) {
    case ANOMALY_NTE_UI_BUTTON_KIND_V1_UMG: return "UMG";
    case ANOMALY_NTE_UI_BUTTON_KIND_V1_COMMON: return "Common";
    case ANOMALY_NTE_UI_BUTTON_KIND_V1_HTUI: return "HTUI";
    case ANOMALY_NTE_UI_BUTTON_KIND_V1_RADIO: return "Radio";
    case ANOMALY_NTE_UI_BUTTON_KIND_V1_LIST_ENTRY: return "List entry";
    default: return "?";
    }
}

std::string CategoryName(const anomaly::sdk::Localizer& localizer, std::uint32_t category) {
    switch (category) {
    case ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE:
        return localizer.Text("category.clickable", "Clickable");
    case ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_BLOCKED:
        return localizer.Text("category.blocked", "Not clickable");
    case ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN:
        return localizer.Text("category.hidden", "Hidden");
    default: return "?";
    }
}

std::string ResultName(const anomaly::sdk::Localizer& localizer, std::uint32_t status) {
    switch (status) {
    case ANOMALY_STATUS_V1_OK:
        return localizer.Text("result.ok", "Invoked");
    case ANOMALY_STATUS_V1_CONFLICT:
        return localizer.Text("result.conflict", "Not clickable");
    case ANOMALY_STATUS_V1_NOT_FOUND:
        return localizer.Text("result.not_found", "Button is stale");
    case ANOMALY_STATUS_V1_CANCELLED:
        return localizer.Text("result.cancelled", "Cancelled");
    case ANOMALY_STATUS_V1_UNAVAILABLE:
        return localizer.Text("result.unavailable", "Service unavailable");
    case ANOMALY_STATUS_V1_FAILED:
        return localizer.Text("result.rejected", "Rejected by the game");
    default: return localizer.Text("result.failed", "Failed");
    }
}

std::string DescribeReasons(const anomaly::sdk::Localizer& localizer, std::uint32_t reasons) {
    std::string text;
    const auto append = [&text](std::string name) {
        if (!text.empty()) text += " / ";
        text += std::move(name);
    };
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_SELF) != 0) {
        append(localizer.Text("reason.self_hidden", "Hidden itself"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_ANCESTOR) != 0) {
        append(localizer.Text("reason.ancestor_hidden", "Hidden by an ancestor"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_TRANSPARENT) != 0) {
        append(localizer.Text("reason.transparent", "Fully transparent"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE) != 0) {
        append(localizer.Text("reason.inactive_page", "Not the active page"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_IN_VIEWPORT) != 0) {
        append(localizer.Text("reason.not_in_viewport", "Window not on screen"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED) != 0) {
        append(localizer.Text("reason.detached", "Not attached to a window"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED) != 0) {
        append(localizer.Text("reason.occluded", "Covered by another window"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_CLOSING) != 0) {
        append(localizer.Text("reason.closing", "Window is closing"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_HIT_TESTABLE) != 0) {
        append(localizer.Text("reason.not_hit_testable", "Not hit-testable"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED) != 0) {
        append(localizer.Text("reason.disabled", "Disabled"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_INTERACTABLE) != 0) {
        append(localizer.Text("reason.not_interactable", "Not interactable"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED) != 0) {
        append(localizer.Text("reason.locked", "Locked"));
    }
    if ((reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_QUERY_FAILED) != 0) {
        append(localizer.Text("reason.query_failed", "Query failed"));
    }
    return text;
}

Row MakeRow(const anomaly::sdk::Localizer& localizer,
            const AnomalyNteUiButtonSnapshotV1& button) {
    Row row;
    row.button = button.button;
    row.kind = button.kind;
    row.category = button.category;
    row.name = button.name;
    row.text = button.text;
    row.window = button.window[0] != '\0' ? button.window : button.root;
    if (button.owner[0] != '\0' && row.window != button.owner) {
        row.window += std::string(" / ") + button.owner;
    }
    row.class_name = button.class_name;
    row.path = button.path;
    row.reasons = DescribeReasons(localizer, button.reasons);
    if (button.cause[0] != '\0') {
        const std::array<std::string_view, 2> args{std::string_view(row.reasons),
                                                   std::string_view(button.cause)};
        row.reasons = localizer.Format("text.reason_with_cause", "{0} ({1})", args);
    }
    row.search = Lower(row.name + '\n' + row.text + '\n' + row.window + '\n' + row.class_name);
    return row;
}

std::string Label(const anomaly::sdk::Localizer& localizer, const Row& row) {
    if (row.text.empty()) return row.name;
    const std::array<std::string_view, 2> args{std::string_view(row.name),
                                               std::string_view(row.text)};
    return localizer.Format("label.row_with_text", "{0} \"{1}\"", args);
}

// 服务可能晚于插件加载才发布，也可能随 Host 代际撤销：每 tick 现查。
bool EnsureService(Context& context) {
    context.buttons = anomaly::sdk::Host(context.host)
                          .Query<AnomalyNteUiButtonsServiceV1>(
                              ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
                              ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION)
                          .get();
    return context.buttons != nullptr &&
        HasField<AnomalyNteUiButtonsServiceV1, decltype(AnomalyNteUiButtonsServiceV1::cancel)>(
            context.buttons, offsetof(AnomalyNteUiButtonsServiceV1, cancel));
}

bool Busy(const Context& context, std::uint32_t kind) {
    return std::any_of(context.pending.begin(), context.pending.end(),
                       [kind](const Pending& p) { return p.kind == kind; });
}

void Submit(Context& context, std::uint32_t kind, std::string label = {},
            AnomalyGenerationHandleV1 button = {}, bool force = false) {
    const auto* service = context.buttons;
    AnomalyGenerationHandleV1 handle{};
    AnomalyStatusV1 result{};
    if (kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN) {
        result = service->request_scan(service->user, &handle);
    } else if (kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK) {
        result = service->request_pick(service->user, &handle);
    } else {
        AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
        click.button = button;
        click.flags = force ? ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE : 0u;
        result = service->request_click(service->user, &click, &handle);
    }
    if (result.code != ANOMALY_STATUS_V1_OK) {
        const std::string reason(result.message.data == nullptr
                                     ? std::string_view{}
                                     : std::string_view(result.message.data,
                                                        result.message.size));
        const std::array<std::string_view, 1> args{std::string_view(reason)};
        SetStatus(context, context.localizer.Format("status.request_rejected",
                                                    "Request rejected: {0}", args));
        return;
    }
    context.pending.push_back({handle, kind, std::move(label), force});
}

// 目录序列号变化时把 Host 目录复制成面板快照。
void RefreshCatalog(Context& context) {
    const auto* service = context.buttons;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) return;
    if ((status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) == 0 ||
        status.catalog_sequence == context.shown_catalog) {
        return;
    }
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->sequence = status.catalog_sequence;
    snapshot->rows.reserve(status.button_count);
    std::uint32_t occluded = 0;
    std::vector<std::string> occluded_causes;
    AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
    for (std::uint32_t i = 0; i < status.button_count; ++i) {
        // 复制途中被更新的目录替换：放弃这一份，下一 tick 重读。
        if (service->button_at(service->user, status.catalog_sequence, i, &button).code !=
            ANOMALY_STATUS_V1_OK) {
            return;
        }
        if (button.category < snapshot->counts.size()) ++snapshot->counts[button.category];
        if ((button.reasons & ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED) != 0) {
            ++occluded;
            if (std::find(occluded_causes.begin(), occluded_causes.end(), button.cause) ==
                occluded_causes.end()) {
                occluded_causes.emplace_back(button.cause);
            }
        }
        snapshot->rows.push_back(MakeRow(context.localizer, button));
    }
    AnomalyNteUiWindowSnapshotV1 window{sizeof(window)};
    for (std::uint32_t i = 0; i < status.window_count; ++i) {
        if (service->window_at(service->user, status.catalog_sequence, i, &window).code !=
            ANOMALY_STATUS_V1_OK) {
            return;
        }
        if (window.window[0] == '\0') continue;
        LayerRow layer;
        layer.layer = window.layer;
        layer.window = window.window;
        layer.showing = (window.flags & ANOMALY_NTE_UI_WINDOW_V1_ACTIVE) != 0 &&
            (window.flags & ANOMALY_NTE_UI_WINDOW_V1_VISIBLE) != 0 &&
            (window.flags & ANOMALY_NTE_UI_WINDOW_V1_CLOSING) == 0;
        layer.blocking = (window.flags & ANOMALY_NTE_UI_WINDOW_V1_BLOCKING) != 0;
        if ((window.flags & ANOMALY_NTE_UI_WINDOW_V1_VISIBLE) == 0) {
            layer.flags = context.localizer.Text("flag.not_visible", "Not visible");
        }
        const auto append_flag = [&layer](std::string name) {
            if (!layer.flags.empty()) layer.flags += " ";
            layer.flags += std::move(name);
        };
        if ((window.flags & ANOMALY_NTE_UI_WINDOW_V1_ACTIVE) != 0) {
            append_flag(context.localizer.Text("flag.active", "Active"));
        }
        if ((window.flags & ANOMALY_NTE_UI_WINDOW_V1_CLOSING) != 0) {
            append_flag(context.localizer.Text("flag.closing", "Closing"));
        }
        if ((window.flags & ANOMALY_NTE_UI_WINDOW_V1_MODAL) != 0) {
            append_flag(context.localizer.Text("flag.modal", "Modal"));
        }
        if ((window.flags & ANOMALY_NTE_UI_WINDOW_V1_HIDES_MAIN_FORM) != 0) {
            append_flag(context.localizer.Text("flag.hides_main_form", "Hides the main UI"));
        }
        if ((window.flags & ANOMALY_NTE_UI_WINDOW_V1_PAUSES_GAME) != 0) {
            append_flag(context.localizer.Text("flag.pauses_game", "Pauses the game"));
        }
        if ((window.flags & ANOMALY_NTE_UI_WINDOW_V1_MENU_INPUT) != 0) {
            append_flag(context.localizer.Text("flag.menu_input", "Menu input"));
        }
        if (layer.blocking) {
            append_flag(context.localizer.Text("flag.blocking", "→ blocks windows below"));
        }
        snapshot->layers.push_back(std::move(layer));
    }
    std::stable_sort(snapshot->layers.begin(), snapshot->layers.end(),
                     [](const LayerRow& a, const LayerRow& b) { return a.showing > b.showing; });
    // 遮挡判定偶发误判时，日志里要能看到当时是哪些界面在挡、挡住了多少按钮。
    std::string occlusion;
    for (const auto& layer : snapshot->layers) {
        if (!layer.blocking) continue;
        occlusion += "\n  blocking: " + layer.layer + " -> " + layer.window + " [" + layer.flags + "]";
    }
    if (occluded != 0) {
        occlusion += "\n  occluded buttons: " + std::to_string(occluded) + ", by:";
        for (const auto& cause : occluded_causes) occlusion += " " + cause + ";";
    }
    if (occlusion != context.logged_occlusion) {
        Log(context, "catalog #" + std::to_string(status.catalog_sequence) + " occlusion changed" +
                         (occlusion.empty() ? std::string("\n  none") : occlusion));
        context.logged_occlusion = occlusion;
    }
    const std::string sequence = std::to_string(status.catalog_sequence);
    const std::string objects = std::to_string(status.objects_scanned);
    const std::string windows = std::to_string(status.window_count);
    const std::string buttons = std::to_string(status.button_count);
    const std::array<std::string_view, 4> catalog_args{
        std::string_view(sequence), std::string_view(objects), std::string_view(windows),
        std::string_view(buttons)};
    const std::string clickable = std::to_string(status.clickable_count);
    const std::string blocked = std::to_string(status.blocked_count);
    const std::string hidden = std::to_string(status.hidden_count);
    const std::string calls = std::to_string(status.process_event_calls);
    const std::string ticks = std::to_string(status.scan_ticks);
    const std::string milliseconds = std::to_string(status.scan_milliseconds);
    const std::array<std::string_view, 6> count_args{
        std::string_view(clickable), std::string_view(blocked), std::string_view(hidden),
        std::string_view(calls), std::string_view(ticks), std::string_view(milliseconds)};
    std::string summary = context.localizer.Format(
        "progress.catalog", "Catalog #{0}: objects {1}, windows {2}, buttons {3}",
        catalog_args);
    summary += "  ";
    summary += context.localizer.Format(
        "progress.counts",
        "clickable {0} / blocked {1} / hidden {2}, {3} game calls, {4} ticks, {5} ms",
        count_args);
    if ((status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_TRUNCATED) != 0) {
        summary += context.localizer.Text("progress.truncated", " (truncated)");
    }
    context.shown_catalog = status.catalog_sequence;
    std::scoped_lock lock(context.mutex);
    context.snapshot = std::move(snapshot);
    context.progress = std::move(summary);
}

void FinishPick(Context& context, const Pending& pending,
                const AnomalyNteUiButtonRequestSnapshotV1& done) {
    const auto* service = context.buttons;
    auto pick = std::make_shared<PickSnapshot>();
    pick->sequence = ++context.pick_sequence;
    pick->checked = done.checked;
    pick->calls = done.process_event_calls;
    if (done.status != ANOMALY_STATUS_V1_OK) pick->error = done.detail;
    AnomalyNteUiButtonSnapshotV1 hit{sizeof(hit)};
    for (std::uint32_t i = 0; i < done.hit_count; ++i) {
        if (service->pick_hit_at(service->user, pending.handle, i, &hit).code ==
            ANOMALY_STATUS_V1_OK) {
            pick->hits.push_back(MakeRow(context.localizer, hit));
        }
    }
    std::string log = "pick: checked " + std::to_string(pick->checked) + " buttons, " +
        std::to_string(pick->hits.size()) + " hovered";
    for (const auto& row : pick->hits) {
        log += "\n  " + Label(context.localizer, row) + " [" +
            CategoryName(context.localizer, row.category) +
            (row.reasons.empty() ? "" : ": " + row.reasons) + "] " + row.path +
            " class=" + row.class_name;
    }
    if (!pick->error.empty()) log += " error=" + pick->error;
    Log(context, log);
    if (!pick->error.empty()) {
        const std::array<std::string_view, 1> args{std::string_view(pick->error)};
        SetStatus(context, context.localizer.Format("status.pick_failed", "Pick failed: {0}",
                                                    args));
    } else if (pick->hits.empty()) {
        SetStatus(context, context.localizer.Text("status.pick_empty",
                                                  "Pick finished: no button under the cursor"));
    } else {
        const auto& top = pick->hits.front();
        const std::string label = Label(context.localizer, top);
        const std::string category = CategoryName(context.localizer, top.category);
        const std::array<std::string_view, 2> args{std::string_view(label),
                                                   std::string_view(category)};
        SetStatus(context, context.localizer.Format("status.pick_done",
                                                    "Pick finished: {0} ({1})", args));
    }
    std::scoped_lock lock(context.mutex);
    context.pick = std::move(pick);
}

void PollRequests(Context& context) {
    const auto* service = context.buttons;
    std::vector<Pending> remaining;
    for (const auto& pending : context.pending) {
        AnomalyNteUiButtonRequestSnapshotV1 done{sizeof(done)};
        if (service->request_snapshot(service->user, pending.handle, &done).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;  // Host 重置后旧请求不再可寻址
        }
        if (done.state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
            if (pending.kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK) {
                SetStatus(context, context.localizer.Text("status.picking",
                                                          "Picking: keep the mouse still..."));
            }
            remaining.push_back(pending);
            continue;
        }
        if (pending.kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK) {
            FinishPick(context, pending, done);
        } else if (pending.kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN) {
            if (done.status != ANOMALY_STATUS_V1_OK) {
                const std::array<std::string_view, 1> args{std::string_view(done.detail)};
                SetStatus(context, context.localizer.Format("status.scan_failed",
                                                            "Scan failed: {0}", args));
            }
        } else {
            const std::string result = ResultName(context.localizer, done.status);
            const std::array<std::string_view, 2> result_args{std::string_view(result),
                                                              std::string_view(pending.label)};
            std::string message = context.localizer.Format("status.click_result", "{0}: {1}",
                                                           result_args);
            if (pending.force) {
                message += " [" + context.localizer.Text("label.forced", "forced") + "]";
            }
            if (done.detail[0] != '\0') {
                const std::array<std::string_view, 2> detail_args{
                    std::string_view(message), std::string_view(done.detail)};
                message = context.localizer.Format("text.reason_with_cause", "{0} ({1})",
                                                   detail_args);
            }
            if (done.reasons != 0) {
                message += "; " + DescribeReasons(context.localizer, done.reasons);
            }
            Log(context, "click " + message);
            SetStatus(context, std::move(message));
            if (done.status == ANOMALY_STATUS_V1_OK) {
                context.rescan_scheduled = true;
                context.rescan_at = Clock::now() + kRescanAfterClick;
            }
        }
    }
    context.pending = std::move(remaining);
}

bool InputReady(const AnomalyInputServiceV1* input) noexcept {
    return HasField<AnomalyInputServiceV1, decltype(AnomalyInputServiceV1::release_hotkey)>(
               input, offsetof(AnomalyInputServiceV1, release_hotkey)) &&
        input->was_pressed != nullptr && input->register_hotkey != nullptr &&
        input->release_hotkey != nullptr;
}

bool ValidPickKey(std::uint32_t key) noexcept {
    return key != 0 && key < 256U && key != VK_ESCAPE &&
        !(key >= VK_LBUTTON && key <= VK_XBUTTON2) && key != VK_SHIFT && key != VK_CONTROL &&
        key != VK_MENU && !(key >= VK_LSHIFT && key <= VK_RMENU);
}

std::string KeyName(const anomaly::sdk::Localizer& localizer, std::uint32_t key) {
    if (key == 0) return localizer.Text("label.unset", "Not set");
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) {
        return std::string(1, static_cast<char>(key));
    }
    if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1U);
    switch (key) {
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_PAUSE: return "Pause";
    case VK_OEM_3: return "`";
    default: return "Key" + std::to_string(key);
    }
}

void ANOMALY_CALL PickHotkey(void* user, AnomalyGenerationHandleV1,
                             const AnomalyInputSnapshotV1*) noexcept {
    auto* context = static_cast<Context*>(user);
    if (context != nullptr && !context->capturing_pick_key.load(std::memory_order_acquire)) {
        context->pick_requested.store(true, std::memory_order_release);
    }
}

void ReleasePickHotkey(Context& context) noexcept {
    if (context.pick_hotkey.id != 0 && InputReady(context.input)) {
        static_cast<void>(context.input->release_hotkey(context.input->user, context.pick_hotkey));
    }
    context.pick_hotkey = {};
    context.pick_hotkey_registered = 0;
}

// 注册拾取快捷键；失败时保留旧键。
std::string RegisterPickHotkey(Context& context, std::uint32_t key) {
    if (!InputReady(context.input)) {
        return context.localizer.Text("status.input_unavailable",
                                      "Input service unavailable, the hotkey does nothing");
    }
    if (!ValidPickKey(key)) {
        return context.localizer.Text("status.key_unsupported", "That key is not supported");
    }
    if (key == context.pick_hotkey_registered && context.pick_hotkey.id != 0) return {};
    AnomalyHotkeySpecV1 spec{sizeof(spec)};
    spec.virtual_key = key;
    spec.flags = ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS | ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED;
    const std::string id = "ui-buttons-pick-" + std::to_string(key);
    spec.id = anomaly::sdk::StringView(id);
    AnomalyGenerationHandleV1 handle{};
    const auto status = context.input->register_hotkey(context.input->user, &spec, PickHotkey,
                                                       &context, &handle);
    if (status.code != ANOMALY_STATUS_V1_OK || handle.id == 0) {
        const std::string name = KeyName(context.localizer, key);
        const std::array<std::string_view, 1> args{std::string_view(name)};
        return status.code == ANOMALY_STATUS_V1_CONFLICT
            ? context.localizer.Format("status.key_conflict",
                                       "{0} is already used by another feature", args)
            : context.localizer.Format("status.key_register_failed",
                                       "{0} failed to register", args);
    }
    ReleasePickHotkey(context);
    context.pick_hotkey = handle;
    context.pick_hotkey_registered = key;
    context.pick_key.store(key, std::memory_order_release);
    return {};
}

// 重新绑定拾取键：在 Game 域轮询按键，Esc 取消。
void CapturePickKey(Context& context) {
    if (!InputReady(context.input)) {
        context.capturing_pick_key.store(false, std::memory_order_release);
        return;
    }
    std::int32_t pressed{};
    if (context.input->was_pressed(context.input->user, VK_ESCAPE, &pressed).code ==
            ANOMALY_STATUS_V1_OK && pressed != 0) {
        context.capturing_pick_key.store(false, std::memory_order_release);
        SetStatus(context, context.localizer.Text("status.pick_key_cancelled",
                                                  "Pick hotkey unchanged"));
        return;
    }
    for (std::uint32_t key = 1; key < 256U; ++key) {
        if (!ValidPickKey(key)) continue;
        pressed = 0;
        if (context.input->was_pressed(context.input->user, key, &pressed).code !=
                ANOMALY_STATUS_V1_OK || pressed == 0) {
            continue;
        }
        const auto error = RegisterPickHotkey(context, key);
        context.capturing_pick_key.store(false, std::memory_order_release);
        if (error.empty()) {
            const std::string name = KeyName(context.localizer, key);
            const std::array<std::string_view, 1> args{std::string_view(name)};
            SetStatus(context, context.localizer.Format("status.pick_key_set",
                                                        "Pick hotkey set to {0}", args));
        } else {
            SetStatus(context, error);
        }
        return;
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *plugin_context = nullptr;
    auto context = std::unique_ptr<Context>(new (std::nothrow) Context());
    if (!context) return Status(ANOMALY_STATUS_V1_FAILED);
    const anomaly::sdk::Host view(host);
    context->host = host;
    context->core = view.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
    context->ui = view.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();
    context->input = view.Query<AnomalyInputServiceV1>(
        ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION).get();
    context->localizer = anomaly::sdk::Localizer(host);
    context->status = context->localizer.Text("status.not_scanned", "Not scanned yet");
    if (!UI_HAS(context->ui, end_window) || !UI_HAS(context->ui, text) ||
        !UI_HAS(context->ui, button)) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "UI buttons requires the UI service");
    }
    *plugin_context = context.release();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    // 快捷键是可选功能：注册失败不阻止插件启动，面板里给出原因。
    const auto error = RegisterPickHotkey(*context, context->pick_key.load(std::memory_order_acquire));
    if (!error.empty()) {
        const std::array<std::string_view, 1> args{std::string_view(error)};
        SetStatus(*context, context->localizer.Format("status.pick_key_error",
                                                      "Pick hotkey: {0}", args));
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->capturing_pick_key.store(false, std::memory_order_release);
    ReleasePickHotkey(*context);
    // 未完成的请求交还给 Host 取消，不让它们在插件卸载后继续执行点击。
    if (context->buttons != nullptr) {
        for (const auto& pending : context->pending) {
            static_cast<void>(context->buttons->cancel(context->buttons->user, pending.handle));
        }
    }
    context->pending.clear();
    context->buttons = nullptr;
    std::scoped_lock lock(context->mutex);
    context->clicks.clear();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    delete static_cast<Context*>(plugin_context);
}

void ANOMALY_CALL Update(void* plugin_context, double) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    if (context->capturing_pick_key.load(std::memory_order_acquire)) CapturePickKey(*context);

    const auto now = Clock::now();
    if (context->delayed_pick_requested.exchange(false, std::memory_order_acq_rel)) {
        context->pick_at = now + kDelayedPick;
        context->delayed_pick_pending = true;
        SetStatus(*context, context->localizer.Text(
                                "status.delayed_pick",
                                "Picking in 3 seconds: move the mouse over the in-game button"));
    }
    if (context->delayed_pick_pending && now >= context->pick_at) {
        context->delayed_pick_pending = false;
        context->pick_requested.store(true, std::memory_order_release);
    }

    if (!EnsureService(*context)) {
        context->pending.clear();
        SetStatus(*context, context->localizer.Text(
                                "status.service_unavailable",
                                "anomaly.nte.ui-buttons service unavailable (profile or game "
                                "state not ready)"));
        return;
    }
    PollRequests(*context);
    RefreshCatalog(*context);

    std::vector<ClickIntent> clicks;
    {
        std::scoped_lock lock(context->mutex);
        clicks.swap(context->clicks);
    }
    for (auto& click : clicks) {
        Submit(*context, ANOMALY_NTE_UI_BUTTON_REQUEST_V1_CLICK, std::move(click.label),
               click.button, click.force);
    }
    if (context->pick_requested.exchange(false, std::memory_order_acq_rel) &&
        !Busy(*context, ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK)) {
        Submit(*context, ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK);
    }
    bool scan = context->scan_requested.exchange(false, std::memory_order_acq_rel);
    if (context->rescan_scheduled && now >= context->rescan_at) {
        context->rescan_scheduled = false;
        scan = true;
    }
    if (context->auto_refresh.load(std::memory_order_acquire) && now >= context->next_auto_scan) {
        scan = true;
    }
    if (scan && !Busy(*context, ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN)) {
        Submit(*context, ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN);
        const auto interval = std::max<std::uint32_t>(
            200, context->auto_interval_ms.load(std::memory_order_acquire));
        context->next_auto_scan = now + std::chrono::milliseconds(interval);
    }
}

void Text(const AnomalyUiServiceV1* ui, std::string_view value) {
    ui->text(ui->user, anomaly::sdk::StringView(value));
}

bool Button(const AnomalyUiServiceV1* ui, std::string_view label, bool enabled = true) {
    if (UI_HAS(ui, button_enabled)) {
        return ui->button_enabled(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F,
                                  enabled ? 1 : 0) != 0;
    }
    return enabled && ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0;
}

void SameLine(const AnomalyUiServiceV1* ui) {
    if (UI_HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, -1.0F);
}

void Separator(const AnomalyUiServiceV1* ui) {
    if (UI_HAS(ui, separator)) ui->separator(ui->user);
}

void Checkbox(const AnomalyUiServiceV1* ui, std::string_view label, int& value) {
    if (UI_HAS(ui, checkbox)) ui->checkbox(ui->user, anomaly::sdk::StringView(label), &value);
}

void QueueClick(Context& context, const Row& row) {
    ClickIntent intent;
    intent.button = row.button;
    intent.force = context.force != 0;
    intent.label = Label(context.localizer, row);
    std::scoped_lock lock(context.mutex);
    context.clicks.push_back(std::move(intent));
}

void DrawRows(Context& context, const AnomalyUiServiceV1* ui, const Snapshot& snapshot,
              std::uint32_t category, const std::string& filter) {
    const bool table = UI_HAS(ui, begin_table) && UI_HAS(ui, end_table) &&
        UI_HAS(ui, table_next_row) && UI_HAS(ui, table_next_column);
    const bool clickable =
        category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE || context.force != 0;
    std::size_t shown = 0;
    std::size_t matched = 0;
    const std::string table_id = "rows" + std::to_string(category);
    const bool in_table = table &&
        ui->begin_table(ui->user, anomaly::sdk::StringView(table_id), 6,
                        ANOMALY_UI_TABLE_V1_SIZING_FIXED_FIT, 0.0F, 0.0F) != 0;
    for (std::size_t index = 0; index < snapshot.rows.size(); ++index) {
        const auto& row = snapshot.rows[index];
        if (row.category != category) continue;
        if (context.show_umg == 0 && row.kind == ANOMALY_NTE_UI_BUTTON_KIND_V1_UMG) continue;
        if (!filter.empty() && row.search.find(filter) == std::string::npos) continue;
        ++matched;
        if (shown >= kMaxRowsPerTab) continue;
        ++shown;
        const std::string label = context.localizer.Label(
            "action.click", "Click", "ui-buttons-click" + std::to_string(index));
        if (in_table) {
            ui->table_next_row(ui->user);
            ui->table_next_column(ui->user);
            if (Button(ui, label, clickable)) QueueClick(context, row);
            ui->table_next_column(ui->user);
            Text(ui, row.name);
            ui->table_next_column(ui->user);
            Text(ui, row.text.empty() ? "-" : row.text);
            ui->table_next_column(ui->user);
            Text(ui, row.window.empty() ? "-" : row.window);
            ui->table_next_column(ui->user);
            Text(ui, std::string(KindName(row.kind)) + " " + row.class_name);
            ui->table_next_column(ui->user);
            Text(ui, row.reasons.empty() ? "-" : row.reasons);
        } else {
            if (Button(ui, label, clickable)) QueueClick(context, row);
            SameLine(ui);
            Text(ui, row.name + "  " + row.text + "  [" + row.window + "]  " + row.reasons);
        }
    }
    if (in_table) ui->end_table(ui->user);
    if (matched > shown) {
        const std::string shown_text = std::to_string(shown);
        const std::string matched_text = std::to_string(matched);
        const std::array<std::string_view, 2> args{std::string_view(shown_text),
                                                   std::string_view(matched_text)};
        Text(ui, context.localizer.Format(
                     "text.rows_truncated",
                     "Showing the first {0} of {1} matches, narrow the filter to see the rest",
                     args));
    } else if (matched == 0) {
        Text(ui, context.localizer.Text("text.none", "(none)"));
    }
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* supplied_ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    const auto* ui = supplied_ui != nullptr ? supplied_ui : context->ui;
    if (!UI_HAS(ui, end_window) || !UI_HAS(ui, text) || !UI_HAS(ui, button)) return;

    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<const PickSnapshot> pick;
    std::string status;
    std::string progress;
    {
        std::scoped_lock lock(context->mutex);
        snapshot = context->snapshot;
        pick = context->pick;
        status = context->status;
        progress = context->progress;
    }

    if (UI_HAS(ui, set_next_window_size)) ui->set_next_window_size(ui->user, 980.0F, 620.0F, 4U);
    int open = 1;
    const std::string title = context->localizer.Label("window.title", "UI Buttons", "ui-buttons");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    const std::string scan_label = context->localizer.Text("action.scan", "Scan buttons");
    if (Button(ui, scan_label)) context->scan_requested.store(true, std::memory_order_release);
    SameLine(ui);
    int auto_refresh = context->auto_refresh.load(std::memory_order_acquire) ? 1 : 0;
    const std::string auto_refresh_label =
        context->localizer.Text("option.auto_refresh", "Auto refresh");
    Checkbox(ui, auto_refresh_label, auto_refresh);
    context->auto_refresh.store(auto_refresh != 0, std::memory_order_release);
    if (UI_HAS(ui, input_uint32)) {
        SameLine(ui);
        const std::string interval_label =
            context->localizer.Text("option.interval", "Interval (ms)");
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView(interval_label),
                             &context->interval_input, 100, 1000) != 0) {
            context->interval_input = std::clamp<std::uint32_t>(context->interval_input, 200,
                                                                60000);
            context->auto_interval_ms.store(context->interval_input,
                                            std::memory_order_release);
        }
    }

    Text(ui, context->localizer.Text(
                 "text.click_method",
                 "Clicking: press → release → click (the host re-checks clickability right "
                 "before the click)"));
    const std::string force_label = context->localizer.Text(
        "option.force_click",
        "Force click (ignores the clickability check, covered buttons are clicked too, which "
        "may put the game into a bad state)");
    Checkbox(ui, force_label, context->force);
    const std::string show_umg_label =
        context->localizer.Text("option.show_umg", "Show native UMG buttons");
    Checkbox(ui, show_umg_label, context->show_umg);
    SameLine(ui);
    const std::string show_layers_label =
        context->localizer.Text("option.show_layers", "Show UI layers");
    Checkbox(ui, show_layers_label, context->show_layers);

    // 拾取：鼠标停在游戏里的按钮上按快捷键，识别 Slate 认为正悬停的按钮。
    Text(ui, context->localizer.Text("label.pick_section", "Button picking:"));
    SameLine(ui);
    if (context->capturing_pick_key.load(std::memory_order_acquire)) {
        const std::string capture_label = context->localizer.Label(
            "action.capture_key", "Press the new pick key... (Esc cancels)", "pick-key");
        if (Button(ui, capture_label)) {
            context->capturing_pick_key.store(false, std::memory_order_release);
        }
    } else {
        const std::string key_name =
            KeyName(context->localizer, context->pick_key.load(std::memory_order_acquire));
        const std::array<std::string_view, 1> args{std::string_view(key_name)};
        const std::string hotkey_label =
            context->localizer.Format("action.pick_hotkey", "Hotkey: {0}", args) + "###pick-key";
        if (Button(ui, hotkey_label)) {
            context->capturing_pick_key.store(true, std::memory_order_release);
        }
    }
    SameLine(ui);
    const std::string delayed_pick_label =
        context->localizer.Label("action.delayed_pick", "Pick in 3 seconds", "pick-delay");
    if (Button(ui, delayed_pick_label)) {
        context->delayed_pick_requested.store(true, std::memory_order_release);
    }

    if (UI_HAS(ui, input_text)) {
        const std::string filter_label = context->localizer.Text(
            "option.filter", "Filter (name / text / window / class)");
        ui->input_text(ui->user, anomaly::sdk::StringView(filter_label),
                       context->filter.data(), context->filter.size(),
                       ANOMALY_UI_TEXT_INPUT_V1_NONE);
    }
    const std::string filter = Lower(std::string_view(context->filter.data()));

    Text(ui, status);
    if (!progress.empty()) Text(ui, progress);
    Separator(ui);

    if (pick) {
        const std::string sequence = std::to_string(pick->sequence);
        const std::string checked = std::to_string(pick->checked);
        const std::string calls = std::to_string(pick->calls);
        const std::array<std::string_view, 3> pick_args{
            std::string_view(sequence), std::string_view(checked), std::string_view(calls)};
        Text(ui, context->localizer.Format(
                     "text.pick_result",
                     "Pick result #{0} (checked {1} buttons, {2} game calls):", pick_args));
        if (!pick->error.empty()) {
            const std::array<std::string_view, 1> args{std::string_view(pick->error)};
            Text(ui, context->localizer.Format("text.pick_error", "  Failed: {0}", args));
        }
        if (pick->hits.empty() && pick->error.empty()) {
            Text(ui, context->localizer.Text(
                         "text.pick_none",
                         "  No button under the cursor (or the mouse was over the panel when "
                         "the key was pressed)"));
        }
        for (std::size_t i = 0; i < pick->hits.size(); ++i) {
            const auto& row = pick->hits[i];
            const bool enabled =
                row.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE || context->force != 0;
            const std::string click_label = context->localizer.Label(
                "action.click", "Click", "ui-buttons-pick" + std::to_string(i));
            if (Button(ui, click_label, enabled)) QueueClick(*context, row);
            SameLine(ui);
            std::string reasons = "[" + CategoryName(context->localizer, row.category);
            if (!row.reasons.empty()) {
                const std::array<std::string_view, 1> reason_args{
                    std::string_view(row.reasons)};
                reasons += context->localizer.Format("text.reason_suffix", ": {0}", reason_args);
            }
            reasons += "]";
            Text(ui, std::string(i == 0 ? "▶ " : "  ") + Label(context->localizer, row) +
                         "  " + reasons);
            Text(ui, "      " + row.path + "  (" + std::string(KindName(row.kind)) + " " +
                         row.class_name + ")");
        }
        if (pick->hits.size() > 1) {
            Text(ui, context->localizer.Text(
                         "text.innermost_hint",
                         "  ▶ is the innermost button; the others are outer buttons that are "
                         "hovered as well"));
        }
        Separator(ui);
    }

    if (!snapshot) {
        Text(ui, context->localizer.Text("text.start_hint",
                                         "Press \"Scan buttons\" to start."));
        return;
    }

    if (context->show_layers != 0) {
        Text(ui, context->localizer.Text("section.layers", "UI layers (at scan time):"));
        for (const auto& layer : snapshot->layers) {
            Text(ui, std::string(layer.showing ? "● " : "○ ") + layer.layer + " → " +
                         layer.window + (layer.flags.empty() ? "" : "  [" + layer.flags + "]"));
        }
        if (snapshot->layers.empty()) {
            Text(ui, context->localizer.Text("text.no_layers",
                                             "(no non-empty UI layer found)"));
        }
        Separator(ui);
    }

    const std::array<std::pair<std::uint32_t, std::string>, 3> kTabs{{
        {ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE,
         context->localizer.Text("category.clickable", "Clickable")},
        {ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_BLOCKED,
         context->localizer.Text("category.blocked", "Not clickable")},
        {ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN,
         context->localizer.Text("category.hidden_offscreen", "Hidden / off-screen")},
    }};
    const bool tabs = UI_HAS(ui, begin_tab_bar) && UI_HAS(ui, begin_tab_item) &&
        UI_HAS(ui, end_tab_item) && UI_HAS(ui, end_tab_bar);
    if (tabs && ui->begin_tab_bar(ui->user, anomaly::sdk::StringView("categories"),
                                  ANOMALY_UI_TAB_BAR_V1_NONE) != 0) {
        for (const auto& [category, name] : kTabs) {
            const std::string label = std::string(name) + " (" +
                std::to_string(snapshot->counts[category]) + ")###tab" + std::to_string(category);
            if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(label), nullptr,
                                   ANOMALY_UI_TAB_ITEM_V1_NONE, 1) != 0) {
                DrawRows(*context, ui, *snapshot, category, filter);
                ui->end_tab_item(ui->user);
            }
        }
        ui->end_tab_bar(ui->user);
    } else if (!tabs) {
        for (const auto& [category, name] : kTabs) {
            Text(ui, std::string(name));
            DrawRows(*context, ui, *snapshot, category, filter);
            Separator(ui);
        }
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView(kPluginId),
        anomaly::sdk::StringView("UI Buttons"),
        anomaly::sdk::StringView("Anomaly"),
        anomaly::sdk::StringView(kPluginVersion),
        Load,
        Start,
        Stop,
        Unload,
        Update,
        Draw,
    };
    return anomaly::sdk::Ok();
}
