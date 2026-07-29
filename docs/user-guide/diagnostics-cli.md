# 诊断 CLI

Runtime 在游戏进程内开了一个本机命名管道，只允许当前 Windows 用户访问。`anomaly-cli.exe` 通过游戏进程 PID 连接这个管道，把命令原样发给 Runtime，再将返回的 JSON 写到标准输出。

## 基本用法

```powershell
.\anomaly-cli.exe --pid <PID> status
.\anomaly-cli.exe --pid <PID> modules
.\anomaly-cli.exe --pid <PID> sections .
.\anomaly-cli.exe --pid <PID> scan . .text "48 8B ?? ?? ?? ?? ?? 48 85 C0"
```

约定：

- `.` 表示**游戏主模块**。
- CLI 直接发送文本命令，每次返回一行 UTF-8 JSON。
- `snapshot [filename]` 会把结果写到 `Anomaly` 目录；文件名中的目录部分会被丢弃。
- PID 记录在 `Anomaly\logs\anomaly-runtime.log`。
- `anomaly-cli.exe` 当前固定连接 `\\.\pipe\LOCAL\Anomaly-<PID>`。如果你修改了 `[Analyzer] PipePrefix`，这个 CLI 将无法直接连接。

## 命令参考

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
| `ue functions <filter\|*> [limit] [cursor]` | 相关 UE5 Feature 可用时，只读分页查询 UFunction 名称、Owner 与参数元数据 |

### 内存读取

| 命令 | 结果 |
| --- | --- |
| `read <address> <size>` | 读取最多 1048576 字节 |
| `chain <base> [offset ...]` | 逐级解引用并应用偏移（指针链） |
| `ptr <address>` | 读取一个指针值 |
| `f32 <address> [count]` | 读取 32 位浮点 |
| `f64 <address> [count]` | 读取 64 位浮点 |
| `rip <instruction> <disp_offset> <instruction_size>` | 计算 RIP 相对指令的目标地址 |
| `snapshot [filename]` | 写入一份 JSON 快照到 `Anomaly` 目录 |

### 内存修改（危险）

| 命令 | 结果 |
| --- | --- |
| `write <address> <hex bytes>` | 写入已有可写页面并返回原字节 |
| `patch <address> <hex bytes>` | 临时切换为 RWX、写入、刷新指令缓存并恢复保护 |
| `protect <address> <size> <r\|rw\|x\|rx\|rwx>` | 修改页面保护并返回旧保护值 |
| `alloc <size> [protection]` | 在目标进程分配虚拟内存 |
| `free <address>` | 释放由 `alloc` 返回的区域 |

> [!TIP]
> 命令列表以 `help` 的实际输出为准；不同版本可能新增或调整命令。

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
