#pragma once

#include "anomaly/sdk/cpp.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace anomaly::plugins {

class Localizer final {
public:
    Localizer() noexcept = default;

    explicit Localizer(const AnomalyHostApiV1* host) noexcept {
        const auto queried = sdk::Host(host).Query<AnomalyLocalizationServiceV1>(
            ANOMALY_LOCALIZATION_SERVICE_V1_ID,
            ANOMALY_LOCALIZATION_SERVICE_V1_VERSION);
        const auto* service = queried.get();
        constexpr std::size_t kTranslateSize =
            offsetof(AnomalyLocalizationServiceV1, translate) +
            sizeof(decltype(AnomalyLocalizationServiceV1::translate));
        if (service != nullptr && service->struct_size >= kTranslateSize &&
            service->translate != nullptr) {
            service_ = service;
        }
    }

    [[nodiscard]] std::string Text(
        const std::string_view key, const std::string_view english_fallback) const {
        return Format(key, english_fallback, {});
    }

    [[nodiscard]] std::string Format(
        const std::string_view key, const std::string_view english_fallback,
        const std::span<const std::string_view> arguments) const {
        if (service_ == nullptr || arguments.size() > kMaximumArguments) {
            return FormatFallback(english_fallback, arguments);
        }

        std::array<AnomalyStringViewV1, kMaximumArguments> views{};
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            views[index] = sdk::StringView(arguments[index]);
        }

        std::array<char, 512> buffer{};
        std::size_t size = buffer.size();
        AnomalyStatusV1 status = service_->translate(
            service_->user, sdk::StringView(key), sdk::StringView(english_fallback),
            views.data(), arguments.size(), buffer.data(), &size);
        if (status.code == ANOMALY_STATUS_V1_OK && size != 0 && size <= buffer.size()) {
            return std::string(buffer.data(), size - 1U);
        }
        return FormatFallback(english_fallback, arguments);
    }

    [[nodiscard]] std::string Label(
        const std::string_view key, const std::string_view english_fallback,
        const std::string_view stable_id) const {
        std::string label = Text(key, english_fallback);
        label.append("###").append(stable_id);
        return label;
    }

private:
    static constexpr std::size_t kMaximumArguments = 8;

    [[nodiscard]] static std::string FormatFallback(
        const std::string_view pattern,
        const std::span<const std::string_view> arguments) {
        std::string result;
        result.reserve(pattern.size() + 32U);
        for (std::size_t index = 0; index < pattern.size();) {
            if (index + 1U < pattern.size() && pattern[index] == '{' &&
                pattern[index + 1U] == '{') {
                result.push_back('{');
                index += 2U;
                continue;
            }
            if (index + 1U < pattern.size() && pattern[index] == '}' &&
                pattern[index + 1U] == '}') {
                result.push_back('}');
                index += 2U;
                continue;
            }
            if (index + 2U < pattern.size() && pattern[index] == '{' &&
                pattern[index + 2U] == '}' && pattern[index + 1U] >= '0' &&
                pattern[index + 1U] <= '7') {
                const std::size_t argument =
                    static_cast<std::size_t>(pattern[index + 1U] - '0');
                if (argument >= arguments.size()) return std::string(pattern);
                result.append(arguments[argument]);
                index += 3U;
                continue;
            }
            result.push_back(pattern[index++]);
        }
        return result;
    }

    const AnomalyLocalizationServiceV1* service_{};
};

}  // namespace anomaly::plugins
