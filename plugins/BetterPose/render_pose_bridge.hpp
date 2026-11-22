#pragma once

// Included after Context and the pose/memory helpers. Native callers retain ownership of their
// input context; the render Update consumes a byte-only borrowed copy synchronously.
namespace render_pose_bridge {
using UpdateFn = void(ANOMALY_CALL *)(void *, std::uint32_t, void *, void *, void *,
                                    void *, void *, std::uint32_t, void *);
inline std::atomic<Context *> active{};
inline std::uintptr_t update_original{};
inline const AnomalyHookServiceV1 *hook_api{};
inline AnomalyGenerationHandleV1 update_lease_source{};

// `from_motion` exists because this used to refuse every publish while a motion was loaded, which
// is exactly when the motion path needs to publish: without a snapshot the engine renders whatever
// buffer it picked itself, so the clock advanced and the writes landed while the character stayed on
// one frame.
inline bool Publish(Context &context, const std::vector<PackedTransform> &pose,
                    const bool from_motion = false) noexcept {
  if (!context.render_bridge.enabled.load(std::memory_order_acquire) || pose.empty() ||
      (!from_motion && context.motion_loaded.load(std::memory_order_acquire)) ||
      context.render_bridge.producer_thread.load(std::memory_order_acquire) != GetCurrentThreadId())
    return false;
  try {
    Context::RenderBridgeState::Sample sample;
    sample.mesh = context.runtime.mesh;
    sample.asset = context.runtime.mesh_asset;
    sample.count = static_cast<std::uint32_t>(pose.size());
    if (!Read(context, sample.mesh + native_layout.kRenderMeshObjectOffset, sample.mesh_object) ||
        sample.mesh_object == 0) return false;
    auto snapshot = std::make_shared<std::vector<Context::RenderBridgeState::Transform>>(pose.size());
    for (std::size_t i{}; i != pose.size(); ++i)
      std::memcpy((*snapshot)[i].raw.data(), &pose[i], sizeof(PackedTransform));
    sample.render_pose = std::move(snapshot);
    std::lock_guard<std::mutex> lock(context.render_bridge.mutex);
    context.render_bridge.samples[sample.mesh] = std::move(sample);
    return true;
  } catch (...) {
    return false;
  }
}

inline void ClearSnapshot(Context &context, const std::uintptr_t mesh) noexcept {
  if (context.render_bridge.producer_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) return;
  std::lock_guard<std::mutex> lock(context.render_bridge.mutex);
  context.render_bridge.samples.erase(mesh);
}

class ProductionScope final {
 public:
  explicit ProductionScope(Context &context) noexcept : context_(context), mesh_(context.runtime.mesh) {}
  ~ProductionScope() { if (!published) ClearSnapshot(context_, mesh_); }
  bool published{};
 private:
  Context &context_;
  std::uintptr_t mesh_{};
};

inline void SynchronizeOwnership(Context &context) noexcept {
  std::lock_guard<std::mutex> lock(context.render_bridge.mutex);
  for (auto it = context.render_bridge.samples.begin(); it != context.render_bridge.samples.end();) {
    const auto mesh = it->first;
    bool owned{};
    if (mesh == context.runtime.mesh) {
      // Both the manual pose and a loaded motion publish a snapshot now, so both own it.
      owned = context.pose_override_enabled.load(std::memory_order_acquire) ||
          context.motion_loaded.load(std::memory_order_acquire);
    } else {
      const auto track = context.entity_motion_states.find(mesh);
      owned = track != context.entity_motion_states.end() &&
          (track->second.track.frame_count != 0 ||
           (!track->second.keyframe_driving_released && !track->second.keyframe_track.Empty()));
    }
    if (!owned) it = context.render_bridge.samples.erase(it);
    else ++it;
  }
}

// The copy span is a Profile-validated caller storage envelope, not an invented UE sizeof.
// It contains borrowed references: do not invoke native constructors/destructors or alter refcounts.
class InputSnapshot final {
 public:
  struct alignas(16) Block { std::uint8_t bytes[16]{}; };
  bool Prepare(Context &context, void *original, const std::uintptr_t receiver,
               const Context::RenderBridgeState::Sample &sample) {
    if (!original || !sample.render_pose || sample.render_pose->size() != sample.count ||
        sample.count == 0 || sample.count > 8192 || sample.mesh_object != receiver) return false;
    const auto span = native_layout.kRenderContextCopySpan;
    const auto data_offset = native_layout.kRenderContextSourceOffset;
    const auto count_offset = native_layout.kRenderContextCountOffset;
    if (span == 0 || span > 4096 || span % alignof(Block) != 0 ||
        data_offset > span - sizeof(std::uintptr_t) || count_offset > span - sizeof(std::uint32_t) ||
        count_offset != data_offset + sizeof(std::uintptr_t)) return false;
    std::uint64_t alternate_source{};
    // Unknown leader mappings retain native input rather than using receiver-order bones.
    if (!Read(context, sample.mesh + native_layout.kRenderLeaderWeakOffset, alternate_source) ||
        alternate_source != 0) return false;
    std::uintptr_t asset{}, mesh_object{};
    if (!Read(context, sample.mesh + native_layout.kMeshSkeletalMeshOffset, asset) || asset != sample.asset ||
        !Read(context, sample.mesh + native_layout.kRenderMeshObjectOffset, mesh_object) || mesh_object != receiver)
      return false;
    std::uint32_t count{};
    const auto address = reinterpret_cast<std::uintptr_t>(original);
    if (!Read(context, address + count_offset, count) || count != sample.count) return false;
    storage_.resize(span / sizeof(Block));
    auto *bytes = reinterpret_cast<std::uint8_t *>(storage_.data());
    if (!ReadBytes(context, address, bytes, span)) return false;
    const auto source = reinterpret_cast<std::uintptr_t>(sample.render_pose->data());
    std::memcpy(bytes + data_offset, &source, sizeof(source));
    std::memcpy(bytes + count_offset, &count, sizeof(count));
    // The dword after count is padding in the borrowed view, not owning TArray capacity.
    return true;
  }
  void *Data() noexcept { return storage_.data(); }
 private:
  std::vector<Block> storage_;
};

inline bool FindSample(Context &context, const std::uintptr_t receiver,
                       Context::RenderBridgeState::Sample &sample) noexcept {
  std::lock_guard<std::mutex> lock(context.render_bridge.mutex);
  for (const auto &[mesh, stored] : context.render_bridge.samples) {
    if (stored.mesh_object == receiver) { sample = stored; return true; }
  }
  return false;
}

inline void ANOMALY_CALL UpdateDetour(void *object, std::uint32_t lod, void *input, void *arg4,
                                     void *arg5, void *arg6, void *arg7, std::uint32_t arg8,
                                     void *arg9) noexcept {
  const auto *api = hook_api;
  AnomalyGenerationHandleV1 lease{};
  const bool leased =
      api != nullptr &&
      api->begin_callback(api->user, update_lease_source, &lease).code == ANOMALY_STATUS_V1_OK;
  {
    // The engine's Update must always run. While the host disables the hook during a
    // reload the lease is refused, and skipping the call breaks the skeletal mesh
    // pipeline; without a lease we simply forward the original input untouched.
    void *render_input = input;
    if (leased) {
      auto *context = active.load(std::memory_order_acquire);
      Context::RenderBridgeState::Sample sample;
      InputSnapshot copied_input;
      try {
        if (context != nullptr && !context->stopping.load(std::memory_order_acquire) &&
            context->render_bridge.enabled.load(std::memory_order_acquire) &&
            FindSample(*context, reinterpret_cast<std::uintptr_t>(object), sample) &&
            copied_input.Prepare(*context, input, reinterpret_cast<std::uintptr_t>(object), sample))
          render_input = copied_input.Data();
      } catch (...) {
        render_input = input;
      }
      try {
        if (update_original)
          reinterpret_cast<UpdateFn>(update_original)(object, lod, render_input, arg4, arg5, arg6,
                                                     arg7, arg8, arg9);
      } catch (...) {
      }
      // Native Update copies/converts the pose before queuing its owned payload. The
      // snapshot and copy die here, before the callback lease is released.
    } else if (update_original) {
      try {
        reinterpret_cast<UpdateFn>(update_original)(object, lod, input, arg4, arg5, arg6, arg7,
                                                    arg8, arg9);
      } catch (...) {
      }
    }
  }
  if (leased)
    static_cast<void>(api->end_callback(api->user, lease));
}

inline bool Release(Context &context) noexcept {
  context.render_bridge.enabled.store(false, std::memory_order_release);
  if (context.render_bridge.update_handle.id != 0) {
    const auto status = context.hook->release(context.hook->user, context.render_bridge.update_handle);
    if (status.code != ANOMALY_STATUS_V1_OK) {
      LogDiagnostic(context, "betterpose render pose bridge hook release failed");
      return false;
    }
    context.render_bridge.update_handle = {};
  }
  active.store(nullptr, std::memory_order_release);
  return true;
}

inline bool Install(Context &context) noexcept {
  try {
    const auto update = compatibility.Address("RenderPose.Update");
    if (!update || !compatibility.MatchesEntry(context.core, "RenderPose.Update", update)) {
      LogDiagnostic(context, "betterpose render pose bridge unavailable: profile entry validation failed");
      return false;
    }
    hook_api = context.hook;
    active.store(&context, std::memory_order_release);
    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = update;
    request.detour = reinterpret_cast<void *>(&UpdateDetour);
    request.label = anomaly::sdk::StringView("pose-render-snapshot");
    const auto status = context.hook->create(context.hook->user, &request, &update_original, &update_lease_source);
    context.render_bridge.update_handle = update_lease_source;
    if (status.code != ANOMALY_STATUS_V1_OK) {
      // A conflict means SDK setup cleanup succeeded; its output handle is already stale.
      if (status.code == ANOMALY_STATUS_V1_CONFLICT) context.render_bridge.update_handle = {};
      static_cast<void>(Release(context));
      LogDiagnostic(context, "betterpose render pose bridge unavailable: hook installation failed");
      return false;
    }
    context.render_bridge.enabled.store(true, std::memory_order_release);
    LogDiagnostic(context, "betterpose render pose bridge ready");
    return true;
  } catch (...) {
    context.render_bridge.update_handle = update_lease_source;
    static_cast<void>(Release(context));
    return false;
  }
}
} // namespace render_pose_bridge
