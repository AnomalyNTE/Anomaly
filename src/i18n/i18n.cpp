#include "anomaly/i18n.hpp"

#include "anomaly/i18n_embedded_catalog.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace anomaly {

class TranslatorFactory final {
public:
    static std::shared_ptr<const Translator> Create(
        Locale locale, std::vector<std::string> messages) {
        auto translator = std::make_shared<Translator>();
        translator->locale_ = locale;
        translator->messages_ = std::move(messages);
        return translator;
    }
};

struct PluginCatalogFactory final {
    static std::shared_ptr<const PluginCatalog> Create(
        const Locale locale,
        std::unordered_map<std::string, std::pair<std::string, std::uint8_t>> messages = {}) {
        auto catalog = std::make_shared<PluginCatalog>();
        catalog->locale_ = locale;
        for (auto& [key, message] : messages) {
            catalog->messages_.emplace(
                std::move(key),
                PluginCatalog::Message{std::move(message.first), message.second});
        }
        return catalog;
    }
};

namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kHostCatalogSchemaVersion = 1;
constexpr std::size_t kMaximumHostCatalogBytes = 1024U * 1024U;
constexpr std::size_t kMaximumPluginCatalogBytes = 1024U * 1024U;
constexpr std::size_t kMaximumPluginCatalogMessages = 4096U;
constexpr std::size_t kMessageCount = static_cast<std::size_t>(MessageId::Count);

constexpr std::array<std::string_view, kMessageCount> kMessageKeys{
#define ANOMALY_I18N_MESSAGE(name, key) key,
#include "anomaly/i18n_messages.inc"
#undef ANOMALY_I18N_MESSAGE
};

struct ParsedCatalog final {
    std::vector<std::optional<std::string>> messages;
    std::vector<std::uint8_t> argument_masks;
    std::vector<CatalogDiagnostic> diagnostics;
};

bool StartsWithAsciiInsensitive(
    std::string_view value, std::string_view prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto actual = static_cast<unsigned char>(value[index]);
        const auto expected = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(actual) != std::tolower(expected)) return false;
    }
    return true;
}

std::optional<std::size_t> FindMessageIndex(std::string_view key) noexcept {
    const auto iterator = std::find(kMessageKeys.begin(), kMessageKeys.end(), key);
    if (iterator == kMessageKeys.end()) return std::nullopt;
    return static_cast<std::size_t>(iterator - kMessageKeys.begin());
}

std::optional<std::uint8_t> InspectFormat(
    std::string_view text, std::string* reason = nullptr) {
    std::uint8_t mask{};
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '{') {
            if (index + 1 < text.size() && text[index + 1] == '{') {
                index += 2;
                continue;
            }
            if (index + 2 < text.size() && text[index + 1] >= '0' &&
                text[index + 1] <= '7' && text[index + 2] == '}') {
                mask = static_cast<std::uint8_t>(
                    mask | (1U << static_cast<unsigned>(text[index + 1] - '0')));
                index += 3;
                continue;
            }
            if (reason != nullptr) *reason = "invalid opening brace or argument placeholder";
            return std::nullopt;
        }
        if (text[index] == '}') {
            if (index + 1 < text.size() && text[index + 1] == '}') {
                index += 2;
                continue;
            }
            if (reason != nullptr) *reason = "unescaped closing brace";
            return std::nullopt;
        }
        ++index;
    }
    return mask;
}

bool ValidPluginMessageKey(const std::string_view key) noexcept {
    if (key.empty() || key.size() > 255U) return false;
    return std::all_of(key.begin(), key.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '.' ||
            character == '_' || character == '-';
    });
}

ParsedCatalog ParseCatalogDocument(
    std::string_view json, Locale expected_locale, bool require_complete) {
    ParsedCatalog result;
    result.messages.resize(kMessageCount);
    result.argument_masks.resize(kMessageCount);
    try {
        const Json document = Json::parse(json);
        if (!document.is_object()) {
            result.diagnostics.push_back({"", "host catalog root must be an object"});
            return result;
        }
        if (!document.contains("schemaVersion") ||
            !document.at("schemaVersion").is_number_unsigned() ||
            document.at("schemaVersion").get<std::uint32_t>() !=
                kHostCatalogSchemaVersion) {
            result.diagnostics.push_back(
                {"/schemaVersion", "unsupported host catalog schema version"});
            return result;
        }
        if (!document.contains("locale") || !document.at("locale").is_string() ||
            document.at("locale").get<std::string>() != LocaleName(expected_locale)) {
            result.diagnostics.push_back(
                {"/locale", "host catalog locale does not match the requested locale"});
            return result;
        }
        if (!document.contains("messages") || !document.at("messages").is_object()) {
            result.diagnostics.push_back(
                {"/messages", "host catalog messages must be an object"});
            return result;
        }

        for (const auto& [key, value] : document.at("messages").items()) {
            const auto message_index = FindMessageIndex(key);
            if (!message_index.has_value()) {
                result.diagnostics.push_back(
                    {"/messages/" + key, "host catalog contains an unknown message key"});
                continue;
            }
            if (!value.is_string()) {
                result.diagnostics.push_back(
                    {"/messages/" + key, "host catalog message must be a string"});
                continue;
            }
            std::string message = value.get<std::string>();
            if (message.empty()) {
                result.diagnostics.push_back(
                    {"/messages/" + key, "host catalog message must not be empty"});
                continue;
            }
            std::string reason;
            const auto argument_mask = InspectFormat(message, &reason);
            if (!argument_mask.has_value()) {
                result.diagnostics.push_back({"/messages/" + key, std::move(reason)});
                continue;
            }
            result.argument_masks[*message_index] = *argument_mask;
            result.messages[*message_index] = std::move(message);
        }
        if (require_complete) {
            for (std::size_t index = 0; index < kMessageCount; ++index) {
                if (!result.messages[index].has_value()) {
                    result.diagnostics.push_back(
                        {"/messages/" + std::string(kMessageKeys[index]),
                         "English baseline is missing a message"});
                }
            }
        }
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"", error.what()});
    }
    return result;
}

std::shared_ptr<const Translator> MakeTranslator(
    Locale locale, const ParsedCatalog& english, const ParsedCatalog* localized = nullptr) {
    std::vector<std::string> messages;
    messages.reserve(kMessageCount);
    for (std::size_t index = 0; index < kMessageCount; ++index) {
        if (localized != nullptr && localized->messages[index].has_value()) {
            messages.push_back(*localized->messages[index]);
        } else {
            messages.push_back(*english.messages[index]);
        }
    }
    return TranslatorFactory::Create(locale, std::move(messages));
}

std::string FormatMessage(
    std::string_view format, std::span<const std::string_view> arguments) {
    std::string output;
    output.reserve(format.size());
    for (std::size_t index = 0; index < format.size();) {
        if (format[index] == '{' && index + 1 < format.size() && format[index + 1] == '{') {
            output.push_back('{');
            index += 2;
            continue;
        }
        if (format[index] == '}' && index + 1 < format.size() && format[index + 1] == '}') {
            output.push_back('}');
            index += 2;
            continue;
        }
        if (format[index] == '{' && index + 2 < format.size() &&
            format[index + 1] >= '0' && format[index + 1] <= '7' &&
            format[index + 2] == '}') {
            const std::size_t argument = static_cast<std::size_t>(format[index + 1] - '0');
            if (argument < arguments.size()) {
                output.append(arguments[argument]);
            } else {
                output.append(format.substr(index, 3));
            }
            index += 3;
            continue;
        }
        output.push_back(format[index]);
        ++index;
    }
    return output;
}

}  // namespace

LanguagePreferenceParseResult ParseLanguagePreference(std::string_view value) noexcept {
    if (value == "auto") return {LanguagePreference::Auto, true};
    if (value == "en-US") return {LanguagePreference::EnUs, true};
    if (value == "zh-CN") return {LanguagePreference::ZhCn, true};
    return {LanguagePreference::EnUs, false};
}

Locale ResolveLocale(
    LanguagePreference preference, std::string_view windows_locale_name) noexcept {
    if (preference == LanguagePreference::EnUs) return Locale::EnUs;
    if (preference == LanguagePreference::ZhCn) return Locale::ZhCn;

    if ((windows_locale_name.size() == 5 &&
            (StartsWithAsciiInsensitive(windows_locale_name, "zh-CN") ||
             StartsWithAsciiInsensitive(windows_locale_name, "zh-SG"))) ||
        StartsWithAsciiInsensitive(windows_locale_name, "zh-Hans")) {
        return Locale::ZhCn;
    }
    return Locale::EnUs;
}

UserLocaleResolution ResolveUserLocale(LanguagePreference preference) noexcept {
    if (preference != LanguagePreference::Auto) {
        return {ResolveLocale(preference, {}), false};
    }

    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> locale_name{};
    const int length = GetUserDefaultLocaleName(
        locale_name.data(), static_cast<int>(locale_name.size()));
    if (length <= 1) return {Locale::EnUs, true};

    const int utf8_length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, locale_name.data(), length - 1,
        nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) return {Locale::EnUs, true};
    std::string utf8_name(static_cast<std::size_t>(utf8_length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, locale_name.data(), length - 1,
            utf8_name.data(), utf8_length, nullptr, nullptr) != utf8_length) {
        return {Locale::EnUs, true};
    }
    return {ResolveLocale(preference, utf8_name), false};
}

std::string_view LocaleName(Locale locale) noexcept {
    switch (locale) {
    case Locale::ZhCn: return "zh-CN";
    case Locale::EnUs: return "en-US";
    }
    return "en-US";
}

std::string_view LanguagePreferenceName(LanguagePreference preference) noexcept {
    switch (preference) {
    case LanguagePreference::Auto: return "auto";
    case LanguagePreference::ZhCn: return "zh-CN";
    case LanguagePreference::EnUs: return "en-US";
    }
    return "en-US";
}

std::string_view MessageKey(MessageId id) noexcept {
    const std::size_t index = static_cast<std::size_t>(id);
    return index < kMessageKeys.size() ? kMessageKeys[index] : std::string_view{};
}

std::string StableDisplayLabel(
    std::string_view display_text, std::string_view stable_id) {
    std::string result;
    result.reserve(display_text.size() + stable_id.size() + 3);
    result.append(display_text);
    result.append("###");
    result.append(stable_id);
    return result;
}

std::string_view Translator::Text(MessageId id) const noexcept {
    const std::size_t index = static_cast<std::size_t>(id);
    return index < messages_.size() ? std::string_view(messages_[index]) : std::string_view{};
}

std::string Translator::Format(
    MessageId id, std::span<const std::string_view> arguments) const {
    return FormatMessage(Text(id), arguments);
}

PluginTranslation PluginCatalog::Translate(
    const std::string_view key,
    const std::string_view english_fallback,
    const std::span<const std::string_view> arguments) const {
    PluginTranslation result;
    const auto fallback_mask = InspectFormat(english_fallback);
    const auto found = messages_.find(std::string(key));
    const bool localized = found != messages_.end() && fallback_mask.has_value() &&
        found->second.argument_mask == *fallback_mask;
    result.used_english_fallback = !localized;
    result.argument_mismatch = found != messages_.end() && !localized;
    result.text = FormatMessage(
        localized ? std::string_view(found->second.text) : english_fallback,
        arguments);
    return result;
}

TranslatorLoadResult ParseHostCatalog(
    Locale requested_locale, std::string_view localized_catalog_json) {
    TranslatorLoadResult result;
    const ParsedCatalog english = ParseCatalogDocument(
        detail::kEmbeddedEnglishHostCatalog, Locale::EnUs, true);
    if (!english.diagnostics.empty()) {
        result.diagnostics = english.diagnostics;
        return result;
    }
    if (requested_locale == Locale::EnUs) {
        result.translator = MakeTranslator(Locale::EnUs, english);
        return result;
    }
    if (localized_catalog_json.empty()) {
        result.diagnostics.push_back({"", "requested host catalog is unavailable"});
        result.translator = MakeTranslator(Locale::EnUs, english);
        result.used_english_fallback = true;
        return result;
    }

    const ParsedCatalog localized = ParseCatalogDocument(
        localized_catalog_json, requested_locale, false);
    result.diagnostics = localized.diagnostics;
    if (!result.diagnostics.empty()) {
        result.translator = MakeTranslator(Locale::EnUs, english);
        result.used_english_fallback = true;
        return result;
    }
    for (std::size_t index = 0; index < kMessageCount; ++index) {
        if (localized.messages[index].has_value() &&
            localized.argument_masks[index] != english.argument_masks[index]) {
            result.diagnostics.push_back(
                {"/messages/" + std::string(kMessageKeys[index]),
                 "localized message arguments do not match the English baseline"});
        }
    }
    if (!result.diagnostics.empty()) {
        result.translator = MakeTranslator(Locale::EnUs, english);
        result.used_english_fallback = true;
        return result;
    }
    result.translator = MakeTranslator(requested_locale, english, &localized);
    return result;
}

TranslatorLoadResult LoadHostCatalog(
    Locale requested_locale, const std::filesystem::path& locale_directory) {
    if (requested_locale == Locale::EnUs) return ParseHostCatalog(Locale::EnUs);

    const std::filesystem::path path =
        locale_directory / (std::string(LocaleName(requested_locale)) + ".json");
    const auto load_failure = [&](std::string message) {
        auto result = ParseHostCatalog(requested_locale);
        if (result.translator != nullptr) {
            result.diagnostics = {{path.string(), std::move(message)}};
        }
        return result;
    };
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumHostCatalogBytes) {
        return load_failure(error
            ? "requested host catalog is unreadable"
            : "requested host catalog exceeds maximum size");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return load_failure("requested host catalog is unreadable");
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input && !contents.empty()) {
        return load_failure("requested host catalog could not be read");
    }
    auto result = ParseHostCatalog(requested_locale, contents);
    for (auto& diagnostic : result.diagnostics) {
        diagnostic.path = path.string() + diagnostic.path;
    }
    return result;
}

PluginCatalogLoadResult ParsePluginCatalog(
    const Locale requested_locale, const std::string_view localized_catalog_json) {
    PluginCatalogLoadResult result;
    if (requested_locale == Locale::EnUs) {
        result.catalog = PluginCatalogFactory::Create(Locale::EnUs);
        return result;
    }
    if (localized_catalog_json.empty()) {
        result.catalog = PluginCatalogFactory::Create(requested_locale);
        result.diagnostics.push_back({"", "requested plugin catalog is unavailable"});
        result.used_english_fallback = true;
        return result;
    }

    std::unordered_map<std::string, std::pair<std::string, std::uint8_t>> messages;
    try {
        const Json document = Json::parse(localized_catalog_json);
        if (!document.is_object()) {
            result.diagnostics.push_back({"", "plugin catalog root must be an object"});
        } else if (!document.contains("schemaVersion") ||
                   !document.at("schemaVersion").is_number_unsigned() ||
                   document.at("schemaVersion").get<std::uint32_t>() !=
                       kHostCatalogSchemaVersion) {
            result.diagnostics.push_back(
                {"/schemaVersion", "unsupported plugin catalog schema version"});
        } else if (!document.contains("locale") || !document.at("locale").is_string() ||
                   document.at("locale").get<std::string>() != LocaleName(requested_locale)) {
            result.diagnostics.push_back(
                {"/locale", "plugin catalog locale does not match the requested locale"});
        } else if (!document.contains("messages") ||
                   !document.at("messages").is_object()) {
            result.diagnostics.push_back(
                {"/messages", "plugin catalog messages must be an object"});
        } else if (document.at("messages").size() > kMaximumPluginCatalogMessages) {
            result.diagnostics.push_back(
                {"/messages", "plugin catalog contains too many messages"});
        } else {
            for (const auto& [key, value] : document.at("messages").items()) {
                if (!ValidPluginMessageKey(key)) {
                    result.diagnostics.push_back(
                        {"/messages/" + key, "plugin message key is invalid"});
                    continue;
                }
                if (!value.is_string()) {
                    result.diagnostics.push_back(
                        {"/messages/" + key, "plugin catalog message must be a string"});
                    continue;
                }
                std::string message = value.get<std::string>();
                if (message.empty()) {
                    result.diagnostics.push_back(
                        {"/messages/" + key, "plugin catalog message must not be empty"});
                    continue;
                }
                std::string reason;
                const auto argument_mask = InspectFormat(message, &reason);
                if (!argument_mask.has_value()) {
                    result.diagnostics.push_back({"/messages/" + key, std::move(reason)});
                    continue;
                }
                messages.emplace(key, std::pair{std::move(message), *argument_mask});
            }
        }
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"", error.what()});
    }
    if (!result.diagnostics.empty()) {
        messages.clear();
        result.used_english_fallback = true;
    }
    result.catalog = PluginCatalogFactory::Create(requested_locale, std::move(messages));
    return result;
}

PluginCatalogLoadResult LoadPluginCatalog(
    const Locale requested_locale, const std::filesystem::path& package_directory) {
    if (requested_locale == Locale::EnUs) return ParsePluginCatalog(Locale::EnUs);

    const std::filesystem::path path = package_directory / L"locales" /
        (std::string(LocaleName(requested_locale)) + ".json");
    const auto load_failure = [&](std::string message) {
        auto result = ParsePluginCatalog(requested_locale);
        result.diagnostics = {{path.string(), std::move(message)}};
        return result;
    };
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumPluginCatalogBytes) {
        return load_failure(error
            ? "requested plugin catalog is unreadable"
            : "requested plugin catalog exceeds maximum size");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return load_failure("requested plugin catalog is unreadable");
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input && !contents.empty()) {
        return load_failure("requested plugin catalog could not be read");
    }
    auto result = ParsePluginCatalog(requested_locale, contents);
    for (auto& diagnostic : result.diagnostics) {
        diagnostic.path = path.string() + diagnostic.path;
    }
    return result;
}

}  // namespace anomaly
