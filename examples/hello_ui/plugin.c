#include "anomaly/sdk/anomaly_sdk.h"

#include <stddef.h>
#include <string.h>

#define HAS_FIELD(table, type, field) \
    ((table) != NULL && (table)->struct_size >= \
        offsetof(type, field) + sizeof((table)->field))

typedef struct HelloUiContext {
    AnomalyGenerationHandleV1 window;
    AnomalyGenerationHandleV1 font;
    AnomalyGenerationHandleV1 texture;
    AnomalyGenerationHandleV1 hotkey;
} HelloUiContext;

static const AnomalyUiServiceV1* g_ui;
static const AnomalyWindowServiceV1* g_window;
static const AnomalyFontServiceV1* g_font;
static const AnomalyTextureServiceV1* g_texture;
static const AnomalyInputServiceV1* g_input;
static const AnomalyLocalizationServiceV1* g_localization;
static HelloUiContext g_context;

static const unsigned char kAccentPixel[] = {35u, 184u, 164u, 255u};

static AnomalyStringViewV1 view(const char* text) {
    AnomalyStringViewV1 result = {text, strlen(text)};
    return result;
}

static AnomalyStatusV1 status(unsigned int code) {
    AnomalyStatusV1 result = {code, 0, {0, 0}};
    return result;
}

static int succeeded(AnomalyStatusV1 result) {
    return result.code == ANOMALY_STATUS_V1_OK;
}

static AnomalyStatusV1 query(
    const AnomalyHostApiV1* host, const char* id, unsigned int version, const void** table) {
    if (table != NULL) *table = NULL;
    if (host == NULL || table == NULL || host->query_service == NULL) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    return host->query_service(host->host_context, view(id), version, table);
}

static int valid_ui(const AnomalyUiServiceV1* service) {
    return service != NULL && service->service_version >= ANOMALY_UI_SERVICE_V1_VERSION &&
        HAS_FIELD(service, AnomalyUiServiceV1, text) && service->text != NULL;
}

static int valid_window(const AnomalyWindowServiceV1* service) {
    return service != NULL && service->service_version >= ANOMALY_WINDOW_SERVICE_V1_VERSION &&
        HAS_FIELD(service, AnomalyWindowServiceV1, end) &&
        service->register_window != NULL && service->release_window != NULL &&
        service->state != NULL && service->begin != NULL && service->end != NULL &&
        service->toggle != NULL;
}

static int valid_font(const AnomalyFontServiceV1* service) {
    return service != NULL && service->service_version >= ANOMALY_FONT_SERVICE_V1_VERSION &&
        HAS_FIELD(service, AnomalyFontServiceV1, pop) && service->request != NULL &&
        service->release != NULL && service->state != NULL && service->push != NULL &&
        service->pop != NULL;
}

static int valid_texture(const AnomalyTextureServiceV1* service) {
    return service != NULL && service->service_version >= ANOMALY_TEXTURE_SERVICE_V1_VERSION &&
        HAS_FIELD(service, AnomalyTextureServiceV1, draw) && service->request != NULL &&
        service->release != NULL && service->state != NULL && service->draw != NULL;
}

static int valid_input(const AnomalyInputServiceV1* service) {
    return service != NULL && service->service_version >= ANOMALY_INPUT_SERVICE_V1_VERSION &&
        HAS_FIELD(service, AnomalyInputServiceV1, capture_state) &&
        service->snapshot != NULL && service->register_hotkey != NULL &&
        service->release_hotkey != NULL;
}

static int valid_localization(const AnomalyLocalizationServiceV1* service) {
    return service != NULL &&
        service->service_version >= ANOMALY_LOCALIZATION_SERVICE_V1_VERSION &&
        HAS_FIELD(service, AnomalyLocalizationServiceV1, translate) &&
        service->locale != NULL && service->translate != NULL;
}

static AnomalyStringViewV1 localized(
    const char* key, const char* english_fallback, char* buffer, size_t capacity) {
    size_t size = capacity;
    if (g_localization != NULL && buffer != NULL && capacity != 0 &&
        succeeded(g_localization->translate(
            g_localization->user, view(key), view(english_fallback), NULL, 0, buffer, &size))) {
        return (AnomalyStringViewV1){buffer, size - 1u};
    }
    return view(english_fallback);
}

static void ANOMALY_CALL toggle_window(
    void* user, AnomalyGenerationHandleV1 hotkey, const AnomalyInputSnapshotV1* snapshot) {
    HelloUiContext* context = (HelloUiContext*)user;
    (void)hotkey;
    (void)snapshot;
    if (context != NULL && context->window.id != 0 && g_window != NULL &&
        g_window->toggle != NULL) {
        (void)g_window->toggle(g_window->user, context->window);
    }
}

static void release_resources(HelloUiContext* context) {
    if (context == NULL) return;
    if (context->hotkey.id != 0 && g_input != NULL && g_input->release_hotkey != NULL) {
        (void)g_input->release_hotkey(g_input->user, context->hotkey);
    }
    if (context->texture.id != 0 && g_texture != NULL && g_texture->release != NULL) {
        (void)g_texture->release(g_texture->user, context->texture);
    }
    if (context->font.id != 0 && g_font != NULL && g_font->release != NULL) {
        (void)g_font->release(g_font->user, context->font);
    }
    if (context->window.id != 0 && g_window != NULL && g_window->release_window != NULL) {
        (void)g_window->release_window(g_window->user, context->window);
    }
    memset(context, 0, sizeof(*context));
}

static AnomalyStatusV1 ANOMALY_CALL load(const AnomalyHostApiV1* host, void** context) {
    const void* table = NULL;
    AnomalyStatusV1 result;

    if (context == NULL) return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *context = NULL;
    g_ui = NULL;
    g_window = NULL;
    g_font = NULL;
    g_texture = NULL;
    g_input = NULL;
    g_localization = NULL;
    memset(&g_context, 0, sizeof(g_context));

    result = query(host, ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_ui((const AnomalyUiServiceV1*)table)) {
        return status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    g_ui = (const AnomalyUiServiceV1*)table;

    result = query(host, ANOMALY_WINDOW_SERVICE_V1_ID, ANOMALY_WINDOW_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_window((const AnomalyWindowServiceV1*)table)) {
        return status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    g_window = (const AnomalyWindowServiceV1*)table;

    result = query(host, ANOMALY_FONT_SERVICE_V1_ID, ANOMALY_FONT_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_font((const AnomalyFontServiceV1*)table)) {
        return status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    g_font = (const AnomalyFontServiceV1*)table;

    result = query(host, ANOMALY_TEXTURE_SERVICE_V1_ID, ANOMALY_TEXTURE_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_texture((const AnomalyTextureServiceV1*)table)) {
        return status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    g_texture = (const AnomalyTextureServiceV1*)table;

    result = query(host, ANOMALY_INPUT_SERVICE_V1_ID, ANOMALY_INPUT_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_input((const AnomalyInputServiceV1*)table)) {
        return status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    g_input = (const AnomalyInputServiceV1*)table;

    result = query(host, ANOMALY_LOCALIZATION_SERVICE_V1_ID,
        ANOMALY_LOCALIZATION_SERVICE_V1_VERSION, &table);
    if (succeeded(result) && valid_localization((const AnomalyLocalizationServiceV1*)table)) {
        g_localization = (const AnomalyLocalizationServiceV1*)table;
    }

    *context = &g_context;
    return status(ANOMALY_STATUS_V1_OK);
}

static AnomalyStatusV1 ANOMALY_CALL start(void* plugin_context) {
    HelloUiContext* context = (HelloUiContext*)plugin_context;
    AnomalyWindowSpecV1 window = {0};
    AnomalyFontRequestV1 font = {0};
    AnomalyTextureRequestV1 texture = {0};
    AnomalyHotkeySpecV1 hotkey = {0};
    AnomalyStatusV1 result;
    char window_title[64];

    if (context == NULL || g_window == NULL || g_font == NULL || g_texture == NULL ||
        g_input == NULL) {
        return status(ANOMALY_STATUS_V1_FAILED);
    }

    window.struct_size = sizeof(window);
    window.id = view("main");
    window.title = localized("window.title", "Hello UI", window_title, sizeof(window_title));
    window.initial_width = 360.0F;
    window.initial_height = 180.0F;
    window.minimum_width = 260.0F;
    window.minimum_height = 120.0F;
    window.default_open = 1;
    result = g_window->register_window(g_window->user, &window, &context->window);
    if (!succeeded(result)) return result;

    font.struct_size = sizeof(font);
    font.relative_path = view("assets/hello-ui.ttf");
    font.size_pixels = 16.0F;
    font.glyph_range = ANOMALY_GLYPH_RANGE_V1_LATIN;
    result = g_font->request(g_font->user, &font, &context->font);
    if (!succeeded(result)) {
        release_resources(context);
        return result;
    }

    texture.struct_size = sizeof(texture);
    texture.encoded_bytes.data = kAccentPixel;
    texture.encoded_bytes.size = sizeof(kAccentPixel);
    texture.format = ANOMALY_TEXTURE_FORMAT_V1_RGBA8;
    texture.width = 1;
    texture.height = 1;
    result = g_texture->request(g_texture->user, &texture, &context->texture);
    if (!succeeded(result)) {
        release_resources(context);
        return result;
    }

    hotkey.struct_size = sizeof(hotkey);
    hotkey.id = view("toggle-main-window");
    hotkey.modifiers = ANOMALY_INPUT_MODIFIER_V1_CONTROL | ANOMALY_INPUT_MODIFIER_V1_SHIFT;
    hotkey.virtual_key = 'H';
    result = g_input->register_hotkey(
        g_input->user, &hotkey, toggle_window, context, &context->hotkey);
    if (!succeeded(result)) context->hotkey = (AnomalyGenerationHandleV1){0};

    return status(ANOMALY_STATUS_V1_OK);
}

static AnomalyStatusV1 ANOMALY_CALL stop(void* plugin_context, uint32_t deadline) {
    (void)deadline;
    release_resources((HelloUiContext*)plugin_context);
    return status(ANOMALY_STATUS_V1_OK);
}

static void ANOMALY_CALL unload(void* plugin_context) {
    (void)plugin_context;
    release_resources(&g_context);
    g_ui = NULL;
    g_window = NULL;
    g_font = NULL;
    g_texture = NULL;
    g_input = NULL;
    g_localization = NULL;
}

static void draw_contents(const HelloUiContext* context, const AnomalyUiServiceV1* ui) {
    AnomalyFontStateV1 font = {sizeof(font)};
    AnomalyTextureStateV1 texture = {sizeof(texture)};
    int font_pushed = 0;
    char greeting[192];

    if (context->font.id != 0 && succeeded(g_font->state(g_font->user, context->font, &font)) &&
        (font.flags & ANOMALY_FONT_STATE_V1_READY) != 0 &&
        succeeded(g_font->push(g_font->user, context->font))) {
        font_pushed = 1;
    }

    ui->text(ui->user, localized(
        "greeting", "Hello from the pure C Anomaly SDK sample.", greeting, sizeof(greeting)));
    if (font_pushed) {
        (void)g_font->pop(g_font->user);
    }

    if (context->texture.id != 0 &&
        succeeded(g_texture->state(g_texture->user, context->texture, &texture)) &&
        (texture.flags & ANOMALY_TEXTURE_STATE_V1_READY) != 0) {
        (void)g_texture->draw(
            g_texture->user, context->texture, 24.0F, 24.0F,
            ANOMALY_RGBA_V1(255u, 255u, 255u, 255u));
    }
}

static void ANOMALY_CALL draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    HelloUiContext* context = (HelloUiContext*)plugin_context;
    AnomalyWindowStateV1 state = {sizeof(state)};
    int visible = 0;

    if (context == NULL || context->window.id == 0) return;
    if (ui == NULL) ui = g_ui;
    if (!valid_ui(ui) || g_window == NULL || g_window->state == NULL ||
        g_window->begin == NULL || g_window->end == NULL) {
        return;
    }
    if (!succeeded(g_window->state(g_window->user, context->window, &state)) || state.open == 0) {
        return;
    }
    if (!succeeded(g_window->begin(g_window->user, context->window, 0, &visible))) return;
    if (visible != 0) draw_contents(context, ui);
    (void)g_window->end(g_window->user, context->window);
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = (AnomalyPluginDescriptorV1){
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        view("anomaly.example.hello-ui"), view("Hello UI"), view("Anomaly"), view("1.0.0"),
        load, start, stop, unload, NULL, draw};
    return status(ANOMALY_STATUS_V1_OK);
}
