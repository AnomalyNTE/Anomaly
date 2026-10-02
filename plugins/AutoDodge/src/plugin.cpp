// 自动闪避（音频触发版）—— ABI v1 入口
//
// 只做三件事：把宿主回调转发给 Session，保证异常不越过 ABI 边界，
// 以及填写插件描述符。描述符里的 id/name/version 必须与 manifest.json 一致。

#include "audio_dodge.hpp"

#include <cstddef>
#include <cstdint>

using anomaly::plugins::auto_dodge::Session;
using anomaly::sdk::StringView;

namespace {

Session g_session;

AnomalyStatusV1 Status(const std::uint32_t code) noexcept { return {code, 0, {}}; }

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *context = nullptr;
    try {
        const AnomalyStatusV1 status = g_session.Load(host);
        if (status.code != ANOMALY_STATUS_V1_OK) return status;
        *context = &g_session;
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED);
    }
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    try {
        g_session.Save();
    } catch (...) {
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    try {
        g_session.Unload();
    } catch (...) {
    }
}

void ANOMALY_CALL Update(void*, double delta_seconds) {
    try {
        g_session.Update(delta_seconds);
    } catch (...) {
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    try {
        g_session.Draw(ui);
    } catch (...) {
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
        StringView("anomaly.local.auto-dodge"), StringView("自动闪避"),
        StringView("YU-1021"), StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
