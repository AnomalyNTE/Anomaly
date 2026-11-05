#pragma once
#include <array>
#include <cstdint>

namespace claw {
struct ReadinessSample {
    // Session-world and player snapshot service tokens, never UObject handles.
    std::array<std::uint64_t,4> identity{};
    std::uint64_t sequence{};
    bool valid{};
};
struct ReadyGate {
    std::array<std::uint64_t,4> identity{};
    std::uint64_t since{},progress{},sequence{};
    bool observed{};
    void Reset(){*this={};}
    bool Same(const ReadinessSample& sample)const{return observed&&sample.valid&&sample.identity==identity;}
    bool Observe(const ReadinessSample& sample,std::uint64_t now) {
        bool valid=sample.valid&&sample.sequence;
        for(const auto part:sample.identity)valid=valid&&part;
        if(!valid){Reset();return false;}
        if(!Same(sample)||sample.sequence<sequence) {
            identity=sample.identity;since=progress=now;sequence=sample.sequence;observed=true;return false;
        }
        if(now<progress||now-progress>1000) {
            since=now;
            if(sample.sequence!=sequence){sequence=sample.sequence;progress=now;}
            return false;
        }
        if(sample.sequence!=sequence){sequence=sample.sequence;progress=now;}
        return now>=since&&now-since>=2000&&now>=progress&&now-progress<=1000;
    }
};
}
