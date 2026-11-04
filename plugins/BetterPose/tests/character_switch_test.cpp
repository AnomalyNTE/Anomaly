#include "../plugin.cpp"
#include "../../../apps/test_host/plugin_profile_fixture_host.hpp"

#include <cstdlib>
#include <iostream>

namespace fixture {
void Check(bool value,const char* message) {
  if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
struct Block {
  std::array<std::uint8_t,0x1000> bytes{};
  std::uintptr_t Address() { return reinterpret_cast<std::uintptr_t>(bytes.data()); }
  template<class T> void Set(std::size_t offset,T value) { std::memcpy(bytes.data()+offset,&value,sizeof(value)); }
  template<class T> T Get(std::size_t offset) const { T value{}; std::memcpy(&value,bytes.data()+offset,sizeof(value)); return value; }
};
struct Region { std::uintptr_t address; std::size_t size; };
std::vector<Region> regions;
template<class T> void Register(T& data) { regions.push_back({reinterpret_cast<std::uintptr_t>(&data),sizeof(data)}); }
bool Contains(std::uintptr_t address,std::size_t size) {
  for (const auto& r:regions)
    if (address>=r.address && address-r.address<=r.size && size<=r.size-(address-r.address)) return true;
  return false;
}
void __fastcall Event(void*,void*,void*);
AnomalyStatusV1 ANOMALY_CALL ReadMemory(void*,std::uintptr_t address,AnomalyMutableByteSpanV1 bytes) {
  if (address == reinterpret_cast<std::uintptr_t>(Event)) return anomaly::testing::RecordedProcessEventEntry(bytes);
  if (!Contains(address,bytes.size)) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
  std::memcpy(bytes.data,reinterpret_cast<void*>(address),bytes.size); return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL WriteMemory(void*,std::uintptr_t address,AnomalyByteSpanV1 bytes) {
  if (!Contains(address,bytes.size)) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
  std::memcpy(reinterpret_cast<void*>(address),bytes.data,bytes.size); return anomaly::sdk::Ok();
}
enum Function { Mode,Lod,Visibility,Destroy,Count };
int functions[Count]{};
unsigned destroyed{};
void __fastcall Event(void* object,void* function,void* parameters) {
  auto* bytes=static_cast<std::uint8_t*>(object);
  if (function==&functions[Mode]) std::memcpy(bytes+native_layout.kMeshAnimationModeOffset,parameters,1);
  if (function==&functions[Lod]) std::memcpy(bytes+native_layout.kMeshForcedLodModelOffset,parameters,4);
  if (function==&functions[Visibility]) bytes[0x300]=*static_cast<std::uint8_t*>(parameters);
  if (function==&functions[Destroy]) {
    Check(*static_cast<std::uintptr_t*>(parameters)==reinterpret_cast<std::uintptr_t>(object),"destroy caller");
    ++destroyed;
  }
}
AnomalyStatusV1 ANOMALY_CALL Find(void*,AnomalyStringViewV1 path,AnomalyGenerationHandleV1* handle) {
  const std::array<std::string_view,Count> names{kFunctionSetAnimationModePath,kFunctionSetForcedLodPath,
                                               kFunctionSceneSetVisibilityPath,kFunctionActorComponentDestroyPath};
  for (std::size_t i=0;i<names.size();++i) if (names[i]==std::string_view(path.data,path.size)) {
    handle->id=i+1; return anomaly::sdk::Ok();
  }
  return Status(ANOMALY_STATUS_V1_NOT_FOUND);
}
struct Character {
  Block actor,mesh,anim;
  std::array<PackedTransform,3> local{},buffer0{},buffer1{};
  Character(std::uintptr_t vtable,std::uint8_t mode,std::uint8_t flags,float rate,float root,int lod) {
    Register(actor); Register(mesh); Register(anim); Register(local); Register(buffer0); Register(buffer1);
    actor.Set(native_layout.kCharacterMeshOffset,mesh.Address()); actor.Set(native_layout.kCharacterAnimRootMotionScaleOffset,root);
    mesh.Set(0,vtable); mesh.Set(native_layout.kMeshAnimationModeOffset,mode); mesh.Set(native_layout.kMeshAnimationFlagsOffset,flags);
    mesh.Set(native_layout.kMeshGlobalAnimRateScaleOffset,rate); mesh.Set(native_layout.kMeshForcedLodModelOffset,lod);
    mesh.Set(native_layout.kMeshAnimScriptInstanceOffset,anim.Address());
    anim.Set(native_layout.kAnimInstanceUseMultiThreadedUpdateOffset,std::uint8_t{0xff});
    for (auto& b:local) b.rotation[3]=1;
    const std::array<std::uint32_t,3> offsets{native_layout.kMeshLocalSpaceTransformsOffset,native_layout.kMeshComponentSpaceBuffer0Offset,native_layout.kMeshComponentSpaceBuffer1Offset};
    const std::array<std::uintptr_t,3> arrays{reinterpret_cast<std::uintptr_t>(local.data()),
        reinterpret_cast<std::uintptr_t>(buffer0.data()),reinterpret_cast<std::uintptr_t>(buffer1.data())};
    for (std::size_t i=0;i<offsets.size();++i) {
      mesh.Set(offsets[i],arrays[i]); mesh.Set(offsets[i]+8,std::int32_t{3});
    }
  }
};
void TakeOver(Context& context) {
  Check(ReadAnimationState(context) && ReadPoseArrays(context),"new skeleton unavailable");
  Check(EnsurePoseAnimationMode(context,true) && EnsurePoseForcedLod(context,true) &&
        ApplyPause(context,true) && ApplyRate(context,true,.25F) &&
        ApplyRootMotion(context,true,.5F) && ApplyMultiThreadedUpdate(context,true),"body takeover failed");
}
void CheckOriginal(const Character& c,std::uint8_t mode,std::uint8_t flags,float rate,float root,int lod) {
  Check(c.mesh.Get<std::uint8_t>(native_layout.kMeshAnimationModeOffset)==mode,"previous character animation mode not restored");
  Check(c.mesh.Get<std::uint8_t>(native_layout.kMeshAnimationFlagsOffset)==flags,"previous character stayed paused");
  Check(c.mesh.Get<float>(native_layout.kMeshGlobalAnimRateScaleOffset)==rate,"rate restored to wrong character");
  Check(c.actor.Get<float>(native_layout.kCharacterAnimRootMotionScaleOffset)==root,"root scale restored to wrong actor");
  Check(c.mesh.Get<std::int32_t>(native_layout.kMeshForcedLodModelOffset)==lod,"LOD restored to wrong mesh");
  Check(c.anim.Get<std::uint8_t>(native_layout.kAnimInstanceUseMultiThreadedUpdateOffset)==0xff,"old anim instance not restored");
}
void CheckExtraAnimationMode() {
  Block accessory;
  Register(accessory);
  constexpr std::uint8_t kOriginalMode = 0;
  constexpr std::uint8_t kOriginalFlags = 0xff;
  accessory.Set(native_layout.kMeshAnimationModeOffset, kOriginalMode);
  accessory.Set(native_layout.kMeshAnimationFlagsOffset, kOriginalFlags);
  Context context;
  AnomalyCoreServiceV1 core{};
  core.struct_size = sizeof(core); core.service_version = 1;
  core.read_memory = ReadMemory;
  core.write_memory = WriteMemory;
  context.core = &core;
  Context::ExtraMesh extra;
  extra.object = accessory.Address();
  Check(EnsureExtraPoseAnimationMode(context, extra, true),
        "accessory animation did not pause");
  Check(accessory.Get<std::uint8_t>(native_layout.kMeshAnimationModeOffset) ==
            kAnimationModeCustom &&
            extra.animation_mode_paused,
        "accessory animation mode was not switched to custom");
  const std::uint8_t paused_flags =
      accessory.Get<std::uint8_t>(native_layout.kMeshAnimationFlagsOffset);
  Check((paused_flags & (1U << native_layout.kAnimationFlagPauseAnimsBit)) != 0 &&
            (paused_flags & (1U << native_layout.kAnimationFlagEnableAnimationBit)) == 0,
        "accessory animation flags were not paused");
  Check(EnsureExtraPoseAnimationMode(context, extra, true),
        "accessory animation pause was not idempotent");
  accessory.Set(native_layout.kMeshAnimationModeOffset, kOriginalMode);
  accessory.Set(native_layout.kMeshAnimationFlagsOffset, kOriginalFlags);
  Check(EnsureExtraPoseAnimationMode(context, extra, true) &&
            accessory.Get<std::uint8_t>(native_layout.kMeshAnimationModeOffset) ==
                kAnimationModeCustom &&
            (accessory.Get<std::uint8_t>(native_layout.kMeshAnimationFlagsOffset) &
             (1U << native_layout.kAnimationFlagPauseAnimsBit)) != 0,
        "accessory animation pause was not reasserted");
  Check(EnsureExtraPoseAnimationMode(context, extra, false),
        "accessory animation did not resume");
  Check(accessory.Get<std::uint8_t>(native_layout.kMeshAnimationModeOffset) ==
            kOriginalMode &&
            accessory.Get<std::uint8_t>(native_layout.kMeshAnimationFlagsOffset) ==
                kOriginalFlags &&
            !extra.animation_mode_paused &&
            !extra.original_animation_mode_saved,
        "accessory animation mode was not restored");
}
void CheckNativeCapsuleRuntime() {
  Context::ExtraMesh extra;
  extra.kawaii_component_space = true;
  extra.bone_names = {"Root", "Tip"};
  better_pose::accessory::Bone root{};
  root[3] = 1.0;
  root[8] = root[9] = root[10] = 1.0;
  better_pose::accessory::Bone tip = root;
  tip[5] = -10.0;
  Check(extra.accessory_dynamics.Configure({root, tip}, {-1, 0},
                                           {"ribbonRoot", "tip"}),
        "native capsule rig failed");
  PackedTransform identity{};
  identity.rotation[3] = 1.0;
  identity.scale[0] = identity.scale[1] = identity.scale[2] = 1.0;
  extra.bind_world.resize(2);
  std::memcpy(extra.bind_world[0].data(), &identity, sizeof(identity));
  PackedTransform tip_transform = identity;
  tip_transform.translation[1] = -10.0;
  std::memcpy(extra.bind_world[1].data(), &tip_transform,
              sizeof(tip_transform));
  Context::ExtraMesh::KawaiiLimit limit;
  limit.driving_bone = 0;
  limit.offset_location = {0.0, 0.0, -10.0};
  limit.radius = 2.0F;
  limit.length = 4.0F;
  limit.sphere_radius = 4.0F;
  extra.kawaii_limits.push_back(limit);
  std::vector<better_pose::accessory::Capsule> capsules;
  AppendKawaiiLimits(extra, {}, capsules);
  Check(capsules.size() == 1 &&
            std::abs(capsules[0].a.z + 12.0) < 1e-6 &&
            std::abs(capsules[0].b.z + 8.0) < 1e-6 &&
            std::abs(capsules[0].radius - 2.0) < 1e-6,
        "native capsule current-driver offset was not used");
}
void CheckKawaiiLimitDecode() {
  std::array<std::uint8_t, 176> raw{}; // Independent SDK FKC physics capsule record.
  const float radius = 15.209F;
  const float length = 4.735F;
  const float sphere_radius = 17.576F;
  std::memcpy(raw.data() + native_layout.kKawaiiLimitRadiusOffset, &radius, sizeof(radius));
  std::memcpy(raw.data() + native_layout.kKawaiiLimitLengthOffset, &length, sizeof(length));
  std::memcpy(raw.data() + native_layout.kKawaiiLimitSphereRadiusOffset, &sphere_radius,
              sizeof(sphere_radius));
  KawaiiCapsuleLimitSnapshot decoded;
  ReadKawaiiLimitGeometry(raw.data(), decoded);
  Check(std::abs(decoded.radius - 15.209) < 1e-5 &&
            std::abs(decoded.length - 4.735) < 1e-5 &&
            std::abs(decoded.sphere_radius - 17.576) < 1e-5,
        "Kawaii capsule floats were not decoded with their native width");

  const float no_radius = 0.0F;
  std::memcpy(raw.data() + native_layout.kKawaiiLimitRadiusOffset, &no_radius,
              sizeof(no_radius));
  ReadKawaiiLimitGeometry(raw.data(), decoded);
  Check(std::abs(decoded.radius - 17.576) < 1e-5,
        "Kawaii capsule sphere radius fallback was not applied");
}
}

int main() {
  anomaly::testing::ProfileHost profile_host("BetterPose");
  if (!better_pose_profile::LoadCompatibility(&profile_host.api)) { std::cerr << profile_host.error; return 1; }
  using namespace fixture;
  {
    AnomalyPluginDescriptorV1 legacy{};
    legacy.struct_size =
        offsetof(AnomalyPluginDescriptorV1, on_draw) +
        sizeof(legacy.on_draw);
    Check(AnomalyPluginEntryV1(&legacy).code == ANOMALY_STATUS_V1_OK &&
              legacy.on_draw == Draw,
          "legacy descriptor without on_overlay was rejected");

    AnomalyPluginDescriptorV1 current{};
    current.struct_size = sizeof(current);
    Check(AnomalyPluginEntryV1(&current).code == ANOMALY_STATUS_V1_OK &&
              current.on_draw == Draw &&
              current.on_overlay == DrawOverlay,
          "current descriptor did not register the persistent overlay");
  }
  Check(ControlTrackPriority("左手握り")==100 &&
            ControlTrackPriority("HandSpread")==95 &&
            ControlTrackPriority("HandControl")==70 &&
            ControlTrackPriority("右手捩")==0 &&
            IsStructuralControlTrack("右手捩") &&
            !IsStructuralControlTrack("D1") &&
            ControlTrackPriority("HairA_01")==0,
        "hand-control source suggestion ordering is wrong");
  Context context;
  bool joint_calibration_default = false;
  {
    std::lock_guard<std::mutex> lock(context.motion_joint_mutex);
    joint_calibration_default = context.motion_joint_calibration.enabled;
  }
  Check(!context.motion_travel_enabled.load() &&
            !context.motion_amplitude_enabled.load() &&
            !context.motion_limb_enabled.load() &&
            !joint_calibration_default,
        "motion calibration masters do not default off");
  AnomalyCoreServiceV1 core{}; core.struct_size=sizeof(core); core.service_version=1; core.read_memory=ReadMemory; core.write_memory=WriteMemory;
  context.core=&core;
  AnomalyUe5ObjectsServiceV1 objects{}; objects.struct_size=sizeof(objects); objects.find_exact=Find; context.objects=&objects;
  std::array<std::uintptr_t,80> vtable{}; Register(vtable);
  vtable[native_layout.kProcessEventVtableSlot]=reinterpret_cast<std::uintptr_t>(Event);
  std::array<std::uint8_t,Count*24> slots{}; Register(slots);
  for (std::size_t i=0;i<Count;++i) {
    const auto ptr=reinterpret_cast<std::uintptr_t>(&functions[i]); std::memcpy(slots.data()+i*24,&ptr,sizeof(ptr));
  }
  auto chunk=reinterpret_cast<std::uintptr_t>(slots.data()); Register(chunk);
  context.object_registry.items=reinterpret_cast<std::uintptr_t>(&chunk); context.object_registry.count=Count;
  context.object_registry.num_chunks=1;
  Character a(reinterpret_cast<std::uintptr_t>(vtable.data()),0,0x20,1,1,2);
  Character b(reinterpret_cast<std::uintptr_t>(vtable.data()),1,0x40,.75F,.8F,3);
  Block world,instance,players,player,controller,source,replacement;
  for (auto* block:{&world,&instance,&players,&player,&controller,&source,&replacement}) Register(*block);
  auto world_pointer=world.Address(); Register(world_pointer);
  context.runtime.g_world_address=reinterpret_cast<std::uintptr_t>(&world_pointer);
  world.Set(native_layout.kWorldGameInstanceOffset,instance.Address()); instance.Set(native_layout.kGameInstanceLocalPlayersOffset,players.Address());
  players.Set(0,player.Address()); player.Set(native_layout.kLocalPlayerControllerOffset,controller.Address());
  controller.Set(native_layout.kControllerPawnOffset,a.actor.Address());
  Check(ResolveLocalCharacter(context),"initial character resolve"); TakeOver(context);
  context.motion_loaded=true; context.motion_playing=false; context.motion_seconds=3;
  context.motion_display_seconds=3; context.motion_seek=2; context.motion_seek_pending=true;
  context.motion_file="dance.vmd";
  const auto old_load_epoch=context.motion_load_epoch;
  context.motion.bone_names={"Root","Bip001-Head"}; context.motion.bone_indices={0,1};
  context.motion.indices_ready=true; context.motion.bone_count=2; context.motion.frame_count=1;
  context.motion.rotations={0,0,0,1,0,0,.70710678F,.70710678F};
  context.motion.has_root=true; context.motion.root_bone_name="Root"; context.motion.root_bone_index=0;
  context.motion.roots={0,0,5}; context.motion.offset_names={"Bip001-Head"};
  context.motion.offset_indices={1}; context.motion.offsets={0,0,12}; context.motion.offsets_ready=true;
  context.bone_names={"Root","Bip001-Head","tip"}; context.bone_names_mesh=a.mesh.Address();
  context.bone_parents={-1,0,1}; context.bone_parents_ready=true;
  context.pose_base_ready=true; context.pose_base_mesh=a.mesh.Address(); context.pose_base_locals.resize(3);
  context.ref_locals.resize(3); context.ref_pose_character=a.actor.Address();
  source.Set(0,reinterpret_cast<std::uintptr_t>(vtable.data())); replacement.Set(0,reinterpret_cast<std::uintptr_t>(vtable.data()));
  context.extra_meshes.emplace_back(); auto& extra=context.extra_meshes.back();
  extra.object=source.Address(); extra.poseable_component=replacement.Address(); extra.poseable_source_visibility_changed=true;
  context.extra_mesh_owner=a.mesh.Address(); context.mesh_scan_owner=a.mesh.Address();
  context.mesh_scan_running=true; context.mesh_scan_candidates={{source.Address(),3}}; context.extra_build_pending=true;

  // A transition can pass through no pawn. Keep restoration ownership while
  // removing the active target so the mesh tick cannot keep writing the old body.
  controller.Set(native_layout.kControllerPawnOffset,std::uintptr_t{});
  Check(!ResolveLocalCharacter(context) && context.runtime.mesh==0,"missing pawn remained active");
  Check(destroyed==0,"transient gap dropped accessories");
  Check(context.motion_loaded,"transient gap unloaded motion");
  controller.Set(native_layout.kControllerPawnOffset,a.actor.Address());
  Check(ResolveLocalCharacter(context) && context.motion_loaded && context.motion.indices_ready,
        "same character returning after a gap unloaded motion");
  controller.Set(native_layout.kControllerPawnOffset,std::uintptr_t{});
  Check(!ResolveLocalCharacter(context),"second gap remained active");
  controller.Set(native_layout.kControllerPawnOffset,b.actor.Address());
  Check(ResolveLocalCharacter(context),"second character resolve");
  CheckOriginal(a,0,0x20,1,1,2);
  Check(!context.runtime.saved_pause && !context.runtime.animation_mode_applied && !context.runtime.forced_lod_applied,
        "new character inherited takeover flags");
  Check(destroyed==1 && source.Get<std::uint8_t>(0x300)==1 && context.extra_meshes.empty(),"old accessories not restored");
  Check(!context.motion_loaded && !context.motion_playing && context.motion_seconds==0 &&
        context.motion_display_seconds==0 && context.motion_seek==-1 && !context.motion_seek_pending,
        "switch did not unload motion and reset transport");
  Check(context.motion.rotations.empty() && context.motion.bone_names.empty() && context.motion.offset_indices.empty(),
        "old skeleton tracks retained");
  Check(context.motion_file=="dance.vmd","switch cleared the selected source file");
  Check(!ResolveMotionIndices(context),"unloaded motion still mapped to new character");
  CheckOriginal(b,1,0x40,.75F,.8F,3);
  Check(!context.pose_base_ready && context.pose_base_locals.empty() && context.bone_names.empty() &&
        context.bone_parents.empty() && context.ref_locals.empty(),"old skeleton caches retained");
  Check(!context.mesh_scan_running && context.mesh_scan_candidates.empty() && context.mesh_scan_requested,
        "old partial scan carried into new character");
  const std::string document=R"json({
    "kind":"better-pose-motion","frameCount":1,"fps":30,
    "bones":{"Root":[[0,0,0,1]],"Bip001-Head":[[0,0,0.70710678,0.70710678]]},
    "rootBone":"Root","rootTranslation":[[0,0,5]],"boneOffsets":{"Bip001-Head":[[0,0,12]]}
  })json";
  Check(!LoadMotionDocument(context,document,"old.betterpose.json",old_load_epoch) && !context.motion_loaded,
        "conversion finishing after switch reloaded the old skeleton");
  Check(LoadMotionDocument(context,document,"new.betterpose.json",context.motion_load_epoch),
        "explicit reload on the new character failed");
  Check(context.motion_file=="dance.vmd","loading converted output replaced the source selection");
  TakeOver(context);
  Check(b.mesh.Get<std::uint8_t>(native_layout.kMeshAnimationModeOffset)==kAnimationModeCustom &&
        (b.mesh.Get<std::uint8_t>(native_layout.kMeshAnimationFlagsOffset)&(1U<<native_layout.kAnimationFlagPauseAnimsBit)),"new body not taken over");
  context.bone_names={"Bip001-Head","Root","tip"}; context.bone_names_mesh=b.mesh.Address();
  context.bone_parents={1,-1,0}; context.ref_locals.resize(3);
  for (std::size_t i=0;i<3;++i) std::memcpy(context.ref_locals[i].data(),&b.local[i],sizeof(PackedTransform));
  Check(ResolveMotionIndices(context),"new tracks not mapped");
  Check(context.motion.bone_indices==std::vector<std::uint32_t>{0,1} && context.motion.root_bone_index==1 &&
        context.motion.offset_indices==std::vector<std::uint32_t>{0},"tracks use old skeleton indices");
  ApplyMotionPoseDirect(context);
  Check(std::abs(b.buffer0[0].rotation[2]-.70710678)<1e-6 && std::abs(b.buffer1[0].translation[2]-17)<1e-6,
        "new main skeleton did not receive rotation and root/offset tracks");
  const auto saved_head_reference = context.ref_locals[0];
  const auto saved_root_reference = context.ref_locals[1];
  context.ref_locals[1][6] = 20.0;
  const std::string blender_document=R"json({
    "kind":"better-pose-motion","frameCount":1,"fps":30,
    "bones":{"Root":[[0,0,0,1]],"Bip001-Head":[[0,0,0.70710678,0.70710678]]},
    "rootBone":"Root","rootTranslationUnit":"cm-delta",
    "rootTranslation":[[0,0,5]],"boneOffsets":{"Bip001-Head":[[0,0,12]]}
  })json";
  Check(LoadMotionDocument(context,blender_document,"motion.bpmotion",context.motion_load_epoch) &&
        ResolveMotionIndices(context),
        "Blender cm-delta motion did not load");
  ApplyMotionPoseDirect(context);
  Check(std::abs(b.buffer1[0].translation[2]-37)<1e-6,
        "Blender cm-delta root was not added to the reference root");
  context.blender_active.store(true);
  const std::array<double, 3> blender_root{11.0, 22.0, 33.0};
  for (std::size_t axis{}; axis != 3; ++axis)
    context.requested_root_offset[axis].store(blender_root[axis]);
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    context.bone_angles.assign(3, {0.0, 0.0, 0.0});
  }
  context.pose_override_enabled.store(true);
  ApplyPoseOverridesDirect(context);
  for (std::size_t axis{}; axis != 3; ++axis) {
    Check(std::abs(context.motion_applied_offset[axis].load() -
                   blender_root[axis]) < 1e-6 &&
              std::abs(context.motion_authored_offset[axis].load() -
                       blender_root[axis]) < 1e-6,
          "Blender root motion was not published to the camera track");
  }
  context.blender_active.store(false);
  context.ref_locals[0] = saved_head_reference;
  context.ref_locals[1] = saved_root_reference;
  const auto local_pose_document=nlohmann::json::parse(R"json({
    "bones":[], "rootOffset":[0,0,0],
    "localPose":{
      "Root":{"rotation":[0,0,0,1],"translation":[0,0,20],"scale":[1,1,1]},
      "Bip001-Head":{"rotation":[0,0,0,1],"translation":[0,0,12],"scale":[1,1,1]}
    }
  })json");
  Check(ApplyPoseDocument(context,local_pose_document),
        "library local-pose document did not load");
  context.pose_override_enabled.store(true,std::memory_order_release);
  ApplyPoseOverridesDirect(context);
  Check(std::abs(b.buffer1[0].translation[2]-32)<1e-6,
        "library local pose did not preserve exact bone translations");
  Check(nlohmann::json::parse(BuildPoseDocument(context)).contains("localPose"),
        "pose profile did not preserve the library local pose");
  std::map<std::string,std::array<double,12>> captured_pose;
  std::string capture_error;
  Check(CaptureCurrentLocalPose(context,captured_pose,capture_error) &&
        captured_pose.contains("Bip001-Head") &&
        std::abs(captured_pose["Bip001-Head"][6]-12)<1e-6,
        "current component pose was not captured as local bone transforms");
  // Character Studio puts attached costume/facial meshes on the same
  // SkeletalMeshActor. They must not become duplicate BetterPose targets.
  {
    Context target_context;
    target_context.core = &core;
    Block root_component, child_component, parent_class;
    Register(root_component);
    Register(child_component);
    Register(parent_class);
    parent_class.Set(native_layout.kObjectClassOffset, parent_class.Address());
    parent_class.Set(native_layout.kObjectNameOffset, std::uint32_t{1});
    child_component.Set(native_layout.kMeshAttachParentOffset, parent_class.Address());
    target_context.mesh_scan_class_cache[parent_class.Address()] =
        "SkeletalMeshComponent";
    Check(IsPoseTargetRootComponent(target_context, root_component.Address()),
          "unattached Character Studio body was rejected as a pose target");
    Check(!IsPoseTargetRootComponent(target_context, child_component.Address()),
          "attached Character Studio child mesh was exposed as a pose target");

    Block actor, actor_class, invalid_actor, invalid_actor_class, component_class;
    std::array<PackedTransform, 3> buffers{};
    std::array<PackedTransform, 3> invalid_buffers{};
    std::array<std::uint8_t, 96> entries{};
    std::array<std::uintptr_t, 1> chunks{};
    Register(actor);
    Register(actor_class);
    Register(invalid_actor);
    Register(invalid_actor_class);
    Register(component_class);
    Register(buffers);
    Register(invalid_buffers);
    Register(entries);
    Register(chunks);
    actor.Set(native_layout.kObjectClassOffset, actor_class.Address());
    invalid_actor.Set(native_layout.kObjectClassOffset, invalid_actor_class.Address());
    root_component.Set(native_layout.kObjectClassOffset, component_class.Address());
    child_component.Set(native_layout.kObjectClassOffset, component_class.Address());
    child_component.Set(native_layout.kMeshAttachParentOffset, root_component.Address());
    root_component.Set(native_layout.kObjectOuterOffset, actor.Address());
    child_component.Set(native_layout.kObjectOuterOffset, actor.Address());
    root_component.Set(native_layout.kMeshComponentSpaceBuffer0Offset,
                       reinterpret_cast<std::uintptr_t>(buffers.data()));
    root_component.Set(native_layout.kMeshComponentSpaceBuffer0Offset + 8,
                       std::int32_t{3});
    child_component.Set(native_layout.kMeshComponentSpaceBuffer0Offset,
                        reinterpret_cast<std::uintptr_t>(buffers.data()));
    child_component.Set(native_layout.kMeshComponentSpaceBuffer0Offset + 8,
                        std::int32_t{3});
    Block invalid_component;
    Register(invalid_component);
    invalid_component.Set(native_layout.kObjectClassOffset, component_class.Address());
    invalid_component.Set(native_layout.kObjectOuterOffset, invalid_actor.Address());
    invalid_component.Set(native_layout.kMeshComponentSpaceBuffer0Offset,
                          reinterpret_cast<std::uintptr_t>(invalid_buffers.data()));
    invalid_component.Set(native_layout.kMeshComponentSpaceBuffer0Offset + 8,
                          std::int32_t{3});
    const auto root_address = root_component.Address();
    std::memcpy(entries.data(), &root_address, sizeof(root_address));
    const auto child_address = child_component.Address();
    std::memcpy(entries.data() + 24, &child_address, sizeof(child_address));
    const auto invalid_address = invalid_component.Address();
    std::memcpy(entries.data() + 48, &invalid_address, sizeof(invalid_address));
    const auto invalid_actor_address = invalid_actor.Address();
    std::memcpy(entries.data() + 72, &invalid_actor_address,
                sizeof(invalid_actor_address));
    const auto entries_address = reinterpret_cast<std::uintptr_t>(entries.data());
    chunks[0] = entries_address;
    target_context.mesh_scan_class_cache[actor_class.Address()] =
        "SkeletalMeshActor";
    target_context.mesh_scan_class_cache[invalid_actor_class.Address()] =
        "HTSkeletalMeshActor_UID_123";
    target_context.mesh_scan_class_cache[component_class.Address()] =
        "SkeletalMeshComponent";
    target_context.object_registry = {
        reinterpret_cast<std::uintptr_t>(chunks.data()), 4, 4, 1, 1};
    target_context.pose_target_scan_requested.store(true);
    target_context.pose_target_scan_running = true;
    target_context.pose_target_scan_cursor = 0;
    target_context.pose_target_scan_results.clear();
    StepPoseTargetScan(target_context);
    Check(target_context.pose_targets.size() == 1 &&
              target_context.pose_targets.front().mesh == root_component.Address(),
          "Character Studio target scan did not keep only the actor root mesh");
  }
  amp::Settings amplitude;
  amplitude.master = 0.5F;
  context.motion_amplitude.Store(amplitude);
  context.motion_amplitude_enabled.store(true);
  context.motion_height_cm.store(10.0F);
  ApplyMotionPoseDirect(context);
  Check(std::abs(b.buffer0[0].rotation[2]-.38268343)<1e-6 &&
            std::abs(b.buffer1[0].translation[2]-27)<1e-6,
        "runtime amplitude/height were not applied to the final pose");
  context.motion_amplitude_enabled.store(false);
  ApplyMotionPoseDirect(context);
  Check(std::abs(b.buffer0[0].rotation[2]-.70710678)<1e-6 &&
            std::abs(b.buffer1[0].translation[2]-27)<1e-6,
        "disabled amplitude still changed the pose");
  context.motion_amplitude_enabled.store(true);
  context.motion_amplitude.Store(amp::Settings{});
  context.motion_height_cm.store(0.0F);
  ApplyMotionPoseDirect(context);
  Check(std::abs(b.buffer0[0].rotation[2]-.70710678)<1e-6 &&
            std::abs(b.buffer1[0].translation[2]-17)<1e-6,
        "runtime amplitude/height did not restore the converted motion");
  joint_cal::Settings joint_settings;
  joint_settings.enabled = true;
  joint_settings.joints["Bip001-Head"].ratio = 0.5F;
  {
    std::lock_guard<std::mutex> lock(context.motion_joint_mutex);
    context.motion_joint_calibration = joint_settings;
  }
  ApplyMotionPoseDirect(context);
  Check(std::abs(b.buffer0[0].rotation[2]-.38268343)<1e-6,
        "runtime joint ratio was not applied");
  joint_settings.enabled = false;
  {
    std::lock_guard<std::mutex> lock(context.motion_joint_mutex);
    context.motion_joint_calibration = joint_settings;
  }
  ApplyMotionPoseDirect(context);
  Check(std::abs(b.buffer0[0].rotation[2]-.70710678)<1e-6,
        "disabled joint calibration still changed the pose");
  {
    std::lock_guard<std::mutex> lock(context.motion_joint_mutex);
    context.motion_joint_calibration = joint_cal::Settings{};
  }

  const nlohmann::json legacy_defaults = nlohmann::json::parse(R"json({
    "bones":[],
    "motionTravelEnabled":true,
    "motionAmplitude":{"enabled":true,"master":0.8,"parts":{}},
    "motionJointCalibration":{"enabled":true,"linked":true,"joints":{}},
    "motionLimbCalibrationEnabled":true
  })json");
  Check(ApplyPoseDocument(context,legacy_defaults),
        "legacy calibration defaults did not load");
  Check(!context.motion_travel_enabled.load() &&
            !context.motion_amplitude_enabled.load() &&
            !context.motion_limb_enabled.load() &&
            !JointCalibrationEnabled(context),
        "legacy profiles did not migrate calibration masters to off");
  const auto migrated_settings=nlohmann::json::parse(BuildPoseDocument(context));
  Check(migrated_settings.value("motionCalibrationDefaultsVersion",0U)==2U,
        "migrated settings did not record the all-off default version");
  const nlohmann::json legacy_tuning = nlohmann::json::parse(R"json({
    "bones":[],
    "dynamicPhysicsTuning":{"air":1.0,
      "materialInertia":{"hair":1.2}}
  })json");
  Check(ApplyPoseDocument(context,legacy_tuning),
        "legacy dynamic tuning did not load");
  const auto migrated_tuning=context.secondary_tuning.Load();
  Check(std::abs(migrated_tuning.air)<1e-6F &&
            !migrated_tuning.extended_collision &&
            std::abs(migrated_tuning.material_inertia[
                         better_pose::secondary::MotionMaterialIndex(
                             better_pose::secondary::MotionMaterial::Hair)]-
                     1.2F)<1e-6F,
        "legacy default air response was not migrated off");

  const nlohmann::json saved_settings = nlohmann::json::parse(R"json({
    "motionCalibrationDefaultsVersion":2,
    "bones":[],"motionSpeed":1.5,"motionHeightCm":12,
    "motionIkMode":2,
    "dynamicPhysicsTuning":{"inertia":1.25,"air":0.5,"damping":1.1,
      "ribbonDamping":1.4,"gravity":0.8,"collision":1.3,
      "extendedCollision":true,"nativeKawaiiLimits":true,
      "boneConstraints":true,"selfCollision":true,
      "selfCollisionOuterPriority":true,
      "selfCollisionPairs":{"hairRibbon":false},
      "materialInertia":{"hair":1.2,"ribbon":0.7},
      "materialCollision":{"hair":0.8,"ribbon":0.25},
      "materialSelfRadius":{"hair":1.2,"ribbon":0.8},
      "materialSelfStrength":{"hair":1.1,"ribbon":0.9},
      "materialSelfDamping":{"hair":1.3,"ribbon":0.7}},
    "manualMotionMappings":{"左ひじ補助":"Bip001-Prop1"},
    "manualMotionParents":[{"source":"多段親","target":"Bip001-L-UpperArm",
      "anchor":"Bip001-Neck","inheritRotation":true,"inheritPosition":true}],
    "manualMotionControls":[{"source":"GripControl","kind":"grip","side":1,
      "finger":1,"inputAxis":2,"gain":-0.75,"paired":false}],
    "manualArmIkControls":[{"source":"ArmTarget","side":0,"paired":true,
      "gain":1.5,"maxBendDegrees":125,"followWrist":true}],
    "ignoredMotionTracks":["SpareTrack","SecondSpare"],
    "motionAmplitude":{"enabled":false,"master":1.25,"parts":{"head":0.5,"left_arm":1.75}},
    "motionJointCalibration":{"enabled":true,"linked":true,
      "joints":{"Bip001-Head":{"ratio":0.75,"offset":[1,2,3],"mirror":[false,true,false]}}}
  })json");
  Check(ApplyPoseDocument(context,saved_settings),"motion settings did not load");
  Check(std::abs(context.motion_speed.load()-1.5F)<1e-6 &&
            std::abs(context.motion_height_cm.load()-12.0F)<1e-6 &&
            context.motion_ik_mode.load()==2,
        "motion speed/height did not persist");
  const auto loaded_tuning=context.secondary_tuning.Load();
  std::cout << "loaded_tuning inertia=" << loaded_tuning.inertia
            << " air=" << loaded_tuning.air
            << " damping=" << loaded_tuning.damping
            << " ribbon_damping=" << loaded_tuning.ribbon_damping
            << " gravity=" << loaded_tuning.gravity
            << " collision=" << loaded_tuning.collision
            << " extended=" << loaded_tuning.extended_collision << '\n';
  Check(std::abs(loaded_tuning.inertia-1.25F)<1e-6F &&
            std::abs(loaded_tuning.air-.5F)<1e-6F &&
            std::abs(loaded_tuning.damping-1.1F)<1e-6F &&
            std::abs(loaded_tuning.ribbon_damping-1.4F)<1e-6F &&
            std::abs(loaded_tuning.gravity-.8F)<1e-6F &&
            std::abs(loaded_tuning.collision-1.3F)<1e-6F &&
            loaded_tuning.extended_collision && loaded_tuning.native_limits &&
            loaded_tuning.bone_constraints && loaded_tuning.self_collision &&
            loaded_tuning.self_collision_outer_priority &&
            !better_pose::secondary::SelfCollisionPairEnabled(
                loaded_tuning.self_collision_pairs,
                better_pose::secondary::MotionMaterial::Hair,
                better_pose::secondary::MotionMaterial::Ribbon) &&
            std::abs(loaded_tuning.self_collision_radius[
                         better_pose::secondary::MotionMaterialIndex(
                             better_pose::secondary::MotionMaterial::Hair)]-
                     1.2F)<1e-6F &&
            std::abs(loaded_tuning.self_collision_strength[
                         better_pose::secondary::MotionMaterialIndex(
                             better_pose::secondary::MotionMaterial::Hair)]-
                     1.1F)<1e-6F &&
            std::abs(loaded_tuning.self_collision_damping[
                         better_pose::secondary::MotionMaterialIndex(
                             better_pose::secondary::MotionMaterial::Ribbon)]-
                     .7F)<1e-6F &&
            std::abs(loaded_tuning.material_inertia[
                         better_pose::secondary::MotionMaterialIndex(
                             better_pose::secondary::MotionMaterial::Hair)]-
                     1.2F)<1e-6F &&
            std::abs(loaded_tuning.material_inertia[
                         better_pose::secondary::MotionMaterialIndex(
                             better_pose::secondary::MotionMaterial::Ribbon)]-
                     .7F)<1e-6F &&
            std::abs(loaded_tuning.material_collision[
                         better_pose::secondary::MotionMaterialIndex(
                             better_pose::secondary::MotionMaterial::Hair)]-
                     .8F)<1e-6F &&
            std::abs(loaded_tuning.material_collision[
                         better_pose::secondary::MotionMaterialIndex(
                             better_pose::secondary::MotionMaterial::Ribbon)]-
                     .25F)<1e-6F,
        "dynamic-bone tuning did not persist");
  {
    std::lock_guard<std::mutex> lock(context.motion_mapping_mutex);
    Check(context.manual_motion_mappings.at("左ひじ補助")=="Bip001-Prop1",
          "manual motion mapping did not persist");
    Check(context.manual_motion_parents.size()==1 &&
              context.manual_motion_parents.front().source=="多段親" &&
              context.manual_motion_parents.front().target=="Bip001-L-UpperArm" &&
              context.manual_motion_parents.front().anchor=="Bip001-Neck" &&
              context.manual_motion_parents.front().inherit_rotation &&
              context.manual_motion_parents.front().inherit_position,
          "manual motion parent did not persist");
    Check(context.manual_motion_finger_controls.size()==1 &&
              context.manual_motion_finger_controls.front().source=="GripControl" &&
              context.manual_motion_finger_controls.front().kind==
                  better_pose::mmd2bip::FingerControlKind::Grip &&
              context.manual_motion_finger_controls.front().side==1 &&
              context.manual_motion_finger_controls.front().finger==1 &&
              context.manual_motion_finger_controls.front().input_axis==2 &&
              std::abs(context.manual_motion_finger_controls.front().gain+.75F)<1e-6F &&
              !context.manual_motion_finger_controls.front().paired,
          "manual hand control did not persist");
    Check(context.manual_motion_arm_ik_controls.size()==1 &&
              context.manual_motion_arm_ik_controls.front().source=="ArmTarget" &&
              context.manual_motion_arm_ik_controls.front().paired &&
              std::abs(context.manual_motion_arm_ik_controls.front().gain-1.5F)<1e-6F &&
              std::abs(context.manual_motion_arm_ik_controls.front().max_bend_degrees-125.0F)<1e-6F &&
              context.manual_motion_arm_ik_controls.front().follow_wrist,
          "manual arm IK control did not persist");
    Check(context.ignored_motion_tracks.count("SpareTrack")==1 &&
              context.ignored_motion_tracks.count("SecondSpare")==1,
          "ignored motion tracks did not persist");
  }
  const auto loaded_amplitude=context.motion_amplitude.Load();
  Check(!context.motion_amplitude_enabled.load() &&
            std::abs(loaded_amplitude.master-1.25F)<1e-6 &&
            std::abs(loaded_amplitude.parts[static_cast<std::size_t>(amp::Part::Head)]-.5F)<1e-6 &&
            std::abs(loaded_amplitude.parts[static_cast<std::size_t>(amp::Part::LeftArm)]-1.75F)<1e-6,
        "motion amplitude did not persist");
  Check(JointCalibrationEnabled(context) &&
            !context.skeleton_overlay_enabled.load(std::memory_order_acquire),
        "joint calibration incorrectly forced the skeleton overlay");
  const auto rebuilt_settings=nlohmann::json::parse(BuildPoseDocument(context));
  joint_cal::Settings loaded_joints;
  {
    std::lock_guard<std::mutex> lock(context.motion_joint_mutex);
    loaded_joints = context.motion_joint_calibration;
  }
  const auto loaded_joint=loaded_joints.joints.find("Bip001-Head");
  Check(std::abs(rebuilt_settings["motionSpeed"].get<float>()-1.5F)<1e-6 &&
            std::abs(rebuilt_settings["motionHeightCm"].get<float>()-12.0F)<1e-6 &&
            rebuilt_settings["motionAmplitude"]["enabled"].get<bool>()==false &&
            rebuilt_settings["motionIkMode"].get<int>()==2 &&
            rebuilt_settings["dynamicPhysicsTuning"]["extendedCollision"]
                    .get<bool>() &&
            rebuilt_settings["dynamicPhysicsTuning"]["nativeKawaiiLimits"]
                    .get<bool>() &&
            rebuilt_settings["dynamicPhysicsTuning"]["boneConstraints"]
                    .get<bool>() &&
            rebuilt_settings["dynamicPhysicsTuning"]["selfCollision"]
                    .get<bool>() &&
            rebuilt_settings["dynamicPhysicsTuning"]
                    ["selfCollisionOuterPriority"]
                        .get<bool>() &&
            rebuilt_settings["dynamicPhysicsTuning"]["selfCollisionPairs"]
                    ["hairRibbon"]
                    .get<bool>() == false &&
            std::abs(rebuilt_settings["dynamicPhysicsTuning"]
                         ["materialSelfRadius"]["hair"]
                             .get<float>() -
                     1.2F) <
                1e-6F &&
            std::abs(rebuilt_settings["dynamicPhysicsTuning"]
                         ["materialSelfDamping"]["ribbon"]
                             .get<float>() -
                     .7F) <
                1e-6F &&
            std::abs(rebuilt_settings["dynamicPhysicsTuning"]["inertia"]
                         .get<float>()-1.25F)<1e-6F &&
            std::abs(rebuilt_settings["dynamicPhysicsTuning"]
                         ["materialInertia"]["hair"].get<float>()-1.2F)<1e-6F &&
            std::abs(rebuilt_settings["dynamicPhysicsTuning"]
                         ["materialCollision"]["ribbon"].get<float>()-.25F)<
                1e-6F &&
            rebuilt_settings["manualMotionMappings"].at("左ひじ補助")=="Bip001-Prop1" &&
            rebuilt_settings["manualMotionParents"].size()==1 &&
            rebuilt_settings["manualMotionParents"][0]["source"]=="多段親" &&
            rebuilt_settings["manualMotionParents"][0]["target"]=="Bip001-L-UpperArm" &&
            rebuilt_settings["manualMotionParents"][0]["anchor"]=="Bip001-Neck" &&
            rebuilt_settings["manualMotionParents"][0]["inheritRotation"]==true &&
            rebuilt_settings["manualMotionParents"][0]["inheritPosition"]==true &&
            rebuilt_settings["manualMotionControls"].size()==1 &&
            rebuilt_settings["manualMotionControls"][0]["source"]=="GripControl" &&
            rebuilt_settings["manualMotionControls"][0]["kind"]=="grip" &&
            rebuilt_settings["manualMotionControls"][0]["inputAxis"]==2 &&
            rebuilt_settings["manualArmIkControls"].size()==1 &&
            rebuilt_settings["manualArmIkControls"][0]["source"]=="ArmTarget" &&
            rebuilt_settings["manualArmIkControls"][0]["paired"]==true &&
            std::abs(rebuilt_settings["manualArmIkControls"][0]["gain"].get<float>()-1.5F)<1e-6F &&
            rebuilt_settings["ignoredMotionTracks"].size()==2 &&
            std::abs(rebuilt_settings["motionAmplitude"]["parts"]["head"].get<float>()-.5F)<1e-6 &&
            loaded_joints.enabled && loaded_joints.linked &&
            loaded_joint!=loaded_joints.joints.end() &&
            std::abs(loaded_joint->second.ratio-.75F)<1e-6 &&
            loaded_joint->second.offset_degrees==std::array<float,3>{1,2,3},
        "motion settings were not written back");
  const auto speed_before_invalid=context.motion_speed.load();
  const auto tuning_before_invalid=context.secondary_tuning.Load();
  const nlohmann::json invalid_settings = nlohmann::json::parse(R"json({
    "bones":[],"motionSpeed":1.75,"motionHeightCm":101,
    "dynamicPhysicsTuning":{"collision":9}
  })json");
  Check(!ApplyPoseDocument(context,invalid_settings),"invalid motion settings were accepted");
  Check(context.motion_speed.load()==speed_before_invalid,
        "invalid settings changed the validated speed");
  Check(context.secondary_tuning.Load().collision==tuning_before_invalid.collision,
        "invalid tuning changed the published values");

  // Resolving the same character must not repeatedly capture an already-frozen state.
  Check(ResolveLocalCharacter(context) && context.motion.indices_ready,"same-character resolve discarded binding");
  context.motion_playing=true;
  controller.Set(native_layout.kControllerPawnOffset,a.actor.Address());
  Check(ResolveLocalCharacter(context),"direct switch back failed");
  Check(!context.motion_loaded && !context.motion_playing,"switch while playing failed to unload");
  CheckOriginal(b,1,0x40,.75F,.8F,3);
  TakeOver(context);
  controller.Set(native_layout.kControllerPawnOffset,std::uintptr_t{});
  Check(!ResolveLocalCharacter(context),"missing pawn before unload");
  RestoreAll(context);
  CheckOriginal(a,0,0x20,1,1,2);
  Check(context.runtime.mesh==0,"unload during gap reactivated old target");
  const auto before_unload=context.motion_load_epoch;
  Check(LoadMotionDocument(context,document,"new.betterpose.json",before_unload),"manual unload setup");
  UnloadMotion(context);
  Check(!LoadMotionDocument(context,document,"late.betterpose.json",before_unload) && !context.motion_loaded,
        "manual unload accepted an older pending load");

  // Live hand-control preview must change only the displayed finger chain and disappear
  // immediately when the control editor is cancelled.
  controller.Set(native_layout.kControllerPawnOffset,a.actor.Address());
  Check(ResolveLocalCharacter(context),"preview fixture character resolve failed");
  TakeOver(context);
  std::array<PackedTransform,8> preview_local{},preview_components{},preview_back{};
  Register(preview_local); Register(preview_components); Register(preview_back);
  for(auto& bone:preview_local) bone.rotation[3]=1;
  const auto set_translation=[](PackedTransform& bone,float x,float y,float z) {
    bone.translation[0]=x; bone.translation[1]=y; bone.translation[2]=z;
  };
  set_translation(preview_local[1],0,0,0);
  set_translation(preview_local[2],2,0,0);
  set_translation(preview_local[3],1,0,0);
  set_translation(preview_local[4],2,1,0);
  set_translation(preview_local[5],1,0,0);
  set_translation(preview_local[6],2,-1,0);
  set_translation(preview_local[7],1,0,0);
  const std::array<std::uint32_t,3> preview_offsets{
      native_layout.kMeshLocalSpaceTransformsOffset,native_layout.kMeshComponentSpaceBuffer0Offset,
      native_layout.kMeshComponentSpaceBuffer1Offset};
  const std::array<std::uintptr_t,3> preview_arrays{
      reinterpret_cast<std::uintptr_t>(preview_local.data()),
      reinterpret_cast<std::uintptr_t>(preview_components.data()),
      reinterpret_cast<std::uintptr_t>(preview_back.data())};
  for(std::size_t i=0;i<3;++i) {
    a.mesh.Set(preview_offsets[i],preview_arrays[i]);
    a.mesh.Set(preview_offsets[i]+8,std::int32_t{8});
  }
  Check(ReadAnimationState(context) && ReadPoseArrays(context),"preview pose arrays unavailable");
  context.bone_names={"Root","Bip001-L-Hand","Bip001-L-Finger1",
                      "Bip001-L-Finger11","Bip001-L-Finger2",
                      "Bip001-L-Finger21","Bip001-L-Finger4",
                      "Bip001-L-Finger41"};
  context.bone_parents={-1,0,1,2,1,4,1,6};
  context.bone_names_mesh=a.mesh.Address(); context.bone_parents_ready=true;
  context.pose_base_ready=true; context.pose_base_mesh=a.mesh.Address();
  context.pose_base_locals.resize(8); context.ref_locals.resize(8);
  for(std::size_t i=0;i<8;++i) {
    std::memcpy(context.pose_base_locals[i].data(),&preview_local[i],
                sizeof(PackedTransform));
    std::memcpy(context.ref_locals[i].data(),&preview_local[i],
                sizeof(PackedTransform));
  }
  context.ref_pose_character=a.actor.Address();
  context.motion.bone_names={"Root"}; context.motion.bone_indices={0};
  context.motion.indices_ready=true; context.motion.bone_count=1;
  context.motion.frame_count=1; context.motion.first_frame=0;
  context.motion.rotations={0,0,0,1}; context.motion.has_root=false;
  context.motion.offset_names.clear(); context.motion.offset_indices.clear();
  context.motion.offsets.clear(); context.motion.offsets_ready=false;
  context.motion_loaded=true; context.motion_playing=false;
  context.motion_seconds=0; context.motion_display_seconds=0;
  context.motion_finger_preview_enabled.store(false,std::memory_order_release);
  ApplyMotionPoseDirect(context);
  const auto finger_rest=preview_components[2];
  context.motion_mapping_control_kind.store(0,std::memory_order_release);
  context.motion_mapping_control_side.store(0,std::memory_order_release);
  context.motion_mapping_control_finger.store(-1,std::memory_order_release);
  context.motion_mapping_control_gain.store(1.0F,std::memory_order_release);
  context.motion_mapping_control_paired.store(false,std::memory_order_release);
  context.motion_finger_preview_amount.store(.65F,std::memory_order_release);
  context.motion_finger_preview_enabled.store(true,std::memory_order_release);
  ApplyMotionPoseDirect(context);
  double preview_delta=0.0;
  for(std::size_t i=0;i<4;++i)
    preview_delta+=std::abs(preview_components[2].rotation[i]-finger_rest.rotation[i]);
  Check(preview_delta>.01,"live hand-control preview did not bend the finger");
  context.motion_finger_preview_enabled.store(false,std::memory_order_release);
  ApplyMotionPoseDirect(context);
  preview_delta=0.0;
  for(std::size_t i=0;i<4;++i)
    preview_delta+=std::abs(preview_components[2].rotation[i]-finger_rest.rotation[i]);
  Check(preview_delta<1e-6,"cancelling live preview did not restore the finger");

  std::array<PackedTransform,4> arm_local{},arm_components{},arm_back{};
  Register(arm_local); Register(arm_components); Register(arm_back);
  for(auto& bone:arm_local) bone.rotation[3]=1;
  set_translation(arm_local[1],0,0,10);
  set_translation(arm_local[2],5,0,0);
  set_translation(arm_local[3],5,0,0);
  const std::array<std::uintptr_t,3> arm_arrays{
      reinterpret_cast<std::uintptr_t>(arm_local.data()),
      reinterpret_cast<std::uintptr_t>(arm_components.data()),
      reinterpret_cast<std::uintptr_t>(arm_back.data())};
  for(std::size_t i=0;i<3;++i) {
    a.mesh.Set(preview_offsets[i],arm_arrays[i]);
    a.mesh.Set(preview_offsets[i]+8,std::int32_t{4});
  }
  Check(ReadAnimationState(context) && ReadPoseArrays(context),"arm IK pose arrays unavailable");
  context.bone_names={"Root","Bip001-L-UpperArm","Bip001-L-Forearm","Bip001-L-Hand"};
  context.bone_parents={-1,0,1,2}; context.bone_names_mesh=a.mesh.Address();
  context.pose_base_ready=true; context.pose_base_mesh=a.mesh.Address();
  context.pose_base_locals.resize(4); context.ref_locals.resize(4);
  for(std::size_t i=0;i<4;++i) {
    std::memcpy(context.pose_base_locals[i].data(),&arm_local[i],sizeof(PackedTransform));
    std::memcpy(context.ref_locals[i].data(),&arm_local[i],sizeof(PackedTransform));
  }
  context.motion=Context::MotionTrack{};
  context.motion.bone_names={"Root"}; context.motion.bone_indices={0};
  context.motion.indices_ready=false; context.motion.bone_count=1;
  context.motion.frame_count=1; context.motion.first_frame=0;
  context.motion.rotations={0,0,0,1};
  Context::MotionTrack::ArmIkTarget arm_target;
  arm_target.side="L"; arm_target.hand_name="Bip001-L-Hand";
  arm_target.follow_wrist=false; arm_target.max_bend_degrees=150;
  arm_target.offsets={0,3,0}; arm_target.rotations={0,0,0,1};
  context.motion.arm_ik_targets.push_back(std::move(arm_target));
  context.motion_loaded=true; Check(ResolveMotionIndices(context),"arm IK indices did not resolve");
  context.motion_arm_preview_enabled.store(false,std::memory_order_release);
  ApplyMotionPoseDirect(context);
  double arm_delta=0.0;
  for(std::size_t i=0;i<4;++i) {
    arm_delta+=std::abs(arm_components[1].rotation[i]-(i==3?1.0:0.0));
    arm_delta+=std::abs(arm_components[2].rotation[i]-(i==3?1.0:0.0));
  }
  Check(arm_delta>.02,"arm IK wrist target did not bend the target arm");

  CheckExtraAnimationMode();
  CheckNativeCapsuleRuntime();
  CheckKawaiiLimitDecode();
  {
    Context motion_slots;
    motion_slots.runtime.character = a.actor.Address();
    motion_slots.runtime.mesh = a.mesh.Address();
    motion_slots.runtime.external_target = true;
    motion_slots.bone_names = {"Root", "Bip001-Spine", "Bip001-Head"};
    motion_slots.bone_parents = {-1, 0, 1};
    motion_slots.bone_parents_ready = true;
    motion_slots.bone_parents_mesh = a.mesh.Address();
    motion_slots.bone_parents_count = 3;
    motion_slots.pose_base_locals.assign(3, {});
    motion_slots.ref_locals = motion_slots.pose_base_locals;
    motion_slots.pose_base_ready = true;
    motion_slots.pose_base_mesh = a.mesh.Address();
    motion_slots.motion.frame_count = 1;
    motion_slots.motion_file = "entity-a.betterpose.json";
    motion_slots.motion_loaded.store(true);
    ParkCurrentEntityMotion(motion_slots, a.mesh.Address());
    Check(!motion_slots.motion_loaded.load() &&
              motion_slots.entity_motion_states.contains(a.mesh.Address()) &&
              motion_slots.entity_motion_states[a.mesh.Address()].file ==
                  "entity-a.betterpose.json",
          "selected entity motion was not parked independently");

    motion_slots.runtime.character = b.actor.Address();
    motion_slots.runtime.mesh = b.mesh.Address();
    motion_slots.bone_names = {"Root", "Bip001-Spine", "Bip001-Head"};
    motion_slots.bone_parents = {-1, 0, 1};
    motion_slots.bone_parents_ready = true;
    motion_slots.bone_parents_mesh = b.mesh.Address();
    motion_slots.bone_parents_count = 3;
    motion_slots.pose_base_locals.assign(3, {});
    motion_slots.ref_locals = motion_slots.pose_base_locals;
    motion_slots.pose_base_ready = true;
    motion_slots.pose_base_mesh = b.mesh.Address();
    motion_slots.motion.frame_count = 1;
    motion_slots.motion_file = "entity-b.betterpose.json";
    motion_slots.motion_loaded.store(true);
    ParkCurrentEntityMotion(motion_slots, b.mesh.Address());
    motion_slots.runtime.character = a.actor.Address();
    motion_slots.runtime.mesh = a.mesh.Address();
    Check(RestoreEntityMotion(motion_slots, a.mesh.Address()) &&
              motion_slots.motion_file == "entity-a.betterpose.json" &&
              motion_slots.entity_motion_states.contains(b.mesh.Address()),
          "independent entity motion slots were not restored");
  }
  {
    Context multi_motion;
    multi_motion.core = &core;
    multi_motion.objects = &objects;
    multi_motion.object_registry = context.object_registry;
    Character selected(reinterpret_cast<std::uintptr_t>(vtable.data()), 0, 0x20,
                        1, 1, 0);
    Character first(reinterpret_cast<std::uintptr_t>(vtable.data()), 0, 0x10,
                    1, 1, 0);
    Character second(reinterpret_cast<std::uintptr_t>(vtable.data()), 0, 0x10,
                     1, 1, 0);
    multi_motion.runtime.character = selected.actor.Address();
    multi_motion.runtime.mesh = selected.mesh.Address();
    multi_motion.runtime.external_target = true;
    multi_motion.local_mesh = 0;
    multi_motion.bone_names = {"Root", "Bip001-Spine", "Bip001-Head"};
    multi_motion.bone_parents = {-1, 0, 1};
    multi_motion.bone_parents_ready = true;
    multi_motion.bone_parents_mesh = selected.mesh.Address();
    multi_motion.bone_parents_count = 3;
    const auto identity_local = [] {
      std::vector<std::array<double, 12>> result(3);
      for (auto &value : result) {
        value[3] = 1.0;
        value[8] = value[9] = value[10] = 1.0;
      }
      return result;
    };
    multi_motion.pose_base_locals = identity_local();
    multi_motion.ref_locals = identity_local();
    multi_motion.pose_base_ready = true;
    multi_motion.pose_base_mesh = selected.mesh.Address();
    const auto add_motion = [&](Character &character, const float angle) {
      Context::EntityMotionState state;
      state.runtime.character = character.actor.Address();
      state.runtime.mesh = character.mesh.Address();
      state.bone_names = {"Root", "Bip001-Spine", "Bip001-Head"};
      state.bone_parents = {-1, 0, 1};
      state.bone_parents_ready = true;
      state.bone_parents_count = 3;
      state.pose_base_locals = identity_local();
      state.ref_locals = identity_local();
      state.track.fps = 30.0;
      state.track.frame_count = 1;
      state.track.bone_count = 1;
      state.track.bone_names = {"Root"};
      const float half = angle * 0.5F;
      state.track.rotations = {
          0.0F, 0.0F, std::sin(half), std::cos(half)};
      state.track.bone_indices = {0};
      state.track.indices_ready = true;
      state.file = "entity-" + std::to_string(character.mesh.Address());
      multi_motion.entity_motion_states[character.mesh.Address()] =
          std::move(state);
    };
    add_motion(first, 0.2F);
    add_motion(second, 0.5F);
    StepInactiveEntityMotions(multi_motion);
    Check(std::abs(first.buffer0[0].rotation[2]) > 1e-4 &&
              std::abs(second.buffer0[0].rotation[2]) > 1e-4 &&
              std::abs(first.buffer0[0].rotation[2] -
                       second.buffer0[0].rotation[2]) > 1e-4,
          "independent inactive entity motions did not both drive their meshes");
    first.buffer0.fill(PackedTransform{});
    first.buffer1.fill(PackedTransform{});
    second.buffer0.fill(PackedTransform{});
    second.buffer1.fill(PackedTransform{});
    Check(ApplyInactiveEntityTick(multi_motion, first.mesh.Address()) &&
              std::abs(first.buffer0[0].rotation[2]) > 1e-4 &&
              std::abs(first.buffer1[0].rotation[2]) > 1e-4,
          "a parked entity's post-tick motion was not written back");
    Check(std::abs(second.buffer0[0].rotation[2]) < 1e-6 &&
              multi_motion.runtime.mesh == selected.mesh.Address() &&
              multi_motion.entity_motion_states[first.mesh.Address()]
                      .track.frame_count == 1,
          "a single parked entity tick leaked state into the selected entity");

    multi_motion.pose_targets = {
        {selected.actor.Address(), selected.mesh.Address(), "Selected"},
        {first.actor.Address(), first.mesh.Address(), "First"},
        {second.actor.Address(), second.mesh.Address(), "Second"}};
    multi_motion.requested_camera_follow_target.store(first.mesh.Address());
    StepCameraFollowTarget(multi_motion);
    Check(multi_motion.camera_follow_target.load() == first.mesh.Address(),
          "camera follow target request was not applied");
    multi_motion.runtime.mesh = second.mesh.Address();
    StepCameraFollowTarget(multi_motion);
    Check(multi_motion.camera_follow_target.load() == first.mesh.Address(),
          "selecting another pose target changed the camera follow target");

    const double applied[3] = {30.0, 40.0, 50.0};
    const double authored[3] = {60.0, 70.0, 80.0};
    multi_motion.runtime.mesh = first.mesh.Address();
    PublishRootOffsets(multi_motion, applied, authored);
    multi_motion.runtime.mesh = second.mesh.Address();
    const double other_applied[3] = {1.0, 2.0, 3.0};
    const double other_authored[3] = {4.0, 5.0, 6.0};
    PublishRootOffsets(multi_motion, other_applied, other_authored);
    double camera_applied[3]{};
    double camera_authored[3]{};
    ReadCameraRootOffsets(multi_motion, camera_applied, camera_authored);
    Check(camera_applied[0] == applied[0] &&
              camera_applied[1] == applied[1] &&
              camera_applied[2] == applied[2] &&
              camera_authored[0] == authored[0] &&
              camera_authored[1] == authored[1] &&
              camera_authored[2] == authored[2],
          "another entity overwrote the camera target's root offsets");

    multi_motion.camera.last_frame = 420.0;
    multi_motion.camera_loaded.store(true);
    Check(std::abs(SharedEntityMotionDuration(multi_motion) - 14.0) < 1e-6,
          "the shared motion timeline ignored the camera VMD duration");

    multi_motion.pose_targets.clear();
    multi_motion.entity_motion_states.erase(first.mesh.Address());
    multi_motion.runtime.mesh = selected.mesh.Address();
    StepCameraFollowTarget(multi_motion);
    Check(multi_motion.camera_follow_target.load() == selected.mesh.Address(),
          "an invalid camera follow target did not fall back to the selected mesh");

    Context camera_units;
    const auto set_leg_locals = [&camera_units](const double calf,
                                                const double foot) {
      camera_units.bone_names = {"Bip001-L-Calf", "Bip001-L-Foot"};
      camera_units.ref_locals.assign(2, {});
      camera_units.ref_locals[0][4] = calf;
      camera_units.ref_locals[1][5] = foot;
      camera_units.pose_base_locals = camera_units.ref_locals;
    };
    constexpr std::uintptr_t first_camera_mesh = 0x7101;
    constexpr std::uintptr_t second_camera_mesh = 0x7102;
    constexpr std::uintptr_t inactive_camera_mesh = 0x7103;
    camera_units.runtime.mesh = first_camera_mesh;
    camera_units.camera_follow_target.store(first_camera_mesh);
    set_leg_locals(40.0, 30.0);
    RefreshCameraUnit(camera_units);
    Check(std::abs(camera_units.camera_target_mmd_unit_cm.load() -
                   70.0 / kReferenceLegUnits) < 1e-6 &&
              camera_units.camera_unit_mesh.load() == first_camera_mesh,
          "camera unit was not measured from the first camera target");

    set_leg_locals(48.0, 36.0);
    InvalidateCameraUnit(camera_units);
    RefreshCameraUnit(camera_units);
    Check(std::abs(camera_units.camera_target_mmd_unit_cm.load() -
                   84.0 / kReferenceLegUnits) < 1e-6,
          "reloading a camera reused the previous character-height cache");

    camera_units.runtime.mesh = second_camera_mesh;
    SetCameraFollowTarget(camera_units, second_camera_mesh);
    set_leg_locals(54.0, 42.0);
    RefreshCameraUnit(camera_units);
    Check(std::abs(camera_units.camera_target_mmd_unit_cm.load() -
                   96.0 / kReferenceLegUnits) < 1e-6 &&
              camera_units.camera_unit_mesh.load() == second_camera_mesh,
          "switching camera targets reused the first character's camera unit");

    Context::EntityMotionState inactive_camera_state;
    inactive_camera_state.bone_names = {"Bip001-L-Calf", "Bip001-L-Foot"};
    inactive_camera_state.ref_locals.assign(2, {});
    inactive_camera_state.ref_locals[0][4] = 60.0;
    inactive_camera_state.ref_locals[1][5] = 50.0;
    camera_units.entity_motion_states[inactive_camera_mesh] =
        std::move(inactive_camera_state);
    SetCameraFollowTarget(camera_units, inactive_camera_mesh);
    RefreshCameraUnit(camera_units);
    Check(std::abs(camera_units.camera_target_mmd_unit_cm.load() -
                   110.0 / kReferenceLegUnits) < 1e-6 &&
              camera_units.camera_unit_mesh.load() == inactive_camera_mesh,
          "an independently selected camera target used the selected pose target's unit");

    multi_motion.motion_load_epoch = 7;
    multi_motion.motion_playing.store(true);
    multi_motion.motion_seconds.store(2.5);
    multi_motion.motion_display_seconds.store(2.5);
    const std::string shared_document =
        R"({"kind":"better-pose-motion","fps":30,"frameCount":1,"bones":{"Root":[[0,0,0,1]]}})";
    Check(LoadMotionDocument(multi_motion, shared_document, "shared.json", 7, true),
          "a shared multiplayer motion document did not load");
    Check(multi_motion.motion_playing.load() &&
              std::abs(multi_motion.motion_seconds.load() - 2.5) < 1e-6 &&
              std::abs(multi_motion.motion_display_seconds.load() - 2.5) < 1e-6,
          "loading one multiplayer entity reset the shared playback clock");

    Context camera_cut;
    better_pose::mmd2bip::VmdCameraKey first_key;
    first_key.frame = 0;
    first_key.distance = 10.0;
    first_key.view_angle = 30;
    better_pose::mmd2bip::VmdCameraKey middle_key = first_key;
    middle_key.frame = 10;
    middle_key.position[0] = 10.0;
    better_pose::mmd2bip::VmdCameraKey last_key = first_key;
    last_key.frame = 20;
    last_key.position[0] = 20.0;
    camera_cut.camera.keys = {first_key, middle_key, last_key};
    camera_cut.camera.manual_cut_frames = {10};
    camera_cut.camera_frame.store(5.0);
    camera_cut.camera_cut_mode.store(2);
    double camera_position[3]{};
    double camera_rotation[3]{};
    double camera_distance{};
    double camera_fov{};
    Check(SampleCameraTrack(camera_cut, camera_position, camera_rotation,
                            &camera_distance, &camera_fov) &&
              std::abs(camera_position[0]) < 1e-6,
          "manual camera cut did not hold the previous shot");
    camera_cut.camera_cut_mode.store(1);
    Check(SampleCameraTrack(camera_cut, camera_position, camera_rotation,
                            &camera_distance, &camera_fov) &&
              camera_position[0] > 0.0 && camera_position[0] < 10.0,
          "continuous camera mode did not interpolate the cut interval");
    const double anchor_feet[3] = {100.0, 200.0, 30.0};
    const double anchor_centre[3] = {100.0, 200.0, 120.0};
    StoreCameraAnchor(camera_cut, anchor_feet, anchor_centre);
    Check(camera_cut.camera_anchored.load() &&
              std::abs(camera_cut.camera_anchor_feet[0].load() - 100.0) <
                  1e-6 &&
              std::abs(camera_cut.camera_anchor_feet[2].load() - 30.0) <
                  1e-6 &&
              std::abs(camera_cut.camera_anchor_centre[2].load() - 120.0) <
                  1e-6,
          "moving the fixed camera origin did not update its world bounds");
    camera_cut.camera_yaw.store(0.0);
    camera_cut.camera_origin_follow.store(false);
    camera_cut.camera_fixed_origin_offset_cm[0].store(10.0);
    camera_cut.camera_fixed_origin_offset_cm[1].store(20.0);
    camera_cut.camera_fixed_origin_offset_cm[2].store(30.0);
    double adjusted_feet[3] = {100.0, 200.0, 30.0};
    double adjusted_centre[3] = {100.0, 200.0, 120.0};
    ApplyCameraFixedOriginOffset(camera_cut, adjusted_feet, adjusted_centre);
    Check(std::abs(adjusted_feet[0] - 110.0) < 1e-6 &&
              std::abs(adjusted_feet[1] - 230.0) < 1e-6 &&
              std::abs(adjusted_feet[2] - 50.0) < 1e-6 &&
              std::abs(adjusted_centre[2] - 140.0) < 1e-6,
          "fixed origin three-axis adjustment was not applied");
    double gizmo_centre[3]{};
    Check(CameraFixedOriginCentre(camera_cut, gizmo_centre) &&
              std::abs(gizmo_centre[0] - 110.0) < 1e-6 &&
              std::abs(gizmo_centre[1] - 230.0) < 1e-6 &&
              std::abs(gizmo_centre[2] - 140.0) < 1e-6,
          "fixed origin gizmo did not use the adjusted world position");

    const auto tracked_camera_at_anchor = [&](const double anchor_x) {
      Context tracked;
      tracked.core = &core;
      tracked.camera_enabled.store(true);
      tracked.camera_loaded.store(true);
      tracked.camera_origin_follow.store(true);
      tracked.camera_yaw.store(0.0);
      tracked.mmd_unit_cm.store(10.0);
      tracked.camera_logged_second.store(2.0);
      for (int axis{}; axis != 3; ++axis) {
        const double current = axis == 1 ? 20.0 : 0.0;
        tracked.motion_applied_offset[axis].store(current);
        tracked.motion_authored_offset[axis].store(current);
        tracked.camera_character_root_world[axis].store(
            axis == 0 ? 100.0 : axis == 1 ? 200.0 : 0.0);
        tracked.camera_anchor_root_world[axis].store(
            axis == 0 ? anchor_x : axis == 1 ? 200.0 : 0.0);
        tracked.camera_anchor_applied_offset[axis].store(0.0);
      }
      tracked.camera_character_root_valid.store(true);
      tracked.camera_anchor_root_valid.store(true);
      double fov = 60.0;
      Register(fov);
      std::array<double, 3> location{};
      std::array<double, 3> rotation{};
      const double feet[3]{};
      const double centre[3]{0.0, 0.0, 100.0};
      const double game_location[3]{0.0, -400.0, 140.0};
      const double game_rotation[3]{};
      const double file_position[3]{0.0, 10.0, 0.0};
      const double file_rotation[3]{};
      DriveCameraTrack(
          tracked, feet, centre, true, game_location, game_rotation,
          file_position, file_rotation, 20.0, 60.0,
          reinterpret_cast<std::uintptr_t>(location.data()),
          reinterpret_cast<std::uintptr_t>(rotation.data()),
          reinterpret_cast<std::uintptr_t>(&fov), location.data(),
          rotation.data());
      return location;
    };
    const auto anchor_a = tracked_camera_at_anchor(0.0);
    const auto anchor_b = tracked_camera_at_anchor(500.0);
    Check(anchor_a == anchor_b,
          "camera track distance changed with the frame used to create its anchor");
  }
  {
    Context keyframe_drive;
    keyframe_drive.core = &core;
    keyframe_drive.objects = &objects;
    keyframe_drive.object_registry = context.object_registry;
    Character selected(reinterpret_cast<std::uintptr_t>(vtable.data()), 0, 0x20,
                       1, 1, 0);
    Character parked(reinterpret_cast<std::uintptr_t>(vtable.data()), 0, 0x10,
                     1, 1, 0);
    keyframe_drive.runtime.character = selected.actor.Address();
    keyframe_drive.runtime.mesh = selected.mesh.Address();
    keyframe_drive.runtime.external_target = true;
    keyframe_drive.local_mesh = 0;
    keyframe_drive.bone_names = {"Root", "Bip001-Spine", "Bip001-Head"};
    keyframe_drive.bone_parents = {-1, 0, 1};
    keyframe_drive.bone_parents_ready = true;
    keyframe_drive.bone_parents_mesh = selected.mesh.Address();
    keyframe_drive.bone_parents_count = 3;
    const auto identity_local = [] {
      std::vector<std::array<double, 12>> result(3);
      for (auto &value : result) {
        value[3] = 1.0;
        value[8] = value[9] = value[10] = 1.0;
      }
      return result;
    };
    keyframe_drive.pose_base_locals = identity_local();
    keyframe_drive.ref_locals = identity_local();
    keyframe_drive.pose_base_ready = true;
    keyframe_drive.pose_base_mesh = selected.mesh.Address();

    Context::EntityMotionState state;
    state.runtime.character = parked.actor.Address();
    state.runtime.mesh = parked.mesh.Address();
    state.bone_names = {"Root", "Bip001-Spine", "Bip001-Head"};
    state.bone_parents = {-1, 0, 1};
    state.bone_parents_ready = true;
    state.bone_parents_count = 3;
    state.pose_base_locals = identity_local();
    state.ref_locals = identity_local();
    better_pose::keyframes::Key key;
    key.frame = 0;
    key.has_pose = true;
    key.bones.push_back({"Bip001-Spine", 0.0, 0.0, 35.0});
    state.keyframe_track.Set(std::move(key));
    keyframe_drive.entity_motion_states[parked.mesh.Address()] =
        std::move(state);
    keyframe_drive.keyframe_frame.store(0.0);

    Check(ApplyInactiveEntityTick(keyframe_drive, parked.mesh.Address()),
          "a parked entity keyframe did not reach its post-tick writer");
    const auto &rotation = parked.buffer0[1].rotation;
    Check(std::abs(rotation[0]) + std::abs(rotation[1]) +
                  std::abs(rotation[2]) >
              1e-4 &&
              keyframe_drive.runtime.mesh == selected.mesh.Address() &&
              keyframe_drive.entity_motion_states[parked.mesh.Address()]
                      .keyframe_track.Size() == 1,
          "a parked entity keyframe leaked state or lost its track");
    parked.buffer0.fill(PackedTransform{});
    parked.buffer1.fill(PackedTransform{});
    FinalizeEntityPoses(keyframe_drive);
    const auto &finalized = parked.buffer0[1].rotation;
    Check(std::abs(finalized[0]) + std::abs(finalized[1]) +
                  std::abs(finalized[2]) >
              1e-4,
          "post-GameTick finalization did not restore an inactive keyframe");
  }
  {
    Context shared;
    shared.core = &core;
    shared.shared_entity_pose_enabled.store(true);
    Character source(reinterpret_cast<std::uintptr_t>(vtable.data()), 0, 0x20,
                     1, 1, 2);
    Character target(reinterpret_cast<std::uintptr_t>(vtable.data()), 0, 0x10,
                     1, 1, 0);
    shared.runtime.mesh = source.mesh.Address();
    shared.runtime.external_target = true;
    shared.local_mesh = 0;
    shared.pose_targets = {
        {source.actor.Address(), source.mesh.Address(), "Source"},
        {target.actor.Address(), target.mesh.Address(), "Target"}};
    shared.motion_loaded.store(true);
    Check(SharedEntityPoseRequested(shared),
          "shared pose was not requested for a second compatible target");
    shared.shared_entity_pose_source = source.mesh.Address();
    shared.bone_names = {"Root", "Bip001-Spine", "Bip001-Head"};
    std::array<PackedTransform, 3> shared_pose{};
    for (auto &transform : shared_pose)
      transform.rotation[3] = 1.0;
    shared_pose[0].rotation[2] = 0.25;
    shared.shared_entity_pose_count = 3;
    shared.shared_entity_pose_compatibility[target.mesh.Address()] = true;
    shared.shared_entity_pose_source_indices[target.mesh.Address()] = {0, 1, 2};
    shared.shared_entity_pose_bytes.resize(sizeof(shared_pose));
    std::memcpy(shared.shared_entity_pose_bytes.data(), shared_pose.data(),
                sizeof(shared_pose));
    Check(ApplySharedEntityPose(shared, target.mesh.Address()),
          "shared pose could not be written to the second target");
    Check(std::abs(target.buffer0[0].rotation[2] - 0.25) < 1e-6 &&
              std::abs(target.buffer1[0].rotation[2] - 0.25) < 1e-6,
          "shared pose did not reach both target pose buffers");
    Check((target.mesh.Get<std::uint8_t>(native_layout.kMeshAnimationFlagsOffset) &
           static_cast<std::uint8_t>(1U << native_layout.kAnimationFlagPauseAnimsBit)) != 0 &&
              target.mesh.Get<std::uint8_t>(native_layout.kMeshAnimationModeOffset) ==
                  kAnimationModeCustom,
          "shared pose did not pause and switch the second target");
    RestoreSharedEntityPoses(shared);
    Check(target.mesh.Get<std::uint8_t>(native_layout.kMeshAnimationFlagsOffset) == 0x10 &&
              target.mesh.Get<std::uint8_t>(native_layout.kMeshAnimationModeOffset) == 0,
          "shared pose did not restore the second target animation state");
    Character remapped_target(reinterpret_cast<std::uintptr_t>(vtable.data()),
                              0, 0x10, 1, 1, 0);
    std::array<PackedTransform, 2> remapped_source{};
    remapped_source[0].rotation[2] = 0.25;
    remapped_source[1].rotation[2] = 0.5;
    shared.bone_names = {"SharedA", "SharedB"};
    shared.shared_entity_pose_count = 2;
    shared.shared_entity_pose_bytes.resize(sizeof(remapped_source));
    std::memcpy(shared.shared_entity_pose_bytes.data(),
                remapped_source.data(), sizeof(remapped_source));
    shared.shared_entity_pose_compatibility[remapped_target.mesh.Address()] =
        true;
    shared.shared_entity_pose_source_indices[remapped_target.mesh.Address()] =
        {1, -1, 0};
    Check(ApplySharedEntityPose(shared, remapped_target.mesh.Address(), false),
          "name-mapped shared pose was not applied across bone counts");
    Check(std::abs(remapped_target.buffer0[0].rotation[2] - 0.5) < 1e-6 &&
              std::abs(remapped_target.buffer0[2].rotation[2] - 0.25) < 1e-6,
          "name-mapped shared pose used the wrong source bones");
    shared.runtime.bound_character = source.actor.Address();
    shared.runtime.bound_mesh = source.mesh.Address();
    shared.runtime.character = source.actor.Address();
    shared.runtime.mesh = source.mesh.Address();
    shared.runtime.external_target = true;
    shared.motion.indices_ready = true;
    shared.motion_loaded.store(true);
    shared.shared_entity_pose_compatibility[target.mesh.Address()] = false;
    shared.shared_entity_pose_compatibility[target.mesh.Address()] = true;
    BindRuntimeCharacter(shared, target.actor.Address(),
                         target.mesh.Address(), true);
    Check(!shared.motion_loaded.load() &&
              shared.entity_motion_states.contains(source.mesh.Address()),
          "external target switch did not park the previous entity MMD");
    Check(shared.runtime.bound_mesh == target.mesh.Address() &&
              shared.runtime.external_target,
          "compatible external target switch did not bind the new target");
    shared.motion_loaded.store(false);
    Check(!SharedEntityPoseRequested(shared),
          "shared pose remained active without motion or timeline data");
  }
  // Character Studio models are ordinary SkeletalMeshActor bodies. BetterPose
  // must be able to bind one as a pose target without treating its actor as an
  // ACharacter or writing the local pawn's root-motion field.
  Character studio(reinterpret_cast<std::uintptr_t>(vtable.data()), 2, 0x10, 1, 7, 0);
  context.pose_targets = {
      {a.actor.Address(), a.mesh.Address(), "Player"},
      {studio.actor.Address(), studio.mesh.Address(), "Studio model"}};
  context.selected_pose_target_mesh = studio.mesh.Address();
  Check(ResolveLocalCharacter(context) &&
            context.runtime.external_target &&
            context.runtime.character == studio.actor.Address() &&
            context.runtime.mesh == studio.mesh.Address(),
        "Character Studio target was not selected");
  Check(ReadAnimationState(context) && ReadPoseArrays(context),
        "Character Studio target pose buffers were not readable");
  {
    std::lock_guard<std::mutex> lock(context.pose_angles_mutex);
    context.bone_parents = {-1, 0, 1};
    context.bone_angles.assign(3, {0.0, 0.0, 0.0});
    context.bone_angles[0][2] = 15.0;
    context.pose_base_ready = false;
    context.pose_base_mesh = 0;
  }
  context.pose_override_enabled.store(true, std::memory_order_release);
  ApplyPoseOverridesDirect(context);
  Check(std::abs(studio.buffer0[0].rotation[0]) > 0.01 ||
            std::abs(studio.buffer0[0].rotation[1]) > 0.01 ||
            std::abs(studio.buffer0[0].rotation[2]) > 0.01,
        "Character Studio target did not receive a pose override");
  Check(ApplyRootMotion(context, true, 0.25F) &&
            studio.actor.Get<float>(native_layout.kCharacterAnimRootMotionScaleOffset) == 7.0F,
        "external target touched the pawn-only root-motion field");
  context.selected_pose_target_mesh = 0;
  Check(ResolveLocalCharacter(context) &&
            !context.runtime.external_target &&
            context.runtime.character == a.actor.Address(),
        "returning to the local player after an external target failed");
  std::cout << "PASS automatic unload, paused/playing switches, multi-entity Character Studio target binding, extra animation restore, modular Kawaii capsule geometry and decoding, transient gap, stale conversion rejection and explicit reload\n";
}
