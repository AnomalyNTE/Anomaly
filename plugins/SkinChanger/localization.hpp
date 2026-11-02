#pragma once
// Relative include so fixtures that compile a plugin translation unit directly
// resolve the shared localizer without the repository root on the include path.
#include "anomaly/sdk/localization.hpp"

namespace skin_change {
// One instance per module: the DLL assigns it in Load, fixtures leave it empty
// and therefore observe the English fallbacks.
inline anomaly::sdk::Localizer& Localizer() {
    static anomaly::sdk::Localizer instance;
    return instance;
}
}
