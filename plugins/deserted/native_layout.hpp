#pragma once
#include <anomaly/sdk/profile.hpp>
#include <array>
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
    std::array<std::uint32_t,5> kScaleOffsets{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "deserted-density-v1"}, {}, false, "layout")) return false;
        Layout parsed;
        parsed.kClassPrivateOffset=compatibility.Value("kClassPrivateOffset",65536,4);
        parsed.kNamePrivateOffset=compatibility.Value("kNamePrivateOffset",65536,4);
        parsed.kDefaultProfileOffset=compatibility.Value("kDefaultProfileOffset",65536,4);
        parsed.kDensityProfilesOffset=compatibility.Value("kDensityProfilesOffset",65536,4);
        parsed.kProfileStride=compatibility.Value("kProfileStride",65536,4);
        parsed.kSpawnCountOffset=compatibility.Value("kSpawnCountOffset",65536,4);
        parsed.kSpawnScaleOffset=compatibility.Value("kSpawnScaleOffset",65536,4);
        parsed.kVehMinActiveOffset=compatibility.Value("kVehMinActiveOffset",65536,4);
        parsed.kObjectChunkSize=compatibility.Value("kObjectChunkSize",65536,4);
        parsed.kObjectItemStride=compatibility.Value("kObjectItemStride",65536,4);
        parsed.kArrayNumOffset=compatibility.Value("kArrayNumOffset",65536,4);
        parsed.kScaleOffsets[0]=compatibility.Value("scale0",65536,4);
        parsed.kScaleOffsets[1]=compatibility.Value("scale1",65536,4);
        parsed.kScaleOffsets[2]=compatibility.Value("scale2",65536,4);
        parsed.kScaleOffsets[3]=compatibility.Value("scale3",65536,4);
        parsed.kScaleOffsets[4]=compatibility.Value("scale4",65536,4);
        if (!parsed.kObjectChunkSize || parsed.kObjectItemStride < sizeof(void*) ||
            parsed.kProfileStride < sizeof(float) || parsed.kDefaultProfileOffset + parsed.kProfileStride > parsed.kDensityProfilesOffset) return false;
        for (std::size_t i{}; i<parsed.kScaleOffsets.size(); ++i) {
            if (parsed.kScaleOffsets[i]+sizeof(float)>parsed.kProfileStride ||
                (i && parsed.kScaleOffsets[i-1]+sizeof(float)>parsed.kScaleOffsets[i])) return false;
        }
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
