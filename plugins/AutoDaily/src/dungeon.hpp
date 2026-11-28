#pragma once

// 与副本插件（ccyellowstar.clone-enter）的 IPC 客户端。
// 打副本本身不在这里实现：本插件只发 start/stop/status，结果按数字码 + reason 判定。

#include "settings_model.hpp"
#include "state.hpp"

#include <cstdint>
#include <string>

namespace daily {

constexpr const char* kCloneEnterPluginId = "ccyellowstar.clone-enter";
constexpr const char* kDungeonEndpointId = "ccyellowstar.clone-enter.onekey";

// 一键副本的对外结果码（与 CloneEnter 的 OneKeyResult 对齐）。
enum class DungeonResult : int { Running = 0, Complete = 1, Aborted = 2 };

struct DungeonStatus {
    bool ok{};
    bool active{};
    int phase{};
    unsigned runs_done{};
    unsigned runs_total{};
    int result{};
    std::string reason{"none"};
};

bool DungeonIpcAvailable();

// Clone Enter 是否已经注册 IPC 端点：发起一次 status 探测。
// Missing = 对方没有端点（未启用/未安装），Pending = 等响应，Ready = 已经能正常调用。
enum class DungeonProbeResult {
    Missing,
    Pending,
    Ready,
};
DungeonProbeResult DungeonProbe();

// 三个请求都走 invoke_async：Game 线程禁止阻塞式 IPC。
bool DungeonStart(const Settings& settings);
bool DungeonStop();
bool DungeonStatusRequest();
void DungeonCancelPending();

// 取出并解析最近一次响应；没有新响应时返回 false。
bool DungeonTakeStatus(DungeonStatus& out);

}  // namespace daily
