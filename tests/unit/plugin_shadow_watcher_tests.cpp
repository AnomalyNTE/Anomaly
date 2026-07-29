#include "anomaly/plugin_file_watcher.hpp"
#include "anomaly/plugin_shadow_store.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

using namespace std::chrono_literals;

namespace {
int failures{};
void Expect(bool value, std::string_view message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
struct TempDirectory final {
    std::filesystem::path path;
    TempDirectory() {
        path = std::filesystem::temp_directory_path() /
            (L"anomaly-shadow-tests-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
std::string Manifest(std::string_view id) {
    return "{\"schemaVersion\":2,\"id\":\"" + std::string(id) +
        "\",\"name\":\"Fixture\",\"version\":\"1.0.0\",\"entry\":\"bin/plugin.dll\","
        "\"api\":{\"major\":3,\"minMinor\":0,\"maxMinor\":0},\"games\":[\"nte\"],"
        "\"builds\":[\"nte-*\"],\"loadPhase\":\"game-ready\",\"capabilities\":[]}";
}
void AddPackage(const std::filesystem::path& root, std::wstring_view name, std::string_view id) {
    const auto package = root / name;
    std::filesystem::create_directories(package / L"bin");
    std::filesystem::create_directories(package / L"assets");
    std::ofstream(package / L"manifest.json") << Manifest(id);
    std::ofstream(package / L"bin" / L"plugin.dll", std::ios::binary) << "dll";
    std::ofstream(package / L"assets" / L"value.txt") << "one";
}
}  // namespace

int main() {
    TempDirectory temp;
    const auto packages = temp.path / L"plugins";
    const auto shadows = temp.path / L"cache";
    std::filesystem::create_directories(packages);
    AddPackage(packages, L"A", "fixture.a");
    AddPackage(packages, L"B", "fixture.b");
    auto catalog = anomaly::DiscoverPluginCatalog(packages);
    const auto* package_a = catalog.Find("fixture.a");
    Expect(package_a != nullptr, "fixture package cataloged");

    anomaly::PluginShadowStore store(shadows);
    auto first = store.Stage(*package_a);
    Expect(first.Ok(), "first generation staged");
    Expect(std::filesystem::exists(first.shadow->package_root / L"assets" / L"value.txt"),
        "full package copied");
    std::ofstream(packages / L"A" / L"assets" / L"value.txt", std::ios::trunc) << "two";
    catalog = anomaly::DiscoverPluginCatalog(packages);
    auto second = store.Stage(*catalog.Find("fixture.a"));
    Expect(second.Ok() && second.shadow->generation != first.shadow->generation,
        "new immutable generation staged");
    Expect(std::filesystem::exists(first.shadow->entry_file), "old generation retained for rollback");
    std::filesystem::create_directories(shadows / L"fixture.a" / L"3");
    auto third = store.Stage(*catalog.Find("fixture.a"));
    Expect(third.Ok() && third.shadow->generation > 3,
        "stale generation directory is skipped after process-id reuse");
    store.CleanupPluginExcept("fixture.a", third.shadow->generation);
    Expect(!std::filesystem::exists(first.shadow->package_root) &&
        std::filesystem::exists(third.shadow->package_root), "old generation retired after commit");

    anomaly::PluginFileWatcher watcher(packages, {.poll_interval = 10ms, .debounce = 100ms});
    const auto start = anomaly::PluginFileWatcher::Clock::now();
    Expect(watcher.PollForTests(start).empty(), "watcher baseline is quiet");
    std::ofstream(packages / L"A" / L"assets" / L"value.txt", std::ios::trunc) << "three";
    Expect(watcher.PollForTests(start + 10ms).empty(), "change enters debounce");
    std::ofstream(packages / L"A" / L"assets" / L"value.txt", std::ios::app) << "-stable";
    Expect(watcher.PollForTests(start + 50ms).empty(), "burst coalesced");
    const auto changed = watcher.PollForTests(start + 151ms);
    Expect(changed.size() == 1 && changed[0] == "A", "only changed package emitted");
    Expect(watcher.PollForTests(start + 300ms).empty(), "stable package not emitted twice");

    store.Cleanup();
    Expect(!std::filesystem::exists(shadows), "shadow store cleanup");
    if (failures != 0) return 1;
    std::cout << "shadow generations and package watcher passed\n";
    return 0;
}
