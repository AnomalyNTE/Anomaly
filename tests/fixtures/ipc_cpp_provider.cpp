#include "anomaly/sdk/anomaly_sdk.h"

#include <cstdint>
#include <cstring>
#include <iterator>

namespace {

const AnomalyIpcServiceV1* g_ipc{};
AnomalyGenerationHandleV1 g_endpoint{};

AnomalyStringViewV1 View(const char* text) { return {text, std::strlen(text)}; }

AnomalyIpcSchemaHashV1 Hash(const std::uint8_t seed) {
    AnomalyIpcSchemaHashV1 result{};
    for (std::size_t index = 0; index < std::size(result.bytes); ++index) {
        result.bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    return result;
}

AnomalyStatusV1 Status(const std::uint32_t code) { return {code, 0, {}}; }

AnomalyStatusV1 ANOMALY_CALL Handle(
    void*, const AnomalyIpcRequestContextV1*, const AnomalyByteSpanV1 request,
    const AnomalyMutableByteSpanV1 response, std::size_t* response_size) {
    if (response_size == nullptr || request.size != 1) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    if (response.size < 1) {
        *response_size = 1;
        return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
    }
    response.data[0] = static_cast<std::uint8_t>(request.data[0] ^ 0xffU);
    *response_size = 1;
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr || host->query_service == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    const void* service{};
    AnomalyStatusV1 status = host->query_service(
        host->host_context, View(ANOMALY_IPC_SERVICE_V1_ID),
        ANOMALY_IPC_SERVICE_V1_VERSION, &service);
    if (status.code != ANOMALY_STATUS_V1_OK || service == nullptr) return status;
    g_ipc = static_cast<const AnomalyIpcServiceV1*>(service);
    AnomalyIpcEndpointDescriptorV1 descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.endpoint_id = View("dev.anomaly.ipc.cpp-provider");
    descriptor.major_version = 1;
    descriptor.request_schema = Hash(11);
    descriptor.response_schema = Hash(12);
    descriptor.modes = ANOMALY_IPC_MODE_V1_SYNC_REQUEST;
    descriptor.affinity = ANOMALY_IPC_AFFINITY_V1_CALLER;
    descriptor.timeout_milliseconds = 100;
    descriptor.reentrancy = ANOMALY_IPC_REENTRANCY_V1_REJECT;
    descriptor.maximum_request_bytes = 1;
    descriptor.maximum_response_bytes = 1;
    descriptor.maximum_queue_depth = 8;
    status = g_ipc->register_endpoint(g_ipc->user, &descriptor, Handle, nullptr, &g_endpoint);
    if (status.code == ANOMALY_STATUS_V1_OK) *context = &g_endpoint;
    return status;
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return Status(ANOMALY_STATUS_V1_OK); }

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    return Status(ANOMALY_STATUS_V1_OK);
}

void ANOMALY_CALL Unload(void*) {
    g_ipc = nullptr;
    g_endpoint = {};
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        View("dev.anomaly.ipc-cpp-provider"), View("IPC C++ Provider"), View("Anomaly"),
        View("1.0.0"), Load, Start, Stop, Unload, nullptr, nullptr};
    return Status(ANOMALY_STATUS_V1_OK);
}
