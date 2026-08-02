#include "anomaly/core_api.h"

#include <atomic>

namespace {

std::atomic<AnomalyBootstrapType> g_bootstrap_type{ANOMALY_BOOTSTRAP_TYPE_UNKNOWN};

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI AnomalyStart(
    const AnomalyStartInfo* start_info) {
    if (start_info == nullptr) return ERROR_INVALID_PARAMETER;
    if (start_info->struct_size < ANOMALY_START_INFO_V1_SIZE) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (start_info->bootstrap_abi_version != ANOMALY_BOOTSTRAP_ABI_VERSION) {
        return ERROR_REVISION_MISMATCH;
    }
    if (start_info->runtime_root == nullptr || start_info->log_directory == nullptr) {
        return ERROR_INVALID_PARAMETER;
    }
    g_bootstrap_type.store(start_info->bootstrap_type, std::memory_order_release);
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) AnomalyBootstrapType WINAPI
AnomalyBootstrapFixtureType() {
    return g_bootstrap_type.load(std::memory_order_acquire);
}
