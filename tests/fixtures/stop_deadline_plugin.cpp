#include "anomaly/sdk/anomaly_sdk.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#ifndef ANOMALY_STOP_FIXTURE_ID
#define ANOMALY_STOP_FIXTURE_ID "anomaly.fixture.stop-deadline"
#endif

#ifndef ANOMALY_STOP_FIXTURE_DELAY_MS
#define ANOMALY_STOP_FIXTURE_DELAY_MS 0
#endif

namespace {

constexpr char kPluginId[] = ANOMALY_STOP_FIXTURE_ID;

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
    std::this_thread::sleep_for(
        std::chrono::milliseconds(ANOMALY_STOP_FIXTURE_DELAY_MS));
    return Status(ANOMALY_STATUS_V1_OK);
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
        View(kPluginId, sizeof(kPluginId) - 1), View("Stop Deadline Fixture", 21),
        View("Anomaly", 7), View("1.0.0", 5),
        Load, Start, Stop, Unload, nullptr, nullptr};
    return Status(ANOMALY_STATUS_V1_OK);
}
