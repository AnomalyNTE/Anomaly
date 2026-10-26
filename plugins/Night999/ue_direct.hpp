#pragma once
// 直接调用：按名称定位 UFunction，再用 ProcessEvent 调用。
// 枚举走宿主的 anomaly.ue5.objects，命中后才用 ue_access.hpp 的 UeResolve 取真实地址；
// 实例判定沿 SuperStruct 链。刷新函数定位顺序：缓存 → find_exact 对象路径 → 全表扫描。
// 扫描与调用只在 Game 回调域（on_update）执行。

namespace {

// 刷新本体挂在 AHTPlayerState 上：FinalTowerCampResetMonsterSpawners(InCampfireID)。
constexpr std::string_view kUeRefreshFunction = "FinalTowerCampResetMonsterSpawners";
// find_exact 用的完整对象路径（包.类.成员）。
constexpr std::string_view kUeRefreshFunctionPath =
    "/Script/HTGame.HTPlayerState.FinalTowerCampResetMonsterSpawners";
// 营火名（FName）：读不到 ActivatedCampfireIDs 时按名字发一次。
constexpr std::string_view kUeCampfireDefault = "FT_Castle_Campfire04";

constexpr std::size_t kUeTextCapacity = 96;
constexpr std::size_t kUeMatchCapacity = 16;
// 单帧扫描槽位上限：名字扫描按时间片截断，找实例没有时间片，所以预算小一些。
constexpr std::uint32_t kUeScanSlotBudget = 32768;
constexpr std::uint32_t kUeOwnerSlotBudget = 8192;
// 单帧解析名字的预算：解析一次要过一次宿主的名字池。
constexpr std::uint32_t kUeResolveBudget = 1024;
constexpr std::chrono::microseconds kUeScanSlice{4000};
constexpr std::chrono::milliseconds kUeRefreshFireInterval{120};
constexpr std::chrono::seconds kUeRefreshTimeout{20};

enum class UeProbePhase : std::uint8_t { idle, scanning, finding_owner, ready, failed };

struct UeMatch {
    std::uintptr_t object{};
    std::uintptr_t owner{};
    bool is_function{};
    std::uint32_t parms_size{};
    char name[kUeTextCapacity]{};
    char owner_name[kUeTextCapacity]{};
};

// --- 脚本循环的 handoff（界面与生命周期线程置位，Game 线程消费）---
// 刷新怪物：定位 PlayerState + 刷新函数 → 逐座营火发送。
std::atomic_bool g_ue_refresh_requested{};
// 0=空闲 1=定位 2=发送。
std::atomic_int g_ue_refresh_stage{0};
// 一次刷新流程结束（任何出口）时置 1，供脚本循环等待；发起前清零。
std::atomic_int g_ue_refresh_done{0};
// 上一次刷新是否确实把请求发出去了（诊断用）。
std::atomic_int g_ue_refresh_ok{0};

// --- 扫描状态（Game 线程独占）---
UeProbePhase g_ue_phase{UeProbePhase::idle};
bool g_ue_ids_only{};
std::uint32_t g_ue_name_id{};
std::uint32_t g_ue_cursor{};
std::uint32_t g_ue_total{};
std::uint32_t g_ue_match_total{};
std::string g_ue_keyword_scan;
std::uintptr_t g_ue_function{};
std::uintptr_t g_ue_owner_class{};
std::uintptr_t g_ue_instance{};
std::uintptr_t g_ue_cdo{};
bool g_ue_instance_is_cdo{};
std::uint32_t g_ue_parms_size{};
std::vector<UeMatch> g_ue_matches;
std::string g_ue_target_name;
std::uint64_t g_ue_campfire_ids[16]{};
std::size_t g_ue_campfire_count{};
std::size_t g_ue_campfire_index{};
std::chrono::steady_clock::time_point g_ue_campfire_next{};

// 刷新的流程状态。
bool g_ue_refresh_selected{};
bool g_ue_refresh_fire_started{};
std::chrono::steady_clock::time_point g_ue_refresh_deadline{};
std::chrono::steady_clock::time_point g_ue_refresh_scan_start{};
// 定位缓存：函数地址 / FName id / 参数大小。
std::uintptr_t g_ue_refresh_fn_cache{};
std::uint32_t g_ue_refresh_fn_name_id{};
std::uint32_t g_ue_refresh_fn_parms_size{};

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

void UeFinish(std::string text, const bool failed) {
    g_ue_phase = failed ? UeProbePhase::failed : UeProbePhase::ready;
    g_ue_cursor = 0;
    UeLog(text);
}

std::string UeSecondsText(const double seconds) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.1f", seconds);
    return text;
}

// 按营火名（FName）单发调用当前目标：名字 → find_utf8 → 8 字节参数。
// 参数缓冲必须按 ParmsSize 开足，目标写更多字节会踩栈。
bool UeCallByName(const UeReader& reader, const std::string& name) {
    const auto* names = g_context.names;
    std::uint32_t id{};
    if (names == nullptr || names->find_utf8 == nullptr ||
        names->find_utf8(names->user, StringView(name), &id).code != ANOMALY_STATUS_V1_OK ||
        id == 0) {
        return false;
    }
    std::uint64_t value = id;
    std::uint8_t buffer[64]{};
    std::memcpy(buffer, &value, sizeof(value));
    const bool ok = g_ue_parms_size <= sizeof(buffer) &&
        UeInvoke(reader, g_ue_instance, g_ue_function, buffer);
    UeLog("campfire by-name " + name + " id=" + std::to_string(id) + (ok ? " ok" : " fail"));
    return ok;
}

void UeAddMatch(const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects,
                const AnomalyUe5ObjectSnapshotV1& snapshot, const std::string& name) {
    ++g_ue_match_total;
    if (g_ue_matches.size() >= kUeMatchCapacity) return;
    UeMatch match{};
    std::snprintf(match.name, sizeof(match.name), "%s", name.c_str());
    match.object = UeResolve(reader, objects, snapshot.handle);
    if (match.object != 0) {
        reader.Read(match.object + kUeObjectOuterOffset, match.owner);
        const std::uintptr_t cls = UeClassOf(reader, match.object);
        if (cls != 0) {
            match.is_function = reader.NameAt(cls + kUeObjectNameOffset) == "Function";
        }
        if (match.owner != 0) {
            const std::string owner_name = reader.NameAt(match.owner + kUeObjectNameOffset);
            std::snprintf(match.owner_name, sizeof(match.owner_name), "%s", owner_name.c_str());
        }
        if (match.is_function) {
            std::uint16_t parms_size{};
            reader.Read(match.object + kUeFunctionParmsSizeOffset, parms_size);
            match.parms_size = parms_size;
        }
    }
    g_ue_matches.push_back(match);
}

void UeScanStep(const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects) {
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    const std::uint32_t end = std::min(g_ue_cursor + kUeScanSlotBudget, g_ue_total);
    const auto slice_start = std::chrono::steady_clock::now();
    std::uint32_t resolved{};
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
            if (g_ue_matches.size() >= kUeMatchCapacity) return;
            continue;
        }
        if (resolved >= kUeResolveBudget) return;
        ++resolved;
        const std::string text = reader.Name(UeFName{snapshot.name_id, 0});
        if (text.empty() || UeLower(text).find(g_ue_keyword_scan) == std::string::npos) continue;
        UeAddMatch(reader, objects, snapshot, text);
        if (g_ue_matches.size() >= kUeMatchCapacity) return;
    }
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
    // 兜底全表扫描：这条路径没经过 UeBeginScan，要自己准备注册表与总数。
    static_cast<void>(UeEnsureRegistry());
    const auto* fallback = g_context.objects;
    if (fallback != nullptr && fallback->count != nullptr) {
        g_ue_total = fallback->count(fallback->user);
    }
    g_ue_phase = UeProbePhase::finding_owner;
    g_ue_cursor = 0;
}

// 定位的兜底路径：用刷新函数全名做一次精确名扫描。
void UeBeginScan() {
    g_ue_matches.clear();
    g_ue_match_total = 0;
    g_ue_function = 0;
    g_ue_owner_class = 0;
    g_ue_instance = 0;
    g_ue_cdo = 0;
    g_ue_instance_is_cdo = false;
    g_ue_parms_size = 0;
    g_ue_cursor = 0;
    g_ue_total = 0;
    g_ue_name_id = 0;
    g_ue_ids_only = false;
    g_ue_target_name.clear();

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

    // 名字池里注册过就按精确名称扫，否则逐对象解析名字做子串匹配。
    g_ue_ids_only = names->find_utf8(names->user, StringView(kUeRefreshFunction), &g_ue_name_id)
            .code == ANOMALY_STATUS_V1_OK &&
        g_ue_name_id != 0;
    if (!g_ue_ids_only) g_ue_keyword_scan = UeLower(std::string(kUeRefreshFunction));
    g_ue_phase = UeProbePhase::scanning;
    UeLog("scan keyword=" + std::string(kUeRefreshFunction) + " total=" +
          std::to_string(g_ue_total) + (g_ue_ids_only ? " (exact)" : " (substring)"));
}

void UeFinishScan() {
    if (g_ue_matches.empty()) {
        UeFinish("注册表里没有任何对象名含「" + std::string(kUeRefreshFunction) + "」（共 " +
                     std::to_string(g_ue_total) + " 个对象）——相关蓝图尚未加载",
                 true);
        return;
    }
    std::stable_partition(g_ue_matches.begin(), g_ue_matches.end(),
                          [](const UeMatch& match) { return match.is_function; });
    std::string list;
    for (const auto& match : g_ue_matches) {
        list += std::string(match.name) + "[" + match.owner_name + "] ";
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
    g_ue_parms_size = match.parms_size;
    g_ue_instance = 0;
    g_ue_cdo = 0;
    g_ue_instance_is_cdo = false;
    g_ue_cursor = 0;
    g_ue_target_name = match.name;
    UeLog("select #" + std::to_string(index) + " " + match.name + " owner=" + match.owner_name +
          " parms=" + std::to_string(g_ue_parms_size));
    if (match.object == 0) {
        UeFinish("无法解析「" + std::string(match.name) + "」的对象地址（注册表基址未就绪）",
                 true);
        return;
    }
    if (g_ue_owner_class == 0) {
        UeFinish("「" + std::string(match.name) + "」没有 Outer，无法定位所属类实例", true);
        return;
    }
    UeResolveInstance(reader);
    if (g_ue_instance != 0) UeFinishOwnerScan(reader);
}

void UeOwnerStep(const UeReader& reader, const AnomalyUe5ObjectsServiceV1* objects) {
    AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    const std::uint32_t end = std::min(g_ue_cursor + kUeOwnerSlotBudget, g_ue_total);
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
        return;
    }
}

void UeFinishOwnerScan(const UeReader& reader) {
    std::string text = "目标「" + g_ue_target_name + "」→ 类 ";
    text += g_ue_owner_class != 0 ? reader.NameAt(g_ue_owner_class + kUeObjectNameOffset)
                                  : std::string("(未知)");
    text += g_ue_instance != 0
        ? (g_ue_instance_is_cdo ? "，只有 CDO（没有活实例）" : "，活实例已找到")
        : "，未找到该类实例（该函数可能只在界面打开时才存在）";
    text += "，扫描 " + std::to_string(g_ue_cursor) + "/" + std::to_string(g_ue_total);
    UeFinish(text, false);
}

// 缓存是否仍指向刷新函数：比 NamePrivate.id，再 resolve 回名字。
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

// 记录一条定位结果（供后续复用）。
void UeRememberRefreshTarget(const UeReader& reader, const std::uintptr_t function,
                             const std::uint32_t parms_size) {
    g_ue_refresh_fn_cache = function;
    g_ue_refresh_fn_parms_size = parms_size;
    std::uint32_t name_id{};
    g_ue_refresh_fn_name_id =
        reader.Read(function + kUeObjectNameOffset, name_id) ? name_id : 0;
}

// find_exact 用完整对象路径直接取 UFunction —— 毫秒级，不扫注册表。
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
    std::uint16_t parms_size{};
    reader.Read(function + kUeFunctionParmsSizeOffset, parms_size);
    g_ue_function = function;
    g_ue_owner_class = owner;
    g_ue_parms_size = parms_size;
    g_ue_cdo = 0;
    g_ue_instance = 0;
    g_ue_instance_is_cdo = false;
    g_ue_cursor = 0;
    UeRememberRefreshTarget(reader, function, parms_size);
    g_ue_target_name = std::string(kUeRefreshFunction);
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
    g_ue_cdo = 0;
    g_ue_instance = state;
    g_ue_instance_is_cdo = false;
    g_ue_target_name = std::string(kUeRefreshFunction);
    return true;
}

// 刷新怪物的流程：定位 → 逐座营火发送。全程只在 Game 回调域推进。
void UeAdvanceRefresh(const UeReader& reader) {
    const int stage = g_ue_refresh_stage.load();
    if (stage == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now > g_ue_refresh_deadline) {
        UeLog("refresh: timeout, refresh function or PlayerState instance not found");
        g_ue_refresh_stage.store(0);
        return;
    }

    if (stage == 1) {  // 定位
        const bool busy = g_ue_phase == UeProbePhase::scanning ||
            g_ue_phase == UeProbePhase::finding_owner;
        if (!g_ue_refresh_selected && !busy) {
            if (UeTryReuseRefreshTarget(reader)) {
                g_ue_refresh_selected = true;
                UeLog("refresh: reused cached target, no scan");
                return;
            }
            std::uintptr_t function{};
            if (UeLocateRefreshFunctionByPath(reader, function) &&
                UeAdoptRefreshFunction(reader, function)) {
                g_ue_refresh_selected = true;
                UeLog("refresh: located by find_exact path, no scan");
                return;
            }
            g_ue_refresh_scan_start = now;
            UeLog("refresh: cache and object path unavailable, falling back to a full scan");
            UeBeginScan();
            if (g_ue_phase != UeProbePhase::scanning) {
                UeLog("refresh: scan could not start (missing ue5 services or name pool)");
                g_ue_refresh_stage.store(0);
            }
            return;
        }
        if (busy) return;  // 进度由 UeAdvance 的扫描步进推进
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
                UeLog("refresh: registry has no " + std::string(kUeRefreshFunction) +
                      " — open a camp in game once so the blueprint loads");
                g_ue_refresh_stage.store(0);
                return;
            }
            const UeMatch& match = g_ue_matches[static_cast<std::size_t>(reset_index)];
            UeRememberRefreshTarget(reader, match.object, match.parms_size);
            UeLog("refresh: located by scan, took " +
                  UeSecondsText(
                      std::chrono::duration<double>(now - g_ue_refresh_scan_start).count()) +
                  "s");
            UeBeginOwnerScan(reader, static_cast<std::size_t>(reset_index));
            g_ue_refresh_selected = true;
            return;
        }
        if (g_ue_instance == 0) {
            UeLog("refresh: no live PlayerState instance, enter the world first");
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
            const bool ok = UeCallByName(reader, std::string(kUeCampfireDefault));
            UeLog(ok ? "refresh: ActivatedCampfireIDs unreadable, sent by campfire name"
                     : "refresh: no ActivatedCampfireIDs and the campfire name is not in the pool");
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
        UeLog("refresh: firing " + std::to_string(g_ue_campfire_count) + " campfire ids");
        return;
    }
    if (g_ue_campfire_index >= g_ue_campfire_count) {
        UeLog("refresh: done, " + std::to_string(g_ue_campfire_count) + " campfire ids sent");
        g_ue_refresh_ok.store(1);
        g_ue_refresh_stage.store(0);
        return;
    }
    if (now >= g_ue_campfire_next) {
        const std::uint64_t id = g_ue_campfire_ids[g_ue_campfire_index];
        // 参数缓冲必须按目标的 ParmsSize 开足：只给 8 字节而目标写更多会踩栈。
        std::uint8_t buffer[64]{};
        std::memcpy(buffer, &id, sizeof(id));
        const bool ok = g_ue_parms_size <= sizeof(buffer) &&
            UeInvoke(reader, g_ue_instance, g_ue_function, buffer);
        const std::string name =
            reader.Name(UeFName{static_cast<std::uint32_t>(id & 0xFFFFFFFFull), 0});
        UeLog("refresh: campfire [" + std::to_string(g_ue_campfire_index) + "] " +
              (name.empty() ? std::string("?") : name) + (ok ? " ok" : " fail"));
        ++g_ue_campfire_index;
        g_ue_campfire_next = now + kUeRefreshFireInterval;
    }
}

void UeAdvance(const UeReader& reader) {
    // 脚本请求刷新时进入流程（阶段推进在 UeAdvanceRefresh 内，仍在 Game 线程）。
    if (g_ue_refresh_requested.exchange(false) && g_ue_refresh_stage.load() == 0) {
        g_ue_refresh_stage.store(1);
        g_ue_refresh_done.store(0);
        g_ue_refresh_ok.store(0);
        g_ue_refresh_selected = false;
        g_ue_refresh_fire_started = false;
        g_ue_refresh_deadline = std::chrono::steady_clock::now() + kUeRefreshTimeout;
    }
    {
        // 完成判据：进入时非空闲、离开时空闲 ⇒ 这一趟结束了。
        const int stage_before = g_ue_refresh_stage.load();
        UeAdvanceRefresh(reader);
        if (stage_before != 0 && g_ue_refresh_stage.load() == 0) g_ue_refresh_done.store(1);
    }

    // 扫描与找实例的步进（只在刷新定位期间发生）。
    if (g_ue_phase != UeProbePhase::scanning && g_ue_phase != UeProbePhase::finding_owner) return;
    const auto* objects = g_context.objects;
    if (objects == nullptr || objects->snapshot_at == nullptr) {
        UeFinish("ue5-objects 服务已不可用", true);
        return;
    }
    if (g_ue_phase == UeProbePhase::scanning) {
        UeScanStep(reader, objects);
        if (g_ue_cursor >= g_ue_total || g_ue_matches.size() >= kUeMatchCapacity) UeFinishScan();
        return;
    }
    UeOwnerStep(reader, objects);
    if (g_ue_cursor >= g_ue_total || g_ue_instance != 0) {
        if (g_ue_instance == 0 && g_ue_cdo != 0) {
            g_ue_instance = g_ue_cdo;
            g_ue_instance_is_cdo = true;
            UeLog("no live instance found; falling back to CDO");
        }
        UeFinishOwnerScan(reader);
    }
}

void UeStop() noexcept {
    g_ue_refresh_requested.store(false);
    g_ue_refresh_stage.store(0);
    g_ue_refresh_selected = false;
    g_ue_refresh_fire_started = false;
    g_ue_campfire_count = 0;
    g_ue_campfire_index = 0;
    g_ue_phase = UeProbePhase::idle;
    g_ue_cursor = 0;
    g_ue_total = 0;
    g_ue_function = 0;
    g_ue_owner_class = 0;
    g_ue_instance = 0;
    g_ue_cdo = 0;
}

}  // namespace
