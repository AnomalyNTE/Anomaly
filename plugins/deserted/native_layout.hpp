#pragma once

#include <anomaly/sdk/profile.hpp>
#include <array>
#include <cstddef>

namespace deserted_profile {

struct Layout {
    std::uint32_t kClassPrivateOffset{};
    std::uint32_t kNamePrivateOffset{};
    std::uint32_t kDefaultProfileOffset{};
    std::uint32_t kDensityProfilesOffset{};
    std::uint32_t kProfileStride{};
    std::uint32_t kSpawnCountOffset{};
    std::uint32_t kSpawnScaleOffset{};
    std::uint32_t kVehMinActiveOffset{};
    std::uint32_t kObjectChunkSize{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kArrayNumOffset{};
    std::array<std::uint32_t, 5> kScaleOffsets{};
};

inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;

inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json",
                                {"nte", "HTGame.exe", ".text", "deserted-density-v1"})) return false;
        Layout layout;
        layout.kClassPrivateOffset = compatibility.Value("kClassPrivateOffset", 65536, 4);
        layout.kNamePrivateOffset = compatibility.Value("kNamePrivateOffset", 65536, 4);
        layout.kDefaultProfileOffset = compatibility.Value("kDefaultProfileOffset", 65536, 4);
        layout.kDensityProfilesOffset = compatibility.Value("kDensityProfilesOffset", 65536, 8);
        layout.kProfileStride = compatibility.Value("kProfileStride", 65536, 4);
        layout.kSpawnCountOffset = compatibility.Value("kSpawnCountOffset", 65536, 4);
        layout.kSpawnScaleOffset = compatibility.Value("kSpawnScaleOffset", 65536, 4);
        layout.kVehMinActiveOffset = compatibility.Value("kVehMinActiveOffset", 0x8000, 4);
        layout.kObjectChunkSize = compatibility.Value("kObjectChunkSize", 65536, 4);
        layout.kObjectItemStride = compatibility.Value("kObjectItemStride", 65536, 4);
        layout.kArrayNumOffset = compatibility.Value("kArrayNumOffset", 65536, 4);
        layout.kScaleOffsets[0] = compatibility.Value("scale0", 65536, 4);
        layout.kScaleOffsets[1] = compatibility.Value("scale1", 65536, 4);
        layout.kScaleOffsets[2] = compatibility.Value("scale2", 65536, 4);
        layout.kScaleOffsets[3] = compatibility.Value("scale3", 65536, 4);
        layout.kScaleOffsets[4] = compatibility.Value("scale4", 65536, 4);
        if (!layout.kObjectChunkSize || layout.kObjectItemStride < sizeof(void*) ||
            !layout.kDefaultProfileOffset || layout.kProfileStride < sizeof(float) * layout.kScaleOffsets.size() ||
            layout.kDefaultProfileOffset + layout.kProfileStride > layout.kDensityProfilesOffset ||
            layout.kVehMinActiveOffset < layout.kSpawnScaleOffset) return false;
        for (std::size_t i{}; i < layout.kScaleOffsets.size(); ++i) {
            if (layout.kScaleOffsets[i] + sizeof(float) > layout.kProfileStride ||
                (i && layout.kScaleOffsets[i - 1] + sizeof(float) > layout.kScaleOffsets[i])) return false;
        }
        native_layout = layout;
        return true;
    } catch (...) { return false; }
}

}  // namespace deserted_profile
