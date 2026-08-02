#include "anomaly/sdk/anomaly_sdk.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr char kPluginId[] = "anomaly.fixture.ui-frame-scope";
constexpr std::size_t kUiRequiredStructSize =
    offsetof(AnomalyUiServiceV1, developer_mode_enabled) +
    sizeof(decltype(AnomalyUiServiceV1::developer_mode_enabled));

const AnomalyUiServiceV1* g_ui{};
const AnomalyInputServiceV1* g_input{};
const AnomalyWindowServiceV1* g_window{};
AnomalyGenerationHandleV1 g_window_handle{};
bool g_update_capture_was_unavailable{};

constexpr AnomalyStringViewV1 View(const char* value, const std::size_t size) noexcept {
    return {value, size};
}

AnomalyStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {nullptr, 0}};
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (host == nullptr || context == nullptr || host->query_service == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    g_ui = nullptr;
    g_input = nullptr;
    g_window = nullptr;
    g_window_handle = {};
    g_update_capture_was_unavailable = false;
    const void* ui_table{};
    const void* input_table{};
    const void* window_table{};
    if (host->query_service(
            host->host_context, View(ANOMALY_UI_SERVICE_V1_ID,
                                     sizeof(ANOMALY_UI_SERVICE_V1_ID) - 1),
            ANOMALY_UI_SERVICE_V1_VERSION, &ui_table).code != ANOMALY_STATUS_V1_OK ||
        host->query_service(
            host->host_context, View(ANOMALY_INPUT_SERVICE_V1_ID,
                                     sizeof(ANOMALY_INPUT_SERVICE_V1_ID) - 1),
            ANOMALY_INPUT_SERVICE_V1_VERSION, &input_table).code != ANOMALY_STATUS_V1_OK ||
        host->query_service(
            host->host_context, View(ANOMALY_WINDOW_SERVICE_V1_ID,
                                     sizeof(ANOMALY_WINDOW_SERVICE_V1_ID) - 1),
            ANOMALY_WINDOW_SERVICE_V1_VERSION, &window_table).code != ANOMALY_STATUS_V1_OK) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    g_ui = static_cast<const AnomalyUiServiceV1*>(ui_table);
    g_input = static_cast<const AnomalyInputServiceV1*>(input_table);
    g_window = static_cast<const AnomalyWindowServiceV1*>(window_table);
    if (g_ui == nullptr || g_ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION ||
        g_ui->struct_size < kUiRequiredStructSize || g_ui->frame_state == nullptr ||
        g_ui->input_double == nullptr || g_ui->developer_mode_enabled == nullptr ||
        g_input == nullptr ||
        g_input->struct_size < offsetof(AnomalyInputServiceV1, capture_state) +
            sizeof(decltype(AnomalyInputServiceV1::capture_state)) ||
        g_input->capture_state == nullptr || g_window == nullptr ||
        g_window->struct_size < sizeof(AnomalyWindowServiceV1) ||
        g_window->register_window == nullptr || g_window->release_window == nullptr ||
        g_window->begin == nullptr || g_window->end == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    *context = reinterpret_cast<void*>(1);
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    AnomalyWindowSpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.id = View("frame-scope", sizeof("frame-scope") - 1);
    spec.title = View("Frame Scope", sizeof("Frame Scope") - 1);
    spec.initial_width = 320.0F;
    spec.initial_height = 240.0F;
    spec.default_open = 1;
    return g_window->register_window(g_window->user, &spec, &g_window_handle);
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    if (g_window != nullptr && g_window_handle.id != 0 && g_window->release_window != nullptr) {
        static_cast<void>(g_window->release_window(g_window->user, g_window_handle));
    }
    g_window_handle = {};
    return Status(ANOMALY_STATUS_V1_OK);
}

void ANOMALY_CALL Unload(void*) {
    g_ui = nullptr;
    g_input = nullptr;
    g_window = nullptr;
    g_window_handle = {};
    g_update_capture_was_unavailable = false;
}

void ANOMALY_CALL Update(void*, double) {
    static_cast<void>(g_ui->frame_state(g_ui->user));
    std::uint32_t capture{};
    g_update_capture_was_unavailable =
        g_input->capture_state(g_input->user, &capture).code == ANOMALY_STATUS_V1_UNAVAILABLE;
    static_cast<void>(g_ui->developer_mode_enabled(g_ui->user));
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr || ui != g_ui || ui->service_version != ANOMALY_UI_SERVICE_V1_VERSION ||
        ui->struct_size < kUiRequiredStructSize) {
        return;
    }
    double value = -10000000.125;
    static_cast<void>(ui->input_double(
        ui->user, View("coordinate", sizeof("coordinate") - 1), &value, 0.0, 0.0));
    static_cast<void>(ui->developer_mode_enabled(ui->user));
    static_cast<void>(ui->frame_state(ui->user));

    std::uint32_t capture{};
    const bool capture_is_current =
        g_input->capture_state(g_input->user, &capture).code == ANOMALY_STATUS_V1_OK;
    if (g_update_capture_was_unavailable && capture_is_current && ui->text != nullptr) {
        ui->text(ui->user, View("capture scoped", sizeof("capture scoped") - 1));
    }
    if (g_window_handle.id != 0) {
        int visible{};
        if (g_window->begin(g_window->user, g_window_handle, 0, &visible).code ==
            ANOMALY_STATUS_V1_OK) {
            static_cast<void>(g_window->end(g_window->user, g_window_handle));
        }
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        View(kPluginId, sizeof(kPluginId) - 1), View("UI Frame Scope Fixture", 22),
        View("Anomaly", 7), View("1.0.0", 5),
        Load, Start, Stop, Unload, Update, Draw};
    return Status(ANOMALY_STATUS_V1_OK);
}
