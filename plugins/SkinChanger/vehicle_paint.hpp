#pragma once
#include <array>
#include <cmath>
#include <cstring>
#include <span>
namespace vehicle_paint {
inline constexpr int ColorIndex=2,Maximum=36;
inline bool Override(int start,std::span<const float> input,float paint,std::array<float,Maximum>& output) {
    if(start<0 || start>ColorIndex || input.empty() || input.size()>Maximum ||
       static_cast<size_t>(ColorIndex-start)>=input.size() || !std::isfinite(paint)) return false;
    std::memcpy(output.data(),input.data(),input.size_bytes());
    output[ColorIndex-start]=paint;return true;
}
}
