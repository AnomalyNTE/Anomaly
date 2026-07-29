# Interop 与内存

对应头文件：`services/interop.h`（签名 / Hook / Patch）与 `services/core.h`（原始内存）。通用约定见 [API 参考总览](README.md)。

> [!CAUTION]
> 本页的服务直接修改进程执行状态。所有句柄绑定 plugin generation：tracked Patch 在 release / stop / reload 时恢复原字节，function Hook 的 detour 必须以 `begin_callback` / `end_callback` 包围。

## 原始内存 grant

`anomaly.core` 的 `read_memory` / `write_memory` 不随服务自动授予，而是独立 grant：

| 调用 | 所需 capability | 缺失时 |
| --- | --- | --- |
| `read_memory` | `memory-read` | `PERMISSION_DENIED` |
| `write_memory` | `memory-write` | `PERMISSION_DENIED` |

签名见 [`anomaly.core`](lifecycle-and-core.md#anomalycore)。新插件应优先使用语义 [NTE 服务](nte-services.md)。

---

## `anomaly.interop.signature`

- **ID**：`"anomaly.interop.signature"` · **版本** 1 · **capability** `interop-signature`

```c
typedef struct AnomalySignatureServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *resolve)(void* user,
        AnomalyStringViewV1 module_name, AnomalyStringViewV1 section_name,
        AnomalyStringViewV1 pattern, uintptr_t* address);
} AnomalySignatureServiceV1;
```

`resolve` 在指定模块 / 节区内匹配字节签名（支持 `??`、`4?`、`?F` 通配），把首个命中地址写入 `*address`。

---

## `anomaly.interop.hook`

- **ID**：`"anomaly.interop.hook"` · **版本** 1 · **capability** `interop-hook`

同一 plugin generation 可对不同函数创建多个 function hook。

```c
typedef enum AnomalyHookKindV1 {
    ANOMALY_HOOK_V1_FUNCTION = 1, ANOMALY_HOOK_V1_IAT = 2,
    ANOMALY_HOOK_V1_EXPORT = 3, ANOMALY_HOOK_V1_VTABLE = 4
} AnomalyHookKindV1;

typedef struct AnomalyHookRequestV1 {
    uint32_t struct_size; uint32_t kind;   // AnomalyHookKindV1
    uintptr_t target; void* detour; AnomalyStringViewV1 label;
} AnomalyHookRequestV1;

typedef struct AnomalyHookServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *create)(void* user, const AnomalyHookRequestV1* request,
        uintptr_t* original, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user, AnomalyGenerationHandleV1 handle);
    AnomalyStatusV1 (ANOMALY_CALL *begin_callback)(void* user,
        AnomalyGenerationHandleV1 hook, AnomalyGenerationHandleV1* callback_lease);
    AnomalyStatusV1 (ANOMALY_CALL *end_callback)(void* user, AnomalyGenerationHandleV1 callback_lease);
} AnomalyHookServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `create(request, original, handle)` | 安装 hook；`original` 回传调用原函数的入口 |
| `release(handle)` | 卸载 hook |
| `begin_callback(hook, callback_lease)` | 进入 detour 时获取回调租约 |
| `end_callback(callback_lease)` | 退出 detour 时归还租约 |

> [!IMPORTANT]
> detour 实现**必须**以 `begin_callback` / `end_callback` 成对包围，宿主借此在停止 / 卸载时安全 drain。当前实现的 hook kind 以 `ANOMALY_HOOK_V1_FUNCTION` 为主。有 Owner 的 HookManager 只启停并回收自身 hook，不使用全局开关。

---

## `anomaly.interop.patch`

- **ID**：`"anomaly.interop.patch"` · **版本** 1 · **capability** `interop-patch`

```c
typedef struct AnomalyPatchServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *apply)(void* user, uintptr_t address,
        AnomalyByteSpanV1 replacement, AnomalyStringViewV1 label, AnomalyGenerationHandleV1* handle);
    AnomalyStatusV1 (ANOMALY_CALL *release)(void* user, AnomalyGenerationHandleV1 handle);
} AnomalyPatchServiceV1;
```

| 函数 | 说明 |
| --- | --- |
| `apply(address, replacement, label, handle)` | 写入 `replacement` 字节，保存原字节；返回 tracked 句柄 |
| `release(handle)` | 恢复原字节 |

> [!NOTE]
> **可恢复的 Patch 必须使用 `anomaly.interop.patch`**（而非直接 `write_memory`），这样宿主能在 release / stop / reload 时确定性恢复原字节。
