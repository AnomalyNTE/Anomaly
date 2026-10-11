#pragma once
// 直接调用：按名称关键词在对象注册表里定位 UObject / UFunction，再用 ProcessEvent 调用。
//
// 枚举用宿主自己的 anomaly.ue5.objects 服务（count + snapshot_at），不自己走注册表内存布局；
// 只有命中之后的少数对象才用 ue_access.hpp 的 UeResolve 拿真实地址。
//
// 匹配两条路自动选择：先用 ue5.names.find_utf8 把关键词换成 FName id，命中就走「整数比较」
// 快路径；命中不了（该名字在当前会话未注册，例如相关蓝图还没加载）就退化为逐个解析名字做
// 子串匹配。
//
// 需要 ue_access.hpp 先包含。扫描与调用都只在 Game 回调域（on_update）执行；ProcessEvent
// 回调里只记 FName id 与对象地址，任何反射解析都留到 on_update 做。

namespace {

constexpr std::string_view kUeDefaultKeyword = "ResetMonster";
constexpr std::size_t kUeTextCapacity = 96;
constexpr std::size_t kUeMatchCapacity = 16;
// 每 tick 遍历的槽位数（快慢两条路都用它；慢路另有解析上限）。
constexpr std::uint32_t kUeSlotBudget = 8192;
// 慢路径每 tick 最多解析多少个存活对象的名字。
constexpr std::uint32_t kUeResolveBudget = 1024;
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
    char owner_name[kUeTextCapacity]{};
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
bool g_ue_auto_call{};
std::uint32_t g_ue_name_id{};
std::uint32_t g_ue_cursor{};
std::uint32_t g_ue_total{};
std::uint32_t g_ue_match_total{};
std::string g_ue_keyword_scan;   // 本趟关键词小写副本，Game 线程独占
std::uintptr_t g_ue_function{};
std::uintptr_t g_ue_owner_class{};
std::uintptr_t g_ue_instance{};
bool g_ue_instance_is_cdo{};
std::uint32_t g_ue_num_parms{};
std::uint32_t g_ue_parms_size{};

// --- 共享状态（on_draw 读，on_update 写）---
std::mutex g_ue_mutex;
std::string g_ue_keyword{kUeDefaultKeyword};
std::array<char, kUeTextCapacity> g_ue_text{};
std::atomic_bool g_ue_text_stale{true};
std::string g_ue_status{"未探测"};
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

void UeLog(const std::string& text) {
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 ue: " + text);
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

void UeCopyOut(std::string& status, std::string& call, std::vector<UeMatch>& matches,
               std::uint32_t& match_total) {
    std::scoped_lock lock(g_ue_mutex);
    status = g_ue_status;
    call = g_ue_call_result;
    matches = g_ue_matches;
    match_total = g_ue_match_total;
}

void UeSetKeyword(const std::string& keyword) {
    std::scoped_lock lock(g_ue_mutex);
    if (g_ue_keyword == keyword) return;
    g_ue_keyword = keyword;
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
    UePublish(text);
    UeLog(text);
}

void UeSetCallResultBoth(std::string text) {
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 ue call: " + text);
    UeSetCallResult(std::move(text));
}

// 命中一个对象：换真实地址，补齐类名、Outer（对函数来说就是所属类）与参数信息。
void UeAddMatch(const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects,
                const AnomalyUe5ObjectSnapshotV1& snapshot, const std::string& name) {
    ++g_ue_match_total;
    if (g_ue_matches.size() >= kUeMatchCapacity) return;
    UeMatch match{};
    std::snprintf(match.name, sizeof(match.name), "%s", name.c_str());
    const std::uintptr_t object = UeResolve(reader, objects, snapshot.handle);
    match.object = object;
    if (object != 0) {
        std::uintptr_t owner{};
        reader.Read(object + kUeObjectOuterOffset, owner);
        match.owner = owner;
        std::uintptr_t cls{};
        if (reader.Read(object + kUeObjectClassOffset, cls) && cls != 0) {
            const std::string class_name = reader.NameAt(cls + kUeObjectNameOffset);
            std::snprintf(match.class_name, sizeof(match.class_name), "%s", class_name.c_str());
            match.is_function = class_name == "Function";
        }
        if (owner != 0) {
            const std::string owner_name = reader.NameAt(owner + kUeObjectNameOffset);
            std::snprintf(match.owner_name, sizeof(match.owner_name), "%s", owner_name.c_str());
        }
        if (match.is_function) {
            std::uint8_t num_parms{};
            std::uint16_t parms_size{};
            reader.Read(object + kUeFunctionNumParmsOffset, num_parms);
            reader.Read(object + kUeFunctionParmsSizeOffset, parms_size);
            match.num_parms = num_parms;
            match.parms_size = parms_size;
        }
    }
    g_ue_matches.push_back(match);
}

bool UeScanStep(const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects,
                std::uint32_t& resolved) {
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    const std::uint32_t end = std::min(g_ue_cursor + kUeSlotBudget, g_ue_total);
    for (; g_ue_cursor < end; ++g_ue_cursor) {
        snapshot.struct_size = sizeof(snapshot);
        if (objects->snapshot_at(objects->user, g_ue_cursor, &snapshot).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (snapshot.name_id == 0) continue;

        if (g_ue_ids_only) {
            if (snapshot.name_id != g_ue_name_id) continue;
            UeAddMatch(reader, objects, snapshot, reader.Name(UeFName{snapshot.name_id, 0}));
            if (g_ue_matches.size() >= kUeMatchCapacity) return true;
            continue;
        }
        if (resolved >= kUeResolveBudget) return true;
        ++resolved;
        const std::string text = reader.Name(UeFName{snapshot.name_id, 0});
        if (text.empty()) continue;
        if (UeLower(text).find(g_ue_keyword_scan) == std::string::npos) continue;
        UeAddMatch(reader, objects, snapshot, text);
        if (g_ue_matches.size() >= kUeMatchCapacity) return true;
    }
    return true;
}

// 沿 Outer 链上溯找目标类的实例。UMG 控件由 WidgetTree 创建，Outer 就是所属的
// UserWidget，所以从按钮控件一路 Outer 上去通常几步就能拿到宿主界面实例，
// 比全注册表扫描便宜几个数量级。
std::uintptr_t UeOwnerViaOuter(
    const UeReader& reader, std::uintptr_t start, const std::uintptr_t owner_class) {
    std::uintptr_t object = start;
    for (int depth = 0; object != 0 && depth < 24; ++depth) {
        std::uintptr_t cls{};
        if (reader.Read(object + kUeObjectClassOffset, cls) && cls == owner_class) return object;
        std::uintptr_t outer{};
        if (!reader.Read(object + kUeObjectOuterOffset, outer) || outer == object) break;
        object = outer;
    }
    return 0;
}

void UeBeginScan() {
    g_ue_matches.clear();
    g_ue_match_total = 0;
    g_ue_function = 0;
    g_ue_owner_class = 0;
    g_ue_instance = 0;
    g_ue_instance_is_cdo = false;
    g_ue_auto_call = false;
    g_ue_num_parms = 0;
    g_ue_parms_size = 0;
    g_ue_cursor = 0;
    g_ue_total = 0;
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
    if (names == nullptr || names->find_utf8 == nullptr || names->resolve_utf8 == nullptr ||
        objects == nullptr || objects->count == nullptr || objects->snapshot_at == nullptr) {
        UeFinish("需要 ue5-names（含 find_utf8/resolve_utf8）与 ue5-objects 服务", true);
        return;
    }
    g_ue_total = objects->count(objects->user);
    if (g_ue_total == 0 || g_ue_total > 16u * 1024u * 1024u) {
        UeFinish("对象注册表计数异常（" + std::to_string(g_ue_total) + "）", true);
        return;
    }
    static_cast<void>(UeEnsureRegistry());

    const UeReader reader{names};
    if (names->find_utf8(names->user, StringView(keyword), &g_ue_name_id).code ==
            ANOMALY_STATUS_V1_OK &&
        g_ue_name_id != 0) {
        g_ue_ids_only = true;
        g_ue_phase = UeProbePhase::scanning;
        UePublish("名字已注册，按精确名称扫描 " + std::to_string(g_ue_total) + " 个对象…");
        UeLog("scan(exact) keyword=" + keyword + " total=" + std::to_string(g_ue_total));
        return;
    }
    g_ue_keyword_scan = UeLower(keyword);
    g_ue_ids_only = false;
    g_ue_phase = UeProbePhase::scanning;
    UeLog("scan(substring) keyword=" + keyword + " total=" + std::to_string(g_ue_total));
    UePublish("名字未注册（相关蓝图可能还没加载），改为逐对象解析名字做子串匹配，共 " +
              std::to_string(g_ue_total) + " 个对象…");
}

void UeFinishScan() {
    if (g_ue_matches.empty()) {
        UeFinish("注册表里没有任何对象名含「" + UeKeyword() + "」（共 " +
                     std::to_string(g_ue_total) + " 个对象）——相关蓝图尚未加载。"
                     "请先在游戏里打开一次营地（或进入对应玩法）再点探测",
                 true);
        return;
    }
    std::stable_partition(g_ue_matches.begin(), g_ue_matches.end(),
                          [](const UeMatch& match) { return match.is_function; });
    std::string list;
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_status = "匹配 " + std::to_string(g_ue_match_total) + " 个对象（列出前 " +
            std::to_string(g_ue_matches.size()) + " 个，函数优先）：按「选用并调用」直接调用";
        for (const auto& match : g_ue_matches) {
            list += std::string(match.name) + "[" + match.owner_name + "] ";
        }
    }
    UeLog("scan done matches=" + std::to_string(g_ue_match_total) + " : " + list);
}

// 找目标类的实例。先走 Outer 链快路，再退化为全注册表扫描。
void UeFinishOwnerScan(const UeReader& reader);
void UeCallFunction();

void UeResolveInstance(const UeReader& reader) {
    for (const auto& anchor : g_ue_matches) {
        if (anchor.is_function || anchor.object == 0) continue;
        const std::uintptr_t found = UeOwnerViaOuter(reader, anchor.object, g_ue_owner_class);
        if (found != 0) {
            g_ue_instance = found;
            g_ue_instance_is_cdo = false;
            UeLog("instance via outer chain anchor=" + std::string(anchor.name));
            return;
        }
    }
    g_ue_phase = UeProbePhase::finding_owner;
    g_ue_cursor = 0;
}

void UeBeginOwnerScan(const UeReader& reader, const std::size_t index) {
    if (index >= g_ue_matches.size()) return;
    const UeMatch match = g_ue_matches[index];
    g_ue_function = match.object;
    g_ue_owner_class = match.owner;
    g_ue_num_parms = match.num_parms;
    g_ue_parms_size = match.parms_size;
    g_ue_instance = 0;
    g_ue_instance_is_cdo = false;
    g_ue_cursor = 0;
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_target_name = match.name;
    }
    UeLog("select #" + std::to_string(index) + " " + match.name + " func=" +
          (match.is_function ? "1" : "0") + " owner=" + match.owner_name + " parms=" +
          UeParmsText());
    if (match.object == 0) {
        UeFinish("无法解析「" + std::string(match.name) + "」的对象地址（注册表基址未就绪）",
                 true);
        return;
    }
    if (!match.is_function) {
        UeFinish("「" + std::string(match.name) + "」不是 UFunction（类 " + match.class_name +
                     "），不能这样调用",
                 true);
        return;
    }
    if (g_ue_owner_class == 0) {
        UeFinish("「" + std::string(match.name) + "」没有 Outer，无法定位所属类实例", true);
        return;
    }
    UePublish("已选中 " + std::string(match.name) + "（类 " + match.owner_name + "，" +
              UeParmsText() + "），找实例中…");
    UeResolveInstance(reader);
    if (g_ue_instance != 0) {
        UeFinishOwnerScan(reader);
        if (g_ue_auto_call) UeCallFunction();
    }
}

bool UeOwnerStep(const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects) {
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    const std::uint32_t end = std::min(g_ue_cursor + kUeSlotBudget, g_ue_total);
    for (; g_ue_cursor < end; ++g_ue_cursor) {
        snapshot.struct_size = sizeof(snapshot);
        if (objects->snapshot_at(objects->user, g_ue_cursor, &snapshot).code !=
            ANOMALY_STATUS_V1_OK) {
            continue;
        }
        if (snapshot.name_id == 0) continue;
        const std::uintptr_t object = UeResolve(reader, objects, snapshot.handle);
        if (object == 0) continue;
        std::uintptr_t cls{};
        if (!reader.Read(object + kUeObjectClassOffset, cls) || cls != g_ue_owner_class) continue;
        const std::string name = reader.NameAt(object + kUeObjectNameOffset);
        if (name.rfind("Default__", 0) == 0) {
            if (g_ue_instance == 0) {
                g_ue_instance = object;
                g_ue_instance_is_cdo = true;
            }
            continue;
        }
        g_ue_instance = object;
        g_ue_instance_is_cdo = false;
        return true;
    }
    return true;
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
    text += g_ue_instance != 0 ? "，实例已找到"
                               : "，未找到该类实例（该函数可能只在界面打开时才存在）";
    UeFinish(text, false);
}

void UeCallFunction() {
    if (g_ue_function == 0 || g_ue_instance == 0) {
        UeSetCallResultBoth("没有可调用的目标（请先探测并选用）");
        return;
    }
    if (g_ue_parms_size > 4096) {
        UeSetCallResultBoth("ParmsSize 异常（" + std::to_string(g_ue_parms_size) +
                           "），已拒绝调用");
        return;
    }
    std::vector<std::uint8_t> parms(g_ue_parms_size, 0);
    const UeReader reader{g_context.names};
    if (!UeEnsureRegistry() || g_context.core == nullptr) {
        UeSetCallResultBoth("对象注册表不可用");
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
        UeSetCallResultBoth("调用「" + target + "」失败（ProcessEvent 抛异常或 vtable 不可执行）");
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
    UeSetCallResultBoth(detail);
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
    const auto* objects = g_context.objects;

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
            UeFinish("已开始记录 ProcessEvent（去重，最多 96 条）", false);
        } else {
            UeFinish("事件订阅失败（需要 game-events）", true);
        }
    }
    UeResolveEvents(reader);

    if (g_ue_probe_requested.exchange(false)) {
        UeBeginScan();
        return;
    }
    const int select = g_ue_select_requested.exchange(-1);
    const bool busy = g_ue_phase == UeProbePhase::scanning ||
        g_ue_phase == UeProbePhase::finding_owner;
    if (!busy && select >= 0) {
        g_ue_auto_call = true;
        UeBeginOwnerScan(reader, static_cast<std::size_t>(select));
        return;
    }
    if (!busy) {
        if (g_ue_call_requested.exchange(false)) UeCallFunction();
        return;
    }
    if (objects == nullptr || objects->snapshot_at == nullptr) {
        UeFinish("ue5-objects 服务已不可用", true);
        return;
    }

    std::uint32_t resolved{};
    if (g_ue_phase == UeProbePhase::scanning) {
        if (!UeScanStep(reader, objects, resolved)) {
            UeFinish("扫描中断（对象服务读取失败）", true);
            return;
        }
        if (g_ue_cursor >= g_ue_total || g_ue_matches.size() >= kUeMatchCapacity) {
            UeFinishScan();
        }
        return;
    }
    if (!UeOwnerStep(reader, objects)) {
        UeFinish("扫描中断（对象服务读取失败）", true);
        return;
    }
    if (g_ue_cursor >= g_ue_total || g_ue_instance != 0) {
        UeFinishOwnerScan(reader);
        if (g_ue_auto_call) UeCallFunction();
    }
}

void UeStop() noexcept {
    g_ue_probe_requested.store(false);
    g_ue_call_requested.store(false);
    g_ue_record_requested.store(false);
    g_ue_clear_log_requested.store(false);
    g_ue_select_requested.store(-1);
    g_ue_phase = UeProbePhase::idle;
    g_ue_auto_call = false;
    g_ue_cursor = 0;
    g_ue_total = 0;
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
        const int enabled = g_ue_function != 0 && g_ue_instance != 0 ? 1 : 0;
        const int pressed = ui->button_enabled != nullptr
            ? ui->button_enabled(ui->user, StringView("再调用一次"), 0.0F, 0.0F, enabled)
            : (enabled && ui->button(ui->user, StringView("再调用一次"), 0.0F, 0.0F) != 0);
        if (pressed != 0) g_ue_call_requested.store(true);
    }

    std::string status, call;
    std::vector<UeMatch> matches;
    std::uint32_t match_total{};
    UeCopyOut(status, call, matches, match_total);
    if (!status.empty()) DrawText(ui, status);
    if (!call.empty()) DrawText(ui, call);

    if (!matches.empty() && ui->button != nullptr) {
        const std::size_t shown = matches.size() < 8 ? matches.size() : 8;
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& match = matches[index];
            const std::string label = "选用并调用##night999-ue-" + std::to_string(index);
            if (ui->button(ui->user, StringView(label), 0.0F, 0.0F) != 0) {
                g_ue_select_requested.store(static_cast<int>(index));
            }
            if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
            std::string line = std::string(match.name) + "  ←  ";
            line += match.owner_name[0] != '\0' ? match.owner_name : match.class_name;
            if (match.is_function) {
                line += "  （函数，" + std::to_string(match.num_parms) + " 参 / " +
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
