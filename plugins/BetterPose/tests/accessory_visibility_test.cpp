#include "../plugin.cpp"

#include <cstdlib>
#include <iostream>

namespace fixture {
enum Function { IsVisible, SetVisibility, SetRelative, SetBone, Destroy, Count };
int functions[Count]{};
bool visibility_query_available{true};
struct Component {
  std::uintptr_t vtable{};
  bool visible{true};
  bool hidden_in_game{};
  bool child_visible{};
  unsigned queries{}, visibility_writes{}, destroy_calls{};
};
void Check(bool value, const char* message) {
  if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void __fastcall Event(void* object, void* function, void* parameters) {
  auto& component=*static_cast<Component*>(object);
  auto* bytes=static_cast<std::uint8_t*>(parameters);
  if (function==&functions[IsVisible]) {
    // The verified native skinned-component getter checks both flags.
    bytes[0]=component.visible && !component.hidden_in_game;
    ++component.queries;
  } else if (function==&functions[SetVisibility]) {
    component.visible=bytes[0]!=0;
    if (bytes[1]) component.child_visible=component.visible;
    ++component.visibility_writes;
  } else if (function==&functions[SetBone]) {
    Check(bytes[112]==1,"replacement bones must use component space");
  } else if (function==&functions[Destroy]) {
    std::uintptr_t caller{};
    std::memcpy(&caller,bytes,sizeof(caller));
    Check(caller==reinterpret_cast<std::uintptr_t>(object),"destroy must pass self");
    ++component.destroy_calls;
  }
}
AnomalyStatusV1 ANOMALY_CALL ReadMemory(void*,std::uintptr_t address,AnomalyMutableByteSpanV1 bytes) {
  std::memcpy(bytes.data,reinterpret_cast<void*>(address),bytes.size);
  return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL WriteMemory(void*,std::uintptr_t,AnomalyByteSpanV1) {
  Check(false,"visibility handling must not write raw source buffers");
  return anomaly::sdk::Ok();
}
AnomalyStatusV1 ANOMALY_CALL Find(void*,AnomalyStringViewV1 name,AnomalyGenerationHandleV1* handle) {
  const std::string_view path(name.data,name.size);
  const std::array<std::string_view,Count> paths{
      kFunctionSceneIsVisiblePath,kFunctionSceneSetVisibilityPath,
      kFunctionSceneSetRelativeTransformPath,kFunctionPoseableSetBoneTransformByNamePath,
      kFunctionActorComponentDestroyPath};
  for (std::size_t i=0;i<paths.size();++i) {
    if (path!=paths[i] || (i==IsVisible && !visibility_query_available)) continue;
    handle->id=i+1;
    return anomaly::sdk::Ok();
  }
  return Status(ANOMALY_STATUS_V1_NOT_FOUND);
}
}

int main() {
  using namespace fixture;
  Context context;
  AnomalyCoreServiceV1 core{};
  core.struct_size=sizeof(core); core.read_memory=ReadMemory; core.write_memory=WriteMemory;
  context.core=&core;
  AnomalyUe5ObjectsServiceV1 objects{};
  objects.struct_size=sizeof(objects); objects.find_exact=Find; context.objects=&objects;
  std::array<std::uintptr_t,80> vtable{};
  vtable[kProcessEventVtableSlot]=reinterpret_cast<std::uintptr_t>(Event);
  Component source{reinterpret_cast<std::uintptr_t>(vtable.data())};
  Component replacement{source.vtable};
  std::array<std::uint8_t,Count*24> slots{};
  for (std::size_t i=0;i<Count;++i) {
    const auto address=reinterpret_cast<std::uintptr_t>(&functions[i]);
    std::memcpy(slots.data()+i*24,&address,sizeof(address));
  }
  auto chunk=reinterpret_cast<std::uintptr_t>(slots.data());
  context.object_registry.items=reinterpret_cast<std::uintptr_t>(&chunk);
  context.object_registry.count=Count; context.object_registry.num_chunks=1;
  context.runtime.character=1;
  context.extra_meshes.emplace_back();
  auto& extra=context.extra_meshes.back();
  extra.object=reinterpret_cast<std::uintptr_t>(&source); extra.asset=1;

  source.visible=false;
  Check(!EnsurePoseableAccessory(context,extra),"invisible accessory acquired a replacement");
  Check(source.queries==1 && !extra.poseable_attempted && extra.poseable_component==0,
        "hidden accessory was created or permanently latched");
  RestoreExtraMeshes(context);
  Check(!source.visible && source.visibility_writes==0,"idle restore revealed hidden accessory");

  source.visible=true; source.hidden_in_game=true;
  Check(!EnsurePoseableAccessory(context,extra),"game-hidden hat acquired a visible replacement");
  Check(source.queries==2 && extra.poseable_component==0,"hidden-in-game query was bypassed");
  RestoreExtraMeshes(context);
  Check(source.hidden_in_game && source.visibility_writes==0,"game-hidden state was overwritten");

  visibility_query_available=false;
  Check(!EnsurePoseableAccessory(context,extra) && !extra.poseable_attempted,
        "unavailable getter must not guess visibility");
  visibility_query_available=true;

  // A visible source is suppressed only after its replacement has a valid pose.
  source.hidden_in_game=false;
  extra.poseable_component=reinterpret_cast<std::uintptr_t>(&replacement);
  extra.poseable_active=true; extra.poseable_attempted=true; extra.poseable_socket_bone=0;
  extra.bone_count=1; extra.bone_fnames.resize(1);
  PackedTransform identity{}; identity.rotation[3]=1;
  extra.bind_world.resize(1);
  std::memcpy(extra.bind_world[0].data(),&identity,sizeof(identity));
  std::memcpy(extra.poseable_socket_relative.data(),&identity,sizeof(identity));
  Check(DrivePoseableSocketPose(context,extra,{identity}),"visible accessory pose failed");
  Check(!source.visible && replacement.visible && !source.child_visible,
        "replacement handoff changed a hidden child");
  Check(extra.poseable_source_visibility_changed,"source visibility ownership not recorded");
  Check(EnsurePoseableAccessory(context,extra),"our own suppression disabled an active replacement");
  Check(source.queries==2,"active source was mistaken for a game-hidden accessory");
  RestoreExtraMeshes(context); RestoreExtraMeshes(context);
  Check(source.visible && !source.child_visible && source.visibility_writes==2,
        "unload must restore only its source, exactly once");
  Check(replacement.destroy_calls==1 && !extra.poseable_source_visibility_changed,
        "unload leaked a replacement or retained visibility ownership");

  // HiddenInGame may change while playback owns bVisible. Restoration must not erase it.
  extra.poseable_component=reinterpret_cast<std::uintptr_t>(&replacement);
  extra.poseable_active=true; extra.poseable_attempted=true;
  Check(DrivePoseableSocketPose(context,extra,{identity}),"second handoff failed");
  source.hidden_in_game=true;
  RestoreExtraMeshes(context);
  Check(source.visible && source.hidden_in_game && !source.child_visible,
        "unload overwrote the game's hidden state");

  // A partially created replacement has no authority over source visibility.
  source.visible=false;
  const auto writes=source.visibility_writes;
  extra.poseable_component=reinterpret_cast<std::uintptr_t>(&replacement);
  RestoreExtraMeshes(context);
  Check(!source.visible && source.visibility_writes==writes,
        "failed creation forcibly revealed the source");
  std::cout << "PASS hidden hats, unavailable getter, visible handoff, hidden children, repeated restore and failure cleanup\n";
}
