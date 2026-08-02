#include "anomaly/sdk/anomaly_sdk.h"

#include <stddef.h>
#include <string.h>

enum RawMemoryCapabilityProfile {
    RAW_MEMORY_CAPABILITY_NONE,
    RAW_MEMORY_CAPABILITY_READ_ONLY,
    RAW_MEMORY_CAPABILITY_WRITE_ONLY,
};

static AnomalyStatusV1 status(uint32_t code) {
    AnomalyStatusV1 value = {code, 0, {NULL, 0}};
    return value;
}

static AnomalyStringViewV1 view(const char* text) {
    AnomalyStringViewV1 value = {text, strlen(text)};
    return value;
}

static int profile_from_package_directory(const AnomalyCoreServiceV1* core) {
    char directory[4096] = {0};
    size_t size = sizeof(directory);
    if (core->plugin_directory(core->user, directory, &size).code != ANOMALY_STATUS_V1_OK ||
        size == 0 || size > sizeof(directory)) {
        return -1;
    }
    directory[sizeof(directory) - 1] = '\0';
    if (strstr(directory, "no-memory") != NULL) {
        return RAW_MEMORY_CAPABILITY_NONE;
    }
    if (strstr(directory, "read-only") != NULL) {
        return RAW_MEMORY_CAPABILITY_READ_ONLY;
    }
    if (strstr(directory, "write-only") != NULL) {
        return RAW_MEMORY_CAPABILITY_WRITE_ONLY;
    }
    return -1;
}

static int status_matches_permission(AnomalyStatusV1 result, int permitted) {
    const uint32_t expected = permitted ? (uint32_t)ANOMALY_STATUS_V1_FAILED
                                        : (uint32_t)ANOMALY_STATUS_V1_PERMISSION_DENIED;
    return result.code == expected;
}

static AnomalyStatusV1 ANOMALY_CALL load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    const void* table = NULL;
    const AnomalyCoreServiceV1* core;
    AnomalyStatusV1 result;
    // Null payloads reach local argument validation only after capability
    // enforcement, so this fixture never performs a process-memory operation.
    AnomalyMutableByteSpanV1 null_destination = {NULL, 1u};
    AnomalyByteSpanV1 null_source = {NULL, 1u};
    int profile;
    int read_permitted;
    int write_permitted;

    if (host == NULL || plugin_context == NULL || host->query_service == NULL) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    result = host->query_service(
        host->host_context, view(ANOMALY_CORE_SERVICE_V1_ID),
        ANOMALY_CORE_SERVICE_V1_VERSION, &table);
    core = (const AnomalyCoreServiceV1*)table;
    if (result.code != ANOMALY_STATUS_V1_OK || core == NULL ||
        core->struct_size < offsetof(AnomalyCoreServiceV1, plugin_directory) +
                sizeof(core->plugin_directory) ||
        core->service_version != ANOMALY_CORE_SERVICE_V1_VERSION ||
        core->read_memory == NULL || core->write_memory == NULL ||
        core->plugin_directory == NULL) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    profile = profile_from_package_directory(core);
    read_permitted = profile == RAW_MEMORY_CAPABILITY_READ_ONLY;
    write_permitted = profile == RAW_MEMORY_CAPABILITY_WRITE_ONLY;
    if (profile < 0 ||
        core->read_memory(NULL, 0, null_destination).code != ANOMALY_STATUS_V1_INVALID_ARGUMENT ||
        core->write_memory(NULL, 0, null_source).code != ANOMALY_STATUS_V1_INVALID_ARGUMENT ||
        !status_matches_permission(
            core->read_memory(core->user, 0, null_destination), read_permitted) ||
        !status_matches_permission(
            core->write_memory(core->user, 0, null_source), write_permitted)) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    *plugin_context = (void*)host;
    return status(ANOMALY_STATUS_V1_OK);
}

static void ANOMALY_CALL unload(void* plugin_context) { (void)plugin_context; }

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = (AnomalyPluginDescriptorV1){
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        view("anomaly.fixture.raw-memory-capability"),
        view("Raw Memory Capability Fixture"),
        view("Anomaly"),
        view("1.0.0"),
        load,
        NULL,
        NULL,
        unload,
        NULL,
        NULL};
    return status(ANOMALY_STATUS_V1_OK);
}
