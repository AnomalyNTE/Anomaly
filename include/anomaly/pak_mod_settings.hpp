#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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

// Explicit opt-in to overlapping mods. The engine's effective read priority selects each shared
// resource; enabling this option does not change that priority or merge the conflicting contents.
inline bool PakModForceLoad(const nlohmann::json& document) {
    return document.value("forceLoad", false);
}

// The name a mod keeps when its folder moves: the folder a directory mod lives in, or the file
// name of a loose container. A key is a path, so everything above the mod is what a move rewrites.
inline std::string PakModSelectionName(std::string_view key) {
    auto text = NormalizePakModKey(std::string(key));
    if (text.ends_with('/')) text.pop_back();
    const auto slash = text.rfind('/');
    return slash == std::string::npos ? text : text.substr(slash + 1);
}

// Whether a saved selection reaches this path at all. A mod whose folder moved has no entry of its
// own, which is what tells the caller to look the mod up by name instead of reading "enabled".
inline bool PakModSelectionApplied(const nlohmann::json& states, const std::string& relative_path) {
    const auto path = NormalizePakModKey(relative_path);
    for (const auto& item : states.items()) {
        const auto key = NormalizePakModKey(item.key());
        if (key.empty()) continue;
        if (key == path || (key.back() == '/' && path.starts_with(key))) return true;
    }
    return false;
}

// The choice a previous layout saved for this mod name. A folder move rewrites the path a selection
// is stored under, and reading "no entry" as enabled would switch the mod back on behind the
// player's back -- including the variants they switched off to break a resource conflict. A name two
// mods share is not resolved: then the entry names neither of them.
inline std::optional<bool> PakModInheritedSelection(const nlohmann::json& states,
    const std::string_view name, const bool unique) {
    if (!unique || name.empty()) return std::nullopt;
    std::optional<bool> result;
    for (const auto& item : states.items()) {
        if (!item.value().is_boolean() || PakModSelectionName(item.key()) != name) continue;
        // JSON key order is not edit history. Preserve an explicit OFF when stale locations
        // disagree; a new explicit selection replaces these stale keys at the write boundary.
        result = result.value_or(true) && item.value().get<bool>();
    }
    return result;
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
