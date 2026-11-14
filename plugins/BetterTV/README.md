# Better TV

把《异环》住宅电视播放的视频替换为本地视频。插件只接管住宅电视
（`BP_FurnitureActorBase_TV_C`），影院、剧情、技能、房间邀请等其它视频保持游戏原样。

## 硬性约束（不可违反）

- **不写游戏安装目录**：不复制、不替换、不新增 `HT/Content`、`*.pak`、`*.ucas` 或任何游戏文件。
  启动器和内容校验会检测内容目录变化，落盘替换会破坏游戏完整性。
- 本地视频文件保留在用户自己的路径，插件只读取它并在**进程内存**里替换数据。
- 所有替换点都必须是内存操作：FString 参数改写、Hook 返回值改写、后续的虚拟文件数据。

## 选择住宅电视

开启替换后，在电视列表中手动选择当前世界已加载的住宅电视；默认不自动选中第一台。
列表每秒刷新，也可点击「刷新电视列表」。实例编号用于区分同名电视，选择不会写入配置。
关闭中的电视可以选中，开启后再接管；切换目标时恢复旧电视的材质和原声，并保留本地片源进度。
离开世界或选中对象卸载后清除选择，需要重新选择；其他电视继续播放原节目。
仅支持已知住宅电视基类及其派生类，以及通过校验的组件和屏幕材质。未知屏幕不会强行替换。
其他住宅的实际蓝图和材质兼容性仍需实机验收。
当前公开 SDK 没有 Game 域停止回调，因此本版本不能保证宿主禁用或热重载时自动恢复电视。面板关闭替换时的 Game 域恢复仍有效；宿主停止路径不在本次插件范围内修复，也不作为离线测试已覆盖的能力。

## 当前状态

- 本地 MP4 画面已通过真实电视人工验证。插件独立驱动电视的 MediaPlayer/MediaTexture，
  仅替换屏幕材质槽位；电视外壳和其它视频场景不变。
- 本地音轨由插件线程输出。电视接管后优先使用 XAudio2，按电视屏幕和游戏相机的位置渲染声像。
  默认 4 米内保持原音量，4 至 16 米线性衰减，16 米及以外静音。两段距离可在面板即时调整。
  空间输出不可用时使用 MFPlay。Game 更新提供视频时钟和相对位置，音频线程不访问 UE 对象。
- 已接入哔哩哔哩链接播放：面板新增「哔哩哔哩链接」输入框与「解析」按钮，插件用原生 B 站接口
  解析并下载单文件 720p 内 MP4，再用 Media Foundation 校验，成功后复用同一套本地播放链路。
  不随包分发 yt-dlp/FFmpeg/ffprobe，也不依赖系统 `PATH` 中的同名工具；真实游戏内从粘贴链接到
  电视出画仍待原版 Anomaly 2.7.0 实测。
- 用户已确认本地画面、声音与同步正常，原电视节目声音可静音。当前游戏后端是 ElectraPlayer，
  它不支持原生音量输出；游戏内 MediaSoundComponent 测试也未产生声音。
- 取消替换时在 Game tick 关闭媒体、停止音轨并恢复电视材质。音轨线程在插件停止时关闭并回收。
  配置变更继续在 Worker 保存，已修复首次保存后任务句柄不清空的问题。
- **尚未完成最终验收**：电视开关、取消替换的完整恢复、切换场景以及宿主禁用/热重载时的
  Game 域清理仍需验证和收尾；不能仅凭播放成功认定插件已完成。

## 哔哩哔哩链接播放

面板只新增一个「哔哩哔哩链接」输入框和一个「解析」按钮。点击后插件在后台线程完成解析、
下载和校验，状态行显示阶段和下载百分比，进度条显示下载进度；进行中同一个按钮变为「取消」。
输入新链接后再次点击可直接提交新任务替换旧任务，选择本地文件也会取消旧任务。准备期间当前
片源继续播放，失败时保留当前片源并显示原因。

- 支持普通免费投稿的 BV、av 和 `b23.tv` 分享短链（也接受包含链接的分享文本），每次只处理
  一 P。有效的 `?p=N` 会保留，省略或非法时使用 P1。短链在后台逐跳校验 HTTPS 和允许的主机，
  最多跟随五次，最终必须落到普通投稿页。
- 插件直接调用 B 站 `view` 与 WBI 签名的 `playurl(fnval=1)`，只使用平台返回的**单段
  muxed MP4**（H.264 + AAC，最高 720p）。下载由 WinHTTP 完成，校验由 Windows Media Foundation
  完成（容器、编码、声道、分辨率、时长），不需要 yt-dlp、FFmpeg 或 ffprobe。
- 文件就绪后，如果替换已开启且住宅电视正在播放，直接切换片源；替换关闭时显示
  「已就绪，开启替换后播放」。每次启动仍需手动开启替换的规则不变，原「选择视频」入口继续可用。
- 同一时间只运行一个准备任务，准备线程只属于插件。通过请求编号丢弃迟到结果；取消下载会立即
  停止并清理临时文件。
- 缓存位于 `%LOCALAPPDATA%\Anomaly\BetterTV\media-cache`，按规范化链接、分 P 和格式策略在
  本次游戏进程内复用。单个成品上限 **512 MiB**，每个进程的缓存和临时文件预算 **2 GiB**。
  关闭替换、换片源或禁用插件时会删除本次下载的成品缓存；电视关闭/离开场景保留缓存以续播。
  取消或失败只清理临时文件。配置保留上次链接和本地路径，启动和热重载时只恢复输入内容，不自动
  联网或打开媒体。
- 只支持无需登录、单段 muxed MP4 的普通投稿。DASH-only、分段视频、付费/充电/加密内容、直播、
  番剧、互动视频和需要额外权限的投稿会明确失败，不会把不完整的文件交给电视。
- 请只处理自己有权播放的投稿；插件不绕过登录、权限或付费限制。自动提取和缓存以平台许可为前提。

## 黑屏诊断（1.5.4 国服）

- 原 Profile 把 `FFrame::Node` 偏移写成了 `0`，现场脚本帧确认它位于 `0x10`，
  `0` 处是虚表。修正后，通过 SDK 调用 `IsUseManaPlayer`，强制选项开启时返回值已从 `1` 变为 `0`。
- 这项修正不足以恢复播放：实时电视组件和组件模板仍引用 `TVManaPlayer`、
  `TVFileManaMovie` 和 `TVManaPlayer_Texture`。独立打开 `TVMediaPlayer` 不会改变这些绑定。
- `MovieAudioPlayerComponent::Play` 原生实现（本次诊断地址 `0x149498BA0`）检查的类型是
  `ManaPlayer` 和 `FileManaMovie`，随后调用 Mana 打开函数；该实现没有 MediaPlayer 播放分支。
  因而不能只改管线开关或把组件字段替换为 Media 资产来完成 MP4 支持。
- `OpenFile=true` 和 `Play=true` 不能作为电视播放成功的判据。必须在后续 Game tick 确认媒体状态，
  并分别验证 MediaTexture 到电视材质的连接和本地音轨的输出；当前实验尚未完成这些步骤。
- FString 的非空 `Num` 包含结尾 NUL；参数构造与原位改写已统一计入该字符。

以上地址只记录诊断位置，不用于运行时寻址。生产兼容数据仍由 Profile 管理。

## 已验证的运行时链路（1.5.4 国服）

- 电视 actor：`BP_FurnitureActorBase_TV_C`，组件 `MovieAudioPlayerComponent`。
- 换节目：`MovieAudioPlayerComponent::PlayByConfig(FName)` → `FileManaMovie.SetFilePath(FString)`
  → `TVFileManaMovie.FilePath` 变成 `./Movies/FFMpeg_Mana/EP/<节目>.usm`。
- 片源在 pak 内，不在磁盘；替换来源只能走解码器，不能靠改磁盘文件。

## 配置

- `enabled`：仅控制本次插件会话。每次启动或热重载均关闭，旧配置中的 `true` 不会自动恢复。
- `videoPath`：本地视频绝对路径（只在内存中改写路径使用，不会复制到游戏目录）。解析成功后就绪的
  成品路径也会写入这里。
- `bilibiliUrl`：上次提交的链接。启动和热重载时只恢复到输入框，不会自动解析。
- `muteGameAudio`：旧配置兼容字段。接管电视时强制静音原节目，面板不显示单独的静音开关。
- `forceMediaPlayer`：旧配置兼容字段，不再作为开关使用，也不再显示实验选项。
- `audioFullVolumeMeters` / `audioMaxDistanceMeters`：3D 音轨的满音量距离和静音距离，单位为米。

## 文件

- `plugin.cpp`：Hook、配置、内存改写、UI；不包含任何游戏目录写入代码。
- `bilibili_source.hpp` / `bilibili_source.cpp`：BV/av/短链解析、HTTPS 与主机白名单逐跳校验。
- `bilibili_api.hpp` / `bilibili_api.cpp`：WBI 签名、`view`/`playurl` 调用与单段 muxed MP4 解析。
- `http_client.hpp` / `http_client.cpp`：WinHTTP 文本 GET、媒体流、HTTPS/目标地址校验。
- `media_download.hpp` / `media_download.cpp`：预检大小/预算、流式下载、进度与取消清理。
- `media_validate.hpp` / `media_validate.cpp`：Media Foundation 容器/编码/声道/时长校验。
- `media_prepare.hpp` / `media_prepare.cpp`：准备任务、进程内缓存、预算与缓存回收。
- `tv_video_profile.hpp` / `profile/compatibility.profile.json`：1.5.4 的函数签名与布局
  （含 `IsUseManaPlayer` thunk）。
- `locales/zh-CN.json`：中文界面文案。

插件包不包含任何第三方可执行文件或随包许可证；所有解析、下载与校验都在 `plugin.dll` 内完成。

## 已知限制

- 只识别电视节目目录 `Movies/FFMpeg_Mana/EP/`，其它视频不动。
- 链接只支持无需登录的普通免费投稿，每次一 P；不支持直播、番剧、互动视频、付费/充电/加密内容。
- 只支持平台返回的单段 muxed MP4（H.264 + AAC，最高 720p）；DASH-only、分段视频或不兼容编码会在
  解析/校验阶段失败并保留当前片源。部分投稿只能取得 360p。
- 解析、下载和校验需要网络；失败时当前片源继续播放，原因显示在播放状态行。
- 旧实验通过游戏管线开关切换电视；当前实现独立接管屏幕，不再依赖该开关改变原生播放实现。
- 热重载/停止会释放全部 Hook；配置写入在 Worker 上执行。

## 测试源码归属

住宅电视专用 fixture 位于 `plugins/BetterTV/tests/`，专用视频素材位于其 `data/` 子目录。
构建注册仍集中在根 CMakeLists.txt，原 target 和 CTest 名称保持不变，测试不进入发布包。
恢复 fixture 同时验证手动选择、候选重排、过期身份、跨世界清理及多电视音频事件隔离。
`better_tv_picker_lifecycle` 验证真实文件对话框的排队取消、打开后取消和线程回收；运行时会短暂显示对话框。
启动 fixture 还校验当前 SDK 描述符容量与尾部哨兵，防止写入未提供的扩展字段。

## 音轨验证夹具

在标准构建树开启 `ANOMALY_BUILD_TEST_FIXTURES` 后，构建 `anomaly_better_tv_audio_fixture`，
运行 `.build/windows-vs2022/bin/RelWithDebInfo/anomaly_better_tv_audio_fixture.exe <本地含音轨视频>`。
带单声道或双声道音轨的短片可加 `--spatial` 验证 XAudio2，带 5.1 音轨的短片可加
`--spatial-fallback` 验证回退。夹具会短暂出声并检查时钟前进、回跳、暂停、时钟过期和关闭。
空间方位、距离衰减和音画同步仍以 2.7.0 实机结果为准。

## 链接与准备管线夹具

开启 `ANOMALY_BUILD_TEST_FIXTURES` 后构建并运行：

```powershell
cmake --build --preset windows-relwithdebinfo --target `
    anomaly_better_tv_link_fixture anomaly_better_tv_bilibili_api_fixture `
    anomaly_better_tv_media_fixture anomaly_better_tv_prepare_fixture `
    anomaly_better_tv_startup_fixture anomaly_better_tv_restore_fixture `
    anomaly_better_tv_audio_fixture anomaly_better_tv_picker_fixture
ctest --test-dir .build\windows-vs2022 -C RelWithDebInfo -R better_tv
```

- `anomaly_better_tv_link_fixture`：不联网，用注入的重定向链覆盖 BV/av/分享文本、`?p=`、
  跳过 1–5 次重定向、超限、HTTPS 降级、非白名单主机、非视频页、循环、网络失败、非法输入。
- `anomaly_better_tv_bilibili_api_fixture`：用固定 WBI 向量和注入的 JSON 文档覆盖 `view`/`playurl`、
  P 选择、`-404`、无 durl、多段 durl、低画质回退与 WBI 失败回退。
- `anomaly_better_tv_media_fixture`：用固定小 MP4 覆盖 Media Foundation 校验正反例，并用假媒体流
  覆盖下载长度、超限、取消与进度。
- `anomaly_better_tv_prepare_fixture`：端到端组合以上模块（注入链接解析、API 文档、媒体流），
  覆盖正常准备、缓存复用、超额/预算、损坏文件、取消、旧任务被取代、缓存回收。

真实网络下载仍建议在游戏环境或本地 harness 中做一次 smoke；夹具本身完全离线。

## 登录隔离

插件启动保留片源，需要手动开启替换。未开启时不查找电视媒体资产、不打开视频、
不启动音频线程；登录阶段手动开启但没有当前世界中正在播放的真实电视时，面板显示等待。
关电视、离开住宅或普通加载时停止本地音画，保留开关和本次片源的播放位置。再次打开电视后续播。
退回登录会关闭替换、清除位置和已下载的成品缓存。手动关闭替换或换片源也会清除位置并回收缓存。
旧的 Mana 路径改写和全局媒体 Hook 已移除，当前仅保留电视节目和电视音频 Hook。
资源下载弹窗是否彻底消失仍须真实登录复测；插件不会绕过游戏资源检查。

## 关闭替换与原节目恢复

关闭替换时先解除原声拦截并通知外部音轨停止，恢复接管时保存的原屏幕材质，再关闭本地媒体、
单独调用原电视的 `PlayAudio`。不再通过 `PlayByConfig` 重新加载整个原节目；先前的卡死日志
停在这次重播调用内，其内部等待机制尚未取得线程栈证据。

原声音频事件来自游戏真实的 `PlayAudio` 参数，恢复前校验事件、电视组件、屏幕与原材质的对象身份。
没有可恢复事件时等待接管，游戏原节目照常播放。关电视时仅恢复有效的屏幕材质，原声由游戏开关管理。
场景已切换或对象失效时仅停止本地播放，不调用旧电视恢复。
当前事件恢复是否与原节目时钟保持一致，仍需真实游戏听觉验证。

`anomaly_better_tv_restore_fixture` 验证恢复顺序、原材质和原音频事件、重复关闭，以及失效对象隔离。
它能捕获关闭时重播整个原节目的回归，不能替代游戏内卡死与音画恢复的实测。

## 播放进度

面板显示当前时间和总时长。点击或拖动进度条后，插件合并短时间内的调整，在游戏更新中重开同一片源并
重建电视的媒体纹理连接。定位期间暂停外部音轨，待视频时钟连续推进后从实际位置同步。定位失败时，
插件尝试恢复跳转前的位置，恢复失败则停止本地替换。等待电视、首次打开片源及不可跳转时，进度条不可操作。

链接准备期间，状态行显示「正在下载视频 xx%（a MiB / b MiB）」，下方进度条为只读下载进度；
宿主的 UI 服务缺少只读滑块扩展时回退为 ASCII 文本条。解析和校验阶段显示对应阶段文本。
