// UI 按钮调试面板：anomaly.nte.ui-buttons 服务的消费者。
//
// 扫描、可点击判定、遮挡判定、鼠标拾取和点击全部由 Host 完成；插件只提交请求、轮询请求状态，
// 并把 Host 发布的按钮目录复制成面板快照。
// 线程边界：服务调用都在 Game 域 on_update；on_draw 只读快照并把点击意图写进队列。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/ui.h"

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

constexpr std::string_view kPluginId = "local.ui-buttons";
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
    std::string status{"尚未扫描"};
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

std::string_view CategoryName(std::uint32_t category) noexcept {
    switch (category) {
    case ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE: return "可点击";
    case ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_BLOCKED: return "不可点击";
    case ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN: return "隐藏";
    default: return "?";
    }
}

std::string_view ResultName(std::uint32_t status) noexcept {
    switch (status) {
    case ANOMALY_STATUS_V1_OK: return "已调用";
    case ANOMALY_STATUS_V1_CONFLICT: return "不可点击";
    case ANOMALY_STATUS_V1_NOT_FOUND: return "按钮已失效";
    case ANOMALY_STATUS_V1_CANCELLED: return "已取消";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "服务不可用";
    case ANOMALY_STATUS_V1_FAILED: return "游戏未接受";
    default: return "失败";
    }
}

std::string DescribeReasons(std::uint32_t reasons) {
    static constexpr std::array<std::pair<std::uint32_t, const char*>, 13> kNames{{
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_SELF, "自身隐藏"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_COLLAPSED_ANCESTOR, "父控件隐藏"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_TRANSPARENT, "完全透明"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_INACTIVE_PAGE, "非当前页"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_IN_VIEWPORT, "界面未显示"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_DETACHED, "未挂到界面"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_OCCLUDED, "被其他界面遮挡"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_CLOSING, "界面关闭中"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_HIT_TESTABLE, "不接受点击"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_DISABLED, "禁用"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_NOT_INTERACTABLE, "不可交互"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_LOCKED, "锁定"},
        {ANOMALY_NTE_UI_BUTTON_REASON_V1_QUERY_FAILED, "查询失败"},
    }};
    std::string text;
    for (const auto& [bit, name] : kNames) {
        if ((reasons & bit) == 0) continue;
        if (!text.empty()) text += " / ";
        text += name;
    }
    return text;
}

Row MakeRow(const AnomalyNteUiButtonSnapshotV1& button) {
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
    row.reasons = DescribeReasons(button.reasons);
    if (button.cause[0] != '\0') row.reasons += std::string("（") + button.cause + "）";
    row.search = Lower(row.name + '\n' + row.text + '\n' + row.window + '\n' + row.class_name);
    return row;
}

std::string Label(const Row& row) {
    return row.name + (row.text.empty() ? "" : "「" + row.text + "」");
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
        SetStatus(context, "请求被拒绝：" +
                               std::string(result.message.data == nullptr
                                               ? std::string_view{}
                                               : std::string_view(result.message.data,
                                                                  result.message.size)));
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
        snapshot->rows.push_back(MakeRow(button));
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
        const std::array<std::pair<std::uint32_t, const char*>, 7> flags{{
            {ANOMALY_NTE_UI_WINDOW_V1_ACTIVE, "激活"},
            {ANOMALY_NTE_UI_WINDOW_V1_CLOSING, "关闭中"},
            {ANOMALY_NTE_UI_WINDOW_V1_MODAL, "模态"},
            {ANOMALY_NTE_UI_WINDOW_V1_HIDES_MAIN_FORM, "隐藏主界面"},
            {ANOMALY_NTE_UI_WINDOW_V1_PAUSES_GAME, "暂停游戏"},
            {ANOMALY_NTE_UI_WINDOW_V1_MENU_INPUT, "菜单输入"},
            {ANOMALY_NTE_UI_WINDOW_V1_BLOCKING, "→ 阻挡下层"},
        }};
        if ((window.flags & ANOMALY_NTE_UI_WINDOW_V1_VISIBLE) == 0) layer.flags = "不可见";
        for (const auto& [bit, name] : flags) {
            if ((window.flags & bit) == 0) continue;
            if (!layer.flags.empty()) layer.flags += " ";
            layer.flags += name;
        }
        snapshot->layers.push_back(std::move(layer));
    }
    std::stable_sort(snapshot->layers.begin(), snapshot->layers.end(),
                     [](const LayerRow& a, const LayerRow& b) { return a.showing > b.showing; });
    // 遮挡判定偶发误判时，日志里要能看到当时是哪些界面在挡、挡住了多少按钮。
    std::string occlusion;
    for (const auto& layer : snapshot->layers) {
        if (layer.flags.find("阻挡下层") == std::string::npos) continue;
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
    char summary[256]{};
    std::snprintf(summary, sizeof(summary),
                  "目录 #%llu：对象 %u，界面层 %u，按钮 %u（可点 %u / 不可点 %u / 隐藏 %u），"
                  "游戏调用 %u 次，%u tick，%u ms%s",
                  static_cast<unsigned long long>(status.catalog_sequence),
                  status.objects_scanned, status.window_count, status.button_count,
                  status.clickable_count, status.blocked_count, status.hidden_count,
                  status.process_event_calls, status.scan_ticks, status.scan_milliseconds,
                  (status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_TRUNCATED) != 0 ? "（已截断）"
                                                                               : "");
    context.shown_catalog = status.catalog_sequence;
    std::scoped_lock lock(context.mutex);
    context.snapshot = std::move(snapshot);
    context.progress = summary;
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
            pick->hits.push_back(MakeRow(hit));
        }
    }
    std::string log = "pick: checked " + std::to_string(pick->checked) + " buttons, " +
        std::to_string(pick->hits.size()) + " hovered";
    for (const auto& row : pick->hits) {
        log += "\n  " + Label(row) + " [" + std::string(CategoryName(row.category)) +
            (row.reasons.empty() ? "" : "：" + row.reasons) + "] " + row.path +
            " class=" + row.class_name;
    }
    if (!pick->error.empty()) log += " error=" + pick->error;
    Log(context, log);
    if (!pick->error.empty()) {
        SetStatus(context, "拾取失败：" + pick->error);
    } else if (pick->hits.empty()) {
        SetStatus(context, "拾取完成：光标下没有找到按钮");
    } else {
        const auto& top = pick->hits.front();
        SetStatus(context, "拾取完成：" + Label(top) + "（" +
                               std::string(CategoryName(top.category)) + "）");
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
                SetStatus(context, "拾取中：请保持鼠标不动…");
            }
            remaining.push_back(pending);
            continue;
        }
        if (pending.kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_PICK) {
            FinishPick(context, pending, done);
        } else if (pending.kind == ANOMALY_NTE_UI_BUTTON_REQUEST_V1_SCAN) {
            if (done.status != ANOMALY_STATUS_V1_OK) {
                SetStatus(context, std::string("扫描失败：") + done.detail);
            }
        } else {
            std::string message = std::string(ResultName(done.status)) + "：" + pending.label;
            if (pending.force) message += " [强制]";
            if (done.detail[0] != '\0') message += std::string("（") + done.detail + "）";
            if (done.reasons != 0) message += "；" + DescribeReasons(done.reasons);
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

std::string KeyName(std::uint32_t key) {
    if (key == 0) return "未设置";
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
    if (!InputReady(context.input)) return "输入服务不可用，快捷键无效";
    if (!ValidPickKey(key)) return "不支持这个键";
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
        return status.code == ANOMALY_STATUS_V1_CONFLICT
            ? KeyName(key) + " 已被其他功能占用"
            : KeyName(key) + " 注册失败";
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
        SetStatus(context, "已取消修改拾取快捷键");
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
        SetStatus(context, error.empty() ? "拾取快捷键改为 " + KeyName(key) : error);
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
    if (!error.empty()) SetStatus(*context, "拾取快捷键：" + error);
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
        SetStatus(*context, "3 秒后拾取：把鼠标移到游戏里的按钮上");
    }
    if (context->delayed_pick_pending && now >= context->pick_at) {
        context->delayed_pick_pending = false;
        context->pick_requested.store(true, std::memory_order_release);
    }

    if (!EnsureService(*context)) {
        context->pending.clear();
        SetStatus(*context, "anomaly.nte.ui-buttons 服务不可用（Profile 或游戏状态未就绪）");
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
    intent.label = Label(row);
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
        const std::string label = "点击##" + std::to_string(index);
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
        Text(ui, "仅显示前 " + std::to_string(shown) + " 条（共 " + std::to_string(matched) +
                     " 条匹配），请用过滤缩小范围");
    } else if (matched == 0) {
        Text(ui, "（无）");
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
    anomaly::sdk::UiWindow window(ui, "UI 按钮###ui-buttons", &open);
    if (!window) return;

    if (Button(ui, "扫描按钮")) context->scan_requested.store(true, std::memory_order_release);
    SameLine(ui);
    int auto_refresh = context->auto_refresh.load(std::memory_order_acquire) ? 1 : 0;
    Checkbox(ui, "自动刷新", auto_refresh);
    context->auto_refresh.store(auto_refresh != 0, std::memory_order_release);
    if (UI_HAS(ui, input_uint32)) {
        SameLine(ui);
        if (ui->input_uint32(ui->user, anomaly::sdk::StringView("间隔(ms)"),
                             &context->interval_input, 100, 1000) != 0) {
            context->interval_input = std::clamp<std::uint32_t>(context->interval_input, 200,
                                                                60000);
            context->auto_interval_ms.store(context->interval_input,
                                            std::memory_order_release);
        }
    }

    Text(ui, "点击方式：按下 → 抬起 → 点击（由 Host 在点击前重新判定是否可点）");
    Checkbox(ui, "强制点击（忽略可点击判定，被遮挡的按钮也会点，可能让游戏进入异常状态）",
             context->force);
    Checkbox(ui, "显示 UMG 原生按钮", context->show_umg);
    SameLine(ui);
    Checkbox(ui, "显示界面层", context->show_layers);

    // 拾取：鼠标停在游戏里的按钮上按快捷键，识别 Slate 认为正悬停的按钮。
    Text(ui, "按钮拾取：");
    SameLine(ui);
    if (context->capturing_pick_key.load(std::memory_order_acquire)) {
        if (Button(ui, "按下新的拾取键…（Esc 取消）##pick-key")) {
            context->capturing_pick_key.store(false, std::memory_order_release);
        }
    } else if (Button(ui, "快捷键：" + KeyName(context->pick_key.load(std::memory_order_acquire)) +
                              "##pick-key")) {
        context->capturing_pick_key.store(true, std::memory_order_release);
    }
    SameLine(ui);
    if (Button(ui, "3 秒后拾取##pick-delay")) {
        context->delayed_pick_requested.store(true, std::memory_order_release);
    }

    if (UI_HAS(ui, input_text)) {
        ui->input_text(ui->user, anomaly::sdk::StringView("过滤（名称 / 文本 / 窗口 / 类）"),
                       context->filter.data(), context->filter.size(),
                       ANOMALY_UI_TEXT_INPUT_V1_NONE);
    }
    const std::string filter = Lower(std::string_view(context->filter.data()));

    Text(ui, status);
    if (!progress.empty()) Text(ui, progress);
    Separator(ui);

    if (pick) {
        Text(ui, "拾取结果 #" + std::to_string(pick->sequence) + "（检查 " +
                     std::to_string(pick->checked) + " 个按钮，游戏调用 " +
                     std::to_string(pick->calls) + " 次）：");
        if (!pick->error.empty()) Text(ui, "  失败：" + pick->error);
        if (pick->hits.empty() && pick->error.empty()) {
            Text(ui, "  光标下没有按钮（或按键时鼠标在插件窗口上）");
        }
        for (std::size_t i = 0; i < pick->hits.size(); ++i) {
            const auto& row = pick->hits[i];
            const bool enabled =
                row.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE || context->force != 0;
            if (Button(ui, "点击##pick" + std::to_string(i), enabled)) QueueClick(*context, row);
            SameLine(ui);
            Text(ui, std::string(i == 0 ? "▶ " : "  ") + Label(row) + "  [" +
                         std::string(CategoryName(row.category)) +
                         (row.reasons.empty() ? "" : "：" + row.reasons) + "]");
            Text(ui, "      " + row.path + "  (" + std::string(KindName(row.kind)) + " " +
                         row.class_name + ")");
        }
        if (pick->hits.size() > 1) Text(ui, "  ▶ 为最内层的按钮；其余是外层同样处于悬停的按钮");
        Separator(ui);
    }

    if (!snapshot) {
        Text(ui, "点「扫描按钮」开始。");
        return;
    }

    if (context->show_layers != 0) {
        Text(ui, "界面层（扫描时刻）：");
        for (const auto& layer : snapshot->layers) {
            Text(ui, std::string(layer.showing ? "● " : "○ ") + layer.layer + " → " +
                         layer.window + (layer.flags.empty() ? "" : "  [" + layer.flags + "]"));
        }
        if (snapshot->layers.empty()) Text(ui, "（没有找到非空的界面层）");
        Separator(ui);
    }

    const std::array<std::pair<std::uint32_t, std::string_view>, 3> kTabs{{
        {ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE, "可点击"},
        {ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_BLOCKED, "不可点击"},
        {ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_HIDDEN, "隐藏 / 不在屏幕"},
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
