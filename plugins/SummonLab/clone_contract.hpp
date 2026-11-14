#pragma once

#include "native_layout.hpp"
#include <string_view>
#include <vector>

namespace summon_lab {

struct CloneLayout {
    std::uint32_t row_map{}, map_size{}, stride{}, value{}, bits{}, heap_bits{}, num_bits{}, max_bits{}, free_count{}, inline_bits{};
    std::uint32_t soft_package{}, soft_asset{}, soft_subpath{};
    std::uint32_t target{};
};

inline bool LoadCloneLayout(const anomaly::sdk::Profile& profile, CloneLayout& out) {
    try {
        const auto& v = profile.layout;
        CloneLayout p{v.at("cloneTableRowMap"), v.at("cloneMapSize"), v.at("cloneMapStride"), v.at("cloneMapValue"),
            v.at("cloneMapBits"), v.at("cloneMapHeapBits"), v.at("cloneMapNumBits"),
            v.at("cloneMapMaxBits"), v.at("cloneMapFreeCount"), v.at("cloneMapInlineBits"),
            v.at("cloneSoftPackage"), v.at("cloneSoftAsset"), v.at("cloneSoftSubpath")};
        if (p.row_map > 4096 || p.row_map % alignof(std::uintptr_t) ||
            p.map_size > 256 || p.map_size % alignof(std::uintptr_t) || p.stride > 256 ||
            p.stride % alignof(std::uintptr_t) ||
            !ValidBuffer(p.stride, {{0, sizeof(std::uint64_t)}, {p.value, sizeof(std::uintptr_t)}}) ||
            p.inline_bits == 0 || p.inline_bits > 1024 || p.inline_bits % 32 ||
            !ValidBuffer(p.map_size, {{0, 16}, {p.bits, p.inline_bits / 8}, {p.heap_bits, 8},
                {p.num_bits, 4}, {p.max_bits, 4}, {p.free_count, 4}}) ||
            !ValidBuffer(sizeof(SoftClassRef), {{0, 8}, {p.soft_package, 8}, {p.soft_asset, 8},
                {p.soft_subpath, 16}})) return false;
        out = p;
        return true;
    } catch (const std::out_of_range&) { return false; }
}

// Only public, canonical character entries from DT_Character can enter this mode.
inline bool PlayableClonePath(bool is_public, std::string_view package, std::string_view asset) {
    if (!is_public || !package.starts_with("/Game/Blueprints/Character/Player/") ||
        !asset.ends_with("_C")) return false;
    const auto leaf = package.substr(package.find_last_of('/') + 1);
    return !leaf.empty() && asset.substr(0, asset.size() - 2) == leaf;
}

// TMap uses sparse slots. Never interpret a freed slot as a row pointer.
template<class Read>
bool ReadCloneRows(std::uintptr_t map, const CloneLayout& p, Read read,
                   std::vector<std::uintptr_t>& rows) {
    struct Array { std::uintptr_t data; std::int32_t count, capacity; } array{};
    std::int32_t count{}, capacity{}, free{};
    std::uintptr_t heap{};
    if (!read(map, &array, sizeof(array)) ||
        !read(map + p.num_bits, &count, sizeof(count)) ||
        !read(map + p.max_bits, &capacity, sizeof(capacity)) ||
        !read(map + p.free_count, &free, sizeof(free)) ||
        !read(map + p.heap_bits, &heap, sizeof(heap)) ||
        count != array.count || count < 0 || count > 4096 || array.capacity < count ||
        capacity < count || capacity > 4096 || free < 0 || free > count ||
        (count && !array.data) || (capacity > static_cast<int>(p.inline_bits) && !heap)) return false;
    std::vector<std::uintptr_t> found;
    const auto bits = heap ? heap : map + p.bits;
    for (std::int32_t i = 0; i < count; ++i) {
        std::uint32_t word{};
        if (!read(bits + (i / 32) * sizeof(word), &word, sizeof(word))) return false;
        if (!(word & (1u << (i % 32)))) continue;
        std::uintptr_t row{};
        if (!read(array.data + static_cast<std::uintptr_t>(i) * p.stride + p.value,
                  &row, sizeof(row)) || !row) return false;
        found.push_back(row);
    }
    if (found.size() != static_cast<std::size_t>(count - free)) return false;
    rows = std::move(found);
    return true;
}

} // namespace summon_lab
