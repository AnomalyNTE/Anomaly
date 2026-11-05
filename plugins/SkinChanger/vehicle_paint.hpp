#pragma once
#include <array>
#include <cmath>
#include <cstring>
#include <span>
namespace vehicle_paint {
inline constexpr int Maximum=36;
inline bool Override(int start,std::span<const float> input,float paint,std::array<float,Maximum>& output,int color_index) {
    if(color_index<0 || color_index>=Maximum || start<0 || start>color_index || input.empty() || input.size()>Maximum ||
       static_cast<size_t>(color_index-start)>=input.size() || !std::isfinite(paint)) return false;
    std::memcpy(output.data(),input.data(),input.size_bytes());
    output[color_index-start]=paint;return true;
}
}
