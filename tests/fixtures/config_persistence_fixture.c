#include "anomaly/sdk/anomaly_sdk.h"

#include <Windows.h>

#include <stddef.h>
#include <string.h>

static const char kSchema[] =
    "{\"type\":\"object\",\"required\":[\"phase\"],\"additionalProperties\":false,"
    "\"properties\":{\"phase\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3}}}";
static const char kPhaseOne[] = "{\"phase\":1}";
static const char kPhaseTwo[] = "{\"phase\":2}";
static const char kPhaseThree[] = "{\"phase\":3}";

struct FixtureContext {
    const AnomalyConfigServiceV1* config;
    unsigned phase;
};

static struct FixtureContext g_context;
static HANDLE g_cached_call_start;
static HANDLE g_cached_call_finished;
static HANDLE g_cached_call_thread;
static volatile LONG g_cached_call_status = ANOMALY_STATUS_V1_FAILED;
static volatile LONG g_game_config_status = ANOMALY_STATUS_V1_OK;
static volatile LONG g_render_config_status = ANOMALY_STATUS_V1_OK;
static volatile LONG g_game_config_called;
static volatile LONG g_render_config_called;

static AnomalyStatusV1 status(const uint32_t code) {
    AnomalyStatusV1 value = {code, 0, {NULL, 0}};
    return value;
}

static AnomalyStringViewV1 view(const char* text) {
    AnomalyStringViewV1 value = {text, strlen(text)};
    return value;
}

static AnomalyStatusV1 write_document(
    const AnomalyConfigServiceV1* config, const char* document) {
    const AnomalyByteSpanV1 bytes = {
        (const uint8_t*)document, strlen(document)};
    return config->write_atomic(config->user, view("settings"), 1u, bytes);
}

static DWORD WINAPI cached_config_call(void* unused) {
    (void)unused;
    if (WaitForSingleObject(g_cached_call_start, INFINITE) == WAIT_OBJECT_0) {
        const AnomalyStatusV1 result = write_document(g_context.config, kPhaseThree);
        InterlockedExchange(&g_cached_call_status, (LONG)result.code);
    }
    SetEvent(g_cached_call_finished);
    return 0;
}

static void stop_cached_config_call(void) {
    if (g_cached_call_thread != NULL) {
        SetEvent(g_cached_call_start);
        WaitForSingleObject(g_cached_call_thread, INFINITE);
        CloseHandle(g_cached_call_thread);
        g_cached_call_thread = NULL;
    }
    if (g_cached_call_finished != NULL) {
        CloseHandle(g_cached_call_finished);
        g_cached_call_finished = NULL;
    }
    if (g_cached_call_start != NULL) {
        CloseHandle(g_cached_call_start);
        g_cached_call_start = NULL;
    }
}

static AnomalyStatusV1 ANOMALY_CALL load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    const void* table = NULL;
    const AnomalyConfigServiceV1* config;
    AnomalyGenerationHandleV1 schema_handle = {0, 0};
    AnomalyStatusV1 result;
    uint32_t schema_version = 0;
    uint8_t document[64] = {0};
    size_t document_size = sizeof(document);

    if (host == NULL || plugin_context == NULL || host->query_service == NULL) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    result = host->query_service(
        host->host_context, view(ANOMALY_CONFIG_SERVICE_V1_ID),
        ANOMALY_CONFIG_SERVICE_V1_VERSION, &table);
    config = (const AnomalyConfigServiceV1*)table;
    if (result.code != ANOMALY_STATUS_V1_OK || config == NULL ||
        config->struct_size < offsetof(AnomalyConfigServiceV1, write_atomic) +
            sizeof(config->write_atomic) ||
        config->service_version < ANOMALY_CONFIG_SERVICE_V1_VERSION ||
        config->register_schema == NULL || config->read == NULL || config->write_atomic == NULL) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    result = config->register_schema(
        config->user, view("settings"), 1u,
        (AnomalyByteSpanV1){(const uint8_t*)kSchema, strlen(kSchema)}, &schema_handle);
    if (result.code != ANOMALY_STATUS_V1_OK || schema_handle.id == 0 ||
        schema_handle.generation == 0) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    result = config->read(
        config->user, view("settings"), &schema_version,
        (AnomalyMutableByteSpanV1){document, sizeof(document)}, &document_size);
    if (result.code == ANOMALY_STATUS_V1_NOT_FOUND) {
        g_context.phase = 1u;
    } else if (result.code == ANOMALY_STATUS_V1_OK && schema_version == 1u &&
               document_size == strlen(kPhaseOne) &&
               memcmp(document, kPhaseOne, document_size) == 0) {
        g_context.phase = 2u;
    } else if (result.code == ANOMALY_STATUS_V1_OK && schema_version == 1u &&
               document_size == strlen(kPhaseTwo) &&
               memcmp(document, kPhaseTwo, document_size) == 0) {
        g_context.phase = 3u;
    } else if (result.code == ANOMALY_STATUS_V1_OK && schema_version == 1u &&
               document_size == strlen(kPhaseThree) &&
               memcmp(document, kPhaseThree, document_size) == 0) {
        g_context.phase = 3u;
    } else {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    g_context.config = config;
    *plugin_context = &g_context;
    g_game_config_status = ANOMALY_STATUS_V1_OK;
    g_render_config_status = ANOMALY_STATUS_V1_OK;
    g_game_config_called = 0;
    g_render_config_called = 0;
    g_cached_call_start = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_cached_call_finished = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_cached_call_start == NULL || g_cached_call_finished == NULL) {
        stop_cached_config_call();
        g_context = (struct FixtureContext){NULL, 0};
        *plugin_context = NULL;
        return status(ANOMALY_STATUS_V1_FAILED);
    }
    g_cached_call_status = ANOMALY_STATUS_V1_FAILED;
    g_cached_call_thread = CreateThread(NULL, 0, cached_config_call, NULL, 0, NULL);
    if (g_cached_call_thread == NULL) {
        stop_cached_config_call();
        g_context = (struct FixtureContext){NULL, 0};
        *plugin_context = NULL;
        return status(ANOMALY_STATUS_V1_FAILED);
    }
    return status(ANOMALY_STATUS_V1_OK);
}

static AnomalyStatusV1 ANOMALY_CALL start(void* plugin_context) {
    return plugin_context == &g_context ? status(ANOMALY_STATUS_V1_OK)
                                        : status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
}

static void ANOMALY_CALL update(void* plugin_context, double delta_seconds) {
    (void)delta_seconds;
    if (plugin_context != &g_context || g_context.config == NULL) return;
    InterlockedExchange(&g_game_config_called, 1);
    InterlockedExchange(
        &g_game_config_status, (LONG)write_document(g_context.config, kPhaseThree).code);
}

static void ANOMALY_CALL draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    (void)ui;
    if (plugin_context != &g_context || g_context.config == NULL) return;
    InterlockedExchange(&g_render_config_called, 1);
    InterlockedExchange(
        &g_render_config_status, (LONG)write_document(g_context.config, kPhaseThree).code);
}

static AnomalyStatusV1 ANOMALY_CALL stop(void* plugin_context, uint32_t deadline_milliseconds) {
    const char* document;
    if (plugin_context != &g_context || g_context.config == NULL) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    SetEvent(g_cached_call_start);
    if (WaitForSingleObject(
            g_cached_call_finished, deadline_milliseconds == 0 ? 1U : deadline_milliseconds) !=
            WAIT_OBJECT_0 ||
        InterlockedCompareExchange(&g_cached_call_status, 0, 0) !=
            ANOMALY_STATUS_V1_UNAVAILABLE) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }
    if ((InterlockedCompareExchange(&g_game_config_called, 0, 0) != 0 &&
            InterlockedCompareExchange(&g_game_config_status, 0, 0) !=
                ANOMALY_STATUS_V1_UNAVAILABLE) ||
        (InterlockedCompareExchange(&g_render_config_called, 0, 0) != 0 &&
            InterlockedCompareExchange(&g_render_config_status, 0, 0) !=
                ANOMALY_STATUS_V1_UNAVAILABLE)) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }
    if (g_context.phase == 1u) {
        document = kPhaseOne;
    } else if (g_context.phase == 2u) {
        document = kPhaseTwo;
    } else if (g_context.phase == 3u) {
        document = kPhaseThree;
    } else {
        return status(ANOMALY_STATUS_V1_FAILED);
    }
    return write_document(g_context.config, document);
}

static void ANOMALY_CALL unload(void* plugin_context) {
    if (plugin_context == &g_context) {
        (void)write_document(g_context.config, kPhaseThree);
        stop_cached_config_call();
        g_context = (struct FixtureContext){NULL, 0};
    }
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = (AnomalyPluginDescriptorV1){
        sizeof(*descriptor),
        ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        view("anomaly.fixture.config-persistence"),
        view("Config Persistence Fixture"),
        view("Anomaly"),
        view("1.0.0"),
        load,
        start,
        stop,
        unload,
        update,
        draw};
    return status(ANOMALY_STATUS_V1_OK);
}
