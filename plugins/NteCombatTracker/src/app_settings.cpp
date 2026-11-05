#include "app_settings.hpp"

#include <windows.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace nte::settings {
namespace {

using Json = nlohmann::json;

constexpr wchar_t kOverlaySettingsMutexName[] =
    L"Local\\NteCombatStats.OverlaySettings.v1";
constexpr wchar_t kPluginSettingsMutexName[] =
    L"Local\\NteCombatStats.PluginSettings.v1";

class SettingsLock {
public:
    SettingsLock(const wchar_t* name, std::string& error) {
        handle_ = CreateMutexW(nullptr, FALSE, name);
        if (handle_ == nullptr) {
            error = "failed to create settings lock: " +
                std::to_string(GetLastError());
            return;
        }
        const DWORD wait = WaitForSingleObject(handle_, 5000);
        acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!acquired_) error = "timed out waiting for settings lock";
    }
    ~SettingsLock() {
        if (acquired_) ReleaseMutex(handle_);
        if (handle_ != nullptr) CloseHandle(handle_);
    }
    SettingsLock(const SettingsLock&) = delete;
    SettingsLock& operator=(const SettingsLock&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return acquired_; }

private:
    HANDLE handle_{};
    bool acquired_{};
};

bool AtomicWrite(
    const std::filesystem::path& path,
    const std::string& content,
    std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "failed to create settings directory: " + filesystem_error.message();
        return false;
    }
    const auto temporary = path.parent_path() /
        (path.filename().wstring() + L"." +
            std::to_wstring(GetCurrentProcessId()) + L".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "failed to open temporary settings file";
            return false;
        }
        stream << content << '\n';
        stream.flush();
        if (!stream) {
            error = "failed to write temporary settings file";
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "failed to replace settings file: " + std::to_string(GetLastError());
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

bool WriteStaged(
    const std::filesystem::path& destination,
    const std::string& content,
    std::filesystem::path& staged,
    std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "failed to create settings directory: " + filesystem_error.message();
        return false;
    }
    staged = destination.parent_path() /
        (destination.filename().wstring() + L"." +
            std::to_wstring(GetCurrentProcessId()) + L".pending");
    std::ofstream stream(staged, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "failed to open staged settings file";
        return false;
    }
    stream << content << '\n';
    stream.flush();
    if (!stream) {
        error = "failed to write staged settings file";
        stream.close();
        std::filesystem::remove(staged, filesystem_error);
        staged.clear();
        return false;
    }
    return true;
}

bool SaveOverlaySettingsUnlocked(
    const std::filesystem::path& path,
    const OverlaySettings& settings,
    std::string& error) {
    const Json json{
        {"schema_version", 2}, {"x", settings.x}, {"y", settings.y},
        {"width", settings.width}, {"height", settings.height},
        {"locked", settings.locked}, {"collapsed", settings.collapsed},
        {"topmost", settings.topmost}, {"hide_idle", settings.hide_idle},
        {"opacity", settings.opacity}, {"ui_scale", settings.ui_scale},
        {"sort_column", static_cast<int>(settings.sort_column)},
        {"descending", settings.descending},
        {"active_tab", static_cast<int>(settings.active_tab)}};
    return AtomicWrite(path, json.dump(2), error);
}

PluginSettings LoadPluginSettingsUnlocked(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_log_root,
    std::string& error) {
    error.clear();
    PluginSettings result{fallback_log_root};
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return result;
    try {
        const Json json = Json::parse(stream);
        if (json.value("schema_version", 0) != 1) {
            error = "unsupported plugin settings schema";
            return result;
        }
        const std::string configured = json.value("log_root", std::string{});
        const auto log_root = PathFromUtf8(configured);
        if (!log_root || !log_root->is_absolute()) {
            error = configured.empty() ? "configured log root is empty" :
                "configured log root is not absolute";
            return result;
        }
        result.log_root = *log_root;
        const std::string export_value = json.value("export_root", std::string{});
        if (!export_value.empty()) {
            const auto export_root = PathFromUtf8(export_value);
            if (!export_root || !export_root->is_absolute()) {
                error = "configured export root is not absolute";
                return PluginSettings{fallback_log_root};
            }
            result.export_root = *export_root;
        }
        result.developer_mode = json.value("developer_mode", false);
        return result;
    } catch (const std::exception& exception) {
        error = std::string("invalid plugin settings: ") + exception.what();
        return result;
    }
}

Json PluginSettingsJson(const PluginSettings& settings) {
    Json json{{"schema_version", 1},
        {"log_root", PathToUtf8(settings.log_root)},
        {"developer_mode", settings.developer_mode}};
    if (settings.export_root) {
        json["export_root"] = PathToUtf8(*settings.export_root);
    }
    return json;
}

}  // namespace

std::filesystem::path SettingsDirectory() {
    PWSTR local{};
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) {
        return {};
    }
    std::filesystem::path result(local);
    CoTaskMemFree(local);
    return result / L"NTECombatStats";
}

std::filesystem::path OverlaySettingsPath() {
    const auto directory = SettingsDirectory();
    return directory.empty() ? std::filesystem::path{} :
        directory / L"overlay-settings.json";
}

std::filesystem::path PluginSettingsPath() {
    const auto directory = SettingsDirectory();
    return directory.empty() ? std::filesystem::path{} :
        directory / L"plugin-settings.json";
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::optional<std::filesystem::path> PathFromUtf8(const std::string& value) {
    if (value.empty()) return std::nullopt;
    try {
        return std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(value.data()), value.size()));
    } catch (...) {
        return std::nullopt;
    }
}

PendingPluginSettings::PendingPluginSettings(
    std::filesystem::path destination,
    std::filesystem::path staged) noexcept
    : destination_(std::move(destination)), staged_(std::move(staged)) {}

PendingPluginSettings::~PendingPluginSettings() {
    if (staged_.empty()) return;
    std::error_code error;
    std::filesystem::remove(staged_, error);
}

PendingPluginSettings::PendingPluginSettings(
    PendingPluginSettings&& other) noexcept
    : destination_(std::move(other.destination_)), staged_(std::move(other.staged_)) {
    other.staged_.clear();
}

PendingPluginSettings& PendingPluginSettings::operator=(
    PendingPluginSettings&& other) noexcept {
    if (this == &other) return *this;
    if (!staged_.empty()) {
        std::error_code error;
        std::filesystem::remove(staged_, error);
    }
    destination_ = std::move(other.destination_);
    staged_ = std::move(other.staged_);
    other.staged_.clear();
    return *this;
}

bool PendingPluginSettings::Commit(std::string& error) {
    error.clear();
    if (staged_.empty() || destination_.empty()) {
        error = "plugin settings update is not staged";
        return false;
    }
    SettingsLock lock(kPluginSettingsMutexName, error);
    if (!lock) return false;
    auto staged = LoadPluginSettingsUnlocked(staged_, {}, error);
    if (!error.empty() || staged.log_root.empty()) return false;
    std::string current_error;
    const auto current = LoadPluginSettingsUnlocked(
        destination_, staged.log_root, current_error);
    std::error_code exists_error;
    if (std::filesystem::exists(destination_, exists_error) &&
        current_error.empty()) {
        staged.export_root = current.export_root;
        staged.developer_mode = current.developer_mode;
    }
    if (!AtomicWrite(destination_, PluginSettingsJson(staged).dump(2), error)) {
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::remove(staged_, filesystem_error);
    staged_.clear();
    return true;
}

bool LoadOverlaySettings(
    const std::filesystem::path& path,
    OverlaySettings& settings,
    std::string& error) {
    error.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    try {
        const Json json = Json::parse(stream);
        if (json.value("schema_version", 0) != 2) {
            error = "unsupported overlay settings schema";
            return false;
        }
        settings.x = json.value("x", settings.x);
        settings.y = json.value("y", settings.y);
        settings.width = std::clamp(json.value("width", settings.width), 420, 1400);
        settings.height = std::clamp(json.value("height", settings.height), 180, 1000);
        settings.locked = json.value("locked", settings.locked);
        settings.collapsed = json.value("collapsed", settings.collapsed);
        settings.topmost = json.value("topmost", settings.topmost);
        settings.hide_idle = json.value("hide_idle", settings.hide_idle);
        settings.opacity = std::clamp(
            json.value("opacity", settings.opacity), 0.4F, 1.0F);
        settings.ui_scale = std::clamp(
            json.value("ui_scale", settings.ui_scale), 0.75F, 2.0F);
        settings.sort_column = static_cast<nte::combat_stats::overlay::SortColumn>(
            std::clamp(json.value("sort_column",
                static_cast<int>(settings.sort_column)), 0, 9));
        settings.descending = json.value("descending", settings.descending);
        settings.active_tab = static_cast<DataTab>(std::clamp(
            json.value("active_tab", static_cast<int>(settings.active_tab)), 0, 3));
        return true;
    } catch (const std::exception& exception) {
        error = std::string("invalid overlay settings: ") + exception.what();
        return false;
    }
}

bool SaveOverlaySettings(
    const std::filesystem::path& path,
    const OverlaySettings& settings,
    std::string& error) {
    SettingsLock lock(kOverlaySettingsMutexName, error);
    return lock && SaveOverlaySettingsUnlocked(path, settings, error);
}

bool UpdateOverlayPreferences(
    const std::filesystem::path& path,
    const OverlaySettings& preferences,
    OverlaySettings& merged,
    std::string& error) {
    SettingsLock lock(kOverlaySettingsMutexName, error);
    if (!lock) return false;
    merged = preferences;
    std::string load_error;
    static_cast<void>(LoadOverlaySettings(path, merged, load_error));
    merged.locked = preferences.locked;
    merged.collapsed = preferences.collapsed;
    merged.topmost = preferences.topmost;
    merged.hide_idle = preferences.hide_idle;
    merged.opacity = preferences.opacity;
    merged.ui_scale = preferences.ui_scale;
    return SaveOverlaySettingsUnlocked(path, merged, error);
}

PluginSettings LoadPluginSettings(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_log_root,
    std::string& error) {
    return LoadPluginSettingsUnlocked(path, fallback_log_root, error);
}

bool SavePluginSettings(
    const std::filesystem::path& path,
    const PluginSettings& settings,
    std::string& error) {
    if (!settings.log_root.is_absolute()) {
        error = "log root must be an absolute path";
        return false;
    }
    if (settings.export_root && !settings.export_root->is_absolute()) {
        error = "export root must be an absolute path";
        return false;
    }
    SettingsLock lock(kPluginSettingsMutexName, error);
    return lock && AtomicWrite(path, PluginSettingsJson(settings).dump(2), error);
}

bool UpdatePluginExportRoot(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_log_root,
    const std::optional<std::filesystem::path>& export_root,
    PluginSettings& merged,
    std::string& error) {
    if (export_root && !export_root->is_absolute()) {
        error = "export root must be an absolute path";
        return false;
    }
    SettingsLock lock(kPluginSettingsMutexName, error);
    if (!lock) return false;
    std::string load_error;
    merged = LoadPluginSettingsUnlocked(path, fallback_log_root, load_error);
    if (!load_error.empty()) {
        error = std::move(load_error);
        return false;
    }
    merged.export_root = export_root;
    return AtomicWrite(path, PluginSettingsJson(merged).dump(2), error);
}

bool UpdatePluginLogRoot(
    const std::filesystem::path& path,
    const std::filesystem::path& log_root,
    PluginSettings& merged,
    std::string& error) {
    if (!log_root.is_absolute()) {
        error = "log root must be an absolute path";
        return false;
    }
    SettingsLock lock(kPluginSettingsMutexName, error);
    if (!lock) return false;
    std::string load_error;
    merged = LoadPluginSettingsUnlocked(path, log_root, load_error);
    if (!load_error.empty()) {
        error = std::move(load_error);
        return false;
    }
    merged.log_root = log_root;
    return AtomicWrite(path, PluginSettingsJson(merged).dump(2), error);
}

bool UpdatePluginDeveloperMode(
    const std::filesystem::path& path,
    const std::filesystem::path& fallback_log_root,
    const bool developer_mode,
    PluginSettings& merged,
    std::string& error) {
    SettingsLock lock(kPluginSettingsMutexName, error);
    if (!lock) return false;
    std::string load_error;
    merged = LoadPluginSettingsUnlocked(path, fallback_log_root, load_error);
    if (!load_error.empty()) {
        error = std::move(load_error);
        return false;
    }
    merged.developer_mode = developer_mode;
    return AtomicWrite(path, PluginSettingsJson(merged).dump(2), error);
}

std::optional<PendingPluginSettings> StagePluginSettings(
    const std::filesystem::path& path,
    const PluginSettings& settings,
    std::string& error) {
    if (path.empty() || !settings.log_root.is_absolute()) {
        error = "log root must be an absolute path";
        return std::nullopt;
    }
    if (settings.export_root && !settings.export_root->is_absolute()) {
        error = "export root must be an absolute path";
        return std::nullopt;
    }
    const Json json = PluginSettingsJson(settings);
    std::filesystem::path staged;
    if (!WriteStaged(path, json.dump(2), staged, error)) return std::nullopt;
    return PendingPluginSettings(path, std::move(staged));
}

bool PrepareLogRoot(const std::filesystem::path& path, std::string& error) {
    error.clear();
    if (path.empty() || !path.is_absolute()) {
        error = "log root must be an absolute path";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(path, filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(path, filesystem_error)) {
        error = "log root could not be created: " + filesystem_error.message();
        return false;
    }
    const auto probe = path /
        (L".nte-write-test." + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const HANDLE handle = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "log root is not writable: " + std::to_string(GetLastError());
        return false;
    }
    CloseHandle(handle);
    return true;
}

}  // namespace nte::settings
