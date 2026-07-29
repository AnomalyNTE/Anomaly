#include "anomaly/core_api.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>

#if !defined(_WIN32) || !defined(_WIN64)
#error "The bootstrap ABI contract targets Windows x64"
#endif

namespace {

using StartFn = DWORD(WINAPI*)(const AnomalyStartInfo*);
using RequestStopFn = DWORD(WINAPI*)();
using GetStateFn = DWORD(WINAPI*)(AnomalyRuntimeStateInfo*);
using WaitForStopFn = DWORD(WINAPI*)(DWORD);

static_assert(sizeof(void*) == 8);
static_assert(sizeof(AnomalyBootstrapType) == sizeof(std::uint32_t));
static_assert(sizeof(AnomalyRuntimeState) == sizeof(std::uint32_t));
static_assert(std::is_standard_layout_v<AnomalyStartInfo>);
static_assert(std::is_trivially_copyable_v<AnomalyStartInfo>);
static_assert(std::is_standard_layout_v<AnomalyRuntimeStateInfo>);
static_assert(std::is_trivially_copyable_v<AnomalyRuntimeStateInfo>);

static_assert(offsetof(AnomalyStartInfo, struct_size) == 0);
static_assert(offsetof(AnomalyStartInfo, bootstrap_abi_version) == 4);
static_assert(offsetof(AnomalyStartInfo, bootstrap_type) == 8);
static_assert(offsetof(AnomalyStartInfo, flags) == 12);
static_assert(offsetof(AnomalyStartInfo, bootstrap_module) == 16);
static_assert(offsetof(AnomalyStartInfo, game_module) == 24);
static_assert(offsetof(AnomalyStartInfo, runtime_root) == 32);
static_assert(offsetof(AnomalyStartInfo, log_directory) == 40);
static_assert(offsetof(AnomalyStartInfo, external_stop_event) == 48);
static_assert(sizeof(AnomalyStartInfo) == ANOMALY_START_INFO_V1_SIZE);
static_assert(alignof(AnomalyStartInfo) == 8);

static_assert(offsetof(AnomalyRuntimeStateInfo, struct_size) == 0);
static_assert(offsetof(AnomalyRuntimeStateInfo, state_info_version) == 4);
static_assert(offsetof(AnomalyRuntimeStateInfo, state) == 8);
static_assert(offsetof(AnomalyRuntimeStateInfo, last_error) == 12);
static_assert(offsetof(AnomalyRuntimeStateInfo, session_generation) == 16);
static_assert(sizeof(AnomalyRuntimeStateInfo) == ANOMALY_RUNTIME_STATE_INFO_V1_SIZE);
static_assert(alignof(AnomalyRuntimeStateInfo) == 8);

static_assert(std::is_same_v<AnomalyStartFn, StartFn>);
static_assert(std::is_same_v<AnomalyRequestStopFn, RequestStopFn>);
static_assert(std::is_same_v<AnomalyGetStateFn, GetStateFn>);
static_assert(std::is_same_v<AnomalyWaitForStopFn, WaitForStopFn>);

}  // namespace

int main() {
    AnomalyStartInfo start_info{};
    start_info.struct_size = ANOMALY_START_INFO_V1_SIZE;
    start_info.bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION;
    AnomalyRuntimeStateInfo state_info{};
    state_info.struct_size = ANOMALY_RUNTIME_STATE_INFO_V1_SIZE;
    state_info.state_info_version = ANOMALY_RUNTIME_STATE_INFO_VERSION;
    if (start_info.struct_size != sizeof(start_info) ||
        state_info.struct_size != sizeof(state_info)) {
        return 1;
    }
    std::cout << "C++ bootstrap contract passed\n";
    return 0;
}
