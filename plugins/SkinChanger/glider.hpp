#pragma once
#include "appearance.hpp"
#include <cstddef>
#include <vector>

namespace accessory {
// HT's 40-byte soft class pointer contains a byte-string subobject path.
// Never shallow-copy its owning string into a game-owned output.
struct SoftClass {
    std::uint64_t weak{};
    Name package, asset;
    const std::uint8_t* subpath{};
    std::int32_t count{}, capacity{};
};
static_assert(sizeof(SoftClass)==40 && offsetof(SoftClass,subpath)==24);
struct GliderPath {
    SoftClass header{};
    std::vector<std::uint8_t> subpath;
    template<class Reader> bool Capture(std::uintptr_t address,Reader read) {
        SoftClass candidate{};
        if(!read(address,&candidate,sizeof(candidate))||!candidate.package.index||!candidate.asset.index||
           candidate.count<0||candidate.count>4096||candidate.capacity<candidate.count||
           (candidate.count&&!candidate.subpath))return false;
        std::vector<std::uint8_t> bytes(candidate.count);
        if(candidate.count&&!read(reinterpret_cast<std::uintptr_t>(candidate.subpath),bytes.data(),bytes.size()))return false;
        header=candidate;header.subpath=nullptr;subpath=std::move(bytes);return true;
    }
    SoftClass View() const {
        auto result=header;result.subpath=subpath.empty()?nullptr:subpath.data();
        result.count=result.capacity=static_cast<std::int32_t>(subpath.size());return result;
    }
};
}
