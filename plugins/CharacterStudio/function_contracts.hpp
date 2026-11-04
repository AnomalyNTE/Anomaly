#pragma once
#include <anomaly/sdk/profile.hpp>
#include <map>
#include <vector>
namespace character_studio {
struct FieldContract { std::string name, type; std::uint32_t offset{}, size{}; };
struct FunctionContract { std::uint32_t size{}; std::vector<FieldContract> fields; };
using FunctionContracts = std::map<std::string,FunctionContract,std::less<>>;
inline FunctionContracts LoadFunctionContracts(const anomaly::sdk::ProfileValues& layout) {
    FunctionContracts result;
    result.emplace("/Script/Engine.SkeletalMeshComponent.SetAnimation", FunctionContract{layout.at("function.SetAnimation.size"), {
        {"NewAnimToPlay","ObjectProperty",layout.at("function.SetAnimation.NewAnimToPlay.offset"),layout.at("function.SetAnimation.NewAnimToPlay.size")},
    }});
    result.emplace("/Script/Engine.AnimSingleNodeInstance.SetPlaying", FunctionContract{layout.at("function.SetPlaying.size"), {
        {"bIsPlaying","BoolProperty",layout.at("function.SetPlaying.bIsPlaying.offset"),layout.at("function.SetPlaying.bIsPlaying.size")},
    }});
    result.emplace("/Script/Engine.AnimSingleNodeInstance.SetLooping", FunctionContract{layout.at("function.SetLooping.size"), {
        {"bIsLooping","BoolProperty",layout.at("function.SetLooping.bIsLooping.offset"),layout.at("function.SetLooping.bIsLooping.size")},
    }});
    result.emplace("/Script/Engine.AnimSingleNodeInstance.SetPlayRate", FunctionContract{layout.at("function.SetPlayRate.size"), {
        {"InPlayRate","FloatProperty",layout.at("function.SetPlayRate.InPlayRate.offset"),layout.at("function.SetPlayRate.InPlayRate.size")},
    }});
    result.emplace("/Script/Engine.GameplayStatics.BeginDeferredActorSpawnFromClass", FunctionContract{layout.at("function.BeginDeferredActorSpawnFromClass.size"), {
        {"WorldContextObject","ObjectProperty",layout.at("function.BeginDeferredActorSpawnFromClass.WorldContextObject.offset"),layout.at("function.BeginDeferredActorSpawnFromClass.WorldContextObject.size")},
        {"ActorClass","ClassProperty",layout.at("function.BeginDeferredActorSpawnFromClass.ActorClass.offset"),layout.at("function.BeginDeferredActorSpawnFromClass.ActorClass.size")},
        {"SpawnTransform","StructProperty",layout.at("function.BeginDeferredActorSpawnFromClass.SpawnTransform.offset"),layout.at("function.BeginDeferredActorSpawnFromClass.SpawnTransform.size")},
        {"CollisionHandlingOverride","EnumProperty|ByteProperty",layout.at("function.BeginDeferredActorSpawnFromClass.CollisionHandlingOverride.offset"),layout.at("function.BeginDeferredActorSpawnFromClass.CollisionHandlingOverride.size")},
        {"Owner","ObjectProperty",layout.at("function.BeginDeferredActorSpawnFromClass.Owner.offset"),layout.at("function.BeginDeferredActorSpawnFromClass.Owner.size")},
        {"TransformScaleMethod","EnumProperty|ByteProperty",layout.at("function.BeginDeferredActorSpawnFromClass.TransformScaleMethod.offset"),layout.at("function.BeginDeferredActorSpawnFromClass.TransformScaleMethod.size")},
        {"ReturnValue","ObjectProperty",layout.at("function.BeginDeferredActorSpawnFromClass.ReturnValue.offset"),layout.at("function.BeginDeferredActorSpawnFromClass.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.GameplayStatics.FinishSpawningActor", FunctionContract{layout.at("function.FinishSpawningActor.size"), {
        {"Actor","ObjectProperty",layout.at("function.FinishSpawningActor.Actor.offset"),layout.at("function.FinishSpawningActor.Actor.size")},
        {"SpawnTransform","StructProperty",layout.at("function.FinishSpawningActor.SpawnTransform.offset"),layout.at("function.FinishSpawningActor.SpawnTransform.size")},
        {"TransformScaleMethod","EnumProperty|ByteProperty",layout.at("function.FinishSpawningActor.TransformScaleMethod.offset"),layout.at("function.FinishSpawningActor.TransformScaleMethod.size")},
        {"ReturnValue","ObjectProperty",layout.at("function.FinishSpawningActor.ReturnValue.offset"),layout.at("function.FinishSpawningActor.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.KismetSystemLibrary.LoadAsset_Blocking", FunctionContract{layout.at("function.LoadAsset_Blocking.size"), {
        {"Asset","SoftObjectProperty",layout.at("function.LoadAsset_Blocking.Asset.offset"),layout.at("function.LoadAsset_Blocking.Asset.size")},
        {"ReturnValue","ObjectProperty",layout.at("function.LoadAsset_Blocking.ReturnValue.offset"),layout.at("function.LoadAsset_Blocking.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.Actor.K2_DestroyActor", FunctionContract{layout.at("function.K2_DestroyActor.size"), {
    }});
    result.emplace("/Script/Engine.Actor.SetReplicates", FunctionContract{layout.at("function.SetReplicates.size"), {
        {"bInReplicates","BoolProperty",layout.at("function.SetReplicates.bInReplicates.offset"),layout.at("function.SetReplicates.bInReplicates.size")},
    }});
    result.emplace("/Script/Engine.Actor.SetReplicateMovement", FunctionContract{layout.at("function.SetReplicateMovement.size"), {
        {"bInReplicateMovement","BoolProperty",layout.at("function.SetReplicateMovement.bInReplicateMovement.offset"),layout.at("function.SetReplicateMovement.bInReplicateMovement.size")},
    }});
    result.emplace("/Script/Engine.Actor.SetActorEnableCollision", FunctionContract{layout.at("function.SetActorEnableCollision.size"), {
        {"bNewActorEnableCollision","BoolProperty",layout.at("function.SetActorEnableCollision.bNewActorEnableCollision.offset"),layout.at("function.SetActorEnableCollision.bNewActorEnableCollision.size")},
    }});
    result.emplace("/Script/Engine.Actor.SetLifeSpan", FunctionContract{layout.at("function.SetLifeSpan.size"), {
        {"InLifespan","FloatProperty",layout.at("function.SetLifeSpan.InLifespan.offset"),layout.at("function.SetLifeSpan.InLifespan.size")},
    }});
    result.emplace("/Script/Engine.Actor.K2_SetActorLocationAndRotation", FunctionContract{layout.at("function.K2_SetActorLocationAndRotation.size"), {
        {"NewLocation","StructProperty",layout.at("function.K2_SetActorLocationAndRotation.NewLocation.offset"),layout.at("function.K2_SetActorLocationAndRotation.NewLocation.size")},
        {"NewRotation","StructProperty",layout.at("function.K2_SetActorLocationAndRotation.NewRotation.offset"),layout.at("function.K2_SetActorLocationAndRotation.NewRotation.size")},
        {"bSweep","BoolProperty",layout.at("function.K2_SetActorLocationAndRotation.bSweep.offset"),layout.at("function.K2_SetActorLocationAndRotation.bSweep.size")},
        {"SweepHitResult","StructProperty",layout.at("function.K2_SetActorLocationAndRotation.SweepHitResult.offset"),layout.at("function.K2_SetActorLocationAndRotation.SweepHitResult.size")},
        {"bTeleport","BoolProperty",layout.at("function.K2_SetActorLocationAndRotation.bTeleport.offset"),layout.at("function.K2_SetActorLocationAndRotation.bTeleport.size")},
        {"ReturnValue","BoolProperty",layout.at("function.K2_SetActorLocationAndRotation.ReturnValue.offset"),layout.at("function.K2_SetActorLocationAndRotation.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.GameplayStatics.GetPlayerPawn", FunctionContract{layout.at("function.GetPlayerPawn.size"), {
        {"WorldContextObject","ObjectProperty",layout.at("function.GetPlayerPawn.WorldContextObject.offset"),layout.at("function.GetPlayerPawn.WorldContextObject.size")},
        {"PlayerIndex","IntProperty",layout.at("function.GetPlayerPawn.PlayerIndex.offset"),layout.at("function.GetPlayerPawn.PlayerIndex.size")},
        {"ReturnValue","ObjectProperty",layout.at("function.GetPlayerPawn.ReturnValue.offset"),layout.at("function.GetPlayerPawn.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.SceneComponent.K2_GetComponentRotation", FunctionContract{layout.at("function.K2_GetComponentRotation.size"), {
        {"ReturnValue","StructProperty",layout.at("function.K2_GetComponentRotation.ReturnValue.offset"),layout.at("function.K2_GetComponentRotation.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.Actor.K2_GetActorLocation", FunctionContract{layout.at("function.K2_GetActorLocation.size"), {
        {"ReturnValue","StructProperty",layout.at("function.K2_GetActorLocation.ReturnValue.offset"),layout.at("function.K2_GetActorLocation.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.Actor.AddComponentByClass", FunctionContract{layout.at("function.AddComponentByClass.size"), {
        {"Class","ClassProperty",layout.at("function.AddComponentByClass.Class.offset"),layout.at("function.AddComponentByClass.Class.size")},
        {"bManualAttachment","BoolProperty",layout.at("function.AddComponentByClass.bManualAttachment.offset"),layout.at("function.AddComponentByClass.bManualAttachment.size")},
        {"RelativeTransform","StructProperty",layout.at("function.AddComponentByClass.RelativeTransform.offset"),layout.at("function.AddComponentByClass.RelativeTransform.size")},
        {"bDeferredFinish","BoolProperty",layout.at("function.AddComponentByClass.bDeferredFinish.offset"),layout.at("function.AddComponentByClass.bDeferredFinish.size")},
        {"ReturnValue","ObjectProperty",layout.at("function.AddComponentByClass.ReturnValue.offset"),layout.at("function.AddComponentByClass.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.SkeletalMeshComponent.SetSkeletalMeshAsset", FunctionContract{layout.at("function.SetSkeletalMeshAsset.size"), {
        {"NewMesh","ObjectProperty",layout.at("function.SetSkeletalMeshAsset.NewMesh.offset"),layout.at("function.SetSkeletalMeshAsset.NewMesh.size")},
    }});
    result.emplace("/Script/Engine.SkeletalMeshComponent.SetAnimationMode", FunctionContract{layout.at("function.SetAnimationMode.size"), {
        {"InAnimationMode","EnumProperty|ByteProperty",layout.at("function.SetAnimationMode.InAnimationMode.offset"),layout.at("function.SetAnimationMode.InAnimationMode.size")},
        {"bForceInitAnimScriptInstance","BoolProperty",layout.at("function.SetAnimationMode.bForceInitAnimScriptInstance.offset"),layout.at("function.SetAnimationMode.bForceInitAnimScriptInstance.size")},
    }});
    result.emplace("/Script/Engine.PrimitiveComponent.SetSimulatePhysics", FunctionContract{layout.at("function.SetSimulatePhysics.size"), {
        {"bSimulate","BoolProperty",layout.at("function.SetSimulatePhysics.bSimulate.offset"),layout.at("function.SetSimulatePhysics.bSimulate.size")},
    }});
    result.emplace("/Script/Engine.PrimitiveComponent.SetCollisionEnabled", FunctionContract{layout.at("function.SetCollisionEnabled.size"), {
        {"NewType","EnumProperty|ByteProperty",layout.at("function.SetCollisionEnabled.NewType.offset"),layout.at("function.SetCollisionEnabled.NewType.size")},
    }});
    result.emplace("/Script/Engine.SceneComponent.K2_AttachToComponent", FunctionContract{layout.at("function.K2_AttachToComponent.size"), {
        {"Parent","ObjectProperty",layout.at("function.K2_AttachToComponent.Parent.offset"),layout.at("function.K2_AttachToComponent.Parent.size")},
        {"SocketName","NameProperty",layout.at("function.K2_AttachToComponent.SocketName.offset"),layout.at("function.K2_AttachToComponent.SocketName.size")},
        {"LocationRule","EnumProperty|ByteProperty",layout.at("function.K2_AttachToComponent.LocationRule.offset"),layout.at("function.K2_AttachToComponent.LocationRule.size")},
        {"RotationRule","EnumProperty|ByteProperty",layout.at("function.K2_AttachToComponent.RotationRule.offset"),layout.at("function.K2_AttachToComponent.RotationRule.size")},
        {"ScaleRule","EnumProperty|ByteProperty",layout.at("function.K2_AttachToComponent.ScaleRule.offset"),layout.at("function.K2_AttachToComponent.ScaleRule.size")},
        {"bWeldSimulatedBodies","BoolProperty",layout.at("function.K2_AttachToComponent.bWeldSimulatedBodies.offset"),layout.at("function.K2_AttachToComponent.bWeldSimulatedBodies.size")},
        {"ReturnValue","BoolProperty",layout.at("function.K2_AttachToComponent.ReturnValue.offset"),layout.at("function.K2_AttachToComponent.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.SceneComponent.K2_SetRelativeTransform", FunctionContract{layout.at("function.K2_SetRelativeTransform.size"), {
        {"NewTransform","StructProperty",layout.at("function.K2_SetRelativeTransform.NewTransform.offset"),layout.at("function.K2_SetRelativeTransform.NewTransform.size")},
        {"bSweep","BoolProperty",layout.at("function.K2_SetRelativeTransform.bSweep.offset"),layout.at("function.K2_SetRelativeTransform.bSweep.size")},
        {"SweepHitResult","StructProperty",layout.at("function.K2_SetRelativeTransform.SweepHitResult.offset"),layout.at("function.K2_SetRelativeTransform.SweepHitResult.size")},
        {"bTeleport","BoolProperty",layout.at("function.K2_SetRelativeTransform.bTeleport.offset"),layout.at("function.K2_SetRelativeTransform.bTeleport.size")},
    }});
    result.emplace("/Script/Engine.SkinnedMeshComponent.SetLeaderPoseComponent", FunctionContract{layout.at("function.SetLeaderPoseComponent.size"), {
        {"NewLeaderBoneComponent","ObjectProperty",layout.at("function.SetLeaderPoseComponent.NewLeaderBoneComponent.offset"),layout.at("function.SetLeaderPoseComponent.NewLeaderBoneComponent.size")},
        {"bForceUpdate","BoolProperty",layout.at("function.SetLeaderPoseComponent.bForceUpdate.offset"),layout.at("function.SetLeaderPoseComponent.bForceUpdate.size")},
        {"bInFollowerShouldTickPose","BoolProperty",layout.at("function.SetLeaderPoseComponent.bInFollowerShouldTickPose.offset"),layout.at("function.SetLeaderPoseComponent.bInFollowerShouldTickPose.size")},
    }});
    result.emplace("/Script/HTGame.HTGameData.GetGameData", FunctionContract{layout.at("function.GetGameData.size"), {
        {"ReturnValue","ObjectProperty",layout.at("function.GetGameData.ReturnValue.offset"),layout.at("function.GetGameData.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.SkeletalMeshComponent.OverrideAnimationData", FunctionContract{layout.at("function.OverrideAnimationData.size"), {
        {"InAnimToPlay","ObjectProperty",layout.at("function.OverrideAnimationData.InAnimToPlay.offset"),layout.at("function.OverrideAnimationData.InAnimToPlay.size")},
        {"bIsLooping","BoolProperty",layout.at("function.OverrideAnimationData.bIsLooping.offset"),layout.at("function.OverrideAnimationData.bIsLooping.size")},
        {"bIsPlaying","BoolProperty",layout.at("function.OverrideAnimationData.bIsPlaying.offset"),layout.at("function.OverrideAnimationData.bIsPlaying.size")},
        {"position","FloatProperty",layout.at("function.OverrideAnimationData.position.offset"),layout.at("function.OverrideAnimationData.position.size")},
        {"PlayRate","FloatProperty",layout.at("function.OverrideAnimationData.PlayRate.offset"),layout.at("function.OverrideAnimationData.PlayRate.size")},
    }});
    result.emplace("/Script/Engine.SkeletalMeshComponent.GetSkeletalMeshAsset", FunctionContract{layout.at("function.GetSkeletalMeshAsset.size"), {
        {"ReturnValue","ObjectProperty",layout.at("function.GetSkeletalMeshAsset.ReturnValue.offset"),layout.at("function.GetSkeletalMeshAsset.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.SkeletalMeshComponent.GetAnimInstance", FunctionContract{layout.at("function.GetAnimInstance.size"), {
        {"ReturnValue","ObjectProperty",layout.at("function.GetAnimInstance.ReturnValue.offset"),layout.at("function.GetAnimInstance.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.AnimSingleNodeInstance.GetAnimationAsset", FunctionContract{layout.at("function.GetAnimationAsset.size"), {
        {"ReturnValue","ObjectProperty",layout.at("function.GetAnimationAsset.ReturnValue.offset"),layout.at("function.GetAnimationAsset.ReturnValue.size")},
    }});
    result.emplace("/Script/Engine.SkeletalMeshComponent.SetDisablePostProcessBlueprint", FunctionContract{layout.at("function.SetDisablePostProcessBlueprint.size"), {
        {"bInDisablePostProcess","BoolProperty",layout.at("function.SetDisablePostProcessBlueprint.bInDisablePostProcess.offset"),layout.at("function.SetDisablePostProcessBlueprint.bInDisablePostProcess.size")},
    }});
    result.emplace("/Script/Engine.SkeletalMeshComponent.SetPosition", FunctionContract{layout.at("function.SetPosition.size"), {
        {"InPos","FloatProperty",layout.at("function.SetPosition.InPos.offset"),layout.at("function.SetPosition.InPos.size")},
        {"bFireNotifies","BoolProperty",layout.at("function.SetPosition.bFireNotifies.offset"),layout.at("function.SetPosition.bFireNotifies.size")},
    }});
    result.emplace("/Script/Engine.Actor.IsActorBeingDestroyed", FunctionContract{layout.at("function.IsActorBeingDestroyed.size"), {
        {"ReturnValue","BoolProperty",layout.at("function.IsActorBeingDestroyed.ReturnValue.offset"),layout.at("function.IsActorBeingDestroyed.ReturnValue.size")},
    }});
    return result;
}
}
