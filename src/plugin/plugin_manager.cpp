#include "plugin_manager.hpp"

#include "json.hpp"
#include "anomaly/adapter_service_registry.hpp"
#include "anomaly/i18n.hpp"
#include "anomaly/plugin_capability_policy.hpp"
#include "anomaly/plugin_dependency_resolver.hpp"
#include "anomaly/plugin_native_dependency.hpp"
#include "anomaly/plugin_package.hpp"
#include "anomaly/scoped_platform_services.hpp"
#include "anomaly/structured_logger.hpp"
#include "anomaly/thread_local_value.hpp"
#include "anomaly/ui_resource_decoder.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <malloc.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ue5mem {

struct PluginCacheOwnerLease final {
    explicit PluginCacheOwnerLease(HANDLE value) noexcept : value(value) {}
    ~PluginCacheOwnerLease() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }

    PluginCacheOwnerLease(const PluginCacheOwnerLease&) = delete;
    PluginCacheOwnerLease& operator=(const PluginCacheOwnerLease&) = delete;

    HANDLE value{INVALID_HANDLE_VALUE};
};

namespace {

PluginManager* g_manager{};
bool g_process_quarantined{};
anomaly::ThreadLocalObject<std::string> g_loading_plugin_id;
anomaly::ThreadLocalObject<std::filesystem::path> g_loading_package_directory;
anomaly::ThreadLocalScalar<anomaly::LogThreadDomain> g_log_thread_domain;
anomaly::ThreadLocalScalar<anomaly::PluginScope*> g_callback_scope;
anomaly::ThreadLocalScalar<std::uint64_t> g_callback_generation;
anomaly::ThreadLocalScalar<bool> g_lifecycle_callback;

constexpr std::wstring_view kPluginCacheOwnerFile{L".owner.lock"};

std::optional<DWORD> CacheDirectoryProcessId(const std::filesystem::path& directory) noexcept {
    const std::string name = directory.filename().string();
    if (name.empty()) return std::nullopt;
    std::uint32_t value{};
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || value == 0) {
        return std::nullopt;
    }
    return static_cast<DWORD>(value);
}

bool CacheDirectoryHasOwner(const std::filesystem::path& directory) noexcept {
    const std::filesystem::path owner_file = directory / kPluginCacheOwnerFile;
    const HANDLE owner = CreateFileW(
        owner_file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (owner != INVALID_HANDLE_VALUE) {
        CloseHandle(owner);
        return false;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) return true;
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
    return true;
}

void PruneStalePluginCaches(const std::filesystem::path& cache_root) noexcept {
    std::vector<std::filesystem::path> stale;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(cache_root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto process_id = CacheDirectoryProcessId(iterator->path());
        if (!process_id || !iterator->is_directory(error)) {
            error.clear();
            continue;
        }
        if (!CacheDirectoryHasOwner(iterator->path())) {
            stale.push_back(iterator->path());
        }
    }
    for (const std::filesystem::path& directory : stale) {
        error.clear();
        std::filesystem::remove_all(directory, error);
    }
}

std::unique_ptr<PluginCacheOwnerLease> AcquirePluginCacheOwner(
    const std::filesystem::path& cache_root,
    const std::filesystem::path& process_cache) {
    PruneStalePluginCaches(cache_root);
    std::error_code error;
    std::filesystem::remove_all(process_cache, error);
    error.clear();
    std::filesystem::create_directories(process_cache, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "plugin cache directory could not be created", process_cache, error);
    }
    const std::filesystem::path owner_file = process_cache / kPluginCacheOwnerFile;
    const HANDLE owner = CreateFileW(
        owner_file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (owner == INVALID_HANDLE_VALUE) {
        const std::error_code owner_error(
            static_cast<int>(GetLastError()), std::system_category());
        std::filesystem::remove_all(process_cache, error);
        throw std::filesystem::filesystem_error(
            "plugin cache ownership could not be acquired", owner_file, owner_error);
    }
    return std::make_unique<PluginCacheOwnerLease>(owner);
}

void RemoveEmptyPluginCacheRoot(const std::filesystem::path& cache_root) noexcept {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(cache_root, error));
}

class ScopedLogThreadDomain final {
public:
    explicit ScopedLogThreadDomain(anomaly::LogThreadDomain domain) noexcept
        : previous_(g_log_thread_domain.Get()) {
        g_log_thread_domain.Set(domain);
    }

    ~ScopedLogThreadDomain() { g_log_thread_domain.Set(previous_); }

private:
    anomaly::LogThreadDomain previous_;
};

class ScopedPluginCallback final {
public:
    ScopedPluginCallback(
        std::shared_ptr<anomaly::PluginScope> scope,
        std::uint64_t generation,
        bool lifecycle) noexcept
        : scope_(std::move(scope)),
          previous_scope_(g_callback_scope.Get()),
          previous_generation_(g_callback_generation.Get()),
          previous_lifecycle_(g_lifecycle_callback.Get()) {
        g_callback_scope.Set(scope_.get());
        g_callback_generation.Set(generation);
        g_lifecycle_callback.Set(lifecycle);
    }

    ~ScopedPluginCallback() {
        g_callback_scope.Set(previous_scope_);
        g_callback_generation.Set(previous_generation_);
        g_lifecycle_callback.Set(previous_lifecycle_);
    }

    ScopedPluginCallback(const ScopedPluginCallback&) = delete;
    ScopedPluginCallback& operator=(const ScopedPluginCallback&) = delete;

private:
    std::shared_ptr<anomaly::PluginScope> scope_;
    anomaly::PluginScope* previous_scope_{};
    std::uint64_t previous_generation_{};
    bool previous_lifecycle_{};
};

anomaly::PluginScope::CallbackLease AcquireCurrentCallbackLease() noexcept {
    anomaly::PluginScope* scope = g_callback_scope.Get();
    if (scope == nullptr) return {};
    return g_lifecycle_callback.Get()
        ? scope->AcquireLifecycleLease(g_callback_generation.Get())
        : scope->AcquireCallback(g_callback_generation.Get());
}

std::chrono::steady_clock::time_point StopDeadlineAfter(
    std::chrono::milliseconds timeout) noexcept {
    const auto bounded = (std::max)(timeout, std::chrono::milliseconds::zero());
    if (bounded == std::chrono::milliseconds::max()) {
        return std::chrono::steady_clock::time_point::max();
    }
    const auto now = std::chrono::steady_clock::now();
    const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now);
    return bounded >= available
        ? std::chrono::steady_clock::time_point::max()
        : now + bounded;
}

std::chrono::milliseconds StopRemaining(
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::milliseconds::max();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds::zero();
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

std::wstring WideUtf8(const char* text) {
    if (text == nullptr || *text == '\0') return {};
    const int length = static_cast<int>(std::strlen(text));
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, result.data(), size);
    return result;
}

std::string Utf8(const std::filesystem::path& path) {
    const auto value = path.wstring();
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

constexpr std::uintmax_t kMaximumUiWindowStateFileBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumUiPersistentWindows = 10000U;
constexpr std::size_t kMaximumPersistentPluginWindows = 10000U;
constexpr std::size_t kMaximumPluginIdBytes = 255U;
constexpr auto kUiWindowStateSaveInterval = std::chrono::milliseconds(500);

struct PersistentUiState final {
    std::vector<anomaly::UiWindowPersistentState> windows;
    std::unordered_map<std::string, bool> plugin_windows;
};

std::string SerializeUiWindowState(
    const std::vector<anomaly::UiWindowPersistentState>& windows,
    const std::unordered_map<std::string, bool>& plugin_windows) {
    nlohmann::json document{{"schemaVersion", 1}, {"windows", nlohmann::json::array()},
        {"pluginWindows", nlohmann::json::array()}};
    for (const anomaly::UiWindowPersistentState& window : windows) {
        document["windows"].push_back({
            {"stableId", window.stable_id},
            {"open", window.open},
            {"width", window.width},
            {"height", window.height},
            {"constraints", {
                {"minimumWidth", window.constraints.minimum_width},
                {"minimumHeight", window.constraints.minimum_height},
                {"maximumWidth", window.constraints.maximum_width},
                {"maximumHeight", window.constraints.maximum_height}}}});
    }
    std::vector<std::pair<std::string, bool>> sorted_plugin_windows(
        plugin_windows.begin(), plugin_windows.end());
    std::ranges::sort(sorted_plugin_windows, {}, &std::pair<std::string, bool>::first);
    for (const auto& [plugin_id, visible] : sorted_plugin_windows) {
        document["pluginWindows"].push_back({
            {"pluginId", plugin_id},
            {"visible", visible}});
    }
    return document.dump(2) + '\n';
}

PersistentUiState ParseUiWindowState(
    const nlohmann::json& document) {
    if (!document.is_object() || document.value("schemaVersion", 0U) != 1U ||
        !document.contains("windows") || !document["windows"].is_array() ||
        document["windows"].size() > kMaximumUiPersistentWindows) {
        throw std::runtime_error("UI window state document is invalid");
    }

    PersistentUiState result;
    result.windows.reserve(document["windows"].size());
    for (const nlohmann::json& item : document["windows"]) {
        if (!item.is_object() || !item.contains("constraints") ||
            !item["constraints"].is_object()) {
            throw std::runtime_error("UI window state record is invalid");
        }
        const nlohmann::json& constraints = item["constraints"];
        anomaly::UiWindowPersistentState state;
        state.stable_id = item.at("stableId").get<std::string>();
        state.open = item.at("open").get<bool>();
        state.width = item.at("width").get<float>();
        state.height = item.at("height").get<float>();
        state.constraints = {
            constraints.at("minimumWidth").get<float>(),
            constraints.at("minimumHeight").get<float>(),
            constraints.at("maximumWidth").get<float>(),
            constraints.at("maximumHeight").get<float>()};
        result.windows.push_back(std::move(state));
    }

    if (!document.contains("pluginWindows")) return result;
    const nlohmann::json& plugin_windows = document["pluginWindows"];
    if (!plugin_windows.is_array() ||
        plugin_windows.size() > kMaximumPersistentPluginWindows) {
        throw std::runtime_error("plugin window state collection is invalid");
    }
    result.plugin_windows.reserve(plugin_windows.size());
    for (const nlohmann::json& item : plugin_windows) {
        if (!item.is_object()) {
            throw std::runtime_error("plugin window state record is invalid");
        }
        std::string plugin_id = item.at("pluginId").get<std::string>();
        const bool visible = item.at("visible").get<bool>();
        if (plugin_id.empty() || plugin_id.size() > kMaximumPluginIdBytes ||
            !result.plugin_windows.emplace(std::move(plugin_id), visible).second) {
            throw std::runtime_error("plugin window state value is invalid");
        }
    }
    return result;
}

struct CallbackMetrics {
    std::uint64_t calls{};
    std::uint64_t faults{};
    std::uint64_t slow_calls{};
    std::deque<double> milliseconds;

    void Record(double elapsed, bool fault, double slow_threshold) {
        ++calls;
        if (fault) ++faults;
        if (elapsed >= slow_threshold) ++slow_calls;
        milliseconds.push_back(elapsed);
        if (milliseconds.size() > 512) milliseconds.pop_front();
    }

    [[nodiscard]] CallbackMetricsView View() const {
        CallbackMetricsView result{calls, faults, slow_calls};
        if (milliseconds.empty()) return result;
        std::vector<double> sorted(milliseconds.begin(), milliseconds.end());
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](double value) {
            const auto index = static_cast<std::size_t>(value * static_cast<double>(sorted.size() - 1));
            return sorted[index];
        };
        result.p50_milliseconds = percentile(0.50);
        result.p95_milliseconds = percentile(0.95);
        result.p99_milliseconds = percentile(0.99);
        return result;
    }
};

enum class UiStackEntryKind : std::uint8_t {
    Window,
    ScopedWindow,
    Child,
    Table,
    Menu,
    Popup,
    Font,
};

struct UiStackEntry final {
    UiStackEntryKind kind{UiStackEntryKind::Window};
    anomaly::UiResourceHandle resource{};
};

struct UiStackTracker final {
    std::vector<UiStackEntry> entries;
    bool mismatch{};

    [[nodiscard]] bool ReserveNext() noexcept {
        if (entries.size() == entries.max_size()) return false;
        try {
            entries.reserve(entries.size() + 1U);
        } catch (...) {
            return false;
        }
        return true;
    }

    void PushReserved(const UiStackEntry entry) noexcept { entries.push_back(entry); }

    [[nodiscard]] bool HasTop(
        const UiStackEntryKind kind,
        const anomaly::UiResourceHandle resource = {}) const noexcept {
        return !entries.empty() && entries.back().kind == kind &&
            (resource.id == 0 || entries.back().resource == resource);
    }

    [[nodiscard]] bool Consume(
        const UiStackEntryKind kind,
        const anomaly::UiResourceHandle resource = {}) noexcept {
        if (!HasTop(kind, resource)) {
            mismatch = true;
            return false;
        }
        entries.pop_back();
        return true;
    }

    void MarkMismatch() noexcept { mismatch = true; }

    void Reset() noexcept {
        entries.clear();
        mismatch = false;
    }
};

struct PluginUiProxyContext {
    const AnomalyUiServiceV1* service{};
    UiStackTracker* ui_stack{};
    std::shared_ptr<anomaly::PluginScope> scope;
    std::string plugin_id;
    std::uint64_t generation{};
    bool close_requested{};
    bool reopen_requested{};
    UiStackEntryKind window_begin_kind{UiStackEntryKind::Window};
    UiStackEntryKind window_end_kind{UiStackEntryKind::Window};
    anomaly::UiResourceHandle window_resource{};
};

void AppendHexIdentifier(std::string& destination, const std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    destination.reserve(destination.size() + value.size() * 2U);
    for (const unsigned char byte : value) {
        destination.push_back(digits[byte >> 4U]);
        destination.push_back(digits[byte & 0x0fU]);
    }
}

std::string NamespacedPluginWindowTitle(
    const PluginUiProxyContext& context, const AnomalyStringViewV1 title) {
    const std::string_view raw = title.data == nullptr
        ? std::string_view{}
        : std::string_view(title.data, title.size);
    if (context.plugin_id.empty()) return std::string(raw);

    const std::size_t marker = raw.find("###");
    const std::string_view visible = marker == std::string_view::npos
        ? raw : raw.substr(0U, marker);
    const std::string_view local_id = marker == std::string_view::npos
        ? raw : raw.substr(marker + 3U);
    std::string result(visible);
    result += "###anomaly-plugin:";
    AppendHexIdentifier(result, context.plugin_id);
    result.push_back(':');
    AppendHexIdentifier(result, local_id);
    return result;
}

class ScopedUiProxyWindowKind final {
public:
    ScopedUiProxyWindowKind(
        PluginUiProxyContext& context, const UiStackEntryKind kind,
        const anomaly::UiResourceHandle resource = {}) noexcept
        : context_(context),
          previous_begin_kind_(context.window_begin_kind),
          previous_end_kind_(context.window_end_kind),
          previous_resource_(context.window_resource) {
        context_.window_begin_kind = kind;
        context_.window_end_kind = kind;
        context_.window_resource = resource;
    }

    ~ScopedUiProxyWindowKind() {
        context_.window_begin_kind = previous_begin_kind_;
        context_.window_end_kind = previous_end_kind_;
        context_.window_resource = previous_resource_;
    }

    ScopedUiProxyWindowKind(const ScopedUiProxyWindowKind&) = delete;
    ScopedUiProxyWindowKind& operator=(const ScopedUiProxyWindowKind&) = delete;

private:
    PluginUiProxyContext& context_;
    UiStackEntryKind previous_begin_kind_;
    UiStackEntryKind previous_end_kind_;
    anomaly::UiResourceHandle previous_resource_;
};

PluginUiProxyContext* UiProxyContext(void* user) noexcept {
    return static_cast<PluginUiProxyContext*>(user);
}

anomaly::PluginScope::CallbackLease AcquireUiCallback(
    const PluginUiProxyContext* context) noexcept {
    if (context == nullptr || context->scope == nullptr ||
        g_callback_scope.Get() != context->scope.get() ||
        g_callback_generation.Get() != context->generation ||
        g_lifecycle_callback.Get() ||
        g_log_thread_domain.Get() != anomaly::LogThreadDomain::Render) {
        return {};
    }
    return context->scope->AcquireCallback(context->generation);
}

anomaly::PluginScope::CallbackLease AcquireUiStateCallback(
    const PluginUiProxyContext* context) noexcept {
    if (context == nullptr || context->scope == nullptr ||
        g_callback_scope.Get() != context->scope.get() ||
        g_callback_generation.Get() != context->generation ||
        g_lifecycle_callback.Get() ||
        (g_log_thread_domain.Get() != anomaly::LogThreadDomain::Render &&
         g_log_thread_domain.Get() != anomaly::LogThreadDomain::Game)) {
        return {};
    }
    return context->scope->AcquireCallback(context->generation);
}

template <typename Field>
bool HasUiField(const AnomalyUiServiceV1* service, std::size_t offset) noexcept {
    return service != nullptr && service->struct_size >= offset + sizeof(Field);
}

bool ReserveUiStackEntry(PluginUiProxyContext* context) noexcept {
    if (context == nullptr || context->ui_stack == nullptr) return false;
    if (context->ui_stack->ReserveNext()) return true;
    context->ui_stack->MarkMismatch();
    return false;
}

bool ConsumeUiStackEntry(
    PluginUiProxyContext* context, const UiStackEntryKind kind) noexcept {
    return context != nullptr && context->ui_stack != nullptr &&
        context->ui_stack->Consume(kind);
}

void ANOMALY_CALL ProxySetNextWindowSize(
    void* user, float width, float height, std::uint32_t condition) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr && context->service != nullptr &&
        context->service->set_next_window_size != nullptr) {
        context->service->set_next_window_size(
            context->service->user, width, height, condition);
    }
}

int ANOMALY_CALL ProxyBeginWindow(
    void* user, AnomalyStringViewV1 title, int* open, std::uint32_t flags) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr || context->service == nullptr ||
        context->service->begin_window == nullptr || context->service->end_window == nullptr ||
        context->ui_stack == nullptr) {
        return 0;
    }
    if (!context->ui_stack->ReserveNext()) {
        context->ui_stack->MarkMismatch();
        return 0;
    }
    if (open != nullptr && context->reopen_requested) {
        *open = 1;
        context->reopen_requested = false;
    }
    const std::string scoped_title = NamespacedPluginWindowTitle(*context, title);
    const int result = context->service->begin_window(
        context->service->user,
        {scoped_title.data(), scoped_title.size()}, open, flags);
    context->ui_stack->PushReserved({context->window_begin_kind, context->window_resource});
    if (open != nullptr && *open == 0 &&
        context->window_begin_kind != UiStackEntryKind::ScopedWindow) {
        context->close_requested = true;
    }
    return result;
}

void ANOMALY_CALL ProxyEndWindow(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr && context->service != nullptr &&
        context->service->end_window != nullptr && context->ui_stack != nullptr &&
        context->ui_stack->Consume(context->window_end_kind, context->window_resource)) {
        context->service->end_window(context->service->user);
    }
}

void ANOMALY_CALL ProxyText(void* user, AnomalyStringViewV1 text) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr && context->service != nullptr && context->service->text != nullptr) {
        context->service->text(context->service->user, text);
    }
}

int ANOMALY_CALL ProxyButton(
    void* user, AnomalyStringViewV1 label, float width, float height) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr && context->service != nullptr &&
            context->service->button != nullptr
        ? context->service->button(context->service->user, label, width, height)
        : 0;
}

int ANOMALY_CALL ProxyDrawEntityBbox(
    void* user, const AnomalyEspCameraV1* camera,
    const AnomalyEspEntityBoundsV1* bounds, const AnomalyEspBoxStyleV1* style) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr && context->service != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::draw_entity_bbox)>(
                context->service, offsetof(AnomalyUiServiceV1, draw_entity_bbox)) &&
            context->service->draw_entity_bbox != nullptr
        ? context->service->draw_entity_bbox(context->service->user, camera, bounds, style)
        : 0;
}

int ANOMALY_CALL ProxyCheckbox(void* user, AnomalyStringViewV1 label, int* value) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::checkbox)>(
                context->service, offsetof(AnomalyUiServiceV1, checkbox)) &&
            context->service->checkbox != nullptr
        ? context->service->checkbox(context->service->user, label, value) : 0;
}

int ANOMALY_CALL ProxySliderFloat(
    void* user, AnomalyStringViewV1 label, float* value, float minimum, float maximum) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::slider_float)>(
                context->service, offsetof(AnomalyUiServiceV1, slider_float)) &&
            context->service->slider_float != nullptr
        ? context->service->slider_float(context->service->user, label, value, minimum, maximum)
        : 0;
}

int ANOMALY_CALL ProxyInputUInt32(
    void* user, AnomalyStringViewV1 label, std::uint32_t* value,
    std::uint32_t step, std::uint32_t step_fast) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::input_uint32)>(
                context->service, offsetof(AnomalyUiServiceV1, input_uint32)) &&
            context->service->input_uint32 != nullptr
        ? context->service->input_uint32(
              context->service->user, label, value, step, step_fast)
        : 0;
}

int ANOMALY_CALL ProxyInputDouble(
    void* user, AnomalyStringViewV1 label, double* value, double step, double step_fast) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::input_double)>(
                context->service, offsetof(AnomalyUiServiceV1, input_double)) &&
            context->service->input_double != nullptr
        ? context->service->input_double(
              context->service->user, label, value, step, step_fast)
        : 0;
}

int ANOMALY_CALL ProxyDeveloperModeEnabled(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiStateCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::developer_mode_enabled)>(
                context->service, offsetof(AnomalyUiServiceV1, developer_mode_enabled)) &&
            context->service->developer_mode_enabled != nullptr
        ? context->service->developer_mode_enabled(context->service->user)
        : 0;
}

int ANOMALY_CALL ProxyInputText(
    void* user, AnomalyStringViewV1 label, char* buffer,
    std::size_t buffer_capacity, std::uint32_t flags) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::input_text)>(
                context->service, offsetof(AnomalyUiServiceV1, input_text)) &&
            context->service->input_text != nullptr
        ? context->service->input_text(
              context->service->user, label, buffer, buffer_capacity, flags)
        : 0;
}

int ANOMALY_CALL ProxyButtonEnabled(
    void* user, AnomalyStringViewV1 label, float width, float height, int enabled) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::button_enabled)>(
                context->service, offsetof(AnomalyUiServiceV1, button_enabled)) &&
            context->service->button_enabled != nullptr
        ? context->service->button_enabled(
              context->service->user, label, width, height, enabled)
        : 0;
}

int ANOMALY_CALL ProxyColorEdit4(
    void* user, AnomalyStringViewV1 label, float rgba[4]) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::color_edit4)>(
                context->service, offsetof(AnomalyUiServiceV1, color_edit4)) &&
            context->service->color_edit4 != nullptr
        ? context->service->color_edit4(context->service->user, label, rgba) : 0;
}

int ANOMALY_CALL ProxyDrawEntityBox3d(
    void* user, const AnomalyEspCameraV1* camera,
    const AnomalyEspEntityBoundsV1* bounds, const AnomalyEspBoxStyleV1* style) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::draw_entity_box3d)>(
                context->service, offsetof(AnomalyUiServiceV1, draw_entity_box3d)) &&
            context->service->draw_entity_box3d != nullptr
        ? context->service->draw_entity_box3d(context->service->user, camera, bounds, style)
        : 0;
}

int ANOMALY_CALL ProxyDrawEntityLabel(
    void* user, const AnomalyEspCameraV1* camera,
    const AnomalyEspEntityBoundsV1* bounds, AnomalyStringViewV1 text,
    std::uint32_t color) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::draw_entity_label)>(
                context->service, offsetof(AnomalyUiServiceV1, draw_entity_label)) &&
            context->service->draw_entity_label != nullptr
        ? context->service->draw_entity_label(
              context->service->user, camera, bounds, text, color)
        : 0;
}

void ANOMALY_CALL ProxySeparator(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::separator)>(
            context->service, offsetof(AnomalyUiServiceV1, separator)) &&
        context->service->separator != nullptr) {
        context->service->separator(context->service->user);
    }
}

int ANOMALY_CALL ProxyBeginChild(
    void* user, AnomalyStringViewV1 id, float width, float height, std::uint32_t flags) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(AnomalyUiServiceV1::begin_child)>(
            context->service, offsetof(AnomalyUiServiceV1, begin_child)) ||
        !HasUiField<decltype(AnomalyUiServiceV1::end_child)>(
            context->service, offsetof(AnomalyUiServiceV1, end_child)) ||
        context->service->begin_child == nullptr || context->service->end_child == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_child(
        context->service->user, id, width, height, flags);
    // ImGui child regions require EndChild even when BeginChild returns false.
    context->ui_stack->PushReserved({UiStackEntryKind::Child});
    return result;
}

void ANOMALY_CALL ProxyEndChild(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::end_child)>(
            context->service, offsetof(AnomalyUiServiceV1, end_child)) &&
        context->service->end_child != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::Child)) {
        context->service->end_child(context->service->user);
    }
}

int ANOMALY_CALL ProxyBeginTable(
    void* user, AnomalyStringViewV1 id, std::int32_t columns,
    std::uint32_t flags, float width, float height) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(AnomalyUiServiceV1::begin_table)>(
            context->service, offsetof(AnomalyUiServiceV1, begin_table)) ||
        !HasUiField<decltype(AnomalyUiServiceV1::end_table)>(
            context->service, offsetof(AnomalyUiServiceV1, end_table)) ||
        context->service->begin_table == nullptr || context->service->end_table == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_table(
        context->service->user, id, columns, flags, width, height);
    if (result != 0) context->ui_stack->PushReserved({UiStackEntryKind::Table});
    return result;
}

void ANOMALY_CALL ProxyTableNextRow(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::table_next_row)>(
            context->service, offsetof(AnomalyUiServiceV1, table_next_row)) &&
        context->service->table_next_row != nullptr) {
        context->service->table_next_row(context->service->user);
    }
}

int ANOMALY_CALL ProxyTableNextColumn(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::table_next_column)>(
                context->service, offsetof(AnomalyUiServiceV1, table_next_column)) &&
            context->service->table_next_column != nullptr
        ? context->service->table_next_column(context->service->user)
        : 0;
}

void ANOMALY_CALL ProxyEndTable(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::end_table)>(
            context->service, offsetof(AnomalyUiServiceV1, end_table)) &&
        context->service->end_table != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::Table)) {
        context->service->end_table(context->service->user);
    }
}

int ANOMALY_CALL ProxyBeginMenu(void* user, AnomalyStringViewV1 label, int enabled) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(AnomalyUiServiceV1::begin_menu)>(
            context->service, offsetof(AnomalyUiServiceV1, begin_menu)) ||
        !HasUiField<decltype(AnomalyUiServiceV1::end_menu)>(
            context->service, offsetof(AnomalyUiServiceV1, end_menu)) ||
        context->service->begin_menu == nullptr || context->service->end_menu == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_menu(
        context->service->user, label, enabled);
    if (result != 0) context->ui_stack->PushReserved({UiStackEntryKind::Menu});
    return result;
}

void ANOMALY_CALL ProxyEndMenu(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::end_menu)>(
            context->service, offsetof(AnomalyUiServiceV1, end_menu)) &&
        context->service->end_menu != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::Menu)) {
        context->service->end_menu(context->service->user);
    }
}

void ANOMALY_CALL ProxyOpenPopup(void* user, AnomalyStringViewV1 id) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::open_popup)>(
            context->service, offsetof(AnomalyUiServiceV1, open_popup)) &&
        context->service->open_popup != nullptr) {
        context->service->open_popup(context->service->user, id);
    }
}

int ANOMALY_CALL ProxyBeginPopupModal(
    void* user, AnomalyStringViewV1 id, int* open, std::uint32_t flags) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    if (context == nullptr ||
        !HasUiField<decltype(AnomalyUiServiceV1::begin_popup_modal)>(
            context->service, offsetof(AnomalyUiServiceV1, begin_popup_modal)) ||
        !HasUiField<decltype(AnomalyUiServiceV1::end_popup)>(
            context->service, offsetof(AnomalyUiServiceV1, end_popup)) ||
        context->service->begin_popup_modal == nullptr || context->service->end_popup == nullptr ||
        !ReserveUiStackEntry(context)) {
        return 0;
    }
    const int result = context->service->begin_popup_modal(
        context->service->user, id, open, flags);
    if (result != 0) context->ui_stack->PushReserved({UiStackEntryKind::Popup});
    return result;
}

void ANOMALY_CALL ProxyEndPopup(void* user) {
    auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::end_popup)>(
            context->service, offsetof(AnomalyUiServiceV1, end_popup)) &&
        context->service->end_popup != nullptr &&
        ConsumeUiStackEntry(context, UiStackEntryKind::Popup)) {
        context->service->end_popup(context->service->user);
    }
}

void ANOMALY_CALL ProxyCloseCurrentPopup(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::close_current_popup)>(
            context->service, offsetof(AnomalyUiServiceV1, close_current_popup)) &&
        context->service->close_current_popup != nullptr) {
        context->service->close_current_popup(context->service->user);
    }
}

int ANOMALY_CALL ProxyFilterMatch(
    void* user, AnomalyStringViewV1 filter, AnomalyStringViewV1 value) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::filter_match)>(
                context->service, offsetof(AnomalyUiServiceV1, filter_match)) &&
            context->service->filter_match != nullptr
        ? context->service->filter_match(context->service->user, filter, value)
        : 0;
}

std::uint32_t ANOMALY_CALL ProxyFrameState(void* user) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return 0;
    return context != nullptr &&
            HasUiField<decltype(AnomalyUiServiceV1::frame_state)>(
                context->service, offsetof(AnomalyUiServiceV1, frame_state)) &&
            context->service->frame_state != nullptr
        ? context->service->frame_state(context->service->user)
        : 0;
}

void ANOMALY_CALL ProxySetNextWindowSizeConstraints(
    void* user, const float minimum_width, const float minimum_height,
    const float maximum_width, const float maximum_height) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::set_next_window_size_constraints)>(
            context->service,
            offsetof(AnomalyUiServiceV1, set_next_window_size_constraints)) &&
        context->service->set_next_window_size_constraints != nullptr) {
        context->service->set_next_window_size_constraints(
            context->service->user, minimum_width, minimum_height,
            maximum_width, maximum_height);
    }
}

void ANOMALY_CALL ProxyGetWindowSize(void* user, float* width, float* height) {
    const auto* context = UiProxyContext(user);
    auto callback = AcquireUiCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr &&
        HasUiField<decltype(AnomalyUiServiceV1::get_window_size)>(
            context->service, offsetof(AnomalyUiServiceV1, get_window_size)) &&
        context->service->get_window_size != nullptr) {
        context->service->get_window_size(context->service->user, width, height);
    }
}

AnomalyUiServiceV1 MakeUiProxy(PluginUiProxyContext* context) noexcept {
    return {
        sizeof(AnomalyUiServiceV1), ANOMALY_UI_SERVICE_V1_VERSION,
        context,
        ProxySetNextWindowSize, ProxyBeginWindow, ProxyEndWindow, ProxyText, ProxyButton,
        ProxyDrawEntityBbox, ProxyCheckbox, ProxySliderFloat, ProxyColorEdit4,
        ProxyDrawEntityBox3d, ProxyDrawEntityLabel,
        ProxySeparator, ProxyBeginChild, ProxyEndChild, ProxyBeginTable,
        ProxyTableNextRow, ProxyTableNextColumn, ProxyEndTable, ProxyBeginMenu,
        ProxyEndMenu, ProxyOpenPopup, ProxyBeginPopupModal, ProxyEndPopup,
        ProxyCloseCurrentPopup, ProxyFilterMatch, ProxyFrameState,
        ProxySetNextWindowSizeConstraints, ProxyGetWindowSize, ProxyInputUInt32, ProxyInputDouble,
        ProxyDeveloperModeEnabled, ProxyInputText, ProxyButtonEnabled};
}

const char* LevelName(AnomalyCoreLogLevelV1 level) {
    switch (level) {
    case ANOMALY_CORE_LOG_LEVEL_V1_TRACE: return "trace";
    case ANOMALY_CORE_LOG_LEVEL_V1_INFO: return "info";
    case ANOMALY_CORE_LOG_LEVEL_V1_WARNING: return "warning";
    case ANOMALY_CORE_LOG_LEVEL_V1_ERROR: return "error";
    default: return "unknown";
    }
}

anomaly::LogLevel StructuredLevel(AnomalyCoreLogLevelV1 level) noexcept {
    switch (level) {
    case ANOMALY_CORE_LOG_LEVEL_V1_TRACE: return anomaly::LogLevel::Trace;
    case ANOMALY_CORE_LOG_LEVEL_V1_INFO: return anomaly::LogLevel::Info;
    case ANOMALY_CORE_LOG_LEVEL_V1_WARNING: return anomaly::LogLevel::Warning;
    case ANOMALY_CORE_LOG_LEVEL_V1_ERROR: return anomaly::LogLevel::Error;
    default: return anomaly::LogLevel::Info;
    }
}

void HostLog(AnomalyCoreLogLevelV1 level, const char* message) {
    auto callback = AcquireCurrentCallbackLease();
    if (g_callback_scope.Get() != nullptr && !callback) return;
    if (g_manager != nullptr) g_manager->Log(level, message == nullptr ? "" : message);
}

const anomaly::CoreMemoryServices* HostMemoryServices() noexcept {
    return g_manager == nullptr ? nullptr : &g_manager->MemoryServices();
}

bool ReadHostMemory(
    const std::uintptr_t address, void* const destination, const std::size_t size) {
    const auto* services = HostMemoryServices();
    return services != nullptr && services->memory != nullptr &&
        services->memory->ReadMemoryInto(address, destination, size);
}

bool WriteHostMemory(
    const std::uintptr_t address, const void* const source, const std::size_t size) {
    const auto* services = HostMemoryServices();
    return services != nullptr && services->memory != nullptr &&
        services->memory->WriteMemory(address, source, size);
}

AnomalyStatusV1 StatusV1(std::uint32_t code, const char* message = nullptr) {
    return {code, 0, {message, message == nullptr ? 0 : std::strlen(message)}};
}

bool ValidPluginStateId(const std::string_view id) noexcept {
    if (id.size() < 3 || id.size() > 255) return false;
    bool saw_separator{};
    std::size_t segment_size{};
    bool previous_hyphen{};
    for (const char character : id) {
        if (character == '.') {
            if (segment_size == 0 || previous_hyphen) return false;
            saw_separator = true;
            segment_size = 0;
            previous_hyphen = false;
            continue;
        }
        const bool alpha_numeric =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
        if (!alpha_numeric && character != '-') return false;
        if (segment_size == 0 && character == '-') return false;
        if (++segment_size > 63) return false;
        previous_hyphen = character == '-';
    }
    return saw_separator && segment_size != 0 && !previous_hyphen;
}

struct PluginServiceContext {
    PluginManager* manager{};
    PluginUiProxyContext* ui_proxy_context{};
    UiStackTracker* ui_stack{};
    std::string plugin_id;
    std::uint64_t generation{};
    std::filesystem::path package_directory;
    std::filesystem::path state_directory;
    std::filesystem::path configuration_directory;
    std::shared_ptr<anomaly::PluginScope> scope;
    anomaly::PluginCapabilityGrant capabilities;
    anomaly::ScopedPlatformServices* platform{};
    anomaly::IpcRegistry* ipc{};
    std::vector<std::string> ipc_dependencies;
    anomaly::Locale localization_locale{anomaly::Locale::EnUs};
    std::shared_ptr<const anomaly::PluginCatalog> localization_catalog;
    std::atomic_bool localization_fallback_logged{};
    AnomalyCoreServiceV1 core{};
    AnomalyPluginStateServiceV1 plugin_state{};
    AnomalyConfigServiceV1 config{};
    AnomalyStorageServiceV1 storage{};
    AnomalyRuntimeInfoServiceV1 runtime_info{};
    AnomalyLocalizationServiceV1 localization{};
    AnomalyDiagnosticsServiceV1 diagnostics{};
    AnomalySchedulerServiceV1 scheduler{};
    AnomalyIpcServiceV1 ipc_service{};
    AnomalyCommandsServiceV1 commands{};
    AnomalyNotificationsServiceV1 notifications{};
    AnomalySignatureServiceV1 signature{};
    AnomalyHookServiceV1 hook{};
    AnomalyPatchServiceV1 patch{};
    const AnomalyUiServiceV1* ui{};
    anomaly::UiResourceRegistry* ui_resources{};
    anomaly::InputService* input{};
    AnomalyWindowServiceV1 window{};
    AnomalyFontServiceV1 font{};
    AnomalyTextureServiceV1 texture{};
    AnomalyInputServiceV1 input_service{};
    std::vector<anomaly::UiResourceHandle> open_windows;
    std::vector<anomaly::UiResourceHandle> pushed_fonts;
};

anomaly::PluginScope::CallbackLease AcquireServiceCallback(
    const PluginServiceContext* context) noexcept {
    if (context == nullptr || context->scope == nullptr) return {};
    const bool is_active_lifecycle_callback = g_lifecycle_callback.Get() &&
        g_callback_scope.Get() == context->scope.get() &&
        g_callback_generation.Get() == context->generation;
    return is_active_lifecycle_callback
        ? context->scope->AcquireLifecycleLease(context->generation)
        : context->scope->AcquireCallback(context->generation);
}

bool ValidServiceContext(const PluginServiceContext* context) noexcept {
    return context != nullptr && context->manager == g_manager &&
        context->scope != nullptr &&
        context->generation == context->scope->Generation() &&
        context->plugin_id == context->scope->Owner();
}

bool IsActiveDrawResourceCallback(const PluginServiceContext& context) noexcept {
    return context.scope != nullptr &&
        g_callback_scope.Get() == context.scope.get() &&
        g_callback_generation.Get() == context.generation &&
        !g_lifecycle_callback.Get() &&
        g_log_thread_domain.Get() == anomaly::LogThreadDomain::Render;
}

bool IsSynchronousStateIoForbidden() noexcept {
    return g_log_thread_domain.Get() == anomaly::LogThreadDomain::Game ||
        g_log_thread_domain.Get() == anomaly::LogThreadDomain::Render;
}

anomaly::ScopedPluginServiceOwner ScopedPlatformOwner(
    const PluginServiceContext& context) {
    return {context.scope, context.state_directory, context.configuration_directory};
}

anomaly::IpcPluginOwner IpcOwner(const PluginServiceContext& context) {
    return {context.scope, context.ipc_dependencies};
}

anomaly::IpcCallingDomain CurrentIpcDomain() noexcept {
    switch (g_log_thread_domain.Get()) {
    case anomaly::LogThreadDomain::Lifecycle: return anomaly::IpcCallingDomain::Lifecycle;
    case anomaly::LogThreadDomain::Worker: return anomaly::IpcCallingDomain::Worker;
    case anomaly::LogThreadDomain::Game: return anomaly::IpcCallingDomain::Game;
    case anomaly::LogThreadDomain::Render: return anomaly::IpcCallingDomain::Render;
    default: return anomaly::IpcCallingDomain::Unknown;
    }
}

template <typename Callback>
AnomalyStatusV1 InvokeScopedPlatform(void* user, Callback&& callback) noexcept {
    auto* context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->platform == nullptr) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try {
        return callback(*context);
    } catch (...) {
        return StatusV1(ANOMALY_STATUS_V1_FAILED, "scoped platform service failed");
    }
}

template <typename Callback>
AnomalyStatusV1 InvokeStateIoService(void* user, Callback&& callback) noexcept {
    if (IsSynchronousStateIoForbidden()) {
        return StatusV1(
            ANOMALY_STATUS_V1_UNAVAILABLE,
            "plugin state I/O is unavailable from game or render callbacks");
    }
    return InvokeScopedPlatform(user, std::forward<Callback>(callback));
}

template <typename Callback>
AnomalyStatusV1 InvokeUiResourceService(void* user, Callback&& callback) noexcept {
    auto* context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->ui_resources == nullptr) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin resource context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try {
        return callback(*context);
    } catch (...) {
        return StatusV1(ANOMALY_STATUS_V1_FAILED, "plugin resource service failed");
    }
}

std::string_view ServiceString(const AnomalyStringViewV1 value) noexcept;

bool ValidGenerationHandle(
    const PluginServiceContext& context, const AnomalyGenerationHandleV1 handle) noexcept {
    return handle.id != 0 && handle.generation == context.generation;
}

anomaly::UiResourceHandle UiHandle(const AnomalyGenerationHandleV1 handle) noexcept {
    return {handle.id};
}

AnomalyGenerationHandleV1 GenerationHandle(
    const PluginServiceContext& context, const anomaly::UiResourceHandle handle) noexcept {
    return {handle.id, context.generation};
}

constexpr std::uint32_t kHostWindowNoCollapse = 1U << 5U;
constexpr std::uint32_t kHostWindowNoSavedSettings = 1U << 8U;
constexpr std::uint32_t kHostWindowFirstUse = 4U;

std::uint32_t ToHostUiWindowFlags(const std::uint32_t flags) noexcept {
    std::uint32_t result{};
    if ((flags & ANOMALY_WINDOW_V1_NO_COLLAPSE) != 0) result |= kHostWindowNoCollapse;
    if ((flags & ANOMALY_WINDOW_V1_NO_SAVED_SETTINGS) != 0) {
        result |= kHostWindowNoSavedSettings;
    }
    return result;
}

std::uint32_t ToInputCaptureFlags(const anomaly::InputCaptureFlags flags) noexcept {
    std::uint32_t result{};
    if (anomaly::HasCaptureFlag(flags, anomaly::InputCaptureFlag::Mouse)) {
        result |= ANOMALY_INPUT_CAPTURE_V1_MOUSE;
    }
    if (anomaly::HasCaptureFlag(flags, anomaly::InputCaptureFlag::Keyboard)) {
        result |= ANOMALY_INPUT_CAPTURE_V1_KEYBOARD;
    }
    if (anomaly::HasCaptureFlag(flags, anomaly::InputCaptureFlag::Text)) {
        result |= ANOMALY_INPUT_CAPTURE_V1_TEXT;
    }
    return result;
}

void PopulateInputSnapshot(
    const anomaly::InputSnapshot& source, AnomalyInputSnapshotV1& destination) noexcept {
    destination = {};
    destination.struct_size = sizeof(destination);
    destination.modifiers = source.modifiers;
    destination.sequence = source.sequence;
    destination.timestamp_milliseconds = source.timestamp_milliseconds;
    destination.mouse_x = source.mouse_x;
    destination.mouse_y = source.mouse_y;
    destination.mouse_wheel = source.mouse_wheel_delta;
    for (std::size_t index = 1; index < source.keys.size(); ++index) {
        if (source.keys[index]) destination.keys[index / 8U] |= static_cast<std::uint8_t>(1U << (index % 8U));
    }
    for (std::size_t index{}; index < source.mouse_buttons.size(); ++index) {
        if (source.mouse_buttons[index]) {
            destination.mouse_buttons |= static_cast<std::uint8_t>(1U << index);
        }
    }
}

struct PackageResourcePath final {
    std::filesystem::path package_directory;
    std::string relative_path;
};

std::optional<PackageResourcePath> ResolvePackageResourcePath(
    const PluginServiceContext& context, const AnomalyStringViewV1 value) {
    const std::string_view raw = ServiceString(value);
    if (raw.empty() || context.package_directory.empty()) return std::nullopt;
    const anomaly::PluginPackagePathResult relative =
        anomaly::ValidatePluginPackageRelativePath(raw, false);
    if (!relative.Ok()) return std::nullopt;
    return PackageResourcePath{context.package_directory, Utf8(relative.path)};
}

class ScopedFileHandle final {
public:
    explicit ScopedFileHandle(const HANDLE value) noexcept : value_(value) {}

    ~ScopedFileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return value_; }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

template <typename Request>
anomaly::UiResourceReadResult ReadPackageResourceBytes(
    const Request& request, const std::size_t maximum_bytes,
    const anomaly::UiResourceAllocationAdmission admission) {
    if (request.package_directory.empty()) {
        return anomaly::ReadUiResourceBytes(
            std::filesystem::path(request.relative_path), maximum_bytes, admission);
    }

    HANDLE raw_file = INVALID_HANDLE_VALUE;
    const anomaly::PluginPackagePathResult opened = anomaly::OpenConfinedPluginPackageFile(
        request.package_directory, request.relative_path, false, &raw_file);
    if (!opened.Ok() || raw_file == INVALID_HANDLE_VALUE) {
        anomaly::UiResourceReadResult failed;
        failed.error = opened.error == anomaly::PluginPackageError::ReparsePoint
            ? anomaly::UiResourceDecodeError::ReparsePoint
            : anomaly::UiResourceDecodeError::PathUnavailable;
        return failed;
    }
    const ScopedFileHandle file(raw_file);
    return anomaly::ReadUiResourceBytesFromFileHandle(file.Get(), maximum_bytes, admission);
}

std::string_view ServiceString(const AnomalyStringViewV1 value) noexcept {
    return value.data == nullptr ? std::string_view{} : std::string_view(value.data, value.size);
}

AnomalyStatusV1 CopyLocalizationString(
    const std::string_view value, char* const destination, std::size_t* const inout_size) noexcept {
    if (inout_size == nullptr) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "localization size is null");
    }
    const std::size_t required = value.size() + 1U;
    if (destination == nullptr) {
        *inout_size = required;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (*inout_size < required) {
        *inout_size = required;
        return StatusV1(
            ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, "localization destination is too small");
    }
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
    *inout_size = required;
    return StatusV1(ANOMALY_STATUS_V1_OK);
}

void ReportLocalizationFallback(
    PluginServiceContext& context, std::string detail) noexcept {
    if (context.localization_locale == anomaly::Locale::EnUs ||
        context.localization_fallback_logged.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    try {
        context.manager->LogPlugin(
            ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
            "plugin localization fallback: " + std::move(detail),
            context.plugin_id, context.generation);
    } catch (...) {
    }
}

template <typename Callback>
AnomalyStatusV1 InvokeLocalizationService(void* const user, Callback&& callback) noexcept {
    auto* const context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->localization_catalog == nullptr) {
        return StatusV1(
            ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin localization context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) {
        return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    try {
        return callback(*context);
    } catch (...) {
        return StatusV1(ANOMALY_STATUS_V1_FAILED, "plugin localization service failed");
    }
}

AnomalyStatusV1 ANOMALY_CALL LocaleV1(
    void* const user, char* const destination, std::size_t* const inout_size) {
    return InvokeLocalizationService(user, [&](const PluginServiceContext& context) {
        return CopyLocalizationString(
            anomaly::LocaleName(context.localization_locale), destination, inout_size);
    });
}

AnomalyStatusV1 ANOMALY_CALL TranslateV1(
    void* const user,
    const AnomalyStringViewV1 key,
    const AnomalyStringViewV1 english_fallback,
    const AnomalyStringViewV1* const arguments,
    const std::size_t argument_count,
    char* const destination,
    std::size_t* const inout_size) {
    if (key.data == nullptr || key.size == 0 || english_fallback.data == nullptr ||
        inout_size == nullptr || argument_count > 8U ||
        (argument_count != 0 && arguments == nullptr)) {
        return StatusV1(
            ANOMALY_STATUS_V1_INVALID_ARGUMENT, "localization request is invalid");
    }
    std::array<std::string_view, 8> argument_views{};
    for (std::size_t index = 0; index < argument_count; ++index) {
        if (arguments[index].data == nullptr && arguments[index].size != 0) {
            return StatusV1(
                ANOMALY_STATUS_V1_INVALID_ARGUMENT, "localization argument is invalid");
        }
        argument_views[index] = ServiceString(arguments[index]);
    }
    return InvokeLocalizationService(user, [&](PluginServiceContext& context) {
        const anomaly::PluginTranslation translation = context.localization_catalog->Translate(
            ServiceString(key), ServiceString(english_fallback),
            std::span<const std::string_view>(argument_views.data(), argument_count));
        if (translation.used_english_fallback) {
            ReportLocalizationFallback(
                context,
                "key=" + std::string(ServiceString(key)) + " reason=" +
                    (translation.argument_mismatch
                        ? "argument-mismatch"
                        : "localized-message-unavailable"));
        }
        return CopyLocalizationString(translation.text, destination, inout_size);
    });
}

void* ANOMALY_CALL AllocateV1(void* user, std::size_t size, std::size_t alignment) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return nullptr;
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    return _aligned_malloc(size, alignment);
}

void* ANOMALY_CALL ReallocateV1(
    void* user, void* memory, std::size_t size, std::size_t alignment) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return nullptr;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    return _aligned_realloc(memory, size, alignment);
}

void ANOMALY_CALL ReleaseV1(void* user, void* memory, std::size_t) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    _aligned_free(memory);
}

void ANOMALY_CALL LogV1(void* user, std::uint32_t level, AnomalyStringViewV1 message) {
    std::string copy = message.data == nullptr ? std::string{} : std::string(message.data, message.size);
    auto* context = static_cast<PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) return;
    if (context != nullptr && context->manager != nullptr && g_manager == context->manager) {
        context->manager->LogPlugin(
            static_cast<AnomalyCoreLogLevelV1>(level), std::move(copy),
            context->plugin_id, context->generation);
    } else {
        HostLog(static_cast<AnomalyCoreLogLevelV1>(level), copy.c_str());
    }
}

AnomalyStatusV1 RequireRawMemoryCapability(
    const PluginServiceContext* context,
    const std::string_view capability) noexcept {
    if (context == nullptr) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    const anomaly::PluginServiceAuthorization authorization =
        context->capabilities.AuthorizeRawMemory(capability);
    if (authorization.allowed) return StatusV1(ANOMALY_STATUS_V1_OK);
    return StatusV1(
        ANOMALY_STATUS_V1_PERMISSION_DENIED,
        capability == "memory-read"
            ? "manifest capability memory-read is required"
            : "manifest capability memory-write is required");
}

AnomalyStatusV1 ANOMALY_CALL ReadV1(
    void* user, std::uintptr_t address, AnomalyMutableByteSpanV1 destination) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    const AnomalyStatusV1 authorization = RequireRawMemoryCapability(context, "memory-read");
    if (authorization.code != ANOMALY_STATUS_V1_OK) return authorization;
    return destination.data != nullptr && ReadHostMemory(address, destination.data, destination.size)
        ? StatusV1(ANOMALY_STATUS_V1_OK)
        : StatusV1(ANOMALY_STATUS_V1_FAILED, "memory read failed");
}

AnomalyStatusV1 ANOMALY_CALL WriteV1(
    void* user, std::uintptr_t address, AnomalyByteSpanV1 source) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    if (!ValidServiceContext(context)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    const AnomalyStatusV1 authorization = RequireRawMemoryCapability(context, "memory-write");
    if (authorization.code != ANOMALY_STATUS_V1_OK) return authorization;
    return source.data != nullptr && WriteHostMemory(address, source.data, source.size)
        ? StatusV1(ANOMALY_STATUS_V1_OK)
        : StatusV1(ANOMALY_STATUS_V1_FAILED, "memory write failed");
}

AnomalyStatusV1 ANOMALY_CALL PluginDirectoryV1(
    void* user, char* destination, std::size_t* inout_size) {
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) {
        return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    if (context == nullptr || inout_size == nullptr || context->package_directory.empty()) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin scope is unavailable");
    }
    const std::string encoded = Utf8(context->package_directory);
    const std::size_t required = encoded.size() + 1;
    if (destination == nullptr) {
        *inout_size = required;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (*inout_size < required) {
        *inout_size = required;
        return StatusV1(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    std::memcpy(destination, encoded.c_str(), required);
    *inout_size = required;
    return StatusV1(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL PluginStateDirectoryV1(
    void* user, char* destination, std::size_t* inout_size) {
    if (IsSynchronousStateIoForbidden()) {
        return StatusV1(
            ANOMALY_STATUS_V1_UNAVAILABLE,
            "plugin state I/O is unavailable from game or render callbacks");
    }
    const auto* context = static_cast<const PluginServiceContext*>(user);
    auto callback = AcquireServiceCallback(context);
    if (context != nullptr && context->scope != nullptr && !callback) {
        return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    if (context == nullptr || inout_size == nullptr || context->state_directory.empty()) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin state scope is unavailable");
    }
    std::error_code error;
    std::filesystem::create_directories(context->state_directory, error);
    if (error || !std::filesystem::is_directory(context->state_directory, error) || error) {
        return StatusV1(ANOMALY_STATUS_V1_FAILED, "plugin state directory is unavailable");
    }
    const std::string encoded = Utf8(context->state_directory);
    const std::size_t required = encoded.size() + 1;
    if (destination == nullptr) {
        *inout_size = required;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (*inout_size < required) {
        *inout_size = required;
        return StatusV1(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL, "destination is too small");
    }
    std::memcpy(destination, encoded.c_str(), required);
    *inout_size = required;
    return StatusV1(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL RegisterConfigSchemaV1(
    void* user, AnomalyStringViewV1 schema_id, std::uint32_t schema_version,
    AnomalyByteSpanV1 schema_json, AnomalyGenerationHandleV1* handle) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->RegisterConfigSchema(
            ScopedPlatformOwner(context), ServiceString(schema_id), schema_version, schema_json, handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL UnregisterConfigSchemaV1(
    void* user, AnomalyGenerationHandleV1 handle) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->UnregisterConfigSchema(ScopedPlatformOwner(context), handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL ReadConfigV1(
    void* user, AnomalyStringViewV1 schema_id, std::uint32_t* schema_version,
    AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->ReadConfig(
            ScopedPlatformOwner(context), ServiceString(schema_id), schema_version, destination, inout_size);
    });
}

AnomalyStatusV1 ANOMALY_CALL WriteConfigV1(
    void* user, AnomalyStringViewV1 schema_id, std::uint32_t schema_version,
    AnomalyByteSpanV1 document) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->WriteConfig(
            ScopedPlatformOwner(context), ServiceString(schema_id), schema_version, document);
    });
}

AnomalyStatusV1 ANOMALY_CALL MigrateConfigV1(
    void* user, AnomalyStringViewV1 schema_id, AnomalyConfigMigrationV1 migration,
    void* migration_user) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->MigrateConfig(
            ScopedPlatformOwner(context), ServiceString(schema_id), migration, migration_user);
    });
}

AnomalyStatusV1 ANOMALY_CALL ReadStorageV1(
    void* user, AnomalyStringViewV1 relative_path, AnomalyMutableByteSpanV1 destination,
    std::size_t* inout_size) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->ReadStorage(
            ScopedPlatformOwner(context), ServiceString(relative_path), destination, inout_size);
    });
}

AnomalyStatusV1 ANOMALY_CALL WriteStorageV1(
    void* user, AnomalyStringViewV1 relative_path, AnomalyByteSpanV1 source) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->WriteStorage(
            ScopedPlatformOwner(context), ServiceString(relative_path), source);
    });
}

AnomalyStatusV1 ANOMALY_CALL RemoveStorageV1(
    void* user, AnomalyStringViewV1 relative_path) {
    return InvokeStateIoService(user, [&](PluginServiceContext& context) {
        return context.platform->RemoveStorage(
            ScopedPlatformOwner(context), ServiceString(relative_path));
    });
}

AnomalyStatusV1 ANOMALY_CALL RuntimeInfoV1(void* user, AnomalyRuntimeInfoV1* snapshot) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->RuntimeInfo(ScopedPlatformOwner(context), snapshot);
    });
}

AnomalyStatusV1 ANOMALY_CALL RuntimeVersionV1(
    void* user, char* destination, std::size_t* inout_size) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        static_cast<void>(context);
        return context.platform->RuntimeVersion(destination, inout_size);
    });
}

AnomalyStatusV1 ANOMALY_CALL RegisterSelfTestV1(
    void* user, AnomalyStringViewV1 id, AnomalyDiagnosticSelfTestV1 callback,
    void* callback_user, AnomalyGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->RegisterSelfTest(
            ScopedPlatformOwner(context), ServiceString(id), callback, callback_user, handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL UnregisterSelfTestV1(
    void* user, AnomalyGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->UnregisterSelfTest(ScopedPlatformOwner(context), handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL RunSelfTestV1(
    void* user, AnomalyStringViewV1 id, AnomalyMutableByteSpanV1 destination,
    std::size_t* inout_size) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->RunSelfTest(
            ScopedPlatformOwner(context), ServiceString(id), destination, inout_size);
    });
}

AnomalyStatusV1 ANOMALY_CALL DiagnosticsSnapshotV1(
    void* user, AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->DiagnosticsSnapshot(
            ScopedPlatformOwner(context), destination, inout_size);
    });
}

AnomalyStatusV1 ANOMALY_CALL ScheduleV1(
    void* user, std::uint32_t delay_milliseconds, AnomalyTaskCallbackV1 callback,
    void* callback_user, AnomalyGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->Schedule(
            ScopedPlatformOwner(context), delay_milliseconds, callback, callback_user, handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL CancelTaskV1(
    void* user, AnomalyGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->CancelTask(ScopedPlatformOwner(context), handle);
    });
}

template <typename Callback>
AnomalyStatusV1 InvokeScopedIpc(void* user, Callback&& callback) noexcept {
    auto* context = static_cast<PluginServiceContext*>(user);
    if (!ValidServiceContext(context) || context->ipc == nullptr) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin IPC context is invalid");
    }
    auto lease = AcquireServiceCallback(context);
    if (!lease) return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    try { return callback(*context); }
    catch (...) { return StatusV1(ANOMALY_STATUS_V1_FAILED, "IPC service failed"); }
}

AnomalyStatusV1 ANOMALY_CALL RegisterIpcEndpointV1(
    void* user, const AnomalyIpcEndpointDescriptorV1* descriptor,
    AnomalyIpcRequestHandlerV1 handler, void* callback_user,
    AnomalyGenerationHandleV1* endpoint) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->RegisterEndpoint(
            IpcOwner(context), descriptor, handler, callback_user, endpoint);
    });
}

AnomalyStatusV1 ANOMALY_CALL UnregisterIpcEndpointV1(
    void* user, AnomalyGenerationHandleV1 endpoint) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->UnregisterEndpoint(IpcOwner(context), endpoint);
    });
}

AnomalyStatusV1 ANOMALY_CALL InvokeIpcV1(
    void* user, const AnomalyIpcEndpointSelectorV1* selector,
    AnomalyByteSpanV1 request, AnomalyMutableByteSpanV1 response,
    std::size_t* response_size) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Invoke(
            IpcOwner(context), CurrentIpcDomain(), selector, request, response, response_size);
    });
}

AnomalyStatusV1 ANOMALY_CALL InvokeIpcAsyncV1(
    void* user, const AnomalyIpcEndpointSelectorV1* selector,
    AnomalyByteSpanV1 request, AnomalyIpcCompletionCallbackV1 completion,
    void* completion_user, AnomalyGenerationHandleV1* pending_call) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->InvokeAsync(
            IpcOwner(context), selector, request, completion, completion_user, pending_call);
    });
}

AnomalyStatusV1 ANOMALY_CALL CancelIpcV1(
    void* user, AnomalyGenerationHandleV1 pending_call) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Cancel(IpcOwner(context), pending_call);
    });
}

AnomalyStatusV1 ANOMALY_CALL SubscribeIpcV1(
    void* user, const AnomalyIpcEndpointSelectorV1* selector,
    AnomalyIpcEventCallbackV1 callback, void* callback_user,
    AnomalyGenerationHandleV1* subscription) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Subscribe(
            IpcOwner(context), selector, callback, callback_user, subscription);
    });
}

AnomalyStatusV1 ANOMALY_CALL UnsubscribeIpcV1(
    void* user, AnomalyGenerationHandleV1 subscription) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Unsubscribe(IpcOwner(context), subscription);
    });
}

AnomalyStatusV1 ANOMALY_CALL PublishIpcV1(
    void* user, AnomalyGenerationHandleV1 endpoint, AnomalyByteSpanV1 event) {
    return InvokeScopedIpc(user, [&](PluginServiceContext& context) {
        return context.ipc->Publish(IpcOwner(context), endpoint, event);
    });
}

AnomalyStatusV1 ANOMALY_CALL RegisterCommandV1(
    void* user, AnomalyStringViewV1 name, AnomalyStringViewV1 description,
    AnomalyCommandCallbackV1 callback, void* callback_user, AnomalyGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->RegisterCommand(
            ScopedPlatformOwner(context), ServiceString(name), ServiceString(description),
            callback, callback_user, handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL UnregisterCommandV1(
    void* user, AnomalyGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->UnregisterCommand(ScopedPlatformOwner(context), handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL InvokeCommandV1(
    void* user, AnomalyStringViewV1 name, AnomalyStringViewV1 arguments,
    AnomalyMutableByteSpanV1 destination, std::size_t* inout_size) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->InvokeCommand(
            ScopedPlatformOwner(context), ServiceString(name), ServiceString(arguments),
            destination, inout_size);
    });
}

AnomalyStatusV1 ANOMALY_CALL PostNotificationV1(
    void* user, AnomalyNotificationSeverityV1 severity, AnomalyStringViewV1 title,
    AnomalyStringViewV1 body, std::uint32_t timeout_milliseconds,
    AnomalyGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->PostNotification(
            ScopedPlatformOwner(context), severity, ServiceString(title), ServiceString(body),
            timeout_milliseconds, handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL DismissNotificationV1(
    void* user, AnomalyGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->DismissNotification(ScopedPlatformOwner(context), handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL ResolveSignatureV1(
    void* user, AnomalyStringViewV1 module_name, AnomalyStringViewV1 section_name,
    AnomalyStringViewV1 pattern, std::uintptr_t* address) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->ResolveSignature(
            ScopedPlatformOwner(context), ServiceString(module_name), ServiceString(section_name),
            ServiceString(pattern), address);
    });
}

AnomalyStatusV1 ANOMALY_CALL CreateHookV1(
    void* user, const AnomalyHookRequestV1* request, std::uintptr_t* original,
    AnomalyGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->CreateHook(ScopedPlatformOwner(context), request, original, handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL ReleaseHookV1(
    void* user, AnomalyGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->ReleaseHook(ScopedPlatformOwner(context), handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL BeginHookCallbackV1(
    void* user, AnomalyGenerationHandleV1 hook, AnomalyGenerationHandleV1* callback_lease) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->BeginHookCallback(
            ScopedPlatformOwner(context), hook, callback_lease);
    });
}

AnomalyStatusV1 ANOMALY_CALL EndHookCallbackV1(
    void* user, AnomalyGenerationHandleV1 callback_lease) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->EndHookCallback(ScopedPlatformOwner(context), callback_lease);
    });
}

AnomalyStatusV1 ANOMALY_CALL ApplyPatchV1(
    void* user, std::uintptr_t address, AnomalyByteSpanV1 replacement,
    AnomalyStringViewV1 label, AnomalyGenerationHandleV1* handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->ApplyPatch(
            ScopedPlatformOwner(context), address, replacement, ServiceString(label), handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL ReleasePatchV1(
    void* user, AnomalyGenerationHandleV1 handle) {
    return InvokeScopedPlatform(user, [&](PluginServiceContext& context) {
        return context.platform->ReleasePatch(ScopedPlatformOwner(context), handle);
    });
}

AnomalyStatusV1 ANOMALY_CALL RegisterWindowV1(
    void* user, const AnomalyWindowSpecV1* spec, AnomalyGenerationHandleV1* handle) {
    if (handle == nullptr) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "window handle is null");
    *handle = {};
    if (spec == nullptr || spec->struct_size < sizeof(*spec)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "window spec is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        anomaly::UiWindowRequest request;
        request.id = std::string(ServiceString(spec->id));
        request.title = std::string(ServiceString(spec->title));
        request.flags = spec->flags;
        request.persist_settings = (spec->flags & ANOMALY_WINDOW_V1_NO_SAVED_SETTINGS) == 0;
        request.initial_width = spec->initial_width;
        request.initial_height = spec->initial_height;
        request.constraints = {
            spec->minimum_width, spec->minimum_height, spec->maximum_width, spec->maximum_height};
        request.default_open = spec->default_open != 0;
        const auto resource = context.ui_resources->RegisterWindow(context.scope, std::move(request));
        if (!resource) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "window spec was rejected");
        *handle = GenerationHandle(context, resource);
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL ReleaseWindowV1(
    void* user, const AnomalyGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !context.ui_resources->Release(context.scope, UiHandle(handle))) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL SetWindowOpenV1(
    void* user, const AnomalyGenerationHandleV1 handle, const std::int32_t open) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !(open == 0
                ? context.ui_resources->CloseWindow(context.scope, UiHandle(handle))
                : context.ui_resources->OpenWindow(context.scope, UiHandle(handle)))) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL ToggleWindowV1(
    void* user, const AnomalyGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !context.ui_resources->ToggleWindow(context.scope, UiHandle(handle))) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL WindowStateV1(
    void* user, const AnomalyGenerationHandleV1 handle, AnomalyWindowStateV1* state) {
    if (state == nullptr || state->struct_size < sizeof(*state)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "window state is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        const auto window = context.ui_resources->WindowState(context.scope, UiHandle(handle));
        if (!window) return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "window handle is not live");
        *state = {sizeof(*state), window->flags, window->width, window->height,
            context.ui_resources->DeviceGeneration(), window->open ? 1 : 0, 0};
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL BeginWindowV1(
    void* user, const AnomalyGenerationHandleV1 handle, const std::uint32_t flags,
    std::int32_t* visible) {
    if (visible == nullptr) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "visible is null");
    *visible = 0;
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "window rendering is not active");
        }
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "window handle is not live");
        }
        const auto window = context.ui_resources->WindowState(context.scope, UiHandle(handle));
        if (!window) return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "window handle is not live");
        if (!window->open) return StatusV1(ANOMALY_STATUS_V1_OK);
        if (context.ui_proxy_context == nullptr || context.ui_stack == nullptr ||
            context.ui_proxy_context->service == nullptr ||
            context.ui_proxy_context->service->begin_window == nullptr) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "window stack is not ready");
        }
        const AnomalyUiServiceV1* ui = reinterpret_cast<const AnomalyUiServiceV1*>(context.ui);
        if (ui == nullptr || ui->set_next_window_size == nullptr || ui->begin_window == nullptr) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "window rendering is not active");
        }
        if (HasUiField<decltype(AnomalyUiServiceV1::set_next_window_size_constraints)>(
                ui, offsetof(AnomalyUiServiceV1, set_next_window_size_constraints)) &&
            ui->set_next_window_size_constraints != nullptr) {
            ui->set_next_window_size_constraints(
                ui->user, window->constraints.minimum_width,
                window->constraints.minimum_height, window->constraints.maximum_width,
                window->constraints.maximum_height);
        }
        if (window->width > 0.0F && window->height > 0.0F) {
            ui->set_next_window_size(ui->user, window->width, window->height, kHostWindowFirstUse);
        }
        const std::string title = window->title + "###" + window->stable_id;
        // Reserve before acquiring the host UI stack. Once begin_window succeeds,
        // recording the matching end must not allocate and fail independently.
        if (context.open_windows.size() == context.open_windows.max_size()) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "window stack is exhausted");
        }
        context.open_windows.reserve(context.open_windows.size() + 1U);
        int open = 1;
        const std::size_t stack_size = context.ui_stack->entries.size();
        int result{};
        {
            ScopedUiProxyWindowKind scoped_kind(
                *context.ui_proxy_context, UiStackEntryKind::ScopedWindow, UiHandle(handle));
            result = ui->begin_window(
                ui->user, {title.data(), title.size()}, &open,
                ToHostUiWindowFlags(window->flags | flags));
        }
        if (context.ui_stack->entries.size() != stack_size + 1U ||
            !context.ui_stack->HasTop(UiStackEntryKind::ScopedWindow, UiHandle(handle))) {
            context.ui_stack->MarkMismatch();
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "window stack is unavailable");
        }
        if (open == 0) static_cast<void>(context.ui_resources->CloseWindow(context.scope, UiHandle(handle)));
        context.open_windows.push_back(UiHandle(handle));
        *visible = result != 0 ? 1 : 0;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL EndWindowV1(
    void* user, const AnomalyGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "window rendering is not active");
        }
        if (!ValidGenerationHandle(context, handle) || context.open_windows.empty() ||
            context.open_windows.back() != UiHandle(handle)) {
            return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "window end is unbalanced");
        }
        if (context.ui_proxy_context == nullptr || context.ui_stack == nullptr ||
            context.ui_proxy_context->service == nullptr ||
            context.ui_proxy_context->service->end_window == nullptr ||
            !context.ui_stack->HasTop(UiStackEntryKind::ScopedWindow, UiHandle(handle))) {
            if (context.ui_stack != nullptr) context.ui_stack->MarkMismatch();
            return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "window end is unbalanced");
        }
        const AnomalyUiServiceV1* ui = reinterpret_cast<const AnomalyUiServiceV1*>(context.ui);
        if (ui == nullptr || ui->end_window == nullptr) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "window rendering is not active");
        }
        if (HasUiField<decltype(AnomalyUiServiceV1::get_window_size)>(
                ui, offsetof(AnomalyUiServiceV1, get_window_size)) &&
            ui->get_window_size != nullptr) {
            float width{};
            float height{};
            ui->get_window_size(ui->user, &width, &height);
            static_cast<void>(context.ui_resources->SetWindowSize(
                context.scope, UiHandle(handle), width, height));
        }
        {
            ScopedUiProxyWindowKind scoped_kind(
                *context.ui_proxy_context, UiStackEntryKind::ScopedWindow, UiHandle(handle));
            ui->end_window(ui->user);
        }
        context.open_windows.pop_back();
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL RequestFontV1(
    void* user, const AnomalyFontRequestV1* request, AnomalyGenerationHandleV1* handle) {
    if (handle == nullptr) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "font handle is null");
    *handle = {};
    if (request == nullptr || request->struct_size < sizeof(*request)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "font request is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        const auto path = ResolvePackageResourcePath(context, request->relative_path);
        if (!path) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "font path escapes package");
        anomaly::UiFontRequest descriptor;
        descriptor.package_directory = path->package_directory;
        descriptor.relative_path = path->relative_path;
        descriptor.flags = request->flags;
        descriptor.size_pixels = request->size_pixels;
        descriptor.glyph_range = static_cast<anomaly::UiGlyphRange>(request->glyph_range);
        const auto resource = context.ui_resources->RequestFont(context.scope, std::move(descriptor));
        if (!resource) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "font request was rejected");
        if (!context.manager->QueueUiFontLoad(context.scope, resource)) {
            static_cast<void>(context.ui_resources->Release(context.scope, resource));
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "font worker is not available");
        }
        *handle = GenerationHandle(context, resource);
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL ReleaseFontV1(
    void* user, const AnomalyGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !context.ui_resources->Release(context.scope, UiHandle(handle))) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "font handle is not live");
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

std::uint32_t ToFontStateFlags(const anomaly::UiResourceState state) noexcept {
    switch (state) {
    case anomaly::UiResourceState::Queued: return ANOMALY_FONT_STATE_V1_QUEUED;
    case anomaly::UiResourceState::Ready: return ANOMALY_FONT_STATE_V1_READY;
    case anomaly::UiResourceState::Failed: return ANOMALY_FONT_STATE_V1_FAILED;
    case anomaly::UiResourceState::StaleDevice: return ANOMALY_FONT_STATE_V1_STALE_DEVICE;
    default: return ANOMALY_FONT_STATE_V1_NONE;
    }
}

AnomalyStatusV1 ANOMALY_CALL FontStateV1(
    void* user, const AnomalyGenerationHandleV1 handle, AnomalyFontStateV1* state) {
    if (state == nullptr || state->struct_size < sizeof(*state)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "font state is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "font handle is not live");
        }
        const auto font = context.ui_resources->ResourceState(context.scope, UiHandle(handle));
        if (!font) return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "font handle is not live");
        *state = {sizeof(*state), ToFontStateFlags(font->state), font->effective_font_size_pixels,
            font->font_scale, font->device_generation,
            font->state == anomaly::UiResourceState::Ready ? 1 : 0, 0};
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL PushFontV1(
    void* user, const AnomalyGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "font rendering is not active");
        }
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "font handle is not live");
        }
        const auto font = context.ui_resources->ResourceState(context.scope, UiHandle(handle));
        if (!font) return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "font handle is not live");
        if (font->state == anomaly::UiResourceState::Failed) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "font atlas build failed");
        }
        if (context.ui_stack == nullptr || !context.ui_stack->ReserveNext()) {
            if (context.ui_stack != nullptr) context.ui_stack->MarkMismatch();
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "font stack is exhausted");
        }
        // Match BeginWindowV1: grow bookkeeping before the backend changes its
        // font stack so a later allocation failure cannot leave it unbalanced.
        if (context.pushed_fonts.size() == context.pushed_fonts.max_size()) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "font stack is exhausted");
        }
        context.pushed_fonts.reserve(context.pushed_fonts.size() + 1U);
        if (!context.manager->PushUiFont(context.scope, UiHandle(handle))) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "font render backend is not ready");
        }
        context.ui_stack->PushReserved({UiStackEntryKind::Font, UiHandle(handle)});
        context.pushed_fonts.push_back(UiHandle(handle));
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL PopFontV1(void* user) {
    return InvokeUiResourceService(user, [](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "font rendering is not active");
        }
        if (context.pushed_fonts.empty() || context.ui_stack == nullptr ||
            !context.ui_stack->HasTop(UiStackEntryKind::Font, context.pushed_fonts.back())) {
            if (context.ui_stack != nullptr) context.ui_stack->MarkMismatch();
            return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "font pop is unbalanced");
        }
        if (!context.manager->PopUiFont()) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "font render backend is not ready");
        }
        static_cast<void>(context.ui_stack->Consume(
            UiStackEntryKind::Font, context.pushed_fonts.back()));
        context.pushed_fonts.pop_back();
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL RequestTextureV1(
    void* user, const AnomalyTextureRequestV1* request, AnomalyGenerationHandleV1* handle) {
    if (handle == nullptr) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "texture handle is null");
    *handle = {};
    if (request == nullptr || request->struct_size < sizeof(*request) ||
        (request->encoded_bytes.size != 0 && request->encoded_bytes.data == nullptr)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "texture request is invalid");
    }
    if (request->format == ANOMALY_TEXTURE_FORMAT_V1_RGBA8) {
        if (request->encoded_bytes.size > anomaly::kDefaultUiResourceDecodedByteLimit) {
            return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "raw RGBA texture exceeds byte limit");
        }
    } else if (request->encoded_bytes.size > anomaly::kDefaultUiResourceEncodedByteLimit) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "encoded texture exceeds byte limit");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        anomaly::UiTextureRequest descriptor;
        if (request->relative_path.size != 0) {
            const auto path = ResolvePackageResourcePath(context, request->relative_path);
            if (!path) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "texture path escapes package");
            descriptor.package_directory = path->package_directory;
            descriptor.relative_path = path->relative_path;
        }
        descriptor.flags = request->flags;
        descriptor.format = static_cast<anomaly::UiTextureFormat>(request->format);
        descriptor.width = request->width;
        descriptor.height = request->height;
        if (descriptor.format == anomaly::UiTextureFormat::Rgba8) {
            constexpr std::uint64_t bytes_per_pixel = 4U;
            const std::uint64_t maximum_bytes = anomaly::kDefaultUiResourceDecodedByteLimit;
            if (request->width == 0 || request->height == 0 ||
                request->width > anomaly::kDefaultUiResourceImageDimensionLimit ||
                request->height > anomaly::kDefaultUiResourceImageDimensionLimit ||
                static_cast<std::uint64_t>(request->width) >
                    maximum_bytes / bytes_per_pixel / request->height) {
                return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "raw RGBA texture dimensions are invalid");
            }
            const std::uint64_t expected = static_cast<std::uint64_t>(request->width) *
                static_cast<std::uint64_t>(request->height) * bytes_per_pixel;
            if (expected != request->encoded_bytes.size) {
                return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "raw RGBA texture dimensions are invalid");
            }
        } else if (request->width != 0 || request->height != 0) {
            return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "encoded texture dimensions must be zero");
        }

        const anomaly::UiResourceStagingReservation reservation =
            context.ui_resources->ReserveStaging(request->encoded_bytes.size);
        if (request->encoded_bytes.size != 0 && !reservation) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "texture staging budget is exhausted");
        }

        anomaly::UiResourceHandle resource;
        try {
            if (request->encoded_bytes.size != 0) {
                descriptor.encoded_bytes.assign(
                    request->encoded_bytes.data,
                    request->encoded_bytes.data + request->encoded_bytes.size);
            }
            resource = context.ui_resources->RequestTexture(
                context.scope, std::move(descriptor), reservation);
        } catch (...) {
            static_cast<void>(context.ui_resources->ReleaseStaging(reservation));
            throw;
        }
        static_cast<void>(context.ui_resources->ReleaseStaging(reservation));
        if (!resource) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "texture request was rejected");
        if (!context.manager->QueueUiTextureLoad(context.scope, resource)) {
            static_cast<void>(context.ui_resources->Release(context.scope, resource));
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "texture worker is not available");
        }
        *handle = GenerationHandle(context, resource);
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL ReleaseTextureV1(
    void* user, const AnomalyGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle) ||
            !context.ui_resources->Release(context.scope, UiHandle(handle))) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "texture handle is not live");
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

std::uint32_t ToTextureStateFlags(const anomaly::UiResourceState state) noexcept {
    switch (state) {
    case anomaly::UiResourceState::Queued: return ANOMALY_TEXTURE_STATE_V1_QUEUED;
    case anomaly::UiResourceState::Ready: return ANOMALY_TEXTURE_STATE_V1_READY;
    case anomaly::UiResourceState::Failed: return ANOMALY_TEXTURE_STATE_V1_FAILED;
    case anomaly::UiResourceState::StaleDevice: return ANOMALY_TEXTURE_STATE_V1_STALE_DEVICE;
    default: return ANOMALY_TEXTURE_STATE_V1_NONE;
    }
}

AnomalyStatusV1 ANOMALY_CALL TextureStateV1(
    void* user, const AnomalyGenerationHandleV1 handle, AnomalyTextureStateV1* state) {
    if (state == nullptr || state->struct_size < sizeof(*state)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "texture state is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "texture handle is not live");
        }
        const auto texture = context.ui_resources->ResourceState(context.scope, UiHandle(handle));
        if (!texture) return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "texture handle is not live");
        *state = {sizeof(*state), ToTextureStateFlags(texture->state), texture->texture_width,
            texture->texture_height, texture->device_generation, texture->staged_bytes};
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL DrawTextureV1(
    void* user, const AnomalyGenerationHandleV1 handle, const float width, const float height,
    std::uint32_t tint_rgba) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "texture rendering is not active");
        }
        if (!ValidGenerationHandle(context, handle)) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "texture handle is not live");
        }
        const auto texture = context.ui_resources->ResourceState(context.scope, UiHandle(handle));
        if (!texture) return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "texture handle is not live");
        if (texture->state == anomaly::UiResourceState::Failed) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "texture decode or upload failed");
        }
        if (!context.manager->DrawUiTexture(
                context.scope, UiHandle(handle), width, height, tint_rgba)) {
            return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "texture render backend is not ready");
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL InputSnapshotV1(void* user, AnomalyInputSnapshotV1* snapshot) {
    if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "input snapshot is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (context.input == nullptr) return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "input is not ready");
        PopulateInputSnapshot(context.input->Snapshot(), *snapshot);
        if (const auto capture = context.input->UiCapture()) {
            snapshot->capture_flags = ToInputCaptureFlags(capture->state.flags);
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL WasPressedV1(
    void* user, const std::uint32_t virtual_key, std::int32_t* pressed) {
    if (pressed == nullptr || virtual_key == 0 || virtual_key >= anomaly::kInputKeyCount) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "input key is invalid");
    }
    *pressed = 0;
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (context.input == nullptr) return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "input is not ready");
        *pressed = context.input->WasPressed(
            anomaly::InputControl::ForKey(static_cast<anomaly::InputKey>(virtual_key))) ? 1 : 0;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL RegisterHotkeyV1(
    void* user, const AnomalyHotkeySpecV1* spec, AnomalyHotkeyCallbackV1 callback,
    void* callback_user, AnomalyGenerationHandleV1* handle) {
    if (handle == nullptr) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "hotkey handle is null");
    *handle = {};
    constexpr std::uint32_t known_flags = ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS |
        ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED |
        ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED;
    if (spec == nullptr || spec->struct_size < sizeof(*spec) || callback == nullptr ||
        spec->virtual_key == 0 || spec->virtual_key >= anomaly::kInputKeyCount ||
        (spec->modifiers & ~static_cast<std::uint32_t>(anomaly::ToMask(anomaly::InputModifier::Shift) |
            anomaly::ToMask(anomaly::InputModifier::Control) |
            anomaly::ToMask(anomaly::InputModifier::Alt) |
            anomaly::ToMask(anomaly::InputModifier::Super))) != 0 ||
        (spec->flags & ~known_flags) != 0 ||
        ((spec->flags & ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED) != 0 &&
         (spec->flags & ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED) != 0)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "hotkey spec is invalid");
    }
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (context.input == nullptr) return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "input is not ready");
        anomaly::HotkeySpec request;
        request.id = std::string(ServiceString(spec->id));
        request.trigger = anomaly::InputControl::ForKey(static_cast<anomaly::InputKey>(spec->virtual_key));
        request.modifiers = spec->modifiers;
        request.exact_modifiers = (spec->flags & ANOMALY_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS) == 0;
        request.capture_policy = (spec->flags & ANOMALY_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED) != 0
            ? anomaly::HotkeyCapturePolicy::AllowWhileUiCaptured
            : (spec->flags & ANOMALY_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED) != 0
                ? anomaly::HotkeyCapturePolicy::OnlyWhileUiCaptured
                : anomaly::HotkeyCapturePolicy::RespectUiCapture;
        const std::shared_ptr<anomaly::PluginScope> scope = context.scope;
        const std::uint64_t generation = context.generation;
        const auto registration = context.input->RegisterHotkey(
            scope, std::move(request),
            [scope, generation, callback, callback_user](const anomaly::HotkeyEvent& event) {
                auto callback_scope = scope->AcquireCallback(generation);
                if (!callback_scope) return;
                ScopedPluginCallback activation(scope, generation, false);
                AnomalyInputSnapshotV1 snapshot{};
                PopulateInputSnapshot(event.snapshot, snapshot);
                snapshot.capture_flags = ToInputCaptureFlags(event.ui_capture.state.flags);
                callback(callback_user, {event.handle.value, generation}, &snapshot);
            });
        if (!registration) {
            return StatusV1(
                registration.status == anomaly::HotkeyRegistrationStatus::Conflict
                    ? ANOMALY_STATUS_V1_CONFLICT
                    : registration.status == anomaly::HotkeyRegistrationStatus::DispatcherUnavailable
                        ? ANOMALY_STATUS_V1_UNAVAILABLE
                        : ANOMALY_STATUS_V1_INVALID_ARGUMENT,
                registration.status == anomaly::HotkeyRegistrationStatus::Conflict
                    ? "hotkey conflicts with an existing binding"
                    : "hotkey registration failed");
        }
        *handle = {registration.handle.value, context.generation};
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL ReleaseHotkeyV1(
    void* user, const AnomalyGenerationHandleV1 handle) {
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (context.input == nullptr || !ValidGenerationHandle(context, handle) ||
            !context.input->ReleaseHotkey(context.scope, {handle.id})) {
            return StatusV1(ANOMALY_STATUS_V1_NOT_FOUND, "hotkey handle is not live");
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL InputCaptureStateV1(void* user, std::uint32_t* flags) {
    if (flags == nullptr) return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "capture flags are null");
    *flags = ANOMALY_INPUT_CAPTURE_V1_NONE;
    return InvokeUiResourceService(user, [&](PluginServiceContext& context) {
        if (!IsActiveDrawResourceCallback(context)) {
            return StatusV1(
                ANOMALY_STATUS_V1_UNAVAILABLE,
                "input capture state is only available during draw");
        }
        if (context.input == nullptr) return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "input is not ready");
        if (const auto capture = context.input->UiCapture()) {
            *flags = ToInputCaptureFlags(capture->state.flags);
        }
        return StatusV1(ANOMALY_STATUS_V1_OK);
    });
}

AnomalyStatusV1 ANOMALY_CALL QueryServiceV1(
    void* host_context, AnomalyStringViewV1 service_id,
    std::uint32_t minimum_version, const void** service) {
    if (service == nullptr || service_id.data == nullptr) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *service = nullptr;
    const std::string_view id(service_id.data, service_id.size);
    auto* context = static_cast<PluginServiceContext*>(host_context);
    if (!ValidServiceContext(context)) {
        return StatusV1(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "plugin service context is invalid");
    }
    auto callback = AcquireServiceCallback(context);
    if (!callback) {
        return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "plugin scope is stopping");
    }
    if (!context->capabilities.AuthorizeService(id).allowed) {
        return StatusV1(
            ANOMALY_STATUS_V1_PERMISSION_DENIED,
            "service capability is not granted");
    }
    if (id == ANOMALY_CORE_SERVICE_V1_ID &&
        minimum_version <= context->core.service_version) {
        *service = &context->core;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_PLUGIN_STATE_SERVICE_V1_ID &&
        minimum_version <= context->plugin_state.service_version) {
        *service = &context->plugin_state;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_CONFIG_SERVICE_V1_ID &&
        minimum_version <= context->config.service_version) {
        *service = &context->config;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_STORAGE_SERVICE_V1_ID &&
        minimum_version <= context->storage.service_version) {
        *service = &context->storage;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_RUNTIME_INFO_SERVICE_V1_ID &&
        minimum_version <= context->runtime_info.service_version) {
        *service = &context->runtime_info;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_LOCALIZATION_SERVICE_V1_ID &&
        minimum_version <= context->localization.service_version) {
        *service = &context->localization;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_DIAGNOSTICS_SERVICE_V1_ID &&
        minimum_version <= context->diagnostics.service_version) {
        *service = &context->diagnostics;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_SCHEDULER_SERVICE_V1_ID &&
        minimum_version <= context->scheduler.service_version) {
        *service = &context->scheduler;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_IPC_SERVICE_V1_ID &&
        minimum_version <= context->ipc_service.service_version) {
        *service = &context->ipc_service;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_COMMANDS_SERVICE_V1_ID &&
        minimum_version <= context->commands.service_version) {
        *service = &context->commands;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_NOTIFICATIONS_SERVICE_V1_ID &&
        minimum_version <= context->notifications.service_version) {
        *service = &context->notifications;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_SIGNATURE_SERVICE_V1_ID &&
        minimum_version <= context->signature.service_version) {
        *service = &context->signature;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_HOOK_SERVICE_V1_ID &&
        minimum_version <= context->hook.service_version) {
        *service = &context->hook;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_PATCH_SERVICE_V1_ID &&
        minimum_version <= context->patch.service_version) {
        *service = &context->patch;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_WINDOW_SERVICE_V1_ID &&
        minimum_version <= context->window.service_version) {
        *service = &context->window;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_FONT_SERVICE_V1_ID &&
        minimum_version <= context->font.service_version) {
        *service = &context->font;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_TEXTURE_SERVICE_V1_ID &&
        minimum_version <= context->texture.service_version) {
        *service = &context->texture;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_INPUT_SERVICE_V1_ID &&
        minimum_version <= context->input_service.service_version) {
        *service = &context->input_service;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    if (id == ANOMALY_UI_SERVICE_V1_ID) {
        const AnomalyUiServiceV1* ui = context->manager->UiService();
        if (ui != nullptr && ui->service_version >= minimum_version) {
            if (context->ui == nullptr || context->ui->service_version < minimum_version) {
                return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "requested UI version is not ready");
            }
            *service = context != nullptr && context->ui != nullptr
                ? static_cast<const void*>(context->ui)
                : static_cast<const void*>(ui);
            return StatusV1(ANOMALY_STATUS_V1_OK);
        }
    }
    if (const void* adapter = anomaly::ProcessAdapterServices().Query(id, minimum_version)) {
        *service = adapter;
        return StatusV1(ANOMALY_STATUS_V1_OK);
    }
    return StatusV1(ANOMALY_STATUS_V1_UNAVAILABLE, "service is not ready");
}

bool RecoverPluginUiStack(
    PluginUiProxyContext& proxy, PluginServiceContext& services) noexcept {
    UiStackTracker* const stack = proxy.ui_stack;
    bool unbalanced = stack == nullptr || stack->mismatch ||
        (stack != nullptr && !stack->entries.empty());

    const auto end_window = [&]() noexcept {
        if (proxy.service == nullptr || proxy.service->end_window == nullptr) return;
        try {
            proxy.service->end_window(proxy.service->user);
        } catch (...) {
        }
    };
    const auto end_child = [&]() noexcept {
        if (!HasUiField<decltype(AnomalyUiServiceV1::end_child)>(
                proxy.service, offsetof(AnomalyUiServiceV1, end_child)) ||
            proxy.service->end_child == nullptr) {
            return;
        }
        try {
            proxy.service->end_child(proxy.service->user);
        } catch (...) {
        }
    };
    const auto end_table = [&]() noexcept {
        if (!HasUiField<decltype(AnomalyUiServiceV1::end_table)>(
                proxy.service, offsetof(AnomalyUiServiceV1, end_table)) ||
            proxy.service->end_table == nullptr) {
            return;
        }
        try {
            proxy.service->end_table(proxy.service->user);
        } catch (...) {
        }
    };
    const auto end_menu = [&]() noexcept {
        if (!HasUiField<decltype(AnomalyUiServiceV1::end_menu)>(
                proxy.service, offsetof(AnomalyUiServiceV1, end_menu)) ||
            proxy.service->end_menu == nullptr) {
            return;
        }
        try {
            proxy.service->end_menu(proxy.service->user);
        } catch (...) {
        }
    };
    const auto end_popup = [&]() noexcept {
        if (!HasUiField<decltype(AnomalyUiServiceV1::end_popup)>(
                proxy.service, offsetof(AnomalyUiServiceV1, end_popup)) ||
            proxy.service->end_popup == nullptr) {
            return;
        }
        try {
            proxy.service->end_popup(proxy.service->user);
        } catch (...) {
        }
    };

    if (stack != nullptr) {
        while (!stack->entries.empty()) {
            const UiStackEntry entry = stack->entries.back();
            stack->entries.pop_back();
            switch (entry.kind) {
            case UiStackEntryKind::Window:
                end_window();
                break;
            case UiStackEntryKind::ScopedWindow:
                end_window();
                if (!services.open_windows.empty() &&
                    services.open_windows.back() == entry.resource) {
                    services.open_windows.pop_back();
                } else {
                    stack->MarkMismatch();
                }
                break;
            case UiStackEntryKind::Child:
                end_child();
                break;
            case UiStackEntryKind::Table:
                end_table();
                break;
            case UiStackEntryKind::Menu:
                end_menu();
                break;
            case UiStackEntryKind::Popup:
                end_popup();
                break;
            case UiStackEntryKind::Font:
                if (services.manager != nullptr) {
                    try {
                        static_cast<void>(services.manager->PopUiFont());
                    } catch (...) {
                    }
                }
                if (!services.pushed_fonts.empty() &&
                    services.pushed_fonts.back() == entry.resource) {
                    services.pushed_fonts.pop_back();
                } else {
                    stack->MarkMismatch();
                }
                break;
            }
        }
        unbalanced = unbalanced || stack->mismatch;
        stack->Reset();
    }

    // The tracker is authoritative for normal operation.  These fallbacks keep
    // a generation from contaminating the next draw if a host table was swapped
    // while a scoped resource was live.
    while (!services.open_windows.empty()) {
        end_window();
        services.open_windows.pop_back();
        unbalanced = true;
    }
    while (!services.pushed_fonts.empty()) {
        if (services.manager != nullptr) {
            try {
                static_cast<void>(services.manager->PopUiFont());
            } catch (...) {
            }
        }
        services.pushed_fonts.pop_back();
        unbalanced = true;
    }
    return unbalanced;
}

}  // namespace

struct PluginManager::LoadedPlugin {
    HMODULE module{};
    AnomalyPluginDescriptorV1 descriptor_v1{};
    void* plugin_context{};
    bool waiting_for_service{};
    bool started{};
    bool faulted{};
    bool capability_audit_logged{};
    bool localization_catalog_loaded{};
    PluginView view;
    std::filesystem::path shadow;
    anomaly::PluginShadowGeneration shadow_generation;
    CallbackMetrics update_metrics;
    CallbackMetrics draw_metrics;
    UiStackTracker ui_stack;
    PluginServiceContext service_context;
    AnomalyHostApiV1 host_api{};
    PluginUiProxyContext ui_proxy_context;
    AnomalyUiServiceV1 ui_proxy{};
    std::shared_ptr<anomaly::PluginScope> scope;
};

struct UiResourceWorkerGate final {
    struct Pending final {
        std::shared_ptr<anomaly::PluginScope> scope;
        anomaly::UiResourceHandle handle;
        anomaly::UiResourceKind kind{anomaly::UiResourceKind::Font};
    };

    std::mutex mutex;
    // A canonical resource can have several scope leases while one Worker job
    // is queued. Keep every candidate so releasing the most recently queued
    // duplicate cannot strand an earlier live lease in Queued state.
    std::unordered_map<std::uint64_t, std::vector<Pending>> pending_resources;
};

void AddUiResourceWorkerCandidate(
    std::vector<UiResourceWorkerGate::Pending>& candidates,
    UiResourceWorkerGate::Pending candidate) {
    const auto existing = std::find_if(
        candidates.begin(), candidates.end(),
        [&candidate](const UiResourceWorkerGate::Pending& current) {
            return current.handle == candidate.handle;
        });
    if (existing != candidates.end()) {
        *existing = std::move(candidate);
    } else {
        candidates.push_back(std::move(candidate));
    }
}

// The lease is captured when a Worker callback is posted, rather than being
// constructed inside that callback. Dispatcher cancellation destroys queued
// callbacks without invoking them, so this is what releases both the gate and
// any resource-scoped staging reservation on every terminal path.
class UiResourceWorkerGateLease final {
public:
    UiResourceWorkerGateLease(
        std::shared_ptr<anomaly::UiResourceRegistry> registry,
        std::shared_ptr<UiResourceWorkerGate> gate, const std::uint64_t resource_id,
        const anomaly::UiResourceKind kind) noexcept
        : registry_(std::move(registry)), gate_(std::move(gate)), resource_id_(resource_id),
          kind_(kind) {}

    ~UiResourceWorkerGateLease() { Finalize(true); }

    UiResourceWorkerGateLease(const UiResourceWorkerGateLease&) = delete;
    UiResourceWorkerGateLease& operator=(const UiResourceWorkerGateLease&) = delete;

    void Activate() noexcept { active_ = true; }
    void Complete() noexcept { Finalize(false); }
    void Fail() noexcept { Finalize(true); }

    [[nodiscard]] std::optional<UiResourceWorkerGate::Pending> CurrentLive() const noexcept {
        std::vector<UiResourceWorkerGate::Pending> candidates;
        try {
            if (!active_ || gate_ == nullptr || registry_ == nullptr) return std::nullopt;
            {
                std::scoped_lock lock(gate_->mutex);
                const auto found = gate_->pending_resources.find(resource_id_);
                if (found == gate_->pending_resources.end()) return std::nullopt;
                candidates = found->second;
            }
            for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); ++candidate) {
                if (candidate->scope == nullptr || !candidate->handle) continue;
                const auto state = registry_->ResourceState(candidate->scope, candidate->handle);
                if (state && state->kind == kind_ && state->resource_id == resource_id_ &&
                    state->state != anomaly::UiResourceState::Revoked) {
                    return *candidate;
                }
            }
        } catch (...) {
        }
        return std::nullopt;
    }

private:
    void Finalize(const bool mark_failed) noexcept {
        std::vector<UiResourceWorkerGate::Pending> pending;
        try {
            if (!active_ || gate_ == nullptr) return;
            active_ = false;
            std::scoped_lock lock(gate_->mutex);
            const auto found = gate_->pending_resources.find(resource_id_);
            if (found == gate_->pending_resources.end()) return;
            pending = std::move(found->second);
            gate_->pending_resources.erase(found);
        } catch (...) {
            return;
        }
        if (!mark_failed || registry_ == nullptr) return;
        for (auto candidate = pending.rbegin(); candidate != pending.rend(); ++candidate) {
            if (candidate->scope == nullptr || !candidate->handle) continue;
            if (kind_ == anomaly::UiResourceKind::Font &&
                registry_->MarkFontFailed(candidate->scope, candidate->handle)) {
                return;
            }
            if (kind_ == anomaly::UiResourceKind::Texture &&
                registry_->MarkTextureFailed(candidate->scope, candidate->handle)) {
                return;
            }
        }
    }

    std::shared_ptr<anomaly::UiResourceRegistry> registry_;
    std::shared_ptr<UiResourceWorkerGate> gate_;
    std::uint64_t resource_id_{};
    anomaly::UiResourceKind kind_{anomaly::UiResourceKind::Font};
    bool active_{};
};

struct UiResourceWorkerStagingAdmission final {
    std::shared_ptr<anomaly::UiResourceRegistry> registry;
    std::shared_ptr<anomaly::PluginScope> scope;
    anomaly::UiResourceHandle handle;
    std::size_t base_bytes{};

    [[nodiscard]] static bool Reserve(void* user, const std::size_t byte_count) noexcept {
        const auto* admission = static_cast<const UiResourceWorkerStagingAdmission*>(user);
        if (admission == nullptr || admission->registry == nullptr || admission->scope == nullptr ||
            !admission->handle ||
            byte_count > (std::numeric_limits<std::size_t>::max)() - admission->base_bytes) {
            return false;
        }
        return admission->registry->ReserveResourceStaging(
            admission->scope, admission->handle, admission->base_bytes + byte_count);
    }

    [[nodiscard]] anomaly::UiResourceAllocationAdmission Callback() noexcept {
        return {this, &Reserve};
    }
};

PluginManager::PluginManager(
    std::filesystem::path root,
    std::filesystem::path plugin_directory,
    anomaly::CoreMemoryServices memory_services,
    PluginCallbackBudgets callback_budgets,
    std::shared_ptr<anomaly::StructuredLogger> logger,
    anomaly::HotkeyDispatcher input_dispatcher,
    UiResourceWorkerDispatcher ui_resource_worker_dispatcher,
    anomaly::IpcPost ipc_post,
    PluginLoadPredicate load_predicate,
    PluginActivationObserver activation_observer)
    : root_(std::filesystem::absolute(root)),
      plugin_directory_(plugin_directory.is_absolute() ? std::move(plugin_directory)
                                                        : root_ / plugin_directory),
      cache_directory_(plugin_directory_ / L".cache" / std::to_wstring(GetCurrentProcessId())),
      memory_services_(anomaly::NormalizeCoreMemoryServices(std::move(memory_services))),
      callback_budgets_(callback_budgets),
      logger_(std::move(logger)),
      shadow_store_(cache_directory_ / L"packages"),
       file_watcher_(plugin_directory_),
      enablement_store_(root_ / L"config" / L"plugin-enablement.json"),
       ui_window_state_file_(root_ / L"state" / L"ui-window-state.json"),
       input_service_(std::move(input_dispatcher)),
       ui_resource_worker_dispatcher_(std::move(ui_resource_worker_dispatcher)),
       ui_resource_worker_gate_(std::make_shared<UiResourceWorkerGate>()),
       load_predicate_(std::move(load_predicate)),
       activation_observer_(std::move(activation_observer)),
       lifecycle_ledger_(std::make_shared<anomaly::ResourceLedger>()),
      platform_services_(std::make_unique<anomaly::ScopedPlatformServices>(
          memory_services_, lifecycle_ledger_)),
      ipc_registry_(std::make_unique<anomaly::IpcRegistry>(std::move(ipc_post))) {
    if (g_manager != nullptr || g_process_quarantined) {
        throw std::logic_error("only one PluginManager may be active at a time");
    }
    cache_owner_ = AcquirePluginCacheOwner(plugin_directory_ / L".cache", cache_directory_);
    g_manager = this;
    LoadPersistentUiWindowState();
    std::string state_error;
    if (!enablement_store_.Load(&state_error)) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "plugin enablement load failed: " + state_error);
    }
}

PluginManager::~PluginManager() {
    SavePersistentUiWindowState(true);
    UnloadAll();
    // A quarantined generation can still be executing inside its DLL.  Retain
    // the broker too, so that a late callback cannot dereference a stale
    // service context while the process is winding down.
    if (quarantined_plugins_.empty()) {
        platform_services_.reset();
    } else {
        static_cast<void>(platform_services_.release());
    }
    std::error_code error;
    std::filesystem::remove_all(cache_directory_, error);
    // A quarantined plugin may still own a thread executing inside its DLL. Keep
    // both its service context and module mapping alive until process teardown.
    const bool process_quarantined = !quarantined_plugins_.empty();
    if (process_quarantined) g_process_quarantined = true;
    for (auto& plugin : quarantined_plugins_) static_cast<void>(plugin.release());
    quarantined_plugins_.clear();
    if (process_quarantined) {
        // A quarantined callback may still read its shadow package. Keep the
        // ownership lock alive with the deliberately retained module mapping.
        static_cast<void>(cache_owner_.release());
    } else {
        cache_owner_.reset();
        error.clear();
        std::filesystem::remove_all(cache_directory_, error);
        RemoveEmptyPluginCacheRoot(plugin_directory_ / L".cache");
    }
    if (g_manager == this) g_manager = nullptr;
}

void PluginManager::Log(AnomalyCoreLogLevelV1 level, std::string message) {
    LogImpl(level, std::move(message), {}, 0);
}

void PluginManager::LogPlugin(
    AnomalyCoreLogLevelV1 level,
    std::string message,
    std::string_view plugin_id,
    std::uint64_t generation) {
    LogImpl(level, std::move(message), std::string(plugin_id), generation);
}

void PluginManager::SetTranslator(
    std::shared_ptr<const anomaly::Translator> translator) noexcept {
    if (!plugins_.empty() || !quarantined_plugins_.empty()) return;
    translator_ = std::move(translator);
}

void PluginManager::LogImpl(
    AnomalyCoreLogLevelV1 level,
    std::string message,
    std::string plugin_id,
    std::uint64_t generation) {
    std::ostringstream line;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    line << std::setfill('0') << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute
         << ':' << std::setw(2) << time.wSecond << " [pid " << GetCurrentProcessId() << "] ["
         << LevelName(level) << "] " << message;
    const std::string rendered = line.str();
    {
        std::scoped_lock events_lock(events_mutex_);
        events_.push_back(rendered);
        if (events_.size() > 256) events_.erase(events_.begin(), events_.begin() + 64);
    }
    if (logger_ != nullptr) {
        anomaly::LogDetails details;
        details.thread_domain = g_log_thread_domain.Get();
        details.event_id = plugin_id.empty() ? "plugin.manager" : "plugin.host";
        if (!plugin_id.empty()) {
            details.plugin = anomaly::PluginLogOwner{
                std::move(plugin_id), generation};
        }
        static_cast<void>(logger_->Log(
            StructuredLevel(level), "plugin-manager", std::move(message),
            std::move(details)));
    }
}

std::vector<std::string> PluginManager::Events() const {
    std::scoped_lock events_lock(events_mutex_);
    return events_;
}

anomaly::PluginScope::CallbackLease PluginManager::AcquireCallback(
    std::string_view plugin_id, std::uint64_t generation) noexcept {
    const auto found = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin->view.id == plugin_id && plugin->view.generation == generation;
    });
    if (found == plugins_.end() || (*found)->scope == nullptr) return {};
    return (*found)->scope->AcquireCallback(generation);
}

void PluginManager::SetQueuedCallbackCanceller(
    std::function<void(std::string_view, std::uint64_t)> canceller) {
    queued_callback_canceller_ = std::move(canceller);
}

void PluginManager::SetCallbackEvidenceObserver(
    std::function<void(const PluginCallbackEvidence&)> observer) {
    callback_evidence_observer_ = std::move(observer);
}

std::vector<PluginStopDiagnostic> PluginManager::StopDiagnostics() const {
    std::scoped_lock lock(stop_diagnostics_mutex_);
    return stop_diagnostics_;
}

bool PluginManager::LoadAllowed(const anomaly::PluginCatalogEntry& entry) const {
    return entry.manifest && (!load_predicate_ || load_predicate_(*entry.manifest));
}

void PluginManager::PublishSuspended(const anomaly::PluginCatalogEntry& entry) {
    if (!entry.manifest) return;
    PluginView view;
    view.id = entry.manifest->id;
    view.name = entry.manifest->name;
    view.author = entry.manifest->author;
    view.version = entry.manifest->version.ToString();
    view.source = entry.entry_file;
    view.package_directory = entry.package_root;
    view.visible = false;
    view.enabled = false;
    view.state = "suspended";
    view.status_reason = "suspended by Runtime recovery policy";
    disabled_plugins_[view.id] = std::move(view);
}

bool PluginManager::LoadCatalogEntry(const anomaly::PluginCatalogEntry& entry) {
    if (!LoadAllowed(entry)) {
        PublishSuspended(entry);
        return false;
    }
    anomaly::PluginShadowResult staged = shadow_store_.Stage(entry);
    if (!staged.Ok()) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
            "package shadow failed for " + std::string(entry.Id()) + ": " + staged.error);
        return false;
    }
    const std::filesystem::path source = entry.entry_file;
    const std::filesystem::path binary = staged.shadow->entry_file;
    const std::filesystem::path package = entry.package_root;
    const std::string plugin_id(staged.shadow->manifest.id);
    const std::uint64_t generation = staged.shadow->generation;
    const auto publish_activation = [&](bool entering) noexcept {
        try {
            if (activation_observer_) {
                activation_observer_(plugin_id, generation, entering);
            }
        } catch (...) {
        }
    };
    publish_activation(true);
    try {
        const bool loaded = LoadBinary(
            source, binary, package, std::move(*staged.shadow));
        publish_activation(false);
        return loaded;
    } catch (...) {
        publish_activation(false);
        throw;
    }
}

bool PluginManager::Activate(LoadedPlugin& plugin) {
    if (plugin.scope == nullptr) return false;
    plugin.plugin_context = nullptr;
    plugin.ui_stack.Reset();
    plugin.service_context.manager = this;
    plugin.service_context.ui_proxy_context = &plugin.ui_proxy_context;
    plugin.service_context.ui_stack = &plugin.ui_stack;
    plugin.service_context.plugin_id = plugin.view.id;
    plugin.service_context.generation = plugin.view.generation;
    plugin.service_context.package_directory = plugin.view.package_directory;
    plugin.service_context.state_directory = ValidPluginStateId(plugin.view.id)
        ? (root_ / L"state" / L"plugins" /
              WideUtf8(plugin.view.id.c_str())).lexically_normal()
        : std::filesystem::path{};
    plugin.service_context.configuration_directory = ValidPluginStateId(plugin.view.id)
        ? (root_ / L"config" / L"plugins" /
              WideUtf8(plugin.view.id.c_str())).lexically_normal()
        : std::filesystem::path{};
    plugin.service_context.scope = plugin.scope;
    plugin.service_context.platform = platform_services_.get();
    plugin.service_context.ipc = ipc_registry_.get();
    plugin.service_context.ipc_dependencies.clear();
    for (const auto& dependency : plugin.shadow_generation.manifest.dependencies) {
        plugin.service_context.ipc_dependencies.push_back(dependency.id);
    }
    plugin.service_context.ui_resources = ui_resources_.get();
    plugin.service_context.input = &input_service_;
    plugin.service_context.open_windows.clear();
    plugin.service_context.pushed_fonts.clear();
    plugin.service_context.capabilities =
        anomaly::ResolvePluginCapabilityGrant(&plugin.shadow_generation.manifest);
    if (!plugin.localization_catalog_loaded) {
        plugin.service_context.localization_locale = translator_ == nullptr
            ? anomaly::Locale::EnUs
            : translator_->locale();
        anomaly::PluginCatalogLoadResult localization = anomaly::LoadPluginCatalog(
            plugin.service_context.localization_locale,
            plugin.shadow_generation.package_root);
        plugin.service_context.localization_catalog = std::move(localization.catalog);
        plugin.service_context.localization_fallback_logged.store(
            false, std::memory_order_relaxed);
        plugin.localization_catalog_loaded = true;
        if (!localization.diagnostics.empty()) {
            const anomaly::CatalogDiagnostic& diagnostic = localization.diagnostics.front();
            std::string detail = diagnostic.message;
            if (!diagnostic.path.empty()) detail += " path=" + diagnostic.path;
            ReportLocalizationFallback(plugin.service_context, std::move(detail));
        }
    }
    if (!plugin.capability_audit_logged) {
        for (const auto& audit : plugin.service_context.capabilities.Audits()) {
            if (audit.code == anomaly::PluginCapabilityAuditCode::UnknownCapability) {
                Log(
                    ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                    "plugin capability audit: unknown capability: plugin=" +
                        plugin.view.id + " capability=" + audit.capability);
            } else {
                Log(
                    ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                    "plugin capability audit: required service missing capability: plugin=" +
                        plugin.view.id + " service=" + audit.service +
                        " capability=" + audit.capability);
            }
        }
        plugin.capability_audit_logged = true;
    }
    plugin.service_context.core = {
        sizeof(AnomalyCoreServiceV1), ANOMALY_CORE_SERVICE_V1_VERSION,
        &plugin.service_context,
        LogV1, ReadV1, WriteV1, PluginDirectoryV1};
    plugin.service_context.plugin_state = {
        sizeof(AnomalyPluginStateServiceV1), ANOMALY_PLUGIN_STATE_SERVICE_V1_VERSION,
        &plugin.service_context, PluginStateDirectoryV1};
    plugin.service_context.config = {
        sizeof(AnomalyConfigServiceV1), ANOMALY_CONFIG_SERVICE_V1_VERSION,
        &plugin.service_context,
        RegisterConfigSchemaV1, UnregisterConfigSchemaV1, ReadConfigV1, WriteConfigV1,
        MigrateConfigV1};
    plugin.service_context.storage = {
        sizeof(AnomalyStorageServiceV1), ANOMALY_STORAGE_SERVICE_V1_VERSION,
        &plugin.service_context, ReadStorageV1, WriteStorageV1, RemoveStorageV1};
    plugin.service_context.runtime_info = {
        sizeof(AnomalyRuntimeInfoServiceV1), ANOMALY_RUNTIME_INFO_SERVICE_V1_VERSION,
        &plugin.service_context, RuntimeInfoV1, RuntimeVersionV1};
    plugin.service_context.localization = {
        sizeof(AnomalyLocalizationServiceV1), ANOMALY_LOCALIZATION_SERVICE_V1_VERSION,
        &plugin.service_context, LocaleV1, TranslateV1};
    plugin.service_context.diagnostics = {
        sizeof(AnomalyDiagnosticsServiceV1), ANOMALY_DIAGNOSTICS_SERVICE_V1_VERSION,
        &plugin.service_context,
        RegisterSelfTestV1, UnregisterSelfTestV1, RunSelfTestV1, DiagnosticsSnapshotV1};
    plugin.service_context.scheduler = {
        sizeof(AnomalySchedulerServiceV1), ANOMALY_SCHEDULER_SERVICE_V1_VERSION,
        &plugin.service_context, ScheduleV1, CancelTaskV1};
    plugin.service_context.ipc_service = {
        sizeof(AnomalyIpcServiceV1), ANOMALY_IPC_SERVICE_V1_VERSION,
        &plugin.service_context, RegisterIpcEndpointV1, UnregisterIpcEndpointV1,
        InvokeIpcV1, InvokeIpcAsyncV1, CancelIpcV1, SubscribeIpcV1,
        UnsubscribeIpcV1, PublishIpcV1};
    plugin.service_context.commands = {
        sizeof(AnomalyCommandsServiceV1), ANOMALY_COMMANDS_SERVICE_V1_VERSION,
        &plugin.service_context, RegisterCommandV1, UnregisterCommandV1, InvokeCommandV1};
    plugin.service_context.notifications = {
        sizeof(AnomalyNotificationsServiceV1), ANOMALY_NOTIFICATIONS_SERVICE_V1_VERSION,
        &plugin.service_context, PostNotificationV1, DismissNotificationV1};
    plugin.service_context.signature = {
        sizeof(AnomalySignatureServiceV1), ANOMALY_SIGNATURE_SERVICE_V1_VERSION,
        &plugin.service_context, ResolveSignatureV1};
    plugin.service_context.hook = {
        sizeof(AnomalyHookServiceV1), ANOMALY_HOOK_SERVICE_V1_VERSION,
        &plugin.service_context,
        CreateHookV1, ReleaseHookV1, BeginHookCallbackV1, EndHookCallbackV1};
    plugin.service_context.patch = {
        sizeof(AnomalyPatchServiceV1), ANOMALY_PATCH_SERVICE_V1_VERSION,
        &plugin.service_context, ApplyPatchV1, ReleasePatchV1};
    plugin.service_context.window = {
        sizeof(AnomalyWindowServiceV1), ANOMALY_WINDOW_SERVICE_V1_VERSION,
        &plugin.service_context,
        RegisterWindowV1, ReleaseWindowV1, SetWindowOpenV1, ToggleWindowV1,
        WindowStateV1, BeginWindowV1, EndWindowV1};
    plugin.service_context.font = {
        sizeof(AnomalyFontServiceV1), ANOMALY_FONT_SERVICE_V1_VERSION,
        &plugin.service_context, RequestFontV1, ReleaseFontV1, FontStateV1, PushFontV1, PopFontV1};
    plugin.service_context.texture = {
        sizeof(AnomalyTextureServiceV1), ANOMALY_TEXTURE_SERVICE_V1_VERSION,
        &plugin.service_context,
        RequestTextureV1, ReleaseTextureV1, TextureStateV1, DrawTextureV1};
    plugin.service_context.input_service = {
        sizeof(AnomalyInputServiceV1), ANOMALY_INPUT_SERVICE_V1_VERSION,
        &plugin.service_context,
        InputSnapshotV1, WasPressedV1, RegisterHotkeyV1, ReleaseHotkeyV1, InputCaptureStateV1};
    plugin.service_context.ui = &plugin.ui_proxy;
    plugin.host_api = {
        sizeof(AnomalyHostApiV1), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        &plugin.service_context,
        {sizeof(AnomalyAllocatorV1), 0, &plugin.service_context,
            AllocateV1, ReallocateV1, ReleaseV1},
        QueryServiceV1};
    plugin.ui_proxy_context.scope = plugin.scope;
    plugin.ui_proxy_context.plugin_id = plugin.view.id;
    plugin.ui_proxy_context.generation = plugin.view.generation;
    plugin.ui_proxy_context.ui_stack = &plugin.ui_stack;
    plugin.ui_proxy_context.service = ui_service_;
    plugin.ui_proxy = MakeUiProxy(&plugin.ui_proxy_context);
    g_loading_plugin_id.Get() = plugin.view.id;
    g_loading_package_directory.Get() = plugin.view.package_directory;
    AnomalyStatusV1 load_status = StatusV1(ANOMALY_STATUS_V1_FAILED);
    {
        auto callback = plugin.scope->AcquireCallback(plugin.view.generation);
        if (callback) {
            ScopedPluginCallback callback_scope(plugin.scope, plugin.view.generation, false);
            try {
                load_status = plugin.descriptor_v1.on_load(
                    &plugin.host_api, &plugin.plugin_context);
            } catch (...) {
                Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "exception while loading ABI v1 plugin " + plugin.view.id);
            }
        } else {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "plugin scope rejected ABI v1 load: " + plugin.view.id);
        }
    }
    g_loading_plugin_id.Get().clear();
    g_loading_package_directory.Get().clear();
    if (load_status.code == ANOMALY_STATUS_V1_UNAVAILABLE) {
        plugin.plugin_context = nullptr;
        plugin.waiting_for_service = true;
        plugin.started = false;
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "ABI v1 plugin waiting for service: " + plugin.view.id);
        return true;
    }
    if (load_status.code != ANOMALY_STATUS_V1_OK) {
        plugin.plugin_context = nullptr;
        plugin.waiting_for_service = false;
        return false;
    }
    if (plugin.descriptor_v1.on_start != nullptr) {
        AnomalyStatusV1 start_status = StatusV1(ANOMALY_STATUS_V1_FAILED);
        auto callback = plugin.scope->AcquireCallback(plugin.view.generation);
        if (callback) {
            ScopedPluginCallback callback_scope(plugin.scope, plugin.view.generation, false);
            try { start_status = plugin.descriptor_v1.on_start(plugin.plugin_context); }
            catch (...) {}
        }
        if (start_status.code != ANOMALY_STATUS_V1_OK) {
            auto unload_callback = plugin.scope->AcquireCallback(plugin.view.generation);
            if (unload_callback) {
                ScopedPluginCallback callback_scope(plugin.scope, plugin.view.generation, false);
                try { plugin.descriptor_v1.on_unload(plugin.plugin_context); } catch (...) {}
            }
            plugin.plugin_context = nullptr;
            return false;
        }
    }
    plugin.waiting_for_service = false;
    plugin.started = true;
    ReconcileWindowVisibility(plugin);
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "ABI v1 plugin started: " + plugin.view.id);
    return true;
}

bool PluginManager::LoadBinary(
    const std::filesystem::path& source,
    const std::filesystem::path& binary,
    const std::filesystem::path& package_directory,
    anomaly::PluginShadowGeneration shadow_generation) {
    const auto discard_shadow = [&] { shadow_store_.Retire(shadow_generation); };

    const anomaly::PluginCapabilityGrant grant =
        anomaly::ResolvePluginCapabilityGrant(&shadow_generation.manifest);
    if (!grant.IsEnforceable()) {
        for (const auto& audit : grant.Audits()) {
            const char* code = audit.code ==
                    anomaly::PluginCapabilityAuditCode::UnknownCapability
                ? "unknown-capability"
                : audit.code ==
                        anomaly::PluginCapabilityAuditCode::RequiredServiceMissingCapability
                    ? "required-service-capability-missing"
                    : "required-service-mapping-missing";
            Log(
                ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                "plugin manifest capability denied: plugin=" +
                    shadow_generation.plugin_id + " code=" + code +
                    " service=" + audit.service +
                    " capability=" + audit.capability);
        }
        discard_shadow();
        return false;
    }

    const anomaly::PluginNativeDependencyPreflightResult dependency_preflight =
        anomaly::PreflightPluginNativeDependencies(binary);
    if (!dependency_preflight.Ok()) {
        for (const auto& diagnostic : dependency_preflight.diagnostics) {
            std::string message = "plugin native dependency denied: package=" +
                Utf8(package_directory) + " module=" +
                Utf8(std::filesystem::path(diagnostic.module_name)) + " code=" +
                std::string(anomaly::PluginNativeDependencyDiagnosticCodeName(diagnostic.code)) +
                " requester=" + Utf8(diagnostic.requester) +
                " detail=" + diagnostic.message;
            if (!diagnostic.expected_path.empty()) {
                message += " expected=" + Utf8(diagnostic.expected_path);
            }
            if (!diagnostic.loaded_path.empty()) {
                message += " loaded=" + Utf8(diagnostic.loaded_path);
            }
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, std::move(message));
        }
        discard_shadow();
        return false;
    }

    const DLL_DIRECTORY_COOKIE search_cookie = AddDllDirectory(binary.parent_path().c_str());
    const HMODULE module = LoadLibraryExW(
        binary.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (search_cookie != nullptr) RemoveDllDirectory(search_cookie);
    if (module == nullptr) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "load failed for " + source.string() + ": " + std::to_string(GetLastError()));
        discard_shadow();
        return false;
    }
    const auto entry_v1 = reinterpret_cast<AnomalyPluginEntryV1Fn>(
        GetProcAddress(module, ANOMALY_PLUGIN_V1_ENTRY_NAME));
    if (entry_v1 != nullptr) {
        AnomalyPluginDescriptorV1 descriptor{};
        descriptor.struct_size = sizeof(descriptor);
        AnomalyStatusV1 entry_status = StatusV1(ANOMALY_STATUS_V1_FAILED);
        try { entry_status = entry_v1(&descriptor); } catch (...) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "exception in ABI v1 plugin entry: " + source.string());
        }
        constexpr std::size_t minimum_size = offsetof(AnomalyPluginDescriptorV1, on_draw) +
            sizeof(descriptor.on_draw);
        const bool valid = entry_status.code == ANOMALY_STATUS_V1_OK &&
            descriptor.struct_size >= minimum_size &&
            descriptor.api_major == ANOMALY_PLUGIN_API_V1_MAJOR &&
            descriptor.api_minor <= ANOMALY_PLUGIN_API_V1_MINOR &&
            descriptor.id.data != nullptr && descriptor.id.size != 0 &&
            descriptor.name.data != nullptr && descriptor.name.size != 0 &&
            descriptor.version.data != nullptr && descriptor.version.size != 0 &&
            descriptor.on_load != nullptr && descriptor.on_unload != nullptr;
        if (!valid) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "invalid ABI v1 plugin metadata: " + source.string());
            FreeLibrary(module);
            discard_shadow();
            return false;
        }
        auto copy_view = [](AnomalyStringViewV1 value) {
            return value.data == nullptr ? std::string{} : std::string(value.data, value.size);
        };
        auto plugin = std::make_unique<LoadedPlugin>();
        plugin->module = module;
        plugin->descriptor_v1 = descriptor;
        plugin->view.id = copy_view(descriptor.id);
        plugin->view.name = copy_view(descriptor.name);
        plugin->view.author = copy_view(descriptor.author);
        plugin->view.version = copy_view(descriptor.version);
        plugin->view.source = source;
        plugin->view.package_directory = package_directory;
        plugin->view.visibility_control = descriptor.on_draw != nullptr;
        plugin->view.generation = shadow_generation.generation;
        if (plugin->view.id != shadow_generation.plugin_id) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                "plugin identity mismatch: manifest=" + shadow_generation.plugin_id +
                    " descriptor=" + plugin->view.id);
            FreeLibrary(module);
            discard_shadow();
            return false;
        }
        plugin->scope = std::make_shared<anomaly::PluginScope>(
            lifecycle_ledger_, plugin->view.id, plugin->view.generation);
        static_cast<void>(plugin->scope->Register(
            anomaly::PluginResourceKind::Task, "plugin.update"));
        if (plugin->view.visibility_control) {
            static_cast<void>(plugin->scope->Register(
                anomaly::PluginResourceKind::Ui, "plugin.draw"));
        }
        plugin->ui_proxy_context.service = ui_service_;
        plugin->ui_proxy_context.ui_stack = &plugin->ui_stack;
        plugin->ui_proxy_context.scope = plugin->scope;
        plugin->ui_proxy_context.plugin_id = plugin->view.id;
        plugin->ui_proxy_context.generation = plugin->view.generation;
        plugin->ui_proxy = MakeUiProxy(&plugin->ui_proxy_context);
        plugin->shadow = binary;
        plugin->shadow_generation = std::move(shadow_generation);
        if (std::any_of(plugins_.begin(), plugins_.end(), [&](const auto& loaded) {
                return loaded->view.id == plugin->view.id;
        }) || std::any_of(quarantined_plugins_.begin(), quarantined_plugins_.end(),
                [&](const auto& loaded) { return loaded->view.id == plugin->view.id; })) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "duplicate plugin id: " + plugin->view.id);
            static_cast<void>(plugin->scope->RevokeAll());
            FreeLibrary(module);
            discard_shadow();
            return false;
        }
        if (!Activate(*plugin)) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "ABI v1 plugin rejected activation: " + plugin->view.id);
            static_cast<void>(plugin->scope->RevokeAll());
            FreeLibrary(module);
            discard_shadow();
            return false;
        }
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            "loaded ABI v1 " + plugin->view.name + " " + plugin->view.version);
        plugins_.push_back(std::move(plugin));
        return true;
    }
    Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "plugin does not export the current ABI entry: " + source.string());
    FreeLibrary(module);
    discard_shadow();
    return false;
}

void PluginManager::LoadAll() {
    std::error_code error;
    std::filesystem::create_directories(plugin_directory_, error);
    const anomaly::PluginCatalogSnapshot catalog =
        anomaly::DiscoverPluginCatalog(plugin_directory_);
    const anomaly::PluginDependencyPlan plan =
        anomaly::ResolvePluginDependencies(catalog);
    disabled_plugins_.clear();
    const auto enablement = enablement_store_.Resolve(catalog);
    const auto disabled_view = [&](const anomaly::PluginCatalogEntry& entry,
                                   std::string state, std::string reason) {
        if (!entry.manifest) return;
        PluginView view;
        view.id = entry.manifest->id;
        view.name = entry.manifest->name;
        view.author = entry.manifest->author;
        view.version = entry.manifest->version.ToString();
        view.source = entry.entry_file;
        view.package_directory = entry.package_root;
        view.visible = false;
        view.enabled = false;
        view.state = std::move(state);
        view.status_reason = std::move(reason);
        disabled_plugins_[view.id] = std::move(view);
    };
    bool loaded = true;
    for (const anomaly::PluginCatalogEntry& entry : catalog.Entries()) {
        if (entry.LoadCandidate()) continue;
        const std::string id = entry.manifest ? entry.manifest->id : entry.package_root.filename().string();
        for (const anomaly::PluginCatalogIssue& issue : entry.issues) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                "catalog rejected " + id + " [" + issue.code + "] " + issue.message);
        }
    }
    for (const anomaly::PluginDependencyNode& node : plan.nodes) {
        if (node.state == anomaly::PluginDependencyState::Ready) continue;
        for (const std::string& diagnostic : node.diagnostics) {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "dependency blocked " + node.id + ": " + diagnostic);
        }
    }
    for (const std::string& id : plan.load_order) {
        const anomaly::PluginCatalogEntry* entry = catalog.Find(id);
        if (entry != nullptr && !LoadAllowed(*entry)) {
            PublishSuspended(*entry);
            continue;
        }
        const auto decision = enablement.find(id);
        if (entry != nullptr && decision != enablement.end() && !decision->second.enabled) {
            disabled_view(*entry, "disabled", decision->second.reason);
            continue;
        }
        loaded = entry != nullptr && LoadCatalogEntry(*entry) && loaded;
    }

    if (plan.load_order.empty()) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "plugin directory is ready: " + plugin_directory_.string());
    }
    file_watcher_.ResetBaseline();
    static_cast<void>(file_watcher_.PollForTests(std::chrono::steady_clock::now()));
    if (!loaded) Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "one or more plugin packages were not activated");
}

void PluginManager::ReloadAll() {
    UnloadAll();
    LoadAll();
}

bool PluginManager::Reload(std::string_view plugin_id) {
    const auto loaded = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin->view.id == plugin_id;
    });
    std::filesystem::path package_directory;
    if (loaded != plugins_.end()) {
        package_directory = (*loaded)->view.package_directory;
    } else {
        const auto disabled = disabled_plugins_.find(std::string(plugin_id));
        if (disabled == disabled_plugins_.end()) return false;
        package_directory = disabled->second.package_directory;
    }
    const std::string package_name = Utf8(package_directory.filename());
    if (package_name.empty()) return false;
    if (!ReloadPackages({package_name})) return false;
    const auto views = Plugins();
    return std::any_of(views.begin(), views.end(), [&](const auto& plugin) {
        return plugin.id == plugin_id;
    });
}

bool PluginManager::SetEnabled(std::string_view plugin_id, bool enabled) {
    const auto catalog = anomaly::DiscoverPluginCatalog(plugin_directory_);
    std::string error;
    if (!enablement_store_.SetPluginEnabled(catalog, plugin_id, enabled, &error)) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "enablement update failed: " + error);
        return false;
    }
    const bool reconciled = ReconcileEnablement(catalog);
    Log(reconciled ? ANOMALY_CORE_LOG_LEVEL_V1_INFO : ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
        "plugin enablement applied without restarting unrelated plugins: " +
            std::string(plugin_id));
    const auto views = Plugins();
    const auto found = std::find_if(views.begin(), views.end(), [&](const auto& plugin) {
        return plugin.id == plugin_id;
    });
    const bool target_quarantined = std::any_of(
        quarantined_plugins_.begin(), quarantined_plugins_.end(),
        [&](const auto& plugin) { return plugin->view.id == plugin_id; });
    return !target_quarantined && found != views.end() && found->enabled == enabled;
}

bool PluginManager::ReconcileEnablement(
    const anomaly::PluginCatalogSnapshot& catalog) {
    const auto decisions = enablement_store_.Resolve(catalog);
    const auto publish_view = [&](const anomaly::PluginCatalogEntry& entry,
                                  std::string state, std::string reason) {
        if (!entry.manifest) return;
        PluginView view;
        view.id = entry.manifest->id;
        view.name = entry.manifest->name;
        view.author = entry.manifest->author;
        view.version = entry.manifest->version.ToString();
        view.source = entry.entry_file;
        view.package_directory = entry.package_root;
        view.visible = false;
        view.enabled = false;
        view.state = std::move(state);
        view.status_reason = std::move(reason);
        disabled_plugins_[view.id] = std::move(view);
    };
    const auto active = [&](std::string_view id) {
        return std::any_of(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
            return plugin->view.id == id;
        });
    };
    const auto quarantined = [&](std::string_view id) {
        return std::any_of(
            quarantined_plugins_.begin(), quarantined_plugins_.end(),
            [&](const auto& plugin) { return plugin->view.id == id; });
    };

    // Reconcile desired state against the generations that actually exist. This
    // intentionally does not rely on a before/after boolean delta: repeating an
    // enable request must retry a generation whose previous activation failed.
    const anomaly::PluginDependencyPlan plan = anomaly::ResolvePluginDependencies(catalog);
    std::unordered_set<std::string> retained;
    for (const auto& plugin : plugins_) {
        const auto decision = decisions.find(plugin->view.id);
        const auto* node = plan.Find(plugin->view.id);
        const auto* entry = catalog.Find(plugin->view.id);
        if (decision != decisions.end() && decision->second.enabled &&
            node != nullptr && node->state == anomaly::PluginDependencyState::Ready &&
            entry != nullptr && LoadAllowed(*entry)) {
            retained.insert(plugin->view.id);
        }
    }
    bool narrowed = true;
    while (narrowed) {
        narrowed = false;
        const std::vector<std::string> candidates(retained.begin(), retained.end());
        for (const std::string& id : candidates) {
            const auto* entry = catalog.Find(id);
            const bool missing_dependency = entry == nullptr || !entry->manifest ||
                std::any_of(
                    entry->manifest->dependencies.begin(),
                    entry->manifest->dependencies.end(),
                    [&](const anomaly::PluginDependencyManifest& dependency) {
                        return !dependency.optional && !retained.contains(dependency.id);
                    });
            if (missing_dependency && retained.erase(id) != 0) narrowed = true;
        }
    }
    std::unordered_set<std::string> unload_ids;
    std::vector<std::string> fallback_stop_order;
    for (auto plugin = plugins_.rbegin(); plugin != plugins_.rend(); ++plugin) {
        if (!retained.contains((*plugin)->view.id)) {
            unload_ids.insert((*plugin)->view.id);
            fallback_stop_order.push_back((*plugin)->view.id);
        }
    }

    const auto manifest_entry = [&](std::string_view id) {
        const auto found = std::find_if(
            catalog.Entries().begin(), catalog.Entries().end(),
            [&](const anomaly::PluginCatalogEntry& entry) {
                return entry.manifest && entry.manifest->id == id;
            });
        return found == catalog.Entries().end() ? nullptr : &*found;
    };
    std::unordered_set<std::string> protected_dependencies;
    std::function<void(std::string_view)> protect_required_dependencies =
        [&](std::string_view consumer_id) {
            const auto* entry = manifest_entry(consumer_id);
            if (entry == nullptr || !entry->manifest) return;
            for (const auto& dependency : entry->manifest->dependencies) {
                if (dependency.optional ||
                    !protected_dependencies.insert(dependency.id).second) {
                    continue;
                }
                protect_required_dependencies(dependency.id);
            }
        };
    for (const auto& plugin : quarantined_plugins_) {
        protect_required_dependencies(plugin->view.id);
    }

    bool stopped = true;
    const auto stop_one = [&](const std::string& id) {
        if (unload_ids.erase(id) == 0) return;
        if (protected_dependencies.contains(id)) {
            stopped = false;
            return;
        }
        const auto found = std::find_if(
            plugins_.begin(), plugins_.end(),
            [&](const auto& plugin) { return plugin->view.id == id; });
        if (found == plugins_.end()) return;
        const std::size_t index = static_cast<std::size_t>(
            std::distance(plugins_.begin(), found));
        if (!UnloadIndicesWithDeadline(
                {index}, true, std::chrono::milliseconds(1000))) {
            stopped = false;
            protect_required_dependencies(id);
        }
    };
    for (const std::string& id : plan.stop_order) stop_one(id);
    for (const std::string& id : fallback_stop_order) stop_one(id);

    // Refresh every catalog view, including entries whose resolved boolean did
    // not change but whose explanation changed from a default to an override or
    // to a dependency-derived reason.
    for (const auto& [id, decision] : decisions) {
        const auto* entry = catalog.Find(id);
        if (entry == nullptr) continue;
        if (!LoadAllowed(*entry)) {
            PublishSuspended(*entry);
        } else if (quarantined(id) || active(id)) {
            disabled_plugins_.erase(id);
        } else if (decision.enabled) {
            disabled_plugins_.erase(id);
        } else {
            publish_view(*entry, "disabled", decision.reason);
        }
    }

    bool loaded = true;
    for (const std::string& id : plan.load_order) {
        const auto decision = decisions.find(id);
        if (decision == decisions.end() || !decision->second.enabled || active(id)) continue;
        const anomaly::PluginCatalogEntry* entry = catalog.Find(id);
        if (entry != nullptr && !LoadAllowed(*entry)) {
            PublishSuspended(*entry);
            continue;
        }
        if (entry == nullptr || !entry->manifest || quarantined(id)) {
            loaded = false;
            continue;
        }

        std::string missing_dependency;
        for (const auto& dependency : entry->manifest->dependencies) {
            const auto dependency_decision = decisions.find(dependency.id);
            if (!dependency.optional &&
                (dependency_decision == decisions.end() ||
                 !dependency_decision->second.enabled || !active(dependency.id))) {
                missing_dependency = dependency.id;
                break;
            }
        }
        if (!missing_dependency.empty()) {
            publish_view(
                *entry, "dependency-blocked",
                "required dependency is not active: " + missing_dependency);
            loaded = false;
            continue;
        }

        if (!LoadCatalogEntry(*entry) || !active(id)) {
            publish_view(*entry, "faulted", "plugin activation failed");
            loaded = false;
        } else {
            disabled_plugins_.erase(id);
        }
    }

    // Nodes omitted from load_order have a statically invalid dependency plan.
    // Publish a deterministic blocked view rather than silently leaving the
    // configured-enabled plugin absent from the runtime snapshot.
    for (const auto& [id, decision] : decisions) {
        if (!decision.enabled || active(id)) continue;
        if (quarantined(id)) {
            loaded = false;
            continue;
        }
        const auto* entry = catalog.Find(id);
        if (entry == nullptr) continue;
        if (!disabled_plugins_.contains(id)) {
            const auto* node = plan.Find(id);
            const std::string reason = node != nullptr && !node->diagnostics.empty()
                ? node->diagnostics.front()
                : "plugin did not publish an active generation";
            publish_view(*entry, "dependency-blocked", reason);
        }
        loaded = false;
    }

    return stopped && loaded;
}

void PluginManager::UnloadAll() {
    static_cast<void>(StopForRuntime());
}

bool PluginManager::StopForRuntime(std::chrono::milliseconds timeout) {
    std::vector<std::size_t> indices;
    indices.reserve(plugins_.size());
    for (std::size_t index = 0; index < plugins_.size(); ++index) indices.push_back(index);
    {
        std::scoped_lock lock(stop_diagnostics_mutex_);
        stop_diagnostics_.clear();
    }
    return UnloadIndicesWithDeadline(indices, true, timeout);
}

void PluginManager::UnloadIndices(
    const std::vector<std::size_t>& indices, const bool retire_shadow_generations) {
    static_cast<void>(UnloadIndicesWithDeadline(
        indices, retire_shadow_generations, std::chrono::milliseconds(1000)));
}

bool PluginManager::UnloadIndicesWithDeadline(
    const std::vector<std::size_t>& indices,
    const bool retire_shadow_generations,
    const std::chrono::milliseconds timeout) {
    std::vector<std::size_t> ordered = indices;
    std::sort(ordered.begin(), ordered.end(), std::greater<>());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    const auto deadline = StopDeadlineAfter(timeout);
    bool all_stopped = true;
    for (const std::size_t index : ordered) {
        if (index >= plugins_.size()) continue;
        auto& plugin = *plugins_[index];
        const std::shared_ptr<anomaly::PluginScope> scope = plugin.scope;
        const std::uint64_t generation = plugin.view.generation;
        const std::size_t resources_before = scope == nullptr ? 0 : scope->Resources().size();
        bool quarantine{};
        bool timed_out{};
        bool drained = scope == nullptr;
        std::string quarantine_reason;

        // Freeze every generation before inspecting the remaining deadline.
        // Even a generation reached after the global deadline must have its
        // queued dispatcher work cancelled before it is quarantined.
        if (scope != nullptr) {
            scope->FreezeCallbackSources();
            if (queued_callback_canceller_) {
                try { queued_callback_canceller_(plugin.view.id, generation); } catch (...) {}
            }
            const bool platform_revoked = platform_services_ == nullptr ||
                platform_services_->RevokeScope(
                    ScopedPlatformOwner(plugin.service_context), deadline,
                    anomaly::ScopedPlatformRevokePhase::PreStop);
            if (!platform_revoked) {
                quarantine = true;
                timed_out = StopRemaining(deadline) == std::chrono::milliseconds::zero();
                quarantine_reason = "platform hook revocation failed";
            } else {
                static_cast<void>(scope->RevokeAllExcept(anomaly::PluginResourceKind::Config));
            }
        }

        const auto remaining_before_stop = StopRemaining(deadline);
        if (!quarantine && remaining_before_stop == std::chrono::milliseconds::zero()) {
            quarantine = true;
            timed_out = true;
            drained = false;
            quarantine_reason = "host stop deadline exceeded";
        }

        // Freeze publication sources before cancelling queued work and draining
        // ordinary generation leases.  The optional dispatcher hook lets the
        // production composition root cancel owner/generation queues here.
        if (!quarantine && scope != nullptr) {
            drained = scope->BeginStop(StopRemaining(deadline));
            if (!drained) {
                quarantine = true;
                timed_out = true;
                quarantine_reason = "callback barrier timed out";
            }
        }

        if (!quarantine && plugin.started &&
            plugin.descriptor_v1.on_stop != nullptr) {
            auto stop_lease = scope == nullptr
                ? anomaly::PluginScope::CallbackLease{}
                : scope->AcquireLifecycleLease(generation);
            if (scope != nullptr && !stop_lease) {
                quarantine = true;
                quarantine_reason = "lifecycle lease unavailable";
            } else {
                std::promise<AnomalyStatusV1> completed;
                std::future<AnomalyStatusV1> completion = completed.get_future();
                const auto stop = plugin.descriptor_v1.on_stop;
                void* context = plugin.plugin_context;
                const auto remaining = StopRemaining(deadline);
                const std::uint32_t stop_deadline = remaining.count() <= 0
                    ? 0u
                    : static_cast<std::uint32_t>(
                          (std::min)(remaining.count(),
                              static_cast<std::int64_t>(UINT32_MAX)));
                std::thread stop_thread([
                    scope, generation, stop, context, stop_deadline,
                    lease = std::move(stop_lease), promise = std::move(completed)]() mutable {
                    AnomalyStatusV1 result = StatusV1(ANOMALY_STATUS_V1_FAILED);
                    ScopedPluginCallback callback_scope(scope, generation, true);
                    try { result = stop(context, stop_deadline); } catch (...) {}
                    try { promise.set_value(result); } catch (...) {}
                });
                if (completion.wait_for(remaining) != std::future_status::ready) {
                    stop_thread.detach();
                    quarantine = true;
                    timed_out = true;
                    quarantine_reason = "on_stop timed out";
                } else {
                    const AnomalyStatusV1 status = completion.get();
                    stop_thread.join();
                    if (status.code != ANOMALY_STATUS_V1_OK) {
                        quarantine = true;
                        quarantine_reason =
                            "on_stop status=" + std::to_string(status.code);
                    }
                }
            }
        }

        // Config schemas remain scoped through on_stop so plugins can use the
        // durable Config ABI for their final save. All scoped resources are
        // revoked before on_unload and before the module can be released.
        if (!quarantine && scope != nullptr) {
            const bool platform_revoked = platform_services_ == nullptr ||
                platform_services_->RevokeScope(
                    ScopedPlatformOwner(plugin.service_context), deadline);
            if (!platform_revoked) {
                quarantine = true;
                timed_out = StopRemaining(deadline) == std::chrono::milliseconds::zero();
                quarantine_reason = "platform hook revocation failed";
            } else {
                static_cast<void>(scope->RevokeAll());
            }
        }

        if (quarantine) {
            all_stopped = false;
            plugin.faulted = true;
            plugin.view.state = "quarantined";
            plugin.view.status_reason = quarantine_reason;
            const std::size_t in_flight = scope == nullptr ? 0 : scope->InFlightCallbacks();
            {
                std::scoped_lock lock(stop_diagnostics_mutex_);
                stop_diagnostics_.push_back({
                    plugin.view.id, generation, drained, timed_out,
                    in_flight, resources_before, quarantine_reason});
            }
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                "plugin quarantined after stop failure: " + plugin.view.id +
                " " + quarantine_reason);
            quarantined_plugins_.push_back(std::move(plugins_[index]));
            plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }

        // A lifecycle lease protects the final unload callback.  It is acquired
        // only after ordinary callbacks have drained and remains held through
        // on_unload, so the module cannot be unmapped while host calls run.
        auto unload_lease = scope == nullptr
            ? anomaly::PluginScope::CallbackLease{}
            : scope->AcquireLifecycleLease(generation);
        if (plugin.started) {
            ScopedPluginCallback callback_scope(scope, generation, true);
            if (unload_lease || scope == nullptr) {
                try { plugin.descriptor_v1.on_unload(plugin.plugin_context); } catch (...) {
                    Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "exception while unloading " + plugin.view.id);
                }
            }
        }
        plugin.started = false;
        plugin.plugin_context = nullptr;
        {
            std::scoped_lock lock(stop_diagnostics_mutex_);
            stop_diagnostics_.push_back({
                plugin.view.id, generation, drained, false, 0, resources_before, {}});
        }
        FreeLibrary(plugin.module);
        if (retire_shadow_generations) shadow_store_.Retire(plugin.shadow_generation);
        plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return all_stopped;
}

void PluginManager::LoadPersistentUiWindowState() {
    try {
        std::error_code error;
        if (!std::filesystem::exists(ui_window_state_file_, error)) {
            if (error) throw std::filesystem::filesystem_error(
                "UI window state existence check failed", ui_window_state_file_, error);
            return;
        }
        const std::uintmax_t byte_count = std::filesystem::file_size(ui_window_state_file_, error);
        if (error || byte_count > kMaximumUiWindowStateFileBytes) {
            throw std::runtime_error("UI window state file is unavailable or too large");
        }

        std::ifstream input(ui_window_state_file_, std::ios::binary);
        if (!input) throw std::runtime_error("UI window state file cannot be opened");
        const nlohmann::json document = nlohmann::json::parse(input);
        PersistentUiState state = ParseUiWindowState(document);
        if (!ui_resources_->ImportPersistentWindowState(state.windows)) {
            throw std::runtime_error("UI window state values are invalid");
        }
        ui_window_state_last_document_ = SerializeUiWindowState(
            state.windows, state.plugin_windows);
        {
            std::scoped_lock lock(plugin_window_visibility_mutex_);
            plugin_window_visibility_ = std::move(state.plugin_windows);
        }
    } catch (const std::exception& exception) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "UI window state load failed: " + std::string(exception.what()));
    } catch (...) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "UI window state load failed");
    }
}

void PluginManager::SavePersistentUiWindowState(const bool force) noexcept {
    try {
        std::scoped_lock lock(ui_window_state_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!force && ui_window_state_last_save_ != std::chrono::steady_clock::time_point{} &&
            now - ui_window_state_last_save_ < kUiWindowStateSaveInterval) {
            return;
        }
        std::unordered_map<std::string, bool> plugin_windows;
        {
            std::scoped_lock visibility_lock(plugin_window_visibility_mutex_);
            plugin_windows = plugin_window_visibility_;
        }
        const std::string document = SerializeUiWindowState(
            ui_resources_->ExportPersistentWindowState(), plugin_windows);
        if (document == ui_window_state_last_document_) {
            ui_window_state_last_save_ = now;
            return;
        }

        std::error_code error;
        std::filesystem::create_directories(ui_window_state_file_.parent_path(), error);
        if (error) throw std::filesystem::filesystem_error(
            "UI window state directory create failed", ui_window_state_file_.parent_path(), error);
        const std::filesystem::path temporary = ui_window_state_file_.wstring() +
            L".tmp-" + std::to_wstring(GetCurrentProcessId());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("UI window state temporary file cannot be opened");
        output << document;
        output.flush();
        output.close();
        if (!output) throw std::runtime_error("UI window state write failed");
        if (!MoveFileExW(
                temporary.c_str(), ui_window_state_file_.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error_code = GetLastError();
            std::filesystem::remove(temporary, error);
            throw std::system_error(
                static_cast<int>(error_code), std::system_category(),
                "UI window state publish failed");
        }
        ui_window_state_last_document_ = document;
        ui_window_state_last_save_ = now;
    } catch (const std::exception& exception) {
        try {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "UI window state save failed: " + std::string(exception.what()));
        } catch (...) {
        }
    } catch (...) {
        try {
            Log(ANOMALY_CORE_LOG_LEVEL_V1_WARNING, "UI window state save failed");
        } catch (...) {
        }
    }
}

void PluginManager::Maintenance() {
    MaintenancePluginState();
    PersistUiWindowState();
}

void PluginManager::MaintenancePluginState() {
    const ScopedLogThreadDomain log_domain(anomaly::LogThreadDomain::Worker);
    PollForChanges();
}

void PluginManager::PersistUiWindowState() {
    const ScopedLogThreadDomain log_domain(anomaly::LogThreadDomain::Worker);
    SavePersistentUiWindowState();
}

void PluginManager::ReconcileWindowVisibility(LoadedPlugin& plugin) noexcept {
    if (plugin.scope == nullptr || ui_resources_ == nullptr) return;
    const anomaly::UiWindowGroupState windows =
        ui_resources_->WindowGroupState(plugin.scope);
    if (windows.window_count != 0) {
        plugin.view.visible = windows.open_window_count != 0;
        std::scoped_lock lock(plugin_window_visibility_mutex_);
        plugin_window_visibility_.erase(plugin.view.id);
        return;
    }
    std::scoped_lock lock(plugin_window_visibility_mutex_);
    const auto persisted = plugin_window_visibility_.find(plugin.view.id);
    if (persisted != plugin_window_visibility_.end()) {
        plugin.view.visible = persisted->second;
    }
}

void PluginManager::SetPersistentPluginWindowVisibility(
    const std::string_view plugin_id, const bool visible) noexcept {
    try {
        std::scoped_lock lock(plugin_window_visibility_mutex_);
        plugin_window_visibility_.insert_or_assign(std::string(plugin_id), visible);
    } catch (...) {
    }
}

void PluginManager::GameUpdate(double delta_seconds) {
    const ScopedLogThreadDomain log_domain(anomaly::LogThreadDomain::Game);
    for (const auto& plugin : plugins_) {
        if (plugin->faulted) continue;
        auto callback = plugin->scope != nullptr
            ? plugin->scope->AcquireCallback(plugin->view.generation)
            : anomaly::PluginScope::CallbackLease{};
        if (plugin->scope != nullptr && !callback) continue;
        ScopedPluginCallback callback_scope(
            plugin->scope, plugin->view.generation, false);
        const auto started = std::chrono::steady_clock::now();
        bool invoked{};
        bool fault{};
        if (plugin->started && plugin->descriptor_v1.on_update != nullptr) {
            invoked = true;
            try { plugin->descriptor_v1.on_update(plugin->plugin_context, delta_seconds); } catch (...) {
                fault = true;
                plugin->faulted = true;
                plugin->view.status_reason = "Update callback raised an exception";
                Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "exception in ABI v1 update: " + plugin->view.id);
            }
        }
        if (invoked) {
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            plugin->update_metrics.Record(
                elapsed, fault, callback_budgets_.update_slow_milliseconds);
            if (callback_evidence_observer_) {
                try {
                    callback_evidence_observer_({
                        PluginCallbackEvidenceKind::Update,
                        plugin->view.id,
                        plugin->view.generation,
                        GetCurrentThreadId(),
                        elapsed * 1000.0,
                        fault});
                } catch (...) {
                }
            }
        }
        ReconcileWindowVisibility(*plugin);
    }
}

bool PluginManager::ReloadPackages(const std::vector<std::string>& package_names) {
    std::string summary;
    for (const std::string& package_name : package_names) {
        if (!summary.empty()) summary += ',';
        summary += package_name;
    }
    std::unordered_set<std::string> changed(package_names.begin(), package_names.end());
    std::unordered_set<std::string> affected;
    for (const auto& plugin : plugins_) {
        if (changed.contains(Utf8(plugin->view.package_directory.filename()))) {
            affected.insert(plugin->view.id);
        }
    }
    for (const auto& [id, plugin] : disabled_plugins_) {
        if (changed.contains(Utf8(plugin.package_directory.filename()))) {
            affected.insert(id);
        }
    }

    const anomaly::PluginCatalogSnapshot catalog =
        anomaly::DiscoverPluginCatalog(plugin_directory_);
    for (const anomaly::PluginCatalogEntry& entry : catalog.Entries()) {
        if (entry.manifest && changed.contains(Utf8(entry.package_root.filename()))) {
            affected.insert(entry.manifest->id);
        }
    }
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const anomaly::PluginCatalogEntry& entry : catalog.Entries()) {
            if (!entry.manifest || affected.contains(entry.manifest->id)) continue;
            const bool depends_on_affected = std::any_of(
                entry.manifest->dependencies.begin(), entry.manifest->dependencies.end(),
                [&](const anomaly::PluginDependencyManifest& dependency) {
                    return affected.contains(dependency.id);
                });
            if (depends_on_affected) {
                affected.insert(entry.manifest->id);
                expanded = true;
            }
        }
    }

    struct RollbackCandidate {
        std::string id;
        std::filesystem::path source;
        std::filesystem::path shadow;
        std::filesystem::path package_directory;
        anomaly::PluginShadowGeneration shadow_generation;
    };
    std::vector<RollbackCandidate> rollback_candidates;
    std::vector<std::size_t> unload_indices;
    for (std::size_t index = 0; index < plugins_.size(); ++index) {
        if (!affected.contains(plugins_[index]->view.id)) continue;
        const auto& plugin = *plugins_[index];
        rollback_candidates.push_back({
            plugin.view.id, plugin.view.source, plugin.shadow,
            plugin.view.package_directory, plugin.shadow_generation});
        unload_indices.push_back(index);
    }
    UnloadIndices(unload_indices, false);
    if (std::any_of(
            quarantined_plugins_.begin(), quarantined_plugins_.end(),
            [&](const auto& plugin) { return affected.contains(plugin->view.id); })) {
        Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
            "plugin package reload stopped by quarantined generation: " + summary);
        return false;
    }

    const auto decisions = enablement_store_.Resolve(catalog);
    const auto publish_disabled = [&](const anomaly::PluginCatalogEntry& entry,
                                      std::string reason) {
        if (!entry.manifest) return;
        PluginView view;
        view.id = entry.manifest->id;
        view.name = entry.manifest->name;
        view.author = entry.manifest->author;
        view.version = entry.manifest->version.ToString();
        view.source = entry.entry_file;
        view.package_directory = entry.package_root;
        view.visible = false;
        view.enabled = false;
        view.state = "disabled";
        view.status_reason = std::move(reason);
        disabled_plugins_[view.id] = std::move(view);
    };
    for (const std::string& id : affected) disabled_plugins_.erase(id);

    const anomaly::PluginDependencyPlan plan = anomaly::ResolvePluginDependencies(catalog);
    bool replacements_ok = true;
    for (const std::string& id : plan.load_order) {
        if (!affected.contains(id)) continue;
        const anomaly::PluginCatalogEntry* entry = catalog.Find(id);
        if (entry != nullptr && !LoadAllowed(*entry)) {
            PublishSuspended(*entry);
            continue;
        }
        const auto decision = decisions.find(id);
        if (entry != nullptr && decision != decisions.end() && !decision->second.enabled) {
            publish_disabled(*entry, decision->second.reason);
            continue;
        }
        if (entry != nullptr) replacements_ok = LoadCatalogEntry(*entry) && replacements_ok;
    }

    for (const RollbackCandidate& candidate : rollback_candidates) {
        std::error_code error;
        if (!std::filesystem::exists(candidate.package_directory, error) ||
            disabled_plugins_.contains(candidate.id)) {
            continue;
        }
        const bool active = std::any_of(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
            return plugin->view.id == candidate.id;
        });
        replacements_ok = active && replacements_ok;
    }

    if (!replacements_ok) {
        std::vector<std::size_t> replacement_indices;
        for (std::size_t index = 0; index < plugins_.size(); ++index) {
            if (affected.contains(plugins_[index]->view.id)) replacement_indices.push_back(index);
        }
        UnloadIndices(replacement_indices);
        bool rollback_ok = true;
        for (const RollbackCandidate& candidate : rollback_candidates) {
            std::error_code error;
            if (!std::filesystem::exists(candidate.package_directory, error) ||
                disabled_plugins_.contains(candidate.id)) {
                shadow_store_.Retire(candidate.shadow_generation);
                continue;
            }
            const bool restored = LoadBinary(
                candidate.source, candidate.shadow_generation.entry_file,
                candidate.package_directory, candidate.shadow_generation);
            rollback_ok = restored && rollback_ok;
        }
        Log(rollback_ok ? ANOMALY_CORE_LOG_LEVEL_V1_WARNING : ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
            std::string(rollback_ok ? "plugin package reload rolled back: "
                                    : "plugin package rollback failed: ") + summary);
        return false;
    }

    for (const RollbackCandidate& candidate : rollback_candidates) {
        shadow_store_.Retire(candidate.shadow_generation);
    }
    Log(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "plugin package change applied: " + summary);
    return true;
}

void PluginManager::PollForChanges() {
    const std::vector<std::string> changed =
        file_watcher_.PollForTests(std::chrono::steady_clock::now());
    if (!changed.empty()) static_cast<void>(ReloadPackages(changed));
}

void PluginManager::Draw(void* imgui_context) {
    const ScopedLogThreadDomain log_domain(anomaly::LogThreadDomain::Render);
    for (const auto& plugin : plugins_) {
        ReconcileWindowVisibility(*plugin);
        if (!plugin->view.visible || plugin->faulted) continue;
        auto callback = plugin->scope != nullptr
            ? plugin->scope->AcquireCallback(plugin->view.generation)
            : anomaly::PluginScope::CallbackLease{};
        if (plugin->scope != nullptr && !callback) continue;
        ScopedPluginCallback callback_scope(
            plugin->scope, plugin->view.generation, false);
        const auto started = std::chrono::steady_clock::now();
        bool invoked{};
        bool fault{};
        if (plugin->started && plugin->descriptor_v1.on_draw != nullptr &&
            ui_service_ != nullptr) {
            invoked = true;
            plugin->ui_proxy_context.close_requested = false;
            try {
                plugin->descriptor_v1.on_draw(
                    plugin->plugin_context,
                    reinterpret_cast<const AnomalyUiServiceV1*>(&plugin->ui_proxy));
                if (plugin->ui_proxy_context.close_requested) {
                    plugin->view.visible = false;
                    if (ui_resources_->WindowGroupState(plugin->scope).window_count == 0) {
                        SetPersistentPluginWindowVisibility(plugin->view.id, false);
                    }
                }
            } catch (...) {
                fault = true;
                plugin->faulted = true;
                plugin->view.status_reason = "Draw callback raised an exception";
            }
            const bool ui_stack_fault = RecoverPluginUiStack(
                plugin->ui_proxy_context, plugin->service_context);
            if (ui_stack_fault) {
                fault = true;
                plugin->faulted = true;
                plugin->view.status_reason = "Draw callback left the UI stack unbalanced";
            }
            if (fault) {
                Log(
                    ANOMALY_CORE_LOG_LEVEL_V1_ERROR,
                    std::string(ui_stack_fault ? "unbalanced UI stack: "
                                               : "exception in ABI v1 draw: ") +
                        plugin->view.id);
            }
        }
        if (invoked) {
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            plugin->draw_metrics.Record(
                elapsed, fault, callback_budgets_.draw_slow_milliseconds);
            if (callback_evidence_observer_) {
                try {
                    callback_evidence_observer_({
                        PluginCallbackEvidenceKind::Draw,
                        plugin->view.id,
                        plugin->view.generation,
                        GetCurrentThreadId(),
                        elapsed * 1000.0,
                        fault});
                } catch (...) {
                }
            }
        }
        ReconcileWindowVisibility(*plugin);
    }
}

void PluginManager::SetImGuiContext(void* imgui_context) noexcept {
    imgui_context_ = imgui_context;
}

void PluginManager::SetUiService(const AnomalyUiServiceV1* service) {
    ui_service_ = service != nullptr &&
            service->service_version == ANOMALY_UI_SERVICE_V1_VERSION &&
            service->struct_size >= sizeof(AnomalyUiServiceV1)
        ? service
        : nullptr;
    for (const auto& plugin : plugins_) {
        plugin->ui_proxy_context.service = ui_service_;
        plugin->ui_proxy = MakeUiProxy(&plugin->ui_proxy_context);
        if (ui_service_ == nullptr) continue;
        if (plugin->waiting_for_service && !Activate(*plugin)) {
            plugin->waiting_for_service = false;
            Log(ANOMALY_CORE_LOG_LEVEL_V1_ERROR, "ABI v1 deferred activation failed: " + plugin->view.id);
        }
    }
}

void PluginManager::PublishInputFrame(
    const anomaly::InputFrameState& frame, const anomaly::InputUiCaptureState capture) {
    static_cast<void>(input_service_.AdvanceFrame(frame, capture));
}

void PluginManager::PublishUiCapture(const anomaly::InputUiCaptureState capture) {
    static_cast<void>(input_service_.RecordUiCapture(capture));
}

void PluginManager::ResetInput(const anomaly::InputResetReason reason) noexcept {
    try {
        static_cast<void>(input_service_.Reset(reason));
    } catch (...) {
    }
}

void PluginManager::OnUiDeviceLost() noexcept {
    try {
        std::shared_ptr<anomaly::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        if (backend != nullptr) backend->OnDeviceLost();
        static_cast<void>(ui_resources_->InvalidateDeviceResources());
        static_cast<void>(input_service_.OnDeviceReset());
    } catch (...) {
    }
}

bool PluginManager::OnUiDeviceRebuilt() noexcept {
    try {
        const std::uint64_t generation = ui_resources_->DeviceGeneration();
        if (!ui_resources_->RebuildDeviceResources(generation)) return false;
        std::shared_ptr<anomaly::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        return backend == nullptr || backend->OnDeviceRebuilt(generation);
    } catch (...) {
        return false;
    }
}

void PluginManager::SetUiResourceRenderBackend(
    std::shared_ptr<anomaly::UiResourceRenderBackend> backend) noexcept {
    try {
        std::scoped_lock lock(ui_resource_backend_mutex_);
        ui_resource_render_backend_ = std::move(backend);
    } catch (...) {
    }
}

void PluginManager::PrepareUiResources() noexcept {
    try {
        std::shared_ptr<anomaly::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        if (backend == nullptr) return;
        backend->CollectGarbage(*ui_resources_);
        for (const auto& plugin : plugins_) {
            if (plugin == nullptr || plugin->scope == nullptr) continue;
            for (const anomaly::UiResourceSnapshot& resource :
                 ui_resources_->Resources(plugin->scope)) {
                if (resource.kind == anomaly::UiResourceKind::Font &&
                    resource.state != anomaly::UiResourceState::Failed) {
                    backend->PrepareFont(*ui_resources_, plugin->scope, resource.handle);
                } else if (resource.kind == anomaly::UiResourceKind::Texture &&
                           resource.state != anomaly::UiResourceState::Failed) {
                    backend->PrepareTexture(*ui_resources_, plugin->scope, resource.handle);
                }
            }
        }
    } catch (...) {
    }
}

void PluginManager::PrepareUiTexture(
    const std::shared_ptr<anomaly::PluginScope>& scope,
    const anomaly::UiResourceHandle handle) noexcept {
    if (scope == nullptr || !handle) return;
    try {
        std::shared_ptr<anomaly::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        if (backend == nullptr) return;
        const auto resource = ui_resources_->ResourceState(scope, handle);
        if (!resource || resource->kind != anomaly::UiResourceKind::Texture ||
            resource->state == anomaly::UiResourceState::Failed ||
            resource->state == anomaly::UiResourceState::Revoked) {
            return;
        }
        backend->PrepareTexture(*ui_resources_, scope, handle);
    } catch (...) {
    }
}

bool PluginManager::QueueUiFontLoad(
    const std::shared_ptr<anomaly::PluginScope>& scope,
    const anomaly::UiResourceHandle handle) noexcept {
    if (scope == nullptr || !handle || !ui_resource_worker_dispatcher_) return false;

    const std::shared_ptr<anomaly::UiResourceRegistry> registry = ui_resources_;
    const auto resource = registry->ResourceState(scope, handle);
    if (!resource || resource->kind != anomaly::UiResourceKind::Font || resource->resource_id == 0 ||
        resource->state == anomaly::UiResourceState::Failed ||
        resource->state == anomaly::UiResourceState::Revoked) {
        return false;
    }

    const std::shared_ptr<UiResourceWorkerGate> gate = ui_resource_worker_gate_;
    if (gate == nullptr) return false;
    const std::uint64_t resource_id = resource->resource_id;
    try {
        std::scoped_lock lock(gate->mutex);
        const auto pending = gate->pending_resources.find(resource_id);
        if (pending != gate->pending_resources.end()) {
            AddUiResourceWorkerCandidate(
                pending->second, {scope, handle, anomaly::UiResourceKind::Font});
            return true;
        }
    } catch (...) {
        return false;
    }
    // A source payload is already staged, or the resource has reached a state
    // owned by the render backend. Neither case needs another Worker read.
    if (resource->state != anomaly::UiResourceState::Queued || resource->staged_bytes != 0 ||
        resource->reserved_staging_bytes != 0) {
        return true;
    }

    std::shared_ptr<UiResourceWorkerGateLease> worker_gate;
    try {
        worker_gate = std::make_shared<UiResourceWorkerGateLease>(
            registry, gate, resource_id, anomaly::UiResourceKind::Font);
        std::scoped_lock lock(gate->mutex);
        auto [pending, inserted] = gate->pending_resources.try_emplace(
            resource_id,
            std::vector<UiResourceWorkerGate::Pending>{
                {scope, handle, anomaly::UiResourceKind::Font}});
        if (!inserted) {
            AddUiResourceWorkerCandidate(
                pending->second, {scope, handle, anomaly::UiResourceKind::Font});
            return true;
        }
        worker_gate->Activate();
    } catch (...) {
        return false;
    }

    try {
        const std::string owner = scope->Owner();
        const std::uint64_t generation = scope->Generation();
        if (!ui_resource_worker_dispatcher_(
            owner, generation,
            [registry, worker_gate, resource_id] {
                try {
                    const auto pending = worker_gate->CurrentLive();
                    if (!pending || pending->kind != anomaly::UiResourceKind::Font) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto lease = pending->scope->AcquireCallback(pending->scope->Generation());
                    if (!lease) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto state = registry->ResourceState(pending->scope, pending->handle);
                    if (!state || state->kind != anomaly::UiResourceKind::Font ||
                        state->resource_id != resource_id ||
                        state->state != anomaly::UiResourceState::Queued || state->staged_bytes != 0) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto request = registry->FontRequest(pending->scope, pending->handle);
                    if (!request) {
                        worker_gate->Fail();
                        return;
                    }

                    const auto read_target = worker_gate->CurrentLive();
                    if (!read_target) {
                        worker_gate->Complete();
                        return;
                    }
                    UiResourceWorkerStagingAdmission read_admission{
                        registry, read_target->scope, read_target->handle};
                    anomaly::UiResourceReadResult read = ReadPackageResourceBytes(
                        *request,
                        anomaly::kDefaultUiResourceEncodedByteLimit, read_admission.Callback());
                    if (!read) {
                        worker_gate->Fail();
                        return;
                    }
                    const auto current = worker_gate->CurrentLive();
                    if (!current || !registry->SetFontData(
                            current->scope, current->handle, std::move(read.bytes))) {
                        worker_gate->Fail();
                        return;
                    }
                    worker_gate->Complete();
                } catch (...) {
                    worker_gate->Fail();
                }
            })) {
            worker_gate->Fail();
            return false;
        }
    } catch (...) {
        worker_gate->Fail();
        return false;
    }
    return true;
}

bool PluginManager::QueueUiTextureLoad(
    const std::shared_ptr<anomaly::PluginScope>& scope,
    const anomaly::UiResourceHandle handle) noexcept {
    if (scope == nullptr || !handle || !ui_resource_worker_dispatcher_) return false;

    const std::shared_ptr<anomaly::UiResourceRegistry> registry = ui_resources_;
    const auto resource = registry->ResourceState(scope, handle);
    if (!resource || resource->kind != anomaly::UiResourceKind::Texture || resource->resource_id == 0 ||
        resource->state == anomaly::UiResourceState::Failed ||
        resource->state == anomaly::UiResourceState::Revoked) {
        return false;
    }

    const std::shared_ptr<UiResourceWorkerGate> gate = ui_resource_worker_gate_;
    if (gate == nullptr) return false;
    const std::uint64_t resource_id = resource->resource_id;
    try {
        std::scoped_lock lock(gate->mutex);
        const auto pending = gate->pending_resources.find(resource_id);
        if (pending != gate->pending_resources.end()) {
            AddUiResourceWorkerCandidate(
                pending->second, {scope, handle, anomaly::UiResourceKind::Texture});
            return true;
        }
    } catch (...) {
        return false;
    }
    // Raw RGBA input and a completed decode are already render-ready payloads.
    // Device recovery uploads the cached pixels again without a Worker decode.
    if (resource->state != anomaly::UiResourceState::Queued ||
        (resource->texture_format == anomaly::UiTextureFormat::Rgba8 &&
         resource->staged_bytes != 0) ||
        resource->reserved_staging_bytes != 0) {
        return true;
    }

    std::shared_ptr<UiResourceWorkerGateLease> worker_gate;
    try {
        worker_gate = std::make_shared<UiResourceWorkerGateLease>(
            registry, gate, resource_id, anomaly::UiResourceKind::Texture);
        std::scoped_lock lock(gate->mutex);
        auto [pending, inserted] = gate->pending_resources.try_emplace(
            resource_id,
            std::vector<UiResourceWorkerGate::Pending>{
                {scope, handle, anomaly::UiResourceKind::Texture}});
        if (!inserted) {
            AddUiResourceWorkerCandidate(
                pending->second, {scope, handle, anomaly::UiResourceKind::Texture});
            return true;
        }
        worker_gate->Activate();
    } catch (...) {
        return false;
    }

    try {
        const std::string owner = scope->Owner();
        const std::uint64_t generation = scope->Generation();
        if (!ui_resource_worker_dispatcher_(
            owner, generation,
            [registry, worker_gate, resource_id] {
                try {
                    const auto pending = worker_gate->CurrentLive();
                    if (!pending || pending->kind != anomaly::UiResourceKind::Texture) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto lease = pending->scope->AcquireCallback(pending->scope->Generation());
                    if (!lease) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto state = registry->ResourceState(pending->scope, pending->handle);
                    if (!state || state->kind != anomaly::UiResourceKind::Texture ||
                        state->resource_id != resource_id ||
                        state->state != anomaly::UiResourceState::Queued ||
                        (state->texture_format == anomaly::UiTextureFormat::Rgba8 &&
                         state->staged_bytes != 0)) {
                        worker_gate->Complete();
                        return;
                    }
                    const auto reservation_target = worker_gate->CurrentLive();
                    if (!reservation_target) {
                        worker_gate->Complete();
                        return;
                    }
                    if (state->staged_bytes != 0 && !registry->ReserveResourceStaging(
                            reservation_target->scope, reservation_target->handle,
                            state->staged_bytes)) {
                        worker_gate->Fail();
                        return;
                    }
                    auto request = registry->TextureRequest(
                        reservation_target->scope, reservation_target->handle);
                    if (!request || request->format != anomaly::UiTextureFormat::Auto) {
                        worker_gate->Fail();
                        return;
                    }

                    std::vector<std::uint8_t> encoded = std::move(request->encoded_bytes);
                    if (encoded.empty()) {
                        const auto read_target = worker_gate->CurrentLive();
                        if (!read_target) {
                            worker_gate->Complete();
                            return;
                        }
                        UiResourceWorkerStagingAdmission read_admission{
                            registry, read_target->scope, read_target->handle};
                        anomaly::UiResourceReadResult read = ReadPackageResourceBytes(
                            *request,
                            anomaly::kDefaultUiResourceEncodedByteLimit, read_admission.Callback());
                        if (!read) {
                            worker_gate->Fail();
                            return;
                        }
                        encoded = std::move(read.bytes);
                    }

                    const auto decode_target = worker_gate->CurrentLive();
                    if (!decode_target) {
                        worker_gate->Complete();
                        return;
                    }
                    UiResourceWorkerStagingAdmission decode_admission{
                        registry, decode_target->scope, decode_target->handle, encoded.size()};
                    anomaly::UiImageDecodeResult decoded = anomaly::DecodeUiImageRgba8(
                        encoded, anomaly::UiImageDecodeLimits{}, decode_admission.Callback());
                    if (!decoded) {
                        worker_gate->Fail();
                        return;
                    }
                    const auto current = worker_gate->CurrentLive();
                    if (!current || !registry->SetTextureData(
                            current->scope, current->handle, std::move(decoded.image.pixels),
                            anomaly::UiTextureFormat::Rgba8,
                            decoded.image.width, decoded.image.height)) {
                        worker_gate->Fail();
                        return;
                    }
                    worker_gate->Complete();
                } catch (...) {
                    worker_gate->Fail();
                }
            })) {
            worker_gate->Fail();
            return false;
        }
    } catch (...) {
        worker_gate->Fail();
        return false;
    }
    return true;
}

bool PluginManager::PushUiFont(
    const std::shared_ptr<anomaly::PluginScope>& scope,
    const anomaly::UiResourceHandle handle) noexcept {
    try {
        std::shared_ptr<anomaly::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        return backend != nullptr && backend->PushFont(*ui_resources_, scope, handle);
    } catch (...) {
        return false;
    }
}

bool PluginManager::PopUiFont() noexcept {
    try {
        std::shared_ptr<anomaly::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        return backend != nullptr && backend->PopFont();
    } catch (...) {
        return false;
    }
}

bool PluginManager::DrawUiTexture(
    const std::shared_ptr<anomaly::PluginScope>& scope,
    const anomaly::UiResourceHandle handle, const float width, const float height,
    const std::uint32_t tint_rgba) noexcept {
    try {
        std::shared_ptr<anomaly::UiResourceRenderBackend> backend;
        {
            std::scoped_lock lock(ui_resource_backend_mutex_);
            backend = ui_resource_render_backend_;
        }
        return backend != nullptr &&
            backend->DrawTexture(*ui_resources_, scope, handle, width, height, tint_rgba);
    } catch (...) {
        return false;
    }
}

PluginRuntimeDiagnosticsSnapshot PluginManager::DiagnosticsSnapshot() const {
    struct ServiceCandidate final {
        std::string_view id;
        std::uint32_t version{};
        bool published{};
    };

    PluginRuntimeDiagnosticsSnapshot snapshot;
    snapshot.plugins.reserve(
        plugins_.size() + quarantined_plugins_.size() + disabled_plugins_.size());
    const anomaly::IpcDiagnostics ipc_snapshot = ipc_registry_ == nullptr
        ? anomaly::IpcDiagnostics{} : ipc_registry_->Snapshot();

    const auto populate_platform_diagnostics =
        [this, &ipc_snapshot](const LoadedPlugin& plugin, PluginView& view) {
            PluginPlatformDiagnosticsView diagnostics;
            const anomaly::PluginCapabilityGrant& grant = plugin.service_context.capabilities;
            diagnostics.capability_enforced = grant.EnforcesRawMemoryCapabilities();
            diagnostics.capabilities = grant.Capabilities();

            const auto deny = [&](const std::string_view service,
                                  const anomaly::PluginServiceAuthorization authorization) {
                if (authorization.allowed) return;
                const std::string reason = authorization.required_capability.empty()
                    ? std::string(service) + ": no capability mapping is available"
                    : std::string(service) + ": required capability " +
                        std::string(authorization.required_capability) + " is not granted";
                if (std::find(
                        diagnostics.deny_reasons.begin(), diagnostics.deny_reasons.end(), reason) ==
                    diagnostics.deny_reasons.end()) {
                    diagnostics.deny_reasons.push_back(reason);
                }
            };
            const auto add_service = [&](const ServiceCandidate candidate) {
                const anomaly::PluginServiceAuthorization authorization =
                    grant.AuthorizeService(candidate.id);
                if (!authorization.allowed) {
                    deny(candidate.id, authorization);
                    return;
                }
                if (candidate.published && candidate.version != 0) {
                    diagnostics.services.push_back({std::string(candidate.id), candidate.version});
                }
            };

            const std::array platform_services{
                ServiceCandidate{ANOMALY_CORE_SERVICE_V1_ID,
                    plugin.service_context.core.service_version, true},
                ServiceCandidate{ANOMALY_PLUGIN_STATE_SERVICE_V1_ID,
                    plugin.service_context.plugin_state.service_version, true},
                ServiceCandidate{ANOMALY_CONFIG_SERVICE_V1_ID,
                    plugin.service_context.config.service_version, true},
                ServiceCandidate{ANOMALY_STORAGE_SERVICE_V1_ID,
                    plugin.service_context.storage.service_version, true},
                ServiceCandidate{ANOMALY_RUNTIME_INFO_SERVICE_V1_ID,
                    plugin.service_context.runtime_info.service_version, true},
                ServiceCandidate{ANOMALY_LOCALIZATION_SERVICE_V1_ID,
                    plugin.service_context.localization.service_version, true},
                ServiceCandidate{ANOMALY_DIAGNOSTICS_SERVICE_V1_ID,
                    plugin.service_context.diagnostics.service_version, true},
                ServiceCandidate{ANOMALY_SCHEDULER_SERVICE_V1_ID,
                    plugin.service_context.scheduler.service_version, true},
                ServiceCandidate{ANOMALY_IPC_SERVICE_V1_ID,
                    plugin.service_context.ipc_service.service_version, true},
                ServiceCandidate{ANOMALY_COMMANDS_SERVICE_V1_ID,
                    plugin.service_context.commands.service_version, true},
                ServiceCandidate{ANOMALY_NOTIFICATIONS_SERVICE_V1_ID,
                    plugin.service_context.notifications.service_version, true},
                ServiceCandidate{ANOMALY_SIGNATURE_SERVICE_V1_ID,
                    plugin.service_context.signature.service_version, true},
                ServiceCandidate{ANOMALY_HOOK_SERVICE_V1_ID,
                    plugin.service_context.hook.service_version, true},
                ServiceCandidate{ANOMALY_PATCH_SERVICE_V1_ID,
                    plugin.service_context.patch.service_version, true},
                ServiceCandidate{ANOMALY_UI_SERVICE_V1_ID,
                    plugin.service_context.ui == nullptr ? 0U : plugin.service_context.ui->service_version,
                    UiService() != nullptr},
                ServiceCandidate{ANOMALY_WINDOW_SERVICE_V1_ID,
                    plugin.service_context.window.service_version, true},
                ServiceCandidate{ANOMALY_FONT_SERVICE_V1_ID,
                    plugin.service_context.font.service_version, true},
                ServiceCandidate{ANOMALY_TEXTURE_SERVICE_V1_ID,
                    plugin.service_context.texture.service_version, true},
                ServiceCandidate{ANOMALY_INPUT_SERVICE_V1_ID,
                    plugin.service_context.input_service.service_version, true},
            };
            for (const ServiceCandidate candidate : platform_services) add_service(candidate);
            for (const auto& service : anomaly::ProcessAdapterServices().Snapshot()) {
                add_service({service.id, service.version, service.table != nullptr});
            }
            std::sort(
                diagnostics.services.begin(), diagnostics.services.end(),
                [](const PluginServiceVersionView& left, const PluginServiceVersionView& right) {
                    return left.id < right.id;
                });
            diagnostics.services.erase(
                std::unique(
                    diagnostics.services.begin(), diagnostics.services.end(),
                    [](const PluginServiceVersionView& left, const PluginServiceVersionView& right) {
                        return left.id == right.id;
                    }),
                diagnostics.services.end());

            if (plugin.scope != nullptr) {
                const std::vector<anomaly::PluginResourceRecord> resources =
                    plugin.scope->Resources();
                diagnostics.resources.ledger_resources = resources.size();
                for (const anomaly::PluginResourceRecord& resource : resources) {
                    switch (resource.kind) {
                    case anomaly::PluginResourceKind::Window:
                        ++diagnostics.resources.windows;
                        break;
                    case anomaly::PluginResourceKind::Font:
                        ++diagnostics.resources.fonts;
                        break;
                    case anomaly::PluginResourceKind::Texture:
                        ++diagnostics.resources.textures;
                        break;
                    case anomaly::PluginResourceKind::Input:
                        ++diagnostics.resources.hotkeys;
                        break;
                    case anomaly::PluginResourceKind::Ipc:
                        ++diagnostics.resources.ipc_resources;
                        break;
                    default:
                        break;
                    }
                }
            }

            if (plugin.scope != nullptr && platform_services_ != nullptr) {
                const anomaly::ScopedPlatformDiagnosticsView scoped = platform_services_->Snapshot(
                    ScopedPlatformOwner(plugin.service_context));
                diagnostics.resources.configs = scoped.resources.configs;
                diagnostics.resources.self_tests = scoped.resources.self_tests;
                diagnostics.resources.tasks = scoped.resources.tasks;
                diagnostics.resources.commands = scoped.resources.commands;
                diagnostics.resources.notifications = scoped.resources.notifications;
                diagnostics.resources.hooks = scoped.resources.hooks;
                diagnostics.resources.patches = scoped.resources.patches;
                diagnostics.queued_tasks = scoped.queued_tasks;
                diagnostics.scoped_callbacks = {
                    scoped.callback_calls, scoped.callback_faults, scoped.slow_callbacks};
            }
            for (const anomaly::IpcEndpointDiagnostics& endpoint : ipc_snapshot.endpoints) {
                if (endpoint.provider == plugin.view.id || std::ranges::find(
                        endpoint.consumers, plugin.view.id) != endpoint.consumers.end()) {
                    diagnostics.ipc_endpoints.push_back(endpoint);
                }
            }
            view.platform_diagnostics = std::move(diagnostics);
        };

    const auto append_loaded = [&](const LoadedPlugin& plugin, const bool quarantined) {
        PluginView view = plugin.view;
        view.enabled = !quarantined;
        view.state = quarantined ? "quarantined"
            : plugin.faulted ? "faulted"
            : plugin.waiting_for_service ? "waiting-for-service"
            : (plugin.started ? "active" : "loaded");
        if (quarantined) {
            view.visible = false;
        } else if (plugin.waiting_for_service) {
            view.status_reason = "required service is not ready";
        } else if (!plugin.faulted) {
            view.status_reason.clear();
        }
        view.update_metrics = plugin.update_metrics.View();
        view.draw_metrics = plugin.draw_metrics.View();
        populate_platform_diagnostics(plugin, view);
        snapshot.plugins.push_back(std::move(view));
    };

    for (const auto& plugin : plugins_) append_loaded(*plugin, false);
    for (const auto& plugin : quarantined_plugins_) append_loaded(*plugin, true);
    for (const auto& [id, disabled] : disabled_plugins_) snapshot.plugins.push_back(disabled);
    std::sort(
        snapshot.plugins.begin(), snapshot.plugins.end(),
        [](const PluginView& left, const PluginView& right) { return left.id < right.id; });
    return snapshot;
}

std::vector<PluginView> PluginManager::Plugins() const {
    return DiagnosticsSnapshot().plugins;
}

std::string PluginManager::DiagnosticsJson() const {
    const PluginRuntimeDiagnosticsSnapshot snapshot = DiagnosticsSnapshot();
    std::string output{"{\"schemaVersion\":" + std::to_string(snapshot.schema_version) +
        ",\"plugins\":["};
    const auto append_strings = [&output](const std::vector<std::string>& values) {
        output.push_back('[');
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0) output.push_back(',');
            output += json::Quote(values[index]);
        }
        output.push_back(']');
    };
    for (std::size_t index = 0; index < snapshot.plugins.size(); ++index) {
        if (index != 0) output.push_back(',');
        const PluginView& plugin = snapshot.plugins[index];
        const PluginPlatformDiagnosticsView& platform = plugin.platform_diagnostics;
        output += "{\"id\":" + json::Quote(plugin.id) +
            ",\"generation\":" + std::to_string(plugin.generation) +
            ",\"state\":" + json::Quote(plugin.state) +
            ",\"statusReason\":" + json::Quote(plugin.status_reason) +
            ",\"capabilityEnforced\":" +
                (platform.capability_enforced ? "true" : "false") +
            ",\"capabilities\":";
        append_strings(platform.capabilities);
        output += ",\"services\":[";
        for (std::size_t service_index = 0; service_index < platform.services.size(); ++service_index) {
            if (service_index != 0) output.push_back(',');
            const PluginServiceVersionView& service = platform.services[service_index];
            output += "{\"id\":" + json::Quote(service.id) +
                ",\"version\":" + std::to_string(service.version) + '}';
        }
        const PluginResourceCountsView& resources = platform.resources;
        output += "],\"resources\":{\"ledger\":" +
            std::to_string(resources.ledger_resources) +
            ",\"configs\":" + std::to_string(resources.configs) +
            ",\"selfTests\":" + std::to_string(resources.self_tests) +
            ",\"tasks\":" + std::to_string(resources.tasks) +
            ",\"ipcResources\":" + std::to_string(resources.ipc_resources) +
            ",\"commands\":" + std::to_string(resources.commands) +
            ",\"notifications\":" + std::to_string(resources.notifications) +
            ",\"hooks\":" + std::to_string(resources.hooks) +
            ",\"patches\":" + std::to_string(resources.patches) +
            ",\"windows\":" + std::to_string(resources.windows) +
            ",\"fonts\":" + std::to_string(resources.fonts) +
            ",\"textures\":" + std::to_string(resources.textures) +
            ",\"hotkeys\":" + std::to_string(resources.hotkeys) +
            "},\"queuedTasks\":" + std::to_string(platform.queued_tasks) +
            ",\"callbacks\":{\"updateSlow\":" +
                std::to_string(plugin.update_metrics.slow_calls) +
            ",\"drawSlow\":" + std::to_string(plugin.draw_metrics.slow_calls) +
            ",\"scoped\":{\"calls\":" +
                std::to_string(platform.scoped_callbacks.calls) +
            ",\"faults\":" + std::to_string(platform.scoped_callbacks.faults) +
            ",\"slowCalls\":" + std::to_string(platform.scoped_callbacks.slow_calls) +
            "}},\"ipcEndpoints\":[";
        for (std::size_t ipc_index = 0; ipc_index < platform.ipc_endpoints.size(); ++ipc_index) {
            if (ipc_index != 0) output.push_back(',');
            const anomaly::IpcEndpointDiagnostics& endpoint = platform.ipc_endpoints[ipc_index];
            output += "{\"id\":" + json::Quote(endpoint.id) +
                ",\"provider\":" + json::Quote(endpoint.provider) +
                ",\"consumers\":";
            append_strings(endpoint.consumers);
            output += ",\"generation\":" + std::to_string(endpoint.generation) +
                ",\"major\":" + std::to_string(endpoint.major_version) +
                ",\"minor\":" + std::to_string(endpoint.minor_version) +
                ",\"requestSchema\":" + json::Quote(endpoint.request_schema_hash) +
                ",\"responseSchema\":" + json::Quote(endpoint.response_schema_hash) +
                ",\"eventSchema\":" + json::Quote(endpoint.event_schema_hash) +
                ",\"modes\":" + std::to_string(endpoint.modes) +
                ",\"affinity\":" + std::to_string(endpoint.affinity) +
                ",\"calls\":" + std::to_string(endpoint.calls) +
                ",\"failures\":" + std::to_string(endpoint.failures) +
                ",\"timeouts\":" + std::to_string(endpoint.timeouts) +
                ",\"events\":" + std::to_string(endpoint.events) +
                ",\"subscriptions\":" + std::to_string(endpoint.subscriptions) +
                ",\"pendingCalls\":" + std::to_string(endpoint.pending_calls) +
                ",\"p95Milliseconds\":" + std::to_string(endpoint.p95_milliseconds) + '}';
        }
        output += "],\"denyReasons\":";
        append_strings(platform.deny_reasons);
        output.push_back('}');
    }
    output += "]}";
    return output;
}

std::filesystem::path PluginManager::PackageDirectory(std::string_view plugin_id) const {
    const auto found = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin->view.id == plugin_id;
    });
    return found == plugins_.end() ? std::filesystem::path{} : (*found)->view.package_directory;
}

bool PluginManager::SetVisible(std::string_view plugin_id, bool visible) {
    const auto found = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& plugin) {
        return plugin->view.id == plugin_id;
    });
    if (found == plugins_.end()) return false;
    auto callback = (*found)->scope != nullptr
        ? (*found)->scope->AcquireCallback((*found)->view.generation)
        : anomaly::PluginScope::CallbackLease{};
    if ((*found)->scope != nullptr && !callback) return false;
    ScopedPluginCallback callback_scope(
        (*found)->scope, (*found)->view.generation, false);
    const anomaly::UiWindowGroupState windows =
        ui_resources_->WindowGroupState((*found)->scope);
    if (windows.window_count != 0 &&
        !ui_resources_->SetWindowGroupOpen((*found)->scope, visible)) {
        return false;
    }
    if (windows.window_count == 0) {
        SetPersistentPluginWindowVisibility((*found)->view.id, visible);
    }
    (*found)->view.visible = visible;
    (*found)->ui_proxy_context.reopen_requested = visible;
    ReconcileWindowVisibility(*(*found));
    return (*found)->view.visibility_control;
}

}  // namespace ue5mem
