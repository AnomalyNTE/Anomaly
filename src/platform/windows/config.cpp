#include "config.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

namespace ue5mem {
namespace {

std::wstring ReadWide(
    const std::filesystem::path& path,
    const wchar_t* section,
    const wchar_t* key,
    const wchar_t* fallback) {
    std::array<wchar_t, 32768> buffer{};
    GetPrivateProfileStringW(
        section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

std::string NarrowAscii(const std::wstring& value) {
    std::string output;
    output.reserve(value.size());
    for (const wchar_t character : value) {
        output.push_back(static_cast<char>(character & 0x7f));
    }
    return output;
}

std::size_t ReadSize(
    const std::filesystem::path& path,
    const wchar_t* section,
    const wchar_t* key,
    std::size_t fallback) {
    const auto text = NarrowAscii(ReadWide(path, section, key, L""));
    if (text.empty()) {
        return fallback;
    }
    std::size_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} ? value : fallback;
}

bool ReadBool(
    const std::filesystem::path& path,
    const wchar_t* section,
    const wchar_t* key,
    bool fallback) {
    return ReadSize(path, section, key, fallback ? 1 : 0) != 0;
}

}  // namespace

AnalyzerConfig AnalyzerConfig::Load(const std::filesystem::path& path) {
    AnalyzerConfig result;
    result.pipe_prefix = ReadWide(path, L"Analyzer", L"PipePrefix", L"Anomaly");
    result.max_scan_results = ReadSize(path, L"Analyzer", L"MaxScanResults", 1024);
    result.platform_enabled = ReadBool(path, L"Platform", L"Enabled", true);
    result.platform_visible = ReadBool(path, L"Platform", L"Visible", false);
    result.platform_embedded = ReadBool(path, L"Platform", L"Embedded", true);
    result.platform_attach_to_process_window =
        ReadBool(path, L"Platform", L"AttachToProcessWindow", true);
    result.platform_toggle_key = static_cast<unsigned>(
        ReadSize(path, L"Platform", L"ToggleKey", VK_INSERT));
    const std::string language =
        NarrowAscii(ReadWide(path, L"Platform", L"Language", L"auto"));
    const auto parsed_language = anomaly::ParseLanguagePreference(language);
    result.platform_language = parsed_language.preference;
    if (!parsed_language.valid) {
        result.diagnostics.push_back({
            "Platform.Language",
            "unsupported language preference '" + language + "'; using en-US"});
    }
    result.plugin_directory = ReadWide(path, L"Platform", L"PluginDirectory", L"plugins");
    result.update_slow_milliseconds = static_cast<double>(
        std::max<std::size_t>(1, ReadSize(path, L"Performance", L"UpdateSlowMilliseconds", 2)));
    result.draw_slow_milliseconds = static_cast<double>(
        std::max<std::size_t>(1, ReadSize(path, L"Performance", L"DrawSlowMilliseconds", 4)));
    result.player_snapshot_tick_interval = std::max<std::size_t>(
        1, ReadSize(path, L"Performance", L"PlayerSnapshotTickInterval", 1));
    result.entity_snapshot_tick_interval = std::max<std::size_t>(
        1, ReadSize(path, L"Performance", L"EntitySnapshotTickInterval", 1));
    result.game_id = NarrowAscii(ReadWide(path, L"Profiles", L"Game", L"nte"));
    result.profile_directory = ReadWide(path, L"Profiles", L"Directory", L"profiles");
    result.local_profile_directory =
        ReadWide(path, L"Profiles", L"LocalDirectory", L"profiles-local");
    result.managed_profile_directory =
        ReadWide(path, L"Profiles", L"ManagedDirectory", L"state/profiles/managed");
    return result;
}

}  // namespace ue5mem
