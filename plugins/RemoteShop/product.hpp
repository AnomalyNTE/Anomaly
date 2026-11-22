#pragma once
#include <compare>
#include <cstdint>
#include <string>

namespace shop {
struct ProductKey {
    std::string item, display, currency;
    int64_t price{};
    auto operator<=>(const ProductKey&) const = default;
};
} // namespace shop
