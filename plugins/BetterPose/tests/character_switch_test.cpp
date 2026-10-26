#include "../plugin.cpp"

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
AnomalyStatusV1 ANOMALY_CALL ReadMemory(void*,std::uintptr_t address,AnomalyMutableByteSpanV1 bytes) {
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
  if (function==&functions[Mode]) std::memcpy(bytes+kMeshAnimationModeOffset,parameters,1);
  if (function==&functions[Lod]) std::memcpy(bytes+kMeshForcedLodModelOffset,parameters,4);
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
    actor.Set(kCharacterMeshOffset,mesh.Address()); actor.Set(kCharacterAnimRootMotionScaleOffset,root);
    mesh.Set(0,vtable); mesh.Set(kMeshAnimationModeOffset,mode); mesh.Set(kMeshAnimationFlagsOffset,flags);
    mesh.Set(kMeshGlobalAnimRateScaleOffset,rate); mesh.Set(kMeshForcedLodModelOffset,lod);
    mesh.Set(kMeshAnimScriptInstanceOffset,anim.Address());
    anim.Set(kAnimInstanceUseMultiThreadedUpdateOffset,std::uint8_t{0xff});
    for (auto& b:local) b.rotation[3]=1;
    const std::array<std::uint32_t,3> offsets{kMeshLocalSpaceTransformsOffset,kMeshComponentSpaceBuffer0Offset,kMeshComponentSpaceBuffer1Offset};
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
  Check(c.mesh.Get<std::uint8_t>(kMeshAnimationModeOffset)==mode,"previous character animation mode not restored");
  Check(c.mesh.Get<std::uint8_t>(kMeshAnimationFlagsOffset)==flags,"previous character stayed paused");
  Check(c.mesh.Get<float>(kMeshGlobalAnimRateScaleOffset)==rate,"rate restored to wrong character");
  Check(c.actor.Get<float>(kCharacterAnimRootMotionScaleOffset)==root,"root scale restored to wrong actor");
  Check(c.mesh.Get<std::int32_t>(kMeshForcedLodModelOffset)==lod,"LOD restored to wrong mesh");
  Check(c.anim.Get<std::uint8_t>(kAnimInstanceUseMultiThreadedUpdateOffset)==0xff,"old anim instance not restored");
}
void CheckExtraAnimationMode() {
  Block accessory;
  Register(accessory);
  constexpr std::uint8_t kOriginalMode = 0;
  constexpr std::uint8_t kOriginalFlags = 0xff;
  accessory.Set(kMeshAnimationModeOffset, kOriginalMode);
  accessory.Set(kMeshAnimationFlagsOffset, kOriginalFlags);
  Context context;
  AnomalyCoreServiceV1 core{};
  core.struct_size = sizeof(core);
  core.read_memory = ReadMemory;
  core.write_memory = WriteMemory;
  context.core = &core;
  Context::ExtraMesh extra;
  extra.object = accessory.Address();
  Check(EnsureExtraPoseAnimationMode(context, extra, true),
        "accessory animation did not pause");
  Check(accessory.Get<std::uint8_t>(kMeshAnimationModeOffset) ==
            kAnimationModeCustom &&
            extra.animation_mode_paused,
        "accessory animation mode was not switched to custom");
  const std::uint8_t paused_flags =
      accessory.Get<std::uint8_t>(kMeshAnimationFlagsOffset);
  Check((paused_flags & (1U << kAnimationFlagPauseAnimsBit)) != 0 &&
            (paused_flags & (1U << kAnimationFlagEnableAnimationBit)) == 0,
        "accessory animation flags were not paused");
  Check(EnsureExtraPoseAnimationMode(context, extra, true),
        "accessory animation pause was not idempotent");
  accessory.Set(kMeshAnimationModeOffset, kOriginalMode);
  accessory.Set(kMeshAnimationFlagsOffset, kOriginalFlags);
  Check(EnsureExtraPoseAnimationMode(context, extra, true) &&
            accessory.Get<std::uint8_t>(kMeshAnimationModeOffset) ==
                kAnimationModeCustom &&
            (accessory.Get<std::uint8_t>(kMeshAnimationFlagsOffset) &
             (1U << kAnimationFlagPauseAnimsBit)) != 0,
        "accessory animation pause was not reasserted");
  Check(EnsureExtraPoseAnimationMode(context, extra, false),
        "accessory animation did not resume");
  Check(accessory.Get<std::uint8_t>(kMeshAnimationModeOffset) ==
            kOriginalMode &&
            accessory.Get<std::uint8_t>(kMeshAnimationFlagsOffset) ==
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
  std::array<std::uint8_t, kKawaiiCapsuleLimitSize> raw{};
  const float radius = 15.209F;
  const float length = 4.735F;
  const float sphere_radius = 17.576F;
  std::memcpy(raw.data() + kKawaiiLimitRadiusOffset, &radius, sizeof(radius));
  std::memcpy(raw.data() + kKawaiiLimitLengthOffset, &length, sizeof(length));
  std::memcpy(raw.data() + kKawaiiLimitSphereRadiusOffset, &sphere_radius,
              sizeof(sphere_radius));
  KawaiiCapsuleLimitSnapshot decoded;
  ReadKawaiiLimitGeometry(raw.data(), decoded);
  Check(std::abs(decoded.radius - 15.209) < 1e-5 &&
            std::abs(decoded.length - 4.735) < 1e-5 &&
            std::abs(decoded.sphere_radius - 17.576) < 1e-5,
        "Kawaii capsule floats were not decoded with their native width");

  const float no_radius = 0.0F;
  std::memcpy(raw.data() + kKawaiiLimitRadiusOffset, &no_radius,
              sizeof(no_radius));
  ReadKawaiiLimitGeometry(raw.data(), decoded);
  Check(std::abs(decoded.radius - 17.576) < 1e-5,
        "Kawaii capsule sphere radius fallback was not applied");
}
}

int main() {
  using namespace fixture;
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
  AnomalyCoreServiceV1 core{}; core.struct_size=sizeof(core); core.read_memory=ReadMemory; core.write_memory=WriteMemory;
  context.core=&core;
  AnomalyUe5ObjectsServiceV1 objects{}; objects.struct_size=sizeof(objects); objects.find_exact=Find; context.objects=&objects;
  std::array<std::uintptr_t,80> vtable{}; Register(vtable);
  vtable[kProcessEventVtableSlot]=reinterpret_cast<std::uintptr_t>(Event);
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
  world.Set(kWorldGameInstanceOffset,instance.Address()); instance.Set(kGameInstanceLocalPlayersOffset,players.Address());
  players.Set(0,player.Address()); player.Set(kLocalPlayerControllerOffset,controller.Address());
  controller.Set(kControllerPawnOffset,a.actor.Address());
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
  controller.Set(kControllerPawnOffset,std::uintptr_t{});
  Check(!ResolveLocalCharacter(context) && context.runtime.mesh==0,"missing pawn remained active");
  Check(destroyed==0,"transient gap dropped accessories");
  Check(context.motion_loaded,"transient gap unloaded motion");
  controller.Set(kControllerPawnOffset,a.actor.Address());
  Check(ResolveLocalCharacter(context) && context.motion_loaded && context.motion.indices_ready,
        "same character returning after a gap unloaded motion");
  controller.Set(kControllerPawnOffset,std::uintptr_t{});
  Check(!ResolveLocalCharacter(context),"second gap remained active");
  controller.Set(kControllerPawnOffset,b.actor.Address());
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
  Check(b.mesh.Get<std::uint8_t>(kMeshAnimationModeOffset)==kAnimationModeCustom &&
        (b.mesh.Get<std::uint8_t>(kMeshAnimationFlagsOffset)&(1U<<kAnimationFlagPauseAnimsBit)),"new body not taken over");
  context.bone_names={"Bip001-Head","Root","tip"}; context.bone_names_mesh=b.mesh.Address();
  context.bone_parents={1,-1,0}; context.ref_locals.resize(3);
  for (std::size_t i=0;i<3;++i) std::memcpy(context.ref_locals[i].data(),&b.local[i],sizeof(PackedTransform));
  Check(ResolveMotionIndices(context),"new tracks not mapped");
  Check(context.motion.bone_indices==std::vector<std::uint32_t>{0,1} && context.motion.root_bone_index==1 &&
        context.motion.offset_indices==std::vector<std::uint32_t>{0},"tracks use old skeleton indices");
  ApplyMotionPoseDirect(context);
  Check(std::abs(b.buffer0[0].rotation[2]-.70710678)<1e-6 && std::abs(b.buffer1[0].translation[2]-17)<1e-6,
        "new main skeleton did not receive rotation and root/offset tracks");
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
  controller.Set(kControllerPawnOffset,a.actor.Address());
  Check(ResolveLocalCharacter(context),"direct switch back failed");
  Check(!context.motion_loaded && !context.motion_playing,"switch while playing failed to unload");
  CheckOriginal(b,1,0x40,.75F,.8F,3);
  TakeOver(context);
  controller.Set(kControllerPawnOffset,std::uintptr_t{});
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
  controller.Set(kControllerPawnOffset,a.actor.Address());
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
      kMeshLocalSpaceTransformsOffset,kMeshComponentSpaceBuffer0Offset,
      kMeshComponentSpaceBuffer1Offset};
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
  std::cout << "PASS automatic unload, paused/playing switches, extra animation restore, modular Kawaii capsule geometry and decoding, transient gap, stale conversion rejection and explicit reload\n";
}
