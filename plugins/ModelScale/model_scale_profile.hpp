#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cfloat>
#include <cstdint>
#include <string_view>

// Offsets this plugin reads and writes on the live NTE build.
//
// Every number here is taken from the Dumper-7 SDK for this exact build,
// `5.6.1-0+UE5-HT-1.4a`, and cross-checked against the active Profile where the two
// describe the same field. The SDK is the source of truth for class layout; the Profile
// supplies the process-global entry point the SDK cannot name.
//
//   * Process globals: `ue5.GWorld`. The Profile publishes the slot address, and a
//     full-image scan of `kGWorldPattern` finds exactly one match, so the plugin can
//     resolve it at runtime instead of trusting a recorded address.
//   * `UWorld -> UGameInstance -> LocalPlayers -> ULocalPlayer -> APlayerController ->
//     APawn` are the Profile's `world.gameInstance`, `gameInstance.localPlayers`,
//     `localPlayer.controller` and `controller.pawn`.
//   * `ACharacter::Mesh` (0x0348) and `ACharacter::CapsuleComponent` (0x0358) come from
//     `Engine_classes.hpp`, as do `USceneComponent::RelativeScale3D` (0x0180) and the
//     `bComponentToWorldUpdated` bit (0x01B0 bit 0) that the engine's own
//     `SetRelativeScale3D` clears to schedule the transform rebuild.
namespace model_scale_profile {
struct Layout {
    std::uint32_t kWorldGameInstanceOffset{};
    std::uint32_t kGameInstanceLocalPlayersOffset{};
    std::uint32_t kLocalPlayerControllerOffset{};
    std::uint32_t kControllerPawnOffset{};
    std::uint32_t kCharacterMeshOffset{};
    std::uint32_t kSceneComponentRelativeScale3D{};
    std::uint32_t kSceneComponentToWorldUpdated{};
    std::uint32_t kSceneComponentToWorldUpdatedBit{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "model-scale-v1"})) return false;
        Layout parsed;
        parsed.kWorldGameInstanceOffset = compatibility.Value("kWorldGameInstanceOffset", 16384);
        parsed.kGameInstanceLocalPlayersOffset = compatibility.Value("kGameInstanceLocalPlayersOffset", 16384);
        parsed.kLocalPlayerControllerOffset = compatibility.Value("kLocalPlayerControllerOffset", 16384);
        parsed.kControllerPawnOffset = compatibility.Value("kControllerPawnOffset", 16384);
        parsed.kCharacterMeshOffset = compatibility.Value("kCharacterMeshOffset", 16384);
        parsed.kSceneComponentRelativeScale3D = compatibility.Value("kSceneComponentRelativeScale3D", 16384);
        parsed.kSceneComponentToWorldUpdated = compatibility.Value("kSceneComponentToWorldUpdated", 16384);
        parsed.kSceneComponentToWorldUpdatedBit = compatibility.Value("kSceneComponentToWorldUpdatedBit", 16384);
        native_layout = parsed;
        return true;
    } catch (...) { return false; }
}


// RIP-relative load of GWorld. Displacement is at byte 3, instruction is 7 bytes.
inline constexpr std::string_view kGWorldPattern = "kGWorldPattern";
inline constexpr std::uint32_t kGWorldResolveOffset = 3;
inline constexpr std::uint32_t kGWorldInstructionSize = 7;

// UWorld -> UGameInstance -> LocalPlayers[0] -> ULocalPlayer -> APlayerController.

// APlayerController::Pawn.

// ACharacter::Mesh, the component the plugin scales.
//
// The capsule (0x0358) is deliberately not a target: it is the component the game's own
// gameplay-scale path drives (`AHTAbilityCharacter::GameplayScale`, `GetCapsuleGameplayScale`),
// and a scale written there was measured to be back at {1,1,1} within a frame.


// USceneComponent::RelativeScale3D, an FVector of three doubles.


// USceneComponent::bComponentToWorldUpdated (0x01B0, bit 0). The engine's
// SetRelativeScale3D writes RelativeScale3D and then clears this bit, which is what makes
// the next tick run UpdateComponentToWorld and push the new scale down to the attached
// children. Writing the vector alone leaves the cached transform in place, so the resize
// never reaches the renderer: clearing the bit is the part that makes it visible.

// Bounds a scale factor to what the component can actually hold. RelativeScale3D is a
// Vector3f, so the factor's range is a float's range: anything outside it is rounded to a
// float limit or to infinity on the way in, which would make the transform meaningless.
// The low end stays positive -- a zero or negative scale collapses or mirrors the component.
// The UI passes these to its own range check and clamps the typed value to them.
inline constexpr double kMinimumScaleFactor = static_cast<double>(FLT_MIN);
inline constexpr double kMaximumScaleFactor = static_cast<double>(FLT_MAX);
inline constexpr double kScaleFactorEpsilon = 1e-4;

}  // namespace model_scale_profile
