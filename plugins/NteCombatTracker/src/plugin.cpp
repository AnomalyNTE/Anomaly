#include "combat_stats_engine.hpp"
#include "character_attribute_source.hpp"
#include "combat_boundary_policy.hpp"
#include "app_settings.hpp"
#include "combat_cursor.hpp"
#include "developer_diagnostics.hpp"
#include "display_name_origin.hpp"
#include "log_worker.hpp"
#include "native_layout.hpp"
#include "uid_detector.hpp"
#include "overlay_publish_policy.hpp"
#include "overlay_protocol.hpp"
#include "overlay_control.hpp"
#include "plugin_version.hpp"

#include <nlohmann/json.hpp>
#include "sdk_compat.hpp"

#include <anomaly/sdk/anomaly_sdk.h>
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/localization.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <shellapi.h>

namespace {

using nte::combat_stats::CombatStatsEngine;
using nte::combat_stats::EncounterState;
using nte::combat_stats::Handle;
using nte::combat_stats::Snapshot;
using nte::combat_stats::TimePoint;
using nte::combat_stats::NteOverlaySnapshot;

constexpr std::size_t kMaximumDrainPerUpdate = 256;
constexpr std::size_t kMaximumResolvedNameBytes = 4096;
constexpr std::size_t kMaximumNameCacheEntries = 4096;
constexpr std::size_t kMaximumServiceGapIntervals = 64;
constexpr std::size_t kMaximumPathBytes = 32768;
// 显示名在宿主侧是延后补全的，未命中后隔这么多事件再试，避免每帧重复查询。
constexpr std::uint64_t kParticipantNameRetrySequenceSpan = 64;

struct NameCacheEntry {
    std::string value;
    std::uint32_t attempts{};
    std::uint64_t retry_after_sequence{};
};

// 开发者在诊断页观察到的 name_id → 显示名记录。名称由宿主 ABI 提供，
// 插件只缓存本次运行见到的结果，用于导出和排查。
struct ObservedNameRecord {
    std::string id_kind{"combat_name_id"};
    std::string kind;
    std::string display_name;
    std::string source_path;
    std::uint64_t observed_count{};
    std::uint64_t first_sequence{};
    std::uint64_t last_sequence{};
    std::uint64_t first_tick_sequence{};
    std::uint64_t last_tick_sequence{};
};

struct RenderState {
    std::shared_ptr<const NteOverlaySnapshot> snapshot;
    nte::damage_log::LogWorkerStatus log;
    std::string log_root;
    std::string export_root;
    std::string overlay_error;
    nte::settings::OverlaySettings overlay_settings;
    bool overlay_enabled{true};
    std::string settings_status;
    std::string settings_error;
    bool developer_available{};
    nte::developer::Snapshot developer;
    nte::account::UidDetectorDiagnostics uid_detector;
    std::string skill_capture_status;
    std::uint64_t skill_frame_sequence{};
    std::uint64_t event_cursor_sequence{};
    std::string id_name_export_status;
    std::string id_name_export_error;
    // Empty when the live character attribute table loaded. Surfaced on the
    // settings page because a silent fallback to identity colours is otherwise
    // indistinguishable from working attribute colours.
    std::string character_attribute_error;
};

enum class LogCaptureState { Idle, Recording, Closing, Failed };

struct LogSession {
    LogCaptureState state{LogCaptureState::Idle};
    Handle world;
    std::string report_id;
    std::string encounter_id;
    TimePoint report_started{};
    std::int64_t report_started_utc_ms{};
    std::int64_t encounter_started_utc_ms{};
    std::uint64_t encounter_started_offset_us{};
    std::uint64_t dropped_events{};
    std::uint64_t dropped_first_sequence{};
    std::uint64_t dropped_last_sequence{};
    std::optional<std::uint64_t> service_unavailable_started_us;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> service_unavailable_intervals;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> missing_sequence_ranges;
    std::optional<std::uint64_t> open_missing_sequence_start;
    bool missing_ranges_truncated{};
    bool service_gap_truncated{};
    std::uint64_t last_recorded_sequence{};
    std::uint64_t cutoff_sequence{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyUiServiceV1* ui{};
    CombatStatsEngine engine;
    nte::combat_stats::CombatCursor cursor;
    std::unique_ptr<nte::damage_log::DamageLogWorker> log_worker;
    std::filesystem::path log_root;
    std::optional<std::filesystem::path> export_root;
    std::unique_ptr<nte::account::UidDetector> uid_detector;
    nte::account::AccountIdentity account;
    LogSession log_session;
    std::string completed_report_id;
    std::string completed_encounter_id;
    std::unordered_map<Handle, NameCacheEntry, nte::combat_stats::HandleHash>
        participant_names;
    std::mutex mutex;
    RenderState render;
    Handle observed_world;
    std::optional<std::uint64_t> pending_cursor_gap_first;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pending_missing_sequence_ranges;
    bool pending_missing_ranges_truncated{};
    std::uint64_t pending_dropped_events{};
    std::uint64_t pending_dropped_first_sequence{};
    std::uint64_t pending_dropped_last_sequence{};
    const AnomalyWebSocketServiceV1* websocket{};
    const AnomalyCoreServiceV1* core{};
    // Resolved once in Load; the active language is fixed for the process.
    anomaly::sdk::Localizer localizer;
    std::string locale{"zh-CN"};
    // Empty when the live character table could not be read; rows then keep
    // their identity colour instead of an attribute colour.
    std::string character_attribute_error;
    bool character_attributes_ready{};
    bool character_attributes_disabled{};
    TimePoint next_character_attribute_attempt{};
    // Profile-derived compatibility snapshot; loaded once in Start (Lifecycle)
    // and only read afterwards.
    nte::native::Snapshot native;
    std::string producer_instance_id;
    std::uint64_t overlay_snapshot_sequence{};
    std::uint64_t local_overlay_drops{};
    nte::combat_stats::OverlayPublishPolicy overlay_policy;
    bool overlay_launch_attempted{};
    bool overlay_restart_pending{};
    bool toggle_overlay_requested{};
    bool open_log_root_requested{};
    bool apply_export_root_requested{};
    bool export_id_names_requested{};
    std::string requested_log_root;
    std::string requested_export_root;
    std::optional<nte::settings::OverlaySettings> requested_overlay_settings;
    std::optional<bool> requested_developer_mode;
    bool clear_debug_damage_requested{};
    bool clear_debug_skills_requested{};
    bool clear_all_debug_requested{};
    nte::settings::OverlaySettings overlay_settings;
    nte::developer::AccessState developer_access;
    bool developer_mode{};
    std::array<char, kMaximumPathBytes> log_root_input{};
    std::array<char, kMaximumPathBytes> export_root_input{};
    std::chrono::steady_clock::time_point next_settings_refresh{};
    std::string settings_status;
    std::string settings_error;
    AnomalyWebSocketServerInfoV1 overlay_server_info{sizeof(AnomalyWebSocketServerInfoV1)};
    std::string overlay_error;
    std::string control_status;
    std::string id_name_export_status;
    std::string id_name_export_error;
    HANDLE manual_end_event{};
    HANDLE plugin_stopping_event{};
    nte::developer::Diagnostics developer_diagnostics;
    std::unordered_map<std::uint64_t, ObservedNameRecord> runtime_id_names;
    std::string skill_capture_status{"disabled"};
    std::uint64_t last_skill_generation{};
    std::uint64_t last_skill_frame_sequence{};
} g_context;

// Localized status text resolved where it is produced: the language is fixed
// for the process, so the strings stored in the render state are already final.
template <typename... Args>
[[nodiscard]] std::string LocalizedStatus(const std::string_view key,
                                          const std::string_view fallback,
                                          Args... args) {
    const std::array<std::string_view, sizeof...(Args)> arguments{
        std::string_view(args)...};
    return g_context.localizer.Format(
        key, fallback, std::span<const std::string_view>(arguments));
}

NteOverlaySnapshot MakeOverlaySnapshot(
    Snapshot stats,
    std::uint32_t service_status,
    std::uint64_t latest_sequence,
    const std::string& report_id,
    const std::string& encounter_id,
    std::int64_t utc_ms,
    bool stopping,
    std::uint64_t snapshot_sequence);
void PublishOverlay(
    const Snapshot& stats,
    std::uint32_t service_status,
    std::uint64_t latest_sequence,
    const std::string& report_id,
    const std::string& encounter_id,
    std::int64_t utc_ms,
    TimePoint now,
    bool stopping,
    bool force);
std::uint32_t CaptureStatusCode(const NteOverlaySnapshot* snapshot) noexcept;
std::optional<std::filesystem::path> PluginDirectory();

bool AppendRange(
    std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
    const std::uint64_t first,
    const std::uint64_t last) {
    if (first > last) return true;
    if (!ranges.empty() && ranges.back().second !=
            (std::numeric_limits<std::uint64_t>::max)() &&
        ranges.back().second + 1 >= first) {
        ranges.back().second = (std::max)(ranges.back().second, last);
    } else if (ranges.size() < kMaximumServiceGapIntervals) {
        ranges.emplace_back(first, last);
    } else {
        return false;
    }
    return true;
}

void RecordMissingRange(const std::uint64_t first, const std::uint64_t last) {
    if (g_context.log_session.encounter_id.empty()) {
        if (!AppendRange(g_context.pending_missing_sequence_ranges, first, last)) {
            g_context.pending_missing_ranges_truncated = true;
        }
    } else {
        if (!AppendRange(g_context.log_session.missing_sequence_ranges, first, last)) {
            g_context.log_session.missing_ranges_truncated = true;
        }
    }
}

void ResetPendingCaptureGaps() {
    g_context.pending_cursor_gap_first.reset();
    g_context.pending_missing_sequence_ranges.clear();
    g_context.pending_missing_ranges_truncated = false;
    g_context.pending_dropped_events = 0;
    g_context.pending_dropped_first_sequence = 0;
    g_context.pending_dropped_last_sequence = 0;
}

bool CombatMethodsAvailable(const AnomalyNteCombatServiceV1* service) noexcept {
    return nte::sdk_compat::CombatServiceV18Available(service);
}

bool SkillMethodsAvailable(const AnomalyNteSkillsServiceV1* service) noexcept {
    return nte::sdk_compat::SkillsServiceV18Available(service);
}

Handle ConvertHandle(const AnomalyGenerationHandleV1 handle) noexcept {
    return {handle.id, handle.generation};
}

bool IsSizingStatus(const AnomalyStatusV1 status) noexcept {
    return status.code == ANOMALY_STATUS_V1_OK ||
        status.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL;
}

template <typename Resolver>
std::string ResolveName(Resolver resolver, const char* fallback) {
    std::size_t size{};
    if (!IsSizingStatus(resolver(nullptr, &size)) ||
        size <= 1 || size > kMaximumResolvedNameBytes) {
        return fallback;
    }
    std::string value(size, '\0');
    if (resolver(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return fallback;
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    return value.empty() ? fallback : value;
}

// 宿主把对象路径当成显示名返回时不算解析成功：路径只能当临时标签。
bool LooksLikeObjectPath(const std::string& value) {
    return value.starts_with("/Game/") || value.starts_with("/Script/");
}

// 事件显示名（伤害/治疗/Buff）。宿主自己完成 GE→GA→文本、怪物配置→显示名的映射，
// 因此这里不需要在插件侧维护任何字典；合法请求尚无缓存名称时返回 NOT_FOUND，
// 调用方应保留记录并在后续 Game 更新中重试。
std::string ResolveEventDisplayName(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyNteCombatEventV1& event) {
    if (!nte::sdk_compat::CombatEventNameResolverAvailable(service)) return {};
    return ResolveName(
        [service, &event](char* destination, std::size_t* size) {
            return service->event_name_utf8(
                service->user, &event, destination, size);
        }, "");
}

// 参与者显示名。未就绪时返回空串，由调用方决定退回路径还是占位符。
std::string ResolveParticipantDisplay(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyGenerationHandleV1 participant) {
    if (participant.id == 0 ||
        !nte::sdk_compat::ParticipantDisplayNameResolverAvailable(service)) {
        return {};
    }
    const std::string display = ResolveName(
        [service, participant](char* destination, std::size_t* size) {
            return service->participant_display_name_utf8(
                service->user, participant, destination, size);
        }, "");
    return LooksLikeObjectPath(display) ? std::string{} : display;
}

std::string ResolveSource(const AnomalyNteCombatServiceV1* service,
    const std::uint64_t source_id);

void RecordRuntimeIdName(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyNteCombatEventV1& event,
    const nte::combat_stats::CombatEventKind kind) {
    if (event.name_id == 0) return;
    std::string display_name = "#" + std::to_string(event.name_id);
    std::string source_path;
    if (kind == nte::combat_stats::CombatEventKind::Damage) {
        source_path = ResolveSource(service, event.name_id);
        const std::string display = ResolveEventDisplayName(service, event);
        if (!display.empty()) display_name = display;
    }

    auto& record = g_context.runtime_id_names[event.name_id];
    if (record.observed_count == 0) {
        record.id_kind = "combat_name_id";
        record.kind = nte::combat_stats::CombatEventKindName(kind);
        record.display_name = display_name;
        record.source_path = source_path;
        record.first_sequence = event.sequence;
        record.last_sequence = event.sequence;
        record.first_tick_sequence = event.tick_sequence;
        record.last_tick_sequence = event.tick_sequence;
    } else {
        if (!display_name.empty()) record.display_name = display_name;
        if (!source_path.empty()) record.source_path = source_path;
        record.last_sequence = event.sequence;
        record.last_tick_sequence = event.tick_sequence;
    }
    ++record.observed_count;
}

std::string ResolveSource(const AnomalyNteCombatServiceV1* service,
    const std::uint64_t source_id) {
    if (service == nullptr || service->source_name_utf8 == nullptr ||
        source_id == 0) return "Unknown source";
    return ResolveName(
        [service, source_id](char* destination, std::size_t* size) {
            return service->source_name_utf8(
                service->user, source_id, destination, size);
        }, "Unknown source");
}

std::string ResolveParticipant(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyGenerationHandleV1 participant) {
    if (participant.id == 0) return "Unknown participant";
    return ResolveName(
        [service, participant](char* destination, std::size_t* size) {
            return service->participant_path_utf8(
                service->user, participant, destination, size);
        }, "Unknown participant");
}

// 显示名尚未解析时的临时标签：优先对象路径，最后才用占位符。
std::string ResolveParticipantPath(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyGenerationHandleV1 participant) {
    return ResolveParticipant(service, participant);
}

// 缓存里已经有值就说明宿主给过显示名；否则这一行只是路径或占位符。
nte::combat_stats::DisplayNameOrigin ParticipantNameOrigin(
    const AnomalyGenerationHandleV1 participant) {
    const Handle handle = ConvertHandle(participant);
    if (handle.id == 0) return nte::combat_stats::DisplayNameOrigin::Unknown;
    const auto found = g_context.participant_names.find(handle);
    if (found != g_context.participant_names.end() && !found->second.value.empty()) {
        return nte::combat_stats::DisplayNameOrigin::Sdk;
    }
    return nte::combat_stats::DisplayNameOrigin::Technical;
}

// 缓存只保存宿主真正解析出来的显示名。路径查询成功也不算「已解析」，否则真名到位的
// 那一帧会被路径长期占位，历史行永远停在 /Game/... 上。
std::string ResolveParticipantCached(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyGenerationHandleV1 participant,
    const std::uint64_t sequence) {
    const Handle handle = ConvertHandle(participant);
    if (handle.id == 0) return "Unknown participant";
    auto found = g_context.participant_names.find(handle);
    if (found != g_context.participant_names.end()) {
        if (!found->second.value.empty()) return found->second.value;
        if (found->second.attempts >= 3 || sequence < found->second.retry_after_sequence) {
            return ResolveParticipantPath(service, participant);
        }
    } else if (g_context.participant_names.size() >= kMaximumNameCacheEntries) {
        return ResolveParticipantPath(service, participant);
    } else {
        found = g_context.participant_names.emplace(handle, NameCacheEntry{}).first;
    }
    const std::string display = ResolveParticipantDisplay(service, participant);
    if (!display.empty()) {
        found->second.value = display;
        return display;
    }
    ++found->second.attempts;
    found->second.retry_after_sequence = sequence + kParticipantNameRetrySequenceSpan;
    return ResolveParticipantPath(service, participant);
}

std::string ResolveAbilityPath(
    const AnomalyNteSkillsServiceV1* service,
    const AnomalyGenerationHandleV1 ability_class) {
    if (ability_class.id == 0) return "Unknown ability";
    return ResolveName(
        [service, ability_class](char* destination, std::size_t* size) {
            return service->ability_path_utf8(
                service->user, ability_class, destination, size);
        }, "Unknown ability");
}

// 技能显示名。宿主未提供该接口或尚未缓存时返回空串，调用方退回对象路径。
std::string ResolveAbilityDisplay(
    const AnomalyNteSkillsServiceV1* service,
    const AnomalyGenerationHandleV1 ability_class) {
    if (ability_class.id == 0 ||
        !nte::sdk_compat::SkillsServiceV18Available(service)) {
        return {};
    }
    const std::string display = ResolveName(
        [service, ability_class](char* destination, std::size_t* size) {
            return service->ability_display_name_utf8(
                service->user, ability_class, destination, size);
        }, "");
    return LooksLikeObjectPath(display) ? std::string{} : display;
}

nte::developer::ParticipantIdentity MakeParticipantIdentity(
    const AnomalyGenerationHandleV1 handle,
    const std::string& technical_path);

nte::developer::DamageRecord MakeDamageRecord(
    const AnomalyNteCombatEventV1& event) {
    nte::developer::DamageRecord record;
    record.sequence = event.sequence;
    record.tick_sequence = event.tick_sequence;
    record.flags = event.flags;
    record.world = ConvertHandle(event.world);
    record.attacker.handle = ConvertHandle(event.source);
    record.victim.handle = ConvertHandle(event.target);
    record.source_id = event.name_id;
    record.value = event.value;
    record.basic_value = event.basic_value;
    record.final_value = event.final_value;
    record.damage_type = event.damage_type;
    record.display_type = event.display_type;
    record.reaction_type = event.reaction_type;
    record.reaction_display_type = event.reaction_display_type;
    return record;
}

nte::developer::CombatEventRecord MakeCombatEventRecord(
    const nte::combat_stats::CombatEvent& event,
    const bool player_related,
    const bool accepted) {
    nte::developer::CombatEventRecord record;
    record.kind = event.kind;
    record.sequence = event.sequence;
    record.tick_sequence = event.tick_sequence;
    record.flags = event.flags;
    record.world = event.world;
    record.attacker.handle = event.attacker;
    record.attacker.technical_path = event.attacker_name;
    record.attacker.display_name = event.attacker_display_name;
    record.victim.handle = event.victim;
    record.victim.technical_path = event.victim_name;
    record.victim.display_name = event.victim_display_name;
    record.name_id = event.name_id;
    record.event_name = event.source_name;
    record.event_display_name = event.source_display_name;
    record.value = event.value;
    record.basic_value = event.basic_value;
    record.final_value = event.final_value;
    record.damage_type = event.damage_type;
    record.display_type = event.display_type;
    record.reaction_type = event.reaction_type;
    record.reaction_display_type = event.reaction_display_type;
    record.duration_seconds = event.duration_seconds;
    record.stack_count = event.stack_count;
    record.player_related = player_related;
    record.accepted = accepted;
    return record;
}

nte::developer::ParticipantIdentity MakeParticipantIdentity(
    const AnomalyGenerationHandleV1 handle,
    const std::string& technical_path) {
    return {ConvertHandle(handle), technical_path, technical_path, {}, {}};
}

void ResetSkillDiagnosticCursor() noexcept {
    g_context.last_skill_generation = 0;
    g_context.last_skill_frame_sequence = 0;
}

void ApplyDeveloperModeAccess() {
    const bool enabled = g_context.developer_access.enabled();
    if (enabled == g_context.developer_mode) return;
    g_context.developer_mode = enabled;
    g_context.developer_diagnostics.SetEnabled(enabled);
    g_context.skill_capture_status = enabled ?
        "waiting for skill frame" : "disabled";
    ResetSkillDiagnosticCursor();
}

void CollectMapDiagnostics(
    const anomaly::sdk::Host& host,
    const Handle combat_world) {
    if (!g_context.developer_mode) return;
    nte::developer::MapIdentity map;
    map.handle = combat_world;
    map.status = "world service unavailable";
    const auto worlds = host.Query<AnomalyUe5WorldServiceV1>(
        ANOMALY_UE5_WORLD_SERVICE_V1_ID, ANOMALY_UE5_WORLD_SERVICE_V1_VERSION);
    if (!worlds || worlds->current == nullptr || worlds->snapshot == nullptr) {
        g_context.developer_diagnostics.SetCurrentMap(
            map.handle.id == 0 ? std::nullopt : std::optional{std::move(map)});
        return;
    }
    AnomalyGenerationHandleV1 current{};
    const auto current_status = worlds->current(worlds->user, &current);
    if (current_status.code != ANOMALY_STATUS_V1_OK || current.id == 0) {
        map.status = "world current status=" +
            std::to_string(current_status.code);
        g_context.developer_diagnostics.SetCurrentMap(
            map.handle.id == 0 ? std::nullopt : std::optional{std::move(map)});
        return;
    }
    map.handle = ConvertHandle(current);
    AnomalyUe5WorldSnapshotV1 snapshot{sizeof(snapshot)};
    const auto snapshot_status = worlds->snapshot(
        worlds->user, current, &snapshot);
    if (snapshot_status.code != ANOMALY_STATUS_V1_OK) {
        map.status = "world snapshot status=" +
            std::to_string(snapshot_status.code);
        g_context.developer_diagnostics.SetCurrentMap(std::move(map));
        return;
    }
    map.name_id = snapshot.name_id;
    const auto names = host.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
    if (!names || names->resolve_utf8 == nullptr) {
        map.status = "names service unavailable";
    } else {
        map.technical_name = ResolveName(
            [service = names.get(), name_id = snapshot.name_id](
                char* destination, std::size_t* size) {
                return service->resolve_utf8(
                    service->user, name_id, destination, size);
            }, "Unknown world");
        map.status = map.technical_name == "Unknown world" ?
            "world name unavailable" : "available";
    }
    g_context.developer_diagnostics.SetCurrentMap(std::move(map));
}

void LoadCharacterAttributes(const anomaly::sdk::Host& services);

void CollectSkillDiagnostics(const anomaly::sdk::Host& host) {
    if (!g_context.developer_mode) return;
    const auto skills = host.Query<AnomalyNteSkillsServiceV1>(
        ANOMALY_NTE_SKILLS_SERVICE_V1_ID, ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION);
    if (!SkillMethodsAvailable(skills.get())) {
        g_context.skill_capture_status = "skills service unavailable";
        return;
    }
    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    const auto frame_status = skills->frame(skills->user, &frame);
    if (frame_status.code != ANOMALY_STATUS_V1_OK) {
        g_context.skill_capture_status =
            "skill frame status=" + std::to_string(frame_status.code);
        return;
    }
    if (g_context.last_skill_generation == frame.generation &&
        g_context.last_skill_frame_sequence == frame.sequence) {
        g_context.skill_capture_status = "capturing";
        return;
    }

    nte::developer::SkillFrame observed;
    observed.generation = frame.generation;
    observed.sequence = frame.sequence;
    observed.complete = (frame.flags & ANOMALY_NTE_SKILL_V1_VALID) != 0 &&
        (frame.flags & (ANOMALY_NTE_SKILL_V1_STALE |
            ANOMALY_NTE_SKILL_V1_PARTIAL)) == 0;
    std::uint32_t offset{};
    bool terminal = frame.skill_count == 0;
    bool page_failed{};
    while (!terminal && offset < frame.skill_count) {
        std::array<AnomalyNteSkillSnapshotV1,
            ANOMALY_NTE_SKILL_PAGE_V1_MAX_CAPACITY> page_items{};
        for (auto& item : page_items) item.struct_size = sizeof(item);
        AnomalyNteSkillPageRequestV1 request{sizeof(request)};
        request.generation = frame.generation;
        request.offset = offset;
        request.capacity = static_cast<std::uint32_t>(page_items.size());
        AnomalyNteSkillPageResultV1 page{sizeof(page)};
        const auto page_status = skills->page(
            skills->user, &request, page_items.data(), &page);
        if (page_status.code != ANOMALY_STATUS_V1_OK ||
            page.generation != frame.generation || page.sequence != frame.sequence ||
            page.returned > page_items.size() || page.next_offset < offset) {
            observed.complete = false;
            page_failed = true;
            g_context.skill_capture_status =
                "skill page status=" + std::to_string(page_status.code);
            break;
        }
        for (std::uint32_t index{}; index < page.returned; ++index) {
            const auto& item = page_items[index];
            const bool valid_item =
                (item.flags & ANOMALY_NTE_SKILL_V1_VALID) != 0 &&
                (item.flags & (ANOMALY_NTE_SKILL_V1_STALE |
                    ANOMALY_NTE_SKILL_V1_PARTIAL)) == 0;
            if (!valid_item) {
                observed.complete = false;
            }
            nte::developer::SkillSnapshot snapshot;
            snapshot.flags = item.flags;
            snapshot.handle = ConvertHandle(item.handle);
            snapshot.character = ConvertHandle(item.character);
            snapshot.ability_class = ConvertHandle(item.ability_class);
            snapshot.sequence = item.sequence;
            snapshot.level = item.level;
            snapshot.input_id = item.input_id;
            snapshot.cooldown_valid =
                (item.flags & ANOMALY_NTE_SKILL_V1_COOLDOWN_VALID) != 0;
            snapshot.cooldown_remaining_seconds = item.cooldown_remaining_seconds;
            snapshot.cooldown_duration_seconds = item.cooldown_duration_seconds;
            if (valid_item) {
                snapshot.ability_path = ResolveAbilityPath(skills.get(), item.ability_class);
                // 技能显示名同样来自宿主，未就绪时退回对象路径。
                const std::string ability_display =
                    ResolveAbilityDisplay(skills.get(), item.ability_class);
                snapshot.ability_display_name = ability_display.empty()
                    ? snapshot.ability_path
                    : ability_display;
            }
            observed.skills.push_back(std::move(snapshot));
        }
        terminal = page.next_offset >= page.total_skills || page.returned == 0;
        if (!terminal && page.next_offset == offset) {
            observed.complete = false;
            break;
        }
        offset = page.next_offset;
    }
    observed.complete = observed.complete && terminal &&
        observed.skills.size() == frame.skill_count;
    const bool complete = observed.complete;
    g_context.developer_diagnostics.ObserveSkillFrame(std::move(observed));
    if (page_failed) return;
    g_context.last_skill_generation = frame.generation;
    g_context.last_skill_frame_sequence = frame.sequence;
    g_context.skill_capture_status = complete ? "capturing" : "partial frame";
}

std::int64_t UtcNowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::uint64_t ReportOffsetMicroseconds(const TimePoint now) {
    if (g_context.log_session.report_started == TimePoint{}) return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - g_context.log_session.report_started).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

std::string ResolveBuildId(const anomaly::sdk::Host& host) {
    const auto service = host.Query<AnomalyNteBuildServiceV1>(
        ANOMALY_NTE_BUILD_SERVICE_V1_ID, ANOMALY_NTE_BUILD_SERVICE_V1_VERSION);
    if (!service || service->build_id == nullptr) return "unknown";
    return ResolveName([service = service.get()](char* destination, std::size_t* size) {
        return service->build_id(service->user, destination, size);
    }, "unknown");
}

std::string ResolveRuntimeVersion(const anomaly::sdk::Host& host) {
    const auto service = host.Query<AnomalyRuntimeInfoServiceV1>(
        ANOMALY_RUNTIME_INFO_SERVICE_V1_ID, ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION);
    if (!service || service->runtime_version_utf8 == nullptr) return "unknown";
    return ResolveName([service = service.get()](char* destination, std::size_t* size) {
        return service->runtime_version_utf8(service->user, destination, size);
    }, "unknown");
}

void RestartLogWorkerStorage() {
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    if (!g_context.log_root.empty()) {
        g_context.log_worker = std::make_unique<nte::damage_log::DamageLogWorker>(
            g_context.log_root);
        static_cast<void>(g_context.log_worker->Start());
    }
}

void RollbackLogOpenFailure(const std::string& error) {
    RestartLogWorkerStorage();
    g_context.log_session = {};
    g_context.participant_names.clear();
    g_context.control_status = "log_open_failed";
    g_context.settings_error = error.empty() ? "Log initialization failed" : error;
}

void RestartLogWorkerAfterFailure(const std::string& error) {
    if (!g_context.log_session.report_id.empty()) {
        g_context.completed_report_id = g_context.log_session.report_id;
        g_context.completed_encounter_id = g_context.log_session.encounter_id;
    }
    RestartLogWorkerStorage();
    g_context.log_session = {};
    g_context.engine.MarkPartial(false);
    g_context.engine.CompleteEncounter();
    g_context.participant_names.clear();
    g_context.control_status = "log_transaction_failed";
    g_context.settings_error = error.empty() ? "Log transaction failed" : error;
}

bool OpenEncounterLog(
    const anomaly::sdk::Host& host,
    const Handle world,
    const TimePoint now,
    const std::int64_t utc_ms) {
    if (!g_context.log_worker ||
        g_context.log_session.state != LogCaptureState::Idle) return false;
    g_context.control_status.clear();
    g_context.completed_report_id.clear();
    g_context.completed_encounter_id.clear();
    auto& session = g_context.log_session;
    session.report_id = nte::damage_log::GenerateUuidV4();
    if (session.report_id.empty()) return false;
    session.world = world;
    session.report_started = now;
    session.report_started_utc_ms = utc_ms;
    session.encounter_id = nte::damage_log::GenerateUuidV4();
    if (session.encounter_id.empty()) {
        session = {};
        return false;
    }
    session.encounter_started_utc_ms = utc_ms;
    session.encounter_started_offset_us = ReportOffsetMicroseconds(now);
    session.dropped_events = 0;
    session.dropped_first_sequence = 0;
    session.dropped_last_sequence = 0;
    session.dropped_events = std::exchange(g_context.pending_dropped_events, 0);
    session.dropped_first_sequence = std::exchange(
        g_context.pending_dropped_first_sequence, 0);
    session.dropped_last_sequence = std::exchange(
        g_context.pending_dropped_last_sequence, 0);
    session.service_unavailable_started_us.reset();
    session.service_unavailable_intervals.clear();
    session.missing_sequence_ranges.clear();
    session.missing_sequence_ranges = std::move(
        g_context.pending_missing_sequence_ranges);
    g_context.pending_missing_sequence_ranges.clear();
    session.open_missing_sequence_start = std::exchange(
        g_context.pending_cursor_gap_first, std::nullopt);
    session.missing_ranges_truncated = g_context.pending_missing_ranges_truncated;
    g_context.pending_missing_ranges_truncated = false;
    session.service_gap_truncated = false;
    session.last_recorded_sequence = 0;
    session.cutoff_sequence = 0;
    const std::string game_build = ResolveBuildId(host);
    if (!g_context.log_worker->Submit(nte::damage_log::ReportOpen{
            session.report_id, world, utc_ms, game_build,
            nte::build::kPluginVersion, ResolveRuntimeVersion(host),
            g_context.account})) {
        session = {};
        return false;
    }
    if (!g_context.log_worker->Submit(nte::damage_log::EncounterOpen{
            session.encounter_id, utc_ms, session.encounter_started_offset_us})) {
        g_context.pending_dropped_events += session.dropped_events;
        if (g_context.pending_dropped_first_sequence == 0 ||
            (session.dropped_first_sequence != 0 &&
             session.dropped_first_sequence < g_context.pending_dropped_first_sequence)) {
            g_context.pending_dropped_first_sequence = session.dropped_first_sequence;
        }
        g_context.pending_dropped_last_sequence = (std::max)(
            g_context.pending_dropped_last_sequence, session.dropped_last_sequence);
        for (const auto range : session.missing_sequence_ranges) {
            if (!AppendRange(g_context.pending_missing_sequence_ranges,
                    range.first, range.second)) {
                g_context.pending_missing_ranges_truncated = true;
            }
        }
        if (session.open_missing_sequence_start) {
            g_context.pending_cursor_gap_first = session.open_missing_sequence_start;
        }
        RollbackLogOpenFailure("EncounterOpen could not be submitted");
        return false;
    }
    std::string flush_error;
    if (!g_context.log_worker->FlushAndWait(
            std::chrono::seconds(5), flush_error)) {
        RollbackLogOpenFailure(flush_error);
        return false;
    }
    session.state = LogCaptureState::Recording;
    return true;
}

bool FinishEncounterLog(
    const char* reason,
    const NteOverlaySnapshot& overlay_snapshot,
    const TimePoint now,
    const std::int64_t utc_ms) {
    const Snapshot& snapshot = overlay_snapshot.combat;
    auto& session = g_context.log_session;
    if (!g_context.log_worker || session.encounter_id.empty() ||
        session.state != LogCaptureState::Recording) return false;
    session.state = LogCaptureState::Closing;
    session.cutoff_sequence = session.last_recorded_sequence;
    Snapshot closing_snapshot = snapshot;
    closing_snapshot.state = EncounterState::Closing;
    PublishOverlay(closing_snapshot, CaptureStatusCode(&overlay_snapshot),
        overlay_snapshot.latest_event_sequence, session.report_id,
        session.encounter_id, utc_ms, now, false, true);
    auto quality = snapshot.quality;
    const std::uint64_t closed_offset_us = ReportOffsetMicroseconds(now);
    if (session.service_unavailable_started_us) {
        if (session.service_unavailable_intervals.size() < kMaximumServiceGapIntervals) {
            session.service_unavailable_intervals.emplace_back(
                *session.service_unavailable_started_us, closed_offset_us);
        } else {
            session.service_gap_truncated = true;
        }
        session.service_unavailable_started_us.reset();
    }
    if (session.dropped_events != 0 || !session.service_unavailable_intervals.empty() ||
        session.service_gap_truncated || session.open_missing_sequence_start ||
        session.missing_ranges_truncated) quality.partial = true;
    nte::damage_log::EncounterClose close{
        session.encounter_id, reason, utc_ms, closed_offset_us,
        snapshot.outgoing_damage, snapshot.incoming_damage,
        snapshot.encounter_dps, snapshot.duration_seconds, quality,
        session.dropped_events, session.dropped_first_sequence,
        session.dropped_last_sequence, {}, session.service_unavailable_intervals,
        session.missing_sequence_ranges, session.open_missing_sequence_start,
        session.service_gap_truncated, session.missing_ranges_truncated,
        snapshot.players};
    close.critical_data_status = snapshot.critical_data_status;
    close.damage_flag_histogram = snapshot.damage_flag_histogram;
    close.outgoing_healing = snapshot.outgoing_healing;
    close.incoming_healing = snapshot.incoming_healing;
    close.outgoing_heal_events = snapshot.outgoing_heal_events;
    close.incoming_heal_events = snapshot.incoming_heal_events;
    close.encounter_hps = snapshot.encounter_hps;
    close.encounter_received_hps = snapshot.encounter_received_hps;
    close.cutoff_sequence = session.cutoff_sequence;
    const std::string report_id = session.report_id;
    const std::string encounter_id = session.encounter_id;
    if (!g_context.log_worker->Submit(std::move(close))) {
        session.state = LogCaptureState::Failed;
        RestartLogWorkerAfterFailure("EncounterClose could not be submitted");
        return false;
    }
    std::string flush_error;
    if (!g_context.log_worker->FlushAndWait(
            std::chrono::seconds(5), flush_error)) {
        session.state = LogCaptureState::Failed;
        RestartLogWorkerAfterFailure(flush_error);
        return false;
    }
    if (!g_context.log_worker->Submit(
            nte::damage_log::ReportClose{reason, utc_ms, false}) ||
        !g_context.log_worker->FlushAndWait(
            std::chrono::seconds(5), flush_error)) {
        session.state = LogCaptureState::Failed;
        RestartLogWorkerAfterFailure(flush_error.empty()
            ? "Report could not be closed" : flush_error);
        return false;
    }
    g_context.completed_report_id = report_id;
    g_context.completed_encounter_id = encounter_id;
    session = {};
    g_context.participant_names.clear();
    ResetPendingCaptureGaps();
    return true;
}

void UpdateServiceGap(const bool available, const TimePoint now) {
    auto& session = g_context.log_session;
    if (session.encounter_id.empty()) return;
    const std::uint64_t offset = ReportOffsetMicroseconds(now);
    if (!available) {
        if (!session.service_unavailable_started_us) {
            session.service_unavailable_started_us = offset;
        }
        return;
    }
    if (!session.service_unavailable_started_us) return;
    if (session.service_unavailable_intervals.size() < kMaximumServiceGapIntervals) {
        session.service_unavailable_intervals.emplace_back(
            *session.service_unavailable_started_us, offset);
    } else {
        session.service_gap_truncated = true;
    }
    session.service_unavailable_started_us.reset();
}

bool FinishReportLog(const char* reason, const std::int64_t utc_ms) {
    auto& session = g_context.log_session;
    if (session.report_id.empty()) return true;
    if (!g_context.log_worker || !g_context.log_worker->Submit(
            nte::damage_log::ReportClose{reason, utc_ms, false})) {
        return false;
    }
    std::string flush_error;
    const bool flushed = g_context.log_worker->FlushAndWait(
        std::chrono::seconds(5), flush_error);
    if (!flushed) {
        RestartLogWorkerAfterFailure(flush_error);
        return false;
    }
    session = {};
    g_context.participant_names.clear();
    ResetPendingCaptureGaps();
    return true;
}

const char* CaptureStatusName(const std::uint32_t status) noexcept {
    switch (status) {
    case ANOMALY_STATUS_V1_OK: return "available";
    case ANOMALY_STATUS_V1_UNAVAILABLE: return "unavailable";
    case ANOMALY_STATUS_V1_PERMISSION_DENIED: return "permission_denied";
    default: return "error";
    }
}

std::uint32_t CaptureStatusCode(const NteOverlaySnapshot* snapshot) noexcept {
    if (snapshot == nullptr) return ANOMALY_STATUS_V1_UNAVAILABLE;
    if (snapshot->capture_status == "available") return ANOMALY_STATUS_V1_OK;
    if (snapshot->capture_status == "permission_denied") {
        return ANOMALY_STATUS_V1_PERMISSION_DENIED;
    }
    if (snapshot->capture_status == "unavailable") return ANOMALY_STATUS_V1_UNAVAILABLE;
    return ANOMALY_STATUS_V1_FAILED;
}

NteOverlaySnapshot MakeOverlaySnapshot(
    Snapshot stats,
    const std::uint32_t service_status,
    const std::uint64_t latest_sequence,
    const std::string& report_id,
    const std::string& encounter_id,
    const std::int64_t utc_ms,
    const bool stopping,
    const std::uint64_t snapshot_sequence) {
    NteOverlaySnapshot::Data data;
    data.producer_instance_id = g_context.producer_instance_id;
    data.snapshot_sequence = snapshot_sequence;
    data.latest_event_sequence = latest_sequence;
    data.sent_utc_ms = utc_ms;
    data.producer_stopping = stopping;
    data.capture_available = service_status == ANOMALY_STATUS_V1_OK;
    data.capture_status = CaptureStatusName(service_status);
    data.healing_capture_status = "unavailable";
    data.control_status = g_context.control_status;
    data.account = g_context.account;
    data.report_id = report_id;
    data.encounter_id = encounter_id;
    if (!stats.outgoing_targets.empty()) {
        data.primary_target = stats.outgoing_targets.front().label;
    }
    const auto& server = g_context.overlay_server_info;
    data.websocket_port = server.port;
    data.connected_clients = server.connected_clients;
    data.published_messages = server.published_messages;
    data.transport_dropped_messages =
        server.dropped_messages + g_context.local_overlay_drops;
    data.locale = g_context.locale;
    data.combat = std::move(stats);
    return NteOverlaySnapshot(std::move(data));
}

std::optional<std::filesystem::path> PluginDirectory() {
    if (g_context.core == nullptr || g_context.core->plugin_directory == nullptr) {
        return std::nullopt;
    }
    std::size_t size{};
    if (!IsSizingStatus(g_context.core->plugin_directory(
            g_context.core->user, nullptr, &size)) || size <= 1 || size > 32768) {
        return std::nullopt;
    }
    std::string utf8(size, '\0');
    if (g_context.core->plugin_directory(
            g_context.core->user, utf8.data(), &size).code != ANOMALY_STATUS_V1_OK) {
        return std::nullopt;
    }
    if (const auto end = utf8.find('\0'); end != std::string::npos) utf8.resize(end);
    if (utf8.empty()) return std::nullopt;
    const int wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_size <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), wide.data(), wide_size) != wide_size) {
        return std::nullopt;
    }
    return std::filesystem::path(std::move(wide));
}

bool LaunchOverlay(
    const std::uint16_t port,
    const bool restart,
    std::string& error_message) {
    error_message.clear();
    if (port == 0) {
        error_message = "WebSocket listener is unavailable";
        return false;
    }
    const auto package = PluginDirectory();
    if (!package) {
        error_message = "Plugin directory is unavailable";
        return false;
    }
    std::error_code error;
    const std::filesystem::path root = std::filesystem::weakly_canonical(*package, error);
    if (error) {
        error_message = "Plugin directory could not be resolved";
        return false;
    }
    const std::filesystem::path executable = std::filesystem::weakly_canonical(
        root / L"NteCombatTracker.exe", error);
    if (error || executable.parent_path() != root ||
        !std::filesystem::is_regular_file(executable, error) || error) {
        error_message = "NteCombatTracker.exe is missing or outside the plugin package";
        return false;
    }
    // 本插件以动态 CRT（/MD）构建，包目录里不带 CRT 副本。CreateProcessW 启动的子进程由
    // Windows 按默认搜索顺序解析导入表，包目录不在其中；系统目录缺少的 CRT 组件会触发
    // 加载器弹窗，或让子进程走开发者模式绕行。这里先把这些模块从包目录解析进当前进程，
    // 子进程再继承同一批已加载实例，就不必依赖那两条路径。
    // 依赖集由构建配置决定，是固定常量；单个模块缺失不阻断启动，仍交给 CreateProcessW 报告。
    for (const wchar_t* const module : {L"msvcp140.dll", L"msvcp140_atomic_wait.dll",
             L"vcruntime140.dll", L"vcruntime140_1.dll"}) {
        LoadLibraryExW((root / module).c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS);
    }
    std::wstring command = L"\"" + executable.wstring() + L"\" --port " +
        std::to_wstring(port) + L" --host-pid " +
        std::to_wstring(GetCurrentProcessId());
    if (restart) command += L" --restart";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr,
            FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, root.c_str(), &startup, &process)) {
        error_message = "CreateProcessW failed: " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

HWND OverlayWindow() {
    return FindWindowW(nte::combat_stats::overlay::kWindowClassName,
        nte::combat_stats::overlay::kWindowTitle);
}

void NotifyOverlaySettingsChanged() {
    const HWND window = OverlayWindow();
    const UINT message = RegisterWindowMessageW(
        nte::combat_stats::overlay::kReloadSettingsMessageName);
    if (window != nullptr && message != 0) PostMessageW(window, message, 0, 0);
}

void CloseOverlayWindow() {
    const HWND window = OverlayWindow();
    const UINT message = RegisterWindowMessageW(
        nte::combat_stats::overlay::kCloseOverlayMessageName);
    if (window != nullptr && message != 0) PostMessageW(window, message, 0, 0);
}

void CopyLogRootToInput(const std::filesystem::path& path) {
    const std::string utf8 = nte::settings::PathToUtf8(path);
    std::scoped_lock lock(g_context.mutex);
    std::fill(g_context.log_root_input.begin(), g_context.log_root_input.end(), '\0');
    const std::size_t count = (std::min)(
        utf8.size(), g_context.log_root_input.size() - 1);
    std::copy_n(utf8.data(), count, g_context.log_root_input.data());
}

void CopyExportRootToInput(
    const std::optional<std::filesystem::path>& path) {
    const std::string utf8 = path ? nte::settings::PathToUtf8(*path) : std::string{};
    std::scoped_lock lock(g_context.mutex);
    std::fill(g_context.export_root_input.begin(),
        g_context.export_root_input.end(), '\0');
    const std::size_t count = (std::min)(
        utf8.size(), g_context.export_root_input.size() - 1);
    std::copy_n(utf8.data(), count, g_context.export_root_input.data());
}

void QueueIdNameExport() {
    std::scoped_lock lock(g_context.mutex);
    g_context.export_id_names_requested = true;
}

bool ExportIdNameMapping(
    std::filesystem::path& exported_path,
    std::string& error) {
    error.clear();
    exported_path.clear();
    if (!g_context.export_root) {
        error = "Configure an export directory on the settings page first";
        return false;
    }
    if (!nte::settings::PrepareLogRoot(*g_context.export_root, error)) {
        return false;
    }

    // 名称全部来自宿主 ABI，插件不再持有静态字典；这里导出本次运行实际观测到的
    // name_id → 显示名映射，以及每个 id 的来源路径和出现次数。
    nlohmann::json names = nlohmann::json::array();
    for (const auto& [id, record] : g_context.runtime_id_names) {
        names.push_back({
            {"id", std::to_string(id)},
            {"kind", record.kind},
            {"display_name", record.display_name},
            {"source_path", record.source_path},
            {"observed_count", record.observed_count},
            {"first_sequence", record.first_sequence},
            {"last_sequence", record.last_sequence}});
    }
    std::sort(names.begin(), names.end(),
        [](const nlohmann::json& left, const nlohmann::json& right) {
            return left.at("id").get_ref<const std::string&>() <
                right.at("id").get_ref<const std::string&>();
        });
    const nlohmann::json document = {
        {"format", "nte-combat-id-names"},
        {"schema_version", 1},
        {"generated_by", nte::build::kPluginVersion},
        {"source", "anomaly.nte.combat event_name_utf8"},
        {"names", std::move(names)}};

    std::error_code filesystem_error;
    std::filesystem::create_directories(*g_context.export_root, filesystem_error);
    if (filesystem_error) {
        error = "Export directory is unavailable: " + filesystem_error.message();
        return false;
    }
    exported_path = *g_context.export_root / "nte-combat-id-names.json";
    const auto text = document.dump(2) + "\n";
    const auto staging = exported_path.string() + ".tmp";
    {
        std::ofstream stream(staging, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Export file could not be opened";
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            error = "Export file could not be written";
            return false;
        }
    }
    std::filesystem::rename(staging, exported_path, filesystem_error);
    if (filesystem_error) {
        std::error_code remove_error;
        std::filesystem::remove(staging, remove_error);
        error = "Export file could not be published";
        return false;
    }
    return true;
}

bool WaitUntilReady(nte::damage_log::DamageLogWorker& worker, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = worker.Status();
        if (status.ready) return true;
        if (!status.running || !status.last_error.empty()) {
            error = status.last_error.empty() ? "Log worker is not running" : status.last_error;
            return false;
        }
        Sleep(10);
    }
    error = "Timed out waiting for the log directory to initialize";
    return false;
}

bool SwitchLogRoot(
    const std::filesystem::path& requested,
    const TimePoint now,
    const std::int64_t utc_ms,
    std::string& error) {
    error.clear();
    if (!nte::settings::PrepareLogRoot(requested, error)) return false;
    std::error_code filesystem_error;
    const auto normalized = std::filesystem::weakly_canonical(requested, filesystem_error);
    const auto target = filesystem_error ? requested.lexically_normal() : normalized;
    filesystem_error.clear();
    const auto current_normalized = std::filesystem::weakly_canonical(
        g_context.log_root, filesystem_error);
    const auto current = filesystem_error ?
        g_context.log_root.lexically_normal() : current_normalized;
    if (target == current) {
        nte::settings::PluginSettings settings;
        if (!nte::settings::UpdatePluginLogRoot(
                nte::settings::PluginSettingsPath(), target,
                settings, error)) return false;
        g_context.log_root = target;
        CopyLogRootToInput(target);
        return true;
    }
    auto candidate = std::make_unique<nte::damage_log::DamageLogWorker>(target);
    if (!candidate->Start() || !WaitUntilReady(*candidate, error)) {
        candidate->Stop();
        if (error.empty()) error = "Log worker could not start for the new directory";
        return false;
    }
    std::string load_error;
    auto settings = nte::settings::LoadPluginSettings(
        nte::settings::PluginSettingsPath(), g_context.log_root, load_error);
    settings.log_root = target;
    auto pending_settings = nte::settings::StagePluginSettings(
        nte::settings::PluginSettingsPath(), settings, error);
    if (!pending_settings) {
        candidate->Stop();
        return false;
    }

    const bool active = g_context.engine.state() == EncounterState::Active;
    const bool has_open_encounter = !g_context.log_session.encounter_id.empty();
    if (has_open_encounter && !FinishEncounterLog("storage_path_changed",
            MakeOverlaySnapshot(g_context.engine.BuildSnapshot(now),
                CaptureStatusCode(g_context.render.snapshot.get()),
                g_context.render.snapshot ?
                    g_context.render.snapshot->latest_event_sequence : 0,
                g_context.log_session.report_id,
                g_context.log_session.encounter_id, utc_ms, false,
                g_context.overlay_snapshot_sequence), now, utc_ms)) {
        candidate->Stop();
        if (active) {
            g_context.engine.Reset();
            g_context.cursor.RequestReset();
            ResetPendingCaptureGaps();
        }
        error = "Current combat log could not be closed cleanly; directory unchanged";
        return false;
    }
    if (!FinishReportLog("storage_path_changed", utc_ms)) {
        candidate->Stop();
        if (active) {
            g_context.engine.Reset();
            g_context.cursor.RequestReset();
            ResetPendingCaptureGaps();
        }
        error = "Previous report could not be closed cleanly; directory unchanged";
        return false;
    }

    if (!pending_settings->Commit(error)) {
        candidate->Stop();
        if (active) {
            g_context.engine.Reset();
            g_context.cursor.RequestReset();
            ResetPendingCaptureGaps();
        }
        return false;
    }

    auto previous = std::move(g_context.log_worker);
    g_context.log_worker = std::move(candidate);
    g_context.log_root = target;
    if (previous) previous->Stop();
    if (active) {
        g_context.engine.Reset();
        g_context.cursor.RequestReset();
        ResetPendingCaptureGaps();
    }
    CopyLogRootToInput(target);
    return true;
}

AnomalyWebSocketServerInfoV1 QueryOverlayServerInfo() {
    AnomalyWebSocketServerInfoV1 info{sizeof(info)};
    if (g_context.websocket == nullptr || g_context.websocket->server_info == nullptr ||
        g_context.websocket->server_info(g_context.websocket->user, &info).code !=
            ANOMALY_STATUS_V1_OK) {
        info.port = 0;
    }
    return info;
}

std::uint64_t OverlayUrgencyRevision(
    const Snapshot& stats,
    const std::uint32_t service_status,
    const std::string& report_id,
    const std::string& encounter_id,
    const bool stopping) noexcept {
    std::uint64_t value = 1469598103934665603ULL;
    const auto mix = [&value](const std::uint64_t item) {
        value ^= item;
        value *= 1099511628211ULL;
    };
    mix(service_status);
    mix(static_cast<std::uint64_t>(stats.state));
    mix(stats.quality.partial);
    mix(stats.quality.overflow);
    mix(stats.quality.cursor_gap);
    mix(stats.quality.aggregate_truncated);
    mix(stopping);
    for (const unsigned char character : report_id) mix(character);
    mix(0xffU);
    for (const unsigned char character : encounter_id) mix(character);
    return value;
}

void PublishOverlay(
    const Snapshot& stats,
    const std::uint32_t service_status,
    const std::uint64_t latest_sequence,
    const std::string& report_id,
    const std::string& encounter_id,
    const std::int64_t utc_ms,
    const TimePoint now,
    const bool stopping = false,
    const bool force = false) {
    if (!g_context.overlay_policy.ShouldPoll(now, force || stopping)) return;
    g_context.overlay_server_info = QueryOverlayServerInfo();
    const auto& server = g_context.overlay_server_info;
    g_context.overlay_policy.ObserveServer(now, server.connected_clients);
    if (!stopping && server.port == 0) {
        g_context.overlay_error = "WebSocket listener is unavailable";
        return;
    }
    if (!stopping && server.port != 0 && !g_context.overlay_launch_attempted) {
        g_context.overlay_launch_attempted = true;
        static_cast<void>(LaunchOverlay(
            server.port, g_context.overlay_restart_pending, g_context.overlay_error));
        g_context.overlay_restart_pending = false;
    }
    if (g_context.websocket == nullptr || g_context.websocket->publish_text == nullptr ||
        server.port == 0) return;
    const std::uint64_t state_revision = OverlayUrgencyRevision(
        stats, service_status, report_id, encounter_id, stopping);
    if (!g_context.overlay_policy.ShouldPublish(
            now, state_revision, force || stopping)) return;
    const NteOverlaySnapshot message = MakeOverlaySnapshot(
        stats, service_status, latest_sequence, report_id, encounter_id,
        utc_ms, stopping, ++g_context.overlay_snapshot_sequence);
    const std::string json = nte::combat_stats::SerializeOverlaySnapshot(message);
    if (json.size() > nte::combat_stats::kMaximumOverlayMessageBytes ||
        g_context.websocket->publish_text(
            g_context.websocket->user, anomaly::sdk::StringView(json)).code !=
            ANOMALY_STATUS_V1_OK) {
        ++g_context.local_overlay_drops;
    }
    g_context.overlay_policy.RecordPublished(now, state_revision);
}

void ResetState() {
    g_context.engine.Reset();
    g_context.cursor.Clear();
    g_context.log_session = {};
    g_context.uid_detector.reset();
    g_context.native = {};
    g_context.character_attributes_disabled = false;
    g_context.account = nte::account::Unknown();
    g_context.participant_names.clear();
    g_context.observed_world = {};
    ResetPendingCaptureGaps();
    g_context.producer_instance_id.clear();
    g_context.overlay_snapshot_sequence = 0;
    g_context.local_overlay_drops = 0;
    g_context.overlay_policy.Reset();
    g_context.overlay_launch_attempted = false;
    g_context.overlay_restart_pending = false;
    g_context.toggle_overlay_requested = false;
    g_context.open_log_root_requested = false;
    g_context.apply_export_root_requested = false;
    g_context.export_id_names_requested = false;
    g_context.requested_log_root.clear();
    g_context.requested_export_root.clear();
    g_context.requested_overlay_settings.reset();
    g_context.requested_developer_mode.reset();
    g_context.overlay_settings = {};
    g_context.developer_access = {};
    g_context.developer_mode = false;
    g_context.clear_debug_damage_requested = false;
    g_context.clear_debug_skills_requested = false;
    g_context.clear_all_debug_requested = false;
    g_context.log_root_input.fill('\0');
    g_context.export_root_input.fill('\0');
    g_context.next_settings_refresh = {};
    g_context.settings_status.clear();
    g_context.settings_error.clear();
    g_context.overlay_server_info = {sizeof(AnomalyWebSocketServerInfoV1)};
    g_context.overlay_error.clear();
    g_context.control_status.clear();
    g_context.id_name_export_status.clear();
    g_context.id_name_export_error.clear();
    g_context.runtime_id_names.clear();
    g_context.developer_diagnostics.SetEnabled(false);
    g_context.skill_capture_status = "disabled";
    ResetSkillDiagnosticCursor();
    std::scoped_lock lock(g_context.mutex);
    g_context.render = {};
}

void CloseControlHandles() {
    if (g_context.manual_end_event != nullptr) {
        CloseHandle(g_context.manual_end_event);
        g_context.manual_end_event = nullptr;
    }
    if (g_context.plugin_stopping_event != nullptr) {
        CloseHandle(g_context.plugin_stopping_event);
        g_context.plugin_stopping_event = nullptr;
    }
}

void Update(void* context, double) {
    if (context != &g_context || g_context.host == nullptr) return;
    const auto now = nte::combat_stats::Clock::now();
    const std::int64_t utc_ms = UtcNowMilliseconds();
    const anomaly::sdk::Host host(g_context.host);
    const auto combat = host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);

    if (g_context.uid_detector) {
        const auto detected = g_context.uid_detector->Update();
        const auto diagnostics = g_context.uid_detector->diagnostics();
        g_context.developer_access.ObserveAccount(
            diagnostics.current_observation_valid ? detected : std::nullopt);
        ApplyDeveloperModeAccess();
        if (nte::account::ShouldTransition(g_context.account, detected)) {
            bool closed = true;
            if (!g_context.log_session.encounter_id.empty()) {
                closed = FinishEncounterLog("uid_changed", MakeOverlaySnapshot(
                    g_context.engine.BuildSnapshot(now),
                    CaptureStatusCode(g_context.render.snapshot.get()),
                    g_context.render.snapshot
                        ? g_context.render.snapshot->latest_event_sequence : 0,
                    g_context.log_session.report_id,
                    g_context.log_session.encounter_id, utc_ms, false,
                    g_context.overlay_snapshot_sequence), now, utc_ms);
            }
            if (closed) closed = FinishReportLog("uid_changed", utc_ms);
            if (closed) {
                g_context.account = *detected;
                g_context.completed_report_id.clear();
                g_context.completed_encounter_id.clear();
                g_context.engine.Reset();
                g_context.cursor.RequestReset();
                ResetPendingCaptureGaps();
                g_context.control_status = "uid_changed";
            }
        }
    }

    const bool combat_methods_available = CombatMethodsAvailable(combat.get());
    bool preflight_available{};
    if (combat_methods_available) {
        AnomalyNteCombatantSnapshotV1 preflight{sizeof(preflight)};
        const auto preflight_status = combat->current_combatant(
            combat->user, &preflight);
        preflight_available = preflight_status.code == ANOMALY_STATUS_V1_OK &&
            (preflight.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 &&
            (preflight.flags & ANOMALY_NTE_COMBATANT_V1_STALE) == 0;
    }
    UpdateServiceGap(preflight_available, now);

    const bool manual_end_requested = g_context.manual_end_event != nullptr &&
        WaitForSingleObject(g_context.manual_end_event, 0) == WAIT_OBJECT_0;
    bool toggle_overlay_requested{};
    bool open_log_root_requested{};
    bool apply_export_root_requested{};
    bool export_id_names_requested{};
    std::string requested_log_root;
    std::string requested_export_root;
    std::optional<nte::settings::OverlaySettings> requested_overlay_settings;
    std::optional<bool> requested_developer_mode;
    bool clear_debug_damage_requested{};
    bool clear_debug_skills_requested{};
    bool clear_all_debug_requested{};
    {
        std::scoped_lock lock(g_context.mutex);
        toggle_overlay_requested = std::exchange(
            g_context.toggle_overlay_requested, false);
        open_log_root_requested = std::exchange(
            g_context.open_log_root_requested, false);
        apply_export_root_requested = std::exchange(
            g_context.apply_export_root_requested, false);
        export_id_names_requested = std::exchange(
            g_context.export_id_names_requested, false);
        requested_log_root = std::move(g_context.requested_log_root);
        g_context.requested_log_root.clear();
        requested_export_root = std::move(g_context.requested_export_root);
        g_context.requested_export_root.clear();
        requested_overlay_settings = std::move(
            g_context.requested_overlay_settings);
        g_context.requested_overlay_settings.reset();
        requested_developer_mode = std::exchange(
            g_context.requested_developer_mode, std::nullopt);
        clear_debug_damage_requested = std::exchange(
            g_context.clear_debug_damage_requested, false);
        clear_debug_skills_requested = std::exchange(
            g_context.clear_debug_skills_requested, false);
        clear_all_debug_requested = std::exchange(
            g_context.clear_all_debug_requested, false);
    }
    const auto authorized_developer_request = requested_developer_mode
        ? g_context.developer_access.AuthorizePreferenceRequest(
            *requested_developer_mode)
        : std::nullopt;

    if (g_context.developer_mode && clear_all_debug_requested) {
        g_context.developer_diagnostics.ClearAll();
        ResetSkillDiagnosticCursor();
    } else if (g_context.developer_mode) {
        if (clear_debug_damage_requested) {
            g_context.developer_diagnostics.ClearDamage();
        }
        if (clear_debug_skills_requested) {
            g_context.developer_diagnostics.ClearSkills();
            ResetSkillDiagnosticCursor();
        }
    }

    if (authorized_developer_request) {
        nte::settings::PluginSettings merged;
        std::string error;
        if (nte::settings::UpdatePluginDeveloperMode(
                nte::settings::PluginSettingsPath(), g_context.log_root,
                *authorized_developer_request, merged, error)) {
            g_context.developer_access.SetSavedPreference(
                merged.developer_mode);
            ApplyDeveloperModeAccess();
            g_context.settings_status = LocalizedStatus(
                merged.developer_mode ? "settings.status.developer_enabled"
                                      : "settings.status.developer_disabled",
                merged.developer_mode ? "Developer mode enabled"
                                      : "Developer mode disabled");
            g_context.settings_error.clear();
        } else {
            g_context.settings_error = "Failed to save developer mode: " + error;
        }
    }

    if (requested_overlay_settings) {
        std::string error;
        nte::settings::OverlaySettings merged;
        if (nte::settings::UpdateOverlayPreferences(
                nte::settings::OverlaySettingsPath(),
                *requested_overlay_settings, merged, error)) {
            g_context.overlay_settings = merged;
            g_context.settings_status = LocalizedStatus(
                "settings.status.overlay_applied", "Overlay settings applied");
            g_context.settings_error.clear();
            NotifyOverlaySettingsChanged();
        } else {
            g_context.settings_error = "Failed to save overlay settings: " + error;
        }
    }

    if (toggle_overlay_requested) {
        const auto action = nte::combat_stats::ResolveOverlayToggleAction(
            g_context.overlay_policy.enabled(),
            !g_context.overlay_error.empty());
        if (action == nte::combat_stats::OverlayToggleAction::Retry) {
            g_context.overlay_launch_attempted = false;
            g_context.overlay_error.clear();
            g_context.settings_status = LocalizedStatus(
                "settings.status.overlay_retry", "Retrying overlay launch");
        } else if (action == nte::combat_stats::OverlayToggleAction::Disable) {
            g_context.overlay_policy.SetEnabled(false);
            g_context.overlay_launch_attempted = false;
            CloseOverlayWindow();
            g_context.settings_status = LocalizedStatus(
                "settings.status.overlay_closed",
                "Overlay closed (it still starts automatically next session)");
        } else {
            g_context.overlay_policy.SetEnabled(true);
            g_context.overlay_launch_attempted = false;
            g_context.overlay_restart_pending = false;
            g_context.overlay_error.clear();
            g_context.settings_status = LocalizedStatus(
                "settings.status.overlay_starting", "Starting overlay");
        }
        g_context.settings_error.clear();
    }

    if (!requested_log_root.empty()) {
        const auto path = nte::settings::PathFromUtf8(requested_log_root);
        std::string error;
        if (!path) {
            g_context.settings_error = "Storage path is not valid UTF-8";
        } else if (SwitchLogRoot(*path, now, utc_ms, error)) {
            g_context.settings_status = LocalizedStatus(
                "settings.status.storage_applied",
                "Statistics storage path applied");
            g_context.settings_error = std::move(error);
        } else {
            g_context.settings_error = std::move(error);
        }
    }

    if (open_log_root_requested) {
        std::string error;
        if (!nte::settings::PrepareLogRoot(g_context.log_root, error)) {
            g_context.settings_error = "Could not open the data folder: " + error;
        } else if (reinterpret_cast<std::intptr_t>(ShellExecuteW(
                nullptr, L"open", g_context.log_root.c_str(), nullptr,
                nullptr, SW_SHOWNORMAL)) <= 32) {
            g_context.settings_error = "Could not open the data folder";
        }
    }

    if (apply_export_root_requested) {
        std::optional<std::filesystem::path> export_root;
        std::string error;
        if (!requested_export_root.empty()) {
            export_root = nte::settings::PathFromUtf8(requested_export_root);
            if (!export_root) {
                g_context.settings_error = "Export directory is not valid UTF-8";
            } else if (!nte::settings::PrepareLogRoot(*export_root, error)) {
                g_context.settings_error = "Export directory is unavailable: " + error;
                export_root.reset();
            }
        }
        if (requested_export_root.empty() || export_root) {
            nte::settings::PluginSettings merged;
            if (nte::settings::UpdatePluginExportRoot(
                    nte::settings::PluginSettingsPath(), g_context.log_root,
                    export_root, merged, error)) {
                g_context.export_root = merged.export_root;
                CopyExportRootToInput(g_context.export_root);
                g_context.settings_status = export_root
                    ? LocalizedStatus("settings.status.export_applied",
                          "Export directory applied")
                    : LocalizedStatus("settings.status.export_cleared",
                          "Export directory cleared");
                g_context.settings_error.clear();
            } else {
                g_context.settings_error = "Failed to save the export directory: " + error;
            }
        }
    }

    if (g_context.developer_mode && export_id_names_requested) {
        std::filesystem::path exported_path;
        std::string error;
        if (ExportIdNameMapping(exported_path, error)) {
            g_context.id_name_export_status = LocalizedStatus(
                "developer.status.export_ok", "ID to name mapping exported to: {0}",
                nte::settings::PathToUtf8(exported_path));
            g_context.id_name_export_error.clear();
        } else {
            g_context.id_name_export_status.clear();
            g_context.id_name_export_error = LocalizedStatus(
                "developer.error.export", "Failed to export the ID to name mapping: {0}",
                error);
        }
    }

    if (now >= g_context.next_settings_refresh &&
        !requested_overlay_settings) {
        nte::settings::OverlaySettings refreshed = g_context.overlay_settings;
        std::string error;
        if (nte::settings::LoadOverlaySettings(
                nte::settings::OverlaySettingsPath(), refreshed, error)) {
            g_context.overlay_settings = refreshed;
        } else if (!error.empty()) {
            g_context.settings_error = "Failed to read overlay settings: " + error;
        }
        std::string plugin_error;
        const auto plugin_settings = nte::settings::LoadPluginSettings(
            nte::settings::PluginSettingsPath(), g_context.log_root, plugin_error);
        if (plugin_error.empty() &&
            plugin_settings.export_root != g_context.export_root) {
            g_context.export_root = plugin_settings.export_root;
            CopyExportRootToInput(g_context.export_root);
        }
        if (plugin_error.empty() &&
            plugin_settings.developer_mode !=
                g_context.developer_access.saved_preference() &&
            !authorized_developer_request) {
            g_context.developer_access.SetSavedPreference(
                plugin_settings.developer_mode);
            ApplyDeveloperModeAccess();
        }
        g_context.next_settings_refresh = now + std::chrono::seconds(1);
    }

    if (manual_end_requested) {
        bool may_reset = true;
        if (!g_context.log_session.encounter_id.empty()) {
            may_reset = FinishEncounterLog(
                "manual_end",
                MakeOverlaySnapshot(
                g_context.engine.BuildSnapshot(now),
                CaptureStatusCode(g_context.render.snapshot.get()),
                g_context.render.snapshot ?
                    g_context.render.snapshot->latest_event_sequence : 0,
                g_context.log_session.report_id,
                g_context.log_session.encounter_id, utc_ms, false,
                g_context.overlay_snapshot_sequence), now, utc_ms);
        }
        g_context.control_status = may_reset
            ? "manual_end_completed" : "manual_end_failed";
        if (may_reset) {
            g_context.engine.Reset();
            g_context.cursor.RequestReset();
            ResetPendingCaptureGaps();
        }
    }

    std::uint32_t service_status = ANOMALY_STATUS_V1_UNAVAILABLE;
    std::uint64_t latest{};
    bool sampled_partial{};
    bool sampled_overflow{};
    bool cursor_gap{};
    bool capture_available{};
    if (combat_methods_available) {
        latest = combat->latest_event_sequence(combat->user);
        const auto cursor_decision = g_context.cursor.BeginFrame(latest);
        cursor_gap = cursor_decision.cursor_gap;
        if (cursor_decision.reset_applied) {
            bool reset_engine = true;
            if (g_context.log_session.state == LogCaptureState::Recording) {
                g_context.engine.MarkPartial(true);
                g_context.engine.CompleteEncounter();
                reset_engine = FinishEncounterLog("cursor_reset",
                    MakeOverlaySnapshot(g_context.engine.BuildSnapshot(now),
                        ANOMALY_STATUS_V1_UNAVAILABLE, latest,
                        g_context.log_session.report_id,
                        g_context.log_session.encounter_id, utc_ms, false,
                        g_context.overlay_snapshot_sequence), now, utc_ms);
            }
            if (reset_engine) g_context.engine.Reset();
        }

        AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
        const AnomalyStatusV1 status = combat->current_combatant(
            combat->user, &combatant);
        service_status = status.code;
        const bool valid_combatant = status.code == ANOMALY_STATUS_V1_OK &&
            (combatant.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0 &&
            (combatant.flags & ANOMALY_NTE_COMBATANT_V1_STALE) == 0;
        capture_available = valid_combatant;
        if (!valid_combatant) {
            g_context.developer_diagnostics.SetCurrentTarget(std::nullopt);
        }
        if (valid_combatant) {
            const Handle observed_world = ConvertHandle(combatant.world);
            if (g_context.observed_world.id != 0 &&
                observed_world != g_context.observed_world) {
                ResetPendingCaptureGaps();
            }
            g_context.observed_world = observed_world;
            if (g_context.developer_mode && combatant.target.id != 0) {
                const auto target_path = ResolveParticipantCached(
                    combat.get(), combatant.target, combatant.sequence);
                g_context.developer_diagnostics.SetCurrentTarget(
                    MakeParticipantIdentity(combatant.target, target_path));
            } else {
                g_context.developer_diagnostics.SetCurrentTarget(std::nullopt);
            }
            if (!g_context.log_session.report_id.empty() &&
                observed_world != g_context.log_session.world) {
                if (!g_context.log_session.encounter_id.empty()) {
                    FinishEncounterLog("world_changed", MakeOverlaySnapshot(
                        g_context.engine.BuildSnapshot(now), service_status, latest,
                        g_context.log_session.report_id,
                        g_context.log_session.encounter_id, utc_ms, false,
                        g_context.overlay_snapshot_sequence), now, utc_ms);
                }
                FinishReportLog("world_changed", utc_ms);
            }
            g_context.engine.ObserveCombatant({
                observed_world, ConvertHandle(combatant.character),
                combatant.hp, combatant.max_hp, combatant.shield}, now);
            if ((combatant.flags & ANOMALY_NTE_COMBATANT_V1_PARTIAL) != 0) {
                sampled_partial = true;
            }
            if (combat->statistics != nullptr) {
                for (const std::uint32_t direction : {
                         ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_ATTACKER,
                         ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_VICTIM}) {
                    AnomalyNteCombatStatisticsRequestV1 request{sizeof(request)};
                    request.world = combatant.world;
                    request.character = combatant.character;
                    request.direction = direction;
                    AnomalyNteCombatStatisticsV1 statistics{sizeof(statistics)};
                    if (combat->statistics(combat->user, &request, &statistics).code ==
                        ANOMALY_STATUS_V1_OK) {
                        if ((statistics.flags & ANOMALY_NTE_COMBAT_STATISTICS_V1_PARTIAL) != 0) {
                            sampled_partial = true;
                        }
                        if ((statistics.flags & ANOMALY_NTE_COMBAT_STATISTICS_V1_OVERFLOW) != 0) {
                            sampled_overflow = true;
                        }
                    }
                }
            }
        }

        if (valid_combatant) {
            for (std::size_t count{}; count < kMaximumDrainPerUpdate; ++count) {
                if (g_context.cursor.value() == latest) break;
                AnomalyNteCombatEventV1 event{sizeof(event)};
                const AnomalyStatusV1 event_status = combat->next_event(
                    combat->user, g_context.cursor.value(), &event);
                if (event_status.code == ANOMALY_STATUS_V1_OK) {
                    if (!nte::sdk_compat::CombatEventV18Available(&event)) {
                        service_status = ANOMALY_STATUS_V1_UNAVAILABLE;
                        capture_available = false;
                        g_context.engine.MarkPartial(false);
                        break;
                    }
                    const auto accept = g_context.cursor.AcceptNext(event.sequence);
                    if (!accept.accepted) {
                        service_status = ANOMALY_STATUS_V1_FAILED;
                        capture_available = false;
                        break;
                    }
                    auto& open_gap = g_context.log_session.encounter_id.empty()
                        ? g_context.pending_cursor_gap_first
                        : g_context.log_session.open_missing_sequence_start;
                    if (open_gap) {
                        if (event.sequence > *open_gap) {
                            RecordMissingRange(*open_gap,
                                event.sequence - 1);
                        }
                        open_gap.reset();
                    } else if (accept.cursor_gap) {
                        RecordMissingRange(accept.missing_first, accept.missing_last);
                        cursor_gap = true;
                        sampled_overflow = true;
                    }
                    nte::combat_stats::CombatEvent normalized;
                    switch (event.kind) {
                    case ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE:
                        normalized.kind = nte::combat_stats::CombatEventKind::Damage;
                        break;
                    case ANOMALY_NTE_COMBAT_EVENT_V1_HEAL:
                        normalized.kind = nte::combat_stats::CombatEventKind::Heal;
                        break;
                    case ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD:
                        normalized.kind = nte::combat_stats::CombatEventKind::BuffAdd;
                        break;
                    case ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE:
                        normalized.kind = nte::combat_stats::CombatEventKind::BuffRemove;
                        break;
                    default:
                        g_context.engine.MarkPartial(false);
                        continue;
                    }
                    normalized.sequence = event.sequence;
                    normalized.tick_sequence = event.tick_sequence;
                    normalized.flags = event.flags;
                    normalized.world = ConvertHandle(event.world);
                    normalized.attacker = ConvertHandle(event.source);
                    normalized.victim = ConvertHandle(event.target);
                    normalized.name_id = event.name_id;
                    normalized.value = event.value;
                    normalized.basic_value = event.basic_value;
                    normalized.final_value = event.final_value;
                    normalized.damage_type = event.damage_type;
                    normalized.display_type = event.display_type;
                    normalized.reaction_type = event.reaction_type;
                    normalized.reaction_display_type = event.reaction_display_type;
                    normalized.duration_seconds = event.duration_seconds;
                    normalized.stack_count = event.stack_count;
                    if (g_context.developer_mode) {
                        RecordRuntimeIdName(combat.get(), event, normalized.kind);
                    }
                    if ((event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_PARTIAL) != 0) {
                        sampled_partial = true;
                    }
                    normalized.received_utc_ms = utc_ms;
                    normalized.received_offset_us = ReportOffsetMicroseconds(now);
                    // 事件名三级回退：宿主显示名 → 来源标识 → name_id 占位符。
                    // 显示名由宿主在 Game 更新中延后补全，未就绪时保留记录并让后续事件重试。
                    {
                        const std::string display =
                            ResolveEventDisplayName(combat.get(), event);
                        if (!display.empty()) {
                            normalized.source_display_name = display;
                            normalized.source_name = display;
                            normalized.source_name_origin = std::string(
                                nte::combat_stats::DisplayNameOriginName(
                                    nte::combat_stats::DisplayNameOrigin::Sdk));
                        } else if (event.name_id == 0) {
                            normalized.source_name = "Unknown event";
                            normalized.source_display_name = normalized.source_name;
                            normalized.source_name_origin = std::string(
                                nte::combat_stats::DisplayNameOriginName(
                                    nte::combat_stats::DisplayNameOrigin::Unknown));
                        } else {
                            const std::string source_path =
                                ResolveSource(combat.get(), event.name_id);
                            const bool has_path = source_path != "Unknown source";
                            normalized.source_name = has_path
                                ? source_path
                                : "#" + std::to_string(event.name_id);
                            normalized.source_display_name = normalized.source_name;
                            normalized.source_name_origin = std::string(
                                nte::combat_stats::DisplayNameOriginName(
                                    nte::combat_stats::DisplayNameOrigin::Technical));
                        }
                    }
                    normalized.attacker_name = ResolveParticipantCached(
                        combat.get(), event.source, event.sequence);
                    normalized.victim_name = ResolveParticipantCached(
                        combat.get(), event.target, event.sequence);
                    // The attribute lookup needs the object path, not the display
                    // name; keep both so one does not starve the other.
                    normalized.attacker_path = ResolveParticipantPath(
                        combat.get(), event.source);
                    normalized.victim_path = ResolveParticipantPath(
                        combat.get(), event.target);
                    // ResolveParticipantCached 已经优先返回宿主显示名，路径只作为
                    // 显示名未就绪时的临时标签，因此这两者直接就是最终展示文本。
                    normalized.attacker_display_name = normalized.attacker_name;
                    normalized.victim_display_name = normalized.victim_name;
                    normalized.attacker_name_origin = std::string(
                        nte::combat_stats::DisplayNameOriginName(
                            ParticipantNameOrigin(event.source)));
                    normalized.victim_name_origin = std::string(
                        nte::combat_stats::DisplayNameOriginName(
                            ParticipantNameOrigin(event.target)));
                    const bool player_related = g_context.engine.IsRelevant(
                        normalized.world, normalized.attacker, normalized.victim);
                    const bool starts_encounter = player_related &&
                        normalized.kind == nte::combat_stats::CombatEventKind::Damage &&
                        normalized.final_value > 0;
                    if (starts_encounter && g_context.log_session.encounter_id.empty() &&
                        !OpenEncounterLog(host, normalized.world, now, utc_ms)) {
                        // Do not activate an in-memory encounter that has no
                        // durable timeline. The next related event retries the
                        // open after a transient storage failure.
                        ++g_context.pending_dropped_events;
                        if (g_context.pending_dropped_first_sequence == 0) {
                            g_context.pending_dropped_first_sequence = normalized.sequence;
                        }
                        g_context.pending_dropped_last_sequence = normalized.sequence;
                        if (!AppendRange(g_context.pending_missing_sequence_ranges,
                                normalized.sequence, normalized.sequence)) {
                            g_context.pending_missing_ranges_truncated = true;
                        }
                        g_context.engine.MarkPartial(false);
                        g_context.engine.MarkOverflow();
                        continue;
                    }
                    const bool accepted = g_context.engine.AcceptEvent(normalized, now);
                    g_context.developer_diagnostics.RecordCombatEvent(
                        MakeCombatEventRecord(normalized, player_related, accepted));
                    if (normalized.kind == nte::combat_stats::CombatEventKind::Damage) {
                        auto debug_record = MakeDamageRecord(event);
                        debug_record.player_related = player_related;
                        debug_record.accepted = accepted;
                        debug_record.attacker = MakeParticipantIdentity(
                            event.source, normalized.attacker_name);
                        debug_record.victim = MakeParticipantIdentity(
                            event.target, normalized.victim_name);
                        g_context.developer_diagnostics.RecordDamage(
                            std::move(debug_record));
                    }
                    if (accepted &&
                        g_context.engine.state() == EncounterState::Active &&
                        g_context.log_worker &&
                        g_context.log_session.state == LogCaptureState::Recording &&
                        !g_context.log_session.encounter_id.empty() &&
                        !g_context.log_worker->Submit(nte::damage_log::LoggedCombatEvent{
                            normalized, utc_ms, normalized.received_offset_us,
                            player_related, g_context.log_session.encounter_id})) {
                        auto& session = g_context.log_session;
                        ++session.dropped_events;
                        if (session.dropped_first_sequence == 0) {
                            session.dropped_first_sequence = normalized.sequence;
                        }
                        session.dropped_last_sequence = normalized.sequence;
                        if (!AppendRange(session.missing_sequence_ranges,
                                normalized.sequence, normalized.sequence)) {
                            session.missing_ranges_truncated = true;
                        }
                        g_context.engine.MarkPartial(false);
                        g_context.engine.MarkOverflow();
                    } else if (accepted &&
                        g_context.engine.state() == EncounterState::Active &&
                        g_context.log_worker &&
                        g_context.log_session.state == LogCaptureState::Recording &&
                        !g_context.log_session.encounter_id.empty()) {
                        g_context.log_session.last_recorded_sequence = normalized.sequence;
                    }
                    continue;
                }
                if (event_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
                    const std::uint64_t missing_after = g_context.cursor.value();
                    if (g_context.cursor.RecoverFromNotFound(latest)) {
                        if (missing_after !=
                            (std::numeric_limits<std::uint64_t>::max)()) {
                            if (g_context.log_session.encounter_id.empty()) {
                                g_context.pending_cursor_gap_first = missing_after + 1;
                            } else {
                                g_context.log_session.open_missing_sequence_start =
                                    missing_after + 1;
                            }
                        }
                        cursor_gap = true;
                        sampled_overflow = true;
                        continue;
                    }
                }
                if (event_status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
                    service_status = event_status.code;
                    capture_available = false;
                }
                break;
            }
        }
        if (sampled_partial || cursor_gap) g_context.engine.MarkPartial(cursor_gap);
        if (sampled_overflow) g_context.engine.MarkOverflow();
    } else {
        static_cast<void>(g_context.cursor.BeginFrame(std::nullopt));
        g_context.developer_diagnostics.SetCurrentTarget(std::nullopt);
    }

    CollectMapDiagnostics(host,
        capture_available ? g_context.observed_world : Handle{});
    CollectSkillDiagnostics(host);
    LoadCharacterAttributes(host);

    UpdateServiceGap(capture_available && service_status == ANOMALY_STATUS_V1_OK, now);

    const auto boundary_action = nte::combat_stats::PlanCombatBoundaryAction(
        g_context.engine.state(),
        g_context.log_session.state == LogCaptureState::Recording,
        combat_methods_available);
    if (boundary_action == nte::combat_stats::CombatBoundaryAction::CompleteForServiceLoss) {
        g_context.engine.MarkPartial(false);
        g_context.engine.CompleteEncounter();
        if (!FinishEncounterLog("service_unavailable", MakeOverlaySnapshot(
                g_context.engine.BuildSnapshot(now),
                ANOMALY_STATUS_V1_UNAVAILABLE,
                g_context.render.snapshot
                    ? g_context.render.snapshot->latest_event_sequence : 0,
                g_context.log_session.report_id,
                g_context.log_session.encounter_id, utc_ms, false,
                g_context.overlay_snapshot_sequence), now, utc_ms)) {
            g_context.control_status = "service_close_failed";
        }
    } else if (boundary_action ==
               nte::combat_stats::CombatBoundaryAction::AdvanceForInactivity) {
        g_context.engine.Advance(now);
        if (!g_context.log_session.encounter_id.empty() &&
            g_context.engine.state() == EncounterState::Completed) {
            FinishEncounterLog("inactivity", MakeOverlaySnapshot(
                g_context.engine.BuildSnapshot(now),
                CaptureStatusCode(g_context.render.snapshot.get()),
                g_context.render.snapshot ?
                    g_context.render.snapshot->latest_event_sequence : 0,
                g_context.log_session.report_id,
                g_context.log_session.encounter_id, utc_ms, false,
                g_context.overlay_snapshot_sequence), now, utc_ms);
        }
    }

    RenderState next;
    Snapshot stats = g_context.engine.BuildSnapshot(now);
    if (g_context.log_worker) next.log = g_context.log_worker->Status();
    next.log_root = nte::settings::PathToUtf8(g_context.log_root);
    next.export_root = g_context.export_root ?
        nte::settings::PathToUtf8(*g_context.export_root) : std::string{};
    const std::string report_id = stats.state == EncounterState::Completed &&
            !g_context.completed_encounter_id.empty()
        ? g_context.completed_report_id : g_context.log_session.report_id;
    const std::string encounter_id = stats.state == EncounterState::Completed
        ? g_context.completed_encounter_id : g_context.log_session.encounter_id;
    PublishOverlay(stats, service_status, latest, report_id,
        encounter_id, utc_ms, now);
    next.snapshot = std::make_shared<const NteOverlaySnapshot>(
        MakeOverlaySnapshot(std::move(stats), service_status, latest,
            report_id, encounter_id, utc_ms, false,
            g_context.overlay_snapshot_sequence));
    next.overlay_error = g_context.overlay_error;
    next.overlay_settings = g_context.overlay_settings;
    next.overlay_enabled = g_context.overlay_policy.enabled();
    next.settings_status = g_context.settings_status;
    next.settings_error = g_context.settings_error;
    next.character_attribute_error = g_context.character_attribute_error;
    next.developer_available = g_context.developer_access.available();
    next.developer = g_context.developer_diagnostics.BuildSnapshot();
    if (g_context.uid_detector) {
        next.uid_detector = g_context.uid_detector->diagnostics();
        if (g_context.account.known()) {
            next.uid_detector.active_account_uid = g_context.account.uid;
        }
    }
    next.skill_capture_status = g_context.skill_capture_status;
    next.skill_frame_sequence = g_context.last_skill_frame_sequence;
    next.event_cursor_sequence = g_context.cursor.value();
    next.id_name_export_status = g_context.id_name_export_status;
    next.id_name_export_error = g_context.id_name_export_error;
    std::scoped_lock lock(g_context.mutex);
    g_context.render = std::move(next);
}

void Text(const AnomalyUiServiceV1* ui, const std::string& value) {
    ui->text(ui->user, anomaly::sdk::StringView(value));
}

// Localizer arguments are strings; numbers go through this so a format call
// never needs a printf specifier.
[[nodiscard]] std::string FormatUnsigned(const unsigned long long value) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%llu", value);
    return buffer.data();
}

// Button labels go through the UI service as a view, so the translated string
// is owned here for the duration of the call rather than by a temporary.
class ScopedLabel final {
public:
    ScopedLabel(const std::string_view catalog_key, const std::string_view fallback)
        : text_(g_context.localizer.Text(catalog_key, fallback)) {}

    [[nodiscard]] AnomalyStringViewV1 View() const noexcept {
        return anomaly::sdk::StringView(std::string_view(text_));
    }

private:
    std::string text_;
};

// Localized UI text. The key and the English fallback are given together so a
// missing catalog or a placeholder mismatch silently yields the fallback, which
// is the Host's documented behaviour; Chinese exists only in locales/*.json.
template <typename... Args>
void TextLocalized(const AnomalyUiServiceV1* ui, const std::string_view key,
                   const std::string_view fallback, Args... args) {
    const std::array<std::string_view, sizeof...(Args)> arguments{
        std::string_view(args)...};
    const std::string text = g_context.localizer.Format(
        key, fallback, std::span<const std::string_view>(arguments));
    Text(ui, text.c_str());
}

// Formats a value that is not user copy (an identifier, a path, a number).
template <typename... Args>
void TextFormat(const AnomalyUiServiceV1* ui, const char* format, Args... args) {
    std::array<char, 512> buffer{};
    std::snprintf(buffer.data(), buffer.size(), format, args...);
    Text(ui, buffer.data());
}

const char* UidPhaseName(const nte::account::UidDetectorPhase phase) noexcept {
    switch (phase) {
    case nte::account::UidDetectorPhase::WaitingForServices:
        return "awaiting service";
    case nte::account::UidDetectorPhase::WaitingForBindings:
        return "awaiting signatures";
    case nte::account::UidDetectorPhase::Scanning:
        return "scanning objects";
    case nte::account::UidDetectorPhase::WidgetPending:
        return "awaiting UID widget";
    case nte::account::UidDetectorPhase::Detected:
        return "resolved";
    }
    return "unknown";
}

const char* SkillChangeName(const nte::developer::SkillChangeKind kind) noexcept {
    switch (kind) {
    case nte::developer::SkillChangeKind::Added: return "added";
    case nte::developer::SkillChangeKind::Changed: return "changed";
    case nte::developer::SkillChangeKind::Removed: return "removed";
    }
    return "unknown";
}

void QueueDebugClear(const bool damage, const bool skills, const bool all) {
    std::scoped_lock lock(g_context.mutex);
    g_context.clear_debug_damage_requested =
        g_context.clear_debug_damage_requested || damage;
    g_context.clear_debug_skills_requested =
        g_context.clear_debug_skills_requested || skills;
    g_context.clear_all_debug_requested =
        g_context.clear_all_debug_requested || all;
}

void DrawDeveloperPage(
    const AnomalyUiServiceV1* ui, const RenderState& render) {
    const auto& uid = render.uid_detector;
    TextLocalized(ui, "developer.account.heading", "Account and UID detection");
    TextLocalized(ui, "developer.account.active_uid", "Active account UID: {0}",
        uid.active_account_uid.empty() ? "unresolved" : uid.active_account_uid.c_str());
    TextLocalized(ui, "developer.account.detected_uid", "Detected UID: {0} / phase: {1}",
        uid.detected_uid.empty() ? "unresolved" : uid.detected_uid.c_str(),
        UidPhaseName(uid.phase));
    TextLocalized(ui, "developer.account.missing", "Missing services: {0} / missing signatures: {1}",
        uid.missing_services.empty() ? "none" : uid.missing_services.c_str(),
        uid.missing_bindings.empty() ? "none" : uid.missing_bindings.c_str());
    TextLocalized(ui, "developer.account.objects", "Objects generation={0} count={1} / scan={2}/{3} / widgets={4}",
        FormatUnsigned(uid.object_generation), FormatUnsigned(uid.object_count),
        FormatUnsigned(uid.scan_cursor), FormatUnsigned(uid.scan_start),
        FormatUnsigned(uid.tracked_widgets));
    TextLocalized(ui, "developer.account.reads", "Reads ok={0} failed={1} / last failure phase: {2} / {3}",
        FormatUnsigned(uid.successful_reads), FormatUnsigned(uid.failed_reads),
        UidPhaseName(uid.last_failure_phase),
        uid.last_failure.empty() ? "none" : uid.last_failure.c_str());
    TextLocalized(ui, "developer.account.retry", "Retry status: {0} / next={1}",
        uid.retry_status.empty() ? "none" : uid.retry_status.c_str(),
        FormatUnsigned(uid.next_retry_tick));

    if (ui->separator != nullptr) ui->separator(ui->user);
    TextLocalized(ui, "developer.capture.heading", "Capture status");
    if (render.developer.current_map) {
        const auto& map = *render.developer.current_map;
        TextLocalized(ui, "developer.map.line", "Current map: name={0} / name_id={1} / handle={2}:{3} / status={4}",
            map.technical_name.empty() ? "unresolved" : map.technical_name.c_str(),
            FormatUnsigned(map.name_id),
            FormatUnsigned(map.handle.id),
            FormatUnsigned(map.handle.generation),
            map.status.c_str());
    } else {
        TextLocalized(ui, "developer.map.unavailable", "Current map: unavailable");
    }
    if (render.developer.current_target) {
        const auto& target = *render.developer.current_target;
        TextLocalized(ui, "developer.target.line", "Current target: {0} / monster_id={1} / config_id={2} / handle={3}:{4}",
            target.display_name.empty() ? "unresolved" : target.display_name.c_str(),
            target.monster_id.empty() ? "unresolved" : target.monster_id.c_str(),
            target.enemy_config_id.empty() ? "unresolved" :
                target.enemy_config_id.c_str(),
            FormatUnsigned(target.handle.id),
            FormatUnsigned(target.handle.generation));
        TextLocalized(ui, "developer.target.path", "  target_path={0}",
            target.technical_path.empty() ? "unresolved" :
                target.technical_path.c_str());
    } else {
        TextLocalized(ui, "developer.target.none", "Current target: none");
    }
    if (render.snapshot) {
        TextLocalized(ui, "developer.combat.status", "Combat status={0} · latest={1} · cursor={2} · report={3} · encounter={4}",
            render.snapshot->capture_status.c_str(),
            FormatUnsigned(render.snapshot->latest_event_sequence),
            FormatUnsigned(render.event_cursor_sequence),
            render.snapshot->report_id.empty() ? "none" : render.snapshot->report_id.c_str(),
            render.snapshot->encounter_id.empty() ? "none" :
                render.snapshot->encounter_id.c_str());
    } else {
        TextLocalized(ui, "developer.combat.unpublished", "Combat snapshot has not been published yet");
    }
    TextLocalized(ui, "developer.log.worker", "Log worker running={0} ready={1} queue={2} dropped={3}",
        FormatUnsigned(render.log.running ? 1 : 0),
        FormatUnsigned(render.log.ready ? 1 : 0),
        FormatUnsigned(render.log.queue_depth),
        FormatUnsigned(render.log.dropped_events));
    TextLocalized(ui, "developer.skills.snapshot", "Skill snapshot: {0} / frame={1}",
        render.skill_capture_status.c_str(),
        FormatUnsigned(render.skill_frame_sequence));
    TextLocalized(ui, "developer.skills.note", "Skill records come from read-only snapshot deltas, not an authoritative cast stream.");

    if (ui->button != nullptr) {
        if (ui->button(ui->user, ScopedLabel("developer.button.clear_damage", "Clear damage log").View(),
                0, 0)) {
            QueueDebugClear(true, false, false);
        }
        if (nte::sdk_compat::HasField<AnomalyUiServiceV1,
                decltype(AnomalyUiServiceV1::same_line)>(
                ui, offsetof(AnomalyUiServiceV1, same_line)) &&
            ui->same_line != nullptr) ui->same_line(ui->user, 0, -1);
        if (ui->button(ui->user, ScopedLabel("developer.button.clear_skills", "Clear skill log").View(),
                0, 0)) {
            QueueDebugClear(false, true, false);
        }
        if (nte::sdk_compat::HasField<AnomalyUiServiceV1,
                decltype(AnomalyUiServiceV1::same_line)>(
                ui, offsetof(AnomalyUiServiceV1, same_line)) &&
            ui->same_line != nullptr) ui->same_line(ui->user, 0, -1);
        if (ui->button(ui->user, ScopedLabel("developer.button.clear_all", "Clear all").View(), 0, 0)) {
            QueueDebugClear(false, false, true);
        }
    }

    TextLocalized(ui, "developer.export.heading", "ID to name mapping export");
    TextLocalized(ui, "developer.export.note", "Export the static full mapping plus the SDK name_id mappings observed in this run.");
    if (ui->button != nullptr && ui->button(ui->user,
            ScopedLabel("developer.button.export_names", "Export ID to name mapping").View(), 0, 0)) {
        QueueIdNameExport();
    }
    if (!render.id_name_export_status.empty()) {
        Text(ui, render.id_name_export_status.c_str());
    }
    if (!render.id_name_export_error.empty()) {
        Text(ui, render.id_name_export_error.c_str());
    }

    if (ui->separator != nullptr) ui->separator(ui->user);
    TextLocalized(ui, "developer.damage.heading", "Raw damage events ({0}/256, overwritten {1})",
        FormatUnsigned(render.developer.damage_records.size()),
        FormatUnsigned(render.developer.damage_overwritten));
    const bool has_children = ui->begin_child != nullptr && ui->end_child != nullptr;
    const bool damage_child = !has_children || ui->begin_child(
        ui->user, anomaly::sdk::StringView("debug-damage-events"), 0, 180, 0);
    if (damage_child) {
        for (auto iterator = render.developer.damage_records.rbegin();
             iterator != render.developer.damage_records.rend(); ++iterator) {
            const auto& event = *iterator;
            TextFormat(ui,
                "seq=%llu tick=%llu flags=0x%08X source=%llu damage=%lld/%lld/%lld related=%d accepted=%d",
                static_cast<unsigned long long>(event.sequence),
                static_cast<unsigned long long>(event.tick_sequence), event.flags,
                static_cast<unsigned long long>(event.source_id),
                static_cast<long long>(event.value),
                static_cast<long long>(event.basic_value),
                static_cast<long long>(event.final_value),
                event.player_related ? 1 : 0, event.accepted ? 1 : 0);
            TextFormat(ui,
                "  world=%llu:%llu attacker=%llu:%llu victim=%llu:%llu types=%u/%u reaction=%u/%u hit=(%.2f,%.2f,%.2f)",
                static_cast<unsigned long long>(event.world.id),
                static_cast<unsigned long long>(event.world.generation),
                static_cast<unsigned long long>(event.attacker.handle.id),
                static_cast<unsigned long long>(event.attacker.handle.generation),
                static_cast<unsigned long long>(event.victim.handle.id),
                static_cast<unsigned long long>(event.victim.handle.generation),
                event.damage_type, event.display_type, event.reaction_type,
                event.reaction_display_type, event.hit_location[0],
                event.hit_location[1], event.hit_location[2]);
            TextFormat(ui,
                "  attacker=%s monster_id=%s config_id=%s path=%s",
                event.attacker.display_name.empty() ? "unresolved" :
                    event.attacker.display_name.c_str(),
                event.attacker.monster_id.empty() ? "not a monster / unresolved" :
                    event.attacker.monster_id.c_str(),
                event.attacker.enemy_config_id.empty() ? "not a monster / unresolved" :
                    event.attacker.enemy_config_id.c_str(),
                event.attacker.technical_path.empty() ? "unresolved" :
                    event.attacker.technical_path.c_str());
            TextFormat(ui,
                "  victim=%s monster_id=%s config_id=%s path=%s",
                event.victim.display_name.empty() ? "unresolved" :
                    event.victim.display_name.c_str(),
                event.victim.monster_id.empty() ? "not a monster / unresolved" :
                    event.victim.monster_id.c_str(),
                event.victim.enemy_config_id.empty() ? "not a monster / unresolved" :
                    event.victim.enemy_config_id.c_str(),
                event.victim.technical_path.empty() ? "unresolved" :
                    event.victim.technical_path.c_str());
        }
    }
    if (has_children) ui->end_child(ui->user);

    TextLocalized(ui, "developer.combat.heading", "Unified combat events ({0}/256, overwritten {1})",
        FormatUnsigned(render.developer.combat_event_records.size()),
        FormatUnsigned(render.developer.combat_event_overwritten));
    const bool event_child = !has_children || ui->begin_child(
        ui->user, anomaly::sdk::StringView("debug-combat-events"), 0, 220, 0);
    if (event_child) {
        for (auto iterator = render.developer.combat_event_records.rbegin();
             iterator != render.developer.combat_event_records.rend(); ++iterator) {
            const auto& event = *iterator;
            if (event.kind != nte::combat_stats::CombatEventKind::BuffAdd &&
                event.kind != nte::combat_stats::CombatEventKind::BuffRemove) continue;
            TextFormat(ui,
                "%s seq=%llu tick=%llu name=%llu value=%lld/%lld/%lld duration=%.2f stack=%d related=%d accepted=%d",
                std::string(nte::combat_stats::CombatEventKindName(event.kind)).c_str(),
                static_cast<unsigned long long>(event.sequence),
                static_cast<unsigned long long>(event.tick_sequence),
                static_cast<unsigned long long>(event.name_id),
                static_cast<long long>(event.value),
                static_cast<long long>(event.basic_value),
                static_cast<long long>(event.final_value), event.duration_seconds,
                event.stack_count, event.player_related ? 1 : 0,
                event.accepted ? 1 : 0);
            TextFormat(ui, "  event=%s (%s)", event.event_display_name.c_str(),
                event.event_name.c_str());
            TextFormat(ui, "  world=%llu:%llu source=%llu:%llu target=%llu:%llu",
                static_cast<unsigned long long>(event.world.id),
                static_cast<unsigned long long>(event.world.generation),
                static_cast<unsigned long long>(event.attacker.handle.id),
                static_cast<unsigned long long>(event.attacker.handle.generation),
                static_cast<unsigned long long>(event.victim.handle.id),
                static_cast<unsigned long long>(event.victim.handle.generation));
            TextFormat(ui, "  source=%s target=%s types=%u/%u reaction=%u/%u",
                event.attacker.display_name.empty() ? event.attacker.technical_path.c_str() :
                    event.attacker.display_name.c_str(),
                event.victim.display_name.empty() ? event.victim.technical_path.c_str() :
                    event.victim.display_name.c_str(), event.damage_type, event.display_type,
                event.reaction_type, event.reaction_display_type);
        }
    }
    if (has_children) ui->end_child(ui->user);

    TextLocalized(ui, "developer.skills.heading", "Skill snapshot deltas ({0}/256, overwritten {1})",
        FormatUnsigned(render.developer.skill_records.size()),
        FormatUnsigned(render.developer.skill_overwritten));
    const bool skill_child = !has_children || ui->begin_child(
        ui->user, anomaly::sdk::StringView("debug-skill-events"), 0, 180, 0);
    if (skill_child) {
        for (auto iterator = render.developer.skill_records.rbegin();
             iterator != render.developer.skill_records.rend(); ++iterator) {
            const auto& event = *iterator;
            const auto& skill = event.skill;
            TextFormat(ui,
                "%s frame=%llu/%llu handle=%llu:%llu flags=0x%08X level=%d input=%d seq=%llu",
                SkillChangeName(event.kind),
                static_cast<unsigned long long>(event.frame_generation),
                static_cast<unsigned long long>(event.frame_sequence),
                static_cast<unsigned long long>(skill.handle.id),
                static_cast<unsigned long long>(skill.handle.generation), skill.flags,
                skill.level, skill.input_id,
                static_cast<unsigned long long>(skill.sequence));
            if (skill.cooldown_valid) {
                TextFormat(ui, "  cooldown=%.2f/%.2f · %s",
                    skill.cooldown_remaining_seconds,
                    skill.cooldown_duration_seconds,
                    (skill.ability_display_name.empty()
                        ? skill.ability_path : skill.ability_display_name).c_str());
            } else {
                TextFormat(ui, "  cooldown=unavailable / %s",
                    (skill.ability_display_name.empty()
                        ? skill.ability_path : skill.ability_display_name).c_str());
            }
        }
    }
    if (has_children) ui->end_child(ui->user);
}

void Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr ||
        ui->end_window == nullptr || ui->text == nullptr) return;
    RenderState render;
    std::array<char, kMaximumPathBytes> path_input{};
    std::array<char, kMaximumPathBytes> export_path_input{};
    {
        std::scoped_lock lock(g_context.mutex);
        render = g_context.render;
        path_input = g_context.log_root_input;
        export_path_input = g_context.export_root_input;
    }
    int open = 1;
    const std::string window_title =
        g_context.localizer.Text("window.title", "NTE Combat Tracker");
    anomaly::sdk::UiWindow window(ui, window_title, &open);
    if (!window) return;

    const auto DrawSettings = [&]() {
    TextLocalized(ui, "settings.overlay.heading", "Overlay settings");
    if (ui->button != nullptr) {
        const std::string_view key = !render.overlay_error.empty()
            ? "settings.overlay.retry"
            : (render.overlay_enabled ? "settings.overlay.close" : "settings.overlay.open");
        const std::string_view fallback = !render.overlay_error.empty()
            ? "Retry overlay launch"
            : (render.overlay_enabled ? "Close overlay" : "Open overlay");
        if (ui->button(ui->user, ScopedLabel(key, fallback).View(), 0.0F, 0.0F)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.toggle_overlay_requested = true;
        }
    }

    auto settings = render.overlay_settings;
    bool settings_changed{};
    if (ui->checkbox != nullptr) {
        const auto checkbox = [&](const std::string_view key,
                                  const std::string_view fallback, bool& value) {
            int checked = value ? 1 : 0;
            if (ui->checkbox(ui->user, ScopedLabel(key, fallback).View(), &checked)) {
                value = checked != 0;
                settings_changed = true;
            }
        };
        checkbox("settings.checkbox.lock", "Lock / click-through", settings.locked);
        checkbox("settings.checkbox.collapse", "Collapse", settings.collapsed);
        checkbox("settings.checkbox.topmost", "Always on top", settings.topmost);
        checkbox("settings.checkbox.hide_idle", "Hide when idle", settings.hide_idle);
        if (render.developer_available) {
            int developer_checked = render.developer.enabled ? 1 : 0;
            if (ui->checkbox(ui->user,
                    ScopedLabel("settings.checkbox.developer", "Developer mode").View(),
                    &developer_checked)) {
                render.developer.enabled = developer_checked != 0;
                std::scoped_lock lock(g_context.mutex);
                g_context.requested_developer_mode = render.developer.enabled;
            }
        }
    }
    if (ui->slider_float != nullptr) {
        settings_changed = ui->slider_float(ui->user,
            ScopedLabel("settings.slider.opacity", "Opacity").View(), &settings.opacity,
            0.4F, 1.0F) != 0 || settings_changed;
        settings_changed = ui->slider_float(ui->user,
            ScopedLabel("settings.slider.ui_scale", "UI scale").View(), &settings.ui_scale,
            0.75F, 2.0F) != 0 || settings_changed;
    }
    if (settings_changed) {
        std::scoped_lock lock(g_context.mutex);
        g_context.requested_overlay_settings = settings;
    }

    if (ui->separator != nullptr) ui->separator(ui->user);
    TextLocalized(ui, "settings.storage.heading", "Statistics storage");
    const bool input_available = nte::sdk_compat::HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::input_text)>(
            ui, offsetof(AnomalyUiServiceV1, input_text)) &&
        ui->input_text != nullptr;
    if (input_available) {
        if (ui->input_text(ui->user,
                ScopedLabel("settings.input.storage_path", "Storage path").View(),
                path_input.data(), path_input.size(), 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.log_root_input = path_input;
        }
    } else {
        TextLocalized(ui, "settings.storage.path", "Storage path: {0}",
            render.log_root.c_str());
    }
    if (ui->button != nullptr && ui->button(ui->user,
            ScopedLabel("settings.button.apply_path", "Apply path").View(), 0.0F, 0.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.requested_log_root = path_input.data();
    }
    if (nte::sdk_compat::HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::same_line)>(
            ui, offsetof(AnomalyUiServiceV1, same_line)) &&
        ui->same_line != nullptr) {
        ui->same_line(ui->user, 0.0F, -1.0F);
    }
    if (ui->button != nullptr && ui->button(ui->user,
            ScopedLabel("settings.button.open_data", "Open data folder").View(), 0.0F, 0.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.open_log_root_requested = true;
    }
    if (input_available) {
        if (ui->input_text(ui->user,
                ScopedLabel("settings.input.export_dir", "Export directory").View(),
                export_path_input.data(), export_path_input.size(), 0)) {
            std::scoped_lock lock(g_context.mutex);
            g_context.export_root_input = export_path_input;
        }
    } else {
        TextLocalized(ui, "settings.storage.export", "Export directory: {0}",
            render.export_root.empty() ? "not configured" : render.export_root.c_str());
    }
    if (ui->button != nullptr && ui->button(ui->user,
            ScopedLabel("settings.button.apply_export", "Apply export directory").View(),
            0.0F, 0.0F)) {
        std::scoped_lock lock(g_context.mutex);
        g_context.apply_export_root_requested = true;
        g_context.requested_export_root = export_path_input.data();
    }
    if (!render.settings_status.empty()) {
        Text(ui, render.settings_status.c_str());
    }
    if (!render.settings_error.empty()) {
        TextLocalized(ui, "settings.error.settings", "Settings error: {0}",
            render.settings_error.c_str());
    }
    if (!render.character_attribute_error.empty()) {
        TextLocalized(ui, "settings.error.attribute", "Character attribute error: {0}",
            render.character_attribute_error.c_str());
    }
    if (!render.overlay_error.empty()) {
        TextLocalized(ui, "settings.error.overlay", "Overlay launch error: {0}",
            render.overlay_error.c_str());
    }
    if (!render.log.last_error.empty()) {
        TextLocalized(ui, "settings.error.storage", "Storage error: {0}",
            render.log.last_error.c_str());
    }
    };

    const bool tabs_available = nte::sdk_compat::HasField<AnomalyUiServiceV1,
            decltype(AnomalyUiServiceV1::end_tab_bar)>(
            ui, offsetof(AnomalyUiServiceV1, end_tab_bar)) &&
        ui->begin_tab_bar != nullptr && ui->begin_tab_item != nullptr &&
        ui->end_tab_item != nullptr && ui->end_tab_bar != nullptr;
    if (!tabs_available) {
        DrawSettings();
        if (render.developer_available && render.developer.enabled) {
            TextLocalized(ui, "settings.developer.unavailable", "The current Anomaly UI service has no tabs; the developer page is unavailable.");
        }
        return;
    }
    if (!ui->begin_tab_bar(ui->user,
            anomaly::sdk::StringView("nte-combat-tracker-tabs"),
            ANOMALY_UI_TAB_BAR_V1_NONE)) {
        DrawSettings();
        return;
    }
    if (ui->begin_tab_item(ui->user,
            ScopedLabel("settings.tab.settings", "Settings").View(),
            nullptr, ANOMALY_UI_TAB_ITEM_V1_NONE, 1)) {
        DrawSettings();
        ui->end_tab_item(ui->user);
    }
    if (render.developer_available && render.developer.enabled && ui->begin_tab_item(
            ui->user, ScopedLabel("settings.tab.developer", "Developer").View(), nullptr,
            ANOMALY_UI_TAB_ITEM_V1_NONE, 1)) {
        DrawDeveloperPage(ui, render);
        ui->end_tab_item(ui->user);
    }
    ui->end_tab_bar(ui->user);
}

// The overlay runs in its own process with no Host services, so it needs the
// active locale to pick the same catalog the plugin resolved.
[[nodiscard]] std::string ResolveLocale(const anomaly::sdk::Host* services) {
    const auto localization = services->Query<AnomalyLocalizationServiceV1>(
        ANOMALY_LOCALIZATION_SERVICE_V1_ID, ANOMALY_LOCALIZATION_SERVICE_V1_VERSION);
    if (!localization || localization->locale == nullptr) return "zh-CN";
    std::size_t size = 0;
    if (localization->locale(localization->user, nullptr, &size).code ==
        ANOMALY_STATUS_V1_OK) {
        return "zh-CN";
    }
    if (size <= 1 || size > 64) return "zh-CN";
    std::string buffer(size, '\0');
    if (localization->locale(localization->user, buffer.data(), &size).code !=
        ANOMALY_STATUS_V1_OK) {
        return "zh-CN";
    }
    while (!buffer.empty() && buffer.back() == '\0') buffer.pop_back();
    return buffer.empty() ? "zh-CN" : buffer;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    const anomaly::sdk::Host services(host);
    const auto ui = services.Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || ui->begin_window == nullptr || ui->end_window == nullptr || ui->text == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    const auto core = services.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    const auto websocket = services.Query<AnomalyWebSocketServiceV1>(
        ANOMALY_WEBSOCKET_SERVICE_V1_ID, ANOMALY_WEBSOCKET_SERVICE_V1_VERSION);
    g_context.host = host;
    g_context.ui = ui.get();
    g_context.core = core.get();
    g_context.websocket = websocket.get();
    g_context.localizer = anomaly::sdk::Localizer(host);
    g_context.locale = ResolveLocale(&services);
    ResetState();
    *context = &g_context;
    return anomaly::sdk::Ok();
}

// Reads the shipped DT_Character rows and installs the resolver. A failure is
// reported, never fatal: rows simply keep their identity colour.
//
// Retried on the Game tick until it succeeds. The table is a content asset, so
// a plugin that loads before the world finished streaming sees "not loaded";
// reading once at Start would then lose the attribute for the whole session.
void LoadCharacterAttributes(const anomaly::sdk::Host& services) {
    if (g_context.character_attributes_ready) return;
    if (g_context.character_attributes_disabled) return;
    if (!g_context.native.complete) {
        // Without the profile layout there is nothing to read; report once
        // instead of polling memory on every game tick.
        g_context.character_attributes_disabled = true;
        g_context.character_attribute_error =
            "compatibility profile is unavailable: " + g_context.native.error;
        return;
    }
    const auto now = nte::combat_stats::Clock::now();
    if (now < g_context.next_character_attribute_attempt) return;
    g_context.next_character_attribute_attempt = now + std::chrono::seconds(2);

    nte::combat_stats::CharacterAttributeServices resolved;
    const auto core = services.Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    const auto objects = services.Query<AnomalyUe5ObjectsServiceV1>(
        ANOMALY_UE5_OBJECTS_SERVICE_V1_ID, ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
    const auto names = services.Query<AnomalyUe5NamesServiceV1>(
        ANOMALY_UE5_NAMES_SERVICE_V1_ID, ANOMALY_UE5_NAMES_SERVICE_V1_VERSION);
    resolved.core = core.get();
    resolved.objects = objects.get();
    resolved.names = names.get();
    resolved.native = &g_context.native;

    std::string error;
    auto source = std::make_shared<nte::combat_stats::CharacterAttributeSource>(
        nte::combat_stats::CharacterAttributeSource::Load(resolved, error));
    g_context.character_attribute_error = error;
    if (!error.empty()) {
        // The settings page alone is too easy to miss: a failure here silently
        // costs every row its attribute colour, so record it in the host log
        // with the concrete reason.
        if (resolved.core != nullptr && resolved.core->log != nullptr) {
            const std::string message = "NteCombatTracker: character attribute table unavailable: " +
                error;
            resolved.core->log(resolved.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                anomaly::sdk::StringView(message));
        }
        return;
    }

    g_context.character_attributes_ready = true;
    if (resolved.core != nullptr && resolved.core->log != nullptr) {
        const std::string message = "NteCombatTracker: resolved " +
            std::to_string(source->count()) + " character attributes from DT_Character";
        resolved.core->log(resolved.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            anomaly::sdk::StringView(message));
    }
    g_context.engine.SetAttributeResolver(
        [source](const std::string& object_path) {
            return source->FindElementForPath(object_path);
        });
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    ResetState();
    g_context.native = nte::native::Load(g_context.host);
    if (!g_context.native.complete && g_context.core != nullptr &&
        g_context.core->log != nullptr) {
        const std::string message =
            "NteCombatTracker: " + g_context.native.error;
        g_context.core->log(g_context.core->user, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            anomaly::sdk::StringView(message));
    }
    g_context.uid_detector = std::make_unique<nte::account::UidDetector>(
        g_context.host, g_context.native);
    CloseControlHandles();
    g_context.manual_end_event = CreateEventW(nullptr, FALSE, FALSE,
        nte::combat_stats::overlay::kManualEndEventName);
    g_context.plugin_stopping_event = CreateEventW(nullptr, FALSE, FALSE,
        nte::combat_stats::overlay::kPluginStoppingEventName);
    if (g_context.manual_end_event != nullptr) ResetEvent(g_context.manual_end_event);
    if (g_context.plugin_stopping_event != nullptr) ResetEvent(g_context.plugin_stopping_event);
    g_context.producer_instance_id = nte::damage_log::GenerateUuidV4();
    const auto default_log_root = nte::damage_log::DefaultLogRoot();
    std::string settings_error;
    const auto plugin_settings = nte::settings::LoadPluginSettings(
        nte::settings::PluginSettingsPath(), default_log_root, settings_error);
    g_context.log_root = plugin_settings.log_root;
    g_context.export_root = plugin_settings.export_root;
    g_context.developer_access.SetSavedPreference(
        plugin_settings.developer_mode);
    ApplyDeveloperModeAccess();
    std::string prepare_error;
    if (!nte::settings::PrepareLogRoot(g_context.log_root, prepare_error)) {
        if (!settings_error.empty()) settings_error += "；";
        settings_error += "Config directory is unavailable: " + prepare_error;
        g_context.log_root = default_log_root;
        std::string fallback_error;
        if (!nte::settings::PrepareLogRoot(g_context.log_root, fallback_error)) {
            settings_error += "; default directory is unavailable: " + fallback_error;
        }
    }
    if (!settings_error.empty()) g_context.settings_error = settings_error;
    CopyLogRootToInput(g_context.log_root);
    CopyExportRootToInput(g_context.export_root);
    std::string overlay_settings_error;
    static_cast<void>(nte::settings::LoadOverlaySettings(
        nte::settings::OverlaySettingsPath(),
        g_context.overlay_settings, overlay_settings_error));
    if (!overlay_settings_error.empty()) {
        g_context.settings_error = overlay_settings_error;
    }
    if (!g_context.log_root.empty()) {
        g_context.log_worker = std::make_unique<nte::damage_log::DamageLogWorker>(
            g_context.log_root);
        static_cast<void>(g_context.log_worker->Start());
    }
    LoadCharacterAttributes(anomaly::sdk::Host(g_context.host));
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    if (g_context.plugin_stopping_event != nullptr) {
        SetEvent(g_context.plugin_stopping_event);
    }
    const auto now = nte::combat_stats::Clock::now();
    const auto utc_ms = UtcNowMilliseconds();
    RenderState render;
    {
        std::scoped_lock lock(g_context.mutex);
        render = g_context.render;
    }
    if (!g_context.producer_instance_id.empty()) {
        PublishOverlay(g_context.engine.BuildSnapshot(now),
            CaptureStatusCode(render.snapshot.get()),
            render.snapshot ? render.snapshot->latest_event_sequence : 0,
            g_context.log_session.report_id,
            g_context.log_session.encounter_id, utc_ms, now, true, true);
    }
    if (!g_context.log_session.encounter_id.empty()) {
        FinishEncounterLog("plugin_stop", MakeOverlaySnapshot(
            g_context.engine.BuildSnapshot(now), CaptureStatusCode(render.snapshot.get()),
            render.snapshot ? render.snapshot->latest_event_sequence : 0,
            g_context.log_session.report_id,
            g_context.log_session.encounter_id, utc_ms, true,
            g_context.overlay_snapshot_sequence), now, utc_ms);
    }
    FinishReportLog("plugin_stop", utc_ms);
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    ResetState();
    CloseControlHandles();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    if (g_context.log_worker || !g_context.log_session.report_id.empty()) {
        static_cast<void>(Stop(nullptr, 0));
    }
    if (g_context.plugin_stopping_event != nullptr) {
        SetEvent(g_context.plugin_stopping_event);
    }
    if (g_context.log_worker) {
        g_context.log_worker->Stop();
        g_context.log_worker.reset();
    }
    ResetState();
    CloseControlHandles();
    g_context.host = nullptr;
    g_context.ui = nullptr;
    g_context.core = nullptr;
    g_context.websocket = nullptr;
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("shikari.nte-combat-tracker"),
        anomaly::sdk::StringView("NTE Combat Tracker"),
        anomaly::sdk::StringView("Shikari"),
        anomaly::sdk::StringView(nte::build::kPluginVersion),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
