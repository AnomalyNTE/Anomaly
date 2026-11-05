#pragma once
#include <anomaly/sdk/profile.hpp>
namespace timeaccel_profile {
struct Layout {
    std::uintptr_t kClassPrivateOffset{};
    std::uintptr_t kNamePrivateOffset{};
    std::uintptr_t kOuterPrivateOffset{};
    std::uintptr_t kLevelWorldSettingsOffset{};
    std::uintptr_t kWorldDilationOffset{};
    std::uintptr_t kEngineTimeDilationOffset{};
    std::uintptr_t kMinDilationOffset{};
    std::uintptr_t kMaxDilationOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host,"compatibility.profile.json",{"nte", "HTGame.exe", ".text", "time-accel-v1"}, {}, false, "layout")) return false;
        Layout parsed;
        parsed.kClassPrivateOffset=compatibility.Value("kClassPrivateOffset",16384);
        parsed.kNamePrivateOffset=compatibility.Value("kNamePrivateOffset",16384);
        parsed.kOuterPrivateOffset=compatibility.Value("kOuterPrivateOffset",16384);
        parsed.kLevelWorldSettingsOffset=compatibility.Value("kLevelWorldSettingsOffset",16384);
        parsed.kWorldDilationOffset=compatibility.Value("kWorldDilationOffset",16384);
        parsed.kEngineTimeDilationOffset=compatibility.Value("kEngineTimeDilationOffset",16384);
        parsed.kMinDilationOffset=compatibility.Value("kMinDilationOffset",16384);
        parsed.kMaxDilationOffset=compatibility.Value("kMaxDilationOffset",16384);
        native_layout=parsed;
        return true;
    } catch (...) { return false; }
}
}
