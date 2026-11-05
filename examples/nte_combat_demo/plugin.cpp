#include "anomaly/sdk/cpp.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

constexpr std::uint32_t kSkillPageCapacity = 8;
constexpr std::size_t kRecentCombatCapacity = 16;
constexpr std::size_t kMaximumDrainPerUpdate = 64;
constexpr std::size_t kPendingCombatCapacity = 64;
constexpr std::size_t kMaximumPendingNameQueriesPerUpdate = 4;
constexpr std::uint64_t kPendingNameRetryInterval = 30;
constexpr std::uint8_t kMaximumPendingNameAttempts = 32;
constexpr std::size_t kMaximumResolvedNameBytes = 1024;

struct CombatRow {
    std::uint64_t sequence{};
    std::array<char, 384> summary{};
};

struct SkillRow {
    std::array<char, 256> name{};
    std::array<char, 384> summary{};
    std::array<char, 320> activation_label{};
    AnomalyGenerationHandleV1 ability_class{};
    AnomalyNteSkillInvocationRequestV1 activation{sizeof(activation)};
    std::uint64_t skill_id{};
    bool skill_current{};
    bool can_activate{};
};

struct RenderSnapshot {
    std::uint32_t combat_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    std::array<char, 192> combat_header{};
    bool has_combatant{};
    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    std::array<char, 256> hp{};
    std::array<char, 256> state{};
    std::array<char, 256> outgoing{};
    std::array<char, 256> incoming{};
    bool statistics_initialized{};

    std::uint64_t latest_combat_sequence{};
    std::array<CombatRow, kRecentCombatCapacity> damage_rows{};
    std::size_t damage_row_count{};
    std::array<CombatRow, kRecentCombatCapacity> heal_rows{};
    std::size_t heal_row_count{};
    std::array<CombatRow, kRecentCombatCapacity> buff_rows{};
    std::size_t buff_row_count{};

    std::uint32_t skills_status{ANOMALY_STATUS_V1_UNAVAILABLE};
    std::array<char, 192> skills_header{};
    std::uint64_t skill_generation{};
    std::uint64_t skill_sequence{};
    std::uint32_t skill_offset{};
    std::uint32_t skill_total{};
    std::uint32_t skill_next_offset{};
    std::array<SkillRow, kSkillPageCapacity> skill_rows{};
    std::size_t skill_row_count{};

    bool invocation_available{};
    bool has_activation_result{};
    std::array<char, 256> activation_result{};
};

struct PendingActivation {
    AnomalyNteSkillInvocationRequestV1 request{sizeof(request)};
    std::uint64_t skill_id{};
    std::array<char, 256> skill_name{};
};

struct PendingCombatEvent {
    AnomalyNteCombatEventV1 event{sizeof(event)};
    std::uint64_t next_retry{};
    std::uint8_t attempts{};
    bool displayed{};
};

struct Context {
    const AnomalyHostApiV1* host{};
    const AnomalyUiServiceV1* ui{};
    std::mutex state_mutex;
    RenderSnapshot snapshot;
    PendingActivation pending_activation;
    bool activation_queued{};
    std::uint32_t requested_skill_offset{};
    std::uint64_t combat_cursor{};
    std::array<PendingCombatEvent, kPendingCombatCapacity> pending_combat{};
    std::size_t pending_combat_count{};
    std::uint64_t combat_update_sequence{};
    std::unordered_map<std::uint64_t, std::string> combat_name_cache;
    std::unordered_map<std::uint64_t, std::string> participant_name_cache;
    std::unordered_map<std::uint64_t, std::string> skill_name_cache;
    std::unordered_map<std::uint64_t, std::uint64_t> skill_name_retry;
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
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

bool SameHandle(
    const AnomalyGenerationHandleV1 left,
    const AnomalyGenerationHandleV1 right) noexcept {
    return left.id == right.id && left.generation == right.generation;
}

bool CombatMethodsAvailable(const AnomalyNteCombatServiceV1* service) noexcept {
    return HasField<AnomalyNteCombatServiceV1,
               decltype(AnomalyNteCombatServiceV1::participant_display_name_utf8)>(
               service, offsetof(AnomalyNteCombatServiceV1, participant_display_name_utf8)) &&
        service->current_combatant != nullptr &&
        service->statistics != nullptr &&
        service->source_name_utf8 != nullptr &&
        service->latest_event_sequence != nullptr &&
        service->next_event != nullptr &&
        service->event_name_utf8 != nullptr &&
        service->participant_path_utf8 != nullptr &&
        service->participant_display_name_utf8 != nullptr;
}

bool SkillMethodsAvailable(const AnomalyNteSkillsServiceV1* service) noexcept {
    return HasField<AnomalyNteSkillsServiceV1,
               decltype(AnomalyNteSkillsServiceV1::ability_display_name_utf8)>(
               service, offsetof(AnomalyNteSkillsServiceV1, ability_display_name_utf8)) &&
        service->frame != nullptr && service->page != nullptr &&
        service->ability_path_utf8 != nullptr &&
        service->ability_display_name_utf8 != nullptr;
}

bool InvocationMethodsAvailable(
    const AnomalyNteSkillInvocationServiceV1* service) noexcept {
    return HasField<AnomalyNteSkillInvocationServiceV1,
               decltype(AnomalyNteSkillInvocationServiceV1::activate)>(
               service, offsetof(AnomalyNteSkillInvocationServiceV1, activate)) &&
        service->activate != nullptr;
}

bool IsSizingStatus(const AnomalyStatusV1 status) noexcept {
    return status.code == ANOMALY_STATUS_V1_OK ||
        status.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL;
}

std::uint64_t ParticipantCacheKey(
    const AnomalyGenerationHandleV1 handle) noexcept {
    // IDs can be reused after an object-generation change. Keep the generation
    // in the cache key so a stale role name cannot cross a world reset.
    return handle.id ^ (handle.generation + 0x9E3779B97F4A7C15ULL +
        (handle.id << 6U) + (handle.id >> 2U));
}

std::string ResolveCombatEventName(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyNteCombatEventV1& event) {
    if (event.name_id == 0 || service->event_name_utf8 == nullptr) return {};
    const auto cached = g_context.combat_name_cache.find(event.name_id);
    if (cached != g_context.combat_name_cache.end()) return cached->second;
    std::size_t size{};
    if (!IsSizingStatus(service->event_name_utf8(service->user, &event, nullptr, &size)) ||
        size <= 1 || size > kMaximumResolvedNameBytes) return {};
    std::string value(size, '\0');
    if (service->event_name_utf8(service->user, &event, value.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    // UObject paths are implementation details, not display labels.
    if (value.rfind("/Game/", 0) == 0 || value.rfind("/Script/", 0) == 0) return {};
    g_context.combat_name_cache.emplace(event.name_id, value);
    return value;
}

std::string ResolveCombatParticipant(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyGenerationHandleV1 handle) {
    if (handle.id == 0) return {};
    const auto cache_key = ParticipantCacheKey(handle);
    const auto cached = g_context.participant_name_cache.find(cache_key);
    if (cached != g_context.participant_name_cache.end()) return cached->second;
    const auto read_string = [&](auto resolver) {
        std::size_t size{};
        if (!IsSizingStatus(resolver(nullptr, &size)) ||
            size <= 1 || size > kMaximumResolvedNameBytes) return std::string{};
        std::string value(size, '\0');
        if (resolver(value.data(), &size).code != ANOMALY_STATUS_V1_OK) return std::string{};
        if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
        return value;
    };
    if (service->participant_display_name_utf8 != nullptr) {
        const auto display = read_string([&](char* destination, std::size_t* size) {
            return service->participant_display_name_utf8(
                service->user, handle, destination, size);
        });
        if (!display.empty() && display.rfind("/Game/", 0) != 0 &&
            display.rfind("/Script/", 0) != 0) {
            g_context.participant_name_cache.emplace(cache_key, display);
            return display;
        }
    }
    // Keep the event visible while the deferred Runtime FName/FText lookup is
    // still pending. A resolved Runtime display name above always wins.
    if (service->participant_path_utf8 != nullptr) {
        const auto path = read_string([&](char* destination, std::size_t* size) {
            return service->participant_path_utf8(
                service->user, handle, destination, size);
        });
        if (!path.empty()) return path;
    }
    return {};
}

void ResolveCombatParticipantState(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyGenerationHandleV1 handle,
    std::string& value,
    bool& resolved) {
    value.clear();
    resolved = handle.id == 0;
    if (handle.id == 0) return;
    const auto cache_key = ParticipantCacheKey(handle);
    const auto cached = g_context.participant_name_cache.find(cache_key);
    if (cached != g_context.participant_name_cache.end()) {
        value = cached->second;
        resolved = true;
        return;
    }
    const auto read_string = [&](auto resolver) {
        std::size_t size{};
        if (!IsSizingStatus(resolver(nullptr, &size)) ||
            size <= 1 || size > kMaximumResolvedNameBytes) return std::string{};
        std::string result(size, '\0');
        if (resolver(result.data(), &size).code != ANOMALY_STATUS_V1_OK) return std::string{};
        if (const auto end = result.find('\0'); end != std::string::npos) result.resize(end);
        return result;
    };
    if (service->participant_display_name_utf8 != nullptr) {
        const auto display = read_string([&](char* destination, std::size_t* size) {
            return service->participant_display_name_utf8(
                service->user, handle, destination, size);
        });
        if (!display.empty() && display.rfind("/Game/", 0) != 0 &&
            display.rfind("/Script/", 0) != 0) {
            g_context.participant_name_cache.emplace(cache_key, display);
            value = display;
            resolved = true;
            return;
        }
    }
    if (service->participant_path_utf8 != nullptr) {
        value = read_string([&](char* destination, std::size_t* size) {
            return service->participant_path_utf8(
                service->user, handle, destination, size);
        });
    }
}

bool FormatCompactCombatRow(
    const AnomalyNteCombatServiceV1* service,
    const AnomalyNteCombatEventV1& event,
    CombatRow& row,
    bool& names_complete) {
    names_complete = false;
    const std::string name = ResolveCombatEventName(service, event);
    const char* display_name = name.empty() ? "-" : name.c_str();
    std::string source;
    std::string target;
    bool source_resolved{};
    bool target_resolved{};
    ResolveCombatParticipantState(service, event.source, source, source_resolved);
    ResolveCombatParticipantState(service, event.target, target, target_resolved);
    // Display numeric events immediately; localized names are optional enrichment.
    names_complete = (event.name_id == 0 || !name.empty()) &&
        source_resolved && target_resolved;
    row.sequence = event.sequence;
    if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_DAMAGE) {
        std::snprintf(row.summary.data(), row.summary.size(),
            "#%llu \xE4\xBC\xA4\xE5\xAE\xB3=%lld%s \xE6\x8A\x80\xE8\x83\xbd=%s  %s -> %s",
            static_cast<unsigned long long>(event.sequence),
            static_cast<long long>(event.final_value),
            (event.flags & ANOMALY_NTE_COMBAT_EVENT_V1_CRITICAL) != 0
                ? " \xE6\x9A\xB4\xE5\x87\xbb" : "",
            display_name,
            source.empty() ? "-" : source.c_str(), target.empty() ? "-" : target.c_str());
    } else if (event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_HEAL) {
        const char* skill = name.empty() ? "\xE6\xB2\xBB\xE7\x96\x97" : name.c_str();
        std::snprintf(row.summary.data(), row.summary.size(),
            "#%llu \xE6\xB2\xBB\xE7\x96\x97=%lld \xE6\x8A\x80\xE8\x83\xBD=%s  %s -> %s",
            static_cast<unsigned long long>(event.sequence),
            static_cast<long long>(event.value),
            skill,
            source.empty() ? "-" : source.c_str(), target.empty() ? "-" : target.c_str());
    } else {
        const char* kind = event.kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD
            ? "\xE5\xA2\x9E\xE7\x9B\x8A+" : "\xE5\xA2\x9E\xE7\x9B\x8A-";
        std::snprintf(row.summary.data(), row.summary.size(),
            "#%llu %s %s \xE6\x8C\x81\xE7\xBB\xad=%.2f \xE5\xB1\x82\xE6\x95\xB0=%d  %s -> %s",
            static_cast<unsigned long long>(event.sequence), kind,
            display_name,
            event.duration_seconds, event.stack_count,
            source.empty() ? "-" : source.c_str(), target.empty() ? "-" : target.c_str());
    }
    return true;
}

bool PushCombatRow(
    RenderSnapshot& snapshot, const CombatRow& row,
    const std::uint32_t kind,
    const bool insert_if_missing) noexcept {
    auto* rows = &snapshot.damage_rows;
    auto* count = &snapshot.damage_row_count;
    if (kind == ANOMALY_NTE_COMBAT_EVENT_V1_HEAL) {
        rows = &snapshot.heal_rows;
        count = &snapshot.heal_row_count;
    } else if (kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_ADD ||
               kind == ANOMALY_NTE_COMBAT_EVENT_V1_BUFF_REMOVE) {
        rows = &snapshot.buff_rows;
        count = &snapshot.buff_row_count;
    }
    for (std::size_t index{}; index < *count; ++index) {
        if ((*rows)[index].sequence == row.sequence) {
            (*rows)[index] = row;
            return true;
        }
    }
    if (!insert_if_missing) return false;
    const std::size_t new_count = (std::min)(*count + 1U, rows->size());
    for (std::size_t index = new_count; index > 1; --index) {
        (*rows)[index - 1] = (*rows)[index - 2];
    }
    (*rows)[0] = row;
    *count = new_count;
    return true;
}

bool DrainCombat(
    const AnomalyNteCombatServiceV1* combat,
    RenderSnapshot& snapshot) {
    if (combat == nullptr ||
        !HasField<AnomalyNteCombatServiceV1,
            decltype(AnomalyNteCombatServiceV1::participant_display_name_utf8)>(
            combat, offsetof(AnomalyNteCombatServiceV1, participant_display_name_utf8)) ||
        combat->latest_event_sequence == nullptr || combat->next_event == nullptr ||
        combat->event_name_utf8 == nullptr || combat->participant_display_name_utf8 == nullptr) {
        return false;
    }
    const std::uint64_t latest = combat->latest_event_sequence(combat->user);
    snapshot.latest_combat_sequence = latest;
    const std::uint64_t update_sequence = ++g_context.combat_update_sequence;
    bool accepted{};
    std::size_t pending_count{};
    std::size_t pending_queries{};
    for (std::size_t index{}; index < g_context.pending_combat_count; ++index) {
        auto pending = g_context.pending_combat[index];
        if (pending.next_retry > update_sequence ||
            pending_queries >= kMaximumPendingNameQueriesPerUpdate) {
            g_context.pending_combat[pending_count++] = pending;
            continue;
        }
        ++pending_queries;
        CombatRow row;
        bool names_complete{};
        const bool displayable = FormatCompactCombatRow(
            combat, pending.event, row, names_complete);
        if (displayable) {
            accepted |= PushCombatRow(
                snapshot, row, pending.event.kind, !pending.displayed);
            pending.displayed = true;
        }
        if ((!displayable || !names_complete) &&
            pending.event.name_id != 0 &&
            ++pending.attempts < kMaximumPendingNameAttempts &&
            pending_count < g_context.pending_combat.size()) {
            pending.next_retry = update_sequence + kPendingNameRetryInterval;
            g_context.pending_combat[pending_count++] = pending;
        }
    }
    g_context.pending_combat_count = pending_count;
    const auto retain_pending = [&](
                                    const AnomalyNteCombatEventV1& event,
                                    const bool displayed) {
        PendingCombatEvent pending{event, update_sequence + 1U, 0, displayed};
        if (pending_count < g_context.pending_combat.size()) {
            g_context.pending_combat[pending_count++] = pending;
            return;
        }
        // Keep the newest unresolved events. The cursor is already advanced,
        // so dropping the oldest pending entry is the only way to make room
        // for a label that may complete after this update.
        std::size_t oldest{};
        for (std::size_t index = 1; index < pending_count; ++index) {
            if (g_context.pending_combat[index].event.sequence <
                g_context.pending_combat[oldest].event.sequence) {
                oldest = index;
            }
        }
        if (event.sequence > g_context.pending_combat[oldest].event.sequence) {
            g_context.pending_combat[oldest] = pending;
        }
    };
    if (latest == g_context.combat_cursor) return accepted;
    bool cursor_rebased{};
    for (std::size_t count{}; count < kMaximumDrainPerUpdate;) {
        AnomalyNteCombatEventV1 event{sizeof(event)};
        const auto status = combat->next_event(combat->user, g_context.combat_cursor, &event);
        if (status.code != ANOMALY_STATUS_V1_OK) {
            if (!cursor_rebased && status.code == ANOMALY_STATUS_V1_NOT_FOUND &&
                g_context.combat_cursor != 0 && latest > g_context.combat_cursor) {
                // The producer ring advanced past this consumer. Starting at
                // zero asks the ABI for the oldest event still retained.
                g_context.combat_cursor = 0;
                cursor_rebased = true;
                continue;
            }
            break;
        }
        g_context.combat_cursor = event.sequence;
        ++count;
        CombatRow row;
        bool names_complete{};
        const bool displayable = FormatCompactCombatRow(
            combat, event, row, names_complete);
        if (displayable) {
            accepted |= PushCombatRow(snapshot, row, event.kind, true);
        }
        if ((!displayable || !names_complete) && event.name_id != 0) {
            retain_pending(event, displayable);
        }
    }
    g_context.pending_combat_count = pending_count;
    return accepted;
}

std::string ResolveAbilityDisplayName(
    const AnomalyNteSkillsServiceV1* service,
    const AnomalyGenerationHandleV1 ability_class) {
    if (service == nullptr || ability_class.id == 0 || service->ability_display_name_utf8 == nullptr) {
        return {};
    }
    std::size_t size{};
    if (!IsSizingStatus(service->ability_display_name_utf8(
            service->user, ability_class, nullptr, &size)) ||
        size <= 1 || size > kMaximumResolvedNameBytes) return {};
    std::string value(size, '\0');
    if (service->ability_display_name_utf8(
            service->user, ability_class, value.data(), &size).code != ANOMALY_STATUS_V1_OK) return {};
    if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    if (value.rfind("/Game/", 0) == 0 || value.rfind("/Script/", 0) == 0) return {};
    return value;
}

std::string ResolveAbilityFallbackName(
    const AnomalyNteSkillsServiceV1* service,
    const AnomalyNteSkillSnapshotV1& skill) {
    std::size_t size{};
    if (IsSizingStatus(service->ability_path_utf8(
            service->user, skill.ability_class, nullptr, &size)) &&
        size > 1 && size <= kMaximumResolvedNameBytes) {
        std::string path(size, '\0');
        if (service->ability_path_utf8(
                service->user, skill.ability_class, path.data(), &size).code ==
            ANOMALY_STATUS_V1_OK) {
            if (const auto end = path.find('\0'); end != std::string::npos) path.resize(end);
            if (const auto separator = path.find_last_of("/."); separator != std::string::npos) {
                path.erase(0, separator + 1);
            }
            if (!path.empty()) return path;
        }
    }
    return "Skill #" + std::to_string(skill.handle.id);
}

bool ReadStatistics(
    const AnomalyNteCombatServiceV1* combat,
    const AnomalyNteCombatantSnapshotV1& combatant,
    const std::uint32_t direction,
    AnomalyNteCombatStatisticsV1& statistics) {
    AnomalyNteCombatStatisticsRequestV1 request{sizeof(request)};
    request.world = combatant.world;
    request.character = combatant.character;
    request.direction = direction;
    statistics = {sizeof(statistics)};
    return combat->statistics(combat->user, &request, &statistics).code ==
        ANOMALY_STATUS_V1_OK;
}

bool UpdateCombat(
    const AnomalyNteCombatServiceV1* combat,
    RenderSnapshot& snapshot) {
    if (!CombatMethodsAvailable(combat)) {
        snapshot.combat_status = ANOMALY_STATUS_V1_UNAVAILABLE;
        snapshot.has_combatant = false;
        return false;
    }

    const std::uint64_t latest = combat->latest_event_sequence(combat->user);
    const bool damage_changed = latest != snapshot.latest_combat_sequence;
    AnomalyNteCombatantSnapshotV1 combatant{sizeof(combatant)};
    const AnomalyStatusV1 status = combat->current_combatant(combat->user, &combatant);
    snapshot.combat_status = status.code;
    const bool valid = status.code == ANOMALY_STATUS_V1_OK &&
        (combatant.flags & ANOMALY_NTE_COMBATANT_V1_VALID) != 0;
    if (!valid) {
        snapshot.has_combatant = false;
        snapshot.statistics_initialized = false;
        return false;
    }

    const bool combatant_changed = !snapshot.has_combatant ||
        !SameHandle(snapshot.combatant.world, combatant.world) ||
        !SameHandle(snapshot.combatant.character, combatant.character);
    snapshot.has_combatant = true;
    snapshot.combatant = combatant;

    const double hp_percent = combatant.max_hp > 0.0
        ? std::clamp(combatant.hp / combatant.max_hp * 100.0, 0.0, 100.0)
        : 0.0;
    std::snprintf(
        snapshot.hp.data(), snapshot.hp.size(),
        "HP %.1f / %.1f (%.1f%%)  Shield %.1f",
        combatant.hp, combatant.max_hp, hp_percent, combatant.shield);
    std::snprintf(
        snapshot.state.data(), snapshot.state.size(), "State %s  Sample %llu%s",
        (combatant.flags & ANOMALY_NTE_COMBATANT_V1_DEAD) != 0 ? "DEAD" : "ALIVE",
        static_cast<unsigned long long>(combatant.sequence),
        (combatant.flags & ANOMALY_NTE_COMBATANT_V1_PARTIAL) != 0 ? "  PARTIAL" : "");

    if (damage_changed || combatant_changed || !snapshot.statistics_initialized) {
        AnomalyNteCombatStatisticsV1 outgoing{sizeof(outgoing)};
        AnomalyNteCombatStatisticsV1 incoming{sizeof(incoming)};
        const bool has_outgoing = ReadStatistics(
            combat, combatant, ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_ATTACKER, outgoing);
        const bool has_incoming = ReadStatistics(
            combat, combatant, ANOMALY_NTE_COMBAT_DIRECTION_V1_AS_VICTIM, incoming);
        snapshot.statistics_initialized = true;
        if (has_outgoing) {
            std::snprintf(
                snapshot.outgoing.data(), snapshot.outgoing.size(),
                "Outgoing: %llu hits / %lld final",
                static_cast<unsigned long long>(outgoing.hit_count),
                static_cast<long long>(outgoing.final_damage_total));
        } else {
            std::snprintf(
                snapshot.outgoing.data(), snapshot.outgoing.size(),
                "Outgoing statistics: UNAVAILABLE");
        }
        if (has_incoming) {
            std::snprintf(
                snapshot.incoming.data(), snapshot.incoming.size(),
                "Incoming: %llu hits / %lld final",
                static_cast<unsigned long long>(incoming.hit_count),
                static_cast<long long>(incoming.final_damage_total));
        } else {
            std::snprintf(
                snapshot.incoming.data(), snapshot.incoming.size(),
                "Incoming statistics: UNAVAILABLE");
        }
    }
    return true;
}

const SkillRow* FindPreviousSkill(
    const std::array<SkillRow, kSkillPageCapacity>& rows,
    const std::size_t count,
    const AnomalyGenerationHandleV1 ability_class) noexcept {
    for (std::size_t index{}; index < count; ++index) {
        if (SameHandle(rows[index].ability_class, ability_class)) return &rows[index];
    }
    return nullptr;
}

void UpdateSkillActivation(
    SkillRow& row,
    const AnomalyNteSkillSnapshotV1& skill,
    const AnomalyNteCombatantSnapshotV1* combatant) noexcept {
    row.activation = {};
    row.activation.struct_size = sizeof(row.activation);
    row.skill_id = skill.handle.id;
    row.activation.character = skill.character;
    row.activation.skill = skill.handle;
    if (combatant != nullptr) row.activation.world = combatant->world;
    row.skill_current =
        (skill.flags & ANOMALY_NTE_SKILL_V1_VALID) != 0;
    row.can_activate = combatant != nullptr && row.skill_current;
    std::snprintf(
        row.activation_label.data(), row.activation_label.size(),
        "\xE6\xBF\x80\xE6\xB4\xBB %s##skill-%llu",
        row.name.data(),
        static_cast<unsigned long long>(row.skill_id));
}

std::uint64_t SkillNameKey(const AnomalyGenerationHandleV1 handle) noexcept {
    return (handle.id * 0x9E3779B185EBCA87ULL) ^ handle.generation;
}

void UpdateSkills(
    const AnomalyNteSkillsServiceV1* skills,
    const AnomalyNteCombatantSnapshotV1* combatant,
    const std::uint32_t requested_offset,
    RenderSnapshot& snapshot) {
    if (!SkillMethodsAvailable(skills)) {
        snapshot.skills_status = ANOMALY_STATUS_V1_UNAVAILABLE;
        snapshot.skill_row_count = 0;
        snapshot.skill_total = 0;
        return;
    }

    AnomalyNteSkillFrameV1 frame{sizeof(frame)};
    const AnomalyStatusV1 frame_status = skills->frame(skills->user, &frame);
    snapshot.skills_status = frame_status.code;
    if (frame_status.code != ANOMALY_STATUS_V1_OK) {
        snapshot.skill_row_count = 0;
        snapshot.skill_total = 0;
        return;
    }

    std::uint32_t offset = requested_offset;
    if (snapshot.skill_generation != frame.generation) offset = 0;
    if (offset >= frame.skill_count && frame.skill_count != 0) {
        offset = ((frame.skill_count - 1U) / kSkillPageCapacity) * kSkillPageCapacity;
    }
    const bool all_names_resolved = std::all_of(
        snapshot.skill_rows.begin(),
        snapshot.skill_rows.begin() + snapshot.skill_row_count,
        [](const SkillRow& row) {
            return g_context.skill_name_cache.contains(SkillNameKey(row.ability_class));
        });
    if (snapshot.skill_generation == frame.generation &&
        snapshot.skill_sequence == frame.sequence &&
        snapshot.skill_offset == offset && all_names_resolved) {
        for (std::size_t index{}; index < snapshot.skill_row_count; ++index) {
            SkillRow& row = snapshot.skill_rows[index];
            row.activation.world = combatant != nullptr
                ? combatant->world
                : AnomalyGenerationHandleV1{};
            row.can_activate = combatant != nullptr && row.skill_current;
        }
        return;
    }

    std::array<AnomalyNteSkillSnapshotV1, kSkillPageCapacity> skills_page{};
    for (auto& skill : skills_page) skill.struct_size = sizeof(skill);
    AnomalyNteSkillPageRequestV1 request{sizeof(request)};
    request.generation = frame.generation;
    request.offset = offset;
    request.capacity = kSkillPageCapacity;
    AnomalyNteSkillPageResultV1 page{sizeof(page)};
    AnomalyStatusV1 page_status = skills->page(
        skills->user, &request, skills_page.data(), &page);
    if (page_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
        request.generation = 0;
        request.offset = 0;
        offset = 0;
        page = {sizeof(page)};
        page_status = skills->page(skills->user, &request, skills_page.data(), &page);
    }
    snapshot.skills_status = page_status.code;
    if (page_status.code != ANOMALY_STATUS_V1_OK) {
        snapshot.skill_row_count = 0;
        snapshot.skill_total = 0;
        return;
    }

    const auto previous_rows = snapshot.skill_rows;
    const std::size_t previous_count = snapshot.skill_row_count;
    snapshot.skill_row_count = (std::min)(
        static_cast<std::size_t>(page.returned), snapshot.skill_rows.size());
    for (std::size_t index{}; index < snapshot.skill_row_count; ++index) {
        const AnomalyNteSkillSnapshotV1& skill = skills_page[index];
        SkillRow row;
        row.ability_class = skill.ability_class;
        const SkillRow* previous = FindPreviousSkill(
            previous_rows, previous_count, skill.ability_class);
        const std::uint64_t name_key = SkillNameKey(skill.ability_class);
        std::string name;
        const auto cached_name = g_context.skill_name_cache.find(name_key);
        if (cached_name != g_context.skill_name_cache.end()) {
            name = cached_name->second;
        } else {
            const auto retry = g_context.skill_name_retry.find(name_key);
            if (retry == g_context.skill_name_retry.end() || frame.sequence >= retry->second) {
                name = ResolveAbilityDisplayName(skills, skill.ability_class);
                if (!name.empty()) {
                    g_context.skill_name_cache.emplace(name_key, name);
                    g_context.skill_name_retry.erase(name_key);
                } else {
                    g_context.skill_name_retry[name_key] = frame.sequence + 60U;
                }
            }
        }
        if (!name.empty()) {
            std::snprintf(row.name.data(), row.name.size(), "%s", name.c_str());
        } else if (previous != nullptr && previous->skill_id == skill.handle.id) {
            row.name = previous->name;
        } else {
            const auto fallback = ResolveAbilityFallbackName(skills, skill);
            std::snprintf(row.name.data(), row.name.size(), "%s", fallback.c_str());
        }
        if ((skill.flags & ANOMALY_NTE_SKILL_V1_COOLDOWN_VALID) != 0) {
            std::snprintf(
                row.summary.data(), row.summary.size(),
                "%s  level=%d input=%d cooldown=%.2f/%.2f%s",
                row.name.data(), skill.level, skill.input_id,
                skill.cooldown_remaining_seconds, skill.cooldown_duration_seconds,
                (skill.flags & ANOMALY_NTE_SKILL_V1_ACTIVE) != 0 ? " ACTIVE" : "");
        } else {
            std::snprintf(
                row.summary.data(), row.summary.size(),
                "%s  level=%d input=%d cooldown=unavailable%s",
                row.name.data(), skill.level, skill.input_id,
                (skill.flags & ANOMALY_NTE_SKILL_V1_ACTIVE) != 0 ? " ACTIVE" : "");
        }
        UpdateSkillActivation(row, skill, combatant);
        snapshot.skill_rows[index] = row;
    }
    snapshot.skill_generation = page.generation;
    snapshot.skill_sequence = page.sequence;
    snapshot.skill_offset = offset;
    snapshot.skill_total = page.total_skills;
    snapshot.skill_next_offset = page.next_offset;
}

void FormatHeaders(RenderSnapshot& snapshot) noexcept {
    std::snprintf(
        snapshot.combat_header.data(), snapshot.combat_header.size(),
        "Combat snapshot: %s", StatusName(snapshot.combat_status));
    std::snprintf(
        snapshot.skills_header.data(), snapshot.skills_header.size(),
        "Skills: %s  granted=%u", StatusName(snapshot.skills_status),
        snapshot.skill_total);
}

void ResetState() {
    g_context.combat_cursor = 0;
    g_context.pending_combat_count = 0;
    g_context.combat_update_sequence = 0;
    std::scoped_lock lock(g_context.state_mutex);
    g_context.snapshot = {};
    FormatHeaders(g_context.snapshot);
    g_context.pending_activation = {};
    g_context.activation_queued = false;
    g_context.requested_skill_offset = 0;
    g_context.combat_name_cache.clear();
    g_context.participant_name_cache.clear();
    g_context.skill_name_cache.clear();
    g_context.skill_name_retry.clear();
}

void QueueSkillOffset(const std::uint32_t offset) {
    std::scoped_lock lock(g_context.state_mutex);
    g_context.requested_skill_offset = offset;
}

void QueueActivation(const SkillRow& row) {
    std::scoped_lock lock(g_context.state_mutex);
    if (g_context.activation_queued) return;
    g_context.pending_activation.request = row.activation;
    g_context.pending_activation.skill_id = row.skill_id;
    g_context.pending_activation.skill_name = row.name;
    g_context.activation_queued = true;
}

bool DrawActionButton(
    const AnomalyUiServiceV1* ui,
    const char* label,
    const bool enabled) {
    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button_enabled)>(
            ui, offsetof(AnomalyUiServiceV1, button_enabled)) &&
        ui->button_enabled != nullptr) {
        return ui->button_enabled(
                   ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F,
                   enabled ? 1 : 0) != 0;
    }
    return enabled && ui->button != nullptr &&
        ui->button(ui->user, anomaly::sdk::StringView(label), 0.0F, 0.0F) != 0;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    const auto ui = anomaly::sdk::Host(host).Query<AnomalyUiServiceV1>(
        ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION);
    if (!ui || ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_context.host = host;
    g_context.ui = ui.get();
    ResetState();
    *context = &g_context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) {
    if (context != &g_context) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    ResetState();
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    ResetState();
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    ResetState();
    g_context.host = nullptr;
    g_context.ui = nullptr;
}

void ANOMALY_CALL Update(void* context, double) {
    if (context != &g_context || g_context.host == nullptr) return;

    RenderSnapshot next;
    PendingActivation activation;
    bool has_activation{};
    std::uint32_t requested_offset{};
    {
        std::scoped_lock lock(g_context.state_mutex);
        next = g_context.snapshot;
        requested_offset = g_context.requested_skill_offset;
        if (g_context.activation_queued) {
            activation = g_context.pending_activation;
            g_context.activation_queued = false;
            has_activation = true;
        }
    }

    const anomaly::sdk::Host host(g_context.host);
    const auto combat = host.Query<AnomalyNteCombatServiceV1>(
        ANOMALY_NTE_COMBAT_SERVICE_V1_ID, ANOMALY_NTE_COMBAT_SERVICE_V1_VERSION);
    const bool has_combatant = UpdateCombat(combat.get(), next);
    static_cast<void>(DrainCombat(combat.get(), next));
    const AnomalyNteCombatantSnapshotV1* combatant = has_combatant
        ? &next.combatant
        : nullptr;

    const auto skills = host.Query<AnomalyNteSkillsServiceV1>(
        ANOMALY_NTE_SKILLS_SERVICE_V1_ID, ANOMALY_NTE_SKILLS_SERVICE_V1_VERSION);
    UpdateSkills(skills.get(), combatant, requested_offset, next);

    const auto invocation = host.Query<AnomalyNteSkillInvocationServiceV1>(
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_ID,
        ANOMALY_NTE_SKILL_INVOCATION_SERVICE_V1_VERSION);
    next.invocation_available = InvocationMethodsAvailable(invocation.get());
    if (has_activation) {
        AnomalyStatusV1 status{ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
        AnomalyNteSkillInvocationResultV1 result{sizeof(result)};
        if (next.invocation_available) {
            status = invocation->activate(
                invocation->user, &activation.request, &result);
        }
        next.has_activation_result = true;
        std::snprintf(
            next.activation_result.data(), next.activation_result.size(),
            "%s activation: %s / accepted=%u / tick=%llu",
            activation.skill_name.data(),
            StatusName(status.code),
            status.code == ANOMALY_STATUS_V1_OK ? result.accepted : 0,
            static_cast<unsigned long long>(
                status.code == ANOMALY_STATUS_V1_OK ? result.tick_sequence : 0));
    }

    FormatHeaders(next);

    std::scoped_lock lock(g_context.state_mutex);
    g_context.snapshot = next;
    if (g_context.requested_skill_offset == requested_offset) {
        g_context.requested_skill_offset = next.skill_offset;
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    if (ui == nullptr || ui->begin_window == nullptr || ui->end_window == nullptr ||
        ui->text == nullptr) {
        return;
    }

    RenderSnapshot snapshot;
    {
        std::scoped_lock lock(g_context.state_mutex);
        snapshot = g_context.snapshot;
    }

    int open = 1;
    anomaly::sdk::UiWindow window(ui, "NTE Combat Demo", &open);
    if (!window) return;

    ui->text(ui->user, anomaly::sdk::StringView(snapshot.combat_header.data()));
    if (snapshot.has_combatant) {
        ui->text(ui->user, anomaly::sdk::StringView(snapshot.hp.data()));
        ui->text(ui->user, anomaly::sdk::StringView(snapshot.state.data()));
    }

    const auto draw_rows = [ui](const char* title,
                                const std::array<CombatRow, kRecentCombatCapacity>& rows,
                                const std::size_t count) {
        ui->text(ui->user, anomaly::sdk::StringView(title));
        if (count == 0) {
            ui->text(ui->user, anomaly::sdk::StringView("No events"));
            return;
        }
        for (std::size_t index{}; index < count; ++index) {
            ui->text(ui->user, anomaly::sdk::StringView(rows[index].summary.data()));
        }
    };
    draw_rows("\xE4\xBC\xA4\xE5\xAE\xB3", snapshot.damage_rows, snapshot.damage_row_count);
    draw_rows("\xE6\xB2\xBB\xE7\x96\x97", snapshot.heal_rows, snapshot.heal_row_count);
    draw_rows("\xE5\xA2\x9E\xE7\x9B\x8A", snapshot.buff_rows, snapshot.buff_row_count);

    if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::separator)>(
            ui, offsetof(AnomalyUiServiceV1, separator)) && ui->separator != nullptr) {
        ui->separator(ui->user);
    }
    ui->text(ui->user, anomaly::sdk::StringView(snapshot.skills_header.data()));
    if (snapshot.skills_status == ANOMALY_STATUS_V1_OK) {
        if (DrawActionButton(
                ui, "Previous skill page",
                snapshot.skill_offset >= kSkillPageCapacity)) {
            QueueSkillOffset(snapshot.skill_offset - kSkillPageCapacity);
        }
        if (HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::same_line)>(
                ui, offsetof(AnomalyUiServiceV1, same_line)) && ui->same_line != nullptr) {
            ui->same_line(ui->user, 0.0F, 8.0F);
        }
        if (DrawActionButton(
                ui, "Next skill page",
                snapshot.skill_next_offset < snapshot.skill_total)) {
            QueueSkillOffset(snapshot.skill_next_offset);
        }
    }
    for (std::size_t index{}; index < snapshot.skill_row_count; ++index) {
        const SkillRow& row = snapshot.skill_rows[index];
        ui->text(ui->user, anomaly::sdk::StringView(row.summary.data()));
        if (DrawActionButton(
                ui, row.activation_label.data(),
                snapshot.invocation_available && row.can_activate)) {
            QueueActivation(row);
        }
    }
    if (snapshot.has_activation_result) {
        ui->text(
            ui->user, anomaly::sdk::StringView(snapshot.activation_result.data()));
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.nte-combat-demo"),
        anomaly::sdk::StringView("NTE Combat Demo"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.1.2"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
