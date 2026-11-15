#pragma once
#include <anomaly/sdk/localization.hpp>

namespace nte_vehicle {
    bool LoadBackend(const AnomalyHostApiV1* host, const anomaly::sdk::Localizer& localizer);
    struct Runtime;
    Runtime* Backend();
    void TickBackend();
    void StopBackend();
    void UnloadBackend();
    // 关闭插件时把摩擦开关字节写回原值（不需要 Game 线程）。
    void RestoreWheelFrictionBackend();
}
