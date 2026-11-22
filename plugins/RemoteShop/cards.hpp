#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <cmath>
#include <anomaly/sdk/services/ui.h>
#include "text.hpp"

namespace shop {
struct CardMetrics {
    float header{},row{},spacing{},height{};
    float FooterGap(float body_height) const {
        return (std::max)(4.0f,body_height - 2 * row - 2 * spacing - 6);
    }
    static CardMetrics ForLineHeight(float line) {
        line = (std::max)(1.0f,line);
        const auto header = (std::max)(20.0f,std::round(line * 25 / 13));
        const auto spacing = std::round(line * 7 / 13),row = header + 4;
        const auto height = (std::max)({112.0f,header + 80,2 * row + 2 * spacing + line * 12 / 13 + 8});
        return {header,row,spacing,height};
    }
};
struct CardGrid {
    static constexpr float Gap = 10;
    int columns{1}; float width{280};
    static CardGrid ForWidth(float width, float minimum = 264, int maximum_columns = 5) {
        const auto available = (std::max)(minimum,width - 32);
        const int columns = (std::clamp)(int((available + Gap) / (minimum + Gap)),1,maximum_columns);
        return {columns,(available - (columns - 1) * Gap) / columns};
    }
};
inline float TextWidth(const AnomalyUiServiceV1& ui, std::string_view text) {
    const auto display = DisplayText(std::string(text));
    text = display;
    float width{},height{};
    if (ui.struct_size >= offsetof(AnomalyUiServiceV1,overlay_text) && ui.overlay_measure_text) {
        ui.overlay_measure_text(ui.user,{text.data(),text.size()},0,&width,&height);
        return width;
    }
    for (unsigned char c : text) if ((c & 0xc0) != 0x80) width += c < 128 ? 7.0f : 14.0f;
    return width;
}
inline std::string FitText(const AnomalyUiServiceV1& ui, std::string text, float width) {
    text = DisplayText(std::move(text));
    if (TextWidth(ui,text) <= width) return text;
    while (!text.empty()) {
        size_t start = text.size() - 1;
        while (start && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
        text.resize(start);
        if (TextWidth(ui,text + "…") <= width) return text + "…";
    }
    return {};
}
inline void RightText(const AnomalyUiServiceV1& ui, std::string text, float width) {
    text = FitText(ui,std::move(text),(std::max)(0.0f,width - 24));
    if (ui.set_cursor_pos_x) ui.set_cursor_pos_x(ui.user,(std::max)(12.0f,width - TextWidth(ui,text) - 16));
    ui.text(ui.user,{text.data(),text.size()});
}
}
