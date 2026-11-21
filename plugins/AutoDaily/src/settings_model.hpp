#pragma once

// 用户设置：全部经宿主 anomaly.config 持久化（schema 校验 + 原子写）。
// 落盘位置：<Anomaly>\config\plugins\yu-1021.daily\config-yu-1021.daily.settings.json
// 用户可以直接改文件：Worker 域每秒轮询一次，改完 1~2 秒内生效。

#include "anomaly/sdk/anomaly_sdk.h"
#include "anomaly/sdk/cpp.hpp"

#include "text_view.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace daily {

constexpr const char* kConfigSchemaId = "yu-1021.daily.settings";
constexpr std::uint32_t kConfigSchemaVersion = 1;
constexpr const char* kProfileFileName = "compatibility.profile.json";
constexpr const char* kProfileContract = "auto-daily-v1";

// 副本对照表的一行：index 就是配置文件里的 dungeon.index（= CloneEnter 面板里从上往下第几项）。
// entry_index 是游戏本体的副本入口序号（1,2,3,4,5,6,9,13），跟面板顺序不是一回事，
// 列出来只是让人在别的资料里能对上号。
struct DungeonRow {
    const char* name_key;   // 本地化 key
    const char* name;       // 英文回退
    std::uint32_t entry_index;
    std::uint32_t sub_count;
    std::uint32_t min_level;
    std::uint32_t max_level;
    const char* sub_keys[6];   // 子副本本地化 key（单子本副本填空串）
    const char* sub_names[6];  // 子副本英文回退
};

// 与 CloneEnter 面板的选本菜单逐项对齐（顺序、子本顺序、难度范围都一致）。
constexpr DungeonRow kDungeonTable[] = {
    {"dungeon.entry.resource", "Resource dungeon", 1, 3, 1, 6,
     {"dungeon.sub.character_exp", "dungeon.sub.disc_exp", "dungeon.sub.armor_coin", "", "", ""},
     {"Character EXP", "Disc EXP", "Armor coin", "", "", ""}},
    {"dungeon.entry.material", "Ability material dungeon", 2, 5, 1, 7,
     {"dungeon.sub.careful_pigeons", "dungeon.sub.poker_tea_party", "dungeon.sub.surprise_party",
      "dungeon.sub.telepathy", "dungeon.sub.escape_artistry", ""},
     {"Careful, pigeons", "Poker tea party", "Surprise party", "Telepathy", "Escape artistry", ""}},
    {"dungeon.entry.disc", "Disc breakthrough dungeon", 3, 5, 1, 7,
     {"dungeon.sub.apple_core", "dungeon.sub.spiral_tune", "dungeon.sub.liquid_dream",
      "dungeon.sub.cold_dessert", "dungeon.sub.drama_core", ""},
     {"Apple core", "Spiral tune", "Liquid dream", "Cold dessert", "Drama core", ""}},
    {"dungeon.entry.hollow", "Hollow dusk dungeon", 4, 6, 1, 6,
     {"dungeon.sub.lost_light_and_diablo", "dungeon.sub.demon_blood_and_street_king",
      "dungeon.sub.forest_fireflies_and_crimson", "dungeon.sub.tia_and_guardian_kingdom",
      "dungeon.sub.blue_hedgehog_and_quiet_manor", "dungeon.sub.creed_of_shadow_and_tiny_adventure"},
     {"Lost light and Diablo", "Demon blood and the street king",
      "Forest fireflies and Crimson", "Tia and the guardian kingdom",
      "Blue hedgehog and the quiet manor", "Creed of shadow and the tiny adventure"}},
    {"dungeon.entry.morphix", "Weekly: Morphix", 5, 1, 1, 7, {"", "", "", "", "", ""},
     {"", "", "", "", "", ""}},
    {"dungeon.entry.show", "Weekly: the show never ends", 6, 1, 1, 7, {"", "", "", "", "", ""},
     {"", "", "", "", "", ""}},
    {"dungeon.entry.bank", "Bank heist", 9, 1, 1, 1, {"", "", "", "", "", ""},
     {"", "", "", "", "", ""}},
    {"dungeon.entry.debt", "Weekly: debt collector", 13, 1, 1, 7, {"", "", "", "", "", ""},
     {"", "", "", "", "", ""}},
};
constexpr std::size_t kDungeonTableCount = sizeof(kDungeonTable) / sizeof(kDungeonTable[0]);

struct DungeonChoice {
    std::uint32_t index{0};        // 对照表序号
    std::uint32_t sub_index{0};    // 子本序号（面板顺序，0 基）
    std::uint32_t level_index{1};  // 难度序号（1 基）
    std::uint32_t runs{6};         // 打几次（清体力次数）
    std::uint32_t enter_wait{4};   // 进本后等待秒数（交给 CloneEnter）
    std::uint32_t exit_wait{8};    // 退出后等待秒数（交给 CloneEnter）
};

struct Settings {
    bool enabled{true};            // 总开关
    bool configured{false};        // 首次设置是否已完成
    // 「下次游戏启动时跑一次」：pending 会在游戏真正重启后才触发。
    // auto_run_pid/auto_run_stamp 记录勾选时的会话（PID + 进程创建时间秒低 32 位），
    // 同一个进程里热重载不算重启，因此不会立刻执行。
    bool auto_run_pending{false};
    std::uint32_t auto_run_pid{0};
    std::uint32_t auto_run_stamp{0};
    bool stage_dungeon{true};
    bool stage_cafe{true};
    // 两种领取分开设置、分开执行：每日活跃奖励 / 环期任务奖励。
    bool stage_daily_rewards{true};
    bool stage_periodic_rewards{true};
    DungeonChoice dungeon{};
    std::uint32_t stop_modifiers{2};        // 2 = ANOMALY_INPUT_MODIFIER_V1_CONTROL
    std::uint32_t stop_vk{0x53};            // VK_S
    std::uint32_t world_settle_seconds{8};  // 到达世界后静置多久再开跑
    std::uint32_t stage_timeout_seconds{1200};
};

// 按对照表把设置收敛到合法范围（改配置/读配置后都要跑一次）。
void NormalizeSettings(Settings& settings) noexcept;

const DungeonRow& DungeonRowAt(std::uint32_t index) noexcept;

std::string SerializeSettings(const Settings& settings);
// 返回 false 表示文档不可解析，out 保持默认值。
bool ParseSettings(const std::string& document, const AnomalyJsonServiceV1* json,
                   Settings& out) noexcept;

// 配置持久化：注册 schema → 读已有文档；写入在 Worker 域执行。
class ConfigStore final {
public:
    void Bind(const AnomalyConfigServiceV1* config, const AnomalyJsonServiceV1* json) noexcept;
    bool RegisterSchema() noexcept;
    bool Ready() const noexcept { return registered_; }
    // NOT_FOUND（首次使用）返回 false 并把 out 保持为默认值。
    bool Read(Settings& out) noexcept;
    bool Write(const Settings& settings) noexcept;
    const char* LastError() const noexcept { return error_; }

private:
    const AnomalyConfigServiceV1* config_{};
    const AnomalyJsonServiceV1* json_{};
    AnomalyGenerationHandleV1 schema_{};
    bool registered_{};
    char error_[160]{};
};

}  // namespace daily
