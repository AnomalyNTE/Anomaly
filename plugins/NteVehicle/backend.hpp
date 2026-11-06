#pragma once
#include <anomaly/sdk/localization.hpp>

namespace nte_vehicle {
    bool LoadBackend(const AnomalyHostApiV1* host, const anomaly::sdk::Localizer& localizer);
    struct Runtime;
    Runtime* Backend();
    void TickBackend();
    void StopBackend();
    void UnloadBackend();
}
