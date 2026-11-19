#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace anomaly {


struct NteDamageCriticalState {
    bool valid{};
    bool critical{};
};


// Own the FName values: a GameplayTagContainer only contains borrowed arrays.
struct NteDamageTags {
    static constexpr std::size_t kTagsPerArray = 16;
    std::array<std::uint64_t, kTagsPerArray * 2> names{};
    std::uint32_t count{};
    bool partial{};

    [[nodiscard]] std::span<const std::uint64_t> Values() const noexcept {
        return std::span(names).first(count);
    }
};

template <typename Reader>
NteDamageTags CaptureNteDamageTags(std::uintptr_t container, const Reader& read) {
    struct ArrayHeader {
        std::uintptr_t data{};
        std::int32_t count{};
        std::int32_t capacity{};
    };
    static_assert(sizeof(ArrayHeader) == 16);
    NteDamageTags result;
    std::array<ArrayHeader, 2> arrays{};
    if (!read(container, arrays.data(), sizeof(arrays))) {
        result.partial = true;
        return result;
    }
    for (const auto& array : arrays) {
        if (array.count < 0 || array.capacity < array.count || array.count > 4096 ||
            (array.count != 0 && array.data == 0)) {
            result.partial = true;
            continue;
        }
        const auto count = (std::min)(
            static_cast<std::size_t>(array.count), NteDamageTags::kTagsPerArray);
        if (count != 0 && !read(array.data, result.names.data() + result.count,
                                count * sizeof(std::uint64_t))) {
            result.partial = true;
            continue;
        }
        result.count += static_cast<std::uint32_t>(count);
        result.partial = result.partial || count != static_cast<std::size_t>(array.count);
    }
    return result;
}

struct NteCharacterDamageCapture {
    float damage{};
    std::int32_t source_index{-1};
    std::int32_t source_serial{};
    std::uintptr_t victim{};
    std::uintptr_t attacker{};
    std::uintptr_t saved_skill_cdo{};
    std::int32_t active_spec_handle{};
    NteDamageCriticalState critical;
    bool applied_health_loss{};
    NteDamageTags tags;
};
static_assert(std::is_trivially_copyable_v<NteCharacterDamageCapture>);

}  // namespace anomaly
