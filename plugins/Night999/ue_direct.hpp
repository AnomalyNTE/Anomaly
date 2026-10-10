#pragma once
// 直接调用：按名称关键词在对象注册表里定位 UObject / UFunction，再用 ProcessEvent 调用。
//
// 定位不依赖完整对象路径：先用 ue5.names.find_utf8 把关键词换成 FName id，能命中就走
// 「整数比较」快路径；命中不了（说明该名字在当前会话未注册，例如蓝图还没加载）就退化为
// 遍历注册表逐个解析名字做子串匹配。
//
// 需要 ue_access.hpp 先包含。扫描与调用都只在 Game 回调域（on_update）执行；ProcessEvent
// 回调里只记 FName id 与对象地址，任何反射解析都留到 on_update 做，避免在宿主持锁的回调里
// 再次进入宿主。

namespace {

constexpr std::string_view kUeDefaultKeyword = "ResetMonster";
constexpr std::size_t kUeTextCapacity = 96;
// 匹配列表上限；超过只报总数。
constexpr std::size_t kUeMatchCapacity = 16;
// 快路径（整数比较）每 tick 遍历的槽位数。
constexpr std::uint32_t kUeSlotBudget = 16384;
// 慢路径（逐个解析名字）每 tick 解析的存活对象数上限。
constexpr std::uint32_t kUeResolveBudget = 1024;
// 一次批量读取的槽位数（4096 * 24 = 96 KB）。
constexpr std::uint32_t kUeRunSlots = 4096;
// 事件记录容量。
constexpr std::size_t kUeEventCapacity = 96;

enum class UeProbePhase : std::uint8_t { idle, scanning, finding_owner, ready, failed };

struct UeMatch {
    std::uintptr_t object{};
    std::uintptr_t owner{};
    bool is_function{};
    std::uint32_t num_parms{};
    std::uint32_t parms_size{};
    char name[kUeTextCapacity]{};
    char class_name[kUeTextCapacity]{};
};

struct UeEventRecord {
    std::uint32_t function_id{};
    std::uintptr_t object{};
    std::uint32_t count{};
    bool resolved{};
    char function_name[kUeTextCapacity]{};
    char class_name[kUeTextCapacity]{};
};

// --- handoff 标志（on_draw 置位）---
std::atomic_bool g_ue_probe_requested{};
std::atomic_bool g_ue_call_requested{};
std::atomic_bool g_ue_clear_log_requested{};
std::atomic_bool g_ue_record_requested{};
std::atomic_int g_ue_select_requested{-1};

// --- 扫描状态（on_update 独占）---
UeProbePhase g_ue_phase{UeProbePhase::idle};
bool g_ue_ids_only{};
// 本趟扫描用的关键词小写副本（Game 线程独占，避免与 on_draw 的输入框写入竞争）。
std::string g_ue_keyword_scan;
std::uint32_t g_ue_name_id{};
std::uint32_t g_ue_slots_this_tick{};
std::uint32_t g_ue_cursor{};
std::uint32_t g_ue_total{};
std::uint32_t g_ue_match_total{};
std::uintptr_t g_ue_chunk{};
std::uint32_t g_ue_chunk_index{0xFFFFFFFFu};
std::vector<std::uint8_t> g_ue_run_buffer;
std::uintptr_t g_ue_function{};
std::uintptr_t g_ue_owner_class{};
std::uintptr_t g_ue_instance{};
bool g_ue_instance_is_cdo{};
std::uint32_t g_ue_num_parms{};
std::uint32_t g_ue_parms_size{};

// --- 共享状态（on_draw 读，on_update 写）---
std::mutex g_ue_mutex;
std::string g_ue_keyword{kUeDefaultKeyword};
std::string g_ue_keyword_lower{kUeDefaultKeyword};
std::string g_ue_status{"未探测"};
std::array<char, kUeTextCapacity> g_ue_text{};
std::atomic_bool g_ue_text_stale{true};
std::string g_ue_call_result;
std::vector<UeMatch> g_ue_matches;
std::string g_ue_target_name;

UeEventRecord g_ue_events[kUeEventCapacity];
std::size_t g_ue_event_count{};
std::uint64_t g_ue_event_total{};
std::atomic_bool g_ue_recording{};
AnomalyGenerationHandleV1 g_ue_event_subscription{};

std::string UeLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a'
                                                                     : character);
    });
    return value;
}

// --- 事件记录回调：只做整数与指针操作 ---
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
        if (g_ue_events[index].function_id != name.id || g_ue_events[index].object != object) {
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

void UePublish(std::string text) {
    std::scoped_lock lock(g_ue_mutex);
    g_ue_status = std::move(text);
}

void UeSetCallResult(std::string text) {
    std::scoped_lock lock(g_ue_mutex);
    g_ue_call_result = std::move(text);
}

void UeCopyOut(std::string& status, std::string& call, std::string& target,
               std::vector<UeMatch>& matches, std::uint32_t& match_total) {
    std::scoped_lock lock(g_ue_mutex);
    status = g_ue_status;
    call = g_ue_call_result;
    target = g_ue_target_name;
    matches = g_ue_matches;
    match_total = g_ue_match_total;
}

void UeSetKeyword(const std::string& keyword) {
    std::scoped_lock lock(g_ue_mutex);
    if (g_ue_keyword == keyword) return;
    g_ue_keyword = keyword;
    g_ue_keyword_lower = UeLower(keyword);
}

std::string UeKeyword() {
    std::scoped_lock lock(g_ue_mutex);
    return g_ue_keyword;
}

const char* UeParmsText() {
    static char text[64]{};
    std::snprintf(text, sizeof(text), "NumParms=%u ParmsSize=%u", g_ue_num_parms,
                  g_ue_parms_size);
    return text;
}

void UeFinish(std::string text, const bool failed) {
    g_ue_phase = failed ? UeProbePhase::failed : UeProbePhase::ready;
    g_ue_cursor = 0;
    g_ue_slots_this_tick = 0;
    g_ue_chunk = 0;
    g_ue_chunk_index = 0xFFFFFFFFu;
    g_ue_run_buffer.clear();
    UePublish(std::move(text));
}

// 命中一个对象：补齐名字、类名与（若为 UFunction）参数信息。
void UeAddMatch(const UeReader& reader, const std::uintptr_t object, const std::string& name) {
    ++g_ue_match_total;
    if (g_ue_matches.size() >= kUeMatchCapacity) return;
    UeMatch match{};
    match.object = object;
    std::snprintf(match.name, sizeof(match.name), "%s", name.c_str());
    std::uintptr_t owner{};
    reader.Read(object + kUeObjectOuterOffset, owner);
    match.owner = owner;

    std::uintptr_t cls{};
    if (reader.Read(object + kUeObjectClassOffset, cls) && cls != 0) {
        const std::string class_name = reader.NameAt(cls + kUeObjectNameOffset);
        std::snprintf(match.class_name, sizeof(match.class_name), "%s", class_name.c_str());
        match.is_function = class_name == "Function";
    }
    if (match.is_function) {
        std::uint8_t num_parms{};
        std::uint16_t parms_size{};
        reader.Read(object + kUeFunctionNumParmsOffset, num_parms);
        reader.Read(object + kUeFunctionParmsSizeOffset, parms_size);
        match.num_parms = num_parms;
        match.parms_size = parms_size;
    }
    g_ue_matches.push_back(match);
}

// 读取一段连续槽位并匹配。返回是否成功读取。
bool UeScanRun(const UeReader& reader, const std::uint32_t run, std::uint32_t& resolved) {
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
    if (core->read_memory(core->user, g_ue_chunk + 24u * within,
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

        if (g_ue_ids_only) {
            // 快路径：名字已在池里，直接比 id，不解析字符串。
            if (name.id != g_ue_name_id) continue;
            UeAddMatch(reader, object, reader.Name(name));
            if (g_ue_matches.size() >= kUeMatchCapacity) return true;
            continue;
        }
        // 慢路径：名字没注册，只能逐个解析出来做子串匹配。
        if (resolved >= kUeResolveBudget) return true;
        ++resolved;
        const std::string text = reader.Name(name);
        if (text.empty()) continue;
        if (UeLower(text).find(g_ue_keyword_scan) == std::string::npos) continue;
        UeAddMatch(reader, object, text);
        if (g_ue_matches.size() >= kUeMatchCapacity) return true;
    }
    return true;
}

void UeBeginScan() {
    g_ue_matches.clear();
    g_ue_match_total = 0;
    g_ue_function = 0;
    g_ue_owner_class = 0;
    g_ue_instance = 0;
    g_ue_instance_is_cdo = false;
    g_ue_num_parms = 0;
    g_ue_parms_size = 0;
    g_ue_cursor = 0;
    g_ue_slots_this_tick = 0;
    g_ue_total = 0;
    g_ue_chunk = 0;
    g_ue_chunk_index = 0xFFFFFFFFu;
    g_ue_name_id = 0;
    g_ue_ids_only = false;
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_target_name.clear();
    }

    const std::string keyword = UeKeyword();
    if (keyword.empty()) {
        UeFinish("关键词为空", true);
        return;
    }
    const auto* names = g_context.names;
    const auto* objects = g_context.objects;
    if (names == nullptr || names->find_utf8 == nullptr || objects == nullptr ||
        objects->snapshot_by_handle == nullptr) {
        UeFinish("需要 ue5-names / ue5-objects 服务", true);
        return;
    }
    const UeReader reader{names};
    if (!UeEnsureRegistry() || !UeRegistryCount(reader, g_ue_total) || g_ue_total == 0) {
        UeFinish("对象注册表不可用", true);
        return;
    }

    // 名字还在池里就走快路径；否则说明相关蓝图未加载，退化为子串扫描。
    if (names->find_utf8(names->user, StringView(keyword), &g_ue_name_id).code ==
            ANOMALY_STATUS_V1_OK &&
        g_ue_name_id != 0) {
        g_ue_ids_only = true;
        g_ue_phase = UeProbePhase::scanning;
        UePublish("名字已注册，按精确名称扫描 " + std::to_string(g_ue_total) + " 槽…");
        return;
    }
    g_ue_keyword_scan = UeLower(keyword);
    g_ue_ids_only = false;
    g_ue_phase = UeProbePhase::scanning;
    UePublish("名字未注册（相关蓝图可能还没加载），改为遍历注册表做子串匹配 " +
              std::to_string(g_ue_total) + " 槽…");
}

void UeFinishScan() {
    if (g_ue_matches.empty()) {
        UeFinish("注册表里没有任何对象名含「" + UeKeyword() + "」（共 " +
                     std::to_string(g_ue_total) + " 槽）——相关蓝图尚未加载。"
                     "请先在游戏里打开一次营地（或进入对应玩法）再点探测",
                 true);
        return;
    }
    // 函数排在前面，便于挑选。
    std::stable_partition(g_ue_matches.begin(), g_ue_matches.end(),
                          [](const UeMatch& match) { return match.is_function; });
    std::scoped_lock lock(g_ue_mutex);
    g_ue_status = "匹配 " + std::to_string(g_ue_match_total) + " 个对象（列出前 " +
        std::to_string(g_ue_matches.size()) + " 个，函数优先）：选一个按「选用」";
}

// 选中某个匹配：记录目标函数，并开始找它所属类的实例。
void UeBeginOwnerScan(const std::size_t index) {
    if (index >= g_ue_matches.size()) return;
    const UeMatch match = g_ue_matches[index];
    g_ue_function = match.object;
    g_ue_owner_class = match.owner;
    g_ue_num_parms = match.num_parms;
    g_ue_parms_size = match.parms_size;
    g_ue_instance = 0;
    g_ue_instance_is_cdo = false;
    g_ue_cursor = 0;
    g_ue_slots_this_tick = 0;
    g_ue_chunk = 0;
    g_ue_chunk_index = 0xFFFFFFFFu;
    UePublish("已选中 " + std::string(match.name) + "（类 " + match.class_name + "，" +
              UeParmsText() + "），正在找该类的实例…");
    if (!match.is_function) {
        g_ue_phase = UeProbePhase::ready;
        g_ue_function = 0;
        UePublish("已选中「" + std::string(match.name) + "」：它不是 UFunction（类 " +
                  match.class_name + "），不能这样调用");
        return;
    }
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_target_name = match.name;
    }
    g_ue_phase = UeProbePhase::finding_owner;
}

void UeFinishOwnerScan(const UeReader& reader) {
    std::string target;
    {
        std::scoped_lock lock(g_ue_mutex);
        target = g_ue_target_name;
    }
    std::string text = "目标「" + target + "」→ 类 ";
    text += g_ue_owner_class != 0 ? reader.NameAt(g_ue_owner_class + kUeObjectNameOffset)
                                  : std::string("(未知)");
    text += "，";
    text += UeParmsText();
    text += g_ue_instance != 0 ? "，实例已找到，可调用"
                               : "，未找到该类实例（该函数可能只在界面打开时才存在）";
    UeFinish(text, false);
}

void UeCallFunction() {
    if (g_ue_function == 0 || g_ue_instance == 0) {
        UeSetCallResult("请先探测并「选用」一个可调用的函数");
        return;
    }
    if (g_ue_parms_size > 4096) {
        UeSetCallResult("ParmsSize 异常（" + std::to_string(g_ue_parms_size) + "），已拒绝调用");
        return;
    }
    std::vector<std::uint8_t> parms(g_ue_parms_size, 0);
    const UeReader reader{g_context.names};
    if (!UeEnsureRegistry() || g_context.core == nullptr) {
        UeSetCallResult("对象注册表不可用");
        return;
    }
    std::string target;
    {
        std::scoped_lock lock(g_ue_mutex);
        target = g_ue_target_name;
    }
    const bool invoked = UeInvoke(reader, g_ue_instance, g_ue_function,
                                 g_ue_parms_size != 0 ? parms.data() : nullptr);
    if (!invoked) {
        UeSetCallResult("调用「" + target + "」失败（ProcessEvent 抛异常或 vtable 不可执行）");
        Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 ue call failed: " + target);
        return;
    }
    std::string detail = "已调用「" + target + "」（" + UeParmsText() +
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
            const std::string owner = reader.NameAt(cls + kUeObjectNameOffset);
            std::snprintf(record.class_name, sizeof(record.class_name), "%s", owner.c_str());
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

    const bool busy = g_ue_phase == UeProbePhase::scanning ||
        g_ue_phase == UeProbePhase::finding_owner;
    if (g_ue_probe_requested.exchange(false)) {
        UeBeginScan();
        return;
    }
    if (!busy) {
        if (g_ue_call_requested.exchange(false)) UeCallFunction();
        const int select = g_ue_select_requested.exchange(-1);
        if (select >= 0) UeBeginOwnerScan(static_cast<std::size_t>(select));
        return;
    }
    if (!UeEnsureRegistry()) {
        UeFinish("对象注册表不可用", true);
        return;
    }

    // 分片遍历：快路径按槽位计预算，慢路径按解析过的存活对象计预算。
    std::uint32_t resolved{};
    while (g_ue_cursor < g_ue_total) {
        if (!g_ue_ids_only && resolved >= kUeResolveBudget) break;
        const std::uint32_t run = std::min(
            {kUeRunSlots, 65536u - (g_ue_cursor % 65536)});
        if (run == 0) break;
        if (!UeScanRun(reader, run, resolved)) {
            UeFinish("扫描中断（注册表读取失败）", true);
            return;
        }
        g_ue_cursor += run;
        if (g_ue_matches.size() >= kUeMatchCapacity) {
            g_ue_cursor = g_ue_total;
            break;
        }
        if (g_ue_ids_only) {
            g_ue_slots_this_tick += run;
            if (g_ue_slots_this_tick >= kUeSlotBudget) break;
        } else {
            break;  // 慢路径：本 tick 的解析预算已交给 UeScanRun
        }
    }
    if (g_ue_cursor < g_ue_total) {
        if (g_ue_ids_only) g_ue_slots_this_tick = 0;
        return;
    }
    g_ue_slots_this_tick = 0;

    if (g_ue_phase == UeProbePhase::scanning) {
        UeFinishScan();
    } else {
        UeFinishOwnerScan(reader);
    }
}

void UeStop() noexcept {
    g_ue_probe_requested.store(false);
    g_ue_call_requested.store(false);
    g_ue_record_requested.store(false);
    g_ue_clear_log_requested.store(false);
    g_ue_select_requested.store(-1);
    g_ue_phase = UeProbePhase::idle;
    g_ue_cursor = 0;
    g_ue_total = 0;
    g_ue_slots_this_tick = 0;
    g_ue_chunk = 0;
    g_ue_chunk_index = 0xFFFFFFFFu;
    g_ue_run_buffer.clear();
    g_ue_function = 0;
    g_ue_owner_class = 0;
    g_ue_instance = 0;
    g_ue_recording.store(false);
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_matches.clear();
        g_ue_match_total = 0;
        g_ue_target_name.clear();
    }
    if (g_context.events != nullptr && g_ue_event_subscription.id != 0 &&
        g_context.events->unsubscribe != nullptr) {
        g_context.events->unsubscribe(g_context.events->user, g_ue_event_subscription);
    }
    g_ue_event_subscription = {};
}

void UeSyncText() {
    if (!g_ue_text_stale.exchange(false)) return;
    const std::string keyword = UeKeyword();
    std::snprintf(g_ue_text.data(), g_ue_text.size(), "%s", keyword.c_str());
}

void UeContent(const AnomalyUiServiceV1* ui) {
    UeSyncText();

    DrawText(ui, g_context.localizer.Text("ue.title", "直接调用（不经 UI）"));
    ui->text(ui->user, StringView("名称关键词"));
    if (ui->same_line != nullptr) ui->same_line(ui->user, 110.0F, 8.0F);
    if (ui->input_text != nullptr) {
        ui->input_text(ui->user, StringView("##night999-ue-keyword"), g_ue_text.data(),
                       g_ue_text.size(), ANOMALY_UI_TEXT_INPUT_V1_NONE);
    } else {
        DrawText(ui, g_ue_text.data());
    }
    UeSetKeyword(g_ue_text.data());

    if (ui->button != nullptr) {
        if (ui->button(ui->user, StringView("探测"), 0.0F, 0.0F) != 0) {
            g_ue_probe_requested.store(true);
        }
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        const int enabled = g_ue_instance != 0 && g_ue_function != 0 ? 1 : 0;
        const int pressed = ui->button_enabled != nullptr
            ? ui->button_enabled(ui->user, StringView("直接调用"), 0.0F, 0.0F, enabled)
            : (enabled && ui->button(ui->user, StringView("直接调用"), 0.0F, 0.0F) != 0);
        if (pressed != 0) g_ue_call_requested.store(true);
    }

    std::string status, call, target;
    std::vector<UeMatch> matches;
    std::uint32_t match_total{};
    UeCopyOut(status, call, target, matches, match_total);
    DrawText(ui, status);
    if (!call.empty()) DrawText(ui, call);

    if (!matches.empty() && ui->button != nullptr) {
        const std::size_t shown = matches.size() < 8 ? matches.size() : 8;
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& match = matches[index];
            const std::string label = "选用##night999-ue-" + std::to_string(index);
            if (ui->button(ui->user, StringView(label), 0.0F, 0.0F) != 0) {
                g_ue_select_requested.store(static_cast<int>(index));
            }
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
            std::string line = std::string(match.name) + "  ←  " + match.class_name;
            if (match.is_function) {
                line += "  （" + std::to_string(match.num_parms) + " 参 / " +
                    std::to_string(match.parms_size) + " 字节）";
            }
            DrawText(ui, line);
        }
        if (match_total > shown) {
            DrawText(ui, "（共匹配 " + std::to_string(match_total) + " 个，只列出前 " +
                             std::to_string(shown) + " 个）");
        }
    }

    if (ui->checkbox != nullptr) {
        int recording = g_ue_recording.load() ? 1 : 0;
        if (ui->checkbox(ui->user, StringView("记录 ProcessEvent（找出按钮真正调的函数）"),
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
        const std::size_t shown = g_ue_event_count < 12 ? g_ue_event_count : 12;
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& record = g_ue_events[index];
            DrawText(ui, std::string(record.resolved ? record.function_name : "(解析中)") +
                             "  ←  " + (record.resolved ? record.class_name : "?") +
                             "  ×" + std::to_string(record.count));
        }
    }
}

}  // namespace
