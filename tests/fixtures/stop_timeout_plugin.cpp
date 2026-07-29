#include "anomaly/sdk/anomaly_sdk.h"

#include <cstddef>
#include <cstdint>

namespace {

AnomalyStatusV1 Status(std::uint32_t code) { return {code, 0, {nullptr, 0}}; }
constexpr AnomalyStringViewV1 View(const char* value, std::size_t size) {
    return {value, size};
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1*, void** context) {
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *context = reinterpret_cast<void*>(1);
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return Status(ANOMALY_STATUS_V1_OK); }
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    return Status(ANOMALY_STATUS_V1_TIMEOUT);
}
void ANOMALY_CALL Unload(void*) {}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        View("anomaly.fixture.stop-timeout", 28), View("Stop Timeout Fixture", 20),
        View("Anomaly", 7), View("1.0.0", 5),
        Load, Start, Stop, Unload, nullptr, nullptr};
    return Status(ANOMALY_STATUS_V1_OK);
}
