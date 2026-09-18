#pragma once

#include <cstdint>
#include <string_view>

namespace better_pose_profile {

// Resolved from HTGame.exe .text. The instruction is a RIP-relative load of
// GWorld; displacement is at byte 3 and the instruction is 7 bytes long.
inline constexpr std::string_view kGWorldPattern =
    "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01";
inline constexpr std::uint32_t kGWorldResolveOffset = 3;
inline constexpr std::uint32_t kGWorldInstructionSize = 7;

// GObjects registry contract. The pattern resolves the in-memory FUObjectArray
// root and the addend removes the 16-byte FRWScopeLock header. The object item
// layout is the same validated contract used by the internal UE5 object
// service.
inline constexpr std::string_view kGObjectsPattern =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8B 04 D1 C3 33 C0 48 8B 00 C3";
inline constexpr std::uint32_t kGObjectsResolveOffset = 3;
inline constexpr std::uint32_t kGObjectsInstructionSize = 7;
inline constexpr std::ptrdiff_t kGObjectsAddend = -16;
inline constexpr std::uint32_t kObjectItemsOffset = 16;
inline constexpr std::uint32_t kObjectMaxCountOffset = 32;
inline constexpr std::uint32_t kObjectCountOffset = 36;
inline constexpr std::uint32_t kObjectMaxChunksOffset = 40;
inline constexpr std::uint32_t kObjectNumChunksOffset = 44;
inline constexpr std::uint32_t kObjectChunkSize = 65536;
inline constexpr std::uint32_t kObjectItemStride = 24;
inline constexpr std::uint32_t kProcessEventVtableSlot = 0x4C;
// K2_SetRelativeTransform's FHitResult parameter extends the reflected call to
// 369 bytes on the active UE build. Keep the shared ProcessEvent scratch space
// large enough for that verified signature as well as the smaller functions.
inline constexpr std::size_t kMaximumUFunctionParameterBytes = 512;

// Prebuilt UFunction actions demonstrated through the plugin-local ProcessEvent
// bridge until the framework exposes a raw reflection invocation service.
inline constexpr std::string_view kFunctionPlayPath =
    "/Script/Engine.SkeletalMeshComponent.Play";
inline constexpr std::string_view kFunctionStopPath =
    "/Script/Engine.SkeletalMeshComponent.Stop";
inline constexpr std::string_view kFunctionSetPositionPath =
    "/Script/Engine.SkeletalMeshComponent.SetPosition";
inline constexpr std::string_view kFunctionRefreshAnimInstancePath =
    "/Script/HTGame.HTAbilityCharacter.RefreshAnimInstance";
inline constexpr std::string_view kFunctionGetBoneNamePath =
    "/Script/Engine.SkinnedMeshComponent.GetBoneName";
inline constexpr std::string_view kFunctionGetBoneIndexPath =
    "/Script/Engine.SkinnedMeshComponent.GetBoneIndex";
inline constexpr std::string_view kFunctionGetBoneTransformPath =
    "/Script/Engine.SkinnedMeshComponent.GetBoneTransform";
inline constexpr std::string_view kFunctionGetParentBonePath =
    "/Script/Engine.SkinnedMeshComponent.GetParentBone";
inline constexpr std::string_view kFunctionGetNumBonesPath =
    "/Script/Engine.SkinnedMeshComponent.GetNumBones";
inline constexpr std::string_view kFunctionSetForcedLodPath =
    "/Script/Engine.SkinnedMeshComponent.SetForcedLOD";
inline constexpr std::string_view kFunctionSetAnimationModePath =
    "/Script/Engine.SkeletalMeshComponent.SetAnimationMode";
inline constexpr std::string_view kFunctionActorAddComponentByClassPath =
    "/Script/Engine.Actor.AddComponentByClass";
inline constexpr std::string_view kFunctionSetSkeletalMeshAssetPath =
    "/Script/Engine.SkeletalMeshComponent.SetSkeletalMeshAsset";
inline constexpr std::string_view kFunctionPoseableSetBoneTransformByNamePath =
    "/Script/Engine.PoseableMeshComponent.SetBoneTransformByName";
inline constexpr std::string_view kFunctionSceneAttachComponentPath =
    "/Script/Engine.SceneComponent.K2_AttachToComponent";
inline constexpr std::string_view kFunctionSceneGetRelativeTransformPath =
    "/Script/Engine.SceneComponent.GetRelativeTransform";
inline constexpr std::string_view kFunctionSceneGetAttachSocketNamePath =
    "/Script/Engine.SceneComponent.GetAttachSocketName";
inline constexpr std::string_view kFunctionSceneGetComponentTransformPath =
    "/Script/Engine.SceneComponent.K2_GetComponentToWorld";
inline constexpr std::string_view kFunctionSceneSetRelativeTransformPath =
    "/Script/Engine.SceneComponent.K2_SetRelativeTransform";
inline constexpr std::string_view kFunctionSceneSetVisibilityPath =
    "/Script/Engine.SceneComponent.SetVisibility";
// Verified reflected signature: one byte ReturnValue at offset 0. The current
// skinned-component override also rejects components hidden in game.
inline constexpr std::string_view kFunctionSceneIsVisiblePath =
    "/Script/Engine.SceneComponent.IsVisible";
inline constexpr std::string_view kFunctionActorComponentDestroyPath =
    "/Script/Engine.ActorComponent.K2_DestroyComponent";
inline constexpr std::string_view kFunctionSetBoneLocationByNamePath =
    "/Script/Engine.PoseableMeshComponent.SetBoneLocationByName";
inline constexpr std::string_view kFunctionSetBoneRotationByNamePath =
    "/Script/Engine.PoseableMeshComponent.SetBoneRotationByName";

// UWorld -> GameInstance -> LocalPlayers[0] -> LocalPlayer -> Controller -> Pawn.
inline constexpr std::uint32_t kWorldGameInstanceOffset = 0x230;
inline constexpr std::uint32_t kGameInstanceLocalPlayersOffset = 0x38;
inline constexpr std::uint32_t kLocalPlayerControllerOffset = 0x30;
inline constexpr std::uint32_t kControllerPawnOffset = 0x308;

// ACharacter::Mesh.
inline constexpr std::uint32_t kCharacterMeshOffset = 0x348;
// ACharacter::AnimRootMotionTranslationScale.
inline constexpr std::uint32_t kCharacterAnimRootMotionScaleOffset = 0x468;

// Camera path. The view-point getter belongs to the free-camera plugin: its detour stub now
// sits at the function's first bytes, and the hook service refuses a second hook on the same
// target. What is available instead is the manager's cached POV -- the struct that getter
// copies its result from, and the same layout the active Profile names as
// cameraManager.location / cameraManager.rotation. Writing it composes with the other
// plugin's transparent passthrough (its detour calls the original and only replaces the
// result while its own free camera is on) instead of fighting it for the hook.
inline constexpr std::uint32_t kControllerCameraManagerOffset = 0x380;
// The manager's vtable slot holding the view-point getter. That getter is hooked by the
// free-camera plugin, but its body names the accessor this plugin hooks instead, so the slot is
// read to locate the getter's code and follow that call.
inline constexpr std::uint32_t kCameraViewPointVtableOffset = 0x850;
// Where the getter reads the two vectors out of the POV it copies: location at +0 and rotation
// at +0x18 (three doubles each, measured from the getter's own movups/movsd offsets).
inline constexpr std::uint32_t kCameraPovRotationOffset = 0x18;
// FMinimalViewInfo::FOV follows the two vectors, and reads 80 in game (the same value the camera
// cache reports at cameraManager.fov).
inline constexpr std::uint32_t kCameraPovFovOffset = 0x30;
// USkinnedMeshComponent's world FBoxSphereBounds {Origin, BoxExtent}: the active Profile names
// these as sceneComponent.boundsOrigin / boundsExtent, and reading them live gives a
// character-sized box (origin near the actor, extent.z = 84.7 for a 169 cm character), which is
// what the camera aims at.
inline constexpr std::uint32_t kCharacterBoundsOriginOffset = 0x118;
inline constexpr std::uint32_t kCharacterBoundsExtentOffset = 0x130;

// USkeletalMeshComponent animation state.
inline constexpr std::uint32_t kMeshAnimClassOffset = 0x930;
inline constexpr std::uint32_t kMeshAnimScriptInstanceOffset = 0x938;
inline constexpr std::uint32_t kMeshCachedBoneSpaceTransformsOffset = 0x9E8;
inline constexpr std::uint32_t kMeshCachedComponentSpaceTransformsOffset = 0x9F8;
// Two component-space buffers, not a local/component pair. The live bone getter
// and rendering-data packer both select 0x628 + 0x10 * CurrentReadIndex.
// Cached* above remains a read-only fallback. The legacy RuntimeState field
// names bone_space_data/component_space_data refer to buffer 0/1 respectively.
inline constexpr std::uint32_t kMeshComponentSpaceBuffer0Offset = 0x628;
inline constexpr std::uint32_t kMeshComponentSpaceBuffer1Offset = 0x638;
inline constexpr std::uint32_t kMeshLocalSpaceTransformsOffset = 0x968;
inline constexpr std::uint32_t kMeshGlobalAnimRateScaleOffset = 0xAA8;
inline constexpr std::uint32_t kMeshForcedLodModelOffset = 0x7A0;
inline constexpr std::uint32_t kMeshAnimationModeOffset = 0xAAF;
inline constexpr std::uint32_t kMeshForceMeshObjectUpdateOffset = 0x7FA;
inline constexpr std::uint32_t kMeshForceMeshObjectUpdateBit = 6;
inline constexpr std::uint32_t kMeshAnimationFlagsOffset = 0xAC0;
// Do not call the unexposed component virtuals from the plugin. The current
// build's submission path was inspected read-only, but invoking its slot is not
// part of the validated plugin contract.
inline constexpr std::uint32_t kSkeletalMeshTickVtableSlot = 128;

// EAnimationMode values relevant to the pose override. Switching the
// SkeletalMeshComponent to AnimationCustomMode stops AnimInstance/animation
// evaluation from rewriting BoneSpaceTransforms every frame, which is required
// for direct pose-buffer edits to survive until the render thread consumes them.
inline constexpr std::uint8_t kAnimationModeSingleNode = 0;
inline constexpr std::uint8_t kAnimationModeCustom = 2;

// UAnimInstance animation-update state. Clearing bit 0 of the byte at +0x31
// forces this AnimInstance to evaluate on the UE Game Thread instead of the
// worker task, which is what populates USkeletalMeshComponent's cached pose
// arrays used by the best-effort bone pose override.
inline constexpr std::uint32_t kAnimInstanceUseMultiThreadedUpdateOffset = 0x31;
inline constexpr std::uint32_t kAnimInstanceUseMultiThreadedUpdateBit = 0;

// bPauseAnims / bEnableAnimation bit positions inside the byte at +0xAC0.
inline constexpr std::uint32_t kAnimationFlagPauseAnimsBit = 4;
inline constexpr std::uint32_t kAnimationFlagEnableAnimationBit = 5;

// FTransform layout used by both cached pose arrays:
// FQuat (4x double), FVector translation (3x double), padding, FVector scale.
inline constexpr std::uint32_t kTransformSize = 0x60;
inline constexpr std::uint32_t kTransformRotationOffset = 0x00;
inline constexpr std::uint32_t kTransformTranslationOffset = 0x20;
inline constexpr std::uint32_t kTransformScaleOffset = 0x40;

// UE TArray header embedded directly in USkeletalMeshComponent.
inline constexpr std::uint32_t kArrayDataOffset = 0x00;
inline constexpr std::uint32_t kArrayCountOffset = 0x08;
inline constexpr std::uint32_t kArrayCapacityOffset = 0x0C;

// Render-side diagnostics and bounds.
inline constexpr std::size_t kMaximumStatusBytes = 256;
inline constexpr std::uint32_t kMaximumBoneIndex = 8191;
inline constexpr std::size_t kMaximumPoseProbeBones = 8;

}  // namespace better_pose_profile
