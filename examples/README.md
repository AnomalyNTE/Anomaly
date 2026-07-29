# Anomaly SDK 示例

| 示例 | 语言 | 演示内容 |
| --- | --- | --- |
| `hello_ui` | C11 | ABI v1 生命周期、作用域 Window/Font/Texture/Input 查询、generation handle，以及一个宿主自有窗口 |
| `tick_counter` | C++20 | 游戏更新回调与轻量 C++ SDK wrapper |
| `reliable_config` | C++20 | 通过 Config ABI 的宿主自有 JSON 设置；UI 改动把状态标记为 dirty，`on_stop` 时提交 |
| `nte_inspector` | C++20 | Session 生命周期事件、有界实体浏览、类名解析与 Host 快照 metrics |

本目录是一个独立的 CMake 工程，消费已安装的 `AnomalySDK` 包。配置时让 CMake 指向 SDK 包目录：

```powershell
cmake -S . -B build `
  -DAnomalySDK_DIR=C:/path/to/sdk/lib/cmake/AnomalySDK
cmake --build build --config RelWithDebInfo
```

四个可加载包写入 `build/packages` 下。每个包包含 `plugin.dll` 与对应的 `manifest.json`。

`nte_inspector` 通过唯一的 Entities V1 合同进行有界浏览，该服务缺失时显示 unavailable 状态。所有 NTE 服务都是可选的：exact Profile、feature、validator 或 capability 拒绝都可能使其保持 unavailable。产品插件位于仓库级 `plugins/` 目录，刻意不纳入这套独立的 SDK 教学集。

`hello_ui` 使用 Manifest schema v2，并为每项资源 capability 显式声明。它的 font 请求命名为 `assets/hello-ui.ttf`；派生包应在该包内相对路径放置一个带合适再分发许可的字体。缺少该文件时 font 进入 `FAILED`，示例只是跳过 `font.push`；它只会 push 状态带 `READY` 标志的字体。示例的 1x1 RGBA 纹理来自调用方所有的字节，因此不需要私有 renderer 或图像解码依赖。
