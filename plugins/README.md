# 内建插件

本目录拥有随 `GameRuntime` 组件发布的产品插件。默认安装包含这些包目录：

- `NtePosition`
- `EntityESP`
- `PinkPawHeistESP`
- `FakeUID`
- `CameraTools`

SDK 教学插件属于 `examples/`，不安装进游戏运行时。诊断插件源码留在 `tools/`；其 manifest 使用 `"audience": "developer"`，因此管理界面仅在会话开发者模式启用时才把它们纳入已安装插件视图。
