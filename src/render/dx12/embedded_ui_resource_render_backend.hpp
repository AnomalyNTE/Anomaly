#pragma once

#include <memory>

namespace anomaly {
class UiResourceRenderBackend;
}

namespace ue5mem::embedded {

struct EmbeddedState;

// The factory is local to the D3D12 host. Its result crosses into
// PluginManager only through the backend-neutral internal interface.
[[nodiscard]] std::shared_ptr<anomaly::UiResourceRenderBackend>
CreateEmbeddedUiResourceRenderBackend(EmbeddedState& state) noexcept;

}  // namespace ue5mem::embedded
