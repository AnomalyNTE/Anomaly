#pragma once

// anomaly.profile 的 C 表声明。
//
// 仓库内构建直接用 include/anomaly/sdk/services/profile.h；独立 SDK（2.4.3）还没导出这份
// 头文件时，按宿主公开文档（docs/api-reference/platform-services.md#anomalyprofile）逐字声明
// 同一份 ABI：纯 C、定宽类型、struct_size/service_version 前缀。声明必须与宿主一致，否则
// query_service 的 struct_size 检查会直接拒绝。

#if __has_include("anomaly/sdk/services/profile.h")
#include "anomaly/sdk/services/profile.h"
#else

#include "anomaly/sdk/base.h"

#define ANOMALY_PROFILE_SERVICE_V1_ID "anomaly.profile"
#define ANOMALY_PROFILE_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnomalyProfileServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    // 读取本插件 package generation 内 profile/<relative_path>，按
    // plugin-profile.schema.json 校验后返回 anomaly.json 文档句柄。
    // 仅在 Lifecycle / Worker 域调用。
    AnomalyStatusV1 (ANOMALY_CALL *load)(
        void* user, AnomalyStringViewV1 relative_path,
        AnomalyGenerationHandleV1* document);
} AnomalyProfileServiceV1;

#ifdef __cplusplus
}
#endif

#endif  // __has_include("anomaly/sdk/services/profile.h")
