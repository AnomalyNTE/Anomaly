#pragma once

#include <Windows.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANOMALY_BOOTSTRAP_ABI_VERSION 1u
#define ANOMALY_RUNTIME_STATE_INFO_VERSION 1u

#define ANOMALY_CORE_START_ENTRY "AnomalyStart"
#define ANOMALY_CORE_MANUAL_MAP_CXX_THROW_ENTRY "AnomalyManualMapCxxThrow"
#define ANOMALY_CORE_REQUEST_STOP_ENTRY "AnomalyRequestStop"
#define ANOMALY_CORE_GET_STATE_ENTRY "AnomalyGetState"
#define ANOMALY_CORE_WAIT_FOR_STOP_ENTRY "AnomalyWaitForStop"

typedef uint32_t AnomalyBootstrapType;
enum {
    ANOMALY_BOOTSTRAP_TYPE_UNKNOWN = 0u,
    ANOMALY_BOOTSTRAP_TYPE_DWMAPI_PROXY = 1u,
    ANOMALY_BOOTSTRAP_TYPE_EXTERNAL = 2u
};

typedef uint32_t AnomalyRuntimeState;
enum {
    ANOMALY_RUNTIME_STATE_DORMANT = 0u,
    ANOMALY_RUNTIME_STATE_BOOTSTRAPPING = 1u,
    ANOMALY_RUNTIME_STATE_STARTING_BLOCKING_SERVICES = 2u,
    ANOMALY_RUNTIME_STATE_STARTING_ASYNC_SERVICES = 3u,
    ANOMALY_RUNTIME_STATE_AWAITING_GAME_READINESS = 4u,
    ANOMALY_RUNTIME_STATE_RUNNING = 5u,
    ANOMALY_RUNTIME_STATE_STOP_REQUESTED = 6u,
    ANOMALY_RUNTIME_STATE_STOPPING_PLUGINS = 7u,
    ANOMALY_RUNTIME_STATE_STOPPING_SERVICES = 8u,
    ANOMALY_RUNTIME_STATE_FAILED = 9u,
    ANOMALY_RUNTIME_STATE_STOPPED = 10u
};

typedef struct AnomalyStartInfo {
    uint32_t struct_size;
    uint32_t bootstrap_abi_version;
    AnomalyBootstrapType bootstrap_type;
    uint32_t flags;
    HMODULE bootstrap_module;
    HMODULE game_module;
    const WCHAR* runtime_root;
    const WCHAR* log_directory;
    HANDLE external_stop_event;
} AnomalyStartInfo;

#define ANOMALY_START_INFO_V1_SIZE 56u

typedef struct AnomalyRuntimeStateInfo {
    uint32_t struct_size;
    uint32_t state_info_version;
    AnomalyRuntimeState state;
    uint32_t last_error;
    uint64_t session_generation;
} AnomalyRuntimeStateInfo;

#define ANOMALY_RUNTIME_STATE_INFO_V1_SIZE 24u

typedef DWORD(WINAPI* AnomalyStartFn)(const AnomalyStartInfo* start_info);
typedef DWORD(WINAPI* AnomalyRequestStopFn)(void);
typedef DWORD(WINAPI* AnomalyGetStateFn)(AnomalyRuntimeStateInfo* state_info);
typedef DWORD(WINAPI* AnomalyWaitForStopFn)(DWORD timeout_ms);

#ifdef __cplusplus
}
#endif
