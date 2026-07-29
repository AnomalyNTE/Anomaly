#include "anomaly/sdk/anomaly_sdk.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

constexpr char kPluginId[] = "anomaly.fixture.ui-draw-fault";

const AnomalyUiServiceV1* g_ui{};
const AnomalyWindowServiceV1* g_window{};
const AnomalyFontServiceV1* g_font{};
AnomalyGenerationHandleV1 g_window_handle{};
AnomalyGenerationHandleV1 g_font_handle{};
std::uint32_t g_draw_count{};
bool g_reopen_probe_pending{};

template <std::size_t N>
constexpr AnomalyStringViewV1 View(const char (&value)[N]) noexcept {
    return {value, N - 1U};
}

AnomalyStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {nullptr, 0}};
}

bool HasUiField(const AnomalyUiServiceV1* service, const std::size_t offset) noexcept {
    return service != nullptr && service->struct_size >= offset + sizeof(void*);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr || host->query_service == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    g_ui = nullptr;
    g_window = nullptr;
    g_font = nullptr;
    g_window_handle = {};
    g_font_handle = {};
    g_draw_count = 0;
    g_reopen_probe_pending = false;
    const void* ui{};
    const void* window{};
    const void* font{};
    if (host->query_service(
            host->host_context, View(ANOMALY_UI_SERVICE_V1_ID),
            ANOMALY_UI_SERVICE_V1_VERSION, &ui).code != ANOMALY_STATUS_V1_OK ||
        host->query_service(
            host->host_context, View(ANOMALY_WINDOW_SERVICE_V1_ID),
            ANOMALY_WINDOW_SERVICE_V1_VERSION, &window).code != ANOMALY_STATUS_V1_OK ||
        host->query_service(
            host->host_context, View(ANOMALY_FONT_SERVICE_V1_ID),
            ANOMALY_FONT_SERVICE_V1_VERSION, &font).code != ANOMALY_STATUS_V1_OK) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    g_ui = static_cast<const AnomalyUiServiceV1*>(ui);
    g_window = static_cast<const AnomalyWindowServiceV1*>(window);
    g_font = static_cast<const AnomalyFontServiceV1*>(font);
    if (!HasUiField(g_ui, offsetof(AnomalyUiServiceV1, end_popup)) ||
        g_ui->begin_window == nullptr || g_ui->end_window == nullptr ||
        g_ui->begin_child == nullptr || g_ui->end_child == nullptr ||
        g_ui->begin_table == nullptr ||
        g_ui->begin_menu == nullptr || g_ui->end_menu == nullptr ||
        g_ui->open_popup == nullptr || g_ui->begin_popup_modal == nullptr ||
        g_ui->end_popup == nullptr || g_window == nullptr ||
        g_window->struct_size < sizeof(*g_window) ||
        g_window->register_window == nullptr || g_window->release_window == nullptr ||
        g_window->state == nullptr || g_window->begin == nullptr ||
        g_window->end == nullptr || g_font == nullptr ||
        g_font->struct_size < sizeof(*g_font) || g_font->request == nullptr ||
        g_font->release == nullptr || g_font->push == nullptr || g_font->pop == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    *context = reinterpret_cast<void*>(1);
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    AnomalyWindowSpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.id = View("scoped-window");
    spec.title = View("Scoped Window");
    spec.initial_width = 320.0F;
    spec.initial_height = 240.0F;
    spec.default_open = 1;
    const AnomalyStatusV1 window_status =
        g_window->register_window(g_window->user, &spec, &g_window_handle);
    if (window_status.code != ANOMALY_STATUS_V1_OK) return window_status;

    AnomalyFontRequestV1 font{};
    font.struct_size = sizeof(font);
    font.relative_path = View("assets/test-font.bin");
    font.size_pixels = 16.0F;
    font.glyph_range = ANOMALY_GLYPH_RANGE_V1_LATIN;
    const AnomalyStatusV1 font_status = g_font->request(g_font->user, &font, &g_font_handle);
    if (font_status.code == ANOMALY_STATUS_V1_OK) return font_status;
    static_cast<void>(g_window->release_window(g_window->user, g_window_handle));
    g_window_handle = {};
    return font_status;
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    if (g_font != nullptr && g_font_handle.id != 0 && g_font->release != nullptr) {
        static_cast<void>(g_font->release(g_font->user, g_font_handle));
    }
    if (g_window != nullptr && g_window_handle.id != 0 && g_window->release_window != nullptr) {
        static_cast<void>(g_window->release_window(g_window->user, g_window_handle));
    }
    g_window_handle = {};
    g_font_handle = {};
    return Status(ANOMALY_STATUS_V1_OK);
}

void ANOMALY_CALL Unload(void*) {
    g_ui = nullptr;
    g_window = nullptr;
    g_font = nullptr;
    g_window_handle = {};
    g_font_handle = {};
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1*) {
    if (g_ui == nullptr || g_window == nullptr || g_font == nullptr) return;

    ++g_draw_count;
    if (g_reopen_probe_pending) {
        g_reopen_probe_pending = false;
        std::int32_t visible{};
        if (g_window->begin(g_window->user, g_window_handle, 0, &visible).code !=
            ANOMALY_STATUS_V1_OK) {
            return;
        }
        if (g_font->push(g_font->user, g_font_handle).code == ANOMALY_STATUS_V1_OK) {
            static_cast<void>(g_font->pop(g_font->user));
        }
        static_cast<void>(g_window->end(g_window->user, g_window_handle));
        return;
    }
    if (g_draw_count == 1U) {
        std::int32_t visible{};
        if (g_window->begin(g_window->user, g_window_handle, 0, &visible).code !=
            ANOMALY_STATUS_V1_OK) {
            return;
        }
        if (g_font->push(g_font->user, g_font_handle).code == ANOMALY_STATUS_V1_OK) {
            static_cast<void>(g_font->pop(g_font->user));
        }
        static_cast<void>(g_window->end(g_window->user, g_window_handle));
        AnomalyWindowStateV1 state{sizeof(state)};
        g_reopen_probe_pending =
            g_window->state(g_window->user, g_window_handle, &state).code ==
                ANOMALY_STATUS_V1_OK &&
            state.open == 0;
        return;
    }

    int open = 1;
    static_cast<void>(g_ui->begin_window(g_ui->user, View("Raw Window"), &open, 0));
    static_cast<void>(g_ui->begin_child(g_ui->user, View("Child"), 0.0F, 0.0F, 0));
    static_cast<void>(g_ui->begin_table(g_ui->user, View("Table"), 1, 0, 0.0F, 0.0F));
    static_cast<void>(g_ui->begin_menu(g_ui->user, View("Menu"), 1));
    g_ui->open_popup(g_ui->user, View("Popup"));
    static_cast<void>(g_ui->begin_popup_modal(g_ui->user, View("Popup"), &open, 0));

    std::int32_t visible{};
    if (g_window->begin(g_window->user, g_window_handle, 0, &visible).code !=
        ANOMALY_STATUS_V1_OK) {
        return;
    }
    if (g_font->push(g_font->user, g_font_handle).code != ANOMALY_STATUS_V1_OK) return;

    // The font and scoped window are on top. The proxy must reject this raw end rather
    // than pass an unsafe end_window call to the host.
    g_ui->end_window(g_ui->user);
    Sleep(5);
    throw std::runtime_error("fixture draw fault");
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        View(kPluginId), View("UI Draw Fault Fixture"), View("Anomaly"), View("1.0.0"),
        Load, Start, Stop, Unload, nullptr, Draw};
    return Status(ANOMALY_STATUS_V1_OK);
}
