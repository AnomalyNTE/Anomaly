#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ue5.h"
#include "camera_blur_fix_profile.hpp"

#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
#include <string_view>

namespace {

using namespace camera_blur_fix_profile;

// One installed hook: the handle keeps it alive, the original is the trampoline
// the detour forwards to.
struct HookSlot final {
  AnomalyGenerationHandleV1 handle{};
  std::uintptr_t original{};
  std::uintptr_t target{};
};

struct CameraLayout final {
  std::uintptr_t manager_class{};
  std::uint32_t game_instance{}, local_players{}, controller{}, manager{};
  // Fade/hide distance squared and the normal-camera fade/hide targets.
  std::array<std::uint32_t, 4> floats{};
  std::uint32_t pitch_curve{};
  bool ready{};
};

struct SavedCamera final {
  AnomalyGenerationHandleV1 handle{};
  std::uintptr_t address{};
  std::array<float, 4> floats{};
  std::uintptr_t pitch_curve{};
  bool applied{};
};

struct Context final {
  const AnomalyCoreServiceV1 *core{};
  const AnomalySignatureServiceV1 *signature{};
  const AnomalyUe5ObjectsServiceV1 *objects{};
  const AnomalyHookServiceV1 *hook{};
  const AnomalyUe5NamesServiceV1 *names{};
  CameraLayout camera_layout{};
  SavedCamera saved_camera{};
  std::uintptr_t registry{};
  std::uintptr_t g_world_address{};
  std::int64_t trace[8]{};
  HookSlot mask{};
  HookSlot fade{};
  double retry_elapsed{};
  bool logged_ready{};
  bool logged_retry{};
};

// Published for the detours, which can run on any game thread.
std::atomic<Context *> g_active{};

using MaskSetterFn = void(ANOMALY_CALL *)(void *, float);
using FadeSetterFn = void(ANOMALY_CALL *)(void *, float, float);

AnomalyStatusV1 Status(const std::uint32_t code,
                       const std::string_view message = {}) noexcept {
  return {code, 0, {message.data(), message.size()}};
}

template <typename Struct, typename Field>
bool HasField(const Struct *value, const std::size_t offset) noexcept {
  return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

template <typename Service>
const Service *Query(const AnomalyHostApiV1 *host, const char *id,
                     const std::uint32_t version) noexcept {
  return anomaly::sdk::Host(host).Query<Service>(id, version).get();
}

bool CoreReady(const AnomalyCoreServiceV1 *service) noexcept {
  return HasField<AnomalyCoreServiceV1,
                  decltype(AnomalyCoreServiceV1::write_memory)>(
             service, offsetof(AnomalyCoreServiceV1, write_memory)) &&
         service->read_memory != nullptr && service->write_memory != nullptr;
}

bool SignatureReady(const AnomalySignatureServiceV1 *service) noexcept {
  return HasField<AnomalySignatureServiceV1,
                  decltype(AnomalySignatureServiceV1::resolve)>(
             service, offsetof(AnomalySignatureServiceV1, resolve)) &&
         service->resolve != nullptr;
}

bool ObjectsReady(const AnomalyUe5ObjectsServiceV1 *service) noexcept {
  return HasField<AnomalyUe5ObjectsServiceV1,
                  decltype(AnomalyUe5ObjectsServiceV1::find_exact)>(
             service, offsetof(AnomalyUe5ObjectsServiceV1, find_exact)) &&
         service->find_exact != nullptr && service->snapshot_at != nullptr;
}

bool CameraServicesReady(const Context &context) noexcept {
  return HasField<AnomalyUe5NamesServiceV1,
                  decltype(AnomalyUe5NamesServiceV1::resolve_utf8)>(
             context.names, offsetof(AnomalyUe5NamesServiceV1, resolve_utf8)) &&
         context.names->resolve_utf8 != nullptr;
}

bool HookReady(const AnomalyHookServiceV1 *service) noexcept {
  return HasField<AnomalyHookServiceV1,
                  decltype(AnomalyHookServiceV1::end_callback)>(
             service, offsetof(AnomalyHookServiceV1, end_callback)) &&
         service->create != nullptr && service->release != nullptr &&
         service->begin_callback != nullptr && service->end_callback != nullptr;
}

void Log(Context &context, const std::uint32_t level,
         const std::string_view message) noexcept {
  if (context.core != nullptr && context.core->log != nullptr)
    context.core->log(context.core->user, level,
                      anomaly::sdk::StringView(message));
}

template <typename T>
bool Read(Context &context, const std::uintptr_t address, T &value) noexcept {
  if (!CoreReady(context.core) || address == 0) return false;
  AnomalyMutableByteSpanV1 destination{
      reinterpret_cast<std::uint8_t *>(&value), sizeof(value)};
  return context.core->read_memory(context.core->user, address, destination)
             .code == ANOMALY_STATUS_V1_OK;
}

template <typename T>
bool Write(Context &context, const std::uintptr_t address,
           const T &value) noexcept {
  return context.core->write_memory(
             context.core->user, address,
             {reinterpret_cast<const std::uint8_t *>(&value), sizeof(value)})
             .code == ANOMALY_STATUS_V1_OK;
}

bool ReadBytes(Context &context, const std::uintptr_t address, void *data,
               const std::size_t size) noexcept {
  if (!CoreReady(context.core) || address == 0 || data == nullptr) return false;
  AnomalyMutableByteSpanV1 destination{reinterpret_cast<std::uint8_t *>(data),
                                       size};
  return context.core->read_memory(context.core->user, address, destination)
             .code == ANOMALY_STATUS_V1_OK;
}

bool AddAddress(const std::uintptr_t base, const std::uint64_t offset,
                std::uintptr_t &result) noexcept {
  if (base == 0 || offset > (std::numeric_limits<std::uintptr_t>::max)() - base)
    return false;
  result = base + static_cast<std::uintptr_t>(offset);
  return true;
}

// Every detour forces the mask argument so the game can never fade the actor.
// The original implementation still runs, so materials, delegates and cached
// state stay consistent instead of being left at whatever value they had.
void ANOMALY_CALL MaskSetterDetour(void *self, float) noexcept {
  auto *context = g_active.load(std::memory_order_acquire);
  AnomalyGenerationHandleV1 lease{};
  bool leased = false;
  MaskSetterFn original = nullptr;
  try {
    if (context != nullptr && context->mask.handle.id != 0 &&
        HookReady(context->hook)) {
      leased = context->hook
                    ->begin_callback(context->hook->user,
                                     context->mask.handle, &lease)
                    .code == ANOMALY_STATUS_V1_OK;
      original = reinterpret_cast<MaskSetterFn>(context->mask.original);
    }
  } catch (...) {
  }
  try {
    if (original != nullptr) original(self, kVisibleMask);
  } catch (...) {
  }
  if (leased && context != nullptr && HookReady(context->hook)) {
    static_cast<void>(
        context->hook->end_callback(context->hook->user, lease));
  }
}

void ANOMALY_CALL FadeSetterDetour(void *self, float, float) noexcept {
  auto *context = g_active.load(std::memory_order_acquire);
  AnomalyGenerationHandleV1 lease{};
  bool leased = false;
  FadeSetterFn original = nullptr;
  try {
    if (context != nullptr && context->fade.handle.id != 0 &&
        HookReady(context->hook)) {
      leased = context->hook
                    ->begin_callback(context->hook->user,
                                     context->fade.handle, &lease)
                    .code == ANOMALY_STATUS_V1_OK;
      original = reinterpret_cast<FadeSetterFn>(context->fade.original);
    }
  } catch (...) {
  }
  try {
    if (original != nullptr) original(self, kVisibleMask, kVisibleMask);
  } catch (...) {
  }
  if (leased && context != nullptr && HookReady(context->hook)) {
    static_cast<void>(
        context->hook->end_callback(context->hook->user, lease));
  }
}

// GObjects registry, so an objects-service handle can be turned back into the
// UFunction address the hook service needs.
bool ResolveRegistry(Context &context) noexcept {
  if (context.registry != 0) return true;
  if (!SignatureReady(context.signature)) return false;
  const auto instruction = compatibility.Address(kGObjectsPattern);
  if (instruction == 0) return false;
  std::int32_t displacement{};
  if (!Read(context, instruction + kRipDisplacementOffset, displacement))
    return false;
  const auto resolved = static_cast<std::intptr_t>(instruction) +
                        kRipInstructionSize + displacement;
  if (resolved <= 0) return false;
  context.registry =
      static_cast<std::uintptr_t>(resolved) + native_layout.kGObjectsAddend;
  return true;
}

bool ResolveGWorld(Context &context) noexcept {
  if (context.g_world_address != 0) return true;
  const auto instruction = compatibility.Address(kGWorldPattern);
  if (instruction == 0) return false;
  std::int32_t displacement{};
  if (!Read(context, instruction + kRipDisplacementOffset, displacement))
    return false;
  const auto address = static_cast<std::intptr_t>(instruction) +
                       kRipInstructionSize + displacement;
  if (address <= 0) return false;
  context.g_world_address = static_cast<std::uintptr_t>(address);
  return true;
}

bool ObjectFromHandle(Context &context, const AnomalyGenerationHandleV1 handle,
                      std::uintptr_t &object) noexcept {
  object = 0;
  if (!ResolveRegistry(context) || handle.id == 0) return false;
  const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
  std::int32_t count{};
  std::uintptr_t chunks{};
  if (!Read(context, context.registry + native_layout.kObjectCountOffset, count) ||
      count <= 0 || static_cast<std::uint32_t>(count) > kMaximumObjectCount ||
      index >= static_cast<std::uint32_t>(count) ||
      !Read(context, context.registry + native_layout.kObjectChunksOffset, chunks) ||
      chunks == 0)
    return false;
  const std::uint32_t chunk_index = index >> native_layout.kObjectChunkBits;
  const std::uint32_t item_index =
      index & ((1U << native_layout.kObjectChunkBits) - 1U);
  std::uintptr_t chunk{};
  if (!Read(context, chunks + static_cast<std::uintptr_t>(chunk_index) *
                                    sizeof(std::uintptr_t),
             chunk) ||
      chunk == 0)
    return false;
  const auto item = chunk + static_cast<std::uintptr_t>(item_index) *
                                native_layout.kObjectItemStride;
  std::uintptr_t candidate{};
  std::uint32_t serial{};
  if (!Read(context, item + native_layout.kObjectItemPointerOffset, candidate) ||
      candidate == 0 ||
      !Read(context, item + native_layout.kObjectItemSerialOffset, serial) ||
      serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle))
    return false;
  object = candidate;
  return true;
}

bool NameEquals(Context &context, const std::uintptr_t address,
                const std::string_view expected) noexcept {
  std::uint32_t id{};
  char name[128]{};
  std::size_t size = sizeof(name);
  return Read(context, address, id) &&
         context.names->resolve_utf8(context.names->user, id, name, &size).code ==
             ANOMALY_STATUS_V1_OK &&
         size == expected.size() + 1 &&
         std::string_view(name, expected.size()) == expected;
}

bool FindType(Context &context, const std::string_view path,
              std::uintptr_t &address) noexcept {
  AnomalyGenerationHandleV1 handle{};
  return context.objects->find_exact(context.objects->user,
                                    anomaly::sdk::StringView(path), &handle).code ==
             ANOMALY_STATUS_V1_OK &&
         ObjectFromHandle(context, handle, address);
}

// Validate the reflected field's type and size before using its offset.
bool FindCameraField(Context &context, const std::uintptr_t type,
                     const std::string_view name,
                     const std::string_view field_type,
                     const std::int32_t size, std::uint32_t &offset) noexcept {
  std::uintptr_t field{};
  // Match the host reflection query's Profile-backed PropertyLink/FField chain.
  if (!Read(context, type + native_layout.kUStructPropertyLinkOffset, field)) return false;
  for (unsigned count{}; field != 0 && count < 2048; ++count) {
    if (NameEquals(context, field + native_layout.kFFieldNameOffset, name)) {
      std::uintptr_t field_class{};
      std::int32_t field_size{}, field_offset{};
      if (!Read(context, field + native_layout.kFFieldClassOffset, field_class) ||
          !NameEquals(context, field_class, field_type) ||
          !Read(context, field + native_layout.kFPropertyElementSizeOffset, field_size) ||
          field_size != size ||
          !Read(context, field + native_layout.kFPropertyOffsetOffset, field_offset) ||
          field_offset < 0)
        return false;
      offset = static_cast<std::uint32_t>(field_offset);
      return true;
    }
    if (!Read(context, field + native_layout.kFFieldNextOffset, field)) return false;
  }
  return false;
}

bool ResolveCameraLayout(Context &context) noexcept {
  if (context.camera_layout.ready) return true;
  if (!CameraServicesReady(context)) return false;
  CameraLayout layout{};
  std::uintptr_t world{}, instance{}, player{}, controller{}, settings{}, instant{};
  if (!FindType(context, "/Script/Engine.World", world) ||
      !FindType(context, "/Script/Engine.GameInstance", instance) ||
      !FindType(context, "/Script/Engine.Player", player) ||
      !FindType(context, "/Script/Engine.PlayerController", controller) ||
      !FindType(context, "/Script/HTGame.HTPlayerCameraManager",
                layout.manager_class) ||
      !FindType(context, "/Script/HTGame.CameraSettings", settings) ||
      !FindType(context, "/Script/HTGame.CameraInstantSetting", instant))
    return false;
  std::uint32_t normal{}, fade{}, hide{}, target{};
  if (!FindCameraField(context, world, "OwningGameInstance", "ObjectProperty", 8,
                       layout.game_instance) ||
      !FindCameraField(context, instance, "LocalPlayers", "ArrayProperty", 16,
                       layout.local_players) ||
      !FindCameraField(context, player, "PlayerController", "ObjectProperty", 8,
                       layout.controller) ||
      !FindCameraField(context, controller, "PlayerCameraManager", "ObjectProperty",
                       8, layout.manager) ||
      !FindCameraField(context, layout.manager_class, "PlayerFadeDistanceSquare",
                       "FloatProperty", 4, layout.floats[0]) ||
      !FindCameraField(context, layout.manager_class, "PlayerHideDistanceSquare",
                       "FloatProperty", 4, layout.floats[1]) ||
      !FindCameraField(context, layout.manager_class, "PlayerPitchFadeCurve",
                       "ObjectProperty", 8, layout.pitch_curve) ||
      !FindCameraField(context, layout.manager_class, "NormalCameraSettings",
                       "StructProperty", 0x570, normal) ||
      !FindCameraField(context, settings, "PlayerFadeDistance", "StructProperty", 8,
                       fade) ||
      !FindCameraField(context, settings, "PlayerHideDistance", "StructProperty", 8,
                       hide) ||
      !FindCameraField(context, instant, "TargetValue", "FloatProperty", 4, target))
    return false;
  layout.floats[2] = normal + fade + target;
  layout.floats[3] = normal + hide + target;
  layout.ready = true;
  context.camera_layout = layout;
  Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO,
      "anti-blur player camera layout validated through reflection");
  return true;
}

bool RestoreCamera(Context &context) noexcept {
  auto &saved = context.saved_camera;
  if (saved.address == 0) return true;
  std::uintptr_t live{};
  // A camera from a previous world can already have been collected.
  if (!ObjectFromHandle(context, saved.handle, live) || live != saved.address) {
    saved = {};
    return true;
  }
  bool ok = true;
  for (std::size_t i{}; i < saved.floats.size(); ++i)
    ok = Write(context, live + context.camera_layout.floats[i], saved.floats[i]) &&
         ok;
  ok = Write(context, live + context.camera_layout.pitch_curve,
             saved.pitch_curve) && ok;
  if (ok) saved = {};
  return ok;
}

bool ApplyCamera(Context &context) noexcept {
  const auto &layout = context.camera_layout;
  if (!layout.ready) return false;
  std::uintptr_t world{}, instance{}, players{}, player{}, controller{}, manager{};
  std::int32_t player_count{};
  if (!ResolveGWorld(context) ||
      !Read(context, context.g_world_address, world) || world == 0 ||
      !Read(context, world + layout.game_instance, instance) || instance == 0 ||
      !Read(context, instance + layout.local_players, players) || players == 0 ||
      !Read(context, instance + layout.local_players + 8, player_count) ||
      player_count <= 0 || !Read(context, players, player) || player == 0 ||
      !Read(context, player + layout.controller, controller) || controller == 0 ||
      !Read(context, controller + layout.manager, manager) || manager == 0) {
    return RestoreCamera(context);
  }
  std::int32_t index{};
  AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
  std::uintptr_t live{};
  if (!Read(context, manager + native_layout.kObjectInternalIndexOffset, index) || index < 0 ||
      context.objects->snapshot_at(context.objects->user,
                                   static_cast<std::uint32_t>(index),
                                   &snapshot).code != ANOMALY_STATUS_V1_OK ||
      !ObjectFromHandle(context, snapshot.handle, live) || live != manager)
    return false;
  auto &saved = context.saved_camera;
  if (saved.handle.id != snapshot.handle.id ||
      saved.handle.generation != snapshot.handle.generation) {
    if (!RestoreCamera(context)) return false;
    std::uintptr_t type{};
    if (!Read(context, manager + native_layout.kObjectClassOffset, type)) return false;
    unsigned depth{};
    while (type != 0 && type != layout.manager_class && depth++ < 64)
      if (!Read(context, type + native_layout.kUStructSuperOffset, type)) return false;
    if (type != layout.manager_class) return false;
    SavedCamera next{};
    for (std::size_t i{}; i < next.floats.size(); ++i)
      if (!Read(context, manager + layout.floats[i], next.floats[i]) ||
          !std::isfinite(next.floats[i]))
        return false;
    if (!Read(context, manager + layout.pitch_curve, next.pitch_curve))
      return false;
    next.address = manager;
    next.handle = snapshot.handle;
    saved = next;
  }
  // Restore the old player path alongside the NPC hooks. Clear the source
  // targets too, or normal-camera updates can repopulate the distance caches.
  bool ok = true;
  for (const auto offset : layout.floats)
    ok = Write(context, manager + offset, 0.0F) && ok;
  ok = Write(context, manager + layout.pitch_curve, std::uintptr_t{}) && ok;
  if (ok && !saved.applied) {
    char message[256]{};
    std::snprintf(
        message, sizeof(message),
        "anti-blur player camera applied manager=0x%llX fade=0 hide=0 pitch=null "
        "offsets=0x%X,0x%X,0x%X,0x%X,0x%X; fade speed unchanged",
        static_cast<unsigned long long>(manager), layout.floats[0],
        layout.floats[1], layout.floats[2], layout.floats[3], layout.pitch_curve);
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
    saved.applied = true;
  }
  return ok;
}

bool LooksLikeFunction(const std::uintptr_t address) noexcept {
  return address > 0x10000 && (address & 0x3U) == 0;
}

// Resolves a native UFUNCTION implementation: reflection gives the UFunction,
// the exec thunk's last "mov rcx, rdi; call" gives the implementation.
bool ResolveNativeTarget(Context &context, const std::string_view path,
                         const std::uint32_t expected_params,
                         const std::uint16_t expected_param_bytes,
                         std::uintptr_t &native) noexcept {
  native = 0;
  if (!ObjectsReady(context.objects)) return false;
  AnomalyGenerationHandleV1 handle{};
  if (context.objects
          ->find_exact(context.objects->user,
                       anomaly::sdk::StringView(path), &handle)
          .code != ANOMALY_STATUS_V1_OK ||
      handle.id == 0)
    return false;
  std::uintptr_t function{};
  if (!ObjectFromHandle(context, handle, function) || function == 0)
    return false;
  std::uint8_t num_params{};
  std::uint16_t param_bytes{};
  std::uintptr_t thunk{};
  if (!Read(context, function + native_layout.kUFunctionNumParmsOffset, num_params) ||
      !Read(context, function + native_layout.kUFunctionParmsSizeOffset, param_bytes) ||
      num_params != expected_params || param_bytes != expected_param_bytes ||
      !Read(context, function + native_layout.kUFunctionExecOffset, thunk) || thunk == 0)
    return false;
  std::uint8_t code[kThunkScanBytes]{};
  if (!ReadBytes(context, thunk, code, sizeof(code))) {
    context.trace[0] = 7;
    return false;
  }
  for (std::uint32_t index = 0; index + sizeof(kNativeCallMarker) + 4U <
                                    kThunkScanBytes;
       ++index) {
    bool matches = true;
    for (std::uint32_t byte = 0; byte < sizeof(kNativeCallMarker); ++byte) {
      if (code[index + byte] != kNativeCallMarker[byte]) {
        matches = false;
        break;
      }
    }
    if (!matches) continue;
    const auto displacement_address =
        thunk + static_cast<std::uintptr_t>(index) +
        sizeof(kNativeCallMarker);
    std::int32_t displacement{};
    if (!Read(context, displacement_address, displacement)) continue;
    const auto call_end = displacement_address + sizeof(displacement);
    std::uintptr_t candidate{};
    if (displacement < 0) {
      const auto magnitude =
          static_cast<std::uint64_t>(-static_cast<std::int64_t>(displacement));
      if (magnitude >= call_end) continue;
      candidate = call_end - static_cast<std::uintptr_t>(magnitude);
    } else {
      candidate = call_end + static_cast<std::uintptr_t>(displacement);
    }
    if (!LooksLikeFunction(candidate)) continue;
    std::uint8_t prologue{};
    if (!Read(context, candidate, prologue) || prologue == 0xCC ||
        prologue == 0xC3 || prologue == 0x00)
      continue;
    native = candidate;
    return true;
  }
  context.trace[0] = 8;
  return false;
}

bool InstallHook(Context &context, const std::uintptr_t target, void *detour,
                 const std::string_view label, HookSlot &slot) noexcept {
  if (slot.handle.id != 0 && slot.target == target) return true;
  if (!HookReady(context.hook) || target == 0) return false;
  AnomalyHookRequestV1 request{sizeof(request)};
  request.kind = ANOMALY_HOOK_V1_FUNCTION;
  request.target = target;
  request.detour = detour;
  request.label = anomaly::sdk::StringView(label);
  std::uintptr_t original{};
  AnomalyGenerationHandleV1 handle{};
  if (context.hook->create(context.hook->user, &request, &original, &handle)
          .code != ANOMALY_STATUS_V1_OK ||
      handle.id == 0 || original == 0)
    return false;
  slot.handle = handle;
  slot.original = original;
  slot.target = target;
  return true;
}

void ReleaseHook(Context &context, HookSlot &slot) noexcept {
  if (slot.handle.id == 0) return;
  if (HookReady(context.hook))
    static_cast<void>(context.hook->release(context.hook->user, slot.handle));
  slot = HookSlot{};
}

bool HooksInstalled(const Context &context) noexcept {
  return context.mask.handle.id != 0 && context.fade.handle.id != 0;
}

// Locates both setters and hooks them.  Runs at Start and, if a target is
// missing, on a slow retry. Player camera settings are maintained separately.
void EnsureHooks(Context &context) noexcept {
  if (HooksInstalled(context)) return;
  std::uintptr_t mask_target{};
  std::uintptr_t fade_target{};
  if (context.mask.handle.id == 0 &&
      ResolveNativeTarget(context, kMaskSetterPath, kMaskSetterNumParams,
                          kMaskSetterParamBytes, mask_target)) {
    static_cast<void>(InstallHook(context, mask_target,
                                  reinterpret_cast<void *>(&MaskSetterDetour),
                                  std::string_view("anti-blur-mask"),
                                  context.mask));
  }
  if (context.fade.handle.id == 0 &&
      ResolveNativeTarget(context, kFadeSetterPath, kFadeSetterNumParams,
                          kFadeSetterParamBytes, fade_target)) {
    static_cast<void>(InstallHook(context, fade_target,
                                  reinterpret_cast<void *>(&FadeSetterDetour),
                                  std::string_view("anti-blur-fade"),
                                  context.fade));
  }
  if (HooksInstalled(context)) {
    if (!context.logged_ready) {
      char message[192]{};
      std::snprintf(message, sizeof(message),
                    "anti-blur forced mask=%.1f mask=0x%llX fade=0x%llX",
                    static_cast<double>(kVisibleMask),
                    static_cast<unsigned long long>(context.mask.target),
                    static_cast<unsigned long long>(context.fade.target));
      Log(context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, message);
      context.logged_ready = true;
    }
  } else if (!context.logged_retry) {
    Log(context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        "anti-blur mask setters not resolved yet; retrying");
    context.logged_retry = true;
  }
}

AnomalyStatusV1 Load(const AnomalyHostApiV1 *host, void **plugin_context) {
  if (host == nullptr || plugin_context == nullptr)
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *plugin_context = nullptr;
  if (!LoadCompatibility(host)) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
  auto *context = new (std::nothrow) Context();
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED);
  context->core = Query<AnomalyCoreServiceV1>(
      host, ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
  context->signature = Query<AnomalySignatureServiceV1>(
      host, ANOMALY_SIGNATURE_SERVICE_V1_ID,
      ANOMALY_SIGNATURE_SERVICE_V1_VERSION);
  context->objects = Query<AnomalyUe5ObjectsServiceV1>(
      host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID,
      ANOMALY_UE5_OBJECTS_SERVICE_V1_VERSION);
  context->hook = Query<AnomalyHookServiceV1>(
      host, ANOMALY_HOOK_SERVICE_V1_ID, ANOMALY_HOOK_SERVICE_V1_VERSION);
  context->names = Query<AnomalyUe5NamesServiceV1>(
      host, ANOMALY_UE5_NAMES_SERVICE_V1_ID, 1);
  if (!CoreReady(context->core) || !SignatureReady(context->signature) ||
      !ObjectsReady(context->objects) || !HookReady(context->hook) ||
      !CameraServicesReady(*context)) {
    delete context;
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                  "core, signature, objects, hook and names services are required");
  }
  *plugin_context = context;
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 Start(void *plugin_context) noexcept {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  context->registry = 0;
  context->g_world_address = 0;
  context->mask = HookSlot{};
  context->fade = HookSlot{};
  context->camera_layout = {};
  context->saved_camera = {};
  context->retry_elapsed = kRetrySeconds;
  context->logged_ready = false;
  context->logged_retry = false;
  g_active.store(context, std::memory_order_release);
  EnsureHooks(*context);
  Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_INFO, "anti-blur started");
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 Stop(void *plugin_context, std::uint32_t) noexcept {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr) return anomaly::sdk::Ok();
  g_active.store(nullptr, std::memory_order_release);
  ReleaseHook(*context, context->mask);
  ReleaseHook(*context, context->fade);
  if (!RestoreCamera(*context))
    Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        "anti-blur could not restore player camera settings");
  return anomaly::sdk::Ok();
}

void Unload(void *plugin_context) noexcept {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr) return;
  static_cast<void>(Stop(context, 0));
  delete context;
}

void Update(void *plugin_context, double delta_seconds) noexcept {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr) return;
  try {
    if (!HooksInstalled(*context) || !context->camera_layout.ready) {
      if (delta_seconds > 0.0 && delta_seconds < 1.0)
        context->retry_elapsed += delta_seconds;
      if (context->retry_elapsed >= kRetrySeconds) {
        context->retry_elapsed = 0.0;
        EnsureHooks(*context);
        if (!ResolveCameraLayout(*context))
          Log(*context, ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
              "anti-blur player camera layout unavailable; NPC hooks remain active");
      }
    }
    static_cast<void>(ApplyCamera(*context));
  } catch (...) {
  }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1 *descriptor) {
  if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor))
    return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  *descriptor = {
      sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
      ANOMALY_PLUGIN_API_V1_MINOR,
      anomaly::sdk::StringView("anomaly.camera-blur-fix"),
      anomaly::sdk::StringView("Camera Blur Fix"),
      anomaly::sdk::StringView("Anomaly"),
      anomaly::sdk::StringView("1.3.2"), Load, Start, Stop, Unload, Update,
      nullptr};
  return anomaly::sdk::Ok();
}
