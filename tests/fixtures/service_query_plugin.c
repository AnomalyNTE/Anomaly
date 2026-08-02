#include "anomaly/sdk/anomaly_sdk.h"

#include <stddef.h>
#include <string.h>

static AnomalyStatusV1 status(uint32_t code) {
    AnomalyStatusV1 value = {code, 0, {NULL, 0}};
    return value;
}

static AnomalyStringViewV1 view(const char* text) {
    AnomalyStringViewV1 value = {text, strlen(text)};
    return value;
}

static AnomalyStatusV1 ANOMALY_CALL load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    const void* table = (const void*)(uintptr_t)1;
    AnomalyStatusV1 result;
    const AnomalyCoreServiceV1* core;
    const AnomalyPluginStateServiceV1* state;
    const AnomalyNteEscMenuButtonServiceV1* esc_menu_buttons;
    char state_directory[4096];
    size_t state_directory_size = 0;
    if (host == NULL || plugin_context == NULL || host->query_service == NULL) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }

    result = host->query_service(
        host->host_context, view(ANOMALY_CORE_SERVICE_V1_ID),
        ANOMALY_CORE_SERVICE_V1_VERSION, &table);
    core = (const AnomalyCoreServiceV1*)table;
    if (result.code != ANOMALY_STATUS_V1_OK || core == NULL ||
        core->struct_size < offsetof(AnomalyCoreServiceV1, user) + sizeof(core->user) ||
        core->service_version != ANOMALY_CORE_SERVICE_V1_VERSION) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    table = (const void*)(uintptr_t)1;
    result = host->query_service(
        host->host_context, view(ANOMALY_CORE_SERVICE_V1_ID),
        ANOMALY_CORE_SERVICE_V1_VERSION + 1u, &table);
    if (result.code != ANOMALY_STATUS_V1_UNAVAILABLE || table != NULL) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    table = NULL;
    result = host->query_service(
        host->host_context, view(ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_ID),
        ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_VERSION, &table);
    esc_menu_buttons = (const AnomalyNteEscMenuButtonServiceV1*)table;
    if (result.code != ANOMALY_STATUS_V1_OK || esc_menu_buttons == NULL ||
        esc_menu_buttons->struct_size <
            offsetof(AnomalyNteEscMenuButtonServiceV1, unregister_button) +
                sizeof(esc_menu_buttons->unregister_button) ||
        esc_menu_buttons->service_version !=
            ANOMALY_NTE_ESC_MENU_BUTTON_SERVICE_V1_VERSION ||
        esc_menu_buttons->register_button == NULL ||
        esc_menu_buttons->unregister_button == NULL) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    table = NULL;
    result = host->query_service(
        host->host_context, view(ANOMALY_PLUGIN_STATE_SERVICE_V1_ID),
        ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION, &table);
    state = (const AnomalyPluginStateServiceV1*)table;
    if (result.code != ANOMALY_STATUS_V1_OK || state == NULL ||
        state->struct_size < offsetof(AnomalyPluginStateServiceV1, directory) +
            sizeof(state->directory) ||
        state->directory == NULL ||
        state->directory(state->user, NULL, &state_directory_size).code !=
            ANOMALY_STATUS_V1_OK ||
        state_directory_size <= 1u || state_directory_size > sizeof(state_directory)) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }
    if (state->directory(
            state->user, state_directory, &state_directory_size).code !=
            ANOMALY_STATUS_V1_OK ||
        strstr(state_directory, "anomaly.fixture.service-query") == NULL) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    table = (const void*)(uintptr_t)1;
    result = host->query_service(
        host->host_context, view(ANOMALY_PLUGIN_STATE_SERVICE_V1_ID),
        ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION + 1u, &table);
    if (result.code != ANOMALY_STATUS_V1_UNAVAILABLE || table != NULL) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    table = (const void*)(uintptr_t)1;
    result = host->query_service(
        host->host_context, view(ANOMALY_UI_SERVICE_V1_ID),
        ANOMALY_UI_SERVICE_V1_VERSION, &table);
    if (result.code != ANOMALY_STATUS_V1_PERMISSION_DENIED || table != NULL) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    table = (const void*)(uintptr_t)1;
    result = host->query_service(
        host->host_context, view("anomaly.fixture.missing"), 1u, &table);
    if (result.code != ANOMALY_STATUS_V1_PERMISSION_DENIED || table != NULL) {
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
        view("anomaly.fixture.service-query"),
        view("Service Query Contract"),
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
