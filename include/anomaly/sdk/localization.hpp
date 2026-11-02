#pragma once
// C++ convenience layer over the `anomaly.localization` service
// (docs/api-reference/lifecycle-and-core.md#anomalylocalization). It is not part of the
// C ABI: it only wraps the service table so a plugin can ask for text by catalog key.
//
// A localized string is a (key, English fallback, arguments) triple. The host resolves
// `key` from the catalog the plugin ships next to its manifest
// (`locales/<locale>.json`, see docs/developer-guide/plugin-development.md), and only
// uses that message when its `{0}..{7}` placeholders are exactly the ones the fallback
// declares; every other case yields the English fallback. The active language is fixed
// for the lifetime of the process, so text may be resolved where it is produced.

#include "anomaly/sdk/cpp.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace anomaly::sdk {

class Localizer final {
public:
    Localizer() noexcept = default;

    explicit Localizer(const AnomalyHostApiV1* host) noexcept {
        const auto queried = Host(host).Query<AnomalyLocalizationServiceV1>(
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
            views[index] = StringView(arguments[index]);
        }
        std::array<char, kInlineBytes> buffer{};
        std::size_t size = buffer.size();
        AnomalyStatusV1 status = Translate(key, english_fallback, views, arguments.size(),
                                           buffer.data(), &size);
        if (status.code == ANOMALY_STATUS_V1_BUFFER_TOO_SMALL && size > buffer.size() &&
            size <= kMaximumBytes) {
            // The service reports the required byte count (NUL included) for the retry.
            std::string sized(size, '\0');
            std::size_t required = size;
            status = Translate(key, english_fallback, views, arguments.size(), sized.data(),
                               &required);
            if (status.code == ANOMALY_STATUS_V1_OK && required >= 1U &&
                required <= sized.size()) {
                return std::string(sized.data(), required - 1U);
            }
            return FormatFallback(english_fallback, arguments);
        }
        if (status.code == ANOMALY_STATUS_V1_OK && size != 0 && size <= buffer.size()) {
            return std::string(buffer.data(), size - 1U);
        }
        return FormatFallback(english_fallback, arguments);
    }

    // Widget labels that can change while the UI is alive need a stable ImGui identity;
    // the host compares the text before the `###` separator, so the id keeps the state.
    [[nodiscard]] std::string Label(
        const std::string_view key, const std::string_view english_fallback,
        const std::string_view stable_id) const {
        std::string label = Text(key, english_fallback);
        label.append("###").append(stable_id);
        return label;
    }

private:
    static constexpr std::size_t kMaximumArguments = 8;
    static constexpr std::size_t kInlineBytes = 512;
    // A catalog may not exceed one megabyte, so any longer message is a service fault and
    // is served from the English fallback instead of a guessed allocation.
    static constexpr std::size_t kMaximumBytes = 64U * 1024U;

    [[nodiscard]] AnomalyStatusV1 Translate(
        const std::string_view key, const std::string_view english_fallback,
        const std::array<AnomalyStringViewV1, kMaximumArguments>& views,
        const std::size_t argument_count, char* const destination,
        std::size_t* const inout_size) const noexcept {
        return service_->translate(service_->user, StringView(key),
                                   StringView(english_fallback), views.data(),
                                   argument_count, destination, inout_size);
    }

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

}  // namespace anomaly::sdk
