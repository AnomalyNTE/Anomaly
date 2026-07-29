#include "plugin_manager.hpp"
#include "anomaly/structured_logger.hpp"
#include "anomaly/ui_resource_decoder.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using Json = nlohmann::json;

namespace {

constexpr std::string_view kRawMemoryCapabilityFixtureId =
    "anomaly.fixture.raw-memory-capability";

struct WindowProbe {
    bool close{};
    std::string title;
};

int ANOMALY_CALL CloseWindow(
    void* user, const AnomalyStringViewV1 title, int* open, std::uint32_t) {
    auto* probe = static_cast<WindowProbe*>(user);
    if (probe != nullptr && title.data != nullptr) {
        probe->title.assign(title.data, title.size);
    }
    if (open != nullptr && probe != nullptr && probe->close) *open = 0;
    return 0;
}

void ANOMALY_CALL EndWindow(void*) {}
void ANOMALY_CALL Text(void*, AnomalyStringViewV1) {}

struct DeferredUiResourceWorker {
    struct Task {
        std::string owner;
        std::uint64_t generation{};
        std::function<void()> callback;
    };

    [[nodiscard]] bool Post(
        std::string owner, const std::uint64_t generation, std::function<void()> callback) {
        if (!accepting || !callback) return false;
        tasks.push_back({std::move(owner), generation, std::move(callback)});
        return true;
    }

    [[nodiscard]] bool HasSingleTask(
        const std::string_view owner, const std::uint64_t generation) const {
        return tasks.size() == 1 && tasks.front().owner == owner &&
            tasks.front().generation == generation && static_cast<bool>(tasks.front().callback);
    }

    [[nodiscard]] bool RunNext() {
        if (tasks.empty()) return false;
        Task task = std::move(tasks.front());
        tasks.erase(tasks.begin());
        task.callback();
        return true;
    }

    [[nodiscard]] bool DropNext() {
        if (tasks.empty()) return false;
        tasks.erase(tasks.begin());
        return true;
    }

    bool accepting{true};
    std::vector<Task> tasks;
};

struct ScopedTemporaryFile {
    std::filesystem::path path;

    ~ScopedTemporaryFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

struct ScopedTemporaryDirectory {
    std::filesystem::path path;

    ~ScopedTemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct ScopedHandle {
    HANDLE value{INVALID_HANDLE_VALUE};

    ~ScopedHandle() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

bool TestPluginCacheCleanup(const anomaly::CoreMemoryServices& memory_services) {
    ScopedTemporaryDirectory fixture{
        std::filesystem::temp_directory_path() /
        (L"anomaly-plugin-cache-cleanup-" + std::to_wstring(GetCurrentProcessId()))};
    std::error_code error;
    std::filesystem::remove_all(fixture.path, error);
    const std::filesystem::path cache_root = fixture.path / L"plugins" / L".cache";
    const std::filesystem::path stale = cache_root / L"4294967295";
    const std::filesystem::path active = cache_root / L"4294967294";
    std::filesystem::create_directories(stale / L"packages", error);
    if (error) return false;
    std::filesystem::create_directories(active, error);
    if (error) return false;
    std::ofstream(stale / L"packages" / L"stale.dll", std::ios::binary) << "stale";
    ScopedHandle active_owner{CreateFileW(
        (active / L".owner.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr)};
    if (active_owner.value == INVALID_HANDLE_VALUE) return false;

    {
        ue5mem::PluginManager manager(fixture.path, L"plugins", memory_services);
        const std::filesystem::path current =
            cache_root / std::to_wstring(GetCurrentProcessId());
        if (std::filesystem::exists(stale) || !std::filesystem::exists(active) ||
            !std::filesystem::exists(current / L".owner.lock")) {
            return false;
        }
    }
    if (!std::filesystem::exists(active)) return false;

    CloseHandle(active_owner.value);
    active_owner.value = INVALID_HANDLE_VALUE;
    {
        ue5mem::PluginManager manager(fixture.path, L"plugins", memory_services);
        if (std::filesystem::exists(active)) return false;
    }
    return !std::filesystem::exists(cache_root);
}

bool TestUiResourceWorkerStaging(
    ue5mem::PluginManager& manager, DeferredUiResourceWorker& worker,
    const std::filesystem::path& root) {
    ScopedTemporaryFile font_file{root / L"ui-worker-staging-font.bin"};
    std::error_code file_error;
    std::filesystem::remove(font_file.path, file_error);
    const std::vector<std::uint8_t> font_bytes{0x74, 0x74, 0x66, 0x2d, 0x73, 0x74, 0x61, 0x67, 0x65};
    {
        std::ofstream output(font_file.path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(
            reinterpret_cast<const char*>(font_bytes.data()),
            static_cast<std::streamsize>(font_bytes.size()));
        if (!output) return false;
    }

    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto active_scope = std::make_shared<anomaly::PluginScope>(
        ledger, "anomaly.test.ui-worker", 41);
    anomaly::UiFontRequest font_request;
    font_request.relative_path = font_file.path.string();
    font_request.size_pixels = 16.0F;
    font_request.glyph_range = anomaly::UiGlyphRange::Latin;
    const auto font = manager.UiResources().RequestFont(active_scope, std::move(font_request));
    if (!font || !manager.QueueUiFontLoad(active_scope, font) ||
        !worker.HasSingleTask(active_scope->Owner(), active_scope->Generation()) ||
        !manager.UiResources().ReserveResourceStaging(
            active_scope, font, font_bytes.size())) {
        return false;
    }
    anomaly::UiFontRequest duplicate_font_request;
    duplicate_font_request.relative_path = font_file.path.string();
    duplicate_font_request.size_pixels = 16.0F;
    duplicate_font_request.glyph_range = anomaly::UiGlyphRange::Latin;
    const auto duplicate_font = manager.UiResources().RequestFont(
        active_scope, std::move(duplicate_font_request));
    if (!duplicate_font || !manager.QueueUiFontLoad(active_scope, duplicate_font) ||
        !manager.UiResources().Release(active_scope, font) ||
        !worker.HasSingleTask(active_scope->Owner(), active_scope->Generation()) ||
        !worker.RunNext()) {
        return false;
    }
    const auto staged_font = manager.UiResources().FontState(active_scope, duplicate_font);
    if (!staged_font || staged_font->resource.state != anomaly::UiResourceState::Queued ||
        staged_font->request.encoded_bytes != font_bytes) {
        return false;
    }

    // The Worker must fall back to the earlier duplicate when the newest
    // pending lease is released before its deferred callback starts.
    anomaly::UiFontRequest fallback_font_request;
    fallback_font_request.relative_path = font_file.path.string();
    fallback_font_request.size_pixels = 17.0F;
    fallback_font_request.glyph_range = anomaly::UiGlyphRange::Latin;
    const auto fallback_font = manager.UiResources().RequestFont(
        active_scope, std::move(fallback_font_request));
    anomaly::UiFontRequest released_font_request;
    released_font_request.relative_path = font_file.path.string();
    released_font_request.size_pixels = 17.0F;
    released_font_request.glyph_range = anomaly::UiGlyphRange::Latin;
    const auto released_font = manager.UiResources().RequestFont(
        active_scope, std::move(released_font_request));
    if (!fallback_font || !released_font || !manager.QueueUiFontLoad(active_scope, fallback_font) ||
        !manager.QueueUiFontLoad(active_scope, released_font) ||
        !manager.UiResources().Release(active_scope, released_font) ||
        !worker.HasSingleTask(active_scope->Owner(), active_scope->Generation()) || !worker.RunNext()) {
        return false;
    }
    const auto fallback_font_state = manager.UiResources().FontState(active_scope, fallback_font);
    if (!fallback_font_state || fallback_font_state->resource.state != anomaly::UiResourceState::Queued ||
        fallback_font_state->request.encoded_bytes != font_bytes) {
        return false;
    }

    const std::vector<std::uint8_t> rgba{0x11, 0x22, 0x33, 0x44};
    anomaly::UiTextureRequest raw_texture_request;
    raw_texture_request.encoded_bytes = rgba;
    raw_texture_request.format = anomaly::UiTextureFormat::Rgba8;
    raw_texture_request.width = 1;
    raw_texture_request.height = 1;
    const auto raw_texture =
        manager.UiResources().RequestTexture(active_scope, std::move(raw_texture_request));
    if (!raw_texture || !manager.QueueUiTextureLoad(active_scope, raw_texture) || !worker.tasks.empty() ||
        !manager.UiResources().MarkTextureReady(
            active_scope, raw_texture, manager.UiResources().DeviceGeneration(), 1, 1) ||
        !manager.QueueUiTextureLoad(active_scope, raw_texture) || !worker.tasks.empty()) {
        return false;
    }
    const auto staged_raw_texture = manager.UiResources().TextureState(active_scope, raw_texture);
    if (!staged_raw_texture || staged_raw_texture->resource.state != anomaly::UiResourceState::Ready ||
        staged_raw_texture->request.format != anomaly::UiTextureFormat::Rgba8 ||
        staged_raw_texture->request.width != 1 || staged_raw_texture->request.height != 1 ||
        staged_raw_texture->request.encoded_bytes != rgba || staged_raw_texture->byte_size != rgba.size()) {
        return false;
    }

    anomaly::UiTextureRequest invalid_encoded_request;
    invalid_encoded_request.encoded_bytes = {0x00};
    invalid_encoded_request.format = anomaly::UiTextureFormat::Auto;
    const auto invalid_encoded =
        manager.UiResources().RequestTexture(active_scope, std::move(invalid_encoded_request));
    anomaly::UiTextureRequest duplicate_invalid_encoded_request;
    duplicate_invalid_encoded_request.encoded_bytes = {0x00};
    duplicate_invalid_encoded_request.format = anomaly::UiTextureFormat::Auto;
    const auto duplicate_invalid_encoded = manager.UiResources().RequestTexture(
        active_scope, std::move(duplicate_invalid_encoded_request));
    if (!invalid_encoded || !duplicate_invalid_encoded ||
        !manager.QueueUiTextureLoad(active_scope, invalid_encoded) ||
        !manager.QueueUiTextureLoad(active_scope, duplicate_invalid_encoded) ||
        !manager.UiResources().Release(active_scope, invalid_encoded) ||
        !worker.HasSingleTask(active_scope->Owner(), active_scope->Generation()) ||
        !worker.RunNext()) {
        return false;
    }
    const auto failed_encoded = manager.UiResources().TextureState(
        active_scope, duplicate_invalid_encoded);
    if (!failed_encoded || failed_encoded->resource.state != anomaly::UiResourceState::Failed) {
        return false;
    }

    anomaly::UiTextureRequest fallback_texture_request;
    fallback_texture_request.encoded_bytes = {0x01};
    fallback_texture_request.format = anomaly::UiTextureFormat::Auto;
    const auto fallback_texture = manager.UiResources().RequestTexture(
        active_scope, std::move(fallback_texture_request));
    anomaly::UiTextureRequest released_texture_request;
    released_texture_request.encoded_bytes = {0x01};
    released_texture_request.format = anomaly::UiTextureFormat::Auto;
    const auto released_texture = manager.UiResources().RequestTexture(
        active_scope, std::move(released_texture_request));
    if (!fallback_texture || !released_texture ||
        !manager.QueueUiTextureLoad(active_scope, fallback_texture) ||
        !manager.QueueUiTextureLoad(active_scope, released_texture) ||
        !manager.UiResources().Release(active_scope, released_texture) ||
        !worker.HasSingleTask(active_scope->Owner(), active_scope->Generation()) || !worker.RunNext()) {
        return false;
    }
    const auto fallback_texture_state = manager.UiResources().TextureState(
        active_scope, fallback_texture);
    if (!fallback_texture_state ||
        fallback_texture_state->resource.state != anomaly::UiResourceState::Failed) {
        return false;
    }

    anomaly::UiTextureRequest oversized_encoded_request;
    oversized_encoded_request.encoded_bytes.resize(
        anomaly::kDefaultUiResourceEncodedByteLimit + 1U, 0U);
    oversized_encoded_request.format = anomaly::UiTextureFormat::Auto;
    const auto oversized_encoded =
        manager.UiResources().RequestTexture(active_scope, std::move(oversized_encoded_request));
    if (!oversized_encoded || !manager.QueueUiTextureLoad(active_scope, oversized_encoded) ||
        !worker.HasSingleTask(active_scope->Owner(), active_scope->Generation()) ||
        !worker.RunNext()) {
        return false;
    }
    const auto oversized_state = manager.UiResources().ResourceState(active_scope, oversized_encoded);
    if (!oversized_state || oversized_state->state != anomaly::UiResourceState::Failed) {
        return false;
    }

    const auto stale_scope = std::make_shared<anomaly::PluginScope>(
        ledger, "anomaly.test.ui-worker", 42);
    anomaly::UiFontRequest stale_font_request;
    stale_font_request.relative_path = font_file.path.string();
    stale_font_request.size_pixels = 18.0F;
    stale_font_request.glyph_range = anomaly::UiGlyphRange::Latin;
    const auto stale_font =
        manager.UiResources().RequestFont(stale_scope, std::move(stale_font_request));
    if (!stale_font || !manager.QueueUiFontLoad(stale_scope, stale_font) ||
        !worker.HasSingleTask(stale_scope->Owner(), stale_scope->Generation()) ||
        !stale_scope->FreezeCallbackSources() || !worker.RunNext()) {
        return false;
    }
    const auto stale_state = manager.UiResources().FontState(stale_scope, stale_font);
    if (!stale_state || !stale_state->request.encoded_bytes.empty() ||
        stale_state->resource.state != anomaly::UiResourceState::Queued ||
        stale_scope->RevokeAll() != 1 || manager.UiResources().FontState(stale_scope, stale_font)) {
        return false;
    }

    return active_scope->RevokeAll() == 6 && worker.tasks.empty();
}

bool TestUiResourceWorkerTerminalPaths(
    ue5mem::PluginManager& manager, DeferredUiResourceWorker& worker,
    const std::filesystem::path& root) {
    ScopedTemporaryFile font_file{root / L"ui-worker-terminal-font.bin"};
    const std::vector<std::uint8_t> font_bytes{0x74, 0x74, 0x66, 0x2d, 0x74, 0x65, 0x72, 0x6d};
    {
        std::ofstream output(font_file.path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(
            reinterpret_cast<const char*>(font_bytes.data()),
            static_cast<std::streamsize>(font_bytes.size()));
        if (!output) return false;
    }

    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = std::make_shared<anomaly::PluginScope>(
        ledger, "anomaly.test.ui-worker-terminal", 44);
    anomaly::UiFontRequest request;
    request.relative_path = font_file.path.string();
    request.size_pixels = 18.0F;
    request.glyph_range = anomaly::UiGlyphRange::Latin;
    const auto font = manager.UiResources().RequestFont(scope, std::move(request));
    if (!font) return false;

    worker.accepting = false;
    if (manager.QueueUiFontLoad(scope, font)) return false;
    const auto rejected = manager.UiResources().ResourceState(scope, font);
    if (!rejected || rejected->state != anomaly::UiResourceState::Failed ||
        rejected->reserved_staging_bytes != 0 || !worker.tasks.empty()) {
        return false;
    }

    worker.accepting = true;
    if (!manager.UiResources().RetryFont(scope, font) ||
        !manager.QueueUiFontLoad(scope, font) ||
        !worker.HasSingleTask(scope->Owner(), scope->Generation()) || !worker.DropNext()) {
        return false;
    }
    const auto cancelled = manager.UiResources().ResourceState(scope, font);
    if (!cancelled || cancelled->state != anomaly::UiResourceState::Failed ||
        cancelled->reserved_staging_bytes != 0 || !worker.tasks.empty()) {
        return false;
    }

    if (!manager.UiResources().RetryFont(scope, font) ||
        !manager.QueueUiFontLoad(scope, font) ||
        !worker.HasSingleTask(scope->Owner(), scope->Generation()) || !worker.RunNext()) {
        return false;
    }
    const auto completed = manager.UiResources().FontState(scope, font);
    return completed && completed->resource.state == anomaly::UiResourceState::Queued &&
        completed->request.encoded_bytes == font_bytes && scope->RevokeAll() == 1;
}

bool TestUiResourceWorkerConfinedPackagePaths(
    ue5mem::PluginManager& manager, DeferredUiResourceWorker& worker,
    const std::filesystem::path& root) {
    ScopedTemporaryDirectory package{root / L"ui-worker-confined-package"};
    std::error_code error;
    std::filesystem::remove_all(package.path, error);
    error.clear();
    std::filesystem::create_directories(package.path / L"assets", error);
    if (error) return false;

    const std::vector<std::uint8_t> bytes{0x63, 0x6f, 0x6e, 0x66, 0x69, 0x6e, 0x65, 0x64};
    {
        std::ofstream output(package.path / L"assets" / L"font.bin", std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) return false;
    }

    const auto ledger = std::make_shared<anomaly::ResourceLedger>();
    const auto scope = std::make_shared<anomaly::PluginScope>(
        ledger, "anomaly.test.ui-worker-confined-package", 45);
    anomaly::UiFontRequest existing;
    existing.package_directory = package.path;
    existing.relative_path = "assets/font.bin";
    existing.size_pixels = 18.0F;
    existing.glyph_range = anomaly::UiGlyphRange::Latin;
    const auto existing_font = manager.UiResources().RequestFont(scope, std::move(existing));
    if (!existing_font || !manager.QueueUiFontLoad(scope, existing_font) ||
        !worker.HasSingleTask(scope->Owner(), scope->Generation()) || !worker.RunNext()) {
        return false;
    }
    const auto loaded = manager.UiResources().FontState(scope, existing_font);
    if (!loaded || loaded->resource.state != anomaly::UiResourceState::Queued ||
        loaded->request.encoded_bytes != bytes) {
        return false;
    }

    // A package-relative AUTO texture must stay in the Worker domain through
    // file confinement and WIC decode before it becomes an uploadable RGBA8 payload.
    const std::vector<std::uint8_t> bitmap{
        0x42, 0x4d, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
        0x28, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22,
        0x33, 0x00};
    {
        std::ofstream output(package.path / L"assets" / L"texture.bmp", std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(bitmap.data()), static_cast<std::streamsize>(bitmap.size()));
        if (!output) return false;
    }
    anomaly::UiTextureRequest texture;
    texture.package_directory = package.path;
    texture.relative_path = "assets/texture.bmp";
    texture.format = anomaly::UiTextureFormat::Auto;
    const auto package_texture = manager.UiResources().RequestTexture(scope, std::move(texture));
    if (!package_texture || !manager.QueueUiTextureLoad(scope, package_texture) ||
        !worker.HasSingleTask(scope->Owner(), scope->Generation()) || !worker.RunNext()) {
        return false;
    }
    const auto decoded = manager.UiResources().TextureState(scope, package_texture);
    if (!decoded || decoded->resource.state != anomaly::UiResourceState::Queued ||
        decoded->request.format != anomaly::UiTextureFormat::Rgba8 ||
        decoded->request.width != 1 || decoded->request.height != 1 ||
        decoded->request.encoded_bytes.size() != 4) {
        return false;
    }

    anomaly::UiFontRequest missing;
    missing.package_directory = package.path;
    missing.relative_path = "assets/missing.ttf";
    missing.size_pixels = 18.0F;
    missing.glyph_range = anomaly::UiGlyphRange::Latin;
    const auto missing_font = manager.UiResources().RequestFont(scope, std::move(missing));
    if (!missing_font || !manager.QueueUiFontLoad(scope, missing_font) ||
        !worker.HasSingleTask(scope->Owner(), scope->Generation()) || !worker.RunNext()) {
        return false;
    }
    const auto missing_state = manager.UiResources().FontState(scope, missing_font);
    return missing_state && missing_state->resource.state == anomaly::UiResourceState::Failed &&
        worker.tasks.empty() && scope->RevokeAll() == 3;
}

bool HasService(
    const ue5mem::PluginPlatformDiagnosticsView& diagnostics,
    const std::string_view id) {
    return std::any_of(
        diagnostics.services.begin(), diagnostics.services.end(),
        [&](const ue5mem::PluginServiceVersionView& service) { return service.id == id; });
}

bool HasDeny(
    const ue5mem::PluginPlatformDiagnosticsView& diagnostics,
    const std::string_view service_id) {
    return std::any_of(
        diagnostics.deny_reasons.begin(), diagnostics.deny_reasons.end(),
        [&](const std::string& reason) { return reason.starts_with(service_id); });
}

struct RawMemoryCapabilityCase {
    const wchar_t* directory_name;
    std::vector<std::string> capabilities;
};

bool WriteRawMemoryCapabilityManifest(
    const std::filesystem::path& package_directory,
    const RawMemoryCapabilityCase& test_case) {
    const Json manifest = {
        {"schemaVersion", 2},
        {"id", std::string(kRawMemoryCapabilityFixtureId)},
        {"name", "Raw Memory Capability Fixture"},
        {"author", "Anomaly"},
        {"version", "1.0.0"},
        {"entry", "plugin.dll"},
        {"api", {{"major", 1}, {"minMinor", 0}, {"maxMinor", 0}}},
        {"games", {"nte"}},
        {"builds", {"nte-win64-*"}},
        {"loadPhase", "game-ready"},
        {"services", Json::array({Json{
            {"id", ANOMALY_CORE_SERVICE_V1_ID},
            {"minVersion", ANOMALY_CORE_SERVICE_V1_VERSION}}})},
        {"capabilities", test_case.capabilities},
    };
    std::ofstream output(
        package_directory / L"manifest.json", std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << manifest.dump(2) << '\n';
    return static_cast<bool>(output);
}

bool TestRawMemoryCapabilityRuntime(
    const std::filesystem::path& root,
    const anomaly::CoreMemoryServices& memory_services) {
    const std::filesystem::path fixture =
        root / L"raw-memory-capability-fixture" / L"plugin.dll";
    std::error_code error;
    if (!std::filesystem::is_regular_file(fixture, error) || error) {
        std::cerr << "raw-memory capability fixture binary is missing\n";
        return false;
    }

    ScopedTemporaryDirectory test_root{root / L"raw-memory-capability-contract"};
    std::filesystem::remove_all(test_root.path, error);
    if (error) {
        std::cerr << "raw-memory capability fixture root cleanup failed\n";
        return false;
    }

    const std::vector<RawMemoryCapabilityCase> cases{
        {L"no-memory", {}},
        {L"read-only", {"memory-read"}},
        {L"write-only", {"memory-write"}},
    };
    const std::string started_event =
        "ABI v1 plugin started: " + std::string(kRawMemoryCapabilityFixtureId);

    for (const RawMemoryCapabilityCase& test_case : cases) {
        const std::filesystem::path case_root = test_root.path / test_case.directory_name;
        const std::filesystem::path package = case_root / L"plugins" / L"RawMemoryCapability";
        const auto fail_case = [&](const std::string_view reason) {
            std::cerr << "raw-memory capability fixture "
                      << std::filesystem::path(test_case.directory_name).string()
                      << ": " << reason << '\n';
            return false;
        };
        std::filesystem::create_directories(package, error);
        if (error) return fail_case("could not create package directory");
        if (!std::filesystem::copy_file(
                fixture, package / L"plugin.dll",
                std::filesystem::copy_options::overwrite_existing, error) ||
            error) {
            return fail_case("could not copy fixture binary");
        }
        if (!WriteRawMemoryCapabilityManifest(package, test_case)) {
            return fail_case("could not write manifest");
        }

        {
            ue5mem::PluginManager manager(case_root, L"plugins", memory_services);
            manager.LoadAll();
            if (!manager.SetEnabled(kRawMemoryCapabilityFixtureId, true)) {
                return fail_case("could not explicitly enable fixture");
            }

            const auto plugins = manager.Plugins();
            if (plugins.size() != 1 || plugins.front().id != kRawMemoryCapabilityFixtureId ||
                plugins.front().generation == 0) {
                std::cerr << "raw-memory capability fixture did not activate: "
                          << std::filesystem::path(test_case.directory_name).string() << '\n';
                for (const std::string& event : manager.Events()) std::cerr << "  " << event << '\n';
                return false;
            }
            const auto diagnostics = manager.DiagnosticsSnapshot();
            if (diagnostics.plugins.size() != 1) return fail_case("did not publish one diagnostics view");
            const auto& platform = diagnostics.plugins.front().platform_diagnostics;
            if (!platform.capability_enforced ||
                platform.capabilities.size() != test_case.capabilities.size()) {
                return fail_case("published unexpected capability diagnostics");
            }
            for (const std::string& capability : test_case.capabilities) {
                if (std::find(
                        platform.capabilities.begin(), platform.capabilities.end(), capability) ==
                    platform.capabilities.end()) {
                    return fail_case("omitted a declared capability");
                }
            }
            const auto events = manager.Events();
            if (std::none_of(events.begin(), events.end(), [&](const std::string& event) {
                    return event.find(started_event) != std::string::npos;
                })) {
                return fail_case("did not publish the start event");
            }
        }
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::filesystem::path root(argv[1]);
    const std::filesystem::path structured_log =
        root / L"logs" / L"plugin-manager-integration.jsonl";
    std::error_code log_error;
    std::filesystem::create_directories(structured_log.parent_path(), log_error);
    std::filesystem::remove(structured_log, log_error);
    auto logger = std::make_shared<anomaly::StructuredLogger>();
    if (!logger->Start(structured_log)) return 13;
    const auto memory_services = anomaly::CreateCoreMemoryServices();
    if (!TestPluginCacheCleanup(memory_services)) return 34;
    if (!TestRawMemoryCapabilityRuntime(root, memory_services)) return 33;
    DeferredUiResourceWorker ui_resource_worker;
    ue5mem::PluginManager manager(
        root, L"plugins", memory_services, {}, logger, {},
        [&ui_resource_worker](
            std::string owner, const std::uint64_t generation,
            std::function<void()> callback) -> bool {
            return ui_resource_worker.Post(std::move(owner), generation, std::move(callback));
        });
    const auto& manager_services = manager.MemoryServices();
    if (manager_services.memory.get() != memory_services.memory.get() ||
        manager_services.patterns.get() != memory_services.patterns.get() ||
        manager_services.patterns->Memory().get() != manager_services.memory.get()) {
        return 2;
    }
    if (!TestUiResourceWorkerStaging(manager, ui_resource_worker, root)) return 30;
    if (!TestUiResourceWorkerTerminalPaths(manager, ui_resource_worker, root)) return 31;
    if (!TestUiResourceWorkerConfinedPackagePaths(manager, ui_resource_worker, root)) return 32;
    bool duplicate_rejected{};
    try {
        ue5mem::PluginManager duplicate(root, L"plugins", memory_services);
    } catch (const std::logic_error&) {
        duplicate_rejected = true;
    }
    if (!duplicate_rejected) return 3;
    manager.LoadAll();
    if (!manager.SetEnabled("anomaly.test.plugin-manager-fixture", true)) return 4;
    AnomalyUiServiceV1 ui_service{};
    ui_service.struct_size = sizeof(ui_service);
    ui_service.service_version = 1;
    manager.SetUiService(&ui_service);

    auto plugins = manager.Plugins();
    if (plugins.size() != 1) return 4;
    const auto& plugin = plugins.front();
    if (plugin.id != "anomaly.test.plugin-manager-fixture" || !plugin.visibility_control ||
        !plugin.visible || plugin.package_directory.filename() != L"PluginManagerFixture" ||
        !std::filesystem::exists(plugin.package_directory / L"watch.txt")) {
        return 5;
    }
    const auto diagnostics = manager.DiagnosticsSnapshot();
    if (diagnostics.schema_version != 1 || diagnostics.plugins.size() != 1) return 19;
    const auto& platform = diagnostics.plugins.front().platform_diagnostics;
    const bool ui_capability = std::find(
        platform.capabilities.begin(), platform.capabilities.end(), "ui") !=
        platform.capabilities.end();
    if (!platform.capability_enforced || !ui_capability ||
        !HasService(platform, ANOMALY_CORE_SERVICE_V1_ID) ||
        !HasService(platform, ANOMALY_UI_SERVICE_V1_ID) ||
        platform.resources.windows != 0 || platform.resources.fonts != 0 ||
        platform.resources.textures != 0 || platform.resources.hotkeys != 0 ||
        !HasDeny(platform, "anomaly.config")) {
        return 20;
    }
    const std::string diagnostics_json = manager.DiagnosticsJson();
    const Json diagnostics_document = Json::parse(diagnostics_json, nullptr, false);
    if (diagnostics_document.is_discarded() || diagnostics_document.value("schemaVersion", 0U) != 1U ||
        !diagnostics_document.contains("plugins") || !diagnostics_document["plugins"].is_array() ||
        diagnostics_document["plugins"].size() != 1 ||
        !diagnostics_document["plugins"][0].value("capabilityEnforced", false) ||
        !diagnostics_document["plugins"][0].contains("services") ||
        !diagnostics_document["plugins"][0].contains("resources") ||
        !diagnostics_document["plugins"][0]["resources"].contains("windows") ||
        !diagnostics_document["plugins"][0]["resources"].contains("fonts") ||
        !diagnostics_document["plugins"][0]["resources"].contains("textures") ||
        !diagnostics_document["plugins"][0]["resources"].contains("hotkeys") ||
        !diagnostics_document["plugins"][0].contains("queuedTasks") ||
        !diagnostics_document["plugins"][0].contains("callbacks") ||
        !diagnostics_document["plugins"][0].contains("denyReasons")) {
        return 21;
    }
    const std::string plugin_id = plugin.id;
    const std::filesystem::path package_directory = plugin.package_directory;
    const auto startup_events = manager.Events();
    if (std::none_of(startup_events.begin(), startup_events.end(), [](const std::string& event) {
            return event.find("ABI v1 plugin started: anomaly.test.plugin-manager-fixture") !=
                std::string::npos;
        })) {
        return 6;
    }
    if (!manager.SetVisible(plugin_id, false)) return 7;
    plugins = manager.Plugins();
    if (plugins.size() != 1 || plugins.front().visible) return 8;
    if (!manager.SetVisible(plugin_id, true)) return 9;
    plugins = manager.Plugins();
    if (plugins.size() != 1 || !plugins.front().visible) return 10;

    WindowProbe window_probe{true};
    ui_service.user = &window_probe;
    ui_service.begin_window = CloseWindow;
    ui_service.end_window = EndWindow;
    ui_service.text = Text;
    manager.Draw(nullptr);
    plugins = manager.Plugins();
    if (plugins.size() != 1 || plugins.front().visible ||
        !window_probe.title.starts_with("Plugin Manager Fixture###anomaly-plugin:") ||
        window_probe.title == "Plugin Manager Fixture") {
        return 16;
    }
    if (!manager.SetVisible(plugin_id, true)) return 17;
    window_probe.close = false;
    manager.Draw(nullptr);
    plugins = manager.Plugins();
    if (plugins.size() != 1 || !plugins.front().visible) return 18;

    const std::filesystem::path watch_file = package_directory / L"watch.txt";
    std::ifstream original_input(watch_file, std::ios::binary);
    const std::string original(
        (std::istreambuf_iterator<char>(original_input)), std::istreambuf_iterator<char>());
    original_input.close();
    std::ofstream(watch_file, std::ios::binary | std::ios::app) << "\nwatcher-probe";
    manager.Maintenance();
    std::this_thread::sleep_for(800ms);
    manager.Maintenance();
    plugins = manager.Plugins();
    if (plugins.size() != 1 || plugins.front().id != plugin_id) return 11;
    const auto& events = manager.Events();
    if (std::none_of(events.begin(), events.end(), [](const std::string& event) {
            return event.find("plugin package change applied: PluginManagerFixture") != std::string::npos;
        })) {
        return 12;
    }
    std::ofstream(watch_file, std::ios::binary | std::ios::trunc) << original;

    manager.LogPlugin(
        ANOMALY_CORE_LOG_LEVEL_V1_INFO, "structured-log-probe", plugin_id,
        plugins.front().generation);
    if (!logger->Flush(2s)) return 14;
    std::ifstream structured_input(structured_log, std::ios::binary);
    const std::string structured_records(
        (std::istreambuf_iterator<char>(structured_input)),
        std::istreambuf_iterator<char>());
    if (structured_records.find("\"component\":\"plugin-manager\"") ==
            std::string::npos ||
        structured_records.find("\"thread_domain\":\"unknown\"") ==
            std::string::npos ||
        structured_records.find("\"event_id\":\"plugin.host\"") ==
            std::string::npos ||
        structured_records.find(
            "\"plugin_id\":\"anomaly.test.plugin-manager-fixture\"") ==
            std::string::npos ||
        structured_records.find("\"plugin_generation\":" +
            std::to_string(plugins.front().generation)) == std::string::npos ||
        structured_records.find("\"message\":\"structured-log-probe\"") ==
            std::string::npos) {
        return 15;
    }

    std::cout << "plugin catalog package, selective reload and visibility controls passed\n";
    return 0;
}
