#include "render/dx12/embedded_host_internal.hpp"

#include "anomaly/host_ui_service.hpp"

namespace ue5mem::embedded {

const AnomalyUiServiceV1* EmbeddedUiServiceTable() noexcept {
    return anomaly::HostUiServiceTable();
}

}  // namespace ue5mem::embedded
