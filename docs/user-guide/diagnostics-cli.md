# 诊断 CLI

Runtime 在游戏进程内开了一个本机命名管道，只允许当前 Windows 用户访问。`anomaly-cli.exe` 通过游戏进程 PID 连接这个管道，把命令原样发给 Runtime，再将返回的 JSON 写到标准输出。

## 基本用法

```powershell
.\anomaly-cli.exe --help
.\anomaly-cli.exe --pid <PID> status
.\anomaly-cli.exe --pid <PID> modules
.\anomaly-cli.exe --pid <PID> sections .
.\anomaly-cli.exe --pid <PID> scan . .text "48 8B ?? ?? ?? ?? ?? 48 85 C0"
```

约定：

- `.` 表示**游戏主模块**。
- CLI 直接发送文本命令，每次返回一行 UTF-8 JSON；判据是 JSON 里的 `ok` 字段。
- 参数以单个空格拼接后发送，因此 `scan . .text 48 8B ?? 48 85 C0` 既可以整体引号传入，也可以拆成多个参数。参数不得含换行，整个请求不超过 64 KiB。
- `snapshot [filename]` 会把结果写到 `Anomaly` 目录；文件名中的目录部分会被丢弃。
- PID 记录在 `Anomaly\logs\anomaly-runtime.log`。
- `anomaly-cli.exe` 当前固定连接 `\\.\pipe\LOCAL\Anomaly-<PID>`。如果你修改了 `[Analyzer] PipePrefix`，这个 CLI 将无法直接连接。
- 普通权限终端即可连接：Runtime 以中完整性标签发布诊断管道，无需管理员权限。

## 帮助与退出码

`--help`、`-h`、`/?` 与 `help` 都会把完整语法写到标准输出并以 `0` 退出；参数不合法时帮助写到标准错误，标准输出保持为空。

| 退出码 | 含义 |
| --- | --- |
| `0` | Runtime 已应答；结果好坏由 JSON 的 `ok` 字段决定 |
| `2` | 客户端用法错误：`--pid` 缺失或非法、参数无法编码、请求超过 64 KiB |
| `3` | 连接失败：`2` 表示该 PID 没有管道，`5` 表示拒绝访问，`231` 表示所有管道实例都在忙 |
| `4` | 管道读取模式配置失败，或请求写入失败 |
| `5` | 响应读取失败，或标准输出写入失败 |

连接失败时会额外打印管道名与针对性提示（PID 不对、权限或完整性级别不符）。

## 命令参考

参数格式：

- `<address>`：`0x` 前缀十六进制或十进制，例如 `0x147e40ba0`；大小与偏移使用同一个解析器，`0x10` 与 `16` 等价。
- `<section>`：PE 节区名，例如 `.text`。
- `<pattern>`：十六进制字节；`??` 为整字节通配，`4?` 固定高半字节，`?F` 固定低半字节。
- `<hex bytes>`：空格分隔的十六进制字节，例如 `90 90` 或 `E9 1B 00 00 00`，最多 4096 字节。
- `<protection>`：`r`、`rw`、`x`、`rx`、`rwx`，或数字形式的 `PAGE_*` 值。

### 状态与枚举（只读）

| 命令 | 结果 |
| --- | --- |
| `ping` | 服务 PID |
| `status` | PID、Runtime 根目录、平台模式与 Runtime 诊断摘要 |
| `modules` | 已加载模块、路径、基址、映像大小 |
| `sections <module\|.>` | PE 节区、基址、大小、RWX 属性 |
| `regions <module\|.>` | 内存区域状态、保护和类型 |
| `help` | 命令列表 |

### 扫描与交叉引用（只读）

| 命令 | 结果 |
| --- | --- |
| `scan <module\|.> <section> <pattern>` | 当前内存中的签名命中地址，`pattern` 支持 `??`、`4?`、`?F` 通配 |
| `xrefs <module\|.> <target>` | 指向 `target` 地址的交叉引用 |

### UE / Profile 状态（只读）

| 命令 | 结果 |
| --- | --- |
| `ue` | 活动 Profile、符号解析结果与 Feature Matrix |
| `ue actors <filter\|*> [limit] [cursor]` | 相关 UE5 Feature 可用时，只读分页查询 Actor 名称、类别与 Outer |
| `ue objects <filter\|*> [limit] [cursor]` | 相关 UE5 Feature 可用时，只读分页查询对象名称、类别与 Outer |
| `ue functions <filter\|*> [limit] [cursor]` | 相关 UE5 Feature 可用时，只读分页查询 UFunction 名称、Owner 与参数元数据 |
| `ue fname <id>` | 把 FName 的 id 解析为文本 |
| `ue ftext <address>` | 读取该地址上的 FText 显示字符串 |
| `ue combat` | 战斗桥接状态 |
| `ue buffs` | Buff 快照 |

`<filter>` 是名称或类别的子串，`*` 表示不筛选；`limit` 与 `cursor` 用于分页，游标由上一步结果返回。

### 内存读取

| 命令 | 结果 |
| --- | --- |
| `read <address> <size>` | 读取最多 1048576 字节，返回原始字节与解码值 |
| `chain <base> [offset ...]` | 逐级解引用并应用偏移（最多 64 个偏移），返回最终地址 |
| `ptr <address>` | 读取一个指针值 |
| `f32 <address> [count]` | 读取 32 位浮点，`count` 默认 3 |
| `f64 <address> [count]` | 读取 64 位浮点，`count` 默认 3 |
| `rip <instruction> <disp_offset> <instruction_size>` | 计算 RIP 相对指令的目标地址，可读时附带该地址上的指针 |
| `snapshot [filename]` | 写入一份 JSON 快照到 `Anomaly` 目录 |

### 内存修改（危险）

| 命令 | 结果 |
| --- | --- |
| `write <address> <hex bytes>` | 写入已有可写页面并返回原字节 |
| `patch <address> <hex bytes>` | 临时切换为 RWX、写入、刷新指令缓存并恢复保护 |
| `protect <address> <size> <r\|rw\|x\|rx\|rwx>` | 修改页面保护并返回旧保护值 |
| `alloc <size> [protection]` | 在目标进程分配虚拟内存，最大 268435456 字节，默认 `rw` |
| `free <address>` | 释放由 `alloc` 返回的区域 |

> [!TIP]
> `anomaly-cli.exe --help` 输出与本文一致的完整语法、参数格式和退出码；命令列表以 Runtime 的 `help` 实际输出为准，不同版本可能新增或调整命令。

## 直接调用 JSON 协议

自己编写管道客户端时，可以发送版本化的 JSON 请求。`anomaly-cli.exe` 不使用这种格式，它走的是上面的单行文本命令接口。

```json
{"protocol":"anomaly.diagnostics","version":1,"type":"request","id":"status-1","command":"status"}
```

响应保留 `protocol`、`version`、`type`、`id` 和 `ok`；成功结果位于 `result`，错误使用带稳定 `code` / `message` 的 `error`。

`status` 响应中的 Runtime 字段包含服务、插件和资源诊断信息。具体字段以当前版本的 `status` 输出为准。

> [!NOTE]
> 该协议用于**诊断**，不是插件间 IPC。插件之间通信使用 [`anomaly.ipc`](../api-reference/ipc.md)。

## 相关

- [NTE Build Profile](nte-profiles.md) — `ue` 命令依赖活动 Profile
- [故障排查](troubleshooting.md) — 连接不上管道、命令返回错误
