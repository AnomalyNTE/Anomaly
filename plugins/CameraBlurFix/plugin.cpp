#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ue5.h"
#include "camera_blur_fix_profile.hpp"

#include <atomic>
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

struct Context final {
  const AnomalyCoreServiceV1 *core{};
  const AnomalySignatureServiceV1 *signature{};
  const AnomalyUe5ObjectsServiceV1 *objects{};
  const AnomalyHookServiceV1 *hook{};
  std::uintptr_t registry{};
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
                  decltype(AnomalyCoreServiceV1::read_memory)>(
             service, offsetof(AnomalyCoreServiceV1, read_memory)) &&
         service->read_memory != nullptr;
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
         service->find_exact != nullptr;
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
  std::uintptr_t instruction{};
  if (context.signature->resolve(
          context.signature->user, anomaly::sdk::StringView("HTGame.exe"),
          anomaly::sdk::StringView(".text"),
          anomaly::sdk::StringView(kGObjectsPattern), &instruction)
          .code != ANOMALY_STATUS_V1_OK ||
      instruction == 0)
    return false;
  std::int32_t displacement{};
  if (!Read(context, instruction + kRipDisplacementOffset, displacement))
    return false;
  const auto resolved = static_cast<std::intptr_t>(instruction) +
                        kRipInstructionSize + displacement;
  if (resolved <= 0) return false;
  context.registry =
      static_cast<std::uintptr_t>(resolved) + kGObjectsAddend;
  return true;
}

bool ObjectFromHandle(Context &context, const AnomalyGenerationHandleV1 handle,
                      std::uintptr_t &object) noexcept {
  object = 0;
  if (!ResolveRegistry(context) || handle.id == 0) return false;
  const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
  std::int32_t count{};
  std::uintptr_t chunks{};
  if (!Read(context, context.registry + kObjectCountOffset, count) ||
      count <= 0 || static_cast<std::uint32_t>(count) > kMaximumObjectCount ||
      index >= static_cast<std::uint32_t>(count) ||
      !Read(context, context.registry + kObjectChunksOffset, chunks) ||
      chunks == 0)
    return false;
  const std::uint32_t chunk_index = index >> kObjectChunkBits;
  const std::uint32_t item_index =
      index & ((1U << kObjectChunkBits) - 1U);
  std::uintptr_t chunk{};
  if (!Read(context, chunks + static_cast<std::uintptr_t>(chunk_index) *
                                    sizeof(std::uintptr_t),
             chunk) ||
      chunk == 0)
    return false;
  const auto item = chunk + static_cast<std::uintptr_t>(item_index) *
                                kObjectItemStride;
  std::uintptr_t candidate{};
  std::uint32_t serial{};
  if (!Read(context, item + kObjectItemPointerOffset, candidate) ||
      candidate == 0 ||
      !Read(context, item + kObjectItemSerialOffset, serial) ||
      serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle))
    return false;
  object = candidate;
  return true;
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
  if (!Read(context, function + kUFunctionNumParmsOffset, num_params) ||
      !Read(context, function + kUFunctionParmsSizeOffset, param_bytes) ||
      num_params != expected_params || param_bytes != expected_param_bytes ||
      !Read(context, function + kUFunctionExecOffset, thunk) || thunk == 0)
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
// missing, on a slow retry: no frame work is done once both are live.
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
                    "anti-blur forced mask=1.0 mask=0x%llX fade=0x%llX",
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
  if (!CoreReady(context->core) || !SignatureReady(context->signature) ||
      !ObjectsReady(context->objects) || !HookReady(context->hook)) {
    delete context;
    return Status(ANOMALY_STATUS_V1_UNAVAILABLE,
                  "core, signature, objects and hook services are required");
  }
  *plugin_context = context;
  return anomaly::sdk::Ok();
}

AnomalyStatusV1 Start(void *plugin_context) noexcept {
  auto *context = static_cast<Context *>(plugin_context);
  if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
  context->registry = 0;
  context->mask = HookSlot{};
  context->fade = HookSlot{};
  context->retry_elapsed = 0.0;
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
  if (context == nullptr || HooksInstalled(*context)) return;
  try {
    if (delta_seconds > 0.0 && delta_seconds < 1.0)
      context->retry_elapsed += delta_seconds;
    if (context->retry_elapsed < kRetrySeconds) return;
    context->retry_elapsed = 0.0;
    EnsureHooks(*context);
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
      anomaly::sdk::StringView("anomaly.local.nte.camera-blur-fix"),
      anomaly::sdk::StringView("\xE5\x8F\x8D\xE8\x99\x9A\xE5\x8C\x96"),
      anomaly::sdk::StringView("Anomaly"),
      anomaly::sdk::StringView("1.3.0"), Load, Start, Stop, Unload, Update,
      nullptr};
  return anomaly::sdk::Ok();
}
