#pragma once

// 一条龙状态机：等世界就绪 → 副本（IPC 调 CloneEnter）→ 一咖舍
// → 每日活跃奖励 → 环期任务奖励。两个领取阶段各有开关，也可以单独跑。
// 只在 Game 线程跑；停止请求由 plugin.cpp 置位后在这里消费。

#include "state.hpp"

namespace daily {

void ChainStart(bool automatic);
// 只跑某一个阶段（等世界就绪后直达该阶段，跑完就收尾）。
void ChainStartSingle(Stage target);
void ChainStop(StopCause cause, const char* note_key, const char* note_fallback);
void ChainTick(double delta_seconds);
bool ChainRunning();

const char* StageKey(Stage stage);
const char* StageFallback(Stage stage);

}  // namespace daily
