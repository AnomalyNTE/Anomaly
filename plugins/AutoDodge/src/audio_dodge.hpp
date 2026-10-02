// 自动闪避（音频触发版）
//
// 触发：WASAPI 回路采集默认播放设备的输出 → 4 阶 Butterworth 高通 →
//       FFT 互相关匹配预设的“闪避提示音”波形 → 得分超过阈值即触发。
//       信号链与参考项目 NTESoundTrigger 一致，参考波形由其 .npy 导出。
//
// 动作：在 on_update（Game 域）执行，三路可选，默认“技能优先，失败按键”：
//   1) 技能桥接 anomaly.nte.skill-invocation —— 直接调用游戏的
//      HTTryActivateAbilityByClass 激活闪避能力，进程内、不依赖输入注入；
//   2) 按键注入 —— SendInput（系统级，同 pydirectinput）与
//      PostMessageW（窗口消息，同仓库内 QuickUltimate）双发，最大化命中概率。
//   按键注入只在本进程拥有前台窗口时进行（安全限制）。
//
// 线程域：采集与匹配跑在插件自己的采集线程上，检测到命中只置一个原子标志；
// 实际动作交给 on_update（Game 域，每帧一次）执行——技能桥接要求游戏线程。

#pragma once

#include "anomaly/sdk/cpp.hpp"

#include "sound_capture.hpp"
#include "sound_matcher.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace anomaly::plugins::auto_dodge {

enum class DodgeKey : std::uint32_t {
    RightMouse = 0,
    LeftShift = 1,
    Space = 2,
    LeftControl = 3,
};

// 动作方式
enum class ActionMode : std::uint32_t {
    SkillThenKey = 0,  // 技能优先，不可用/被拒时按键
    SkillOnly = 1,
    KeyOnly = 2,
};

// 技能桥接的三态结果：被游戏明确拒绝时不再退回按键（拒绝原因对按键同样成立）。
enum class SkillActivation : std::uint32_t {
    Accepted = 0,
    Refused = 1,
    Unavailable = 2,
};

// 单帧匹配长度：采集线程攒够这么多 32kHz 样本就匹配一次。
constexpr std::uint32_t kMatchFrameMilliseconds = 50;
constexpr std::uint32_t kMatchFrameSamples =
    kReferenceSampleRate * kMatchFrameMilliseconds / 1000U;

constexpr std::uint32_t kDefaultThresholdMilli = 130;   // 参考项目 DODGE_THRESH = 0.13
constexpr std::uint32_t kDefaultCooldownMilliseconds = 500;
constexpr std::uint32_t kDefaultKeyHoldMilliseconds = 24;

class Session final {
public:
    AnomalyStatusV1 Load(const AnomalyHostApiV1* host) noexcept;
    void Unload() noexcept;

    void Update(double delta_seconds) noexcept;
    void Draw(const AnomalyUiServiceV1* ui) noexcept;

    void Save() noexcept;

private:
    static void ANOMALY_CALL OnAudioFrame(void* user, const float* mono,
                                          std::size_t count) noexcept;

    void RefreshServices() noexcept;
    bool LoadReference() noexcept;
    void StartCapture() noexcept;
    void HandleAudioFrame(const float* mono, std::size_t count) noexcept;

    // 动作（on_update / Game 域）
    void ExecutePendingAction() noexcept;
    void RefreshDodgeSkill() noexcept;
    SkillActivation ActivateDodgeSkill() noexcept;
    void SendDodgeKey(bool down) noexcept;
    void ServicePendingKeyRelease() noexcept;

    void LoadSettings() noexcept;
    void SaveSettingsIfDirty() noexcept;
    void RecordEvent(const char* text) noexcept;
    void Log(std::uint32_t level, const std::string& message) const noexcept;

    const AnomalyHostApiV1* host_{};
    const AnomalyCoreServiceV1* core_{};
    const AnomalyUiServiceV1* ui_{};
    const AnomalyPluginStateServiceV1* plugin_state_{};
    const AnomalyUe5WorldServiceV1* ue5_world_{};
    const AnomalyNteSkillsServiceV1* skills_{};
    const AnomalyNteSkillInvocationServiceV1* invocation_{};
    std::string state_directory_{};
    std::string package_directory_{};
    bool reference_loaded_{};

    LoopbackCapture capture_{};
    SoundMatcher matcher_{};

    // 设置：on_draw 写、采集线程/Game 线程读
    std::atomic_bool enabled_{true};
    std::atomic_bool log_near_miss_{false};
    std::atomic<std::uint32_t> threshold_milli_{kDefaultThresholdMilli};
    std::atomic<std::uint32_t> cooldown_milliseconds_{kDefaultCooldownMilliseconds};
    std::atomic<std::uint32_t> dodge_key_{static_cast<std::uint32_t>(DodgeKey::RightMouse)};
    std::atomic<std::uint32_t> action_mode_{
        static_cast<std::uint32_t>(ActionMode::SkillThenKey)};
    std::atomic<std::uint32_t> key_hold_milliseconds_{kDefaultKeyHoldMilliseconds};

    // 检测 → 动作的交接（采集线程写、Game 线程消费）
    std::atomic_bool pending_action_{false};
    std::atomic<std::uint32_t> pending_score_micro_{};

    // 诊断
    std::atomic<std::uint32_t> score_micro_{};
    std::atomic<std::uint32_t> peak_micro_{};
    std::atomic<std::uint32_t> last_trigger_score_micro_{};
    std::atomic<std::uint64_t> trigger_count_{};
    std::atomic<std::uint64_t> skill_hit_count_{};
    std::atomic<std::uint64_t> key_hit_count_{};
    std::atomic<std::uint64_t> refused_count_{};
    std::atomic<std::uint64_t> blocked_count_{};
    std::atomic<std::uint64_t> fail_count_{};
    std::atomic<std::uint64_t> frame_count_{};
    mutable std::mutex event_mutex_{};
    std::array<char, 192> last_event_{"等待音频帧"};

    // 仅采集线程访问
    std::chrono::steady_clock::time_point last_fire_{};
    std::chrono::steady_clock::time_point last_near_log_{};
    bool has_fired_{};
    bool ready_{true};

    // 仅 Game 线程访问（技能桥接）
    struct SkillChoice {
        AnomalyGenerationHandleV1 handle{};
        AnomalyGenerationHandleV1 character{};
        char label[96]{};
        bool valid{};
    } skill_{};
    std::uint64_t skill_generation_{};
    std::uint32_t skill_refresh_countdown_{};

    // 仅 Game 线程访问（按键抬起，延迟一个帧）
    bool key_pending_{};
    std::chrono::steady_clock::time_point key_release_at_{};

    std::atomic<std::uint64_t> settings_revision_{};
    std::uint64_t applied_revision_{};

    // 采集状态变化只记一次日志（on_update 里比较）
    std::array<char, 64> logged_capture_state_{};
    std::array<char, 192> logged_capture_error_{};
};

}  // namespace anomaly::plugins::auto_dodge
