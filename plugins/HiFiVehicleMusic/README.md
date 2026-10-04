# HiFi Vehicle Music

拦截游戏载具背景音乐（`UHTSoundSubsystem` 的 player music），用本地无损文件和独立输出后端
播放，支持 WASAPI 独占 / WASAPI 共享 / DirectSound / ASIO。

## 工作方式

1. 通过 `anomaly.interop.signature` 在 `HTGame.exe` `.text` 中解析 6 个必需函数和 7 个专辑函数（外加
   3 个辅助函数），用 `anomaly.interop.hook` 挂 detour。必需签名或任一 Hook 失败则释放全部 Hook，插件退化为不接管；
   专辑部分全有或全无：任一缺失则不加专辑，游戏歌单保持原样。
2. `PostPlayerMusicSound` detour：读取 `PlayerType`（载具 = 1）与 `UAkAudioEvent` 的 FName，
   经 `anomaly.ue5.names` 解析出事件名。若插件启用、为载具音乐且曲库非空，则跳过原函数（Wwise 不发声），先调用原始 `StopPlayerMusicSound` 停掉正在播的 Wwise
   音乐，再把播放命令投递给音频引擎；否则停止引擎并调用原函数。
   `pos` 按 0–1 的比例处理（0 或 ≥1 从头播放）。这一语义来自反汇编推断，尚未实测确认。
3. `StopPlayerMusicSound` / `Pause` / `Resume` detour：同步通知引擎，再调用原函数。
   `AHTPlayerCharacter::EndGetOffVehicle` detour：下车动作结束时，若该角色是本地玩家
   （`Pawn.Controller → PlayerController.Player → Player.PlayerController` 回指同一控制器，
   排除 NPC/AIController）则立即停止引擎。原因：接管时跳过了原始 Post，PlayingID 为 0，
   游戏自己的下车停止路径都按 PlayingID 判断而被跳过，所以必须用下车动作本身作信号。
   `SetMusicPlayerType` 切到非载具、非载具事件的 Post 同样立即停止，不走宽限期。
4. 曲库：`musicFolder` 目录（含子目录）下所有 `.flac|.wav|.mp3|.ape|.dsf|.dff`，文件名无需修改，按路径排序
   组成歌单，替代游戏电台：首个载具音乐事件开始播放，之后游戏切歌/切台不打断当前歌曲，
   只有歌曲播完（自动接下一首，末尾循环）或点「下一首」才切换。游戏 Stop 后 1.5 s 内没有新的
   载具 Post 才真正停止并释放设备；本地玩家下车（`EndGetOffVehicle`）立即停止。可选：若存在与事件名同名
   的文件（如 `Play_Music_Radio_Progressive_Metal_001.flac`），该事件固定播放它并单曲循环、
   跟随游戏 seek。目录扫描在引擎线程完成，Hook 与 Draw 只读取内存快照，不做文件 I/O。
5. 游戏内专辑（名称为曲库文件夹名，如 `D:\Music\周杰伦` → 「周杰伦」；换文件夹后重建专辑行）：曲库有几首，专辑里就有几首（追加在游戏歌曲之后，不替换、不隐藏游戏歌曲）。
   - 合成行：音乐表 `ForeachRow` 遍历时记下第一行作模板，按曲库标题复制出 `FPlayerMusicData` 行
     （标题用游戏自己的 `FText::FromString`，`AlbumID` 指向新专辑，排序在所有游戏条目之后）；
     专辑行复制模板歌曲所属专辑的 `FMusicAlbumData`。行名 FName = 真实行的 ComparisonIndex +
     自定义 Number（第 k 首 `0x48460000+k+1`，专辑 `0x4846FFFF`）。行常驻不释放（UI 持有行指针）。
   - 查找/遍历：`FindRow` / `GetAlbumData` 命中合成名时返回合成行；两个 `ForeachRow` 遍历完原表后追加合成行。
   - 已拥有列表：`GetOwnedMusicListIDs` 的副本追加曲库 ID；`ReGenerateNewMusicListIDs` /
     `SetCurrentMusicListID` 直接读 `+0x3E0`，调用前临时加入、调用后原位移除，存档不会写入合成 ID。
   - 播放：只有专辑里的歌（list ID 解析出曲库序号 k）走 HiFi 引擎播放曲库第 k 首；游戏自带歌曲照常由 Wwise 播放。
   - 重新扫描曲库后按新标题重建合成行（旧行泄漏，避免 UI 悬垂指针）。
   - 锁定状态：音乐面板 `RefreshItemStates`、载具音乐面板列表构建、`ResolveCurrentMusicListID`、
     `SyncCurrentMusicListID` 也直接读 `+0x3E0`（不经 `GetOwnedMusicListIDs`），不加入就会被判为未拥有
     （state 3 = 锁定）或把当前歌切回游戏歌曲；同样只在调用期间临时加入（嵌套调用只加一次）。
   - 专辑封面：曲库根目录下的 `cover|folder|front|album` + `.png|.jpg|.jpeg|.bmp`，用游戏自带的
     `ImportFileAsTexture2D` 导入为 Transient 纹理，`FUObjectItem::SetFlags(RootSet)` 防 GC，
     专辑行的 `TSoftObjectPtr` 只写路径（弱指针置空，由引擎按名解析）。没有图片或任一步失败则保留模板封面。

## 开源依赖

| 库 | 用途 | 许可证 |
| --- | --- | --- |
| [miniaudio](https://github.com/mackron/miniaudio) 0.11.25 | 解码（FLAC/WAV/MP3）、WASAPI 独占/共享、DirectSound | MIT-0 / Public Domain |
| [RtAudio](https://github.com/thestk/rtaudio) 6.0.1 | ASIO 输出（只启用 ASIO API）；其 Steinberg ASIO 宿主代码也用于原生 DSD 输出 | MIT（内含 Steinberg ASIO 宿主代码） |
| [Monkey's Audio SDK](https://monkeysaudio.com/developers.html) 13.27 | APE 解码（作为 miniaudio 自定义解码后端） | BSD-3-Clause |
| [dsd2pcm](https://github.com/GiangLH/dsd2pcm)（Sebastian Gesemann） | DSD 无法直通时转 PCM（8:1 抽取，DSD64 → 352.8 kHz） | BSD-2-Clause |

DSD 输出方式（设置 `dsdMode`，界面「DSD 输出」）：`pcm` 仅 PCM / `native-dop-pcm` Native > DoP > PCM（默认）/
`dop-native-pcm` DoP > Native > PCM。Native 与 DoP 只在 `asio` 后端尝试，失败的原因会附在输出说明里；
驱动拒绝 ASIO DSD 模式后，在换后端/设备/模式前不再重复尝试。其余后端直接走 PCM。

DSF / DFF 为原生 DSD 直通，不转 PCM：插件自己解析文件（DSF 按声道块存放、LSB 在前；DFF 按字节交错、
MSB 在前；不支持 DST 压缩的 DFF），用 `ASIOFuture(kAsioSetIoFormat, kASIODSDFormat)` 把驱动切到 DSD 模式，
采样率设为 DSD 速率（DSD64 = 2822400 Hz），按驱动报告的 `ASIOSTDSDInt8LSB1/MSB1` 位序原样写入比特流。
只在 `asio` 后端可用。驱动拒绝 ASIO 原生 DSD 模式时（如 xDuoo 驱动，返回码会显示在状态里），回退到 DoP
（DSD over PCM v1.1）：同一个 ASIO 设备以 DSD 速率的 1/16 开 32 位整数流（DSD64 → 176400 Hz），每帧每声道
高 24 位放 `0x05/0xFA` 交替标记 + 两个 DSD 字节，DAC 识别标记后按 DSD 解码，比特流同样不经任何转换。
两种都不行时跳过这首并显示原因。DSD 流不能做数字音量，音量设置对其无效；
暂停和曲尾填 DSD 静音码 `0x69`。

WASAPI 独占模式按文件原生采样率打开设备，设备不支持时由 miniaudio 回退到设备原生格式并重采样。

## 逆向记录（HTGame 1.4a，UE 5.6.1）

| 函数 | 地址 | 签名 |
| --- | --- | --- |
| PostPlayerMusicSound(this, UAkAudioEvent*, FName* listId, float pos) | 0x148A45C40 | `48 8B C4 4C 89 40 18 55 53 56 57 41 54 41 56 41 57 48 8D 68 88 48 81 EC 40 01 00 00 0F 29 70 B8` |
| StopPlayerMusicSound(this, uint8 bNoTransition) | 0x148A4EB30 | `48 83 EC 28 0F B6 C2 48 89 5C 24 38` |
| Pause(this) | 0x148A442A0 | `40 53 48 83 EC 20 8B 91 20 04 00 00 48 8B D9 85 D2 7E ?? 33 C9 41 B1 04 44 8D 41 64 E8 ?? ?? ?? ?? 48 8D 8B 80 03 00 00 C6 83 24 04 00 00 01` |
| Resume(this) | 0x148A4B7B0 | 同 Pause，末尾为 `C6 83 24 04 00 00 00` |
| SetMusicPlayerType(this, type) | 0x148A4CFC0 | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D 99 90 04 00 00 4C 89 74 24 30 48 8D B1 28 04 00 00 8B EA 48 8B F9 39 91 28 04 00 00` |
| AHTPlayerCharacter::EndGetOffVehicle(this, bool)（vtable +0x15A0） | 0x147E01600 | `40 55 53 56 57 41 54 41 55 41 57 48 8D AC 24 10 FF FF FF 48 81 EC F0 01 00 00 44 0F B6 EA 48 8B F9 E8` |
| FText::FromString(FText* out, const TCHAR*) | 0x141554E60 | 序言不唯一，取 `UHTUI_MusicListItem::SetListItem`（`40 55 41 56 48 83 EC 58 4C 8B F2 48 8B E9 E8`）`+0xB8` 处 `E8 rel32` |

专辑相关函数（FindRow、两个 ForeachRow 取调用点 `E8 rel32`；GetGameInstance、GetAlbumData、
`TArray<FName>::AddUnique`、GetOwnedMusicListIDs、ReGenerateNewMusicListIDs、SetCurrentMusicListID）
的签名及行结构偏移见 `hifi_vehicle_music_profile.hpp`。GameInstance 音乐表 `+0x1840`、专辑表 `+0x1848`。

`UHTSoundSubsystem` 偏移：AkComponent `0x3D8`、ListID `0x400`、EventName `0x418`、
PlayingID `0x420`、Paused `0x424`、PendingSeek `0x425`、PlayerType `0x428`；
`UAkAudioEvent` 名称 FName 在 `0x18`。本地玩家判定：`APawn::Controller` `0x2F8`、
`APlayerController::Player` `0x368`、`UPlayer::PlayerController` `0x30`。已拥有歌单 ID
`TArray<FName>` 在 `0x3E0`。签名与偏移统一定义在 `hifi_vehicle_music_profile.hpp`。

## 设置

| 键 | 说明 |
| --- | --- |
| `enabled` | 是否接管载具音乐 |
| `backend` | `wasapi-exclusive` / `wasapi-shared` / `directsound` / `asio` |
| `device` | 设备名，空为默认设备 |
| `bufferMs` | 输出缓冲时长 |
| `volume` | 0–1 线性音量（独占/ASIO 下为数字域音量，1.0 为 bit-perfect） |
| `musicFolder` | 曲库绝对路径，通过窗口里的「浏览...」（Windows 选择文件夹对话框）选择；相对路径会被忽略，因为插件包每次加载都在不同的 `.cache/<pid>/` 目录 |

设置经 config 服务持久化：任何修改后由 scheduler 延迟 500 ms 合并写入，Stop 时再补写一次。

## 行为说明

- 未选文件夹、曲库为空或输出设备打开失败时不接管，游戏 Wwise 照常播放（窗口与日志会显示原因）。
- 插件选择的输出设备只承载替换后的音乐，不影响游戏自身音频设备；切换插件设备不会让游戏静音。
- 日志写入 `Anomaly/logs/anomaly-runtime.jsonl`：Hook 安装结果、每个新音乐事件的名称与是否替换。
- 界面文本走 localization 服务，`locales/zh-CN.json` 提供中文。

## 进度

- [x] IDA 定位函数、签名唯一性验证、偏移确认
- [x] SDK 接口核对（hook / signature / config / ui / ue5.names / core.plugin_directory）
- [x] Profile、manifest、音频引擎、插件主体、CMake
- [x] 构建通过（build.cmd，windows-vs2022 RelWithDebInfo）
- [x] 文件夹选择对话框、设置持久化（scheduler）、zh-CN 本地化、事件/Hook 日志
- [x] 歌单模式：直接使用曲库原文件名，按顺序播放并自动切歌
- [x] 歌单替代游戏电台：游戏切歌不打断；音量下方「上一首 / 暂停·播放 / 下一首」播放控制
- [x] 下车停止：Hook `EndGetOffVehicle`（本地玩家判定），待实测
- [x] ~~歌单改名 + 取模对齐~~（已废弃：曲库少时会被藏起来/重复）
- [x] 新增游戏内专辑「HiFi 曲库」：曲库几首就追加几首，游戏歌曲不动；实测专辑和歌曲出现，但全部锁定
- [x] 修锁定：专辑页列表构建（`RefreshPageList`）也直接读已拥有列表，Hook 后实测曲库歌曲已解锁
- [x] 解锁全部游戏歌曲：遍历音乐表时记下所有可列出（IsList 且非 IsHidden）的歌，和曲库歌曲一起
  临时加入已拥有列表，只加原本没有的、调用后原样移除，存档不变；构建通过，待实测
- [x] 切歌回到游戏歌曲：播放队列构建（ReGenerate / SetCurrent / Resolve / Sync）在当前歌是曲库歌时
  只看到曲库歌曲（快照已拥有列表、调用后原样写回），界面路径仍看到全部解锁；构建通过，待实测
- [x] 实测切歌只在曲库内
- [x] 专辑要上下车才出现：专辑列表可能先于任何音乐表遍历构建，此时还没有模板行；现在构建专辑时
  主动用原始 ForeachRow 遍历一次音乐表取模板；构建通过，待实测
- [x] 实测：专辑一上车就出现
- [x] 引擎自己接下一首后游戏仍认为是上一首（标题不变、游戏切歌从旧位置算）：每帧（AHUD 回调，Game 线程）
  把游戏的当前歌 ID 用 `SetCurrentPlayerMusicListID` 同步到引擎实际在放的曲库歌；只在当前是曲库歌时改。
  另吞掉当前为曲库歌时的 Wwise 结束回调；构建通过，待实测
- [x] 实测：歌名随引擎接歌同步；「放一半被切」是 30 秒的歌正常结束
- [x] 游戏播放器进度条：接管时把 PlayingID 设为固定的 `0x7FFFFF00`（Wwise 忽略），Hook 两个 Wwise 位置查询
  （比例 / 秒）对该 ID 返回引擎位置；Hook 时长回调，用扫描时测得的文件时长替换模板歌曲时长；
  拖动进度条（同一首歌带位置重新投递）在引擎内 seek；构建通过，待实测
- [x] 实测：进度条与真实进度同步、时长正确；拖动会被拉回
- [x] 拖动跳转：进度条松手走 `ChangePlayerMusicSound(position)`（重新异步加载模板事件再投递），
  当前是曲库歌时改为直接让引擎 seek；构建通过，待实测
- [x] 实测：拖动跳转正常
- [x] 专辑页点歌（曲库少于 7 首时无反应）：Hook `OnMusicDetailedViewEntryClicked`，原函数没切过去时，
  对曲库歌按歌曲结束的路径补切（入队、设为当前、投递，投递照常被接管）；构建通过，待实测。
  门槛本身（游戏只在面板自己的列表条目里找这首歌）没有改
- [x] 实测：补切有效，但要点两下。原因推测是每帧同步在新歌异步投递到引擎前把当前歌改回了旧歌；
  现在只在引擎自己换歌时同步；构建通过，待实测
- [x] 实测：封面正常显示目录里的图
- [ ] 7 首门槛本身：未治本。实测少于 7 首时必须双击，高亮和切歌同时发生；单击、双击都没触发
  `OnMusicDetailedViewEntryClicked` / `OnMusicItemSelected`，所以真正的点歌路径另有其处。
  诊断结果：双击走 `UHTUI_MusicPlayer::OnPauseResumeStateChanged`（`0x148FF3160`）→ `ChangePlayerMusicSound`；
  专辑详情页歌曲列表的选歌处理是 `0x148FF12B0`，挂在列表控件的多播委托（列表对象 `+0xC90`）上。
  这个委托在少于 7 首时只在双击时触发。列表外层是游戏自己的对齐滚动框 `SHTAlignScrollBox`
  （`HTAlignScrollBox.cpp`，构建在 `0x148B97750`），它在列表对象 `+0xC90` 上包了一层；
  ProcessEvent 追踪：单击只触发条目的 `BP_UpdateSelected`（选中视觉），双击才有
  `BP_OnItemSelectionChanged` + 播放。点击广播在 `0x1481F29B0`（列表对象 `+0xC90` / `+0xCF0`）。
  列表是 `UHTListView`，有逐条入场动画（OneByOne，状态在 `+0xD50`）；动画进行中会清掉内层控件
  `+0x958` 的标志。实测：动画状态 0（空闲），推测被否定（其余字段读数无效，偏移有误，已删）。
  已加诊断：Hook 行点击处理 `0x148219F80` 和列表点击广播 `0x1481F29B0`。实测单击、双击都没有触发
  这两处，专辑列表的点击不走通用列表的点击路径，仍未定位
- [x] 进度条总时长一直是 3:06：专辑页双击/继续播放走另一个时长回调 `0x148FD1EE0`（info 多一层指针），
  之前没 Hook，显示的是模板游戏歌的时长；已 Hook，构建通过，待实测
- [x] 歌名与实际播放不一致、拖动又被拉回：游戏有时不经投递就把当前歌改回存档里的游戏歌
  （Wandering In The Night），曲库歌照常在放。判断「插件是否在放」改为看 PlayingID 是否为接管值；
  不一致超过 2 秒就把当前歌改回引擎在放的曲库歌；拖动和时长也按这个判断；构建通过，待实测
- [x] 暂停偶尔没反应：专辑页「继续」走重新投递，接管时跳过了原 Post，子系统的暂停标志 `+0x424`
  留在 1，下一次点暂停被当成继续。接管时把它清 0；构建通过，待实测
- [x] 专辑封面：不再改专辑行；Hook 专辑详情页（`+0xF98`）和专辑列表条目（`+0x658`）的封面设置，
  原函数设完后，若是 HiFi 专辑就对 UImage 调 `SetBrushFromTexture`（vtable `+0x320`）换成导入的纹理
  （首次显示时导入并 RootSet）。没有图片或导入失败保留模板封面；构建通过，待实测
- [x] 实测：暂停、歌名与进度一致、拖动正常；7 首门槛按用户要求暂缓
- [x] 新格式：APE（Monkey's Audio SDK，走原有 PCM 路径，所有后端可用）；DSF/DFF 原生 DSD 直通
  （仅 ASIO，驱动切 DSD 模式，不转 PCM），时长、进度、拖动同样生效；构建通过，待实测
- [x] 实测：xDuoo ASIO 驱动拒绝 `kAsioSetIoFormat`。加 DoP 回退（同样是原生 DSD 比特流，DAC 显示 DSD），
  状态里显示驱动返回码；构建通过，待实测
- [x] 实测：xDuoo 只能 DoP。加 DSD 输出选项（仿 QQ 音乐）：仅 PCM / Native > DoP > PCM / DoP > Native > PCM，
  逐级回退，最后一级用 dsd2pcm 转 PCM；实测正常
- [x] 专辑名改为曲库文件夹名；窗口去掉设备说明和「最近事件」两行；构建通过，待实测
- [x] 修复「带插件开机必卡死、切图也挂起」：诊断用的 ProcessEvent 追踪在 Game 线程上调
  `anomaly.ue5.names` 的 `resolve_utf8`，而宿主在这条调用里会等待自己的语义状态发布完成；
  Game 线程一旦等在那里，整个游戏就不再推进（dump 显示 GameThread 停在
  `msvcp140!Cnd_wait → SleepConditionVariableSRW`，调用链正是本插件的追踪回调）。追踪已移除，
  它此前的结论保留在上面第 7 条；宿主不再阻塞 Game 线程调用者后可以再加回来。
- [x] 修复「开机启用后专辑和音乐都没了」：`EnsureRows` 只在游戏遍历专辑/音乐表（开音乐播放器、
  上下车建列表）时被驱动，开机直接开车时它没跑过，`AlbumActive` 为假，于是专辑不进列表、
  车载音乐全部 `skipped: game song`；另外首次扫描还没出结果时它会发布一张空专辑并记下当时的
  曲库代次，之后要等下一次扫描才会重建。现在空曲库视为「还没扫完」不发布、不记代次，
  且在投递事件判断前先建一次行（已建好时这里只比一次代次）。日志证据：正常那次是
  `library yes` + `... (playerType 1): replaced`，开机那次是 `library no` + `... skipped: game song`。
- [x] 开机启用时 `current song sync unavailable`：`ue5.ahud` 是可选服务，宿主只在游戏的反射门
  就绪后才发布它，而开机加载发生在门打开之前；插件当时拿到空指针就永久放弃，这一局的歌名/进度
  每帧同步一直是关的（游戏内启用或热载入时门已开，所以以前看不到这个问题）。
  现在 `EnsureSyncSubscription` 是幂等的：每次音乐事件都会用 `Context` 里保存的 host 指针重新
  查询该服务，拿到后订阅一次。查询本身就代表服务已发布，因此能避开「在 Game 线程上调用尚未就绪
  的宿主状态」那种卡死；日志改为成功时的 `current song sync subscribed`
- [x] 诊断探针全部停用：`LogCaller`（调用点 + 24 字节 code before）、专辑条目/行/列表点击、
  `SetCurrentPlayerMusicListID` 调用点、开机的 module base/trampoline，以及这次为定位专辑问题
  临时加的 `library album not built: <原因>`。代码按注释保留在原地，需要时整段解注释即可；
  保留的是状态日志（hooks installed、音乐事件 replaced/skipped、签名/hook 失败告警）
- [ ] 游戏播放器界面（歌名、进度条、拖动、上一首/下一首）接到插件引擎：函数已定位，未实现
- [ ] 专辑封面：两种写法（只写路径 / 弱指针 + 路径）实测都会让专辑从列表消失，已停用，
  专辑沿用模板封面；导入代码保留未调用
- [ ] 游戏内实测（需用户执行）：确认事件名、seek 语义、各后端独占/ASIO 打开情况
