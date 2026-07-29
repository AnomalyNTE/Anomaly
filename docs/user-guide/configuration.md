# 配置参考

Anomaly 的平台配置位于 `Anomaly\anomaly.ini`。签名与结构偏移**不在** INI 中——它们属于 [NTE Build Profile](nte-profiles.md)；`anomaly.ini` 只选择游戏、Profile 目录与平台行为。

## anomaly.ini 全字段

```ini
[Analyzer]
PipePrefix=Anomaly
MaxScanResults=1024

[Platform]
Enabled=1
Visible=0
Embedded=1
AttachToProcessWindow=1
ToggleKey=45
Language=auto
PluginDirectory=plugins

[Performance]
UpdateSlowMilliseconds=2
DrawSlowMilliseconds=4
PlayerSnapshotTickInterval=1
EntitySnapshotTickInterval=1

[Profiles]
Game=nte
Directory=profiles
LocalDirectory=profiles-local
ManagedDirectory=state/profiles/managed
```

### [Analyzer]

| 字段 | 说明 |
| --- | --- |
| `PipePrefix` | 命名管道名前缀，供诊断协议使用。 |
| `MaxScanResults` | 单次签名扫描返回的最大命中数量。 |

### [Platform]

| 字段 | 说明 |
| --- | --- |
| `Enabled` | 是否启用平台（UI 与插件宿主）。 |
| `Visible` | 启动时界面是否可见，默认 `0`；按 `Insert` 手动呼出。 |
| `Embedded` | `1` 启用 D3D12 交换链内嵌模式（游戏内界面）。 |
| `AttachToProcessWindow` | 仅在关闭内嵌模式、使用 Win32 回退后端时生效；独立演示程序始终使用普通窗口。 |
| `ToggleKey` | 呼出 / 折叠界面的 Win32 虚拟键码。默认 `45` 即 `VK_INSERT`。 |
| `Language` | UI 语言，见下文。 |
| `PluginDirectory` | 插件目录（相对 `Anomaly\`），默认 `plugins`。 |

### [Performance]

| 字段 | 说明 |
| --- | --- |
| `UpdateSlowMilliseconds` | 判定插件 `on_update` 回调为"慢"的阈值（毫秒）。 |
| `DrawSlowMilliseconds` | 判定插件 `on_draw` 回调为"慢"的阈值（毫秒）。 |
| `PlayerSnapshotTickInterval` | 玩家快照的采样间隔（tick）。 |
| `EntitySnapshotTickInterval` | 实体快照的采样间隔（tick）。 |

### [Profiles]

| 字段 | 说明 |
| --- | --- |
| `Game` | 当前游戏标识（如 `nte`）。 |
| `Directory` | 运行包自带的 Profile 目录。 |
| `LocalDirectory` | 最高优先级的本地 Profile 覆盖目录。 |
| `ManagedDirectory` | 更新源下发的 Profile 目录。 |

Runtime 的选择顺序是：本地覆盖 > 更新源下发 > 运行包自带。详见 [NTE Build Profile](nte-profiles.md)。

## 语言

`Language` 允许三种取值：

- `auto` — 进程启动时按 Windows 用户语言选择。
- `en-US` — 英文。
- `zh-CN` — 简体中文。

> [!NOTE]
> 修改 `Language` 后需要**重启对应进程**才能生效。

## 切换键

`ToggleKey` 使用 Win32 虚拟键码。常见值：

| 键 | 十进制码 |
| --- | --- |
| `Insert`（默认） | `45` |
| `Home` | `36` |
| `End` | `35` |
| `F1` | `112` |

完整键码见微软的 [Virtual-Key Codes](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes) 文档。

## 界面布局

游戏内主界面是可拖动、可缩放的 ImGui 浮动窗口，位置与尺寸保存在 `Anomaly\anomaly-imgui.ini`：

- 首次显示时居中为 `1040×700`。
- 主管理窗口不提供关闭按钮，旧版本留下的关闭状态会在启动时自动修复。
- `Insert` 折叠 / 展开未锁定菜单；标题栏锁按钮可让单个窗口保持展开。

## 第三方插件（plugin-repositories.json）

日常添加、下载、更新和卸载插件的步骤见[第三方插件](third-party-plugins.md)。这里仅说明配置文件格式。

`Anomaly\plugin-repositories.json` 保存 **Plugins > 第三方插件** 中的插件源地址。运行包自带一个已启用的 GitHub 索引；具体地址以运行包中的 JSON 为准。插件源和下载地址默认必须使用 HTTPS。`file://` 只供本地测试，需要显式设置 `"allowInsecureSources": true`。

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "allowInsecureSources": false,
  "repositories": [
    {
      "url": "https://example.com/pluginmaster.json",
      "enabled": true
    }
  ]
}
```

在界面中应用插件源后会立即刷新，不需要重启 Runtime。如果网络刷新失败，界面仍会显示上一次成功获取的列表。安装时会重新下载插件包，并检查 ZIP 路径、Manifest ID、版本、游戏、Plugin API 和入口 DLL。

从插件源安装的插件可以在操作菜单中卸载。卸载只删除插件包，`Anomaly\config\plugins\` 中的设置会保留。

## 签名更新源（repository.json）

`Anomaly\repository.json` 是 Runtime 使用的 Profile 签名更新源。当前运行包默认关闭该功能（`"enabled": false`），也没有预置更新地址和信任密钥：

```json
{
  "schemaVersion": 1,
  "enabled": false,
  "allowFileSources": false,
  "withdrawalPolicy": "block-new",
  "freshness": {
    "maximumClockSkewSeconds": 300,
    "maximumIndexAgeSeconds": 86400,
    "maximumOfflineAgeSeconds": 604800,
    "downgradePolicy": "reject"
  },
  "sources": [],
  "trustKeys": []
}
```

要启用它，必须同时配置 `sources` 和 `trustKeys`。普通用户不需要修改这个文件。

## 相关

- [NTE Build Profile](nte-profiles.md) — 签名与结构偏移
- [插件的持久化设置](../developer-guide/plugin-development.md) — 插件自身的 JSON 配置存放在 `Anomaly\config\plugins\`
