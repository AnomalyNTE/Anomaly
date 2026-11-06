#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace anomaly {

// Chosen before engine startup and fixed for the session: switching signature strategies after
// containers have started asynchronous reads cannot undo the old strategy's process-owned hooks.
inline bool PakModCompatibilityMode(const nlohmann::json& document) {
    const auto mode = document.value("mode", std::string{"hot"});
    if (mode != "hot" && mode != "compatibility")
        throw std::runtime_error("unknown pak loading mode");
    return mode == "compatibility";
}

inline nlohmann::json ReadPakModSettings(const std::filesystem::path& runtime_root) {
    const auto path = runtime_root / L"config" / L"pak-mods.json";
    std::ifstream stream(path);
    if (!stream) {
        if (std::filesystem::exists(path)) throw std::runtime_error("cannot read pak-mods.json");
        return nlohmann::json::object();
    }
    auto document = nlohmann::json::parse(stream);
    if (!document.is_object() || document.value("schemaVersion", 1) != 1)
        throw std::runtime_error("invalid pak-mods configuration");
    return document;
}

// One game installation owns one selection across proxy, launch and attach runtimes.
// Existing runtime-local configuration is a read-only migration fallback until first save.
inline nlohmann::json ReadGamePakModSettings(const std::filesystem::path& runtime_root,
    const std::filesystem::path& game_directory) {
    const auto root = game_directory / L"Anomaly";
    return ReadPakModSettings(std::filesystem::exists(root / L"config" / L"pak-mods.json") ? root : runtime_root);
}

// Keys are paths relative to ~mods. A trailing slash denotes a complete mod directory; a loose
// container uses its .pak filename. These choices also apply to an explicit manifest.
inline std::string NormalizePakModKey(std::string path) {
    for (auto& c : path) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return path;
}

inline void ValidatePakModStates(const nlohmann::json& states) {
    if (!states.is_object()) throw std::runtime_error("modStates must be an object");
    for (const auto& [key, value] : states.items()) {
        if (!value.is_boolean() || key.empty()) throw std::runtime_error("invalid modStates entry");
    }
}

// The configuration boundary validates once before matching any discovered files.
inline bool PakModEnabled(const nlohmann::json& states, const std::string& relative_path) {
    const auto path = NormalizePakModKey(relative_path);
    bool enabled = true;
    std::size_t specificity{};
    for (const auto& [raw_key, value] : states.items()) {
        const auto key = NormalizePakModKey(raw_key);
        if ((key == path || (key.back() == '/' && path.starts_with(key))) &&
            key.size() >= specificity) {
            enabled = value.get<bool>();
            specificity = key.size();
        }
    }
    return enabled;
}

inline void SetPakModEnabled(nlohmann::json& states, const std::string& raw_key, const bool enabled) {
    if (states.is_null()) states = nlohmann::json::object();
    if (!states.is_object()) throw std::runtime_error("modStates must be an object");
    const auto key = NormalizePakModKey(raw_key);
    if (key.empty()) throw std::runtime_error("empty mod selection key");
    // A whole-mod toggle replaces previous per-file overrides for that mod. Otherwise a nested
    // true override could keep a container mounted after its folder's toggle says OFF.
    for (auto it = states.begin(); it != states.end();) {
        const auto existing = NormalizePakModKey(it.key());
        if (existing == key || (key.back() == '/' && existing.starts_with(key))) it = states.erase(it);
        else ++it;
    }
    states[raw_key] = enabled;
}

}  // namespace anomaly
