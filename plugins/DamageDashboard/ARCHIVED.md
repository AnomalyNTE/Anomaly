# ARCHIVED

NTE 伤害仪表盘（`haaland-a.damage-dashboard`）已归档，不再随运行包构建、打包或安装。

## 当前状态

- 根 `CMakeLists.txt` 已移除 `anomaly_add_plugin(anomaly_builtin_damage_dashboard …)`、
  `ANOMALY_BUILTIN_PLUGIN_TARGETS` 中的条目，以及它的 `/utf-8` 编译选项。
- 因此构建树与发布包里都不会出现 `DamageDashboard` 包，`anomaly_test_host` 也不再依赖它。
- 本目录只保留 `plugin.cpp`、`manifest.json` 与 `locales/`，作为实现参考；不要在其它插件或宿主代码里依赖它。

## 需要恢复时

1. 在根 `CMakeLists.txt` 的 `if(ANOMALY_BUILD_BUILTIN_PLUGINS)` 块里恢复
   `anomaly_add_plugin(anomaly_builtin_damage_dashboard …)`，参数与其它内建插件一致：
   `SOURCES plugins/DamageDashboard/plugin.cpp`、`MANIFEST plugins/DamageDashboard/manifest.json`、
   `PACKAGE_NAME DamageDashboard`、`OUTPUT_DIRECTORY …/builtin-plugins/DamageDashboard`。
2. 把 `anomaly_builtin_damage_dashboard` 加回 `set(ANOMALY_BUILTIN_PLUGIN_TARGETS …)`。
3. 在 `if(MSVC)` 的 `/utf-8` 选项列表里加回 `target_compile_options(anomaly_builtin_damage_dashboard PRIVATE /utf-8)`，
   源码含中文字符串，缺了这项会按本地代码页编码窄字符串。
4. 重新 configure 后构建，并按 `AGENTS.md` 跑 `tools/check_plugin_i18n.ps1 -Plugin DamageDashboard`。

不要在本目录放 `CMakeLists.txt`：`plugins/<名字>/` 会被 SDK 消费者当成独立工程入口。
