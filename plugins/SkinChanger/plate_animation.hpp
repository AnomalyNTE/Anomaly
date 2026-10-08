#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace plate_animation {
inline constexpr std::wstring_view Glyphs = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\u00b7- \u2665";
inline std::optional<std::wstring> Normalize(std::wstring text, size_t width) {
    if (text.empty() || text.size() > width) return {};
    for (auto& c : text) {
        if (c >= L'a' && c <= L'z') c -= L'a'-L'A';
        if (Glyphs.find(c) == std::wstring_view::npos) return {};
    }
    return text;
}
inline std::wstring Frame(std::wstring_view text, size_t width, double elapsed, double interval, bool dynamic) {
    if (dynamic && width && !text.empty() && interval > 0 && std::isfinite(interval) && std::isfinite(elapsed)) {
        std::wstring ring(text.substr(0,width)); ring.resize(width,L' ');
        const auto step = static_cast<size_t>(std::fmod(std::max(0.0,elapsed)/interval,static_cast<double>(ring.size())));
        std::wstring frame(width,L' ');
        for (size_t i = 0; i < width; ++i) frame[i] = ring[(step+i)%ring.size()];
        return frame;
    }
    std::wstring frame(text.substr(0,std::min(text.size(),width)));
    frame.resize(width,L' ');
    return frame;
}
}
