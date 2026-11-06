#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cstdint>
#include <string_view>

namespace nte_tv_video_profile {

// Values the plugin needs from HTGame.exe's live layout. Everything that is a
// game offset or a function signature lives in profile/compatibility.profile.json;
// this header only validates and caches the snapshot.
struct Layout {
    std::uint32_t kObjectClassOffset{};
    std::uint32_t kObjectNameOffset{};
    std::uint32_t kObjectOuterOffset{};
    std::uint32_t kFStringNumOffset{};
    std::uint32_t kFStringMaxOffset{};
};

inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;

// Semantic contract for the residence TV video source. It describes what the
// plugin supports, never a build identity.
inline constexpr anomaly::sdk::ProfileContract kContract{"nte", "HTGame.exe", ".text",
                                                        "nte-tv-video-v1"};

inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json", kContract)) return false;
        Layout parsed;
        parsed.kObjectClassOffset = compatibility.Value("kObjectClassOffset", 1024, 4);
        parsed.kObjectNameOffset = compatibility.Value("kObjectNameOffset", 1024, 4);
        parsed.kObjectOuterOffset = compatibility.Value("kObjectOuterOffset", 1024, 8);
        parsed.kFStringNumOffset = compatibility.Value("kFStringNumOffset", 64, 4);
        parsed.kFStringMaxOffset = compatibility.Value("kFStringMaxOffset", 64, 4);
        native_layout = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] inline std::uintptr_t Address(const std::string_view symbol) noexcept {
    return compatibility.Address(symbol);
}

}  // namespace nte_tv_video_profile
