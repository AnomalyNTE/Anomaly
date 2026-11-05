#include "anomaly/host_ui_service.hpp"
#include "anomaly/hook_manager.hpp"
#include "anomaly/plugin_config_watcher.hpp"
#include "anomaly/plugin_file_watcher.hpp"
#include "anomaly/plugin_shadow_store.hpp"
#include "pattern.hpp"
#include "plugin_manager.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct UiProbe { std::uint64_t windows{}; std::uint64_t text{}; std::uint64_t buttons{}; } g_ui;
void ANOMALY_CALL SetSize(void*, float, float, std::uint32_t) {}
int ANOMALY_CALL Begin(void*, AnomalyStringViewV1, int*, std::uint32_t) { ++g_ui.windows; return 1; }
void ANOMALY_CALL End(void*) {}
void ANOMALY_CALL Text(void*, AnomalyStringViewV1) { ++g_ui.text; }
int ANOMALY_CALL Button(void*, AnomalyStringViewV1, float, float) { ++g_ui.buttons; return 0; }
int ANOMALY_CALL InputUInt32(
    void*, AnomalyStringViewV1, std::uint32_t*, std::uint32_t, std::uint32_t) {
    return 0;
}
int ANOMALY_CALL InputDouble(
    void*, AnomalyStringViewV1, double*, double, double) {
    return 0;
}
void ANOMALY_CALL SameLine(void*, float, float) {}
void ANOMALY_CALL SetCursorPosX(void*, float) {}
int ANOMALY_CALL TextLink(void*, AnomalyStringViewV1, AnomalyStringViewV1) { return 0; }
const AnomalyUiServiceV1 kUi = [] {
    AnomalyUiServiceV1 ui{};
    ui.struct_size = sizeof(ui);
    ui.service_version = ANOMALY_UI_SERVICE_V1_VERSION;
    ui.set_next_window_size = SetSize;
    ui.begin_window = Begin;
    ui.end_window = End;
    ui.text = Text;
    ui.button = Button;
    ui.input_uint32 = InputUInt32;
    ui.input_double = InputDouble;
    ui.same_line = SameLine;
    ui.set_cursor_pos_x = SetCursorPosX;
    ui.text_link = TextLink;
    return ui;
}();

constexpr std::uint64_t kDefaultPrivateGrowthBudgetBytes = 10ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxPrivateGrowthBudgetBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;

void Usage() {
    std::cerr << "usage: anomaly-test-host --watcher-fixture | --config-watcher-fixture | "
                 "--hook-stop-order-fixture "
                 "--enablement-drain-fixture "
                 "--parallel-stage-fixture | "
                 "--plugin <package-or-root> [--reload N] [--ticks N] "
                 "[--duration-seconds N] [--tick-interval-ms N] [--reload-every-ticks N] "
                 "[--private-growth-budget-bytes N]\n";
}

bool ParseUnsigned(std::wstring_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    std::uint64_t parsed{};
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return false;
        const auto digit = static_cast<std::uint64_t>(character - L'0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10ULL) {
            return false;
        }
        parsed = parsed * 10ULL + digit;
    }
    value = parsed;
    return true;
}

std::uint64_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (GetProcessMemoryInfo(
            GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            static_cast<DWORD>(sizeof(counters))) == FALSE) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.PrivateUsage);
}

bool VerifyPatternMatcher() {
    const std::array<std::uint8_t, 10> bytes{
        0x00, 0xA1, 0x2B, 0xCC, 0xA7, 0x3B, 0xCC, 0x7F, 0x7F, 0x7F};
    const auto masked = ue5mem::Pattern::Parse("A? ?B CC");
    if (masked.FindAll(bytes) != std::vector<std::size_t>{1, 4}) return false;

    const auto overlap = ue5mem::Pattern::Parse("7F 7F");
    if (overlap.FindAll(bytes) != std::vector<std::size_t>{7, 8}) return false;
    if (overlap.FindAll(bytes, 1) != std::vector<std::size_t>{7}) return false;

    const auto wildcard = ue5mem::Pattern::Parse("??");
    if (wildcard.FindAll(std::span(bytes).first(3), 2) !=
        std::vector<std::size_t>{0, 1}) return false;
    const std::array batch{
        ue5mem::Pattern::Parse("A? ?B CC"),
        ue5mem::Pattern::Parse("7F 7F"),
        ue5mem::Pattern::Parse("??"),
        ue5mem::Pattern::Parse("A1 2B"),
        ue5mem::Pattern::Parse("?? A1 2B CC A?"),
        ue5mem::Pattern::Parse("00 00")};
    return ue5mem::Pattern::FindAllBatch(bytes, batch, 2) ==
        std::vector<std::vector<std::size_t>>{{1, 4}, {7, 8}, {0, 1}, {1}, {0}, {}};
}

bool VerifyPluginFileWatcher() {
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-watcher-fixture-" + std::to_wstring(GetCurrentProcessId()));
    const auto package = root / L"FixturePackage";
    const auto manifest = package / L"manifest.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(package, error);
    if (error) return false;
    {
        std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
        output << "{}";
        if (!output) return false;
    }

    std::mutex mutex;
    std::condition_variable changed_condition;
    std::vector<std::string> changed_packages;
    anomaly::PluginFileWatcher watcher(
        root, {std::chrono::milliseconds(20), std::chrono::milliseconds(100)});
    watcher.SetDiagnosticsEnabled(true);
    if (!watcher.Start([&](std::vector<std::string> changed) {
            {
                std::scoped_lock lock(mutex);
                changed_packages.insert(
                    changed_packages.end(), changed.begin(), changed.end());
            }
            changed_condition.notify_all();
        })) {
        std::filesystem::remove_all(root, error);
        return false;
    }
    const std::uint64_t baseline_scans = watcher.Diagnostics().scans;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const std::uint64_t settled_scans = watcher.Diagnostics().scans;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const bool idle_did_not_scan = watcher.Diagnostics().scans == settled_scans;
    {
        std::ofstream output(manifest, std::ios::binary | std::ios::app);
        output << '\n';
    }
    bool delivered{};
    {
        std::unique_lock lock(mutex);
        delivered = changed_condition.wait_for(lock, std::chrono::seconds(3), [&] {
            return std::ranges::find(changed_packages, "FixturePackage") !=
                changed_packages.end();
        });
    }
    watcher.Stop();
    const auto diagnostics = watcher.Diagnostics();
    std::filesystem::remove_all(root, error);
    const bool valid = idle_did_not_scan && delivered && diagnostics.notifications != 0 &&
        diagnostics.scans >= settled_scans + 2 && diagnostics.changed_packages != 0;
    if (!valid) {
        std::cerr << "watcher diagnostics: baseline_scans=" << baseline_scans
                  << " settled_scans=" << settled_scans
                  << " idle_scans=" << (idle_did_not_scan ? 0 : 1)
                  << " delivered=" << (delivered ? 1 : 0)
                  << " notifications=" << diagnostics.notifications
                  << " scans=" << diagnostics.scans
                  << " changed_packages=" << diagnostics.changed_packages << '\n';
    }
    return valid;
}

bool VerifyPluginConfigFileWatcher() {
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-config-watcher-fixture-" + std::to_wstring(GetCurrentProcessId()));
    const auto config = root / L"config";
    const auto file = config / L"plugin-enablement.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(config, error);
    if (error) return false;
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << "{}";
        if (!output) return false;
    }

    std::mutex mutex;
    std::condition_variable changed_condition;
    std::uint64_t changes{};
    anomaly::PluginConfigFileWatcher watcher(
        config, L"plugin-enablement.json",
        {std::chrono::milliseconds(100)});
    if (!watcher.Start([&] {
            {
                std::scoped_lock lock(mutex);
                ++changes;
            }
            changed_condition.notify_all();
        })) {
        std::filesystem::remove_all(root, error);
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    bool idle_quiet{};
    {
        std::scoped_lock lock(mutex);
        idle_quiet = changes == 0;
    }

    {
        std::ofstream output(file, std::ios::binary | std::ios::app);
        output << '\n';
    }
    bool first_delivered{};
    {
        std::unique_lock lock(mutex);
        first_delivered = changed_condition.wait_for(lock, std::chrono::seconds(3), [&] {
            return changes >= 1;
        });
    }

    // Atomic publish: temp file then rename in the watched directory.
    const auto temporary = file.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "{}";
    }
    std::filesystem::remove(file, error);
    error.clear();
    std::filesystem::rename(temporary, file, error);
    if (error) {
        watcher.Stop();
        std::filesystem::remove_all(root, error);
        return false;
    }
    bool second_delivered{};
    {
        std::unique_lock lock(mutex);
        second_delivered = changed_condition.wait_for(lock, std::chrono::seconds(3), [&] {
            return changes >= 2;
        });
    }

    watcher.Stop();
    std::filesystem::remove_all(root, error);
    const bool valid = idle_quiet && first_delivered && second_delivered;
    if (!valid) {
        std::cerr << "config watcher diagnostics: idle_quiet=" << (idle_quiet ? 1 : 0)
                  << " first_delivered=" << (first_delivered ? 1 : 0)
                  << " second_delivered=" << (second_delivered ? 1 : 0)
                  << " changes=" << changes << '\n';
    }
    return valid;
}

bool VerifyConcurrentShadowStaging() {
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-parallel-stage-fixture-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    anomaly::PluginShadowStore store(root / L"shadow");
    std::vector<anomaly::PluginCatalogEntry> entries;
    entries.reserve(8);
    for (int i = 0; i < 8; ++i) {
        auto package = root / (L"Package" + std::to_wstring(i));
        std::filesystem::create_directories(package, error);
        if (error) return false;
        {
            std::ofstream output(package / L"plugin.dll", std::ios::binary);
            output << "fixture " << i;
            if (!output) return false;
        }
        anomaly::PluginCatalogEntry entry;
        entry.package_root = std::move(package);
        entry.status = anomaly::PluginCatalogStatus::Valid;
        entry.manifest.emplace();
        entry.manifest->id = "fixture-" + std::to_string(i);
        entry.manifest->entry = "plugin.dll";
        entries.push_back(std::move(entry));
    }
    std::vector<std::future<anomaly::PluginShadowResult>> pending;
    for (const auto& entry : entries) {
        pending.push_back(std::async(std::launch::async, [&store, &entry] {
            return store.Stage(entry);
        }));
    }
    std::set<std::uint64_t> generations;
    bool valid = true;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const auto staged = pending[i].get();
        if (!staged.Ok() || !generations.insert(staged.shadow->generation).second) {
            valid = false;
            continue;
        }
        std::ifstream input(staged.shadow->entry_file, std::ios::binary);
        std::string contents;
        std::getline(input, contents);
        valid &= contents == "fixture " + std::to_string(i);
    }
    std::filesystem::remove_all(root, error);
    return valid;
}

bool VerifyEnablementConfigReload(const std::filesystem::path& root) {
    const auto inline_dispatch = [](
        std::string, std::uint64_t, std::function<void()> callback) -> bool {
        if (!callback) return false;
        callback();
        return true;
    };
    ue5mem::PluginManager manager(
        root, root, {}, {}, {}, inline_dispatch, inline_dispatch);
    manager.SetUiService(&kUi);
    manager.LoadAll();

    const auto views = manager.Plugins();
    if (views.empty() ||
        std::ranges::any_of(views, [](const auto& plugin) { return plugin.enabled; })) {
        std::cerr << "enablement fixture expected a disabled catalog first\n";
        return false;
    }
    const std::string target_id = views.front().id;
    const std::filesystem::path store = root / L"config" / L"plugin-enablement.json";
    {
        std::ofstream output(store, std::ios::binary | std::ios::trunc);
        output << "{\"defaultEnabled\": false, \"plugins\": {\"" << target_id
               << "\": true}, \"schemaVersion\": 1}\n";
        if (!output) return false;
    }

    bool enabled{};
    for (int attempt = 0; attempt < 40 && !enabled; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        static_cast<void>(manager.MaintenancePluginState());
        enabled = std::ranges::any_of(manager.Plugins(), [&](const auto& plugin) {
            return plugin.id == target_id && plugin.enabled;
        });
    }
    if (!enabled) {
        std::cerr << "enablement config change was not applied for " << target_id << '\n';
        return false;
    }
    manager.UnloadAll();
    manager.LoadAll();
    const bool reloaded = std::ranges::any_of(manager.Plugins(), [&](const auto& plugin) {
        return plugin.id == target_id && plugin.enabled;
    });
    std::cout << "ok enablement_config_reload target=" << target_id << '\n';
    return reloaded;
}

struct HookStopOrderProbe {
    std::atomic<int> batch_disable_calls{};
    std::atomic<int> individual_disable_calls{};
    std::atomic<int> remove_calls{};
};

class RejectBatchHookBackend final : public anomaly::HookBackend {
public:
    explicit RejectBatchHookBackend(std::shared_ptr<HookStopOrderProbe> probe)
        : probe_(std::move(probe)) {}

    bool Initialize() noexcept override { return true; }
    void Uninitialize() noexcept override {}

    bool Create(void* target, void*, void** original) noexcept override {
        if (target == nullptr || original == nullptr) return false;
        *original = target;
        return true;
    }

    bool Enable(void*) noexcept override { return true; }
    bool Disable(void*) noexcept override {
        ++probe_->individual_disable_calls;
        return true;
    }

    bool Remove(void*) noexcept override {
        ++probe_->remove_calls;
        return true;
    }

    bool QueueDisable(void*) noexcept override {
        ++probe_->batch_disable_calls;
        return false;
    }

    bool ApplyQueued() noexcept override { return true; }

private:
    std::shared_ptr<HookStopOrderProbe> probe_;
};

bool VerifyHookStopOrderFixture(const std::filesystem::path& root) {
    const auto inline_dispatch = [](
        std::string, std::uint64_t, std::function<void()> callback) -> bool {
        if (!callback) return false;
        callback();
        return true;
    };
    auto probe = std::make_shared<HookStopOrderProbe>();
    ue5mem::PluginManager manager(
        root, root, {}, {}, {}, inline_dispatch, inline_dispatch, {},
        std::make_unique<RejectBatchHookBackend>(probe));
    manager.LoadAll();
    auto loaded = manager.Plugins();
    auto plugin = std::ranges::find_if(loaded, [](const auto& view) {
        return view.id == "anomaly.hook-lifecycle";
    });
    if (plugin != loaded.end() && plugin->state == "disabled" &&
        manager.SetEnabled(plugin->id, true)) {
        loaded = manager.Plugins();
        plugin = std::ranges::find_if(loaded, [](const auto& view) {
            return view.id == "anomaly.hook-lifecycle";
        });
    }
    if (plugin == loaded.end() || plugin->state != "active") {
        std::cerr << "hook stop order fixture plugin did not activate";
        if (plugin == loaded.end()) {
            std::cerr << ": missing\n";
        } else {
            std::cerr << ": state=" << plugin->state
                      << " reason=" << plugin->status_reason << '\n';
        }
        return false;
    }

    const bool stopped = manager.StopForRuntime(std::chrono::seconds(1));
    const bool quarantined = std::ranges::any_of(
        manager.Plugins(), [](const auto& view) {
            return view.id == "anomaly.hook-lifecycle" &&
                view.state == "quarantined";
        });
    const bool released_from_on_stop =
        probe->individual_disable_calls.load() == 1 &&
        probe->remove_calls.load() == 1 &&
        probe->batch_disable_calls.load() == 0;
    if (!stopped || quarantined || !released_from_on_stop) {
        std::cerr
            << "hook stop order fixture failed: stopped=" << (stopped ? 1 : 0)
            << " quarantined=" << (quarantined ? 1 : 0)
            << " batch_disable_calls=" << probe->batch_disable_calls.load()
            << " individual_disable_calls="
            << probe->individual_disable_calls.load()
            << " remove_calls=" << probe->remove_calls.load() << '\n';
        return false;
    }
    std::cout << "ok plugin_hook_stop_order individual_release=1 fallback=0\n";
    return true;
}

bool VerifyEnablementDrainFixture(const std::filesystem::path& root) {
    constexpr wchar_t kHoldVariable[] =
        L"ANOMALY_HOOK_FIXTURE_HOLD_MS";
    const DWORD previous_size =
        GetEnvironmentVariableW(kHoldVariable, nullptr, 0);
    std::wstring previous;
    if (previous_size != 0) {
        previous.resize(previous_size);
        const DWORD written = GetEnvironmentVariableW(
            kHoldVariable, previous.data(), previous_size);
        if (written == 0 || written >= previous_size)
            previous.clear();
    }
    SetEnvironmentVariableW(kHoldVariable, L"1600");

    const auto inline_dispatch = [](
        std::string, std::uint64_t, std::function<void()> callback) -> bool {
        if (!callback) return false;
        callback();
        return true;
    };
    auto probe = std::make_shared<HookStopOrderProbe>();
    bool valid{};
    {
        ue5mem::PluginManager manager(
            root, root, {}, {}, {}, inline_dispatch, inline_dispatch, {},
            std::make_unique<RejectBatchHookBackend>(probe));
        manager.LoadAll();
        auto loaded = manager.Plugins();
        auto plugin = std::ranges::find_if(loaded, [](const auto& view) {
            return view.id == "anomaly.hook-lifecycle";
        });
        if (plugin != loaded.end() && plugin->state == "disabled" &&
            manager.SetEnabled(plugin->id, true)) {
            loaded = manager.Plugins();
            plugin = std::ranges::find_if(loaded, [](const auto& view) {
                return view.id == "anomaly.hook-lifecycle";
            });
        }
        if (plugin == loaded.end() || plugin->state != "active") {
            std::cerr << "enablement drain fixture plugin did not activate\n";
        } else {
            const auto started = std::chrono::steady_clock::now();
            const bool disabled = manager.SetEnabled(plugin->id, false);
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
            const bool quarantined = std::ranges::any_of(
                manager.Plugins(), [](const auto& view) {
                    return view.id == "anomaly.hook-lifecycle" &&
                        view.state == "quarantined";
                });
            valid = disabled && !quarantined &&
                elapsed >= std::chrono::milliseconds(1200) &&
                probe->individual_disable_calls.load() == 1 &&
                probe->remove_calls.load() == 1;
            if (!valid) {
                wchar_t stage[64]{};
                GetEnvironmentVariableW(
                    L"ANOMALY_HOOK_FIXTURE_STAGE", stage,
                    static_cast<DWORD>(std::size(stage)));
                std::string stage_utf8;
                const int stage_size = WideCharToMultiByte(
                    CP_UTF8, 0, stage, -1, nullptr, 0, nullptr, nullptr);
                if (stage_size > 1) {
                    stage_utf8.resize(
                        static_cast<std::size_t>(stage_size - 1));
                    static_cast<void>(WideCharToMultiByte(
                        CP_UTF8, 0, stage, -1, stage_utf8.data(),
                        stage_size, nullptr, nullptr));
                }
                std::cerr
                    << "enablement drain fixture failed: disabled="
                    << (disabled ? 1 : 0)
                    << " quarantined=" << (quarantined ? 1 : 0)
                    << " elapsed_ms=" << elapsed.count()
                    << " individual_disable_calls="
                    << probe->individual_disable_calls.load()
                    << " remove_calls=" << probe->remove_calls.load()
                    << " stage=" << stage_utf8
                    << '\n';
            }
        }
    }
    SetEnvironmentVariableW(
        kHoldVariable, previous.empty() ? nullptr : previous.c_str());
    SetEnvironmentVariableW(L"ANOMALY_HOOK_FIXTURE_STAGE", nullptr);
    if (valid)
        std::cout << "ok plugin_enablement_drain timeout=5000 drain=1\n";
    return valid;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path input;
    bool watcher_fixture{};
    bool config_watcher_fixture{};
    bool hook_stop_order_fixture{};
    bool enablement_drain_fixture{};
    bool parallel_stage_fixture{};
    bool enablement_fixture{};
    int reloads = 1;
    int ticks = 3;
    int duration_seconds = 0;
    int tick_interval_ms = 16;
    int reload_every_ticks = 3600;
    std::uint64_t private_growth_budget_bytes = kDefaultPrivateGrowthBudgetBytes;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--watcher-fixture") watcher_fixture = true;
        else if (argument == L"--config-watcher-fixture") config_watcher_fixture = true;
        else if (argument == L"--hook-stop-order-fixture") hook_stop_order_fixture = true;
        else if (argument == L"--enablement-drain-fixture") enablement_drain_fixture = true;
        else if (argument == L"--parallel-stage-fixture") parallel_stage_fixture = true;
        else if (argument == L"--enablement-fixture") enablement_fixture = true;
        else if (argument == L"--plugin" && index + 1 < argc) input = argv[++index];
        else if (argument == L"--reload" && index + 1 < argc) reloads = _wtoi(argv[++index]);
        else if (argument == L"--ticks" && index + 1 < argc) ticks = _wtoi(argv[++index]);
        else if (argument == L"--duration-seconds" && index + 1 < argc) duration_seconds = _wtoi(argv[++index]);
        else if (argument == L"--tick-interval-ms" && index + 1 < argc) tick_interval_ms = _wtoi(argv[++index]);
        else if (argument == L"--reload-every-ticks" && index + 1 < argc) reload_every_ticks = _wtoi(argv[++index]);
        else if (argument == L"--private-growth-budget-bytes" && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], private_growth_budget_bytes)) { Usage(); return 1; }
        }
        else { Usage(); return 1; }
    }
    if (!VerifyPatternMatcher()) {
        std::cerr << "pattern matcher fixture failed\n";
        return 9;
    }
    if (watcher_fixture) {
        if (!VerifyPluginFileWatcher()) {
            std::cerr << "plugin file watcher fixture failed\n";
            return 10;
        }
        std::cout << "ok plugin_file_watcher idle_scans=0 change_delivered=1\n";
        return 0;
    }
    if (config_watcher_fixture) {
        if (!VerifyPluginConfigFileWatcher()) {
            std::cerr << "plugin config file watcher fixture failed\n";
            return 11;
        }
        std::cout << "ok plugin_config_file_watcher idle_quiet=1 change_delivered=2\n";
        return 0;
    }
    if (parallel_stage_fixture) {
        if (!VerifyConcurrentShadowStaging()) {
            std::cerr << "parallel shadow staging fixture failed\n";
            return 13;
        }
        std::cout << "ok parallel_shadow_staging packages=8\n";
        return 0;
    }
    if (input.empty() || reloads < 0 || ticks < 0 || duration_seconds < 0 ||
        duration_seconds > 7 * 24 * 60 * 60 || tick_interval_ms < 0 ||
        tick_interval_ms > 60000 || reload_every_ticks <= 0 ||
        private_growth_budget_bytes == 0 ||
        private_growth_budget_bytes > kMaxPrivateGrowthBudgetBytes) {
        Usage(); return 1;
    }
    std::error_code error;
    input = std::filesystem::weakly_canonical(input, error);
    if (error || !std::filesystem::is_directory(input, error)) {
        std::cerr << "plugin path is unavailable\n"; return 2;
    }
    const bool single_package = std::filesystem::is_regular_file(input / L"manifest.json", error);
    auto root = input;
    std::filesystem::path staging;
    if (single_package) {
        staging = std::filesystem::temp_directory_path() /
            (L"anomaly-test-host-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(staging, error); error.clear();
        std::filesystem::create_directories(staging / input.filename(), error);
        if (!error) std::filesystem::copy(input, staging / input.filename(),
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
        if (error) { std::cerr << "staging failed: " << error.message() << '\n'; return 3; }
        root = staging;
    }
    if (enablement_fixture) {
        const bool ok = VerifyEnablementConfigReload(root);
        if (!staging.empty()) std::filesystem::remove_all(staging, error);
        if (!ok) {
            std::cerr << "plugin enablement reload fixture failed\n";
            return 12;
        }
        std::cout << "ok enablement_config_reload enable_applied=1\n";
        return 0;
    }
    if (hook_stop_order_fixture) {
        const bool ok = VerifyHookStopOrderFixture(root);
        if (!staging.empty()) std::filesystem::remove_all(staging, error);
        if (!ok) {
            std::cerr << "plugin hook stop order fixture failed\n";
            return 13;
        }
        return 0;
    }
    if (enablement_drain_fixture) {
        const bool ok = VerifyEnablementDrainFixture(root);
        if (!staging.empty()) std::filesystem::remove_all(staging, error);
        if (!ok) {
            std::cerr << "plugin enablement drain fixture failed\n";
            return 14;
        }
        return 0;
    }
    int result = 0;
    try {
        const std::uint64_t private_before = PrivateBytes();
        const auto inline_dispatch = [](
            std::string, std::uint64_t, std::function<void()> callback) -> bool {
            if (!callback) return false;
            callback();
            return true;
        };
        // The standalone host has no RuntimeSession dispatcher. It runs
        // resource staging synchronously only for deterministic SDK smoke
        // coverage; production routes both adapters through RuntimeDispatchers.
        ue5mem::PluginManager manager(
            root, root, {}, {}, {}, inline_dispatch, inline_dispatch);
        manager.SetUiService(&kUi);
        manager.LoadAll();
        for (const auto& plugin : manager.Plugins()) {
            if (plugin.state == "disabled" && !manager.SetEnabled(plugin.id, true)) {
                std::cerr << "plugin activation failed: " << plugin.id << '\n';
                result = 4;
            }
        }
        if (std::ranges::none_of(
                manager.Plugins(), [](const auto& plugin) { return plugin.enabled; })) {
            std::cerr << "no plugins activated\n";
            result = 4;
        }
        std::uint64_t executed_ticks{};
        std::uint64_t executed_reloads{};
        if (duration_seconds == 0) {
            for (int round = 0; result == 0 && round <= reloads; ++round) {
                for (int tick = 0; tick < ticks; ++tick) {
                    manager.GameUpdate(1.0 / 60.0);
                    ++executed_ticks;
                }
                manager.Draw(nullptr);
                if (round != reloads) {
                    manager.ReloadAll();
                    ++executed_reloads;
                }
            }
        } else {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(duration_seconds);
            while (result == 0 && std::chrono::steady_clock::now() < deadline) {
                manager.GameUpdate(1.0 / 60.0);
                manager.Draw(nullptr);
                ++executed_ticks;
                if (executed_ticks % static_cast<std::uint64_t>(reload_every_ticks) == 0) {
                    manager.ReloadAll();
                    ++executed_reloads;
                }
                if (tick_interval_ms != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(tick_interval_ms));
                }
            }
        }
        const auto active = static_cast<std::size_t>(std::ranges::count_if(
            manager.Plugins(), [](const auto& plugin) { return plugin.enabled; }));
        manager.UnloadAll();
        if (std::ranges::any_of(manager.Plugins(), [](const auto& plugin) {
                return plugin.enabled || plugin.state == "quarantined";
            })) {
            result = 5;
        }
        const std::size_t resource_leases_after_stop = manager.UiResources().ResourceLeaseCount();
        const std::size_t resource_staging_after_stop = manager.UiResources().StagingBytesInUse();
        const std::size_t hotkeys_after_stop = manager.Input().HotkeyCount();
        if (result == 0 &&
            (resource_leases_after_stop != 0 || resource_staging_after_stop != 0 ||
             hotkeys_after_stop != 0)) {
            std::cerr << "resource cleanup incomplete after stop: leases="
                      << resource_leases_after_stop << " staging=" << resource_staging_after_stop
                      << " hotkeys=" << hotkeys_after_stop << '\n';
            result = 8;
        }
        const std::uint64_t private_after = PrivateBytes();
        const std::uint64_t private_growth = private_after > private_before
            ? private_after - private_before
            : 0;
        if (executed_reloads >= 100 && private_growth > private_growth_budget_bytes) {
            std::cerr << "private memory growth exceeded budget: " << private_growth
                      << " > " << private_growth_budget_bytes << '\n';
            result = 7;
        }
        std::cout << "ok active=" << active << " reloads=" << executed_reloads
                  << " ticks=" << executed_ticks << " windows=" << g_ui.windows
                  << " text=" << g_ui.text
                  << " resource_leases_after_stop=" << resource_leases_after_stop
                  << " resource_staging_after_stop=" << resource_staging_after_stop
                  << " hotkeys_after_stop=" << hotkeys_after_stop << '\n';
        std::cout << "metrics private_growth_bytes=" << private_growth
                  << " private_growth_budget_bytes=" << private_growth_budget_bytes
                  << " duration_seconds=" << duration_seconds << '\n';
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n'; result = 6;
    }
    if (!staging.empty()) std::filesystem::remove_all(staging, error);
    return result;
}
