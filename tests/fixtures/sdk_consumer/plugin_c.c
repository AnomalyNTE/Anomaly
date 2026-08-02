#include "anomaly/sdk/anomaly_sdk.h"

#include <Windows.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static AnomalyStringViewV1 view(const char* text) {
    AnomalyStringViewV1 result = {text, strlen(text)};
    return result;
}

static AnomalyStatusV1 status(uint32_t code) {
    AnomalyStatusV1 result = {code, 0, {0, 0}};
    return result;
}

static int verify_reload_sentinel(const char* directory) {
    static const wchar_t suffix[] = L"\\external-c-reload.sentinel";
    wchar_t environment_name[96];
    wchar_t environment_value[32];
    wchar_t next_value[32];
    wchar_t* path = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD expected = 0;
    DWORD transferred = 0;
    DWORD observed = 0;
    int passed = 0;

    const int directory_characters = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1, NULL, 0);
    if (directory_characters <= 1 || directory_characters > 32768) return 0;
    const size_t path_characters =
        (size_t)directory_characters + sizeof(suffix) / sizeof(suffix[0]);
    path = (wchar_t*)calloc(path_characters, sizeof(wchar_t));
    if (path == NULL || MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1,
            path, directory_characters) != directory_characters) {
        goto cleanup;
    }
    {
        const size_t length = wcslen(path);
        const wchar_t* append =
            length != 0 && (path[length - 1] == L'\\' || path[length - 1] == L'/')
            ? suffix + 1 : suffix;
        if (wcscat_s(path, path_characters, append) != 0) goto cleanup;
    }
    if (_snwprintf_s(
            environment_name, _countof(environment_name), _TRUNCATE,
            L"ANOMALY_EXTERNAL_C_STATE_%lu", (unsigned long)GetCurrentProcessId()) < 0) {
        goto cleanup;
    }
    SetLastError(ERROR_SUCCESS);
    {
        const DWORD length = GetEnvironmentVariableW(
            environment_name, environment_value, _countof(environment_value));
        if (length == 0) {
            if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) goto cleanup;
        } else {
            wchar_t* end = NULL;
            const unsigned long parsed = wcstoul(environment_value, &end, 10);
            if (length >= _countof(environment_value) || end == environment_value ||
                *end != L'\0' || parsed == 0 || parsed >= MAXDWORD) {
                goto cleanup;
            }
            expected = (DWORD)parsed;
        }
    }

    file = CreateFileW(
        path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
        expected == 0 ? CREATE_NEW : OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto cleanup;
    {
        LARGE_INTEGER size;
        LARGE_INTEGER start;
        size.QuadPart = 0;
        start.QuadPart = 0;
        if (!GetFileSizeEx(file, &size) ||
            size.QuadPart != (expected == 0 ? 0 : (LONGLONG)sizeof(observed))) {
            goto cleanup;
        }
        if (expected != 0 &&
            (!ReadFile(file, &observed, sizeof(observed), &transferred, NULL) ||
             transferred != sizeof(observed) || observed != expected)) {
            goto cleanup;
        }
        observed = expected + 1;
        if (!SetFilePointerEx(file, start, NULL, FILE_BEGIN) ||
            !WriteFile(file, &observed, sizeof(observed), &transferred, NULL) ||
            transferred != sizeof(observed) || !SetEndOfFile(file) ||
            !FlushFileBuffers(file)) {
            goto cleanup;
        }
    }
    if (_snwprintf_s(
            next_value, _countof(next_value), _TRUNCATE,
            L"%lu", (unsigned long)observed) < 0 ||
        !SetEnvironmentVariableW(environment_name, next_value)) {
        goto cleanup;
    }
    passed = 1;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    free(path);
    return passed;
}

static int exercise_plugin_state(const AnomalyHostApiV1* host) {
    const size_t query_size = offsetof(AnomalyHostApiV1, query_service) +
        sizeof(host->query_service);
    const void* table = NULL;
    const AnomalyPluginStateServiceV1* state;
    AnomalyStatusV1 result;
    size_t directory_size = 0;
    size_t capacity;
    char* directory;
    int passed;
    if (host->struct_size < query_size || host->query_service == NULL) return 1;

    result = host->query_service(
        host->host_context, view(ANOMALY_PLUGIN_STATE_SERVICE_V1_ID),
        ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION, &table);
    if (result.code == ANOMALY_STATUS_V1_UNAVAILABLE) return 1;
    state = (const AnomalyPluginStateServiceV1*)table;
    if (result.code != ANOMALY_STATUS_V1_OK || state == NULL ||
        state->service_version < ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION ||
        state->struct_size < offsetof(AnomalyPluginStateServiceV1, directory) +
            sizeof(state->directory) || state->directory == NULL ||
        state->directory(state->user, NULL, &directory_size).code !=
            ANOMALY_STATUS_V1_OK || directory_size <= 1 || directory_size > 32768) {
        return 0;
    }
    capacity = directory_size;
    directory = (char*)malloc(capacity);
    if (directory == NULL) return 0;
    result = state->directory(state->user, directory, &directory_size);
    passed = result.code == ANOMALY_STATUS_V1_OK && directory_size > 1 &&
        directory_size <= capacity && directory[directory_size - 1] == '\0' &&
        verify_reload_sentinel(directory);
    free(directory);
    return passed;
}

static AnomalyStatusV1 ANOMALY_CALL load(const AnomalyHostApiV1* host, void** context) {
    if (host == 0 || context == 0) return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (!exercise_plugin_state(host)) return status(ANOMALY_STATUS_V1_FAILED);
    *context = host->host_context;
    return status(ANOMALY_STATUS_V1_OK);
}

static void ANOMALY_CALL unload(void* context) { (void)context; }

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == 0 || descriptor->struct_size < sizeof(*descriptor)) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = (AnomalyPluginDescriptorV1){
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        view("example.external-c-sdk"),
        view("External C SDK"), view("Fixture"), view("1.0.0"),
        load, 0, 0, unload, 0, 0};
    return status(ANOMALY_STATUS_V1_OK);
}
