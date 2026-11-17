#include "native_layout.hpp"
// Anomaly 插件：空无一人（一键把人群密度环境倍率压到近乎为 0）
//    勾选：读取当前 昼/夜/晴/雨/雪 环境倍率作为初值 → 全部置 0.01
//    取消：恢复初值
// 目标字段：HTMassCrowdSpawner（默认档 +kDefaultProfileOffset、数组 +kDensityProfilesOffset）
//          内 HTMassCrowdDensityProfile 的 +0x0C/+0x10/+0x14/+0x18/+0x1C（1.4b 起未变）。
// 等待世界就绪后扫描；界面可请求重新扫描。
//
// 兼容策略（游戏每次更新只会动这几处）：
//   1. 版本无关的字段全部来自 Profile 的 layout 块；随版本变化的三处偏移放在 layout 符号的
//      candidates[].values 里，签名命中时优先采用。
//   2. 签名全部失配（补丁改了函数体）时，改由真实生成器对象的形状判定唯一候选：数组 TArray
//      头、默认档 5 个倍率、车辆保底数量都必须自洽，且只有一个候选成立才生效；否则整插件
//      停用并显示原因，绝不回退到代码里的数字。
//   3. 天气切换/关卡刷新会让游戏重算密度，所以勾选期间按慢周期重新对齐（读-比-写）。

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/localization.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

using anomaly::sdk::Host;
using anomaly::sdk::StringView;

#define HAS(ui, field)                                                        \
    ((ui) != nullptr &&                                                       \
     (ui)->struct_size >=                                                     \
         offsetof(AnomalyUiServiceV1, field) + sizeof((ui)->field) &&         \
     (ui)->field != nullptr)

constexpr std::uint32_t kCondFirstUseEver = 4u;

// UObject 布局



// HTMassCrowdSpawner 上的密度字段（NTE 1.5a：DefaultDensityProfile 0x458、DensityProfiles 0x478，
// 相比 1.4b 的 0x408/0x428 整体后移 0x50 —— 基类 HTMassSpawnerBase 由 0x80 长到 0xD0 所致；
// AMassSpawner.Count(+0x2E8)/SpawningCountScale(+0x328) 并未移动）

  // TArray


constexpr std::uint32_t kMaxProfiles = 16;
constexpr int kMaxSpawners = 256;
// 勾选期间按慢周期重新对齐：游戏在天气切换/关卡刷新时会重算密度。
constexpr double kReapplySeconds = 1.0;
// 扫描走完但仍缺目标（换关、布局待判定）时的重试周期。
constexpr double kRetrySeconds = 2.0;
const float kEmptyScales[5] = {0.01F, 0.01F, 0.01F, 0.01F, 0.01F};  // 空街值（昼/夜/晴/雨/雪）
const float kDefaultScales[5] = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F};     // 游戏默认环境倍率（兜底初值）

// 车辆生成器（HTMassVehicleSpawner）上的字段
//   MassSpawner 基类：Count(+0x2E8, int32) 生成上限、SpawningCountScale(+0x328, f32) 数量倍率
//   HTMassVehicleSpawner 自己：MinActiveVehiclesNum(+0x4AC, int32) 保底在线车辆数（1.4b 为 0x45C）



constexpr int kDefaultVehCount = 3000;
constexpr float kDefaultVehScale = 0.8F;
constexpr int kDefaultVehMinActive = 50;

// 游戏自带「索引 → 对象地址」助手签名（HTGame.exe .text 唯一命中）


constexpr int kConfVersion = 5;
constexpr const char* kConfFile = "deserted.conf";

// State 头部标记，供外部工具在 .data 中定位本结构
constexpr std::uint64_t kStateMagic = 0x0054442D4D4F4E41ULL;  // "ANOM-DT\0"
constexpr std::uint32_t kStateVersion = 8;
enum LayoutIndex {
    kLayoutChecked = 0,   // 与 req_toggle 相邻，外部可一次写入两个
    kLayoutReqToggle,
    kLayoutBaseline,      // float[5]
    kLayoutBaselineValid,
    kLayoutSpawnerCount,
    kLayoutWrites,
    kLayoutFaults,
    kLayoutStatus,
    kLayoutVehCount,   // int
    kLayoutVehArray,   // uintptr_t[kMaxSpawners]
    kLayoutCount
};

struct State {
    std::uint64_t magic{kStateMagic};
    std::uint32_t struct_version{kStateVersion};
    std::uint32_t layout[kLayoutCount]{};

    // 扫描
    AnomalyGenerationHandleV1 world{};
    int req_rescan{};
    int sig_tried{};
    std::uintptr_t gobjects_slot{};
    std::uint32_t walk_total{};
    std::uint32_t walk_cursor{};
    std::uint64_t walk_generation{};
    int walk_done{};
    std::uint32_t spawner_found_index[kMaxSpawners]{};
    AnomalyGenerationHandleV1 spawner_found_handle[kMaxSpawners]{};
    int spawner_found{};

    // 目标
    std::uintptr_t spawner[kMaxSpawners]{};
    std::uint32_t spawner_index[kMaxSpawners]{};
    AnomalyGenerationHandleV1 spawner_handle[kMaxSpawners]{};
    int spawner_count{};

    // 车辆生成器（同一套扫描流程，名字含 MassVehicleSpawner）
    std::uint32_t veh_found_index[kMaxSpawners]{};
    AnomalyGenerationHandleV1 veh_found_handle[kMaxSpawners]{};
    int veh_found{};
    std::uintptr_t veh[kMaxSpawners]{};
    std::uint32_t veh_index[kMaxSpawners]{};
    AnomalyGenerationHandleV1 veh_handle[kMaxSpawners]{};
    int veh_count{};
    int veh_baseline_valid[kMaxSpawners]{};
    int veh_baseline_count[kMaxSpawners]{};
    float veh_baseline_scale[kMaxSpawners]{};
    int veh_baseline_min[kMaxSpawners]{};
    int veh_has_min_active[kMaxSpawners]{};

    // 开关状态
    int checked{};        // 勾选状态
    int req_toggle{};     // 置 1 让 on_update 处理一次开关（紧邻 checked，便于外部一次写入）
    float baseline[5]{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    int baseline_valid{};
    int need_apply{};
    double validate_accum{};
    double reapply_accum{};  // 勾选期间重新对齐的慢周期累计
    double retry_accum{};    // 扫描走完但仍缺目标时的重试累计

    // 窗口
    int window_open{1};

    // 诊断
    std::uint64_t writes{};
    std::uint64_t faults{};
    char status[192]{};
};
State g_state;

const AnomalyUiServiceV1* g_ui{};
const AnomalyCoreServiceV1* g_core{};
const AnomalySignatureServiceV1* g_signature{};
const AnomalyUe5NamesServiceV1* g_names{};
const AnomalyUe5ObjectsServiceV1* g_objects{};
const AnomalyNteSessionServiceV1* g_session{};
const AnomalyStorageServiceV1* g_storage{};
const AnomalySchedulerServiceV1* g_scheduler{};
anomaly::sdk::Localizer g_localizer;

void SetStatus(const char* text) {
    std::snprintf(g_state.status, sizeof(g_state.status), "%s", (text != nullptr) ? text : "");
}

// --------------------------- 内存访问 ----------------------------------------
bool SafeReadPtr(std::uintptr_t address, std::uintptr_t* out) {
    if (out == nullptr) return false;
    __try {
        *out = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint32_t SafeReadU32(std::uintptr_t address) {
    __try {
        return *reinterpret_cast<const std::uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SafeReadF32(std::uintptr_t address, float* out) {
    if (out == nullptr) return false;
    __try {
        *out = *reinterpret_cast<const volatile float*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LooksLikePointer(std::uintptr_t v) {
    return v > 0x10000ULL && v < 0x800000000000ULL;
}

bool WriteF32(std::uintptr_t address, float value) {
    State& s = g_state;
    if (!LooksLikePointer(address)) {
        ++s.faults;
        return false;
    }
    bool ok = false;
    if (g_core != nullptr && g_core->write_memory != nullptr) {
        AnomalyByteSpanV1 span{};
        span.data = reinterpret_cast<const std::uint8_t*>(&value);
        span.size = sizeof(value);
        ok = g_core->write_memory(g_core->user, address, span).code == ANOMALY_STATUS_V1_OK;
    }
    if (!ok) {
        __try {
            *reinterpret_cast<volatile float*>(address) = value;
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    float back = 0.0F;
    if (ok && SafeReadF32(address, &back) && back == value) {
        ++s.writes;
        return true;
    }
    ++s.faults;
    return false;
}

// --------------------------- 写 u32 ------------------------------------------
bool WriteU32(std::uintptr_t address, std::uint32_t value) {
    State& s = g_state;
    if (!LooksLikePointer(address)) {
        ++s.faults;
        return false;
    }
    bool ok = false;
    if (g_core != nullptr && g_core->write_memory != nullptr) {
        AnomalyByteSpanV1 span{};
        span.data = reinterpret_cast<const std::uint8_t*>(&value);
        span.size = sizeof(value);
        ok = g_core->write_memory(g_core->user, address, span).code == ANOMALY_STATUS_V1_OK;
    }
    if (!ok) {
        __try {
            *reinterpret_cast<volatile std::uint32_t*>(address) = value;
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (!ok) {
        ++s.faults;
        return false;
    }
    if (SafeReadU32(address) == value) {
        ++s.writes;
        return true;
    }
    ++s.faults;
    return false;
}

// --------------------------- 对象定位 ----------------------------------------
void TryResolveGobjectsSlot() {
    State& s = g_state;
    if (s.sig_tried != 0) return;
    s.sig_tried = 1;
    if (g_signature == nullptr || g_signature->resolve == nullptr) {
        SetStatus(g_localizer.Text("status.signature_unavailable", "The signature service is unavailable").c_str());
        return;
    }
    const std::uintptr_t match = deserted_profile::compatibility.Address("objects");
    if (match == 0) {
        SetStatus(g_localizer.Text("status.gobjects_signature_missing",
                     "The object table signature did not match (the game version may have changed)").c_str());
        return;
    }
    const std::int32_t disp = static_cast<std::int32_t>(SafeReadU32(match + 3));
    const std::uintptr_t slot =
        match + 7 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(disp));
    if (!LooksLikePointer(slot)) {
        SetStatus(g_localizer.Text("status.gobjects_address_invalid", "The object table symbol address is invalid").c_str());
        return;
    }
    s.gobjects_slot = slot;
}

std::uintptr_t IndexToAddress(std::uint32_t index) {
    State& s = g_state;
    if (s.gobjects_slot == 0) return 0;
    std::uintptr_t chunk_array = 0;
    if (!SafeReadPtr(s.gobjects_slot, &chunk_array) || chunk_array == 0) return 0;
    std::uintptr_t chunk_ptr = 0;
    if (!SafeReadPtr(chunk_array + (index / deserted_profile::native_layout.kObjectChunkSize) * sizeof(std::uintptr_t), &chunk_ptr) || chunk_ptr == 0) return 0;
    std::uintptr_t object = 0;
    if (!SafeReadPtr(chunk_ptr + (index % deserted_profile::native_layout.kObjectChunkSize) * deserted_profile::native_layout.kObjectItemStride, &object)) return 0;
    return object;
}

bool ResolveCurrentObject(std::uint32_t index, AnomalyGenerationHandleV1 expected,
                          std::uintptr_t* address) {
    if (address == nullptr || expected.id == 0 || g_objects == nullptr ||
        g_objects->snapshot_by_handle == nullptr ||
        ANOMALY_UE5_OBJECT_HANDLE_INDEX(expected) != index) return false;
    AnomalyUe5ObjectSnapshotV1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    if (g_objects->snapshot_by_handle(g_objects->user, expected, &snapshot).code !=
        ANOMALY_STATUS_V1_OK || snapshot.handle.id != expected.id ||
        snapshot.handle.generation != expected.generation) return false;
    const std::uintptr_t current = IndexToAddress(index);
    if (!LooksLikePointer(current)) return false;
    *address = current;
    return true;
}

// 对象类名里是否含 needle（不区分大小写）
bool ClassNameContains(std::uintptr_t addr, const char* needle) {
    if (!LooksLikePointer(addr) || needle == nullptr) return false;
    if (g_names == nullptr || g_names->resolve_utf8 == nullptr) return false;
    std::uintptr_t cls = 0;
    if (!SafeReadPtr(addr + deserted_profile::native_layout.kClassPrivateOffset, &cls) || cls == 0) return false;
    const std::uint32_t id = SafeReadU32(cls + deserted_profile::native_layout.kNamePrivateOffset);
    if (id == 0) return false;
    char name[128]{};
    std::size_t size = sizeof(name);
    if (g_names->resolve_utf8(g_names->user, id, name, &size).code != ANOMALY_STATUS_V1_OK) {
        return false;
    }
    for (const char* p = name; *p != '\0'; ++p) {
        std::size_t k = 0;
        while (needle[k] != '\0' && p[k] != '\0') {
            char a = p[k];
            char b = needle[k];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
            ++k;
        }
        if (needle[k] == '\0') return true;
    }
    return false;
}

bool IsSpawnerName(const char* name) {
    if (name == nullptr || name[0] == '\0') return false;
    if (std::strncmp(name, "Default__", 9) == 0) return false;
    if (std::strstr(name, "Subsystem") != nullptr) return false;
    return std::strstr(name, "MassCrowdSpawner") != nullptr;
}

// ------------------------- 布局候选与对象形状 -----------------------------
const deserted_profile::Candidate& ActiveCandidate() {
    return deserted_profile::candidates[static_cast<std::size_t>(deserted_profile::selected_candidate)];
}

// 默认档的 5 个倍率：有限、0..1000。判定候选时还要求至少一个明显非零，因为写入后的空街值
// （0.01）不能当作“这组偏移是对的”的证据。
bool DefaultScalesSane(std::uintptr_t addr, std::uint32_t profile_offset, bool require_active_values) {
    bool any = false;
    for (int i = 0; i < 5; ++i) {
        float value = -1.0F;
        if (!SafeReadF32(addr + profile_offset + deserted_profile::base_layout.kScaleOffsets[i], &value)) return false;
        if (!(value >= 0.0F) || value > 1000.0F) return false;  // NaN 在这里被拒
        if (value > 0.2F) any = true;
    }
    return require_active_values ? any : true;
}

// 密度 Profile 数组：空数组必须是空指针，非空必须是 1..kMaxProfiles 且数据指针可读。
bool DensityArraySane(std::uintptr_t addr, std::uint32_t profiles_offset) {
    std::uintptr_t data = 0;
    if (!SafeReadPtr(addr + profiles_offset, &data)) return false;
    const std::uint32_t num = SafeReadU32(addr + profiles_offset + deserted_profile::base_layout.kArrayNumOffset);
    if (num == 0) return data == 0;
    return num <= kMaxProfiles && LooksLikePointer(data);
}

bool ShapeMatchesCrowd(std::uintptr_t addr, const deserted_profile::Candidate& candidate,
                       bool require_active_values) {
    if (!LooksLikePointer(addr)) return false;
    if (!DensityArraySane(addr, candidate.kDensityProfilesOffset)) return false;
    return DefaultScalesSane(addr, candidate.kDefaultProfileOffset, require_active_values);
}

bool ShapeMatchesVehicle(std::uintptr_t addr, const deserted_profile::Candidate& candidate,
                         bool has_min_active) {
    if (!LooksLikePointer(addr)) return false;
    const std::int32_t count = static_cast<std::int32_t>(
        SafeReadU32(addr + deserted_profile::base_layout.kSpawnCountOffset));
    float scale = 0.0F;
    if (!SafeReadF32(addr + deserted_profile::base_layout.kSpawnScaleOffset, &scale)) return false;
    if (count < 0 || count > 1000000) return false;
    if (!(scale >= 0.0F) || scale > 100.0F) return false;
    if (!has_min_active) return true;
    const std::int32_t min_active = static_cast<std::int32_t>(SafeReadU32(addr + candidate.kVehMinActiveOffset));
    return min_active >= 0 && min_active <= 10000;
}

// 版本无关：只按类名收敛候选地址。
bool IsCrowdSpawnerCandidate(std::uintptr_t addr) {
    return LooksLikePointer(addr) && ClassNameContains(addr, "MassCrowdSpawner");
}

// 结构校验：布局选定后，用该候选的偏移核对真实对象，防止索引过期指向别的对象。
bool IsSpawnerObject(std::uintptr_t addr) {
    if (!deserted_profile::LayoutSelected()) return false;
    if (!IsCrowdSpawnerCandidate(addr)) return false;
    // 本插件写入空街值后这里必须仍然成立，否则勾选后目标会自己失效。
    return ShapeMatchesCrowd(addr, ActiveCandidate(), false);
}

// --------------------------- 车辆生成器 --------------------------------------
bool IsVehicleSpawnerName(const char* name) {
    if (name == nullptr || name[0] == '\0') return false;
    if (std::strncmp(name, "Default__", 9) == 0) return false;
    if (std::strstr(name, "Subsystem") != nullptr) return false;
    if (std::strstr(name, "Crowd") != nullptr) return false;
    return std::strstr(name, "MassBusSpawner") != nullptr ||
           std::strstr(name, "MassVehicleSpawner") != nullptr ||
           std::strstr(name, "TrafficMonsterMassSpawner") != nullptr ||
           std::strstr(name, "MassTrainSpawner") != nullptr;
}

bool IsVehicleSpawnerCandidate(std::uintptr_t addr) {
    if (!LooksLikePointer(addr)) return false;
    if (ClassNameContains(addr, "Crowd") || ClassNameContains(addr, "Subsystem") ||
        ClassNameContains(addr, "SpawnDataGenerator")) return false;
    return ClassNameContains(addr, "MassBusSpawner") ||
           ClassNameContains(addr, "MassVehicleSpawner") ||
           ClassNameContains(addr, "TrafficMonsterMassSpawner") ||
           ClassNameContains(addr, "MassTrainSpawner");
}

bool VehicleSpawnerHasMinActive(std::uintptr_t addr) {
    // 该字段只属于 HTMassVehicleSpawner；公交和列车在同一偏移放了其他数据。
    return ClassNameContains(addr, "MassVehicleSpawner");
}

bool IsVehicleSpawnerObject(std::uintptr_t addr) {
    if (!deserted_profile::LayoutSelected()) return false;
    if (!IsVehicleSpawnerCandidate(addr)) return false;
    return ShapeMatchesVehicle(addr, ActiveCandidate(), VehicleSpawnerHasMinActive(addr));
}

void ReadVehicleParams(std::uintptr_t addr, int* count, float* scale, int* min_active,
                       bool has_min_active) {
    *count = static_cast<std::int32_t>(SafeReadU32(addr + deserted_profile::native_layout.kSpawnCountOffset));
    if (!SafeReadF32(addr + deserted_profile::native_layout.kSpawnScaleOffset, scale)) *scale = kDefaultVehScale;
    *min_active = has_min_active
        ? static_cast<std::int32_t>(SafeReadU32(addr + deserted_profile::native_layout.kVehMinActiveOffset))
        : -1;
}

// 把一台车辆生成器的生成能力对齐到 (count, scale, min_active)：先读后比，不一致才写
int SyncOneVehicle(std::uintptr_t addr, int count, float scale, int min_active) {
    int written = 0;
    const std::int32_t cur_count =
        static_cast<std::int32_t>(SafeReadU32(addr + deserted_profile::native_layout.kSpawnCountOffset));
    if (cur_count != count &&
        WriteU32(addr + deserted_profile::native_layout.kSpawnCountOffset, static_cast<std::uint32_t>(count))) {
        ++written;
    }
    float cur_scale = 0.0F;
    if (SafeReadF32(addr + deserted_profile::native_layout.kSpawnScaleOffset, &cur_scale) && cur_scale != scale &&
        WriteF32(addr + deserted_profile::native_layout.kSpawnScaleOffset, scale)) {
        ++written;
    }
    if (min_active >= 0) {
        const std::int32_t cur_min = static_cast<std::int32_t>(SafeReadU32(addr + deserted_profile::native_layout.kVehMinActiveOffset));
        if (cur_min != min_active &&
            WriteU32(addr + deserted_profile::native_layout.kVehMinActiveOffset, static_cast<std::uint32_t>(min_active))) {
            ++written;
        }
    }
    return written;
}

// 勾选时把车辆生成能力压到 0；取消时用记录的初值恢复
int SyncVehicles(int to_zero) {
    State& s = g_state;
    int written = 0;
    for (int i = 0; i < s.veh_count && i < kMaxSpawners; ++i) {
        std::uintptr_t addr = 0;
        if (!ResolveCurrentObject(s.veh_index[i], s.veh_handle[i], &addr)) continue;
        s.veh[i] = addr;
        if (!IsVehicleSpawnerObject(addr)) continue;
        const int min_active = s.veh_has_min_active[i] != 0 ? 0 : -1;
        if (to_zero != 0) {
            written += SyncOneVehicle(addr, 0, 0.0F, min_active);
        } else if (s.veh_baseline_valid[i] != 0) {
            written += SyncOneVehicle(addr, s.veh_baseline_count[i], s.veh_baseline_scale[i],
                                      s.veh_baseline_min[i]);
        }
    }
    return written;
}

void CaptureVehicleBaseline() {
    State& s = g_state;
    for (int i = 0; i < s.veh_count && i < kMaxSpawners; ++i) {
        if (s.veh_baseline_valid[i] != 0) continue;
        std::uintptr_t addr = 0;
        if (!ResolveCurrentObject(s.veh_index[i], s.veh_handle[i], &addr)) continue;
        s.veh[i] = addr;
        if (!IsVehicleSpawnerObject(addr)) continue;
        int count = 0;
        int min_active = 0;
        float scale = 1.0F;
        const bool has_min_active = s.veh_has_min_active[i] != 0;
        ReadVehicleParams(addr, &count, &scale, &min_active, has_min_active);
        // 已经是 0（上次没恢复）⇒ 不当作初值，用游戏默认值兜底
        if (count <= 0 || scale <= 0.0F) {
            count = kDefaultVehCount;
            scale = kDefaultVehScale;
            min_active = has_min_active ? kDefaultVehMinActive : -1;
        }
        s.veh_baseline_count[i] = count;
        s.veh_baseline_scale[i] = scale;
        s.veh_baseline_min[i] = min_active;
        s.veh_baseline_valid[i] = 1;
    }
}

// --------------------------- 读写倍率 ----------------------------------------
// 读默认档上的 5 个环境倍率（作为初值）
bool ReadScales(std::uintptr_t sp, float out[5]) {
    for (int i = 0; i < 5; ++i) {
        float v = 0.0F;
        if (!SafeReadF32(sp + deserted_profile::native_layout.kDefaultProfileOffset + deserted_profile::native_layout.kScaleOffsets[i], &v)) return false;
        if (!(v >= 0.0F) || v > 1000.0F) return false;
        out[i] = v;
    }
    return true;
}

// 初值合理性：接近 0（0 或本插件的空街值）不能作为初值，否则恢复后仍是空街
bool BaselineSane(const float* v) {
    for (int i = 0; i < 5; ++i) {
        if (v[i] > 0.2F) return true;
    }
    return false;
}

// 把所有生成器（默认档 + 每个 Profile）的 5 个环境倍率写成 values
// 把所有生成器（默认档 + 每个 Profile）的 5 个环境倍率对齐到 values。
// 先读后比：值已经对就不写（游戏不回写时几乎零开销，被回写了就在下一个周期纠正）。
int SyncScale(std::uintptr_t address, const float* values) {
    int written = 0;
    for (int k = 0; k < 5; ++k) {
        float current = 0.0F;
        if (SafeReadF32(address + deserted_profile::native_layout.kScaleOffsets[k], &current) && current == values[k]) continue;
        if (WriteF32(address + deserted_profile::native_layout.kScaleOffsets[k], values[k])) ++written;
    }
    return written;
}

int SyncScales(const float* values) {
    State& s = g_state;
    int written = 0;
    for (int i = 0; i < s.spawner_count; ++i) {
        std::uintptr_t sp = 0;
        if (!ResolveCurrentObject(s.spawner_index[i], s.spawner_handle[i], &sp)) continue;
        s.spawner[i] = sp;
        if (!IsSpawnerObject(sp)) continue;
        written += SyncScale(sp + deserted_profile::native_layout.kDefaultProfileOffset, values);

        std::uintptr_t data = 0;
        std::uint32_t num = 0;
        if (SafeReadPtr(sp + deserted_profile::native_layout.kDensityProfilesOffset, &data) && LooksLikePointer(data)) {
            num = SafeReadU32(sp + deserted_profile::native_layout.kDensityProfilesOffset + deserted_profile::native_layout.kArrayNumOffset);
        }
        if (data == 0 || num == 0 || num > kMaxProfiles) continue;
        for (std::uint32_t p = 0; p < num; ++p) {
            written += SyncScale(data + p * deserted_profile::native_layout.kProfileStride, values);
        }
    }
    return written;
}

// --------------------------- 目标扫描 ----------------------------------------
void RestartDiscovery() {
    State& s = g_state;
    // 保留已发现目标，避免重扫早期批次尚未走到它们时丢失恢复值。
    s.spawner_found = s.spawner_count;
    s.veh_found = s.veh_count;
    for (int i = 0; i < s.spawner_count; ++i) {
        s.spawner_found_index[i] = s.spawner_index[i];
        s.spawner_found_handle[i] = s.spawner_handle[i];
    }
    for (int i = 0; i < s.veh_count; ++i) {
        s.veh_found_index[i] = s.veh_index[i];
        s.veh_found_handle[i] = s.veh_handle[i];
    }
    s.walk_done = 0;
    s.walk_cursor = 0;
    s.walk_total = 0;
    s.retry_accum = 0.0;
    s.reapply_accum = 0.0;
    s.need_apply = (s.checked != 0) ? 1 : 0;  // 换关后重新应用（只在勾选状态下有意义）
    SetStatus("");
}

void ResetScan() {
    g_state.spawner_count = 0;
    g_state.veh_count = 0;
    RestartDiscovery();
    g_state.walk_generation = 0;
    std::memset(g_state.veh_baseline_valid, 0, sizeof(g_state.veh_baseline_valid));
}

bool ContainsHandle(const AnomalyGenerationHandleV1* handles, int count,
                    AnomalyGenerationHandleV1 handle) {
    for (int i = 0; i < count; ++i) {
        if (handles[i].id == handle.id && handles[i].generation == handle.generation) return true;
    }
    return false;
}

bool UpdateWorld() {
    AnomalyNteSessionSnapshotV1 session{};
    session.struct_size = sizeof(session);
    State& s = g_state;
    if (g_session == nullptr || g_session->snapshot == nullptr ||
        g_session->snapshot(g_session->user, &session).code != ANOMALY_STATUS_V1_OK ||
        session.state != ANOMALY_NTE_SESSION_V1_WORLD_READY || session.world.id == 0) {
        if (s.world.id != 0) ResetScan();
        s.world = {};
        return false;
    }
    if (s.world.id != session.world.id || s.world.generation != session.world.generation) {
        ResetScan();
        s.world = session.world;
    }
    return true;
}

// 候选布局判定：签名全失配时用真实对象做证据，只有唯一候选自洽才采用，否则不猜。
bool SelectLayoutFromObjects(const std::uintptr_t* crowd, int crowd_count,
                             const std::uintptr_t* vehicles, int vehicle_count) {
    int winner = -1;
    int matches = 0;
    for (std::size_t i = 0; i < deserted_profile::candidate_count; ++i) {
        const deserted_profile::Candidate& candidate = deserted_profile::candidates[i];
        int score = 0;
        for (int k = 0; k < crowd_count; ++k) {
            // 判定候选时要求默认档仍是游戏原始值，空街值不能反过来当作证据。
            if (ShapeMatchesCrowd(crowd[k], candidate, true)) ++score;
        }
        for (int k = 0; k < vehicle_count; ++k) {
            if (ShapeMatchesVehicle(vehicles[k], candidate,
                                    VehicleSpawnerHasMinActive(vehicles[k]))) ++score;
        }
        if (score == 0) continue;
        ++matches;
        winner = static_cast<int>(i);
    }
    if (matches != 1 || winner < 0) return false;
    return deserted_profile::SelectCandidate(static_cast<std::size_t>(winner));
}

void ResolveFoundObjects() {
    State& s = g_state;
    struct Target {
        std::uint32_t index{};
        AnomalyGenerationHandleV1 handle{};
        std::uintptr_t address{};
        int baseline_valid{};
        int baseline_count{};
        float baseline_scale{};
        int baseline_min{};
    };
    Target crowd_targets[kMaxSpawners]{};
    Target vehicle_targets[kMaxSpawners]{};
    std::uintptr_t crowd[kMaxSpawners]{};
    std::uintptr_t vehicles[kMaxSpawners]{};
    int crowd_found = 0;
    int vehicles_found = 0;
    for (int i = 0; i < s.spawner_found && crowd_found < kMaxSpawners; ++i) {
        const auto index = s.spawner_found_index[i];
        const auto handle = s.spawner_found_handle[i];
        std::uintptr_t addr = 0;
        if (!ResolveCurrentObject(index, handle, &addr)) continue;
        if (!IsCrowdSpawnerCandidate(addr)) continue;
        // 跳过「本身是类」的对象（UClass / BlueprintGeneratedClass）
        if (ClassNameContains(addr, "Class")) continue;
        crowd_targets[crowd_found] = {index, handle, addr};
        crowd[crowd_found++] = addr;
    }
    for (int i = 0; i < s.veh_found && vehicles_found < kMaxSpawners; ++i) {
        const auto index = s.veh_found_index[i];
        const auto handle = s.veh_found_handle[i];
        std::uintptr_t addr = 0;
        if (!ResolveCurrentObject(index, handle, &addr)) continue;
        if (!IsVehicleSpawnerCandidate(addr)) continue;
        if (ClassNameContains(addr, "Class")) continue;
        vehicle_targets[vehicles_found] = {index, handle, addr};
        // 同一世界重扫时按句柄保留原值，不能把插件已写入的零值捕获为初值。
        auto& target = vehicle_targets[vehicles_found];
        for (int j = 0; j < s.veh_count; ++j) {
            if (s.veh_handle[j].id != handle.id || s.veh_handle[j].generation != handle.generation) continue;
            target.baseline_valid = s.veh_baseline_valid[j];
            target.baseline_count = s.veh_baseline_count[j];
            target.baseline_scale = s.veh_baseline_scale[j];
            target.baseline_min = s.veh_baseline_min[j];
            break;
        }
        vehicles[vehicles_found++] = addr;
    }
    if (!deserted_profile::LayoutSelected() &&
        !SelectLayoutFromObjects(crowd, crowd_found, vehicles, vehicles_found)) {
        s.spawner_count = 0;
        s.veh_count = 0;
        s.need_apply = 0;
        SetStatus(g_localizer.Text("status.layout_unverified",
            "The crowd density layout could not be verified for this game build").c_str());
        return;
    }
    int n = 0;
    for (int i = 0; i < crowd_found; ++i) {
        if (!IsSpawnerObject(crowd[i])) continue;
        s.spawner[n++] = crowd[i];
        s.spawner_index[n - 1] = crowd_targets[i].index;
        s.spawner_handle[n - 1] = crowd_targets[i].handle;
    }
    s.spawner_count = n;

    int v = 0;
    for (int i = 0; i < vehicles_found; ++i) {
        if (!IsVehicleSpawnerObject(vehicles[i])) continue;
        s.veh[v] = vehicles[i];
        s.veh_index[v] = vehicle_targets[i].index;
        s.veh_handle[v] = vehicle_targets[i].handle;
        s.veh_baseline_valid[v] = vehicle_targets[i].baseline_valid;
        s.veh_baseline_count[v] = vehicle_targets[i].baseline_count;
        s.veh_baseline_scale[v] = vehicle_targets[i].baseline_scale;
        s.veh_baseline_min[v] = vehicle_targets[i].baseline_min;
        s.veh_has_min_active[v] = VehicleSpawnerHasMinActive(vehicles[i]) ? 1 : 0;
        ++v;
    }
    s.veh_count = v;

    s.need_apply = (s.checked != 0) ? 1 : 0;
    SetStatus("");
}

// 分片遍历对象表，找 HTMassCrowdSpawner 实例
void AdvanceScan(double dt) {
    State& s = g_state;
    if (s.sig_tried == 0) TryResolveGobjectsSlot();
    if (s.gobjects_slot == 0) return;
    if (g_objects == nullptr || g_objects->snapshot_at == nullptr ||
        g_objects->count == nullptr || g_objects->generation == nullptr ||
        g_names == nullptr || g_names->resolve_utf8 == nullptr) {
        s.walk_done = 1;
        SetStatus(g_localizer.Text("status.registry_unavailable", "The object table or name service is unavailable").c_str());
        return;
    }
    const std::uint64_t generation = g_objects->generation(g_objects->user);
    if (s.walk_generation != 0 && generation != s.walk_generation) ResetScan();
    if (s.walk_generation == 0) s.walk_generation = generation;
    if (s.walk_done != 0 && s.spawner_count > 0 && s.veh_count > 0) return;
    if (s.walk_done != 0) {  // 已走完但仍缺目标：按慢周期重试
        s.retry_accum += (dt > 0.0) ? dt : 0.0;
        if (s.retry_accum < kRetrySeconds) return;
        // 新对象可追加或复用已扫过的槽位，且不改变 registry generation。
        RestartDiscovery();
    }
    if (s.walk_total == 0) s.walk_total = g_objects->count(g_objects->user);

    const int previous_crowd_found = s.spawner_found;
    const int previous_vehicle_found = s.veh_found;
    std::uint32_t budget = 4096;
    while (budget-- > 0 && s.walk_cursor < s.walk_total) {
        AnomalyUe5ObjectSnapshotV1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        if (g_objects->snapshot_at(g_objects->user, s.walk_cursor, &snapshot).code ==
            ANOMALY_STATUS_V1_OK) {
            char name[192]{};
            std::size_t size = sizeof(name);
            if (g_names->resolve_utf8(g_names->user, snapshot.name_id, name, &size).code ==
                ANOMALY_STATUS_V1_OK) {
                const bool crowd = IsSpawnerName(name);
                const bool vehicle = !crowd && IsVehicleSpawnerName(name);
                if (crowd || vehicle) {
                    const std::uintptr_t addr = IndexToAddress(s.walk_cursor);
                    // 跳过「本身是类」的对象（UClass / BlueprintGeneratedClass）
                    if (addr != 0 && !ClassNameContains(addr, "Class")) {
                        if (crowd && s.spawner_found < kMaxSpawners &&
                            !ContainsHandle(s.spawner_found_handle, s.spawner_found, snapshot.handle) &&
                            ANOMALY_UE5_OBJECT_HANDLE_INDEX(snapshot.handle) == s.walk_cursor) {
                            s.spawner_found_index[s.spawner_found] = s.walk_cursor;
                            s.spawner_found_handle[s.spawner_found++] = snapshot.handle;
                        } else if (vehicle && s.veh_found < kMaxSpawners &&
                                   !ContainsHandle(s.veh_found_handle, s.veh_found, snapshot.handle) &&
                                   ANOMALY_UE5_OBJECT_HANDLE_INDEX(snapshot.handle) == s.walk_cursor) {
                            s.veh_found_index[s.veh_found] = s.walk_cursor;
                            s.veh_found_handle[s.veh_found++] = snapshot.handle;
                        }
                    }
                }
            }
        }
        ++s.walk_cursor;
    }
    if (s.walk_cursor >= s.walk_total) {
        s.walk_done = 1;
        ResolveFoundObjects();
    } else if (deserted_profile::LayoutSelected() &&
               (s.spawner_found != previous_crowd_found || s.veh_found != previous_vehicle_found)) {
        ResolveFoundObjects();
    }
}

// 每秒校验目标地址（换关/过图会让对象失效）
void ValidateTargets(double dt) {
    State& s = g_state;
    s.validate_accum += dt;
    if (s.validate_accum < 1.0) return;
    s.validate_accum = 0.0;
    if (s.spawner_count == 0 && s.veh_count == 0) return;
    for (int i = 0; i < s.spawner_count; ++i) {
        std::uintptr_t current = 0;
        if (!ResolveCurrentObject(s.spawner_index[i], s.spawner_handle[i], &current) ||
            !IsSpawnerObject(current)) {
            ResetScan();
            return;
        }
        s.spawner[i] = current;
    }
    for (int i = 0; i < s.veh_count; ++i) {
        std::uintptr_t current = 0;
        if (!ResolveCurrentObject(s.veh_index[i], s.veh_handle[i], &current) ||
            !IsVehicleSpawnerObject(current)) {
            ResetScan();
            return;
        }
        s.veh[i] = current;
    }
}

// --------------------------- 开关逻辑 ----------------------------------------
AnomalyStatusV1 SaveConfigImmediate();  // 前向声明（实现见「配置持久化」）

void HandleToggle() {
    State& s = g_state;
    s.need_apply = 1;
    if (s.checked != 0) {
        // 勾选：优先用游戏当前值当初值；当前已是空街值就沿用旧初值；再没有就用默认 1.0 兜底
        float fresh[5]{};
        std::uintptr_t first_spawner = 0;
        if (s.spawner_count > 0 &&
            ResolveCurrentObject(s.spawner_index[0], s.spawner_handle[0], &first_spawner) &&
            IsSpawnerObject(first_spawner) && ReadScales(first_spawner, fresh) && BaselineSane(fresh)) {
            std::memcpy(s.baseline, fresh, sizeof(s.baseline));
            s.baseline_valid = 1;
        } else if (s.baseline_valid == 0) {
            std::memcpy(s.baseline, kDefaultScales, sizeof(s.baseline));
            s.baseline_valid = 1;
        }
        // 车辆同样以「当前值」为初值（每次勾选都重新读）
        std::memset(s.veh_baseline_valid, 0, sizeof(s.veh_baseline_valid));
        if (s.veh_count > 0) CaptureVehicleBaseline();
    }
    SaveConfigImmediate();
    SetStatus("");
}

// --------------------------- 配置持久化 --------------------------------------
// 走宿主 storage 服务：anomaly.storage::write_atomic 在 Lifecycle 域（Stop /
// Unload）直接调用；Game 域的「改了立刻落盘」通过 anomaly.scheduler 推迟到宿主允许的域执行。
// 文件位置由 Host 管理，插件只提供相对路径，不自己打开文件。

std::size_t FormatConfig(char* text, std::size_t capacity) {
    State& s = g_state;
    int written = std::snprintf(text, capacity, "conf_version=%d\nchecked=%d\nbaseline_valid=%d\n",
                                kConfVersion, s.checked, s.baseline_valid);
    if (written <= 0) return 0;
    std::size_t off = static_cast<std::size_t>(written);
    for (int i = 0; i < 5 && off < capacity; ++i) {
        written = std::snprintf(text + off, capacity - off, "baseline%d=%.6f\n", i,
                                static_cast<double>(s.baseline[i]));
        if (written <= 0) break;
        off += static_cast<std::size_t>(written);
    }
    return (off < capacity) ? off : capacity - 1;
}

// 走宿主 storage 服务落盘。仅 Lifecycle 域（Stop / Unload）直接调用。
AnomalyStatusV1 SaveConfig() {
    if (g_storage == nullptr || g_storage->write_atomic == nullptr) {
        return AnomalyStatusV1{static_cast<std::uint32_t>(ANOMALY_STATUS_V1_UNAVAILABLE), 0, {}};
    }
    char text[512]{};
    const std::size_t size = FormatConfig(text, sizeof(text));
    AnomalyByteSpanV1 span{};
    span.data = reinterpret_cast<const std::uint8_t*>(text);
    span.size = size;
    return g_storage->write_atomic(g_storage->user, StringView(kConfFile), span);
}

// 调度器任务：在宿主允许的线程域里真正写盘（配置文本现取现用，保证写的是最新值）。
void ANOMALY_CALL PersistTask(void* user, AnomalyGenerationHandleV1 task) {
    (void)user;
    (void)task;
    SaveConfig();
}

// 立即落盘：任何线程域都能调用。用 scheduler 把写盘推迟到宿主允许的域执行，避免在
// Game/Render 域同步做文件 I/O。调度器不可用时跳过（Stop / Unload 仍会兜底落盘）。
AnomalyStatusV1 SaveConfigImmediate() {
    if (g_scheduler == nullptr || g_scheduler->schedule == nullptr) {
        return AnomalyStatusV1{static_cast<std::uint32_t>(ANOMALY_STATUS_V1_UNAVAILABLE), 0, {}};
    }
    AnomalyGenerationHandleV1 task{};
    return g_scheduler->schedule(g_scheduler->user, 0, PersistTask, nullptr, &task);
}

void LoadConfig() {
    State& s = g_state;
    if (g_storage == nullptr || g_storage->read == nullptr) return;
    char text[512]{};
    AnomalyMutableByteSpanV1 span{};
    span.data = reinterpret_cast<std::uint8_t*>(text);
    span.size = sizeof(text) - 1;
    std::size_t size = span.size;
    const AnomalyStatusV1 status =
        g_storage->read(g_storage->user, StringView(kConfFile), span, &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size == 0) return;
    text[size < sizeof(text) ? size : sizeof(text) - 1] = '\0';

    int conf_version = 0;
    int checked = 0;
    int baseline_valid = 0;
    float baseline[5] = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    char* line = text;
    while (line != nullptr && *line != '\0') {
        char* next = std::strchr(line, '\n');
        if (next != nullptr) *next = '\0';
        char* eq = std::strchr(line, '=');
        if (eq != nullptr) {
            *eq = '\0';
            const char* key = line;
            const char* value = eq + 1;
            if (std::strcmp(key, "conf_version") == 0) {
                conf_version = std::atoi(value);
            } else if (std::strcmp(key, "checked") == 0) {
                checked = (std::atoi(value) != 0) ? 1 : 0;
            } else if (std::strcmp(key, "baseline_valid") == 0) {
                baseline_valid = (std::atoi(value) != 0) ? 1 : 0;
            } else if (std::strncmp(key, "baseline", 8) == 0) {
                const int idx = std::atoi(key + 8);
                if (idx >= 0 && idx < 5) baseline[idx] = static_cast<float>(std::atof(value));
            }
        }
        line = (next != nullptr) ? next + 1 : nullptr;
    }

    if (conf_version != kConfVersion) return;  // 版本不符 ⇒ 全用默认值
    if (baseline_valid != 0 && BaselineSane(baseline)) {
        std::memcpy(s.baseline, baseline, sizeof(s.baseline));
        s.baseline_valid = 1;
    }
    if (checked != 0) {
        s.checked = 1;
        s.need_apply = 1;  // 上次是开启状态 ⇒ 启动后继续置 0
    }
}

// ------------------------------- 绘制 ---------------------------------------
void Text(const AnomalyUiServiceV1* ui, const char* utf8) {
    if (HAS(ui, text)) ui->text(ui->user, StringView(utf8));
}

void DrawMain(const AnomalyUiServiceV1* ui) {
    State& s = g_state;

    const std::string state_text = (s.checked != 0)
        ? g_localizer.Text("status.deserted", "Status: deserted")
        : g_localizer.Text("status.normal", "Status: normal");
    Text(ui, state_text.c_str());
    if (s.world.id == 0) {
        Text(ui, g_localizer.Text("status.waiting_world", "Waiting for world initialization...").c_str());
    } else if (s.status[0] != '\0') {
        Text(ui, s.status);
    } else if (s.spawner_count == 0 && s.veh_count == 0) {
        if (s.walk_done != 0) {
            Text(ui, g_localizer.Text("status.no_spawners",
                "No supported generators found yet; retrying automatically").c_str());
        } else {
            char cursor[16]{};
            char total[16]{};
            std::snprintf(cursor, sizeof(cursor), "%u", s.walk_cursor);
            std::snprintf(total, sizeof(total), "%u", s.walk_total);
            const std::string progress = g_localizer.Format("status.scan_progress",
                "Scanning object table: {0}/{1}",
                std::array<std::string_view, 2>{cursor, total});
            Text(ui, progress.c_str());
        }
    } else {
        char crowd_count[16]{};
        char vehicle_count[16]{};
        std::snprintf(crowd_count, sizeof(crowd_count), "%d", s.spawner_count);
        std::snprintf(vehicle_count, sizeof(vehicle_count), "%d", s.veh_count);
        const std::string targets = g_localizer.Format("status.targets",
            "Located: {0} crowd spawners / {1} vehicles",
            std::array<std::string_view, 2>{crowd_count, vehicle_count});
        Text(ui, targets.c_str());
    }

    if (HAS(ui, separator)) ui->separator(ui->user);

    if (HAS(ui, checkbox)) {
        int checked = s.checked;
        const std::string empty_label =
            g_localizer.Label("option.empty_streets", "Empty streets", "td-empty");
        if (ui->checkbox(ui->user, StringView(empty_label), &checked) != 0) {
            s.checked = (checked != 0) ? 1 : 0;
            s.req_toggle = 1;  // 真正的读写与落盘放 on_update（Game 域）
        }
    }

    if (HAS(ui, separator)) ui->separator(ui->user);

    const std::string rescan_label =
        g_localizer.Label("action.rescan", "Rescan", "td-rescan");
    if (HAS(ui, button) && ui->button(ui->user, StringView(rescan_label), 0.0F, 0.0F)) {
        s.req_rescan = 1;  // 扫描在 Game Update 执行，Render 只投递请求。
    }
}

void ANOMALY_CALL Draw(void* context, const AnomalyUiServiceV1* ui_v1) {
    (void)context;
    const AnomalyUiServiceV1* ui = (ui_v1 != nullptr) ? ui_v1 : g_ui;
    if (!HAS(ui, begin_window) || !HAS(ui, end_window)) return;
    State& s = g_state;

    if (HAS(ui, set_next_window_size_constraints)) {
        ui->set_next_window_size_constraints(ui->user, 320.0F, 150.0F, 100000.0F, 100000.0F);
    }
    if (HAS(ui, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 440.0F, 210.0F, kCondFirstUseEver);
    }
    const std::string title = g_localizer.Text("window.title", "Deserted");
    if (ui->begin_window(ui->user, StringView(title), &s.window_open, 0u) != 0) {
        DrawMain(ui);
    }
    ui->end_window(ui->user);
}

// ------------------------------- 生命周期 ------------------------------------
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    if (!deserted_profile::LoadCompatibility(host)) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    const Host view(host);
    g_localizer = anomaly::sdk::Localizer(host);

    const auto ui = view.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID,
                                                   ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || !HAS(ui.get(), text) || !HAS(ui.get(), begin_window)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_ui = ui.get();
    const auto core = view.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID,
                                                       ANOMALY_CORE_SERVICE_V1_VERSION);
    g_core = core ? core.get() : nullptr;
    const auto signature = view.Query<AnomalySignatureServiceV1>(
        ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
    g_signature = signature ? signature.get() : nullptr;
    const auto names = view.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID,
                                                            ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
    g_names = names ? names.get() : nullptr;
    const auto objects = view.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
    g_objects = objects ? objects.get() : nullptr;
    const auto session = view.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID,
                                                               ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    g_session = session ? session.get() : nullptr;
    const auto storage = view.Query<AnomalyStorageServiceV1>(ANOMALY_STORAGE_SERVICE_V1_ID,
                                                             ANOMALY_STORAGE_SERVICE_V1_VERSION);
    g_storage = storage ? storage.get() : nullptr;
    const auto scheduler = view.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID,
                                                                 ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    g_scheduler = scheduler ? scheduler.get() : nullptr;

    g_state = State{};
    g_state.layout[kLayoutChecked] = static_cast<std::uint32_t>(offsetof(State, checked));
    g_state.layout[kLayoutReqToggle] = static_cast<std::uint32_t>(offsetof(State, req_toggle));
    g_state.layout[kLayoutBaseline] = static_cast<std::uint32_t>(offsetof(State, baseline));
    g_state.layout[kLayoutBaselineValid] =
        static_cast<std::uint32_t>(offsetof(State, baseline_valid));
    g_state.layout[kLayoutSpawnerCount] = static_cast<std::uint32_t>(offsetof(State, spawner_count));
    g_state.layout[kLayoutWrites] = static_cast<std::uint32_t>(offsetof(State, writes));
    g_state.layout[kLayoutFaults] = static_cast<std::uint32_t>(offsetof(State, faults));
    g_state.layout[kLayoutStatus] = static_cast<std::uint32_t>(offsetof(State, status));
    g_state.layout[kLayoutVehCount] = static_cast<std::uint32_t>(offsetof(State, veh_count));
    g_state.layout[kLayoutVehArray] = static_cast<std::uint32_t>(offsetof(State, veh));
    *context = &g_state;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    (void)context;
    g_state.req_toggle = 0;
    LoadConfig();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t deadline_milliseconds) {
    (void)context;
    (void)deadline_milliseconds;
    // 卸载/重载时恢复初值，避免把游戏留在空街上
    if (g_state.checked != 0) {
        if (g_state.baseline_valid != 0 && g_state.spawner_count > 0) SyncScales(g_state.baseline);
        SyncVehicles(0);
    }
    SaveConfig();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* context) {
    (void)context;
    g_signature = nullptr;
    g_names = nullptr;
    g_objects = nullptr;
    g_session = nullptr;
    g_core = nullptr;
    g_storage = nullptr;
    g_scheduler = nullptr;
    g_ui = nullptr;
    g_localizer = {};
    g_state = State{};
}

void ANOMALY_CALL Update(void* context, double delta_seconds) {
    (void)context;
    State& s = g_state;
    const double dt = (delta_seconds > 0.0) ? delta_seconds : 0.0;

    // 保活头部标记（外部工具据此定位 State）
    if (s.magic != kStateMagic || s.struct_version != kStateVersion) {
        s.magic = kStateMagic;
        s.struct_version = kStateVersion;
    }

    const bool world_ready = UpdateWorld();
    if (s.req_rescan != 0 && world_ready) {
        s.req_rescan = 0;
        RestartDiscovery();
    }
    if (world_ready) {
        AdvanceScan(dt);
        ValidateTargets(dt);
    }

    if (s.req_toggle != 0) {
        s.req_toggle = 0;
        HandleToggle();
    }
    if (!world_ready || (s.spawner_count == 0 && s.veh_count == 0)) return;

    if (s.checked != 0) {
        // 勾选：确保有初值，然后把环境倍率压到空街值并保持
        if (s.spawner_count > 0 && s.baseline_valid == 0) {
            float fresh[5]{};
            std::uintptr_t first_spawner = 0;
            if (ResolveCurrentObject(s.spawner_index[0], s.spawner_handle[0], &first_spawner) &&
                IsSpawnerObject(first_spawner) && ReadScales(first_spawner, fresh) && BaselineSane(fresh)) {
                std::memcpy(s.baseline, fresh, sizeof(s.baseline));
            } else {
                std::memcpy(s.baseline, kDefaultScales, sizeof(s.baseline));  // 兜底初值
            }
            s.baseline_valid = 1;
            SaveConfigImmediate();
        }
        if (s.veh_count > 0) CaptureVehicleBaseline();
        if (s.need_apply != 0) {
            s.need_apply = 0;
            if (s.spawner_count > 0) SyncScales(kEmptyScales);
            if (s.veh_count > 0) SyncVehicles(1);
            s.reapply_accum = 0.0;
            return;
        }
        // 天气切换/关卡刷新会让游戏重算密度，按慢周期再对齐一次；值没变时 SyncScales /
        // SyncVehicles 只做读-比，不落写。
        s.reapply_accum += dt;
        if (s.reapply_accum >= kReapplySeconds) {
            s.reapply_accum = 0.0;
            if (s.spawner_count > 0) SyncScales(kEmptyScales);
            if (s.veh_count > 0) SyncVehicles(1);
        }
        return;  // 其余时间不做逐帧检测
    }

    // 未勾选：恢复初值（只在确实置 0 过之后才写）
    if (s.need_apply != 0) {
        s.need_apply = 0;
        if (s.spawner_count > 0 && s.baseline_valid != 0 && BaselineSane(s.baseline)) {
            SyncScales(s.baseline);
        }
        if (s.veh_count > 0) SyncVehicles(0);
    }
}

}  // namespace

// 导出入口：id / name / author / version 必须与 manifest.json 一致
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   StringView("wawmew.deserted"), StringView("Deserted"),
                   StringView("WawMew"), StringView("0.6.8"), Load, Start, Stop, Unload, Update,
                   Draw};
    return anomaly::sdk::Ok();
}
