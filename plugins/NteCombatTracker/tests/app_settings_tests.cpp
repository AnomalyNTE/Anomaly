#include "app_settings.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

int failures{};

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path TemporaryRoot(const std::string& name) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        (name + "-" + std::to_string(nonce));
}

void OverlaySettingsRemainCompatibleAndPreserveAllFields() {
    const auto root = TemporaryRoot("nte-overlay-settings-test");
    const auto path = root / "overlay-settings.json";
    std::filesystem::create_directories(root);
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"({"schema_version":2,"x":321,"y":654,"width":777,"height":444,)"
                  R"("locked":true,"collapsed":false,"topmost":false,)"
                  R"("hide_idle":true,"opacity":0.75,"ui_scale":1.25,)"
                  R"("sort_column":5,"descending":false,"active_tab":1})";
    }

    nte::settings::OverlaySettings settings;
    std::string error;
    Check(nte::settings::LoadOverlaySettings(path, settings, error),
        "existing schema v2 overlay settings should load");
    Check(error.empty() && settings.x == 321 && settings.y == 654 &&
            settings.width == 777 && settings.height == 444 && settings.locked &&
            !settings.topmost && settings.hide_idle && settings.opacity == 0.75F &&
            settings.ui_scale == 1.25F &&
            settings.sort_column == nte::combat_stats::overlay::SortColumn::MaximumHit &&
            !settings.descending && settings.active_tab == nte::settings::DataTab::Incoming,
        "all existing overlay settings should be retained");

    settings.opacity = 0.6F;
    settings.active_tab = nte::settings::DataTab::HealingReceived;
    Check(nte::settings::SaveOverlaySettings(path, settings, error),
        "overlay settings should save atomically");
    nte::settings::OverlaySettings round_trip;
    Check(nte::settings::LoadOverlaySettings(path, round_trip, error) &&
            round_trip.x == 321 && round_trip.width == 777 &&
            round_trip.sort_column == nte::combat_stats::overlay::SortColumn::MaximumHit &&
            round_trip.active_tab == nte::settings::DataTab::HealingReceived &&
            round_trip.opacity == 0.6F,
        "editing an exposed setting should preserve hidden fields");

    auto stale_menu_copy = round_trip;
    stale_menu_copy.opacity = 1.0F;
    auto newer_overlay_copy = round_trip;
    newer_overlay_copy.x = 999;
    newer_overlay_copy.sort_column =
        nte::combat_stats::overlay::SortColumn::RollingDps;
    Check(nte::settings::SaveOverlaySettings(path, newer_overlay_copy, error),
        "overlay should be able to save newer hidden state");
    Check(nte::settings::UpdateOverlayPreferences(
            path, stale_menu_copy, round_trip, error) &&
            round_trip.x == 999 &&
            round_trip.sort_column ==
                nte::combat_stats::overlay::SortColumn::RollingDps &&
            round_trip.opacity == 1.0F,
        "plugin preference updates should merge with the latest hidden fields");
    std::filesystem::remove_all(root);
}

void PluginSettingsUseFallbackAndRoundTripUnicodeLogRoot() {
    const auto root = TemporaryRoot("nte-plugin-settings-test");
    const auto path = root / "plugin-settings.json";
    const auto fallback = root / "fallback";
    std::string error;
    const auto missing = nte::settings::LoadPluginSettings(path, fallback, error);
    Check(missing.log_root == fallback && error.empty(),
        "missing plugin settings should quietly use the default log root");

    std::filesystem::create_directories(root);
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "{\"schema_version\":1,\"log_root\":"
               << std::quoted(nte::settings::PathToUtf8(fallback)) << '}';
    }
    const auto legacy = nte::settings::LoadPluginSettings(path, fallback, error);
    Check(error.empty() && legacy.log_root == fallback && !legacy.developer_mode,
        "schema v1 settings without developer_mode should default it off");

    const auto chosen = root / std::filesystem::path(L"战斗统计");
    nte::settings::PluginSettings expected{chosen};
    Check(nte::settings::SavePluginSettings(path, expected, error),
        "plugin settings should save a Unicode log root");
    const auto loaded = nte::settings::LoadPluginSettings(path, fallback, error);
    Check(error.empty() && loaded.log_root == chosen && !loaded.developer_mode,
        "plugin settings should round trip a Unicode log root and default developer mode off");

    expected.developer_mode = true;
    Check(nte::settings::SavePluginSettings(path, expected, error),
        "developer mode should save with plugin settings");
    const auto developer_loaded =
        nte::settings::LoadPluginSettings(path, fallback, error);
    Check(developer_loaded.log_root == chosen && developer_loaded.developer_mode,
        "developer mode should round trip without changing the log root");

    const auto next = root / std::filesystem::path(L"下一目录");
    auto pending = nte::settings::StagePluginSettings(
        path, nte::settings::PluginSettings{next, std::nullopt, true}, error);
    Check(pending.has_value(), "plugin settings update should stage successfully");
    Check(nte::settings::LoadPluginSettings(path, fallback, error).log_root == chosen,
        "staged plugin settings should not be visible before commit");
    Check(pending && pending->Commit(error) &&
            nte::settings::LoadPluginSettings(path, fallback, error).log_root == next &&
            nte::settings::LoadPluginSettings(path, fallback, error).developer_mode,
        "committing staged plugin settings should atomically publish the new root and preserve developer mode");

    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "not-json";
    }
    const auto malformed = nte::settings::LoadPluginSettings(path, fallback, error);
    Check(malformed.log_root == fallback && !error.empty(),
        "malformed plugin settings should report an error and use the fallback");
    std::filesystem::remove_all(root);
}

void ExportRootUpdatesPreserveConcurrentLogRootChanges() {
    const auto root = TemporaryRoot("nte-plugin-export-settings-test");
    const auto path = root / "plugin-settings.json";
    const auto fallback = root / "fallback";
    const auto original_logs = root / std::filesystem::path(L"原日志");
    const auto next_logs = root / std::filesystem::path(L"新日志");
    const auto exports = root / std::filesystem::path(L"导出目录");
    std::string error;

    Check(nte::settings::SavePluginSettings(path, {original_logs, std::nullopt, true}, error),
        "legacy settings without export_root should still save");
    auto loaded = nte::settings::LoadPluginSettings(path, fallback, error);
    Check(loaded.log_root == original_logs && !loaded.export_root &&
            loaded.developer_mode,
        "missing export_root should load as unset and preserve developer mode");

    auto pending = nte::settings::StagePluginSettings(path, {next_logs}, error);
    Check(pending.has_value(), "log root change should stage before publication");
    nte::settings::PluginSettings merged;
    Check(nte::settings::UpdatePluginExportRoot(
            path, fallback, exports, merged, error) &&
            merged.log_root == original_logs && merged.export_root == exports &&
            merged.developer_mode,
        "setting export_root should preserve the current log_root and developer mode");
    Check(nte::settings::UpdatePluginLogRoot(
            path, original_logs, merged, error) && merged.export_root == exports &&
            merged.developer_mode,
        "updating log_root in place should preserve export_root and developer mode under the same lock");
    Check(pending && pending->Commit(error),
        "staged log root should commit after a concurrent export update");
    loaded = nte::settings::LoadPluginSettings(path, fallback, error);
    Check(loaded.log_root == next_logs && loaded.export_root == exports &&
            loaded.developer_mode,
        "committing log_root should preserve a newer export_root and developer mode");

    const auto final_logs = root / std::filesystem::path(L"最终日志");
    pending = nte::settings::StagePluginSettings(
        path, {final_logs, exports, true}, error);
    Check(nte::settings::UpdatePluginExportRoot(
            path, fallback, std::nullopt, merged, error) &&
            merged.log_root == next_logs && !merged.export_root,
        "clearing export_root should not change log_root");
    Check(pending && pending->Commit(error),
        "staged log root should commit after export_root is cleared");
    loaded = nte::settings::LoadPluginSettings(path, fallback, error);
    Check(loaded.log_root == final_logs && !loaded.export_root,
        "committing log_root should preserve a newer export_root clear");
    std::filesystem::remove_all(root);
}

void LogRootValidationRejectsRelativePathsAndAcceptsWritableDirectories() {
    std::string error;
    Check(!nte::settings::PrepareLogRoot("relative/logs", error) && !error.empty(),
        "relative log roots should be rejected");

    const auto root = TemporaryRoot("nte-log-root-test") /
        std::filesystem::path(L"可写目录");
    Check(nte::settings::PrepareLogRoot(root, error) && error.empty() &&
            std::filesystem::is_directory(root),
        "an absolute writable log root should be prepared");
    const auto file = root.parent_path() / "not-a-directory";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << "occupied";
    }
    Check(!nte::settings::PrepareLogRoot(file, error) && !error.empty(),
        "a regular file should be rejected as a log root");
    std::filesystem::remove_all(root.parent_path());
}

void HideIdlePolicyAppliesImmediatelyToCurrentState() {
    using nte::combat_stats::EncounterState;
    using nte::combat_stats::overlay::ShouldHideOverlay;
    Check(ShouldHideOverlay(true, EncounterState::Idle),
        "hide-idle should hide an idle overlay immediately");
    Check(!ShouldHideOverlay(true, EncounterState::Active) &&
            !ShouldHideOverlay(false, EncounterState::Idle) &&
            !ShouldHideOverlay(true, std::nullopt),
        "hide-idle should not hide active, disabled, or uninitialized overlays");
}

}  // namespace

int main() {
    OverlaySettingsRemainCompatibleAndPreserveAllFields();
    PluginSettingsUseFallbackAndRoundTripUnicodeLogRoot();
    ExportRootUpdatesPreserveConcurrentLogRootChanges();
    LogRootValidationRejectsRelativePathsAndAcceptsWritableDirectories();
    HideIdlePolicyAppliesImmediatelyToCurrentState();
    if (failures != 0) return 1;
    std::cout << "All app settings tests passed\n";
    return 0;
}
