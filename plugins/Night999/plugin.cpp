#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/plugin_state.h"
#include "plugins/common/localization.hpp"
// 共享自动战斗状态机（静态库 anomaly_plugin_combat）。必须放在匿名命名空间之外包含，
// 否则它的 anomaly::plugins::combat 会被套进匿名命名空间、与静态库里的定义对不上。
#include "plugins/common/combat/auto_combat.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

using anomaly::plugins::Localizer;
using anomaly::sdk::Ok;
using anomaly::sdk::StringView;
using anomaly::sdk::UiWindow;

// ---------------------------------------------------------------------------
// 脚本用的两个传送点与传送调参
// ---------------------------------------------------------------------------
// 营地（刷新怪物处）。
constexpr double kDefaultCampX = -145902.874;
constexpr double kDefaultCampY = -144517.527;
constexpr double kDefaultCampZ = 10144.029;
// 战斗点。
constexpr double kDefaultFightX = -145438.826;
constexpr double kDefaultFightY = -140743.761;
constexpr double kDefaultFightZ = 10159.201;

// 传送桥默认走「预载模式」：宿主先把目标点所在的流送单元加载出来，再在同一次窗口内把角色
// 交回地面，避免落进未加载地形。延迟为 0 时退化为立即传送，此时才需要 Z 抬升与沉底重试。
constexpr double kPreloadDelayDefault = 2.0;
constexpr double kPreloadDelayMaximum = 15.0;
constexpr double kLandingLiftDefault = 250.0;
constexpr double kLandingLiftMaximum = 2000.0;
constexpr std::uint32_t kSinkRetriesDefault = 2;
constexpr std::uint32_t kSinkRetriesMaximum = 8;
constexpr std::uint32_t kLandingSettleTicks = 40;
constexpr std::uint32_t kLandingWatchTicks = 180;
constexpr double kSinkThreshold = 300.0;
constexpr double kSinkRetryLiftStep = 200.0;
constexpr double kMaximumCoordinate = 1.0e9;

constexpr std::size_t kCoordinateTextCapacity = 32;
constexpr std::size_t kNumberTextCapacity = 16;
// 仅用于读旧版设置文件（v2 尾部有这个字段）。
constexpr std::size_t kResetButtonNameCapacity = 64;

constexpr std::uint32_t kSettingsMagic =
    static_cast<std::uint32_t>('N') | (static_cast<std::uint32_t>('9') << 8u) |
    (static_cast<std::uint32_t>('9') << 16u) | (static_cast<std::uint32_t>('K') << 24u);
constexpr std::uint32_t kSettingsVersion = 3u;
constexpr std::uint32_t kSettingsVersionV2 = 2u;
constexpr std::uint32_t kSettingsVersionV1 = 1u;

constexpr std::string_view kSettingsFileName = "night999-settings-v1.bin";

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

constexpr AnomalyStatusV1 StatusCode(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

template <typename Service>
struct ServiceQuery {
    const Service* service{};
    AnomalyStatusV1 status{StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};

    [[nodiscard]] explicit operator bool() const noexcept { return service != nullptr; }
};

template <typename Service>
ServiceQuery<Service> QueryService(
    const AnomalyHostApiV1* host, const std::string_view id,
    const std::uint32_t minimum_version) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->api_major != ANOMALY_PLUGIN_API_V1_MAJOR || host->query_service == nullptr) {
        return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};
    }

    const void* table{};
    const AnomalyStatusV1 status = host->query_service(
        host->host_context, StringView(id), minimum_version, &table);
    if (status.code != ANOMALY_STATUS_V1_OK) return {nullptr, status};
    if (table == nullptr) return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};

    const auto* service = static_cast<const Service*>(table);
    constexpr std::size_t kServicePrefixSize = offsetof(Service, user) + sizeof(void*);
    if (service->struct_size < kServicePrefixSize || service->service_version < minimum_version) {
        return {nullptr, StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE)};
    }
    return {service, StatusCode(ANOMALY_STATUS_V1_OK)};
}

const char* StatusName(const std::uint32_t code) noexcept {
    switch (code) {
    case ANOMALY_STATUS_V1_OK: return "OK";
    case ANOMALY_STATUS_V1_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "UNAVAILABLE";
    case ANOMALY_STATUS_V1_NOT_FOUND: return "NOT_FOUND";
    case ANOMALY_STATUS_V1_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case ANOMALY_STATUS_V1_FAILED: return "FAILED";
    case ANOMALY_STATUS_V1_TIMEOUT: return "TIMEOUT";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "PERMISSION_DENIED";
    case ANOMALY_STATUS_V1_CONFLICT: return "CONFLICT";
    case ANOMALY_STATUS_V1_CANCELLED: return "CANCELLED";
    default: return "UNKNOWN_STATUS";
    }
}

struct Settings {
    double camp_target[3]{kDefaultCampX, kDefaultCampY, kDefaultCampZ};
    double fight_target[3]{kDefaultFightX, kDefaultFightY, kDefaultFightZ};
    double preload_delay{kPreloadDelayDefault};
    double landing_lift{kLandingLiftDefault};
    std::uint32_t sink_retries{kSinkRetriesDefault};
    std::uint32_t loop_count{};
};

struct SettingsFileHeader {
    std::uint32_t magic{};
    std::uint32_t version{};
};

// v1：只有一个目标点与传送调参；v2：追加刷新按钮名（现已不使用，读取时跳过）。
struct SettingsFileV1 {
    std::uint32_t magic{};
    std::uint32_t version{};
    double target[3]{};
    double preload_delay{};
    double landing_lift{};
    std::uint32_t sink_retries{};
    std::uint32_t reserved{};
};

struct SettingsFileV2 {
    std::uint32_t magic{};
    std::uint32_t version{};
    double target[3]{};
    double preload_delay{};
    double landing_lift{};
    std::uint32_t sink_retries{};
    std::uint32_t force_click{};
    char reset_button[kResetButtonNameCapacity]{};
};

// v3：营地 / 战斗点两个坐标 + 循环次数。
struct SettingsFileV3 {
    std::uint32_t magic{};
    std::uint32_t version{};
    double camp_target[3]{};
    double fight_target[3]{};
    double preload_delay{};
    double landing_lift{};
    std::uint32_t sink_retries{};
    std::uint32_t loop_count{};
};

// 传送请求：句柄必须是「当前」快照里的 world / player，过期句柄宿主会直接拒绝。
struct PendingTeleport {
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double position[3]{};
};

// 传送之后的沉降观察：宿主用 bSweep=false 把角色精确放到目标点，若目标点在地板下沿，
// 角色会嵌进几何体。这里只用于立即传送路径，发现 Z 明显低于落点就抬高重传。
struct LandingMonitor {
    bool active{};
    AnomalyGenerationHandleV1 world{};
    AnomalyGenerationHandleV1 player{};
    double base_position[3]{};
    std::uint32_t retries{};
    std::uint32_t settle_ticks{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    Localizer localizer;
    const AnomalyCoreServiceV1* core{};
    const AnomalyPluginStateServiceV1* plugin_state{};
    const AnomalyUe5NamesServiceV1* names{};
    const AnomalyUe5ObjectsServiceV1* objects{};
    const AnomalyUe5WorldServiceV1* world{};
    const AnomalyUe5ProcessEventServiceV1* events{};
    const AnomalyUe5FrameworkServiceV1* framework{};
    // 自动战斗要用的服务（actors/entities/combat/skills/skill-invocation/pickup 随世界加载
    // 才发布，空指针由 CombatRequireServices 惰性补查）。
    const AnomalyNteSessionServiceV1* session{};
    const AnomalyNtePlayerServiceV1* player{};
    const AnomalyNtePlayerTeleportServiceV1* teleport{};
    const AnomalyNteNavigationServiceV1* navigation{};
    const AnomalyNteCombatServiceV1* combat{};
    const AnomalyNteSkillsServiceV1* skills{};
    const AnomalyNteSkillInvocationServiceV1* skill_invocation{};
    const AnomalyNteActorsServiceV1* actors{};
    const AnomalyNteEntitiesServiceV1* entities{};
    const AnomalyNtePickupServiceV1* pickup{};
    const AnomalySignatureServiceV1* signature{};
    std::filesystem::path settings_path;
    std::mutex mutex;
    Settings settings{};
    bool has_result{};
    std::uint32_t result_code{ANOMALY_STATUS_V1_UNAVAILABLE};
    char result_message[192]{};
} g_context;

// on_draw 线程独占的输入缓冲（ImGui 的文本存储必须跨帧保留）。
std::array<char, kCoordinateTextCapacity> g_camp_x_text{};
std::array<char, kCoordinateTextCapacity> g_camp_y_text{};
std::array<char, kCoordinateTextCapacity> g_camp_z_text{};
std::array<char, kCoordinateTextCapacity> g_fight_x_text{};
std::array<char, kCoordinateTextCapacity> g_fight_y_text{};
std::array<char, kCoordinateTextCapacity> g_fight_z_text{};
std::array<char, kNumberTextCapacity> g_preload_text{};

// 跨线程标志。
std::atomic_bool g_text_stale{true};
std::atomic_bool g_settings_dirty{false};

// on_update（Game 回调域）独占的状态。
LandingMonitor g_landing{};
std::chrono::steady_clock::time_point g_next_save{};

void Log(const std::uint32_t level, const std::string& message) noexcept {
    const auto* core = g_context.core;
    if (core == nullptr || core->log == nullptr) return;
    try {
        core->log(core->user, level, StringView(message));
    } catch (...) {
    }
}

bool IsFinitePosition(const double position[3]) noexcept {
    return position != nullptr && std::isfinite(position[0]) && std::isfinite(position[1]) &&
        std::isfinite(position[2]);
}

bool IsUsableCoordinate(const double value) noexcept {
    return std::isfinite(value) && std::abs(value) <= kMaximumCoordinate;
}

bool IsUsablePosition(const double position[3]) noexcept {
    return position != nullptr && IsUsableCoordinate(position[0]) &&
        IsUsableCoordinate(position[1]) && IsUsableCoordinate(position[2]);
}

bool SameValue(const double left, const double right) noexcept {
    return std::abs(left - right) <= 1.0e-6;
}

bool SamePosition(const double left[3], const double right[3]) noexcept {
    return SameValue(left[0], right[0]) && SameValue(left[1], right[1]) &&
        SameValue(left[2], right[2]);
}

bool IsCurrentWorld(const AnomalyNteSessionSnapshotV1& snapshot) noexcept {
    return snapshot.struct_size >= sizeof(snapshot) &&
        snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY &&
        snapshot.world.id != 0 && snapshot.world.generation != 0;
}

bool IsCurrentPlayer(const AnomalyNtePlayerSnapshotV1& snapshot) noexcept {
    return snapshot.struct_size >= sizeof(snapshot) &&
        (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
        (snapshot.flags & (ANOMALY_NTE_SNAPSHOT_V1_STALE | ANOMALY_NTE_SNAPSHOT_V1_PARTIAL)) ==
            0 &&
        snapshot.handle.id != 0 && snapshot.handle.generation != 0;
}

bool TeleportServiceMethodsAvailable(
    const AnomalyNtePlayerTeleportServiceV1* service) noexcept {
    return service != nullptr &&
        service->service_version >= ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION &&
        HasField<AnomalyNtePlayerTeleportServiceV1,
            decltype(AnomalyNtePlayerTeleportServiceV1::teleport)>(
            service, offsetof(AnomalyNtePlayerTeleportServiceV1, teleport)) &&
        HasField<AnomalyNtePlayerTeleportServiceV1,
            decltype(AnomalyNtePlayerTeleportServiceV1::preload)>(
            service, offsetof(AnomalyNtePlayerTeleportServiceV1, preload)) &&
        service->teleport != nullptr && service->preload != nullptr;
}

bool SessionSnapshot(
    const AnomalyHostApiV1* host, AnomalyNteSessionSnapshotV1& snapshot) noexcept {
    const auto session = QueryService<AnomalyNteSessionServiceV1>(
        host, ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    if (!session ||
        !HasField<AnomalyNteSessionServiceV1,
            decltype(AnomalyNteSessionServiceV1::snapshot)>(
            session.service, offsetof(AnomalyNteSessionServiceV1, snapshot)) ||
        session.service->snapshot == nullptr) {
        return false;
    }
    snapshot = {sizeof(snapshot)};
    return session.service->snapshot(session.service->user, &snapshot).code ==
            ANOMALY_STATUS_V1_OK && IsCurrentWorld(snapshot);
}

bool PlayerSnapshot(
    const AnomalyHostApiV1* host, AnomalyNtePlayerSnapshotV1& snapshot) noexcept {
    const auto player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    if (!player ||
        !HasField<AnomalyNtePlayerServiceV1,
            decltype(AnomalyNtePlayerServiceV1::snapshot)>(
            player.service, offsetof(AnomalyNtePlayerServiceV1, snapshot)) ||
        player.service->snapshot == nullptr) {
        return false;
    }
    snapshot = {sizeof(snapshot)};
    return player.service->snapshot(player.service->user, &snapshot).code ==
            ANOMALY_STATUS_V1_OK && IsCurrentPlayer(snapshot);
}

bool TryReadCurrentPosition(const AnomalyHostApiV1* host, double position[3]) noexcept {
    AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
    if (!PlayerSnapshot(host, snapshot) || !IsFinitePosition(snapshot.position)) return false;
    std::copy(snapshot.position, snapshot.position + 3, position);
    return true;
}

void RecordResult(const AnomalyStatusV1 status) noexcept {
    std::scoped_lock lock(g_context.mutex);
    g_context.has_result = true;
    g_context.result_code = status.code;
    const std::size_t size = status.message.data == nullptr
        ? 0
        : (status.message.size < sizeof(g_context.result_message) - 1
               ? status.message.size
               : sizeof(g_context.result_message) - 1);
    if (size != 0) {
        std::memcpy(g_context.result_message, status.message.data, size);
    }
    g_context.result_message[size] = '\0';
}

// ---------------------------------------------------------------------------
// 设置持久化：状态目录由 anomaly.plugin-state 提供，文件写在插件自己的 state 目录下。
// ---------------------------------------------------------------------------
void ResolveSettingsPath() noexcept {
    g_context.settings_path.clear();
    const auto* state = g_context.plugin_state;
    if (state == nullptr || state->directory == nullptr) return;

    std::size_t bytes{};
    if (state->directory(state->user, nullptr, &bytes).code != ANOMALY_STATUS_V1_OK ||
        bytes < 2 || bytes > 4096) {
        return;
    }
    std::vector<char> directory(bytes, '\0');
    if (state->directory(state->user, directory.data(), &bytes).code != ANOMALY_STATUS_V1_OK) {
        return;
    }
    const auto terminator = std::find(directory.begin(), directory.end(), '\0');
    if (terminator == directory.begin()) return;
    const std::string text(directory.begin(), terminator);
    try {
        g_context.settings_path =
            std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(text.data()), text.size())) /
            std::filesystem::path(kSettingsFileName);
    } catch (...) {
        g_context.settings_path.clear();
    }
}

void ApplyTuningFields(
    Settings& settings, const double preload_delay, const double landing_lift,
    const std::uint32_t sink_retries) noexcept {
    if (std::isfinite(preload_delay) && preload_delay >= 0.0 &&
        preload_delay <= kPreloadDelayMaximum) {
        settings.preload_delay = preload_delay;
    }
    if (std::isfinite(landing_lift) && landing_lift >= 0.0 &&
        landing_lift <= kLandingLiftMaximum) {
        settings.landing_lift = landing_lift;
    }
    if (sink_retries <= kSinkRetriesMaximum) settings.sink_retries = sink_retries;
}

void LoadSettings() noexcept {
    Settings settings{};
    if (!g_context.settings_path.empty()) {
        std::ifstream input(g_context.settings_path, std::ios::binary);
        SettingsFileHeader header{};
        if (input.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
            header.magic == kSettingsMagic) {
            input.seekg(0);
            if (header.version == kSettingsVersionV1) {
                SettingsFileV1 file{};
                if (input.read(reinterpret_cast<char*>(&file), sizeof(file))) {
                    if (IsUsablePosition(file.target)) {
                        std::copy(file.target, file.target + 3, settings.fight_target);
                    }
                    ApplyTuningFields(settings, file.preload_delay, file.landing_lift,
                                      file.sink_retries);
                }
            } else if (header.version == kSettingsVersionV2) {
                SettingsFileV2 file{};
                if (input.read(reinterpret_cast<char*>(&file), sizeof(file))) {
                    if (IsUsablePosition(file.target)) {
                        std::copy(file.target, file.target + 3, settings.fight_target);
                    }
                    ApplyTuningFields(settings, file.preload_delay, file.landing_lift,
                                      file.sink_retries);
                }
            } else if (header.version == kSettingsVersion) {
                SettingsFileV3 file{};
                if (input.read(reinterpret_cast<char*>(&file), sizeof(file))) {
                    if (IsUsablePosition(file.camp_target)) {
                        std::copy(file.camp_target, file.camp_target + 3, settings.camp_target);
                    }
                    if (IsUsablePosition(file.fight_target)) {
                        std::copy(file.fight_target, file.fight_target + 3, settings.fight_target);
                    }
                    ApplyTuningFields(settings, file.preload_delay, file.landing_lift,
                                      file.sink_retries);
                    settings.loop_count = file.loop_count;
                }
            }
        }
    }
    std::scoped_lock lock(g_context.mutex);
    g_context.settings = settings;
}

bool SaveSettings() noexcept {
    if (g_context.settings_path.empty()) return false;
    Settings settings{};
    {
        std::scoped_lock lock(g_context.mutex);
        settings = g_context.settings;
    }

    SettingsFileV3 file{};
    file.magic = kSettingsMagic;
    file.version = kSettingsVersion;
    std::copy(settings.camp_target, settings.camp_target + 3, file.camp_target);
    std::copy(settings.fight_target, settings.fight_target + 3, file.fight_target);
    file.preload_delay = settings.preload_delay;
    file.landing_lift = settings.landing_lift;
    file.sink_retries = settings.sink_retries;
    file.loop_count = settings.loop_count;

    std::ofstream output(g_context.settings_path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    return output.good();
}

void SaveIfDue() noexcept {
    if (!g_settings_dirty.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_save) return;
    g_next_save = now + std::chrono::seconds(2);
    if (SaveSettings()) g_settings_dirty.store(false);
}

// ---------------------------------------------------------------------------
// 传送执行（只在 Game 回调域调用）
// ---------------------------------------------------------------------------
bool PreloadModeEnabled() noexcept {
    std::scoped_lock lock(g_context.mutex);
    return std::isfinite(g_context.settings.preload_delay) &&
        g_context.settings.preload_delay > 0.0;
}

AnomalyStatusV1 IssueTeleport(
    const AnomalyHostApiV1* host, const AnomalyGenerationHandleV1 world,
    const AnomalyGenerationHandleV1 player, const double position[3]) noexcept {
    const auto teleport = QueryService<AnomalyNtePlayerTeleportServiceV1>(
        host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
    if (!teleport || !TeleportServiceMethodsAvailable(teleport.service)) {
        return teleport ? StatusCode(ANOMALY_STATUS_V1_UNAVAILABLE) : teleport.status;
    }

    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    request.flags = PreloadModeEnabled()
        ? 0u
        : ANOMALY_NTE_PLAYER_TELEPORT_REQUEST_V1_IMMEDIATE;
    request.world = world;
    request.player = player;
    for (std::size_t axis = 0; axis != 3; ++axis) request.position[axis] = position[axis];
    return teleport.service->teleport(teleport.service->user, &request);
}

// 宿主自己的预载窗口：插件只提交目标点，流送由框架负责。失败不致命，退化成普通传送。
void ArmPreload(const PendingTeleport& request) noexcept {
    const auto teleport = QueryService<AnomalyNtePlayerTeleportServiceV1>(
        g_context.host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
    if (!teleport || !TeleportServiceMethodsAvailable(teleport.service)) return;

    double delay{};
    {
        std::scoped_lock lock(g_context.mutex);
        delay = g_context.settings.preload_delay;
    }
    if (!std::isfinite(delay) || delay <= 0.0) return;

    AnomalyNtePlayerTeleportPreloadRequestV1 preload{sizeof(preload)};
    preload.duration_milliseconds = static_cast<std::uint32_t>(delay * 1000.0);
    for (std::size_t axis = 0; axis != 3; ++axis) preload.position[axis] = request.position[axis];
    const AnomalyStatusV1 status =
        teleport.service->preload(teleport.service->user, &preload);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "night999 preload unavailable; teleporting without a streamed preload");
    }
}

void IssueRequest(const AnomalyHostApiV1* host, const PendingTeleport& request) noexcept {
    ArmPreload(request);

    const bool preloaded = PreloadModeEnabled();
    double landing_lift{};
    {
        std::scoped_lock lock(g_context.mutex);
        landing_lift = g_context.settings.landing_lift;
    }
    // 预载路径天然落在已加载地形上，抬升与沉底观察只属于立即传送路径。
    const bool apply_landing_lift = !preloaded && std::isfinite(landing_lift) && landing_lift > 0.0;

    std::array<double, 3> target{};
    std::copy(request.position, request.position + 3, target.begin());
    if (apply_landing_lift) target[2] += landing_lift;

    const AnomalyStatusV1 status = IssueTeleport(host, request.world, request.player, target.data());
    RecordResult(status);

    char detail[192]{};
    std::snprintf(
        detail, sizeof(detail), "night999 teleport %s target=(%.3f, %.3f, %.3f) code=%u",
        preloaded ? "preload" : "immediate", target[0], target[1], target[2],
        static_cast<unsigned>(status.code));
    Log(status.code == ANOMALY_STATUS_V1_OK ? ANOMALY_CORE_LOG_LEVEL_V1_INFO
                                            : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        detail);

    if (apply_landing_lift && status.code == ANOMALY_STATUS_V1_OK) {
        g_landing = {};
        g_landing.active = true;
        g_landing.world = request.world;
        g_landing.player = request.player;
        std::copy(request.position, request.position + 3, g_landing.base_position);
        g_landing.retries = 0;
        g_landing.settle_ticks = 0;
    }
}

// 指定坐标传送：快照校验 → 预载 → 传送，与界面按钮同一条路径。
// 返回 false 表示这一次没能发出去（世界未就绪或服务不可用），调用方可以重试。
bool TeleportToPosition(const AnomalyHostApiV1* host, const double position[3]) noexcept {
    if (host == nullptr || !IsFinitePosition(position)) return false;
    const auto teleport = QueryService<AnomalyNtePlayerTeleportServiceV1>(
        host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
    if (!teleport || !TeleportServiceMethodsAvailable(teleport.service)) return false;

    AnomalyNteSessionSnapshotV1 session{sizeof(session)};
    if (!SessionSnapshot(host, session)) return false;
    AnomalyNtePlayerSnapshotV1 player{sizeof(player)};
    if (!PlayerSnapshot(host, player)) return false;

    PendingTeleport request{};
    request.world = session.world;
    request.player = player.handle;
    std::copy(position, position + 3, request.position);
    IssueRequest(host, request);
    return true;
}

void ProcessLandingMonitor(const AnomalyHostApiV1* host) noexcept {
    if (!g_landing.active || host == nullptr) return;

    ++g_landing.settle_ticks;
    if (g_landing.settle_ticks < kLandingSettleTicks) return;
    if (g_landing.settle_ticks > kLandingWatchTicks) {
        g_landing = {};
        return;
    }

    double position[3]{};
    if (!TryReadCurrentPosition(host, position)) return;
    if (position[2] >= g_landing.base_position[2] - kSinkThreshold) {
        g_landing = {};
        return;
    }

    std::uint32_t sink_retries{kSinkRetriesDefault};
    double landing_lift{kLandingLiftDefault};
    {
        std::scoped_lock lock(g_context.mutex);
        sink_retries = g_context.settings.sink_retries;
        landing_lift = g_context.settings.landing_lift;
    }
    if (g_landing.retries >= sink_retries) {
        g_landing = {};
        return;
    }

    std::array<double, 3> retry{};
    std::copy(g_landing.base_position, g_landing.base_position + 3, retry.begin());
    retry[2] += (std::isfinite(landing_lift) ? landing_lift : kLandingLiftDefault) +
        static_cast<double>(g_landing.retries + 1U) * kSinkRetryLiftStep;
    const AnomalyStatusV1 status =
        IssueTeleport(host, g_landing.world, g_landing.player, retry.data());
    if (status.code != ANOMALY_STATUS_V1_OK) {
        g_landing = {};
        return;
    }
    ++g_landing.retries;
    g_landing.settle_ticks = 0;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
void DrawText(const AnomalyUiServiceV1* ui, const std::string_view text) {
    ui->text(ui->user, StringView(text));
}

double ParseNumber(const char* text) noexcept {
    if (text == nullptr || text[0] == '\0') return std::numeric_limits<double>::quiet_NaN();
    char* end{};
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0') return std::numeric_limits<double>::quiet_NaN();
    return value;
}

// ---------------------------------------------------------------------------
// 子模块：UE 反射直调（含一键刷新）→ 自动战斗 → 脚本循环
// ---------------------------------------------------------------------------
#include "ue_access.hpp"
#include "ue_direct.hpp"
#include "combat.hpp"
#include "script.hpp"

// 把界面上的输入框同步回设置。只有确实变化才标脏，避免每帧写盘。
void CommitInputs() noexcept {
    const double camp[3]{ParseNumber(g_camp_x_text.data()), ParseNumber(g_camp_y_text.data()),
                         ParseNumber(g_camp_z_text.data())};
    const double fight[3]{ParseNumber(g_fight_x_text.data()), ParseNumber(g_fight_y_text.data()),
                          ParseNumber(g_fight_z_text.data())};
    const double preload = ParseNumber(g_preload_text.data());

    bool dirty = false;
    {
        std::scoped_lock lock(g_context.mutex);
        if (IsUsablePosition(camp) && !SamePosition(camp, g_context.settings.camp_target)) {
            std::copy(camp, camp + 3, g_context.settings.camp_target);
            dirty = true;
        }
        if (IsUsablePosition(fight) && !SamePosition(fight, g_context.settings.fight_target)) {
            std::copy(fight, fight + 3, g_context.settings.fight_target);
            dirty = true;
        }
        if (std::isfinite(preload) && preload >= 0.0 && preload <= kPreloadDelayMaximum &&
            !SameValue(preload, g_context.settings.preload_delay)) {
            g_context.settings.preload_delay = preload;
            dirty = true;
        }
    }
    if (dirty) g_settings_dirty.store(true);
}

void SyncTextBuffers() {
    if (!g_text_stale.exchange(false)) return;
    Settings settings{};
    {
        std::scoped_lock lock(g_context.mutex);
        settings = g_context.settings;
    }
    std::snprintf(g_camp_x_text.data(), g_camp_x_text.size(), "%.3f", settings.camp_target[0]);
    std::snprintf(g_camp_y_text.data(), g_camp_y_text.size(), "%.3f", settings.camp_target[1]);
    std::snprintf(g_camp_z_text.data(), g_camp_z_text.size(), "%.3f", settings.camp_target[2]);
    std::snprintf(g_fight_x_text.data(), g_fight_x_text.size(), "%.3f", settings.fight_target[0]);
    std::snprintf(g_fight_y_text.data(), g_fight_y_text.size(), "%.3f", settings.fight_target[1]);
    std::snprintf(g_fight_z_text.data(), g_fight_z_text.size(), "%.3f", settings.fight_target[2]);
    std::snprintf(g_preload_text.data(), g_preload_text.size(), "%.1f", settings.preload_delay);
}

void LastTeleportWarning(const AnomalyUiServiceV1* ui) {
    bool has_result{};
    std::uint32_t result_code{};
    char result_message[sizeof(g_context.result_message)]{};
    {
        std::scoped_lock lock(g_context.mutex);
        has_result = g_context.has_result;
        result_code = g_context.result_code;
        std::memcpy(result_message, g_context.result_message, sizeof(result_message));
    }
    // 只报「失败」这一类关键信息；成功不必占地方。
    if (!has_result || result_code == ANOMALY_STATUS_V1_OK) return;
    std::string line = std::string("上次传送失败：") + StatusName(result_code);
    if (result_message[0] != '\0') line.append(" - ").append(result_message);
    DrawText(ui, line);
}

void PointInput(
    const AnomalyUiServiceV1* ui, const char* id, char* buffer, const std::size_t capacity) {
    if (ui->input_text != nullptr) {
        ui->input_text(ui->user, StringView(id), buffer, capacity,
                       ANOMALY_UI_TEXT_INPUT_V1_NONE);
    } else {
        DrawText(ui, buffer);
    }
}

// 两个传送点：营地 / 战斗点，各三个坐标。用四列表格排成紧凑的两行。
void PointRows(const AnomalyUiServiceV1* ui) {
    const auto fallback_row = [&](const char* label, const char* x_id, char* x_text,
                                  const char* y_id, char* y_text, const char* z_id,
                                  char* z_text, const std::size_t capacity) {
        ui->text(ui->user, StringView(label));
        if (ui->same_line != nullptr) ui->same_line(ui->user, 72.0F, 8.0F);
        PointInput(ui, x_id, x_text, capacity);
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        PointInput(ui, y_id, y_text, capacity);
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        PointInput(ui, z_id, z_text, capacity);
    };
    const bool table_ready = ui->begin_table != nullptr && ui->table_next_row != nullptr &&
        ui->table_next_column != nullptr && ui->end_table != nullptr &&
        ui->begin_table(ui->user, StringView("##night999-points"), 4, 0, 0.0F, 0.0F) != 0;
    if (!table_ready) {
        // 表格不可用（旧宿主）或创建失败：退化成逐行布局，功能一致。
        fallback_row("营地", "##night999-camp-x", g_camp_x_text.data(), "##night999-camp-y",
                     g_camp_y_text.data(), "##night999-camp-z", g_camp_z_text.data(),
                     g_camp_x_text.size());
        fallback_row("战斗点", "##night999-fight-x", g_fight_x_text.data(), "##night999-fight-y",
                     g_fight_y_text.data(), "##night999-fight-z", g_fight_z_text.data(),
                     g_fight_x_text.size());
        return;
    }

    ui->table_next_row(ui->user);
    DrawText(ui, "");
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, "X");
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, "Y");
    static_cast<void>(ui->table_next_column(ui->user));
    DrawText(ui, "Z");

    ui->table_next_row(ui->user);
    DrawText(ui, "营地");
    static_cast<void>(ui->table_next_column(ui->user));
    PointInput(ui, "##night999-camp-x", g_camp_x_text.data(), g_camp_x_text.size());
    static_cast<void>(ui->table_next_column(ui->user));
    PointInput(ui, "##night999-camp-y", g_camp_y_text.data(), g_camp_y_text.size());
    static_cast<void>(ui->table_next_column(ui->user));
    PointInput(ui, "##night999-camp-z", g_camp_z_text.data(), g_camp_z_text.size());

    ui->table_next_row(ui->user);
    DrawText(ui, "战斗点");
    static_cast<void>(ui->table_next_column(ui->user));
    PointInput(ui, "##night999-fight-x", g_fight_x_text.data(), g_fight_x_text.size());
    static_cast<void>(ui->table_next_column(ui->user));
    PointInput(ui, "##night999-fight-y", g_fight_y_text.data(), g_fight_y_text.size());
    static_cast<void>(ui->table_next_column(ui->user));
    PointInput(ui, "##night999-fight-z", g_fight_z_text.data(), g_fight_z_text.size());

    ui->end_table(ui->user);
}

void Content(const AnomalyHostApiV1* host, const AnomalyUiServiceV1* ui) {
    (void)host;
    SyncTextBuffers();

    // ── 脚本循环 ──
    DrawText(ui, g_context.localizer.Text("script.title", "脚本循环"));
    if (ui->input_uint32 != nullptr) {
        ui->text(ui->user, StringView("循环次数"));
        if (ui->same_line != nullptr) ui->same_line(ui->user, 72.0F, 8.0F);
        std::uint32_t count = g_script_loop_count.load();
        if (ui->input_uint32(ui->user, StringView("##night999-loop"), &count, 1, 10) != 0) {
            g_script_loop_count.store(count);
            {
                std::scoped_lock lock(g_context.mutex);
                g_context.settings.loop_count = count;
            }
            g_settings_dirty.store(true);
        }
        if (ui->same_line != nullptr) {
            ui->same_line(ui->user, 190.0F, 8.0F);
            DrawText(ui, "（0 = 无限循环）");
        }
    }
    if (ui->button != nullptr) {
        const bool running = g_script_running.load();
        const int start = ui->button_enabled != nullptr
            ? ui->button_enabled(ui->user, StringView("开始脚本"), 0.0F, 0.0F, running ? 0 : 1)
            : ui->button(ui->user, StringView("开始脚本"), 0.0F, 0.0F);
        if (start != 0) g_script_start_requested.store(true);
        if (ui->same_line != nullptr) ui->same_line(ui->user, 0.0F, 8.0F);
        const int stop = ui->button_enabled != nullptr
            ? ui->button_enabled(ui->user, StringView("停止脚本"), 0.0F, 0.0F, running ? 1 : 0)
            : ui->button(ui->user, StringView("停止脚本"), 0.0F, 0.0F);
        if (stop != 0) g_script_stop_requested.store(true);
    }
    DrawText(ui, ScriptStatusText());

    if (ui->separator != nullptr) ui->separator(ui->user);

    // ── 传送点 ──
    DrawText(ui, g_context.localizer.Text("points.title", "传送点"));
    PointRows(ui);
    ui->text(ui->user, StringView("预载延迟"));
    if (ui->same_line != nullptr) ui->same_line(ui->user, 72.0F, 8.0F);
    if (ui->input_text != nullptr) {
        ui->input_text(ui->user, StringView("##night999-preload"), g_preload_text.data(),
                       g_preload_text.size(), ANOMALY_UI_TEXT_INPUT_V1_NONE);
    } else {
        DrawText(ui, g_preload_text.data());
    }
    if (ui->same_line != nullptr) {
        ui->same_line(ui->user, 190.0F, 8.0F);
        DrawText(ui, "秒（0 = 立即传送）");
    }
    LastTeleportWarning(ui);

    if (ui->separator != nullptr) ui->separator(ui->user);

    // ── 自动战斗 ──
    CombatContent(ui);

    CommitInputs();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    g_context.host = host;
    g_context.localizer = Localizer(host);
    g_context.core = QueryService<AnomalyCoreServiceV1>(
        host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).service;
    g_context.plugin_state = QueryService<AnomalyPluginStateServiceV1>(
        host, ANOMALY_PLUGIN_STATE_SERVICE_V1_ID, ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION).service;
    g_context.names = QueryService<AnomalyUe5NamesServiceV1>(
        host, ANOMALY_UE5_NAMES_SERVICE_V1_ID, 2).service;
    g_context.objects = QueryService<AnomalyUe5ObjectsServiceV1>(
        host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
        ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION).service;
    g_context.world = QueryService<AnomalyUe5WorldServiceV1>(
        host, ANOMALY_UE5_WORLD_SERVICE_V1_ID,
        ANOMALY_UE5_WORLD_SERVICE_V1_VERSION).service;
    g_context.events = QueryService<AnomalyUe5ProcessEventServiceV1>(
        host, ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID,
        ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_VERSION).service;
    g_context.framework = QueryService<AnomalyUe5FrameworkServiceV1>(
        host, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID,
        ANOMALY_UE5_FRAMEWORK_SERVICE_V1_VERSION).service;
    g_context.session = QueryService<AnomalyNteSessionServiceV1>(
        host, ANOMALY_NTE_SESSION_SERVICE_V1_ID, ANOMALY_NTE_SESSION_SERVICE_V1_VERSION).service;
    g_context.player = QueryService<AnomalyNtePlayerServiceV1>(
        host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID, ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION).service;
    g_context.teleport = QueryService<AnomalyNtePlayerTeleportServiceV1>(
        host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION).service;
    g_context.navigation = QueryService<AnomalyNteNavigationServiceV1>(
        host, ANOMALY_NTE_NAVIGATION_SERVICE_V1_ID,
        ANOMALY_NTE_NAVIGATION_SERVICE_V1_VERSION).service;
    g_context.combat = QueryService<AnomalyNteCombatServiceV1>(
        host, ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION).service;
    g_context.skills = QueryService<AnomalyNteSkillsServiceV1>(
        host, ANOMALY_NTE_SKILLS_SERVICE_V1_ID, ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION).service;
    g_context.skill_invocation = QueryService<AnomalyNteSkillInvocationServiceV1>(
        host, ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION).service;
    g_context.actors = QueryService<AnomalyNteActorsServiceV1>(
        host, ANOMALY_NTE_ACTORS_SERVICE_V1_ID, ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION).service;
    g_context.entities = QueryService<AnomalyNteEntitiesServiceV1>(
        host, ANOMALY_NTE_ENTITIES_SERVICE_V1_ID, ANOMALY_NTE_ENTITIES_SERVICE_V1_VERSION).service;
    g_context.pickup = QueryService<AnomalyNtePickupServiceV1>(
        host, ANOMALY_NTE_PICKUP_SERVICE_V1_ID, ANOMALY_NTE_PICKUP_SERVICE_V1_VERSION).service;
    g_context.signature = QueryService<AnomalySignatureServiceV1>(
        host, ANOMALY_SIGNATURE_SERVICE_V1_ID, ANOMALY_SIGNATURE_SERVICE_V1_VERSION).service;
    g_context.settings_path.clear();
    {
        std::scoped_lock lock(g_context.mutex);
        g_context.settings = Settings{};
        g_context.has_result = false;
        g_context.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_context.result_message[0] = '\0';
    }
    *context = &g_context;
    return Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context == nullptr) return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    g_landing = {};
    g_next_save = {};
    g_text_stale.store(true);
    UeStop();
    CombatShutdown();
    ScriptShutdown();
    CombatPublish("自动战斗：未开始");
    ResolveSettingsPath();
    LoadSettings();
    g_settings_dirty.store(false);
    {
        std::scoped_lock lock(g_context.mutex);
        g_script_loop_count.store(g_context.settings.loop_count);
        g_context.has_result = false;
        g_context.result_code = ANOMALY_STATUS_V1_UNAVAILABLE;
        g_context.result_message[0] = '\0';
    }
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 script started");
    return Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    if (g_script_running.load()) {
        if (g_ue_refresh_stage.load() != 0) g_ue_refresh_stage.store(0);
        if (g_combat_active.load()) g_combat_stop_requested.store(true);
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "night999 script: stopped by lifecycle");
    }
    ScriptShutdown();
    UeStop();
    CombatShutdown();
    g_landing = {};
    try {
        static_cast<void>(SaveSettings());
        g_settings_dirty.store(false);
    } catch (...) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 settings could not be saved");
    }
    return Ok();
}

void ANOMALY_CALL Unload(void*) {}

void ANOMALY_CALL Update(void*, double) {
    try {
        const auto* host = g_context.host;
        if (host == nullptr) return;

        ProcessLandingMonitor(host);
        SaveIfDue();

        // 刷新 / 战斗 / 脚本循环都要在 Game 回调域推进。
        const bool game_thread = g_context.framework != nullptr &&
            g_context.framework->is_game_thread != nullptr &&
            g_context.framework->is_game_thread(g_context.framework->user) != 0;
        if (!game_thread) return;

        try {
            if (g_context.names != nullptr && g_context.core != nullptr) {
                const UeReader ue_reader{g_context.names};
                UeAdvance(ue_reader);
            }
        } catch (...) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 ue aborted");
        }
        try {
            AdvanceCombat();
        } catch (...) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 combat aborted");
        }
        try {
            AdvanceScript(host);
        } catch (...) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "night999 script aborted");
        }
    } catch (...) {
        RecordResult(StatusCode(ANOMALY_STATUS_V1_FAILED));
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    try {
        const auto* host = g_context.host;
        if (host == nullptr || ui == nullptr || ui->begin_window == nullptr ||
            ui->end_window == nullptr || ui->text == nullptr) {
            return;
        }
        int open = 1;
        const std::string title =
            g_context.localizer.Label("window.title", "999夜脚本", "night999");
        UiWindow window(ui, title, &open);
        if (!window) return;
        Content(host, ui);
    } catch (...) {
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return StatusCode(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("anomaly.local.nte.night999"),
        StringView("999夜脚本"), StringView("WawMew"), StringView("0.14.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
