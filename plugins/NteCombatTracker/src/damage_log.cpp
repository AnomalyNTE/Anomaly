#include "damage_log.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nte::damage_log {
namespace {

using nte::combat_stats::IsCriticalPositiveDamage;
using nte::combat_stats::kDamageFlagCritical;
using nte::combat_stats::kDamageFlagHeadHit;
using nte::combat_stats::kDamageFlagWeakUnbalance;

constexpr std::size_t kFlushEventCount = 128;
constexpr std::size_t kSealEventCount = 1024;
constexpr std::size_t kSealByteCount = 1024 * 1024;
constexpr std::size_t kMaximumAggregateKeys = 16384;
constexpr std::size_t kMaximumDictionaryEntries = 4096;
constexpr std::size_t kMaximumHistoryEncounters = 100;

std::string JsonEscape(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result += hex[(character >> 4U) & 0x0fU];
                result += hex[character & 0x0fU];
            } else {
                result += static_cast<char>(character);
            }
        }
    }
    return result;
}

std::string Quoted(const std::string_view value) {
    return "\"" + JsonEscape(value) + "\"";
}

std::string Decimal(const std::uint64_t value) {
    return Quoted(std::to_string(value));
}

std::string Decimal(const std::int64_t value) {
    return Quoted(std::to_string(value));
}

bool IsUuid(const std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t index{}; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f') ||
                     (value[index] >= 'A' && value[index] <= 'F'))) {
            return false;
        }
    }
    return true;
}

std::filesystem::path AccountRoot(
    const std::filesystem::path& root,
    const account::AccountIdentity& value) {
    return root / "accounts" / account::DirectoryName(value);
}

void WriteAccountJson(
    std::ostringstream& json, const account::AccountIdentity& value) {
    json << "{\"uid\":" << (value.known() ? Quoted(value.uid) : "null")
         << ",\"source\":" << Quoted(account::SourceName(value.source)) << '}';
}

bool AtomicWrite(
    const std::filesystem::path& path,
    const std::string_view content,
    std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "create directory failed: " + filesystem_error.message();
        return false;
    }
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "open temporary file failed";
            return false;
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream) {
            error = "write temporary file failed";
            return false;
        }
    }
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE || !FlushFileBuffers(file)) {
        const DWORD flush_error = GetLastError();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        error = "flush temporary file failed: " + std::to_string(flush_error);
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    CloseHandle(file);
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "atomic replace failed: " + std::to_string(GetLastError());
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

bool AtomicAppend(
    const std::filesystem::path& path,
    const std::string_view content,
    std::string& error) {
    const std::filesystem::path temporary = path.wstring() + L".append.tmp";
    if (!CopyFileW(path.c_str(), temporary.c_str(), FALSE)) {
        error = "copy append file failed: " + std::to_string(GetLastError());
        return false;
    }
    HANDLE file = CreateFileW(temporary.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "open append file failed: " + std::to_string(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    DWORD written{};
    const bool ok = content.size() <= (std::numeric_limits<DWORD>::max)() &&
        WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
        written == content.size() && FlushFileBuffers(file);
    const DWORD append_error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        error = "append file failed: " + std::to_string(append_error);
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "atomic append replace failed: " + std::to_string(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::string JsonNumber(const double value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream result;
    result << std::setprecision(17) << value;
    return result.str();
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

void ReplaceOnce(std::string& value, const std::string_view from, const std::string_view to) {
    if (const std::size_t position = value.find(from); position != std::string::npos) {
        value.replace(position, from.size(), to);
    }
}

std::optional<std::string> ExtractString(
    const std::string_view json,
    const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const std::size_t start = json.find(needle);
    if (start == std::string_view::npos) return std::nullopt;
    std::string value;
    bool escaped{};
    for (std::size_t index = start + needle.size(); index < json.size(); ++index) {
        const char character = json[index];
        if (escaped) {
            switch (character) {
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            default: value += character; break;
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return value;
        } else {
            value += character;
        }
    }
    return std::nullopt;
}

std::uint64_t ExtractUnsigned(
    const std::string_view json,
    const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    std::size_t start = json.find(needle);
    if (start == std::string_view::npos) return 0;
    start += needle.size();
    if (start < json.size() && json[start] == '"') ++start;
    std::uint64_t result{};
    static_cast<void>(std::from_chars(json.data() + start, json.data() + json.size(), result));
    return result;
}

std::int64_t ExtractSigned(const std::string_view json, const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    std::size_t start = json.find(needle);
    if (start == std::string_view::npos) return 0;
    start += needle.size();
    if (start < json.size() && json[start] == '"') ++start;
    std::int64_t result{};
    static_cast<void>(std::from_chars(json.data() + start, json.data() + json.size(), result));
    return result;
}

bool ExtractBool(const std::string_view json, const std::string_view key, const bool fallback) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const std::size_t start = json.find(needle);
    if (start == std::string_view::npos) return fallback;
    return json.substr(start + needle.size(), 4) == "true";
}

double ExtractDouble(const std::string_view json, const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const std::size_t found = json.find(needle);
    if (found == std::string_view::npos) return 0.0;
    const std::size_t start = found + needle.size();
    std::size_t end = start;
    while (end < json.size() && (json[end] == '-' || json[end] == '+' ||
        json[end] == '.' || (json[end] >= '0' && json[end] <= '9') ||
        json[end] == 'e' || json[end] == 'E')) ++end;
    try {
        return std::stod(std::string(json.substr(start, end - start)));
    } catch (...) {
        return 0.0;
    }
}

std::string Sha256(const std::string_view content) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return {};
    }
    const NTSTATUS created = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (created >= 0) {
        static_cast<void>(BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(content.data())),
            static_cast<ULONG>(content.size()), 0));
        static_cast<void>(BCryptFinishHash(hash, digest.data(),
            static_cast<ULONG>(digest.size()), 0));
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (created < 0) return {};
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) result << std::setw(2) << static_cast<int>(byte);
    return result.str();
}

std::string Sha256File(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Sha256("");
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return {};
    }
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    std::array<char, 64 * 1024> buffer{};
    while (status >= 0 && stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes = stream.gcount();
        if (bytes > 0) {
            status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(bytes), 0);
        }
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, digest.data(),
            static_cast<ULONG>(digest.size()), 0);
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) result << std::setw(2) << static_cast<int>(byte);
    return result.str();
}

std::string SegmentName(const std::size_t index) {
    std::ostringstream name;
    name << "events-" << std::setw(6) << std::setfill('0') << index << ".jsonl";
    return name.str();
}

}  // namespace

struct DamageLogRepository::Impl {
    struct DamageAggregate {
        std::int64_t damage{};
        std::uint64_t hits{};
        std::uint64_t critical_hits{};
        std::uint64_t head_hits{};
        std::uint64_t weak_hits{};
        std::int64_t maximum_hit{};
        std::uint64_t first_offset_us{};
        std::uint64_t last_offset_us{};
    };
    struct ActorEntry {
        std::uint32_t actor_id{};
        combat_stats::Handle handle;
        std::string path;
        std::string display_name;
        std::string name_origin;
        std::string resource_version;
    };
    struct AbilityEntry {
        std::uint32_t ability_id{};
        std::uint64_t source_id{};
        std::string name;
        std::string display_name;
        std::string name_origin;
        std::string resource_version;
    };
    struct SegmentEntry {
        std::string name;
        std::uint64_t events{};
        std::uint64_t bytes{};
        std::string sha256;
    };
    struct ActiveReport {
        ReportOpen open;
        std::filesystem::path directory;
        std::optional<EncounterOpen> encounter;
        std::string encounter_id;
        std::uint64_t event_count{};
        std::uint64_t first_sequence{};
        std::uint64_t last_sequence{};
        std::uint64_t encounter_event_count{};
        std::uint64_t encounter_first_sequence{};
        std::uint64_t encounter_last_sequence{};
        combat_stats::EventTypeCounts encounter_event_type_counts;
        std::uint64_t encounter_count{};
        std::size_t segment_index{1};
        std::size_t segment_events{};
        std::size_t events_since_flush{};
        std::string segment_content;
        std::string actors_content;
        std::string abilities_content;
        std::unordered_map<combat_stats::Handle, ActorEntry,
            combat_stats::HandleHash> actors;
        std::map<std::pair<std::uint64_t, std::string>, AbilityEntry> abilities;
        std::vector<SegmentEntry> segments;
        std::map<std::uint64_t, DamageAggregate> actor_outgoing;
        std::map<std::uint64_t, DamageAggregate> actor_incoming;
        std::map<std::uint64_t, DamageAggregate> ability_stats;
        std::map<std::uint64_t, DamageAggregate> actor_ability;
        std::map<std::uint64_t, DamageAggregate> actor_target;
        std::map<std::uint64_t, DamageAggregate> ability_target;
        bool aggregate_truncated{};
        bool actors_dirty{};
        bool abilities_dirty{};
        std::string write_error;
        ReportHistory history;
    };

    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}

    std::filesystem::path root;
    std::vector<ReportHistory> history;
    std::optional<ActiveReport> active;

    void RefreshActiveBytes() {
        if (!active) return;
        std::error_code filesystem_error;
        std::uint64_t total{};
        for (const auto& file : std::filesystem::directory_iterator(
                 active->directory, filesystem_error)) {
            if (filesystem_error) break;
            if (file.is_regular_file(filesystem_error) && !filesystem_error) {
                total += file.file_size(filesystem_error);
                if (filesystem_error) break;
            }
        }
        if (!filesystem_error) active->history.total_bytes = total;
    }

    static void AddAggregate(
        std::map<std::uint64_t, DamageAggregate>& rows,
        std::uint64_t key,
        const LoggedCombatEvent& damage,
        bool& truncated) {
        if (!rows.contains(key) && rows.size() >= kMaximumAggregateKeys) {
            key = 0;
            truncated = true;
        }
        auto& row = rows[key];
        if (row.hits == 0) row.first_offset_us = damage.received_offset_us;
        row.last_offset_us = damage.received_offset_us;
        if (damage.event.final_value > 0 &&
            row.damage <= (std::numeric_limits<std::int64_t>::max)() -
                damage.event.final_value) {
            row.damage += damage.event.final_value;
        } else if (damage.event.final_value > 0) {
            row.damage = (std::numeric_limits<std::int64_t>::max)();
            truncated = true;
        }
        ++row.hits;
        if (IsCriticalPositiveDamage(damage.event)) ++row.critical_hits;
        if ((damage.event.flags & kDamageFlagHeadHit) != 0) ++row.head_hits;
        if ((damage.event.flags & kDamageFlagWeakUnbalance) != 0) ++row.weak_hits;
        row.maximum_hit = (std::max)(row.maximum_hit, damage.event.final_value);
    }

    static std::uint64_t Composite(const std::uint32_t left, const std::uint32_t right) {
        return (static_cast<std::uint64_t>(left) << 32U) | right;
    }

    static void WriteAggregateFields(
        std::ostringstream& json,
        const DamageAggregate& row,
        const std::uint64_t encounter_duration_us) {
        const double encounter_seconds = (std::max)(1.0,
            static_cast<double>(encounter_duration_us) / 1'000'000.0);
        const double active_seconds = (std::max)(1.0,
            static_cast<double>(row.last_offset_us - row.first_offset_us) / 1'000'000.0);
        json << "\"damage\":" << Decimal(row.damage)
             << ",\"hits\":" << Decimal(row.hits)
             << ",\"critical_hits\":" << Decimal(row.critical_hits)
             << ",\"head_hits\":" << Decimal(row.head_hits)
             << ",\"weak_hits\":" << Decimal(row.weak_hits)
             << ",\"maximum_hit\":" << Decimal(row.maximum_hit)
             << ",\"encounter_dps\":" << static_cast<double>(row.damage) / encounter_seconds
             << ",\"active_dps\":" << static_cast<double>(row.damage) / active_seconds;
    }

    template <typename IdentifierWriter>
    static void WriteAggregateArray(
        std::ostringstream& json,
        const std::map<std::uint64_t, DamageAggregate>& rows,
        const std::uint64_t encounter_duration_us,
        IdentifierWriter write_identifiers) {
        json << '[';
        bool first = true;
        for (const auto& [key, row] : rows) {
            if (!first) json << ',';
            first = false;
            json << '{';
            write_identifiers(json, key);
            json << ',';
            WriteAggregateFields(json, row, encounter_duration_us);
            json << '}';
        }
        json << ']';
    }

    bool WriteCatalog(
        const account::AccountIdentity& account_identity,
        std::string& error) const {
        std::ostringstream json;
        json << "{\"schema_version\":3,\"account\":";
        WriteAccountJson(json, account_identity);
        json << ",\"reports\":[";
        bool first = true;
        for (const auto& item : history) {
            if (item.account != account_identity) continue;
            if (!first) json << ',';
            first = false;
            json << "{\"report_id\":" << Quoted(item.report_id)
                 << ",\"world\":{\"id\":" << Decimal(item.world.id)
                 << ",\"generation\":" << Decimal(item.world.generation) << '}'
                 << ",\"started_utc_ms\":" << item.started_utc_ms
                 << ",\"closed_utc_ms\":" << item.closed_utc_ms
                 << ",\"event_count\":" << Decimal(item.event_count)
                 << ",\"encounter_count\":" << Decimal(item.encounter_count)
                 << ",\"total_bytes\":" << Decimal(item.total_bytes)
                 << ",\"open\":" << (item.open ? "true" : "false")
                 << ",\"complete\":" << (item.complete ? "true" : "false")
                 << ",\"close_reason\":" << Quoted(item.close_reason) << '}';
        }
        json << "]}\n";
        return AtomicWrite(
            AccountRoot(root, account_identity) / "catalog.json", json.str(), error);
    }

    static void RebuildActorsContent(ActiveReport& report) {
        std::vector<const ActorEntry*> rows;
        rows.reserve(report.actors.size());
        for (const auto& [handle, actor] : report.actors) {
            static_cast<void>(handle);
            rows.push_back(&actor);
        }
        std::ranges::sort(rows, {}, &ActorEntry::actor_id);
        report.actors_content.clear();
        report.actors_content =
            "{\"schema_version\":3,\"actor_id\":0,\"handle\":{\"id\":\"0\","
            "\"generation\":\"0\"},\"path\":\"Other\",\"resolution_status\":\"synthetic\"}\n";
        for (const ActorEntry* actor : rows) {
            report.actors_content += "{\"schema_version\":3,\"actor_id\":" +
                std::to_string(actor->actor_id) + ",\"handle\":{\"id\":" +
                Decimal(actor->handle.id) + ",\"generation\":" +
                Decimal(actor->handle.generation) + "},\"path\":" +
                Quoted(actor->path) + ",\"resolution_status\":" +
                Quoted(actor->path.starts_with("Unknown") ? "unknown" : "resolved") +
                ",\"technical_name\":" + Quoted(actor->path) +
                ",\"display_name\":" + Quoted(actor->display_name) +
                ",\"name_origin\":" + Quoted(actor->name_origin) +
                ",\"name_resource_version\":" + Quoted(actor->resource_version) + "}\n";
        }
    }

    static void RebuildAbilitiesContent(ActiveReport& report) {
        std::vector<const AbilityEntry*> rows;
        rows.reserve(report.abilities.size());
        for (const auto& [identity, ability] : report.abilities) {
            static_cast<void>(identity);
            rows.push_back(&ability);
        }
        std::ranges::sort(rows, {}, &AbilityEntry::ability_id);
        report.abilities_content.clear();
        report.abilities_content =
            "{\"schema_version\":3,\"ability_id\":0,\"source_id\":\"0\","
            "\"name\":\"Other\",\"resolution_status\":\"synthetic\"}\n";
        for (const AbilityEntry* ability : rows) {
            report.abilities_content += "{\"schema_version\":3,\"ability_id\":" +
                std::to_string(ability->ability_id) + ",\"source_id\":" +
                Decimal(ability->source_id) + ",\"name\":" +
                Quoted(ability->name) + ",\"resolution_status\":" +
                Quoted(ability->name.starts_with("Unknown") ? "unknown" : "resolved") +
                ",\"technical_name\":" + Quoted(ability->name) +
                ",\"display_name\":" + Quoted(ability->display_name) +
                ",\"name_origin\":" + Quoted(ability->name_origin) +
                ",\"name_resource_version\":" + Quoted(ability->resource_version) + "}\n";
        }
    }

    bool WriteReport(std::string& error) const {
        if (!active) return true;
        const auto& report = *active;
        const auto& history_item = report.history;
        std::ostringstream json;
        json << "{\"schema_version\":3,\"metric_version\":1"
             << ",\"report_id\":" << Quoted(report.open.report_id)
             << ",\"encounter_id\":" << Quoted(report.encounter_id)
             << ",\"capture_scope\":\"player_and_engaged_actors\""
             << ",\"event_scope\":\"player_and_engaged_actors\""
             << ",\"boundary_policy\":\"player_damage_inactivity_10s\""
             << ",\"healing_capture_status\":\"unavailable\""
             << ",\"timestamp_basis\":\"client_receive\""
             << ",\"game_build\":" << Quoted(report.open.game_build)
             << ",\"plugin_version\":" << Quoted(report.open.plugin_version)
             << ",\"runtime_version\":" << Quoted(report.open.runtime_version)
             << ",\"account\":";
        WriteAccountJson(json, report.open.account);
        json
             << ",\"world\":{\"id\":" << Decimal(report.open.world.id)
             << ",\"generation\":" << Decimal(report.open.world.generation) << '}'
             << ",\"started_utc_ms\":" << history_item.started_utc_ms
             << ",\"closed_utc_ms\":" << history_item.closed_utc_ms
             << ",\"event_count\":" << Decimal(history_item.event_count)
             << ",\"first_sequence\":" << Decimal(report.first_sequence)
             << ",\"last_sequence\":" << Decimal(report.last_sequence)
             << ",\"cutoff_sequence\":" << Decimal(
                    report.encounter_last_sequence)
             << ",\"encounter_count\":" << Decimal(history_item.encounter_count)
             << ",\"open\":" << (history_item.open ? "true" : "false")
             << ",\"complete\":" << (history_item.complete ? "true" : "false")
             << ",\"write_error\":" << Quoted(report.write_error)
             << ",\"close_reason\":" << Quoted(history_item.close_reason) << "}\n";
        return AtomicWrite(report.directory / "report.json", json.str(), error);
    }

    std::uint32_t ActorId(const combat_stats::Handle handle, const std::string& path,
        const std::string& display_name, const std::string& name_origin,
        const std::string& resource_version, std::string& error) {
        auto& report = *active;
        if (const auto found = report.actors.find(handle); found != report.actors.end()) {
            if (found->second.path.starts_with("Unknown") && !path.starts_with("Unknown")) {
                found->second.path = path;
                report.actors_dirty = true;
            }
            if (!display_name.empty() && found->second.display_name != display_name) {
                found->second.display_name = display_name;
                found->second.name_origin = name_origin;
                found->second.resource_version = resource_version;
                report.actors_dirty = true;
            }
            if (report.actors_dirty) RebuildActorsContent(report);
            if (report.actors_dirty && AtomicWrite(report.directory / "actors.jsonl",
                    report.actors_content, error)) report.actors_dirty = false;
            return report.actors_dirty ? 0 : found->second.actor_id;
        }
        if (report.actors.size() >= kMaximumDictionaryEntries) {
            report.aggregate_truncated = true;
            return 0;
        }
        const std::uint32_t id = static_cast<std::uint32_t>(report.actors.size() + 1);
        report.actors.emplace(handle, ActorEntry{id, handle, path,
            display_name, name_origin, resource_version});
        report.actors_dirty = true;
        RebuildActorsContent(report);
        if (AtomicWrite(report.directory / "actors.jsonl",
                report.actors_content, error)) report.actors_dirty = false;
        return report.actors_dirty ? 0 : id;
    }

    std::uint32_t AbilityId(const std::uint64_t source_id, const std::string& name,
        const std::string& display_name, const std::string& name_origin,
        const std::string& resource_version, std::string& error) {
        auto& report = *active;
        const auto identity = std::pair{source_id, name};
        if (const auto found = report.abilities.find(identity);
            found != report.abilities.end()) {
            if (!display_name.empty() && found->second.display_name != display_name) {
                found->second.display_name = display_name;
                found->second.name_origin = name_origin;
                found->second.resource_version = resource_version;
                report.abilities_dirty = true;
            }
            if (report.abilities_dirty) RebuildAbilitiesContent(report);
            if (report.abilities_dirty && AtomicWrite(report.directory / "abilities.jsonl",
                    report.abilities_content, error)) report.abilities_dirty = false;
            return report.abilities_dirty ? 0 : found->second.ability_id;
        }
        if (source_id != 0) {
            const auto same_name_id = std::ranges::find_if(
                report.abilities, [source_id](const auto& item) {
                    return item.first.first == source_id;
                });
            if (same_name_id != report.abilities.end()) {
                auto& entry = same_name_id->second;
                const bool incoming_resolved =
                    name_origin == "sdk" || name_origin == "resource";
                const bool existing_unresolved =
                    entry.name_origin != "sdk" && entry.name_origin != "resource";
                if (incoming_resolved && existing_unresolved) {
                    entry.name = name;
                    entry.display_name = display_name;
                    entry.name_origin = name_origin;
                    entry.resource_version = resource_version;
                    report.abilities_dirty = true;
                }
                if (report.abilities_dirty) RebuildAbilitiesContent(report);
                if (report.abilities_dirty && AtomicWrite(
                        report.directory / "abilities.jsonl",
                        report.abilities_content, error)) {
                    report.abilities_dirty = false;
                }
                return report.abilities_dirty ? 0 : entry.ability_id;
            }
        }
        if (report.abilities.size() >= kMaximumDictionaryEntries) {
            report.aggregate_truncated = true;
            return 0;
        }
        const std::uint32_t id = static_cast<std::uint32_t>(report.abilities.size() + 1);
        report.abilities.emplace(identity, AbilityEntry{id, source_id, name,
            display_name, name_origin, resource_version});
        report.abilities_dirty = true;
        RebuildAbilitiesContent(report);
        if (AtomicWrite(report.directory / "abilities.jsonl",
                report.abilities_content, error)) report.abilities_dirty = false;
        return report.abilities_dirty ? 0 : id;
    }

    bool FlushSegment(std::string& error) {
        if (!active || active->segment_content.empty()) return true;
        const bool written = AtomicWrite(active->directory /
            SegmentName(active->segment_index), active->segment_content, error);
        if (written) active->events_since_flush = 0;
        return written;
    }

    bool WriteIndex(std::string& error) const {
        if (!active) return true;
        std::ostringstream json;
        json << "{\"schema_version\":3,\"report_id\":"
             << Quoted(active->open.report_id) << ",\"segments\":[";
        std::string content_hash_input;
        for (std::size_t index{}; index < active->segments.size(); ++index) {
            if (index != 0) json << ',';
            const auto& segment = active->segments[index];
            content_hash_input += segment.sha256;
            json << "{\"name\":" << Quoted(segment.name)
                 << ",\"events\":" << Decimal(segment.events)
                 << ",\"bytes\":" << Decimal(segment.bytes)
                 << ",\"sha256\":" << Quoted(segment.sha256) << '}';
        }
        if (!active->segment_content.empty()) {
            if (!active->segments.empty()) json << ',';
            const std::string active_hash = Sha256(active->segment_content);
            content_hash_input += active_hash;
            json << "{\"name\":" << Quoted(SegmentName(active->segment_index))
                 << ",\"events\":" << Decimal(active->segment_events)
                 << ",\"bytes\":" << Decimal(
                        static_cast<std::uint64_t>(active->segment_content.size()))
                 << ",\"sha256\":" << Quoted(active_hash)
                 << ",\"sealed\":false}";
        }
        content_hash_input += Sha256(active->actors_content);
        content_hash_input += Sha256(active->abilities_content);
        const std::string encounters_hash = Sha256File(
            active->directory / "encounters.jsonl");
        content_hash_input += encounters_hash;
        json << "],\"actors_sha256\":" << Quoted(Sha256(active->actors_content))
             << ",\"abilities_sha256\":" << Quoted(Sha256(active->abilities_content))
             << ",\"encounters_sha256\":" << Quoted(encounters_hash)
             << ",\"content_sha256\":" << Quoted(Sha256(content_hash_input)) << "}\n";
        return AtomicWrite(active->directory / "index.json", json.str(), error);
    }

    bool SealSegment(std::string& error) {
        if (!active || active->segment_content.empty()) return true;
        if (!FlushSegment(error)) return false;
        active->segments.push_back({SegmentName(active->segment_index),
            active->segment_events,
            static_cast<std::uint64_t>(active->segment_content.size()),
            Sha256(active->segment_content)});
        ++active->segment_index;
        active->segment_events = 0;
        active->segment_content.clear();
        return WriteIndex(error);
    }
};

DamageLogRepository::DamageLogRepository(std::filesystem::path root)
    : impl_(std::make_unique<Impl>(std::move(root))) {}
DamageLogRepository::~DamageLogRepository() = default;
DamageLogRepository::DamageLogRepository(DamageLogRepository&&) noexcept = default;
DamageLogRepository& DamageLogRepository::operator=(DamageLogRepository&&) noexcept = default;

bool DamageLogRepository::Recover(std::string& error) {
    error.clear();
    impl_->history.clear();
    std::error_code filesystem_error;
    const auto accounts_root = impl_->root / "accounts";
    std::filesystem::create_directories(accounts_root, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    std::vector<account::AccountIdentity> recovered_accounts;
    for (const auto& account_entry : std::filesystem::directory_iterator(
             accounts_root, filesystem_error)) {
        if (filesystem_error) break;
        if (!account_entry.is_directory()) continue;
        const auto account_identity = account::FromDirectoryName(
            account_entry.path().filename().string());
        if (!account_identity) continue;
        recovered_accounts.push_back(*account_identity);
        const auto reports = account_entry.path() / "reports";
        if (!std::filesystem::is_directory(reports, filesystem_error)) {
            filesystem_error.clear();
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(
                 reports, filesystem_error)) {
            if (filesystem_error) break;
            if (!entry.is_directory() || !IsUuid(entry.path().filename().string())) continue;
            const auto content = ReadFile(entry.path() / "report.json");
            if (!content) continue;
            const auto schema = ExtractUnsigned(*content, "schema_version");
            if (schema != 2 && schema != kSchemaVersion) continue;
            ReportHistory item;
            item.account = *account_identity;
            item.report_id = ExtractString(*content, "report_id").value_or("");
            if (!IsUuid(item.report_id)) continue;
            item.world.id = ExtractUnsigned(*content, "id");
            item.world.generation = ExtractUnsigned(*content, "generation");
            item.started_utc_ms = ExtractSigned(*content, "started_utc_ms");
            item.closed_utc_ms = ExtractSigned(*content, "closed_utc_ms");
            item.event_count = ExtractUnsigned(*content, "event_count");
            item.encounter_count = ExtractUnsigned(*content, "encounter_count");
            item.open = ExtractBool(*content, "open", true);
            item.complete = ExtractBool(*content, "complete", false);
            item.close_reason = ExtractString(*content, "close_reason").value_or("");
            if (item.open) {
                item.open = false;
                item.complete = false;
                item.close_reason = "unclean_shutdown";
                std::string repaired = *content;
                ReplaceOnce(repaired, "\"open\":true", "\"open\":false");
                ReplaceOnce(repaired, "\"complete\":true", "\"complete\":false");
                ReplaceOnce(repaired, "\"close_reason\":\"\"",
                    "\"close_reason\":\"unclean_shutdown\"");
                if (!AtomicWrite(entry.path() / "report.json", repaired, error)) return false;
            }
            if (const auto encounters = ReadFile(entry.path() / "encounters.jsonl")) {
                std::istringstream lines(*encounters);
                std::string line;
                while (std::getline(lines, line)) {
                    if (line.empty()) continue;
                    const auto encounter_schema = ExtractUnsigned(line, "schema_version");
                    if (encounter_schema != 2 &&
                        encounter_schema != kSchemaVersion) continue;
                    EncounterHistory encounter;
                    encounter.encounter_id = ExtractString(line, "encounter_id").value_or("");
                    if (!IsUuid(encounter.encounter_id)) continue;
                    encounter.end_reason = ExtractString(line, "end_reason").value_or("unknown");
                    encounter.duration_seconds = ExtractDouble(line, "duration_seconds");
                    encounter.outgoing_damage = ExtractSigned(line, "outgoing_damage");
                    encounter.incoming_damage = ExtractSigned(line, "incoming_damage");
                    encounter.encounter_dps = ExtractDouble(line, "encounter_dps");
                    encounter.complete = ExtractBool(line, "complete", false);
                    item.encounters.push_back(std::move(encounter));
                    if (item.encounters.size() > kMaximumHistoryEncounters) {
                        item.encounters.erase(item.encounters.begin());
                    }
                }
            }
            for (const auto& file : std::filesystem::recursive_directory_iterator(
                     entry.path(), filesystem_error)) {
                if (!filesystem_error && file.is_regular_file()) {
                    item.total_bytes += file.file_size();
                }
            }
            if (filesystem_error) break;
            impl_->history.push_back(std::move(item));
        }
        if (filesystem_error) break;
    }
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    std::ranges::sort(impl_->history, [](const auto& left, const auto& right) {
        return left.started_utc_ms > right.started_utc_ms;
    });
    for (const auto& account_identity : recovered_accounts) {
        if (!impl_->WriteCatalog(account_identity, error)) return false;
    }
    return true;
}

bool DamageLogRepository::BeginReport(const ReportOpen& report, std::string& error) {
    if (impl_->active || !IsUuid(report.report_id) ||
        !account::IsValid(report.account)) {
        error = "invalid or duplicate report";
        return false;
    }
    Impl::ActiveReport active;
    active.open = report;
    active.directory = AccountRoot(impl_->root, report.account) /
        "reports" / report.report_id;
    active.history.report_id = report.report_id;
    active.history.world = report.world;
    active.history.started_utc_ms = report.started_utc_ms;
    active.history.open = true;
    active.history.account = report.account;
    std::error_code filesystem_error;
    std::filesystem::create_directories(active.directory, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    impl_->history.insert(impl_->history.begin(), active.history);
    impl_->active = std::move(active);
    if (!AtomicWrite(impl_->active->directory / "actors.jsonl", "", error) ||
        !AtomicWrite(impl_->active->directory / "abilities.jsonl", "", error) ||
        !AtomicWrite(impl_->active->directory / "encounters.jsonl", "", error) ||
        !impl_->WriteIndex(error) || !impl_->WriteReport(error) ||
        !impl_->WriteCatalog(report.account, error)) {
        impl_->active.reset();
        if (!impl_->history.empty() &&
            impl_->history.front().report_id == report.report_id) {
            impl_->history.erase(impl_->history.begin());
        }
        return false;
    }
    return true;
}

bool DamageLogRepository::BeginEncounter(
    const EncounterOpen& encounter, std::string& error) {
    if (!impl_->active || impl_->active->encounter ||
        impl_->active->encounter_count != 0 || !IsUuid(encounter.encounter_id)) {
        error = "invalid encounter state";
        return false;
    }
    impl_->active->encounter = encounter;
    impl_->active->encounter_id = encounter.encounter_id;
    impl_->active->actor_outgoing.clear();
    impl_->active->actor_incoming.clear();
    impl_->active->ability_stats.clear();
    impl_->active->actor_ability.clear();
    impl_->active->actor_target.clear();
    impl_->active->ability_target.clear();
    impl_->active->aggregate_truncated = false;
    impl_->active->encounter_event_count = 0;
    impl_->active->encounter_first_sequence = 0;
    impl_->active->encounter_last_sequence = 0;
    impl_->active->encounter_event_type_counts = {};
    return true;
}

bool DamageLogRepository::Append(const LoggedCombatEvent& damage, std::string& error) {
    if (!impl_->active || !impl_->active->encounter) {
        error = "no active encounter";
        return false;
    }
    auto& report = *impl_->active;
    if (!damage.encounter_id.empty() &&
        damage.encounter_id != report.encounter->encounter_id) {
        error = "encounter token mismatch";
        return false;
    }
    error.clear();
    const std::uint32_t attacker_id = impl_->ActorId(
        damage.event.attacker, damage.event.attacker_name,
        damage.event.attacker_display_name, damage.event.attacker_name_origin,
        damage.event.name_resource_version, error);
    if (!error.empty()) return false;
    const std::uint32_t victim_id = impl_->ActorId(
        damage.event.victim, damage.event.victim_name,
        damage.event.victim_display_name, damage.event.victim_name_origin,
        damage.event.name_resource_version, error);
    if (!error.empty()) return false;
    const std::uint32_t ability_id = impl_->AbilityId(
        damage.event.name_id, damage.event.source_name,
        damage.event.source_display_name, damage.event.source_name_origin,
        damage.event.name_resource_version, error);
    if (!error.empty()) return false;
    if (damage.event.kind == combat_stats::CombatEventKind::Damage && damage.event.final_value > 0) {
        Impl::AddAggregate(report.actor_outgoing, attacker_id, damage,
            report.aggregate_truncated);
        Impl::AddAggregate(report.actor_incoming, victim_id, damage,
            report.aggregate_truncated);
        Impl::AddAggregate(report.ability_stats, ability_id, damage,
            report.aggregate_truncated);
        Impl::AddAggregate(report.actor_ability,
            Impl::Composite(attacker_id, ability_id), damage,
            report.aggregate_truncated);
        Impl::AddAggregate(report.actor_target,
            Impl::Composite(attacker_id, victim_id), damage,
            report.aggregate_truncated);
        Impl::AddAggregate(report.ability_target,
            Impl::Composite(ability_id, victim_id), damage,
            report.aggregate_truncated);
    }
    std::ostringstream json;
    json << std::setprecision(17)
         << "{\"schema_version\":3,\"event_type\":"
         << Quoted(combat_stats::CombatEventKindName(damage.event.kind))
         << ",\"report_id\":" << Quoted(report.open.report_id)
         << ",\"encounter_id\":" << Quoted(report.encounter->encounter_id)
         << ",\"sequence\":" << Decimal(damage.event.sequence)
         << ",\"tick_sequence\":" << Decimal(damage.event.tick_sequence)
         << ",\"received_utc_ms\":" << damage.received_utc_ms
         << ",\"received_offset_us\":" << Decimal(damage.received_offset_us)
         << ",\"timestamp_basis\":\"client_receive\""
         << ",\"flags\":" << damage.event.flags
         << ",\"critical\":" << (damage.event.kind == combat_stats::CombatEventKind::Damage &&
                (damage.event.flags & kDamageFlagCritical) != 0 ? "true" : "false")
         << ",\"head_hit\":" << (damage.event.kind == combat_stats::CombatEventKind::Damage &&
                (damage.event.flags & kDamageFlagHeadHit) != 0 ? "true" : "false")
         << ",\"weak_unbalance\":" << (damage.event.kind == combat_stats::CombatEventKind::Damage &&
                (damage.event.flags & kDamageFlagWeakUnbalance) != 0 ? "true" : "false")
         << ",\"player_related\":" << (damage.player_related ? "true" : "false")
         << ",\"world\":{\"id\":" << Decimal(damage.event.world.id)
         << ",\"generation\":" << Decimal(damage.event.world.generation) << '}'
         << ",\"source_actor_id\":" << attacker_id
         << ",\"target_actor_id\":" << victim_id
         << ",\"ability_id\":" << ability_id
         << ",\"source\":{\"id\":" << Decimal(damage.event.attacker.id)
         << ",\"generation\":" << Decimal(damage.event.attacker.generation) << '}'
         << ",\"target\":{\"id\":" << Decimal(damage.event.victim.id)
         << ",\"generation\":" << Decimal(damage.event.victim.generation) << '}'
          << ",\"name_id\":" << Decimal(damage.event.name_id)
         << ",\"event_name\":" << Quoted(damage.event.source_name)
         << ",\"event_display_name\":" << Quoted(damage.event.source_display_name)
         << ",\"event_name_origin\":" << Quoted(damage.event.source_name_origin)
         << ",\"source_path\":" << Quoted(damage.event.attacker_name)
         << ",\"source_display_name\":" << Quoted(damage.event.attacker_display_name)
         << ",\"source_name_origin\":" << Quoted(damage.event.attacker_name_origin)
         << ",\"target_path\":" << Quoted(damage.event.victim_name)
         << ",\"target_display_name\":" << Quoted(damage.event.victim_display_name)
         << ",\"target_name_origin\":" << Quoted(damage.event.victim_name_origin)
         << ",\"name_resource_version\":" << Quoted(damage.event.name_resource_version)
         << ",\"value\":" << Decimal(damage.event.value)
         << ",\"basic_value\":" << Decimal(damage.event.basic_value)
         << ",\"final_value\":" << Decimal(damage.event.final_value)
         << ",\"hit_location\":[" << JsonNumber(damage.event.hit_location[0]) << ','
         << JsonNumber(damage.event.hit_location[1]) << ','
         << JsonNumber(damage.event.hit_location[2]) << ']'
         << ",\"damage_type\":" << damage.event.damage_type
         << ",\"display_type\":" << damage.event.display_type
         << ",\"reaction_type\":" << damage.event.reaction_type
          << ",\"reaction_display_type\":" << damage.event.reaction_display_type
          << ",\"duration_seconds\":" << JsonNumber(damage.event.duration_seconds)
          << ",\"stack_count\":" << damage.event.stack_count
         << "}\n";
    report.segment_content += json.str();
    ++report.segment_events;
    ++report.events_since_flush;
    ++report.event_count;
    if (report.first_sequence == 0) report.first_sequence = damage.event.sequence;
    report.last_sequence = damage.event.sequence;
    if (report.encounter_first_sequence == 0) {
        report.encounter_first_sequence = damage.event.sequence;
    }
    report.encounter_last_sequence = damage.event.sequence;
    ++report.encounter_event_count;
    switch (damage.event.kind) {
    case combat_stats::CombatEventKind::Damage:
        ++report.encounter_event_type_counts.damage;
        break;
    case combat_stats::CombatEventKind::Heal:
        ++report.encounter_event_type_counts.heal;
        break;
    case combat_stats::CombatEventKind::BuffAdd:
        ++report.encounter_event_type_counts.buff_add;
        break;
    case combat_stats::CombatEventKind::BuffRemove:
        ++report.encounter_event_type_counts.buff_remove;
        break;
    }
    report.history.event_count = report.event_count;
    impl_->history.front().event_count = report.event_count;
    if (report.segment_events >= kSealEventCount ||
        report.segment_content.size() >= kSealByteCount) return impl_->SealSegment(error);
    if (report.events_since_flush >= kFlushEventCount) return impl_->FlushSegment(error);
    return true;
}

bool DamageLogRepository::Flush(std::string& error) {
    if (!impl_->FlushSegment(error)) return false;
    if (!impl_->active) return true;
    impl_->RefreshActiveBytes();
    impl_->history.front() = impl_->active->history;
    return impl_->WriteReport(error) && impl_->WriteIndex(error) &&
        impl_->WriteCatalog(impl_->active->open.account, error);
}

bool DamageLogRepository::EndEncounter(
    const EncounterClose& encounter, std::string& error) {
    if (!impl_->active || !impl_->active->encounter ||
        impl_->active->encounter->encounter_id != encounter.encounter_id) {
        error = "encounter mismatch";
        return false;
    }
    auto& report = *impl_->active;
    const auto opened = *report.encounter;
    const double duration = encounter.duration_seconds > 0.0
        ? encounter.duration_seconds
        : (encounter.closed_offset_us >= opened.started_offset_us
              ? static_cast<double>(encounter.closed_offset_us - opened.started_offset_us) /
                    1'000'000.0
              : 0.0);
    const bool complete = !encounter.quality.partial && !encounter.quality.overflow &&
        !encounter.quality.cursor_gap && !report.aggregate_truncated &&
        encounter.dropped_events == 0 && encounter.write_error.empty() &&
        encounter.service_unavailable_intervals.empty() &&
        !encounter.open_missing_sequence_start &&
        !encounter.service_intervals_truncated && !encounter.missing_ranges_truncated;
    const auto encounter_duration_us = static_cast<std::uint64_t>(
        (std::max)(0.0, duration) * 1'000'000.0);
    struct ControlledPlayerIds {
        const combat_stats::PlayerRow* player{};
        std::uint32_t actor_id{};
        std::uint32_t maximum_ability_id{};
    };
    std::vector<ControlledPlayerIds> controlled_players;
    controlled_players.reserve(encounter.confirmed_controlled_players.size());
    for (const auto& player : encounter.confirmed_controlled_players) {
        const std::string path = player.object_path.empty() ? player.label : player.object_path;
        const std::uint32_t actor_id = impl_->ActorId(player.handle, path,
            player.display_name.empty() ? player.label : player.display_name,
            player.name_origin, player.name_resource_version, error);
        if (actor_id == 0 && !error.empty()) return false;
        std::uint32_t ability_id{};
        if (player.maximum_source_id != 0) {
            ability_id = impl_->AbilityId(player.maximum_source_id,
                player.maximum_source_name,
                player.maximum_source_display_name,
                player.maximum_source_name_origin,
                player.maximum_source_resource_version, error);
            if (ability_id == 0 && !error.empty()) return false;
        }
        controlled_players.push_back({&player, actor_id, ability_id});
    }
    std::ostringstream line;
    line << "{\"schema_version\":3,\"metric_version\":1"
         << ",\"encounter_id\":" << Quoted(encounter.encounter_id)
         << ",\"started_utc_ms\":" << opened.started_utc_ms
         << ",\"closed_utc_ms\":" << encounter.closed_utc_ms
         << ",\"duration_seconds\":" << JsonNumber(duration)
         << ",\"end_reason\":" << Quoted(encounter.reason)
         << ",\"outgoing_damage\":" << Decimal(encounter.outgoing_damage)
         << ",\"incoming_damage\":" << Decimal(encounter.incoming_damage)
         << ",\"encounter_dps\":" << JsonNumber(encounter.encounter_dps)
         << ",\"outgoing_healing\":" << Decimal(encounter.outgoing_healing)
         << ",\"incoming_healing\":" << Decimal(encounter.incoming_healing)
         << ",\"outgoing_heal_events\":" << Decimal(encounter.outgoing_heal_events)
         << ",\"incoming_heal_events\":" << Decimal(encounter.incoming_heal_events)
         << ",\"encounter_hps\":" << JsonNumber(encounter.encounter_hps)
         << ",\"encounter_received_hps\":"
         << JsonNumber(encounter.encounter_received_hps)
         << ",\"event_count\":" << Decimal(report.encounter_event_count)
         << ",\"event_type_counts\":{\"damage\":"
         << Decimal(report.encounter_event_type_counts.damage)
         << ",\"heal\":" << Decimal(report.encounter_event_type_counts.heal)
         << ",\"buff_add\":" << Decimal(report.encounter_event_type_counts.buff_add)
         << ",\"buff_remove\":" << Decimal(report.encounter_event_type_counts.buff_remove)
         << '}'
         << ",\"first_sequence\":" << Decimal(report.encounter_first_sequence)
         << ",\"last_sequence\":" << Decimal(report.encounter_last_sequence)
         << ",\"cutoff_sequence\":" << Decimal(encounter.cutoff_sequence)
         << ",\"dropped_events\":" << Decimal(encounter.dropped_events)
         << ",\"dropped_first_sequence\":" << Decimal(encounter.dropped_first_sequence)
         << ",\"dropped_last_sequence\":" << Decimal(encounter.dropped_last_sequence)
         << ",\"write_error\":" << Quoted(encounter.write_error)
         << ",\"service_unavailable_intervals\":[";
    for (std::size_t index{}; index < encounter.service_unavailable_intervals.size(); ++index) {
        if (index != 0) line << ',';
        line << "{\"start_offset_us\":"
             << Decimal(encounter.service_unavailable_intervals[index].first)
             << ",\"end_offset_us\":"
             << Decimal(encounter.service_unavailable_intervals[index].second) << '}';
    }
    line << ']'
         << ",\"missing_sequence_ranges\":[";
    for (std::size_t index{}; index < encounter.missing_sequence_ranges.size(); ++index) {
        if (index != 0) line << ',';
        line << "{\"first\":" << Decimal(encounter.missing_sequence_ranges[index].first)
             << ",\"last\":" << Decimal(encounter.missing_sequence_ranges[index].second)
             << '}';
    }
    line << ']'
         << ",\"open_missing_sequence_start\":";
    if (encounter.open_missing_sequence_start) {
        line << Decimal(*encounter.open_missing_sequence_start);
    } else {
        line << "null";
    }
    line << ",\"critical_data_status\":"
         << Quoted(combat_stats::CriticalDataStatusName(
                encounter.critical_data_status))
         << ",\"damage_flag_histogram\":[";
    for (std::size_t index{}; index < encounter.damage_flag_histogram.size(); ++index) {
        if (index != 0) line << ',';
        const auto& item = encounter.damage_flag_histogram[index];
        line << "{\"flags\":" << item.flags
             << ",\"count\":" << Decimal(item.count) << '}';
    }
    line << "]"
         << ",\"confirmed_controlled_players\":[";
    for (std::size_t index{}; index < controlled_players.size(); ++index) {
        if (index != 0) line << ',';
        const auto& ids = controlled_players[index];
        const auto& player = *ids.player;
        line << "{\"actor_id\":" << ids.actor_id
             << ",\"label\":" << Quoted(player.label)
             << ",\"display_name\":" << Quoted(player.display_name)
             << ",\"name_origin\":" << Quoted(player.name_origin)
             << ",\"name_resource_version\":"
             << Quoted(player.name_resource_version)
             << ",\"damage\":" << Decimal(player.damage)
             << ",\"positive_hits\":" << Decimal(player.positive_hits)
             << ",\"critical_hits\":" << Decimal(player.critical_hits)
             << ",\"head_hits\":" << Decimal(player.head_hits)
             << ",\"weak_hits\":" << Decimal(player.weak_hits)
             << ",\"damage_share\":" << JsonNumber(player.damage_share)
             << ",\"encounter_dps\":" << JsonNumber(player.encounter_dps)
             << ",\"active_dps\":" << JsonNumber(player.active_dps)
             << ",\"rolling_dps_10s\":" << JsonNumber(player.rolling_dps)
             << ",\"healing_done\":" << Decimal(player.healing_done)
             << ",\"healing_done_events\":" << Decimal(player.healing_done_events)
             << ",\"maximum_heal\":" << Decimal(player.maximum_heal)
             << ",\"healing_share\":" << JsonNumber(player.healing_share)
             << ",\"encounter_hps\":" << JsonNumber(player.encounter_hps)
             << ",\"healing_received\":" << Decimal(player.healing_received)
             << ",\"healing_received_events\":" << Decimal(player.healing_received_events)
             << ",\"maximum_heal_received\":" << Decimal(player.maximum_heal_received)
             << ",\"healing_received_share\":" << JsonNumber(player.healing_received_share)
             << ",\"healing_received_hps\":" << JsonNumber(player.healing_received_hps)
             << ",\"maximum_hit\":" << Decimal(player.maximum_hit)
             << ",\"maximum_ability_id\":" << ids.maximum_ability_id << '}';
    }
    line << ']';
    line
         << ",\"complete\":" << (complete ? "true" : "false")
         << ",\"quality\":{\"partial\":" << (encounter.quality.partial ? "true" : "false")
         << ",\"overflow\":" << (encounter.quality.overflow ? "true" : "false")
         << ",\"cursor_gap\":" << (encounter.quality.cursor_gap ? "true" : "false")
         << ",\"aggregate_truncated\":"
         << ((encounter.quality.aggregate_truncated || report.aggregate_truncated) ? "true" : "false")
         << ",\"service_intervals_truncated\":"
         << (encounter.service_intervals_truncated ? "true" : "false")
         << ",\"missing_ranges_truncated\":"
         << (encounter.missing_ranges_truncated ? "true" : "false")
         << "},\"actor_outgoing\":";
    Impl::WriteAggregateArray(line, report.actor_outgoing, encounter_duration_us,
        [](auto& json, const std::uint64_t key) { json << "\"actor_id\":" << key; });
    line << ",\"actor_incoming\":";
    Impl::WriteAggregateArray(line, report.actor_incoming, encounter_duration_us,
        [](auto& json, const std::uint64_t key) { json << "\"actor_id\":" << key; });
    line << ",\"ability_stats\":";
    Impl::WriteAggregateArray(line, report.ability_stats, encounter_duration_us,
        [](auto& json, const std::uint64_t key) { json << "\"ability_id\":" << key; });
    const auto composite_ids = [](const char* left, const char* right) {
        return [left, right](auto& json, const std::uint64_t key) {
            json << '\"' << left << "\":" << (key >> 32U)
                 << ",\"" << right << "\":" << (key & 0xffffffffULL);
        };
    };
    line << ",\"actor_ability\":";
    Impl::WriteAggregateArray(line, report.actor_ability, encounter_duration_us,
        composite_ids("actor_id", "ability_id"));
    line << ",\"actor_target\":";
    Impl::WriteAggregateArray(line, report.actor_target, encounter_duration_us,
        composite_ids("actor_id", "target_actor_id"));
    line << ",\"ability_target\":";
    Impl::WriteAggregateArray(line, report.ability_target, encounter_duration_us,
        composite_ids("ability_id", "target_actor_id"));
    line << "}\n";
    if (!AtomicAppend(report.directory / "encounters.jsonl", line.str(), error)) return false;
    ++report.encounter_count;
    report.history.encounter_count = report.encounter_count;
    report.history.encounters.push_back({encounter.encounter_id, encounter.reason,
        duration, encounter.outgoing_damage, encounter.incoming_damage,
        encounter.encounter_dps, complete});
    if (report.history.encounters.size() > kMaximumHistoryEncounters) {
        report.history.encounters.erase(report.history.encounters.begin());
    }
    if (!complete) report.history.complete = false;
    impl_->RefreshActiveBytes();
    impl_->history.front() = report.history;
    report.encounter.reset();
    return impl_->FlushSegment(error) && impl_->WriteReport(error) &&
        impl_->WriteIndex(error) &&
        impl_->WriteCatalog(report.open.account, error);
}

bool DamageLogRepository::CloseReport(const ReportClose& close, std::string& error) {
    if (!impl_->active) {
        error = "no active report";
        return false;
    }
    auto& report = *impl_->active;
    if (report.encounter) {
        if (!close.incomplete && !close.unclean_shutdown) {
            error = "report cannot close with active encounter";
            return false;
        }
        const auto opened = *report.encounter;
        const double duration = close.closed_utc_ms >= opened.started_utc_ms
            ? static_cast<double>(close.closed_utc_ms - opened.started_utc_ms) /
                1000.0
            : 0.0;
        std::ostringstream aborted;
        aborted << "{\"schema_version\":3,\"metric_version\":1"
                << ",\"encounter_id\":" << Quoted(opened.encounter_id)
                << ",\"started_utc_ms\":" << opened.started_utc_ms
                << ",\"closed_utc_ms\":" << close.closed_utc_ms
                << ",\"duration_seconds\":" << JsonNumber(duration)
                << ",\"end_reason\":\"unclean_shutdown\""
                << ",\"outgoing_damage\":\"0\",\"incoming_damage\":\"0\""
                << ",\"encounter_dps\":0,\"outgoing_healing\":\"0\""
                << ",\"incoming_healing\":\"0\",\"outgoing_heal_events\":\"0\""
                << ",\"incoming_heal_events\":\"0\",\"encounter_hps\":0"
                << ",\"encounter_received_hps\":0,\"event_count\":"
                << Decimal(report.encounter_event_count)
                << ",\"event_type_counts\":{\"damage\":"
                << Decimal(report.encounter_event_type_counts.damage)
                << ",\"heal\":" << Decimal(report.encounter_event_type_counts.heal)
                << ",\"buff_add\":" << Decimal(report.encounter_event_type_counts.buff_add)
                << ",\"buff_remove\":" << Decimal(report.encounter_event_type_counts.buff_remove)
                << "},\"first_sequence\":" << Decimal(report.encounter_first_sequence)
                << ",\"last_sequence\":" << Decimal(report.encounter_last_sequence)
                << ",\"cutoff_sequence\":" << Decimal(report.encounter_last_sequence)
                << ",\"dropped_events\":\"0\",\"dropped_first_sequence\":\"0\""
                << ",\"dropped_last_sequence\":\"0\",\"write_error\":"
                << Quoted(close.write_error)
                << ",\"service_unavailable_intervals\":[],\"missing_sequence_ranges\":[]"
                << ",\"open_missing_sequence_start\":null"
                << ",\"critical_data_status\":\"unavailable\""
                << ",\"damage_flag_histogram\":[],\"confirmed_controlled_players\":[]"
                << ",\"complete\":false,\"quality\":{\"partial\":true"
                << ",\"overflow\":false,\"cursor_gap\":false"
                << ",\"aggregate_truncated\":false"
                << ",\"service_intervals_truncated\":false"
                << ",\"missing_ranges_truncated\":false}"
                << ",\"actor_outgoing\":[],\"actor_incoming\":[]"
                << ",\"ability_stats\":[],\"actor_ability\":[]"
                << ",\"actor_target\":[],\"ability_target\":[]}\n";
        if (!AtomicAppend(report.directory / "encounters.jsonl",
                aborted.str(), error)) return false;
        ++report.encounter_count;
        report.history.encounter_count = report.encounter_count;
        report.history.encounters.push_back({opened.encounter_id,
            "unclean_shutdown", duration, 0, 0, 0.0, false});
        report.encounter.reset();
        report.history.complete = false;
    }
    if (!impl_->SealSegment(error)) return false;
    if (!impl_->WriteIndex(error)) return false;
    std::error_code filesystem_error;
    std::uint64_t total_bytes{};
    for (const auto& file : std::filesystem::recursive_directory_iterator(
             report.directory, filesystem_error)) {
        if (!filesystem_error && file.is_regular_file()) {
            total_bytes += file.file_size();
        }
    }
    report.history.open = false;
    report.history.closed_utc_ms = close.closed_utc_ms;
    report.history.close_reason = close.unclean_shutdown ? "unclean_shutdown" : close.reason;
    report.history.complete = report.history.complete && !close.unclean_shutdown &&
        !close.incomplete;
    report.history.total_bytes = total_bytes;
    report.write_error = close.write_error;
    impl_->history.front() = report.history;
    if (!impl_->WriteCatalog(report.open.account, error) ||
        !impl_->WriteReport(error)) {
        const std::string close_error = error.empty() ? "report close failed" : error;
        report.history.complete = false;
        report.write_error = close_error;
        impl_->history.front() = report.history;
        std::string ignored;
        static_cast<void>(impl_->WriteReport(ignored));
        static_cast<void>(impl_->WriteCatalog(report.open.account, ignored));
        error = close_error;
        return false;
    }
    impl_->active.reset();
    return true;
}

bool DamageLogRepository::DeleteReport(
    const account::AccountIdentity& account_identity,
    const std::string& report_id,
    std::string& error) {
    if (!account::IsValid(account_identity) || !IsUuid(report_id) ||
        (impl_->active && impl_->active->open.account == account_identity &&
         impl_->active->open.report_id == report_id)) {
        error = "report is invalid or active";
        return false;
    }
    std::error_code filesystem_error;
    const auto root = std::filesystem::weakly_canonical(impl_->root, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    const auto target = std::filesystem::weakly_canonical(
        AccountRoot(impl_->root, account_identity) /
            "reports" / report_id, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    const auto relative = target.lexically_relative(root);
    if (relative.empty() || relative.native().starts_with(L"..")) {
        error = "report path escapes log root";
        return false;
    }
    if (!std::filesystem::is_directory(target, filesystem_error) || filesystem_error) {
        error = "report does not exist";
        return false;
    }
    std::unordered_set<std::string> allowed{
        "report.json", "actors.jsonl", "abilities.jsonl", "encounters.jsonl", "index.json"};
    if (const auto index = ReadFile(target / "index.json")) {
        std::size_t offset{};
        while ((offset = index->find("\"name\":\"", offset)) != std::string::npos) {
            offset += 8;
            const std::size_t end = index->find('"', offset);
            if (end == std::string::npos) break;
            const std::string name = index->substr(offset, end - offset);
            if (name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
                name.starts_with("events-") && name.ends_with(".jsonl")) {
                allowed.insert(name);
            }
            offset = end + 1;
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(target, filesystem_error)) {
        const bool regular = entry.is_regular_file(filesystem_error);
        if (filesystem_error || !regular ||
            !allowed.contains(entry.path().filename().string())) {
            error = "report contains an unexpected path";
            return false;
        }
    }
    for (const auto& name : allowed) {
        std::filesystem::remove(target / name, filesystem_error);
        if (filesystem_error) {
            error = filesystem_error.message();
            return false;
        }
    }
    std::filesystem::remove(target, filesystem_error);
    if (filesystem_error) {
        error = filesystem_error.message();
        return false;
    }
    std::erase_if(impl_->history, [&](const ReportHistory& item) {
        return item.account == account_identity && item.report_id == report_id;
    });
    return impl_->WriteCatalog(account_identity, error);
}

std::vector<ReportHistory> DamageLogRepository::History() const {
    const std::size_t count = (std::min<std::size_t>)(50, impl_->history.size());
    return {impl_->history.begin(), impl_->history.begin() + count};
}

bool DamageLogRepository::HasOpenReport() const noexcept {
    return impl_->active.has_value();
}

}  // namespace nte::damage_log
