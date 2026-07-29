#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define ANOMALY_CALL __cdecl
#if defined(__cplusplus)
#define ANOMALY_SDK_EXPORT extern "C" __declspec(dllexport)
#else
#define ANOMALY_SDK_EXPORT __declspec(dllexport)
#endif
#else
#define ANOMALY_CALL
#define ANOMALY_SDK_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnomalyStringViewV1 { const char* data; size_t size; } AnomalyStringViewV1;
typedef struct AnomalyByteSpanV1 { const uint8_t* data; size_t size; } AnomalyByteSpanV1;
typedef struct AnomalyMutableByteSpanV1 { uint8_t* data; size_t size; } AnomalyMutableByteSpanV1;

typedef enum AnomalyStatusCodeV1 {
    ANOMALY_STATUS_V1_OK = 0,
    ANOMALY_STATUS_V1_INVALID_ARGUMENT = 1,
    ANOMALY_STATUS_V1_UNAVAILABLE = 2,
    ANOMALY_STATUS_V1_NOT_FOUND = 3,
    ANOMALY_STATUS_V1_BUFFER_TOO_SMALL = 4,
    ANOMALY_STATUS_V1_FAILED = 5,
    ANOMALY_STATUS_V1_TIMEOUT = 6,
    ANOMALY_STATUS_V1_PERMISSION_DENIED = 7,
    ANOMALY_STATUS_V1_CONFLICT = 8,
    ANOMALY_STATUS_V1_CANCELLED = 9
} AnomalyStatusCodeV1;

typedef struct AnomalyStatusV1 {
    uint32_t code;
    uint32_t reserved;
    AnomalyStringViewV1 message;
} AnomalyStatusV1;

typedef struct AnomalyAllocatorV1 {
    uint32_t struct_size;
    uint32_t reserved;
    void* user;
    void* (ANOMALY_CALL *allocate)(void* user, size_t size, size_t alignment);
    void* (ANOMALY_CALL *reallocate)(void* user, void* memory, size_t size, size_t alignment);
    void (ANOMALY_CALL *release)(void* user, void* memory, size_t alignment);
} AnomalyAllocatorV1;

typedef struct AnomalyGenerationHandleV1 { uint64_t id; uint64_t generation; } AnomalyGenerationHandleV1;
typedef enum AnomalyFeatureStateV1 {
    ANOMALY_FEATURE_V1_UNAVAILABLE = 0,
    ANOMALY_FEATURE_V1_AVAILABLE = 1
} AnomalyFeatureStateV1;

#ifdef __cplusplus
}
#endif
