# NTE 角色战斗 ABI 规划

## 文档状态

- 状态：v1 已实现；正式合同见 [`docs/api-reference/nte-services.md`](../api-reference/nte-services.md)。
- 研究基线：`C:\Dumper-7\5.6.1-0+UE5-HT-1.3\CppSDK`。
- 目标：为插件提供不暴露 UE 对象指针的角色战斗快照、伤害统计、技能目录和受控技能调用。
- 首版原则：只发布已由活动 Profile、反射形状、对象 generation 和 Game 线程共同验证的能力；任一门禁失败时，整项服务降级为不可用。

## 范围

首版分为三个独立服务：

1. `anomaly.nte.combat`：当前角色战斗快照、角色原生伤害事件流和按条件汇总。
2. `anomaly.nte.skills`：当前角色已授予技能的分页快照、活动状态和可验证的冷却信息。
3. `anomaly.nte.skill-invocation`：使用 `anomaly.nte.skills` 返回的当前 generation skill handle 请求技能激活。

首版不包含：

- 任意 UObject 地址、`UFunction*`、`FGameplayAbilitySpec*` 或 GAS/HTGame 原生结构跨 DLL 暴露。
- 任意函数名、对象路径、裸 ability class 或参数缓冲区调用。
- 直接调用 `HTServerTryActivateAbilityWithData`，以及指定位置、旋转、目标、section、prediction key 的参数化施法。
- 修改生命、伤害、阵营、冷却、技能授予、Buff 或 GameplayEffect。
- 把客户端伤害飘字数据声明为服务端权威战斗日志。

## SDK 证据

以下内容来自 Dumper 输出，只作为候选结构和反射契约的离线证据。实现仍须由活动 Profile 在运行时验证。

### 伤害入口

| 候选 | SDK 证据 | 结论 |
| --- | --- | --- |
| 伤害入口 | `AHTAbilityCharacter::CharacterOnDamaged` 原生广播 | 精确广播模板只有一个调用点，不经过飘字或全局事件分发 |
| 伤害数值 | `FHTDamageEvent::Damage`，偏移 `0x10` | 作为实际伤害写入 `final_damage` |
| 参与者 | 广播的 `DamagedCharacter` 与 `InstigatorCharacter` 实参 | 转换成对象索引、serial 和当前 registry generation 组成的 opaque handle |
| 来源 | `FHTDamageEvent::DamageGEDef`，偏移 `0x138` | 解析 Gameplay Effect 对象路径并映射为当前 combat generation 的 source id |
| 展示元数据 | 广播不携带飘字分类 | `display_damage`、`basic_damage`、暴击、头部命中和位置字段不作猜测 |

采集事件带 `CHARACTER_EVENT` 标志，表示数值和来源来自角色伤害广播；`CLIENT_PRESENTED` 保留给可能的独立展示事件，不用于此采集路径。

### 角色战斗状态

`AHTAbilityCharacter` 提供以下反射候选：

- `K2_GetAbilitySystemComponent()`：参数大小 `0x08`，返回 `UHTAbilitySystemComponent*`。
- `GetHP()`、`GetHPMax(bool)`、`GetIsDead()`、`GetAttackTarget()`。
- `UHTAttributeComponent` 提供 `GetHPCurrent()`、`GetShieldHealth()` 等只读函数。

首版只采集当前玩家角色的 `hp`、`max_hp`、`shield`、死亡状态和攻击目标。攻击力、韧性、失衡、元素增伤/抗性虽然在 `UHTAttributeComponent` 中可见，但字段很多且含义依赖玩法，推迟到独立的属性页 ABI，避免把构建专用属性表固化进核心 combat v1。

### 技能目录和调用

| 候选 | SDK 证据 | 结论 |
| --- | --- | --- |
| ASC 定位 | `AHTAbilityCharacter::K2_GetAbilitySystemComponent()` | 通过已验证 ProcessEvent 调用取得，不硬编码角色成员偏移 |
| 已授予技能容器 | `UAbilitySystemComponent::ActivatableAbilities` 位于当前 SDK 的 `0x408` | 仅由 Profile/validator 使用，不进入公开 ABI |
| 容器项目 | `FGameplayAbilitySpecContainer::Items` 位于 `0x108` | 以有界 `TArray` 只读快照方式枚举 |
| spec | `FGameplayAbilitySpec` 大小 `0xF0`；`Handle=0x0C`、`Ability=0x10`、`Level=0x18`、`InputID=0x1C`、`ActiveCount=0x28`、状态位 `0x29` | 生成 host-owned skill snapshot；每个偏移都必须在 Profile 中并由反射形状验证 |
| 技能类 | `FGameplayAbilitySpec::Ability` 是 ability CDO；其 `object.class` 指向实际 `UClass` | 对外返回对象 handle 和 UTF-8 全路径；调用时由宿主重新解析并确认仍为同一个已授予 spec |
| 冷却 | `UHTGameplayAbility::CooldownGameplayEffectClass` 位于 `0x1B8`；`UHTAbilitySystemComponent::GetActiveEffectTimeRemainingAndDuration` 的 SDK 参数结构为 `0x10` | 以已验证的 GameplayEffect class 查询剩余时间和持续时间；形状不通过时只把该 skill 标记为 `PARTIAL`，不猜测冷却 |
| 激活 | `UHTAbilitySystemComponent::HTTryActivateAbilityByClass` 的 SDK 参数结构为 `0x10`，运行时 `UFunction::ParmsSize=0x09`；class 位于 `0x00`，bool 返回值位于 `0x08` | 首版唯一 mutation 入口；返回值表示游戏是否接受请求 |
| 参数化激活 | `HTServerTryActivateAbilityWithData` 参数大小 `0x80`，含目标、section、旋转、方向、位置和触发时间 | 推迟到后续版本；不能把该原生结构直接做成 ABI |

`GetAllAbilities(TArray<...>&)` 不作为首版枚举入口。由 ProcessEvent 产生的输出 `TArray` 涉及 UE 分配器所有权和析构，宿主当前没有可复用的稳定数组所有权桥。首版直接读取已验证、只读且有上限的 `ActivatableAbilities.Items`，避免跨分配器释放。

## ABI 草案

最终声明放入 `include/anomaly/sdk/services/nte.h`。以下仅描述字段和函数形状，字段顺序在实现前还要用 Windows x64 ABI snapshot 固化。

### `anomaly.nte.combat` v1

建议 capability：`nte-combat-read`。

```c
typedef struct AnomalyNteCombatantSnapshotV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t sequence;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 target;
    double hp;
    double max_hp;
    double shield;
} AnomalyNteCombatantSnapshotV1;

typedef struct AnomalyNteDamageEventV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t sequence;
    uint64_t tick_sequence;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 attacker;
    AnomalyGenerationHandleV1 victim;
    uint64_t source_id;
    int64_t display_damage;
    int64_t basic_damage;
    int64_t final_damage;
    double hit_location[3];
    uint32_t damage_type;
    uint32_t display_type;
    uint32_t reaction_type;
    uint32_t reaction_display_type;
} AnomalyNteDamageEventV1;

typedef struct AnomalyNteCombatStatisticsRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    uint64_t source_id;
    uint32_t direction;
    uint32_t reserved;
} AnomalyNteCombatStatisticsRequestV1;

typedef struct AnomalyNteCombatStatisticsV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t through_sequence;
    uint64_t hit_count;
    uint64_t critical_count;
    uint64_t head_hit_count;
    int64_t display_damage_total;
    int64_t basic_damage_total;
    int64_t final_damage_total;
} AnomalyNteCombatStatisticsV1;

typedef struct AnomalyNteCombatServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    AnomalyStatusV1 (ANOMALY_CALL *current_combatant)(
        void*, AnomalyNteCombatantSnapshotV1*);
    uint64_t (ANOMALY_CALL *latest_damage_sequence)(void*);
    AnomalyStatusV1 (ANOMALY_CALL *next_damage_event)(
        void*, uint64_t after_sequence, AnomalyNteDamageEventV1*);
    AnomalyStatusV1 (ANOMALY_CALL *statistics)(
        void*, const AnomalyNteCombatStatisticsRequestV1*,
        AnomalyNteCombatStatisticsV1*);
    AnomalyStatusV1 (ANOMALY_CALL *source_name_utf8)(
        void*, uint64_t source_id, char*, size_t*);
    AnomalyStatusV1 (ANOMALY_CALL *participant_path_utf8)(
        void*, AnomalyGenerationHandleV1 participant, char*, size_t*);
} AnomalyNteCombatServiceV1;
```

约定：

- combatant flags 至少包含 `VALID`、`PARTIAL`、`DEAD`；damage flags 包含 `CHARACTER_EVENT`，并为独立展示事件保留 `CLIENT_PRESENTED`、`CRITICAL`、`HEAD_HIT`、`WEAK_UNBALANCE`。两组枚举独立定义，不复用含义不相干的位。
- `direction` 支持 `ANY`、`AS_ATTACKER`、`AS_VICTIM`。零 character 表示不按参与者过滤，零 source id 表示不按来源过滤。
- 伤害序列在一个 Host lifecycle 内单调递增；World 变化时清空 aggregate，并在序列上保留 discontinuity。
- 环形队列采用固定上限。stale 的非零 cursor 和没有更新都返回 `NOT_FOUND`，与现有 session event 合同一致。
- 聚合使用 64 位计数和有符号 64 位伤害总量，并对溢出设置 `PARTIAL`，禁止环绕。
- 统计只覆盖 ring/aggregate 成功接纳的规范化事件。解析失败、队列截断和丢弃数进入 metrics，不伪装为完整统计。
- 伤害只来自 `AHTAbilityCharacter::CharacterOnDamaged` 的精确原生广播；`Damage`、attacker、victim 和 `DamageGEDef` 在同一个回调中读取，不再关联飘字或 RPC。
- participant 路径在游戏线程处理伤害时缓存，ABI 查询只读缓存，不从插件 UI/Render 线程读取 UObject。

### `anomaly.nte.skills` v1

建议 capability：`nte-skills-read`。分页形状复用 entities 服务的 generation 固定迭代语义，建议单页上限 128。

```c
typedef struct AnomalyNteSkillFrameV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t generation;
    uint64_t sequence;
    AnomalyGenerationHandleV1 character;
    uint32_t skill_count;
    uint32_t reserved;
} AnomalyNteSkillFrameV1;

typedef struct AnomalyNteSkillSnapshotV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyGenerationHandleV1 handle;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 ability_class;
    uint64_t sequence;
    int32_t level;
    int32_t input_id;
    float cooldown_remaining_seconds;
    float cooldown_duration_seconds;
} AnomalyNteSkillSnapshotV1;
```

服务函数：

- `frame`：返回当前不可变技能帧。
- `snapshot_at` / `page`：只访问 host cache，不在插件线程遍历实时 UE 数组。
- `ability_path_utf8`：解析 ability class handle 的完整路径，采用 buffer sizing 合同。
- `snapshot_by_handle`：验证 skill handle 的 frame generation 和对应 spec identity。

skill handle 不是 UE `FGameplayAbilitySpecHandle` 的直接透传。建议 `id` 由宿主分配，cache 内保存 `{spec handle, ability CDO object handle, class object handle}`。每次刷新只有在三者仍匹配时才延续 identity；技能移除、重新授予、角色切换或 World 变化都会产生新的 generation。

首版 skill flags：`VALID`、`PARTIAL`、`ACTIVE`、`INPUT_PRESSED`、`PENDING_REMOVE`、`REMOVE_AFTER_ACTIVATION`、`COOLDOWN_VALID`。没有纯只读且已验证的通用 preflight 前，不发布含义模糊的 `READY`；实际接受结果由调用服务返回。

### `anomaly.nte.skill-invocation` v1

建议 capability：`nte-skill-invocation`。该服务独立于只读技能服务，延续 `player-teleport` 的 mutation 隔离模式。

```c
typedef struct AnomalyNteSkillInvocationRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    AnomalyGenerationHandleV1 world;
    AnomalyGenerationHandleV1 character;
    AnomalyGenerationHandleV1 skill;
} AnomalyNteSkillInvocationRequestV1;

typedef struct AnomalyNteSkillInvocationResultV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t tick_sequence;
    uint32_t accepted;
    uint32_t reserved;
} AnomalyNteSkillInvocationResultV1;
```

调用顺序：

1. 要求位于活动 Game callback domain，否则返回 `CONFLICT`。
2. 验证 world、character 和 skill 三个 generation；任一 stale 返回 `NOT_FOUND`。
3. 重新读取当前 spec，确认 spec handle、ability CDO 和 class 与 cache 一致。
4. 确认 class 继承自 `UHTGameplayAbility`，且该 spec 仍属于当前角色的 ASC。
5. 通过验证后的 `HTTryActivateAbilityByClass` UFunction 和 ProcessEvent invoker 调用。
6. ProcessEvent 正常执行时返回 `OK`，游戏 bool 返回值写入 `accepted`；传输/反射调用失败返回 `FAILED`。

不得自动重试。`accepted == 0` 是游戏拒绝当前请求，不代表桥接调用失败。首版 `flags` 必须为 0，不提供 target、位置、方向或远程激活开关。

## Profile 与 validator 设计

建议新增 Feature：

| Feature | 依赖 | validator |
| --- | --- | --- |
| `nte.combat` | `nte.player`、`ue5.names`、`ue5.objects`、`ue5.process-event`，以及精确 native damage symbol | `nte-combat-reflection-v1` |
| `nte.skills` | `nte.player`、`ue5.names`、`ue5.objects`、`ue5.process-event` | `nte-skills-layout-v1` |
| `nte.skill-invocation` | `nte.skills`、`ue5.process-event` | `nte-skill-invocation-v1` |

Profile layout 至少需要表达：

- 通用 `TArray` data/num/max 布局及合理上限。
- `FGameplayAbilitySpecContainer.Items`、spec stride 和首版读取字段。
- `FHTDamageEvent` 大小、`Damage`、`DamageGEDef` 和弱对象 index/serial 字段偏移。
- 若现有 FProperty 反射不足以验证数组 inner，则增加 `FArrayProperty::Inner` 布局，并先验证该通用引擎布局。

validator 必须验证：

- UFunction 的 outer/class、参数数量、`parmsSize`、return offset 和每个参数的种类/大小/偏移。
- `HTDamageEvent` 的继承字段 `Damage` 与本体字段 `DamageGEDef` 偏移和类型。
- ASC、spec container、Items TArray、spec stride 和所读字段的反射形状。
- `K2_GetAbilitySystemComponent` 与 `HTTryActivateAbilityByClass` 的参数形状。
- 对象指针必须映射回当前 GObjects slot 且 serial 一致；class/outer 继承链设置深度上限。
- TArray 满足 `0 <= num <= max <= feature limit`，乘法和地址加法不溢出，整个读取范围可读。

不得仅因 Dumper offset 匹配就发布服务。Profile 缺字段、反射对象未加载、形状变化、ProcessEvent ABI 未通过、Game tick 未建立或角色对象 stale 时，都必须保持对应 Feature 不可用。

## 运行时所有权与线程

- `NteProfileRuntime` 只在 Profile 解析并验证 `BroadcastCharacterOnDamaged` 后安装一个精确 native hook，不走全局 `ProcessEvent`、Actor vtable 或飘字入口。
- native damage callback 只读取 `FHTDamageEvent` 的已验证字段、规范化参与者和来源、写入固定 ring；不调用插件、不做文件/网络 I/O。
- combat aggregate、技能 cache 和服务表属于同一个 NTE Profile generation；Stop 时先关闭 admission，再撤销服务并 drain hook callback。
- 当前角色战斗快照和技能帧在 Game tick 上按 demand 刷新。插件查询只读 host cache，不跨线程访问实时 UE 对象。
- consumer 在 Game 域 `on_update` 增量推进 damage cursor，并仅为新事件解析名称和生成不可变展示行；Render 域 `on_draw` 只复制本地快照并调用 UI。
- 技能调用仅在 Game callback domain 同步执行；不会从 Render、Lifecycle 或 Worker 线程代替调用方跳转执行。
- 服务表使用现有 `SemanticServiceEndpoint` call gate。旧 generation 表在 Stop 后只返回 `UNAVAILABLE`，不能重新绑定到下一次 Start。

## 实现阶段

### 阶段 0：离线契约和 fixture

- 将上述 Dumper 证据整理成最小 synthetic reflection/object-memory fixture。
- 为 `FHTDamageEvent`、弱对象、ASC/spec 容器和两个技能 UFunction 写独立 validator fixture。
- 验证 native 广播模板只有一个目标，并固定 attacker、victim、damage causer 的实参顺序。

退出条件：错误的字段偏移、stride、参数大小、array count、对象 serial 或 outer chain 均能使 validator fixture 失败。

### 阶段 1：只读 combat

- 实现当前角色 combatant cache。
- 实现伤害事件规范化、固定 ring、source name cache 和 World-scoped aggregate。
- 发布 `anomaly.nte.combat`，补充 capability policy、API 文档和 metrics。

退出条件：World 切换、角色切换、ring wrap、stale cursor、无攻击者/受击者、Miss/immune、负值和聚合溢出均有独立验证。

### 阶段 2：只读 skills

- 实现 demand-driven skill frame、分页、opaque handle 和完整 class path。
- 首先发布 level/input/active 状态；冷却仅在独立子门禁通过时发布并设置 `COOLDOWN_VALID`。
- 验证移除后重新授予同一 class 不会复用 stale skill handle。

退出条件：角色/World/generation 切换、spec 重排、重复 class、空 ability、异常 TArray 和部分冷却能力均有验证。

### 阶段 3：技能调用

- 发布独立 mutation service 和 capability。
- 只接受 skills service 当前帧返回的 handle。
- 通过 mock ProcessEvent 验证参数字节、返回 bool、线程拒绝和 stale identity。

退出条件：`accepted=1`、`accepted=0`、调用失败、非 Game 线程、旧 world、旧 character、旧 skill 和 class/spec 被替换都有独立结果。

### 阶段 4：消费者和文档

- `nte_combat_demo` 保留 HP、汇总、技能分页和 opt-in 技能调用，同时只使用正确的角色伤害事件路径。
- 所有服务查询、增量 drain、统计和名称解析位于 Game 域；Render 域只绘制本地快照并排队分页或技能调用意图。
- 更新 API reference、manifest capability 表和开发指南。

## 预计改动面

| 区域 | 预计内容 |
| --- | --- |
| `include/anomaly/sdk/services/nte.h` | 三个 v1 服务及稳定枚举/结构 |
| `apps/abi_snapshot/main.cpp`、`abi/anomaly-sdk-v1-windows-x64.json` | 固化大小、对齐、offset、常量和 service table |
| `src/game/ue5/` | 通用反射形状检查、对象 handle 转换和有界数组读取原语 |
| `src/game/nte/` | NTE UFunction 绑定、combat ring/aggregate、skill cache、调用门禁和生命周期 |
| `profiles/nte/*.json`、`schemas/build-profile.schema.json` | Feature、依赖、validator 和必要布局字段；只有 schema 形状变化时才改 schema |
| `src/plugin/plugin_capability_policy.cpp` | 三个 service -> capability 映射和已知 capability |
| `docs/api-reference/` | 正式合同、状态码、线程、cursor、stale 和角色伤害语义 |
| `examples/` | 完整战斗 consumer；角色伤害采集保持单一 native 路径 |

## 验证矩阵

实现后最低验证：

1. 构建 `anomaly_adapter_services`、`anomaly_nte_profile_runtime`、`anomaly_abi_snapshot`、`anomaly_test_host` 和新增示例 target。
2. 运行 validator/object-memory synthetic fixture，覆盖正确与错误布局。
3. 运行 service contract fixture，覆盖分页、cursor、aggregate、generation、call gate 和 Stop drain。
4. 运行 `anomaly-abi-snapshot --check abi/anomaly-sdk-v1-windows-x64.json`。
5. 用纯 C 和 C++ 外部消费者分别编译三个服务表。
6. 对 native damage hook、nested array 解析、ring 和 Stop/rollback 路径执行定向 fixture 与降级路径验证。
7. 最后执行 `git diff --check`，检查 API 文档链接和 capability 表一致性。

真实进程 smoke 不替代上述 fixture。只有在活动 Profile 已包含新 Feature 且明确安排诊断时，才验证：伤害事件与界面表现一致、技能目录与当前角色一致、一次调用返回的 `accepted` 与游戏实际动作一致；诊断结束后不保留后台 tracker 或含对象路径的未审查日志。

## 关键决策

- 采用三个服务而不是一个大而全的 combat 表：只读能力和 mutation capability 可独立授权、发布和降级。
- 伤害首版采用 `CharacterOnDamaged` 原生广播：数值、参与者和 `DamageGEDef` 来自同一事件；展示值、暴击和命中位置保持未发布，不从飘字补齐。
- 技能枚举读取验证后的内部容器，不调用返回 UE `TArray` 的反射函数，避免分配器所有权问题。
- 技能调用只接受 host-issued skill handle，不接受任意 class/path/function，确保调用对象仍属于当前角色。
- 参数化施法、Buff/GameplayEffect、完整属性表和技能生命周期事件在获得独立证据后再扩展，不提前固化进 v1。
