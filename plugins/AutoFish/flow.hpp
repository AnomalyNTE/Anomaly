#pragma once
#include <cstdint>
#include <string_view>
#include <algorithm>
#include <cmath>
namespace fishing {
enum class Action { None, Cast, Hook, Pull, Continue, Stop };
inline int EventPhase(std::string_view event) {
    if(event=="Client_StartThrowRod") return 2;
    if(event=="Client_CreateFeatureFish") return 3;
    if(event=="Client_HasFishBait") return 4;
    if(event=="ClientPlayerSureFishing" || event=="BP_RefreshFishEndurance") return 5;
    if(event=="Client_ThrowUpResult" || event=="Server_TryExitFishingState") return 0;
    return -1;
}
inline float LockDelta(double target,double position,float speed,float elapsed) {
    if(!std::isfinite(target) || !std::isfinite(position) || !std::isfinite(speed) || speed<=0 ||
       !std::isfinite(elapsed) || elapsed<=0) return 0;
    return static_cast<float>(std::min(std::abs(target-position)/speed,static_cast<double>(std::min(elapsed,0.05F))));
}
inline double MarkerCenter(double position,double width,double alignment) {
    return position+(0.5-alignment)*width;
}
struct Flow {
    int phase{-1};bool result{};std::uint64_t changed{},last_action{};int attempts{};
    void Reset() {*this={};phase=-1;}
    Action Next(std::uint8_t state,bool main_visible,bool result_visible,bool repeat,std::uint64_t now,bool result_ready=true) {
        if(state>5) return Action::Stop;
        if(phase!=state || result!=result_visible) {phase=state;result=result_visible;changed=now;attempts=0;last_action=0;}
        if(result_visible) {
            if(!repeat) return Action::Stop;
            if(!result_ready) return now-changed>=30000?Action::Stop:Action::None;
            if(now-changed<250 || (last_action && now-last_action<2000)) return Action::None;
            return attempts>=3?Action::Stop:Action::Continue;
        }
        if(!main_visible) return Action::None;
        if(state==1) {
            if(now-changed<700 || (last_action && now-last_action<2500)) return Action::None;
            return attempts>=3?Action::Stop:Action::Cast;
        }
        if(state==4 && attempts==0 && now-changed>=150) return Action::Hook;
        if(state==5) return now-changed>=180000?Action::Stop:now-changed>=200?Action::Pull:Action::None;
        if(state==4 && attempts && now-last_action>=10000) return Action::Stop;
        return Action::None;
    }
    void Sent(std::uint64_t now) {last_action=now;++attempts;}
};
}
