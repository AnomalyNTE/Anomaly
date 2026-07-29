#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anomaly {

struct TranslatorLoadResult;
class TranslatorFactory;

enum class LanguagePreference : std::uint8_t {
    Auto,
    EnUs,
    ZhCn,
};

enum class Locale : std::uint8_t {
    EnUs,
    ZhCn,
};

struct LanguagePreferenceParseResult final {
    LanguagePreference preference{LanguagePreference::Auto};
    bool valid{true};
};

struct UserLocaleResolution final {
    Locale locale{Locale::EnUs};
    bool system_query_failed{};
};

[[nodiscard]] LanguagePreferenceParseResult ParseLanguagePreference(
    std::string_view value) noexcept;
[[nodiscard]] Locale ResolveLocale(
    LanguagePreference preference, std::string_view windows_locale_name) noexcept;
[[nodiscard]] UserLocaleResolution ResolveUserLocale(
    LanguagePreference preference) noexcept;
[[nodiscard]] std::string_view LocaleName(Locale locale) noexcept;
[[nodiscard]] std::string_view LanguagePreferenceName(
    LanguagePreference preference) noexcept;

enum class MessageId : std::uint16_t {
#define ANOMALY_I18N_MESSAGE(name, key) name,
#include "anomaly/i18n_messages.inc"
#undef ANOMALY_I18N_MESSAGE
    Count,
};

[[nodiscard]] std::string_view MessageKey(MessageId id) noexcept;
[[nodiscard]] std::string StableDisplayLabel(
    std::string_view display_text, std::string_view stable_id);

class Translator final {
public:
    [[nodiscard]] Locale locale() const noexcept { return locale_; }
    [[nodiscard]] std::string_view Text(MessageId id) const noexcept;
    [[nodiscard]] std::string Format(
        MessageId id, std::span<const std::string_view> arguments) const;

private:
    friend class TranslatorFactory;
    friend TranslatorLoadResult ParseHostCatalog(Locale, std::string_view);

    Locale locale_{Locale::EnUs};
    std::vector<std::string> messages_;
};

struct CatalogDiagnostic final {
    std::string path;
    std::string message;
};

struct PluginTranslation final {
    std::string text;
    bool used_english_fallback{};
    bool argument_mismatch{};
};

class PluginCatalog final {
public:
    [[nodiscard]] Locale locale() const noexcept { return locale_; }
    [[nodiscard]] PluginTranslation Translate(
        std::string_view key,
        std::string_view english_fallback,
        std::span<const std::string_view> arguments) const;

private:
    friend struct PluginCatalogFactory;

    struct Message final {
        std::string text;
        std::uint8_t argument_mask{};
    };

    Locale locale_{Locale::EnUs};
    std::unordered_map<std::string, Message> messages_;
};

struct PluginCatalogLoadResult final {
    std::shared_ptr<const PluginCatalog> catalog;
    std::vector<CatalogDiagnostic> diagnostics;
    bool used_english_fallback{};
};

struct TranslatorLoadResult final {
    std::shared_ptr<const Translator> translator;
    std::vector<CatalogDiagnostic> diagnostics;
    bool used_english_fallback{};

    [[nodiscard]] bool Ok() const noexcept {
        return translator != nullptr && diagnostics.empty();
    }
};

[[nodiscard]] TranslatorLoadResult ParseHostCatalog(
    Locale requested_locale, std::string_view localized_catalog_json = {});
[[nodiscard]] TranslatorLoadResult LoadHostCatalog(
    Locale requested_locale, const std::filesystem::path& locale_directory);
[[nodiscard]] PluginCatalogLoadResult ParsePluginCatalog(
    Locale requested_locale, std::string_view localized_catalog_json = {});
[[nodiscard]] PluginCatalogLoadResult LoadPluginCatalog(
    Locale requested_locale, const std::filesystem::path& package_directory);

}  // namespace anomaly
