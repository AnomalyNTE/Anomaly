#pragma once

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

// RIP-relative load of GWorld. Displacement is at byte 3, instruction is 7 bytes.
inline constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
inline constexpr std::uint32_t kGWorldResolveOffset = 3;
inline constexpr std::uint32_t kGWorldInstructionSize = 7;

// UWorld -> UGameInstance -> LocalPlayers[0] -> ULocalPlayer -> APlayerController.
inline constexpr std::uint32_t kWorldGameInstanceOffset = 0x230;
inline constexpr std::uint32_t kGameInstanceLocalPlayersOffset = 0x38;
inline constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;

// APlayerController::Pawn.
inline constexpr std::uint32_t kControllerPawnOffset = 0x308;
// ACharacter::Mesh, the component the plugin scales.
//
// The capsule (0x0358) is deliberately not a target: it is the component the game's own
// gameplay-scale path drives (`AHTAbilityCharacter::GameplayScale`, `GetCapsuleGameplayScale`),
// and a scale written there was measured to be back at {1,1,1} within a frame.
inline constexpr std::uint32_t kCharacterMeshOffset = 0x348;

// USceneComponent::RelativeScale3D, an FVector of three doubles.
inline constexpr std::uint32_t kSceneComponentRelativeScale3D = 0x180;
inline constexpr std::uint32_t kVectorSize = 0x18;

// USceneComponent::bComponentToWorldUpdated (0x01B0, bit 0). The engine's
// SetRelativeScale3D writes RelativeScale3D and then clears this bit, which is what makes
// the next tick run UpdateComponentToWorld and push the new scale down to the attached
// children. Writing the vector alone leaves the cached transform in place, so the resize
// never reaches the renderer: clearing the bit is the part that makes it visible.
inline constexpr std::uint32_t kSceneComponentToWorldUpdated = 0x1B0;
inline constexpr std::uint32_t kSceneComponentToWorldUpdatedBit = 0;

// Bounds a scale factor to what the component can actually hold. RelativeScale3D is a
// Vector3f, so the factor's range is a float's range: anything outside it is rounded to a
// float limit or to infinity on the way in, which would make the transform meaningless.
// The low end stays positive -- a zero or negative scale collapses or mirrors the component.
// The UI passes these to its own range check and clamps the typed value to them.
inline constexpr double kMinimumScaleFactor = static_cast<double>(FLT_MIN);
inline constexpr double kMaximumScaleFactor = static_cast<double>(FLT_MAX);
inline constexpr double kScaleFactorEpsilon = 1e-4;

}  // namespace model_scale_profile
