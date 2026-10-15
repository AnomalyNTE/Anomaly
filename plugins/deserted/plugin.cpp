// Anomaly 插件：空无一人（一键把人群密度环境倍率压到近乎为 0）
//    勾选：读取当前 昼/夜/晴/雨/雪 环境倍率作为初值 → 全部置 0.01
//    取消：恢复初值
// 目标字段：HTMassCrowdSpawner（+0x408 DefaultDensityProfile、+0x428 DensityProfiles）
//          内 HTMassCrowdDensityProfile 的 +0x0C/+0x10/+0x14/+0x18/+0x1C。
// 窗口热键固定 F8；开关热键可改键，默认未绑定。

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/platform.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
constexpr std::uintptr_t kClassPrivateOffset = 0x10;
constexpr std::uintptr_t kNamePrivateOffset = 0x18;

// HTMassCrowdSpawner 上的密度字段
constexpr std::uintptr_t kDefaultProfileOffset = 0x408;
constexpr std::uintptr_t kDensityProfilesOffset = 0x428;  // TArray
constexpr std::uintptr_t kProfileStride = 0x20;
constexpr std::uintptr_t kScaleOffsets[5] = {0x0C, 0x10, 0x14, 0x18, 0x1C};
constexpr std::uint32_t kMaxProfiles = 16;
constexpr int kMaxSpawners = 16;
const float kEmptyScales[5] = {0.01F, 0.01F, 0.01F, 0.01F, 0.01F};  // 空街值（昼/夜/晴/雨/雪）
const float kDefaultScales[5] = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F};     // 游戏默认环境倍率（兜底初值）

// 车辆生成器（HTMassVehicleSpawner）上的字段
//   MassSpawner 基类：Count(+0x2E8, int32) 生成上限、SpawningCountScale(+0x328, f32) 数量倍率
//   HTMassVehicleSpawner 自己：MinActiveVehiclesNum(+0x45C, int32) 保底在线车辆数
constexpr std::uintptr_t kSpawnCountOffset = 0x2E8;
constexpr std::uintptr_t kSpawnScaleOffset = 0x328;
constexpr std::uintptr_t kVehMinActiveOffset = 0x45C;
constexpr int kDefaultVehCount = 3000;
constexpr float kDefaultVehScale = 0.8F;
constexpr int kDefaultVehMinActive = 50;

// 游戏自带「索引 → 对象地址」助手签名（HTGame.exe .text 唯一命中）
constexpr char kIndexToAddressPattern[] =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3";

constexpr int kConfVersion = 5;
constexpr const char* kConfFile = "deserted.conf";
constexpr std::uint32_t kWindowKey = VK_F8;

// State 头部标记，供外部工具在 .data 中定位本结构
constexpr std::uint64_t kStateMagic = 0x0054442D4D4F4E41ULL;  // "ANOM-DT\0"
constexpr std::uint32_t kStateVersion = 2;
enum LayoutIndex {
    kLayoutChecked = 0,   // 与 req_toggle 相邻，外部可一次写入两个
    kLayoutReqToggle,
    kLayoutBaseline,      // float[5]
    kLayoutBaselineValid,
    kLayoutHotkey,
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
    int sig_tried{};
    std::uintptr_t gobjects_slot{};
    std::uint32_t walk_total{};
    std::uint32_t walk_cursor{};
    int walk_done{};
    std::uint32_t spawner_index[kMaxSpawners]{};
    int spawner_found{};

    // 目标
    std::uintptr_t spawner[kMaxSpawners]{};
    int spawner_count{};

    // 车辆生成器（同一套扫描流程，名字含 MassVehicleSpawner）
    std::uint32_t veh_index[kMaxSpawners]{};
    int veh_found{};
    std::uintptr_t veh[kMaxSpawners]{};
    int veh_count{};
    int veh_baseline_valid{};
    int veh_baseline_count[kMaxSpawners]{};
    float veh_baseline_scale[kMaxSpawners]{};
    int veh_baseline_min[kMaxSpawners]{};

    // 开关状态
    int checked{};        // 勾选状态
    int req_toggle{};     // 置 1 让 on_update 处理一次开关（紧邻 checked，便于外部一次写入）
    float baseline[5]{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    int baseline_valid{};
    int need_apply{};
    double validate_accum{};

    // 热键 / 窗口
    std::uint32_t hotkey{0};
    int capturing{};
    int window_open{1};
    unsigned char prev_keys[32]{};

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
const AnomalyInputServiceV1* g_input{};
const AnomalyStorageServiceV1* g_storage{};
const AnomalySchedulerServiceV1* g_scheduler{};
AnomalyGenerationHandleV1 g_hotkey_window{};
AnomalyGenerationHandleV1 g_hotkey_toggle{};

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
        SetStatus("签名服务不可用");
        return;
    }
    std::uintptr_t match = 0;
    if (g_signature->resolve(g_signature->user, StringView("HTGame.exe"), StringView(".text"),
                             StringView(kIndexToAddressPattern), &match).code !=
            ANOMALY_STATUS_V1_OK ||
        match == 0) {
        SetStatus("未命中对象表签名（游戏版本可能变了）");
        return;
    }
    const std::int32_t disp = static_cast<std::int32_t>(SafeReadU32(match + 3));
    const std::uintptr_t slot =
        match + 7 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(disp));
    if (!LooksLikePointer(slot)) {
        SetStatus("对象表符号地址非法");
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
    if (!SafeReadPtr(chunk_array + (index >> 16) * 8u, &chunk_ptr) || chunk_ptr == 0) return 0;
    std::uintptr_t object = 0;
    if (!SafeReadPtr(chunk_ptr + (index & 0xFFFFu) * 0x18u, &object)) return 0;
    return object;
}

// 对象类名里是否含 needle（不区分大小写）
bool ClassNameContains(std::uintptr_t addr, const char* needle) {
    if (!LooksLikePointer(addr) || needle == nullptr) return false;
    if (g_names == nullptr || g_names->resolve_utf8 == nullptr) return false;
    std::uintptr_t cls = 0;
    if (!SafeReadPtr(addr + kClassPrivateOffset, &cls) || cls == 0) return false;
    const std::uint32_t id = SafeReadU32(cls + kNamePrivateOffset);
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

// 结构校验：密度 Profile 数组必须正常，防止索引过期指向别的对象
bool IsSpawnerObject(std::uintptr_t addr) {
    if (!LooksLikePointer(addr)) return false;
    if (!ClassNameContains(addr, "MassCrowdSpawner")) return false;
    std::uintptr_t data = 0;
    if (!SafeReadPtr(addr + kDensityProfilesOffset, &data) || !LooksLikePointer(data)) return false;
    const std::uint32_t num = SafeReadU32(addr + kDensityProfilesOffset + 8);
    return num > 0 && num <= kMaxProfiles;
}

// --------------------------- 车辆生成器 --------------------------------------
bool IsVehicleSpawnerName(const char* name) {
    if (name == nullptr || name[0] == '\0') return false;
    if (std::strncmp(name, "Default__", 9) == 0) return false;
    if (std::strstr(name, "Subsystem") != nullptr) return false;
    return std::strstr(name, "MassVehicleSpawner") != nullptr;
}

bool IsVehicleSpawnerObject(std::uintptr_t addr) {
    if (!LooksLikePointer(addr)) return false;
    if (!ClassNameContains(addr, "VehicleSpawner")) return false;  // 挡掉同名 UFunction
    if (ClassNameContains(addr, "SpawnDataGenerator")) return false;
    const std::int32_t count = static_cast<std::int32_t>(SafeReadU32(addr + kSpawnCountOffset));
    float scale = 0.0F;
    if (!SafeReadF32(addr + kSpawnScaleOffset, &scale)) return false;
    return count >= 0 && count <= 1000000 && scale >= 0.0F && scale <= 100.0F;
}

void ReadVehicleParams(std::uintptr_t addr, int* count, float* scale, int* min_active) {
    *count = static_cast<std::int32_t>(SafeReadU32(addr + kSpawnCountOffset));
    if (!SafeReadF32(addr + kSpawnScaleOffset, scale)) *scale = kDefaultVehScale;
    *min_active = static_cast<std::int32_t>(SafeReadU32(addr + kVehMinActiveOffset));
}

// 把一台车辆生成器的生成能力对齐到 (count, scale, min_active)：先读后比，不一致才写
int SyncOneVehicle(std::uintptr_t addr, int count, float scale, int min_active) {
    int written = 0;
    const std::int32_t cur_count =
        static_cast<std::int32_t>(SafeReadU32(addr + kSpawnCountOffset));
    if (cur_count != count &&
        WriteU32(addr + kSpawnCountOffset, static_cast<std::uint32_t>(count))) {
        ++written;
    }
    float cur_scale = 0.0F;
    if (SafeReadF32(addr + kSpawnScaleOffset, &cur_scale) && cur_scale != scale &&
        WriteF32(addr + kSpawnScaleOffset, scale)) {
        ++written;
    }
    const std::int32_t cur_min = static_cast<std::int32_t>(SafeReadU32(addr + kVehMinActiveOffset));
    if (cur_min != min_active &&
        WriteU32(addr + kVehMinActiveOffset, static_cast<std::uint32_t>(min_active))) {
        ++written;
    }
    return written;
}

// 勾选时把车辆生成能力压到 0；取消时用记录的初值恢复
int SyncVehicles(int to_zero) {
    State& s = g_state;
    int written = 0;
    for (int i = 0; i < s.veh_count && i < kMaxSpawners; ++i) {
        const std::uintptr_t addr = s.veh[i];
        if (!IsVehicleSpawnerObject(addr)) continue;
        if (to_zero != 0) {
            written += SyncOneVehicle(addr, 0, 0.0F, 0);
        } else if (s.veh_baseline_valid != 0) {
            written += SyncOneVehicle(addr, s.veh_baseline_count[i], s.veh_baseline_scale[i],
                                      s.veh_baseline_min[i]);
        }
    }
    return written;
}

void CaptureVehicleBaseline() {
    State& s = g_state;
    s.veh_baseline_valid = 0;
    for (int i = 0; i < s.veh_count && i < kMaxSpawners; ++i) {
        if (!IsVehicleSpawnerObject(s.veh[i])) continue;
        int count = 0;
        int min_active = 0;
        float scale = 1.0F;
        ReadVehicleParams(s.veh[i], &count, &scale, &min_active);
        // 已经是 0（上次没恢复）⇒ 不当作初值，用游戏默认值兜底
        if (count <= 0 || scale <= 0.0F) {
            count = kDefaultVehCount;
            scale = kDefaultVehScale;
            min_active = kDefaultVehMinActive;
        }
        s.veh_baseline_count[i] = count;
        s.veh_baseline_scale[i] = scale;
        s.veh_baseline_min[i] = min_active;
        s.veh_baseline_valid = 1;
    }
}

// --------------------------- 读写倍率 ----------------------------------------
// 读默认档上的 5 个环境倍率（作为初值）
bool ReadScales(std::uintptr_t sp, float out[5]) {
    for (int i = 0; i < 5; ++i) {
        float v = 0.0F;
        if (!SafeReadF32(sp + kDefaultProfileOffset + kScaleOffsets[i], &v)) return false;
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
        if (SafeReadF32(address + kScaleOffsets[k], &current) && current == values[k]) continue;
        if (WriteF32(address + kScaleOffsets[k], values[k])) ++written;
    }
    return written;
}

int SyncScales(const float* values) {
    State& s = g_state;
    int written = 0;
    for (int i = 0; i < s.spawner_count; ++i) {
        const std::uintptr_t sp = s.spawner[i];
        if (!IsSpawnerObject(sp)) continue;
        written += SyncScale(sp + kDefaultProfileOffset, values);

        std::uintptr_t data = 0;
        std::uint32_t num = 0;
        if (SafeReadPtr(sp + kDensityProfilesOffset, &data) && LooksLikePointer(data)) {
            num = SafeReadU32(sp + kDensityProfilesOffset + 8);
        }
        if (data == 0 || num == 0 || num > kMaxProfiles) continue;
        for (std::uint32_t p = 0; p < num; ++p) {
            written += SyncScale(data + p * kProfileStride, values);
        }
    }
    return written;
}

// --------------------------- 目标扫描 ----------------------------------------
void ResetScan() {
    State& s = g_state;
    s.spawner_count = 0;
    s.spawner_found = 0;
    s.veh_count = 0;
    s.veh_found = 0;
    s.walk_done = 0;
    s.walk_cursor = 0;
    s.walk_total = 0;
    s.need_apply = (s.checked != 0) ? 1 : 0;  // 换关后重新应用（只在勾选状态下有意义）
}

void ResolveFoundObjects() {
    State& s = g_state;
    int n = 0;
    for (int i = 0; i < s.spawner_found && n < kMaxSpawners; ++i) {
        const std::uintptr_t addr = IndexToAddress(s.spawner_index[i]);
        if (!IsSpawnerObject(addr)) continue;
        s.spawner[n++] = addr;
    }
    s.spawner_count = n;

    int v = 0;
    for (int i = 0; i < s.veh_found && v < kMaxSpawners; ++i) {
        const std::uintptr_t addr = IndexToAddress(s.veh_index[i]);
        if (!IsVehicleSpawnerObject(addr)) continue;
        s.veh[v++] = addr;
    }
    s.veh_count = v;

    s.need_apply = (s.checked != 0) ? 1 : 0;
    SetStatus("");
}

// 分片遍历对象表，找 HTMassCrowdSpawner 实例
void AdvanceScan() {
    State& s = g_state;
    if (s.walk_done != 0 && s.spawner_count > 0 && s.veh_count > 0) return;
    if (s.sig_tried == 0) TryResolveGobjectsSlot();
    if (s.gobjects_slot == 0) return;
    if (s.walk_done != 0) {  // 已走完：补齐缺失的一类
        ResolveFoundObjects();
        return;
    }
    if (g_objects == nullptr || g_objects->snapshot_at == nullptr ||
        g_objects->count == nullptr || g_names == nullptr || g_names->resolve_utf8 == nullptr) {
        s.walk_done = 1;
        SetStatus("对象表/名字服务不可用");
        return;
    }
    if (s.walk_total == 0) s.walk_total = g_objects->count(g_objects->user);

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
                        if (crowd && s.spawner_found < kMaxSpawners) {
                            s.spawner_index[s.spawner_found++] = s.walk_cursor;
                        } else if (vehicle && s.veh_found < kMaxSpawners) {
                            s.veh_index[s.veh_found++] = s.walk_cursor;
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
        if (!IsSpawnerObject(s.spawner[i])) {
            ResetScan();
            return;
        }
    }
    for (int i = 0; i < s.veh_count; ++i) {
        if (!IsVehicleSpawnerObject(s.veh[i])) {
            ResetScan();
            return;
        }
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
        if (s.spawner_count > 0 && ReadScales(s.spawner[0], fresh) && BaselineSane(fresh)) {
            std::memcpy(s.baseline, fresh, sizeof(s.baseline));
            s.baseline_valid = 1;
        } else if (s.baseline_valid == 0) {
            std::memcpy(s.baseline, kDefaultScales, sizeof(s.baseline));
            s.baseline_valid = 1;
        }
        // 车辆同样以「当前值」为初值（每次勾选都重新读）
        s.veh_baseline_valid = 0;
        if (s.veh_count > 0) CaptureVehicleBaseline();
    }
    SaveConfigImmediate();
    SetStatus("");
}

// --------------------------- 热键 -------------------------------------------
void RegisterHotkeys();
void ReleaseHotkeys();

void ANOMALY_CALL OnHotkey(void* user, AnomalyGenerationHandleV1 hotkey,
                           const AnomalyInputSnapshotV1* snapshot) {
    (void)user;
    (void)snapshot;
    State& s = g_state;
    if (hotkey.id == g_hotkey_window.id) {
        s.window_open = (s.window_open != 0) ? 0 : 1;
    } else if (hotkey.id == g_hotkey_toggle.id) {
        s.req_toggle = 1;
    }
}

void RegisterHotkeys() {
    if (g_input == nullptr || g_input->register_hotkey == nullptr) return;
    AnomalyHotkeySpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.flags = ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED;  // UI 抢键盘时也生效

    if (g_state.hotkey != 0) {
        spec.modifiers = 0u;
        spec.virtual_key = g_state.hotkey;
        spec.id = StringView("deserted.toggle");
        g_input->register_hotkey(g_input->user, &spec, &OnHotkey, nullptr, &g_hotkey_toggle);
    }
    spec.modifiers = 0u;
    spec.virtual_key = kWindowKey;
    spec.id = StringView("deserted.window");
    g_input->register_hotkey(g_input->user, &spec, &OnHotkey, nullptr, &g_hotkey_window);
}

void ReleaseHotkeys() {
    if (g_input != nullptr && g_input->release_hotkey != nullptr) {
        if (g_hotkey_toggle.id != 0) g_input->release_hotkey(g_input->user, g_hotkey_toggle);
        if (g_hotkey_window.id != 0) g_input->release_hotkey(g_input->user, g_hotkey_window);
    }
    g_hotkey_toggle = AnomalyGenerationHandleV1{};
    g_hotkey_window = AnomalyGenerationHandleV1{};
}

void BeginCapture() {
    g_state.capturing = 1;
    ReleaseHotkeys();
}

void EndCapture() {
    g_state.capturing = 0;
    RegisterHotkeys();
}

void CaptureTick() {
    State& s = g_state;
    if (g_input == nullptr || g_input->snapshot == nullptr) return;
    AnomalyInputSnapshotV1 snap{};
    snap.struct_size = sizeof(snap);
    if (g_input->snapshot(g_input->user, &snap).code != ANOMALY_STATUS_V1_OK) return;

    if (s.capturing == 0) {
        std::memcpy(s.prev_keys, snap.keys, sizeof(s.prev_keys));
        return;
    }
    for (int i = 0; i < 32; ++i) {
        const unsigned int freshly = static_cast<unsigned int>(snap.keys[i] & ~s.prev_keys[i]);
        if (freshly == 0) continue;
        std::uint32_t bit = 0;
        while (bit < 8 && (freshly & (1u << bit)) == 0) ++bit;
        if (bit >= 8) continue;
        const std::uint32_t vk = static_cast<std::uint32_t>(i) * 8u + bit;
        if (vk == VK_ESCAPE) {
            EndCapture();
            return;
        }
        s.hotkey = (vk == VK_BACK) ? 0u : vk;  // 退格 = 解除绑定
        s.capturing = 0;
        RegisterHotkeys();
        SaveConfigImmediate();
        return;
    }
    std::memcpy(s.prev_keys, snap.keys, sizeof(s.prev_keys));
}

void KeyName(std::uint32_t vk, char* out, std::size_t capacity) {
    if (vk == 0) {
        std::snprintf(out, capacity, "未绑定");
    } else if (vk >= '0' && vk <= '9') {
        std::snprintf(out, capacity, "%c", static_cast<char>(vk));
    } else if (vk >= 'A' && vk <= 'Z') {
        std::snprintf(out, capacity, "%c", static_cast<char>(vk));
    } else if (vk >= VK_F1 && vk <= VK_F12) {
        std::snprintf(out, capacity, "F%u", vk - VK_F1 + 1u);
    } else {
        switch (vk) {
            case VK_SPACE: std::snprintf(out, capacity, "空格"); break;
            case VK_TAB: std::snprintf(out, capacity, "Tab"); break;
            case VK_RETURN: std::snprintf(out, capacity, "回车"); break;
            case VK_DELETE: std::snprintf(out, capacity, "Delete"); break;
            case VK_INSERT: std::snprintf(out, capacity, "Insert"); break;
            default: std::snprintf(out, capacity, "VK 0x%02X", vk); break;
        }
    }
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
    if (off < capacity) {
        written = std::snprintf(text + off, capacity - off, "hotkey=%u\n", s.hotkey);
        if (written > 0) off += static_cast<std::size_t>(written);
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
    std::uint32_t hotkey = 0;
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
            } else if (std::strcmp(key, "hotkey") == 0) {
                hotkey = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            }
        }
        line = (next != nullptr) ? next + 1 : nullptr;
    }

    if (conf_version != kConfVersion) return;  // 版本不符 ⇒ 全用默认值
    s.hotkey = (hotkey <= 0xFFu) ? hotkey : 0u;
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
    char line[160];
    char key_name[64];

    Text(ui, (s.checked != 0) ? "当前状态：空无一人" : "当前状态：正常");
    if (s.spawner_count == 0) {
        Text(ui, "正在定位人群生成器…");
    } else if (s.status[0] != '\0') {
        Text(ui, s.status);
    }

    if (HAS(ui, separator)) ui->separator(ui->user);

    if (HAS(ui, checkbox)) {
        int checked = s.checked;
        if (ui->checkbox(ui->user, StringView("空无一人##td-empty"), &checked) != 0) {
            s.checked = (checked != 0) ? 1 : 0;
            s.req_toggle = 1;  // 真正的读写与落盘放 on_update（Game 域）
        }
    }

    if (HAS(ui, separator)) ui->separator(ui->user);

    KeyName(s.hotkey, key_name, sizeof(key_name));
    std::snprintf(line, sizeof(line), "快捷键：%s", key_name);
    Text(ui, line);
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("改键##td-rb"), 64.0F, 0.0F)) {
        BeginCapture();
    }
    if (s.capturing != 0) {
        Text(ui, "请按下新快捷键…（Backspace 解除绑定 / Esc 取消）");
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
    if (ui->begin_window(ui->user, StringView("空无一人"), &s.window_open, 0u) != 0) {
        DrawMain(ui);
    }
    ui->end_window(ui->user);
}

// ------------------------------- 生命周期 ------------------------------------
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const Host view(host);

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
    const auto input = view.Query<AnomalyInputServiceV1>(ANOMALY_INPUT_SERVICE_V1_ID,
                                                         ANOMALY_INPUT_SERVICE_V1_VERSION);
    g_input = input ? input.get() : nullptr;
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
    g_state.layout[kLayoutHotkey] = static_cast<std::uint32_t>(offsetof(State, hotkey));
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
    g_state.capturing = 0;
    g_state.req_toggle = 0;
    LoadConfig();
    RegisterHotkeys();
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
    ReleaseHotkeys();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* context) {
    (void)context;
    ReleaseHotkeys();
    g_signature = nullptr;
    g_names = nullptr;
    g_objects = nullptr;
    g_input = nullptr;
    g_core = nullptr;
    g_storage = nullptr;
    g_scheduler = nullptr;
    g_ui = nullptr;
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

    AdvanceScan();
    ValidateTargets(dt);
    CaptureTick();

    if (s.req_toggle != 0) {
        s.req_toggle = 0;
        HandleToggle();
    }
    if (s.spawner_count == 0 && s.veh_count == 0) return;

    if (s.checked != 0) {
        // 勾选：确保有初值，然后把环境倍率压到空街值并保持
        if (s.spawner_count > 0 && s.baseline_valid == 0) {
            float fresh[5]{};
            if (ReadScales(s.spawner[0], fresh) && BaselineSane(fresh)) {
                std::memcpy(s.baseline, fresh, sizeof(s.baseline));
            } else {
                std::memcpy(s.baseline, kDefaultScales, sizeof(s.baseline));  // 兜底初值
            }
            s.baseline_valid = 1;
            SaveConfigImmediate();
        }
        if (s.veh_count > 0 && s.veh_baseline_valid == 0) CaptureVehicleBaseline();
        if (s.need_apply != 0) {
            s.need_apply = 0;
            if (s.spawner_count > 0) SyncScales(kEmptyScales);
            if (s.veh_count > 0) SyncVehicles(1);
            return;
        }
        return;  // 只写这一次，之后不再周期性检测/回写
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
                   StringView("anomaly.builtin.nte.deserted"), StringView("Deserted"),
                   StringView("WawMew"), StringView("0.6.3"), Load, Start, Stop, Unload, Update,
                   Draw};
    return anomaly::sdk::Ok();
}
