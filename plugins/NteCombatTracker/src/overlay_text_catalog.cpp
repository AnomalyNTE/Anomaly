#include "overlay_text_catalog.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace nte::combat_stats::overlay {
namespace {

// UTF-8 to UTF-16 without relying on the process ACP: the overlay renders wide
// text and the catalog is UTF-8.
[[nodiscard]] std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        wide.data(), count);
    return wide;
}

}  // namespace

void TextCatalog::Load(const std::filesystem::path& executable_root,
                       const std::string& locale) {
    messages_.clear();
    if (locale.empty()) return;
    // The locale reaches us from another process; accept only a plain token so
    // it can never steer the read outside the locales directory.
    for (const char character : locale) {
        const bool safe = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_';
        if (!safe) return;
    }
    const auto path = executable_root / "locales" / (locale + ".json");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return;
    try {
        const auto document = nlohmann::json::parse(stream);
        if (!document.contains("messages") || !document.at("messages").is_object()) {
            return;
        }
        for (const auto& [key, value] : document.at("messages").items()) {
            if (!value.is_string()) continue;
            const std::string text = value.get<std::string>();
            if (text.empty()) continue;
            messages_.emplace(key, Widen(text));
        }
    } catch (const std::exception&) {
        // A malformed catalog is equivalent to a missing one: the caller keeps
        // its English fallback rather than the overlay failing to render.
        messages_.clear();
    }
}

std::wstring TextCatalog::Text(const std::string_view key,
                               const std::string_view fallback) const {
    const auto found = messages_.find(std::string(key));
    if (found != messages_.end()) return found->second;
    return Widen(std::string(fallback));
}

}  // namespace nte::combat_stats::overlay
