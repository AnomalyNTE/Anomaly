#pragma once

// 副本插件（Clone Enter）的启用状态管理。
//
// 公开 SDK 没有「启用/停用别的插件」的接口，所以这里改宿主的
// <runtime root>/config/plugin-enablement.json：宿主对它有文件监视器
// （debounce 750ms），文件一变就重新加载启用表，等于在运行时启用/停用插件。
// 「关闭再打开」就是先写 false 等它卸载，再写 true 等它重新加载。

namespace daily {

// 在 Load（Lifecycle）里调用一次：解析宿主的 enablement 文件路径。
bool BindCloneEnterControl();

// 路径解析成功且写盘任务需要的服务都可用。
bool CloneEnterControlAvailable();

// 请求把副本插件设为启用/停用；写盘在 Worker 线程执行。
void CloneEnterRequestEnabled(bool enabled);

// 还有写盘任务在排队或在飞（含后续补写）。
bool CloneEnterWriteInFlight();

}  // namespace daily
