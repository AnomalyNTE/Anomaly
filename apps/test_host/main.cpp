#include "anomaly/host_ui_service.hpp"
#include "anomaly/plugin_file_watcher.hpp"
#include "pattern.hpp"
#include "plugin_manager.hpp"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
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
    std::cerr << "usage: anomaly-test-host --watcher-fixture | "
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
    return wildcard.FindAll(std::span(bytes).first(3), 2) ==
        std::vector<std::size_t>{0, 1};
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
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool idle_silent{};
    {
        std::scoped_lock lock(mutex);
        idle_silent = changed_packages.empty();
    }
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
    std::filesystem::remove_all(root, error);
    const bool valid = idle_silent && delivered;
    if (!valid) {
        std::cerr << "watcher fixture: idle_silent=" << (idle_silent ? 1 : 0)
                  << " delivered=" << (delivered ? 1 : 0) << '\n';
    }
    return valid;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path input;
    bool watcher_fixture{};
    int reloads = 1;
    int ticks = 3;
    int duration_seconds = 0;
    int tick_interval_ms = 16;
    int reload_every_ticks = 3600;
    std::uint64_t private_growth_budget_bytes = kDefaultPrivateGrowthBudgetBytes;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--watcher-fixture") watcher_fixture = true;
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
        std::cout << "ok plugin_file_watcher idle_silent=1 change_delivered=1\n";
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
