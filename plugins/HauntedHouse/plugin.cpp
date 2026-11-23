#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// v2：活动物件点位按实测重排过 id，旧目录里的坐标会被当成别的位置，所以换一个 key，
// 让老配置自然失效（旧文件留在原处，不影响加载）。
constexpr std::string_view kSettingsSchemaId = "haunted-house-settings-v2";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 8192;

// 扫描：鬼屋物件是本局随机刷的，只能实时读 Actor。全图分页遍历，按类名前缀归到四个品类，
// 同一物件的父子 Actor 常差十几厘米，所以品类内 2 m 内合并。
constexpr std::size_t kMaximumScannedEntities = 32768;
constexpr std::size_t kMaximumScannedItems = 512;
constexpr double kItemDedupeCentimeters = 200.0;
constexpr double kCentimetersPerMeter = 100.0;

// 自动拾取：逐个传送过去，落地后点一次 F 交互按钮（游戏里它是名为 InteractButton 的 UI 按钮，
// 实体扫描永远扫不到它）。落地等待与重试窗口都按粉爪那套「等事件而不是死等固定时长」的思路：
// 传送调用是同步返回结果的，剩下的只是给 UI 目录与交互提示留出重试窗口。
constexpr std::uint64_t kAutoTeleportSettleMilliseconds = 250;
constexpr std::uint64_t kAutoClickWindowMilliseconds = 2500;
constexpr std::string_view kInteractButtonName = "InteractButton";
// 「交互-查看」之后要关掉的查看窗口，等效于按 ESC：它是 WB_CommonDeactivate 上的
// ButtonDeactivate，点它比等你按 ESC 更省事。
constexpr std::string_view kDeactivateButtonName = "ButtonDeactivate";
// 关窗按钮所属的 widget：名字对不上时，退一步在这个 widget 里找一个可点的按钮。
constexpr std::string_view kDeactivateWindowName = "WB_CommonDeactivate";
// 交互/关窗都在等「新一版 UI 目录」，而扫描是异步的：只扫一次很容易扫在窗口打开之前。
// 所以按固定间隔反复重扫，直到找到按钮或窗口超时。
constexpr std::uint64_t kCatalogRescanIntervalMilliseconds = 400;
// 查看窗口是交互之后才创建的：先等它弹出来，再去找关窗按钮；找不到就周期性重扫。
constexpr std::uint64_t kDeactivateDelayMilliseconds = 2000;
constexpr std::uint64_t kDeactivateWindowMilliseconds = 6000;
// 主线每步落地后多等一会儿再找交互提示（物品拾取可以短，主线要稳）：提示出现得慢，
// 太急就会点空、整步白丢。
constexpr std::uint64_t kMainTeleportSettleMilliseconds = 900;
constexpr std::uint64_t kMainInteractWindowMilliseconds = 4000;

enum class ScanState : std::uint32_t {
    idle = 0,
    ready = 1,
    unavailable = 2,
    failed = 3,
};

struct ScannedItem final {
    std::size_t kind{};
    double distance_meters{};
    std::uint64_t entity_id{};
    std::array<double, 3> position{};
};

// 一键完成主线的阶段：等落地 → 交互（点 F 按钮）→ 等一会儿 → 点关窗按钮。
enum class MainPhase : std::uint8_t {
    idle = 0,
    settle = 1,
    delay = 2,
    interact = 3,
    deactivate_delay = 4,
    deactivate = 5,
};

// 传送点。坐标是游戏世界坐标（UE 单位，厘米），属于游戏数据而不是文案；
// 默认值只在没有保存过配置时使用，之后一律以 config 里的值为准。
struct PointSpec final {
    std::string_view id;
    std::string_view name_key;
    std::string_view english_name;
    std::array<double, 3> default_position;
};

// 活动物件的四个品类。扫描时按类名前缀识别，一个实例一行。
struct ItemKind final {
    std::string_view key;
    std::string_view english;
    std::string_view class_prefix; // 游戏类名，数据不本地化
};

constexpr std::array<ItemKind, 4> kItemKinds{{
    {"item.poster", "Haunted flyer", "BP_HauntedHousePoster"},
    {"item.battery", "Spare battery", "BP_HauntedHouse_Battery"},
    {"item.spray", "Sanity spray", "BP_HauntedHouse_Spray"},
    {"item.ginger", "Gingerbread kid", "BP_GingerBreadMan"},
}};

// 一键完成主线：主线每一步到位后要做什么，顺序与 kPoints 里的主线点严格一一对应。
// interact：到位后点一次 F 交互按钮；deactivate：交互后点 ButtonDeactivate 关掉查看窗口
// （等效 ECS，点不到时退回等你手动按 ESC）；interact_delay_ms：交互前额外等多久。
struct StepAuto final {
    bool interact{};
    bool deactivate{};
    std::uint32_t interact_delay_ms{};
};

constexpr std::array<StepAuto, 18> kMainAutoSteps{{
    {},              // 1  前往古堡大厅（只传送）
    {},              // 2  探索古堡大厅（只传送）
    {true, true, 0}, // 3  探索右侧房间：交互-查看后点 ButtonDeactivate 关窗
    {true},          // 4  拿雪糕
    {true},          // 5  解开封印
    {true},          // 6  获取左侧诡乐糖果
    {true},          // 7  解开左侧房间封印
    {true},          // 8  第一颗宝石
    {true},          // 9  第二颗宝石
    {true},          // 10 第三颗宝石
    {true},          // 11 第四颗宝石
    {true},          // 12 提交宝石1
    {true},          // 13 提交宝石2
    {true},          // 14 提交宝石3
    {true},          // 15 提交宝石4
    {true, false, 10000}, // 16 获取糖果钥匙：落地后等 10 秒再交互
    {true},          // 17 物归原主
    {},              // 18 拜访伯爵先生（只传送）
}};

// 主线/支线的传送点表。活动物件不在这张表里：鬼屋的物件是随机刷点，硬编码只能碰运气，
// 所以四个品类改成运行时扫描（见 Scan）。
constexpr std::array<PointSpec, 18> kPoints{{
    {"gate-hall", "point.gate_hall", "Head to the castle hall",
     {-1838.0, 1708.0, -574.0}},
    {"explore-hall", "point.explore_hall", "Explore the castle hall",
     {-886.0, 2761.0, -104.0}},
    {"explore-right-room", "point.explore_right_room", "Explore the right room",
     {4231.0, 5891.0, 374.0}},
    {"snow-cone-pickup", "point.snow_cone_pickup", "Pick up ice cream",
     {-45.0, 4056.0, -573.0}},
    {"snow-cone-turn-in", "point.snow_cone_turn_in", "Break the seal",
     {3937.0, 5929.0, 372.0}},
    {"candy-claim", "point.candy_claim", "Pick up the left-room candy",
     {3944.0, 6388.0, 373.0}},
    {"candy-use", "point.candy_use", "Break the left-room seal",
     {105.0, 1034.0, -575.0}},
    {"gem-first", "point.gem_first", "First gem",
     {6156.0, -894.0, -575.0}},
    {"gem-second", "point.gem_second", "Second gem",
     {3276.0, -4989.0, -104.0}},
    {"gem-third", "point.gem_third", "Third gem",
     {4214.0, -2533.0, -575.0}},
    {"gem-fourth", "point.gem_fourth", "Fourth gem",
     {-1662.0, -2551.0, -575.0}},
    {"gem-turn-in-1", "point.gem_turn_in_1", "Hand in gem 1",
     {321.0, -1118.0, -575.0}},
    {"gem-turn-in-2", "point.gem_turn_in_2", "Hand in gem 2",
     {323.0, -1772.0, -575.0}},
    {"gem-turn-in-3", "point.gem_turn_in_3", "Hand in gem 3",
     {-150.0, -1751.0, -575.0}},
    {"gem-turn-in-4", "point.gem_turn_in_4", "Hand in gem 4",
     {-163.0, -1135.0, -575.0}},
    {"candy-key", "point.candy_key", "Pick up candy key",
     {73.0, -1351.0, -575.0}},
    {"candy-key-turn-in", "point.candy_key_turn_in", "Return the candy key",
     {2333.0, 1352.0, -104.0}},
    {"meet-count", "point.meet_count", "Visit the count",
     {7202.0, 1649.0, -1518.0}},

    // 支线流程：暂时没有点位，面板会显示空状态。加新支线点插在这一段即可，编号自动顺延。

}};

constexpr std::string_view kSettingsSchema = R"json(
{
  "type":"object",
  "additionalProperties":false,
  "required":["points"],
  "properties":{
    "points":{
      "type":"object",
      "additionalProperties":{
        "type":"array","items":{"type":"number"},"minItems":3,"maxItems":3
      }
    }
  }
}
)json";

enum class TeleportState : std::uint32_t {
    idle = 0,
    done = 1,
    not_ready = 2,
    unavailable = 3,
    failed = 4,
};

struct Context final {
    const AnomalyHostApiV1 *host{};
    anomaly::sdk::Localizer localizer;
    const AnomalyCoreServiceV1 *core{};
    const AnomalyConfigServiceV1 *config{};
    const AnomalyUiServiceV1 *ui{};
    AnomalyGenerationHandleV1 settings_schema{};

    std::array<std::array<std::atomic<double>, 3>, kPoints.size()> positions{};

    // 扫描：UI 线程只置标志，Game 域执行，结果用 mutex 交接。
    std::atomic_bool scan_requested{};
    // 物件列表只在用户点「扫描」时更新：打开面板不扫描，避免额外的整树遍历。
    std::atomic<std::uint32_t> scan_state{
        static_cast<std::uint32_t>(ScanState::idle)};
    std::atomic<std::uint32_t> scan_count{};
    std::mutex result_mutex;
    std::vector<ScannedItem> results;
    // “已拿”按实体 id 记：重扫后仍在的物件保持勾选，消失的自然不再出现。只存内存。
    // UI 线程写、Game 域读（自动拾取跳过已拿的点），所以要加锁。
    std::mutex collected_mutex;
    std::unordered_set<std::uint64_t> collected;

    // 自动拾取：UI 线程写起始品类与停止标志，Game 域跑逐点循环。
    std::atomic_bool auto_pick_requested{};
    std::atomic_bool auto_stop_requested{};
    std::atomic<std::uint32_t> auto_pick_kind{};
    std::atomic_bool auto_active{};
    std::atomic<std::uint32_t> auto_done{};
    std::atomic<std::uint32_t> auto_total{};
    std::vector<ScannedItem> auto_targets;
    std::size_t auto_cursor{};
    bool auto_clicking{};
    std::uint64_t auto_scan_deadline{};
    // 传送那一刻的 UI 目录序号：只有比它新的目录才是「落地之后」扫出来的。
    std::uint64_t auto_catalog_floor{};
    AnomalyGenerationHandleV1 auto_click_request{};
    std::uint64_t auto_deadline{};

    // 一键完成主线：按主线顺序跑一遍，逐点传送 + 交互（个别点等你按 ESC）。
    std::atomic_bool main_run_requested{};
    std::atomic_bool main_stop_requested{};
    std::atomic_bool main_active{};
    std::atomic<std::uint32_t> main_index{};   // 当前第几步（1 起）
    std::atomic<std::uint32_t> main_total{};
    std::size_t main_cursor{};
    MainPhase main_phase{MainPhase::idle};
    std::uint64_t main_deadline{};
    std::uint64_t main_scan_deadline{};
    std::uint64_t main_catalog_floor{};
    AnomalyGenerationHandleV1 main_click_request{};

    // 传送由 UI 线程排队、Game 域执行：宿主的 teleport 只接受 Game 回调域。
    std::mutex teleport_mutex;
    std::array<double, 3> teleport_target{};
    std::atomic_bool teleport_requested{};
    std::atomic<std::uint32_t> teleport_state{
        static_cast<std::uint32_t>(TeleportState::idle)};
};

template <typename Struct, typename Field>
bool HasField(const Struct *value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

AnomalyStatusV1 Status(const std::uint32_t code,
                       const std::string_view message = {}) noexcept {
    return {code, 0, {message.data(), message.size()}};
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

template <typename Service>
const Service *Query(const AnomalyHostApiV1 *host, const char *id,
                     const std::uint32_t version) noexcept {
    if (!HasField<AnomalyHostApiV1, decltype(AnomalyHostApiV1::query_service)>(
            host, offsetof(AnomalyHostApiV1, query_service)) ||
        host->query_service == nullptr) {
        return nullptr;
    }
    const void *table{};
    if (host->query_service(host->host_context, anomaly::sdk::StringView(id),
                            version, &table)
                .code != ANOMALY_STATUS_V1_OK ||
        table == nullptr) {
        return nullptr;
    }
    const auto *service = static_cast<const Service *>(table);
    constexpr std::size_t prefix = offsetof(Service, user) + sizeof(void *);
    return service->struct_size >= prefix && service->service_version >= version
               ? service
               : nullptr;
}

bool ConfigReady(const AnomalyConfigServiceV1 *service) noexcept {
    return HasField<AnomalyConfigServiceV1,
                    decltype(AnomalyConfigServiceV1::write_atomic)>(
               service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
           service->read != nullptr && service->write_atomic != nullptr &&
           service->register_schema != nullptr &&
           service->unregister_schema != nullptr;
}

bool UiReady(const AnomalyUiServiceV1 *service) noexcept {
    return HasField<AnomalyUiServiceV1,
                    decltype(AnomalyUiServiceV1::begin_table)>(
               service, offsetof(AnomalyUiServiceV1, begin_table)) &&
           service->begin_window != nullptr && service->end_window != nullptr &&
           service->set_next_window_size != nullptr &&
           service->text != nullptr && service->button != nullptr &&
           service->button_enabled != nullptr && service->separator != nullptr &&
           service->checkbox != nullptr && service->begin_table != nullptr &&
           service->table_next_row != nullptr &&
           service->table_next_column != nullptr &&
           service->end_table != nullptr;
}

// 标签页是追加式接口：老宿主没有它，面板退回“两页上下堆在一起”。
bool TabsReady(const AnomalyUiServiceV1 *service) noexcept {
    return HasField<AnomalyUiServiceV1,
                    decltype(AnomalyUiServiceV1::begin_tab_bar)>(
               service, offsetof(AnomalyUiServiceV1, begin_tab_bar)) &&
           service->begin_tab_bar != nullptr &&
           service->begin_tab_item != nullptr &&
           service->end_tab_item != nullptr && service->end_tab_bar != nullptr;
}

bool PlayerReady(const AnomalyNtePlayerServiceV1 *service) noexcept {
    return HasField<AnomalyNtePlayerServiceV1,
                    decltype(AnomalyNtePlayerServiceV1::snapshot)>(
               service, offsetof(AnomalyNtePlayerServiceV1, snapshot)) &&
           service->snapshot != nullptr;
}

bool SessionReady(const AnomalyNteSessionServiceV1 *service) noexcept {
    return HasField<AnomalyNteSessionServiceV1,
                    decltype(AnomalyNteSessionServiceV1::snapshot)>(
               service, offsetof(AnomalyNteSessionServiceV1, snapshot)) &&
           service->snapshot != nullptr;
}

bool TeleportReady(const AnomalyNtePlayerTeleportServiceV1 *service) noexcept {
    return HasField<AnomalyNtePlayerTeleportServiceV1,
                    decltype(AnomalyNtePlayerTeleportServiceV1::teleport)>(
               service, offsetof(AnomalyNtePlayerTeleportServiceV1, teleport)) &&
           service->teleport != nullptr;
}

bool ActorsReady(const AnomalyNteActorsServiceV1 *service) noexcept {
    return HasField<AnomalyNteActorsServiceV1,
                    decltype(AnomalyNteActorsServiceV1::fname_property_utf8)>(
               service,
               offsetof(AnomalyNteActorsServiceV1, fname_property_utf8)) &&
           service->frame != nullptr && service->page != nullptr &&
           service->class_name_utf8 != nullptr;
}

bool UiButtonsReady(const AnomalyNteUiButtonsServiceV1 *service) noexcept {
    return HasField<AnomalyNteUiButtonsServiceV1,
                    decltype(AnomalyNteUiButtonsServiceV1::request_scan)>(
               service, offsetof(AnomalyNteUiButtonsServiceV1, request_scan)) &&
           service->status != nullptr && service->find != nullptr &&
           service->request_scan != nullptr && service->request_click != nullptr;
}

bool CompleteSnapshot(const std::uint32_t flags) noexcept {
    return (flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0 &&
           (flags & ANOMALY_NTE_SNAPSHOT_V1_PARTIAL) == 0;
}

template <typename Resolve>
std::string ResolveUtf8(Resolve &&resolve) {
    std::size_t size{};
    if (resolve(nullptr, &size).code != ANOMALY_STATUS_V1_OK || size <= 1 ||
        size > 1024) {
        return {};
    }
    std::string value(size, '\0');
    if (resolve(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (const std::size_t end = value.find('\0'); end != std::string::npos) {
        value.resize(end);
    }
    return value;
}

bool MatchesItemKind(const std::string_view class_name, std::size_t &kind) noexcept {
    for (std::size_t index{}; index != kItemKinds.size(); ++index) {
        if (class_name.starts_with(kItemKinds[index].class_prefix)) {
            kind = index;
            return true;
        }
    }
    return false;
}

bool CurrentWorld(const AnomalyNteSessionSnapshotV1 &snapshot) noexcept {
    return snapshot.struct_size >= sizeof(snapshot) &&
           snapshot.state == ANOMALY_NTE_SESSION_V1_WORLD_READY &&
           snapshot.world.id != 0 && snapshot.world.generation != 0;
}

void Log(const Context &context, const std::uint32_t level,
         const std::string_view message) noexcept {
    if (context.core != nullptr && context.core->log != nullptr) {
        context.core->log(context.core->user, level,
                          anomaly::sdk::StringView(message));
    }
}

std::array<double, 3> LoadPosition(const Context &context,
                                   const std::size_t index) noexcept {
    std::array<double, 3> position{};
    for (std::size_t axis{}; axis != position.size(); ++axis) {
        position[axis] =
            context.positions[index][axis].load(std::memory_order_acquire);
    }
    return position;
}

void StorePosition(Context &context, const std::size_t index,
                   const std::array<double, 3> &position) noexcept {
    for (std::size_t axis{}; axis != position.size(); ++axis) {
        context.positions[index][axis].store(position[axis],
                                             std::memory_order_release);
    }
}

void ResetPositions(Context &context) noexcept {
    for (std::size_t index{}; index != kPoints.size(); ++index) {
        StorePosition(context, index, kPoints[index].default_position);
    }
}

bool PersistSettings(Context &context) noexcept {
    if (!ConfigReady(context.config)) return false;
    try {
        nlohmann::json points = nlohmann::json::object();
        for (std::size_t index{}; index != kPoints.size(); ++index) {
            const auto position = LoadPosition(context, index);
            points[std::string(kPoints[index].id)] =
                nlohmann::json::array({position[0], position[1], position[2]});
        }
        const std::string document =
            nlohmann::json{{"points", std::move(points)}}.dump();
        return context.config
                   ->write_atomic(context.config->user,
                                  anomaly::sdk::StringView(kSettingsSchemaId),
                                  kSettingsSchemaVersion, Bytes(document))
                   .code == ANOMALY_STATUS_V1_OK;
    } catch (...) {
        return false;
    }
}

bool ReadStoredPoint(const nlohmann::json &points, const std::string_view id,
                     std::array<double, 3> &position) {
    if (!points.is_object() || !points.contains(std::string(id))) return false;
    const auto &value = points.at(std::string(id));
    if (!value.is_array() || value.size() != 3) return false;
    for (std::size_t axis{}; axis != position.size(); ++axis) {
        if (!value[axis].is_number()) return false;
        const double coordinate = value[axis].get<double>();
        if (!std::isfinite(coordinate)) return false;
        position[axis] = coordinate;
    }
    return true;
}

bool LoadSettings(Context &context) noexcept {
    try {
        std::uint32_t version{};
        std::size_t size{};
        const auto status = context.config->read(
            context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
            &version, {nullptr, 0}, &size);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            ResetPositions(context);
            return PersistSettings(context);
        }
        if (status.code != ANOMALY_STATUS_V1_OK ||
            version != kSettingsSchemaVersion || size == 0 ||
            size > kMaximumSettingsBytes) {
            return false;
        }
        std::string document(size, '\0');
        std::size_t copied = size;
        if (context.config
                    ->read(context.config->user,
                           anomaly::sdk::StringView(kSettingsSchemaId), &version,
                           {reinterpret_cast<std::uint8_t *>(document.data()),
                            document.size()},
                           &copied)
                    .code != ANOMALY_STATUS_V1_OK ||
            copied == 0 || copied > document.size()) {
            return false;
        }
        const auto json =
            nlohmann::json::parse(document.begin(), document.begin() + copied);
        if (!json.is_object() || json.size() != 1 || !json.contains("points") ||
            !json.at("points").is_object()) {
            return false;
        }
        const auto &points = json.at("points");
        for (std::size_t index{}; index != kPoints.size(); ++index) {
            const PointSpec &point = kPoints[index];
            std::array<double, 3> position = point.default_position;
            static_cast<void>(ReadStoredPoint(points, point.id, position));
            StorePosition(context, index, position);
        }
        return true;
    } catch (...) {
        return false;
    }
}

void ReleaseSettingsSchema(Context &context) noexcept {
    if (context.settings_schema.id != 0 && ConfigReady(context.config)) {
        static_cast<void>(context.config->unregister_schema(
            context.config->user, context.settings_schema));
    }
    context.settings_schema = {};
}

// 扫描只在 Game 回调域内执行：Actor 快照与类名解析都属于该域。走 actors 表（全关卡）而不是
// entities 表，后者只扫持久关卡，流式子关卡里的鬼屋物件完全看不到。
void Scan(Context &context) {
    const auto set_state = [&context](const ScanState state) {
        context.scan_state.store(static_cast<std::uint32_t>(state),
                                 std::memory_order_release);
    };
    std::vector<ScannedItem> items;
    const auto publish = [&context, &items](const ScanState state) {
        const auto count = static_cast<std::uint32_t>(items.size());
        {
            std::scoped_lock lock(context.result_mutex);
            context.results = std::move(items);
        }
        context.scan_count.store(count, std::memory_order_release);
        context.scan_state.store(static_cast<std::uint32_t>(state),
                                 std::memory_order_release);
    };

    const auto actors = Query<AnomalyNteActorsServiceV1>(
        context.host, ANOMALY_NTE_ACTORS_SERVICE_V1_ID,
        ANOMALY_NTE_ACTORS_SERVICE_V1_VERSION);
    if (!ActorsReady(actors)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "haunted house: actor scan service is unavailable for the active profile");
        publish(ScanState::unavailable);
        return;
    }

    // 距离只用来排序与显示，物件可能在整张地图任何角落，所以不设半径。
    std::array<double, 3> player_position{};
    bool player_valid = false;
    const auto player = Query<AnomalyNtePlayerServiceV1>(
        context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    if (PlayerReady(player) &&
        player->snapshot(player->user, &player_snapshot).code ==
            ANOMALY_STATUS_V1_OK &&
        (player_snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) != 0) {
        std::ranges::copy(player_snapshot.position, player_position.begin());
        player_valid = std::ranges::all_of(player_position, [](const double value) {
            return std::isfinite(value);
        });
    }

    AnomalyNteEntityFrameV1 frame{sizeof(frame)};
    if (actors->frame(actors->user, &frame).code != ANOMALY_STATUS_V1_OK ||
        !CompleteSnapshot(frame.flags) || frame.generation == 0) {
        set_state(ScanState::unavailable);
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "haunted house: actor snapshot is not ready");
        publish(ScanState::unavailable);
        return;
    }
    if (frame.entity_count == 0) {
        publish(ScanState::ready);
        return;
    }

    struct Candidate final {
        std::size_t kind{};
        double distance_squared{};
        std::uint64_t entity_id{};
        std::array<double, 3> position{};
    };
    std::unordered_map<std::uint64_t, std::string> class_names;
    std::vector<Candidate> candidates;
    std::array<AnomalyNteEntitySnapshotV1,
               ANOMALY_NTE_ENTITY_PAGE_V1_MAX_CAPACITY>
        page{};
    std::uint32_t offset{};
    for (;;) {
        for (auto &snapshot : page) {
            snapshot = AnomalyNteEntitySnapshotV1{sizeof(snapshot)};
        }
        AnomalyNteEntityPageRequestV1 request{
            sizeof(request),        0, frame.generation, offset,
            static_cast<std::uint32_t>(page.size()),
            0,                      0, 0,               0,
            0};
        AnomalyNteEntityPageResultV1 result{sizeof(result)};
        if (actors->page(actors->user, &request, page.data(), &result).code !=
                ANOMALY_STATUS_V1_OK ||
            !CompleteSnapshot(result.flags) ||
            result.generation != frame.generation ||
            result.returned > page.size() ||
            offset > result.total_matches ||
            result.returned > result.total_matches - offset) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "haunted house: actor page could not be read");
            publish(ScanState::unavailable);
            return;
        }
        for (std::uint32_t index{}; index < result.returned; ++index) {
            const auto &snapshot = page[index];
            if (!CompleteSnapshot(snapshot.flags)) continue;
            auto [cached, inserted] = class_names.try_emplace(snapshot.class_id);
            if (inserted) {
                cached->second = ResolveUtf8(
                    [&](char *destination, std::size_t *size) {
                        return actors->class_name_utf8(actors->user,
                                                       snapshot.class_id,
                                                       destination, size);
                    });
            }
            std::size_t kind{};
            if (!MatchesItemKind(cached->second, kind)) continue;
            Candidate candidate;
            candidate.kind = kind;
            candidate.entity_id = snapshot.entity_id;
            std::ranges::copy(snapshot.bounds_center,
                              candidate.position.begin());
            candidate.distance_squared =
                player_valid ? [&] {
                    const double dx = candidate.position[0] - player_position[0];
                    const double dy = candidate.position[1] - player_position[1];
                    const double dz = candidate.position[2] - player_position[2];
                    return dx * dx + dy * dy + dz * dz;
                }()
                              : 0.0;
            if (!std::isfinite(candidate.distance_squared)) continue;
            candidates.push_back(std::move(candidate));
        }
        const std::uint32_t consumed = offset + result.returned;
        if (result.returned == 0 || consumed >= result.total_matches) break;
        if (result.next_offset != consumed) {
            publish(ScanState::unavailable);
            return;
        }
        offset = consumed;
        if (offset >= kMaximumScannedEntities) break;
    }

    // 同一物件的父/子 Actor 常差十几厘米，品类内 2 m 合并，保留离玩家最近的那条。
    std::ranges::sort(candidates, [](const Candidate &left, const Candidate &right) {
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.distance_squared < right.distance_squared;
    });
    const double dedupe_squared = kItemDedupeCentimeters * kItemDedupeCentimeters;
    items.reserve((std::min)(kMaximumScannedItems, candidates.size()));
    for (const auto &candidate : candidates) {
        if (items.size() >= kMaximumScannedItems) break;
        const bool duplicate = std::ranges::any_of(
            items, [&](const ScannedItem &kept) {
                if (kept.kind != candidate.kind) return false;
                const double dx = kept.position[0] - candidate.position[0];
                const double dy = kept.position[1] - candidate.position[1];
                const double dz = kept.position[2] - candidate.position[2];
                return dx * dx + dy * dy + dz * dz <= dedupe_squared;
            });
        if (duplicate) continue;
        ScannedItem item;
        item.kind = candidate.kind;
        item.distance_meters =
            std::sqrt(candidate.distance_squared) / kCentimetersPerMeter;
        item.entity_id = candidate.entity_id;
        item.position = candidate.position;
        items.push_back(item);
    }

    const auto count = items.size();
    // 先写日志再交接：publish 会把 items 移走，之后再遍历就是空的了。
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "haunted house scan: " + std::to_string(count) + " items");
    for (const auto &item : items) {
        char line[256]{};
        std::snprintf(line, sizeof(line), "haunted house item: %.1fm | %s | %.0f,%.0f,%.0f",
                      item.distance_meters,
                      std::string(kItemKinds[item.kind].class_prefix).c_str(),
                      item.position[0], item.position[1], item.position[2]);
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, line);
    }
    publish(ScanState::ready);
}

// 传送只在 Game 回调域内执行。flags = 0 是宿主的预载模式：先把流式源指向终点，
// 随即在同一调用里落位并短暂冻结，避免落到还没加载的地形上。
void ExecuteTeleport(Context &context) {
    std::array<double, 3> target{};
    {
        std::scoped_lock lock(context.teleport_mutex);
        target = context.teleport_target;
    }
    const auto set_state = [&context](const TeleportState state) {
        context.teleport_state.store(static_cast<std::uint32_t>(state),
                                     std::memory_order_release);
    };
    const auto session = Query<AnomalyNteSessionServiceV1>(
        context.host, ANOMALY_NTE_SESSION_SERVICE_V1_ID,
        ANOMALY_NTE_SESSION_SERVICE_V1_VERSION);
    AnomalyNteSessionSnapshotV1 session_snapshot{sizeof(session_snapshot)};
    if (!SessionReady(session) ||
        session->snapshot(session->user, &session_snapshot).code !=
            ANOMALY_STATUS_V1_OK) {
        set_state(TeleportState::unavailable);
        return;
    }
    if (!CurrentWorld(session_snapshot)) {
        set_state(TeleportState::not_ready);
        return;
    }
    const auto player = Query<AnomalyNtePlayerServiceV1>(
        context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    AnomalyNtePlayerSnapshotV1 player_snapshot{sizeof(player_snapshot)};
    if (!PlayerReady(player) ||
        player->snapshot(player->user, &player_snapshot).code !=
            ANOMALY_STATUS_V1_OK ||
        (player_snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) {
        set_state(TeleportState::not_ready);
        return;
    }
    const auto teleport = Query<AnomalyNtePlayerTeleportServiceV1>(
        context.host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
    if (!TeleportReady(teleport)) {
        set_state(TeleportState::unavailable);
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "haunted house: teleport service is unavailable for the active profile");
        return;
    }
    AnomalyNtePlayerTeleportRequestV1 request{sizeof(request)};
    request.flags = 0;
    request.world = session_snapshot.world;
    request.player = player_snapshot.handle;
    std::ranges::copy(target, request.position);
    const auto status = teleport->teleport(teleport->user, &request);
    set_state(status.code == ANOMALY_STATUS_V1_OK ? TeleportState::done
                                                  : TeleportState::failed);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "haunted house: teleport request failed");
    }
}

// ---- 自动拾取 ----------------------------------------------------------------
// 逐点：传送到物件 → 等落地 → 找到并点一次 F 交互按钮 → 等点击完成 → 下一个。

enum class ClickOutcome { sent, unavailable, retry };

// 点击结果：还等着 / 成功 / 失败。
enum class ClickResult { pending, ok, failed };

void TeleportTo(Context &context, const std::array<double, 3> &position) {
    {
        std::scoped_lock lock(context.teleport_mutex);
        context.teleport_target = position;
    }
    ExecuteTeleport(context);
}

std::uint64_t CatalogSequence(const Context &context) noexcept {
    const auto service = Query<AnomalyNteUiButtonsServiceV1>(
        context.host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
        ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION);
    if (!UiButtonsReady(service)) return 0;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) {
        return 0;
    }
    if ((status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) == 0) {
        return 0;
    }
    return status.catalog_sequence;
}

// 找按钮的条件。name 精确匹配；window 匹配按钮所在窗口/owner/root/path 任一段；
// category_mask 为 0 表示不限可点状态（点击本身带 FORCE，被遮挡/隐藏的也能试着点）。
struct ButtonLookup final {
    std::string_view name;
    std::string_view window;
    std::uint32_t category_mask{};
};

// 目录由 request_scan 异步发布。落地后必须拿到「传送之后」才发布的目录，否则 find 到的是
// 上一个点的交互提示，会点错对象——所以用 catalog_floor 卡住旧目录。
ClickOutcome ClickUiButton(Context &context, const ButtonLookup &lookup,
                           AnomalyGenerationHandleV1 &request,
                           const std::uint64_t catalog_floor,
                           std::uint64_t &scan_deadline) {
    const auto service = Query<AnomalyNteUiButtonsServiceV1>(
        context.host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
        ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION);
    if (!UiButtonsReady(service)) return ClickOutcome::unavailable;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) {
        return ClickOutcome::unavailable;
    }
    const bool fresh =
        (status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) != 0 &&
        status.catalog_sequence > catalog_floor;
    if (!fresh) {
        const auto now = GetTickCount64();
        if (now >= scan_deadline) {
            scan_deadline = now + kCatalogRescanIntervalMilliseconds;
            AnomalyGenerationHandleV1 scan_request{};
            static_cast<void>(service->request_scan(service->user, &scan_request));
        }
        return ClickOutcome::retry;
    }
    AnomalyNteUiButtonQueryV1 query{sizeof(query)};
    query.category_mask = lookup.category_mask;
    query.name = anomaly::sdk::StringView(lookup.name);
    query.window = anomaly::sdk::StringView(lookup.window);
    AnomalyNteUiButtonSnapshotV1 found{sizeof(found)};
    std::uint32_t matches{};
    if (service->find(service->user, &query, &found, &matches).code !=
            ANOMALY_STATUS_V1_OK ||
        matches == 0) {
        // 目录是新的，但里面还没有这个按钮：提示可能比目录晚一步才出现。所以这里也要按间隔
        // 重扫，否则只查手里这份旧目录，等到窗口超时也不会命中。
        const auto now = GetTickCount64();
        if (now >= scan_deadline) {
            scan_deadline = now + kCatalogRescanIntervalMilliseconds;
            AnomalyGenerationHandleV1 scan_request{};
            static_cast<void>(service->request_scan(service->user, &scan_request));
        }
        return ClickOutcome::retry;
    }
    AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
    click.flags = ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE;
    click.button = found.button;
    if (service->request_click(service->user, &click, &request).code !=
        ANOMALY_STATUS_V1_OK) {
        return ClickOutcome::retry;
    }
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        std::string("haunted house ui click: ") + std::string(found.name) +
            " | " + std::string(found.class_name) + " | cat " +
            std::to_string(found.category) + " | reasons " +
            std::to_string(found.reasons));
    return ClickOutcome::sent;
}

// 关窗按钮的挑选结果。目录里常有两个同名 ButtonDeactivate：owner 为空的实例点了必失败，
// 必须挑 WB_CommonDeactivate 里的那个，所以不能只靠 find 取第一个匹配。
enum class CloseLookup { found, absent, not_ready, unavailable };

struct CloseTarget final {
    AnomalyGenerationHandleV1 handle{};
    std::string name;
    std::string owner;
    std::uint32_t category{};
    std::uint32_t reasons{};
    std::uint32_t position{};
};

bool FreshCatalog(const AnomalyNteUiButtonsStatusV1 &status,
                  const std::uint64_t catalog_floor) noexcept {
    return (status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) != 0 &&
           status.catalog_sequence > catalog_floor;
}

CloseLookup SelectCloseButton(Context &context, CloseTarget &target,
                              const std::uint64_t catalog_floor,
                              std::uint32_t &catalog_size) {
    const auto service = Query<AnomalyNteUiButtonsServiceV1>(
        context.host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
        ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION);
    if (!UiButtonsReady(service)) return CloseLookup::unavailable;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK) {
        return CloseLookup::unavailable;
    }
    if (!FreshCatalog(status, catalog_floor)) return CloseLookup::not_ready;
    catalog_size = status.button_count;

    int best_rank{-1};
    AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
    for (std::uint32_t index{}; index < status.button_count; ++index) {
        if (service->button_at(service->user, status.catalog_sequence, index,
                               &button)
                .code != ANOMALY_STATUS_V1_OK) {
            return CloseLookup::unavailable;
        }
        const std::string_view name(button.name);
        const std::string_view owner(button.owner);
        const bool by_name = name == kDeactivateButtonName;
        const bool by_owner = owner == kDeactivateWindowName;
        const bool close_like = name.find("Close") != std::string_view::npos;
        if (!by_name && !by_owner && !close_like) continue;

        // 优先可点的实例，但同 widget 里的那个即使被判成隐藏/透明也照样点（FORCE），
        // 否则一旦游戏把它归类成不可点，这一步就永远关不掉窗。
        int rank{1};
        if (by_name && by_owner) rank = 40;
        else if (by_owner) rank = 30;
        else if (by_name) rank = 20;
        else if (close_like) rank = 10;
        if (by_owner) rank += 10; // 关窗 widget 里的实例优先于其他同名残留
        if (button.category == ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE) rank += 5;
        if (rank <= best_rank) continue;
        best_rank = rank;
        target.handle = button.button;
        target.name = button.name;
        target.owner = button.owner;
        target.category = button.category;
        target.reasons = button.reasons;
        target.position = index;
    }
    return best_rank >= 0 ? CloseLookup::found : CloseLookup::absent;
}

// 选好之后点它；FORCE 让被判为遮挡/透明的实例也能试。
ClickOutcome ClickSelected(Context &context, const CloseTarget &target,
                           AnomalyGenerationHandleV1 &request) {
    const auto service = Query<AnomalyNteUiButtonsServiceV1>(
        context.host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
        ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION);
    if (!UiButtonsReady(service)) return ClickOutcome::unavailable;
    AnomalyNteUiButtonClickRequestV1 click{sizeof(click)};
    click.flags = ANOMALY_NTE_UI_BUTTON_CLICK_V1_FORCE;
    click.button = target.handle;
    if (service->request_click(service->user, &click, &request).code !=
        ANOMALY_STATUS_V1_OK) {
        return ClickOutcome::retry;
    }
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "haunted house ui click: " + target.name + " | owner " + target.owner +
            " | cat " + std::to_string(target.category) + " | reasons " +
            std::to_string(target.reasons) + " | #" +
            std::to_string(target.position));
    return ClickOutcome::sent;
}

// 找不到关窗按钮时，把目录里名字像按钮的都记一遍，方便定位真实组件名。
void LogUiCandidates(Context &context) {
    const auto service = Query<AnomalyNteUiButtonsServiceV1>(
        context.host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
        ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION);
    if (!UiButtonsReady(service)) return;
    AnomalyNteUiButtonsStatusV1 status{sizeof(status)};
    if (service->status(service->user, &status).code != ANOMALY_STATUS_V1_OK ||
        (status.flags & ANOMALY_NTE_UI_BUTTONS_STATUS_V1_CATALOG) == 0) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "haunted house ui: no catalog while looking for the close button");
        return;
    }
    AnomalyNteUiButtonSnapshotV1 button{sizeof(button)};
    std::uint32_t dumped{};
    for (std::uint32_t index{}; index < status.button_count && dumped < 12;
         ++index) {
        if (service->button_at(service->user, status.catalog_sequence, index,
                               &button)
                .code != ANOMALY_STATUS_V1_OK) {
            return;
        }
        const std::string_view name(button.name);
        if (name.find("Deactivate") == std::string_view::npos &&
            name.find("Close") == std::string_view::npos &&
            name.find("Letter") == std::string_view::npos) {
            continue;
        }
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            std::string("haunted house ui candidate: ") + std::string(button.name) +
                " | " + std::string(button.class_name) + " | cat " +
                std::to_string(button.category) + " | reasons " +
                std::to_string(button.reasons) + " | win " +
                std::string(button.window) + " / " +
                std::string(button.owner));
        ++dumped;
    }
    if (dumped == 0) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "haunted house ui: no Deactivate/Close/Letter button in catalog of " +
                std::to_string(status.button_count) + " buttons");
    }
}

ClickOutcome ClickInteractButton(Context &context,
                                 AnomalyGenerationHandleV1 &request,
                                 const std::uint64_t catalog_floor,
                                 std::uint64_t &scan_deadline) {
    const ButtonLookup lookup{
        kInteractButtonName, {},
        ANOMALY_NTE_UI_BUTTON_QUERY_V1_CATEGORY(
            ANOMALY_NTE_UI_BUTTON_CATEGORY_V1_CLICKABLE)};
    return ClickUiButton(context, lookup, request, catalog_floor, scan_deadline);
}

ClickResult PollClick(Context &context, const AnomalyGenerationHandleV1 request) {
    const auto service = Query<AnomalyNteUiButtonsServiceV1>(
        context.host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
        ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION);
    if (!UiButtonsReady(service) || service->request_snapshot == nullptr) {
        return ClickResult::failed;
    }
    AnomalyNteUiButtonRequestSnapshotV1 snapshot{sizeof(snapshot)};
    if (service->request_snapshot(service->user, request, &snapshot).code !=
        ANOMALY_STATUS_V1_OK) {
        return ClickResult::failed;
    }
    if (snapshot.state != ANOMALY_NTE_UI_BUTTON_REQUEST_V1_COMPLETE) {
        return ClickResult::pending;
    }
    return snapshot.status == ANOMALY_STATUS_V1_OK ? ClickResult::ok
                                                   : ClickResult::failed;
}

void MarkCollected(Context &context, const std::uint64_t entity_id) {
    std::scoped_lock lock(context.collected_mutex);
    context.collected.insert(entity_id);
}

void StopAutoPick(Context &context) noexcept {
    context.auto_active.store(false, std::memory_order_release);
    context.auto_targets.clear();
    context.auto_cursor = 0;
    context.auto_clicking = false;
    context.auto_scan_deadline = 0;
    context.auto_catalog_floor = 0;
    context.auto_click_request = {};
    context.auto_deadline = 0;
}

void StartAutoPick(Context &context, const std::size_t kind) {
    const auto buttons = Query<AnomalyNteUiButtonsServiceV1>(
        context.host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
        ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION);
    if (!UiButtonsReady(buttons)) {
        // 没有 UI 按钮服务就只能逐个传送过去，F 交互得自己按。
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "haunted house auto pick: ui-buttons service is unavailable, teleport only");
    }
    std::vector<ScannedItem> targets;
    {
        std::scoped_lock lock(context.result_mutex);
        std::scoped_lock taken(context.collected_mutex);
        for (const auto &item : context.results) {
            if (item.kind != kind) continue;
            if (context.collected.contains(item.entity_id)) continue;
            targets.push_back(item);
        }
    }
    if (targets.empty()) {
        StopAutoPick(context);
        context.auto_done.store(0, std::memory_order_release);
        context.auto_total.store(0, std::memory_order_release);
        return;
    }
    context.auto_targets = std::move(targets);
    context.auto_cursor = 0;
    context.auto_clicking = false;
    context.auto_scan_deadline = 0;
    context.auto_click_request = {};
    context.auto_done.store(0, std::memory_order_release);
    context.auto_total.store(
        static_cast<std::uint32_t>(context.auto_targets.size()),
        std::memory_order_release);
    context.auto_active.store(true, std::memory_order_release);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "haunted house auto pick: start with " +
            std::to_string(context.auto_targets.size()) + " items");
    TeleportTo(context, context.auto_targets.front().position);
    // 先记下当前目录序号，之后只有更新的一版目录才算「落地之后」的 UI。
    context.auto_catalog_floor = CatalogSequence(context);
    context.auto_deadline = GetTickCount64() + kAutoTeleportSettleMilliseconds;
}

void StepAutoPick(Context &context) {
    if (context.auto_stop_requested.exchange(false, std::memory_order_acq_rel)) {
        StopAutoPick(context);
        return;
    }
    const auto now = GetTickCount64();
    if (!context.auto_clicking) {
        // 传送落地等待。
        if (now < context.auto_deadline) return;
        context.auto_clicking = true;
        context.auto_scan_deadline = 0;
        context.auto_click_request = {};
        context.auto_deadline = now + kAutoClickWindowMilliseconds;
        return;
    }

    bool advance = false;
    bool picked = false;
    if (context.auto_click_request.id != 0) {
        // 已经发出点击：等它完成，别急着传走把点击丢在空处。
        switch (PollClick(context, context.auto_click_request)) {
        case ClickResult::ok:
            picked = true;
            advance = true;
            break;
        case ClickResult::failed:
            advance = true;
            break;
        case ClickResult::pending:
            if (now >= context.auto_deadline) advance = true;
            else return;
            break;
        }
        if (advance) context.auto_click_request = {};
    } else {
        AnomalyGenerationHandleV1 request{};
        const auto outcome =
            ClickInteractButton(context, request, context.auto_catalog_floor,
                                context.auto_scan_deadline);
        if (outcome == ClickOutcome::sent) {
            context.auto_click_request = request;
            return; // 等这一下点完
        }
        if (outcome == ClickOutcome::unavailable) {
            advance = true; // 没有 UI 按钮服务：只能当作纯传送
        } else if (now >= context.auto_deadline) {
            advance = true; // 窗口内一直没等到交互提示：跳过这个点
        } else {
            return; // 目录/提示还没就绪，下个 tick 再试
        }
    }
    if (!advance) return;

    if (picked) {
        MarkCollected(context, context.auto_targets[context.auto_cursor].entity_id);
    } else {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "haunted house auto pick: no interact prompt at item " +
                std::to_string(context.auto_cursor));
    }
    context.auto_done.fetch_add(1, std::memory_order_acq_rel);
    ++context.auto_cursor;
    context.auto_clicking = false;
    if (context.auto_cursor >= context.auto_targets.size()) {
        StopAutoPick(context);
        // 收尾再扫一次：真被拿走的物件会从列表里消失。
        context.scan_requested.store(true, std::memory_order_release);
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "haunted house auto pick: done");
        return;
    }
    TeleportTo(context, context.auto_targets[context.auto_cursor].position);
    // 目录序号在传送前取：之后只有更新的一版目录才可能是落地后的 UI。
    context.auto_catalog_floor = CatalogSequence(context);
    context.auto_deadline = now + kAutoTeleportSettleMilliseconds;
}

// ---- 一键完成主线 ----------------------------------------------------------------
// 按主线顺序逐点：传送 → 等落地 →（可选）等一会儿 → 点一次交互 →（可选）等你按 ESC
// → 下一个点。主线坐标来自 kPoints（可被配置覆盖），行为来自 kMainAutoSteps。


// 第 ordinal 个主线点（0 起，按 kPoints 顺序）。表里现在只有主线点，所以就是下标本身。
bool MainPointIndex(const std::size_t ordinal, std::size_t &index) noexcept {
    if (ordinal >= kPoints.size()) return false;
    index = ordinal;
    return true;
}

StepAuto MainStepRule(const std::size_t ordinal) noexcept {
    return ordinal < kMainAutoSteps.size() ? kMainAutoSteps[ordinal] : StepAuto{};
}

void StopMainRun(Context &context) noexcept {
    context.main_active.store(false, std::memory_order_release);
    context.main_phase = MainPhase::idle;
    context.main_cursor = 0;
    context.main_click_request = {};
    context.main_scan_deadline = 0;
    context.main_catalog_floor = 0;
    context.main_deadline = 0;
}

void StartMainRun(Context &context) {
    std::size_t index{};
    if (!MainPointIndex(0, index)) {
        StopMainRun(context);
        return;
    }
    StopMainRun(context);
    context.main_index.store(1, std::memory_order_release);
    context.main_total.store(static_cast<std::uint32_t>(kPoints.size()),
                             std::memory_order_release);
    context.main_active.store(true, std::memory_order_release);
    context.main_phase = MainPhase::settle;
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
        "haunted house main run: start");
    TeleportTo(context, LoadPosition(context, index));
    context.main_catalog_floor = CatalogSequence(context);
    context.main_deadline = GetTickCount64() + kMainTeleportSettleMilliseconds;
}

// 推进一步：换下一个点；到末尾就结束。
void AdvanceMainRun(Context &context) {
    ++context.main_cursor;
    std::size_t index{};
    if (!MainPointIndex(context.main_cursor, index)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "haunted house main run: done");
        StopMainRun(context);
        return;
    }
    context.main_index.store(
        static_cast<std::uint32_t>(context.main_cursor + 1),
        std::memory_order_release);
    context.main_phase = MainPhase::settle;
    context.main_click_request = {};
    context.main_scan_deadline = 0;
    TeleportTo(context, LoadPosition(context, index));
    context.main_catalog_floor = CatalogSequence(context);
    context.main_deadline = GetTickCount64() + kMainTeleportSettleMilliseconds;
}

void BeginMainInteract(Context &context, const std::uint64_t now) {
    context.main_phase = MainPhase::interact;
    context.main_click_request = {};
    context.main_scan_deadline = 0;
    context.main_deadline = now + kMainInteractWindowMilliseconds;
}

void StepMainRun(Context &context) {
    if (context.main_stop_requested.exchange(false, std::memory_order_acq_rel)) {
        Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "haunted house main run: stopped at step " +
                std::to_string(context.main_index.load(std::memory_order_acquire)));
        StopMainRun(context);
        return;
    }
    const auto now = GetTickCount64();
    const StepAuto step = MainStepRule(context.main_cursor);
    switch (context.main_phase) {
    case MainPhase::settle:
        if (now < context.main_deadline) return;
        if (!step.interact) {
            AdvanceMainRun(context);
            return;
        }
        if (step.interact_delay_ms != 0) {
            context.main_phase = MainPhase::delay;
            context.main_deadline = now + step.interact_delay_ms;
            return;
        }
        BeginMainInteract(context, now);
        return;
    case MainPhase::delay:
        if (now < context.main_deadline) return;
        BeginMainInteract(context, now);
        return;
    case MainPhase::interact: {
        bool done = false;
        bool picked = false;
        if (context.main_click_request.id != 0) {
            switch (PollClick(context, context.main_click_request)) {
            case ClickResult::ok:
                picked = true;
                done = true;
                break;
            case ClickResult::failed:
                // 点空了（提示还没出现在这个位置）：换一版目录再来，直到窗口结束。
                // 之前这里直接判 done，于是落地 0.25 秒就「点一下就走」，整步白丢。
                context.main_click_request = {};
                context.main_scan_deadline = 0;
                if (now >= context.main_deadline) done = true;
                else return;
                break;
            case ClickResult::pending:
                if (now < context.main_deadline) return;
                done = true;
                break;
            }
        } else {
            AnomalyGenerationHandleV1 request{};
            const auto outcome = ClickInteractButton(
                context, request, context.main_catalog_floor,
                context.main_scan_deadline);
            if (outcome == ClickOutcome::sent) {
                context.main_click_request = request;
                return;
            }
            if (outcome == ClickOutcome::unavailable || now >= context.main_deadline) {
                done = true;
            } else {
                return; // 目录/提示还没就绪
            }
        }
        if (!done) return;
        context.main_click_request = {};
        if (!picked) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "haunted house main run: no interact prompt at step " +
                    std::to_string(context.main_index.load(
                        std::memory_order_acquire)));
        }
        if (!step.deactivate) {
            AdvanceMainRun(context);
            return;
        }
        // 交互-查看会弹出查看窗口，点 ButtonDeactivate 关掉（等效 ESC）。窗口要等一会儿才
        // 出现，所以先给一段延时再进关窗阶段；关窗阶段里会周期性重扫 UI 目录。
        context.main_phase = MainPhase::deactivate_delay;
        context.main_click_request = {};
        context.main_scan_deadline = 0;
        context.main_deadline = now + kDeactivateDelayMilliseconds;
        return;
    }
    case MainPhase::deactivate_delay:
        if (now < context.main_deadline) return;
        context.main_phase = MainPhase::deactivate;
        context.main_scan_deadline = 0;
        context.main_deadline = now + kDeactivateWindowMilliseconds;
        return;
    case MainPhase::deactivate: {
        bool done = false;
        bool closed = false;
        if (context.main_click_request.id != 0) {
            switch (PollClick(context, context.main_click_request)) {
            case ClickResult::ok:
                closed = true;
                done = true;
                break;
            case ClickResult::failed:
                done = true;
                break;
            case ClickResult::pending:
                if (now < context.main_deadline) return;
                done = true;
                break;
            }
        } else {
            CloseTarget target;
            std::uint32_t catalog_size{};
            const auto lookup = SelectCloseButton(
                context, target, context.main_catalog_floor, catalog_size);
            if (lookup == CloseLookup::found) {
                AnomalyGenerationHandleV1 request{};
                // 同名按钮有多个实例，必挑 owner 匹配的那个，否则点击会被游戏丢弃。
                if (ClickSelected(context, target, request) == ClickOutcome::sent) {
                    context.main_click_request = request;
                    return;
                }
                done = true;
            } else if (lookup == CloseLookup::not_ready) {
                const auto tick = GetTickCount64();
                if (tick >= context.main_scan_deadline) {
                    context.main_scan_deadline =
                        tick + kCatalogRescanIntervalMilliseconds;
                    const auto service = Query<AnomalyNteUiButtonsServiceV1>(
                        context.host, ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_ID,
                        ANOMALY_NTE_UI_BUTTONS_SERVICE_V1_VERSION);
                    if (UiButtonsReady(service)) {
                        AnomalyGenerationHandleV1 scan_request{};
                        static_cast<void>(
                            service->request_scan(service->user, &scan_request));
                    }
                }
                if (now < context.main_deadline) return;
                done = true;
            } else {
                // absent：目录是新的但没有关窗按钮 → 查看窗口没开（比如这封信已经看过）。
                // unavailable：服务不可用。两种都不该把流程卡住。
                done = true;
                if (lookup == CloseLookup::absent) LogUiCandidates(context);
                Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
                    "haunted house main run: no close button (" +
                        std::to_string(catalog_size) + " buttons, " +
                        (lookup == CloseLookup::absent ? "absent" : "unavailable") +
                        "), continue");
            }
        }
        if (!done) return;
        context.main_click_request = {};
        if (!closed) {
            Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                "haunted house main run: deactivate click rejected, continue");
        }
        // 点成功、被游戏丢弃、或窗口本来就不在：都往下走，不再停在这一步。
        AdvanceMainRun(context);
        return;
    }
    case MainPhase::idle:
    default:
        StopMainRun(context);
        return;
    }
}

void ANOMALY_CALL Update(void *plugin_context, const double) {
    auto *context = static_cast<Context *>(plugin_context);
    if (context == nullptr) return;
    try {
        if (context->scan_requested.exchange(false, std::memory_order_acq_rel)) {
            Scan(*context);
        }
        if (context->teleport_requested.exchange(false, std::memory_order_acq_rel)) {
            ExecuteTeleport(*context);
        }
        if (context->auto_pick_requested.exchange(false, std::memory_order_acq_rel)) {
            StartAutoPick(*context,
                          context->auto_pick_kind.load(std::memory_order_acquire));
        }
        if (context->auto_active.load(std::memory_order_acquire)) {
            StepAutoPick(*context);
        }
        if (context->main_run_requested.exchange(false, std::memory_order_acq_rel)) {
            StartMainRun(*context);
        }
        if (context->main_active.load(std::memory_order_acquire)) {
            StepMainRun(*context);
        }
    } catch (...) {
        context->teleport_state.store(
            static_cast<std::uint32_t>(TeleportState::failed),
            std::memory_order_release);
        context->scan_state.store(static_cast<std::uint32_t>(ScanState::failed),
                                  std::memory_order_release);
    }
}

std::string LocalizedStatus(const Context &context) {
    switch (static_cast<TeleportState>(
        context.teleport_state.load(std::memory_order_acquire))) {
    case TeleportState::done:
        return context.localizer.Text("status.teleport_done",
                                      "Teleported to the target position.");
    case TeleportState::not_ready:
        return context.localizer.Text("status.teleport_not_ready",
                                      "The world is not ready yet. Try again in a scene.");
    case TeleportState::unavailable:
        return context.localizer.Text(
            "status.teleport_unavailable",
            "Teleport is unavailable for the current profile.");
    case TeleportState::failed:
        return context.localizer.Text("status.teleport_failed",
                                      "Teleport failed. Check the runtime log.");
    case TeleportState::idle:
    default:
        return context.localizer.Text("status.idle", "Nothing teleported yet.");
    }
}

std::string LocalizedPlayerPosition(const Context &context) {
    const auto player = Query<AnomalyNtePlayerServiceV1>(
        context.host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID,
        ANOMALY_NTE_PLAYER_SERVICE_V1_VERSION);
    AnomalyNtePlayerSnapshotV1 snapshot{sizeof(snapshot)};
    if (!PlayerReady(player) ||
        player->snapshot(player->user, &snapshot).code != ANOMALY_STATUS_V1_OK ||
        (snapshot.flags & ANOMALY_NTE_SNAPSHOT_V1_VALID) == 0) {
        return context.localizer.Text("status.position_unavailable",
                                      "Current position is unavailable.");
    }
    const auto format_axis = [](const double value) {
        std::string text = std::to_string(static_cast<long long>(std::llround(value)));
        return text;
    };
    // Format 的参数是 string_view，文本必须活到调用结束。
    const std::array<std::string, 3> parts{format_axis(snapshot.position[0]),
                                           format_axis(snapshot.position[1]),
                                           format_axis(snapshot.position[2])};
    const std::array<std::string_view, 3> arguments{parts[0], parts[1], parts[2]};
    return context.localizer.Format("status.position",
                                    "Current position: {0}, {1}, {2}",
                                    arguments);
}

// 传送目标在点击时就固定下来：主线/支线与扫描结果共用一个排队槽，必须当场取值。
void RequestTeleport(Context &context,
                     const std::array<double, 3> &target) noexcept {
    {
        std::scoped_lock lock(context.teleport_mutex);
        context.teleport_target = target;
    }
    context.teleport_state.store(static_cast<std::uint32_t>(TeleportState::idle),
                                 std::memory_order_release);
    context.teleport_requested.store(true, std::memory_order_release);
}

void RequestTeleport(Context &context, const std::size_t index) noexcept {
    RequestTeleport(context, LoadPosition(context, index));
}

// 扫描结果用实体 id 做按钮身份，同一个物件的按钮在重扫之间保持稳定。
void TeleportCellAt(const AnomalyUiServiceV1 *ui, Context &context,
                    const std::array<double, 3> &position,
                    const bool can_teleport, const std::string_view label,
                    const std::uint64_t identity) {
    std::string text(label);
    text.append("###haunted-house-teleport-scan-")
        .append(std::to_string(identity));
    if (ui->button_enabled(ui->user, anomaly::sdk::StringView(text), 0.0F, 0.0F,
                           can_teleport ? 1 : 0) != 0) {
        RequestTeleport(context, position);
    }
}

void TeleportCell(const AnomalyUiServiceV1 *ui, Context &context,
                  const std::size_t index, const bool can_teleport,
                  const std::string_view label) {
    std::string text(label);
    text.append("###haunted-house-teleport-").append(std::to_string(index));
    if (ui->button_enabled(ui->user, anomaly::sdk::StringView(text), 0.0F, 0.0F,
                           can_teleport ? 1 : 0) != 0) {
        RequestTeleport(context, index);
    }
}

std::string LocalizedScanStatus(Context &context) {
    switch (static_cast<ScanState>(
        context.scan_state.load(std::memory_order_acquire))) {
    case ScanState::ready: {
        const std::string count =
            std::to_string(context.scan_count.load(std::memory_order_acquire));
        const std::array<std::string_view, 1> arguments{count};
        return context.localizer.Format("status.scan_ready",
                                        "Found {0} items.", arguments);
    }
    case ScanState::unavailable:
        return context.localizer.Text(
            "status.scan_unavailable",
            "Scan is unavailable for the current profile.");
    case ScanState::failed:
        return context.localizer.Text("status.scan_failed",
                                      "Scan failed. Check the runtime log.");
    case ScanState::idle:
    default:
        return context.localizer.Text(
            "status.scan_idle",
            "Not scanned yet. Press Scan to list the items in the current run.");
    }
}

// 主线的有序列表。编号按表内顺序现算，所以调整顺序只要挪表项。
void DrawMainList(const AnomalyUiServiceV1 *ui, Context &context,
                  const bool can_teleport, const std::string_view table_id) {
    const std::array<std::string, 2> headers{
        context.localizer.Text("column.step", "Step"),
        context.localizer.Text("column.action", "Action")};
    const std::string teleport_label =
        context.localizer.Text("action.teleport", "Teleport");
    if (ui->begin_table(ui->user, anomaly::sdk::StringView(table_id), 2, 0, 0.0F,
                        0.0F) == 0) {
        return;
    }
    ui->table_next_row(ui->user);
    for (const auto &header : headers) {
        static_cast<void>(ui->table_next_column(ui->user));
        ui->text(ui->user, anomaly::sdk::StringView(header));
    }
    std::size_t ordinal{};
    for (std::size_t index{}; index != kPoints.size(); ++index) {
        const PointSpec &point = kPoints[index];
        ++ordinal;
        ui->table_next_row(ui->user);
        static_cast<void>(ui->table_next_column(ui->user));
        const std::string name =
            context.localizer.Text(point.name_key, point.english_name);
        const std::string label = std::to_string(ordinal) + ". " + name;
        ui->text(ui->user, anomaly::sdk::StringView(label));
        static_cast<void>(ui->table_next_column(ui->user));
        TeleportCell(ui, context, index, can_teleport, teleport_label);
    }
    ui->end_table(ui->user);
}

// 第一页：主线流程。顶部是一键完成主线控制条，下面是有序列表。
// 主线最后一个点（BOSS 房）在 kPoints 里的下标：坐标跟着主线第 18 步走，配置改了它也变。
bool LastMainPointIndex(std::size_t &index) noexcept {
    if (kPoints.empty()) return false;
    index = kPoints.size() - 1;
    return true;
}

void DrawMainPage(const AnomalyUiServiceV1 *ui, Context &context,
                  const bool can_teleport) {
    const bool running = context.main_active.load(std::memory_order_acquire);
    if (!running) {
        const std::string run_label = context.localizer.Label(
            "action.run_all", "Run all", "haunted-house-run-all");
        if (ui->button(ui->user, anomaly::sdk::StringView(run_label), 0.0F, 0.0F) !=
            0) {
            context.main_run_requested.store(true, std::memory_order_release);
        }
        // 右边跟一个一步到 BOSS 房的快捷按钮；自动化跑着时这个按钮不出现，免得抢传送槽。
        std::size_t boss_index{};
        if (LastMainPointIndex(boss_index)) {
            if (ui->same_line != nullptr) {
                ui->same_line(ui->user, 0.0F, 8.0F);
            }
            const std::string boss_label = context.localizer.Label(
                "action.to_boss", "Teleport to the boss room",
                "haunted-house-boss-room");
            if (ui->button_enabled(ui->user, anomaly::sdk::StringView(boss_label),
                                   0.0F, 0.0F,
                                   can_teleport ? 1 : 0) != 0) {
                RequestTeleport(context, boss_index);
            }
        }
    } else {
        const std::string stop_label = context.localizer.Label(
            "action.stop", "Stop", "haunted-house-main-stop");
        if (ui->button(ui->user, anomaly::sdk::StringView(stop_label), 0.0F,
                       0.0F) != 0) {
            context.main_stop_requested.store(true, std::memory_order_release);
        }
        if (ui->same_line != nullptr) {
            ui->same_line(ui->user, 0.0F, 8.0F);
        }
        const std::string index =
            std::to_string(context.main_index.load(std::memory_order_acquire));
        const std::string total =
            std::to_string(context.main_total.load(std::memory_order_acquire));
        const std::array<std::string_view, 2> progress_arguments{index, total};
        const std::string progress = context.localizer.Format(
            "status.main_progress", "Step {0}/{1}", progress_arguments);
        ui->text(ui->user, anomaly::sdk::StringView(progress));
    }
    DrawMainList(ui, context, can_teleport, "haunted-house-main");
}

// 活动物件的每一品类各占一页。内容是扫描结果，不是固定表：物件本局刷在哪就只能扫到哪。
void DrawItemPage(const AnomalyUiServiceV1 *ui, Context &context,
                  const bool can_teleport, const std::size_t kind) {
    const std::array<std::string, 4> headers{
        context.localizer.Text("column.index", "No."),
        context.localizer.Text("column.distance", "Distance"),
        context.localizer.Text("column.collected", "Taken"),
        context.localizer.Text("column.action", "Action")};
    const std::string teleport_label =
        context.localizer.Text("action.teleport", "Teleport");

    // 第一行：扫描 + 上次扫描结果。
    const std::string scan_label = context.localizer.Label(
        "action.scan", "Scan", "haunted-house-scan");
    if (ui->button(ui->user, anomaly::sdk::StringView(scan_label), 0.0F, 0.0F) !=
        0) {
        context.scan_requested.store(true, std::memory_order_release);
    }
    if (ui->same_line != nullptr) {
        ui->same_line(ui->user, 0.0F, 8.0F);
    }
    const std::string scan_status = LocalizedScanStatus(context);
    ui->text(ui->user, anomaly::sdk::StringView(scan_status));

    // 第二行：自动拾取（运行中换成停止 + 进度）。
    const bool auto_active = context.auto_active.load(std::memory_order_acquire);
    if (!auto_active) {
        const std::string auto_label = context.localizer.Label(
            "action.auto_pick", "Auto pick", "haunted-house-auto-pick");
        if (ui->button(ui->user, anomaly::sdk::StringView(auto_label), 0.0F,
                       0.0F) != 0) {
            context.auto_pick_kind.store(static_cast<std::uint32_t>(kind),
                                         std::memory_order_release);
            context.auto_pick_requested.store(true, std::memory_order_release);
        }
    } else {
        const std::string stop_label = context.localizer.Label(
            "action.stop", "Stop", "haunted-house-auto-stop");
        if (ui->button(ui->user, anomaly::sdk::StringView(stop_label), 0.0F,
                       0.0F) != 0) {
            context.auto_stop_requested.store(true, std::memory_order_release);
        }
        if (ui->same_line != nullptr) {
            ui->same_line(ui->user, 0.0F, 8.0F);
        }
        const std::string done =
            std::to_string(context.auto_done.load(std::memory_order_acquire));
        const std::string total =
            std::to_string(context.auto_total.load(std::memory_order_acquire));
        const std::array<std::string_view, 2> arguments{done, total};
        const std::string progress = context.localizer.Format(
            "status.auto_progress", "Auto pick {0}/{1}", arguments);
        ui->text(ui->user, anomaly::sdk::StringView(progress));
    }

    // 每个品类一个独立表 id，滚动位置与列宽互不干扰。
    const std::string table_id =
        "haunted-house-item-" + std::to_string(kind);
    if (ui->begin_table(ui->user, anomaly::sdk::StringView(table_id), 4, 0, 0.0F,
                        0.0F) == 0) {
        return;
    }
    ui->table_next_row(ui->user);
    for (const auto &header : headers) {
        static_cast<void>(ui->table_next_column(ui->user));
        ui->text(ui->user, anomaly::sdk::StringView(header));
    }

    // 结果向量由 Game 域在扫描结束时整体替换，这里持锁读完即可，不需要复制。
    std::scoped_lock lock(context.result_mutex);
    std::size_t ordinal{};
    for (const auto &item : context.results) {
        if (item.kind != kind) continue;
        ++ordinal;
        ui->table_next_row(ui->user);
        static_cast<void>(ui->table_next_column(ui->user));
        const std::string ordinal_text = std::to_string(ordinal);
        ui->text(ui->user, anomaly::sdk::StringView(ordinal_text));

        static_cast<void>(ui->table_next_column(ui->user));
        char distance[32]{};
        std::snprintf(distance, sizeof(distance), "%.1f m", item.distance_meters);
        ui->text(ui->user, anomaly::sdk::StringView(distance));

        static_cast<void>(ui->table_next_column(ui->user));
        int ticked = 0;
        {
            std::scoped_lock taken(context.collected_mutex);
            ticked = context.collected.contains(item.entity_id) ? 1 : 0;
        }
        const std::string check_label =
            "##haunted-house-taken-" + std::to_string(item.entity_id);
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(check_label),
                         &ticked) != 0) {
            std::scoped_lock taken(context.collected_mutex);
            if (ticked != 0) {
                context.collected.insert(item.entity_id);
            } else {
                context.collected.erase(item.entity_id);
            }
        }

        static_cast<void>(ui->table_next_column(ui->user));
        TeleportCellAt(ui, context, item.position, can_teleport, teleport_label,
                       item.entity_id);
    }
    if (ordinal == 0) {
        const std::string empty =
            context.localizer.Text("item.empty", "Nothing scanned for this kind yet.");
        ui->text(ui->user, anomaly::sdk::StringView(empty));
    }
    ui->end_table(ui->user);
}

void ANOMALY_CALL Draw(void *plugin_context, const AnomalyUiServiceV1 *ui) {
    auto *context = static_cast<Context *>(plugin_context);
    if (context == nullptr || !UiReady(ui)) return;
    try {
        const std::string title =
            context->localizer.Text("window.title", "Haunted House");
        int open = 1;
        // 六个分类标签都要露全（含「诡乐宣传单 (30)」这类带数量的），420 会挤到横向滚动。
        ui->set_next_window_size(ui->user, 640.0F, 0.0F, 4U);
        anomaly::sdk::UiWindow window(ui, title, &open, 0);
        if (!window) return;

        const auto teleport_service = Query<AnomalyNtePlayerTeleportServiceV1>(
            context->host, ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_ID,
            ANOMALY_NTE_PLAYER_TELEPORT_SERVICE_V1_VERSION);
        const bool can_teleport = TeleportReady(teleport_service);

        // 每个品类的数量用来做标签文字，一次加锁统计完。
        std::array<std::size_t, kItemKinds.size()> kind_counts{};
        {
            std::scoped_lock lock(context->result_mutex);
            for (const auto &item : context->results) {
                if (item.kind < kind_counts.size()) ++kind_counts[item.kind];
            }
        }

        const bool tabs = TabsReady(ui) &&
                          ui->begin_tab_bar(ui->user,
                                            anomaly::sdk::StringView(
                                                "##haunted-house-tabs"),
                                            0) != 0;
        if (!tabs) {
            DrawMainPage(ui, *context, can_teleport);
            for (std::size_t kind{}; kind != kItemKinds.size(); ++kind) {
                ui->separator(ui->user);
                DrawItemPage(ui, *context, can_teleport, kind);
            }
        } else {
            const std::string main_label = context->localizer.Label(
                "tab.main", "Main flow", "haunted-house-tab-main");
            if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(main_label),
                                   nullptr, 0U, 1) != 0) {
                DrawMainPage(ui, *context, can_teleport);
                ui->end_tab_item(ui->user);
            }
            // 四个物件品类各占一页，标签上带本局扫到的数量。
            for (std::size_t kind{}; kind != kItemKinds.size(); ++kind) {
                const std::string kind_name = context->localizer.Text(
                    kItemKinds[kind].key, kItemKinds[kind].english);
                std::string tab_text =
                    kind_name + " (" + std::to_string(kind_counts[kind]) + ")";
                tab_text.append("###haunted-house-tab-item-")
                    .append(std::to_string(kind));
                if (ui->begin_tab_item(ui->user, anomaly::sdk::StringView(tab_text),
                                       nullptr, 0U, 1) != 0) {
                    DrawItemPage(ui, *context, can_teleport, kind);
                    ui->end_tab_item(ui->user);
                }
            }
            ui->end_tab_bar(ui->user);
        }

        ui->separator(ui->user);
        const std::string status = LocalizedStatus(*context);
        ui->text(ui->user, anomaly::sdk::StringView(status));
        const std::string position_text = LocalizedPlayerPosition(*context);
        ui->text(ui->user, anomaly::sdk::StringView(position_text));
    } catch (...) {
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1 *host,
                                  void **plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *plugin_context = nullptr;
    auto *context = new (std::nothrow) Context();
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);
    context->localizer = anomaly::sdk::Localizer(host);
    context->host = host;
    context->core = Query<AnomalyCoreServiceV1>(
        host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    context->config = Query<AnomalyConfigServiceV1>(
        host, ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    context->ui = Query<AnomalyUiServiceV1>(host, ANOMALY_UI_SERVICE_V1_ID,
                                            ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ConfigReady(context->config) || !UiReady(context->ui)) {
        delete context;
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                      "required plugin services are unavailable");
    }
    ResetPositions(*context);
    const auto schema_status = context->config->register_schema(
        context->config->user, anomaly::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(kSettingsSchema),
        &context->settings_schema);
    if (schema_status.code != ANOMALY_STATUS_V1_OK ||
        context->settings_schema.id == 0 || !LoadSettings(*context)) {
        ReleaseSettingsSchema(*context);
        delete context;
        return Status(ANOMALY_STATUS_V1_FAILED,
                      "haunted house settings are invalid");
    }
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void *plugin_context) {
    auto *context = static_cast<Context *>(plugin_context);
    return context != nullptr ? anomaly::sdk::Ok()
                              : Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
}

AnomalyStatusV1 ANOMALY_CALL Stop(void *plugin_context, std::uint32_t) {
    auto *context = static_cast<Context *>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    context->teleport_requested.store(false, std::memory_order_release);
    context->teleport_state.store(static_cast<std::uint32_t>(TeleportState::idle),
                                  std::memory_order_release);
    context->scan_requested.store(false, std::memory_order_release);
    context->auto_pick_requested.store(false, std::memory_order_release);
    context->auto_stop_requested.store(false, std::memory_order_release);
    context->main_run_requested.store(false, std::memory_order_release);
    context->main_stop_requested.store(false, std::memory_order_release);
    StopAutoPick(*context);
    StopMainRun(*context);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void *plugin_context) {
    auto *context = static_cast<Context *>(plugin_context);
    if (context == nullptr) return;
    static_cast<void>(Stop(context, 0));
    ReleaseSettingsSchema(*context);
    delete context;
}

} // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1 *descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "descriptor is invalid");
    }
    *descriptor = {sizeof(*descriptor),
                   ANOMALY_PLUGIN_API_V1_MAJOR,
                   ANOMALY_PLUGIN_API_V1_MINOR,
                   anomaly::sdk::StringView("xuancaiyajiji.haunted-house"),
                   anomaly::sdk::StringView("Haunted House"),
                   anomaly::sdk::StringView("Anomaly"),
                   anomaly::sdk::StringView("1.0.0"),
                   Load,
                   Start,
                   Stop,
                   Unload,
                   Update,
                   Draw};
    return anomaly::sdk::Ok();
}
