#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
struct HudMapHeader {
    std::uintptr_t data{};std::int32_t count{},capacity{};
    std::uint32_t inline_bits[4]{};std::uintptr_t extra_bits{};
    std::int32_t bit_count{},bit_capacity{},first_free{},free_count{};
    std::uint32_t inline_hash{},pad{};std::uintptr_t extra_hash{};
    std::int32_t hash_count{},padding{};
    bool Valid() const {
        return count>=0 && count<=256 && capacity>=count && bit_count>=count && bit_count<=4096 &&
            bit_capacity>=bit_count && (extra_bits || count<=128) && (!count || data);
    }
};
static_assert(sizeof(HudMapHeader)==80 && offsetof(HudMapHeader,bit_count)==40 && offsetof(HudMapHeader,first_free)==48);
