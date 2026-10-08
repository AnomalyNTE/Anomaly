#pragma once
#include <cstdint>
namespace fishing {
enum class SellAction { None, Send, Wait, Received, Timeout };
struct SellPolicy {
    bool pending{},failed{};
    std::uint64_t sent{},last_received{};
    std::int64_t baseline{};
    SellAction Next(bool enabled,std::uint32_t threshold,std::int64_t total,std::int64_t locked,bool idle,std::uint64_t now) {
        if(total<0 || locked<0 || locked>total) return pending?SellAction::Wait:SellAction::None;
        if(pending) {
            if(total<baseline) {pending=false;last_received=now;return SellAction::Received;}
            if(now-sent>=10000) {pending=false;failed=true;return SellAction::Timeout;}
            return SellAction::Wait;
        }
        if(!enabled || !threshold || !idle || failed || (last_received && now-last_received<3000)) return SellAction::None;
        return total>=threshold && total>locked?SellAction::Send:SellAction::None;
    }
    void Begin(std::int64_t total,std::uint64_t now) {pending=true;baseline=total;sent=now;}
    void Retry() {if(!pending) failed=false;}
};
}
