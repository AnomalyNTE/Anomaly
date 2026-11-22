#pragma once
#include <string>

namespace shop {
inline std::string DisplayText(std::string text) {
    // The host's default Latin range includes °, but its Chinese range omits ℃ (U+2103).
    // Keep catalog identities intact and expand the symbol only for display/search.
    for (size_t at = 0; (at = text.find("℃",at)) != std::string::npos; at += std::string("°C").size())
        text.replace(at,std::string("℃").size(),"°C");
    return text;
}
} // namespace shop
