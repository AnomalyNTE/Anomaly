# NTE Build Profile

玩家坐标、实体快照和传送都需要游戏内部的签名和结构偏移。这些数据放在 **Build Profile** 中，不写进 `anomaly.ini`。

## Profile 是什么

一个 Profile 记录签名、节区、结构偏移，以及每个 Feature 依赖哪些符号。Runtime 用它定位 `GWorld`、`GObjects`、`FNamePool` 和 Game Tick，然后按已验证的 Feature 向插件提供 UE5 / NTE 服务。

Profile 目录（相对 `Anomaly\`，见 [`[Profiles]` 配置](configuration.md#profiles)）：

| 目录 | 优先级 | 来源 |
| --- | --- | --- |
| `profiles-local\` | 最高 | 本地覆盖（你手动放置） |
| `state\profiles\managed\` | 中 | 仓库发布、签名的 Profile |
| `profiles\` | 最低 | 随运行包 bundled 的 Profile |

Runtime 按 `local override > repository > bundled` 的优先级选择**一个**活动 Profile。它只根据游戏 ID 和来源优先级选择，不会先计算当前 EXE 的 fingerprint 再做版本匹配。

## Runtime 如何使用 Profile

> [!IMPORTANT]
> Runtime 启动时不计算游戏 PE fingerprint，也不用 Build identity 选择 Profile。但在需要扫描签名时，仍会读取 Profile 指定的 `.text` 等节区。

- Resolver 每次启动都按活动 Profile 扫描符号，并对每个解析结果运行 Profile 声明的 validator。
- 符号组装完成后，Feature 仍要满足 Profile 声明的依赖、布局 validator、ABI 和线程条件。

### 自动学习与签名恢复

Runtime 会在符号成功解析后读取最多 128 字节的 x64 指令，自动生成用于下次恢复的指令签名，
不需要编写备用 pattern。指令解码保留 opcode、寄存器选择、成员和栈位移及普通立即数，
仅将 RIP 相对地址与相对分支操作数通配化。至少需要 4 条完整指令和 24 个固定字节；
生成的签名必须在声明的可执行节区内唯一匹配原位置，
才会写入 `state/signatures/<game>/signatures.json`。

原签名找不到目标或所有候选校验失败时，Resolver 尝试扫描学习记录；没有记录或学习窗口
零匹配时，尝试从原 pattern 中可完整解码的指令重新生成签名，保留原声明已有的通配符。
恢复候选仍须通过原有符号 validator 和 Feature 布局、ABI 校验。
多匹配、证据不足或保留的指令字节无法匹配时会保持降级，不按相似度猜测；
遇到不支持的指令时只使用此前已完整解码的证据。
RIP 相对目标地址会从新指令重新计算，不复用旧 RVA。CLI `ue` 的符号结果增加
`signatureRecovered` 和 `activePattern`，用于区分成功恢复与原签名命中。

学习记录按模块、节区、原 pattern 和活动 Profile 内容哈希/符号 ID 隔离；它不参与 Profile
选择，也不修改 Profile 来源或原文件。缓存损坏会忽略，写入失败作为诊断报告，
不影响已通过校验的解析结果。禁用 Profile overrides 的安全模式也会禁用 Profile 符号的学习与恢复。
学习与恢复在启动解析期间执行，停止请求会中止后续工作；单次底层节区扫描仍为同步调用。

插件签名保留在插件原有位置。现有 `anomaly.interop.signature` v1 接口使用同一恢复器，
学习记录分别放在各插件 state 目录的 `signature-knowledge/signatures.json`，无需修改插件、
manifest 或 SDK，也无需迁入总 Profile。Game/Render 回调不执行学习、恢复或状态文件 I/O。

需要同时学习字段偏移的插件可选择 signature v2：在插件内声明字段种子和函数证据，
由框架解析整组地址与字段快照，不把插件签名放入总 Profile。HiFi 已接入 PlayingID、Paused、
PlayerType、CurrentID、OwnedIDs，分别保留核心播放与可选专辑的降级边界。旧版 v1 插件保持原合同。

这是基于已有指令证据的恢复，无法覆盖任意函数重写。v1 不能修改已编译插件中的偏移常量；
v2 只更新明确声明且通过对象来源、访问宽度、对齐和一致性验证的字段，其他布局仍可能需要人工适配。

## Feature 降级

如果没有活动 Profile，或某些符号、布局、ABI 校验失败：

- Core、UI、Diagnostics 以及不依赖 NTE 的插件**照常工作**。
- 依赖失效符号的 Adapter 服务按 **Feature** 保持 `unavailable`。
- 声明这些服务为 `optional` 的插件仍能加载，只是对应功能不可用。

在管理界面的 **Diagnostics > Developer > NTE Compatibility**，或用 CLI 的 `ue` 命令，可以查看活动 Profile、符号解析结果和 Feature Matrix。

## 校验一个 Profile

`anomaly-profile.exe` 可以检查 Profile 格式，也可以离线生成 PE fingerprint。`fingerprint` 用于记录验证证据，Runtime 启动时不会调用它。

```powershell
# 1. （可选）离线生成 PE / .text 诊断身份
.\anomaly-profile.exe fingerprint C:\path\to\HTGame.exe nte

# 2. 从模板复制一份当前 Profile
Copy-Item .\Anomaly\profiles\nte\nte-build-profile.json.example `
          .\Anomaly\profiles\nte\nte-current.json

# 3. 填入经过验证的签名后校验
.\anomaly-profile.exe validate .\Anomaly\profiles\nte\nte-current.json
```

bundled 的 `nte-current.json` 保存当前活动声明。

## 兼容性说明

- **Profile 不会与 Build identity 自动匹配。** 实机验证可以记录当时的 fingerprint，但这个标记不是运行时兼容门。
- **mutation 服务更严格。** 修改类服务（如玩家传送）只在其引擎 ABI / 反射、依赖签名与 Game-thread gate 同时通过时才发布，且禁止 Pawn-vtable fallback。单次实机验证不构成跨版本支持。
- **Profile 或 Feature 校验失败**时，Core、UI、Diagnostics 和不依赖 NTE 的插件仍可使用。

> [!NOTE]
> 迁移说明与当前 NTE 坐标布局见仓库内的 [`profiles/nte/README.md`](../../profiles/nte/README.md)。Profile 与签名的具体格式见 [`schemas/build-profile.schema.json`](../../schemas/build-profile.schema.json)。

## 相关

- [配置参考 · [Profiles]](configuration.md#profiles)
- [诊断 CLI · `ue` 命令](diagnostics-cli.md#ue--profile-状态只读)
- [故障排查](troubleshooting.md)
