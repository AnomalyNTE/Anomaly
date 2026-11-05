# 模组（Mods）

> 🌐 [English](../../en/user-guide/mods.md) · 简体中文

启动器的「模组」页用来管理游戏目录里的 pak 模组：看有哪些、开关、卸载、重命名、导入 zip，以及打开「启用模组加载」所需的开关。

## 打开位置

启动器顶部模式栏：**代理安装 / 实时附加 / 模组**。点「模组」即可，不需要游戏在运行。

页面顶部显示实际使用的目录：

```
<游戏目录>\Client\WindowsNoEditor\HT\Content\Paks\~mods
```

> 目录随「客户端」切换（CN / Global）与游戏安装位置变化；启动器会自动推导，找不到时回退到 `~mods` 的默认位置。

## 能做什么

| 操作 | 说明 |
| --- | --- |
| 列表 | 每个模组一行：显示名 · 开/关 · 文件数 · 占用 · 作者 · 版本 |
| 启用 / 禁用 | 文件夹型改文件夹名，散装文件改文件名（加/去 `.disabled`） |
| 卸载 | 删除该模组的文件或文件夹 |
| 重命名 | 弹窗输入新名字，直接改磁盘上的名字 |
| 导入 zip | 选一个 mod 压缩包，自动解压并把 `.pak/.utoc/.ucas/mod.json` 放进 `~mods\<压缩包名>\` |
| 搜索 | 按显示名、目录名、作者模糊匹配 |
| 过滤 | 全部 / 已启用 / 已禁用 |
| 来源 | `mod.json` 里有链接时多一个按钮，用浏览器打开 |

**所有改动都需要重启游戏才生效**：pak 容器在启动时挂载，运行中不能卸载。

## 目录与文件约定

```
Paks\~mods\
  Mint CSU Prodigy Black\        ← 一个模组一个文件夹（推荐）
    mod.json
    xxx_P.pak
    xxx_P.utoc
    xxx_P.ucas
  SomeOldMod_P.pak               ← 也支持散装文件，按基名自动归组
  SomeOldMod_P.utoc
  SomeOldMod_P.ucas
```

- 支持扩展名：`.pak`、`.utoc`、`.ucas`（IoStore 模组通常是三件套）。
- 禁用标记：文件名或文件夹名结尾加 `.disabled`。
- `~mods` 的挂载优先级高于游戏自带的 `pakchunk*`，所以放在这里才会覆盖原版资源。

`mod.json`（可选）字段：

```json
{
  "name": "显示名称",
  "version": "1.0",
  "author": "作者",
  "modLink": "https://gamebanana.com/mods/xxxxx"
}
```

## 启用模组加载（验签绕道）

游戏会拒绝挂载**没有签名**的 pak。页面上的「启用模组加载」开关做的是：让 Core 在引擎挂载 pak 之前，把签名校验函数改成恒返回「有效」。

开关写在配置文件里：

```json
{
  "schemaVersion": 1,
  "enabled": true,
  "verifiedFor": "1.4.18",
  "module": "HTGame.exe",
  "pattern": "48 8D ?? ?? ?? ?? ?? E9 ... E9",
  "jumpOffset": 71
}
```

| 字段 | 含义 |
| --- | --- |
| `enabled` | 总开关。关闭时不做任何事（默认关闭）。 |
| `pattern` | 定位校验代码的 AOB 特征。**游戏更新后可能失效**，需要重新获取。 |
| `jumpOffset` | 从特征命中处偏移多少字节读到跳转指令（本版本为 71 / `0x47`）。 |
| `verifiedFor` | 这个 pattern 对应的游戏版本，仅作记录。 |
| `module` | 目标模块，通常是 `HTGame.exe`。 |

配置文件位置取决于运行方式：

- **代理模式**：`<游戏目录>\...\Binaries\Win64\Anomaly\config\pak-signature-bypass.json`
- **实时附加**：`<启动器目录>\Anomaly\config\pak-signature-bypass.json`

页面的开关会读写实际生效的那一份（两份都存在时都会更新）。

### 游戏更新之后

症状：模组全部失效，`Anomaly\logs\pak-signature-bypass.log` 里出现

```
pattern not found within the wait window
```

处理：

1. 重新获取对应版本的 `pattern`（社区配置或自行扫描，扫描可用只读探针插件）；
2. 更新 `verifiedFor`；
3. 保存后重启游戏。

### 限制与注意

- **必须重启游戏**，运行中不能启用/禁用模组。
- **只在代理模式生效**：实时附加发生在引擎挂载之后，本次会话的模组不会加载。
- 这条实现会改写游戏运行时的校验逻辑，游戏的反作弊可能将其视为非法修改；是否启用由你决定。
- 相关日志：`Anomaly\logs\pak-signature-bypass.log`（绕道）与 `Anomaly\logs\mod-operations.log`（排队操作的落盘结果）。

## 排障

| 现象 | 先看什么 |
| --- | --- |
| 模组不生效 | 是否代理模式启动；`enabled` 是否为 `true`；`pak-signature-bypass.log` 有没有 `hooked` |
| 列表为空 | 目录路径是否正确；文件扩展名是否是三件套；游戏是否把文件锁住了（运行中也能列出，但不能改） |
| 点了禁用没变化 | 文件是否被占用（先退出游戏）；文件名是否被别的程序改过 |
| 导入 zip 失败 | 压缩包是否有效；系统 `tar.exe` 是否可用（Windows 10+ 自带） |

## 浏览与在线安装

启动器第四个页签「浏览」直接对接 GameBanana 上的 Neverness To Everness 模组（游戏 id `23012`）。

| 操作 | 说明 |
| --- | --- |
| 搜索 | 关键词 + 「搜索」。**请用英文关键词**（`loading`、`hair`、`outfit`）：GameBanana 的搜索对中文几乎不过滤，中文词常返回一堆无关结果 |
| 留空浏览 | 关键词留空时浏览该游戏的最新模组 |
| 加载更多 | 每次追加一页（服务端固定 15 条/页） |
| 排序 | 按名称 / 下载量 / 更新时间 / 大小，再点同一按钮切换升降序 |
| 缩略图 | 列表左侧 120×68 预览；鼠标悬停显示 530px 大图（首次悬停才下载，之后走缓存） |
| 页面 | 用浏览器打开该模组的 GameBanana 页面 |
| 已安装 | 已通过本启动器安装过的模组会显示「已安装」，按钮变为「重装」 |

安装流程：取文件地址（`ProfilePage` 没有 `_aFiles` 时自动改用 `/Mod/<id>/Files`）→ 下载 → 按文件头判断格式（zip 用系统 `tar.exe`，7z/rar 用本机 7-Zip）→ 解压 → **整棵树复制**到 `~mods\<模组名>\` → 把封面写成 `icon.png` → 写入 `.aurora-source.json`。

网络：优先使用系统代理（本机实测走代理约 5 MB/s、直连约 14 KB/s），代理不可用时回退直连；下载失败会再尝试 `curl.exe`。失败原因写在 `Anomaly\logs\launcher-update.log`。

> 版本变慢或卡住时先看这个日志：`dl: total timeout`、`dl: stalled after N bytes`、`install: WinHTTP failed, trying curl`。

## 三类模组：哪些装了就能用

| 类型 | 形态 | 能否在本框架生效 |
| --- | --- | --- |
| 资源替换 | `.pak/.utoc/.ucas` 三件套 | ✅ 引擎直接挂载，配合「启用模组加载」即可 |
| 资源 + 配套文件 | 三件套 + 图片/脚本目录（如加载页替换：pak + 45 张 jpg） | ⚠️ pak 能挂上，但配套资源需要对应工具读取（例如 NTEUiSwap Plus），否则看不到效果 |
| Aurora 脚本 / 模块 | 只有 `.lua`、`.dll` 或模块清单 | ❌ 依赖 Aurora 运行时，本启动器不加载 |

判断方法：安装时会检查压缩包里有没有 pak 三件套；只有脚本的会被拒绝并提示「可能是 Aurora 脚本或模块」。

## 更新检查与在线更新

- 「检查更新」读取每个模组目录里的 `.aurora-source.json`（与 Aurora 同名同格式：`mod_id`/`file_id`/`file_name`/`md5`/`author`/`name`）。
- 判定：`file_id` 变化 **或** `md5` 变化即视为有新版本；没有来源记录的模组不做跟踪（与 Aurora 一致）。
- 首次登记：模组 `mod.json` 里有 `modLink` 时，第一次检查会把当前最新文件记为基线。
- 有新版本时该行出现 `↑ 新版本 vX` 与「更新」按钮，点击后下载并替换文件、保留 `icon.png`。
## 界面偏好

启动器会记住这些选择（存在 `<启动器>\\Anomaly\\config\\launcher-prefs.json`）：

| 键 | 含义 |
| --- | --- |
| `browseSort` | 下载页的全量排序（`default` / `new` / `downloads` / `views` / `likes`） |
| `browseCategory` | 选中的分类 id（空 = 全部分类） |
| `hideMature` | 「隐藏 NSFW」勾选状态 |
| `gridView` | 模组管理页是否用网格视图 |

## 能力边界（都由 GameBanana 接口决定）

| 想做的事 | 接口现实 | 我们的做法 |
| --- | --- | --- |
| 按下载量/浏览/喜爱全量排序 | 只有 `apiv13/Mod/Index` 支持（`Generic_MostDownloaded` / `MostViewed` / `MostLiked` / `Newest`） | 走该接口，选排序即重新拉第一页 |
| 关键词搜索 + 排序 | 搜索只有 `_sOrder=best_match`，其它值 400 | 搜索按相关性，界面上隐藏分类与全量排序 |
| 搜索 + 分类 | 搜索接口忽略分类参数 | 搜索时**隐藏**分类下拉 |
| 列表里显示下载量 | 列表接口不返回 `_nDownloadCount` | 逐 mod 从 `/Mod/<id>/Files` 取（和文件大小同一次请求） |
| 分类筛选 | `_aFilters[Generic_Category]` 只认**数字 id** | 从记录的分类 URL（`…/cats/43029`）取 id |
| NSFW 过滤 | NTE 的记录里只有 `_bHasContentRatings` 被填 | 本地过滤；`_bHasNsfwContent`/`_bIsNsfw`/分类名含 NSFW 也一并判定 |

## 已知限制

- **游戏运行时**不能启用/禁用/卸载/重命名/导入/安装：`.ucas`/`.utoc` 被引擎占用，改名会失败甚至半途中断。界面会把这些按钮置灰并给出提示。
- **改动都要重启游戏**才生效（pak 只在启动时挂载）。
- **Lua 脚本类 mod 不生效**：本框架没有 Lua 运行时。压缩包里有 `.lua` 或页面描述提到 Lua/UE4SS/NTEUiSwap 时，会写入 `needsLua` 标记，模组管理页顶部显示「可能有 N 个模组需要 Lua 运行时」。
- **Aurora 模块（dll/包）** 无法使用：那需要 Aurora 自己的模块加载器。

## 关于 RE-UE4SS（实验记录）

我们试过用插件在 `game-ready` 阶段加载 `ue4ss\UE4SS.dll`（官方实验版，`MinorVersion=6` 可认 UE5.6，Lua mod 能跑，实测 `NTENoTraffic` 生效）。但**只要 UE4SS 驻留进程，框架热键就明显失灵**，关掉多数 hook 与内置按键 mod 也无法消除。因此当前版本不启用该方案，相关文件已移出游戏目录。