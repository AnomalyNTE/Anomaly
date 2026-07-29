#include "anomaly/plugin_manifest.hpp"

#include "plugin_manifest_schema.hpp"

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace anomaly {
namespace {

using Json = nlohmann::json;
using JsonParseEvent = nlohmann::json::parse_event_t;

bool StartsWithGameId(std::string_view value, std::string_view game_id) noexcept {
    return !game_id.empty() && value.size() > game_id.size() + 1 &&
        value.starts_with(game_id) && value[game_id.size()] == '-';
}

bool GameIdExtends(std::string_view value, std::string_view game_id) noexcept {
    return !game_id.empty() && value.size() > game_id.size() &&
        value.starts_with(game_id) && value[game_id.size()] == '-';
}

void AddDiagnostic(
    PluginManifestParseResult& result, PluginManifestErrorCode code,
    std::string path, std::string message,
    std::size_t source_offset = kUnknownManifestOffset,
    std::size_t value_offset = kUnknownManifestOffset) {
    if (result.diagnostics.size() >= kMaximumPluginManifestDiagnostics) return;
    result.diagnostics.push_back({
        code,
        std::move(path),
        source_offset,
        value_offset,
        std::move(message),
    });
}

bool IsContinuationByte(unsigned char value) noexcept {
    return value >= 0x80 && value <= 0xbf;
}

bool IsValidUtf8(std::string_view text, std::size_t& invalid_offset) noexcept {
    std::size_t index{};
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }

        if (first >= 0xc2 && first <= 0xdf) {
            if (index + 1 >= text.size() ||
                !IsContinuationByte(static_cast<unsigned char>(text[index + 1]))) {
                invalid_offset = index;
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xe0 && first <= 0xef) {
            if (index + 2 >= text.size()) {
                invalid_offset = index;
                return false;
            }
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            const bool valid_second = first == 0xe0
                ? second >= 0xa0 && second <= 0xbf
                : first == 0xed
                    ? second >= 0x80 && second <= 0x9f
                    : IsContinuationByte(second);
            if (!valid_second || !IsContinuationByte(third)) {
                invalid_offset = index;
                return false;
            }
            index += 3;
            continue;
        }

        if (first >= 0xf0 && first <= 0xf4) {
            if (index + 3 >= text.size()) {
                invalid_offset = index;
                return false;
            }
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            const auto fourth = static_cast<unsigned char>(text[index + 3]);
            const bool valid_second = first == 0xf0
                ? second >= 0x90 && second <= 0xbf
                : first == 0xf4
                    ? second >= 0x80 && second <= 0x8f
                    : IsContinuationByte(second);
            if (!valid_second || !IsContinuationByte(third) ||
                !IsContinuationByte(fourth)) {
                invalid_offset = index;
                return false;
            }
            index += 4;
            continue;
        }

        invalid_offset = index;
        return false;
    }
    return true;
}

std::string EscapeJsonPointerToken(std::string_view token) {
    std::string escaped;
    escaped.reserve(token.size());
    for (const char value : token) {
        if (value == '~') {
            escaped += "~0";
        } else if (value == '/') {
            escaped += "~1";
        } else {
            escaped.push_back(value);
        }
    }
    return escaped;
}

void FindEmbeddedNulls(
    const Json& value, const std::string& path, PluginManifestParseResult& result) {
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        const std::size_t offset = text.find('\0');
        if (offset != std::string::npos) {
            AddDiagnostic(result, PluginManifestErrorCode::EmbeddedNull, path,
                          "decoded JSON string contains U+0000",
                          kUnknownManifestOffset, offset);
        }
        return;
    }

    if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            FindEmbeddedNulls(
                value[index], path + "/" + std::to_string(index), result);
        }
        return;
    }

    if (!value.is_object()) return;
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        const std::size_t offset = iterator.key().find('\0');
        if (offset != std::string::npos) {
            AddDiagnostic(result, PluginManifestErrorCode::EmbeddedNull, path,
                          "decoded JSON object key contains U+0000",
                          kUnknownManifestOffset, offset);
        }
        FindEmbeddedNulls(
            iterator.value(), path + "/" + EscapeJsonPointerToken(iterator.key()), result);
    }
}

class ManifestStructureLimit final {
public:
    explicit ManifestStructureLimit(PluginManifestErrorCode code) : code_(code) {}
    [[nodiscard]] PluginManifestErrorCode Code() const noexcept { return code_; }

private:
    PluginManifestErrorCode code_;
};

struct DuplicateKey {
    std::string path;
};

class ManifestJsonObserver final {
public:
    bool operator()(int depth, JsonParseEvent event, Json& parsed) {
        if ((event == JsonParseEvent::object_start ||
             event == JsonParseEvent::array_start) &&
            depth >= static_cast<int>(kMaximumPluginManifestNesting)) {
            throw ManifestStructureLimit(PluginManifestErrorCode::DocumentTooDeep);
        }
        if (event == JsonParseEvent::object_start || event == JsonParseEvent::array_start ||
            event == JsonParseEvent::key || event == JsonParseEvent::value) {
            ++node_count_;
            if (node_count_ > kMaximumPluginManifestNodes) {
                throw ManifestStructureLimit(PluginManifestErrorCode::DocumentTooComplex);
            }
        }

        switch (event) {
        case JsonParseEvent::object_start: {
            Frame frame;
            frame.object = true;
            frame.path = ConsumeChildPath();
            frames_.push_back(std::move(frame));
            break;
        }
        case JsonParseEvent::array_start: {
            Frame frame;
            frame.path = ConsumeChildPath();
            frames_.push_back(std::move(frame));
            break;
        }
        case JsonParseEvent::key:
            if (!frames_.empty() && frames_.back().object) {
                const auto& key = parsed.get_ref<const std::string&>();
                Frame& frame = frames_.back();
                frame.pending_key = key;
                if (!frame.keys.insert(key).second) {
                    duplicate_keys_.push_back({
                        frame.path + "/" + EscapeJsonPointerToken(key),
                    });
                }
            }
            break;
        case JsonParseEvent::value:
            static_cast<void>(ConsumeChildPath());
            break;
        case JsonParseEvent::object_end:
        case JsonParseEvent::array_end:
            if (!frames_.empty()) frames_.pop_back();
            break;
        default:
            break;
        }
        return true;
    }

    [[nodiscard]] const std::vector<DuplicateKey>& DuplicateKeys() const noexcept {
        return duplicate_keys_;
    }

private:
    struct Frame {
        bool object{};
        std::string path;
        std::unordered_set<std::string> keys;
        std::string pending_key;
        std::size_t next_index{};
    };

    std::string ConsumeChildPath() {
        if (frames_.empty()) return {};
        Frame& parent = frames_.back();
        if (!parent.object) {
            return parent.path + "/" + std::to_string(parent.next_index++);
        }
        const std::string path =
            parent.path + "/" + EscapeJsonPointerToken(parent.pending_key);
        parent.pending_key.clear();
        return path;
    }

    std::size_t node_count_{};
    std::vector<Frame> frames_;
    std::vector<DuplicateKey> duplicate_keys_;
};

class SchemaErrorHandler final : public nlohmann::json_schema::error_handler {
public:
    explicit SchemaErrorHandler(PluginManifestParseResult& result) : result_(result) {}

    void error(
        const Json::json_pointer& pointer, const Json&, const std::string& message) override {
        AddDiagnostic(result_, PluginManifestErrorCode::SchemaViolation,
                      pointer.to_string(), message);
    }

private:
    PluginManifestParseResult& result_;
};

const nlohmann::json_schema::json_validator& ManifestValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        Json schema = Json::parse(
            detail::kPluginManifestSchemaJson.begin(),
            detail::kPluginManifestSchemaJson.end());
        return nlohmann::json_schema::json_validator(std::move(schema));
    }();
    return validator;
}

bool HasUnsupportedSchemaVersion(const Json& document) {
    const auto iterator = document.find("schemaVersion");
    if (iterator == document.end()) return false;
    if (iterator->is_number_unsigned()) {
        const std::uint64_t value = iterator->get<std::uint64_t>();
        return value != kPluginManifestSchemaVersion;
    }
    if (iterator->is_number_integer()) {
        const std::int64_t value = iterator->get<std::int64_t>();
        return value >= 0 && value != kPluginManifestSchemaVersion;
    }
    return false;
}

bool RejectOversizedKnownArrays(
    const Json& document, PluginManifestParseResult& result) {
    struct ArrayLimit {
        const char* name;
        std::size_t maximum;
    };
    constexpr ArrayLimit limits[]{
        {"games", 16},
        {"builds", 128},
        {"dependencies", 128},
        {"services", 128},
        {"capabilities", 64},
    };

    bool rejected{};
    for (const ArrayLimit& limit : limits) {
        const auto value = document.find(limit.name);
        if (value != document.end() && value->is_array() && value->size() > limit.maximum) {
            AddDiagnostic(result, PluginManifestErrorCode::SchemaViolation,
                          "/" + std::string(limit.name),
                          "manifest array exceeds its item limit");
            rejected = true;
        }
    }
    return rejected;
}

void SortDiagnostics(PluginManifestParseResult& result) {
    std::stable_sort(
        result.diagnostics.begin(), result.diagnostics.end(),
        [](const PluginManifestDiagnostic& left, const PluginManifestDiagnostic& right) {
            return std::tie(left.path, left.code, left.value_offset) <
                std::tie(right.path, right.code, right.value_offset);
        });
}

void MaterializeManifest(
    const Json& document, PluginManifestParseResult& result) {
    PluginManifest manifest;
    manifest.schema_version = document.at("schemaVersion").get<std::uint32_t>();
    manifest.id = document.at("id").get<std::string>();
    manifest.name = document.at("name").get<std::string>();
    manifest.description = document.value("description", std::string{});
    manifest.author = document.value("author", std::string{});
    manifest.license = document.value("license", std::string{});
    const auto& audience = document.value("audience", std::string{"user"});
    manifest.audience = audience == "developer"
        ? PluginAudience::Developer
        : PluginAudience::User;
    manifest.entry = document.at("entry").get<std::string>();
    manifest.games = document.at("games").get<std::vector<std::string>>();
    manifest.builds = document.at("builds").get<std::vector<std::string>>();
    manifest.capabilities = document.value("capabilities", std::vector<std::string>{});

    for (std::size_t left = 0; left < manifest.games.size(); ++left) {
        for (std::size_t right = left + 1; right < manifest.games.size(); ++right) {
            if (GameIdExtends(manifest.games[left], manifest.games[right]) ||
                GameIdExtends(manifest.games[right], manifest.games[left])) {
                AddDiagnostic(
                    result, PluginManifestErrorCode::OverlappingGameId,
                    "/games/" + std::to_string(right),
                    "game ids overlap under the build prefix contract");
            }
        }
    }

    for (std::size_t index = 0; index < manifest.builds.size(); ++index) {
        const bool owned = std::any_of(
            manifest.games.begin(), manifest.games.end(),
            [&](const std::string& game) {
                return StartsWithGameId(manifest.builds[index], game);
            });
        if (!owned) {
            AddDiagnostic(
                result, PluginManifestErrorCode::UnownedBuildPattern,
                "/builds/" + std::to_string(index),
                "build pattern must use <game-id>-<non-empty-build-pattern>");
        }
    }

    for (std::size_t index = 0; index < manifest.games.size(); ++index) {
        const bool has_pattern = std::any_of(
            manifest.builds.begin(), manifest.builds.end(),
            [&](const std::string& pattern) {
                return StartsWithGameId(pattern, manifest.games[index]);
            });
        if (!has_pattern) {
            AddDiagnostic(
                result, PluginManifestErrorCode::GameWithoutBuildPattern,
                "/games/" + std::to_string(index),
                "game id has no associated build pattern");
        }
    }

    const auto& load_phase = document.at("loadPhase").get_ref<const std::string&>();
    if (load_phase == "game-ready") {
        manifest.load_phase = PluginLoadPhase::GameReady;
    } else {
        throw std::logic_error("schema accepted an unknown plugin load phase");
    }

    const Json& api = document.at("api");
    manifest.api.major = api.at("major").get<std::uint32_t>();
    manifest.api.minimum_minor = api.at("minMinor").get<std::uint32_t>();
    manifest.api.maximum_minor = api.at("maxMinor").get<std::uint32_t>();
    if (manifest.api.minimum_minor > manifest.api.maximum_minor) {
        AddDiagnostic(result, PluginManifestErrorCode::InvalidApiRange, "/api/maxMinor",
                      "api maxMinor is lower than minMinor");
    }

    const auto& version_text = document.at("version").get_ref<const std::string&>();
    SemVerParseError version_error;
    auto version = ParseSemanticVersion(version_text, &version_error);
    if (!version) {
        AddDiagnostic(result, PluginManifestErrorCode::InvalidSemanticVersion, "/version",
                      version_error.message, kUnknownManifestOffset, version_error.offset);
    } else {
        manifest.version = std::move(*version);
    }

    std::unordered_set<std::string> dependency_ids;
    const auto dependencies = document.find("dependencies");
    if (dependencies != document.end()) {
        manifest.dependencies.reserve(dependencies->size());
        for (std::size_t index = 0; index < dependencies->size(); ++index) {
            const Json& value = (*dependencies)[index];
            PluginDependencyManifest dependency;
            dependency.id = value.at("id").get<std::string>();
            dependency.version_expression = value.at("version").get<std::string>();
            dependency.optional = value.value("optional", false);
            const std::string base = "/dependencies/" + std::to_string(index);

            if (dependency.id == manifest.id) {
                AddDiagnostic(result, PluginManifestErrorCode::SelfDependency, base + "/id",
                              "plugin depends on itself");
            }
            if (!dependency_ids.insert(dependency.id).second) {
                AddDiagnostic(result, PluginManifestErrorCode::DuplicateDependency,
                              base + "/id", "plugin dependency id is duplicated");
            }

            SemVerParseError range_error;
            auto range = ParseSemanticVersionRange(
                dependency.version_expression, &range_error);
            if (!range) {
                AddDiagnostic(result, PluginManifestErrorCode::InvalidVersionRange,
                              base + "/version", range_error.message,
                              kUnknownManifestOffset, range_error.offset);
            } else {
                dependency.version_range = std::move(*range);
            }
            manifest.dependencies.push_back(std::move(dependency));
        }
    }

    std::unordered_set<std::string> service_ids;
    const auto services = document.find("services");
    if (services != document.end()) {
        manifest.services.reserve(services->size());
        for (std::size_t index = 0; index < services->size(); ++index) {
            const Json& value = (*services)[index];
            PluginServiceRequirement service;
            service.id = value.at("id").get<std::string>();
            service.minimum_version = value.at("minVersion").get<std::uint32_t>();
            service.optional = value.value("optional", false);
            if (!service_ids.insert(service.id).second) {
                AddDiagnostic(result, PluginManifestErrorCode::DuplicateService,
                              "/services/" + std::to_string(index) + "/id",
                              "service requirement id is duplicated");
            }
            manifest.services.push_back(std::move(service));
        }
    }

    SortDiagnostics(result);
    if (result.diagnostics.empty()) result.manifest = std::move(manifest);
}

}  // namespace

PluginManifestParseResult ParsePluginManifest(std::string_view utf8_json) {
    PluginManifestParseResult result;
    if (utf8_json.size() > kMaximumPluginManifestBytes) {
        AddDiagnostic(result, PluginManifestErrorCode::DocumentTooLarge, "",
                      "plugin manifest exceeds the byte limit");
        return result;
    }

    const std::size_t null_offset = utf8_json.find('\0');
    if (null_offset != std::string_view::npos) {
        AddDiagnostic(result, PluginManifestErrorCode::EmbeddedNull, "",
                      "plugin manifest contains a raw NUL byte", null_offset);
        return result;
    }

    if (utf8_json.size() >= 3 &&
        static_cast<unsigned char>(utf8_json[0]) == 0xef &&
        static_cast<unsigned char>(utf8_json[1]) == 0xbb &&
        static_cast<unsigned char>(utf8_json[2]) == 0xbf) {
        AddDiagnostic(result, PluginManifestErrorCode::InvalidUtf8, "",
                      "UTF-8 byte order marks are not accepted", 0);
        return result;
    }
    std::size_t invalid_utf8{};
    if (!IsValidUtf8(utf8_json, invalid_utf8)) {
        AddDiagnostic(result, PluginManifestErrorCode::InvalidUtf8, "",
                      "plugin manifest is not strict UTF-8", invalid_utf8);
        return result;
    }

    ManifestJsonObserver observer;
    Json document;
    try {
        Json::parser_callback_t callback = std::ref(observer);
        document = Json::parse(
            utf8_json.begin(), utf8_json.end(), callback, true, false);
    } catch (const ManifestStructureLimit& limit) {
        AddDiagnostic(
            result, limit.Code(), "",
            limit.Code() == PluginManifestErrorCode::DocumentTooDeep
                ? "plugin manifest exceeds the nesting limit"
                : "plugin manifest exceeds the structural complexity limit");
        return result;
    } catch (const Json::parse_error& error) {
        const std::size_t offset = error.byte == 0
            ? kUnknownManifestOffset
            : static_cast<std::size_t>(error.byte - 1);
        AddDiagnostic(result, PluginManifestErrorCode::JsonSyntax, "",
                      error.what(), offset);
        return result;
    } catch (const Json::exception& error) {
        AddDiagnostic(result, PluginManifestErrorCode::JsonSyntax, "", error.what());
        return result;
    }

    for (const DuplicateKey& key : observer.DuplicateKeys()) {
        AddDiagnostic(result, PluginManifestErrorCode::DuplicateJsonKey, key.path,
                      "duplicate JSON object key");
    }
    if (!result.diagnostics.empty()) return result;

    FindEmbeddedNulls(document, "", result);
    if (!result.diagnostics.empty()) return result;
    if (!document.is_object()) {
        AddDiagnostic(result, PluginManifestErrorCode::RootNotObject, "",
                      "plugin manifest root must be an object");
        return result;
    }
    if (HasUnsupportedSchemaVersion(document)) {
        AddDiagnostic(result, PluginManifestErrorCode::UnsupportedSchemaVersion,
                      "/schemaVersion", "plugin manifest schema version is unsupported");
        return result;
    }
    if (RejectOversizedKnownArrays(document, result)) return result;

    try {
        SchemaErrorHandler handler(result);
        static_cast<void>(ManifestValidator().validate(document, handler));
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        AddDiagnostic(result, PluginManifestErrorCode::InternalFailure, "",
                      std::string("manifest schema validation failed internally: ") +
                          error.what());
        return result;
    }
    if (!result.diagnostics.empty()) {
        SortDiagnostics(result);
        return result;
    }

    try {
        MaterializeManifest(document, result);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        result.manifest.reset();
        AddDiagnostic(result, PluginManifestErrorCode::InternalFailure, "",
                      std::string("manifest materialization failed internally: ") +
                          error.what());
        SortDiagnostics(result);
    }
    return result;
}

std::string_view PluginManifestSchemaJson() noexcept {
    return detail::kPluginManifestSchemaJson;
}

}  // namespace anomaly
