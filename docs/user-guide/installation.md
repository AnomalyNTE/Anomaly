# 安装与更新

普通用户不用编译源码。下载 Runtime 压缩包，完整解压后运行启动器即可。接入方式推荐 **启动 → 注入模式**（启动器里默认选中的那一项）：它把 Core 直接映射进 `HTGame.exe`，不往游戏目录写任何文件；**代理模式** 适合想让游戏目录自带 Anomaly、以后从快捷方式启动就生效的场景。

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
AnomalyLauncher.exe          安装、注入与更新
dwmapi.dll                   游戏启动时使用的代理入口
Anomaly\
  Anomaly.Core.dll           Runtime 主程序
  anomaly.ini
  repository.json
  plugin-repositories.json
  assets\
  locales\
  profiles\
  plugins\                   随包提供的内建插件包
```

从源码构建时，等价的目录位于 `.build\windows-vs2022\game-package`。具体步骤见[从源码构建](../developer-guide/building.md)。

## 推荐：注入模式接入

注入模式把 Core 直接映射进 `HTGame.exe`，不往游戏目录写 `dwmapi.dll` 或 `Anomaly\`，因此也不会和游戏目录里已有的文件混在一起。

1. 关闭仍在运行的 `AnomalyLauncher.exe`（更新旧版本时尤其要关）。
2. 运行刚解压出来的 `AnomalyLauncher.exe`。弹出 UAC 提示时允许即可。
3. 打开顶部的 **启动** 页面，确认 **启动器目录** 指向官方 `NTELauncher.exe`；自动找不到时点右侧的按钮重选。
4. 确认 **游戏目录** 指向**直接包含 `HTGame.exe` 的文件夹**，不要选游戏安装根目录。路径不对同样点右侧的按钮重选。
5. 页面下方保持默认的 **注入模式**，按游戏当前状态选一个按钮：
   - 游戏还没启动：点 **启动并附加**，启动器会拉起官方启动器，等待这次新起的 `HTGame.exe` 并映射 Core。
   - 游戏已经在运行：点 **注入到运行中的进程**，只映射 Core，不重启游戏。
6. 按平时的方式启动游戏，进入后按 `Insert` 呼出 Anomaly 界面。

两个按钮互补：页面里已经列出了正在运行的 `HTGame.exe` 时“启动并附加”不可用（它会另起一个进程），此时用“注入到运行中的进程”；反过来，没有正在运行的游戏时“注入到运行中的进程”不可用。

游戏目录通常是：

```text
Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64\
```

`AnomalyLauncher.exe` 不需要复制到游戏目录，游戏启动也不依赖解压出来的原始文件夹；可以保留它以后更新，也可以在需要时重新解压新版 Runtime。

## 代理模式安装（可选）

想让游戏目录自带 Anomaly、以后从快捷方式启动游戏就自动生效时，用代理模式。代价是游戏目录里会多出 `dwmapi.dll` 与 `Anomaly\`，也不能和别的同名代理（例如 RE-UE4SS）共存。

1. 先退出游戏和启动器。
2. 打开 **启动** 页面，在分段控件里切到 **代理模式**。
3. 状态显示“未安装”后点击 **安装**，等状态变成“已启用”再关闭启动器。
4. 像平时一样启动游戏，进入后按 `Insert` 呼出界面。

安装完成后，游戏目录里至少会有：

```text
Win64\
  HTGame.exe
  dwmapi.dll
  Anomaly\
    Anomaly.Core.dll
```

成功启动后会生成两份日志：

- `Anomaly\logs\anomaly-runtime.log`：Core 是否启动、游戏 PID 和诊断管道信息。
- `Anomaly\anomaly-platform.log`：界面、插件和平台服务日志。

`Anomaly\logs\` 下的结构化日志会按体积滚动，旧的日志文件也会被自动删除（最后写入超过 3 天，
或让目录超出 48 MiB），所以不需要手动清理。顺带一提：需要保留的日志请及时复制走。

如果按 `Insert` 没反应，先看这两个文件有没有生成，再按[故障排查](troubleshooting.md)继续检查。

## 自动检查更新

启动器会自己去 GitHub 看有没有新版，不需要手动找 ZIP：

1. 打开 `AnomalyLauncher.exe`，它会读取本仓库在 GitHub 上的最新 release，并与当前构建的版本比较。第一次成功后，启动器会记住是哪个加速入口答的（记在 `AnomalyLauncher.json` 的 `update.endpoint`），下次启动直接问它，不会先卡在连不上的地址上。
2. 有新版本时，启动器会弹出一个窗口：上面是当前版本，中间是这个版本的**更新说明**，下面左边是 **跳过此版本**、右边是 **更新到 x.y.z**。
3. 只有点了 **更新到 x.y.z** 才会开始：下载该版本的 `Anomaly-<版本号>-runtime.zip`（窗口里有进度条：已下载/总量、百分比、耗时，旁边还有 **取消**），解压后覆盖这个解压目录，然后自动重启到新版本。点 **跳过此版本** 就不再为这**一个**版本弹窗（记在 `AnomalyLauncher.json` 里，重启后也不再问），需要时仍然可以在 **设置 → 更新** 里更新。取消下载不算失败，版本还在，可以再来一次。

已经跳过或关掉弹窗后，模式栏（启动 / 模组 / 设置 那一行）会显示“发现新版本 x.y.z”，点它重新打开同一个窗口。

**设置 → 更新** 里随时可以手动点 **检查更新**；如果 GitHub 返回限流（HTTP 403），页面会直接说明，并给出去发布页的入口。

### 镜像与校验

**检查更新**（读取 release 信息）先直连 GitHub，失败或限流时依次换 gh-proxy 镜像重试：

```text
https://v4.gh-proxy.org/<原始地址>
https://gh-proxy.org/<原始地址>
https://v6.gh-proxy.org/<原始地址>
https://cdn.gh-proxy.org/<原始地址>
https://axisnow.gh-proxy.org/<原始地址>
```

**安装包本身不走镜像**：只从 release 自己公布的 `github.com` 地址下载，下载完先校验文件大小和 **SHA-256**（GitHub 的 asset digest），通过才解压。校验值会写进 `Anomaly\logs\launcher-update.log`。

自动更新只接受本仓库对应版本的 Runtime 包；缺少文件大小或有效 SHA-256 的包不能自动安装，可从发布页手动下载。

如果这个网络根本连不上 `github.com`，更新弹窗下面还有 **夸克网盘** 和 **百度网盘** 两个按钮，指向同一版本的国内下载源（夸克 `https://pan.quark.cn/s/81a25f50de52?pwd=gGdV`，百度 `https://pan.baidu.com/s/1x5PpiYT8GM2z0_axmcAJ9w?pwd=hfs5`）；从网盘下到的 Runtime ZIP 按[更新版本](#更新版本)的步骤手动解压即可。

更新只替换新版包里的文件，你写的东西会保留：

- `AnomalyLauncher.json`（启动器自己的设置）；
- `Anomaly\anomaly.ini`、`repository.json`、`plugin-repositories.json`；
- `Anomaly\config\`、`state\`、`logs\`、`crashes\`。

旧版本会先被改名成 `AnomalyLauncher.exe.old-<编号>`；新进程等待旧进程保存设置并退出后，再读取配置和清理旧文件。

不想让它自己联网，就把 **自动检查更新** 关掉。

> [!NOTE]
> 更新只作用于当前这个解压目录。已经装到游戏目录里的 `dwmapi.dll` 和 `Anomaly\` 不会跟着换，需要在 **启动 → 代理模式** 再点一次 **更新**。

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

在 **启动 → 代理模式** 页面点击 **禁用**，启动器会把：

```text
dwmapi.dll
```

改名为：

```text
dwmapi.dll.disabled
```

需要恢复时点击 **启用**。这个操作只影响**下一次**启动，不会把已经进入当前游戏进程的 Runtime 卸掉。建议先退出游戏再切换。

## 卸载

退出游戏，在 **启动 → 代理模式** 点击 **卸载**，核对弹窗中的目录与删除范围后确认。这会删除游戏目录中的 `dwmapi.dll`（或 `dwmapi.dll.disabled`）和整个 `Anomaly\` 目录，包括配置、插件与运行数据。只想暂时停用时使用 **禁用**。

## 不用启动器，手动复制

干净安装也可以手动完成：退出游戏，把 Runtime 包里的 `dwmapi.dll` 和整个 `Anomaly\` 目录复制到 `HTGame.exe` 旁边即可。

如果目标目录已经有 `dwmapi.dll`，先确认它属于谁。Anomaly 不会和另一个同名代理共用这个位置，也不应该直接覆盖来历不明的文件。已有安装的更新建议交给启动器处理，这样用户配置和自定义插件不会被普通文件覆盖误伤。

## 注入模式细节

注入模式不会把 `dwmapi.dll` 或 `Anomaly\` 复制到游戏目录。它把当前 Runtime 包中的 Core 手工映射进 `HTGame.exe`，两种时机都可以：

- **启动并附加**：由启动器启动官方 `NTELauncher.exe`，等待这次新建的 `HTGame.exe`，然后映射 Core。
- **注入到运行中的进程**：游戏已经在跑（自己从快捷方式启动的、或者启动器上次没接管的）也可以补挂。启动器只映射 Core，不重启游戏；失败时游戏保持原样继续运行。

注意两点：

- 保持整个 Runtime 包结构不变，注入时读的就是这个解压目录里的 Core 与 Profile。
- 注入和代理互斥。代理已启用时两个注入按钮都会禁用，需要先在 **启动 → 代理模式** 点 **禁用**。

补挂一个已经运行的游戏时，进入进程的方式由启动器自己按顺序尝试：先重定向停在系统调用里的线程（不改游戏代码、也不新建线程），再退回到新建线程。两条路都不创建配置文件、也不改游戏目录，因此游戏下次正常启动不受影响。

如果游戏从创建开始就限制进程句柄，注入会失败。这种情况请改用 **代理模式** 安装。

## 冲突与卸载

Anomaly 与 RE-UE4SS 都使用游戏目录下的 `dwmapi.dll`，两者不能同时放在这里。启动器遇到不认识的 `dwmapi.dll` 时会显示“存在冲突”，并且不会覆盖它。如果 `dwmapi.dll` 和 `dwmapi.dll.disabled` 同时存在，也会按冲突处理。

推荐使用 **启动 → 代理模式 → 卸载**，在确认弹窗中确认后即可删除游戏目录中的代理与整个 `Anomaly\`。发生冲突或需要手动移除时：

1. 退出游戏和启动器。
2. 删除游戏目录中的 Anomaly 代理：`dwmapi.dll` 或 `dwmapi.dll.disabled`。只删除确认由 Anomaly 安装的那一个，不要误删其他工具的同名文件。
3. 删除游戏目录中的 `Anomaly\`。需要保留设置或自定义插件时，先备份对应文件。
4. 如需一并清除启动器记住的路径，删除解压目录里的 `AnomalyLauncher.json`。

## 下一步

- [快速上手](quickstart.md)：第一次启动与呼出界面
- [第三方插件](third-party-plugins.md)：在游戏内下载、更新和卸载插件
- [配置参考](configuration.md)：调整切换键、语言和 Profile 目录
- [NTE Build Profile](nte-profiles.md)：了解依赖游戏结构的功能为什么可能不可用
