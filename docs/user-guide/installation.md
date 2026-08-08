# 安装与更新

普通用户不用编译源码。下载 Runtime 压缩包，完整解压后运行启动器即可。第一次安装建议使用“代理安装”；“实时附加”适合不想往游戏目录放代理文件的临时使用场景。

> [!CAUTION]
> Anomaly 会进入游戏进程并读取或修改运行状态。使用前请先阅读根目录 [README](../../README.md) 中的免责声明。

## 下载哪个包

发布页通常会同时提供几个压缩包。日常使用只需要：

```text
Anomaly-<版本号>-runtime.zip
```

`sdk.zip` 给插件开发者使用，`tools.zip` 是命令行工具，都不能代替 Runtime。

把 Runtime ZIP **完整解压**到一个普通文件夹，不要直接在压缩包里运行，也不要只拿出 `AnomalyLauncher.exe`。启动器安装时还要读取同级的 `dwmapi.dll` 和 `Anomaly\` 目录。

解压后的主要文件如下：

```text
AnomalyLauncher.exe          安装、更新和实时附加
dwmapi.dll                   游戏启动时使用的代理入口
Anomaly\
  Anomaly.Core.dll           Runtime 主程序
  AnomalyCrashCoordinator.exe
  anomaly.ini
  repository.json
  plugin-repositories.json
  assets\
  locales\
  profiles\
  plugins\                   随包提供的六个内建插件
```

从源码构建时，等价的目录位于 `.build\windows-vs2022\game-package`。具体步骤见[从源码构建](../developer-guide/building.md)。

## 推荐：用启动器安装

1. 先退出游戏。更新旧版本时，也要关掉仍在运行的 `AnomalyLauncher.exe`。
2. 运行刚解压出来的 `AnomalyLauncher.exe`。弹出 UAC 提示是正常的，启动器需要管理员权限才能写入游戏目录。
3. 留在顶部的 **代理安装** 页面。
4. 检查“游戏目录”。如果自动找到的路径不对，点击文件夹按钮，选择**直接包含 `HTGame.exe` 的文件夹**，不要选择游戏安装根目录。
5. 状态显示“未安装”后，点击 **安装**。等状态变成“已启用”再关闭启动器。
6. 像平时一样启动游戏，进入后按 `Insert` 呼出 Anomaly 界面。

游戏目录通常是：

```text
Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64\
```

安装完成后，这里至少会有：

```text
Win64\
  HTGame.exe
  dwmapi.dll
  Anomaly\
    Anomaly.Core.dll
```

`AnomalyLauncher.exe` 不需要复制到游戏目录。游戏启动也不依赖解压出来的原始文件夹；可以保留它以后更新，也可以在需要时重新解压新版 Runtime。

成功启动后会生成两份日志：

- `Anomaly\logs\anomaly-runtime.log`：Core 是否启动、游戏 PID 和诊断管道信息。
- `Anomaly\anomaly-platform.log`：界面、插件和平台服务日志。

如果按 `Insert` 没反应，先看这两个文件有没有生成，再按[故障排查](troubleshooting.md)继续检查。

## 更新版本

更新时不要把新 ZIP 直接覆盖到旧的解压目录里。把新版 Runtime 完整解压到一个新文件夹，然后：

1. 退出游戏。
2. 运行新版 `AnomalyLauncher.exe`，确认它选中了正确的 `HTGame.exe` 所在目录。
3. 状态显示“可更新”后，点击 **更新**。

启动器会比较新旧 `Anomaly.Core.dll` 的 SHA-256。Core 不同时才会显示“可更新”。更新过程中会保留：

- `anomaly.ini`、仓库配置和 `config\` 下的用户设置；
- `state\`、`logs\`、`crashes\` 中的运行数据；
- 自己添加的插件和其他不属于新版运行包的文件；
- 代理的启用或禁用状态。

新版自带的 Core、Profile 和内建插件会换成新包里的版本。也就是说，自己修改过的内建文件可能会被更新覆盖；需要长期保留的改动应放在自定义插件或本地配置中。

> [!NOTE]
> 安装和更新使用临时目录与备份目录完成切换。操作失败时，启动器会尽量恢复原安装，不需要先手工删除旧版。

## 暂时停用代理

在 **代理安装** 页面点击 **禁用**，启动器会把：

```text
dwmapi.dll
```

改名为：

```text
dwmapi.dll.disabled
```

需要恢复时点击 **启用**。这个操作只影响**下一次**启动，不会把已经进入当前游戏进程的 Runtime 卸掉。建议先退出游戏再切换。

## 不用启动器，手动复制

干净安装也可以手动完成：退出游戏，把 Runtime 包里的 `dwmapi.dll` 和整个 `Anomaly\` 目录复制到 `HTGame.exe` 旁边即可。

如果目标目录已经有 `dwmapi.dll`，先确认它属于谁。Anomaly 不会和另一个同名代理共用这个位置，也不应该直接覆盖来历不明的文件。已有安装的更新建议交给启动器处理，这样用户配置和自定义插件不会被普通文件覆盖误伤。

## 实时附加

**实时附加**不会把 `dwmapi.dll` 或 `Anomaly\` 复制到游戏目录。它会启动官方 `NTELauncher.exe`，等待这次新建的 `HTGame.exe`，然后把当前 Runtime 包中的 Core 映射进去。

使用时：

1. 保持整个 Runtime 包结构不变，并确认游戏尚未运行。
2. 运行 `AnomalyLauncher.exe`，切到 **实时附加**。
3. 如果没有自动找到官方启动器，手动选择 `NTELauncher.exe`。
4. 点击 **启动并附加**，再按官方启动器的正常流程进入游戏。

页面里只要列出了一个正在运行的 `HTGame.exe`，“启动并附加”就不会启用。它不是给现有进程补挂的按钮；请先退出游戏，再从这里重新启动。

如果游戏从创建开始就限制进程句柄，实时附加会失败。这种情况请改用代理安装。

## 冲突与卸载

Anomaly 与 RE-UE4SS 都使用游戏目录下的 `dwmapi.dll`，两者不能同时放在这里。启动器遇到不认识的 `dwmapi.dll` 时会显示“存在冲突”，并且不会覆盖它。如果 `dwmapi.dll` 和 `dwmapi.dll.disabled` 同时存在，也会按冲突处理。

启动器目前没有卸载按钮。要完整移除 Anomaly：

1. 退出游戏和启动器。
2. 删除游戏目录中的 Anomaly 代理：`dwmapi.dll` 或 `dwmapi.dll.disabled`。只删除确认由 Anomaly 安装的那一个，不要误删其他工具的同名文件。
3. 删除游戏目录中的 `Anomaly\`。需要保留设置或自定义插件时，先备份对应文件。
4. 如需一并清除启动器记住的路径，删除解压目录里的 `AnomalyLauncher.json`。

## 下一步

- [快速上手](quickstart.md)：第一次启动与呼出界面
- [第三方插件](third-party-plugins.md)：在游戏内下载、更新和卸载插件
- [配置参考](configuration.md)：调整切换键、语言和 Profile 目录
- [NTE Build Profile](nte-profiles.md)：了解依赖游戏结构的功能为什么可能不可用
