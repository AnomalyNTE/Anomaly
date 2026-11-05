#include <anomaly/sdk/cpp.hpp>
#include "localization.hpp"
#include "cosmetics.hpp"
#include "awakening.hpp"
#include "plates.hpp"
#include "vehicle.hpp"
#include "awakening_profile.hpp"
#include <chrono>

namespace {
using namespace anomaly::sdk;
using Clock = std::chrono::steady_clock;

int category = 1;
Clock::time_point next_update{};
bool cosmetics_started{};
const AnomalyWindowServiceV1* window_service{};
AnomalyGenerationHandleV1 window_handle{};
int fallback_open{1};

bool EnsureWindow() noexcept {
    if (window_handle.id != 0) return true;
    if (!window_service || window_service->struct_size <
            offsetof(AnomalyWindowServiceV1, end) +
                sizeof(window_service->end) ||
        !window_service->register_window ||
        !window_service->release_window || !window_service->state ||
        !window_service->begin || !window_service->end) return false;
    AnomalyWindowSpecV1 spec{};
    spec.struct_size = sizeof(spec);
    spec.id = StringView("skin-changer-lite-main");
    const auto title=lite_i18n::localizer.Text("window.title", "SkinChanger Lite");
    spec.title = StringView(title);
    spec.initial_width = 430.0F;
    spec.initial_height = 540.0F;
    spec.minimum_width = 300.0F;
    spec.minimum_height = 300.0F;
    spec.maximum_width = 850.0F;
    spec.maximum_height = 900.0F;
    spec.default_open = 1;
    return window_service->register_window(window_service->user, &spec,
        &window_handle).code == ANOMALY_STATUS_V1_OK && window_handle.id != 0;
}

void ReleaseWindow() noexcept {
    if (window_handle.id == 0 || !window_service) return;
    if (window_service->release_window(window_service->user,
            window_handle).code == ANOMALY_STATUS_V1_OK)
        window_handle = {};
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) noexcept {
    if(!context)return {ANOMALY_STATUS_V1_INVALID_ARGUMENT,0,{}};
    *context=nullptr;
    lite_i18n::localizer = anomaly::sdk::Localizer(host);
    if(!accessory::profile::LoadCompatibility(host)||!skin_awakening_profile::LoadLayout()) {
        static thread_local std::string message;
        message=lite_i18n::localizer.Text("error.profile", "The compatibility profile is unavailable or incompatible. No game features were started.");
        return {ANOMALY_STATUS_V1_UNAVAILABLE,0,StringView(message)};
    }
    window_service = Host(host).Query<AnomalyWindowServiceV1>(
        ANOMALY_WINDOW_SERVICE_V1_ID, ANOMALY_WINDOW_SERVICE_V1_VERSION).get();
    window_handle = {};
    fallback_open = 1;
    lite_awakening::Load(host);
    lite_plates::Load(host);
    lite_vehicle::Load(host);
    return CosmeticsLoad(host, context);
}

AnomalyStatusV1 ANOMALY_CALL Start(void* context) noexcept {
    category = 1;
    next_update = {};
    lite_awakening::Start();
    lite_plates::Start();
    lite_vehicle::Start();
    cosmetics_started = CosmeticsStart(context).code == ANOMALY_STATUS_V1_OK;
    static_cast<void>(EnsureWindow());
    return Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* context, uint32_t reason) noexcept {
    lite_awakening::SetVisible(false);
    lite_vehicle::SetVisible(false);
    ReleaseWindow();
    lite_plates::Stop();
    const auto vehicle_status=lite_vehicle::Stop();
    if(vehicle_status.code!=ANOMALY_STATUS_V1_OK) return vehicle_status;
    const auto awakening_status = lite_awakening::Stop();
    if (awakening_status.code != ANOMALY_STATUS_V1_OK) return awakening_status;
    if (!cosmetics_started) return Ok();
    const auto result = CosmeticsStop(context, reason);
    if (result.code == ANOMALY_STATUS_V1_OK) cosmetics_started = false;
    return result;
}

void ANOMALY_CALL Unload(void* context) noexcept {
    ReleaseWindow();
    CosmeticsUnload(context);
    window_service = nullptr;
}

void ANOMALY_CALL Update(void* context, double delta) noexcept {
    lite_awakening::Update();
    lite_plates::Update();
    lite_vehicle::Update();
    const auto now = Clock::now();
    if (cosmetics_started && now >= next_update) {
        next_update = now + std::chrono::milliseconds(100);
        CosmeticsUpdate(context, delta);
    }
}

bool DrawContent(void* context, const AnomalyUiServiceV1* ui) {
    const bool has_tabs = ui->struct_size >= offsetof(AnomalyUiServiceV1, end_tab_bar) +
        sizeof(ui->end_tab_bar) && ui->begin_tab_bar && ui->begin_tab_item &&
        ui->end_tab_item && ui->end_tab_bar;
    if (!has_tabs) {
        ui->text(ui->user, StringView(lite_i18n::localizer.Text("error.tabs", "This host does not provide tab controls. Please update Anomaly.")));
        return false;
    }
    if (!ui->begin_tab_bar(ui->user, StringView("##skin-changer-lite-tabs"),
            ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN)) return false;
    struct EndTabBar {
        const AnomalyUiServiceV1* ui;
        ~EndTabBar() { ui->end_tab_bar(ui->user); }
    } end_bar{ui};
    struct Page { int category; const char* key; const char* fallback; const char* id; };
    constexpr Page pages[]{
        {1, "tab.accessories", "Accessories", "category-accessories"},
        {2, "tab.glider", "Glider", "category-glider"},
        {3, "tab.awakening", "Awakening", "category-awakening"},
        {4, "tab.plates", "Plates", "category-plates"},
        {5, "tab.vehicle", "Vehicle", "category-vehicle"}
    };
    bool visible{};
    for (const auto& page : pages) {
        const auto label = lite_i18n::localizer.Label(page.key, page.fallback, page.id);
        if (!ui->begin_tab_item(ui->user, StringView(label), nullptr, 0, 1)) continue;
        struct EndTabItem {
            const AnomalyUiServiceV1* ui;
            ~EndTabItem() { ui->end_tab_item(ui->user); }
        } end_item{ui};
        category = page.category;
        visible = true;
        if (category == 5) lite_vehicle::Draw(ui);
        else if (category == 4) lite_plates::Draw(ui);
        else if (category == 3) lite_awakening::Draw(ui);
        else if (cosmetics_started) CosmeticsDraw(context, ui, category);
        else ui->text(ui->user, StringView(lite_i18n::localizer.Text("cosmetics.unavailable", "The accessory/glider interface is unavailable for this game build.")));
    }
    return visible;
}

void ANOMALY_CALL Draw(void* context, const AnomalyUiServiceV1* ui) noexcept {
    struct Visibility {
        bool content{};
        ~Visibility() {
            lite_awakening::SetVisible(content && category == 3);
            lite_vehicle::SetVisible(content && category == 5);
        }
    } visibility;
    try {
        if (!ui || !ui->begin_window || !ui->end_window || !ui->text) return;
        if (EnsureWindow()) {
            AnomalyWindowStateV1 state{sizeof(state)};
            if (window_service->state(window_service->user, window_handle,
                    &state).code != ANOMALY_STATUS_V1_OK || state.open == 0) return;
            std::int32_t visible{};
            if (window_service->begin(window_service->user, window_handle,
                    0, &visible).code != ANOMALY_STATUS_V1_OK) return;
            struct EndWindow {
                ~EndWindow() { static_cast<void>(window_service->end(window_service->user, window_handle)); }
            } end_window;
            if (visible != 0) visibility.content = DrawContent(context, ui);
            return;
        }
        if (fallback_open == 0) return;
        UiWindow window(ui, lite_i18n::localizer.Text("window.title", "SkinChanger Lite"), &fallback_open);
        if (window) visibility.content = DrawContent(context, ui);
    } catch (...) {}
}
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("b1ank.skin-changer"), anomaly::sdk::StringView("SkinChanger Lite"),
        anomaly::sdk::StringView("b1ank"), anomaly::sdk::StringView("0.1.45"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
