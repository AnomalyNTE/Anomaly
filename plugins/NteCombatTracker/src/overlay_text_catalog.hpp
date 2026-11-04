#pragma once

// Text catalog for the overlay process.
//
// The overlay is a separate executable launched by the plugin, so it has no
// AnomalyHostApiV1 and cannot query `anomaly.localization`. It instead reads the
// same `locales/<locale>.json` the plugin ships, using the locale the plugin
// forwards in each snapshot. A missing catalog or a missing key yields the
// English fallback passed by the caller, so the overlay stays usable on its own.

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nte::combat_stats::overlay {

class TextCatalog {
public:
    // Locates `locales/<locale>.json` beside the overlay executable. Never
    // throws: an unreadable catalog leaves every lookup on its fallback.
    void Load(const std::filesystem::path& executable_root, const std::string& locale);

    // English fallback when the catalog has no entry for `key`.
    [[nodiscard]] std::wstring Text(std::string_view key, std::string_view fallback) const;

    [[nodiscard]] bool loaded() const noexcept { return !messages_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return messages_.size(); }

private:
    std::unordered_map<std::string, std::wstring> messages_;
};

}  // namespace nte::combat_stats::overlay
