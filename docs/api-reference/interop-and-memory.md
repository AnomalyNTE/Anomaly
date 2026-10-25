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

- **ID**：`"anomaly.interop.signature"` · **版本** 1 / 2 · **capability** `interop-signature`

```c
typedef struct AnomalySignatureServiceV1 {
    uint32_t struct_size; uint32_t service_version; void* user;
    AnomalyStatusV1 (ANOMALY_CALL *resolve)(void* user,
        AnomalyStringViewV1 module_name, AnomalyStringViewV1 section_name,
        AnomalyStringViewV1 pattern, uintptr_t* address);
} AnomalySignatureServiceV1;
```

查询版本 1 始终返回原来的 `AnomalySignatureServiceV1`。`resolve` 在指定模块 / 节区内匹配
字节签名（支持 `??`、`4?`、`?F` 通配），唯一命中时写入 `*address`；多匹配返回 `CONFLICT`。
Lifecycle / Worker 调用会学习指令，原签名零匹配时尝试恢复。v1 保留对象成员和栈位移，
不更新插件内的字段常量。Game / Render 调用不执行学习、恢复或状态文件 I/O。

### 版本 2：同时解析地址与字段布局

查询版本 2 返回独立的 `AnomalySignatureServiceV2`，保留原来的 `resolve` 并增加：

```c
AnomalyStatusV1 (ANOMALY_CALL *resolve_layout)(
    void* user, const AnomalySignatureLayoutRequestV2* request,
    uintptr_t* addresses, uint32_t address_count,
    uint32_t* offsets, uint32_t offset_count);
```

声明由插件保管，不需要迁入总 Profile。各结构均须填写 `struct_size`，完整定义见
`anomaly/sdk/services/interop.h`：

| 声明 | 内容 |
| --- | --- |
| `AnomalySignatureFieldV2` | 唯一字段 `id`、原始 `seed_offset`、`alignment`（1 / 2 / 4 / 8） |
| `AnomalySignatureMemberV2` | `field_index`、`addend`、访问 `width`（1 / 2 / 4 / 8 字节）、允许的 `access` 标志 |
| `AnomalySignatureWitnessV2` | 唯一函数 `id`、原始 `pattern`、成员绑定数组及数量 |
| `AnomalySignatureLayoutRequestV2` | 模块、可执行节区、字段数组及数量、函数数组及数量 |

`access` 可组合 `ANOMALY_SIGNATURE_MEMBER_READ_V2`、`ANOMALY_SIGNATURE_MEMBER_WRITE_V2`、
`ANOMALY_SIGNATURE_MEMBER_ADDRESS_V2`。
地址计算不访问字段，因此不检查其访问宽度；读写操作必须符合声明的宽度。
提取关系为“指令中的对象成员位移 = 字段偏移 + addend”，例如通过 `TArray::Num` 学习数组
基址时使用 `addend = 8`。原偏移只标识旧指令中的语义位置，解析失败不会退回这些值。

每组最多 32 个字段、32 个函数，每函数最多 32 个绑定，pattern 文本最多 1024 字节。
函数签名从 Windows x64 函数入口开始，以入口 RCX 为对象。框架通过受支持的 MOV / LEA
追踪对象来源，仅放开绑定的成员位移，保留其他成员、栈位置、普通立即数及指令结构。
扫描窗口最多 128 字节，至少 4 条完整指令、16 个固定字节；不足 24 个固定字节的函数，
每个字段都必须有另一个不同函数地址给出一致结果。同一字段的所有提取点和函数证据必须一致，
并满足 alignment；歧义或证据冲突返回 `CONFLICT`，证据不足或零匹配返回 `NOT_FOUND`。

该调用同步执行，只允许 Lifecycle / Worker；Game / Render 返回 `UNAVAILABLE`。
输出数组由调用方分配，数量必须分别等于 `witness_count` 与 `field_count`，按声明顺序返回。
插件上下文有效且请求前缀、输出指针和数量有效后，任何失败都会清零两组输出；成功时整组发布。
不分配跨 DLL 的句柄或宿主对象。状态消息须在当前线程下一次布局调用前复制。

学习记录与 v1 一起保存在各插件 state 目录的 `signature-knowledge/signatures.json`，
按模块、节区、原签名和字段绑定隔离；整组通过验证后才保存。记录损坏被忽略，保存失败不使
已验证的结果失效。函数重写、对象来源不明、访问宽度改变或指令编码发生变化时，仍会保持降级。
这是基于插件所给语义证据的恢复，不是任意新字段的自动语义识别。

C++ 插件可使用 [`anomaly/sdk/signature.hpp`](../../include/anomaly/sdk/signature.hpp) 中的
`FieldSeed` / `MemberUse` / `Witness` 声明及 `ResolveLayout`。返回快照由插件本地持有，
`address(id)` / `offset(id)` 查找不扫描、不访问磁盘；查询未声明的 ID 会抛出 `std::out_of_range`。

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
