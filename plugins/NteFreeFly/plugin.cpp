// NTE Free Fly —— 角色自由飞行 / 穿墙插件（Anomaly ABI v1）
//
// 上游立场：插件的世界读写只走 anomaly.nte.* 语义服务 —— 不扫描 GObjects / World、
// 不解析 DataTable、不调用 UE ProcessEvent、不直接写游戏内存、不装 hook、不打 patch。
// 本插件完全遵守这条线，因此也不需要 memory-read / memory-write / interop-* 任何授权。
//
// 实现机制：
//   * Game 域每 tick 用一个由插件自己维护的"虚拟位置"推进玩家位置，并通过
//     anomaly.nte.player-teleport 落位。宿主以 bSweep=false / bTeleport=true 执行，
//     位移不经过碰撞检测 —— 这就是穿墙，也是它在空中不会下坠的原因。
//   * anomaly.nte.player-hold 把角色的重力系数归零、速度清零，角色因此停在插件给的
//     位置上，不会被重力拉回地面；配合空格 / Ctrl 就得到垂直飞行。
//   * 停用 / 停止 / 卸载时立即 release hold 交还重力。插件不留下任何
//     持久修改：未启用时游戏行为与没有安装本插件完全一致。
//
// 线程域：on_update = Game 域（唯一接触游戏服务的地方，也是 teleport / hold 唯一合法的域）；
//         on_draw   = Render 域（只复制插件自己的展示快照，不做任何游戏查询）。

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/nte.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ui.h"
#include "anomaly/sdk/services/ui_resources.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>

namespace {

using anomaly::sdk::StringView;

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

// Win32 虚拟键码。宿主 anomaly.input 的 keys[] 位图以虚拟键码为索引，
// 位序为 keys[vk / 8] 的第 (vk % 8) 位。
constexpr std::uint32_t kVkControl = 0x11;
constexpr std::uint32_t kVkSpace = 0x20;
constexpr std::uint32_t kVkA = 0x41;
constexpr std::uint32_t kVkD = 0x44;
constexpr std::uint32_t kVkS = 0x53;
constexpr std::uint32_t kVkW = 0x57;
constexpr std::uint32_t kVkEscape = 0x1B;

// 开关快捷键。可以在面板里改，并持久化到 anomaly.config；这是出厂默认值。
constexpr std::uint32_t kDefaultToggleKey = 0x75;  // VK_F6

constexpr double kDefaultHorizontalSpeed = 1200.0;  // UE 单位（厘米）/秒
constexpr double kDefaultVerticalSpeed = 800.0;
constexpr float kMinimumSpeed = 100.0F;
constexpr float kMaximumSpeed = 12000.0F;

constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

// hold 只读诊断量的刷新间隔。
constexpr double kHoldReportSeconds = 1.0;
// NTE 服务未发布时的重查间隔：它们只在 Profile 校验与 Game-thread gate 通过后才出现。
constexpr double kServiceRetrySeconds = 1.0;
// 角色偏离虚拟位置超过这么多厘米才落位一次。宿主传送是 ProcessEvent 调用，逐帧打会明显
// 吃性能；hold 生效时角色根本不漂移，所以悬停中几乎不需要调用传送。
constexpr double kDriftToleranceCentimeters = 15.0;
// hold 的重申间隔。它是幂等写入、由宿主持续生效，不需要逐帧重写。
constexpr double kHoldReassertSeconds = 0.15;
// 单帧最大步长：读盘 / 切图造成的长帧不应该把角色一次性弹飞。
constexpr double kMaximumTickSeconds = 0.1;
// 连续这么多次落位失败就自动关闭，避免游戏已经接管角色时插件还在原地漂移。
// 取值约半秒：既容得下一次短暂的加载 / 过场卡顿，又不会放任角色长时间漂移。
constexpr std::uint32_t kMaximumConsecutiveFailures = 30;

// 服务表尾部扩展字段的安全访问：先确认 struct_size 覆盖到该字段。
#define HAS_FIELD(service, type, field)                                                \
    ((service) != nullptr &&                                                           \
     (service)->struct_size >= offsetof(type, field) + sizeof((service)->field) &&      \
     (service)->field != nullptr)

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

struct Services final {
    const AnomalyCoreServiceV1* core{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyInputServiceV1* input{};
    const AnomalyConfigServiceV1* config{};
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyNtePlayerTeleportServiceV1* teleport{};
    const AnomalyNtePlayerHoldServiceV1* hold{};
};

struct InputState final {
    bool forward{};
    bool back{};
    bool left{};
    bool right{};
    bool up{};
    bool down{};
    bool toggle_pressed{};
};

// Game 域写、Render 域读的展示快照。on_draw 只在锁内复制它，不查游戏服务。
struct DisplayState final {
    bool active{};
    bool hold_engaged{};
    bool hold_service_available{};
    std::uint32_t hold_status{ANOMALY_STATUS_V1_OK};
    bool camera_available{};
    bool has_game_services{};
    bool capturing{};
    bool settings_persisted{};
    char toggle_key_name[24]{};
    std::uint64_t teleports{};
    std::uint32_t last_status{ANOMALY_STATUS_V1_OK};
    std::uint32_t consecutive_failures{};
    std::uint32_t movement_mode{};
    double position[3]{};
    char hint[192]{};
};

struct Context final {
    const AnomalyHostApiV1* host{};
    Services services{};

    // 设置：Render 域（UI 控件）写，Game 域读。
    std::atomic<double> horizontal_speed{kDefaultHorizontalSpeed};
    std::atomic<double> vertical_speed{kDefaultVerticalSpeed};
    std::atomic_bool toggle_requested{false};
    // 快捷键：Render 域（UI 按钮）请求捕获，Game 域完成捕获并落盘。
    std::atomic<std::uint32_t> toggle_key{kDefaultToggleKey};
    std::atomic_bool capture_requested{false};
    std::atomic_bool capture_aborted{false};

    // 设置持久化（anomaly.config 不可用时整段降级为"仅本次会话生效"）。
    AnomalyGenerationHandleV1 settings_schema{};
    bool settings_dirty{};
    bool settings_schema_ready{};

    // 以下成员只属于 Game 域。
    bool enabled{};
    bool want_enabled{};
    bool hold_engaged{};
    std::uint32_t hold_status{ANOMALY_STATUS_V1_OK};
    bool have_virtual_position{};
    bool previous_toggle_key{};
    bool capturing{};
    bool camera_available{};
    // 捕获快捷键时用相邻两帧的位图做按下边缘检测。
    std::uint8_t current_keys[32]{};
    std::uint8_t previous_keys[32]{};
    double virtual_position[3]{};
    double yaw_degrees{};
    double idle_seconds{};
    double hold_report_seconds{};
    double hold_reassert_seconds{};
    double service_retry_seconds{};
    std::uint32_t consecutive_failures{};
    std::uint32_t last_status{ANOMALY_STATUS_V1_OK};
    std::uint32_t movement_mode{};
    std::uint64_t teleport_count{};
    AnomalyGenerationHandleV1 flight_world{};
    AnomalyGenerationHandleV1 flight_player{};

    // Game 域写、Render 域读。
    mutable std::mutex display_mutex;
    DisplayState display{};
};

AnomalyStatusV1 Fail(const std::uint32_t code) noexcept { return {code, 0, {}}; }

void Log(const Context& context, const std::uint32_t level, const char* message) noexcept {
    const auto* core = context.services.core;
    if (core == nullptr || core->log == nullptr) return;
    core->log(core->user, level, StringView(message));
}

// ---------------------------------------------------------------------------
// 设置持久化（anomaly.config）
//
// 只存一个开关快捷键。config 服务不可用时整段降级：快捷键仍然可以在面板里改，
// 只是本次会话结束后回到默认值，插件其余功能不受影响。
// ---------------------------------------------------------------------------

// 定义在下面的"输入"小节，这里先用，故前置声明。
bool IsReservedKey(std::uint32_t key) noexcept;
void VirtualKeyName(std::uint32_t key, char* out, std::size_t size) noexcept;

constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 256;
constexpr std::string_view kSettingsSchema = R"json(
{"type":"object","additionalProperties":false,"required":["toggleKey"],"properties":{"toggleKey":{"type":"integer","minimum":1,"maximum":255}}}
)json";

bool ConfigMethodsAvailable(const AnomalyConfigServiceV1* service) noexcept {
    return HAS_FIELD(service, AnomalyConfigServiceV1, write_atomic) &&
           service->register_schema != nullptr && service->read != nullptr &&
           service->write_atomic != nullptr;
}

bool SettingsAvailable(const Context& context) noexcept {
    return context.settings_schema_ready && ConfigMethodsAvailable(context.services.config);
}

// 文档是本插件自己写出去的固定形状，所以这里只做受约束的扫描，不引入 JSON 解析依赖。
bool ParseToggleKey(const std::string_view document, std::uint32_t& key) noexcept {
    const std::size_t at = document.find("\"toggleKey\"");
    if (at == std::string_view::npos) return false;
    std::size_t index = at + 11U;
    while (index < document.size() &&
           (document[index] == ':' || document[index] == ' ' || document[index] == '\t')) {
        ++index;
    }
    if (index >= document.size() || document[index] < '0' || document[index] > '9') return false;
    std::uint32_t value = 0;
    while (index < document.size() && document[index] >= '0' && document[index] <= '9') {
        value = value * 10U + static_cast<std::uint32_t>(document[index] - '0');
        if (value > 255U) return false;
        ++index;
    }
    if (value == 0U) return false;
    key = value;
    return true;
}

void RegisterSettingsSchema(Context& context) noexcept {
    const auto* config = context.services.config;
    if (config == nullptr || config->register_schema == nullptr) return;

    const AnomalyByteSpanV1 schema{
        reinterpret_cast<const std::uint8_t*>(kSettingsSchema.data()), kSettingsSchema.size()};
    const AnomalyStatusV1 status = config->register_schema(
        config->user, StringView(kSettingsSchemaId), kSettingsSchemaVersion, schema,
        &context.settings_schema);
    context.settings_schema_ready =
        status.code == ANOMALY_STATUS_V1_OK && context.settings_schema.id != 0 &&
        context.settings_schema.generation != 0;
    if (!context.settings_schema_ready) context.settings_schema = {};
}

void LoadSettings(Context& context) noexcept {
    if (!SettingsAvailable(context)) return;
    const auto* config = context.services.config;

    std::uint32_t schema_version = 0;
    std::size_t size = 0;
    const AnomalyStatusV1 size_status = config->read(
        config->user, StringView(kSettingsSchemaId), &schema_version, {nullptr, 0}, &size);
    if (size_status.code == ANOMALY_STATUS_V1_NOT_FOUND) return;  // 尚未保存过，沿用默认值
    if (size_status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > kMaximumSettingsBytes) {
        return;
    }

    char document[kMaximumSettingsBytes]{};
    std::size_t copied = size;
    const AnomalyStatusV1 read_status = config->read(
        config->user, StringView(kSettingsSchemaId), &schema_version,
        {reinterpret_cast<std::uint8_t*>(document), sizeof(document)}, &copied);
    if (read_status.code != ANOMALY_STATUS_V1_OK || copied == 0 || copied > sizeof(document)) {
        return;
    }

    std::uint32_t key = 0;
    if (!ParseToggleKey(std::string_view(document, copied), key) || IsReservedKey(key)) return;
    context.toggle_key.store(key, std::memory_order_relaxed);
    context.settings_dirty = false;

    char name[24];
    char message[128];
    VirtualKeyName(key, name, sizeof(name));
    std::snprintf(message, sizeof(message), "free fly: toggle key restored from settings (%s)", name);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
}

void SaveSettings(Context& context) noexcept {
    if (!context.settings_dirty || !SettingsAvailable(context)) return;
    const auto* config = context.services.config;

    char document[64];
    const int written = std::snprintf(document, sizeof(document), "{\"toggleKey\":%u}",
                                      context.toggle_key.load(std::memory_order_relaxed));
    if (written <= 0) return;

    const AnomalyByteSpanV1 bytes{reinterpret_cast<const std::uint8_t*>(document),
                                  static_cast<std::size_t>(written)};
    if (config->write_atomic(config->user, StringView(kSettingsSchemaId), kSettingsSchemaVersion,
                             bytes).code != ANOMALY_STATUS_V1_OK) {
        return;
    }
    context.settings_dirty = false;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "free fly: toggle key saved");
}

// ---------------------------------------------------------------------------
// 服务查询与快照读取（全部只在 Game 域调用）
// ---------------------------------------------------------------------------

void ResolveGameServices(Context& context) noexcept {
    const anomaly::sdk::Host host(context.host);
    if (context.services.session == nullptr) {
        context.services.session = host.Query<AnomalyNteSessionServiceV1>(
            ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).get();
    }
    if (context.services.player == nullptr) {
        context.services.player = host.Query<AnomalyNtePlayerServiceV1>(
            ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).get();
    }
    if (context.services.teleport == nullptr) {
        context.services.teleport = host.Query<AnomalyNtePlayerTeleportServiceV1>(
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION).get();
    }
    if (context.services.hold == nullptr) {
        context.services.hold = host.Query<AnomalyNtePlayerHoldServiceV1>(
            ANOMALY_NTE_PLAYER_HOLD_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_HOLD_SERVICE_V1_VERSION).get();
    }
}

bool ReadWorld(Context& context, AnomalyGenerationHandleV1& world) noexcept {
    const auto* session = context.services.session;
    if (session == nullptr || session->snapshot == nullptr) return false;

    AnomalyNteSessionSnapshotV1 snapshot{sizeof(snapshot)};
    if (session->snapshot(session->user, &snapshot).code != ANOMALY_STATUS_V1_OK) return false;
    if (snapshot.struct_size < sizeof(snapshot)) return false;
    if (snapshot.state != ANOMALY_NTE_SESSION_V1_WORLD_READY) return false;
    if (snapshot.world.id == 0 || snapshot.world.generation == 0) return false;

    world = snapshot.world;
    return true;
}

bool ReadPlayer(Context& context, AnomalyGenerationHandleV1& player,
                double position[3]) noexcept {
    const auto* service = context.services.player;
    if (service == nullptr || service->snapshot == nullptr) return false;

    AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
    if (service->snapshot(service->user, &snapshot).code != ANOMALY_STATUS_V1_OK) return false;
    if (snapshot.struct_size < sizeof(snapshot)) return false;
    if ((snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) return false;
    if ((snapshot.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) != 0) {
        return false;
    }
    if (snapshot.handle.id == 0 || snapshot.handle.generation == 0) return false;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(snapshot.position[axis])) return false;
        position[axis] = snapshot.position[axis];
    }

    player = snapshot.handle;
    return true;
}

// 相机朝向。rotation 是 Unreal FRotator（Pitch, Yaw, Roll，单位为度），水平前进方向
// 因此是 (cos yaw, sin yaw, 0) —— 与官方 NteTeleport 的前向传送取法一致。
// 相机数据只在活动 Profile 验证了 Player 服务的可选 nte.player-esp 后才有值，
// 拿不到就退化为世界轴向（飞行仍可用，只是方向不再跟随视角）。
bool ReadCameraYaw(Context& context, double& yaw_degrees) noexcept {
    const auto* player = context.services.player;
    if (!HAS_FIELD(player, AnomalyNtePlayerServiceV1, camera_snapshot)) return false;

    AnomalyNteCameraSnapshotV1 snapshot{sizeof(snapshot)};
    if (player->camera_snapshot(player->user, &snapshot).code != ANOMALY_STATUS_V1_OK) return false;
    if (snapshot.struct_size < sizeof(snapshot)) return false;
    if ((snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) return false;
    if ((snapshot.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) != 0) {
        return false;
    }
    if (!std::isfinite(snapshot.rotation[1])) return false;

    yaw_degrees = snapshot.rotation[1];
    return true;
}

// ---------------------------------------------------------------------------
// 输入（Game 域读取宿主的规范化键盘状态）
// ---------------------------------------------------------------------------

bool KeyDown(const AnomalyInputSnapshotV1& snapshot, const std::uint32_t virtual_key) noexcept {
    return (snapshot.keys[virtual_key / 8U] & (1U << (virtual_key % 8U))) != 0;
}

// 这些键在飞行中每帧都要读，绑成开关只会互相打架，所以捕获时直接拒绝。
bool IsReservedKey(const std::uint32_t key) noexcept {
    return key == kVkW || key == kVkA || key == kVkS || key == kVkD || key == kVkControl ||
           key == kVkSpace;
}

// 虚拟键码 → 可读名字。刻意不用 Win32 的 GetKeyNameText：那需要 windows.h 与
// user32.lib，而主仓库的 anomaly_add_plugin 不链接 user32，引入它会让插件在上游
// 构建里链接失败。这里覆盖键盘常用键，其余退回 "键 0xNN"。
void VirtualKeyName(const std::uint32_t key, char* out, const std::size_t size) noexcept {
    if (out == nullptr || size == 0) return;
    if (key >= 0x41U && key <= 0x5AU) {  // A-Z
        std::snprintf(out, size, "%c", static_cast<char>(key));
        return;
    }
    if (key >= 0x30U && key <= 0x39U) {  // 0-9
        std::snprintf(out, size, "%c", static_cast<char>(key));
        return;
    }
    if (key >= 0x70U && key <= 0x87U) {  // F1-F24
        std::snprintf(out, size, "F%u", key - 0x70U + 1U);
        return;
    }
    if (key >= 0x60U && key <= 0x69U) {  // 小键盘 0-9
        std::snprintf(out, size, "小键盘%u", key - 0x60U);
        return;
    }

    const char* name = nullptr;
    switch (key) {
        case kVkEscape: name = "Esc"; break;
        case kVkSpace: name = "空格"; break;
        case kVkControl: name = "Ctrl"; break;
        case 0x10U: name = "Shift"; break;
        case 0x12U: name = "Alt"; break;
        case 0x14U: name = "CapsLock"; break;
        case 0x09U: name = "Tab"; break;
        case 0x0DU: name = "回车"; break;
        case 0x08U: name = "退格"; break;
        case 0x2DU: name = "Insert"; break;
        case 0x2EU: name = "Delete"; break;
        case 0x24U: name = "Home"; break;
        case 0x23U: name = "End"; break;
        case 0x21U: name = "PageUp"; break;
        case 0x22U: name = "PageDown"; break;
        case 0x25U: name = "←"; break;
        case 0x26U: name = "↑"; break;
        case 0x27U: name = "→"; break;
        case 0x28U: name = "↓"; break;
        case 0xBAU: name = ";"; break;
        case 0xBBU: name = "="; break;
        case 0xBCU: name = ","; break;
        case 0xBDU: name = "-"; break;
        case 0xBEU: name = "."; break;
        case 0xBFU: name = "/"; break;
        case 0xC0U: name = "`"; break;
        case 0xDBU: name = "["; break;
        case 0xDCU: name = "\\"; break;
        case 0xDDU: name = "]"; break;
        case 0xDEU: name = "'"; break;
        default: break;
    }
    if (name != nullptr) {
        std::snprintf(out, size, "%s", name);
        return;
    }
    std::snprintf(out, size, "键 0x%02X", key);
}

InputState ReadInput(Context& context) noexcept {
    InputState state;
    const auto* input = context.services.input;
    if (!HAS_FIELD(input, AnomalyInputServiceV1, snapshot)) return state;

    AnomalyInputSnapshotV1 snapshot{sizeof(snapshot)};
    if (input->snapshot(input->user, &snapshot).code != ANOMALY_STATUS_V1_OK) return state;
    if (snapshot.struct_size < offsetof(AnomalyInputSnapshotV1, keys) + sizeof(snapshot.keys)) {
        return state;
    }
    std::memcpy(context.current_keys, snapshot.keys, sizeof(context.current_keys));

    state.forward = KeyDown(snapshot, kVkW);
    state.back = KeyDown(snapshot, kVkS);
    state.left = KeyDown(snapshot, kVkA);
    state.right = KeyDown(snapshot, kVkD);
    state.up = KeyDown(snapshot, kVkSpace);
    state.down = KeyDown(snapshot, kVkControl);

    const std::uint32_t toggle_key = context.toggle_key.load(std::memory_order_relaxed);
    const bool toggle = KeyDown(snapshot, toggle_key);
    state.toggle_pressed = toggle && !context.previous_toggle_key;
    context.previous_toggle_key = toggle;
    return state;
}

// 捕获开关快捷键：取这一帧新按下的第一个键。Esc 取消，飞行按键拒绝绑定（保持捕获）。
void CaptureToggleKey(Context& context) noexcept {
    for (std::uint32_t key = 1; key < 256U; ++key) {
        const bool down = (context.current_keys[key / 8U] & (1U << (key % 8U))) != 0;
        const bool was = (context.previous_keys[key / 8U] & (1U << (key % 8U))) != 0;
        if (!down || was) continue;

        if (key == kVkEscape) {
            context.capturing = false;
            return;
        }
        if (IsReservedKey(key)) return;  // 保持捕获状态，等一个可用的键

        context.toggle_key.store(key, std::memory_order_relaxed);
        // 这个键此刻是被按住的：把边缘状态对齐到按下，免得松手后立刻触发一次开关。
        context.previous_toggle_key = true;
        context.settings_dirty = true;
        context.capturing = false;
        // 立即落盘：改键是低频操作，没理由等停止再写 —— 否则游戏崩溃就白改了。
        SaveSettings(context);
        return;
    }
}

// ---------------------------------------------------------------------------
// 飞行
// ---------------------------------------------------------------------------

AnomalyStatusV1 SendTeleport(Context& context, const AnomalyGenerationHandleV1 world,
                             const AnomalyGenerationHandleV1 player,
                             const double position[3]) noexcept {
    const auto* teleport = context.services.teleport;
    if (!HAS_FIELD(teleport, AnomalyNtePlayerTeleportServiceV1, teleport)) {
        return Fail(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    // IMMEDIATE：每 tick 的步长只有几厘米到几十厘米，落点必然已经在已加载区域内。
    // 宿主默认模式（flags = 0）会先流式预载、再冻结角色，那是为一次性跨区传送设计的窗口，
    // 与"每帧连续控制"互相打架，所以这里明确走同步语义。
    request.flags = ANOMALY_NTE_PLAYER_TELEPORT_REQUEST_V1_IMMEDIATE;
    request.world = world;
    request.player = player;
    for (std::size_t axis = 0; axis < 3; ++axis) request.position[axis] = position[axis];
    return teleport->teleport(teleport->user, &request);
}

// hold 有两个入口：独立的 anomaly.nte.player-hold 服务，以及挂在 player 服务表尾的
// hold_engage / hold_release / hold_snapshot 三件套。宿主按 Profile 校验结果二选一发布
// —— 官方 NteMovementHold 走的就是表尾那条路，所以两条都必须试，只试一条会在另一条上
// 拿到 UNAVAILABLE。
bool HasTailHold(const Context& context) noexcept {
    const auto* player = context.services.player;
    return HAS_FIELD(player, AnomalyNtePlayerServiceV1, hold_engage) &&
           HAS_FIELD(player, AnomalyNtePlayerServiceV1, hold_release) &&
           HAS_FIELD(player, AnomalyNtePlayerServiceV1, hold_snapshot);
}

bool HoldAvailable(const Context& context) noexcept {
    const auto* hold = context.services.hold;
    return HasTailHold(context) || (hold != nullptr && hold->engage != nullptr);
}

void EngageHold(Context& context) noexcept {
    const auto* hold = context.services.hold;
    const auto* player = context.services.player;

    // 优先走 player 服务表尾：官方 NteMovementHold 用的就是这条路，SDK 头注释也写明
    // "the hold entry points live on the player service"。独立服务按 Profile 校验结果
    // 可能整张表都不发布（那时查询返回 UNAVAILABLE），表尾字段才是常见可用路径。
    AnomalyStatusV1 status{ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    if (HAS_FIELD(player, AnomalyNtePlayerServiceV1, hold_engage)) {
        status = player->hold_engage(player->user);
    } else if (hold != nullptr && hold->engage != nullptr) {
        status = hold->engage(hold->user);
    }

    // 角色被送到空中后引擎会把它置为 MOVE_Falling，hold 的写入在该状态下会被游戏覆盖
    // （hold_snapshot 的 movement_mode 为 3 就是"被覆盖了"的可观测信号），所以要按固定
    // 间隔持续重申，单次 engage 维持不住。
    context.hold_status = status.code;
    context.hold_engaged = status.code == ANOMALY_STATUS_V1_OK;
}

void ReleaseHold(Context& context) noexcept {
    if (!context.hold_engaged) return;
    const auto* hold = context.services.hold;
    const auto* player = context.services.player;
    // 停用路径必须交还重力，绝不能让角色带着"重力归零"离开本次插件生命周期。
    // 释放要和 engage 走同一个入口，所以顺序保持一致。
    if (HAS_FIELD(player, AnomalyNtePlayerServiceV1, hold_release)) {
        static_cast<void>(player->hold_release(player->user));
    } else if (hold != nullptr && hold->release != nullptr) {
        static_cast<void>(hold->release(hold->user));
    }
    context.hold_engaged = false;
}

bool StartFlying(Context& context) noexcept {
    ResolveGameServices(context);

    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double position[3]{};
    if (!ReadWorld(context, world) || !ReadPlayer(context, player, position)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "free fly: the game world or the local player is not available yet");
        return false;
    }
    if (!HAS_FIELD(context.services.teleport, AnomalyNtePlayerTeleportServiceV1, teleport)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "free fly: the player teleport service is unavailable, flight stays off");
        return false;
    }

    context.flight_world = world;
    context.flight_player = player;
    std::memcpy(context.virtual_position, position, sizeof(position));
    context.have_virtual_position = true;
    context.consecutive_failures = 0;
    context.idle_seconds = 0.0;
    context.hold_report_seconds = 0.0;
    context.teleport_count = 0;
    context.camera_available = ReadCameraYaw(context, context.yaw_degrees);
    context.enabled = true;

    EngageHold(context);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        context.hold_engaged
            ? "free fly: engaged (WASD to move, Space up, Ctrl down)"
            : "free fly: engaged without the character hold, altitude may drift");
    return true;
}

void StopFlying(Context& context, const char* reason) noexcept {
    // 交还重力。传送只在 Game 域有效，从 Lifecycle 域（on_stop / on_unload）调用时
    // hold_release 会排队到 Game tick 执行，重力一定会回来。
    ReleaseHold(context);
    context.enabled = false;
    context.have_virtual_position = false;
    context.idle_seconds = 0.0;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        reason != nullptr ? reason : "free fly: stopped");
}

void UpdateFlight(Context& context, const InputState& input, const double delta_seconds) noexcept {
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double current[3]{};
    if (!ReadWorld(context, world) || !ReadPlayer(context, player, current)) {
        StopFlying(context, "free fly: the world or the player went away, flight stopped");
        return;
    }

    // 换世界 / 换角色后旧坐标不再有意义，重新以角色当前位置为基准。
    const bool rebased = world.id != context.flight_world.id ||
                         world.generation != context.flight_world.generation ||
                         player.id != context.flight_player.id ||
                         player.generation != context.flight_player.generation;
    context.flight_world = world;
    context.flight_player = player;
    if (rebased) {
        std::memcpy(context.virtual_position, current, sizeof(current));
        context.have_virtual_position = true;
        context.consecutive_failures = 0;
    }
    if (!context.have_virtual_position) {
        std::memcpy(context.virtual_position, current, sizeof(current));
        context.have_virtual_position = true;
    }

    double forward_axis = (input.forward ? 1.0 : 0.0) - (input.back ? 1.0 : 0.0);
    double right_axis = (input.right ? 1.0 : 0.0) - (input.left ? 1.0 : 0.0);
    const double vertical_axis = (input.up ? 1.0 : 0.0) - (input.down ? 1.0 : 0.0);

    // 斜向不加速；没有水平输入时就不必付相机查询的代价。
    const double planar = std::sqrt(forward_axis * forward_axis + right_axis * right_axis);
    if (planar > 0.0) {
        forward_axis /= planar;
        right_axis /= planar;
        context.camera_available = ReadCameraYaw(context, context.yaw_degrees);
    }

    const double yaw = (context.camera_available ? context.yaw_degrees : 0.0) * kDegreesToRadians;
    const double forward_x = std::cos(yaw);
    const double forward_y = std::sin(yaw);
    const double speed = context.horizontal_speed.load(std::memory_order_relaxed);
    const double climb = context.vertical_speed.load(std::memory_order_relaxed);

    context.virtual_position[0] +=
        (forward_x * forward_axis - forward_y * right_axis) * speed * delta_seconds;
    context.virtual_position[1] +=
        (forward_y * forward_axis + forward_x * right_axis) * speed * delta_seconds;
    context.virtual_position[2] += vertical_axis * climb * delta_seconds;

    // 落位按需触发，而不是逐帧。宿主传送走 ProcessEvent，每帧一次会明显吃性能；而 hold
    // 生效时角色压根不漂移，悬停中根本不需要传送。被覆盖时角色持续下落，靠这个漂移阈值
    // 把纠正频率压到每秒几次，代价是抖动幅度等于阈值本身（而不是零）。
    const bool has_input = planar > 0.0 || vertical_axis != 0.0;
    double drift = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double gap = std::fabs(current[axis] - context.virtual_position[axis]);
        if (gap > drift) drift = gap;
    }
    if (has_input || drift > kDriftToleranceCentimeters) {
        const AnomalyStatusV1 status =
            SendTeleport(context, world, player, context.virtual_position);
        context.last_status = status.code;
        if (status.code == ANOMALY_STATUS_V1_OK) {
            context.consecutive_failures = 0;
            ++context.teleport_count;
        } else if (++context.consecutive_failures >= kMaximumConsecutiveFailures) {
            StopFlying(context, "free fly: teleport kept failing, flight stopped");
            return;
        }
    }

    // hold 按固定间隔重申：它是幂等写入、由宿主持续生效，不需要逐帧重写。缺服务时
    // 顺带按秒重查（NTE 服务随 Profile 校验进度逐个出现，可能起飞之后才发布）。
    context.hold_reassert_seconds += delta_seconds;
    if (context.hold_reassert_seconds >= kHoldReassertSeconds) {
        context.hold_reassert_seconds = 0.0;
        if (context.services.hold == nullptr) {
            context.service_retry_seconds += delta_seconds;
            if (context.service_retry_seconds >= kServiceRetrySeconds) {
                context.service_retry_seconds = 0.0;
                ResolveGameServices(context);
            }
        }
        EngageHold(context);
    }

    // hold 的只读诊断：movement_mode 为 3（MOVE_Falling）表示游戏仍在报告下落，
    // 那是"游戏覆盖了 hold 写入"的唯一可观测信号。两个入口都读。
    context.hold_report_seconds += delta_seconds;
    if (context.hold_report_seconds >= kHoldReportSeconds) {
        context.hold_report_seconds = 0.0;
        AnomalyNtePlayerHoldSnapshotV1 snapshot{sizeof(snapshot)};
        bool read = false;
        const auto* hold = context.services.hold;
        const auto* player = context.services.player;
        if (HAS_FIELD(player, AnomalyNtePlayerServiceV1, hold_snapshot)) {
            read = player->hold_snapshot(player->user, &snapshot).code == ANOMALY_STATUS_V1_OK;
        } else if (hold != nullptr && hold->snapshot != nullptr) {
            read = hold->snapshot(hold->user, &snapshot).code == ANOMALY_STATUS_V1_OK;
        }
        if (read) context.movement_mode = snapshot.movement_mode;
    }
}

void PublishDisplay(Context& context) noexcept {
    std::scoped_lock lock(context.display_mutex);
    DisplayState& display = context.display;

    display.active = context.enabled;
    display.hold_engaged = context.hold_engaged;
    display.hold_status = context.hold_status;
    display.hold_service_available = HoldAvailable(context);
    display.camera_available = context.camera_available;
    display.capturing = context.capturing;
    display.settings_persisted = SettingsAvailable(context);
    VirtualKeyName(context.toggle_key.load(std::memory_order_relaxed), display.toggle_key_name,
                   sizeof(display.toggle_key_name));
    display.has_game_services = context.services.session != nullptr &&
                                context.services.player != nullptr &&
                                context.services.teleport != nullptr;
    display.teleports = context.teleport_count;
    display.last_status = context.last_status;
    display.consecutive_failures = context.consecutive_failures;
    display.movement_mode = context.movement_mode;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        display.position[axis] = context.enabled ? context.virtual_position[axis] : 0.0;
    }

    const char* hint = "";
    if (!display.has_game_services) {
        hint = "等待游戏会话：NTE 服务尚未发布，此时飞行不可用。";
    } else if (display.active && !display.camera_available) {
        hint = "相机朝向不可用，移动方向按世界轴向（未启用 nte.player-esp）。";
    } else if (display.active && display.consecutive_failures != 0) {
        hint = "本次落位被拒绝，正在重试；连续失败会自动关闭飞行。";
    } else if (display.active) {
        hint = "飞行中：位移不经过碰撞检测，可直接穿过墙体。";
    } else {
        hint = "未启用：游戏未被修改，与没有安装本插件时完全一致。";
    }
    std::snprintf(display.hint, sizeof(display.hint), "%s", hint);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) return Fail(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *plugin_context = nullptr;

    auto* context = new (std::nothrow) Context();
    if (context == nullptr) return Fail(ANOMALY_STATUS_V1_FAILED);
    context->host = host;

    const anomaly::sdk::Host view(host);
    const auto core = view.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    const auto ui = view.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    const auto input = view.Query<AnomalyInputServiceV1>(
        ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION);
    // 设置持久化是可选的：拿不到 config 时插件照常工作，只是快捷键不跨会话保留。
    const auto config = view.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);

    if (!HAS_FIELD(ui.get(), AnomalyUiServiceV1, begin_window) ||
        !HAS_FIELD(ui.get(), AnomalyUiServiceV1, end_window)) {
        delete context;
        return Fail(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    if (!HAS_FIELD(input.get(), AnomalyInputServiceV1, snapshot)) {
        delete context;
        return Fail(ANOMALY_STATUS_V1_UNAVAILABLE);
    }

    context->services.core = core.get();
    context->services.ui = ui.get();
    context->services.input = input.get();
    context->services.config = ConfigMethodsAvailable(config.get()) ? config.get() : nullptr;

    RegisterSettingsSchema(*context);
    LoadSettings(*context);

    // NTE 服务由游戏桥接层在玩家存在、Profile 校验通过之后才发布。在这里返回 UNAVAILABLE
    // 会把插件永久停在"等待服务"状态，所以先加载，运行期再补齐。
    ResolveGameServices(*context);

    *plugin_context = context;
    char name[24];
    char message[128];
    VirtualKeyName(context->toggle_key.load(std::memory_order_relaxed), name, sizeof(name));
    std::snprintf(message, sizeof(message),
                  "free fly loaded: toggle with %s or the panel button", name);
    Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Fail(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "free fly ready: the toggle key or the panel button starts flight");
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t /*deadline_milliseconds*/) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Fail(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->want_enabled = false;
    StopFlying(*context, "free fly: stopped");
    SaveSettings(*context);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    StopFlying(*context, "free fly: unloaded");
    SaveSettings(*context);
    delete context;
}

void ANOMALY_CALL Update(void* plugin_context, double delta_seconds) noexcept {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    try {
        if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) delta_seconds = 0.0;
        if (delta_seconds > kMaximumTickSeconds) delta_seconds = kMaximumTickSeconds;

        const InputState input = ReadInput(*context);

        // 快捷键捕获：面板点了"修改"之后，在 Game 域等一个可用的按键。
        if (context->capture_aborted.exchange(false, std::memory_order_relaxed)) {
            context->capturing = false;
        }
        if (context->capture_requested.exchange(false, std::memory_order_relaxed)) {
            context->capturing = true;
            // 把上一帧对齐到当前帧，否则进入捕获前就已经按着的键会被当成新按下。
            std::memcpy(context->previous_keys, context->current_keys,
                        sizeof(context->previous_keys));
        }
        if (context->capturing) {
            CaptureToggleKey(*context);
        } else if (context->toggle_requested.exchange(false, std::memory_order_relaxed) ||
                   input.toggle_pressed) {
            context->want_enabled = !context->want_enabled;
        }

        // 留给下一帧做按下边缘检测。
        std::memcpy(context->previous_keys, context->current_keys,
                    sizeof(context->previous_keys));

        if (context->want_enabled && !context->enabled) {
            if (!StartFlying(*context)) context->want_enabled = false;
        } else if (!context->want_enabled && context->enabled) {
            StopFlying(*context, "free fly: disengaged");
        }

        if (context->enabled) {
            UpdateFlight(*context, input, delta_seconds);
        } else if (context->services.session == nullptr || context->services.player == nullptr ||
                   context->services.teleport == nullptr || context->services.hold == nullptr) {
            // 只在确实还缺服务时才重查，避免每帧无谓地打服务图。
            context->service_retry_seconds += delta_seconds;
            if (context->service_retry_seconds >= kServiceRetrySeconds) {
                context->service_retry_seconds = 0.0;
                ResolveGameServices(*context);
            }
        }

        PublishDisplay(*context);
    } catch (...) {
        // 宿主回调边界：插件异常不得逃逸。
    }
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui_v1) noexcept {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    try {
        const AnomalyUiServiceV1* ui = (ui_v1 != nullptr) ? ui_v1 : context->services.ui;
        if (!HAS_FIELD(ui, AnomalyUiServiceV1, begin_window) ||
            !HAS_FIELD(ui, AnomalyUiServiceV1, end_window)) {
            return;
        }

        // 锁内只复制快照，锁外才调用宿主 UI。
        DisplayState display;
        {
            std::scoped_lock lock(context->display_mutex);
            display = context->display;
        }

        if (ui->begin_window(ui->user, StringView("NTE 自由飞行"), nullptr, 0u) != 0) {
            if (HAS_FIELD(ui, AnomalyUiServiceV1, button) && HAS_FIELD(ui, AnomalyUiServiceV1, text)) {
                char line[320];
                std::snprintf(line, sizeof(line), "状态：%s",
                              display.active ? (display.hold_engaged ? "飞行中" : "飞行中（未冻结角色）")
                                             : "未启用");
                ui->text(ui->user, StringView(line));
                char keys_line[160];
                std::snprintf(keys_line, sizeof(keys_line),
                              "WASD 前后左右 · 空格上升 · Ctrl 下降 · %s 开关",
                              display.toggle_key_name);
                ui->text(ui->user, StringView(keys_line));

                if (ui->button(ui->user, StringView(display.active ? "关闭飞行" : "开启飞行"),
                               0.0F, 0.0F) != 0) {
                    context->toggle_requested.store(true, std::memory_order_relaxed);
                }

                if (display.active) {
                    std::snprintf(line, sizeof(line),
                                  "位置：%.1f, %.1f, %.1f",
                                  display.position[0], display.position[1], display.position[2]);
                    ui->text(ui->user, StringView(line));
                    std::snprintf(line, sizeof(line), "已落位 %llu 次，失败 %u 次",
                                  static_cast<unsigned long long>(display.teleports),
                                  display.consecutive_failures);
                    ui->text(ui->user, StringView(line));
                    // 抖动时看这一行：hold 服务不可用 / engage 被拒 / 模式为 3（仍在 Falling）
                    // 分别指向不同的原因。
                    std::snprintf(line, sizeof(line),
                                  "重力冻结：服务%s，engage 返回 %u，移动模式 %u",
                                  display.hold_service_available ? "可用" : "不可用",
                                  display.hold_status, display.movement_mode);
                    ui->text(ui->user, StringView(line));
                }

                ui->text(ui->user, StringView(display.hint));
            }

            if (HAS_FIELD(ui, AnomalyUiServiceV1, separator)) ui->separator(ui->user);

            // 开关快捷键：点"修改"后进入捕获，在 Game 域取下一个按下的键。
            if (HAS_FIELD(ui, AnomalyUiServiceV1, text) &&
                HAS_FIELD(ui, AnomalyUiServiceV1, button)) {
                if (display.capturing) {
                    ui->text(ui->user, StringView("请按下要绑定的按键…（Esc 取消）"));
                    if (ui->button(ui->user, StringView("取消"), 0.0F, 0.0F) != 0) {
                        context->capture_aborted.store(true, std::memory_order_relaxed);
                    }
                } else {
                    char key_line[128];
                    std::snprintf(key_line, sizeof(key_line), "开关快捷键：%s",
                                  display.toggle_key_name);
                    ui->text(ui->user, StringView(key_line));
                    if (ui->button(ui->user, StringView("修改快捷键"), 0.0F, 0.0F) != 0) {
                        context->capture_requested.store(true, std::memory_order_relaxed);
                    }
                    if (!display.settings_persisted) {
                        ui->text(ui->user,
                                 StringView("（配置服务不可用，改动只在本次游戏会话内有效）"));
                    }
                }
            }

            if (HAS_FIELD(ui, AnomalyUiServiceV1, slider_float)) {
                float value =
                    static_cast<float>(context->horizontal_speed.load(std::memory_order_relaxed));
                static_cast<void>(ui->slider_float(ui->user, StringView("水平速度（厘米/秒）"),
                                                   &value, kMinimumSpeed, kMaximumSpeed));
                context->horizontal_speed.store(static_cast<double>(value),
                                                std::memory_order_relaxed);

                value = static_cast<float>(context->vertical_speed.load(std::memory_order_relaxed));
                static_cast<void>(ui->slider_float(ui->user, StringView("升降速度（厘米/秒）"),
                                                   &value, kMinimumSpeed, kMaximumSpeed));
                context->vertical_speed.store(static_cast<double>(value),
                                              std::memory_order_relaxed);
            }

        }

        // 宿主从 begin_window 起就记一个窗口，所以即使窗口本帧不可见也必须调用 end。
        ui->end_window(ui->user);
    } catch (...) {
        // 宿主回调边界：插件异常不得逃逸。
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Fail(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {sizeof(*descriptor),
                   ANOMALY_PLUGIN_API_V1_MAJOR,
                   ANOMALY_PLUGIN_API_V1_MINOR,
                   StringView("local.nte.free-fly"),
                   StringView("NTE Free Fly"),
                   StringView("Anomaly Plugin"),
                   StringView("1.5.1"),
                   Load,
                   Start,
                   Stop,
                   Unload,
                   Update,
                   Draw};
    return anomaly::sdk::Ok();
}
