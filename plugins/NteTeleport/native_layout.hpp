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
    bool tracked_target_available{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host, std::string* diagnostic = nullptr) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "nte-teleport-v1"}, {}, false, "layout", diagnostic, false)) return false;
        Layout parsed;
        parsed.kWorldGameInstanceOffset=compatibility.Value("kWorldGameInstanceOffset",16384);
        parsed.kGameInstanceLocalPlayersOffset=compatibility.Value("kGameInstanceLocalPlayersOffset",16384);
        parsed.kLocalPlayerControllerOffset=compatibility.Value("kLocalPlayerControllerOffset",16384);
        parsed.kControllerPlayerStateOffset=compatibility.Value("kControllerPlayerStateOffset",16384);
        parsed.kNavPathActorLastRequestParamOffset=compatibility.Value("kNavPathActorLastRequestParamOffset",16384);
        parsed.kNavPathRequestGoalLocationOffset=compatibility.Value("kNavPathRequestGoalLocationOffset",16384);
        if (const auto* candidate = compatibility.Selected("layout")) {
            const auto offset = candidate->values.find("kPlayerStateCurNavigationPathEffectOffset");
            if (offset != candidate->values.end() && offset->second <= 16384) {
                parsed.kPlayerStateCurNavigationPathEffectOffset = offset->second;
                parsed.tracked_target_available = true;
            }
        }
        native_layout=parsed;
        return true;
    } catch (const std::exception& error) {
        if (diagnostic != nullptr) {
            try { *diagnostic = error.what(); } catch (...) {}
        }
        return false;
    } catch (...) {
        if (diagnostic != nullptr) {
            try { *diagnostic = "unknown compatibility layout failure"; } catch (...) {}
        }
        return false;
    }
}
}
