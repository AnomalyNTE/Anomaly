#pragma once
// 直接调用刷新函数：不经 UI，按对象名在注册表里定位 UFunction，再用 ProcessEvent 调用。
//
// 需要 ue_access.hpp 先包含。全部逻辑只在 Game 回调域（on_update）执行；ProcessEvent 回调里
// 只记名字 id 与对象地址，任何反射解析都留到 on_update 做，避免在宿主持锁的回调里再进宿主。

namespace {

constexpr std::string_view kUeDefaultFunction = "FinalTowerCampResetMonsterSpawners";
constexpr std::size_t kUeFunctionNameCapacity = 96;
// 每个 tick 遍历的注册表槽位数：一趟全量扫描分若干 tick 完成。
constexpr std::uint32_t kUeScanBudget = 16384;
// 一次批量读取的槽位数（4096 * 24 = 96 KB）。
constexpr std::uint32_t kUeRunSlots = 4096;
// 事件记录容量。
constexpr std::size_t kUeEventCapacity = 96;
constexpr std::size_t kUeEventNameCapacity = 96;

enum class UeProbePhase : std::uint8_t { idle, finding_function, finding_owner, ready, failed };

struct UeEventRecord {
    std::uint32_t function_id{};
    std::uintptr_t object{};
    std::uint32_t count{};
    bool resolved{};
    char function_name[kUeEventNameCapacity]{};
    char class_name[kUeEventNameCapacity]{};
};

// --- 探测状态（on_update 独占；三个 handoff 标志由 on_draw 置位）---
std::atomic_bool g_ue_probe_requested{};
std::atomic_bool g_ue_call_requested{};
std::atomic_bool g_ue_clear_log_requested{};
std::atomic_bool g_ue_record_requested{};

UeProbePhase g_ue_phase{UeProbePhase::idle};
std::uint32_t g_ue_name_id{};
bool g_ue_name_id_ready{};
std::uint32_t g_ue_cursor{};
std::uint32_t g_ue_total{};
std::uintptr_t g_ue_chunk{};
std::uint32_t g_ue_chunk_index{0xFFFFFFFFu};
std::vector<std::uint8_t> g_ue_run_buffer;
std::vector<std::uintptr_t> g_ue_function_hits;
std::uintptr_t g_ue_function{};
std::uintptr_t g_ue_owner_class{};
std::uintptr_t g_ue_instance{};
bool g_ue_instance_is_cdo{};
std::uint32_t g_ue_num_parms{};
std::uint32_t g_ue_parms_size{};

std::mutex g_ue_mutex;
std::string g_ue_function_name{kUeDefaultFunction};
std::string g_ue_status{"未探测"};
std::array<char, kUeFunctionNameCapacity> g_ue_name_text{};
std::atomic_bool g_ue_text_stale{true};
std::string g_ue_call_result;

UeEventRecord g_ue_events[kUeEventCapacity];
std::size_t g_ue_event_count{};
std::uint64_t g_ue_event_total{};
std::atomic_bool g_ue_recording{};

// --- 事件记录（ProcessEvent 回调；只做整数比较与指针记录）---
void ANOMALY_CALL UeObserve(void*, std::uintptr_t object, std::uintptr_t function, void*) {
    if (!g_ue_recording.load() || function == 0) return;
    UeFName name{};
    const auto* core = g_context.core;
    if (core == nullptr || core->read_memory == nullptr ||
        core->read_memory(core->user, function + kUeObjectNameOffset,
                          {reinterpret_cast<std::uint8_t*>(&name), sizeof(name)}).code !=
            ANOMALY_STATUS_V1_OK) {
        return;
    }
    std::scoped_lock lock(g_ue_mutex);
    ++g_ue_event_total;
    for (std::size_t index = 0; index < g_ue_event_count; ++index) {
        if (g_ue_events[index].function_id != name.id ||
            g_ue_events[index].object != object) {
            continue;
        }
        ++g_ue_events[index].count;
        return;
    }
    if (g_ue_event_count >= kUeEventCapacity) return;
    auto& record = g_ue_events[g_ue_event_count++];
    record.function_id = name.id;
    record.object = object;
    record.count = 1;
}

AnomalyGenerationHandleV1 g_ue_event_subscription{};

// --- 状态文本 ---
void UePublish(std::string text) {
    std::scoped_lock lock(g_ue_mutex);
    g_ue_status = std::move(text);
}

std::string UeStatus() {
    std::scoped_lock lock(g_ue_mutex);
    return g_ue_status;
}

std::string UeFunctionName() {
    std::scoped_lock lock(g_ue_mutex);
    return g_ue_function_name;
}

void UeSetFunctionName(const std::string& name) {
    std::scoped_lock lock(g_ue_mutex);
    if (g_ue_function_name == name) return;
    g_ue_function_name = name;
}

std::string UeCallResult() {
    std::scoped_lock lock(g_ue_mutex);
    return g_ue_call_result;
}

void UeSetCallResult(std::string text) {
    std::scoped_lock lock(g_ue_mutex);
    g_ue_call_result = std::move(text);
}

void UeFinish(const std::string& text, const bool failed) {
    g_ue_phase = failed ? UeProbePhase::failed : UeProbePhase::ready;
    g_ue_cursor = 0;
    g_ue_chunk = 0;
    g_ue_chunk_index = 0xFFFFFFFFu;
    g_ue_run_buffer.clear();
    UePublish(text);
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 ue: " + text);
}

const char* UeParmsText() {
    static char text[64]{};
    std::snprintf(text, sizeof(text), "NumParms=%u ParmsSize=%u", g_ue_num_parms,
                  g_ue_parms_size);
    return text;
}

// 读取注册表里一段连续槽位，返回非空对象的 FName id 列表交由调用方比较。
bool UeScanRun(const UeReader& reader, const std::uint32_t run) {
    if (g_ue_run_buffer.size() < static_cast<std::size_t>(run) * 24) {
        g_ue_run_buffer.resize(static_cast<std::size_t>(run) * 24);
    }
    const auto* core = g_context.core;
    if (core == nullptr || core->read_memory == nullptr) return false;
    const auto within = g_ue_cursor % 65536;
    if (g_ue_chunk == 0 || g_ue_cursor / 65536 != g_ue_chunk_index) {
        if (!reader.Read(g_ue_registry + 16 + 8 * (g_ue_cursor / 65536), g_ue_chunk) ||
            g_ue_chunk == 0) {
            return false;
        }
        g_ue_chunk_index = g_ue_cursor / 65536;
    }
    if (core->read_memory(
            core->user, g_ue_chunk + 24u * within,
            {g_ue_run_buffer.data(), static_cast<std::size_t>(run) * 24}).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    for (std::uint32_t index = 0; index < run; ++index) {
        std::uintptr_t object{};
        std::memcpy(&object, g_ue_run_buffer.data() + static_cast<std::size_t>(index) * 24,
                    sizeof(object));
        if (object == 0) continue;
        UeFName name{};
        if (!reader.Read(object + kUeObjectNameOffset, name)) continue;
        if (g_ue_phase == UeProbePhase::finding_function) {
            if (name.id != g_ue_name_id) continue;
            if (g_ue_function_hits.size() < 8) g_ue_function_hits.push_back(object);
            continue;
        }
        // finding_owner：按 ClassPrivate 匹配目标类，分别记下第一个实例与 CDO。
        std::uintptr_t cls{};
        if (!reader.Read(object + kUeObjectClassOffset, cls) || cls != g_ue_owner_class) continue;
        const std::string short_name = reader.Name(name);
        if (short_name.rfind("Default__", 0) == 0) {
            if (g_ue_instance == 0 || g_ue_instance_is_cdo) {
                g_ue_instance = object;
                g_ue_instance_is_cdo = true;
            }
            continue;
        }
        if (g_ue_instance_is_cdo || g_ue_instance == 0) {
            g_ue_instance = object;
            g_ue_instance_is_cdo = false;
        }
    }
    return true;
}

void UeBeginProbe() {
    g_ue_function_hits.clear();
    g_ue_function = 0;
    g_ue_owner_class = 0;
    g_ue_instance = 0;
    g_ue_instance_is_cdo = false;
    g_ue_num_parms = 0;
    g_ue_parms_size = 0;
    g_ue_cursor = 0;
    g_ue_total = 0;
    g_ue_chunk = 0;
    g_ue_chunk_index = 0xFFFFFFFFu;
    g_ue_name_id = 0;
    g_ue_name_id_ready = false;

    const std::string name = UeFunctionName();
    if (name.empty()) {
        UeFinish("函数名为空", true);
        return;
    }
    const auto* names = g_context.names;
    if (names == nullptr || names->find_utf8 == nullptr) {
        UeFinish("名称服务不可用（需要 ue5-names）", true);
        return;
    }
    if (names->find_utf8(names->user, StringView(name), &g_ue_name_id).code !=
            ANOMALY_STATUS_V1_OK ||
        g_ue_name_id == 0) {
        UeFinish("名称池里没有「" + name + "」——该名字在当前会话未注册", true);
        return;
    }
    UeReader reader{g_context.names};
    if (!UeEnsureRegistry() || !UeRegistryCount(reader, g_ue_total) || g_ue_total == 0) {
        UeFinish("对象注册表不可用", true);
        return;
    }
    g_ue_name_id_ready = true;
    g_ue_phase = UeProbePhase::finding_function;
    UePublish("正在扫描对象注册表（" + std::to_string(g_ue_total) + " 槽）…");
}

void UeFinishProbe(const UeReader& reader) {
    if (g_ue_function_hits.empty()) {
        UeFinish("注册表里找不到对象「" + UeFunctionName() + "」", true);
        return;
    }
    g_ue_function = g_ue_function_hits.front();
    std::uint8_t num_parms{};
    std::uint16_t parms_size{};
    reader.Read(g_ue_function + kUeFunctionNumParmsOffset, num_parms);
    reader.Read(g_ue_function + kUeFunctionParmsSizeOffset, parms_size);
    g_ue_num_parms = num_parms;
    g_ue_parms_size = parms_size;

    std::uintptr_t owner{};
    reader.Read(g_ue_function + kUeObjectOuterOffset, owner);
    g_ue_owner_class = owner;
    const std::string class_name =
        owner != 0 ? reader.NameAt(owner + kUeObjectNameOffset) : std::string("(未知)");

    // 第二趟：在同一批槽位里找该类的实例（优先非 CDO）。
    g_ue_owner_class = owner;
    g_ue_instance = 0;
    g_ue_instance_is_cdo = false;
    g_ue_cursor = 0;
    g_ue_chunk = 0;
    g_ue_chunk_index = 0xFFFFFFFFu;
    g_ue_phase = UeProbePhase::finding_owner;
    UePublish("找到函数对象 ×" + std::to_string(g_ue_function_hits.size()) + "，所属类 " +
              class_name + "，" + UeParmsText() + "；正在找实例…");
}

void UeFinishOwnerScan() {
    UeReader reader{g_context.names};
    std::string text = "函数「" + UeFunctionName() + "」→ 类 ";
    text += g_ue_owner_class != 0 ? reader.NameAt(g_ue_owner_class + kUeObjectNameOffset)
                                  : std::string("(未知)");
    text += "，";
    text += UeParmsText();
    if (g_ue_instance != 0) {
        text += "，实例 " + std::string(g_ue_instance_is_cdo ? "CDO" : "已找到") + "，可调用";
    } else {
        text += "，未找到该类的实例（可能只在界面打开时创建）";
    }
    UeFinish(text, false);
}

void UeCallFunction() {
    if (g_ue_phase != UeProbePhase::ready || g_ue_function == 0 || g_ue_instance == 0) {
        UeSetCallResult("请先探测到一个可调用的实例");
        return;
    }
    if (g_ue_parms_size > 4096) {
        UeSetCallResult("ParmsSize 异常（" + std::to_string(g_ue_parms_size) + "），已拒绝调用");
        return;
    }
    std::vector<std::uint8_t> parms(g_ue_parms_size, 0);
    UeReader reader{g_context.names};
    if (!UeEnsureRegistry() || g_context.core == nullptr) {
        UeSetCallResult("对象注册表不可用");
        return;
    }
    const bool invoked = UeInvoke(reader, g_ue_instance, g_ue_function,
                                 g_ue_parms_size != 0 ? parms.data() : nullptr);
    if (!invoked) {
        UeSetCallResult("调用失败（ProcessEvent 抛异常或 vtable 不可执行）");
        Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 ue call failed");
        return;
    }
    std::string detail = "已调用 " + UeFunctionName() + "（" + UeParmsText() +
        (g_ue_instance_is_cdo ? "，对象 CDO" : "，对象实例") + "）";
    if (g_ue_parms_size != 0) {
        detail += "，返回 ";
        for (std::uint32_t index = 0; index < g_ue_parms_size && index < 16; ++index) {
            char item[8]{};
            std::snprintf(item, sizeof(item), "%02X ", parms[index]);
            detail += item;
        }
    }
    UeSetCallResult(detail);
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, std::string("night999 ue call: ") + detail);
}

// 把事件记录里未解析的条目补上名字（只在 on_update 调用，每 tick 限量）。
void UeResolveEvents(const UeReader& reader) {
    if (!g_ue_recording.load()) return;
    std::scoped_lock lock(g_ue_mutex);
    int budget = 6;
    for (std::size_t index = 0; index < g_ue_event_count && budget > 0; ++index) {
        auto& record = g_ue_events[index];
        if (record.resolved || record.function_id == 0) continue;
        const std::string name = reader.Name(UeFName{record.function_id, 0});
        if (name.empty()) continue;
        std::snprintf(record.function_name, sizeof(record.function_name), "%s", name.c_str());
        std::uintptr_t cls{};
        if (record.object != 0 && reader.Read(record.object + kUeObjectClassOffset, cls) &&
            cls != 0) {
            const std::string own = reader.NameAt(cls + kUeObjectNameOffset);
            std::snprintf(record.class_name, sizeof(record.class_name), "%s", own.c_str());
        } else {
            std::snprintf(record.class_name, sizeof(record.class_name), "%s", "(未知)");
        }
        record.resolved = true;
        --budget;
    }
}

void UeAdvance(const UeReader& reader) {
    if (g_ue_clear_log_requested.exchange(false)) {
        std::scoped_lock lock(g_ue_mutex);
        std::memset(g_ue_events, 0, sizeof(g_ue_events));
        g_ue_event_count = 0;
        g_ue_event_total = 0;
    }
    if (g_ue_record_requested.exchange(false) && g_context.events != nullptr &&
        g_context.events->subscribe != nullptr) {
        if (g_ue_event_subscription.id != 0 && g_context.events->unsubscribe != nullptr) {
            g_context.events->unsubscribe(g_context.events->user, g_ue_event_subscription);
            g_ue_event_subscription = {};
        }
        {
            std::scoped_lock lock(g_ue_mutex);
            std::memset(g_ue_events, 0, sizeof(g_ue_events));
            g_ue_event_count = 0;
            g_ue_event_total = 0;
        }
        AnomalyGenerationHandleV1 handle{};
        if (g_context.events->subscribe(g_context.events->user, UeObserve, nullptr, &handle).code ==
                ANOMALY_STATUS_V1_OK &&
            handle.id != 0) {
            g_ue_event_subscription = handle;
            UePublish("已开始记录 ProcessEvent（去重，最多 96 条）");
        } else {
            UePublish("事件订阅失败（需要 game-events）");
        }
    }
    UeResolveEvents(reader);

    if (g_ue_probe_requested.exchange(false) && g_ue_phase != UeProbePhase::finding_function &&
        g_ue_phase != UeProbePhase::finding_owner) {
        UeBeginProbe();
    }
    if (g_ue_call_requested.exchange(false)) UeCallFunction();

    if (g_ue_phase != UeProbePhase::finding_function && g_ue_phase != UeProbePhase::finding_owner) {
        return;
    }
    if (!UeEnsureRegistry()) {
        UeFinish("对象注册表不可用", true);
        return;
    }
    if (!g_ue_name_id_ready) {
        UeBeginProbe();
        return;
    }
    // 分片遍历：每 tick 最多 kUeScanBudget 个槽位。
    std::uint32_t work{};
    while (work < kUeScanBudget && g_ue_cursor < g_ue_total) {
        const std::uint32_t run = std::min(
            {kUeRunSlots, kUeScanBudget - work, 65536u - (g_ue_cursor % 65536)});
        if (run == 0) break;
        if (!UeScanRun(reader, run)) {
            UeFinish("扫描中断（注册表读取失败）", true);
            return;
        }
        g_ue_cursor += run;
        work += run;
    }
    if (g_ue_cursor < g_ue_total) return;

    if (g_ue_phase == UeProbePhase::finding_function) {
        UeFinishProbe(reader);
    } else {
        UeFinishOwnerScan();
    }
}

void UeStop() noexcept {
    g_ue_probe_requested.store(false);
    g_ue_call_requested.store(false);
    g_ue_record_requested.store(false);
    g_ue_clear_log_requested.store(false);
    g_ue_phase = UeProbePhase::idle;
    g_ue_cursor = 0;
    g_ue_total = 0;
    g_ue_chunk = 0;
    g_ue_chunk_index = 0xFFFFFFFFu;
    g_ue_run_buffer.clear();
    g_ue_chunk = 0;
    g_ue_recording.store(false);
    if (g_context.events != nullptr && g_ue_event_subscription.id != 0 &&
        g_context.events->unsubscribe != nullptr) {
        g_context.events->unsubscribe(g_context.events->user, g_ue_event_subscription);
    }
    g_ue_event_subscription = {};
}

void UeSyncText() {
    if (!g_ue_text_stale.exchange(false)) return;
    const std::string name = UeFunctionName();
    std::snprintf(g_ue_name_text.data(), g_ue_name_text.size(), "%s", name.c_str());
}

void UeContent(const AnomalyUiServiceV1* ui) {
    UeSyncText();

    DrawText(ui, g_context.localizer.Text("ue.title", "直接调用（不经 UI）"));
    ui->text(ui->user, StringView("函数名"));
    if (ui->same_line != nullptr) ui->same_line(ui->user, 96.0F, 8.0F);
    if (ui->input_text != nullptr) {
        ui->input_text(ui->user, StringView("##night999-ue-fn"), g_ue_name_text.data(),
                       g_ue_name_text.size(), ANOMALY_UI_TEXT_INPUT_V1_NONE);
    } else {
        DrawText(ui, g_ue_name_text.data());
    }
    UeSetFunctionName(g_ue_name_text.data());

    if (ui->button != nullptr) {
        if (ui->button(ui->user, StringView("探测"), 0.0F, 0.0F) != 0) {
            g_ue_probe_requested.store(true);
        }
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        const int enabled = g_ue_phase == UeProbePhase::ready && g_ue_instance != 0 ? 1 : 0;
        const int pressed = ui->button_enabled != nullptr
            ? ui->button_enabled(ui->user, StringView("直接调用"), 0.0F, 0.0F, enabled)
            : (enabled && ui->button(ui->user, StringView("直接调用"), 0.0F, 0.0F) != 0);
        if (pressed != 0) g_ue_call_requested.store(true);
    }
    DrawText(ui, UeStatus());
    const std::string call = UeCallResult();
    if (!call.empty()) DrawText(ui, call);

    if (ui->checkbox != nullptr) {
        int recording = g_ue_recording.load() ? 1 : 0;
        if (ui->checkbox(ui->user, StringView("记录 ProcessEvent（用于找出按钮真正调的函数）"),
                         &recording) != 0) {
            g_ue_recording.store(recording != 0);
            if (recording != 0) g_ue_record_requested.store(true);
        }
    }
    if (g_ue_recording.load()) {
        if (ui->button != nullptr &&
            ui->button(ui->user, StringView("清空记录"), 0.0F, 0.0F) != 0) {
            g_ue_clear_log_requested.store(true);
        }
        std::scoped_lock lock(g_ue_mutex);
        DrawText(ui, "共记录 " + std::to_string(g_ue_event_total) + " 次调用，" +
                         std::to_string(g_ue_event_count) + " 个不同（函数 × 对象）：");
        for (std::size_t index = 0; index < g_ue_event_count && index < 12; ++index) {
            const auto& record = g_ue_events[index];
            DrawText(ui, std::string(record.resolved ? record.function_name : "(解析中)") +
                             "  ←  " + (record.resolved ? record.class_name : "?") +
                             "  ×" + std::to_string(record.count));
        }
    }
}

}  // namespace
