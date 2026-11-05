#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cstdint>
#include <string_view>

namespace camera_tools_profile {
struct Layout {
    std::uint32_t kWorldGameInstanceOffset{};
    std::uint32_t kGameInstanceLocalPlayersOffset{};
    std::uint32_t kLocalPlayerControllerOffset{};
    std::uint32_t kControllerCameraManagerOffset{};
    std::uint32_t kControllerPlayerInputOffset{};
    std::uint32_t kCameraViewPointVtableOffset{};
    std::uint32_t kCameraPovAccessorVtableOffset{};
    std::uint32_t kCameraPovFovOffset{};
    std::uint32_t kPlayerInputKeyVtableOffset{};
    std::uint32_t kInputKeyEventArgsKeyOffset{};
    std::uint32_t kNamePoolBlocksOffset{};
    std::uint32_t kNameBlockBits{};
    std::uint32_t kNameEntryStride{};
    std::uint32_t kNameLengthShift{};
    std::uint32_t kMouseXNameId{};
    std::uint32_t kMouseYNameId{};
    std::uint32_t kMouse2DNameId{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
// Optional: a missing/unsupported rotation path must not disable the existing tools.
// The 1.4b and 1.5a native bodies share the rotation ABI and vtable slot, but
// have different prologues. Resolve their separate Profile candidates uniquely.
inline std::uint32_t process_view_rotation_slot{};
inline constexpr std::string_view kProcessViewRotation = "processViewRotation";
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "camera-tools-v1"})) return false;
        Layout parsed;
        parsed.kWorldGameInstanceOffset = compatibility.Value("kWorldGameInstanceOffset", 16384);
        parsed.kGameInstanceLocalPlayersOffset = compatibility.Value("kGameInstanceLocalPlayersOffset", 16384);
        parsed.kLocalPlayerControllerOffset = compatibility.Value("kLocalPlayerControllerOffset", 16384);
        parsed.kControllerCameraManagerOffset = compatibility.Value("kControllerCameraManagerOffset", 16384);
        parsed.kControllerPlayerInputOffset = compatibility.Value("kControllerPlayerInputOffset", 16384);
        parsed.kCameraViewPointVtableOffset = compatibility.Value("kCameraViewPointVtableOffset", 16384);
        parsed.kCameraPovAccessorVtableOffset = compatibility.Value("kCameraPovAccessorVtableOffset", 16384);
        parsed.kCameraPovFovOffset = compatibility.Value("kCameraPovFovOffset", 16384);
        parsed.kPlayerInputKeyVtableOffset = compatibility.Value("kPlayerInputKeyVtableOffset", 16384);
        parsed.kInputKeyEventArgsKeyOffset = compatibility.Value("kInputKeyEventArgsKeyOffset", 16384);
        parsed.kNamePoolBlocksOffset = compatibility.Value("kNamePoolBlocksOffset", 16384);
        parsed.kNameBlockBits = compatibility.Value("kNameBlockBits", 16384);
        parsed.kNameEntryStride = compatibility.Value("kNameEntryStride", 16384);
        parsed.kNameLengthShift = compatibility.Value("kNameLengthShift", 16384);
        parsed.kMouseXNameId = compatibility.Value("kMouseXNameId", 65536);
        parsed.kMouseYNameId = compatibility.Value("kMouseYNameId", 65536);
        parsed.kMouse2DNameId = compatibility.Value("kMouse2DNameId", 65536);
        native_layout = parsed;
        process_view_rotation_slot = 0;
        if (const auto* candidate = compatibility.Selected(kProcessViewRotation)) {
            const auto slot = candidate->values.find("vtableSlot");
            if (slot != candidate->values.end() && slot->second != 0 &&
                slot->second <= 16384 && slot->second % sizeof(std::uintptr_t) == 0)
                process_view_rotation_slot = slot->second;
        }
        return true;
    } catch (...) { return false; }
}


// These contracts match the currently validated NTE camera-manager path used
// by FreeCamera. The active manager's vtable must match the resolved hook target
// before the returned camera location is adjusted.
inline constexpr std::string_view kGWorldPattern = "kGWorldPattern";

inline constexpr std::string_view kCameraViewPointPattern = "kCameraViewPointPattern";

inline constexpr std::string_view kFNamePoolPattern = "kFNamePoolPattern";

// One 48-byte signature written across three source lines; the compiler concatenates
// the literals, so the host always resolves the whole instruction run. Do not read the
// trailing `FF 90 80 01 00 00 48 85 C0` as an anchor on its own: that fragment alone
// hits the scanner's match cap (it is just `call [rax+0x180]; test rax, rax`), which is
// why a per-line extraction reports it as ambiguous while the joined pattern resolves
// to exactly one site.
inline constexpr std::string_view kPlayerInputKeyPattern = "kPlayerInputKeyPattern";

inline constexpr std::uint32_t kGWorldResolveOffset = 3;
inline constexpr std::uint32_t kGWorldInstructionSize = 7;
inline constexpr std::uint32_t kFNamePoolResolveOffset = 3;
inline constexpr std::uint32_t kFNamePoolInstructionSize = 7;

// The POV the view-point getter copies its result from. The getter's own body reaches it through
// this vtable slot (the `call [rax+A0 07 00 00]` inside kCameraViewPointPattern), and the slot
// holds a `lea rax,[rcx+disp32]; ret` accessor naming the manager's cached FMinimalViewInfo.
// FOV follows that struct's location and rotation vectors and is the one part of the view the
// getter's out-parameters cannot carry.

} // namespace camera_tools_profile
