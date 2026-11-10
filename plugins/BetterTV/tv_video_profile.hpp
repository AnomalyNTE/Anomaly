#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cstdint>
#include <string_view>

namespace better_tv_profile {

// Values the plugin needs from HTGame.exe's live layout. Everything that is a
// game offset or a function signature lives in profile/compatibility.profile.json;
// this header only validates and caches the snapshot.
struct Layout {
    std::uint32_t kObjectClassOffset{};
    std::uint32_t kObjectNameOffset{};
    std::uint32_t kObjectOuterOffset{};
    std::uint32_t kObjectIndexOffset{};
    std::uint32_t kFunctionParmsSizeOffset{};
    std::uint32_t kStructPropertyLinkOffset{};
    std::uint32_t kStructSuperOffset{};
    std::uint32_t kPropertyNameOffset{};
    std::uint32_t kPropertyNextOffset{};
    std::uint32_t kPropertyValueOffset{};
    std::uint32_t kPropertySizeOffset{};
    std::uint32_t kFieldClassOffset{};
    std::uint32_t kFieldClassNameOffset{};
    std::uint32_t kStructPropertyStructOffset{};
    std::uint32_t kBoolPropertyLayoutOffset{};
    std::uint32_t kFStringNumOffset{};
    std::uint32_t kFStringMaxOffset{};
    // FFrame::Node (the UFunction being executed) in a native UFunction thunk.
    std::uint32_t kFrameNodeOffset{};
    // UFileMediaSource::FilePath, the final path handed to the media framework.
    std::uint32_t kMediaSourcePathOffset{};
};

inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;

// Semantic contract for the residence TV video source. It describes what the
// plugin supports, never a build identity.
inline constexpr anomaly::sdk::ProfileContract kContract{"nte", "HTGame.exe", ".text",
                                                        "better-tv-video-v1"};

inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json", kContract)) return false;
        Layout parsed;
        parsed.kObjectClassOffset = compatibility.Value("kObjectClassOffset", 1024, 4);
        parsed.kObjectNameOffset = compatibility.Value("kObjectNameOffset", 1024, 4);
        parsed.kObjectOuterOffset = compatibility.Value("kObjectOuterOffset", 1024, 8);
        parsed.kObjectIndexOffset = compatibility.Value("kObjectIndexOffset", 1024, 4);
        parsed.kFunctionParmsSizeOffset = compatibility.Value("kFunctionParmsSizeOffset", 1024, 2);
        parsed.kStructPropertyLinkOffset = compatibility.Value("kStructPropertyLinkOffset", 1024, 8);
        parsed.kStructSuperOffset = compatibility.Value("kStructSuperOffset", 1024, 8);
        parsed.kPropertyNameOffset = compatibility.Value("kPropertyNameOffset", 1024, 4);
        parsed.kPropertyNextOffset = compatibility.Value("kPropertyNextOffset", 1024, 8);
        parsed.kPropertyValueOffset = compatibility.Value("kPropertyValueOffset", 1024, 4);
        parsed.kPropertySizeOffset = compatibility.Value("kPropertySizeOffset", 1024, 4);
        parsed.kFieldClassOffset = compatibility.Value("kFieldClassOffset", 256, 8);
        parsed.kFieldClassNameOffset = compatibility.Value("kFieldClassNameOffset", 256, 4);
        parsed.kStructPropertyStructOffset = compatibility.Value("kStructPropertyStructOffset", 256, 8);
        parsed.kBoolPropertyLayoutOffset = compatibility.Value("kBoolPropertyLayoutOffset", 256, 1);
        parsed.kFStringNumOffset = compatibility.Value("kFStringNumOffset", 64, 4);
        parsed.kFStringMaxOffset = compatibility.Value("kFStringMaxOffset", 64, 4);
        parsed.kFrameNodeOffset = compatibility.Value("kFrameNodeOffset", 256, 1);
        parsed.kMediaSourcePathOffset = compatibility.Value("kMediaSourcePathOffset", 4096, 8);
        native_layout = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] inline std::uintptr_t Address(const std::string_view symbol) noexcept {
    return compatibility.Address(symbol);
}

}  // namespace better_tv_profile
