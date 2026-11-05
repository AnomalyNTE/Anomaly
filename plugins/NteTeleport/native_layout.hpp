#pragma once
#include <anomaly/sdk/profile.hpp>
namespace nteteleport_profile {
struct Layout {
    std::uint32_t kWorldGameInstanceOffset{};
    std::uint32_t kGameInstanceLocalPlayersOffset{};
    std::uint32_t kLocalPlayerControllerOffset{};
    std::uint32_t kControllerPlayerStateOffset{};
    std::uint32_t kPlayerStateCurNavigationPathEffectOffset{};
    std::uint32_t kNavPathActorLastRequestParamOffset{};
    std::uint32_t kNavPathRequestGoalLocationOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "nte-teleport-v1"}, {}, false, "layout")) return false;
        Layout parsed;
        parsed.kWorldGameInstanceOffset=compatibility.Value("kWorldGameInstanceOffset",16384);
        parsed.kGameInstanceLocalPlayersOffset=compatibility.Value("kGameInstanceLocalPlayersOffset",16384);
        parsed.kLocalPlayerControllerOffset=compatibility.Value("kLocalPlayerControllerOffset",16384);
        parsed.kControllerPlayerStateOffset=compatibility.Value("kControllerPlayerStateOffset",16384);
        parsed.kPlayerStateCurNavigationPathEffectOffset=compatibility.Value("kPlayerStateCurNavigationPathEffectOffset",16384);
        parsed.kNavPathActorLastRequestParamOffset=compatibility.Value("kNavPathActorLastRequestParamOffset",16384);
        parsed.kNavPathRequestGoalLocationOffset=compatibility.Value("kNavPathRequestGoalLocationOffset",16384);
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
