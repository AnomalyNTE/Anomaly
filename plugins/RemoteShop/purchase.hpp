#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace shop {
struct Name {
    uint32_t id{}, number{};
    bool operator==(const Name&) const = default;
};
struct CityParameters {
    uint8_t city{};
    uint8_t padding[3]{};
    Name brand{};
    uint8_t likeability{};
    uint8_t padding2[3]{};
    int32_t level{};
};
struct BuyParameters {
    Name shop, goods;
    int64_t quantity{};
    uint8_t popup{1};
    uint8_t padding[3]{};
    CityParameters city{};
};
struct BuyReceipt {
    Name shop, goods;
    int64_t quantity{};
    uint8_t city{};
    uint8_t padding[7]{};
};
static_assert(sizeof(Name) == 8 && sizeof(CityParameters) == 0x14);
static_assert(sizeof(BuyParameters) == 0x30 && offsetof(BuyParameters, city) == 0x1c);
static_assert(sizeof(BuyReceipt) == 0x20 && offsetof(BuyReceipt, quantity) == 0x10);
// UFunction::ParmsSize excludes the C++ struct's seven bytes of tail padding.
inline constexpr size_t ReceiptParameterSize = offsetof(BuyReceipt, city) + sizeof(BuyReceipt::city);

inline std::optional<int64_t> TotalPrice(int64_t price, uint32_t quantity) {
    if (price < 0 || !quantity || price > std::numeric_limits<int64_t>::max() / quantity) return {};
    return price * quantity;
}

enum class Outcome { Idle, Waiting, Confirmed, Partial, Unconfirmed, SceneChanged, DispatchFailed };
struct Purchase {
    BuyParameters order{};
    uint64_t started{}, actor_handle{}, actor_generation{};
    int64_t confirmed{};
    Outcome outcome{Outcome::Idle};
    bool ambiguous{};

    bool Waiting() const { return outcome == Outcome::Waiting; }
    bool SameGoods(Name shop, Name goods) const { return order.shop == shop && order.goods == goods; }
    void Begin(const BuyParameters& value, uint64_t handle, uint64_t generation, uint64_t now) {
        *this = {value, now, handle, generation, 0, Outcome::Waiting, false};
    }
    bool Accept(const BuyReceipt& receipt, uint64_t handle, uint64_t generation) {
        if (!Waiting() || handle != actor_handle || generation != actor_generation ||
            !SameGoods(receipt.shop, receipt.goods) || receipt.city != order.city.city || receipt.quantity <= 0) return false;
        confirmed = receipt.quantity;
        outcome = ambiguous || receipt.quantity > order.quantity ? Outcome::Unconfirmed :
            receipt.quantity == order.quantity ? Outcome::Confirmed : Outcome::Partial;
        return true;
    }
    void ObserveOtherOrder(const BuyParameters& other) {
        if (Waiting() && SameGoods(other.shop, other.goods)) ambiguous = true;
    }
    bool Expire(uint64_t now) {
        if (!Waiting() || now < started || now - started < 20000) return false;
        outcome = Outcome::Unconfirmed;
        return true;
    }
};
} // namespace shop
