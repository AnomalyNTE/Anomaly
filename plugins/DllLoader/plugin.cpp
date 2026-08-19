#include "anomaly/sdk/cpp.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr std::string_view kSettingsSchemaId = "dll-loader-settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::string_view kDefaultDllPath = "dumper-7.dll";
constexpr std::size_t kMaximumDllPathBytes = 4096;
constexpr std::string_view kSettingsSchema = R"json(
{
  "type":"object",
  "additionalProperties":false,
  "required":["dllPath"],
  "properties":{"dllPath":{"type":"string","maxLength":4096}}
}
)json";

HMODULE g_plugin_module{};

struct Context final {
    const AnomalyConfigServiceV1* config{};
    const AnomalyUiServiceV1* ui{};
    AnomalyGenerationHandleV1 settings_schema{};
    std::mutex mutex;
    std::array<char, kMaximumDllPathBytes + 1> editor{};
    std::string persisted_dll_path{kDefaultDllPath};
    std::string dll_path{kDefaultDllPath};
    std::string loaded_path;
    std::string status{"Waiting for plugin activation"};
    HMODULE loaded_module{};
    bool settings_dirty{};
    bool stopped{};
};

AnomalyStatusV1 Status(const std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

bool ConfigReady(const AnomalyConfigServiceV1* service) noexcept {
    return HasField<AnomalyConfigServiceV1, decltype(AnomalyConfigServiceV1::write_atomic)>(
               service, offsetof(AnomalyConfigServiceV1, write_atomic)) &&
        service->service_version >= ANOMALY_CONFIG_SERVICE_V1_VERSION &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr;
}

bool HasUiWindow(const AnomalyUiServiceV1* ui) noexcept {
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::end_window)>(
               ui, offsetof(AnomalyUiServiceV1, end_window)) &&
        ui->begin_window != nullptr && ui->end_window != nullptr && ui->text != nullptr;
}

bool HasUiInput(const AnomalyUiServiceV1* ui) noexcept {
    return HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::input_text)>(
               ui, offsetof(AnomalyUiServiceV1, input_text)) &&
        HasField<AnomalyUiServiceV1, decltype(AnomalyUiServiceV1::button)>(
            ui, offsetof(AnomalyUiServiceV1, button)) &&
        ui->input_text != nullptr && ui->button != nullptr;
}

std::wstring Utf8ToWide(const std::string_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required) != required) {
        return {};
    }
    return result;
}

std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

std::filesystem::path PluginPackageDirectory(std::string& error) {
    if (g_plugin_module == nullptr) {
        error = "The plugin module handle is unavailable";
        return {};
    }
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        g_plugin_module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        error = "The plugin package path is unavailable";
        return {};
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
}

bool ResolveLibraryPath(
    const std::string_view configured_path, std::filesystem::path& resolved,
    std::string& error) {
    if (configured_path.empty()) {
        error = "DLL loading is disabled";
        return false;
    }
    if (configured_path.size() > kMaximumDllPathBytes) {
        error = "The configured DLL path is too long";
        return false;
    }
    const std::wstring wide_path = Utf8ToWide(configured_path);
    if (wide_path.empty()) {
        error = "The configured DLL path is not valid UTF-8";
        return false;
    }

    std::filesystem::path candidate(wide_path);
    if (candidate.extension().empty()) candidate += L".dll";
    if (_wcsicmp(candidate.extension().c_str(), L".dll") != 0) {
        error = "The configured file must have a .dll extension";
        return false;
    }
    if (candidate.is_relative()) {
        const std::filesystem::path package_directory = PluginPackageDirectory(error);
        if (package_directory.empty()) return false;
        candidate = package_directory / candidate;
    }

    std::error_code filesystem_error;
    resolved = std::filesystem::absolute(candidate, filesystem_error).lexically_normal();
    if (filesystem_error) {
        error = "The configured DLL path could not be resolved";
        return false;
    }
    if (!std::filesystem::is_regular_file(resolved, filesystem_error) || filesystem_error) {
        error = "The configured DLL file was not found";
        return false;
    }
    return true;
}

void SetEditor(Context& context, const std::string_view value) noexcept {
    context.editor.fill('\0');
    const std::size_t count = (std::min)(value.size(), context.editor.size() - 1U);
    std::copy_n(value.data(), count, context.editor.data());
}

bool ReadSettings(Context& context) {
    std::uint32_t schema_version{};
    std::size_t size{};
    const AnomalyStatusV1 size_status = context.config->read(
        context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
        SetEditor(context, context.dll_path);
        return true;
    }
    if (size_status.code != ANOMALY_STATUS_V1_OK ||
        schema_version != kSettingsSchemaVersion || size == 0 ||
        size > kMaximumDllPathBytes + 256U) {
        return false;
    }

    std::string document(size, '\0');
    std::size_t copied = document.size();
    const AnomalyStatusV1 read_status = context.config->read(
        context.config->user, anomaly::sdk::StringView(kSettingsSchemaId), &schema_version,
        {reinterpret_cast<std::uint8_t*>(document.data()), document.size()}, &copied);
    if (read_status.code != ANOMALY_STATUS_V1_OK ||
        schema_version != kSettingsSchemaVersion || copied == 0 || copied > document.size()) {
        return false;
    }
    document.resize(copied);

    const nlohmann::json json = nlohmann::json::parse(document, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return false;
    const auto found = json.find("dllPath");
    if (found == json.end() || !found->is_string()) return false;
    const std::string path = found->get<std::string>();
    if (path.size() > kMaximumDllPathBytes) return false;

    context.persisted_dll_path = path;
    context.dll_path = path;
    SetEditor(context, path);
    return true;
}

bool SaveSettings(Context& context) {
    std::string path;
    {
        std::scoped_lock lock(context.mutex);
        if (!context.settings_dirty) return true;
        path = context.dll_path;
    }

    try {
        const std::string document = nlohmann::json{{"dllPath", path}}.dump();
        const AnomalyStatusV1 status = context.config->write_atomic(
            context.config->user, anomaly::sdk::StringView(kSettingsSchemaId),
            kSettingsSchemaVersion, Bytes(document));
        if (status.code != ANOMALY_STATUS_V1_OK) {
            std::scoped_lock lock(context.mutex);
            context.status = "Failed to save the DLL path";
            return false;
        }
        std::scoped_lock lock(context.mutex);
        context.persisted_dll_path = path;
        context.settings_dirty = false;
        return true;
    } catch (...) {
        std::scoped_lock lock(context.mutex);
        context.status = "Failed to save the DLL path";
        return false;
    }
}

void LoadConfiguredLibrary(Context& context) noexcept {
    std::string configured_path;
    {
        std::scoped_lock lock(context.mutex);
        if (context.loaded_module != nullptr) return;
        configured_path = context.dll_path;
    }
    if (configured_path.empty()) {
        std::scoped_lock lock(context.mutex);
        context.status = "DLL loading is disabled";
        return;
    }

    try {
        std::filesystem::path path;
        std::string error;
        if (!ResolveLibraryPath(configured_path, path, error)) {
            std::scoped_lock lock(context.mutex);
            context.status = std::move(error);
            return;
        }
        const HMODULE module = LoadLibraryExW(
            path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module == nullptr) {
            const DWORD load_error = GetLastError();
            std::scoped_lock lock(context.mutex);
            context.status =
                "LoadLibraryExW failed (Win32 error " + std::to_string(load_error) + ')';
            return;
        }
        const std::string display_path = WideToUtf8(path.native());
        std::scoped_lock lock(context.mutex);
        context.loaded_module = module;
        context.loaded_path = display_path.empty() ? configured_path : display_path;
        context.status = "DLL loaded";
    } catch (...) {
        std::scoped_lock lock(context.mutex);
        context.status = "DLL loading failed";
    }
}

void UnloadConfiguredLibrary(Context& context) noexcept {
    HMODULE module{};
    {
        std::scoped_lock lock(context.mutex);
        module = std::exchange(context.loaded_module, nullptr);
        context.loaded_path.clear();
    }
    if (module == nullptr) return;
    const BOOL released = FreeLibrary(module);
    std::scoped_lock lock(context.mutex);
    context.status = released != FALSE ? "DLL unloaded" : "FreeLibrary failed";
}

AnomalyStatusV1 ANOMALY_CALL Load(
    const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host and context are required");
    }
    *plugin_context = nullptr;
    try {
        auto* context = new (std::nothrow) Context();
        if (context == nullptr) {
            return Status(ANOMALY_STATUS_V1_FAILED, "context allocation failed");
        }
        const anomaly::sdk::Host host_view(host);
        context->config = host_view.Query<AnomalyConfigServiceV1>(
            ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION).get();
        context->ui = host_view.Query<AnomalyUiServiceV1>(
            ANOMALY_UI_SERVICE_V1_ID, ANOMALY_UI_SERVICE_V1_VERSION).get();
        if (!ConfigReady(context->config) || context->ui == nullptr) {
            delete context;
            return Status(
                ANOMALY_STATUS_V1_UNAVAILABLE, "required plugin services are unavailable");
        }
        const AnomalyStatusV1 schema_status = context->config->register_schema(
            context->config->user, anomaly::sdk::StringView(kSettingsSchemaId),
            kSettingsSchemaVersion, Bytes(kSettingsSchema), &context->settings_schema);
        if (schema_status.code != ANOMALY_STATUS_V1_OK ||
            context->settings_schema.id == 0 || !ReadSettings(*context)) {
            delete context;
            return Status(ANOMALY_STATUS_V1_FAILED, "DLL loader settings are invalid");
        }
        *plugin_context = context;
        return anomaly::sdk::Ok();
    } catch (...) {
        return Status(ANOMALY_STATUS_V1_FAILED, "DLL loader initialization failed");
    }
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is required");
    }
    LoadConfiguredLibrary(*context);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "context is required");
    }
    {
        std::scoped_lock lock(context->mutex);
        if (context->stopped) return anomaly::sdk::Ok();
        context->stopped = true;
    }
    const bool saved = SaveSettings(*context);
    UnloadConfiguredLibrary(*context);
    return saved ? anomaly::sdk::Ok()
                 : Status(
                       ANOMALY_STATUS_V1_FAILED,
                       "DLL loader settings could not be saved");
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    UnloadConfiguredLibrary(*context);
    delete context;
}

void ANOMALY_CALL Draw(
    void* plugin_context, const AnomalyUiServiceV1* supplied_ui) {
    auto* const context = static_cast<Context*>(plugin_context);
    const AnomalyUiServiceV1* const ui = supplied_ui != nullptr
        ? supplied_ui
        : context == nullptr ? nullptr : context->ui;
    if (context == nullptr || !HasUiWindow(ui)) return;
    try {
        int open = 1;
        anomaly::sdk::UiWindow window(ui, "DLL Loader", &open);
        if (!window) return;
        ui->text(ui->user, anomaly::sdk::StringView(
            "DLL path (relative paths use this plugin package)"));
        if (!HasUiInput(ui)) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "Text input is unavailable in this host."));
            return;
        }
        if (ui->input_text(
                ui->user, anomaly::sdk::StringView("DLL##path"),
                context->editor.data(), context->editor.size(),
                ANOMALY_UI_TEXT_INPUT_V1_NONE) != 0) {
            std::scoped_lock lock(context->mutex);
            context->dll_path = context->editor.data();
            context->settings_dirty =
                context->dll_path != context->persisted_dll_path;
            context->status = context->settings_dirty
                ? "Configuration changed; reload this plugin to apply it"
                : "Configuration matches the saved path";
        }
        if (ui->button(
                ui->user, anomaly::sdk::StringView("Discard changes"),
                0.0F, 0.0F) != 0) {
            std::scoped_lock lock(context->mutex);
            context->dll_path = context->persisted_dll_path;
            SetEditor(*context, context->dll_path);
            context->settings_dirty = false;
            context->status = "Restored the saved DLL path";
        }

        std::string status;
        std::string loaded_path;
        bool dirty{};
        {
            std::scoped_lock lock(context->mutex);
            status = context->status;
            loaded_path = context->loaded_path;
            dirty = context->settings_dirty;
        }
        ui->text(ui->user, anomaly::sdk::StringView("Status: " + status));
        if (!loaded_path.empty()) {
            ui->text(ui->user, anomaly::sdk::StringView("Loaded: " + loaded_path));
        }
        if (dirty) {
            ui->text(ui->user, anomaly::sdk::StringView(
                "Reload this plugin from Plugins to save the path and load the new DLL."));
        } else {
            ui->text(ui->user, anomaly::sdk::StringView(
                "Disable or reload this plugin to release the DLL handle."));
        }
    } catch (...) {
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_plugin_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(
            ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin descriptor is invalid");
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR,
        ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("anomaly.builtin.dll-loader"),
        anomaly::sdk::StringView("DLL Loader"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, nullptr, Draw};
    return anomaly::sdk::Ok();
}
