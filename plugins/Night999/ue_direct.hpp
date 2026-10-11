#pragma once
// 直接调用：按名称关键词在对象注册表里定位 UObject / UFunction，再用 ProcessEvent 调用。
//
// 枚举用宿主自己的 anomaly.ue5.objects 服务（count + snapshot_at），不自己走注册表内存布局；
// 只有命中之后的少数对象才用 ue_access.hpp 的 UeResolve 拿真实地址。
//
// 实例判定一律用 UeClassDerivesFrom（沿 SuperStruct 链），因为活的游戏对象几乎都是蓝图子类。
// 找实例的顺序：HTUtil::GetMainPlayerState 直取 → 其它匹配对象的 Outer 链 → 全表扫描 → CDO 兜底。
//
// 需要 ue_access.hpp 先包含。扫描与调用都只在 Game 回调域（on_update）执行；ProcessEvent
// 回调里只做整数比较与内存拷贝，名字解析留到 on_update。

namespace {

constexpr std::string_view kUeDefaultKeyword = "ResetMonster";
constexpr std::size_t kUeTextCapacity = 96;
constexpr std::size_t kUeMatchCapacity = 16;
constexpr std::uint32_t kUeSlotBudget = 8192;
constexpr std::uint32_t kUeResolveBudget = 1024;
// 一键刷新是用户主动触发的，允许更大的时间片（但仍有 wall-clock 上限，避免卡帧）。
constexpr std::uint32_t kUeSlotBudgetFast = 32768;
constexpr std::chrono::microseconds kUeScanSlice{4000};
// 监视名单上限：命中对象的 FName id，回调里用整数比较识别，命中就记下实参。
constexpr std::size_t kUeMonitorCapacity = 32;
// 类级监视：命中对象的所属类 id。该类上任一函数被调用都会记下来 ——
// 于是不必猜「客户端到底发了哪个 RPC」，看列表即可。
constexpr std::size_t kUeClassMonitorCapacity = 16;
// 「其它调用」的过滤器：只用一张开放寻址表记「见过没」，只记首次出现的函数。
// 这样先开几秒让常驻函数（tick/动画）把表填满，再清空记录去触发一次操作，
// 剩下的基本就是这次操作真正新调用的函数链。
constexpr std::size_t kUeSeenSlots = 16384;
constexpr std::size_t kUeEventCapacity = 128;
// 单次调用最多记多少字节实参。RPC 载荷可能到 32 字节以上，留足。
constexpr std::size_t kUeParmsCapture = 64;
// 营火名（FName），用于单发快速刷新。
constexpr std::string_view kUeCampfireDefault = "FT_Castle_Campfire04";

// ---- 一键刷新 ----
// 刷新本体挂在 AHTPlayerState 上（IDA 实测）：
//   FinalTowerCampResetMonsterSpawners(InCampfireID) —— 组 ServerFinalTowerRPC(Type=21)
constexpr std::string_view kUeRefreshFunction = "FinalTowerCampResetMonsterSpawners";
// ★ find_exact 用的完整对象路径。格式与 UeLocalPlayerState 里已验证可用的
//   "/Script/HTGame.HTUtil.GetMainPlayerState" 完全一致（包.类.成员）。
//   实测 find_utf8 对反射函数名返回 NOT_FOUND，所以定位优先走这条路径，不再依赖名字池。
constexpr std::string_view kUeRefreshFunctionPath =
    "/Script/HTGame.HTPlayerState.FinalTowerCampResetMonsterSpawners";
constexpr std::chrono::milliseconds kUeRefreshFireInterval{120};
constexpr std::chrono::seconds kUeRefreshTimeout{20};

enum class UeProbePhase : std::uint8_t { idle, scanning, finding_owner, ready, failed };

struct UeMatch {
    std::uintptr_t object{};
    std::uintptr_t owner{};
    bool is_function{};
    std::uint32_t num_parms{};
    std::uint32_t parms_size{};
    std::uint32_t name_id{};
    char name[kUeTextCapacity]{};
    char class_name[kUeTextCapacity]{};
    char owner_name[kUeTextCapacity]{};
};

struct UeEventRecord {
    std::uint32_t function_id{};
    std::uintptr_t object{};
    std::uint32_t count{};
    std::uint32_t parms_size{};
    bool monitored{};
    bool resolved{};
    bool decoded{};
    bool logged{};
    std::uint64_t parms_value{};
    std::uint8_t parms_raw[kUeParmsCapture]{};
    char parms_hex[kUeParmsCapture * 3 + 8]{};
    // 首 8 字节若是指针（TArray 的 data 指针等），把它指向的前 64 字节也记下来。
    char pointee_hex[kUeParmsCapture * 3 + 8]{};
    char parms_fname[kUeTextCapacity]{};
    char function_name[kUeTextCapacity]{};
    char class_name[kUeTextCapacity]{};
};

// --- handoff 标志（on_draw 置位）---
std::atomic_bool g_ue_probe_requested{};
std::atomic_bool g_ue_call_requested{};
std::atomic_bool g_ue_clear_log_requested{};
std::atomic_bool g_ue_record_requested{};
std::atomic_bool g_ue_record_all{};
std::atomic_int g_ue_select_requested{-1};
// 按「所属类名」匹配关键词（用于找某类的活实例，例如场景里的篝火 Actor）。
std::atomic_bool g_ue_match_class{};
// 字段转储请求（把当前实例的反射字段全列出来，写进日志）
std::atomic_bool g_ue_fields_requested{};
// 「依次尝试营火 ID」：读实例上的 ActivatedCampfireIDs，逐个当参数调用当前目标。
std::atomic_bool g_ue_campfire_requested{};
// 单发：按营火名刷新（快路径）。
std::atomic_bool g_ue_campfire_single_requested{};
// 一键刷新怪物：自动定位 PlayerState + 刷新函数 → 逐座营火发送。
std::atomic_bool g_ue_refresh_requested{};
// 0=空闲 1=定位 2=发送。
std::atomic_int g_ue_refresh_stage{0};
// 一次刷新流程结束（任何出口）时置 1，供脚本循环等待；发起前清零。
std::atomic_int g_ue_refresh_done{0};
// 上一次刷新是否确实把请求发出去了（诊断用）。
std::atomic_int g_ue_refresh_ok{0};

// --- 扫描状态（on_update 独占）---
UeProbePhase g_ue_phase{UeProbePhase::idle};
bool g_ue_ids_only{};
bool g_ue_auto_call{};
bool g_ue_class_match{};
std::uint32_t g_ue_name_id{};
bool g_ue_fast_scan{};
std::uint32_t g_ue_cursor{};
std::uint32_t g_ue_total{};
std::uint32_t g_ue_match_total{};
std::string g_ue_keyword_scan;
std::uintptr_t g_ue_function{};
std::uintptr_t g_ue_owner_class{};
std::uintptr_t g_ue_instance{};
std::uintptr_t g_ue_cdo{};
bool g_ue_instance_is_cdo{};
std::uint32_t g_ue_num_parms{};
std::uint32_t g_ue_parms_size{};
std::uint32_t g_ue_monitor_ids[kUeMonitorCapacity]{};
std::size_t g_ue_monitor_count{};
std::uint32_t g_ue_monitor_class_ids[kUeClassMonitorCapacity]{};
std::size_t g_ue_monitor_class_count{};
std::uint32_t g_ue_seen_ids[kUeSeenSlots]{};
std::size_t g_ue_seen_used{};

// --- 共享状态（on_draw 读，on_update 写）---
std::mutex g_ue_mutex;
std::string g_ue_keyword{kUeDefaultKeyword};
// 「调用参数」与「营火名」的取值：面板已移除，保留字段供内部路径使用
// （前者始终为空 ⇒ 调用按全零参数；后者仍可由 UeCallByName 兜底使用）。
std::string g_ue_param;
std::string g_ue_campfire_name{kUeCampfireDefault};
std::string g_ue_status{"未探测"};
std::string g_ue_call_result;
std::string g_ue_params_dump;
std::vector<UeMatch> g_ue_matches;
std::string g_ue_target_name;
std::vector<std::string> g_ue_field_lines;
std::size_t g_ue_field_cursor{};
std::size_t g_ue_field_logged{};
std::uint64_t g_ue_campfire_ids[16]{};
std::size_t g_ue_campfire_count{};
std::size_t g_ue_campfire_index{};
bool g_ue_campfire_running{};
std::chrono::steady_clock::time_point g_ue_campfire_next{};
// 调用参数：空 = 全零；0x…/十进制 = 64 位整数；其它按 FName 名字查 id。

// 一键刷新的流程状态（on_update 独占；stage 本身是 atomic 供 on_draw 读）。
bool g_ue_refresh_scan_started{};
bool g_ue_refresh_selected{};
bool g_ue_refresh_fire_started{};
std::chrono::steady_clock::time_point g_ue_refresh_deadline{};
std::chrono::steady_clock::time_point g_ue_refresh_scan_start{};
// ★ 定位缓存：首次定位成功后把「函数地址 / 它自己的 FName id / 所属类 / 参数大小」记下来，
// 之后每次点击只做几次廉价读校验 + 重新取一次 PlayerState 实例，完全不再扫描注册表。
// 校验方式是「对象 +0x18 的 id 必须等于记录值」且「该 id resolve 出的名字必须等于目标名」
// —— 刻意不走 find_utf8（实测它对反射函数名返回 NOT_FOUND）。任一步不过就退回定位流程。
std::uintptr_t g_ue_refresh_fn_cache{};
std::uint32_t g_ue_refresh_fn_name_id{};
std::uintptr_t g_ue_refresh_owner_cache{};
std::uint32_t g_ue_refresh_fn_parms_size{};

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

// 把实参前若干字节追加成十六进制（自己拼，避免 strncat 的 CRTC 警告）。
void UeAppendHex(char* destination, const std::size_t capacity, const void* source,
                 const std::size_t count) {
    static const char kDigits[] = "0123456789ABCDEF";
    if (destination == nullptr || source == nullptr || capacity < 4) return;
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    std::size_t used = std::strlen(destination);
    for (std::size_t index = 0; index < count; ++index) {
        if (used + 3 >= capacity) break;
        destination[used++] = kDigits[bytes[index] >> 4];
        destination[used++] = kDigits[bytes[index] & 0x0F];
        destination[used++] = ' ';
    }
    destination[used] = '\0';
}

// 首 8 字节若落在合法地址上（很像 TArray 的 data 指针），把它指向的前 64 字节读出来。
// 回调是**执行前**钩子，此刻那块内存还有效 —— 这正是「照抄载荷」失败、但能在这里
// 读到真实内容的原因。
void UeFollowPointer(UeEventRecord& record, const std::uint8_t* bytes,
                     const std::uint32_t size) {
    record.pointee_hex[0] = '\0';
    if (size < 8) return;
    std::uint64_t value{};
    std::memcpy(&value, bytes, 8);
    if (value <= 0x10000) return;
    const auto* core = g_context.core;
    if (core == nullptr || core->read_memory == nullptr) return;
    std::uint8_t buffer[64]{};
    if (core->read_memory(core->user, static_cast<std::uintptr_t>(value),
                          {buffer, sizeof(buffer)}).code != ANOMALY_STATUS_V1_OK) {
        return;
    }
    UeAppendHex(record.pointee_hex, sizeof(record.pointee_hex), buffer, sizeof(buffer));
}

// 记录一次调用的实参：十六进制（给人看）与原字节（给解析用）。
void UeCaptureParms(UeEventRecord& record, void* parameters, const std::uint16_t parms_size) {
    if (parameters == nullptr) return;
    const std::size_t take = std::min<std::size_t>(parms_size, kUeParmsCapture);
    UeAppendHex(record.parms_hex, sizeof(record.parms_hex), parameters, take);
    std::memcpy(record.parms_raw, parameters, take);
    record.parms_size = parms_size;
    UeFollowPointer(record, static_cast<const std::uint8_t*>(parameters), parms_size);
}

// 开放寻址：返回 true 表示这个 id 之前没见过（并把它记下）。
bool UeMarkSeen(const std::uint32_t id) noexcept {
    if (id == 0) return false;
    std::uint32_t slot = (id * 2654435761u) & (kUeSeenSlots - 1);
    for (std::size_t step = 0; step < 64; ++step) {
        const std::uint32_t current = g_ue_seen_ids[slot];
        if (current == id) return false;
        if (current == 0) {
            if (g_ue_seen_used + 64 >= kUeSeenSlots) return false;
            g_ue_seen_ids[slot] = id;
            ++g_ue_seen_used;
            return true;
        }
        slot = (slot + 1) & (kUeSeenSlots - 1);
    }
    return false;
}

void UeClearSeen() noexcept {
    std::memset(g_ue_seen_ids, 0, sizeof(g_ue_seen_ids));
    g_ue_seen_used = 0;
}

bool UeIsMonitoredClass(const std::uint32_t owner_id) noexcept {
    if (owner_id == 0) return false;
    for (std::size_t index = 0; index < g_ue_monitor_class_count; ++index) {
        if (g_ue_monitor_class_ids[index] == owner_id) return true;
    }
    return false;
}

bool UeIsMonitored(const std::uint32_t function_id) noexcept {
    for (std::size_t index = 0; index < g_ue_monitor_count; ++index) {
        if (g_ue_monitor_ids[index] == function_id) return true;
    }
    return false;
}

// --- 事件记录回调：只做整数比较、内存读取与拷贝 ---
void ANOMALY_CALL UeObserve(void*, std::uintptr_t object, std::uintptr_t function, void* parameters) {
    if (!g_ue_recording.load() || function == 0) return;
    const auto* core = g_context.core;
    if (core == nullptr || core->read_memory == nullptr) return;

    UeFName name{};
    std::uint16_t parms_size{};
    if (core->read_memory(core->user, function + kUeObjectNameOffset,
                          {reinterpret_cast<std::uint8_t*>(&name), sizeof(name)}).code !=
            ANOMALY_STATUS_V1_OK ||
        core->read_memory(core->user, function + kUeFunctionParmsSizeOffset,
                          {reinterpret_cast<std::uint8_t*>(&parms_size), sizeof(parms_size)})
                .code != ANOMALY_STATUS_V1_OK) {
        return;
    }

    bool monitored = UeIsMonitored(name.id);
    if (!monitored && g_ue_monitor_class_count != 0) {
        // 所属类命中：读 UFunction 的 Outer（类对象）再取其 FName id 比对。
        std::uintptr_t outer{};
        std::uint32_t owner_id{};
        if (core->read_memory(core->user, function + kUeObjectOuterOffset,
                              {reinterpret_cast<std::uint8_t*>(&outer), sizeof(outer)}).code ==
                ANOMALY_STATUS_V1_OK &&
            outer != 0 &&
            core->read_memory(core->user, outer + kUeObjectNameOffset,
                              {reinterpret_cast<std::uint8_t*>(&owner_id), sizeof(owner_id)})
                    .code == ANOMALY_STATUS_V1_OK) {
            monitored = UeIsMonitoredClass(owner_id);
        }
    }
    if (!monitored) {
        if (!g_ue_record_all.load() || !UeMarkSeen(name.id)) return;
    }

    std::scoped_lock lock(g_ue_mutex);
    ++g_ue_event_total;
    for (std::size_t index = 0; index < g_ue_event_count; ++index) {
        auto& record = g_ue_events[index];
        if (record.function_id != name.id || record.object != object) continue;
        ++record.count;
        if (record.parms_hex[0] == '\0') UeCaptureParms(record, parameters, parms_size);
        return;
    }
    if (g_ue_event_count >= kUeEventCapacity) return;
    auto& record = g_ue_events[g_ue_event_count++];
    record.function_id = name.id;
    record.object = object;
    record.count = 1;
    record.monitored = monitored;
    UeCaptureParms(record, parameters, parms_size);
}

void UePublish(std::string text) {
    std::scoped_lock lock(g_ue_mutex);
    g_ue_status = std::move(text);
}

void UeSetCallResult(std::string text) {
    std::scoped_lock lock(g_ue_mutex);
    g_ue_call_result = std::move(text);
}

std::string UeKeyword() {
    std::scoped_lock lock(g_ue_mutex);
    return g_ue_keyword;
}

int UeHexDigit(const char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 把字节序列拼成 "AA BB CC" 形式（用于原样重放捕获到的载荷）。
std::string UeBytesText(const std::uint8_t* bytes, const std::size_t count) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string text;
    text.reserve(count * 3);
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) text += ' ';
        text += kDigits[bytes[index] >> 4];
        text += kDigits[bytes[index] & 0x0F];
    }
    return text;
}

// 解析 "E0 65 41 36 01 00 00 00" 这类以空格/逗号分隔的十六进制字节串。
bool UeParseByteList(const std::string& text, std::vector<std::uint8_t>& parms,
                     std::string& detail) {
    std::vector<std::uint8_t> bytes;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() &&
               (text[index] == ' ' || text[index] == ',' || text[index] == '-' ||
                text[index] == '\t')) {
            ++index;
        }
        if (index >= text.size()) break;
        std::uint32_t value{};
        std::size_t digits = 0;
        if (index + 1 < text.size() && text[index] == '0' &&
            (text[index + 1] == 'x' || text[index + 1] == 'X')) {
            index += 2;
        }
        while (index < text.size() && digits < 2) {
            const int digit = UeHexDigit(text[index]);
            if (digit < 0) break;
            value = value * 16u + static_cast<std::uint32_t>(digit);
            ++digits;
            ++index;
        }
        if (digits == 0) return false;
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    if (bytes.size() < 2) return false;
    const std::size_t take = std::min(bytes.size(), parms.size());
    std::copy(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(take), parms.begin());
    detail = "载荷 " + std::to_string(take) + " 字节";
    return true;
}

// 按字段名在对象（含父类）的 PropertyLink 链里找它的地址。
bool UeFindField(const UeReader& reader, const std::uintptr_t object,
                 const std::string& field_name, std::uintptr_t& address) {
    std::uintptr_t cls = UeClassOf(reader, object);
    for (int depth = 0; cls != 0 && depth < 8; ++depth) {
        std::uintptr_t field{};
        if (reader.Read(cls + kUeStructPropertyLinkOffset, field)) {
            for (int index = 0; field != 0 && index < 2000; ++index) {
                const std::string name = reader.NameAt(field + kUeFieldNameOffset);
                if (name.empty() || name.size() > 64) break;
                if (name == field_name) {
                    std::int32_t offset{};
                    if (!reader.Read(field + kUeFieldOffsetOffset, offset)) return false;
                    address = object + static_cast<std::uintptr_t>(offset);
                    return true;
                }
                std::uintptr_t next{};
                if (!reader.Read(field + kUeFieldNextOffset, next) || next == field) break;
                field = next;
            }
        }
        std::uintptr_t parent{};
        if (!reader.Read(cls + kUeStructSuperOffset, parent) || parent == cls) break;
        cls = parent;
    }
    return false;
}

// 把对象自身的反射字段逐个列出来：名字 / 偏移 / 大小 / 值。数组(16 字节对齐)额外读
// 出 Num 并跟随 Data 指针 —— 用来找 PlayerState 上的 ActivatedCampfireIDs 这类字段。
void UeBuildFields(const UeReader& reader, std::vector<std::string>& lines,
                   const std::string& filter) {
    lines.clear();
    const std::uintptr_t object = g_ue_instance;
    if (object == 0) {
        lines.emplace_back("没有可用实例（先「选用」把目标定下来）");
        return;
    }
    const std::string needle = UeLower(filter);
    std::uintptr_t cls = UeClassOf(reader, object);
    for (int depth = 0; cls != 0 && depth < 6 && lines.size() < 800; ++depth) {
        const std::string class_name = reader.NameAt(cls + kUeObjectNameOffset);
        const std::size_t before = lines.size();
        std::uintptr_t field{};
        if (reader.Read(cls + kUeStructPropertyLinkOffset, field)) {
            for (int index = 0; field != 0 && index < 2000 && lines.size() < 800; ++index) {
                const std::string name = reader.NameAt(field + kUeFieldNameOffset);
                if (name.empty() || name.size() > 64) break;
                const bool keep = needle.empty() || UeLower(name).find(needle) != std::string::npos;
                std::int32_t size{};
                std::int32_t offset{};
                reader.Read(field + kUeFieldSizeOffset, size);
                reader.Read(field + kUeFieldOffsetOffset, offset);
                if (keep) {
                    char head[192]{};
                    std::snprintf(head, sizeof(head), "  %s @%X size=%u", name.c_str(), offset,
                                  size);
                    std::uint64_t value{};
                    if (reader.Read(object + static_cast<std::uintptr_t>(offset), value)) {
                        char tail[64]{};
                        std::snprintf(tail, sizeof(tail), " = %llX",
                                      static_cast<unsigned long long>(value));
                        lines.push_back(std::string(head) + tail);
                        // size==16 视作 TArray{Data,Num,Max}：跟随 Data 读前 64 字节。
                        if (size == 16) {
                            std::uint32_t num{};
                            reader.Read(object + static_cast<std::uintptr_t>(offset) + 8, num);
                            if (value > 0x10000 && num > 0 && num < 4096) {
                                std::uint8_t buffer[64]{};
                                if (g_context.core != nullptr &&
                                    g_context.core->read_memory != nullptr &&
                                    g_context.core->read_memory(
                                        g_context.core->user, static_cast<std::uintptr_t>(value),
                                        {buffer, sizeof(buffer)}).code == ANOMALY_STATUS_V1_OK) {
                                    lines.push_back("    Num=" + std::to_string(num) + " [0]=" +
                                                    UeBytesText(buffer, sizeof(buffer)));
                                }
                            }
                        }
                    } else {
                        lines.push_back(head);
                    }
                }
                std::uintptr_t next{};
                if (!reader.Read(field + kUeFieldNextOffset, next) || next == field) break;
                field = next;
            }
        }
        if (lines.size() != before) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(before),
                         "== 类 " + (class_name.empty() ? std::string("?") : class_name));
        }
        std::uintptr_t parent{};
        if (!reader.Read(cls + kUeStructSuperOffset, parent) || parent == cls) break;
        cls = parent;
    }
    if (lines.empty()) lines.emplace_back("没有名字含「" + filter + "」的字段");
}

// 前置声明：本段插入点早于它的定义。
void UeSetCallResultBoth(std::string text);

std::string UeCampfireName() {
    std::scoped_lock lock(g_ue_mutex);
    return g_ue_campfire_name;
}

// 按营火名（FName）单发调用当前目标：名字 -> find_utf8 -> 8 字节参数。
// 比「依次尝试全部」快得多；名字不在名字池里时返回 false，由调用方退化为依次尝试。
bool UeCallByName(const UeReader& reader, const std::string& name) {
    const auto* names = g_context.names;
    std::uint32_t id{};
    if (names == nullptr || names->find_utf8 == nullptr ||
        names->find_utf8(names->user, StringView(name), &id).code != ANOMALY_STATUS_V1_OK ||
        id == 0) {
        return false;
    }
    const std::uint64_t value = id;
    std::uint8_t parms[8]{};
    std::memcpy(parms, &value, sizeof(value));
    std::uint8_t buffer[32]{};
    std::memcpy(buffer, parms, sizeof(parms));
    const bool ok = UeInvoke(reader, g_ue_instance, g_ue_function, buffer);
    std::string back;
    if (ok && g_ue_parms_size > 8) {
        back = " 返回 ";
        for (std::uint32_t index = 8; index < g_ue_parms_size && index < 24; ++index) {
            char item[8]{};
            std::snprintf(item, sizeof(item), "%02X ", buffer[index]);
            back += item;
        }
    }
    UeSetCallResultBoth("按名字刷新 " + name + " id=" + std::to_string(id) +
                        (ok ? " 已调用" : " 调用失败") + back);
    UeLog("campfire by-name " + name + " id=" + std::to_string(id) + (ok ? " ok" : " fail") +
          back);
    return ok;
}

std::string UeParam() {
    std::scoped_lock lock(g_ue_mutex);
    return g_ue_param;
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

// 一键刷新的提示：状态行 + 结果行 + 日志一起写，保证足够醒目。
void UeNotice(std::string text) {
    UePublish(text);
    UeSetCallResultBoth(std::move(text));
}

std::string UeSecondsText(const double seconds) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.1f", seconds);
    return text;
}

// 沿 FField 链导出 UFunction 的参数表：名字 / 大小 / 偏移。用来判断那 8 字节是什么。
std::string UeDumpParams(const UeReader& reader, const std::uintptr_t function) {
    std::string text;
    const std::uint32_t heads[2]{kUeStructChildPropertiesOffset, kUeStructPropertyLinkOffset};
    for (const std::uint32_t head : heads) {
        std::uintptr_t field{};
        if (!reader.Read(function + head, field) || field == 0) continue;
        std::string chain;
        for (int index = 0; index < 8 && field != 0; ++index) {
            const std::string name = reader.NameAt(field + kUeFieldNameOffset);
            if (name.empty() || name.size() > 64) break;
            std::int32_t size{};
            std::int32_t offset{};
            reader.Read(field + kUeFieldSizeOffset, size);
            reader.Read(field + kUeFieldOffsetOffset, offset);
            chain += name + "(大小 " + std::to_string(size) + " 偏移 " + std::to_string(offset) +
                ") ";
            std::uintptr_t next{};
            if (!reader.Read(field + kUeFieldNextOffset, next) || next == field) break;
            field = next;
        }
        if (!chain.empty()) {
            text += (head == kUeStructChildPropertiesOffset ? "[0x50] " : "[0x70] ");
            text += chain;
        }
    }
    return text.empty() ? std::string("(没有可读的参数链)") : text;
}

void UeAddMatch(const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects,
                const AnomalyUe5ObjectSnapshotV1& snapshot, const std::string& name) {
    ++g_ue_match_total;
    if (g_ue_matches.size() >= kUeMatchCapacity) return;
    UeMatch match{};
    std::snprintf(match.name, sizeof(match.name), "%s", name.c_str());
    match.name_id = snapshot.name_id;
    const std::uintptr_t object = UeResolve(reader, objects, snapshot.handle);
    match.object = object;
    if (object != 0) {
        std::uintptr_t owner{};
        reader.Read(object + kUeObjectOuterOffset, owner);
        match.owner = owner;
        const std::uintptr_t cls = UeClassOf(reader, object);
        if (cls != 0) {
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
    // 命中对象进监视名单：手动触发时能把实参抓下来。
    if (snapshot.name_id != 0 && g_ue_monitor_count < kUeMonitorCapacity) {
        bool known = false;
        for (std::size_t index = 0; index < g_ue_monitor_count; ++index) {
            if (g_ue_monitor_ids[index] == snapshot.name_id) known = true;
        }
        if (!known) g_ue_monitor_ids[g_ue_monitor_count++] = snapshot.name_id;
    }
    // 所属类同样进监视名单：该类上任何函数被调用都会记录（用于找真正的 RPC）。
    if (match.owner != 0 && g_ue_monitor_class_count < kUeClassMonitorCapacity) {
        std::uint32_t owner_id{};
        if (reader.Read(match.owner + kUeObjectNameOffset, owner_id) && owner_id != 0) {
            bool known = false;
            for (std::size_t index = 0; index < g_ue_monitor_class_count; ++index) {
                if (g_ue_monitor_class_ids[index] == owner_id) known = true;
            }
            if (!known) g_ue_monitor_class_ids[g_ue_monitor_class_count++] = owner_id;
        }
    }
    g_ue_matches.push_back(match);
}

bool UeScanStep(const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects,
                std::uint32_t& resolved) {
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    const std::uint32_t budget = g_ue_fast_scan ? kUeSlotBudgetFast : kUeSlotBudget;
    const std::uint32_t end = std::min(g_ue_cursor + budget, g_ue_total);
    const auto slice_start = std::chrono::steady_clock::now();
    for (; g_ue_cursor < end; ++g_ue_cursor) {
        // 每 1024 槽看一次钟：快机器多扫几万槽，慢机器也不会占满一帧。
        if ((g_ue_cursor & 0x3FFu) == 0 &&
            std::chrono::steady_clock::now() - slice_start >= kUeScanSlice) {
            break;
        }
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
        bool hit = UeLower(text).find(g_ue_keyword_scan) != std::string::npos;
        if (!hit && g_ue_class_match) {
            // 对象名不含关键词时再看它的所属类名 —— 用来把某类的活实例列出来。
            const std::uintptr_t object = UeResolve(reader, objects, snapshot.handle);
            if (object != 0) {
                const std::uintptr_t cls = UeClassOf(reader, object);
                if (cls != 0) {
                    const std::string class_name = reader.NameAt(cls + kUeObjectNameOffset);
                    if (!class_name.empty() &&
                        UeLower(class_name).find(g_ue_keyword_scan) != std::string::npos) {
                        hit = true;
                    }
                }
            }
        }
        if (!hit) continue;
        UeAddMatch(reader, objects, snapshot, text);
        if (g_ue_matches.size() >= kUeMatchCapacity) return true;
    }
    return true;
}

std::uintptr_t UeOwnerViaOuter(
    const UeReader& reader, std::uintptr_t start, const std::uintptr_t owner_class) {
    std::uintptr_t object = start;
    for (int depth = 0; object != 0 && depth < 24; ++depth) {
        const std::uintptr_t cls = UeClassOf(reader, object);
        if (cls != 0 && UeClassDerivesFrom(reader, cls, owner_class)) return object;
        std::uintptr_t outer{};
        if (!reader.Read(object + kUeObjectOuterOffset, outer) || outer == object) break;
        object = outer;
    }
    return 0;
}

std::uintptr_t UeLocalPlayerState(const UeReader& reader) {
    const auto* objects = g_context.objects;
    const auto* world = g_context.world;
    if (objects == nullptr || objects->find_exact == nullptr || world == nullptr ||
        world->current == nullptr) {
        return 0;
    }
    AnomalyGenerationHandleV1 world_handle{};
    if (world->current(world->user, &world_handle).code != ANOMALY_STATUS_V1_OK ||
        world_handle.id == 0) {
        return 0;
    }
    const std::uintptr_t world_address = UeResolve(reader, objects, world_handle);
    if (world_address == 0) return 0;

    AnomalyGenerationHandleV1 util{};
    AnomalyGenerationHandleV1 getter{};
    if (objects->find_exact(objects->user, StringView("/Script/HTGame.Default__HTUtil"), &util).code !=
            ANOMALY_STATUS_V1_OK ||
        objects->find_exact(objects->user, StringView("/Script/HTGame.HTUtil.GetMainPlayerState"),
                            &getter).code != ANOMALY_STATUS_V1_OK) {
        return 0;
    }
    const std::uintptr_t util_address = UeResolve(reader, objects, util);
    const std::uintptr_t getter_address = UeResolve(reader, objects, getter);
    if (util_address == 0 || getter_address == 0) return 0;

    std::array<std::uintptr_t, 2> parms{world_address, 0};
    if (!UeInvoke(reader, util_address, getter_address, parms.data())) return 0;
    return parms[1];
}

void UeFinishOwnerScan(const UeReader& reader);
void UeCallFunction();

void UeResolveInstance(const UeReader& reader) {
    const std::uintptr_t state = UeLocalPlayerState(reader);
    if (state != 0 && UeClassDerivesFrom(reader, UeClassOf(reader, state), g_ue_owner_class)) {
        g_ue_instance = state;
        g_ue_instance_is_cdo = false;
        UeLog("instance via HTUtil::GetMainPlayerState");
        return;
    }
    for (const auto& anchor : g_ue_matches) {
        if (anchor.is_function || anchor.object == 0) continue;
        const std::uintptr_t found = UeOwnerViaOuter(reader, anchor.object, g_ue_owner_class);
        if (found != 0) {
            g_ue_instance = found;
            g_ue_instance_is_cdo = false;
            UeLog("instance via outer chain anchor=" + std::string(anchor.name));
            return;
        }
        if (anchor.owner != 0) {
            const std::uintptr_t via = UeOwnerViaOuter(reader, anchor.owner, g_ue_owner_class);
            if (via != 0) {
                g_ue_instance = via;
                g_ue_instance_is_cdo = false;
                UeLog("instance via outer chain owner=" + std::string(anchor.name));
                return;
            }
        }
    }
    // 兜底全表扫描：这条路径可能没经过 UeBeginScan，必须自己把注册表和总数准备好，
    // 否则 UeOwnerStep 会拿 g_ue_total==0 空转（实测踩过：scanned=0/0）。
    static_cast<void>(UeEnsureRegistry());
    const auto* fallback = g_context.objects;
    if (fallback != nullptr && fallback->count != nullptr) {
        g_ue_total = fallback->count(fallback->user);
    }
    g_ue_phase = UeProbePhase::finding_owner;
    g_ue_cursor = 0;
}

void UeBeginScan(const std::string& keyword) {
    g_ue_matches.clear();
    g_ue_match_total = 0;
    g_ue_function = 0;
    g_ue_owner_class = 0;
    g_ue_instance = 0;
    g_ue_cdo = 0;
    g_ue_instance_is_cdo = false;
    g_ue_auto_call = false;
    g_ue_num_parms = 0;
    g_ue_parms_size = 0;
    g_ue_cursor = 0;
    g_ue_total = 0;
    g_ue_name_id = 0;
    g_ue_ids_only = false;
    g_ue_monitor_count = 0;
    g_ue_monitor_class_count = 0;
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_target_name.clear();
        g_ue_params_dump.clear();
    }

    g_ue_class_match = g_ue_match_class.load();
    g_ue_fast_scan = false;
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
        UeLog("scan(exact) keyword=" + keyword);
        return;
    }
    g_ue_keyword_scan = UeLower(keyword);
    g_ue_ids_only = false;
    g_ue_phase = UeProbePhase::scanning;
    UeLog("scan(substring) keyword=" + keyword + " total=" + std::to_string(g_ue_total));
    UePublish("名字未注册，改为逐对象解析名字做子串匹配，共 " + std::to_string(g_ue_total) +
              " 个对象…");
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
    g_ue_phase = UeProbePhase::ready;
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
    g_ue_cdo = 0;
    g_ue_instance_is_cdo = false;
    g_ue_cursor = 0;
    const std::string dump = match.object != 0 && match.is_function
        ? UeDumpParams(reader, match.object)
        : std::string{};
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_target_name = match.name;
        g_ue_params_dump = dump;
    }
    UeLog("select #" + std::to_string(index) + " " + match.name + " func=" +
          (match.is_function ? "1" : "0") + " owner=" + match.owner_name + " parms=" +
          UeParmsText());
    if (!dump.empty()) UeLog("params: " + dump);
    if (match.object == 0) {
        UeFinish("无法解析「" + std::string(match.name) + "」的对象地址（注册表基址未就绪）",
                 true);
        return;
    }
    if (!match.is_function) {
        // 非函数对象（控件 / Actor / 组件…）：设为转储目标，可以用「转储字段」看它的字段。
        g_ue_function = 0;
        g_ue_instance = match.object;
        g_ue_instance_is_cdo = false;
        UeFinish("已选中对象「" + std::string(match.name) + "」（类 " + match.class_name +
                     "）：点「转储字段」查看其字段",
                 false);
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
        const std::uintptr_t cls = UeClassOf(reader, object);
        if (cls == 0 || !UeClassDerivesFrom(reader, cls, g_ue_owner_class)) continue;
        const std::string name = reader.NameAt(object + kUeObjectNameOffset);
        if (name.rfind("Default__", 0) == 0) {
            if (g_ue_cdo == 0) g_ue_cdo = object;
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
    text += g_ue_instance != 0
        ? (g_ue_instance_is_cdo ? "，只有 CDO（没有活实例）" : "，活实例已找到")
        : "，未找到该类实例（该函数可能只在界面打开时才存在）";
    UeFinish(text, false);
    UeLog("owner done scanned=" + std::to_string(g_ue_cursor) + "/" +
          std::to_string(g_ue_total) + " instance=" + std::to_string(g_ue_instance) +
          " cdo=" + (g_ue_instance_is_cdo ? "1" : "0"));
}

// 把「调用参数」写进参数缓冲：空 = 全零；数字 = 64 位整数；其它 = FName 名字。
bool UeFillParms(std::vector<std::uint8_t>& parms, std::string& detail) {
    const std::string text = UeParam();
    if (text.empty()) {
        detail = "全零";
        return true;
    }
    if (parms.size() < 8) {
        detail = "ParmsSize=" + std::to_string(parms.size()) + "，放不下参数，按全零调用";
        return true;
    }
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    const std::size_t start = hex ? 2 : 0;
    std::uint64_t value{};
    bool numeric = start < text.size();
    for (std::size_t index = start; index < text.size() && numeric; ++index) {
        const char c = text[index];
        std::uint32_t digit{};
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            numeric = false;
            break;
        }
        value = value * (hex ? 16u : 10u) + digit;
    }
    if (numeric) {
        std::memcpy(parms.data(), &value, 8);
        char number[32]{};
        std::snprintf(number, sizeof(number), "%llu", static_cast<unsigned long long>(value));
        detail = std::string(hex ? "0x" : "") + number;
        return true;
    }
    if (UeParseByteList(text, parms, detail)) return true;
    const auto* names = g_context.names;
    std::uint32_t id{};
    if (names == nullptr || names->find_utf8 == nullptr ||
        names->find_utf8(names->user, StringView(text), &id).code != ANOMALY_STATUS_V1_OK ||
        id == 0) {
        detail = "「" + text + "」既不是数字，名字池里也没有它（本次按全零调用）";
        return false;
    }
    value = id;
    std::memcpy(parms.data(), &value, 8);
    detail = "FName「" + text + "」id=" + std::to_string(id);
    return true;
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
    std::string param_detail;
    UeFillParms(parms, param_detail);
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
        (g_ue_instance_is_cdo ? "，对象 CDO" : "，对象实例") + "，参数 " + param_detail + "）";
    if (g_ue_parms_size != 0) {
        detail += "，调用后缓冲 ";
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
        const std::uintptr_t cls = UeClassOf(reader, record.object);
        if (cls != 0) {
            const std::string owner = reader.NameAt(cls + kUeObjectNameOffset);
            std::snprintf(record.class_name, sizeof(record.class_name), "%s", owner.c_str());
        } else {
            std::snprintf(record.class_name, sizeof(record.class_name), "%s", "(未知)");
        }
        record.resolved = true;
        --budget;
    }
    for (std::size_t index = 0; index < g_ue_event_count; ++index) {
        auto& record = g_ue_events[index];
        if (record.decoded || record.parms_size < 8) continue;
        record.decoded = true;
        std::uint64_t raw{};
        std::memcpy(&raw, record.parms_raw, 8);
        record.parms_value = raw;
        // 低 4 字节当 FName id、高 4 字节当 number；number 过大说明这是指针而不是 FName。
        const std::uint32_t id = static_cast<std::uint32_t>(raw & 0xFFFFFFFFull);
        const std::uint32_t number = static_cast<std::uint32_t>(raw >> 32);
        if (id == 0 || number > 4) continue;
        const std::string text = reader.Name(UeFName{id, number});
        // 真 FName 的索引很小、名字也短；解析出超长串或含 ? 的，都是索引落在了
        // 字符串池中间造成的伪结果，宁可当作「不是 FName」。
        if (text.empty() || text.size() > 48 || text.find('?') != std::string::npos) continue;
        std::snprintf(record.parms_fname, sizeof(record.parms_fname), "%s", text.c_str());
    }
}

// 把已解析的记录写进插件日志（面板放不下那么多行）。加锁期间只组装字符串，
// 真正的 Log 调用放在解锁之后。
void UeLogRecords() {
    if (!g_ue_recording.load()) return;
    std::array<std::string, 8> lines;
    std::size_t count = 0;
    {
        std::scoped_lock lock(g_ue_mutex);
        for (std::size_t index = 0; index < g_ue_event_count && count < lines.size(); ++index) {
            auto& record = g_ue_events[index];
            if (record.logged || !record.resolved) continue;
            char line[768]{};
            std::snprintf(line, sizeof(line),
                          "rec %s <- %s x%u obj=%llX parms[%u] %s%s%s%s%s",
                          record.function_name, record.class_name, record.count,
                          static_cast<unsigned long long>(record.object), record.parms_size,
                          record.parms_hex,
                          record.parms_fname[0] != '\0' ? "FName=" : "", record.parms_fname,
                          record.pointee_hex[0] != '\0' ? " || *ptr=" : "", record.pointee_hex);
            lines[count++] = std::string("night999 ue rec: ") + line;
            record.logged = true;
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, lines[index]);
    }
}

// 缓存是否仍指向那个刷新函数：比对象自己的 NamePrivate.id（首次定位时记录），再把该 id
// resolve 回名字做字符串比对 —— 刻意不用 find_utf8（实测它对反射函数名返回 NOT_FOUND，
// 之前依赖它导致「缓存永远校验不过、每次都重新扫描」）。地址失效、对象被换、跨会话 id
// 漂移都会被这一步挡下。
bool UeCachedRefreshFunctionValid(const UeReader& reader) {
    if (g_ue_refresh_fn_cache == 0 || g_ue_refresh_fn_name_id == 0) return false;
    std::uint32_t name_id{};
    if (!reader.Read(g_ue_refresh_fn_cache + kUeObjectNameOffset, name_id) ||
        name_id != g_ue_refresh_fn_name_id) {
        return false;
    }
    if (reader.Name(UeFName{g_ue_refresh_fn_name_id, 0}) != std::string(kUeRefreshFunction)) {
        return false;
    }
    const std::uintptr_t cls = UeClassOf(reader, g_ue_refresh_fn_cache);
    return cls != 0 && reader.NameAt(cls + kUeObjectNameOffset) == "Function";
}

// 记录一条定位结果（供后续点击复用）。
void UeRememberRefreshTarget(const UeReader& reader, const std::uintptr_t function,
                             const std::uintptr_t owner, const std::uint32_t parms_size) {
    g_ue_refresh_fn_cache = function;
    g_ue_refresh_owner_cache = owner;
    g_ue_refresh_fn_parms_size = parms_size;
    std::uint32_t name_id{};
    g_ue_refresh_fn_name_id =
        reader.Read(function + kUeObjectNameOffset, name_id) ? name_id : 0;
}

// find_exact 用完整对象路径直接取 UFunction —— 毫秒级，不扫注册表。
// 同一套写法在 UeLocalPlayerState 里已被实测验证过。
bool UeLocateRefreshFunctionByPath(const UeReader& reader, std::uintptr_t& function) {
    const auto* objects = g_context.objects;
    if (objects == nullptr || objects->find_exact == nullptr || !UeEnsureRegistry()) return false;
    AnomalyGenerationHandleV1 handle{};
    if (objects->find_exact(objects->user, StringView(kUeRefreshFunctionPath), &handle).code !=
            ANOMALY_STATUS_V1_OK ||
        handle.id == 0) {
        return false;
    }
    function = UeResolve(reader, objects, handle);
    return function != 0;
}

// 把一个已定位的 UFunction 变成可用目标：校验身份、读参数表、记缓存，然后找实例。
bool UeAdoptRefreshFunction(const UeReader& reader, const std::uintptr_t function) {
    if (function == 0) return false;
    const std::uintptr_t cls = UeClassOf(reader, function);
    if (cls == 0 || reader.NameAt(cls + kUeObjectNameOffset) != "Function") return false;
    if (reader.NameAt(function + kUeObjectNameOffset) != std::string(kUeRefreshFunction)) {
        return false;
    }
    std::uintptr_t owner{};
    if (!reader.Read(function + kUeObjectOuterOffset, owner) || owner == 0) return false;
    std::uint8_t num_parms{};
    std::uint16_t parms_size{};
    reader.Read(function + kUeFunctionNumParmsOffset, num_parms);
    reader.Read(function + kUeFunctionParmsSizeOffset, parms_size);
    g_ue_function = function;
    g_ue_owner_class = owner;
    g_ue_num_parms = num_parms;
    g_ue_parms_size = parms_size;
    g_ue_cdo = 0;
    g_ue_instance = 0;
    g_ue_instance_is_cdo = false;
    g_ue_cursor = 0;
    UeRememberRefreshTarget(reader, function, owner, parms_size);
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_target_name = std::string(kUeRefreshFunction);
        g_ue_params_dump.clear();
    }
    UeResolveInstance(reader);
    return true;
}

// 复用上次的定位结果：不扫描注册表，只做几次读校验 + 重新取一次 PlayerState 实例。
bool UeTryReuseRefreshTarget(const UeReader& reader) {
    if (!UeCachedRefreshFunctionValid(reader)) return false;
    std::uintptr_t owner{};
    if (!reader.Read(g_ue_refresh_fn_cache + kUeObjectOuterOffset, owner) || owner == 0) {
        return false;
    }
    std::uintptr_t state = UeLocalPlayerState(reader);
    if (state == 0 || !UeClassDerivesFrom(reader, UeClassOf(reader, state), owner)) {
        state = g_ue_instance;
    }
    if (state == 0 || !UeClassDerivesFrom(reader, UeClassOf(reader, state), owner)) return false;

    g_ue_function = g_ue_refresh_fn_cache;
    g_ue_owner_class = owner;
    g_ue_parms_size = g_ue_refresh_fn_parms_size;
    g_ue_num_parms = 0;
    g_ue_cdo = 0;
    g_ue_instance = state;
    g_ue_instance_is_cdo = false;
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_target_name = std::string(kUeRefreshFunction);
        g_ue_params_dump.clear();
    }
    return true;
}

// 一键刷新的扫描兜底：用「刷新函数」的全名做一次精确名扫描。
void UeStartQuickScan() {
    UeBeginScan(std::string(kUeRefreshFunction));
    g_ue_fast_scan = true;
}

// 一键刷新怪物的流程：定位 → 逐座营火发送。全程只在 Game 回调域推进。
// 定位优先级：① 上次的缓存（几次读）→ ② find_exact 对象路径（毫秒级）→ ③ 全表扫描（兜底）。
void UeAdvanceRefresh(const UeReader& reader) {
    const int stage = g_ue_refresh_stage.load();
    if (stage == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now > g_ue_refresh_deadline) {
        UeNotice("一键刷新：超时（没能定位到刷新函数或 PlayerState 实例）");
        g_ue_refresh_stage.store(0);
        return;
    }

    if (stage == 1) {  // 定位
        const bool busy = g_ue_phase == UeProbePhase::scanning ||
            g_ue_phase == UeProbePhase::finding_owner;
        if (!g_ue_refresh_selected && !busy) {
            if (UeTryReuseRefreshTarget(reader)) {
                g_ue_refresh_selected = true;
                g_ue_refresh_scan_started = true;
                g_ue_fast_scan = false;
                UePublish("一键刷新：复用上次定位（未扫描）");
                UeLog("refresh: reused cached target, no scan");
                return;
            }
            std::uintptr_t function{};
            if (UeLocateRefreshFunctionByPath(reader, function) &&
                UeAdoptRefreshFunction(reader, function)) {
                g_ue_refresh_selected = true;
                g_ue_refresh_scan_started = true;
                g_ue_fast_scan = false;
                UePublish("一键刷新：按对象路径直接定位（未扫描）");
                UeLog("refresh: located by find_exact path, no scan");
                return;
            }
            g_ue_refresh_scan_start = now;
            UeNotice("一键刷新：缓存与对象路径都不可用，改用扫描定位（较慢）…");
            UeStartQuickScan();
            g_ue_refresh_scan_started = true;
            if (g_ue_phase != UeProbePhase::scanning) {
                UeNotice("一键刷新：扫描无法开始（缺 ue5 服务，或名字池未就绪）");
                g_ue_refresh_stage.store(0);
            }
            return;
        }
        if (busy) {
            UePublish("一键刷新：定位中 " + std::to_string(g_ue_cursor) + "/" +
                      std::to_string(g_ue_total) + " …");
            return;
        }
        if (!g_ue_refresh_selected) {
            int reset_index = -1;
            for (std::size_t index = 0; index < g_ue_matches.size(); ++index) {
                if (g_ue_matches[index].is_function &&
                    std::string_view(g_ue_matches[index].name) == kUeRefreshFunction) {
                    reset_index = static_cast<int>(index);
                    break;
                }
            }
            if (reset_index < 0) {
                UeNotice("一键刷新：注册表里没有 " + std::string(kUeRefreshFunction) +
                         " —— 先在游戏里打开一次营地让蓝图加载");
                g_ue_refresh_stage.store(0);
                return;
            }
            const UeMatch& match = g_ue_matches[static_cast<std::size_t>(reset_index)];
            UeRememberRefreshTarget(reader, match.object, match.owner, match.parms_size);
            UeLog("refresh: located by scan, took " +
                  UeSecondsText(
                      std::chrono::duration<double>(now - g_ue_refresh_scan_start).count()) +
                  "s");
            // 只找实例，不让它顺手调用（真正的调用在下一阶段，参数要逐个给营火 ID）。
            g_ue_auto_call = false;
            UeBeginOwnerScan(reader, static_cast<std::size_t>(reset_index));
            g_ue_refresh_selected = true;
            return;
        }
        if (g_ue_instance == 0) {
            UeNotice("一键刷新：没找到活着的 PlayerState 实例（先进入游戏世界）");
            g_ue_refresh_stage.store(0);
            return;
        }
        g_ue_refresh_stage.store(2);
        return;
    }

    // stage == 2：逐座营火发送刷新请求（服务器会忽略不在身边的那几座）。
    if (!g_ue_refresh_fire_started) {
        std::uintptr_t address{};
        std::uintptr_t data{};
        std::uint32_t num{};
        const bool have_list =
            UeFindField(reader, g_ue_instance, "ActivatedCampfireIDs", address) &&
            reader.Read(address, data) && reader.Read(address + 8, num) && data > 0x10000 &&
            num > 0 && num <= 16;
        if (!have_list) {
            const bool ok = UeCallByName(reader, UeCampfireName());
            UeNotice(ok ? "一键刷新：未读到营火 ID 列表，已按营火名发送"
                        : "一键刷新：未读到 ActivatedCampfireIDs，名字池里也没有该营火名");
            if (ok) g_ue_refresh_ok.store(1);
            g_ue_refresh_stage.store(0);
            return;
        }
        g_ue_campfire_count = 0;
        for (std::uint32_t index = 0; index < num; ++index) {
            std::uint64_t id{};
            if (!reader.Read(data + 8 * index, id)) break;
            g_ue_campfire_ids[g_ue_campfire_count++] = id;
        }
        g_ue_campfire_index = 0;
        g_ue_campfire_next = now;
        g_ue_refresh_fire_started = true;
        UePublish("一键刷新：向 " + std::to_string(g_ue_campfire_count) + " 座营火发送刷新请求…");
        UeLog("refresh: firing " + std::to_string(g_ue_campfire_count) + " campfire ids");
        return;
    }
    if (g_ue_campfire_index >= g_ue_campfire_count) {
        UeNotice("一键刷新：已发出刷新请求（营火 " + std::to_string(g_ue_campfire_count) +
                 " 座，只有你身边那座会生效）");
        UeLog("refresh: done");
        g_ue_refresh_ok.store(1);
        g_ue_refresh_stage.store(0);
        return;
    }
    if (now >= g_ue_campfire_next) {
        const std::uint64_t id = g_ue_campfire_ids[g_ue_campfire_index];
        std::uint8_t buffer[64]{};
        std::memcpy(buffer, &id, sizeof(id));
        const bool ok = g_ue_parms_size <= sizeof(buffer) &&
            UeInvoke(reader, g_ue_instance, g_ue_function, buffer);
        const std::string name =
            reader.Name(UeFName{static_cast<std::uint32_t>(id & 0xFFFFFFFFull), 0});
        UeLog("refresh: campfire [" + std::to_string(g_ue_campfire_index) + "] " +
              (name.empty() ? std::string("?") : name) + (ok ? " ok" : " fail"));
        UePublish("一键刷新：营火 [" + std::to_string(g_ue_campfire_index) + "] " +
                  (name.empty() ? std::string("?") : name) + (ok ? " 已发送" : " 发送失败"));
        ++g_ue_campfire_index;
        g_ue_campfire_next = now + kUeRefreshFireInterval;
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
            UeClearSeen();
        }
        AnomalyGenerationHandleV1 handle{};
        if (g_context.events->subscribe(g_context.events->user, UeObserve, nullptr, &handle).code ==
                ANOMALY_STATUS_V1_OK &&
            handle.id != 0) {
            g_ue_event_subscription = handle;
            UeFinish("已开始记录：命中名单内的函数会连实参一起记下", false);
        } else {
            UeFinish("事件订阅失败（需要 game-events）", true);
        }
    }
    UeResolveEvents(reader);
    UeLogRecords();

    if (g_ue_fields_requested.exchange(false)) {
        std::vector<std::string> lines;
        UeBuildFields(reader, lines, UeKeyword());
        std::scoped_lock lock(g_ue_mutex);
        g_ue_field_lines = std::move(lines);
        g_ue_field_cursor = 0;
        g_ue_field_logged = 0;
    }
    {
        std::string batch;
        {
            std::scoped_lock lock(g_ue_mutex);
            if (g_ue_field_cursor < g_ue_field_lines.size()) {
                batch = "fields: ";
                std::size_t taken = 0;
                while (g_ue_field_cursor < g_ue_field_lines.size() && taken < 12) {
                    batch += g_ue_field_lines[g_ue_field_cursor++] + " | ";
                    ++taken;
                }
                if (g_ue_field_cursor >= g_ue_field_lines.size()) batch += "（转储结束）";
                g_ue_field_logged = g_ue_field_cursor;
            }
        }
        if (!batch.empty()) Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, batch);
    }

    // 「依次尝试营火 ID」：读实例上的 ActivatedCampfireIDs（TArray<FName>），
    // 逐个作为参数调用当前目标（典型是 FinalTowerCampResetMonsterSpawners），
    // 每个间隔 600ms，日志里带上解析出的名字，便于判断哪个是当前营火。
    if (g_ue_refresh_stage.load() != 0) {
        // 一键刷新正在跑：清掉手动营火按钮的请求，避免两边共用同一组索引互相踩踏。
        g_ue_campfire_requested.store(false);
        g_ue_campfire_single_requested.store(false);
    }
    if (g_ue_campfire_requested.exchange(false)) {
        g_ue_campfire_running = false;
        g_ue_campfire_count = 0;
        g_ue_campfire_index = 0;
        std::uintptr_t address{};
        std::uintptr_t data{};
        std::uint32_t num{};
        if (g_ue_function == 0 || g_ue_instance == 0) {
            UeSetCallResultBoth("请先探测并「选用」FinalTowerCampResetMonsterSpawners（实例需为 PlayerState）");
        } else if (!UeFindField(reader, g_ue_instance, "ActivatedCampfireIDs", address)) {
            UeSetCallResultBoth("该实例上没有 ActivatedCampfireIDs 字段（实例可能不是 PlayerState）");
        } else if (!reader.Read(address, data) || !reader.Read(address + 8, num) ||
                   data <= 0x10000 || num == 0 || num > 16) {
            UeSetCallResultBoth("ActivatedCampfireIDs 为空或不可读");
        } else {
            std::string text = "营火 ID 共 " + std::to_string(num) + " 个：";
            for (std::uint32_t index = 0; index < num; ++index) {
                std::uint64_t id{};
                if (!reader.Read(data + 8 * index, id)) break;
                g_ue_campfire_ids[g_ue_campfire_count++] = id;
                const std::string name = reader.Name(
                    UeFName{static_cast<std::uint32_t>(id & 0xFFFFFFFFull), 0});
                text += " [" + std::to_string(index) + "]" + (name.empty() ? "?" : name);
            }
            UePublish(text + " —— 开始逐个调用");
            UeLog(text);
            g_ue_campfire_running = true;
            g_ue_campfire_next = std::chrono::steady_clock::now();
        }
    }
    if (g_ue_campfire_single_requested.exchange(false)) {
        if (g_ue_function == 0 || g_ue_instance == 0) {
            UeSetCallResultBoth("请先探测并「选用」FinalTowerCampResetMonsterSpawners（实例需为 PlayerState）");
        } else if (UeCallByName(reader, UeCampfireName())) {
            // 名字在池里且调用未抛异常：不再走「依次尝试」。
        } else {
            UeSetCallResultBoth("名字池里没有该营火名，改为依次尝试全部 ID");
            g_ue_campfire_requested.store(true);
        }
    }
    if (g_ue_campfire_running && g_ue_phase != UeProbePhase::scanning &&
        g_ue_phase != UeProbePhase::finding_owner) {
        const auto now = std::chrono::steady_clock::now();
        if (g_ue_campfire_index >= g_ue_campfire_count) {
            g_ue_campfire_running = false;
            UeSetCallResultBoth("已依次尝试 " + std::to_string(g_ue_campfire_count) + " 个营火 ID");
        } else if (now >= g_ue_campfire_next) {
            const std::uint64_t id = g_ue_campfire_ids[g_ue_campfire_index];
            // ⛔ 参数缓冲必须按目标的 ParmsSize 开足：只给 8 字节而目标写更多会踩栈。
            std::uint8_t buffer[64]{};
            std::memcpy(buffer, &id, sizeof(id));
            const bool ok = g_ue_parms_size <= sizeof(buffer) &&
                UeInvoke(reader, g_ue_instance, g_ue_function, buffer);
            const std::string name =
                reader.Name(UeFName{static_cast<std::uint32_t>(id & 0xFFFFFFFFull), 0});
            std::string back;
            if (ok && g_ue_parms_size != 0) {
                back = " 返回 ";
                for (std::uint32_t index = 0; index < g_ue_parms_size && index < 16; ++index) {
                    char item[8]{};
                    std::snprintf(item, sizeof(item), "%02X ", buffer[index]);
                    back += item;
                }
            }
            const std::string text = "campfire [" + std::to_string(g_ue_campfire_index) + "] " +
                (name.empty() ? std::string("?") : name) + " id=" + std::to_string(id) +
                (ok ? " 已调用" : " 调用失败") + back;
            UeLog(text);
            UeSetCallResultBoth(text);
            ++g_ue_campfire_index;
            g_ue_campfire_next = now + std::chrono::milliseconds(120);
        }
    }

    // 一键刷新：按下按钮则进入流程（阶段推进在 UeAdvanceRefresh 内，仍在 Game 线程）。
    if (g_ue_refresh_requested.exchange(false) && g_ue_refresh_stage.load() == 0) {
        g_ue_refresh_stage.store(1);
        g_ue_refresh_done.store(0);
        g_ue_refresh_ok.store(0);
        g_ue_refresh_scan_started = false;
        g_ue_refresh_selected = false;
        g_ue_refresh_fire_started = false;
        g_ue_refresh_deadline = std::chrono::steady_clock::now() + kUeRefreshTimeout;
    }
    {
        // 刷新流程的完成由这里统一判定：进入时非空闲、离开时空闲 ⇒ 这一趟结束了。
        const int stage_before = g_ue_refresh_stage.load();
        UeAdvanceRefresh(reader);
        if (stage_before != 0 && g_ue_refresh_stage.load() == 0) g_ue_refresh_done.store(1);
    }

    if (g_ue_probe_requested.exchange(false)) {
        UeBeginScan(UeKeyword());
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
        if (g_ue_instance == 0 && g_ue_cdo != 0) {
            g_ue_instance = g_ue_cdo;
            g_ue_instance_is_cdo = true;
            UeLog("no live instance found; falling back to CDO");
        }
        UeFinishOwnerScan(reader);
        if (g_ue_auto_call) UeCallFunction();
    }
}

void UeStop() noexcept {
    g_ue_probe_requested.store(false);
    g_ue_call_requested.store(false);
    g_ue_record_requested.store(false);
    g_ue_record_all.store(false);
    g_ue_clear_log_requested.store(false);
    g_ue_select_requested.store(-1);
    g_ue_fields_requested.store(false);
    g_ue_campfire_requested.store(false);
    g_ue_campfire_single_requested.store(false);
    g_ue_refresh_requested.store(false);
    g_ue_refresh_stage.store(0);
    g_ue_refresh_scan_started = false;
    g_ue_refresh_selected = false;
    g_ue_refresh_fire_started = false;
    g_ue_campfire_running = false;
    g_ue_campfire_count = 0;
    g_ue_campfire_index = 0;
    g_ue_phase = UeProbePhase::idle;
    g_ue_auto_call = false;
    g_ue_cursor = 0;
    g_ue_total = 0;
    g_ue_function = 0;
    g_ue_owner_class = 0;
    g_ue_instance = 0;
    g_ue_cdo = 0;
    g_ue_monitor_count = 0;
    g_ue_monitor_class_count = 0;
    g_ue_recording.store(false);
    {
        std::scoped_lock lock(g_ue_mutex);
        g_ue_matches.clear();
        g_ue_match_total = 0;
        g_ue_target_name.clear();
        g_ue_params_dump.clear();
    }
    if (g_context.events != nullptr && g_ue_event_subscription.id != 0 &&
        g_context.events->unsubscribe != nullptr) {
        g_context.events->unsubscribe(g_context.events->user, g_ue_event_subscription);
    }
    g_ue_event_subscription = {};
}

}  // namespace
