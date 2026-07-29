#include "anomaly/sdk/anomaly_sdk.h"

#include <cstdint>
#include <cstring>
#include <iterator>

namespace {

const AnomalyIpcServiceV1* g_ipc{};

AnomalyStringViewV1 View(const char* text) { return {text, std::strlen(text)}; }

AnomalyIpcSchemaHashV1 Hash(const std::uint8_t seed) {
    AnomalyIpcSchemaHashV1 result{};
    for (std::size_t index = 0; index < std::size(result.bytes); ++index) {
        result.bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    return result;
}

AnomalyStatusV1 Status(const std::uint32_t code) { return {code, 0, {}}; }

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr || host->query_service == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const void* service{};
    const AnomalyStatusV1 status = host->query_service(
        host->host_context, View(ANOMALY_IPC_SERVICE_V1_ID),
        ANOMALY_IPC_SERVICE_V1_VERSION, &service);
    if (status.code != ANOMALY_STATUS_V1_OK || service == nullptr) return status;
    g_ipc = static_cast<const AnomalyIpcServiceV1*>(service);
    *context = const_cast<AnomalyIpcServiceV1*>(g_ipc);
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    AnomalyIpcEndpointSelectorV1 selector{};
    selector.struct_size = sizeof(selector);
    selector.endpoint_id = View("dev.anomaly.ipc.c-provider");
    selector.major_version = 1;
    selector.request_schema = Hash(1);
    selector.response_schema = Hash(2);
    const std::uint8_t request = 41;
    std::uint8_t response{};
    std::size_t response_size = sizeof(response);
    const AnomalyStatusV1 status = g_ipc->invoke(
        g_ipc->user, &selector, {&request, 1}, {&response, 1}, &response_size);
    return status.code == ANOMALY_STATUS_V1_OK && response_size == 1 && response == 42
        ? status : Status(ANOMALY_STATUS_V1_FAILED);
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    return Status(ANOMALY_STATUS_V1_OK);
}

void ANOMALY_CALL Unload(void*) { g_ipc = nullptr; }

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        View("dev.anomaly.ipc-cpp-consumer"), View("IPC C++ Consumer"), View("Anomaly"),
        View("1.0.0"), Load, Start, Stop, Unload, nullptr, nullptr};
    return Status(ANOMALY_STATUS_V1_OK);
}
