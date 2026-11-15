#include "settings_model.hpp"

#include <cstdio>

#include <cstdint>
#include <cstring>

namespace daily {
namespace {

// 设置文档的 JSON Schema：与 SerializeSettings 的键一一对应。
// dungeon_table 是写配置时自动生成的「序号对照表」，只读参考，读取时忽略。
constexpr const char* kSchemaJson = R"JSON({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "type": "object",
  "additionalProperties": false,
  "required": ["enabled", "configured", "auto_run_pending", "stages", "dungeon", "hotkey", "timing"],
  "properties": {
    "enabled": { "type": "boolean" },
    "configured": { "type": "boolean" },
    "auto_run_pending": { "type": "boolean" },
    "auto_run_pid": { "type": "integer", "minimum": 0 },
    "auto_run_stamp": { "type": "integer", "minimum": 0 },
    "stages": {
      "type": "object",
      "additionalProperties": false,
      "required": ["dungeon", "cafe", "daily_rewards", "periodic_rewards"],
      "properties": {
        "dungeon": { "type": "boolean" },
        "cafe": { "type": "boolean" },
        "daily_rewards": { "type": "boolean" },
        "periodic_rewards": { "type": "boolean" }
      }
    },
    "dungeon": {
      "type": "object",
      "additionalProperties": false,
      "required": ["index", "sub_index", "level_index", "runs", "enter_wait", "exit_wait"],
      "properties": {
        "index": { "type": "integer", "minimum": 0, "maximum": 15 },
        "sub_index": { "type": "integer", "minimum": 0, "maximum": 15 },
        "level_index": { "type": "integer", "minimum": 1, "maximum": 8 },
        "runs": { "type": "integer", "minimum": 1, "maximum": 50 },
        "enter_wait": { "type": "integer", "minimum": 1, "maximum": 60 },
        "exit_wait": { "type": "integer", "minimum": 1, "maximum": 60 }
      }
    },
    "hotkey": {
      "type": "object",
      "additionalProperties": false,
      "required": ["stop_modifiers", "stop_vk"],
      "properties": {
        "stop_modifiers": { "type": "integer", "minimum": 0, "maximum": 15 },
        "stop_vk": { "type": "integer", "minimum": 1, "maximum": 254 }
      }
    },
    "timing": {
      "type": "object",
      "additionalProperties": false,
      "required": ["world_settle_seconds", "stage_timeout_seconds"],
      "properties": {
        "world_settle_seconds": { "type": "integer", "minimum": 0, "maximum": 120 },
        "stage_timeout_seconds": { "type": "integer", "minimum": 30, "maximum": 7200 }
      }
    },
    "dungeon_table": {
      "type": "array",
      "maxItems": 64,
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["index", "entry", "name", "subs", "level_min", "level_max"],
        "properties": {
          "index": { "type": "integer", "minimum": 0, "maximum": 63 },
          "entry": { "type": "integer", "minimum": 0, "maximum": 63 },
          "name": { "type": "string", "maxLength": 96 },
          "subs": { "type": "integer", "minimum": 1, "maximum": 16 },
          "level_min": { "type": "integer", "minimum": 1, "maximum": 8 },
          "level_max": { "type": "integer", "minimum": 1, "maximum": 8 },
          "names": {
            "type": "array",
            "maxItems": 8,
            "items": { "type": "string", "maxLength": 96 }
          }
        }
      }
    }
  }
})JSON";

bool ReadBool(const AnomalyJsonServiceV1* json, AnomalyGenerationHandleV1 object,
              const char* key, bool& out) noexcept {
    if (json == nullptr || json->object_find == nullptr) return false;
    AnomalyGenerationHandleV1 child{};
    if (json->object_find(json->user, object, Str(key, std::strlen(key)), &child).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    int32_t value = 0;
    const bool ok =
        json->boolean_value(json->user, child, &value).code == ANOMALY_STATUS_V1_OK;
    static_cast<void>(json->release(json->user, child));
    if (ok) out = value != 0;
    return ok;
}

bool ReadNumber(const AnomalyJsonServiceV1* json, AnomalyGenerationHandleV1 object,
                const char* key, std::uint32_t& out) noexcept {
    if (json == nullptr || json->object_find == nullptr) return false;
    AnomalyGenerationHandleV1 child{};
    if (json->object_find(json->user, object, Str(key, std::strlen(key)), &child).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    double value = 0.0;
    const bool ok = json->number_value(json->user, child, &value).code == ANOMALY_STATUS_V1_OK;
    static_cast<void>(json->release(json->user, child));
    if (ok && value >= 0.0) out = static_cast<std::uint32_t>(value + 0.5);
    return ok;
}

AnomalyGenerationHandleV1 FindObject(const AnomalyJsonServiceV1* json,
                                     AnomalyGenerationHandleV1 parent,
                                     const char* key) noexcept {
    AnomalyGenerationHandleV1 child{};
    if (json == nullptr || json->object_find == nullptr) return child;
    if (json->object_find(json->user, parent, Str(key, std::strlen(key)), &child).code !=
        ANOMALY_STATUS_V1_OK) {
        return {};
    }
    return child;
}

void AppendUnsigned(std::string& out, const std::uint32_t value) {
    out += std::to_string(value);
}

void AppendBool(std::string& out, const bool value) { out += value ? "true" : "false"; }

}  // namespace

void NormalizeSettings(Settings& settings) noexcept {
    if (settings.dungeon.index >= kDungeonTableCount) settings.dungeon.index = 0;
    const DungeonRow& row = kDungeonTable[settings.dungeon.index];
    if (settings.dungeon.sub_index >= row.sub_count) settings.dungeon.sub_index = 0;
    if (settings.dungeon.level_index < row.min_level) settings.dungeon.level_index = row.min_level;
    if (settings.dungeon.level_index > row.max_level) settings.dungeon.level_index = row.max_level;
    if (settings.dungeon.runs < 1) settings.dungeon.runs = 1;
    if (settings.dungeon.runs > 50) settings.dungeon.runs = 50;
    if (settings.dungeon.enter_wait < 1) settings.dungeon.enter_wait = 1;
    if (settings.dungeon.enter_wait > 60) settings.dungeon.enter_wait = 60;
    if (settings.dungeon.exit_wait < 1) settings.dungeon.exit_wait = 1;
    if (settings.dungeon.exit_wait > 60) settings.dungeon.exit_wait = 60;
    settings.stop_modifiers &= 0x0F;
    if (settings.stop_vk < 1) settings.stop_vk = 1;
    if (settings.stop_vk > 254) settings.stop_vk = 254;
    if (settings.world_settle_seconds > 120) settings.world_settle_seconds = 120;
    if (settings.stage_timeout_seconds < 30) settings.stage_timeout_seconds = 30;
    if (settings.stage_timeout_seconds > 7200) settings.stage_timeout_seconds = 7200;
}

const DungeonRow& DungeonRowAt(const std::uint32_t index) noexcept {
    return kDungeonTable[index < kDungeonTableCount ? index : 0];
}

std::string SerializeSettings(const Settings& settings) {
    const Settings value = [&settings] {
        Settings copy = settings;
        NormalizeSettings(copy);
        return copy;
    }();
    std::string out;
    out.reserve(1024);
    out += "{\n  \"enabled\": ";
    AppendBool(out, value.enabled);
    out += ",\n  \"configured\": ";
    AppendBool(out, value.configured);
    out += ",\n  \"auto_run_pending\": ";
    AppendBool(out, value.auto_run_pending);
    out += ",\n  \"auto_run_pid\": ";
    AppendUnsigned(out, value.auto_run_pid);
    out += ",\n  \"auto_run_stamp\": ";
    AppendUnsigned(out, value.auto_run_stamp);
    out += ",\n  \"stages\": {\"dungeon\": ";
    AppendBool(out, value.stage_dungeon);
    out += ", \"cafe\": ";
    AppendBool(out, value.stage_cafe);
    out += ", \"daily_rewards\": ";
    AppendBool(out, value.stage_daily_rewards);
    out += ", \"periodic_rewards\": ";
    AppendBool(out, value.stage_periodic_rewards);
    out += "},\n  \"dungeon\": {\"index\": ";
    AppendUnsigned(out, value.dungeon.index);
    out += ", \"sub_index\": ";
    AppendUnsigned(out, value.dungeon.sub_index);
    out += ", \"level_index\": ";
    AppendUnsigned(out, value.dungeon.level_index);
    out += ", \"runs\": ";
    AppendUnsigned(out, value.dungeon.runs);
    out += ", \"enter_wait\": ";
    AppendUnsigned(out, value.dungeon.enter_wait);
    out += ", \"exit_wait\": ";
    AppendUnsigned(out, value.dungeon.exit_wait);
    out += "},\n  \"hotkey\": {\"stop_modifiers\": ";
    AppendUnsigned(out, value.stop_modifiers);
    out += ", \"stop_vk\": ";
    AppendUnsigned(out, value.stop_vk);
    out += "},\n  \"timing\": {\"world_settle_seconds\": ";
    AppendUnsigned(out, value.world_settle_seconds);
    out += ", \"stage_timeout_seconds\": ";
    AppendUnsigned(out, value.stage_timeout_seconds);
    out += "},\n  \"dungeon_table\": [";
    for (std::size_t i = 0; i < kDungeonTableCount; ++i) {
        const DungeonRow& row = kDungeonTable[i];
        if (i != 0) out += ",";
        out += "\n    {\"index\": ";
        AppendUnsigned(out, static_cast<std::uint32_t>(i));
        out += ", \"entry\": ";
        AppendUnsigned(out, row.entry_index);
        out += ", \"name\": \"";
        out += row.name;
        out += "\", \"subs\": ";
        AppendUnsigned(out, row.sub_count);
        out += ", \"level_min\": ";
        AppendUnsigned(out, row.min_level);
        out += ", \"level_max\": ";
        AppendUnsigned(out, row.max_level);
        if (row.sub_count > 1) {
            out += ", \"names\": [";
            for (std::uint32_t s = 0; s < row.sub_count; ++s) {
                if (s != 0) out += ", ";
                out += "\"";
                out += row.sub_names[s] != nullptr ? row.sub_names[s] : "";
                out += "\"";
            }
            out += "]";
        }
        out += "}";
    }
    out += "\n  ]\n}\n";
    return out;
}

bool ParseSettings(const std::string& document, const AnomalyJsonServiceV1* json,
                   Settings& out) noexcept {
    if (json == nullptr || json->parse == nullptr || document.empty()) return false;
    AnomalyGenerationHandleV1 root{};
    if (json->parse(json->user, Str(document.data(), document.size()), &root).code !=
        ANOMALY_STATUS_V1_OK) {
        return false;
    }
    Settings parsed{};
    ReadBool(json, root, "enabled", parsed.enabled);
    ReadBool(json, root, "configured", parsed.configured);
    ReadBool(json, root, "auto_run_pending", parsed.auto_run_pending);
    ReadNumber(json, root, "auto_run_pid", parsed.auto_run_pid);
    ReadNumber(json, root, "auto_run_stamp", parsed.auto_run_stamp);
    const auto stages = FindObject(json, root, "stages");
    if (stages.id != 0) {
        ReadBool(json, stages, "dungeon", parsed.stage_dungeon);
        ReadBool(json, stages, "cafe", parsed.stage_cafe);
        ReadBool(json, stages, "daily_rewards", parsed.stage_daily_rewards);
        ReadBool(json, stages, "periodic_rewards", parsed.stage_periodic_rewards);
        static_cast<void>(json->release(json->user, stages));
    }
    const auto dungeon = FindObject(json, root, "dungeon");
    if (dungeon.id != 0) {
        ReadNumber(json, dungeon, "index", parsed.dungeon.index);
        ReadNumber(json, dungeon, "sub_index", parsed.dungeon.sub_index);
        ReadNumber(json, dungeon, "level_index", parsed.dungeon.level_index);
        ReadNumber(json, dungeon, "runs", parsed.dungeon.runs);
        ReadNumber(json, dungeon, "enter_wait", parsed.dungeon.enter_wait);
        ReadNumber(json, dungeon, "exit_wait", parsed.dungeon.exit_wait);
        static_cast<void>(json->release(json->user, dungeon));
    }
    const auto hotkey = FindObject(json, root, "hotkey");
    if (hotkey.id != 0) {
        ReadNumber(json, hotkey, "stop_modifiers", parsed.stop_modifiers);
        ReadNumber(json, hotkey, "stop_vk", parsed.stop_vk);
        static_cast<void>(json->release(json->user, hotkey));
    }
    const auto timing = FindObject(json, root, "timing");
    if (timing.id != 0) {
        ReadNumber(json, timing, "world_settle_seconds", parsed.world_settle_seconds);
        ReadNumber(json, timing, "stage_timeout_seconds", parsed.stage_timeout_seconds);
        static_cast<void>(json->release(json->user, timing));
    }
    static_cast<void>(json->release(json->user, root));
    NormalizeSettings(parsed);
    out = parsed;
    return true;
}

void ConfigStore::Bind(const AnomalyConfigServiceV1* config,
                       const AnomalyJsonServiceV1* json) noexcept {
    config_ = config;
    json_ = json;
}

bool ConfigStore::RegisterSchema() noexcept {
    registered_ = false;
    if (config_ == nullptr || config_->register_schema == nullptr) {
        std::snprintf(error_, sizeof(error_), "%s", "anomaly.config unavailable");
        return false;
    }
    const AnomalyStatusV1 status = config_->register_schema(
        config_->user, Str(kConfigSchemaId, std::strlen(kConfigSchemaId)),
        kConfigSchemaVersion,
        AnomalyByteSpanV1{reinterpret_cast<const std::uint8_t*>(kSchemaJson),
                          std::strlen(kSchemaJson)},
        &schema_);
    if (status.code != ANOMALY_STATUS_V1_OK) {
        std::snprintf(error_, sizeof(error_), "%s", "register_schema failed");
        return false;
    }
    registered_ = true;
    return true;
}

bool ConfigStore::Read(Settings& out) noexcept {
    out = Settings{};
    NormalizeSettings(out);
    if (!registered_ || config_ == nullptr || config_->read == nullptr) return false;
    std::uint32_t stored_version = 0;
    std::size_t required = 0;
    AnomalyStatusV1 status = config_->read(
        config_->user, Str(kConfigSchemaId, std::strlen(kConfigSchemaId)), &stored_version,
        AnomalyMutableByteSpanV1{nullptr, 0}, &required);
    if (status.code != ANOMALY_STATUS_V1_OK || required == 0 || required > 64u * 1024u) {
        return false;
    }
    std::string document(required, '\0');
    std::size_t size = required;
    status = config_->read(
        config_->user, Str(kConfigSchemaId, std::strlen(kConfigSchemaId)), &stored_version,
        AnomalyMutableByteSpanV1{reinterpret_cast<std::uint8_t*>(document.data()), document.size()},
        &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > document.size()) return false;
    document.resize(size);
    return ParseSettings(document, json_, out);
}

bool ConfigStore::Write(const Settings& settings) noexcept {
    if (!registered_ || config_ == nullptr || config_->write_atomic == nullptr) return false;
    const std::string document = SerializeSettings(settings);
    const AnomalyStatusV1 status = config_->write_atomic(
        config_->user, Str(kConfigSchemaId, std::strlen(kConfigSchemaId)),
        kConfigSchemaVersion,
        AnomalyByteSpanV1{reinterpret_cast<const std::uint8_t*>(document.data()), document.size()});
    if (status.code != ANOMALY_STATUS_V1_OK) {
        std::snprintf(error_, sizeof(error_), "%s", "write_atomic rejected the document");
        return false;
    }
    return true;
}

}  // namespace daily
