#include "i18n.hpp"

#include "anomaly/sdk/cpp.hpp"

#include <cstring>

namespace daily {
namespace {

const AnomalyLocalizationServiceV1* g_service{};
constexpr std::size_t kBufferBytes = 1024;

}  // namespace

void BindLocalization(const AnomalyLocalizationServiceV1* service) noexcept { g_service = service; }

std::string TextLocalized(const char* key, const char* fallback) {
    const std::string fallback_text = fallback != nullptr ? fallback : "";
    if (g_service == nullptr || g_service->translate == nullptr || key == nullptr) {
        return fallback_text;
    }
    char buffer[kBufferBytes]{};
    std::size_t size = sizeof(buffer);
    const AnomalyStatusV1 status = g_service->translate(
        g_service->user, AnomalyStringViewV1{key, std::strlen(key)},
        AnomalyStringViewV1{fallback_text.data(), fallback_text.size()}, nullptr, 0, buffer,
        &size);
    if (status.code != ANOMALY_STATUS_V1_OK || size == 0 || size > sizeof(buffer)) {
        return fallback_text;
    }
    return std::string(buffer, buffer + (size - 1));
}

std::string TextLocalized(const char* key, const char* fallback, const std::string* arguments,
               std::size_t count) {
    const std::string text = TextLocalized(key, fallback);
    std::string out;
    out.reserve(text.size() + 16);
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '{' && i + 2 < text.size() && text[i + 2] == '}' && text[i + 1] >= '0' &&
            text[i + 1] <= '7') {
            const std::size_t index = static_cast<std::size_t>(text[i + 1] - '0');
            if (index < count) out += arguments[index];
            i += 3;
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

}  // namespace daily
