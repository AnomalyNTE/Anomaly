// Anomaly 插件：NTE 时间加速（本地世界时间膨胀）
// =============================================================================
// 功能：把本地世界时间倍率设为 N 倍 —— 动画、物理、本地移动、演出整体变快，玩家角色同样变快。
//      到点 / 关窗 / 「立即恢复」/ Stop / Unload 都会恢复 1 倍。
//
// 写入目标：HTWorldSettings + 0x5E0（非反射的 C++ 私有 float 成员，夹在
//          VisualNPCMgr@+0x5D8 与 PlatformPreloadAssetsCache@+0x5E8 之间）。
//          游戏每帧把它同步给引擎标准的 AWorldSettings::TimeDilation(+0x400)；
//          直接写 +0x400 会被游戏覆盖，所以以 +0x5E0 为准。
//          钳位：MinGlobalTimeDilation(+0x40C)=0.0001、MaxGlobalTimeDilation(+0x410)=20。
//
// 寻址链（全反射偏移）：
//          PlayerController + 0x20  (AActor::OuterPrivate → ULevel)
//            → ULevel + 0x2B0        (ULevel::WorldSettings) → HTWorldSettings
//          PlayerController 由 process-event 回调给出（官方 ABI 只在这条路上暴露裸地址）。
//
// 备选函数（本插件不使用，仅记录）：
//          HTWorldSettings::SetWorldTimeDilation  实现 0x14883E520（r8b=bNextTick，先写 +0x5E0）
//          AWorldSettings::SetTimeDilation        实现 0x144ABDDE0（钳位后写 +0x400）
//          HTCharacter::SetCustomTimeDilation     槽 +0xAD0，内部生效值取 min(1.0, arr[4..8])
//                                                 ⇒ 上界恒为 1.0，只能减速，不能用于加速
//
// 安全设计：
//  * 写只在 on_update（Game 域）做；热键回调与 on_draw 只置请求标志。
//  * 写用 anomaly.core::write_memory（单次 4 字节 float，需 memory-write capability），写后读回确认。
//  * one-shot 到点自动恢复 1 倍；持续模式每 0.5 秒保活一次。
//  * 倍率钳在 [0.05, 20]（与游戏自身上下限一致）。
//  * 内存读写全部用 SEH 兜住，异常计入 faults。
//  * 闸门用官方 anomaly.nte.player 的 snapshot().flags（角色是否在世界中）。
//  * 配置改动立即落盘，见 SaveConfigImmediate() 的说明。
//
// 用法：三个功能键默认未绑定，在界面按需改键（捕获时 Backspace 解除绑定、Esc 取消）；
//      窗口恒开（对齐内建插件），无需回窗热键。

#include "anomaly/sdk/cpp.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {

using anomaly::sdk::Host;
using anomaly::sdk::StringView;

// 访问 UI 字段前先用 struct_size 确认宿主确实实现到了该字段（ABI 尾部扩展兼容）。
#define HAS(ui, field)                                                        \
    ((ui) != nullptr &&                                                       \
     (ui)->struct_size >=                                                     \
         offsetof(AnomalyUiServiceV1, field) + sizeof((ui)->field) &&         \
     (ui)->field != nullptr)

constexpr std::uint32_t kCondFirstUseEver = 4u;

// 玩家控制器类名：用它当"锚"。控制器几乎每帧都有事件，角色则不一定。
constexpr char kControllerClassName[] = "BP_PlayerControllerBase_C";

// UObject 布局
constexpr std::uintptr_t kClassPrivateOffset = 0x10;
constexpr std::uintptr_t kNamePrivateOffset = 0x18;
constexpr std::uintptr_t kOuterPrivateOffset = 0x20;

// 指针链（均为反射属性，不是魔数）
constexpr std::uintptr_t kLevelWorldSettingsOffset = 0x2B0;  // ULevel::WorldSettings

// HTWorldSettings 自己的世界倍率（非反射私有成员）
constexpr std::uintptr_t kWorldDilationOffset = 0x5E0;
// 引擎标准字段（仅供显示/自检，写入走 +0x5E0）
constexpr std::uintptr_t kEngineTimeDilationOffset = 0x400;
constexpr std::uintptr_t kMinDilationOffset = 0x40C;
constexpr std::uintptr_t kMaxDilationOffset = 0x410;

constexpr double kMinMultiplier = 0.05;
constexpr double kMaxMultiplier = 20.0;

constexpr double kKeepAlivePeriod = 0.5;  // 持续模式每 0.5 秒保活写一次

constexpr int kConfVersion = 1;
constexpr const char* kConfFile = "time-accel.conf";

struct State {
    // ---- 指针链解析结果 ----
    std::uintptr_t controller{};
    std::uintptr_t level{};
    std::uintptr_t ws{};
    std::uint32_t controller_class_name_id{};

    // ---- 配置 ----
    double multiplier{3.0};
    double duration{10.0};
    int req_action{};  // 0=无，1=加速一次，2=持续开关，3=立即恢复
    int capture_target{};
    std::uint32_t key_once{0};    // 0 = 未绑定（默认不占用任何快捷键，用户按需在界面改键）
    std::uint32_t key_toggle{0};
    std::uint32_t key_restore{0};
    std::uint32_t mod_once{};
    std::uint32_t mod_toggle{};
    unsigned char prev_keys[32]{};

    // ---- 运行态 ----
    int active{};
    int infinite{};
    double remaining{};
    double keep_accum{};
    float observed{1.0F};    // 当前 WorldSettings+0x5E0
    float engine_value{1.0F}; // 当前 WorldSettings+0x400（应跟随 +0x5E0）
    float max_dilation{20.0F};
    int last_result{-1};      // -1 未试过，1 成功，0 失败
    std::uint64_t applies{};
    std::uint64_t restores{};
    std::uint64_t faults{};

    // 官方 nte.player 的「是否在世界中」判据
    int player_valid{-1};
    double player_pos[3]{};
    double gate_accum{};

    // 类名 id 解析状态
    int find_utf8_tried{};
    int walk_done{};
    std::uint32_t walk_total{};
    std::uint32_t walk_cursor{};

    std::uint64_t events{};
    char status[256]{};

    // 追加字段放在末尾，保持前面所有字段的偏移不变。
    int req_save{};  // UI 里改了倍率/时长 ⇒ 置 1，由 on_update 落盘
    int last_save_code{-1};  // 最近一次「立即落盘」的返回码：0=成功，非 0=失败（诊断用）
    std::uint32_t mod_restore{};  // 「立即恢复」热键的修饰键（key_restore 本体在 +0x40）

    // 倍率 / 时长输入框的文本缓冲，由插件自己维护和解析（见 DecimalField）。
    char rate_text[32]{"1.0"};
    char dur_text[32]{"10.0"};
    int rate_refresh{1};  // 1 = 下一帧用 multiplier 重写 rate_text
    int dur_refresh{1};
};
State g_state;

// 服务表绑定 Host 的生命周期 generation，Stop/Unload 必须退订。
const AnomalyUiServiceV1* g_ui{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyUe5NamesServiceV1* g_names{};
const AnomalyUe5ObjectsServiceV1* g_objects{};
const AnomalyUe5ProcessEventServiceV1* g_process_event{};
const AnomalyInputServiceV1* g_input{};
const AnomalyStorageServiceV1* g_storage{};
const AnomalySchedulerServiceV1* g_scheduler{};
const AnomalyNtePlayerServiceV1* g_player{};

AnomalyGenerationHandleV1 g_events_handle{};
AnomalyGenerationHandleV1 g_hotkey_once{};
AnomalyGenerationHandleV1 g_hotkey_toggle{};
AnomalyGenerationHandleV1 g_hotkey_restore{};

// 只在「出错/不可用」时写提示，界面不显示进度类信息。
void SetError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vsnprintf(g_state.status, sizeof(g_state.status), format, args);
    va_end(args);
}

// --------------------------- 极简安全内存读写 --------------------------------
// 回调每帧进几千至上万次，这里只做内存读，不调服务，直接用 SEH 兜住。
// 注意：含 __try 的函数里不能有需要析构的局部对象（C2712），所以只用 POD。
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

// 只读一个方向性检查：地址看起来像本进程的用户态指针。
bool LooksLikePointer(std::uintptr_t v) {
    return v > 0x10000ULL && v < 0x800000000000ULL;
}

// --------------------------- 写世界倍率 --------------------------------------
// 优先用官方 anomaly.core::write_memory（受 memory-write capability 约束）；
// 服务不可用时退回直接写（同样用 SEH 兜住）。
bool WriteWorldDilation(float value) {
    State& s = g_state;
    if (!LooksLikePointer(s.ws)) return false;

    const std::uintptr_t address = s.ws + kWorldDilationOffset;
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
    if (!ok) {
        ++s.faults;
        return false;
    }

    // 读回确认。
    float back = 0.0F;
    if (SafeReadF32(address, &back) && back == value) {
        ++s.applies;
        return true;
    }
    ++s.faults;
    return false;
}

bool RestoreDilation() {
    State& s = g_state;
    const bool ok = WriteWorldDilation(1.0F);
    if (ok) {
        ++s.restores;
        s.observed = 1.0F;
    }
    return ok;
}

void StopAccel(const char* reason) {
    State& s = g_state;
    const bool was_active = (s.active != 0) || (s.infinite != 0);
    s.active = 0;
    s.infinite = 0;
    s.remaining = 0.0;
    if (was_active) {
        if (!RestoreDilation() && reason != nullptr) {
            SetError("恢复失败（%s）：WorldSettings 地址可能已失效，点「立即恢复 1 倍」重试或重载插件",
                     reason);
            return;
        }
    }
    if (reason != nullptr) SetError("%s", reason);
}

// --------------------------- ProcessEvent 捕获控制器（锚）--------------------
bool ClassNameMatches(std::uintptr_t object, std::uint32_t name_id) {
    if (object == 0 || name_id == 0) return false;
    std::uintptr_t cls = 0;
    if (!SafeReadPtr(object + kClassPrivateOffset, &cls) || cls == 0) return false;
    return SafeReadU32(cls + kNamePrivateOffset) == name_id;
}

void ANOMALY_CALL OnProcessEvent(void* user, std::uintptr_t object, std::uintptr_t function,
                                 void* parameters) {
    (void)user;
    (void)function;
    (void)parameters;

    State& s = g_state;
    ++s.events;
    if (object == 0 || s.controller_class_name_id == 0) return;
    // 回调运行在 Game 线程、游戏原函数之前：只做两次读 + 一次比较。
    if (ClassNameMatches(object, s.controller_class_name_id)) s.controller = object;
}

// 每帧沿指针链取 WorldSettings：换关卡/切场景会自动跟上。
void ResolveWorldSettings() {
    State& s = g_state;
    if (!LooksLikePointer(s.controller)) return;

    std::uintptr_t level = 0;
    if (!SafeReadPtr(s.controller + kOuterPrivateOffset, &level) || !LooksLikePointer(level)) {
        s.level = 0;
        s.ws = 0;
        return;
    }
    s.level = level;

    std::uintptr_t ws = 0;
    if (!SafeReadPtr(level + kLevelWorldSettingsOffset, &ws) || !LooksLikePointer(ws)) {
        s.ws = 0;
        return;
    }
    s.ws = ws;
}

// 官方 nte.player 的「是否在世界中」判据，顺便记录位置。0.5 秒一次。
void PollPlayerGate(double dt) {
    State& s = g_state;
    if (g_player == nullptr || g_player->snapshot == nullptr) {
        s.player_valid = -1;
        return;
    }
    s.gate_accum += dt;
    if (s.player_valid >= 0 && s.gate_accum < 0.5) return;
    s.gate_accum = 0.0;

    AnomalyNtePlayerSnapshotV1 snap{};
    snap.struct_size = sizeof(snap);
    if (g_player->snapshot(g_player->user, &snap).code != ANOMALY_STATUS_V1_OK) {
        s.player_valid = 0;
        return;
    }
    const bool ok = (snap.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
                    (snap.flags & ANOMALY_NTE_SNAPSHOT_V1_STALE) == 0;
    s.player_valid = ok ? 1 : 0;
    if (ok) {
        s.player_pos[0] = snap.position[0];
        s.player_pos[1] = snap.position[1];
        s.player_pos[2] = snap.position[2];
    }
}

// 服务可用时只有在世界中才动手；服务不可用时一律放行（不因服务缺失锁死功能）。
bool PlayerGateOpen() {
    return g_state.player_valid != 0;
}

// 开发者模式门禁：本插件会写游戏内存，只在开发者模式开启时放行
// （对齐 BoxAuto / CameraTools 对 developer_mode_enabled 的用法）。
// 宿主未暴露该字段时视为未开启，宁可不加速也不越界。
bool DeveloperModeEnabled() {
    if (g_ui == nullptr || !HAS(g_ui, developer_mode_enabled)) return false;
    return g_ui->developer_mode_enabled(g_ui->user) != 0;
}

// --------------------------- 类名 id 解析 ------------------------------------
// find_utf8 是有界搜索，BP 类名的 id 可能在名字池很深处 ⇒ 兜底遍历对象表。
// 名字池 id 每局都会变，所以「找到就用」，不能硬编码。
void ResolveControllerClassNameId() {
    State& s = g_state;
    if (s.controller_class_name_id != 0) return;

    if (s.find_utf8_tried == 0) {
        s.find_utf8_tried = 1;
        if (g_names != nullptr && g_names->find_utf8 != nullptr) {
            std::uint32_t id = 0;
            if (g_names->find_utf8(g_names->user, StringView(kControllerClassName), &id).code ==
                ANOMALY_STATUS_V1_OK) {
                s.controller_class_name_id = id;
                return;
            }
        }
    }

    if (s.walk_done != 0) return;
    if (g_objects == nullptr || g_objects->snapshot_at == nullptr || g_objects->count == nullptr ||
        g_names == nullptr || g_names->resolve_utf8 == nullptr) {
        s.walk_done = 1;
        SetError("对象表/名字服务不可用，无法解析控制器类名");
        return;
    }
    if (s.walk_total == 0) s.walk_total = g_objects->count(g_objects->user);

    std::uint32_t budget = 2048;  // 分片：每 tick 最多处理这么多个对象
    while (budget-- > 0 && s.walk_cursor < s.walk_total) {
        AnomalyUe5ObjectSnapshotV1 snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        if (g_objects->snapshot_at(g_objects->user, s.walk_cursor, &snapshot).code ==
            ANOMALY_STATUS_V1_OK) {
            char name[128]{};
            std::size_t size = sizeof(name);
            if (g_names->resolve_utf8(g_names->user, snapshot.name_id, name, &size).code ==
                    ANOMALY_STATUS_V1_OK &&
                std::strcmp(name, kControllerClassName) == 0) {
                s.controller_class_name_id = snapshot.name_id;
                s.walk_done = 1;
                return;
            }
        }
        ++s.walk_cursor;
    }
    if (s.walk_cursor >= s.walk_total) {
        s.walk_done = 1;
        SetError("未找到控制器类名 %s（游戏版本可能变了）", kControllerClassName);
    }
}

// --------------------------- 订阅与热键 --------------------------------------
void SubscribeEvents() {
    if (g_process_event == nullptr || g_process_event->subscribe == nullptr) return;
    if (g_events_handle.id != 0) return;
    g_process_event->subscribe(g_process_event->user, &OnProcessEvent, nullptr, &g_events_handle);
}

void UnsubscribeEvents() {
    if (g_process_event != nullptr && g_process_event->unsubscribe != nullptr &&
        g_events_handle.id != 0) {
        g_process_event->unsubscribe(g_process_event->user, g_events_handle);
    }
    g_events_handle = AnomalyGenerationHandleV1{};
}

// 热键回调不一定在游戏线程：只置请求标志，真正的写入交给 on_update。
void ANOMALY_CALL OnHotkey(void* user, AnomalyGenerationHandleV1 hotkey,
                           const AnomalyInputSnapshotV1* snapshot) {
    (void)user;
    (void)snapshot;
    State& s = g_state;
    if (hotkey.id == g_hotkey_once.id) {
        s.req_action = 1;
    } else if (hotkey.id == g_hotkey_toggle.id) {
        s.req_action = 2;
    } else if (hotkey.id == g_hotkey_restore.id) {
        s.req_action = 3;
    }
}

void RegisterHotkeys() {
    if (g_input == nullptr || g_input->register_hotkey == nullptr) return;
    AnomalyHotkeySpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.flags = 0u;

    // vk == 0 表示「未绑定」—— 不注册，绝不占用按键（尤其别让 VK 0 被当成真按键）。
    if (g_state.key_once != 0) {
        spec.modifiers = g_state.mod_once;
        spec.virtual_key = g_state.key_once;
        spec.id = StringView("timeaccel.once");
        g_input->register_hotkey(g_input->user, &spec, &OnHotkey, nullptr, &g_hotkey_once);
    }

    if (g_state.key_toggle != 0) {
        spec.modifiers = g_state.mod_toggle;
        spec.virtual_key = g_state.key_toggle;
        spec.id = StringView("timeaccel.toggle");
        g_input->register_hotkey(g_input->user, &spec, &OnHotkey, nullptr, &g_hotkey_toggle);
    }

    if (g_state.key_restore != 0) {
        spec.modifiers = g_state.mod_restore;
        spec.virtual_key = g_state.key_restore;
        spec.id = StringView("timeaccel.restore");
        g_input->register_hotkey(g_input->user, &spec, &OnHotkey, nullptr, &g_hotkey_restore);
    }
}

void ReleaseHotkeys() {
    if (g_input != nullptr && g_input->release_hotkey != nullptr) {
        if (g_hotkey_once.id != 0) g_input->release_hotkey(g_input->user, g_hotkey_once);
        if (g_hotkey_toggle.id != 0) g_input->release_hotkey(g_input->user, g_hotkey_toggle);
        if (g_hotkey_restore.id != 0) g_input->release_hotkey(g_input->user, g_hotkey_restore);
    }
    g_hotkey_once = AnomalyGenerationHandleV1{};
    g_hotkey_toggle = AnomalyGenerationHandleV1{};
    g_hotkey_restore = AnomalyGenerationHandleV1{};
}

// --------------------------- 改键 -------------------------------------------
// 前向声明：改键成功后要立刻落盘（见下），而 SaveConfig* 定义在配置持久化一节。
AnomalyStatusV1 SaveConfig();
AnomalyStatusV1 SaveConfigImmediate();

void BeginCapture(int target) {
    g_state.capture_target = target;
    ReleaseHotkeys();
}

void CancelCapture() {
    g_state.capture_target = 0;
    RegisterHotkeys();
}

std::uint32_t FirstBitIndex(unsigned char bits) {
    for (std::uint32_t i = 0; i < 8; ++i) {
        if ((bits & (1u << i)) != 0) return i;
    }
    return 0xFFFFFFFFu;
}

void CaptureTick() {
    State& s = g_state;
    if (g_input == nullptr || g_input->snapshot == nullptr) return;
    AnomalyInputSnapshotV1 snap{};
    snap.struct_size = sizeof(snap);
    if (g_input->snapshot(g_input->user, &snap).code != ANOMALY_STATUS_V1_OK) return;

    if (s.capture_target == 0) {
        std::memcpy(s.prev_keys, snap.keys, sizeof(s.prev_keys));
        return;
    }

    // 只看这一帧「刚按下」的键（与上一帧相比 0 -> 1）。
    for (int i = 0; i < 32; ++i) {
        const unsigned int freshly = static_cast<unsigned int>(snap.keys[i] & ~s.prev_keys[i]);
        if (freshly == 0) continue;
        const std::uint32_t bit = FirstBitIndex(static_cast<unsigned char>(freshly));
        if (bit == 0xFFFFFFFFu) continue;
        const std::uint32_t vk = static_cast<std::uint32_t>(i) * 8u + bit;

        if (vk == VK_ESCAPE) {
            CancelCapture();
        } else {
            // 退格 = 解除绑定（virtual_key 置 0）。
            const std::uint32_t bound = (vk == VK_BACK) ? 0u : vk;
            const std::uint32_t modifiers = (vk == VK_BACK) ? 0u : snap.modifiers;
            if (s.capture_target == 1) {
                s.key_once = bound;
                s.mod_once = modifiers;
            } else if (s.capture_target == 3) {
                s.key_restore = bound;
                s.mod_restore = modifiers;
            } else {
                s.key_toggle = bound;
                s.mod_toggle = modifiers;
            }
            s.capture_target = 0;
            RegisterHotkeys();
            // 改键后立刻写盘。
            g_state.last_save_code = static_cast<int>(SaveConfigImmediate().code);
        }
        std::memcpy(s.prev_keys, snap.keys, sizeof(s.prev_keys));
        return;
    }
    std::memcpy(s.prev_keys, snap.keys, sizeof(s.prev_keys));
}

// 把虚拟键码转成好认的名字（认不出就显示 VK 0xNN）。
void KeyName(std::uint32_t vk, std::uint32_t modifiers, char* out, std::size_t capacity) {
    char body[48]{};
    if (vk == 0) {
        std::snprintf(out, capacity, "未绑定");
        return;
    }
    if (vk >= '0' && vk <= '9') {
        std::snprintf(body, sizeof(body), "%c", static_cast<char>(vk));
    } else if (vk >= 'A' && vk <= 'Z') {
        std::snprintf(body, sizeof(body), "%c", static_cast<char>(vk));
    } else if (vk >= VK_F1 && vk <= VK_F12) {
        std::snprintf(body, sizeof(body), "F%u", vk - VK_F1 + 1u);
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        std::snprintf(body, sizeof(body), "小键盘%u", vk - VK_NUMPAD0);
    } else {
        switch (vk) {
            case VK_SPACE: std::snprintf(body, sizeof(body), "空格"); break;
            case VK_TAB: std::snprintf(body, sizeof(body), "Tab"); break;
            case VK_RETURN: std::snprintf(body, sizeof(body), "回车"); break;
            case VK_ESCAPE: std::snprintf(body, sizeof(body), "Esc"); break;
            case VK_BACK: std::snprintf(body, sizeof(body), "退格"); break;
            case VK_INSERT: std::snprintf(body, sizeof(body), "Insert"); break;
            case VK_DELETE: std::snprintf(body, sizeof(body), "Delete"); break;
            case VK_HOME: std::snprintf(body, sizeof(body), "Home"); break;
            case VK_END: std::snprintf(body, sizeof(body), "End"); break;
            case VK_PRIOR: std::snprintf(body, sizeof(body), "PageUp"); break;
            case VK_NEXT: std::snprintf(body, sizeof(body), "PageDown"); break;
            case VK_LEFT: std::snprintf(body, sizeof(body), "左方向键"); break;
            case VK_RIGHT: std::snprintf(body, sizeof(body), "右方向键"); break;
            case VK_UP: std::snprintf(body, sizeof(body), "上方向键"); break;
            case VK_DOWN: std::snprintf(body, sizeof(body), "下方向键"); break;
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
                std::snprintf(body, sizeof(body), "Shift"); break;
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
                std::snprintf(body, sizeof(body), "Ctrl"); break;
            case VK_MENU: case VK_LMENU: case VK_RMENU:
                std::snprintf(body, sizeof(body), "Alt"); break;
            default: std::snprintf(body, sizeof(body), "VK 0x%02X", vk); break;
        }
    }

    char prefix[24]{};
    std::size_t off = 0;
    const auto add = [&](const char* text) {
        const int written = std::snprintf(prefix + off, sizeof(prefix) - off, "%s", text);
        if (written > 0) off += static_cast<std::size_t>(written);
    };
    if ((modifiers & ANOMALY_INPUT_MODIFIER_V1_CONTROL) != 0) add("Ctrl+");
    if ((modifiers & ANOMALY_INPUT_MODIFIER_V1_SHIFT) != 0) add("Shift+");
    if ((modifiers & ANOMALY_INPUT_MODIFIER_V1_ALT) != 0) add("Alt+");
    if ((modifiers & ANOMALY_INPUT_MODIFIER_V1_SUPER) != 0) add("Win+");
    std::snprintf(out, capacity, "%s%s", prefix, body);
}

// --------------------------- 配置持久化 --------------------------------------
// `anomaly.storage::write_atomic` 是宿主管理的文件 I/O，不适合在 on_update（Game 域）或
// on_draw（Render 域）里同步调用。因此「改了立刻落盘」通过 anomaly.scheduler 把写盘推迟到
// 宿主允许的域执行；Stop / Unload 在 Lifecycle 域直接写。

// 把当前配置序列化成文本。
std::size_t FormatConfig(char* text, std::size_t capacity) {
    State& s = g_state;
    const int written =
        std::snprintf(text, capacity,
                      "conf_version=%d\nmultiplier=%.4f\nduration=%.4f\n"
                      "key_once=%u\nkey_toggle=%u\nkey_restore=%u\n"
                      "mod_once=%u\nmod_toggle=%u\nmod_restore=%u\n",
                      kConfVersion, s.multiplier, s.duration, s.key_once, s.key_toggle, s.key_restore,
                      s.mod_once, s.mod_toggle, s.mod_restore);
    if (written <= 0) return 0;
    const std::size_t size = static_cast<std::size_t>(written);
    return (size < capacity) ? size : capacity - 1;
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
// Game/Render 域同步做文件 I/O。调度器不可用时跳过（Stop/Unload 仍会兜底落盘）。
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
            } else if (std::strcmp(key, "multiplier") == 0) {
                s.multiplier = std::atof(value);
            } else if (std::strcmp(key, "duration") == 0) {
                s.duration = std::atof(value);
            } else if (std::strcmp(key, "key_once") == 0) {
                s.key_once = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            } else if (std::strcmp(key, "key_toggle") == 0) {
                s.key_toggle = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            } else if (std::strcmp(key, "key_restore") == 0) {
                s.key_restore = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            } else if (std::strcmp(key, "mod_once") == 0) {
                s.mod_once = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            } else if (std::strcmp(key, "mod_toggle") == 0) {
                s.mod_toggle = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            } else if (std::strcmp(key, "mod_restore") == 0) {
                s.mod_restore = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
            }
        }
        line = (next != nullptr) ? next + 1 : nullptr;
    }
    if (conf_version != kConfVersion) {
        s.multiplier = 3.0;
        s.duration = 10.0;
        SetError("配置文件版本不符，已回到默认值");
    }
    if (s.multiplier < kMinMultiplier) s.multiplier = kMinMultiplier;
    if (s.multiplier > kMaxMultiplier) s.multiplier = kMaxMultiplier;
    if (s.duration < 0.5) s.duration = 0.5;
    if (s.duration > 3600.0) s.duration = 3600.0;
    s.rate_refresh = 1;  // 让文本框按载入后的值重写
    s.dur_refresh = 1;
}

// ------------------------------- 绘制 ---------------------------------------
void Text(const AnomalyUiServiceV1* ui, const char* utf8) {
    if (HAS(ui, text)) ui->text(ui->user, StringView(utf8));
}

// 量化到 1 位小数：避免 1.1+0.1 累积成 1.2000000000000002。
double Quantize1(double v) {
    return (v >= 0.0) ? std::floor(v * 10.0 + 0.5) / 10.0 : std::ceil(v * 10.0 - 0.5) / 10.0;
}

// 自绘的「小数输入」控件：标签画在输入框前面（同一行，与「加速一次」按钮行同样的对齐方式），
// 后面跟 [±粗调] [±微调] 两组步进按钮 + 单位说明。
// 数值框里的文本由插件自己维护、自己解析，显示统一 "%.1f"
// （宿主 input_double 固定按 "%.17g" 格式化，1.1 会显示成 1.1000000000000001）。
// step_coarse > 0 时额外渲染一组 ±step_coarse 按钮（粗调）；step 恒为微调步进。
// 返回 1 表示值被改动（调用方据此置保存请求）。
bool DecimalField(const AnomalyUiServiceV1* ui, const char* label, const char* hint, const char* id,
                  char* text, std::size_t capacity, int* refresh, double* value, double lo,
                  double hi, double step, double step_coarse, float field_width) {
    if (!HAS(ui, input_text) || !HAS(ui, button)) return false;

    if (*refresh != 0) {
        std::snprintf(text, capacity, "%.1f", *value);
        *refresh = 0;
    }

    bool changed = false;

    // 数值框宽度：ABI 没有 set_next_item_width，用「单列定宽表格」把输入框压到指定宽度
    // （表格会按行数自适应高度，不会占满剩余高度）。end_table 只在 begin_table 返回非 0 时调用。
    // 注意：宿主 input_text 会把 label 渲染在输入框右侧，所以 label 必须用 ## 隐藏、由插件
    // 自己先画在前面；## 后面的 id 用作控件 ID。
    Text(ui, label);
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    char field_id[48];
    std::snprintf(field_id, sizeof(field_id), "##%s", id);
    char table_id[48];
    std::snprintf(table_id, sizeof(table_id), "##%s-w", id);

    const bool boxed = HAS(ui, begin_table) && HAS(ui, end_table) && HAS(ui, table_next_column);
    bool in_table = false;
    if (boxed && ui->begin_table(ui->user, StringView(table_id), 1, 0u, field_width, 0.0F) != 0) {
        in_table = true;
        ui->table_next_column(ui->user);
    }
    const bool edited = ui->input_text(ui->user, StringView(field_id), text, capacity, 0u) != 0;
    if (in_table) ui->end_table(ui->user);

    // 文本框是不是还在编辑中（正在编辑就别回写缓冲，否则会和用户输入打架）。
    std::uint32_t frame_state = 0u;
    if (HAS(ui, frame_state)) frame_state = ui->frame_state(ui->user);
    const bool item_active = (frame_state & ANOMALY_UI_FRAME_V1_ITEM_ACTIVE) != 0;

    if (edited) {
        char* end = nullptr;
        const double parsed = std::strtod(text, &end);
        if (end != text && *end == '\0' && std::isfinite(parsed)) {
            double v = Quantize1(parsed);
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            if (v != *value) {
                *value = v;
                changed = true;
            }
            // 被量化或钳位过 ⇒ 回写规范文本，免得框里留着一个与实值不符的数字。
            if (v != parsed) *refresh = 1;
        } else if (!item_active) {
            // 输入不合法且已经结束编辑（比如被清空）⇒ 恢复上一次的有效值。
            *refresh = 1;
        }
    }

    // 步进按钮：可选粗调（step_coarse > 0），恒有微调（step）。同一控件行内用 id 后缀区分 ID。
    char step_label[48];
    if (step_coarse > 0.0) {
        if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
        std::snprintf(step_label, sizeof(step_label), "-%.1f##%s-cdn", step_coarse, id);
        if (ui->button(ui->user, StringView(step_label), 46.0F, 0.0F)) {
            double v = Quantize1(*value - step_coarse);
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            *value = v;
            *refresh = 1;
            changed = true;
        }
        if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
        std::snprintf(step_label, sizeof(step_label), "+%.1f##%s-cup", step_coarse, id);
        if (ui->button(ui->user, StringView(step_label), 46.0F, 0.0F)) {
            double v = Quantize1(*value + step_coarse);
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            *value = v;
            *refresh = 1;
            changed = true;
        }
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
    std::snprintf(step_label, sizeof(step_label), "-%.1f##%s-dn", step, id);
    if (ui->button(ui->user, StringView(step_label), 46.0F, 0.0F)) {
        double v = Quantize1(*value - step);
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        *value = v;
        *refresh = 1;
        changed = true;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 6.0F);
    std::snprintf(step_label, sizeof(step_label), "+%.1f##%s-up", step, id);
    if (ui->button(ui->user, StringView(step_label), 46.0F, 0.0F)) {
        double v = Quantize1(*value + step);
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        *value = v;
        *refresh = 1;
        changed = true;
    }

    // 单位/说明放在加减按钮后面。
    if (hint != nullptr && hint[0] != '\0') {
        if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 10.0F);
        Text(ui, hint);
    }
    return changed;
}

void DrawMain(const AnomalyUiServiceV1* ui) {
    State& s = g_state;
    char line[256];
    char key_name[64];

    // 非开发者模式下顶部提示；真正拦截在 Update 的 DeveloperModeEnabled 门禁。
    if (!DeveloperModeEnabled()) {
        Text(ui, "需要开发者模式（平台设置 > 高级 > 开发者模式）才能加速");
        if (HAS(ui, separator)) ui->separator(ui->user);
    }

    // ---- 倍率 / 时长（自绘文本输入；倍率含粗调 ±1.0 + 微调 ±0.1，时长步进 1.0）----
    // 改了值就置保存请求；真正落盘在 on_update（on_draw 是 Render 域）。
    if (DecimalField(ui, "时间倍率", "（1 = 正常，越大越快）", "ta-mul", s.rate_text,
                     sizeof(s.rate_text), &s.rate_refresh, &s.multiplier, kMinMultiplier,
                     kMaxMultiplier, 0.1, 1.0, 86.0F)) {
        s.req_save = 1;
    }
    if (DecimalField(ui, "持续时长", "（秒）", "ta-dur", s.dur_text, sizeof(s.dur_text),
                     &s.dur_refresh, &s.duration, 0.5, 3600.0, 1.0, 0.0, 86.0F)) {
        s.req_save = 1;
    }

    // 注意：ImGui 用「标签」当控件 ID，同名按钮必须用 ## 后缀区分（## 之后只参与 ID）。
    // ---- 加速一次 ----
    if (HAS(ui, button) && ui->button(ui->user, StringView("加速一次##ta-once"), 130.0F, 0.0F)) {
        if (s.capture_target != 0) CancelCapture();
        s.req_action = 1;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    KeyName(s.key_once, s.mod_once, key_name, sizeof(key_name));
    std::snprintf(line, sizeof(line), "快捷键：%s", key_name);
    Text(ui, line);
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("改键##ta-rb-once"), 70.0F, 0.0F)) {
        BeginCapture(1);
    }

    // ---- 持续加速 ----
    if (HAS(ui, button)) {
        const bool on = (s.infinite != 0);
        if (ui->button(ui->user,
                       StringView(on ? "停止持续加速##ta-toggle" : "持续加速##ta-toggle"), 130.0F,
                       0.0F)) {
            if (s.capture_target != 0) CancelCapture();
            s.req_action = 2;
        }
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    KeyName(s.key_toggle, s.mod_toggle, key_name, sizeof(key_name));
    std::snprintf(line, sizeof(line), "快捷键：%s", key_name);
    Text(ui, line);
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("改键##ta-rb-tgl"), 70.0F, 0.0F)) {
        BeginCapture(2);
    }

    // ---- 立即恢复 ----
    if (HAS(ui, button) &&
        ui->button(ui->user, StringView("立即恢复 1 倍##ta-restore"), 130.0F, 0.0F)) {
        if (s.capture_target != 0) CancelCapture();
        s.req_action = 3;
    }
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    KeyName(s.key_restore, s.mod_restore, key_name, sizeof(key_name));
    std::snprintf(line, sizeof(line), "快捷键：%s", key_name);
    Text(ui, line);
    if (HAS(ui, same_line)) ui->same_line(ui->user, 0.0F, 8.0F);
    if (HAS(ui, button) && ui->button(ui->user, StringView("改键##ta-rb-rst"), 70.0F, 0.0F)) {
        BeginCapture(3);
    }

    if (s.capture_target != 0) {
        Text(ui, s.capture_target == 1   ? "请按下「加速一次」的新快捷键…（Backspace 解除绑定 / Esc 取消）"
                : s.capture_target == 3 ? "请按下「立即恢复」的新快捷键…（Backspace 解除绑定 / Esc 取消）"
                                        : "请按下「持续加速」的新快捷键…（Backspace 解除绑定 / Esc 取消）");
    }

    if (HAS(ui, separator)) ui->separator(ui->user);

    // ---- 状态：只保留这一行，显示当前实际生效的加速倍率 ----
    //   倍率取引擎的 WorldSettings+0x400（真正生效的那个）；读不到时退回游戏自有的 +0x5E0。
    {
        double now = static_cast<double>(s.engine_value);
        if (!(now > 0.0) || now > 1000.0) now = static_cast<double>(s.observed);
        if (!(now > 0.0) || now > 1000.0) now = 1.0;

        if (s.active == 0) {
            std::snprintf(line, sizeof(line), "状态：未加速（当前 %.2f 倍）", now);
        } else if (s.infinite != 0) {
            std::snprintf(line, sizeof(line), "状态：持续加速中（当前 %.2f 倍 ／ 目标 %.2f 倍）",
                          now, s.multiplier);
        } else {
            std::snprintf(line, sizeof(line),
                          "状态：加速中（当前 %.2f 倍 ／ 目标 %.2f 倍 ／ 剩余 %.1f 秒）", now,
                          s.multiplier, s.remaining);
        }
        // 出错时把原因并进同一行，避免多出第二条信息行（但错误不能看不见）。
        if (s.status[0] != '\0') {
            const std::size_t used = std::strlen(line);
            if (used + 4 < sizeof(line)) {
                std::snprintf(line + used, sizeof(line) - used, "　／ %s", s.status);
            }
        }
        Text(ui, line);
    }
}

void ANOMALY_CALL Draw(void* context, const AnomalyUiServiceV1* ui_v1) {
    (void)context;
    const AnomalyUiServiceV1* ui = (ui_v1 != nullptr) ? ui_v1 : g_ui;
    if (!HAS(ui, begin_window) || !HAS(ui, end_window)) return;

    if (HAS(ui, set_next_window_size_constraints)) {
        ui->set_next_window_size_constraints(ui->user, 420.0F, 320.0F, 100000.0F, 100000.0F);
    }
    if (HAS(ui, set_next_window_size)) {
        ui->set_next_window_size(ui->user, 640.0F, 480.0F, kCondFirstUseEver);
    }

    // 窗口恒开（对齐内建插件）：open 用局部变量，点关闭按钮下一帧自动重开，无需自备回窗热键。
    // 注意 end_window 仍要无条件调用，即使 begin 返回 0（否则宿主判 unbalanced UI stack）。
    int open = 1;
    if (ui->begin_window(ui->user, StringView("时间加速 (Time Accel)"), &open, 0u) != 0) {
        DrawMain(ui);
    }
    ui->end_window(ui->user);
}

// ------------------------------- 生命周期 ------------------------------------
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;

    const Host view(host);

    // UI 是硬依赖。
    const auto ui = view.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID,
                                                   ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || !HAS(ui.get(), text) || !HAS(ui.get(), begin_window)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_ui = ui.get();

    // 其余全部可选：缺哪块只禁用对应能力。
    const auto core = view.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID,
                                                       ANOMALY_CORE_SERVICE_V1_VERSION);
    g_core = core ? core.get() : nullptr;

    const auto names = view.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID,
                                                            ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
    g_names = names ? names.get() : nullptr;

    const auto objects = view.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
    g_objects = objects ? objects.get() : nullptr;

    const auto process_event = view.Query<AnomalyUe5ProcessEventServiceV1>(
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID, ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION);
    g_process_event = process_event ? process_event.get() : nullptr;

    const auto input = view.Query<AnomalyInputServiceV1>(ANOMALY_INPUT_SERVICE_V1_ID,
                                                         ANOMALY_INPUT_SERVICE_V1_VERSION);
    g_input = input ? input.get() : nullptr;

    const auto storage = view.Query<AnomalyStorageServiceV1>(ANOMALY_STORAGE_SERVICE_V1_ID,
                                                             ANOMALY_STORAGE_SERVICE_V1_VERSION);
    g_storage = storage ? storage.get() : nullptr;

    const auto scheduler = view.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID,
                                                                 ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    g_scheduler = scheduler ? scheduler.get() : nullptr;

    const auto player = view.Query<AnomalyNtePlayerServiceV1>(ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
                                                              ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    g_player = player ? player.get() : nullptr;

    g_state = State{};
    *context = &g_state;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    (void)context;
    g_state.capture_target = 0;
    g_state.req_action = 0;
    g_state.active = 0;
    g_state.infinite = 0;
    g_state.remaining = 0.0;
    LoadConfig();
    RegisterHotkeys();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, std::uint32_t deadline_milliseconds) {
    (void)context;
    (void)deadline_milliseconds;
    // 退出前必须恢复，否则插件停了世界还留在加速态。
    // Lifecycle 不是 Game 线程域，这里是尽力而为（写 4 字节 float，风险很低）。
    StopAccel(nullptr);
    SaveConfig();
    // 服务表绑定 Host 的生命周期 generation：Stop 与 Unload 都必须退订。
    UnsubscribeEvents();
    ReleaseHotkeys();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* context) {
    (void)context;
    StopAccel(nullptr);
    UnsubscribeEvents();
    ReleaseHotkeys();
    g_process_event = nullptr;
    g_names = nullptr;
    g_objects = nullptr;
    g_input = nullptr;
    g_storage = nullptr;
    g_scheduler = nullptr;
    g_player = nullptr;
    g_core = nullptr;
    g_ui = nullptr;
    g_state = State{};
}

void ANOMALY_CALL Update(void* context, double delta_seconds) {
    (void)context;
    State& s = g_state;

    if (s.controller_class_name_id == 0) ResolveControllerClassNameId();
    SubscribeEvents();      // 一直订阅：用它认控制器（链路的起点）
    ResolveWorldSettings(); // 每帧沿 PC -> Level -> WorldSettings 重取

    const double dt = (delta_seconds > 0.0) ? delta_seconds : 0.0;
    PollPlayerGate(dt);
    CaptureTick();

    // 界面改了倍率/时长 ⇒ 立刻写盘（用直接写盘通道；宿主 storage 在 Game 域被拒）。
    if (s.req_save != 0) {
        s.req_save = 0;
        s.last_save_code = static_cast<int>(SaveConfigImmediate().code);
    }

    // 采样当前值（界面自检：世界倍率与引擎 TimeDilation 应当相等）。
    if (s.ws != 0) {
        float v = 1.0F;
        if (SafeReadF32(s.ws + kWorldDilationOffset, &v)) s.observed = v;
        float e = 1.0F;
        if (SafeReadF32(s.ws + kEngineTimeDilationOffset, &e)) s.engine_value = e;
        float mx = 20.0F;
        if (SafeReadF32(s.ws + kMaxDilationOffset, &mx)) s.max_dilation = mx;
    }

    // 请求处理：一律在 on_update（游戏线程）里真正写。
    if (s.req_action != 0) {
        const int action = s.req_action;
        s.req_action = 0;

        if (action == 3) {
            StopAccel("已恢复 1 倍");
        } else if (!DeveloperModeEnabled()) {
            SetError("需要开发者模式：本插件会写游戏内存，请在平台设置里开启开发者模式");
            s.remaining = 0.0;
        } else if (!PlayerGateOpen()) {
            SetError("官方 nte.player 报告角色不在世界中（大厅/加载中），已取消");
            s.remaining = 0.0;
        } else if (s.ws == 0) {
            SetError("尚未解析到 WorldSettings（指针链未就绪），请稍后再试");
            s.remaining = 0.0;
        } else if (action == 1) {
            s.infinite = 0;
            s.remaining = s.duration;
            s.last_result = WriteWorldDilation(static_cast<float>(s.multiplier)) ? 1 : 0;
            s.active = (s.last_result == 1) ? 1 : 0;
            if (s.last_result == 1) {
                SetError("");
            } else {
                SetError("写入失败（WorldSettings 地址可能已失效）");
                s.remaining = 0.0;
            }
        } else if (action == 2) {
            if (s.infinite != 0) {
                StopAccel("已停止持续加速，恢复 1 倍");
            } else {
                s.last_result = WriteWorldDilation(static_cast<float>(s.multiplier)) ? 1 : 0;
                s.infinite = (s.last_result == 1) ? 1 : 0;
                s.active = (s.last_result == 1) ? 1 : 0;
                s.remaining = 0.0;
                if (s.last_result == 1) {
                    SetError("");
                } else {
                    SetError("写入失败（WorldSettings 地址可能已失效）");
                }
            }
        }
    }

    if (s.active != 0 && s.ws != 0) {
        if (s.infinite == 0) {
            s.remaining -= dt;
            if (s.remaining <= 0.0) {
                StopAccel("加速时长结束，已恢复 1 倍");
                return;
            }
        }
        // 保活：只在被游戏改回时才补写一次（不做无意义的高频写）。
        s.keep_accum += dt;
        const float target = static_cast<float>(s.multiplier);
        const float diff = (s.observed > target) ? (s.observed - target) : (target - s.observed);
        if (diff > 0.001F && s.keep_accum >= kKeepAlivePeriod) {
            s.keep_accum = 0.0;
            WriteWorldDilation(target);
        }
    } else {
        s.keep_accum = 0.0;
    }
}

}  // namespace

// 导出入口：id / name / author / version 必须与 manifest.json 一致。
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
                   StringView("anomaly.builtin.nte-time-accel"), StringView("Time Accel"),
                   StringView("WawMew"), StringView("0.3.7"), Load, Start, Stop, Unload, Update,
                   Draw};
    return anomaly::sdk::Ok();
}
