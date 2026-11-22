# 自动日常（Auto Daily）

内建插件：进游戏后一条龙完成日常——把打副本交给 [Clone Enter](../CloneEnter/)，接着领一咖舍、
领奖励。首次使用配置一次，之后每次启动游戏自动跑一次；游戏内 **Ctrl+S** 随时强制停止。

- 插件 id：`yu-1021.daily`
- 包目录：`Anomaly/plugins/AutoDaily`
- 版本：4.3.1（与 `manifest.json` 一致）

## 功能

| 阶段 | 做什么 | 依赖 |
| --- | --- | --- |
| 等世界 | `anomaly.nte.session` 报 `WORLD_READY`，且游戏内才有的控件（`W_HTButton_AdventureManual` / `W_BoundActionButton_Close_C`）出现在目录里且不是隐藏状态，才算真的进了游戏；期间每秒检测一次登录按钮 `UI_Button_Login`（最多 8 次），就绪后再等 `world_settle_seconds` 秒 | nte.session（可选）+ ui-buttons |
| 打副本 | 经 `anomaly.ipc` 调 `ccyellowstar.clone-enter.onekey`：`start` / `status` / `stop`；`start` 里固定带 `"melee":true`，平A模式常开、没有开关 | Clone Enter ≥ 0.2.16（可选） |
| 领一咖舍 | `ServerStoreManagementRPC`（Type=17）直发，再逐层关弹窗：`Button_Sure`「确定」→ `ButtonDeactivate` → `WB_CommonDeactivate`（最外层 `UI_CommonAwardsPopUpWindows_C_<实例号>`） | 反射 + ui-buttons |
| 领每日活跃奖励 | 反射领取探索指南五类，再走控件链：开面板 → **「全部」tab（每日活跃页）** → 活跃度奖励 → 最终领取 → 关弹窗 → 关面板 | 反射 + ui-buttons |
| 领环期任务奖励 | 控件链：**`W_HTButton_CombatAward`（HUD 入口，不先开面板）** → **「环期任务」tab** → **`BtnSubmitAllQuest`「全部领取」** → 关弹窗 → 关面板 | ui-buttons |

阶段顺序固定为 **打副本 → 领一咖舍 → 领每日活跃奖励 → 领环期任务奖励**：一咖舍的 RPC 与弹窗都
处理完（`FonsChainBusy()` 为假）才进奖励阶段，日志里对应 `dungeon stage finished` /
`cafe stage finished` / `daily reward stage finished`。

两种领取是**两条独立链路**：设置里各有一个开关（`stages.daily_rewards` /
`stages.periodic_rewards`），面板上也有各自的一键按钮，可以只跑其中一种。前者按
`ReceiveButton`（活跃度奖励）→ `ButtonAccept`（最终领取）的顺序点，并把同名按钮**全部**
点一遍；后者点 `BtnSubmitAllQuest` 一次性全部领取。

领取按钮是**幂等**的：控件在目录里但宿主说不可点击（已经领过 / 条件不满足）时，直接记
`reward step … nothing to claim, skipping` 往下走，不耗重试、不回退页签；只有「页面没切过去」
这类问题才会重切（最多 2 轮），单步重试 5 次封顶。整条链在「今天已经领完」的情况下会很快走完。

环期任务入口 `W_HTButton_CombatAward` 在**主界面 HUD** 上：先开探索指南面板会把它挡成不可点击，
点下去只会看到上次停留的每日活跃页。所以这条链路先点一次关闭（清掉上一条链路残留的面板），
再点 HUD 入口，随后才点「环期任务」tab。名字对了但宿主报不可点击时，日志会打出
`... present but not clickable (category=N)`，一看就知道被什么挡住了。

窗口关闭键 `W_BoundActionButton_Close_C` 的控件名带实例号后缀（`..._C_2147458308`），
所以按**名字前缀**匹配，实例号每次都变也照样点得到。

收尾是**验证式**的：依次试 `Button_Sure`「确定」→ `ButtonDeactivate` → `WB_CommonDeactivate`
→ `W_BoundActionButton_Close*`「关闭」，一次只点一个，点完重新扫目录，直到面板的 tab 行
`BPUI_RadioBox_Table_1` 与关闭键都变成 HIDDEN（真的从屏幕上消失）才算这一步过了。宿主的按钮目录
会保留已关闭窗口的控件，只有 `category=HIDDEN` 才代表它不在屏幕上——这正是以前「点了关闭就当完成」
误判的地方：面板还开着链路就进了下一步，而下一步要点的 HUD 入口被面板挡成不可点击，整条链卡死。
关不掉时会打出屏幕上还剩下的控件（`reward ui candidate: …`）并**停止整条链**
（`stage.reward_window_stuck`），提示手动关窗后再继续，而不是带着开着的面板往下走。

两个 tab 用的是同一个控件名 `BPUI_RadioBox_Table_1`，只能靠 caption 区分（「全部」/「环期任务」）。
caption 属于游戏数据，代码里写成 UTF-8 字节转义与原文本逐字比较，不参与本地化；点错或找不到时
日志会把实际 caption 打出来。

弹窗根节点带实例号（如 `UI_CommonAwardsPopUpWindows_C_2147456120/WB_CommonDeactivate`），
所以弹窗按钮按「控件名 或 path 最后一段」匹配，实例号变了也不影响。

切页与开面板是前置步骤：只点宿主判定为 `CLICKABLE` 的按钮（不可点击的点了没有任何效果），
前置步骤没做成会退回本链路自己的切页步骤重来（每日活跃退「全部」、环期任务退「环期任务」页，
最多 2 轮），而不是把后面的领取步骤空点一遍；重切仍失败才跳过该步并在日志里留痕。
日志会打出被点 tab 的 caption，便于确认切的是哪一页。

打副本不重复造轮子：本插件只发 IPC 请求，不碰副本寻路、战斗、结算逻辑。Clone Enter 未安装或
未启用时自动跳过副本阶段，其余阶段照常执行。

## 「每次启动自动运行」

勾上这个开关**不会立刻执行**：插件会记下当前游戏进程的会话标识（PID + 进程创建时间），
之后**每次启动游戏**、且**总开关开着**时，都会在进入游戏后自动跑一遍。开关是常驻的：
跑完不会自己关掉，只有你手动取消勾选才会停。同一次游戏内插件热重载、热更都不算重启，
不会重复触发；手动点「开始一条龙」或单阶段按钮也不影响这个开关。

登录页的 `UI_Button_Login` 要先发一次 `request_scan`、等扫描完成后再读目录——宿主的按钮目录
只在扫描完成后才更新，没扫过时 `button_count` 恒为 0。

**一律只检测、不强制**：所有代点都要求宿主报 `CLICKABLE` 才发请求，不用 `FORCE` 标志。
登录按钮没就绪（加载遮罩、还没加载出来）就继续等，一秒一轮。

判据按实测校准：**不拿登录页做门**。进世界后 `UI_Button_Login` 还会残留在目录里、甚至仍报
可点击，用它判断会永远卡住；所以反过来只认「游戏内才有的控件」——
`W_HTButton_AdventureManual` 或 `W_BoundActionButton_Close_C` 出现在目录里且不是隐藏状态，
才算真的进了游戏。卡住时会周期性打出
`waiting to enter the game: session_ready=… login_page=… hud=… buttons=…`，
一看就知道是没扫到控件还是确实还没进游戏。

## 配置

配置文件：`Anomaly/config/plugins/yu-1021.daily/config-yu-1021.daily.settings.json`
（外层是宿主信封 `{"data":{...},"schemaVersion":1}`，改里面的 `data` 即可；插件每 2 秒热读
一次，改完保存就生效，不用重启）。

```jsonc
{
  "data": {
    "enabled": true,              // 总开关
    "configured": true,           // 首次使用配置完成标记
    "auto_run_pending": true,     // 「每次启动自动运行」：勾上不会立刻执行，之后每次启动都跑
    "auto_run_pid": 12345,        // 勾选时的游戏进程 PID（判断「是否已重启」用，自动维护）
    "auto_run_stamp": 987654321,  // 勾选时进程创建时间秒（同上）
    "stages": {
      "dungeon": true,            // 打副本
      "cafe": true,               // 领一咖舍
      "daily_rewards": true,      // 领每日活跃奖励（「全部」页）
      "periodic_rewards": true    // 领环期任务奖励（「环期任务」页 全部领取）
    },
    "dungeon": {
      "index": 0,                 // 副本序号，见下表
      "sub_index": 0,             // 子副本序号
      "level_index": 1,           // 难度
      "runs": 6,                  // 打几次
      "enter_wait": 4,            // 进本等待（秒）
      "exit_wait": 8              // 出本等待（秒）
      // 平A模式固定开启（请求里恒为 "melee":true），没有开关，也不写进配置
    },
    "hotkey": { "stop_modifiers": 2, "stop_vk": 83 },   // Ctrl + S
    "timing": { "world_settle_seconds": 8, "stage_timeout_seconds": 1200 },
    "dungeon_table": [ /* 只读对照表，写配置时自动生成，读取时忽略 */ ]
  },
  "schemaVersion": 1
}
```

`hotkey.stop_modifiers` 按位：Shift=1、Ctrl=2、Alt=4、Super=8；`stop_vk` 是 Win32 虚拟键码
（S=83）。

### 副本序号对照表

配置文件里的 `dungeon_table` 与面板里的列表由同一张表生成：

| 序号 | 游戏入口 | 名称 | 子副本 | 难度范围 |
| --- | --- | --- | --- | --- |
| 0 | 1 | 资源副本 | 0=角色经验 1=弧盘经验 2=甲硬币 | 1–6 |
| 1 | 2 | 异能材料副本 | 0=小心鸽子 1=扑克茶会 2=惊喜派对 3=心电感应 4=越狱艺术 | 1–7 |
| 2 | 3 | 弧盘突破副本 | 0=苹果核 1=螺旋乐 2=液态梦 3=冷甜点 4=戏剧芯 | 1–7 |
| 3 | 4 | 空暮副本 | 0=失落光芒和迪亚波 1=恶魔之血和街头拳王 2=森林萤火和真红 3=缇娅和守卫王国 4=音速蓝刺猬和静谧山庄 5=影之信条和小小大冒险 | 1–6 |
| 4 | 5 | 周本·墨菲克斯 | 单本 | 1–7 |
| 5 | 6 | 周本·永不谢幕 | 单本 | 1–7 |
| 6 | 9 | 抢银行 | 单本 | 1 |
| 7 | 13 | 周本·讨债人 | 单本 | 1–7 |

「序号」是 Clone Enter 面板里从上往下的第几项（也就是 `dungeon.index` 的值），与游戏本体的
副本入口序号（表中「游戏入口」列，1/2/3/4/5/6/9/13）不是一回事。越界的子副本、难度会被钳制到
合法范围；面板选本方式和 Clone Enter 一样，用菜单点名字。

## 面板

`on_draw` 里的一张窗（标题取 `window.title`）：

- 首次使用（`configured=false`）显示配置向导：总开关、每次启动自动运行、阶段开关、副本/子副本
  菜单 + 难度/次数/等待，下面附对照表，确认后写回配置。
- 副本选择区与 Clone Enter 面板一致：副本菜单点名字、子副本菜单（多子本才有）、难度按该本
  上下限钳制，切本时子副本归零、难度回到下限。
- 之后显示：阶段进度、动作按钮（开始一条龙 / 停止 / 立即领每日活跃奖励 / 立即领环期任务奖励 /
  立即领一咖舍 / 立即打副本 / 保存配置 / 恢复默认）、设置项、日志。

## 构建与打包

由仓库根 `CMakeLists.txt` 的 `if(ANOMALY_BUILD_BUILTIN_PLUGINS)` 块登记
（`anomaly_add_plugin(anomaly_builtin_auto_daily …)` + `ANOMALY_BUILTIN_PLUGIN_TARGETS`），
本目录不放自己的 `CMakeLists.txt`：

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo --target anomaly_builtin_auto_daily
```

产物在构建树的 `builtin-plugins/AutoDaily/`（`plugin.dll` + `manifest.json` +
`locales/` + `profile/`），并随 `GameRuntime` 组件安装到 `Anomaly/plugins/AutoDaily`。

## 源码结构

| 文件 | 职责 |
| --- | --- |
| `src/plugin.cpp` | 生命周期、服务查询、Worker 线程配置读写、面板 |
| `src/state.hpp/.cpp` | 共享状态、日志、状态栏、`HAS()` 能力探测 |
| `src/settings.hpp/.cpp` | 设置模型、副本表、JSON 序列化与 `anomaly.config` 读写 |
| `src/profile_layout.hpp/.cpp` | 读取并校验 `profile/compatibility.profile.json`、解析签名 |
| `src/game.hpp/.cpp` | 内存读取、`ProcessEvent`、ui-buttons 代点、两条控件链路 |
| `src/dungeon.hpp/.cpp` | Clone Enter 的 IPC 客户端（`invoke_async`） |
| `src/chain.hpp/.cpp` | 一条龙状态机 |
| `src/i18n.hpp/.cpp` | `anomaly.localization` 薄封装，英文回退 + `{0}..{7}` 占位 |

## 约定与限制

- **偏移和签名只在 profile 里**：`profile/compatibility.profile.json` 是唯一来源；文件缺失、
  `contract` 不匹配或任一字段越界时，反射相关功能（一咖舍 RPC、反射领取、本地
  `PlayerState`）整段停用，只用控件链，绝不回落到代码里的默认偏移。
- **Game/Render 回调不做文件 I/O**：配置写入与热读都通过 `anomaly.scheduler` 投递到 Worker 线程。
- **IPC 走异步**：Game 线程禁止阻塞式 `invoke`，副本请求全部 `invoke_async`，响应在下一
  Game tick 解析。
- **文案全部本地化**：中文只在 `locales/zh-CN.json`，`src/*.cpp` 里只写英文回退；
  `tools/check_plugin_i18n.ps1 -Plugin AutoDaily` 校验 key 覆盖与残留中文字面量。
- 停止语义：Ctrl+S 热键只置一个原子标志，由 Game tick 消费；`enabled=false` 也会在下一 tick
  停止；停止时若正在打副本，会向 Clone Enter 发 `stop`。
