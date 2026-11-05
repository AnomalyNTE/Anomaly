#pragma once

#include "overlay_model.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace nte::settings {

enum class DataTab : std::uint8_t {
    Outgoing,
    Incoming,
    HealingDone,
    HealingReceived,
};

struct OverlaySettings {
    int x{100};
    int y{100};
    int width{560};
    int height{400};
    bool locked{};
    bool collapsed{};
    bool topmost{true};
    bool hide_idle{};
    float opacity{0.90F};
    float ui_scale{1.0F};
    nte::combat_stats::overlay::SortColumn sort_column{
        nte::combat_stats::overlay::SortColumn::Damage};
    bool descending{true};
    DataTab active_tab{DataTab::Outgoing};
};

struct PluginSettings {
    std::filesystem::path log_root;
    std::optional<std::filesystem::path> export_root;
    bool developer_mode{};
};

class PendingPluginSettings {
public:
    PendingPluginSettings() = default;
    ~PendingPluginSettings();
    PendingPluginSettings(PendingPluginSettings&& other) noexcept;
    PendingPluginSettings& operator=(PendingPluginSettings&& other) noexcept;
    PendingPluginSettings(const PendingPluginSettings&) = delete;
    PendingPluginSettings& operator=(const PendingPluginSettings&) = delete;

    [[nodiscard]] bool Commit(std::string& error);

private:
    friend std::optional<PendingPluginSettings> StagePluginSettings(
        const std::filesystem::path&, const PluginSettings&, std::string&);
    PendingPluginSettings(
        std::filesystem::path destination,
        std::filesystem::path staged) noexcept;

    std::filesystem::path destination_;
    std::filesystem::path staged_;
};

[[nodiscard]] std::filesystem::path SettingsDirectory();
[[nodiscard]] std::filesystem::path OverlaySettingsPath();
[[nodiscard]] std::filesystem::path PluginSettingsPath();
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::filesystem::path> PathFromUtf8(
    const std::string& value);

[[nodiscard]] bool LoadOverlaySettings(
    const std::filesystem::path& path,
    OverlaySettings& settings,
    std::string& error);
[[nodiscard]] bool SaveOverlaySettings(
    const std::filesystem::path& path,
    const OverlaySettings& settings,
    std::string& error);
[[nodiscard]] bool UpdateOverlayPreferences(
    const std::filesystem::path& path,
    const OverlaySettings& preferences,
    OverlaySettings& merged,
    std::string& error);

[[nodiscard]] PluginSettings LoadPluginSettings(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_log_root,
    std::string& error);
[[nodiscard]] bool SavePluginSettings(
    const std::filesystem::path& path,
    const PluginSettings& settings,
    std::string& error);
[[nodiscard]] bool UpdatePluginExportRoot(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_log_root,
    const std::optional<std::filesystem::path>& export_root,
    PluginSettings& merged,
    std::string& error);
[[nodiscard]] bool UpdatePluginLogRoot(
    const std::filesystem::path& path,
    const std::filesystem::path& log_root,
    PluginSettings& merged,
    std::string& error);
[[nodiscard]] bool UpdatePluginDeveloperMode(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_log_root,
    bool developer_mode,
    PluginSettings& merged,
    std::string& error);
[[nodiscard]] std::optional<PendingPluginSettings> StagePluginSettings(
    const std::filesystem::path& path,
    const PluginSettings& settings,
    std::string& error);

[[nodiscard]] bool PrepareLogRoot(
    const std::filesystem::path& path,
    std::string& error);

}  // namespace nte::settings
