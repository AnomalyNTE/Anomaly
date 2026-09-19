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
}

int main() {
  using namespace fixture;
  Context context;
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
  std::cout << "PASS automatic unload, paused/playing switches, transient gap, stale conversion rejection and explicit reload\n";
}
