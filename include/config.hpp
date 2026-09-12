#pragma once

#include "anomaly/i18n.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ue5mem {

struct ConfigDiagnostic final {
    std::string key;
    std::string message;
};

struct SymbolConfig {
    std::string name;
    std::wstring module;
    std::string section;
    std::string pattern;
    std::size_t rip_offset{};
    std::size_t instruction_size{};
    std::ptrdiff_t addend{};
};

struct AnalyzerConfig {
    std::wstring pipe_prefix{L"Anomaly"};
    std::size_t max_scan_results{1024};
    std::vector<SymbolConfig> symbols;

    bool platform_enabled{true};
    bool platform_visible{};
    bool platform_embedded{true};
    bool platform_attach_to_process_window{true};
    unsigned platform_toggle_key{0x2d};
    anomaly::LanguagePreference platform_language{anomaly::LanguagePreference::Auto};
    std::filesystem::path plugin_directory{L"plugins"};
    double update_slow_milliseconds{2.0};
    double draw_slow_milliseconds{4.0};
    std::size_t player_snapshot_tick_interval{1};
    std::size_t entity_snapshot_tick_interval{1};
    // 全部关卡 actor 扫描远重于持久化关卡扫描，因此按 tick 节流而不是每帧重扫；
    // 默认约一秒一次，足以跟上大世界的刷怪与清怪。
    std::size_t actor_snapshot_tick_interval{60};
    std::string game_id{"nte"};
    std::filesystem::path profile_directory{L"profiles"};
    std::filesystem::path local_profile_directory{L"profiles-local"};
    std::filesystem::path managed_profile_directory{L"state/profiles/managed"};
    std::vector<ConfigDiagnostic> diagnostics;
    static AnalyzerConfig Load(const std::filesystem::path& path);
};

}  // namespace ue5mem
