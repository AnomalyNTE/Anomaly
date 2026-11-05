#pragma once
#include "../common/combat/native_layout.hpp"
namespace combatbox_profile {
struct Layout {
    std::uint32_t kDataTableRowMapOffset{};
    std::uint32_t kDataTableRowStride{};
    std::uint32_t kDataTableRowPointerOffset{};
    std::uint32_t kCoordOffset{};
    std::uint32_t kObjectItemsOffset{};
    std::uint32_t kObjectCountOffset{};
    std::uint32_t kObjectNumChunksOffset{};
    std::uint32_t kObjectChunkSize{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kFallOutMaximumRetries{};
    std::uint32_t kMaximumMapLandmarks{};
    std::uint32_t kGObjectsSubtract{};
    std::uint32_t kArrayNumOffset{};
    std::uint32_t kArrayMaxOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "combat-box-v1"}, {"ProcessEvent.entry"})) return false;
        Layout parsed;
        parsed.kDataTableRowMapOffset=compatibility.Value("kDataTableRowMapOffset",65536);
        parsed.kDataTableRowStride=compatibility.Value("kDataTableRowStride",65536);
        parsed.kDataTableRowPointerOffset=compatibility.Value("kDataTableRowPointerOffset",65536);
        parsed.kCoordOffset=compatibility.Value("kCoordOffset",65536);
        parsed.kObjectItemsOffset=compatibility.Value("kObjectItemsOffset",65536);
        parsed.kObjectCountOffset=compatibility.Value("kObjectCountOffset",65536);
        parsed.kObjectNumChunksOffset=compatibility.Value("kObjectNumChunksOffset",65536);
        parsed.kObjectChunkSize=compatibility.Value("kObjectChunkSize",65536);
        parsed.kObjectItemStride=compatibility.Value("kObjectItemStride",65536);
        parsed.kFallOutMaximumRetries=compatibility.Value("kFallOutMaximumRetries",65536);
        parsed.kMaximumMapLandmarks=compatibility.Value("kMaximumMapLandmarks",65536);
        parsed.kGObjectsSubtract=compatibility.Value("kGObjectsSubtract",65536);
        parsed.kArrayNumOffset=compatibility.Value("kArrayNumOffset",65536);
        parsed.kArrayMaxOffset=compatibility.Value("kArrayMaxOffset",65536);
        if (!parsed.kObjectChunkSize || parsed.kObjectItemStride<sizeof(void*) || !parsed.kDataTableRowStride) return false;
        if (!anomaly::plugins::combat::profile::Configure(host,compatibility)) return false;
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
