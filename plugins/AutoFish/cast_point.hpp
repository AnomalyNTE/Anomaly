#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
struct CastPoint {
    uint32_t magic{0x41465348},version{1};
    std::array<double,3> point{};
    std::array<char,128> world{};
    bool Valid(std::string_view level,const std::array<double,3>& player) const {
        if(magic!=0x41465348 || version!=1 || level.size()>=world.size() ||
           std::memcmp(world.data(),level.data(),level.size())!=0 || world[level.size()]!=0) return false;
        double distance{};
        for(int i=0;i<3;++i) {
            if(!std::isfinite(point[i]) || !std::isfinite(player[i])) return false;
            distance+=(point[i]-player[i])*(point[i]-player[i]);
        }
        return distance<=6000.0*6000.0;
    }
};
static_assert(sizeof(CastPoint)==160);
