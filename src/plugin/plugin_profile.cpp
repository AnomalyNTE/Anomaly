#include "anomaly/plugin_profile.hpp"
#include "anomaly/reliable_storage.hpp"
#include "plugin_profile_schema.hpp"

#include <Windows.h>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace anomaly {

PluginProfileResult ParsePluginProfile(const std::string_view text) {
    if (text.empty() || text.size() > kMaximumPluginProfileBytes)
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, "profile exceeds document size limits", {}};
    try {
        // Duplicate keys must not silently replace an address or layout declaration.
        std::vector<std::unordered_set<std::string>> objects;
        const auto callback = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
            if (depth > 16) throw std::invalid_argument("profile nesting exceeds schema limits");
            using Event = nlohmann::json::parse_event_t;
            if (event == Event::object_start) objects.emplace_back();
            else if (event == Event::object_end) objects.pop_back();
            else if (event == Event::key && !objects.back().insert(value.get<std::string>()).second)
                throw std::invalid_argument("duplicate profile key");
            return true;
        };
        auto document = std::make_shared<nlohmann::json>(
            nlohmann::json::parse(text.begin(), text.end(), callback));
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(nlohmann::json::parse(detail::kPluginProfileSchemaJson));
        validator.validate(*document);
        std::unordered_set<std::string> names;
        for (const auto& symbol : document->at("symbols")) {
            if (!names.insert(symbol.at("name").get<std::string>()).second)
                throw std::invalid_argument("duplicate profile symbol name");
            for (const auto& candidate : symbol.at("candidates")) {
                const auto& pattern = candidate.at("pattern").get_ref<const std::string&>();
                if (pattern.find_first_not_of("? ") == std::string::npos)
                    throw std::invalid_argument("profile pattern needs a concrete byte");
                const auto count = static_cast<std::size_t>(std::count(pattern.begin(), pattern.end(), ' ')) + 1;
                if (candidate.contains("callOffset") &&
                    candidate.at("callOffset").get<std::size_t>() + 5 > count)
                    throw std::invalid_argument("profile callOffset is outside its pattern");
            }
        }
        return {ANOMALY_STATUS_V1_OK, {}, std::move(document)};
    } catch (const std::exception& error) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, error.what(), {}};
    }
}

PluginProfileResult LoadPluginProfile(
    const std::filesystem::path& package_directory, const std::string_view relative_path) {
    if (relative_path.empty() || relative_path.size() > 512 ||
        !relative_path.ends_with(".json") || relative_path.find('\0') != std::string_view::npos)
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, "profile requires a relative JSON filename", {}};
    std::wstring wide(relative_path.size(), L'\0');
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        relative_path.data(), static_cast<int>(relative_path.size()), wide.data(), static_cast<int>(wide.size()));
    if (count == 0)
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, "profile filename is not UTF-8", {}};
    wide.resize(static_cast<std::size_t>(count));
    if (std::filesystem::path(wide).has_root_path())
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, "profile filename must be relative", {}};
    // Use the package as the trusted root so profile/ itself cannot be a reparse point.
    // ReliableStorage rejects rooted paths, parent traversal and reparse components.
    ReliableStorage storage(package_directory.wstring());
    const auto read = storage.Read(L"profile/" + wide, kMaximumPluginProfileBytes);
    if (!read) {
        const auto code = read.result.win32_error == ERROR_FILE_NOT_FOUND ||
            read.result.win32_error == ERROR_PATH_NOT_FOUND ? ANOMALY_STATUS_V1_NOT_FOUND :
            read.result.error == StorageError::IoFailure ? ANOMALY_STATUS_V1_FAILED :
            ANOMALY_STATUS_V1_INVALID_ARGUMENT;
        return {static_cast<std::uint32_t>(code), "profile file could not be read within the package", {}};
    }
    return ParsePluginProfile(std::string_view(
        reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size()));
}

}  // namespace anomaly
