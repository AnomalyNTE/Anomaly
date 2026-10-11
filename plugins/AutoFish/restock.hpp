#pragma once
#include <cstdint>
#include <limits>
namespace fishing {
inline bool PurchaseCost(std::int64_t price,std::uint32_t quantity,std::int64_t& total) {
    if(price<=0 || !quantity || price>std::numeric_limits<std::int64_t>::max()/quantity) return false;
    total=price*quantity;return true;
}
// A request stays in flight until inventory actually increases. Never resend on timeout:
// a late server reply must not cause a duplicate purchase.
struct Restock {
    bool pending{},failed{};
    std::uint64_t sent{};
    std::int64_t baseline{};
    bool Begin(std::uint64_t now,std::int64_t stock) {
        if(pending || failed || stock<0) return false;
        pending=true;sent=now;baseline=stock;return true;
    }
    bool Received(std::int64_t stock) {
        if(!pending || stock<=baseline) return false;
        pending=false;return true;
    }
    bool Expired(std::uint64_t now) {
        if(!pending || now-sent<10000) return false;
        pending=false;failed=true;return true;
    }
    void Retry() {if(!pending) failed=false;}
};
}
