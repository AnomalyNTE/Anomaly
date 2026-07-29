#include "anomaly/platform_settings.hpp"

#include "anomaly/reliable_storage.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace anomaly {
namespace {

constexpr std::wstring_view kSettingsPath = L"config/platform-settings.json";
constexpr std::wstring_view kAnalyzerConfigPath = L"anomaly.ini";
constexpr std::size_t kMaximumSettingsBytes = 64U * 1024U;

LanguagePreference ReadLanguagePreference(const std::filesystem::path& path) noexcept {
    std::array<wchar_t, 32> buffer{};
    GetPrivateProfileStringW(L"Platform", L"Language", L"auto", buffer.data(),
        static_cast<DWORD>(buffer.size()), path.c_str());
    const std::wstring_view value(buffer.data());
    if (value == L"auto") return LanguagePreference::Auto;
    if (value == L"zh-CN") return LanguagePreference::ZhCn;
    return LanguagePreference::EnUs;
}

const wchar_t* LanguagePreferenceWide(const LanguagePreference value) noexcept {
    switch (value) {
    case LanguagePreference::Auto: return L"auto";
    case LanguagePreference::EnUs: return L"en-US";
    case LanguagePreference::ZhCn: return L"zh-CN";
    }
    return nullptr;
}

bool WriteLanguagePreference(
    const std::filesystem::path& path, const LanguagePreference value) noexcept {
    const wchar_t* const text = LanguagePreferenceWide(value);
    return text != nullptr &&
        WritePrivateProfileStringW(L"Platform", L"Language", text, path.c_str()) != FALSE;
}

bool IsMenuKeyValid(const std::uint32_t key) noexcept {
    if (key == 0 || key > 0xff || key == VK_ESCAPE ||
        (key >= VK_LBUTTON && key <= VK_XBUTTON2)) {
        return false;
    }
    return key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU &&
        key != VK_LSHIFT && key != VK_RSHIFT && key != VK_LCONTROL &&
        key != VK_RCONTROL && key != VK_LMENU && key != VK_RMENU;
}

PlatformInputCapturePolicy ParseCapturePolicy(const std::string_view value) noexcept {
    return value == "menu-open"
        ? PlatformInputCapturePolicy::MenuOpen
        : PlatformInputCapturePolicy::Automatic;
}

PlatformUpdateChannel ParseUpdateChannel(const std::string_view value) noexcept {
    if (value == "preview") return PlatformUpdateChannel::Preview;
    if (value == "nightly") return PlatformUpdateChannel::Nightly;
    return PlatformUpdateChannel::Stable;
}

PlatformMinimumLogLevel ParseLogLevel(const std::string_view value) noexcept {
    if (value == "trace") return PlatformMinimumLogLevel::Trace;
    if (value == "debug") return PlatformMinimumLogLevel::Debug;
    if (value == "warning") return PlatformMinimumLogLevel::Warning;
    if (value == "error") return PlatformMinimumLogLevel::Error;
    return PlatformMinimumLogLevel::Info;
}

nlohmann::json Serialize(
    const PlatformSettingsValues& values,
    const std::uint64_t revision,
    const std::string_view last_route) {
    return {
        {"schemaVersion", 1},
        {"revision", revision},
        {"lastRoute", last_route},
        {"values", {
            {"interface.scale_percent", values.interface_scale_percent},
            {"interface.opacity_percent", values.interface_opacity_percent},
            {"interface.reduced_motion", values.interface_reduced_motion},
            {"interface.remember_last_route", values.interface_remember_last_route},
            {"input.menu_toggle", values.input_menu_toggle},
            {"input.capture_policy", PlatformInputCapturePolicyName(values.input_capture_policy)},
            {"input.gamepad_navigation", values.input_gamepad_navigation},
            {"updates.channel", PlatformUpdateChannelName(values.updates_channel)},
            {"updates.automatic_check", values.updates_automatic_check},
            {"updates.include_disabled", values.updates_include_disabled},
            {"diagnostics.log_level", PlatformMinimumLogLevelName(values.diagnostics_log_level)},
            {"diagnostics.ring_capacity", values.diagnostics_ring_capacity},
            {"advanced.developer_mode", values.advanced_developer_mode},
        }},
    };
}

template <typename T>
void ReadValue(const nlohmann::json& values, const char* id, T& output) {
    const auto found = values.find(id);
    if (found != values.end()) output = found->get<T>();
}

bool IsKnownRoute(const std::string_view route) noexcept {
    return route == "plugins" || route == "diagnostics" || route == "settings";
}

}  // namespace

std::vector<PlatformSettingsValidationError> ValidatePlatformSettings(
    const PlatformSettingsValues& values) {
    std::vector<PlatformSettingsValidationError> errors;
    if (LanguagePreferenceWide(values.interface_language) == nullptr) {
        errors.push_back({"interface.language",
            "Choose auto, en-US, or zh-CN."});
    }
    const auto range_step = [&](const char* id, const std::uint32_t value,
                                const std::uint32_t minimum, const std::uint32_t maximum,
                                const std::uint32_t step, const char* message) {
        if (value < minimum || value > maximum || (value - minimum) % step != 0) {
            errors.push_back({id, message});
        }
    };
    range_step("interface.scale_percent", values.interface_scale_percent,
        75, 200, 5, "Use a value from 75 to 200 in steps of 5.");
    range_step("interface.opacity_percent", values.interface_opacity_percent,
        10, 100, 5, "Use a value from 10 to 100 in steps of 5.");
    if (!IsMenuKeyValid(values.input_menu_toggle)) {
        errors.push_back({"input.menu_toggle",
            "Choose a keyboard key that is not Escape, a modifier, or a mouse button."});
    }
    range_step("diagnostics.ring_capacity", values.diagnostics_ring_capacity,
        1000, 100000, 1000, "Use 1,000 to 100,000 records in steps of 1,000.");
    return errors;
}

std::string_view PlatformInputCapturePolicyName(
    const PlatformInputCapturePolicy value) noexcept {
    switch (value) {
    case PlatformInputCapturePolicy::Automatic: return "automatic";
    case PlatformInputCapturePolicy::MenuOpen: return "menu-open";
    }
    return "automatic";
}

std::string_view PlatformUpdateChannelName(const PlatformUpdateChannel value) noexcept {
    switch (value) {
    case PlatformUpdateChannel::Stable: return "stable";
    case PlatformUpdateChannel::Preview: return "preview";
    case PlatformUpdateChannel::Nightly: return "nightly";
    }
    return "stable";
}

std::string_view PlatformMinimumLogLevelName(
    const PlatformMinimumLogLevel value) noexcept {
    switch (value) {
    case PlatformMinimumLogLevel::Trace: return "trace";
    case PlatformMinimumLogLevel::Debug: return "debug";
    case PlatformMinimumLogLevel::Info: return "info";
    case PlatformMinimumLogLevel::Warning: return "warning";
    case PlatformMinimumLogLevel::Error: return "error";
    }
    return "info";
}

class PlatformSettingsStore::Impl final {
public:
    explicit Impl(std::filesystem::path runtime_root)
        : runtime_root_(std::move(runtime_root)), storage_(runtime_root_.native()) {}

    bool Start() {
        std::scoped_lock lock(mutex_);
        snapshot_ = {};
        const LanguagePreference configured_language = ReadLanguagePreference(
            runtime_root_ / kAnalyzerConfigPath);
        snapshot_.values.interface_language = configured_language;
        snapshot_.revision = 1;
        snapshot_.reason.clear();
        const StorageResult initialized = storage_.InitializationResult();
        if (!initialized) {
            snapshot_.reason = "settings storage is unavailable";
            return false;
        }

        const StorageReadResult read = storage_.Read(kSettingsPath, kMaximumSettingsBytes);
        if (!read) {
            if (read.result.win32_error == ERROR_FILE_NOT_FOUND ||
                read.result.win32_error == ERROR_PATH_NOT_FOUND) {
                snapshot_.ready = true;
                snapshot_.reason = "using default settings";
                return true;
            }
            snapshot_.reason = "settings file could not be read";
            return false;
        }

        try {
            const std::string text(
                reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
            const nlohmann::json document = nlohmann::json::parse(text);
            if (!document.is_object() || document.value("schemaVersion", 0) != 1 ||
                !document.contains("values") || !document.at("values").is_object()) {
                throw nlohmann::json::type_error::create(
                    302, "platform settings document has an unsupported shape", &document);
            }
            const nlohmann::json& values = document.at("values");
            ReadValue(values, "interface.scale_percent", snapshot_.values.interface_scale_percent);
            ReadValue(values, "interface.opacity_percent", snapshot_.values.interface_opacity_percent);
            ReadValue(values, "interface.reduced_motion", snapshot_.values.interface_reduced_motion);
            ReadValue(values, "interface.remember_last_route", snapshot_.values.interface_remember_last_route);
            ReadValue(values, "input.menu_toggle", snapshot_.values.input_menu_toggle);
            ReadValue(values, "input.gamepad_navigation", snapshot_.values.input_gamepad_navigation);
            ReadValue(values, "updates.automatic_check", snapshot_.values.updates_automatic_check);
            ReadValue(values, "updates.include_disabled", snapshot_.values.updates_include_disabled);
            ReadValue(values, "diagnostics.ring_capacity", snapshot_.values.diagnostics_ring_capacity);
            ReadValue(values, "advanced.developer_mode", snapshot_.values.advanced_developer_mode);
            snapshot_.values.input_capture_policy = ParseCapturePolicy(
                values.value("input.capture_policy", std::string{"automatic"}));
            snapshot_.values.updates_channel = ParseUpdateChannel(
                values.value("updates.channel", std::string{"stable"}));
            snapshot_.values.diagnostics_log_level = ParseLogLevel(
                values.value("diagnostics.log_level", std::string{"info"}));
            snapshot_.revision = (std::max)(
                std::uint64_t{1}, document.value("revision", std::uint64_t{1}));
            const std::string route = document.value("lastRoute", std::string{"plugins"});
            if (IsKnownRoute(route)) snapshot_.last_route = route;
            const auto errors = ValidatePlatformSettings(snapshot_.values);
            if (!errors.empty()) throw std::invalid_argument(errors.front().message);
            snapshot_.ready = true;
            snapshot_.reason.clear();
            return true;
        } catch (...) {
            snapshot_.values = {};
            snapshot_.values.interface_language = configured_language;
            snapshot_.revision = 1;
            snapshot_.last_route = "plugins";
            snapshot_.ready = true;
            snapshot_.reason = "persisted settings were invalid; using defaults";
            return true;
        }
    }

    PlatformSettingsSnapshot Snapshot() const {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    PlatformSettingsApplyResult Apply(const PlatformSettingsApplyRequest& request) {
        std::scoped_lock lock(mutex_);
        PlatformSettingsApplyResult result;
        result.snapshot = snapshot_;
        if (!snapshot_.ready) {
            result.code = PlatformSettingsApplyCode::ProviderUnavailable;
            result.message = snapshot_.reason;
            return result;
        }
        if (request.expected_revision != snapshot_.revision) {
            result.code = PlatformSettingsApplyCode::RevisionConflict;
            result.message = "settings changed after this draft was created";
            return result;
        }
        result.validation_errors = ValidatePlatformSettings(request.values);
        if (!result.validation_errors.empty()) {
            result.code = PlatformSettingsApplyCode::ValidationFailed;
            result.message = "one or more settings are invalid";
            return result;
        }

        const std::uint64_t revision = snapshot_.revision + 1;
        const std::string text = Serialize(request.values, revision, snapshot_.last_route).dump(2);
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        const bool language_changed =
            request.values.interface_language != snapshot_.values.interface_language;
        const std::filesystem::path config_path = runtime_root_ / kAnalyzerConfigPath;
        if (language_changed &&
            !WriteLanguagePreference(config_path, request.values.interface_language)) {
            result.code = PlatformSettingsApplyCode::IoFailure;
            result.message = "language preference could not be written";
            return result;
        }
        const StorageResult write = storage_.WriteAtomic(kSettingsPath, bytes);
        if (!write) {
            const bool rolled_back = !language_changed ||
                WriteLanguagePreference(config_path, snapshot_.values.interface_language);
            result.code = PlatformSettingsApplyCode::IoFailure;
            result.message = rolled_back
                ? "settings could not be written atomically"
                : "settings write failed and the language preference rollback also failed";
            return result;
        }
        snapshot_.values = request.values;
        snapshot_.revision = revision;
        snapshot_.reason.clear();
        result.code = PlatformSettingsApplyCode::Applied;
        result.snapshot = snapshot_;
        result.message = "settings saved";
        return result;
    }

    bool RecordLastRoute(const std::string_view route) {
        if (!IsKnownRoute(route)) return false;
        std::scoped_lock lock(mutex_);
        if (!snapshot_.ready || !snapshot_.values.interface_remember_last_route) return false;
        if (snapshot_.last_route == route) return true;
        const std::string text = Serialize(snapshot_.values, snapshot_.revision, route).dump(2);
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        if (!storage_.WriteAtomic(kSettingsPath, bytes)) return false;
        snapshot_.last_route = route;
        return true;
    }

private:
    std::filesystem::path runtime_root_;
    ReliableStorage storage_;
    mutable std::mutex mutex_;
    PlatformSettingsSnapshot snapshot_;
};

PlatformSettingsStore::PlatformSettingsStore(std::filesystem::path runtime_root)
    : impl_(std::make_unique<Impl>(std::move(runtime_root))) {}

PlatformSettingsStore::~PlatformSettingsStore() = default;

bool PlatformSettingsStore::Start() { return impl_->Start(); }

PlatformSettingsSnapshot PlatformSettingsStore::Snapshot() const { return impl_->Snapshot(); }

PlatformSettingsApplyResult PlatformSettingsStore::Apply(
    const PlatformSettingsApplyRequest& request) {
    return impl_->Apply(request);
}

bool PlatformSettingsStore::RecordLastRoute(const std::string_view route) {
    return impl_->RecordLastRoute(route);
}

}  // namespace anomaly
