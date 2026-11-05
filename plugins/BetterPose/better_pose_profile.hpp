#pragma once
#include <anomaly/sdk/profile.hpp>

#include <cstdint>
#include <string_view>

namespace better_pose_profile {
struct TransformContract {
  double rotation[4]{};
  double translation[3]{};
  double padding{};
  double scale[3]{1.0, 1.0, 1.0};
  double tail_padding{};
};
static_assert(sizeof(TransformContract) == 96);

struct Layout {
    std::uint32_t kTransformSize{};
    std::uint32_t kMeshSkeletalMeshOffset{};
    std::uint32_t kMeshMorphTargetsOffset{};
    std::uint32_t kFunctionParmsSizeOffset{};

    std::uint32_t kObjectClassOffset{};
    std::uint32_t kObjectNameOffset{};
    std::uint32_t kObjectOuterOffset{};
    std::uint32_t kMeshAttachParentOffset{};

    std::ptrdiff_t kGObjectsAddend{};
    std::uint32_t kObjectItemsOffset{};
    std::uint32_t kObjectMaxCountOffset{};
    std::uint32_t kObjectCountOffset{};
    std::uint32_t kObjectMaxChunksOffset{};
    std::uint32_t kObjectNumChunksOffset{};
    std::uint32_t kObjectChunkSize{};
    std::uint32_t kObjectItemStride{};
    std::uint32_t kProcessEventVtableSlot{};
    std::uint32_t kWorldGameInstanceOffset{};
    std::uint32_t kGameInstanceLocalPlayersOffset{};
    std::uint32_t kLocalPlayerControllerOffset{};
    std::uint32_t kControllerPawnOffset{};
    std::uint32_t kCharacterMeshOffset{};
    std::uint32_t kCharacterAnimRootMotionScaleOffset{};
    std::uint32_t kControllerCameraManagerOffset{};
    std::uint32_t kCameraViewPointVtableOffset{};
    std::uint32_t kCameraPovRotationOffset{};
    std::uint32_t kCameraPovFovOffset{};
    std::uint32_t kCharacterBoundsOriginOffset{};
    std::uint32_t kCharacterBoundsExtentOffset{};
    std::uint32_t kMeshAnimClassOffset{};
    std::uint32_t kMeshAnimScriptInstanceOffset{};
    std::uint32_t kMeshCachedBoneSpaceTransformsOffset{};
    std::uint32_t kMeshCachedComponentSpaceTransformsOffset{};
    std::uint32_t kMeshComponentSpaceBuffer0Offset{};
    std::uint32_t kMeshComponentSpaceBuffer1Offset{};
    std::uint32_t kMeshLocalSpaceTransformsOffset{};
    std::uint32_t kMeshGlobalAnimRateScaleOffset{};
    std::uint32_t kMeshForcedLodModelOffset{};
    std::uint32_t kMeshVisibilityBasedAnimTickOptionOffset{};
    std::uint32_t kMeshAnimationModeOffset{};
    std::uint32_t kMeshForceMeshObjectUpdateOffset{};
    std::uint32_t kMeshForceMeshObjectUpdateBit{};
    std::uint32_t kMeshAnimationFlagsOffset{};
    std::uint32_t kSkeletalMeshTickVtableSlot{};
    std::uint32_t kUStructChildPropertiesOffset{};
    std::uint32_t kFFieldNextOffset{};
    std::uint32_t kFFieldNameOffset{};
    std::uint32_t kFPropertyElementSizeOffset{};
    std::uint32_t kFPropertyOffsetInternalOffset{};
    std::uint32_t kKawaiiRootBoneOffset{};
    std::uint32_t kKawaiiPhysicsSettingsOffset{};
    std::uint32_t kKawaiiTargetFrameRateOffset{};
    std::uint32_t kKawaiiWarmUpFramesOffset{};
    std::uint32_t kKawaiiSphericalLimitsOffset{};
    std::uint32_t kKawaiiCapsuleLimitsOffset{};
    std::uint32_t kKawaiiBoxLimitsOffset{};
    std::uint32_t kKawaiiPlanarLimitsOffset{};
    std::uint32_t kKawaiiGravityOffset{};
    std::uint32_t kKawaiiWindEnabledOffset{};
    std::uint32_t kKawaiiWindScaleOffset{};
    std::uint32_t kKawaiiModifyBonesOffset{};
    std::uint32_t kKawaiiModifyBoneSize{};
    std::uint32_t kKawaiiModifyBoneNameOffset{};
    std::uint32_t kKawaiiModifyBoneSettingsOffset{};
    std::uint32_t kKawaiiModifyBoneDummyOffset{};
    std::uint32_t kKawaiiModifyBoneSkipOffset{};
    std::uint32_t kKawaiiLimitDrivingBoneOffset{};
    std::uint32_t kKawaiiLimitCachedBoneIndexOffset{};
    std::uint32_t kKawaiiLimitOffsetLocationOffset{};
    std::uint32_t kKawaiiLimitOffsetRotationOffset{};
    std::uint32_t kKawaiiLimitEnabledOffset{};
    std::uint32_t kKawaiiLimitRadiusOffset{};
    std::uint32_t kKawaiiLimitLengthOffset{};
    std::uint32_t kKawaiiLimitSphereRadiusOffset{};
    std::uint32_t kKawaiiCapsuleLimitSize{};
    std::uint32_t kAnimInstanceUseMultiThreadedUpdateOffset{};
    std::uint32_t kAnimInstanceUseMultiThreadedUpdateBit{};
    std::uint32_t kAnimationFlagPauseAnimsBit{};
    std::uint32_t kAnimationFlagEnableAnimationBit{};
    std::uint32_t kTransformRotationOffset{};
    std::uint32_t kTransformTranslationOffset{};
    std::uint32_t kTransformScaleOffset{};
    std::uint32_t kArrayDataOffset{};
    std::uint32_t kArrayCountOffset{};
    std::uint32_t kArrayCapacityOffset{};
};
inline Layout native_layout;
inline anomaly::sdk::ProfileSnapshot compatibility;
inline bool LoadCompatibility(const AnomalyHostApiV1* host) noexcept {
    try {
        if (!compatibility.Load(host, "compatibility.profile.json", {"nte", "HTGame.exe", ".text", "better-pose-v1"}, {"ProcessEvent.entry"})) return false;
        Layout parsed;
        parsed.kTransformSize=compatibility.Value("kTransformSize",16384,alignof(double));
        parsed.kMeshSkeletalMeshOffset=compatibility.Value("kMeshSkeletalMeshOffset",16384);
        parsed.kMeshMorphTargetsOffset=compatibility.Value("kMeshMorphTargetsOffset",16384);
        parsed.kFunctionParmsSizeOffset=compatibility.Value("kFunctionParmsSizeOffset",16384);

        parsed.kObjectClassOffset = compatibility.Value("kObjectClassOffset", 65536, 8);
        parsed.kObjectNameOffset = compatibility.Value("kObjectNameOffset", 65536, 8);
        parsed.kObjectOuterOffset = compatibility.Value("kObjectOuterOffset", 65536, 8);
        parsed.kMeshAttachParentOffset = compatibility.Value("kMeshAttachParentOffset", 65536, 8);

        parsed.kGObjectsAddend = -static_cast<std::ptrdiff_t>(compatibility.Value("kGObjectsAddendMagnitude", 16384));
        parsed.kObjectItemsOffset = compatibility.Value("kObjectItemsOffset", 16384);
        parsed.kObjectMaxCountOffset = compatibility.Value("kObjectMaxCountOffset", 16384);
        parsed.kObjectCountOffset = compatibility.Value("kObjectCountOffset", 16384);
        parsed.kObjectMaxChunksOffset = compatibility.Value("kObjectMaxChunksOffset", 65536);
        parsed.kObjectNumChunksOffset = compatibility.Value("kObjectNumChunksOffset", 65536);
        parsed.kObjectChunkSize = compatibility.Value("kObjectChunkSize", 65536);
        parsed.kObjectItemStride = compatibility.Value("kObjectItemStride", 16384);
        parsed.kProcessEventVtableSlot = compatibility.Value("kProcessEventVtableSlot", 16384);
        parsed.kWorldGameInstanceOffset = compatibility.Value("kWorldGameInstanceOffset", 16384);
        parsed.kGameInstanceLocalPlayersOffset = compatibility.Value("kGameInstanceLocalPlayersOffset", 16384);
        parsed.kLocalPlayerControllerOffset = compatibility.Value("kLocalPlayerControllerOffset", 16384);
        parsed.kControllerPawnOffset = compatibility.Value("kControllerPawnOffset", 16384);
        parsed.kCharacterMeshOffset = compatibility.Value("kCharacterMeshOffset", 16384);
        parsed.kCharacterAnimRootMotionScaleOffset = compatibility.Value("kCharacterAnimRootMotionScaleOffset", 16384);
        parsed.kControllerCameraManagerOffset = compatibility.Value("kControllerCameraManagerOffset", 16384);
        parsed.kCameraViewPointVtableOffset = compatibility.Value("kCameraViewPointVtableOffset", 16384);
        parsed.kCameraPovRotationOffset = compatibility.Value("kCameraPovRotationOffset", 16384);
        parsed.kCameraPovFovOffset = compatibility.Value("kCameraPovFovOffset", 16384);
        parsed.kCharacterBoundsOriginOffset = compatibility.Value("kCharacterBoundsOriginOffset", 16384);
        parsed.kCharacterBoundsExtentOffset = compatibility.Value("kCharacterBoundsExtentOffset", 16384);
        parsed.kMeshAnimClassOffset = compatibility.Value("kMeshAnimClassOffset", 16384);
        parsed.kMeshAnimScriptInstanceOffset = compatibility.Value("kMeshAnimScriptInstanceOffset", 16384);
        parsed.kMeshCachedBoneSpaceTransformsOffset = compatibility.Value("kMeshCachedBoneSpaceTransformsOffset", 16384);
        parsed.kMeshCachedComponentSpaceTransformsOffset = compatibility.Value("kMeshCachedComponentSpaceTransformsOffset", 16384);
        parsed.kMeshComponentSpaceBuffer0Offset = compatibility.Value("kMeshComponentSpaceBuffer0Offset", 16384);
        parsed.kMeshComponentSpaceBuffer1Offset = compatibility.Value("kMeshComponentSpaceBuffer1Offset", 16384);
        parsed.kMeshLocalSpaceTransformsOffset = compatibility.Value("kMeshLocalSpaceTransformsOffset", 16384);
        parsed.kMeshGlobalAnimRateScaleOffset = compatibility.Value("kMeshGlobalAnimRateScaleOffset", 16384);
        parsed.kMeshForcedLodModelOffset = compatibility.Value("kMeshForcedLodModelOffset", 16384);
        parsed.kMeshVisibilityBasedAnimTickOptionOffset = compatibility.Value("kMeshVisibilityBasedAnimTickOptionOffset", 16384);
        parsed.kMeshAnimationModeOffset = compatibility.Value("kMeshAnimationModeOffset", 16384);
        parsed.kMeshForceMeshObjectUpdateOffset = compatibility.Value("kMeshForceMeshObjectUpdateOffset", 16384);
        parsed.kMeshForceMeshObjectUpdateBit = compatibility.Value("kMeshForceMeshObjectUpdateBit", 16384);
        parsed.kMeshAnimationFlagsOffset = compatibility.Value("kMeshAnimationFlagsOffset", 16384);
        parsed.kSkeletalMeshTickVtableSlot = compatibility.Value("kSkeletalMeshTickVtableSlot", 16384);
        parsed.kUStructChildPropertiesOffset = compatibility.Value("kUStructChildPropertiesOffset", 16384);
        parsed.kFFieldNextOffset = compatibility.Value("kFFieldNextOffset", 16384);
        parsed.kFFieldNameOffset = compatibility.Value("kFFieldNameOffset", 16384);
        parsed.kFPropertyElementSizeOffset = compatibility.Value("kFPropertyElementSizeOffset", 16384);
        parsed.kFPropertyOffsetInternalOffset = compatibility.Value("kFPropertyOffsetInternalOffset", 16384);
        parsed.kKawaiiRootBoneOffset = compatibility.Value("kKawaiiRootBoneOffset", 16384);
        parsed.kKawaiiPhysicsSettingsOffset = compatibility.Value("kKawaiiPhysicsSettingsOffset", 16384);
        parsed.kKawaiiTargetFrameRateOffset = compatibility.Value("kKawaiiTargetFrameRateOffset", 16384);
        parsed.kKawaiiWarmUpFramesOffset = compatibility.Value("kKawaiiWarmUpFramesOffset", 16384);
        parsed.kKawaiiSphericalLimitsOffset = compatibility.Value("kKawaiiSphericalLimitsOffset", 16384);
        parsed.kKawaiiCapsuleLimitsOffset = compatibility.Value("kKawaiiCapsuleLimitsOffset", 16384);
        parsed.kKawaiiBoxLimitsOffset = compatibility.Value("kKawaiiBoxLimitsOffset", 16384);
        parsed.kKawaiiPlanarLimitsOffset = compatibility.Value("kKawaiiPlanarLimitsOffset", 16384);
        parsed.kKawaiiGravityOffset = compatibility.Value("kKawaiiGravityOffset", 16384);
        parsed.kKawaiiWindEnabledOffset = compatibility.Value("kKawaiiWindEnabledOffset", 16384);
        parsed.kKawaiiWindScaleOffset = compatibility.Value("kKawaiiWindScaleOffset", 16384);
        parsed.kKawaiiModifyBonesOffset = compatibility.Value("kKawaiiModifyBonesOffset", 16384);
        parsed.kKawaiiModifyBoneSize = compatibility.Value("kKawaiiModifyBoneSize", 16384);
        parsed.kKawaiiModifyBoneNameOffset = compatibility.Value("kKawaiiModifyBoneNameOffset", 16384);
        parsed.kKawaiiModifyBoneSettingsOffset = compatibility.Value("kKawaiiModifyBoneSettingsOffset", 16384);
        parsed.kKawaiiModifyBoneDummyOffset = compatibility.Value("kKawaiiModifyBoneDummyOffset", 16384);
        parsed.kKawaiiModifyBoneSkipOffset = compatibility.Value("kKawaiiModifyBoneSkipOffset", 16384);
        parsed.kKawaiiLimitDrivingBoneOffset = compatibility.Value("kKawaiiLimitDrivingBoneOffset", 16384);
        parsed.kKawaiiLimitCachedBoneIndexOffset = compatibility.Value("kKawaiiLimitCachedBoneIndexOffset", 16384);
        parsed.kKawaiiLimitOffsetLocationOffset = compatibility.Value("kKawaiiLimitOffsetLocationOffset", 16384);
        parsed.kKawaiiLimitOffsetRotationOffset = compatibility.Value("kKawaiiLimitOffsetRotationOffset", 16384);
        parsed.kKawaiiLimitEnabledOffset = compatibility.Value("kKawaiiLimitEnabledOffset", 16384);
        parsed.kKawaiiLimitRadiusOffset = compatibility.Value("kKawaiiLimitRadiusOffset", 16384);
        parsed.kKawaiiLimitLengthOffset = compatibility.Value("kKawaiiLimitLengthOffset", 16384);
        parsed.kKawaiiLimitSphereRadiusOffset = compatibility.Value("kKawaiiLimitSphereRadiusOffset", 16384);
        parsed.kKawaiiCapsuleLimitSize = compatibility.Value("kKawaiiCapsuleLimitSize", 16384);
        parsed.kAnimInstanceUseMultiThreadedUpdateOffset = compatibility.Value("kAnimInstanceUseMultiThreadedUpdateOffset", 16384);
        parsed.kAnimInstanceUseMultiThreadedUpdateBit = compatibility.Value("kAnimInstanceUseMultiThreadedUpdateBit", 16384);
        parsed.kAnimationFlagPauseAnimsBit = compatibility.Value("kAnimationFlagPauseAnimsBit", 16384);
        parsed.kAnimationFlagEnableAnimationBit = compatibility.Value("kAnimationFlagEnableAnimationBit", 16384);
        parsed.kTransformRotationOffset = compatibility.Value("kTransformRotationOffset", 16384);
        parsed.kTransformTranslationOffset = compatibility.Value("kTransformTranslationOffset", 16384);
        parsed.kTransformScaleOffset = compatibility.Value("kTransformScaleOffset", 16384);
        parsed.kArrayDataOffset = compatibility.Value("kArrayDataOffset", 16384);
        parsed.kArrayCountOffset = compatibility.Value("kArrayCountOffset", 16384);
        parsed.kArrayCapacityOffset = compatibility.Value("kArrayCapacityOffset", 16384);
        if (parsed.kTransformSize != sizeof(TransformContract) ||
            parsed.kTransformRotationOffset != offsetof(TransformContract,rotation) ||
            parsed.kTransformTranslationOffset != offsetof(TransformContract,translation) ||
            parsed.kTransformScaleOffset != offsetof(TransformContract,scale) ||
            parsed.kObjectChunkSize == 0 || parsed.kObjectItemStride < sizeof(void*) ||
            parsed.kAnimInstanceUseMultiThreadedUpdateBit > 7 ||
            parsed.kAnimationFlagPauseAnimsBit > 7 || parsed.kAnimationFlagEnableAnimationBit > 7 ||
            parsed.kKawaiiLimitOffsetLocationOffset + sizeof(double)*3 > parsed.kKawaiiCapsuleLimitSize ||
            parsed.kKawaiiLimitOffsetRotationOffset + sizeof(double)*3 > parsed.kKawaiiCapsuleLimitSize ||
            parsed.kKawaiiLimitRadiusOffset + sizeof(float) > parsed.kKawaiiCapsuleLimitSize ||
            parsed.kKawaiiLimitLengthOffset + sizeof(float) > parsed.kKawaiiCapsuleLimitSize ||
            parsed.kKawaiiModifyBoneNameOffset + sizeof(std::uint64_t) > parsed.kKawaiiModifyBoneSize)
            return false;
        native_layout = parsed;
        return true;
    } catch (...) { return false; }
}


// Resolved from HTGame.exe .text. The instruction is a RIP-relative load of
// GWorld; displacement is at byte 3 and the instruction is 7 bytes long.
inline constexpr std::string_view kGWorldPattern = "kGWorldPattern";
inline constexpr std::uint32_t kGWorldResolveOffset = 3;
inline constexpr std::uint32_t kGWorldInstructionSize = 7;

// GObjects registry contract. The pattern resolves the in-memory FUObjectArray
// root and the addend removes the 16-byte FRWScopeLock header. The object item
// layout is the same validated contract used by the internal UE5 object
// service.
inline constexpr std::string_view kGObjectsPattern = "kGObjectsPattern";
inline constexpr std::uint32_t kGObjectsResolveOffset = 3;
inline constexpr std::uint32_t kGObjectsInstructionSize = 7;

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
inline constexpr std::string_view kFunctionGetAnimInstancePath =
    "/Script/Engine.SkeletalMeshComponent.GetAnimInstance";
inline constexpr std::string_view kFunctionSetAutoRegisterBudgetPath =
    "/Script/AnimationBudgetAllocator.SkeletalMeshComponentBudgeted."
    "SetAutoRegisterWithBudgetAllocator";
inline constexpr std::string_view kFunctionSetComponentTickEnabledPath =
    "/Script/Engine.ActorComponent.SetComponentTickEnabled";
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

// ACharacter::Mesh.

// ACharacter::AnimRootMotionTranslationScale.


// Camera path. The view-point getter belongs to the free-camera plugin: its detour stub now
// sits at the function's first bytes, and the hook service refuses a second hook on the same
// target. What is available instead is the manager's cached POV -- the struct that getter
// copies its result from, and the same layout the active Profile names as
// cameraManager.location / cameraManager.rotation. Writing it composes with the other
// plugin's transparent passthrough (its detour calls the original and only replaces the
// result while its own free camera is on) instead of fighting it for the hook.

// The manager's vtable slot holding the view-point getter. That getter is hooked by the
// free-camera plugin, but its body names the accessor this plugin hooks instead, so the slot is
// read to locate the getter's code and follow that call.

// Where the getter reads the two vectors out of the POV it copies: location at +0 and rotation
// at +0x18 (three doubles each, measured from the getter's own movups/movsd offsets).

// FMinimalViewInfo::FOV follows the two vectors, and reads 80 in game (the same value the camera
// cache reports at cameraManager.fov).

// USkinnedMeshComponent's world FBoxSphereBounds {Origin, BoxExtent}: the active Profile names
// these as sceneComponent.boundsOrigin / boundsExtent, and reading them live gives a
// character-sized box (origin near the actor, extent.z = 84.7 for a 169 cm character), which is
// what the camera aims at.

// USkeletalMeshComponent animation state.

// Two component-space buffers, not a local/component pair. The live bone getter
// and rendering-data packer both select 0x628 + 0x10 * CurrentReadIndex.
// Cached* above remains a read-only fallback. The legacy RuntimeState field
// names bone_space_data/component_space_data refer to buffer 0/1 respectively.

// Do not call the unexposed component virtuals from the plugin. The current
// build's submission path was inspected read-only, but invoking its slot is not
// part of the validated plugin contract.


// FField / FProperty fields used to locate generated AnimBP nodes without
// hard-coding a node's final member offset. The generated class owns the chain
// at UStruct::ChildProperties; AnimBP graphs currently place the standard
// KawaiiPhysics node at +0x510 or +0x520 depending on generated members.

// FAnimNode_KawaiiPhysics, verified against the installed NTE build. These are
// read-only runtime layout offsets; simulation still belongs to BetterPose.

// FCapsuleLimit, used by KawaiiPhysics to constrain its spring bones. These
// entries are not body-collision capsules.

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

// bPauseAnims / bEnableAnimation bit positions inside the byte at +0xAC0.

// FTransform layout used by both cached pose arrays:
// FQuat (4x double), FVector translation (3x double), padding, FVector scale.

// UE TArray header embedded directly in USkeletalMeshComponent.

// Render-side diagnostics and bounds.
inline constexpr std::size_t kMaximumStatusBytes = 256;
inline constexpr std::uint32_t kMaximumBoneIndex = 8191;
inline constexpr std::size_t kMaximumPoseProbeBones = 8;

}  // namespace better_pose_profile
