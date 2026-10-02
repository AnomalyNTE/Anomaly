#include "audio_dodge.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace anomaly::plugins::auto_dodge {

using anomaly::sdk::StringView;

namespace {

constexpr std::uint32_t kOk = ANOMALY_STATUS_V1_OK;
constexpr std::uint32_t kInfo = ANOMALY_CORE_LOG_LEVEL_V1_INFO;
constexpr std::uint32_t kWarning = ANOMALY_CORE_LOG_LEVEL_V1_WARNING;
constexpr std::uint32_t kError = ANOMALY_CORE_LOG_LEVEL_V1_ERROR;

// struct_size 尾部探测：ABI v1 只保证插件拿到的表至少覆盖查询时声明的版本。
#define HAS(pointer, type, member)                                             \
    ((pointer) != nullptr &&                                                   \
     (pointer)->struct_size >=                                                 \
         offsetof(type, member) + sizeof(decltype(type::member)) &&            \
     (pointer)->member != nullptr)

constexpr std::size_t MaxTextBuffer = 512;

class TextBuffer final {
public:
    template <typename Read>
    bool Read(Read read) noexcept {
        std::size_t size{};
        if (read(nullptr, &size).code != kOk || size == 0 || size > MaxTextBuffer) {
            return false;
        }
        if (read(data_.data(), &size).code != kOk || size > data_.size()) return false;
        if (size != 0 && data_[size - 1U] == '\0') --size;
        size_ = size;
        return true;
    }

    [[nodiscard]] const char* c_str() noexcept {
        data_[size_] = '\0';
        return data_.data();
    }

private:
    std::array<char, MaxTextBuffer + 1> data_{};
    std::size_t size_{};
};

void TrimInPlace(std::string& value) noexcept {
    const auto not_space = [](const unsigned char character) {
        return std::isspace(character) == 0;
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
}

bool IsExtendedKey(const std::uint32_t virtual_key) noexcept {
    switch (virtual_key) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
        return true;
    default:
        return false;
    }
}

std::uint32_t VirtualKeyFor(const DodgeKey key) noexcept {
    switch (key) {
    case DodgeKey::LeftShift:
        return VK_LSHIFT;
    case DodgeKey::Space:
        return VK_SPACE;
    case DodgeKey::LeftControl:
        return VK_LCONTROL;
    case DodgeKey::RightMouse:
    default:
        return VK_RBUTTON;
    }
}

const char* KeyName(const DodgeKey key) noexcept {
    switch (key) {
    case DodgeKey::LeftShift:
        return "Shift";
    case DodgeKey::Space:
        return "空格";
    case DodgeKey::LeftControl:
        return "Ctrl";
    case DodgeKey::RightMouse:
    default:
        return "右键";
    }
}

void SendKeyState(const DodgeKey key, const bool down) noexcept {
    INPUT input{};
    if (key == DodgeKey::RightMouse) {
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else {
        const std::uint32_t virtual_key = VirtualKeyFor(key);
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(virtual_key);
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        if (IsExtendedKey(virtual_key)) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &input, sizeof(INPUT));
}

// 只认本进程拥有的前台窗口：SendInput 会投递给前台窗口，
// 游戏在后台时绝不能把按键打进别的程序。
bool GameWindowIsForeground() noexcept {
    const HWND window = GetForegroundWindow();
    if (window == nullptr) return false;
    DWORD process_id{};
    return GetWindowThreadProcessId(window, &process_id) != 0 &&
        process_id == GetCurrentProcessId();
}

// PostMessageW 要发给游戏自己的窗口：优先 UnrealWindow，否则取本进程前台窗口。
HWND ResolveGameWindow() noexcept {
    const HWND foreground = GetForegroundWindow();
    DWORD foreground_pid{};
    if (foreground == nullptr ||
        GetWindowThreadProcessId(foreground, &foreground_pid) == 0 ||
        foreground_pid != GetCurrentProcessId()) {
        return nullptr;
    }
    const HWND unreal = FindWindowW(L"UnrealWindow", nullptr);
    DWORD unreal_pid{};
    if (unreal != nullptr &&
        GetWindowThreadProcessId(unreal, &unreal_pid) != 0 &&
        unreal_pid == GetCurrentProcessId()) {
        return unreal;
    }
    return foreground;
}

std::uint32_t ToMicro(const double score) noexcept {
    const double clamped = std::clamp(score, 0.0, 1000.0);
    return static_cast<std::uint32_t>(clamped * 1e6 + 0.5);
}

}  // namespace

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

AnomalyStatusV1 Session::Load(const AnomalyHostApiV1* host) noexcept {
    if (host == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    host_ = host;
    RefreshServices();
    if (!HAS(ui_, AnomalyUiServiceV1, begin_window) ||
        !HAS(ui_, AnomalyUiServiceV1, text)) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }

    // 插件包目录：参考波形随包发布（audio/ 由 CMake 复制进去）。
    if (HAS(core_, AnomalyCoreServiceV1, plugin_directory)) {
        TextBuffer buffer;
        if (buffer.Read([this](char* destination, std::size_t* size) {
                return core_->plugin_directory(core_->user, destination, size);
            })) {
            package_directory_ = buffer.c_str();
        }
    }
    LoadSettings();
    reference_loaded_ = LoadReference();
    if (!reference_loaded_) {
        Log(kError, std::string("自动闪避：参考波形不可用，插件不会触发（")
                + last_event_.data() + "）");
    }
    StartCapture();
    return anomaly::sdk::Ok();
}

void Session::Unload() noexcept {
    capture_.Stop();
    Save();
    host_ = nullptr;
    core_ = nullptr;
    ui_ = nullptr;
    plugin_state_ = nullptr;
    ue5_world_ = nullptr;
    skills_ = nullptr;
    invocation_ = nullptr;
}

void Session::RefreshServices() noexcept {
    if (host_ == nullptr) return;
    const anomaly::sdk::Host view(host_);
    const auto core = view.Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID, 1u);
    core_ = core ? core.get() : nullptr;
    const auto ui = view.Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID, 1u);
    ui_ = ui ? ui.get() : nullptr;
    const auto plugin_state = view.Query<AnomalyPluginStateServiceV1>(
        ANOMALY_PLUGIN_STATE_SERVICE_V1_ID, 1u);
    plugin_state_ = plugin_state ? plugin_state.get() : nullptr;
    // 技能桥接用到的服务要等玩家进入世界后才可用，每帧重查（宿主内是哈希查找）。
    const auto ue5_world = view.Query<AnomalyUe5WorldServiceV1>(
        ANOMALY_UE5_WORLD_SERVICE_V1_ID, 1u);
    ue5_world_ = ue5_world ? ue5_world.get() : nullptr;
    const auto skills = view.Query<AnomalyNteSkillsServiceV1>(
        ANOMALY_NTE_SKILLS_SERVICE_V1_ID, 1u);
    skills_ = skills ? skills.get() : nullptr;
    const auto invocation = view.Query<AnomalyNteSkillInvocationServiceV1>(
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID, 1u);
    invocation_ = invocation ? invocation.get() : nullptr;
}

bool Session::LoadReference() noexcept {
    if (package_directory_.empty()) {
        RecordEvent("插件包目录不可用");
        return false;
    }
    try {
        const std::filesystem::path path =
            std::filesystem::path(package_directory_) / "audio" / "dodge.f64";
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            RecordEvent("缺少 audio/dodge.f64");
            return false;
        }
        std::uint64_t count{};
        stream.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!stream || count == 0 || count > (1ULL << 22U)) {
            RecordEvent("audio/dodge.f64 头部无效");
            return false;
        }
        std::vector<double> reference(static_cast<std::size_t>(count));
        stream.read(reinterpret_cast<char*>(reference.data()),
            static_cast<std::streamsize>(count * sizeof(double)));
        if (!stream) {
            RecordEvent("audio/dodge.f64 数据截断");
            return false;
        }
        if (!matcher_.Prepare(reference, kReferenceSampleRate)) {
            RecordEvent(matcher_.error());
            return false;
        }
        char line[160]{};
        std::snprintf(line, sizeof(line), "参考波形就绪：%zu 样本 / %.3f 秒 / FFT %zu",
            matcher_.reference_samples(),
            static_cast<double>(matcher_.reference_samples()) / kReferenceSampleRate,
            matcher_.fft_size());
        RecordEvent(line);
        Log(kInfo, std::string("自动闪避：") + line);
        return true;
    } catch (...) {
        RecordEvent("读取参考波形异常");
        return false;
    }
}

void Session::StartCapture() noexcept {
    if (!capture_.Start(this, &Session::OnAudioFrame, kMatchFrameSamples)) {
        RecordEvent("音频采集线程启动失败");
        Log(kError, "自动闪避：音频采集线程启动失败");
    }
}

void Session::Update(double delta_seconds) noexcept {
    static_cast<void>(delta_seconds);
    try {
        RefreshServices();
        SaveSettingsIfDirty();

        // 技能桥接只认 Game 域；先刷新闪避技能，再服务按键抬起与待执行动作。
        RefreshDodgeSkill();
        ServicePendingKeyRelease();
        ExecutePendingAction();

        // 采集是异步启动的，把状态与错误的变化各记一条日志，方便离线定位。
        const CaptureStatus status = capture_.Status();
        if (std::strcmp(status.state, logged_capture_state_.data()) != 0) {
            std::snprintf(logged_capture_state_.data(), logged_capture_state_.size(), "%s",
                status.state);
            char line[256]{};
            if (status.device_sample_rate != 0) {
                std::snprintf(line, sizeof(line), "音频采集：%s  %s  %uHz %uch %ubit",
                    status.state, status.device, status.device_sample_rate,
                    status.device_channels, status.device_bits);
            } else {
                std::snprintf(line, sizeof(line), "音频采集：%s", status.state);
            }
            Log(kInfo, std::string("自动闪避：") + line);
        }
        if (std::strcmp(status.error, logged_capture_error_.data()) != 0) {
            std::snprintf(logged_capture_error_.data(), logged_capture_error_.size(), "%s",
                status.error);
            if (status.error[0] != '\0') {
                Log(kError, std::string("自动闪避：音频采集失败：") + status.error);
            }
        }
    } catch (...) {
        RecordEvent("更新流程异常");
    }
}

void ANOMALY_CALL Session::OnAudioFrame(void* user, const float* mono,
                                        const std::size_t count) noexcept {
    auto* session = static_cast<Session*>(user);
    if (session == nullptr) return;
    try {
        session->HandleAudioFrame(mono, count);
    } catch (...) {
    }
}

void Session::HandleAudioFrame(const float* mono, const std::size_t count) noexcept {
    frame_count_.fetch_add(1, std::memory_order_relaxed);
    if (!reference_loaded_) return;

    const double score = matcher_.Feed(mono, count);
    const std::uint32_t micro = ToMicro(score);
    score_micro_.store(micro, std::memory_order_relaxed);
    std::uint32_t peak = peak_micro_.load(std::memory_order_relaxed);
    while (micro > peak &&
           !peak_micro_.compare_exchange_weak(peak, micro, std::memory_order_relaxed)) {
    }

    const double threshold =
        static_cast<double>(threshold_milli_.load(std::memory_order_relaxed)) / 1000.0;
    const auto now = std::chrono::steady_clock::now();

    if (score < threshold) {
        // 边沿触发：回到阈值以下才重新武装，和参考项目的 ready 标志一致。
        ready_ = true;
        if (log_near_miss_.load(std::memory_order_relaxed) && threshold > 0.0 &&
            score >= threshold * 0.7 &&
            (last_near_log_.time_since_epoch().count() == 0 ||
                now - last_near_log_ > std::chrono::milliseconds(2000))) {
            last_near_log_ = now;
            char line[160]{};
            std::snprintf(line, sizeof(line), "接近阈值：得分 %.4f / 阈值 %.3f", score, threshold);
            Log(kInfo, std::string("自动闪避：") + line);
        }
        return;
    }

    if (!ready_) return;
    const auto cooldown = std::chrono::milliseconds(
        static_cast<std::int64_t>(cooldown_milliseconds_.load(std::memory_order_relaxed)));
    if (has_fired_ && now - last_fire_ < cooldown) return;
    ready_ = false;

    if (!enabled_.load(std::memory_order_relaxed)) {
        RecordEvent("命中提示音（开关关闭）");
        Log(kWarning, "自动闪避：命中提示音，但插件开关已关闭");
        return;
    }

    // 检测到命中：交给 on_update（Game 域）执行动作。
    has_fired_ = true;
    last_fire_ = now;
    pending_score_micro_.store(micro, std::memory_order_relaxed);
    pending_action_.store(true, std::memory_order_release);
    trigger_count_.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// 动作（Game 域）
// ---------------------------------------------------------------------------

void Session::ExecutePendingAction() noexcept {
    if (!pending_action_.exchange(false, std::memory_order_acq_rel)) return;

    const double score =
        static_cast<double>(pending_score_micro_.load(std::memory_order_relaxed)) / 1e6;
    const double threshold =
        static_cast<double>(threshold_milli_.load(std::memory_order_relaxed)) / 1000.0;
    const auto key = static_cast<DodgeKey>(dodge_key_.load(std::memory_order_relaxed));
    const auto now = std::chrono::steady_clock::now();

    const auto mode = static_cast<ActionMode>(action_mode_.load(std::memory_order_relaxed));
    bool handled = false;
    bool refused = false;
    const char* path = "无";
    if (mode != ActionMode::KeyOnly) {
        switch (ActivateDodgeSkill()) {
        case SkillActivation::Accepted:
            skill_hit_count_.fetch_add(1, std::memory_order_relaxed);
            handled = true;
            path = "技能桥接";
            break;
        case SkillActivation::Refused:
            refused = true;
            break;
        case SkillActivation::Unavailable:
        default:
            break;
        }
    }
    // 被游戏拒绝时不退回按键：同样的冷却/条件对按键一样成立。
    if (!handled && !refused && mode != ActionMode::SkillOnly) {
        // 技能桥接与 PostMessageW 都只针对本进程的窗口，不需要前台；
        // SendInput 需要前台，在 SendDodgeKey 内部单独判断。
        if (ResolveGameWindow() == nullptr) {
            blocked_count_.fetch_add(1, std::memory_order_relaxed);
            RecordEvent("命中提示音，但定位不到游戏窗口");
            Log(kWarning, std::string("自动闪避：命中提示音（得分 ") +
                std::to_string(score) + "），但定位不到游戏窗口，已拦截本次动作");
            return;
        }
        SendDodgeKey(true);
        key_hit_count_.fetch_add(1, std::memory_order_relaxed);
        handled = true;
        path = "按键双发";
        key_pending_ = true;
        key_release_at_ = now +
            std::chrono::milliseconds(
                static_cast<std::int64_t>(key_hold_milliseconds_.load(std::memory_order_relaxed)));
    }

    if (handled) {
        last_trigger_score_micro_.store(
            static_cast<std::uint32_t>(score * 1e6 + 0.5), std::memory_order_relaxed);
        char line[192]{};
        std::snprintf(line, sizeof(line),
            "触发闪避：得分 %.4f（阈值 %.3f）路径 %s 按键 %s",
            score, threshold, path, KeyName(key));
        RecordEvent(line);
        Log(kInfo, std::string("自动闪避：") + line);
    } else if (refused) {
        refused_count_.fetch_add(1, std::memory_order_relaxed);
        RecordEvent("技能被游戏拒绝（冷却或条件不足）");
        char line[192]{};
        std::snprintf(line, sizeof(line),
            "自动闪避：技能桥接被游戏拒绝（得分 %.4f），本次不发送按键", score);
        Log(kWarning, line);
    } else {
        fail_count_.fetch_add(1, std::memory_order_relaxed);
        RecordEvent("动作失败：技能与按键都不可用");
        Log(kError, std::string("自动闪避：动作失败：技能桥接不可用且按键路径失败"));
    }
}

void Session::RefreshDodgeSkill() noexcept {
    if (!HAS(skills_, AnomalyNteSkillsServiceV1, frame) ||
        !HAS(skills_, AnomalyNteSkillsServiceV1, page)) {
        skill_ = {};
        return;
    }
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    if (skills_->frame(skills_->user, &frame).code != kOk) return;
    // 生成号变了（换角色 / 重新授予技能）立即重扫；已解析成功就不再扫，失败时限速重试。
    if (frame.generation == skill_generation_) {
        if (skill_.valid) return;
        if (skill_refresh_countdown_ > 0) {
            --skill_refresh_countdown_;
            return;
        }
    }

    std::array<AnomalyNteSkillSnapshotV1, ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> page{};
    for (auto& entry : page) entry.struct_size = sizeof(entry);
    std::uint32_t offset{};
    std::uint32_t total{};
    std::size_t path_scans{};
    AnomalyNteSkillSnapshotV1 by_input{};
    AnomalyNteSkillSnapshotV1 by_path{};
    char path_buffer[96]{};

    for (std::uint32_t round{}; round < 8U; ++round) {
        AnomalyNteSkillPageRequestV1 request{sizeof(request)};
        request.generation = frame.generation;
        request.offset = offset;
        request.capacity = static_cast<std::uint32_t>(page.size());
        AnomalyNteSkillPageResultV1 result{sizeof(result)};
        auto status = skills_->page(skills_->user, &request, page.data(), &result);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND && offset != 0) {
            request.generation = 0;
            request.offset = 0;
            result = AnomalyNteSkillPageResultV1{sizeof(result)};
            status = skills_->page(skills_->user, &request, page.data(), &result);
        }
        if (status.code != kOk) break;
        total = result.total_skills;
        for (std::uint32_t index{}; index < result.returned; ++index) {
            const auto& entry = page[index];
            if ((entry.flags & ANOMALY_NTE_SKILL_V1_VALID) == 0) continue;
            // 不按 STALE 过滤：技能快照每几个 tick 刷新一次，其余帧都被标记 STALE，
            // 但 handle 在同一个 generation 内有效，激活时宿主会重新校验。
            if (entry.handle.id == 0 || entry.ability_class.id == 0) continue;
            // InputID_Evade = 1（ESkillInputIDType）。
            if (entry.input_id == 1 && by_input.handle.id == 0) {
                by_input = entry;
                continue;
            }
            if (by_input.handle.id != 0 || by_path.handle.id != 0) continue;
            if (path_scans >= 128 ||
                !HAS(skills_, AnomalyNteSkillsServiceV1, ability_path_utf8)) {
                continue;
            }
            ++path_scans;
            std::size_t size = sizeof(path_buffer);
            if (skills_->ability_path_utf8(skills_->user, entry.ability_class, path_buffer,
                    &size).code != kOk) {
                continue;
            }
            path_buffer[sizeof(path_buffer) - 1U] = '\0';
            if (std::strstr(path_buffer, "Evade") != nullptr &&
                std::strstr(path_buffer, "PerfectEvade") == nullptr &&
                std::strstr(path_buffer, "ShadowEvade") == nullptr &&
                std::strstr(path_buffer, "EvadeAtk") == nullptr &&
                std::strstr(path_buffer, "EvadeShadow") == nullptr &&
                std::strstr(path_buffer, "LongEvade") == nullptr) {
                by_path = entry;
            }
        }
        if (result.returned == 0U) break;
        offset = result.next_offset;
        if (offset >= total) break;
    }

    const AnomalyNteSkillSnapshotV1 chosen = by_input.handle.id != 0 ? by_input : by_path;
    skill_generation_ = frame.generation;
    skill_refresh_countdown_ = 30U;
    skill_ = {};
    if (chosen.handle.id == 0) return;
    skill_.handle = chosen.handle;
    skill_.character = chosen.character;
    skill_.valid = true;
    if (HAS(skills_, AnomalyNteSkillsServiceV1, ability_path_utf8)) {
        std::size_t size = sizeof(skill_.label);
        if (skills_->ability_path_utf8(skills_->user, chosen.ability_class, skill_.label,
                &size).code == kOk) {
            skill_.label[sizeof(skill_.label) - 1U] = '\0';
        }
    }
}

SkillActivation Session::ActivateDodgeSkill() noexcept {
    if (!skill_.valid || !HAS(invocation_, AnomalyNteSkillInvocationServiceV1, activate)) {
        return SkillActivation::Unavailable;
    }
    AnomalyGenerationHandleV1 world{};
    if (!HAS(ue5_world_, AnomalyUe5WorldServiceV1, current) ||
        ue5_world_->current(ue5_world_->user, &world).code != kOk || world.id == 0) {
        return SkillActivation::Unavailable;
    }
    AnomalyNteSkillInvocationRequestV1 request{sizeof(request)};
    request.world = world;
    request.character = skill_.character;
    request.skill = skill_.handle;
    AnomalyNteSkillInvocationResultV1 result{sizeof(result)};
    const auto status = invocation_->activate(invocation_->user, &request, &result);
    if (status.code != kOk) return SkillActivation::Unavailable;
    return result.accepted != 0 ? SkillActivation::Accepted : SkillActivation::Refused;
}

// 按键注入双发：SendInput（系统级，同 pydirectinput）+ PostMessageW（窗口消息，同
// QuickUltimate）。两条路径都发，任一路被游戏接收即可；重复消息由游戏自身去重。
// SendInput 需要游戏在前台（否则会投递给别的程序），PostMessageW 只针对本进程窗口。
void Session::SendDodgeKey(const bool down) noexcept {
    const auto key = static_cast<DodgeKey>(dodge_key_.load(std::memory_order_relaxed));
    // SendInput 需要游戏在前台（否则会投递给别的程序）；PostMessageW 只针对本进程窗口。
    if (GameWindowIsForeground()) {
        SendKeyState(key, down);
    }
    const HWND window = ResolveGameWindow();
    if (window == nullptr) return;
    if (key == DodgeKey::RightMouse) {
        POINT client{};
        if (!GetCursorPos(&client) || !ScreenToClient(window, &client)) {
            client = POINT{};
        }
        PostMessageW(window, down ? WM_RBUTTONDOWN : WM_RBUTTONUP,
            down ? static_cast<WPARAM>(MK_RBUTTON) : 0, MAKELPARAM(client.x, client.y));
    } else {
        const std::uint32_t virtual_key = VirtualKeyFor(key);
        const UINT scan_code = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
        LPARAM lparam = 1;
        lparam |= static_cast<LPARAM>(scan_code & 0xFFU) << 16U;
        if (IsExtendedKey(virtual_key)) lparam |= 1LL << 24U;
        if (!down) lparam |= (1LL << 30U) | (1LL << 31U);
        PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP,
            static_cast<WPARAM>(virtual_key), lparam);
    }
}

void Session::ServicePendingKeyRelease() noexcept {
    if (!key_pending_) return;
    if (std::chrono::steady_clock::now() < key_release_at_) return;
    key_pending_ = false;
    SendDodgeKey(false);
}

// ---------------------------------------------------------------------------
// 设置持久化
// ---------------------------------------------------------------------------

void Session::LoadSettings() noexcept {
    if (HAS(plugin_state_, AnomalyPluginStateServiceV1, directory)) {
        TextBuffer buffer;
        if (buffer.Read([this](char* destination, std::size_t* size) {
                return plugin_state_->directory(plugin_state_->user, destination, size);
            })) {
            state_directory_ = buffer.c_str();
        }
    }
    if (state_directory_.empty()) return;
    try {
        std::ifstream stream(std::filesystem::path(state_directory_) / "settings.txt");
        if (!stream) return;
        std::string line;
        while (std::getline(stream, line)) {
            const auto separator = line.find('=');
            if (separator == std::string::npos) continue;
            std::string key = line.substr(0, separator);
            std::string value = line.substr(separator + 1U);
            TrimInPlace(key);
            TrimInPlace(value);
            if (key == "enabled") {
                enabled_.store(value == "1");
            } else if (key == "threshold_milli") {
                const auto parsed = std::strtoul(value.c_str(), nullptr, 10);
                threshold_milli_.store(static_cast<std::uint32_t>(
                    std::clamp<unsigned long>(parsed, 10UL, 800UL)));
            }
        }
    } catch (...) {
    }
}

void Session::SaveSettingsIfDirty() noexcept {
    if (applied_revision_ == settings_revision_.load(std::memory_order_relaxed)) return;
    static std::chrono::steady_clock::time_point last_write{};
    const auto now = std::chrono::steady_clock::now();
    if (last_write.time_since_epoch().count() != 0 &&
        now - last_write < std::chrono::milliseconds(1500)) {
        return;
    }
    last_write = now;
    applied_revision_ = settings_revision_.load(std::memory_order_relaxed);
    Save();
}

void Session::Save() noexcept {
    if (state_directory_.empty()) return;
    try {
        std::filesystem::create_directories(std::filesystem::path(state_directory_));
        std::ofstream stream(
            std::filesystem::path(state_directory_) / "settings.txt", std::ios::trunc);
        if (!stream) return;
        stream << "enabled=" << (enabled_.load() ? 1 : 0) << '\n';
        stream << "threshold_milli=" << threshold_milli_.load() << '\n';
    } catch (...) {
    }
}

void Session::RecordEvent(const char* text) noexcept {
    if (text == nullptr) return;
    std::scoped_lock lock(event_mutex_);
    std::snprintf(last_event_.data(), last_event_.size(), "%s", text);
}

void Session::Log(const std::uint32_t level, const std::string& message) const noexcept {
    if (!HAS(core_, AnomalyCoreServiceV1, log)) return;
    core_->log(core_->user, level, StringView(message));
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void Session::Draw(const AnomalyUiServiceV1* ui) noexcept {
    const AnomalyUiServiceV1* service = ui != nullptr ? ui : ui_;
    if (!HAS(service, AnomalyUiServiceV1, begin_window) ||
        !HAS(service, AnomalyUiServiceV1, end_window)) {
        return;
    }
    bool window_open = false;
    try {
        if (HAS(service, AnomalyUiServiceV1, set_next_window_size_constraints)) {
            // 1080p 下 418x158 刚刚好；最小尺寸限制在 320x120，防止拖坏布局。
            service->set_next_window_size_constraints(
                service->user, 320.0F, 120.0F, 100000.0F, 100000.0F);
        }
        if (HAS(service, AnomalyUiServiceV1, set_next_window_size)) {
            service->set_next_window_size(service->user, 418.0F, 158.0F, 4U);
        }
        int open = 1;
        const int visible = service->begin_window(service->user, StringView("自动闪避"),
            &open, 0U);
        window_open = true;
        if (visible == 0) {
            service->end_window(service->user);
            window_open = false;
            return;
        }

        bool changed = false;
        // 第一行：开关 + 命中/失败并排，省掉一行高度。
        int enabled = enabled_.load() ? 1 : 0;
        if (HAS(service, AnomalyUiServiceV1, checkbox) &&
            service->checkbox(service->user, StringView("启用自动闪避"), &enabled)) {
            enabled_.store(enabled != 0);
            changed = true;
        }
        if (HAS(service, AnomalyUiServiceV1, same_line) &&
            HAS(service, AnomalyUiServiceV1, text)) {
            service->same_line(service->user, 0.0F, 16.0F);
            char line[96]{};
            std::snprintf(line, sizeof(line), "命中 %llu  失败 %llu",
                static_cast<unsigned long long>(trigger_count_.load()),
                static_cast<unsigned long long>(fail_count_.load()));
            service->text(service->user, StringView(line));
        }

        // 第二行：阈值滑杆。
        float threshold_value =
            static_cast<float>(threshold_milli_.load(std::memory_order_relaxed)) / 1000.0F;
        if (HAS(service, AnomalyUiServiceV1, slider_float) &&
            service->slider_float(service->user, StringView("匹配阈值"), &threshold_value,
                0.02F, 0.80F)) {
            threshold_milli_.store(static_cast<std::uint32_t>(threshold_value * 1000.0F + 0.5F));
            changed = true;
        }

        service->end_window(service->user);
        window_open = false;
        if (changed) settings_revision_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        if (window_open && HAS(service, AnomalyUiServiceV1, end_window)) {
            service->end_window(service->user);
        }
    }
}

}  // namespace anomaly::plugins::auto_dodge
