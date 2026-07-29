#include "anomaly/core_api.h"

#include <stddef.h>

_Static_assert(sizeof(AnomalyBootstrapType) == sizeof(uint32_t), "bootstrap type width");
_Static_assert(sizeof(AnomalyRuntimeState) == sizeof(uint32_t), "runtime state width");
_Static_assert(offsetof(AnomalyStartInfo, struct_size) == 0, "start info prefix");
_Static_assert(offsetof(AnomalyStartInfo, bootstrap_abi_version) == 4, "bootstrap ABI offset");
_Static_assert(offsetof(AnomalyStartInfo, bootstrap_type) == 8, "bootstrap type offset");
_Static_assert(offsetof(AnomalyStartInfo, flags) == 12, "start flags offset");
_Static_assert(offsetof(AnomalyStartInfo, bootstrap_module) == 16, "bootstrap module offset");
_Static_assert(offsetof(AnomalyStartInfo, game_module) == 24, "game module offset");
_Static_assert(offsetof(AnomalyStartInfo, runtime_root) == 32, "runtime root offset");
_Static_assert(offsetof(AnomalyStartInfo, log_directory) == 40, "log directory offset");
_Static_assert(offsetof(AnomalyStartInfo, external_stop_event) == 48, "stop event offset");
_Static_assert(sizeof(AnomalyStartInfo) == 56, "start info v1 x64 layout");
_Static_assert(_Alignof(AnomalyStartInfo) == 8, "start info alignment");
_Static_assert(ANOMALY_START_INFO_V1_SIZE == 56u, "start info v1 prefix size");
_Static_assert(offsetof(AnomalyRuntimeStateInfo, struct_size) == 0, "state info prefix");
_Static_assert(offsetof(AnomalyRuntimeStateInfo, state_info_version) == 4, "state version offset");
_Static_assert(offsetof(AnomalyRuntimeStateInfo, state) == 8, "runtime state offset");
_Static_assert(offsetof(AnomalyRuntimeStateInfo, last_error) == 12, "last error offset");
_Static_assert(offsetof(AnomalyRuntimeStateInfo, session_generation) == 16, "generation offset");
_Static_assert(sizeof(AnomalyRuntimeStateInfo) == 24, "state info v1 x64 layout");
_Static_assert(_Alignof(AnomalyRuntimeStateInfo) == 8, "state info alignment");
_Static_assert(ANOMALY_RUNTIME_STATE_INFO_V1_SIZE == 24u, "state info v1 prefix size");

int main(void) {
    AnomalyStartInfo start_info = {0};
    AnomalyRuntimeStateInfo state_info = {0};
    start_info.struct_size = ANOMALY_START_INFO_V1_SIZE;
    start_info.bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION;
    state_info.struct_size = ANOMALY_RUNTIME_STATE_INFO_V1_SIZE;
    state_info.state_info_version = ANOMALY_RUNTIME_STATE_INFO_VERSION;
    return start_info.struct_size == 0u || state_info.struct_size == 0u;
}
