# NTE TV Video

把《异环》住宅电视播放的视频替换为本地视频。插件只改写住宅电视
（`BP_FurnitureActorBase_TV_C`）的媒体来源，影院、剧情、技能、房间邀请等其它
Mana 视频保持游戏原样。

## 当前状态

- 插件可在运行中的游戏里加载、注册配置、选择本地视频、安装 Hook，并把电视资产
  `TVFileManaMovie` 的路径改写为 `./Movies/FFMpeg_Mana/TV/ntetv.mp4`（本地视频会先复制到
  `HT/Content/Movies/FFMpeg_Mana/TV/ntetv.mp4`）。路径改写已在 1.5.4 国服实测生效。
- **未通过的门槛**：电视随后黑屏，CRI Mana 没有播放本地 H.264 MP4。原因待查：Mana
  `FileManaMovie` 可能只接受 Sofdec Prime（USM），或需要额外的格式开关。插件默认
  `enabled=false`，关闭时电视保持游戏原视频。

## 已验证的运行时链路（1.5.4 国服）

- 电视 actor：`BP_FurnitureActorBase_TV_C`，组件 `MovieAudioPlayerComponent`。
- 换节目：`MovieAudioPlayerComponent::PlayByConfig(FName)` → `FileManaMovie.SetFilePath(FString)`
  → `TVFileManaMovie.FilePath` 变成 `./Movies/FFMpeg_Mana/EP/<节目>.usm`。
- 视频在 pak 内，不在磁盘；本地替换必须经过 Mana 的解码器。

## 配置

- `enabled`：是否接管电视。
- `videoPath`：本地视频绝对路径（复制到游戏目录后再交给 Mana）。
- `muteGameAudio`：跳过电视原本的 Wwise 事件，避免与本地视频音轨不同步。

## 文件

- `plugin.cpp`：Hook、配置、复制、UI。
- `tv_video_profile.hpp` / `profile/compatibility.profile.json`：1.5.4 的函数签名与布局。
- `locales/zh-CN.json`：中文界面文案。

## 已知限制

- 只识别电视节目目录 `Movies/FFMpeg_Mana/EP/`，其它 Mana 视频不动。
- 复制目标固定为 `HT/Content/Movies/FFMpeg_Mana/TV/ntetv.mp4`；游戏更新不会删除它。
- 热重载/停止会释放全部 Hook；复制与配置写入在 Worker 上执行。
