#include "anomaly/sdk/cpp.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

static_assert(ANOMALY_PLUGIN_API_V1_MAJOR == 1u);
static_assert(ANOMALY_PLUGIN_API_V1_MINOR == 0u);
static_assert(std::is_standard_layout_v<AnomalyStringViewV1>);
static_assert(std::is_standard_layout_v<AnomalyStatusV1>);
static_assert(std::is_standard_layout_v<AnomalyHostApiV1>);
static_assert(std::is_standard_layout_v<AnomalyPluginDescriptorV1>);
static_assert(sizeof(AnomalyStringViewV1) == 16u);
static_assert(sizeof(AnomalyStatusV1) == 24u);
static_assert(sizeof(AnomalyHostApiV1) == 64u);
static_assert(sizeof(AnomalyPluginDescriptorV1) == 120u);
static_assert(sizeof(AnomalyUiServiceV1) == 280u);
static_assert(offsetof(AnomalyUiServiceV1, separator) == 104u);
static_assert(offsetof(AnomalyUiServiceV1, input_uint32) == 240u);
static_assert(offsetof(AnomalyUiServiceV1, input_double) == 248u);
static_assert(offsetof(AnomalyUiServiceV1, developer_mode_enabled) == 256u);
static_assert(offsetof(AnomalyUiServiceV1, input_text) == 264u);
static_assert(offsetof(AnomalyUiServiceV1, button_enabled) == 272u);
static_assert(sizeof(AnomalyNteSessionServiceV1) == 40u);
static_assert(offsetof(AnomalyNteSessionServiceV1, next_event) == 24u);
static_assert(sizeof(AnomalyNtePlayerServiceV1) == 40u);
static_assert(offsetof(AnomalyNtePlayerServiceV1, camera_snapshot) == 32u);
static_assert(sizeof(AnomalyNteEntitiesServiceV1) == 80u);
static_assert(offsetof(AnomalyNteEntitiesServiceV1, page) == 48u);
static_assert(offsetof(AnomalyNteEntitiesServiceV1, component_bounds) == 56u);
static_assert(offsetof(AnomalyNteEntitiesServiceV1, fname_property_utf8) == 72u);
static_assert(std::is_standard_layout_v<AnomalyNteEscMenuButtonSpecV1>);
static_assert(std::is_standard_layout_v<AnomalyNteEscMenuButtonServiceV1>);
static_assert(sizeof(AnomalyNteEscMenuButtonSpecV1) == 64u);
static_assert(offsetof(AnomalyNteEscMenuButtonSpecV1, id) == 8u);
static_assert(offsetof(AnomalyNteEscMenuButtonSpecV1, label) == 24u);
static_assert(offsetof(AnomalyNteEscMenuButtonSpecV1, icon_format) == 40u);
static_assert(offsetof(AnomalyNteEscMenuButtonSpecV1, icon_bytes) == 48u);
static_assert(sizeof(AnomalyNteEscMenuButtonServiceV1) == 32u);
static_assert(offsetof(AnomalyNteEscMenuButtonServiceV1, register_button) == 16u);
static_assert(offsetof(AnomalyNteEscMenuButtonServiceV1, unregister_button) == 24u);

namespace {

struct Fixture final {
    AnomalyCoreServiceV1 core{};
    std::uint32_t calls{};
    std::uint32_t observed_minimum{};
    bool fail_query{};
    int begin_calls{};
    int end_calls{};
};

AnomalyStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {nullptr, 0}};
}

AnomalyStatusV1 ANOMALY_CALL QueryService(
    void* context, const AnomalyStringViewV1 id, const std::uint32_t minimum,
    const void** output) {
    auto& fixture = *static_cast<Fixture*>(context);
    ++fixture.calls;
    fixture.observed_minimum = minimum;
    if (output == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *output = nullptr;
    constexpr std::string_view expected{ANOMALY_CORE_SERVICE_V1_ID};
    if (fixture.fail_query || id.data == nullptr ||
        std::string_view{id.data, id.size} != expected ||
        minimum > ANOMALY_CORE_SERVICE_V1_VERSION) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    }
    *output = &fixture.core;
    return Status(ANOMALY_STATUS_V1_OK);
}

int ANOMALY_CALL BeginWindow(void* context, AnomalyStringViewV1, int*, std::uint32_t) {
    ++static_cast<Fixture*>(context)->begin_calls;
    return 1;
}

void ANOMALY_CALL EndWindow(void* context) {
    ++static_cast<Fixture*>(context)->end_calls;
}

AnomalyHostApiV1 MakeHost(Fixture& fixture) noexcept {
    AnomalyHostApiV1 host{};
    host.struct_size = sizeof(host);
    host.api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    host.api_minor = ANOMALY_PLUGIN_API_V1_MINOR;
    host.host_context = &fixture;
    host.query_service = QueryService;
    return host;
}

}  // namespace

int main() {
    static_assert(anomaly::sdk::StringView("v1").size == 2u);
    static_assert(anomaly::sdk::Succeeded(anomaly::sdk::Ok()));

    Fixture fixture{};
    fixture.core.struct_size = sizeof(fixture.core);
    fixture.core.service_version = ANOMALY_CORE_SERVICE_V1_VERSION;
    auto host = MakeHost(fixture);

    const auto core = anomaly::sdk::Host(&host).Query<AnomalyCoreServiceV1>(
        ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION);
    if (!core || core.get() != &fixture.core || fixture.calls != 1u ||
        fixture.observed_minimum != ANOMALY_CORE_SERVICE_V1_VERSION) {
        return 1;
    }

    fixture.fail_query = true;
    if (anomaly::sdk::Host(&host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID)) {
        return 2;
    }
    fixture.fail_query = false;

    const auto calls = fixture.calls;
    host.api_major = ANOMALY_PLUGIN_API_V1_MAJOR + 1u;
    if (anomaly::sdk::Host(&host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID) ||
        fixture.calls != calls) {
        return 3;
    }
    host = MakeHost(fixture);
    host.struct_size = offsetof(AnomalyHostApiV1, query_service);
    if (anomaly::sdk::Host(&host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID) ||
        fixture.calls != calls) {
        return 4;
    }

    host = MakeHost(fixture);
    fixture.core.struct_size = offsetof(AnomalyCoreServiceV1, user);
    if (anomaly::sdk::Host(&host).Query<AnomalyCoreServiceV1>(ANOMALY_CORE_SERVICE_V1_ID)) {
        return 5;
    }
    fixture.core.struct_size = sizeof(fixture.core);

    AnomalyUiServiceV1 ui{};
    ui.struct_size = sizeof(ui);
    ui.service_version = ANOMALY_UI_SERVICE_V1_VERSION;
    ui.user = &fixture;
    ui.begin_window = BeginWindow;
    ui.end_window = EndWindow;
    {
        anomaly::sdk::UiWindow window(&ui, "contract");
        if (!window || fixture.begin_calls != 1 || fixture.end_calls != 0) return 6;
    }
    return fixture.end_calls == 1 ? 0 : 7;
}
